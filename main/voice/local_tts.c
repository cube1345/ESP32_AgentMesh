#include "voice/local_tts.h"

#include "drivers/max98357.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espagent_config.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "voice_local_tts";

#define LOCAL_TTS_HTTP_TIMEOUT_MS 45000
#define LOCAL_TTS_PCM_SAMPLE_RATE 16000
#define LOCAL_TTS_PCM_BITS        16
#define LOCAL_TTS_HTTP_MAX_RETRIES 5
#define LOCAL_TTS_HTTP_RETRY_DELAY_MS 1200

typedef struct {
    char *response_body;
    size_t response_len;
    size_t response_cap;
} local_tts_http_ctx_t;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} local_tts_audio_buf_t;

static size_t json_escape_copy(char *dst, size_t dst_size, const char *src)
{
    size_t used = 0;

    if (!dst || dst_size == 0) {
        return 0;
    }
    dst[0] = '\0';
    if (!src) {
        return 0;
    }

    while (*src && used + 1 < dst_size) {
        const unsigned char ch = (unsigned char)(*src++);
        const char *escape = NULL;
        char unicode_escape[7];

        switch (ch) {
        case '\"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            if (ch < 0x20) {
                snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", ch);
                escape = unicode_escape;
            }
            break;
        }

        if (!escape) {
            dst[used++] = (char)ch;
            continue;
        }

        size_t escape_len = strlen(escape);
        if (used + escape_len >= dst_size) {
            break;
        }
        memcpy(dst + used, escape, escape_len);
        used += escape_len;
    }

    dst[used] = '\0';
    return used;
}

static esp_err_t local_tts_append_body(local_tts_http_ctx_t *ctx,
                                       const char *data,
                                       int data_len)
{
    if (!ctx || !data || data_len <= 0) {
        return ESP_OK;
    }

    size_t needed = ctx->response_len + (size_t)data_len + 1;
    if (needed > ctx->response_cap) {
        size_t new_cap = ctx->response_cap ? ctx->response_cap : 4096;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        char *tmp = heap_caps_realloc(ctx->response_body, new_cap, MALLOC_CAP_SPIRAM);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        ctx->response_body = tmp;
        ctx->response_cap = new_cap;
    }
    memcpy(ctx->response_body + ctx->response_len, data, (size_t)data_len);
    ctx->response_len += (size_t)data_len;
    ctx->response_body[ctx->response_len] = '\0';
    return ESP_OK;
}

static esp_err_t local_tts_append_audio(local_tts_audio_buf_t *buf,
                                        const uint8_t *data,
                                        size_t data_len)
{
    if (!buf || !data || data_len == 0) {
        return ESP_OK;
    }

    size_t needed = buf->len + data_len;
    if (needed > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap : 16384;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t *tmp = heap_caps_realloc(buf->data, new_cap, MALLOC_CAP_SPIRAM);
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        buf->data = tmp;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, data, data_len);
    buf->len += data_len;
    return ESP_OK;
}

static int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static esp_err_t decode_base64_audio(const char *input,
                                     uint8_t **output,
                                     size_t *output_len)
{
    if (!input || !output || !output_len) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t in_len = strlen(input);
    uint8_t *buf = heap_caps_malloc((in_len / 4 + 1) * 3, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    size_t out_off = 0;
    int quad[4];
    int quad_len = 0;

    for (size_t i = 0; i < in_len; ++i) {
        char ch = input[i];
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') {
            continue;
        }
        if (ch == '=') {
            quad[quad_len++] = -2;
        } else {
            int v = base64_value(ch);
            if (v < 0) {
                heap_caps_free(buf);
                return ESP_ERR_INVALID_ARG;
            }
            quad[quad_len++] = v;
        }

        if (quad_len == 4) {
            if (quad[0] < 0 || quad[1] < 0) {
                heap_caps_free(buf);
                return ESP_ERR_INVALID_ARG;
            }
            buf[out_off++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
            if (quad[2] != -2) {
                if (quad[2] < 0) {
                    heap_caps_free(buf);
                    return ESP_ERR_INVALID_ARG;
                }
                buf[out_off++] = (uint8_t)(((quad[1] & 0x0F) << 4) | (quad[2] >> 2));
                if (quad[3] != -2) {
                    if (quad[3] < 0) {
                        heap_caps_free(buf);
                        return ESP_ERR_INVALID_ARG;
                    }
                    buf[out_off++] = (uint8_t)(((quad[2] & 0x03) << 6) | quad[3]);
                }
            }
            quad_len = 0;
        }
    }

    *output = buf;
    *output_len = out_off;
    return ESP_OK;
}

static esp_err_t collect_ndjson_audio(char *body,
                                      local_tts_audio_buf_t *audio,
                                      char *api_message,
                                      size_t api_message_size)
{
    if (!body || !audio) {
        return ESP_ERR_INVALID_ARG;
    }
    if (api_message && api_message_size > 0) {
        api_message[0] = '\0';
    }

    bool saw_audio = false;
    char *cursor = body;
    while (cursor && *cursor) {
        char *line = cursor;
        char *newline = strpbrk(cursor, "\r\n");
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
            while (*cursor == '\r' || *cursor == '\n') {
                cursor++;
            }
        } else {
            cursor = line + strlen(line);
        }

        if (!line[0]) {
            continue;
        }

        cJSON *root = cJSON_Parse(line);
        if (!root) {
            return ESP_FAIL;
        }

        cJSON *code = cJSON_GetObjectItem(root, "code");
        cJSON *message = cJSON_GetObjectItem(root, "message");
        cJSON *data = cJSON_GetObjectItem(root, "data");
        int code_value = cJSON_IsNumber(code) ? code->valueint : -1;
        const char *message_value = cJSON_IsString(message) ? message->valuestring : "n/a";

        if (code_value != 0 && code_value != 20000000) {
            if (api_message && api_message_size > 0) {
                snprintf(api_message, api_message_size, "%s", message_value);
            }
            cJSON_Delete(root);
            return ESP_FAIL;
        }

        if (cJSON_IsString(data) && data->valuestring[0]) {
            uint8_t *chunk = NULL;
            size_t chunk_len = 0;
            esp_err_t decode_err = decode_base64_audio(data->valuestring, &chunk, &chunk_len);
            if (decode_err != ESP_OK || !chunk || chunk_len == 0) {
                heap_caps_free(chunk);
                cJSON_Delete(root);
                return decode_err != ESP_OK ? decode_err : ESP_FAIL;
            }

            esp_err_t append_err = local_tts_append_audio(audio, chunk, chunk_len);
            heap_caps_free(chunk);
            if (append_err != ESP_OK) {
                cJSON_Delete(root);
                return append_err;
            }
            saw_audio = true;
        }

        if (api_message && api_message_size > 0 && message_value && message_value[0]) {
            snprintf(api_message, api_message_size, "%s", message_value);
        }

        cJSON_Delete(root);
    }

    return saw_audio ? ESP_OK : ESP_FAIL;
}

static esp_err_t play_pcm_audio(const uint8_t *pcm,
                                size_t pcm_len,
                                size_t *i2s_bytes,
                                char *diag,
                                size_t diag_size)
{
    max98357_config_t cfg;
    max98357_stream_t stream;
    char local_diag[160];

    max98357_default_config(&cfg);
    cfg.sample_rate_hz = LOCAL_TTS_PCM_SAMPLE_RATE;

    esp_err_t err = max98357_stream_open(&stream, &cfg, local_diag, sizeof(local_diag));
    if (err != ESP_OK) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "%s", local_diag);
        }
        return err;
    }

    err = max98357_stream_write_pcm_mono16le(&stream, pcm, pcm_len,
                                             local_diag, sizeof(local_diag));
    if (err == ESP_OK) {
        err = max98357_stream_close(&stream, local_diag, sizeof(local_diag));
    } else {
        (void)max98357_stream_close(&stream, local_diag, sizeof(local_diag));
    }

    if (i2s_bytes) {
        *i2s_bytes = stream.i2s_bytes_out;
    }
    if (diag && diag_size > 0) {
        snprintf(diag, diag_size, "%s", local_diag);
    }
    return err;
}

static esp_err_t local_tts_http_event_handler(esp_http_client_event_t *evt)
{
    local_tts_http_ctx_t *ctx = (local_tts_http_ctx_t *)evt->user_data;
    if (!ctx) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        return local_tts_append_body(ctx, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

bool espagent_voice_local_tts_enabled(void)
{
    return ESPAGENT_VOICE_LOCAL_TTS_ENABLE != 0;
}

bool espagent_voice_local_tts_configured(void)
{
    return ESPAGENT_VOLC_TTS_API_KEY[0] != '\0' &&
           ESPAGENT_VOLC_TTS_RESOURCE_ID[0] != '\0' &&
           ESPAGENT_VOLC_TTS_SPEAKER[0] != '\0';
}

esp_err_t espagent_voice_local_tts_speak(const char *text,
                                         char *diag,
                                         size_t diag_size)
{
    if (diag && diag_size > 0) {
        diag[0] = '\0';
    }

    if (!text || !text[0]) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: empty TTS text");
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (!espagent_voice_local_tts_enabled()) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: local S3 TTS is disabled");
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!espagent_voice_local_tts_configured()) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size,
                     "Error: local S3 TTS missing Volc config (api_key/resource_id/speaker)");
        }
        return ESP_ERR_INVALID_STATE;
    }

    char escaped_text[768];
    char request_body[1152];
    char request_id[48];
    local_tts_http_ctx_t ctx = {0};

    json_escape_copy(escaped_text, sizeof(escaped_text), text);
    int body_len = snprintf(request_body,
                            sizeof(request_body),
                            "{\"user\":{\"uid\":\"%s\"},\"event\":100,"
                            "\"req_params\":{\"text\":\"%s\",\"speaker\":\"%s\","
                            "\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":%d}}}",
                            ESPAGENT_NODE_ID,
                            escaped_text,
                            ESPAGENT_VOLC_TTS_SPEAKER,
                            LOCAL_TTS_PCM_SAMPLE_RATE);
    if (body_len <= 0 || body_len >= (int)sizeof(request_body)) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: local TTS request body too large");
        }
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(request_id, sizeof(request_id), "s3-tts-%lld",
             (long long)(esp_timer_get_time() / 1000));

    esp_err_t ret = ESP_FAIL;
    int status = 0;
    int attempts = 0;
    for (attempts = 1; attempts <= LOCAL_TTS_HTTP_MAX_RETRIES; attempts++) {
        heap_caps_free(ctx.response_body);
        memset(&ctx, 0, sizeof(ctx));

        esp_http_client_config_t config = {
            .url = "https://openspeech.bytedance.com/api/v3/tts/unidirectional",
            .event_handler = local_tts_http_event_handler,
            .user_data = &ctx,
            .timeout_ms = LOCAL_TTS_HTTP_TIMEOUT_MS,
            .buffer_size = 1024,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .addr_type = HTTP_ADDR_TYPE_INET,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            if (diag && diag_size > 0) {
                snprintf(diag, diag_size, "Error: local TTS http client init failed");
            }
            return ESP_FAIL;
        }

        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_header(client, "Accept", "*/*");
        esp_http_client_set_header(client, "X-Api-Key", ESPAGENT_VOLC_TTS_API_KEY);
        esp_http_client_set_header(client, "X-Api-Resource-Id", ESPAGENT_VOLC_TTS_RESOURCE_ID);
        esp_http_client_set_header(client, "X-Api-Request-Id", request_id);
        esp_http_client_set_post_field(client, request_body, body_len);

        ret = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        bool allow_incomplete_200 = (ret == ESP_ERR_HTTP_INCOMPLETE_DATA &&
                                     status == 200 &&
                                     ctx.response_body &&
                                     ctx.response_len > 0);
        if (ret == ESP_OK && status == 200) {
            break;
        }
        if (allow_incomplete_200) {
            ESP_LOGW(TAG, "local TTS request attempt %d/%d returned incomplete chunked body; continuing with %u bytes",
                     attempts, LOCAL_TTS_HTTP_MAX_RETRIES, (unsigned)ctx.response_len);
            ret = ESP_OK;
            break;
        }

        ESP_LOGW(TAG, "local TTS request attempt %d/%d failed: ret=%s status=%d",
                 attempts, LOCAL_TTS_HTTP_MAX_RETRIES, esp_err_to_name(ret), status);
        if (attempts < LOCAL_TTS_HTTP_MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(LOCAL_TTS_HTTP_RETRY_DELAY_MS));
        }
    }

    if (ret != ESP_OK) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: local TTS http perform failed after %d attempt(s) (%s)",
                     attempts, esp_err_to_name(ret));
        }
        heap_caps_free(ctx.response_body);
        return ret;
    }

    if (status != 200) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: local TTS http status=%d after %d attempt(s) body=%s",
                     status, attempts, ctx.response_body ? ctx.response_body : "n/a");
        }
        heap_caps_free(ctx.response_body);
        return ESP_FAIL;
    }

    local_tts_audio_buf_t audio = {0};
    char api_message[96] = {0};
    esp_err_t parse_err = collect_ndjson_audio(ctx.response_body ? ctx.response_body : "",
                                               &audio,
                                               api_message,
                                               sizeof(api_message));
    if (parse_err != ESP_OK || !audio.data || audio.len == 0) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: local TTS audio parse failed (%s)",
                     api_message[0] ? api_message : esp_err_to_name(parse_err));
        }
        heap_caps_free(audio.data);
        heap_caps_free(ctx.response_body);
        return parse_err != ESP_OK ? parse_err : ESP_FAIL;
    }

    size_t i2s_bytes = 0;
    char play_diag[160] = {0};
    esp_err_t play_err = play_pcm_audio(audio.data, audio.len, &i2s_bytes,
                                        play_diag, sizeof(play_diag));
    heap_caps_free(audio.data);
    heap_caps_free(ctx.response_body);
    if (play_err != ESP_OK) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: local TTS audio output failed (%s)",
                     play_diag[0] ? play_diag : esp_err_to_name(play_err));
        }
        return play_err;
    }

    if (diag && diag_size > 0) {
        snprintf(diag, diag_size,
                 "OK: local TTS played pcm=%uB i2s=%uB req=%s speaker=%s rate=%d",
                 (unsigned)audio.len,
                 (unsigned)i2s_bytes,
                 request_id,
                 ESPAGENT_VOLC_TTS_SPEAKER,
                 LOCAL_TTS_PCM_SAMPLE_RATE);
    }
    ESP_LOGI(TAG, "%s", diag && diag[0] ? diag : "local TTS played");
    return ESP_OK;
}

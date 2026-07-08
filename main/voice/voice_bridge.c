#include "voice/voice_bridge.h"

#include "bus/message_bus.h"
#include "drivers/ina_mic.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espagent_config.h"
#include "sensors/sensor_mqtt.h"
#include "voice/local_tts.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char *TAG = "voice_bridge";

typedef struct {
    char request_id[48];
    char device_id[48];
    char reply_channel[24];
    char reply_chat_id[64];
    char hint_text[96];
    uint32_t capture_ms;
    uint32_t sample_rate_hz;
} stt_capture_job_t;

typedef struct {
    stt_capture_job_t *job;
    uint8_t pending[ESPAGENT_VOICE_STT_CHUNK_SAMPLES * sizeof(int16_t)];
    size_t pending_len;
    size_t total_samples;
    uint32_t chunk_index;
} stt_capture_publish_ctx_t;

static SemaphoreHandle_t s_stt_capture_mutex = NULL;

static bool topic_equals(const char *topic,
                         size_t topic_len,
                         const char *expected)
{
    return topic && expected &&
           strlen(expected) == topic_len &&
           strncmp(topic, expected, topic_len) == 0;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void json_add_string_if(cJSON *root, const char *key, const char *value)
{
    if (root && key && value && value[0]) {
        cJSON_AddStringToObject(root, key, value);
    }
}

static SemaphoreHandle_t stt_capture_mutex(void)
{
    if (!s_stt_capture_mutex) {
        s_stt_capture_mutex = xSemaphoreCreateMutex();
    }
    return s_stt_capture_mutex;
}

static esp_err_t publish_voice_event(const char *event,
                                     const char *status,
                                     const char *request_id,
                                     const char *detail)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.voice.event.v1");
    cJSON_AddStringToObject(root, "event", event ? event : "voice_event");
    cJSON_AddStringToObject(root, "status", status ? status : "info");
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    json_add_string_if(root, "request_id", request_id);
    json_add_string_if(root, "detail", detail);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_VOICE_EVENTS, json);
    cJSON_free(json);
    return err;
}

static esp_err_t publish_stt_audio_chunk(stt_capture_publish_ctx_t *ctx,
                                         const uint8_t *audio_bytes,
                                         size_t audio_len)
{
    if (!ctx || !ctx->job || !audio_bytes || audio_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t b64_len = 4 * ((audio_len + 2) / 3);
    char *b64 = calloc(1, b64_len + 1);
    if (!b64) {
        return ESP_ERR_NO_MEM;
    }

    size_t out_len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)b64,
                                   b64_len + 1,
                                   &out_len,
                                   audio_bytes,
                                   audio_len);
    if (rc != 0) {
        free(b64);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(b64);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "schema", "espagent.voice.stt_audio_chunk.v1");
    cJSON_AddStringToObject(root, "event", "stt_audio_chunk");
    cJSON_AddStringToObject(root, "request_id", ctx->job->request_id);
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "device_id", ctx->job->device_id);
    cJSON_AddNumberToObject(root, "chunk_index", (double)ctx->chunk_index++);
    cJSON_AddNumberToObject(root, "sample_rate_hz", (double)ctx->job->sample_rate_hz);
    cJSON_AddNumberToObject(root, "channels", 1);
    cJSON_AddStringToObject(root, "format", "pcm_s16le");
    cJSON_AddStringToObject(root, "audio_b64", b64);
    cJSON_AddNumberToObject(root, "audio_bytes", (double)audio_len);
    json_add_string_if(root, "reply_channel", ctx->job->reply_channel);
    json_add_string_if(root, "reply_chat_id", ctx->job->reply_chat_id);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(b64);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_VOICE_STT_AUDIO_CHUNK, json);
    cJSON_free(json);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    return err;
}

static esp_err_t stt_pcm_writer(const int16_t *samples, size_t sample_count, void *arg)
{
    stt_capture_publish_ctx_t *ctx = (stt_capture_publish_ctx_t *)arg;
    const uint8_t *src = (const uint8_t *)samples;
    size_t src_len = sample_count * sizeof(int16_t);

    if (!ctx || !samples || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->total_samples += sample_count;
    while (src_len > 0) {
        size_t avail = sizeof(ctx->pending) - ctx->pending_len;
        size_t copy_len = src_len < avail ? src_len : avail;
        memcpy(ctx->pending + ctx->pending_len, src, copy_len);
        ctx->pending_len += copy_len;
        src += copy_len;
        src_len -= copy_len;

        if (ctx->pending_len == sizeof(ctx->pending)) {
            esp_err_t err = publish_stt_audio_chunk(ctx, ctx->pending, ctx->pending_len);
            if (err != ESP_OK) {
                return err;
            }
            ctx->pending_len = 0;
        }
    }

    return ESP_OK;
}

static esp_err_t flush_stt_pending_audio(stt_capture_publish_ctx_t *ctx)
{
    if (!ctx || ctx->pending_len == 0) {
        return ESP_OK;
    }
    esp_err_t err = publish_stt_audio_chunk(ctx, ctx->pending, ctx->pending_len);
    if (err == ESP_OK) {
        ctx->pending_len = 0;
    }
    return err;
}

static esp_err_t publish_stt_audio_done(const stt_capture_publish_ctx_t *ctx,
                                        const char *status,
                                        const char *detail)
{
    if (!ctx || !ctx->job) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.voice.stt_audio_done.v1");
    cJSON_AddStringToObject(root, "event", "stt_audio_done");
    cJSON_AddStringToObject(root, "request_id", ctx->job->request_id);
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "device_id", ctx->job->device_id);
    cJSON_AddStringToObject(root, "status", status ? status : "ok");
    cJSON_AddNumberToObject(root, "chunks", (double)ctx->chunk_index);
    cJSON_AddNumberToObject(root, "sample_rate_hz", (double)ctx->job->sample_rate_hz);
    cJSON_AddNumberToObject(root, "channels", 1);
    cJSON_AddStringToObject(root, "format", "pcm_s16le");
    cJSON_AddNumberToObject(root, "sample_count", (double)ctx->total_samples);
    json_add_string_if(root, "reply_channel", ctx->job->reply_channel);
    json_add_string_if(root, "reply_chat_id", ctx->job->reply_chat_id);
    json_add_string_if(root, "detail", detail);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_VOICE_STT_AUDIO_DONE, json);
    cJSON_free(json);
    return err;
}

esp_err_t espagent_voice_publish_tts_request(const char *text,
                                             const char *source_channel,
                                             const char *chat_id,
                                             const char *trace_id)
{
    return espagent_voice_publish_tts_request_to_device(text,
                                                        ESPAGENT_VOICE_DEFAULT_DEVICE,
                                                        source_channel,
                                                        chat_id,
                                                        trace_id);
}

esp_err_t espagent_voice_publish_tts_request_to_device(const char *text,
                                                       const char *device_id,
                                                       const char *source_channel,
                                                       const char *chat_id,
                                                       const char *trace_id)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    char request_id[48];
    snprintf(request_id, sizeof(request_id), "tts-%lld", (long long)now_ms());

    cJSON_AddStringToObject(root, "schema", "espagent.voice.tts_request.v1");
    cJSON_AddStringToObject(root, "event", "tts_request");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "device_id",
                            (device_id && device_id[0]) ? device_id : ESPAGENT_VOICE_DEFAULT_DEVICE);
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddBoolToObject(root, "auto_play", true);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());
    json_add_string_if(root, "source_channel", source_channel);
    json_add_string_if(root, "chat_id", chat_id);
    json_add_string_if(root, "trace_id", trace_id);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_VOICE_TTS_REQUEST, json);
    if (err == ESP_OK) {
        (void)sensor_mqtt_publish_timeline_event("voice",
                                                 "tts_request",
                                                 "queued",
                                                 text,
                                                 request_id,
                                                 "display_agent",
                                                 (device_id && device_id[0]) ? device_id : ESPAGENT_VOICE_DEFAULT_DEVICE,
                                                 "tts_request");
    }
    cJSON_free(json);
    return err;
}

esp_err_t espagent_voice_publish_stt_request(const char *session_id,
                                             const char *reply_channel,
                                             const char *reply_chat_id,
                                             const char *hint_text,
                                             bool auto_route_reply)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    char request_id[48];
    snprintf(request_id, sizeof(request_id), "%s",
             (session_id && session_id[0]) ? session_id : "stt-session");

    cJSON_AddStringToObject(root, "schema", "espagent.voice.stt_request.v1");
    cJSON_AddStringToObject(root, "event", "stt_request");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "device_id", ESPAGENT_VOICE_DEFAULT_DEVICE);
    cJSON_AddBoolToObject(root, "auto_route_reply", auto_route_reply);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());
    json_add_string_if(root, "reply_channel", reply_channel);
    json_add_string_if(root, "reply_chat_id", reply_chat_id);
    json_add_string_if(root, "hint_text", hint_text);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_VOICE_STT_REQUEST, json);
    if (err == ESP_OK) {
        (void)sensor_mqtt_publish_timeline_event("voice",
                                                 "stt_request",
                                                 "queued",
                                                 hint_text && hint_text[0] ? hint_text : "Voice capture requested",
                                                 request_id,
                                                 "display_agent",
                                                 ESPAGENT_VOICE_DEFAULT_DEVICE,
                                                 "stt_request");
    }
    cJSON_free(json);
    return err;
}

esp_err_t espagent_voice_build_status_json(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "schema", "espagent.voice.status.v1");
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "device_id", ESPAGENT_VOICE_DEFAULT_DEVICE);
    cJSON_AddBoolToObject(root, "auto_tts_enabled", ESPAGENT_VOICE_AUTO_TTS != 0);
    cJSON_AddStringToObject(root, "tts_request_topic", ESPAGENT_MESH_TOPIC_VOICE_TTS_REQUEST);
    cJSON_AddStringToObject(root, "tts_status_topic", ESPAGENT_MESH_TOPIC_VOICE_TTS_STATUS);
    cJSON_AddStringToObject(root, "stt_request_topic", ESPAGENT_MESH_TOPIC_VOICE_STT_REQUEST);
    cJSON_AddStringToObject(root, "stt_result_topic", ESPAGENT_MESH_TOPIC_VOICE_STT_RESULT);
    cJSON_AddBoolToObject(root, "mqtt_connected", sensor_mqtt_is_connected());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

static bool handle_stt_result(const char *payload, size_t payload_len)
{
    char json_buf[1024];
    size_t copy_len = payload_len >= sizeof(json_buf) ? sizeof(json_buf) - 1 : payload_len;
    memcpy(json_buf, payload, copy_len);
    json_buf[copy_len] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *transcript = cJSON_GetObjectItem(root, "transcript");
    const cJSON *status = cJSON_GetObjectItem(root, "status");
    const cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
    const cJSON *reply_channel = cJSON_GetObjectItem(root, "reply_channel");
    const cJSON *reply_chat_id = cJSON_GetObjectItem(root, "reply_chat_id");

    if (!cJSON_IsString(transcript) || transcript->valuestring[0] == '\0') {
        ESP_LOGI(TAG, "STT result ignored: no transcript. status=%s",
                 cJSON_IsString(status) ? status->valuestring : "unknown");
        cJSON_Delete(root);
        return true;
    }

    espagent_msg_t msg = {0};
    snprintf(msg.channel, sizeof(msg.channel), "%s",
             cJSON_IsString(reply_channel) && reply_channel->valuestring[0]
                 ? reply_channel->valuestring
                 : ESPAGENT_CHAN_VOICE);
    snprintf(msg.chat_id, sizeof(msg.chat_id), "%s",
             cJSON_IsString(reply_chat_id) && reply_chat_id->valuestring[0]
                 ? reply_chat_id->valuestring
                 : (cJSON_IsString(request_id) ? request_id->valuestring : "voice-session"));
    msg.content = strdup(transcript->valuestring);
    if (!msg.content) {
        cJSON_Delete(root);
        return true;
    }

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Injected voice transcript into agent_loop: channel=%s chat_id=%s text=%s",
                 msg.channel, msg.chat_id, transcript->valuestring);
        (void)sensor_mqtt_publish_timeline_event("voice",
                                                 "stt_result",
                                                 "ok",
                                                 transcript->valuestring,
                                                 cJSON_IsString(request_id) ? request_id->valuestring : NULL,
                                                 ESPAGENT_NODE_ROLE,
                                                 ESPAGENT_NODE_ID,
                                                 "stt_result");
    } else {
        free(msg.content);
        ESP_LOGW(TAG, "Failed to inject voice transcript: %s", esp_err_to_name(err));
    }

    cJSON_Delete(root);
    return true;
}

static bool voice_device_matches_local(const char *device_id)
{
    if (!device_id || !device_id[0]) {
        return false;
    }
    return strcmp(device_id, ESPAGENT_NODE_ID) == 0 ||
           strcmp(device_id, ESPAGENT_NODE_ROLE) == 0;
}

static void stt_capture_task(void *arg)
{
    stt_capture_job_t *job = (stt_capture_job_t *)arg;
    char diag[256] = {0};
    ina_mic_config_t cfg;
    ina_mic_stream_t stream = {0};
    stt_capture_publish_ctx_t pub = {
        .job = job,
    };
    esp_err_t err;

    ina_mic_default_config(&cfg);
    job->sample_rate_hz = cfg.sample_rate_hz;

    (void)publish_voice_event("stt_capture_started", "ok", job->request_id,
                              job->hint_text[0] ? job->hint_text : "mic capture start");

    err = ina_mic_stream_open(&stream, &cfg, diag, sizeof(diag));
    if (err == ESP_OK) {
        size_t sample_count = 0;
        err = ina_mic_stream_capture_pcm16(&stream,
                                           job->capture_ms,
                                           stt_pcm_writer,
                                           &pub,
                                           &sample_count,
                                           diag,
                                           sizeof(diag));
        if (err == ESP_OK) {
            err = flush_stt_pending_audio(&pub);
        }
        pub.total_samples = sample_count;
        (void)ina_mic_stream_close(&stream, diag, sizeof(diag));
    }

    if (err == ESP_OK) {
        (void)publish_stt_audio_done(&pub, "ok", diag);
        (void)publish_voice_event("stt_capture_finished", "ok", job->request_id, diag);
    } else {
        (void)publish_stt_audio_done(&pub, "error", diag[0] ? diag : esp_err_to_name(err));
        (void)publish_voice_event("stt_capture_finished", "error", job->request_id,
                                  diag[0] ? diag : esp_err_to_name(err));
    }

    SemaphoreHandle_t mutex = stt_capture_mutex();
    if (mutex) {
        xSemaphoreGive(mutex);
    }
    free(job);
    vTaskDelete(NULL);
}

static bool handle_stt_request(const char *payload, size_t payload_len)
{
    char json_buf[1024];
    size_t copy_len = payload_len >= sizeof(json_buf) ? sizeof(json_buf) - 1 : payload_len;
    memcpy(json_buf, payload, copy_len);
    json_buf[copy_len] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *device_id = cJSON_GetObjectItem(root, "device_id");
    const cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
    const cJSON *reply_channel = cJSON_GetObjectItem(root, "reply_channel");
    const cJSON *reply_chat_id = cJSON_GetObjectItem(root, "reply_chat_id");
    const cJSON *hint_text = cJSON_GetObjectItem(root, "hint_text");
    const char *device_value = cJSON_IsString(device_id) ? device_id->valuestring : "";
    const char *request_value = cJSON_IsString(request_id) ? request_id->valuestring : "";

    if (!voice_device_matches_local(device_value)) {
        cJSON_Delete(root);
        return false;
    }

    SemaphoreHandle_t mutex = stt_capture_mutex();
    if (!mutex) {
        cJSON_Delete(root);
        return true;
    }
    if (xSemaphoreTake(mutex, 0) != pdTRUE) {
        (void)publish_voice_event("stt_capture_busy",
                                  "busy",
                                  request_value,
                                  "previous mic capture still running");
        cJSON_Delete(root);
        return true;
    }

    stt_capture_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        xSemaphoreGive(mutex);
        cJSON_Delete(root);
        return true;
    }

    snprintf(job->request_id, sizeof(job->request_id), "%s",
             request_value[0] ? request_value : "stt-session");
    snprintf(job->device_id, sizeof(job->device_id), "%s",
             device_value[0] ? device_value : ESPAGENT_NODE_ID);
    if (cJSON_IsString(reply_channel)) {
        snprintf(job->reply_channel, sizeof(job->reply_channel), "%s", reply_channel->valuestring);
    }
    if (cJSON_IsString(reply_chat_id)) {
        snprintf(job->reply_chat_id, sizeof(job->reply_chat_id), "%s", reply_chat_id->valuestring);
    }
    if (cJSON_IsString(hint_text)) {
        snprintf(job->hint_text, sizeof(job->hint_text), "%s", hint_text->valuestring);
    }
    job->capture_ms = ESPAGENT_VOICE_STT_CAPTURE_MS;

    BaseType_t ok = xTaskCreatePinnedToCore(stt_capture_task,
                                            "voice_stt_capture",
                                            ESPAGENT_VOICE_STT_TASK_STACK,
                                            job,
                                            ESPAGENT_VOICE_STT_TASK_PRIO,
                                            NULL,
                                            0);
    if (ok != pdPASS) {
        xSemaphoreGive(mutex);
        free(job);
        (void)publish_voice_event("stt_capture_spawn_failed",
                                  "error",
                                  request_value,
                                  "failed to create stt capture task");
    }

    cJSON_Delete(root);
    return true;
}

static bool handle_tts_request(const char *payload, size_t payload_len)
{
    char json_buf[1024];
    size_t copy_len = payload_len >= sizeof(json_buf) ? sizeof(json_buf) - 1 : payload_len;
    memcpy(json_buf, payload, copy_len);
    json_buf[copy_len] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *device_id = cJSON_GetObjectItem(root, "device_id");
    const cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
    const cJSON *text = cJSON_GetObjectItem(root, "text");
    const char *device_value = cJSON_IsString(device_id) ? device_id->valuestring : "";
    const char *request_value = cJSON_IsString(request_id) ? request_id->valuestring : "";
    const char *text_value = cJSON_IsString(text) ? text->valuestring : "";

    if (!voice_device_matches_local(device_value) || !text_value[0]) {
        cJSON_Delete(root);
        return false;
    }

    char diag[256] = {0};
    esp_err_t err = espagent_voice_local_tts_speak(text_value, diag, sizeof(diag));
    ESP_LOGI(TAG, "Local voice TTS request handled: device=%s request_id=%s status=%s detail=%s",
             device_value[0] ? device_value : "(none)",
             request_value[0] ? request_value : "(none)",
             esp_err_to_name(err),
             diag[0] ? diag : "no detail");

    cJSON *status_root = cJSON_CreateObject();
    if (status_root) {
        cJSON_AddStringToObject(status_root, "schema", "espagent.voice.tts_status.v1");
        cJSON_AddStringToObject(status_root, "event", "tts_status");
        cJSON_AddStringToObject(status_root, "request_id", request_value);
        cJSON_AddStringToObject(status_root, "node_id", ESPAGENT_NODE_ID);
        cJSON_AddStringToObject(status_root, "role", ESPAGENT_NODE_ROLE);
        cJSON_AddStringToObject(status_root, "device_id", device_value);
        cJSON_AddStringToObject(status_root, "status", err == ESP_OK ? "ok" : "error");
        cJSON_AddStringToObject(status_root, "detail", diag[0] ? diag : esp_err_to_name(err));
        cJSON_AddNumberToObject(status_root, "ts_ms", (double)now_ms());
        char *status_json = cJSON_PrintUnformatted(status_root);
        if (status_json) {
            (void)sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_VOICE_TTS_STATUS, status_json);
            cJSON_free(status_json);
        }
        cJSON_Delete(status_root);
    }

    cJSON_Delete(root);
    return true;
}

bool espagent_voice_handle_mqtt_message(const char *topic,
                                        size_t topic_len,
                                        const char *payload,
                                        size_t payload_len)
{
    if (topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_VOICE_STT_REQUEST)) {
        return handle_stt_request(payload, payload_len);
    }

    if (topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_VOICE_TTS_REQUEST)) {
        return handle_tts_request(payload, payload_len);
    }

    if (topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_VOICE_STT_RESULT)) {
        return handle_stt_result(payload, payload_len);
    }

    if (topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_VOICE_TTS_STATUS) ||
        topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_VOICE_EVENTS)) {
        char detail[192];
        size_t copy_len = payload_len >= sizeof(detail) ? sizeof(detail) - 1 : payload_len;
        memcpy(detail, payload, copy_len);
        detail[copy_len] = '\0';
        ESP_LOGI(TAG, "Voice MQTT event on %.*s: %s", (int)topic_len, topic, detail);
        return true;
    }

    return false;
}

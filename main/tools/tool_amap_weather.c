#include "tool_amap_weather.h"

#include "espagent_config.h"
#include "proxy/http_proxy.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "amap_weather";

#define AMAP_HOST "restapi.amap.com"
#define AMAP_BUF_SIZE (12 * 1024)

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} amap_buf_t;

static char s_key[128] = {0};
static char s_default_location[96] = {0};
static char s_default_adcode[16] = {0};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    amap_buf_t *buf = (amap_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && buf->data) {
        size_t needed = buf->len + evt->data_len;
        if (needed < buf->cap) {
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
            buf->data[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool is_adcode(const char *value)
{
    if (!value || strlen(value) != 6) {
        return false;
    }
    for (const char *p = value; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; src && *src && pos < dst_size - 4; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            dst[pos++] = (char)c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    if (dst_size > 0) {
        dst[pos] = '\0';
    }
    return pos;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static esp_err_t amap_direct_get_scheme(const char *scheme, const char *path,
                                        amap_buf_t *buf)
{
    char url[512];
    snprintf(url, sizeof(url), "%s://" AMAP_HOST "%s", scheme, path);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = buf,
        .timeout_ms = 15000,
        .buffer_size = 4096,
    };
    if (strcmp(scheme, "https") == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "Amap API returned HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t amap_direct_get(const char *path, amap_buf_t *buf)
{
    esp_err_t err = amap_direct_get_scheme("https", path, buf);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (buf && buf->data && buf->cap > 0) {
        buf->len = 0;
        buf->data[0] = '\0';
    }
    ESP_LOGW(TAG, "Amap HTTPS request failed (%s), retrying with HTTP fallback",
             esp_err_to_name(err));
    return amap_direct_get_scheme("http", path, buf);
}

static esp_err_t amap_proxy_get(const char *path, amap_buf_t *buf)
{
    proxy_conn_t *conn = proxy_conn_open(AMAP_HOST, 443, 15000);
    if (!conn) {
        return ESP_ERR_HTTP_CONNECT;
    }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "GET %s HTTP/1.1\r\n"
                        "Host: " AMAP_HOST "\r\n"
                        "Accept: application/json\r\n"
                        "Connection: close\r\n\r\n",
                        path);
    if (hlen <= 0 || proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) {
            break;
        }
        size_t copy = (total + (size_t)n < buf->cap - 1)
                          ? (size_t)n
                          : buf->cap - 1 - total;
        if (copy > 0) {
            memcpy(buf->data + total, tmp, copy);
            total += copy;
        }
    }
    buf->data[total] = '\0';
    buf->len = total;
    proxy_conn_close(conn);

    int status = 0;
    if (total > 5 && strncmp(buf->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf->data, ' ');
        if (sp) {
            status = atoi(sp + 1);
        }
    }

    char *body = strstr(buf->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t body_len = total - (size_t)(body - buf->data);
        memmove(buf->data, body, body_len);
        buf->len = body_len;
        buf->data[buf->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Amap API returned HTTP %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t amap_get_json(const char *path, cJSON **out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    amap_buf_t buf = {0};
    buf.data = heap_caps_calloc(1, AMAP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf.data) {
        return ESP_ERR_NO_MEM;
    }
    buf.cap = AMAP_BUF_SIZE;

    esp_err_t err = http_proxy_is_enabled() ? amap_proxy_get(path, &buf)
                                            : amap_direct_get(path, &buf);
    if (err != ESP_OK) {
        free(buf.data);
        return err;
    }

    cJSON *root = cJSON_Parse(buf.data);
    free(buf.data);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (!cJSON_IsString(status) || strcmp(status->valuestring, "1") != 0) {
        const char *info = json_str(root, "info");
        ESP_LOGW(TAG, "Amap API status failed: %s", info);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *out = root;
    return ESP_OK;
}

static esp_err_t amap_geocode(const char *address, char *adcode,
                              size_t adcode_size, char *resolved,
                              size_t resolved_size)
{
    char encoded[192];
    url_encode(address, encoded, sizeof(encoded));

    char path[384];
    snprintf(path, sizeof(path),
             "/v3/geocode/geo?key=%s&address=%s&output=JSON", s_key,
             encoded);

    cJSON *root = NULL;
    esp_err_t err = amap_get_json(path, &root);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *geocodes = cJSON_GetObjectItem(root, "geocodes");
    cJSON *first = cJSON_IsArray(geocodes) ? cJSON_GetArrayItem(geocodes, 0) : NULL;
    const char *code = first ? json_str(first, "adcode") : "";
    const char *formatted = first ? json_str(first, "formatted_address") : "";
    if (!is_adcode(code)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(adcode, adcode_size, "%s", code);
    if (resolved && resolved_size > 0) {
        snprintf(resolved, resolved_size, "%s", formatted[0] ? formatted : address);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static void format_live(cJSON *root, char *output, size_t output_size)
{
    cJSON *lives = cJSON_GetObjectItem(root, "lives");
    cJSON *live = cJSON_IsArray(lives) ? cJSON_GetArrayItem(lives, 0) : NULL;
    if (!live) {
        snprintf(output, output_size, "No live weather result found.");
        return;
    }

    snprintf(output, output_size,
             "Live weather for %s%s (adcode=%s): %s, temperature=%s C, "
             "humidity=%s%%, wind=%s %s, report_time=%s.",
             json_str(live, "province"), json_str(live, "city"),
             json_str(live, "adcode"), json_str(live, "weather"),
             json_str(live, "temperature"), json_str(live, "humidity"),
             json_str(live, "winddirection"), json_str(live, "windpower"),
             json_str(live, "reporttime"));
}

static void format_forecast(cJSON *root, char *output, size_t output_size)
{
    cJSON *forecasts = cJSON_GetObjectItem(root, "forecasts");
    cJSON *forecast = cJSON_IsArray(forecasts) ? cJSON_GetArrayItem(forecasts, 0) : NULL;
    if (!forecast) {
        snprintf(output, output_size, "No forecast weather result found.");
        return;
    }

    size_t off = 0;
    int n = snprintf(output + off, output_size - off,
                     "Weather forecast for %s%s (adcode=%s), report_time=%s:\n",
                     json_str(forecast, "province"), json_str(forecast, "city"),
                     json_str(forecast, "adcode"), json_str(forecast, "reporttime"));
    if (n < 0 || (size_t)n >= output_size - off) {
        output[output_size - 1] = '\0';
        return;
    }
    off += (size_t)n;

    cJSON *casts = cJSON_GetObjectItem(forecast, "casts");
    cJSON *cast = NULL;
    int idx = 0;
    cJSON_ArrayForEach(cast, casts) {
        if (idx++ >= 4 || off >= output_size - 1) {
            break;
        }
        n = snprintf(output + off, output_size - off,
                     "- %s: day=%s %s C, night=%s %s C, wind=%s %s\n",
                     json_str(cast, "date"), json_str(cast, "dayweather"),
                     json_str(cast, "daytemp"), json_str(cast, "nightweather"),
                     json_str(cast, "nighttemp"), json_str(cast, "daywind"),
                     json_str(cast, "daypower"));
        if (n < 0 || (size_t)n >= output_size - off) {
            output[output_size - 1] = '\0';
            return;
        }
        off += (size_t)n;
    }
}

esp_err_t tool_amap_weather_init(void)
{
    if (ESPAGENT_SECRET_AMAP_KEY[0] != '\0') {
        snprintf(s_key, sizeof(s_key), "%s", ESPAGENT_SECRET_AMAP_KEY);
    }
    snprintf(s_default_location, sizeof(s_default_location), "%s",
             ESPAGENT_AMAP_DEFAULT_LOCATION);
    snprintf(s_default_adcode, sizeof(s_default_adcode), "%s",
             ESPAGENT_AMAP_DEFAULT_ADCODE);

    nvs_handle_t nvs;
    if (nvs_open(ESPAGENT_NVS_AMAP, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_key);
        nvs_get_str(nvs, ESPAGENT_NVS_KEY_API_KEY, s_key, &len);
        len = sizeof(s_default_location);
        nvs_get_str(nvs, ESPAGENT_NVS_KEY_LOCATION, s_default_location, &len);
        len = sizeof(s_default_adcode);
        nvs_get_str(nvs, ESPAGENT_NVS_KEY_ADCODE, s_default_adcode, &len);
        nvs_close(nvs);
    }

    ESP_LOGI(TAG, "Amap weather initialized (configured=%d default=%s/%s)",
             s_key[0] != '\0', s_default_location, s_default_adcode);
    return ESP_OK;
}

esp_err_t tool_amap_weather_execute(const char *input_json, char *output,
                                    size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_key[0] == '\0') {
        snprintf(output, output_size,
                 "Error: No Amap API key configured. Set ESPAGENT_SECRET_AMAP_KEY or use set_amap_key.");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *input = cJSON_Parse((input_json && input_json[0]) ? input_json : "{}");
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *location = json_str(input, "location");
    const char *adcode_in = json_str(input, "adcode");
    const char *extensions_in = json_str(input, "extensions");
    const char *type = json_str(input, "type");
    const char *extensions = "base";
    if (strcmp(extensions_in, "all") == 0 || strcmp(type, "forecast") == 0) {
        extensions = "all";
    }

    char adcode[16] = {0};
    char resolved[128] = {0};
    esp_err_t err = ESP_OK;

    if (is_adcode(adcode_in)) {
        snprintf(adcode, sizeof(adcode), "%s", adcode_in);
        snprintf(resolved, sizeof(resolved), "%s", adcode_in);
    } else if (location && location[0]) {
        if (is_adcode(location)) {
            snprintf(adcode, sizeof(adcode), "%s", location);
            snprintf(resolved, sizeof(resolved), "%s", location);
        } else {
            err = amap_geocode(location, adcode, sizeof(adcode), resolved,
                               sizeof(resolved));
        }
    } else {
        snprintf(adcode, sizeof(adcode), "%s", s_default_adcode);
        snprintf(resolved, sizeof(resolved), "%s", s_default_location);
    }
    cJSON_Delete(input);

    if (err != ESP_OK || !is_adcode(adcode)) {
        snprintf(output, output_size, "Error: Failed to resolve location to adcode");
        return err == ESP_OK ? ESP_FAIL : err;
    }

    char path[384];
    snprintf(path, sizeof(path),
             "/v3/weather/weatherInfo?key=%s&city=%s&extensions=%s&output=JSON",
             s_key, adcode, extensions);

    cJSON *root = NULL;
    err = amap_get_json(path, &root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: Amap weather request failed");
        return err;
    }

    if (strcmp(extensions, "all") == 0) {
        format_forecast(root, output, output_size);
    } else {
        format_live(root, output, output_size);
    }
    cJSON_Delete(root);

    if (resolved[0]) {
        ESP_LOGI(TAG, "Amap weather resolved %s -> %s", resolved, adcode);
    }
    return ESP_OK;
}

esp_err_t tool_amap_weather_set_key(const char *api_key)
{
    if (!api_key || !api_key[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(ESPAGENT_NVS_AMAP, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, ESPAGENT_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    snprintf(s_key, sizeof(s_key), "%s", api_key);
    ESP_LOGI(TAG, "Amap API key saved");
    return ESP_OK;
}

esp_err_t tool_amap_weather_set_default_location(const char *location,
                                                 const char *adcode)
{
    if (!location || !location[0] || !is_adcode(adcode)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(ESPAGENT_NVS_AMAP, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, ESPAGENT_NVS_KEY_LOCATION, location));
    ESP_ERROR_CHECK(nvs_set_str(nvs, ESPAGENT_NVS_KEY_ADCODE, adcode));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    snprintf(s_default_location, sizeof(s_default_location), "%s", location);
    snprintf(s_default_adcode, sizeof(s_default_adcode), "%s", adcode);
    ESP_LOGI(TAG, "Amap default location saved: %s/%s", location, adcode);
    return ESP_OK;
}

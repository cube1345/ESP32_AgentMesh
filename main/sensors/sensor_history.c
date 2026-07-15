#include "sensors/sensor_history.h"

#include "espagent_config.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "sensor_history";

#define ENV_LINE_MAX 384
#define ENV_PATH_MAX 320
#define ENV_STATUS_MAX 64
#define ENV_STATUS_JSON_MAX (48 * 6 + 1)

typedef struct {
    int64_t ts;
    int64_t uptime_ms;
    int temp_x10;
    int humidity_x10;
    int light_x10;
    int co2_ppm;
    int tvoc_ppb;
    char status[ENV_STATUS_MAX];
} env_hist_sample_t;

typedef struct {
    int count;
    int min;
    int max;
    int64_t sum;
} metric_acc_t;

static SemaphoreHandle_t s_history_lock;
static int64_t s_last_append_ms;
static int64_t s_last_cleanup_ms;

static bool time_is_valid(time_t now)
{
    return now > ESPAGENT_TIME_VALID_AFTER_EPOCH;
}

static void metric_add(metric_acc_t *m, int value)
{
    if (!m || value < 0) {
        return;
    }
    if (m->count == 0 || value < m->min) {
        m->min = value;
    }
    if (m->count == 0 || value > m->max) {
        m->max = value;
    }
    m->sum += value;
    m->count++;
}

static double metric_avg_x10(const metric_acc_t *m)
{
    return (m && m->count > 0) ? ((double)m->sum / (double)m->count / 10.0) : -1.0;
}

static void build_history_path(char *path, size_t path_size, time_t now)
{
    if (!path || path_size == 0) {
        return;
    }
    if (!time_is_valid(now)) {
        snprintf(path, path_size, "%s/unsynced.jsonl", ESPAGENT_ENV_HISTORY_DIR);
        return;
    }
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    snprintf(path, path_size, "%s/%04d-%02d-%02d.jsonl",
             ESPAGENT_ENV_HISTORY_DIR,
             tm_now.tm_year + 1900,
             tm_now.tm_mon + 1,
             tm_now.tm_mday);
}

static void json_escape_status(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    size_t off = 0;
    for (size_t i = 0; src && src[i] && i < 48 && off < dst_size - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (off + 2 >= dst_size) {
                break;
            }
            dst[off++] = '\\';
            dst[off++] = (char)c;
        } else if (c < 0x20) {
            if (off + 6 >= dst_size) {
                break;
            }
            int n = snprintf(dst + off, dst_size - off, "\\u%04x", c);
            if (n <= 0) {
                break;
            }
            off += (size_t)n;
        } else {
            dst[off++] = (char)c;
        }
    }
    dst[off] = '\0';
}

static void enforce_unsynced_history_cap(const char *path, time_t now)
{
    if (!path || time_is_valid(now) || ESPAGENT_ENV_HISTORY_UNSYNCED_MAX_BYTES <= 0) {
        return;
    }
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > ESPAGENT_ENV_HISTORY_UNSYNCED_MAX_BYTES) {
        if (remove(path) == 0) {
            ESP_LOGW(TAG, "reset oversized unsynced env history: %s size=%lld",
                     path,
                     (long long)st.st_size);
        }
    }
}

static bool env_entry_name(const char *name)
{
    return name &&
           strncmp(name, "env/", 4) == 0 &&
           strstr(name, ".jsonl") != NULL;
}

static time_t entry_date_epoch(const char *name)
{
    int y = 0;
    int m = 0;
    int d = 0;
    if (!name || sscanf(name, "env/%d-%d-%d.jsonl", &y, &m, &d) != 3) {
        return 0;
    }
    if (y < 2020 || m < 1 || m > 12 || d < 1 || d > 31) {
        return 0;
    }
    struct tm tm_day = {0};
    tm_day.tm_year = y - 1900;
    tm_day.tm_mon = m - 1;
    tm_day.tm_mday = d;
    tm_day.tm_isdst = -1;
    return mktime(&tm_day);
}

static void cleanup_old_files(time_t now)
{
    if (!time_is_valid(now) || ESPAGENT_ENV_HISTORY_RETENTION_DAYS <= 0) {
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_cleanup_ms != 0 &&
        now_ms - s_last_cleanup_ms < 12LL * 60LL * 60LL * 1000LL) {
        return;
    }
    s_last_cleanup_ms = now_ms;

    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE);
    if (!dir) {
        return;
    }

    time_t cutoff = now - (time_t)ESPAGENT_ENV_HISTORY_RETENTION_DAYS * 86400;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!env_entry_name(ent->d_name)) {
            continue;
        }
        time_t day = entry_date_epoch(ent->d_name);
        if (day == 0 || day >= cutoff) {
            continue;
        }
        char path[ENV_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", ESPAGENT_SPIFFS_BASE, ent->d_name);
        if (remove(path) == 0) {
            ESP_LOGI(TAG, "removed old env history: %s", path);
        }
    }
    closedir(dir);
}

esp_err_t sensor_history_init(void)
{
    if (!s_history_lock) {
        s_history_lock = xSemaphoreCreateMutex();
        if (!s_history_lock) {
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG,
                 "Sensor history enabled interval=%ums retention=%ud path=%s",
                 (unsigned)ESPAGENT_ENV_HISTORY_INTERVAL_MS,
                 (unsigned)ESPAGENT_ENV_HISTORY_RETENTION_DAYS,
                 ESPAGENT_ENV_HISTORY_DIR);
    }
    return ESP_OK;
}

esp_err_t sensor_history_maybe_append(const tool_environment_values_t *values,
                                      const char *status)
{
    if (!values) {
        return ESP_ERR_INVALID_ARG;
    }
    if (values->temperature_c_x10 < 0 && values->humidity_percent_x10 < 0 &&
        values->light_lux_x10 < 0 && values->co2eq_ppm < 0 &&
        values->tvoc_ppb < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sensor_history_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_append_ms != 0 &&
        now_ms - s_last_append_ms < (int64_t)ESPAGENT_ENV_HISTORY_INTERVAL_MS) {
        return ESP_ERR_NOT_FINISHED;
    }

    if (xSemaphoreTake(s_history_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_last_append_ms != 0 &&
        now_ms - s_last_append_ms < (int64_t)ESPAGENT_ENV_HISTORY_INTERVAL_MS) {
        xSemaphoreGive(s_history_lock);
        return ESP_ERR_NOT_FINISHED;
    }

    time_t now = time(NULL);
    cleanup_old_files(now);

    char path[ENV_PATH_MAX];
    build_history_path(path, sizeof(path), now);
    enforce_unsynced_history_cap(path, now);
    char status_json[ENV_STATUS_JSON_MAX] = {0};
    json_escape_status(status, status_json, sizeof(status_json));
    FILE *f = fopen(path, "a");
    if (!f) {
        xSemaphoreGive(s_history_lock);
        ESP_LOGW(TAG, "cannot append env history: %s", path);
        return ESP_FAIL;
    }

    int written = fprintf(
        f,
        "{\"ts\":%lld,\"uptime_ms\":%lld,\"temp_x10\":%d,\"humidity_x10\":%d,"
        "\"light_x10\":%d,\"co2\":%d,\"tvoc\":%d,\"light_raw\":%d,"
        "\"warming\":%s,\"status\":\"%s\"}\n",
        (long long)(time_is_valid(now) ? (int64_t)now : 0),
        (long long)now_ms,
        values->temperature_c_x10,
        values->humidity_percent_x10,
        values->light_lux_x10,
        values->co2eq_ppm,
        values->tvoc_ppb,
        values->light_raw,
        values->sgp30_warming_up ? "true" : "false",
        status_json);
    fclose(f);

    if (written <= 0) {
        xSemaphoreGive(s_history_lock);
        return ESP_FAIL;
    }
    s_last_append_ms = now_ms;
    xSemaphoreGive(s_history_lock);
    ESP_LOGI(TAG, "appended env history: %s", path);
    return ESP_OK;
}

static int json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static int64_t json_i64(cJSON *root, const char *key, int64_t fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsNumber(item) ? (int64_t)item->valuedouble : fallback;
}

static bool parse_sample_line(const char *line, env_hist_sample_t *sample)
{
    if (!line || !sample) {
        return false;
    }
    cJSON *root = cJSON_Parse(line);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    memset(sample, 0, sizeof(*sample));
    sample->ts = json_i64(root, "ts", 0);
    sample->uptime_ms = json_i64(root, "uptime_ms", 0);
    sample->temp_x10 = json_int(root, "temp_x10", -1);
    sample->humidity_x10 = json_int(root, "humidity_x10", -1);
    sample->light_x10 = json_int(root, "light_x10", -1);
    sample->co2_ppm = json_int(root, "co2", -1);
    sample->tvoc_ppb = json_int(root, "tvoc", -1);
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsString(status)) {
        snprintf(sample->status, sizeof(sample->status), "%s", status->valuestring);
    }
    cJSON_Delete(root);
    return true;
}

typedef void (*sample_visitor_t)(const env_hist_sample_t *sample, void *ctx);

static int scan_history(sample_visitor_t visitor, void *ctx)
{
    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE);
    if (!dir) {
        return -1;
    }
    int scanned = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!env_entry_name(ent->d_name)) {
            continue;
        }
        char path[ENV_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", ESPAGENT_SPIFFS_BASE, ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) {
            continue;
        }
        char line[ENV_LINE_MAX];
        while (fgets(line, sizeof(line), f)) {
            env_hist_sample_t sample;
            if (parse_sample_line(line, &sample)) {
                visitor(&sample, ctx);
                scanned++;
            }
        }
        fclose(f);
    }
    closedir(dir);
    return scanned;
}

typedef struct {
    bool has_time;
    int64_t cutoff;
    int samples;
    metric_acc_t temp;
    metric_acc_t humidity;
    metric_acc_t light;
    metric_acc_t co2;
    metric_acc_t tvoc;
    bool has_first_temp;
    bool has_last_temp;
    int64_t first_ts;
    int64_t last_ts;
    int first_temp_x10;
    int last_temp_x10;
    int first_humidity_x10;
    int last_humidity_x10;
} summary_ctx_t;

static void summary_visit(const env_hist_sample_t *s, void *ctx)
{
    summary_ctx_t *sum = (summary_ctx_t *)ctx;
    if (sum->has_time) {
        if (s->ts <= 0 || s->ts < sum->cutoff) {
            return;
        }
    }
    sum->samples++;
    metric_add(&sum->temp, s->temp_x10);
    metric_add(&sum->humidity, s->humidity_x10);
    metric_add(&sum->light, s->light_x10);
    metric_add(&sum->co2, s->co2_ppm * 10);
    metric_add(&sum->tvoc, s->tvoc_ppb * 10);
    if (s->temp_x10 >= 0 && s->humidity_x10 >= 0) {
        int64_t key = s->ts > 0 ? s->ts : s->uptime_ms / 1000;
        if (!sum->has_first_temp || key < sum->first_ts) {
            sum->has_first_temp = true;
            sum->first_ts = key;
            sum->first_temp_x10 = s->temp_x10;
            sum->first_humidity_x10 = s->humidity_x10;
        }
        if (!sum->has_last_temp || key > sum->last_ts) {
            sum->has_last_temp = true;
            sum->last_ts = key;
            sum->last_temp_x10 = s->temp_x10;
            sum->last_humidity_x10 = s->humidity_x10;
        }
    }
}

esp_err_t sensor_history_build_summary(uint32_t hours,
                                       char *out,
                                       size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hours == 0) {
        hours = 24;
    }
    if (hours > ESPAGENT_ENV_HISTORY_MAX_SUMMARY_HOURS) {
        hours = ESPAGENT_ENV_HISTORY_MAX_SUMMARY_HOURS;
    }

    time_t now = time(NULL);
    summary_ctx_t sum = {
        .has_time = time_is_valid(now),
        .cutoff = (int64_t)now - (int64_t)hours * 3600,
    };
    int scanned = scan_history(summary_visit, &sum);
    if (scanned < 0) {
        snprintf(out, out_size, "Error: cannot open environment history store");
        return ESP_FAIL;
    }
    if (sum.samples == 0) {
        snprintf(out, out_size, "OK: no environment history samples found for last %u hours", (unsigned)hours);
        return ESP_OK;
    }

    double temp_trend = (sum.has_first_temp && sum.has_last_temp)
                            ? (double)(sum.last_temp_x10 - sum.first_temp_x10) / 10.0
                            : 0.0;
    double humidity_trend = (sum.has_first_temp && sum.has_last_temp)
                                ? (double)(sum.last_humidity_x10 - sum.first_humidity_x10) / 10.0
                                : 0.0;
    snprintf(out, out_size,
             "OK: env_history_summary hours=%u samples=%d time_filter=%s\n"
             "temperature_c avg=%.1f min=%.1f max=%.1f trend=%.1f\n"
             "humidity_pct avg=%.1f min=%.1f max=%.1f trend=%.1f\n"
             "light_lux avg=%.1f min=%.1f max=%.1f\n"
             "eco2_ppm avg=%.0f min=%.0f max=%.0f; tvoc_ppb avg=%.0f min=%.0f max=%.0f",
             (unsigned)hours,
             sum.samples,
             sum.has_time ? "epoch" : "all_unsynced",
             metric_avg_x10(&sum.temp),
             sum.temp.count ? (double)sum.temp.min / 10.0 : -1.0,
             sum.temp.count ? (double)sum.temp.max / 10.0 : -1.0,
             temp_trend,
             metric_avg_x10(&sum.humidity),
             sum.humidity.count ? (double)sum.humidity.min / 10.0 : -1.0,
             sum.humidity.count ? (double)sum.humidity.max / 10.0 : -1.0,
             humidity_trend,
             metric_avg_x10(&sum.light),
             sum.light.count ? (double)sum.light.min / 10.0 : -1.0,
             sum.light.count ? (double)sum.light.max / 10.0 : -1.0,
             metric_avg_x10(&sum.co2),
             sum.co2.count ? (double)sum.co2.min / 10.0 : -1.0,
             sum.co2.count ? (double)sum.co2.max / 10.0 : -1.0,
             metric_avg_x10(&sum.tvoc),
             sum.tvoc.count ? (double)sum.tvoc.min / 10.0 : -1.0,
             sum.tvoc.count ? (double)sum.tvoc.max / 10.0 : -1.0);
    return ESP_OK;
}

typedef struct {
    env_hist_sample_t *items;
    uint32_t capacity;
    uint32_t count;
} recent_ctx_t;

static int64_t sample_sort_key(const env_hist_sample_t *s)
{
    if (!s) {
        return 0;
    }
    return s->ts > 0 ? s->ts : s->uptime_ms / 1000;
}

static void recent_visit(const env_hist_sample_t *s, void *ctx)
{
    recent_ctx_t *recent = (recent_ctx_t *)ctx;
    if (!recent || !recent->items || recent->capacity == 0) {
        return;
    }
    if (recent->count < recent->capacity) {
        recent->items[recent->count++] = *s;
        return;
    }
    uint32_t oldest = 0;
    int64_t oldest_key = sample_sort_key(&recent->items[0]);
    for (uint32_t i = 1; i < recent->capacity; i++) {
        int64_t key = sample_sort_key(&recent->items[i]);
        if (key < oldest_key) {
            oldest = i;
            oldest_key = key;
        }
    }
    if (sample_sort_key(s) > oldest_key) {
        recent->items[oldest] = *s;
    }
}

static int sample_compare(const void *a, const void *b)
{
    const env_hist_sample_t *sa = (const env_hist_sample_t *)a;
    const env_hist_sample_t *sb = (const env_hist_sample_t *)b;
    int64_t ka = sample_sort_key(sa);
    int64_t kb = sample_sort_key(sb);
    return (ka > kb) - (ka < kb);
}

static size_t append_recent_line(char *out,
                                 size_t out_size,
                                 size_t off,
                                 const env_hist_sample_t *s)
{
    if (!out || !s || off >= out_size) {
        return off;
    }
    double temp = s->temp_x10 >= 0 ? (double)s->temp_x10 / 10.0 : -1.0;
    double humidity = s->humidity_x10 >= 0 ? (double)s->humidity_x10 / 10.0 : -1.0;
    double light = s->light_x10 >= 0 ? (double)s->light_x10 / 10.0 : -1.0;
    int n = snprintf(out + off, out_size - off,
                     "- ts=%lld uptime_ms=%lld temp=%.1fC humidity=%.1f%% "
                     "light=%.1flux eco2=%dppm tvoc=%dppb status=%.32s\n",
                     (long long)s->ts,
                     (long long)s->uptime_ms,
                     temp,
                     humidity,
                     light,
                     s->co2_ppm,
                     s->tvoc_ppb,
                     s->status);
    if (n < 0) {
        return off;
    }
    size_t add = (size_t)n;
    return off + add < out_size ? off + add : out_size - 1;
}

esp_err_t sensor_history_build_recent(uint32_t limit,
                                      char *out,
                                      size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (limit == 0) {
        limit = 12;
    }
    if (limit > ESPAGENT_ENV_HISTORY_MAX_RECENT) {
        limit = ESPAGENT_ENV_HISTORY_MAX_RECENT;
    }

    env_hist_sample_t *items = calloc(limit, sizeof(env_hist_sample_t));
    if (!items) {
        snprintf(out, out_size, "Error: out of memory reading env history");
        return ESP_ERR_NO_MEM;
    }

    recent_ctx_t ctx = {
        .items = items,
        .capacity = limit,
    };
    int scanned = scan_history(recent_visit, &ctx);
    if (scanned < 0) {
        free(items);
        snprintf(out, out_size, "Error: cannot open environment history store");
        return ESP_FAIL;
    }
    if (ctx.count == 0) {
        free(items);
        snprintf(out, out_size, "OK: no environment history samples found");
        return ESP_OK;
    }

    qsort(items, ctx.count, sizeof(env_hist_sample_t), sample_compare);
    size_t off = 0;
    off += snprintf(out + off, out_size - off,
                    "OK: env_history_recent count=%u scanned=%d\n",
                    (unsigned)ctx.count,
                    scanned);
    for (uint32_t i = 0; i < ctx.count && off < out_size - 1; i++) {
        off = append_recent_line(out, out_size, off, &items[i]);
    }
    free(items);
    return ESP_OK;
}

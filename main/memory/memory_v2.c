#include "memory/memory_v2.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espagent_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "memory_v2";

#define PROFILE_FACT_MAX 24
#define SKILL_OBS_MAX 16
#define PROFILE_LINE_MAX 512
#define RELEVANT_ITEMS_MAX 6

typedef struct {
    char key[48];
    char value[128];
    char source[48];
    float confidence;
    int64_t ts;
    char previous_value[128];
    char conflict_note[160];
} profile_fact_t;

typedef struct {
    int index;
    int score;
    int64_t ts;
} relevant_rank_t;

typedef struct {
    char skill[48];
    char status[24];
    char summary[128];
    int64_t ts;
} skill_observation_t;

static const char *profile_path(void)
{
    return ESPAGENT_SPIFFS_MEMORY_DIR "/profile.jsonl";
}

static const char *skill_index_path(void)
{
    return ESPAGENT_SPIFFS_MEMORY_DIR "/skills_index.jsonl";
}

static const char *profile_conflict_path(void)
{
    return ESPAGENT_SPIFFS_MEMORY_DIR "/profile_conflicts.jsonl";
}

static esp_err_t rewrite_jsonl_file(const char *path, cJSON *items)
{
    if (!path || !items || !cJSON_IsArray(items)) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for rewrite", path);
        return ESP_FAIL;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, items) {
        char *line = cJSON_PrintUnformatted(item);
        if (!line) {
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
        fprintf(f, "%s\n", line);
        cJSON_free(line);
    }
    fclose(f);
    return ESP_OK;
}

static int64_t now_epoch(void)
{
    time_t now = 0;
    time(&now);
    if (now > 100000) {
        return (int64_t)now;
    }
    return esp_timer_get_time() / 1000000;
}

static void copy_str(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) {
        return;
    }
    snprintf(dst, size, "%s", src ? src : "");
}

static bool contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !needle[0]) {
        return false;
    }

    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            unsigned char hc = (unsigned char)p[i];
            unsigned char nc = (unsigned char)needle[i];
            if (hc >= 'A' && hc <= 'Z') hc = (unsigned char)(hc - 'A' + 'a');
            if (nc >= 'A' && nc <= 'Z') nc = (unsigned char)(nc - 'A' + 'a');
            if (hc != nc) {
                break;
            }
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static bool contains_any_ci(const char *text, const char *const *patterns, size_t count)
{
    if (!text || !patterns) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (patterns[i] && contains_ci(text, patterns[i])) {
            return true;
        }
    }
    return false;
}

static const char *canonical_profile_key_for(const char *sanitized)
{
    static const char *humidity_keys[] = {
        "humidity", "humid", "rh"
    };
    static const char *temperature_keys[] = {
        "temperature", "temp"
    };
    static const char *light_keys[] = {
        "light", "lux", "brightness", "illum"
    };
    static const char *air_quality_keys[] = {
        "air_quality", "co2", "tvoc", "iaq", "voc"
    };
    static const char *sleep_keys[] = {
        "sleep", "bedtime", "night"
    };
    static const char *wake_keys[] = {
        "wake", "morning", "get_up"
    };
    static const char *privacy_keys[] = {
        "privacy", "share", "sharing", "sensitive", "upload", "cloud", "llm"
    };
    static const char *light_device_keys[] = {
        "lamp", "light_color", "light_colour", "led_color", "led_colour",
        "ws2812", "rgb"
    };
    static const char *humidifier_keys[] = {
        "humidifier"
    };
    static const char *fan_keys[] = {
        "fan", "vent"
    };
    static const char *ac_keys[] = {
        "air_conditioner", "conditioner", "gree_ac", "ac"
    };

    if (contains_any_ci(sanitized, humidity_keys, sizeof(humidity_keys) / sizeof(humidity_keys[0]))) {
        return "env.humidity_preference";
    }
    if (contains_any_ci(sanitized, temperature_keys, sizeof(temperature_keys) / sizeof(temperature_keys[0]))) {
        return "env.temperature_preference";
    }
    if (contains_any_ci(sanitized, light_keys, sizeof(light_keys) / sizeof(light_keys[0]))) {
        return "env.light_preference";
    }
    if (contains_any_ci(sanitized, air_quality_keys, sizeof(air_quality_keys) / sizeof(air_quality_keys[0]))) {
        return "env.air_quality_preference";
    }
    if (contains_any_ci(sanitized, sleep_keys, sizeof(sleep_keys) / sizeof(sleep_keys[0]))) {
        return "habit.sleep_schedule";
    }
    if (contains_any_ci(sanitized, wake_keys, sizeof(wake_keys) / sizeof(wake_keys[0]))) {
        return "habit.wake_schedule";
    }
    if (contains_any_ci(sanitized, privacy_keys, sizeof(privacy_keys) / sizeof(privacy_keys[0]))) {
        return "privacy.data_sharing";
    }
    if (contains_any_ci(sanitized, light_device_keys, sizeof(light_device_keys) / sizeof(light_device_keys[0]))) {
        return "device.light_preference";
    }
    if (contains_any_ci(sanitized, humidifier_keys, sizeof(humidifier_keys) / sizeof(humidifier_keys[0]))) {
        return "device.humidifier_policy";
    }
    if (contains_any_ci(sanitized, fan_keys, sizeof(fan_keys) / sizeof(fan_keys[0]))) {
        return "device.fan_policy";
    }
    if (contains_any_ci(sanitized, ac_keys, sizeof(ac_keys) / sizeof(ac_keys[0]))) {
        return "device.ac_policy";
    }
    return NULL;
}

static int query_match_score(const char *query,
                             const char *primary,
                             const char *secondary,
                             const char *tertiary)
{
    if (!query || !query[0]) {
        return 0;
    }

    int score = 0;
    char token[48];
    size_t ti = 0;
    for (const unsigned char *p = (const unsigned char *)query; ; p++) {
        unsigned char c = *p;
        bool token_char = (c >= '0' && c <= '9') ||
                          (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c & 0x80);
        if (token_char && ti + 1 < sizeof(token)) {
            token[ti++] = (char)c;
            continue;
        }
        if (ti > 0) {
            token[ti] = '\0';
            if (ti >= 2) {
                if (contains_ci(primary, token)) {
                    score += 5;
                }
                if (contains_ci(secondary, token)) {
                    score += 3;
                }
                if (contains_ci(tertiary, token)) {
                    score += 1;
                }
            }
            ti = 0;
        }
        if (c == '\0') {
            break;
        }
    }
    return score;
}

void memory_v2_normalize_profile_key(const char *raw_key,
                                     char *normalized_key,
                                     size_t normalized_key_size)
{
    if (!normalized_key || normalized_key_size == 0) {
        return;
    }
    normalized_key[0] = '\0';
    if (!raw_key || !raw_key[0]) {
        return;
    }

    char sanitized[64];
    size_t wi = 0;
    bool last_sep = false;
    for (const unsigned char *p = (const unsigned char *)raw_key; *p && wi + 1 < sizeof(sanitized); p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        bool keep = (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') ||
                    c == '.' || c == '_';
        if (keep) {
            sanitized[wi++] = (char)c;
            last_sep = false;
        } else if (!last_sep) {
            sanitized[wi++] = '_';
            last_sep = true;
        }
    }
    while (wi > 0 && sanitized[wi - 1] == '_') {
        wi--;
    }
    sanitized[wi] = '\0';

    const char *canonical = canonical_profile_key_for(sanitized);
    if (canonical && canonical[0]) {
        snprintf(normalized_key, normalized_key_size, "%s", canonical);
        return;
    }

    const char *prefix = "preference.";
    if (strchr(sanitized, '.')) {
        prefix = "";
    } else if (contains_ci(sanitized, "humidity") ||
               contains_ci(sanitized, "temperature") ||
               contains_ci(sanitized, "air") ||
               contains_ci(sanitized, "light") ||
               contains_ci(sanitized, "co2") ||
               contains_ci(sanitized, "tvoc")) {
        prefix = "env.";
    } else if (contains_ci(sanitized, "sleep") ||
               contains_ci(sanitized, "wake") ||
               contains_ci(sanitized, "morning") ||
               contains_ci(sanitized, "night") ||
               contains_ci(sanitized, "daily")) {
        prefix = "habit.";
    } else if (contains_ci(sanitized, "privacy") ||
               contains_ci(sanitized, "sensitive") ||
               contains_ci(sanitized, "share")) {
        prefix = "privacy.";
    } else if (contains_ci(sanitized, "lamp") ||
               contains_ci(sanitized, "light_color") ||
               contains_ci(sanitized, "humidifier") ||
               contains_ci(sanitized, "fan") ||
               contains_ci(sanitized, "ac") ||
               contains_ci(sanitized, "device")) {
        prefix = "device.";
    }

    snprintf(normalized_key, normalized_key_size, "%s%s", prefix, sanitized);
}

static esp_err_t append_profile_conflict_record(const char *key,
                                                const char *old_value,
                                                const char *new_value,
                                                const char *old_source,
                                                const char *new_source)
{
    FILE *f = fopen(profile_conflict_path(), "a");
    if (!f) {
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "schema", "espagent.profile_conflict.v1");
    cJSON_AddStringToObject(obj, "key", key ? key : "");
    cJSON_AddStringToObject(obj, "old_value", old_value ? old_value : "");
    cJSON_AddStringToObject(obj, "new_value", new_value ? new_value : "");
    cJSON_AddStringToObject(obj, "old_source", old_source ? old_source : "");
    cJSON_AddStringToObject(obj, "new_source", new_source ? new_source : "");
    cJSON_AddNumberToObject(obj, "ts", (double)now_epoch());

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!line) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fprintf(f, "%s\n", line);
    cJSON_free(line);
    fclose(f);
    return ESP_OK;
}

esp_err_t memory_v2_init(void)
{
    ESP_LOGI(TAG, "Memory v2 initialized profile=%s skills=%s",
             profile_path(), skill_index_path());
    return ESP_OK;
}

esp_err_t memory_v2_upsert_profile_fact(const char *key,
                                        const char *value,
                                        const char *source,
                                        float confidence,
                                        char *change_note,
                                        size_t change_note_size)
{
    if (!key || !key[0] || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (change_note && change_note_size > 0) {
        change_note[0] = '\0';
    }

    cJSON *items = cJSON_CreateArray();
    if (!items) {
        return ESP_ERR_NO_MEM;
    }

    char previous_value[128] = {0};
    char previous_source[48] = {0};
    float previous_confidence = 0.0f;
    bool had_previous = false;

    FILE *f = fopen(profile_path(), "r");
    if (f) {
        char line[PROFILE_LINE_MAX];
        while (fgets(line, sizeof(line), f)) {
            cJSON *obj = cJSON_Parse(line);
            if (!obj) {
                continue;
            }
            cJSON *item_key = cJSON_GetObjectItem(obj, "key");
            if (cJSON_IsString(item_key) && strcmp(item_key->valuestring, key) == 0) {
                cJSON *item_value = cJSON_GetObjectItem(obj, "value");
                cJSON *item_source = cJSON_GetObjectItem(obj, "source");
                cJSON *item_conf = cJSON_GetObjectItem(obj, "confidence");
                copy_str(previous_value, sizeof(previous_value),
                         cJSON_IsString(item_value) ? item_value->valuestring : "");
                copy_str(previous_source, sizeof(previous_source),
                         cJSON_IsString(item_source) ? item_source->valuestring : "agent");
                previous_confidence = cJSON_IsNumber(item_conf) ? (float)item_conf->valuedouble : 0.0f;
                had_previous = true;
                cJSON_Delete(obj);
                continue;
            }
            cJSON_AddItemToArray(items, obj);
        }
        fclose(f);
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        cJSON_Delete(items);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "schema", "espagent.profile_fact.v1");
    cJSON_AddStringToObject(obj, "key", key);
    cJSON_AddStringToObject(obj, "value", value);
    cJSON_AddStringToObject(obj, "source", source ? source : "agent");
    cJSON_AddNumberToObject(obj, "confidence", confidence);
    cJSON_AddNumberToObject(obj, "ts", (double)now_epoch());
    if (had_previous && previous_value[0] && strcmp(previous_value, value) != 0) {
        cJSON_AddStringToObject(obj, "previous_value", previous_value);
        cJSON_AddStringToObject(obj, "conflict_note", "new observation overrides previous profile value");
    }
    cJSON_AddItemToArray(items, obj);

    esp_err_t err = rewrite_jsonl_file(profile_path(), items);
    cJSON_Delete(items);
    if (err == ESP_OK && change_note && change_note_size > 0) {
        if (!had_previous) {
            snprintf(change_note, change_note_size, "new fact");
        } else if (strcmp(previous_value, value) == 0) {
            snprintf(change_note, change_note_size,
                     "refreshed existing fact, conf %.2f->%.2f",
                     (double)previous_confidence, (double)confidence);
        } else {
            snprintf(change_note, change_note_size,
                     "overrode previous value: %s -> %s",
                     previous_value[0] ? previous_value : "(empty)",
                     value);
            (void)append_profile_conflict_record(key,
                                                 previous_value,
                                                 value,
                                                 previous_source,
                                                 source ? source : "agent");
        }
    }
    return err;
}

esp_err_t memory_v2_append_skill_observation(const char *skill,
                                             const char *status,
                                             const char *summary)
{
    if (!skill || !skill[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *items = cJSON_CreateArray();
    if (!items) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(skill_index_path(), "r");
    if (f) {
        char line[PROFILE_LINE_MAX];
        while (fgets(line, sizeof(line), f)) {
            cJSON *obj = cJSON_Parse(line);
            if (!obj) {
                continue;
            }
            cJSON *item_skill = cJSON_GetObjectItem(obj, "skill");
            if (cJSON_IsString(item_skill) && strcmp(item_skill->valuestring, skill) == 0) {
                cJSON_Delete(obj);
                continue;
            }
            cJSON_AddItemToArray(items, obj);
        }
        fclose(f);
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        cJSON_Delete(items);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "schema", "espagent.skill_observation.v1");
    cJSON_AddStringToObject(obj, "skill", skill);
    cJSON_AddStringToObject(obj, "status", status ? status : "observed");
    cJSON_AddStringToObject(obj, "summary", summary ? summary : "");
    cJSON_AddNumberToObject(obj, "ts", (double)now_epoch());
    cJSON_AddItemToArray(items, obj);

    esp_err_t err = rewrite_jsonl_file(skill_index_path(), items);
    cJSON_Delete(items);
    return err;
}

static void profile_apply(profile_fact_t *facts, int *count, cJSON *obj)
{
    cJSON *key = cJSON_GetObjectItem(obj, "key");
    cJSON *value = cJSON_GetObjectItem(obj, "value");
    if (!cJSON_IsString(key) || !cJSON_IsString(value)) {
        return;
    }

    int slot = -1;
    for (int i = 0; i < *count; i++) {
        if (strcmp(facts[i].key, key->valuestring) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (*count >= PROFILE_FACT_MAX) {
            slot = 0;
        } else {
            slot = (*count)++;
        }
    }

    copy_str(facts[slot].key, sizeof(facts[slot].key), key->valuestring);
    copy_str(facts[slot].value, sizeof(facts[slot].value), value->valuestring);
    cJSON *source = cJSON_GetObjectItem(obj, "source");
    copy_str(facts[slot].source, sizeof(facts[slot].source),
             cJSON_IsString(source) ? source->valuestring : "agent");
    cJSON *confidence = cJSON_GetObjectItem(obj, "confidence");
    facts[slot].confidence = cJSON_IsNumber(confidence) ? (float)confidence->valuedouble : 0.5f;
    cJSON *ts = cJSON_GetObjectItem(obj, "ts");
    facts[slot].ts = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;
    cJSON *previous_value = cJSON_GetObjectItem(obj, "previous_value");
    copy_str(facts[slot].previous_value, sizeof(facts[slot].previous_value),
             cJSON_IsString(previous_value) ? previous_value->valuestring : "");
    cJSON *conflict_note = cJSON_GetObjectItem(obj, "conflict_note");
    copy_str(facts[slot].conflict_note, sizeof(facts[slot].conflict_note),
             cJSON_IsString(conflict_note) ? conflict_note->valuestring : "");
}

esp_err_t memory_v2_build_profile_summary(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    FILE *f = fopen(profile_path(), "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    profile_fact_t facts[PROFILE_FACT_MAX] = {0};
    int count = 0;
    char line[PROFILE_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        cJSON *obj = cJSON_Parse(line);
        if (obj) {
            profile_apply(facts, &count, obj);
            cJSON_Delete(obj);
        }
    }
    fclose(f);

    size_t off = 0;
    for (int i = 0; i < count && off < size - 1; i++) {
        int n = snprintf(buf + off, size - off,
                         "- %s: %s (source=%s confidence=%.2f)\n",
                         facts[i].key, facts[i].value,
                         facts[i].source, (double)facts[i].confidence);
        if (n < 0) {
            break;
        }
        if ((size_t)n >= size - off) {
            off = size - 1;
            break;
        }
        off += (size_t)n;
    }
    return ESP_OK;
}

esp_err_t memory_v2_build_relevant_profile_summary(const char *query,
                                                   char *buf,
                                                   size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    if (!query || !query[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(profile_path(), "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    profile_fact_t facts[PROFILE_FACT_MAX] = {0};
    int count = 0;
    char line[PROFILE_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        cJSON *obj = cJSON_Parse(line);
        if (obj) {
            profile_apply(facts, &count, obj);
            cJSON_Delete(obj);
        }
    }
    fclose(f);

    relevant_rank_t ranks[PROFILE_FACT_MAX] = {0};
    int matched = 0;
    for (int i = 0; i < count; i++) {
        int score = query_match_score(query,
                                      facts[i].key,
                                      facts[i].value,
                                      facts[i].conflict_note[0] ? facts[i].conflict_note : facts[i].source);
        if (score <= 0) {
            continue;
        }
        ranks[matched].index = i;
        ranks[matched].score = score;
        ranks[matched].ts = facts[i].ts;
        matched++;
    }
    for (int i = 0; i < matched; i++) {
        for (int j = i + 1; j < matched; j++) {
            if (ranks[j].score > ranks[i].score ||
                (ranks[j].score == ranks[i].score && ranks[j].ts > ranks[i].ts)) {
                relevant_rank_t tmp = ranks[i];
                ranks[i] = ranks[j];
                ranks[j] = tmp;
            }
        }
    }

    size_t off = 0;
    int limit = matched < RELEVANT_ITEMS_MAX ? matched : RELEVANT_ITEMS_MAX;
    for (int r = 0; r < limit && off < size - 1; r++) {
        int i = ranks[r].index;
        int n = snprintf(buf + off, size - off,
                         "- %s: %s (source=%s confidence=%.2f score=%d%s%s)\n",
                         facts[i].key, facts[i].value,
                         facts[i].source, (double)facts[i].confidence, ranks[r].score,
                         facts[i].conflict_note[0] ? " note=" : "",
                         facts[i].conflict_note[0] ? facts[i].conflict_note : "");
        if (n < 0) {
            break;
        }
        if ((size_t)n >= size - off) {
            off = size - 1;
            break;
        }
        off += (size_t)n;
    }
    return limit > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t memory_v2_build_relevant_profile_conflict_summary(const char *query,
                                                            char *buf,
                                                            size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    if (!query || !query[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(profile_conflict_path(), "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    typedef struct {
        char key[48];
        char old_value[128];
        char new_value[128];
        char old_source[48];
        char new_source[48];
        int64_t ts;
    } conflict_item_t;

    conflict_item_t items[RELEVANT_ITEMS_MAX * 2] = {0};
    int count = 0;
    char line[PROFILE_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        cJSON *obj = cJSON_Parse(line);
        if (!obj) {
            continue;
        }
        cJSON *key = cJSON_GetObjectItem(obj, "key");
        cJSON *old_value = cJSON_GetObjectItem(obj, "old_value");
        cJSON *new_value = cJSON_GetObjectItem(obj, "new_value");
        cJSON *old_source = cJSON_GetObjectItem(obj, "old_source");
        cJSON *new_source = cJSON_GetObjectItem(obj, "new_source");
        cJSON *ts = cJSON_GetObjectItem(obj, "ts");
        if (cJSON_IsString(key) && count < (int)(sizeof(items) / sizeof(items[0]))) {
            copy_str(items[count].key, sizeof(items[count].key), key->valuestring);
            copy_str(items[count].old_value, sizeof(items[count].old_value),
                     cJSON_IsString(old_value) ? old_value->valuestring : "");
            copy_str(items[count].new_value, sizeof(items[count].new_value),
                     cJSON_IsString(new_value) ? new_value->valuestring : "");
            copy_str(items[count].old_source, sizeof(items[count].old_source),
                     cJSON_IsString(old_source) ? old_source->valuestring : "");
            copy_str(items[count].new_source, sizeof(items[count].new_source),
                     cJSON_IsString(new_source) ? new_source->valuestring : "");
            items[count].ts = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;
            count++;
        }
        cJSON_Delete(obj);
    }
    fclose(f);

    relevant_rank_t ranks[RELEVANT_ITEMS_MAX * 2] = {0};
    int matched = 0;
    for (int i = 0; i < count; i++) {
        int score = query_match_score(query, items[i].key, items[i].new_value, items[i].old_value);
        if (score <= 0) {
            continue;
        }
        ranks[matched].index = i;
        ranks[matched].score = score;
        ranks[matched].ts = items[i].ts;
        matched++;
    }
    for (int i = 0; i < matched; i++) {
        for (int j = i + 1; j < matched; j++) {
            if (ranks[j].score > ranks[i].score ||
                (ranks[j].score == ranks[i].score && ranks[j].ts > ranks[i].ts)) {
                relevant_rank_t tmp = ranks[i];
                ranks[i] = ranks[j];
                ranks[j] = tmp;
            }
        }
    }

    size_t off = 0;
    int limit = matched < RELEVANT_ITEMS_MAX ? matched : RELEVANT_ITEMS_MAX;
    for (int r = 0; r < limit && off < size - 1; r++) {
        int i = ranks[r].index;
        int n = snprintf(buf + off, size - off,
                         "- %s changed: %s -> %s (old=%s new=%s score=%d)\n",
                         items[i].key,
                         items[i].old_value,
                         items[i].new_value,
                         items[i].old_source,
                         items[i].new_source,
                         ranks[r].score);
        if (n < 0) {
            break;
        }
        if ((size_t)n >= size - off) {
            off = size - 1;
            break;
        }
        off += (size_t)n;
    }
    return limit > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t memory_v2_build_skill_summary(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    FILE *f = fopen(skill_index_path(), "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    skill_observation_t obs[SKILL_OBS_MAX] = {0};
    int count = 0;
    char line[PROFILE_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        cJSON *obj = cJSON_Parse(line);
        if (!obj) {
            continue;
        }
        cJSON *skill = cJSON_GetObjectItem(obj, "skill");
        if (cJSON_IsString(skill)) {
            int slot = count < SKILL_OBS_MAX ? count++ : 0;
            copy_str(obs[slot].skill, sizeof(obs[slot].skill), skill->valuestring);
            cJSON *status = cJSON_GetObjectItem(obj, "status");
            copy_str(obs[slot].status, sizeof(obs[slot].status),
                     cJSON_IsString(status) ? status->valuestring : "observed");
            cJSON *summary = cJSON_GetObjectItem(obj, "summary");
            copy_str(obs[slot].summary, sizeof(obs[slot].summary),
                     cJSON_IsString(summary) ? summary->valuestring : "");
            cJSON *ts = cJSON_GetObjectItem(obj, "ts");
            obs[slot].ts = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;
        }
        cJSON_Delete(obj);
    }
    fclose(f);

    size_t off = 0;
    for (int i = 0; i < count && off < size - 1; i++) {
        int n = snprintf(buf + off, size - off,
                         "- %s: %s, %s\n",
                         obs[i].skill, obs[i].status, obs[i].summary);
        if (n < 0) {
            break;
        }
        if ((size_t)n >= size - off) {
            off = size - 1;
            break;
        }
        off += (size_t)n;
    }
    return ESP_OK;
}

esp_err_t memory_v2_build_relevant_skill_summary(const char *query,
                                                 char *buf,
                                                 size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    if (!query || !query[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(skill_index_path(), "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    skill_observation_t obs[SKILL_OBS_MAX] = {0};
    int count = 0;
    char line[PROFILE_LINE_MAX];
    while (fgets(line, sizeof(line), f)) {
        cJSON *obj = cJSON_Parse(line);
        if (!obj) {
            continue;
        }
        cJSON *skill = cJSON_GetObjectItem(obj, "skill");
        if (cJSON_IsString(skill)) {
            int slot = count < SKILL_OBS_MAX ? count++ : 0;
            copy_str(obs[slot].skill, sizeof(obs[slot].skill), skill->valuestring);
            cJSON *status = cJSON_GetObjectItem(obj, "status");
            copy_str(obs[slot].status, sizeof(obs[slot].status),
                     cJSON_IsString(status) ? status->valuestring : "observed");
            cJSON *summary = cJSON_GetObjectItem(obj, "summary");
            copy_str(obs[slot].summary, sizeof(obs[slot].summary),
                     cJSON_IsString(summary) ? summary->valuestring : "");
            cJSON *ts = cJSON_GetObjectItem(obj, "ts");
            obs[slot].ts = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;
        }
        cJSON_Delete(obj);
    }
    fclose(f);

    relevant_rank_t ranks[SKILL_OBS_MAX] = {0};
    int matched = 0;
    for (int i = 0; i < count; i++) {
        int score = query_match_score(query, obs[i].skill, obs[i].summary, obs[i].status);
        if (score <= 0) {
            continue;
        }
        ranks[matched].index = i;
        ranks[matched].score = score;
        ranks[matched].ts = obs[i].ts;
        matched++;
    }
    for (int i = 0; i < matched; i++) {
        for (int j = i + 1; j < matched; j++) {
            if (ranks[j].score > ranks[i].score ||
                (ranks[j].score == ranks[i].score && ranks[j].ts > ranks[i].ts)) {
                relevant_rank_t tmp = ranks[i];
                ranks[i] = ranks[j];
                ranks[j] = tmp;
            }
        }
    }

    size_t off = 0;
    int limit = matched < RELEVANT_ITEMS_MAX ? matched : RELEVANT_ITEMS_MAX;
    for (int r = 0; r < limit && off < size - 1; r++) {
        int i = ranks[r].index;
        int n = snprintf(buf + off, size - off,
                         "- %s: %s, %s (score=%d)\n",
                         obs[i].skill, obs[i].status, obs[i].summary, ranks[r].score);
        if (n < 0) {
            break;
        }
        if ((size_t)n >= size - off) {
            off = size - 1;
            break;
        }
        off += (size_t)n;
    }
    return limit > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

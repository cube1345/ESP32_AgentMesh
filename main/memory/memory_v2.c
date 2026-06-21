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

typedef struct {
    char key[48];
    char value[128];
    char source[48];
    float confidence;
    int64_t ts;
} profile_fact_t;

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

esp_err_t memory_v2_init(void)
{
    ESP_LOGI(TAG, "Memory v2 initialized profile=%s skills=%s",
             profile_path(), skill_index_path());
    return ESP_OK;
}

esp_err_t memory_v2_upsert_profile_fact(const char *key,
                                        const char *value,
                                        const char *source,
                                        float confidence)
{
    if (!key || !key[0] || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(profile_path(), "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", profile_path());
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "schema", "espagent.profile_fact.v1");
    cJSON_AddStringToObject(obj, "key", key);
    cJSON_AddStringToObject(obj, "value", value);
    cJSON_AddStringToObject(obj, "source", source ? source : "agent");
    cJSON_AddNumberToObject(obj, "confidence", confidence);
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

esp_err_t memory_v2_append_skill_observation(const char *skill,
                                             const char *status,
                                             const char *summary)
{
    if (!skill || !skill[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(skill_index_path(), "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", skill_index_path());
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(obj, "schema", "espagent.skill_observation.v1");
    cJSON_AddStringToObject(obj, "skill", skill);
    cJSON_AddStringToObject(obj, "status", status ? status : "observed");
    cJSON_AddStringToObject(obj, "summary", summary ? summary : "");
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

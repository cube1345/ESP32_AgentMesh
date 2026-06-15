#include "session_mgr.h"
#include "espagent_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "session";

static uint64_t session_hash_chat_id(const char *chat_id)
{
    uint64_t hash = 1469598103934665603ULL;
    const unsigned char *p = (const unsigned char *)(chat_id ? chat_id : "");
    while (*p) {
        hash ^= (uint64_t)(*p++);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void session_path(const char *chat_id, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return;
    }

    uint64_t hash = session_hash_chat_id(chat_id);
    int n = snprintf(buf, size, "%s/session_%016" PRIx64 ".jsonl",
                     ESPAGENT_SPIFFS_SESSION_DIR, hash);
    if (n < 0 || (size_t)n >= size) {
        buf[0] = '\0';
    }
}

static bool session_legacy_chat_id_is_safe(const char *chat_id)
{
    if (!chat_id || chat_id[0] == '\0') {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)chat_id; *p; p++) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= 'a' && *p <= 'z') || *p == '_' || *p == '-' || *p == '.') {
            continue;
        }
        return false;
    }
    return true;
}

static void session_legacy_path(const char *chat_id, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return;
    }
    buf[0] = '\0';
    if (!session_legacy_chat_id_is_safe(chat_id)) {
        return;
    }

    int n = snprintf(buf, size, "%s/session_%s.jsonl",
                     ESPAGENT_SPIFFS_SESSION_DIR, chat_id);
    if (n < 0 || (size_t)n >= size) {
        buf[0] = '\0';
    }
}

esp_err_t session_mgr_init(void)
{
    ESP_LOGI(TAG, "Session manager initialized at %s", ESPAGENT_SPIFFS_SESSION_DIR);
    return ESP_OK;
}

esp_err_t session_append(const char *chat_id, const char *role, const char *content)
{
    if (!role || !content) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[128];
    session_path(chat_id, path, sizeof(path));
    if (path[0] == '\0') {
        ESP_LOGE(TAG, "Cannot build session path");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open session file %s", path);
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "role", role);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddNumberToObject(obj, "ts", (double)time(NULL));

    char *line = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (line) {
        fprintf(f, "%s\n", line);
        free(line);
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t session_get_history_json(const char *chat_id, char *buf, size_t size, int max_msgs)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (max_msgs <= 0) {
        snprintf(buf, size, "[]");
        return ESP_OK;
    }
    if (max_msgs > ESPAGENT_SESSION_MAX_MSGS) {
        max_msgs = ESPAGENT_SESSION_MAX_MSGS;
    }

    char path[128];
    session_path(chat_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        char legacy_path[128];
        session_legacy_path(chat_id, legacy_path, sizeof(legacy_path));
        if (legacy_path[0] != '\0') {
            f = fopen(legacy_path, "r");
            if (f) {
                ESP_LOGI(TAG, "Loaded legacy session path %s", legacy_path);
            }
        }
        if (!f) {
            /* No history yet */
            snprintf(buf, size, "[]");
            return ESP_OK;
        }
    }

    /* Read all lines into a ring buffer of cJSON objects */
    cJSON *messages[ESPAGENT_SESSION_MAX_MSGS];
    int count = 0;
    int write_idx = 0;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;

        cJSON *obj = cJSON_Parse(line);
        if (!obj) continue;

        /* Ring buffer: overwrite oldest if full */
        if (count >= max_msgs) {
            cJSON_Delete(messages[write_idx]);
        }
        messages[write_idx] = obj;
        write_idx = (write_idx + 1) % max_msgs;
        if (count < max_msgs) count++;
    }
    fclose(f);

    /* Build JSON array with only role + content */
    cJSON *arr = cJSON_CreateArray();
    int start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % max_msgs;
        cJSON *src = messages[idx];

        cJSON *entry = cJSON_CreateObject();
        cJSON *role = cJSON_GetObjectItem(src, "role");
        cJSON *content = cJSON_GetObjectItem(src, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            cJSON_AddStringToObject(entry, "role", role->valuestring);
            cJSON_AddStringToObject(entry, "content", content->valuestring);
        }
        cJSON_AddItemToArray(arr, entry);
    }

    /* Cleanup ring buffer */
    int cleanup_start = (count < max_msgs) ? 0 : write_idx;
    for (int i = 0; i < count; i++) {
        int idx = (cleanup_start + i) % max_msgs;
        cJSON_Delete(messages[idx]);
    }

    char *json_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    if (json_str) {
        strncpy(buf, json_str, size - 1);
        buf[size - 1] = '\0';
        free(json_str);
    } else {
        snprintf(buf, size, "[]");
    }

    return ESP_OK;
}

esp_err_t session_clear(const char *chat_id)
{
    char path[128];
    session_path(chat_id, path, sizeof(path));

    bool removed = false;
    if (path[0] != '\0' && remove(path) == 0) {
        removed = true;
    }

    char legacy_path[128];
    session_legacy_path(chat_id, legacy_path, sizeof(legacy_path));
    if (legacy_path[0] != '\0' && remove(legacy_path) == 0) {
        removed = true;
    }

    if (removed) {
        ESP_LOGI(TAG, "Session %s cleared", chat_id);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

void session_list(void)
{
    DIR *dir = opendir(ESPAGENT_SPIFFS_SESSION_DIR);
    if (!dir) {
        /* SPIFFS is flat, so list all files matching pattern */
        dir = opendir(ESPAGENT_SPIFFS_BASE);
        if (!dir) {
            ESP_LOGW(TAG, "Cannot open SPIFFS directory");
            return;
        }
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, "session_") && strstr(entry->d_name, ".jsonl")) {
            ESP_LOGI(TAG, "  Session: %s", entry->d_name);
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGI(TAG, "  No sessions found");
    }
}

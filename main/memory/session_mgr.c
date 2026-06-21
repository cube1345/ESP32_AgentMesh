#include "session_mgr.h"
#include "espagent_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
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

static void session_trace_path(const char *chat_id, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return;
    }

    uint64_t hash = session_hash_chat_id(chat_id);
    int n = snprintf(buf, size, "%s/trace_%016" PRIx64 ".jsonl",
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

esp_err_t session_append_trace(const char *chat_id,
                               const char *event_type,
                               const char *summary,
                               const char *raw_json)
{
    char path[128];
    session_trace_path(chat_id, path, sizeof(path));
    if (path[0] == '\0') {
        ESP_LOGE(TAG, "Cannot build trace path");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open trace file %s", path);
        return ESP_FAIL;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(obj, "type", "trace");
    cJSON_AddStringToObject(obj, "event", event_type ? event_type : "event");
    cJSON_AddStringToObject(obj, "summary", summary ? summary : "");
    if (raw_json && raw_json[0]) {
        cJSON *raw = cJSON_Parse(raw_json);
        if (raw) {
            cJSON_AddItemToObject(obj, "raw", raw);
        } else {
            cJSON_AddStringToObject(obj, "raw_text", raw_json);
        }
    }
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
            cJSON_AddItemToArray(arr, entry);
        } else {
            cJSON_Delete(entry);
        }
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

esp_err_t session_get_trace_json(const char *chat_id, char *buf, size_t size, int max_events)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_events <= 0) {
        snprintf(buf, size, "[]");
        return ESP_OK;
    }
    if (max_events > 32) {
        max_events = 32;
    }

    char path[128];
    session_trace_path(chat_id, path, sizeof(path));
    if (path[0] == '\0') {
        snprintf(buf, size, "[]");
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(buf, size, "[]");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *items[32] = {0};
    int count = 0;
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        cJSON *obj = cJSON_Parse(line);
        if (!obj) {
            continue;
        }
        if (count < max_events) {
            items[count++] = obj;
        } else {
            cJSON_Delete(items[0]);
            memmove(&items[0], &items[1], sizeof(items[0]) * (size_t)(max_events - 1));
            items[max_events - 1] = obj;
        }
    }
    fclose(f);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        for (int i = 0; i < count; i++) {
            cJSON_Delete(items[i]);
        }
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < count; i++) {
        cJSON_AddItemToArray(arr, items[i]);
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(buf, size, "%s", json);
    free(json);
    return ESP_OK;
}

static cJSON *task_tree_find_or_add(cJSON *tasks, const char *task_key)
{
    cJSON *task = NULL;
    cJSON_ArrayForEach(task, tasks) {
        cJSON *key = cJSON_GetObjectItem(task, "task_key");
        if (cJSON_IsString(key) && strcmp(key->valuestring, task_key) == 0) {
            return task;
        }
    }

    task = cJSON_CreateObject();
    if (!task) {
        return NULL;
    }
    cJSON_AddStringToObject(task, "task_key", task_key);
    cJSON_AddItemToObject(task, "events", cJSON_CreateArray());
    cJSON_AddItemToArray(tasks, task);
    return task;
}

static void task_tree_add_event(cJSON *tasks, cJSON *trace)
{
    cJSON *raw = cJSON_GetObjectItem(trace, "raw");
    cJSON *src = cJSON_IsObject(raw) ? raw : trace;
    const char *trace_id = "";
    const char *command_id = "";
    const char *event = "";
    const char *action = "";
    const char *status = "";
    const char *summary = "";

    cJSON *item = cJSON_GetObjectItem(src, "trace_id");
    if (cJSON_IsString(item)) {
        trace_id = item->valuestring;
    }
    item = cJSON_GetObjectItem(src, "command_id");
    if (cJSON_IsString(item)) {
        command_id = item->valuestring;
    }
    item = cJSON_GetObjectItem(src, "event");
    if (cJSON_IsString(item)) {
        event = item->valuestring;
    }
    item = cJSON_GetObjectItem(src, "action");
    if (cJSON_IsString(item)) {
        action = item->valuestring;
    }
    item = cJSON_GetObjectItem(src, "status");
    if (cJSON_IsString(item)) {
        status = item->valuestring;
    }
    item = cJSON_GetObjectItem(trace, "summary");
    if (cJSON_IsString(item)) {
        summary = item->valuestring;
    }

    char task_key[80] = {0};
    snprintf(task_key, sizeof(task_key), "%s",
             trace_id[0] ? trace_id : (command_id[0] ? command_id : "local"));
    cJSON *task = task_tree_find_or_add(tasks, task_key);
    if (!task) {
        return;
    }
    if (trace_id[0] && !cJSON_GetObjectItem(task, "trace_id")) {
        cJSON_AddStringToObject(task, "trace_id", trace_id);
    }
    if (command_id[0] && !cJSON_GetObjectItem(task, "command_id")) {
        cJSON_AddStringToObject(task, "command_id", command_id);
    }

    cJSON *events = cJSON_GetObjectItem(task, "events");
    if (!cJSON_IsArray(events)) {
        return;
    }
    cJSON *ev = cJSON_CreateObject();
    if (!ev) {
        return;
    }
    cJSON_AddStringToObject(ev, "event", event[0] ? event : "event");
    cJSON_AddStringToObject(ev, "action", action);
    cJSON_AddStringToObject(ev, "status", status);
    cJSON_AddStringToObject(ev, "summary", summary);
    item = cJSON_GetObjectItem(trace, "ts");
    if (cJSON_IsNumber(item)) {
        cJSON_AddNumberToObject(ev, "ts", item->valuedouble);
    }
    cJSON_AddItemToArray(events, ev);
}

esp_err_t session_get_task_tree_json(const char *chat_id, char *buf, size_t size, int max_events)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_events <= 0) {
        max_events = 32;
    }
    if (max_events > 64) {
        max_events = 64;
    }

    char trace_json[8192] = {0};
    esp_err_t err = session_get_trace_json(chat_id, trace_json, sizeof(trace_json), max_events);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        snprintf(buf, size, "{\"schema\":\"espagent.task_tree.v1\",\"tasks\":[]}");
        return err;
    }

    cJSON *trace_arr = cJSON_Parse(trace_json);
    if (!trace_arr || !cJSON_IsArray(trace_arr)) {
        cJSON_Delete(trace_arr);
        snprintf(buf, size, "{\"schema\":\"espagent.task_tree.v1\",\"tasks\":[]}");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *tasks = cJSON_CreateArray();
    if (!root || !tasks) {
        cJSON_Delete(trace_arr);
        cJSON_Delete(root);
        cJSON_Delete(tasks);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.task_tree.v1");
    cJSON_AddStringToObject(root, "chat_id", chat_id ? chat_id : "");
    cJSON_AddNumberToObject(root, "max_events", max_events);
    cJSON_AddItemToObject(root, "tasks", tasks);

    cJSON *trace = NULL;
    cJSON_ArrayForEach(trace, trace_arr) {
        if (cJSON_IsObject(trace)) {
            task_tree_add_event(tasks, trace);
        }
    }
    cJSON_Delete(trace_arr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(buf, size, "%s", json);
    free(json);
    return ESP_OK;
}

esp_err_t session_get_trace_index_json(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *dir = opendir(ESPAGENT_SPIFFS_SESSION_DIR);
    const char *dir_path = ESPAGENT_SPIFFS_SESSION_DIR;
    if (!dir) {
        dir = opendir(ESPAGENT_SPIFFS_BASE);
        dir_path = ESPAGENT_SPIFFS_BASE;
    }
    if (!dir) {
        snprintf(buf, size, "{\"schema\":\"espagent.trace_index.v1\",\"items\":[]}");
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        closedir(dir);
        cJSON_Delete(root);
        cJSON_Delete(items);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.trace_index.v1");
    cJSON_AddStringToObject(root, "dir", dir_path);
    cJSON_AddItemToObject(root, "items", items);

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!strstr(entry->d_name, "trace_") || !strstr(entry->d_name, ".jsonl")) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        char path[320] = {0};
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        cJSON_AddStringToObject(obj, "file", entry->d_name);
        struct stat st;
        if (stat(path, &st) == 0) {
            cJSON_AddNumberToObject(obj, "size_bytes", (double)st.st_size);
        }
        cJSON_AddItemToArray(items, obj);
    }
    closedir(dir);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(buf, size, "%s", json);
    free(json);
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

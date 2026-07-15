#include "session_mgr.h"
#include "espagent_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "session";

#define SESSION_BRIEF_MAX_HISTORY 12
#define SESSION_BRIEF_MAX_USER_ITEMS 3
#define SESSION_BRIEF_MAX_ASSISTANT_ITEMS 2
#define SESSION_BRIEF_MAX_TASKS 3
#define SESSION_BRIEF_ITEM_CHARS 160
#define SESSION_TRIM_LINE_MAX 4096

static esp_err_t remove_if_exists(const char *path, bool *removed);

static char *session_strdup_line(const char *line)
{
    if (!line) {
        return NULL;
    }
    size_t len = strlen(line);
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, line, len + 1);
    return copy;
}

static void free_line_ring(char **ring, int keep_lines)
{
    if (!ring) {
        return;
    }
    for (int i = 0; i < keep_lines; i++) {
        free(ring[i]);
    }
    free(ring);
}

static esp_err_t trim_jsonl_tail(const char *path, int keep_lines)
{
    if (!path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (keep_lines <= 0) {
        bool removed = false;
        return remove_if_exists(path, &removed);
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    char **ring = calloc((size_t)keep_lines, sizeof(char *));
    if (!ring) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    char line[SESSION_TRIM_LINE_MAX];
    int count = 0;
    int next = 0;
    int total = 0;
    int skipped_overlong = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        bool complete = (len > 0 && line[len - 1] == '\n') || feof(f);
        if (!complete) {
            int ch = 0;
            while ((ch = fgetc(f)) != EOF && ch != '\n') {
            }
            total++;
            skipped_overlong++;
            ESP_LOGW(TAG, "Dropping overlong JSONL line while trimming %s", path);
            continue;
        }

        char *copy = session_strdup_line(line);
        if (!copy) {
            fclose(f);
            free_line_ring(ring, keep_lines);
            return ESP_ERR_NO_MEM;
        }
        free(ring[next]);
        ring[next] = copy;
        next = (next + 1) % keep_lines;
        if (count < keep_lines) {
            count++;
        }
        total++;
    }
    fclose(f);

    if (total <= keep_lines && skipped_overlong == 0) {
        free_line_ring(ring, keep_lines);
        return ESP_OK;
    }

    f = fopen(path, "w");
    if (!f) {
        free_line_ring(ring, keep_lines);
        return ESP_FAIL;
    }

    int start = (count < keep_lines) ? 0 : next;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % keep_lines;
        if (!ring[idx]) {
            continue;
        }
        fputs(ring[idx], f);
        size_t len = strlen(ring[idx]);
        if (len == 0 || ring[idx][len - 1] != '\n') {
            fputc('\n', f);
        }
    }
    fclose(f);
    free_line_ring(ring, keep_lines);
    ESP_LOGI(TAG, "Trimmed %s from %d to %d JSONL lines (%d overlong dropped)",
             path, total, count, skipped_overlong);
    return ESP_OK;
}

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

static void session_brief_path(const char *chat_id, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return;
    }

    uint64_t hash = session_hash_chat_id(chat_id);
    int n = snprintf(buf, size, "%s/brief_%016" PRIx64 ".md",
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

static void compact_line_text(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    size_t wi = 0;
    bool last_space = false;
    for (const unsigned char *p = (const unsigned char *)src; *p && wi + 1 < dst_size; p++) {
        unsigned char c = *p;
        if (c == '\r' || c == '\n' || c == '\t') {
            c = ' ';
        }
        if (c == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }
        dst[wi++] = (char)c;
    }
    while (wi > 0 && dst[wi - 1] == ' ') {
        wi--;
    }
    dst[wi] = '\0';
}

static size_t append_brief_line(char *buf, size_t size, size_t off, const char *fmt, ...)
{
    if (!buf || size == 0) {
        return 0;
    }
    if (off >= size - 1) {
        buf[size - 1] = '\0';
        return size - 1;
    }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + off, size - off, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return off;
    }
    if ((size_t)n >= size - off) {
        buf[size - 1] = '\0';
        return size - 1;
    }
    return off + (size_t)n;
}

static int count_lines_in_file(const char *path)
{
    if (!path || path[0] == '\0') {
        return -1;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    int count = 0;
    int c = 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') {
            count++;
        }
    }
    fclose(f);
    return count;
}

static long file_size_bytes(const char *path)
{
    if (!path || path[0] == '\0') {
        return -1;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (long)st.st_size;
}

static esp_err_t remove_if_exists(const char *path, bool *removed)
{
    if (!path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (remove(path) == 0) {
        if (removed) {
            *removed = true;
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t clear_matching_session_store_files(void)
{
    DIR *dir = opendir(ESPAGENT_SPIFFS_SESSION_DIR);
    if (!dir) {
        dir = opendir(ESPAGENT_SPIFFS_BASE);
        if (!dir) {
            return ESP_ERR_NOT_FOUND;
        }
    }

    bool removed = false;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        bool match = (strncmp(entry->d_name, "session_", 8) == 0 &&
                      strstr(entry->d_name, ".jsonl") != NULL) ||
                     (strncmp(entry->d_name, "trace_", 6) == 0 &&
                      strstr(entry->d_name, ".jsonl") != NULL) ||
                     (strncmp(entry->d_name, "brief_", 6) == 0 &&
                      strstr(entry->d_name, ".md") != NULL);
        if (!match) {
            continue;
        }

        char path[320];
        int n = snprintf(path, sizeof(path), "%s/%s", ESPAGENT_SPIFFS_BASE, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        if (remove(path) == 0) {
            removed = true;
        }
    }
    closedir(dir);
    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
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

static bool text_has_query_token_local(const char *text, const char *query)
{
    if (!text || !query || !query[0]) {
        return false;
    }
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
            if (ti >= 2 && strstr(text, token)) {
                return true;
            }
            ti = 0;
        }
        if (c == '\0') {
            break;
        }
    }
    return false;
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

esp_err_t session_build_relevant_task_brief(const char *chat_id,
                                            const char *query,
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

    char task_tree_json[8192] = {0};
    esp_err_t err = session_get_task_tree_json(chat_id, task_tree_json, sizeof(task_tree_json), 24);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(task_tree_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *tasks = cJSON_GetObjectItem(root, "tasks");
    if (!cJSON_IsArray(tasks)) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    size_t off = 0;
    int matched = 0;
    cJSON *task = NULL;
    cJSON_ArrayForEach(task, tasks) {
        if (!cJSON_IsObject(task)) {
            continue;
        }
        cJSON *task_key = cJSON_GetObjectItem(task, "task_key");
        cJSON *events = cJSON_GetObjectItem(task, "events");
        if (!cJSON_IsString(task_key) || !cJSON_IsArray(events) || cJSON_GetArraySize(events) <= 0) {
            continue;
        }

        bool relevant = text_has_query_token_local(task_key->valuestring, query);
        cJSON *last_ev = cJSON_GetArrayItem(events, cJSON_GetArraySize(events) - 1);
        cJSON *event = last_ev ? cJSON_GetObjectItem(last_ev, "event") : NULL;
        cJSON *action = last_ev ? cJSON_GetObjectItem(last_ev, "action") : NULL;
        cJSON *status = last_ev ? cJSON_GetObjectItem(last_ev, "status") : NULL;
        cJSON *summary = last_ev ? cJSON_GetObjectItem(last_ev, "summary") : NULL;
        if (!relevant) {
            relevant = (cJSON_IsString(event) && text_has_query_token_local(event->valuestring, query)) ||
                       (cJSON_IsString(action) && text_has_query_token_local(action->valuestring, query)) ||
                       (cJSON_IsString(summary) && text_has_query_token_local(summary->valuestring, query));
        }
        if (!relevant) {
            continue;
        }

        if (matched == 0) {
            off = append_brief_line(buf, size, off, "## Relevant Task State\n\n");
        }
        char compact[SESSION_BRIEF_ITEM_CHARS + 1];
        compact_line_text(cJSON_IsString(summary) ? summary->valuestring : "",
                          compact,
                          sizeof(compact));
        off = append_brief_line(buf, size, off,
                                "- %s -> %s/%s/%s%s%s\n",
                                task_key->valuestring,
                                cJSON_IsString(event) ? event->valuestring : "event",
                                cJSON_IsString(action) ? action->valuestring : "",
                                cJSON_IsString(status) ? status->valuestring : "unknown",
                                compact[0] ? ": " : "",
                                compact);
        matched++;
        if (matched >= SESSION_BRIEF_MAX_TASKS || off >= size - 1) {
            break;
        }
    }

    cJSON_Delete(root);
    return matched > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
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

static esp_err_t build_context_brief_uncached(const char *chat_id, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    char history_json[8192] = {0};
    esp_err_t history_err = session_get_history_json(chat_id,
                                                     history_json,
                                                     sizeof(history_json),
                                                     SESSION_BRIEF_MAX_HISTORY);
    if (history_err != ESP_OK && history_err != ESP_ERR_NOT_FOUND) {
        return history_err;
    }

    char task_tree_json[8192] = {0};
    esp_err_t task_err = session_get_task_tree_json(chat_id,
                                                    task_tree_json,
                                                    sizeof(task_tree_json),
                                                    16);
    if (task_err != ESP_OK && task_err != ESP_ERR_NOT_FOUND &&
        task_err != ESP_ERR_INVALID_RESPONSE) {
        return task_err;
    }

    cJSON *history = cJSON_Parse(history_json);
    cJSON *task_tree = cJSON_Parse(task_tree_json);
    size_t off = 0;
    bool wrote = false;

    if (history && cJSON_IsArray(history)) {
        char recent_user[SESSION_BRIEF_MAX_USER_ITEMS][SESSION_BRIEF_ITEM_CHARS + 1];
        char recent_asst[SESSION_BRIEF_MAX_ASSISTANT_ITEMS][SESSION_BRIEF_ITEM_CHARS + 1];
        int user_count = 0;
        int asst_count = 0;

        cJSON *item = NULL;
        cJSON_ArrayForEach(item, history) {
            cJSON *role = cJSON_GetObjectItem(item, "role");
            cJSON *content = cJSON_GetObjectItem(item, "content");
            if (!cJSON_IsString(role) || !cJSON_IsString(content) ||
                content->valuestring[0] == '\0') {
                continue;
            }
            if (strcmp(role->valuestring, "user") == 0) {
                compact_line_text(content->valuestring,
                                  recent_user[user_count % SESSION_BRIEF_MAX_USER_ITEMS],
                                  sizeof(recent_user[0]));
                user_count++;
            } else if (strcmp(role->valuestring, "assistant") == 0) {
                compact_line_text(content->valuestring,
                                  recent_asst[asst_count % SESSION_BRIEF_MAX_ASSISTANT_ITEMS],
                                  sizeof(recent_asst[0]));
                asst_count++;
            }
        }

        int kept_user = user_count < SESSION_BRIEF_MAX_USER_ITEMS ? user_count : SESSION_BRIEF_MAX_USER_ITEMS;
        int kept_asst = asst_count < SESSION_BRIEF_MAX_ASSISTANT_ITEMS ? asst_count : SESSION_BRIEF_MAX_ASSISTANT_ITEMS;
        if (kept_user > 0 || kept_asst > 0) {
            off = append_brief_line(buf, size, off, "## Session Brief\n\n");
            wrote = true;
        }

        if (kept_user > 0) {
            off = append_brief_line(buf, size, off, "- Recent user goals:\n");
            int start = user_count > kept_user ? user_count - kept_user : 0;
            for (int i = 0; i < kept_user; i++) {
                int idx = (start + i) % SESSION_BRIEF_MAX_USER_ITEMS;
                off = append_brief_line(buf, size, off, "  - %s\n", recent_user[idx]);
            }
        }

        if (kept_asst > 0) {
            off = append_brief_line(buf, size, off, "- Recent assistant conclusions:\n");
            int start = asst_count > kept_asst ? asst_count - kept_asst : 0;
            for (int i = 0; i < kept_asst; i++) {
                int idx = (start + i) % SESSION_BRIEF_MAX_ASSISTANT_ITEMS;
                off = append_brief_line(buf, size, off, "  - %s\n", recent_asst[idx]);
            }
        }
    }

    if (task_tree && cJSON_IsObject(task_tree)) {
        cJSON *tasks = cJSON_GetObjectItem(task_tree, "tasks");
        if (tasks && cJSON_IsArray(tasks) && cJSON_GetArraySize(tasks) > 0) {
            if (!wrote) {
                off = append_brief_line(buf, size, off, "## Session Brief\n\n");
                wrote = true;
            }
            off = append_brief_line(buf, size, off, "- Recent task traces:\n");
            int total = cJSON_GetArraySize(tasks);
            int start = total > SESSION_BRIEF_MAX_TASKS ? total - SESSION_BRIEF_MAX_TASKS : 0;
            for (int i = start; i < total; i++) {
                cJSON *task = cJSON_GetArrayItem(tasks, i);
                if (!cJSON_IsObject(task)) {
                    continue;
                }
                cJSON *task_key = cJSON_GetObjectItem(task, "task_key");
                cJSON *events = cJSON_GetObjectItem(task, "events");
                if (!cJSON_IsString(task_key) || !cJSON_IsArray(events) || cJSON_GetArraySize(events) <= 0) {
                    continue;
                }
                cJSON *last_ev = cJSON_GetArrayItem(events, cJSON_GetArraySize(events) - 1);
                cJSON *event = last_ev ? cJSON_GetObjectItem(last_ev, "event") : NULL;
                cJSON *status = last_ev ? cJSON_GetObjectItem(last_ev, "status") : NULL;
                cJSON *summary = last_ev ? cJSON_GetObjectItem(last_ev, "summary") : NULL;
                char compact[SESSION_BRIEF_ITEM_CHARS + 1];
                compact_line_text(cJSON_IsString(summary) ? summary->valuestring : "",
                                  compact,
                                  sizeof(compact));
                off = append_brief_line(buf, size, off,
                                        "  - %s -> %s/%s%s%s\n",
                                        task_key->valuestring,
                                        cJSON_IsString(event) ? event->valuestring : "event",
                                        cJSON_IsString(status) ? status->valuestring : "unknown",
                                        compact[0] ? ": " : "",
                                        compact);
            }
        }
    }

    cJSON_Delete(history);
    cJSON_Delete(task_tree);

    if (!wrote) {
        buf[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t session_refresh_context_brief(const char *chat_id)
{
    char brief[4096] = {0};
    esp_err_t err = build_context_brief_uncached(chat_id, brief, sizeof(brief));
    if (err != ESP_OK) {
        return err;
    }

    char path[128];
    session_brief_path(chat_id, path, sizeof(path));
    if (path[0] == '\0') {
        return ESP_FAIL;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        return ESP_FAIL;
    }
    fputs(brief, f);
    fclose(f);
    return ESP_OK;
}

esp_err_t session_build_context_brief(const char *chat_id, char *buf, size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    char path[128];
    session_brief_path(chat_id, path, sizeof(path));
    if (path[0] != '\0') {
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = fread(buf, 1, size - 1, f);
            buf[n] = '\0';
            fclose(f);
            if (buf[0]) {
                return ESP_OK;
            }
        }
    }

    esp_err_t err = build_context_brief_uncached(chat_id, buf, size);
    if (err == ESP_OK) {
        (void)session_refresh_context_brief(chat_id);
    }
    return err;
}

esp_err_t session_trim_completed_task_context(const char *chat_id)
{
    if (!chat_id || chat_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char history_path[128];
    char trace_path[128];
    session_path(chat_id, history_path, sizeof(history_path));
    session_trace_path(chat_id, trace_path, sizeof(trace_path));

    esp_err_t history_err = trim_jsonl_tail(history_path, ESPAGENT_SESSION_TRIM_MAX_MSGS);
    esp_err_t trace_err = trim_jsonl_tail(trace_path, ESPAGENT_TRACE_TRIM_MAX_EVENTS);

    if (history_err == ESP_OK || trace_err == ESP_OK) {
        (void)session_refresh_context_brief(chat_id);
    }

    bool history_ok = (history_err == ESP_OK || history_err == ESP_ERR_NOT_FOUND);
    bool trace_ok = (trace_err == ESP_OK || trace_err == ESP_ERR_NOT_FOUND);
    if (history_ok && trace_ok) {
        return ESP_OK;
    }
    return history_err != ESP_OK ? history_err : trace_err;
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

esp_err_t session_clear_trace(const char *chat_id)
{
    char path[128];
    session_trace_path(chat_id, path, sizeof(path));

    bool removed = false;
    (void)remove_if_exists(path, &removed);
    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t session_clear_brief(const char *chat_id)
{
    char path[128];
    session_brief_path(chat_id, path, sizeof(path));

    bool removed = false;
    (void)remove_if_exists(path, &removed);
    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t session_clear_all_context(const char *chat_id)
{
    bool removed = false;
    if (session_clear(chat_id) == ESP_OK) {
        removed = true;
    }
    if (session_clear_trace(chat_id) == ESP_OK) {
        removed = true;
    }
    if (session_clear_brief(chat_id) == ESP_OK) {
        removed = true;
    }
    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t session_clear_all_sessions_and_traces(void)
{
    return clear_matching_session_store_files();
}

esp_err_t session_context_status_text(const char *chat_id, char *buf, size_t size)
{
    if (!chat_id || !buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char session_file[128];
    char trace_file[128];
    char brief_file[128];
    session_path(chat_id, session_file, sizeof(session_file));
    session_trace_path(chat_id, trace_file, sizeof(trace_file));
    session_brief_path(chat_id, brief_file, sizeof(brief_file));

    int history_messages = count_lines_in_file(session_file);
    if (history_messages < 0) {
        char legacy_file[128];
        session_legacy_path(chat_id, legacy_file, sizeof(legacy_file));
        history_messages = count_lines_in_file(legacy_file);
    }
    int trace_events = count_lines_in_file(trace_file);
    long brief_bytes = file_size_bytes(brief_file);

    snprintf(buf, size,
             "Context status for chat_id=%s\n"
             "- history_messages: %d (%s)\n"
             "- trace_events: %d (%s)\n"
             "- brief_bytes: %ld (%s)\n",
             chat_id,
             history_messages > 0 ? history_messages : 0,
             history_messages >= 0 ? "present" : "missing",
             trace_events > 0 ? trace_events : 0,
             trace_events >= 0 ? "present" : "missing",
             brief_bytes > 0 ? brief_bytes : 0,
             brief_bytes >= 0 ? "present" : "missing");

    return ESP_OK;
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

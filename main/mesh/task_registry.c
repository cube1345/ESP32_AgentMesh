#include "mesh/task_registry.h"

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>

#define ESPAGENT_TASK_REGISTRY_DEPTH 16
#define ESPAGENT_TASK_SUMMARY_MAX 192

typedef struct {
    bool used;
    int64_t updated_ms;
    espagent_task_meta_t task;
    char summary[ESPAGENT_TASK_SUMMARY_MAX];
} task_record_t;

static task_record_t s_records[ESPAGENT_TASK_REGISTRY_DEPTH];
static SemaphoreHandle_t s_mutex;

static esp_err_t ensure_mutex(void)
{
    if (s_mutex) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static task_record_t *find_record(const char *task_or_command_id)
{
    if (!task_or_command_id || !task_or_command_id[0]) return NULL;
    for (size_t i = 0; i < ESPAGENT_TASK_REGISTRY_DEPTH; ++i) {
        task_record_t *record = &s_records[i];
        if (record->used &&
            (strcmp(record->task.task_id, task_or_command_id) == 0 ||
             strcmp(record->task.command_id, task_or_command_id) == 0)) {
            return record;
        }
    }
    return NULL;
}

static task_record_t *find_oldest_or_free(void)
{
    task_record_t *oldest = &s_records[0];
    for (size_t i = 0; i < ESPAGENT_TASK_REGISTRY_DEPTH; ++i) {
        task_record_t *record = &s_records[i];
        if (!record->used) return record;
        if (record->updated_ms < oldest->updated_ms) oldest = record;
    }
    return oldest;
}

esp_err_t espagent_task_registry_upsert(const espagent_task_meta_t *task,
                                        const char *summary)
{
    if (!task || !task->command_id[0]) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;

    task_record_t *record = find_record(task->task_id[0] ? task->task_id : task->command_id);
    if (!record) record = find_oldest_or_free();
    memset(record, 0, sizeof(*record));
    record->used = true;
    record->updated_ms = esp_timer_get_time() / 1000;
    record->task = *task;
    if (!record->task.task_id[0]) {
        snprintf(record->task.task_id, sizeof(record->task.task_id), "task-%s",
                 record->task.command_id);
    }
    snprintf(record->summary, sizeof(record->summary), "%s", summary ? summary : "");
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t espagent_task_registry_update(const char *command_id,
                                        espagent_task_status_t status,
                                        const char *summary)
{
    if (!command_id || !command_id[0]) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;

    task_record_t *record = find_record(command_id);
    if (!record) {
        espagent_task_meta_t task = {0};
        snprintf(task.command_id, sizeof(task.command_id), "%s", command_id);
        snprintf(task.task_id, sizeof(task.task_id), "task-%s", command_id);
        task.status = status;
        record = find_oldest_or_free();
        memset(record, 0, sizeof(*record));
        record->used = true;
        record->task = task;
    } else {
        record->task.status = status;
    }
    record->updated_ms = esp_timer_get_time() / 1000;
    snprintf(record->summary, sizeof(record->summary), "%s", summary ? summary : "");
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t espagent_task_registry_write_json(const char *task_or_command_id,
                                            char *output,
                                            size_t output_size)
{
    if (!output || output_size == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_mutex();
    if (err != ESP_OK) return err;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return ESP_ERR_TIMEOUT;

    task_record_t *record = find_record(task_or_command_id);
    if (!record) {
        xSemaphoreGive(s_mutex);
        snprintf(output, output_size, "Error: task not found");
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "task_id", record->task.task_id);
    cJSON_AddStringToObject(root, "parent_task_id", record->task.parent_task_id);
    cJSON_AddStringToObject(root, "command_id", record->task.command_id);
    cJSON_AddStringToObject(root, "trace_id", record->task.trace_id);
    cJSON_AddStringToObject(root, "source_channel", record->task.source_channel);
    cJSON_AddStringToObject(root, "source_chat_id", record->task.source_chat_id);
    cJSON_AddStringToObject(root, "target_role", record->task.target_role);
    cJSON_AddNumberToObject(root, "deadline_ms", (double)record->task.deadline_ms);
    cJSON_AddNumberToObject(root, "retry_count", record->task.retry_count);
    cJSON_AddStringToObject(root, "result_status",
                            espagent_task_status_name(record->task.status));
    cJSON_AddStringToObject(root, "summary", record->summary);
    cJSON_AddNumberToObject(root, "updated_ms", (double)record->updated_ms);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    xSemaphoreGive(s_mutex);
    if (!json) return ESP_ERR_NO_MEM;
    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

#include "guardian/approval_queue.h"

#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mesh/mesh_types.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define GUARDIAN_APPROVAL_DEPTH 8

typedef struct {
    bool used;
    bool resolved;
    bool approved;
    bool consumed;
    int64_t ts_ms;
    char approval_id[32];
    char command_id[ESPAGENT_MESH_ID_MAX];
    char trace_id[ESPAGENT_MESH_TRACE_MAX];
    char action[ESPAGENT_MESH_ACTION_MAX];
    char target_role[ESPAGENT_MESH_ROLE_MAX];
    char args_json[ESPAGENT_MESH_ARGS_JSON_MAX];
    char reason[160];
} approval_item_t;

static SemaphoreHandle_t s_lock;
static approval_item_t s_items[GUARDIAN_APPROVAL_DEPTH];
static uint32_t s_next;

static esp_err_t ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

esp_err_t guardian_approval_queue_init(void)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    lock();
    memset(s_items, 0, sizeof(s_items));
    s_next = 0;
    unlock();
    return ESP_OK;
}

esp_err_t guardian_approval_add(const char *command_id,
                                const char *trace_id,
                                const char *action,
                                const char *target_role,
                                const char *args_json,
                                const char *reason,
                                char *approval_id,
                                size_t approval_id_size)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }

    int64_t ts_ms = esp_timer_get_time() / 1000;
    char id[32] = {0};
    snprintf(id, sizeof(id), "appr-%08llx", (unsigned long long)(ts_ms & 0xffffffffULL));

    lock();
    approval_item_t *slot = &s_items[s_next % GUARDIAN_APPROVAL_DEPTH];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->ts_ms = ts_ms;
    snprintf(slot->approval_id, sizeof(slot->approval_id), "%s", id);
    snprintf(slot->command_id, sizeof(slot->command_id), "%s", command_id ? command_id : "");
    snprintf(slot->trace_id, sizeof(slot->trace_id), "%s", trace_id ? trace_id : "");
    snprintf(slot->action, sizeof(slot->action), "%s", action ? action : "");
    snprintf(slot->target_role, sizeof(slot->target_role), "%s", target_role ? target_role : "");
    snprintf(slot->args_json, sizeof(slot->args_json), "%s", args_json ? args_json : "{}");
    snprintf(slot->reason, sizeof(slot->reason), "%s", reason ? reason : "");
    s_next++;
    unlock();

    if (approval_id && approval_id_size) {
        snprintf(approval_id, approval_id_size, "%s", id);
    }
    return ESP_OK;
}

esp_err_t guardian_approval_list_json(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }

    lock();
    cJSON_AddStringToObject(root, "schema", "espagent.guardian_approvals.v1");
    for (int i = 0; i < GUARDIAN_APPROVAL_DEPTH; i++) {
        approval_item_t *item = &s_items[i];
        if (!item->used) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddStringToObject(obj, "approval_id", item->approval_id);
        cJSON_AddStringToObject(obj, "command_id", item->command_id);
        cJSON_AddStringToObject(obj, "trace_id", item->trace_id);
        cJSON_AddStringToObject(obj, "action", item->action);
        cJSON_AddStringToObject(obj, "target_role", item->target_role);
        cJSON_AddStringToObject(obj, "reason", item->reason);
        cJSON_AddBoolToObject(obj, "resolved", item->resolved);
        cJSON_AddBoolToObject(obj, "approved", item->approved);
        cJSON_AddBoolToObject(obj, "consumed", item->consumed);
        cJSON_AddNumberToObject(obj, "ts_ms", (double)item->ts_ms);
        cJSON_AddItemToArray(arr, obj);
    }
    unlock();
    cJSON_AddItemToObject(root, "items", arr);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t guardian_approval_resolve(const char *approval_id,
                                    bool approve,
                                    char *output,
                                    size_t output_size)
{
    if (!approval_id || !approval_id[0]) {
        snprintf(output, output_size, "Error: approval_id is required");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }

    lock();
    for (int i = 0; i < GUARDIAN_APPROVAL_DEPTH; i++) {
        approval_item_t *item = &s_items[i];
        if (item->used && strcmp(item->approval_id, approval_id) == 0) {
            item->resolved = true;
            item->approved = approve;
            unlock();
            snprintf(output,
                     output_size,
                     "OK: approval %s %s; resubmit command with explicit confirmation if execution is still desired",
                     approval_id,
                     approve ? "approved" : "denied");
            return ESP_OK;
        }
    }
    unlock();
    snprintf(output, output_size, "Error: approval %s not found", approval_id);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t guardian_approval_consume(const char *approval_id,
                                    const char *action,
                                    const char *target_role,
                                    char *reason,
                                    size_t reason_size)
{
    if (!approval_id || !approval_id[0]) {
        snprintf(reason, reason_size, "approval_id is required");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        snprintf(reason, reason_size, "approval queue unavailable");
        return err;
    }

    lock();
    for (int i = 0; i < GUARDIAN_APPROVAL_DEPTH; i++) {
        approval_item_t *item = &s_items[i];
        if (!item->used || strcmp(item->approval_id, approval_id) != 0) {
            continue;
        }
        if (item->consumed) {
            unlock();
            snprintf(reason, reason_size, "approval %s already consumed", approval_id);
            return ESP_ERR_INVALID_STATE;
        }
        if (!item->resolved) {
            unlock();
            snprintf(reason, reason_size, "approval %s is still pending", approval_id);
            return ESP_ERR_INVALID_STATE;
        }
        if (!item->approved) {
            unlock();
            snprintf(reason, reason_size, "approval %s was denied", approval_id);
            return ESP_ERR_INVALID_STATE;
        }
        if (action && action[0] && strcmp(item->action, action) != 0) {
            unlock();
            snprintf(reason, reason_size,
                     "approval %s action mismatch: expected=%s actual=%s",
                     approval_id, item->action, action);
            return ESP_ERR_INVALID_ARG;
        }
        if (target_role && target_role[0] && strcmp(item->target_role, target_role) != 0) {
            unlock();
            snprintf(reason, reason_size,
                     "approval %s target_role mismatch: expected=%s actual=%s",
                     approval_id, item->target_role, target_role);
            return ESP_ERR_INVALID_ARG;
        }
        item->consumed = true;
        unlock();
        snprintf(reason, reason_size, "allowed by one-shot human approval_id=%s", approval_id);
        return ESP_OK;
    }
    unlock();
    snprintf(reason, reason_size, "approval %s not found", approval_id);
    return ESP_ERR_NOT_FOUND;
}

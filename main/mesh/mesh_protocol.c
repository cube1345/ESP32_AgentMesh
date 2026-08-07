#include "mesh/mesh_protocol.h"

#include "cJSON.h"
#include "esp_err.h"
#include "espagent_config.h"
#include "mesh/mesh_auth.h"
#include "node/node_profile.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char *espagent_task_status_name(espagent_task_status_t status)
{
    switch (status) {
    case ESPAGENT_TASK_CREATED: return "created";
    case ESPAGENT_TASK_POLICY_PENDING: return "policy_pending";
    case ESPAGENT_TASK_QUEUED: return "queued";
    case ESPAGENT_TASK_RUNNING: return "running";
    case ESPAGENT_TASK_WAITING_RESULT: return "waiting_result";
    case ESPAGENT_TASK_SUCCEEDED: return "succeeded";
    case ESPAGENT_TASK_FAILED: return "failed";
    case ESPAGENT_TASK_EXPIRED: return "expired";
    case ESPAGENT_TASK_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

static void set_err(char *err_buf, size_t err_buf_size, const char *msg)
{
    if (err_buf && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "%s", msg ? msg : "unknown error");
    }
}

static void copy_field(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static const char *json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_int(cJSON *root, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : default_value;
}

static int64_t json_i64(cJSON *root, const char *key, int64_t default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? (int64_t)item->valuedouble : default_value;
}

static bool json_bool(cJSON *root, const char *key, bool default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item) {
        return default_value;
    }
    return cJSON_IsTrue(item);
}

static espagent_task_status_t json_task_status(cJSON *root)
{
    const char *status = json_string(root, "task_status");
    if (!status) return ESPAGENT_TASK_QUEUED;
    if (strcmp(status, "created") == 0) return ESPAGENT_TASK_CREATED;
    if (strcmp(status, "policy_pending") == 0) return ESPAGENT_TASK_POLICY_PENDING;
    if (strcmp(status, "running") == 0) return ESPAGENT_TASK_RUNNING;
    if (strcmp(status, "waiting_result") == 0) return ESPAGENT_TASK_WAITING_RESULT;
    if (strcmp(status, "succeeded") == 0) return ESPAGENT_TASK_SUCCEEDED;
    if (strcmp(status, "failed") == 0) return ESPAGENT_TASK_FAILED;
    if (strcmp(status, "expired") == 0) return ESPAGENT_TASK_EXPIRED;
    if (strcmp(status, "cancelled") == 0) return ESPAGENT_TASK_CANCELLED;
    return ESPAGENT_TASK_QUEUED;
}

static esp_err_t copy_args_json(cJSON *root, espagent_mesh_command_t *out)
{
    const char *args_json = json_string(root, "args_json");
    if (args_json) {
        copy_field(out->args_json, sizeof(out->args_json), args_json);
        return ESP_OK;
    }

    cJSON *args = cJSON_GetObjectItem(root, "args");
    if (!args) {
        copy_field(out->args_json, sizeof(out->args_json), "{}");
        return ESP_OK;
    }

    char *printed = cJSON_PrintUnformatted(args);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    copy_field(out->args_json, sizeof(out->args_json), printed);
    cJSON_free(printed);
    return ESP_OK;
}

esp_err_t espagent_mesh_build_node_topic(const char *node_id,
                                         const char *suffix,
                                         char *out,
                                         size_t out_size)
{
    if (!node_id || node_id[0] == '\0' || !suffix || suffix[0] == '\0' ||
        !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(out, out_size, "%s/nodes/%s/%s",
                     ESPAGENT_MESH_TOPIC_PREFIX, node_id, suffix);
    return (n > 0 && (size_t)n < out_size) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t espagent_mesh_build_role_topic(const char *role,
                                         const char *suffix,
                                         char *out,
                                         size_t out_size)
{
    if (!role || role[0] == '\0' || !suffix || suffix[0] == '\0' ||
        !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int n = snprintf(out, out_size, "%s/roles/%s/%s",
                     ESPAGENT_MESH_TOPIC_PREFIX, role, suffix);
    return (n > 0 && (size_t)n < out_size) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t espagent_mesh_parse_command_json(const char *json,
                                           size_t json_len,
                                           espagent_mesh_command_t *out,
                                           char *err_buf,
                                           size_t err_buf_size)
{
    if (!json || json_len == 0 || !out) {
        set_err(err_buf, err_buf_size, "empty command payload");
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        set_err(err_buf, err_buf_size, "command payload is not a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    const char *action = json_string(root, "action");
    if (!action || action[0] == '\0') {
        cJSON_Delete(root);
        set_err(err_buf, err_buf_size, "missing required string field: action");
        return ESP_ERR_INVALID_ARG;
    }

    copy_field(out->command_id, sizeof(out->command_id), json_string(root, "command_id"));
    copy_field(out->trace_id, sizeof(out->trace_id), json_string(root, "trace_id"));
    copy_field(out->target_node, sizeof(out->target_node), json_string(root, "target_node"));
    copy_field(out->target_role, sizeof(out->target_role), json_string(root, "target_role"));
    copy_field(out->action, sizeof(out->action), action);
    copy_field(out->signature, sizeof(out->signature), json_string(root, "signature"));
    copy_field(out->nonce, sizeof(out->nonce), json_string(root, "nonce"));
    out->ts_ms = json_i64(root, "ts_ms", 0);
    out->ttl_ms = json_int(root, "ttl_ms", 30000);
    out->safety_level = json_int(root, "safety_level", ESPAGENT_MESH_SAFETY_MEDIUM);
    out->require_ack = json_bool(root, "require_ack", true);
    copy_field(out->task.task_id, sizeof(out->task.task_id), json_string(root, "task_id"));
    if (!out->task.task_id[0] && out->command_id[0]) {
        snprintf(out->task.task_id, sizeof(out->task.task_id), "task-%.34s", out->command_id);
    }
    copy_field(out->task.parent_task_id, sizeof(out->task.parent_task_id), json_string(root, "parent_task_id"));
    copy_field(out->task.source_channel, sizeof(out->task.source_channel), json_string(root, "source_channel"));
    copy_field(out->task.source_chat_id, sizeof(out->task.source_chat_id), json_string(root, "source_chat_id"));
    out->task.deadline_ms = json_i64(root, "deadline_ms", out->ts_ms + out->ttl_ms);
    out->task.retry_count = (uint16_t)json_int(root, "retry_count", 0);
    out->task.status = json_task_status(root);
    copy_field(out->task.command_id, sizeof(out->task.command_id), out->command_id);
    copy_field(out->task.trace_id, sizeof(out->task.trace_id), out->trace_id);
    copy_field(out->task.target_role, sizeof(out->task.target_role), out->target_role);

    esp_err_t args_err = copy_args_json(root, out);
    cJSON_Delete(root);
    if (args_err != ESP_OK) {
        set_err(err_buf, err_buf_size, "failed to serialize args");
        return args_err;
    }

    if (out->ttl_ms < 1000 || out->ttl_ms > 30000) {
        set_err(err_buf, err_buf_size, "ttl_ms must be 1000..30000");
        return ESP_ERR_INVALID_ARG;
    }
    if (out->safety_level < ESPAGENT_MESH_SAFETY_LOW ||
        out->safety_level > ESPAGENT_MESH_SAFETY_HIGH) {
        set_err(err_buf, err_buf_size, "safety_level must be 0..2");
        return ESP_ERR_INVALID_ARG;
    }

    char auth_reason[96] = {0};
    esp_err_t auth_err = espagent_mesh_auth_verify_command(out, auth_reason, sizeof(auth_reason));
    if (auth_err != ESP_OK) {
        set_err(err_buf, err_buf_size, auth_reason);
        return auth_err;
    }

    if (out->target_node[0] != '\0' && strcmp(out->target_node, espagent_node_id()) != 0) {
        set_err(err_buf, err_buf_size, "command target_node does not match this node");
        return ESP_ERR_NOT_FOUND;
    }

    if (out->target_role[0] != '\0' && strcmp(out->target_role, espagent_node_role()) != 0) {
        set_err(err_buf, err_buf_size, "command target_role does not match this node role");
        return ESP_ERR_NOT_FOUND;
    }

    set_err(err_buf, err_buf_size, "ok");
    return ESP_OK;
}

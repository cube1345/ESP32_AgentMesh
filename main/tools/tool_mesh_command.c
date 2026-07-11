#include "tools/tool_mesh_command.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "espagent_config.h"
#include "bus/message_bus.h"
#include "mesh/mesh_auth.h"
#include "mesh/mesh_protocol.h"
#include "net/net_guard.h"
#include "sensors/sensor_mqtt.h"
#include "tools/tool_sandbox.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char task_id[48];
    char command_id[ESPAGENT_MESH_ID_MAX];
    char trace_id[ESPAGENT_MESH_TRACE_MAX];
    char target_role[ESPAGENT_MESH_ROLE_MAX];
    char target_node[ESPAGENT_MESH_NODE_MAX];
    char action[ESPAGENT_MESH_ACTION_MAX];
    char reply_channel[16];
    char reply_chat_id[96];
    uint32_t wait_ms;
} mesh_wait_task_ctx_t;

#define MESH_OUTPUT_JSON_SIZE 1024
#define MESH_POLICY_JSON_SIZE 1024
#define MESH_BACKGROUND_NET_DEFER_MS 15000
#define MESH_ACK_RETRY_COUNT 4
#define MESH_POLICY_RETRY_COUNT 3

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

static bool json_bool(cJSON *root, const char *key, bool default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item) {
        return default_value;
    }
    return cJSON_IsTrue(item);
}

static void compact_text_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!src) {
        return;
    }

    size_t j = 0;
    bool last_space = false;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dst_size; i++) {
        char ch = src[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
        if (ch == ' ') {
            if (last_space) {
                continue;
            }
            last_space = true;
        } else {
            last_space = false;
        }
        dst[j++] = ch;
    }
    dst[j] = '\0';
}

static void build_compact_output_summary(const char *output_json,
                                         char *summary,
                                         size_t summary_size)
{
    if (!summary || summary_size == 0) {
        return;
    }
    summary[0] = '\0';
    if (!output_json || output_json[0] == '\0') {
        return;
    }

    cJSON *root = cJSON_Parse(output_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        compact_text_copy(summary, summary_size, output_json);
        return;
    }

    const char *status = json_string(root, "status");
    const char *action = json_string(root, "action");
    const char *node_id = json_string(root, "node_id");
    const char *result_text = NULL;
    const char *error_text = NULL;

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsObject(result)) {
        result_text = json_string(result, "text");
    }
    if (cJSON_IsObject(error)) {
        error_text = json_string(error, "message");
    }

    char compact[320] = {0};
    compact_text_copy(compact, sizeof(compact),
                      result_text && result_text[0] ? result_text : error_text);

    snprintf(summary, summary_size,
             "%s%s%s%s%s%s%s",
             status && status[0] ? status : "unknown",
             action && action[0] ? " action=" : "",
             action && action[0] ? action : "",
             node_id && node_id[0] ? " node=" : "",
             node_id && node_id[0] ? node_id : "",
             compact[0] ? " result=" : "",
             compact[0] ? compact : "");
    cJSON_Delete(root);
}

static void build_mesh_async_user_reply(const mesh_wait_task_ctx_t *ctx,
                                        const char *output_json,
                                        bool timed_out,
                                        char *reply,
                                        size_t reply_size)
{
    if (!reply || reply_size == 0) {
        return;
    }
    reply[0] = '\0';

    if (timed_out) {
        snprintf(reply, reply_size,
                 "远程%s未在 %u ms 内返回结果，请稍后重试。",
                 ctx && ctx->target_role[0] ? ctx->target_role : "节点",
                 (unsigned)(ctx ? ctx->wait_ms : 0));
        return;
    }

    cJSON *root = cJSON_Parse(output_json ? output_json : "");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(reply, reply_size,
                 "远程%s已返回结果，但结果格式无法解析。",
                 ctx && ctx->target_role[0] ? ctx->target_role : "节点");
        return;
    }

    const char *status = json_string(root, "status");
    const char *action = json_string(root, "action");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *error = cJSON_GetObjectItem(root, "error");
    const char *result_text = NULL;
    const char *error_text = NULL;
    if (cJSON_IsObject(result)) {
        result_text = json_string(result, "text");
    }
    if (cJSON_IsObject(error)) {
        error_text = json_string(error, "message");
    }

    char compact[512] = {0};
    compact_text_copy(compact, sizeof(compact),
                      result_text && result_text[0] ? result_text : error_text);

    const char *role = (ctx && ctx->target_role[0]) ? ctx->target_role : "节点";
    const char *verb = "已返回";
    if (action && action[0] &&
        (strcmp(action, "set_status_light") == 0 ||
         strcmp(action, "ws2812_set") == 0 ||
         strcmp(action, "virtual_device_control") == 0 ||
         strcmp(action, "servo_write") == 0 ||
         strcmp(action, "set_humidifier") == 0 ||
         strcmp(action, "set_fan") == 0 ||
         strcmp(action, "set_device_led") == 0 ||
         strcmp(action, "copper_gpio_write") == 0 ||
         strcmp(action, "gpio_write") == 0)) {
        verb = "已执行";
    }

    if (status && strcmp(status, "ok") == 0) {
        snprintf(reply, reply_size, "远程%s%s：%s",
                 role,
                 verb,
                 compact[0] ? compact : "执行成功。");
    } else {
        snprintf(reply, reply_size, "远程%s执行失败：%s",
                 role,
                 compact[0] ? compact : "未返回详细错误。");
    }
    cJSON_Delete(root);
}

static bool is_control_action(const char *action)
{
    return action &&
           (strcmp(action, "set_status_light") == 0 ||
            strcmp(action, "ws2812_set") == 0 ||
            strcmp(action, "virtual_device_control") == 0 ||
            strcmp(action, "servo_write") == 0 ||
            strcmp(action, "set_humidifier") == 0 ||
            strcmp(action, "set_fan") == 0 ||
            strcmp(action, "set_device_led") == 0 ||
            strcmp(action, "copper_gpio_write") == 0 ||
            strcmp(action, "gpio_write") == 0 ||
            strcmp(action, "tts_speak") == 0 ||
            strcmp(action, "gree_ac_control") == 0 ||
            strcmp(action, "control_state") == 0 ||
            strcmp(action, "control_emergency_stop") == 0 ||
            strcmp(action, "control_clear_emergency_stop") == 0);
}

static bool is_sensor_action(const char *action)
{
    return action &&
           (strcmp(action, "read_temperature_humidity") == 0 ||
            strcmp(action, "virtual_device_read") == 0);
}

static bool is_allowed_mesh_action(const char *action)
{
    return is_sensor_action(action) ||
           is_control_action(action) ||
           (action && strcmp(action, "agent_task") == 0);
}

static esp_err_t publish_mesh_command_payload(const char *topic,
                                              const char *payload,
                                              const char *target_role,
                                              bool require_ack)
{
    esp_err_t err = sensor_mqtt_publish_text(topic, payload);
    if (err != ESP_OK) {
        return err;
    }

    if (!require_ack || !target_role || !target_role[0]) {
        return ESP_OK;
    }

    /*
     * Role topics are QoS0 in the tiny MQTT client. Retrying the same
     * command_id is safe because receivers de-duplicate before execution,
     * and it removes the last observed stress-test gap where Guardian had
     * allowed a sensor command but the sensor board never saw the role topic.
     */
    static const uint16_t retry_delay_ms[MESH_ACK_RETRY_COUNT - 1] = {80, 180, 350};
    for (size_t i = 0; i < MESH_ACK_RETRY_COUNT - 1; i++) {
        vTaskDelay(pdMS_TO_TICKS(retry_delay_ms[i]));
        esp_err_t retry_err = sensor_mqtt_publish_text(topic, payload);
        if (retry_err != ESP_OK) {
            ESP_LOGW("tool_mesh_command",
                     "Mesh command retry publish failed: role=%s attempt=%u/%u err=%s",
                     target_role,
                     (unsigned)(i + 2),
                     (unsigned)MESH_ACK_RETRY_COUNT,
                     esp_err_to_name(retry_err));
        } else {
            ESP_LOGI("tool_mesh_command",
                     "Mesh command retry queued: role=%s attempt=%u/%u topic=%s",
                     target_role,
                     (unsigned)(i + 2),
                     (unsigned)MESH_ACK_RETRY_COUNT,
                     topic);
        }
    }
    return ESP_OK;
}

static esp_err_t request_policy_decision(const char *command_id,
                                         const char *trace_id,
                                         const char *target_role,
                                         const char *target_node,
                                         const char *action,
                                         const char *args_json,
                                         int safety_level,
                                         int ttl_ms,
                                         char *decision_json,
                                         size_t decision_json_size,
                                         char *reason,
                                         size_t reason_size)
{
    cJSON *policy = cJSON_CreateObject();
    if (!policy) {
        return ESP_ERR_NO_MEM;
    }

    int64_t ts_ms = esp_timer_get_time() / 1000;
    cJSON_AddStringToObject(policy, "schema", "espagent.policy_check.v1");
    cJSON_AddStringToObject(policy, "event", "policy_check");
    cJSON_AddStringToObject(policy, "command_id", command_id);
    cJSON_AddStringToObject(policy, "trace_id", trace_id);
    cJSON_AddStringToObject(policy, "source_role", "coordinator_agent");
    cJSON_AddStringToObject(policy, "target_role", target_role ? target_role : "");
    cJSON_AddStringToObject(policy, "target_node", target_node ? target_node : "");
    cJSON_AddStringToObject(policy, "action", action);
    cJSON_AddStringToObject(policy, "args_json", args_json ? args_json : "{}");
    cJSON_AddNumberToObject(policy, "safety_level", safety_level);
    cJSON_AddNumberToObject(policy, "ttl_ms", ttl_ms);
    cJSON_AddNumberToObject(policy, "ts_ms", (double)ts_ms);

    char *json = cJSON_PrintUnformatted(policy);
    cJSON_Delete(policy);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t wait_ms = ttl_ms > 0 ? (uint32_t)ttl_ms : 30000U;
    if (wait_ms > 15000U) {
        wait_ms = 15000U;
    }
    if (wait_ms < 3000U) {
        wait_ms = 3000U;
    }

    ESP_LOGI("tool_mesh_command", "Policy check requested: %s", json);
    esp_err_t err = ESP_ERR_TIMEOUT;
    for (int attempt = 1; attempt <= MESH_POLICY_RETRY_COUNT; attempt++) {
        err = sensor_mqtt_wait_connected(12000);
        if (err != ESP_OK) {
            cJSON_free(json);
            snprintf(reason, reason_size, "MQTT is not connected for policy_check: %s", esp_err_to_name(err));
            return err;
        }
        err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_POLICY_CHECK, json);
        if (err != ESP_OK) {
            cJSON_free(json);
            snprintf(reason, reason_size, "failed to publish policy_check: %s", esp_err_to_name(err));
            return err;
        }
        espagent_net_guard_defer_background(MESH_BACKGROUND_NET_DEFER_MS);

        err = sensor_mqtt_wait_policy_decision(command_id,
                                               decision_json,
                                               decision_json_size,
                                               wait_ms);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW("tool_mesh_command",
                 "policy_decision wait timed out: command_id=%s attempt=%d/%d wait_ms=%u",
                 command_id,
                 attempt,
                 MESH_POLICY_RETRY_COUNT,
                 (unsigned)wait_ms);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    cJSON_free(json);
    if (err != ESP_OK) {
        snprintf(reason, reason_size,
                 "policy_decision wait timed out after %u attempts (%ums each)",
                 (unsigned)MESH_POLICY_RETRY_COUNT,
                 (unsigned)wait_ms);
        return err;
    }

    cJSON *decision = cJSON_Parse(decision_json);
    if (!decision || !cJSON_IsObject(decision)) {
        cJSON_Delete(decision);
        snprintf(reason, reason_size, "policy_decision is not valid JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *decision_item = cJSON_GetObjectItem(decision, "decision");
    cJSON *reason_item = cJSON_GetObjectItem(decision, "reason");
    const char *decision_text = cJSON_IsString(decision_item) ? decision_item->valuestring : "";
    const char *reason_text = cJSON_IsString(reason_item) ? reason_item->valuestring : "";
    snprintf(reason, reason_size, "%s", reason_text);
    bool allowed = strcmp(decision_text, "allow") == 0;
    cJSON_Delete(decision);

    return allowed ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static esp_err_t copy_args(cJSON *root, cJSON *cmd)
{
    const char *args_json = json_string(root, "args_json");
    if (args_json && args_json[0]) {
        cJSON *args = cJSON_Parse(args_json);
        if (!args) {
            return ESP_ERR_INVALID_ARG;
        }
        cJSON_AddItemToObject(cmd, "args", args);
        return ESP_OK;
    }

    cJSON *args = cJSON_GetObjectItem(root, "args");
    if (args) {
        cJSON_AddItemReferenceToObject(cmd, "args", args);
    } else {
        cJSON_AddItemToObject(cmd, "args", cJSON_CreateObject());
    }
    return ESP_OK;
}

static void mesh_wait_task(void *arg)
{
    mesh_wait_task_ctx_t *ctx = (mesh_wait_task_ctx_t *)arg;
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    char output_json[MESH_OUTPUT_JSON_SIZE] = {0};
    esp_err_t wait_err = sensor_mqtt_wait_output_message(ctx->command_id,
                                                         output_json,
                                                         sizeof(output_json),
                                                         ctx->wait_ms);
    const bool ok = wait_err == ESP_OK;
    char summary[192] = {0};
    snprintf(summary, sizeof(summary), "%s command_id=%s action=%s",
             ok ? "Async mesh task completed" : "Async mesh task timed out",
             ctx->command_id,
             ctx->action);

    (void)sensor_mqtt_publish_timeline_event("result",
                                             "mesh_async_result",
                                             ok ? "ok" : "timeout",
                                             summary,
                                             ctx->command_id,
                                             ctx->target_role,
                                             ctx->target_node,
                                             ctx->action);

    if (ctx->reply_channel[0] && ctx->reply_chat_id[0]) {
        espagent_msg_t out = {0};
        char reply[768] = {0};
        build_mesh_async_user_reply(ctx, output_json, !ok, reply, sizeof(reply));

        snprintf(out.channel, sizeof(out.channel), "%s", ctx->reply_channel);
        snprintf(out.chat_id, sizeof(out.chat_id), "%s", ctx->reply_chat_id);
        out.content = strdup(reply);
        if (out.content) {
            (void)sensor_mqtt_publish_timeline_event("final",
                                                     "mesh_async_reply",
                                                     ok ? "ok" : "timeout",
                                                     out.content,
                                                     ctx->command_id,
                                                     ctx->target_role,
                                                     ctx->target_node,
                                                     ctx->action);
            (void)sensor_mqtt_publish_output_message("final_reply",
                                                     ctx->command_id,
                                                     ctx->trace_id,
                                                     ctx->action,
                                                     ctx->reply_channel,
                                                     ok ? ESP_OK : wait_err,
                                                     out.content,
                                                     out.content);
            if (message_bus_push_outbound(&out) != ESP_OK) {
                free(out.content);
            }
        }
    }

    free(ctx);
    vTaskDelete(NULL);
}

static esp_err_t start_mesh_wait_task(const char *command_id,
                                      const char *trace_id,
                                      const char *target_role,
                                      const char *target_node,
                                      const char *action,
                                      const char *reply_channel,
                                      const char *reply_chat_id,
                                      uint32_t wait_ms,
                                      char *task_id,
                                      size_t task_id_size)
{
    mesh_wait_task_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(ctx->task_id, sizeof(ctx->task_id), "mesh-task-%s", command_id);
    snprintf(ctx->command_id, sizeof(ctx->command_id), "%s", command_id);
    snprintf(ctx->trace_id, sizeof(ctx->trace_id), "%s", trace_id ? trace_id : "");
    snprintf(ctx->target_role, sizeof(ctx->target_role), "%s", target_role ? target_role : "");
    snprintf(ctx->target_node, sizeof(ctx->target_node), "%s", target_node ? target_node : "");
    snprintf(ctx->action, sizeof(ctx->action), "%s", action ? action : "");
    snprintf(ctx->reply_channel, sizeof(ctx->reply_channel), "%s", reply_channel ? reply_channel : "");
    snprintf(ctx->reply_chat_id, sizeof(ctx->reply_chat_id), "%s", reply_chat_id ? reply_chat_id : "");
    ctx->wait_ms = wait_ms;

    if (task_id && task_id_size > 0) {
        snprintf(task_id, task_id_size, "%s", ctx->task_id);
    }

    if (xTaskCreate(mesh_wait_task, "mesh_wait", 6 * 1024, ctx, 4, NULL) != pdPASS) {
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tool_mesh_send_command_execute(const char *input_json,
                                         char *output,
                                         size_t output_size)
{
    espagent_net_guard_defer_background(MESH_BACKGROUND_NET_DEFER_MS);

    char sandbox_reason[192] = {0};
    esp_err_t sandbox_err = tool_sandbox_check("mesh_send_command",
                                               input_json,
                                               sandbox_reason,
                                               sizeof(sandbox_reason));
    if (sandbox_err != ESP_OK) {
        snprintf(output, output_size, "Error: %s",
                 sandbox_reason[0] ? sandbox_reason : "sandbox denied mesh command");
        return sandbox_err;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *target_node = json_string(root, "target_node");
    const char *target_role = json_string(root, "target_role");
    const char *action = json_string(root, "action");
    if (!action || !action[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: action is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_allowed_mesh_action(action)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: unsupported mesh action=%s", action);
        return ESP_ERR_INVALID_ARG;
    }
    if ((!target_node || !target_node[0]) && (!target_role || !target_role[0]) &&
        is_sensor_action(action)) {
        target_role = "sensor_agent";
    } else if ((!target_node || !target_node[0]) && (!target_role || !target_role[0]) &&
               is_control_action(action)) {
        target_role = "control_agent";
    }
    if ((!target_node || !target_node[0]) && (!target_role || !target_role[0])) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "Error: target_node or target_role is required for action=%s",
                 action);
        return ESP_ERR_INVALID_ARG;
    }
    if (target_role && target_role[0]) {
        if (is_sensor_action(action) && strcmp(target_role, "sensor_agent") != 0) {
            cJSON_Delete(root);
            snprintf(output, output_size,
                     "Error: action=%s must target sensor_agent, got %s",
                     action, target_role);
            return ESP_ERR_INVALID_ARG;
        }
        if (is_control_action(action) && strcmp(target_role, "control_agent") != 0) {
            cJSON_Delete(root);
            snprintf(output, output_size,
                     "Error: action=%s must target control_agent, got %s",
                     action, target_role);
            return ESP_ERR_INVALID_ARG;
        }
        if (strcmp(action, "agent_task") == 0 &&
            strcmp(target_role, "sensor_agent") != 0 &&
            strcmp(target_role, "control_agent") != 0 &&
            strcmp(target_role, "guardian_agent") != 0) {
            cJSON_Delete(root);
            snprintf(output, output_size,
                     "Error: agent_task must target sensor_agent, control_agent, or guardian_agent, got %s",
                     target_role);
            return ESP_ERR_INVALID_ARG;
        }
    }

    char action_copy[ESPAGENT_MESH_ACTION_MAX] = {0};
    char command_id_copy[ESPAGENT_MESH_ID_MAX] = {0};
    char trace_id_copy[ESPAGENT_MESH_TRACE_MAX] = {0};
    char target_role_copy[ESPAGENT_MESH_ROLE_MAX] = {0};
    char target_node_copy[ESPAGENT_MESH_NODE_MAX] = {0};
    copy_text(action_copy, sizeof(action_copy), action);
    copy_text(target_role_copy, sizeof(target_role_copy), target_role);
    copy_text(target_node_copy, sizeof(target_node_copy), target_node);

    char topic[160] = {0};
    esp_err_t topic_err = ESP_OK;
    if (target_node_copy[0]) {
        topic_err = espagent_mesh_build_node_topic(target_node_copy, "command", topic, sizeof(topic));
    } else {
        topic_err = espagent_mesh_build_role_topic(target_role_copy, "command", topic, sizeof(topic));
    }
    if (topic_err != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to build MQTT command topic");
        return topic_err;
    }

    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    const char *command_id = json_string(root, "command_id");
    char generated_id[40] = {0};
    if (!command_id || !command_id[0]) {
        snprintf(generated_id, sizeof(generated_id), "cmd-%lld", (long long)(esp_timer_get_time() / 1000));
        command_id = generated_id;
    }
    copy_text(command_id_copy, sizeof(command_id_copy), command_id);

    const char *trace_id = json_string(root, "trace_id");
    char generated_trace_id[48] = {0};
    if (!trace_id || !trace_id[0]) {
        snprintf(generated_trace_id, sizeof(generated_trace_id), "trace-%s", command_id_copy);
        trace_id = generated_trace_id;
    }
    copy_text(trace_id_copy, sizeof(trace_id_copy), trace_id);

    int ttl_ms = json_int(root, "ttl_ms", 30000);
    if (ttl_ms < 1000) {
        ttl_ms = 1000;
    } else if (ttl_ms > 30000) {
        ttl_ms = 30000;
    }
    int safety_level = json_int(root, "safety_level", 1);
    if (safety_level < ESPAGENT_MESH_SAFETY_LOW ||
        safety_level > ESPAGENT_MESH_SAFETY_HIGH) {
        cJSON_Delete(cmd);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: safety_level must be 0, 1, or 2");
        return ESP_ERR_INVALID_ARG;
    }
    bool require_ack = json_bool(root, "require_ack", true);
    bool async_wait = json_bool(root, "async", true);
    const char *reply_channel = json_string(root, "reply_channel");
    const char *reply_chat_id = json_string(root, "reply_chat_id");
    char reply_channel_copy[16] = {0};
    char reply_chat_id_copy[96] = {0};
    copy_text(reply_channel_copy, sizeof(reply_channel_copy), reply_channel);
    copy_text(reply_chat_id_copy, sizeof(reply_chat_id_copy), reply_chat_id);

    int64_t ts_ms = esp_timer_get_time() / 1000;
    cJSON_AddStringToObject(cmd, "command_id", command_id_copy);
    cJSON_AddStringToObject(cmd, "trace_id", trace_id_copy);
    if (target_node_copy[0]) {
        cJSON_AddStringToObject(cmd, "target_node", target_node_copy);
    }
    if (target_role_copy[0]) {
        cJSON_AddStringToObject(cmd, "target_role", target_role_copy);
    }
    cJSON_AddStringToObject(cmd, "action", action);
    cJSON_AddNumberToObject(cmd, "ts_ms", (double)ts_ms);
    cJSON_AddNumberToObject(cmd, "ttl_ms", ttl_ms);
    cJSON_AddNumberToObject(cmd, "safety_level", safety_level);
    cJSON_AddBoolToObject(cmd, "require_ack", require_ack);

    esp_err_t args_err = copy_args(root, cmd);
    if (args_err != ESP_OK) {
        cJSON_Delete(cmd);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: args_json is not valid JSON");
        return args_err;
    }

    espagent_mesh_command_t sign_cmd = {0};
    snprintf(sign_cmd.command_id, sizeof(sign_cmd.command_id), "%s", command_id_copy);
    snprintf(sign_cmd.trace_id, sizeof(sign_cmd.trace_id), "%s", trace_id_copy);
    snprintf(sign_cmd.target_node, sizeof(sign_cmd.target_node), "%s", target_node_copy);
    snprintf(sign_cmd.target_role, sizeof(sign_cmd.target_role), "%s", target_role_copy);
    snprintf(sign_cmd.action, sizeof(sign_cmd.action), "%s", action_copy);
    sign_cmd.ttl_ms = ttl_ms;
    sign_cmd.safety_level = safety_level;
    sign_cmd.require_ack = require_ack;
    sign_cmd.ts_ms = ts_ms;
    cJSON *args_for_sign = cJSON_GetObjectItem(cmd, "args");
    char *args_printed = args_for_sign ? cJSON_PrintUnformatted(args_for_sign) : NULL;
    snprintf(sign_cmd.args_json, sizeof(sign_cmd.args_json), "%s", args_printed ? args_printed : "{}");
    cJSON_free(args_printed);
    char signature[ESPAGENT_MESH_SIGNATURE_MAX] = {0};
    esp_err_t sign_err = espagent_mesh_auth_sign_command(&sign_cmd, signature, sizeof(signature));
    if (sign_err != ESP_OK) {
        cJSON_Delete(cmd);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: failed to sign mesh command (%s)", esp_err_to_name(sign_err));
        return sign_err;
    }
    if (signature[0]) {
        cJSON_AddStringToObject(cmd, "signature", signature);
    }

    char *payload = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    cJSON_Delete(root);
    if (!payload) {
        snprintf(output, output_size, "Error: failed to serialize command");
        return ESP_ERR_NO_MEM;
    }

    char policy_json[MESH_POLICY_JSON_SIZE] = {0};
    char policy_reason[160] = {0};
    esp_err_t policy_err = request_policy_decision(command_id_copy,
                                                   trace_id_copy,
                                                   target_role_copy,
                                                   target_node_copy,
                                                   action_copy,
                                                   sign_cmd.args_json,
                                                   safety_level,
                                                   ttl_ms,
                                                   policy_json,
                                                   sizeof(policy_json),
                                                   policy_reason,
                                                   sizeof(policy_reason));
    if (policy_err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: Guardian policy blocked mesh command action=%s command_id=%s reason=%s decision=%s",
                 action_copy, command_id_copy, policy_reason, policy_json[0] ? policy_json : "(none)");
        (void)sensor_mqtt_publish_timeline_event("policy",
                                                 "policy_check",
                                                 "blocked",
                                                 output,
                                                 command_id_copy,
                                                 target_role_copy,
                                                 target_node_copy,
                                                 action_copy);
        cJSON_free(payload);
        return policy_err;
    }

    (void)sensor_mqtt_publish_timeline_event("policy",
                                             "policy_check",
                                             "ok",
                                             policy_reason[0] ? policy_reason : "Guardian allowed command",
                                             command_id_copy,
                                             target_role_copy,
                                             target_node_copy,
                                             action_copy);

    esp_err_t dispatch_err = sensor_mqtt_wait_connected(12000);
    if (dispatch_err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: MQTT is not connected for mesh dispatch action=%s command_id=%s (%s)",
                 action_copy, command_id_copy, esp_err_to_name(dispatch_err));
        (void)sensor_mqtt_publish_timeline_event("dispatch",
                                                 "mesh_command_queued",
                                                 "error",
                                                 output,
                                                 command_id_copy,
                                                 target_role_copy,
                                                 target_node_copy,
                                                 action_copy);
        cJSON_free(payload);
        return dispatch_err;
    }

    espagent_net_guard_defer_background(MESH_BACKGROUND_NET_DEFER_MS);
    esp_err_t err = publish_mesh_command_payload(topic,
                                                 payload,
                                                 target_role_copy,
                                                 require_ack);
    if (err == ESP_OK) {
        snprintf(output, output_size,
                 "OK: queued MQTT mesh command action=%s topic=%s command_id=%s",
                 action_copy, topic, command_id_copy);
        (void)sensor_mqtt_publish_timeline_event("dispatch",
                                                 "mesh_command_queued",
                                                 "ok",
                                                 output,
                                                 command_id_copy,
                                                 target_role_copy,
                                                 target_node_copy,
                                                 action_copy);
        if (require_ack) {
            uint32_t wait_ms = ttl_ms > 0 ? (uint32_t)ttl_ms : 30000U;
            if (async_wait) {
                if (reply_channel_copy[0] && reply_chat_id_copy[0]) {
                    char task_id[48] = {0};
                    esp_err_t task_err = start_mesh_wait_task(command_id_copy,
                                                              trace_id_copy,
                                                              target_role_copy,
                                                              target_node_copy,
                                                              action_copy,
                                                              reply_channel_copy,
                                                              reply_chat_id_copy,
                                                              wait_ms,
                                                              task_id,
                                                              sizeof(task_id));
                    if (task_err == ESP_OK) {
                        snprintf(output, output_size,
                                 "OK: queued MQTT mesh command action=%s topic=%s command_id=%s async_task_id=%s; result will be injected when OutputMessage arrives",
                                 action_copy, topic, command_id_copy, task_id);
                    } else {
                        snprintf(output, output_size,
                                 "OK: queued MQTT mesh command action=%s topic=%s command_id=%s; async wait task failed: %s",
                                 action_copy, topic, command_id_copy, esp_err_to_name(task_err));
                    }
                } else {
                    snprintf(output, output_size,
                             "OK: queued MQTT mesh command action=%s topic=%s command_id=%s; OutputMessage will be published on MQTT timeline/events",
                             action_copy, topic, command_id_copy);
                }
            } else {
                char output_json[MESH_OUTPUT_JSON_SIZE] = {0};
                esp_err_t wait_err = sensor_mqtt_wait_output_message(command_id_copy,
                                                                     output_json,
                                                                     sizeof(output_json),
                                                                     wait_ms);
                if (wait_err == ESP_OK) {
                    char compact_summary[448] = {0};
                    build_compact_output_summary(output_json,
                                                 compact_summary,
                                                 sizeof(compact_summary));
                    snprintf(output, output_size,
                             "OK: mesh command completed command_id=%s %s",
                             command_id_copy,
                             compact_summary[0] ? compact_summary
                                                : "result=ok");
                } else {
                    snprintf(output, output_size,
                             "OK: queued MQTT mesh command action=%s topic=%s command_id=%s; OutputMessage wait timed out after %ums",
                             action_copy, topic, command_id_copy, (unsigned)wait_ms);
                }
            }
        }
    } else {
        snprintf(output, output_size,
                 "Error: failed to queue MQTT mesh command (%s)",
                 esp_err_to_name(err));
        (void)sensor_mqtt_publish_timeline_event("dispatch",
                                                 "mesh_command_queued",
                                                 "error",
                                                 output,
                                                 command_id_copy,
                                                 target_role_copy,
                                                 target_node_copy,
                                                 action_copy);
    }
    cJSON_free(payload);
    return err;
}

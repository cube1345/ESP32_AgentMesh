#include "tools/tool_mesh_command.h"

#include "esp_timer.h"
#include "espagent_config.h"
#include "mesh/mesh_protocol.h"
#include "sensors/sensor_mqtt.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

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

static bool is_control_action(const char *action)
{
    return action &&
           (strcmp(action, "set_status_light") == 0 ||
            strcmp(action, "ws2812_set") == 0 ||
            strcmp(action, "servo_write") == 0 ||
            strcmp(action, "gpio_write") == 0);
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

esp_err_t tool_mesh_send_command_execute(const char *input_json,
                                         char *output,
                                         size_t output_size)
{
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
    if ((!target_node || !target_node[0]) && (!target_role || !target_role[0]) &&
        strcmp(action, "read_temperature_humidity") == 0) {
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

    char action_copy[ESPAGENT_MESH_ACTION_MAX] = {0};
    char command_id_copy[ESPAGENT_MESH_ID_MAX] = {0};
    copy_text(action_copy, sizeof(action_copy), action);

    char topic[160] = {0};
    esp_err_t topic_err = ESP_OK;
    if (target_node && target_node[0]) {
        topic_err = espagent_mesh_build_node_topic(target_node, "command", topic, sizeof(topic));
    } else {
        topic_err = espagent_mesh_build_role_topic(target_role, "command", topic, sizeof(topic));
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

    cJSON_AddStringToObject(cmd, "command_id", command_id_copy);
    if (target_node && target_node[0]) {
        cJSON_AddStringToObject(cmd, "target_node", target_node);
    }
    if (target_role && target_role[0]) {
        cJSON_AddStringToObject(cmd, "target_role", target_role);
    }
    cJSON_AddStringToObject(cmd, "action", action);
    cJSON_AddNumberToObject(cmd, "ttl_ms", json_int(root, "ttl_ms", 30000));
    cJSON_AddNumberToObject(cmd, "safety_level", json_int(root, "safety_level", 1));
    cJSON_AddBoolToObject(cmd, "require_ack", json_bool(root, "require_ack", true));

    esp_err_t args_err = copy_args(root, cmd);
    if (args_err != ESP_OK) {
        cJSON_Delete(cmd);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: args_json is not valid JSON");
        return args_err;
    }

    char *payload = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    cJSON_Delete(root);
    if (!payload) {
        snprintf(output, output_size, "Error: failed to serialize command");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sensor_mqtt_publish_text(topic, payload);
    if (err == ESP_OK) {
        snprintf(output, output_size,
                 "OK: queued MQTT mesh command action=%s topic=%s command_id=%s",
                 action_copy, topic, command_id_copy);
    } else {
        snprintf(output, output_size,
                 "Error: failed to queue MQTT mesh command (%s)",
                 esp_err_to_name(err));
    }
    cJSON_free(payload);
    return err;
}

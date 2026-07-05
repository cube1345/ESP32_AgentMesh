#include "tools/tool_gateway.h"

#include "cJSON.h"
#include "device/device_registry.h"
#include "espagent_config.h"
#include "gateway/ble_mesh_bridge.h"
#include "node/node_profile.h"
#include "sensors/sensor_mqtt.h"
#include "wifi/wifi_manager.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "tool_gateway";

static const char *json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_bool(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item) {
        return fallback;
    }
    return cJSON_IsTrue(item);
}

esp_err_t tool_gateway_status_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char devices[3072] = {0};
    char ble[512] = {0};
    esp_err_t dev_err = espagent_device_registry_to_json(devices, sizeof(devices));
    esp_err_t ble_err = espagent_ble_mesh_bridge_status_json(ble, sizeof(ble));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: OOM");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.gateway.status.v1");
    cJSON_AddStringToObject(root, "node_id", espagent_node_id());
    cJSON_AddStringToObject(root, "role", espagent_node_role());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    cJSON_AddNumberToObject(root, "ts_ms", (double)(esp_timer_get_time() / 1000));

    cJSON *dev_root = dev_err == ESP_OK ? cJSON_Parse(devices) : NULL;
    cJSON *ble_root = ble_err == ESP_OK ? cJSON_Parse(ble) : NULL;
    if (dev_root) {
        cJSON_AddItemToObject(root, "device_registry", dev_root);
    }
    if (ble_root) {
        cJSON_AddItemToObject(root, "ble_mesh", ble_root);
    }
    cJSON_AddStringToObject(root, "ota_gateway",
                            "plan_only: serial HTTPS OTA executor exists; remote OTA requires explicit Guardian/human confirmation before execution");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        snprintf(output, output_size, "Error: OOM");
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "OK: %s", json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t tool_gateway_register_ble_mesh_device_execute(const char *input_json, char *output, size_t output_size)
{
    return espagent_ble_mesh_bridge_register_device(input_json, output, output_size);
}

esp_err_t tool_gateway_ble_mesh_send_execute(const char *input_json, char *output, size_t output_size)
{
    return espagent_ble_mesh_bridge_send(input_json, output, output_size);
}

esp_err_t tool_ota_gateway_plan_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *target_node = json_string(root, "target_node");
    const char *target_role = json_string(root, "target_role");
    const char *url = json_string(root, "url");
    const char *version = json_string(root, "version");
    bool confirmed = json_bool(root, "confirmed", false);

    if (!url || strncmp(url, "https://", 8) != 0) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: OTA gateway requires an https:// firmware URL");
        return ESP_ERR_INVALID_ARG;
    }
    if ((!target_node || !target_node[0]) && (!target_role || !target_role[0])) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: target_node or target_role is required");
        return ESP_ERR_INVALID_ARG;
    }

    char target_label[8] = {0};
    char target_value[64] = {0};
    char version_note[32] = {0};
    snprintf(target_label, sizeof(target_label), "%s",
             target_node && target_node[0] ? "node=" : "role=");
    snprintf(target_value, sizeof(target_value), "%s",
             target_node && target_node[0] ? target_node : target_role);
    snprintf(version_note, sizeof(version_note), "%s",
             version && version[0] ? " version provided" : "");

    cJSON *event = cJSON_CreateObject();
    if (!event) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: OOM");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(event, "schema", "espagent.ota_gateway.plan.v1");
    cJSON_AddStringToObject(event, "event", "ota_gateway_plan");
    cJSON_AddStringToObject(event, "source_node", espagent_node_id());
    cJSON_AddStringToObject(event, "source_role", espagent_node_role());
    cJSON_AddStringToObject(event, "target_node", target_node ? target_node : "");
    cJSON_AddStringToObject(event, "target_role", target_role ? target_role : "");
    cJSON_AddStringToObject(event, "url", url);
    cJSON_AddStringToObject(event, "version", version ? version : "");
    cJSON_AddBoolToObject(event, "confirmed", confirmed);
    cJSON_AddStringToObject(event, "status", confirmed ? "ready_for_manual_execution" : "requires_confirmation");
    cJSON_AddStringToObject(event, "safety",
                            "OTA is plan-only in this firmware path; execute via serial ota_update or future Guardian-gated remote OTA command.");
    cJSON_AddNumberToObject(event, "ts_ms", (double)(esp_timer_get_time() / 1000));

    char *json = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    cJSON_Delete(root);
    if (!json) {
        snprintf(output, output_size, "Error: OOM");
        return ESP_ERR_NO_MEM;
    }

    (void)sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_TIMELINE, json);
    (void)sensor_mqtt_publish_text(ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json);
    snprintf(output,
             output_size,
             "OK: OTA gateway plan recorded for %s%s%s; %s. timeline=%s",
             target_label,
             target_value,
             version_note,
             confirmed ? "manual execution may proceed after operator check" : "confirmation is still required",
             json);
    ESP_LOGI(TAG, "OTA gateway plan: %s", json);
    cJSON_free(json);
    return ESP_OK;
}

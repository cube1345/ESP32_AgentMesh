#include "gateway/ble_mesh_bridge.h"

#include "cJSON.h"
#include "device/device_registry.h"
#include "espagent_config.h"
#include "node/node_profile.h"
#include "sensors/sensor_mqtt.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

#ifndef ESPAGENT_ENABLE_BLE_MESH_GATEWAY
#define ESPAGENT_ENABLE_BLE_MESH_GATEWAY 0
#endif

static const char *TAG = "ble_mesh_bridge";

static const char *json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void copy_field(char *dst, size_t dst_size, const char *src)
{
    if (dst && dst_size > 0) {
        snprintf(dst, dst_size, "%s", src ? src : "");
    }
}

esp_err_t espagent_ble_mesh_bridge_init(void)
{
    ESP_LOGI(TAG, "BLE Mesh bridge initialized: enabled=%d", ESPAGENT_ENABLE_BLE_MESH_GATEWAY);
    return ESP_OK;
}

esp_err_t espagent_ble_mesh_bridge_status_json(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(buf,
             buf_size,
             "{\"schema\":\"espagent.gateway.ble_mesh.v1\","
             "\"enabled\":%s,"
             "\"role\":\"%s\","
             "\"status\":\"%s\","
             "\"note\":\"%s\"}",
             ESPAGENT_ENABLE_BLE_MESH_GATEWAY ? "true" : "false",
             espagent_node_role(),
             ESPAGENT_ENABLE_BLE_MESH_GATEWAY ? "ready" : "not_linked",
             ESPAGENT_ENABLE_BLE_MESH_GATEWAY
                 ? "BLE Mesh backend is enabled"
                 : "BLE Mesh backend is not linked in this firmware; device registry and policy bridge are available only");
    return ESP_OK;
}

esp_err_t espagent_ble_mesh_bridge_register_device(const char *input_json,
                                                   char *output,
                                                   size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *device_id = json_string(root, "device_id");
    const char *name = json_string(root, "name");
    const char *address = json_string(root, "address");
    const char *capabilities = json_string(root, "capabilities");
    if (!device_id || !device_id[0] || !address || !address[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: device_id and address are required");
        return ESP_ERR_INVALID_ARG;
    }

    espagent_device_record_t record = {0};
    copy_field(record.id, sizeof(record.id), device_id);
    copy_field(record.name, sizeof(record.name), name && name[0] ? name : device_id);
    copy_field(record.protocol, sizeof(record.protocol), "ble_mesh");
    copy_field(record.role, sizeof(record.role), "gateway_device");
    copy_field(record.address, sizeof(record.address), address);
    copy_field(record.capabilities, sizeof(record.capabilities), capabilities);
    copy_field(record.state, sizeof(record.state), ESPAGENT_ENABLE_BLE_MESH_GATEWAY ? "known" : "bridge_not_linked");
    record.last_seen_ms = esp_timer_get_time() / 1000;
    record.external = true;

    esp_err_t err = espagent_device_registry_upsert(&record);
    cJSON_Delete(root);
    snprintf(output,
             output_size,
             "%s: registered BLE Mesh device id=%s addr=%s bridge=%s",
             err == ESP_OK ? "OK" : "Error",
             record.id,
             record.address,
             ESPAGENT_ENABLE_BLE_MESH_GATEWAY ? "enabled" : "not_linked");
    return err;
}

esp_err_t espagent_ble_mesh_bridge_send(const char *input_json,
                                        char *output,
                                        size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *device_id = json_string(root, "device_id");
    const char *address = json_string(root, "address");
    const char *opcode = json_string(root, "opcode");
    const char *data = json_string(root, "data");
    if ((!device_id || !device_id[0]) && (!address || !address[0])) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: device_id or address is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (!opcode || !opcode[0] || !data) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: opcode and data are required");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *event = cJSON_CreateObject();
    if (event) {
        cJSON_AddStringToObject(event, "schema", "espagent.gateway.ble_mesh_command.v1");
        cJSON_AddStringToObject(event, "event", "gateway_ble_mesh_command");
        cJSON_AddStringToObject(event, "node_id", espagent_node_id());
        cJSON_AddStringToObject(event, "role", espagent_node_role());
        cJSON_AddStringToObject(event, "device_id", device_id ? device_id : "");
        cJSON_AddStringToObject(event, "address", address ? address : "");
        cJSON_AddStringToObject(event, "opcode", opcode);
        cJSON_AddStringToObject(event, "data", data);
        cJSON_AddStringToObject(event, "status", ESPAGENT_ENABLE_BLE_MESH_GATEWAY ? "queued" : "not_linked");
        cJSON_AddNumberToObject(event, "ts_ms", (double)(esp_timer_get_time() / 1000));
        char *json = cJSON_PrintUnformatted(event);
        if (json) {
            (void)sensor_mqtt_publish_text(ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json);
            (void)sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_TIMELINE, json);
            cJSON_free(json);
        }
        cJSON_Delete(event);
    }

#if ESPAGENT_ENABLE_BLE_MESH_GATEWAY
    snprintf(output,
             output_size,
             "OK: BLE Mesh command queued device=%s address=%s opcode=%s data=%s",
             device_id ? device_id : "",
             address ? address : "",
             opcode,
             data);
    cJSON_Delete(root);
    return ESP_OK;
#else
    snprintf(output,
             output_size,
             "Error: BLE Mesh backend is not linked in this firmware; registered gateway command only. device=%s address=%s opcode=%s",
             device_id ? device_id : "",
             address ? address : "",
             opcode);
    cJSON_Delete(root);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}


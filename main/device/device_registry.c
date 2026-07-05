#include "device/device_registry.h"

#include "espagent_config.h"
#include "node/node_profile.h"
#include "cJSON.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_REGISTRY_MAX_ITEMS 24
#define DEVICE_REGISTRY_PATH ESPAGENT_SPIFFS_BASE "/device_registry.json"

static const char *TAG = "device_registry";

static espagent_device_record_t s_devices[DEVICE_REGISTRY_MAX_ITEMS];
static size_t s_device_count;
static SemaphoreHandle_t s_lock;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
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

static void registry_lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void registry_unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t registry_save_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "schema", "espagent.device_registry.v1");
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());
    cJSON_AddItemToObject(root, "devices", arr);

    for (size_t i = 0; i < s_device_count; i++) {
        const espagent_device_record_t *dev = &s_devices[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(obj, "id", dev->id);
        cJSON_AddStringToObject(obj, "name", dev->name);
        cJSON_AddStringToObject(obj, "protocol", dev->protocol);
        cJSON_AddStringToObject(obj, "role", dev->role);
        cJSON_AddStringToObject(obj, "address", dev->address);
        cJSON_AddStringToObject(obj, "capabilities", dev->capabilities);
        cJSON_AddStringToObject(obj, "state", dev->state);
        cJSON_AddNumberToObject(obj, "last_seen_ms", (double)dev->last_seen_ms);
        cJSON_AddBoolToObject(obj, "external", dev->external);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    FILE *fp = fopen(DEVICE_REGISTRY_PATH, "w");
    if (!fp) {
        cJSON_free(json);
        return ESP_FAIL;
    }
    fputs(json, fp);
    fclose(fp);
    cJSON_free(json);
    return ESP_OK;
}

static void registry_seed_local_locked(void)
{
    espagent_device_record_t local = {0};
    copy_field(local.id, sizeof(local.id), espagent_node_id());
    copy_field(local.name, sizeof(local.name), espagent_node_id());
    copy_field(local.protocol, sizeof(local.protocol), "mqtt_mesh");
    copy_field(local.role, sizeof(local.role), espagent_node_role());
    copy_field(local.address, sizeof(local.address), espagent_node_id());
    copy_field(local.capabilities, sizeof(local.capabilities), espagent_node_capabilities());
    copy_field(local.state, sizeof(local.state), "booting");
    local.last_seen_ms = now_ms();
    local.external = false;

    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].id, local.id) == 0) {
            s_devices[i] = local;
            return;
        }
    }
    if (s_device_count < DEVICE_REGISTRY_MAX_ITEMS) {
        s_devices[s_device_count++] = local;
    }
}

static esp_err_t registry_load_locked(void)
{
    FILE *fp = fopen(DEVICE_REGISTRY_PATH, "r");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }

    char *buf = calloc(1, 4096);
    if (!buf) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(buf, 1, 4095, fp);
    fclose(fp);
    if (n == 0) {
        free(buf);
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *root = cJSON_ParseWithLength(buf, n);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Ignoring invalid device registry JSON");
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *arr = cJSON_GetObjectItem(root, "devices");
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_device_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsObject(item) || s_device_count >= DEVICE_REGISTRY_MAX_ITEMS) {
            continue;
        }
        espagent_device_record_t *dev = &s_devices[s_device_count++];
        memset(dev, 0, sizeof(*dev));
        copy_field(dev->id, sizeof(dev->id), json_string(item, "id"));
        copy_field(dev->name, sizeof(dev->name), json_string(item, "name"));
        copy_field(dev->protocol, sizeof(dev->protocol), json_string(item, "protocol"));
        copy_field(dev->role, sizeof(dev->role), json_string(item, "role"));
        copy_field(dev->address, sizeof(dev->address), json_string(item, "address"));
        copy_field(dev->capabilities, sizeof(dev->capabilities), json_string(item, "capabilities"));
        copy_field(dev->state, sizeof(dev->state), json_string(item, "state"));
        cJSON *last_seen = cJSON_GetObjectItem(item, "last_seen_ms");
        dev->last_seen_ms = cJSON_IsNumber(last_seen) ? (int64_t)last_seen->valuedouble : 0;
        dev->external = cJSON_IsTrue(cJSON_GetObjectItem(item, "external"));
        if (dev->id[0] == '\0') {
            s_device_count--;
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t espagent_device_registry_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    registry_lock();
    (void)registry_load_locked();
    registry_seed_local_locked();
    esp_err_t err = registry_save_locked();
    registry_unlock();

    ESP_LOGI(TAG, "Device registry initialized: count=%u", (unsigned)s_device_count);
    return err;
}

esp_err_t espagent_device_registry_upsert(const espagent_device_record_t *record)
{
    if (!record || record->id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    registry_lock();
    espagent_device_record_t normalized = *record;
    if (normalized.last_seen_ms <= 0) {
        normalized.last_seen_ms = now_ms();
    }
    if (normalized.state[0] == '\0') {
        copy_field(normalized.state, sizeof(normalized.state), "known");
    }

    bool updated = false;
    for (size_t i = 0; i < s_device_count; i++) {
        if (strcmp(s_devices[i].id, normalized.id) == 0) {
            s_devices[i] = normalized;
            updated = true;
            break;
        }
    }
    if (!updated) {
        if (s_device_count >= DEVICE_REGISTRY_MAX_ITEMS) {
            registry_unlock();
            return ESP_ERR_NO_MEM;
        }
        s_devices[s_device_count++] = normalized;
    }

    esp_err_t err = registry_save_locked();
    registry_unlock();
    return err;
}

esp_err_t espagent_device_registry_note_mqtt_payload(const char *kind,
                                                     const char *payload,
                                                     size_t payload_len)
{
    if (!payload || payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *node_id = json_string(root, "node_id");
    if (!node_id || node_id[0] == '\0') {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }

    espagent_device_record_t record = {0};
    copy_field(record.id, sizeof(record.id), node_id);
    copy_field(record.name, sizeof(record.name), node_id);
    copy_field(record.protocol, sizeof(record.protocol), "mqtt_mesh");
    copy_field(record.role, sizeof(record.role), json_string(root, "role"));
    copy_field(record.address, sizeof(record.address), node_id);
    copy_field(record.capabilities, sizeof(record.capabilities), json_string(root, "capabilities"));
    copy_field(record.state, sizeof(record.state), json_string(root, "state"));
    if (record.state[0] == '\0') {
        copy_field(record.state, sizeof(record.state),
                   kind && strcmp(kind, "telemetry") == 0 ? "telemetry" : "online");
    }
    record.last_seen_ms = now_ms();
    record.external = false;
    cJSON_Delete(root);

    return espagent_device_registry_upsert(&record);
}

esp_err_t espagent_device_registry_to_json(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    registry_lock();
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        registry_unlock();
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.device_registry.v1");
    cJSON_AddNumberToObject(root, "count", (double)s_device_count);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());
    cJSON_AddItemToObject(root, "devices", arr);

    for (size_t i = 0; i < s_device_count; i++) {
        const espagent_device_record_t *dev = &s_devices[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddStringToObject(obj, "id", dev->id);
        cJSON_AddStringToObject(obj, "name", dev->name);
        cJSON_AddStringToObject(obj, "protocol", dev->protocol);
        cJSON_AddStringToObject(obj, "role", dev->role);
        cJSON_AddStringToObject(obj, "address", dev->address);
        cJSON_AddStringToObject(obj, "capabilities", dev->capabilities);
        cJSON_AddStringToObject(obj, "state", dev->state);
        cJSON_AddNumberToObject(obj, "last_seen_ms", (double)dev->last_seen_ms);
        cJSON_AddBoolToObject(obj, "external", dev->external);
        cJSON_AddItemToArray(arr, obj);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    registry_unlock();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(buf, buf_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

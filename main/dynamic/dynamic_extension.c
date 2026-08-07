#include "dynamic/dynamic_extension.h"

#include "cJSON.h"
#include "capability/capability_registry.h"
#include "esp_log.h"
#include "espagent_config.h"
#include "lua/espagent_lua_runtime.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "dynamic_ext";

esp_err_t dynamic_extension_init(void)
{
    ESP_LOGI(TAG, "Dynamic extension catalog initialized");
    return ESP_OK;
}

static bool is_manifest_file(const char *name)
{
    if (!name) {
        return false;
    }
    const char *json = strstr(name, ".json");
    if (!json || strcmp(json, ".json") != 0) {
        return false;
    }
    return strstr(name, ".json.sha256") == NULL;
}

static void strip_json_suffix(const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    const char *source = name ? name : "";
    size_t copy_len = strnlen(source, out_size - 1);
    memcpy(out, source, copy_len);
    out[copy_len] = '\0';
    char *suffix = strstr(out, ".json");
    if (suffix) {
        *suffix = '\0';
    }
}

static void add_protocol_notes(cJSON *root)
{
    cJSON *primitives = cJSON_CreateArray();
    if (!primitives) {
        return;
    }
    cJSON_AddItemToObject(root, "supported_manifest_primitives", primitives);

    const char *supported[] = {
        "i2c_register_read",
        "uart_query",
        "modbus_rtu_read",
        "spi_transfer_read",
        "adc_oneshot",
        "gpio_input",
        "gpio_output",
        "relay_control",
        "pwm_output",
        "ledc_pwm",
        "workflow_sequence",
        "condition_rule",
        "lua_runtime_optional",
    };
    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        cJSON_AddItemToArray(primitives, cJSON_CreateString(supported[i]));
    }

    cJSON *planned = cJSON_CreateArray();
    if (!planned) {
        return;
    }
    cJSON_AddItemToObject(root, "planned_primitives", planned);
    const char *future[] = {
        "can_twai",
        "ble_gatt",
        "one_wire",
        "i2s_pdm",
        "rmt_ir",
        "usb_cdc",
        "sdio_sdmmc",
    };
    for (size_t i = 0; i < sizeof(future) / sizeof(future[0]); i++) {
        cJSON_AddItemToArray(planned, cJSON_CreateString(future[i]));
    }
}

esp_err_t dynamic_extension_build_catalog(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON *devices = cJSON_CreateArray();
    if (!root || !devices) {
        cJSON_Delete(root);
        cJSON_Delete(devices);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.dynamic_extension.v1");
    cJSON_AddStringToObject(root, "manifest_dir", ESPAGENT_SPIFFS_BASE "/devices");
    cJSON_AddStringToObject(root, "lua_runtime_status", espagent_lua_runtime_status());
    cJSON_AddItemToObject(root, "devices", devices);
    add_protocol_notes(root);

    DIR *dir = opendir(ESPAGENT_SPIFFS_BASE "/devices");
    const char *dir_path = ESPAGENT_SPIFFS_BASE "/devices";
    if (!dir) {
        dir = opendir(ESPAGENT_SPIFFS_BASE);
        dir_path = ESPAGENT_SPIFFS_BASE;
    }
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (!is_manifest_file(entry->d_name)) {
                continue;
            }
            cJSON *obj = cJSON_CreateObject();
            if (!obj) {
                continue;
            }
            char device[64] = {0};
            strip_json_suffix(entry->d_name, device, sizeof(device));
            cJSON_AddStringToObject(obj, "device", device);
            cJSON_AddStringToObject(obj, "file", entry->d_name);
            cJSON_AddStringToObject(obj, "dir", dir_path);
            cJSON *contracts = cJSON_CreateArray();
            if (contracts) {
                char contract[2048] = {0};
                if (espagent_capability_write_contract_json("virtual_device_read",
                                                            contract, sizeof(contract)) == ESP_OK) {
                    cJSON *item = cJSON_Parse(contract);
                    if (item) cJSON_AddItemToArray(contracts, item);
                }
                memset(contract, 0, sizeof(contract));
                if (espagent_capability_write_contract_json("virtual_device_control",
                                                            contract, sizeof(contract)) == ESP_OK) {
                    cJSON *item = cJSON_Parse(contract);
                    if (item) cJSON_AddItemToArray(contracts, item);
                }
                cJSON_AddItemToObject(obj, "capability_contracts", contracts);
            }
            cJSON_AddItemToArray(devices, obj);
        }
        closedir(dir);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(buf, size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

#include "capability/capability_registry.h"

#include "capability/role_capability_profile.h"
#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "capability";

#define ESPAGENT_MAX_CAPABILITIES 64

static espagent_capability_descriptor_t s_caps[ESPAGENT_MAX_CAPABILITIES];
static int s_cap_count;

static bool streq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static bool starts_with(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static const char *legacy_family_for_tool(const char *name)
{
    if (streq(name, "web_search") || streq(name, "get_weather")) {
        return "network";
    }
    if (streq(name, "get_current_time")) {
        return "time";
    }
    if (starts_with(name, "automation_")) {
        return "automation";
    }
    if (streq(name, "mesh_send_command")) {
        return "mesh";
    }
    if (streq(name, "spawn_subagent")) {
        return "subagent";
    }
    if (starts_with(name, "cron_")) {
        return "scheduler";
    }
    if (starts_with(name, "lua_")) {
        return "script";
    }
    if (starts_with(name, "memory_") || starts_with(name, "skill_observation_")) {
        return "memory";
    }
    if (strstr(name, "file") || streq(name, "list_dir")) {
        return "file";
    }
    if (starts_with(name, "read_") || starts_with(name, "env_history_") ||
        strstr(name, "_read_") ||
        streq(name, "gpio_read") || streq(name, "gpio_read_all") ||
        streq(name, "virtual_device_read") || streq(name, "hc_sr05_read_distance")) {
        return "sensor";
    }
    if (streq(name, "gpio_write") || streq(name, "copper_gpio_write") ||
        streq(name, "set_humidifier") || streq(name, "set_fan") ||
        streq(name, "set_device_led") ||
        streq(name, "ws2812_set") ||
        streq(name, "set_status_light") || streq(name, "set_curtain") ||
        streq(name, "servo_write") ||
        streq(name, "gree_ac_control") || streq(name, "max98357_play_tone") ||
        streq(name, "virtual_device_control")) {
        return "control";
    }
    return "utility";
}

static espagent_capability_risk_t legacy_risk_for_tool(const char *name)
{
    if (streq(name, "web_search") || streq(name, "get_weather")) {
        return ESPAGENT_CAP_RISK_NETWORK;
    }
    if (streq(name, "write_file") || streq(name, "edit_file")) {
        return ESPAGENT_CAP_RISK_WRITE_FILE;
    }
    if (starts_with(name, "memory_") || starts_with(name, "skill_observation_")) {
        return ESPAGENT_CAP_RISK_PRIVACY;
    }
    if (streq(name, "lua_run_script") || streq(name, "lua_run_script_async") ||
        streq(name, "lua_run_source") || streq(name, "lua_stop_job")) {
        return ESPAGENT_CAP_RISK_SYSTEM;
    }
    if (streq(name, "gpio_write") || streq(name, "copper_gpio_write") ||
        streq(name, "set_humidifier") || streq(name, "set_fan") ||
        streq(name, "set_device_led") ||
        streq(name, "ws2812_set") ||
        streq(name, "set_status_light") || streq(name, "set_curtain") ||
        streq(name, "servo_write") ||
        streq(name, "gree_ac_control") || streq(name, "max98357_play_tone") ||
        streq(name, "virtual_device_control") ||
        starts_with(name, "automation_")) {
        return ESPAGENT_CAP_RISK_CONTROL;
    }
    if (streq(name, "mesh_send_command")) {
        return ESPAGENT_CAP_RISK_CONTROL;
    }
    return ESPAGENT_CAP_RISK_READ_ONLY;
}

static uint32_t legacy_flags_for_tool(const char *name)
{
    uint32_t flags = ESPAGENT_CAP_FLAG_LLM_VISIBLE;
    espagent_capability_risk_t risk = legacy_risk_for_tool(name);

    if (risk != ESPAGENT_CAP_RISK_READ_ONLY) {
        flags |= ESPAGENT_CAP_FLAG_RESTRICTED;
    }
    if (streq(name, "web_search") || streq(name, "get_weather") ||
        streq(name, "get_current_time") || streq(name, "read_file") ||
        streq(name, "list_dir")) {
        flags |= ESPAGENT_CAP_FLAG_SUBAGENT_ALLOWED;
    }
    if (streq(name, "virtual_device_read") || streq(name, "virtual_device_control")) {
        flags |= ESPAGENT_CAP_FLAG_DYNAMIC;
    }
    return flags;
}

esp_err_t espagent_capability_registry_init(void)
{
    s_cap_count = 0;
    ESP_LOGI(TAG, "Capability registry initialized");
    return ESP_OK;
}

esp_err_t espagent_capability_register(const espagent_capability_descriptor_t *descriptor)
{
    if (!descriptor || !descriptor->name || !descriptor->execute) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cap_count >= ESPAGENT_MAX_CAPABILITIES) {
        ESP_LOGE(TAG, "Capability registry full");
        return ESP_ERR_NO_MEM;
    }
    if (espagent_capability_find(descriptor->name)) {
        ESP_LOGW(TAG, "Capability already registered: %s", descriptor->name);
        return ESP_ERR_INVALID_STATE;
    }

    s_caps[s_cap_count++] = *descriptor;
    ESP_LOGI(TAG, "Registered capability: %s family=%s risk=%s",
             descriptor->name,
             descriptor->family ? descriptor->family : "unknown",
             espagent_capability_risk_name(descriptor->risk));
    return ESP_OK;
}

esp_err_t espagent_capability_register_legacy_tool(const char *name,
                                                   const char *description,
                                                   const char *input_schema_json,
                                                   espagent_capability_execute_fn execute)
{
    espagent_capability_descriptor_t cap = {
        .id = name,
        .name = name,
        .family = legacy_family_for_tool(name),
        .description = description,
        .input_schema_json = input_schema_json,
        .flags = legacy_flags_for_tool(name),
        .risk = legacy_risk_for_tool(name),
        .execute = execute,
    };
    return espagent_capability_register(&cap);
}

const espagent_capability_descriptor_t *espagent_capability_find(const char *name_or_id)
{
    if (!name_or_id) {
        return NULL;
    }
    for (int i = 0; i < s_cap_count; i++) {
        if (streq(s_caps[i].name, name_or_id) || streq(s_caps[i].id, name_or_id)) {
            return &s_caps[i];
        }
    }
    return NULL;
}

esp_err_t espagent_capability_execute(const char *name_or_id,
                                      const char *input_json,
                                      espagent_capability_caller_t caller,
                                      char *output,
                                      size_t output_size)
{
    const espagent_capability_descriptor_t *cap = espagent_capability_find(name_or_id);
    if (!cap) {
        snprintf(output, output_size, "Error: unknown capability '%s'", name_or_id ? name_or_id : "");
        return ESP_ERR_NOT_FOUND;
    }
    if (!espagent_capability_callable_by_current_role(cap, caller)) {
        snprintf(output, output_size, "Error: capability '%s' is not visible to this role", cap->name);
        return ESP_ERR_NOT_ALLOWED;
    }
    return cap->execute(input_json, output, output_size);
}

char *espagent_capability_build_llm_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }

    int visible = 0;
    for (int i = 0; i < s_cap_count; i++) {
        const espagent_capability_descriptor_t *cap = &s_caps[i];
        if (!espagent_capability_visible_to_current_role(cap)) {
            continue;
        }

        cJSON *tool = cJSON_CreateObject();
        if (!tool) {
            continue;
        }
        cJSON_AddStringToObject(tool, "name", cap->name);
        cJSON_AddStringToObject(tool, "description", cap->description ? cap->description : "");
        cJSON_AddStringToObject(tool, "family", cap->family ? cap->family : "utility");
        cJSON_AddStringToObject(tool, "risk", espagent_capability_risk_name(cap->risk));

        cJSON *schema = cJSON_Parse(cap->input_schema_json ? cap->input_schema_json : "{}");
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }
        cJSON_AddItemToArray(arr, tool);
        visible++;
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "Built LLM capability tools JSON: visible=%d total=%d", visible, s_cap_count);
    return json;
}

void espagent_capability_list(const espagent_capability_descriptor_t **items, int *count)
{
    if (items) {
        *items = s_caps;
    }
    if (count) {
        *count = s_cap_count;
    }
}

const char *espagent_capability_risk_name(espagent_capability_risk_t risk)
{
    switch (risk) {
    case ESPAGENT_CAP_RISK_READ_ONLY:
        return "read_only";
    case ESPAGENT_CAP_RISK_WRITE_FILE:
        return "write_file";
    case ESPAGENT_CAP_RISK_CONTROL:
        return "control";
    case ESPAGENT_CAP_RISK_NETWORK:
        return "network";
    case ESPAGENT_CAP_RISK_PRIVACY:
        return "privacy";
    case ESPAGENT_CAP_RISK_SYSTEM:
        return "system";
    default:
        return "unknown";
    }
}

#include "tool_registry.h"

#include "espagent_config.h"
#include "tools/tool_files.h"
#include "tools/tool_get_time.h"
#include "tools/tool_lua.h"
#include "tools/tool_mesh_command.h"
#include "tools/tool_sandbox.h"
#include "tools/tool_subagent.h"
#include "tools/tool_virtual_device.h"
#include "tools/tool_gpio.h"
#include "tools/tool_aht10.h"
#include "tools/tool_environment.h"
#include "tools/tool_hc_sr05.h"
#include "tools/tool_servo.h"
#include "tools/tool_sensor_history.h"
#include "tools/tool_gree_ac.h"
#include "tools/tool_max98357.h"
#include "tools/tool_sgp30.h"
#include "tools/tool_bh1750.h"
#include "tools/tool_web_search.h"
#include "tools/tool_amap_weather.h"
#include "capability/capability_registry.h"
#include "events/espagent_event.h"
#include "memory/memory_v2.h"
#include "roles/role_config.h"
#include "sensors/sensor_mqtt.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 64

static espagent_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;
static char *s_tools_json_compact_coordinator = NULL;
static char *s_tools_json_mesh_only = NULL;

static void publish_sandbox_denial_observability(const char *name,
                                                 esp_err_t sandbox_err,
                                                 const char *sandbox_reason)
{
    char summary[192] = {0};
    snprintf(summary, sizeof(summary), "%s blocked by sandbox",
             name && name[0] ? name : "tool");
    (void)sensor_mqtt_publish_output_message("sandbox_denied",
                                             NULL,
                                             NULL,
                                             name,
                                             "local_client",
                                             sandbox_err,
                                             summary,
                                             sandbox_reason && sandbox_reason[0]
                                                 ? sandbox_reason
                                                 : "sandbox denied tool call");
}

static bool json_has_key(cJSON *root, const char *key)
{
    return root && cJSON_GetObjectItem(root, key) != NULL;
}

static bool json_bool_value(cJSON *root, const char *key, bool default_value)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    if (!item) {
        return default_value;
    }
    return cJSON_IsTrue(item);
}

static const char *json_string_value(cJSON *root, const char *key)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t tool_memory_profile_set_execute(const char *input_json,
                                                 char *output,
                                                 size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *key = json_string_value(root, "key");
    const char *value = json_string_value(root, "value");
    const char *source = json_string_value(root, "source");
    cJSON *confidence_item = cJSON_GetObjectItem(root, "confidence");
    float confidence = cJSON_IsNumber(confidence_item) ? (float)confidence_item->valuedouble : 0.7f;
    if (!key || !key[0] || !value) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: key and value are required");
        return ESP_ERR_INVALID_ARG;
    }
    char key_copy[64];
    memory_v2_normalize_profile_key(key, key_copy, sizeof(key_copy));
    if (key_copy[0] == '\0') {
        snprintf(key_copy, sizeof(key_copy), "%s", key);
    }

    char change_note[192] = {0};
    esp_err_t err = memory_v2_upsert_profile_fact(key_copy,
                                                  value,
                                                  source ? source : "agent",
                                                  confidence,
                                                  change_note,
                                                  sizeof(change_note));
    cJSON_Delete(root);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: structured profile fact saved key=%s%s%s",
                 key_copy,
                 change_note[0] ? " (" : "",
                 change_note[0] ? change_note : "");
        if (change_note[0]) {
            size_t len = strlen(output);
            if (len + 1 < output_size) {
                snprintf(output + len, output_size - len, ")");
            }
        }
    } else {
        snprintf(output, output_size, "Error: profile save failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t tool_skill_observation_add_execute(const char *input_json,
                                                    char *output,
                                                    size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *skill = json_string_value(root, "skill");
    const char *status = json_string_value(root, "status");
    const char *summary = json_string_value(root, "summary");
    if (!skill || !skill[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: skill is required");
        return ESP_ERR_INVALID_ARG;
    }
    char skill_copy[64];
    snprintf(skill_copy, sizeof(skill_copy), "%s", skill);

    esp_err_t err = memory_v2_append_skill_observation(skill, status, summary);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: skill observation saved skill=%s", skill_copy);
    } else {
        snprintf(output, output_size, "Error: skill observation save failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t tool_read_temperature_humidity_execute(const char *input_json,
                                                        char *output,
                                                        size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    bool local = false;
    bool has_local_diagnostics = false;
    if (root && cJSON_IsObject(root)) {
        local = json_bool_value(root, "local", false);
        has_local_diagnostics = json_has_key(root, "sda_gpio") ||
                                json_has_key(root, "scl_gpio") ||
                                json_has_key(root, "i2c_port") ||
                                json_has_key(root, "scl_hz") ||
                                json_has_key(root, "address");
    }
    if (root) {
        cJSON_Delete(root);
    }

    if (espagent_role_is_coordinator() && !local && !has_local_diagnostics) {
        return tool_mesh_send_command_execute(
            "{\"target_role\":\"sensor_agent\",\"action\":\"read_temperature_humidity\",\"args\":{}}",
            output,
            output_size);
    }

    if (!has_local_diagnostics) {
        tool_environment_values_t values = {0};
        char status[96] = {0};
        esp_err_t env_err = tool_environment_read_values(&values, status, sizeof(status));
        if (env_err == ESP_OK &&
            values.temperature_c_x10 != -1 &&
            values.humidity_percent_x10 != -1) {
            snprintf(output, output_size,
                     "OK: AHT20 on SDA=%d SCL=%d addr=0x%02x -> temperature=%.1f C, humidity=%.1f%% [%s]",
                     ESPAGENT_AHT10_DEFAULT_SDA_GPIO,
                     ESPAGENT_AHT10_DEFAULT_SCL_GPIO,
                     ESPAGENT_AHT10_DEFAULT_ADDR,
                     (double)values.temperature_c_x10 / 10.0,
                     (double)values.humidity_percent_x10 / 10.0,
                     status);
            return ESP_OK;
        }

        snprintf(output, output_size,
                 "Error: AHT20 not readable on SDA=%d SCL=%d addr=0x%02x [%s]",
                 ESPAGENT_AHT10_DEFAULT_SDA_GPIO,
                 ESPAGENT_AHT10_DEFAULT_SCL_GPIO,
                 ESPAGENT_AHT10_DEFAULT_ADDR,
                 status[0] ? status : esp_err_to_name(env_err));
        return env_err == ESP_OK ? ESP_ERR_NOT_FOUND : env_err;
    }

    return tool_aht10_read_temperature_humidity_execute(input_json, output, output_size);
}

typedef esp_err_t (*tool_exec_fn_t)(const char *input_json, char *output, size_t output_size);

static esp_err_t route_control_action_to_mesh(const char *action,
                                              const char *input_json,
                                              char *output,
                                              size_t output_size)
{
    cJSON *args = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!args || !cJSON_IsObject(args)) {
        if (args) {
            cJSON_Delete(args);
        }
        args = cJSON_CreateObject();
    }
    if (!args) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(args, "local");

    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) {
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(cmd, "target_role", "control_agent");
    cJSON_AddStringToObject(cmd, "action", action);
    cJSON_AddItemToObject(cmd, "args", args);

    char *payload = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    if (!payload) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = tool_mesh_send_command_execute(payload, output, output_size);
    cJSON_free(payload);
    return err;
}

static esp_err_t coordinator_control_or_local(const char *action,
                                              const char *input_json,
                                              char *output,
                                              size_t output_size,
                                              tool_exec_fn_t local_execute)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    bool local = false;
    if (root && cJSON_IsObject(root)) {
        local = json_bool_value(root, "local", false);
    }
    if (root) {
        cJSON_Delete(root);
    }

    if (espagent_role_is_coordinator() && !local) {
        return route_control_action_to_mesh(action, input_json, output, output_size);
    }

    return local_execute(input_json, output, output_size);
}

static esp_err_t tool_gpio_write_routed_execute(const char *input_json,
                                                char *output,
                                                size_t output_size)
{
    return coordinator_control_or_local("gpio_write", input_json, output, output_size,
                                        tool_gpio_write_execute);
}

static esp_err_t tool_copper_gpio_write_routed_execute(const char *input_json,
                                                       char *output,
                                                       size_t output_size)
{
    return coordinator_control_or_local("copper_gpio_write", input_json, output, output_size,
                                        tool_copper_gpio_write_execute);
}

static esp_err_t tool_set_humidifier_routed_execute(const char *input_json,
                                                    char *output,
                                                    size_t output_size)
{
    return coordinator_control_or_local("set_humidifier", input_json, output, output_size,
                                        tool_set_humidifier_execute);
}

static esp_err_t tool_set_fan_routed_execute(const char *input_json,
                                             char *output,
                                             size_t output_size)
{
    return coordinator_control_or_local("set_fan", input_json, output, output_size,
                                        tool_set_fan_execute);
}

static esp_err_t tool_set_device_led_routed_execute(const char *input_json,
                                                    char *output,
                                                    size_t output_size)
{
    return coordinator_control_or_local("set_device_led", input_json, output, output_size,
                                        tool_set_device_led_execute);
}

static esp_err_t tool_ws2812_set_routed_execute(const char *input_json,
                                                char *output,
                                                size_t output_size)
{
    return coordinator_control_or_local("ws2812_set", input_json, output, output_size,
                                        tool_ws2812_set_execute);
}

static esp_err_t tool_set_status_light_routed_execute(const char *input_json,
                                                      char *output,
                                                      size_t output_size)
{
    return coordinator_control_or_local("set_status_light", input_json, output, output_size,
                                        tool_set_status_light_execute);
}

static esp_err_t tool_servo_write_routed_execute(const char *input_json,
                                                 char *output,
                                                 size_t output_size)
{
    return coordinator_control_or_local("servo_write", input_json, output, output_size,
                                        tool_servo_write_execute);
}

static esp_err_t tool_set_curtain_routed_execute(const char *input_json,
                                                 char *output,
                                                 size_t output_size)
{
    return coordinator_control_or_local("set_curtain", input_json, output, output_size,
                                        tool_set_curtain_execute);
}

static esp_err_t tool_gree_ac_control_routed_execute(const char *input_json,
                                                     char *output,
                                                     size_t output_size)
{
    return coordinator_control_or_local("gree_ac_control", input_json, output, output_size,
                                        tool_gree_ac_control_execute);
}

static void register_tool(const espagent_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }

    s_tools[s_tool_count++] = *tool;
    esp_err_t cap_err = espagent_capability_register_legacy_tool(tool->name,
                                                                tool->description,
                                                                tool->input_schema_json,
                                                                tool->execute);
    if (cap_err != ESP_OK) {
        ESP_LOGW(TAG, "Capability mirror register failed for %s: %s",
                 tool->name, esp_err_to_name(cap_err));
    }
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static bool coordinator_compact_tool_allowed(const char *name)
{
    static const char *const allowed[] = {
        "web_search",
        "get_weather",
        "get_current_time",
        "mesh_send_command",
        "read_file",
        "list_dir",
        "memory_profile_set",
        "skill_observation_add",
        "lua_runtime_info",
        "lua_list_scripts",
    };

    if (!name) {
        return false;
    }

    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(name, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool mesh_only_tool_allowed(const char *name)
{
    return name && strcmp(name, "mesh_send_command") == 0;
}

static char *build_tools_json_from_registry(bool (*filter_fn)(const char *name),
                                            const char *label)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return NULL;
    }

    int visible = 0;

    for (int i = 0; i < s_tool_count; i++) {
        if (filter_fn && !filter_fn(s_tools[i].name)) {
            continue;
        }
        cJSON *tool = cJSON_CreateObject();
        if (!tool) {
            continue;
        }
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
        visible++;
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    ESP_LOGI(TAG, "Tools JSON built (%s visible=%d total=%d)",
             label ? label : "default",
             visible,
             s_tool_count);
    return json;
}

static void build_tools_json(void)
{
    char *cap_json = espagent_capability_build_llm_tools_json();
    if (cap_json) {
        free(s_tools_json);
        s_tools_json = cap_json;
    } else {
        free(s_tools_json);
        s_tools_json = build_tools_json_from_registry(NULL, "fallback-default");
    }

    free(s_tools_json_compact_coordinator);
    s_tools_json_compact_coordinator =
        build_tools_json_from_registry(coordinator_compact_tool_allowed,
                                       "coordinator-compact");

    free(s_tools_json_mesh_only);
    s_tools_json_mesh_only =
        build_tools_json_from_registry(mesh_only_tool_allowed, "mesh-only");
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;
    espagent_capability_registry_init();

    tool_web_search_init();
    tool_amap_weather_init();
    tool_gpio_init();

    register_tool(&(espagent_tool_t){
        .name = "web_search",
        .description = "Search the web for current information via Tavily (preferred) or Brave when configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "get_weather",
        .description = "Get structured current or forecast weather from Amap WebService. Prefer this over web_search for weather, temperature, rain, wind, forecast, 出门建议, 天气, 气温, 下雨, 降温, 穿衣, or daily proactive weather. Defaults to the configured home location when no location/adcode is provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"location\":{\"type\":\"string\",\"description\":\"Optional city/district/address such as 南京市栖霞区. If it is not an adcode, Amap geocoding resolves it first.\"},"
            "\"adcode\":{\"type\":\"string\",\"description\":\"Optional 6-digit Amap adcode. Overrides location when provided.\"},"
            "\"extensions\":{\"type\":\"string\",\"description\":\"'base' for live weather or 'all' for forecast. Defaults to 'base'.\"},"
            "\"type\":{\"type\":\"string\",\"description\":\"Optional alias: 'live' maps to base, 'forecast' maps to all.\"}},"
            "\"required\":[]}",
        .execute = tool_amap_weather_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_get_time_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_runtime_info",
        .description = "Report whether the optional Lua runtime is available on this firmware, plus script roots, size limit, and timeout limits. Call this before lua_run_script when unsure.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}",
        .execute = tool_lua_runtime_info_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_list_modules",
        .description = "List Lua modules available in this firmware. ESPAgent exposes a safe espagent module and restricted standard libraries; direct hardware modules are intentionally not linked unless shown here.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}",
        .execute = tool_lua_list_modules_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_list_scripts",
        .description = "List runnable Lua scripts under /spiffs/scripts/ and /spiffs/skills/. Use this before lua_run_script when the path is unknown.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}",
        .execute = tool_lua_list_scripts_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_run_source",
        .description = "Run bounded inline Lua source with the same safe ESPAgent Lua environment as lua_run_script. Requires confirmed=true. Prefer known script files for production; use inline source for development smoke tests.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"source\":{\"type\":\"string\",\"description\":\"Lua source code, bounded by ESPAGENT_LUA_MAX_SCRIPT_BYTES\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Optional arguments exposed to Lua as args and ESPAGENT_ARGS_JSON\"},"
            "\"args_json\":{\"type\":\"string\",\"description\":\"Optional raw JSON object string exposed to Lua\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000,\"description\":\"Execution timeout, defaults to 1000ms and max 5000ms\"},"
            "\"confirmed\":{\"type\":\"boolean\",\"description\":\"Required because inline Lua is a script execution capability\"}},"
            "\"required\":[\"source\",\"confirmed\"],\"additionalProperties\":false}",
        .execute = tool_lua_run_source_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_run_script",
        .description = "Run a bounded Lua script from SPIFFS. Scripts must be absolute .lua paths under /spiffs/scripts/ or /spiffs/skills/, with no '..'. The runtime is optional: if Lua is not linked, this returns a clear not-supported error instead of pretending to execute. Hardware access must still go through ESPAgent capabilities, sandbox, Mesh, and Guardian; Lua scripts must not bypass safety controls.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute .lua path under /spiffs/scripts/ or /spiffs/skills/\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Optional arguments exposed to Lua as ESPAGENT_ARGS_JSON\"},"
            "\"args_json\":{\"type\":\"string\",\"description\":\"Optional raw JSON object string exposed as ESPAGENT_ARGS_JSON\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000,\"description\":\"Execution timeout, defaults to 1000ms and max 5000ms\"},"
            "\"confirmed\":{\"type\":\"boolean\",\"description\":\"Required because Lua is a script execution capability\"}},"
            "\"required\":[\"path\",\"confirmed\"],\"additionalProperties\":false}",
        .execute = tool_lua_run_script_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_run_script_async",
        .description = "Start a bounded Lua script as a background job. Use this for scripts that may take longer than one tool turn. Requires confirmed=true and the same /spiffs/scripts/ or /spiffs/skills/ path limits as lua_run_script.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute .lua path under /spiffs/scripts/ or /spiffs/skills/\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Optional arguments exposed to Lua as ESPAGENT_ARGS_JSON\"},"
            "\"args_json\":{\"type\":\"string\",\"description\":\"Optional raw JSON object string exposed as ESPAGENT_ARGS_JSON\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000,\"description\":\"Execution timeout, defaults to 1000ms and max 5000ms\"},"
            "\"confirmed\":{\"type\":\"boolean\",\"description\":\"Required because Lua is a script execution capability\"}},"
            "\"required\":[\"path\",\"confirmed\"],\"additionalProperties\":false}",
        .execute = tool_lua_run_script_async_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_list_jobs",
        .description = "List recent Lua async jobs with state, path, timing, and captured output.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[],\"additionalProperties\":false}",
        .execute = tool_lua_list_jobs_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_get_job",
        .description = "Get one Lua async job by job_id, including state and captured output.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"Lua job id returned by lua_run_script_async\"}},"
            "\"required\":[\"job_id\"],\"additionalProperties\":false}",
        .execute = tool_lua_get_job_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "lua_stop_job",
        .description = "Request cooperative stop for a running Lua async job. The runtime hook observes the stop flag when Lua is linked.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"Lua job id returned by lua_run_script_async\"}},"
            "\"required\":[\"job_id\"],\"additionalProperties\":false}",
        .execute = tool_lua_stop_job_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "memory_profile_set",
        .description = "Store or update a structured user-profile fact in Memory v2. Use this for stable user preferences, habits, constraints, and repeated contradictions with previous preferences. Keys are normalized into stable namespaces such as env.humidity_preference, env.temperature_preference, habit.sleep_schedule, device.light_preference, device.humidifier_policy, and privacy.data_sharing.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Stable profile key. Prefer env.humidity_preference, env.temperature_preference, env.light_preference, env.air_quality_preference, habit.sleep_schedule, habit.wake_schedule, device.light_preference, device.humidifier_policy, device.fan_policy, device.ac_policy, or privacy.data_sharing.\"},"
            "\"value\":{\"type\":\"string\",\"description\":\"Current fact or preference value. Keep it short and specific, for example humidity<40 => open humidifier, preferred_color=blue, bedtime=23:30.\"},"
            "\"source\":{\"type\":\"string\",\"description\":\"Where this fact came from, e.g. feishu, observation, correction\"},"
            "\"confidence\":{\"type\":\"number\",\"minimum\":0,\"maximum\":1,\"description\":\"Confidence in this fact, defaults to 0.7\"}},"
            "\"required\":[\"key\",\"value\"],\"additionalProperties\":false}",
        .execute = tool_memory_profile_set_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "skill_observation_add",
        .description = "Record a structured observation about a skill, capability, benchmark case, hardware manifest, or validation result. Use this after tests or repeated failures so future prompts know which skills are reliable.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"skill\":{\"type\":\"string\",\"description\":\"Skill or capability name\"},"
            "\"status\":{\"type\":\"string\",\"description\":\"observed, passed, failed, partial, blocked\"},"
            "\"summary\":{\"type\":\"string\",\"description\":\"Short validation note\"}},"
            "\"required\":[\"skill\"],\"additionalProperties\":false}",
        .execute = tool_skill_observation_add_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_temperature_humidity",
        .description = "Read temperature and humidity from this board's local AHT10/AHT20 I2C sensor. On a coordinator_agent, do not use this for ordinary Feishu room temperature/humidity requests; route those through mesh_send_command to sensor_agent unless the user explicitly asks for this board, local sensor, or I2C wiring diagnostics. Optional SDA/SCL GPIO overrides can be provided for wiring diagnostics.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SDA GPIO override\"},"
            "\"scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SCL GPIO override\"},"
            "\"i2c_port\":{\"type\":\"integer\",\"description\":\"Optional I2C port override\"},"
            "\"scl_hz\":{\"type\":\"integer\",\"description\":\"Optional I2C clock speed in Hz, defaults to 100000\"},"
            "\"address\":{\"type\":\"integer\",\"description\":\"Optional AHT10 I2C address, normally 0x38\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly testing this board's local AHT10/AHT20 sensor instead of routing through sensor_agent\"}},"
            "\"required\":[]}",
        .execute = tool_read_temperature_humidity_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "mesh_send_command",
        .description = "Publish a standard MQTT Mesh command to another ESPAgent node or role. Use structured actions for deterministic hardware work. Use action=agent_task with args.task when the coordinator should delegate a natural-language subtask to a remote role's own local AI loop. For ordinary environment requests such as '读取温湿度/光照', use action=read_environment or read_temperature_humidity and target_role=sensor_agent; target_node is optional. For remote WS2812 light, curtain servo, GPIO, or Gree air-conditioner requests, use target_role=control_agent with the matching action and structured args. Do not claim a Mesh command was sent unless this tool returns OK.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"target_node\":{\"type\":\"string\",\"description\":\"Optional target node id such as esp32s3-sensor-01. Overrides target_role when set.\"},"
            "\"target_role\":{\"type\":\"string\",\"enum\":[\"sensor_agent\",\"control_agent\",\"guardian_agent\"],\"description\":\"Optional target role. Use sensor_agent for reads, control_agent for actuators, guardian_agent for policy/audit subtasks.\"},"
            "\"action\":{\"type\":\"string\",\"enum\":[\"agent_task\",\"read_temperature_humidity\",\"read_environment\",\"read_light_level\",\"virtual_device_read\",\"virtual_device_control\",\"set_status_light\",\"ws2812_set\",\"set_curtain\",\"servo_write\",\"set_humidifier\",\"set_fan\",\"set_device_led\",\"copper_gpio_write\",\"gpio_write\",\"gree_ac_control\",\"control_state\",\"control_emergency_stop\",\"control_clear_emergency_stop\",\"guardian_approval_list\",\"guardian_approval_confirm\",\"guardian_approval_deny\"],\"description\":\"Whitelisted mesh command action. agent_task delegates args.task to the target role's local AI loop; guardian_approval_* actions resolve Guardian approval requests on the guardian_agent.\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Optional JSON arguments for the command. For agent_task, include task, reply_channel, and reply_chat_id when a user-facing response is needed.\"},"
            "\"args_json\":{\"type\":\"string\",\"description\":\"Optional raw JSON object string for arguments\"},"
            "\"command_id\":{\"type\":\"string\",\"description\":\"Optional command id. Auto-generated when omitted.\"},"
            "\"trace_id\":{\"type\":\"string\",\"description\":\"Optional trace id shared across the user request and downstream OutputMessage\"},"
            "\"ttl_ms\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":30000,\"description\":\"Command time-to-live in milliseconds, defaults to 30000\"},"
            "\"safety_level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2,\"description\":\"Safety level hint: 0 low, 1 medium, 2 high; defaults to 1\"},"
            "\"require_ack\":{\"type\":\"boolean\",\"description\":\"Whether the remote node should acknowledge, defaults to true\"},"
            "\"async\":{\"type\":\"boolean\",\"description\":\"When true, return immediately and inject the remote OutputMessage later; defaults to true\"}},"
            "\"required\":[\"action\"],\"additionalProperties\":false}",
        .execute = tool_mesh_send_command_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "virtual_device_read",
        .description = "Read or perform a bounded UART exchange with a simple runtime hardware device described by /spiffs/devices/<device>.json. Current phase supports bounded I2C, UART query, Modbus RTU function 3/4 register reads, SPI transfer-read, ADC one-shot, and GPIO input manifests; I2C decode types include raw_u8, raw_u16_be/le, and aht20_temp_humidity. For UART manifests, command_ascii or command_bytes may be overridden at call time, and expect_response=false enables send-only serial pushes such as HC-05 phone bridge text output. The manifest must use manifest_version=1, role=sensor_agent, permissions=[read], and risk=read_only. On a coordinator_agent, this routes to sensor_agent unless local=true is explicitly set. Use this when a developer added a protocol manifest for a new simple sensor or serial module and no dedicated C tool exists.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"device\":{\"type\":\"string\",\"description\":\"Device manifest name, loaded from /spiffs/devices/<device>.json\"},"
            "\"command_ascii\":{\"type\":\"string\",\"description\":\"Optional runtime UART ASCII payload override for uart manifests such as HC-05 bridge text\"},"
            "\"expect_response\":{\"type\":\"boolean\",\"description\":\"For uart manifests, set false for send-only writes that should not wait for reply\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly reading this board locally instead of routing to sensor_agent\"}},"
            "\"required\":[\"device\"],\"additionalProperties\":false}",
        .execute = tool_virtual_device_read_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "virtual_device_control",
        .description = "Control a bounded runtime hardware device described by /spiffs/devices/<device>.json. Current phase supports gpio_output, relay_control, pwm_output, and ledc_pwm manifests with allowlisted pins, max_duration_ms, cooldown_ms, safe_level, required SHA-256 sidecar, and confirmed=true requirements for persistent high-impact actions. Bounded duration schedules a background safe-state restore rather than blocking the caller. On a coordinator_agent, this routes to control_agent unless local=true is explicitly set.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"device\":{\"type\":\"string\",\"description\":\"Device manifest name, loaded from /spiffs/devices/<device>.json\"},"
            "\"level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1,\"description\":\"GPIO output level for gpio_output/relay_control\"},"
            "\"active\":{\"type\":\"boolean\",\"description\":\"Set relay/control device active or safe\"},"
            "\"duty_pct\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100,\"description\":\"PWM duty percentage\"},"
            "\"duration_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":30000,\"description\":\"Bounded duration before returning to safe state; 0 means persistent\"},"
            "\"confirmed\":{\"type\":\"boolean\",\"description\":\"Required for persistent high-impact control\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this board locally instead of routing to control_agent\"}},"
            "\"required\":[\"device\"],\"additionalProperties\":false}",
        .execute = tool_virtual_device_control_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "spawn_subagent",
        .description = "Delegate an independent subtask to a temporary ESPAgent subagent. The subagent runs its own short ReAct tool loop, cannot spawn nested subagents, and returns a concise result. Use this for separable work such as searching, reading files, or summarizing a focused subtask before the main answer.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"task\":{\"type\":\"string\",\"description\":\"The focused subtask for the subagent to complete\"},"
            "\"context\":{\"type\":\"string\",\"description\":\"Optional context, constraints, or relevant prior information for the subagent\"}},"
            "\"required\":[\"task\"]}",
        .execute = tool_subagent_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_environment",
        .description = "Read the 3-I2C environment sensor set in one call: AHT20/AHT10 temperature and humidity on hardware I2C0, SGP30 eCO2/TVOC on hardware I2C1, and GY-30/BH1750 light level on software I2C. Prefer this when the user asks for a combined environment test, comprehensive sensor test, AHT20+SGP30+GY30, or Chinese phrases like '综合测试', '环境数据', '读取全部传感器', '温湿度空气质量光照'.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"aht_sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional AHT20 hardware I2C SDA GPIO override\"},"
            "\"aht_scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional AHT20 hardware I2C SCL GPIO override\"},"
            "\"aht_i2c_port\":{\"type\":\"integer\",\"description\":\"Optional AHT20 hardware I2C port override\"},"
            "\"sgp30_sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SGP30 hardware I2C SDA GPIO override\"},"
            "\"sgp30_scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SGP30 hardware I2C SCL GPIO override\"},"
            "\"sgp30_i2c_port\":{\"type\":\"integer\",\"description\":\"Optional SGP30 hardware I2C port override\"},"
            "\"gy30_sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional GY-30 software I2C SDA GPIO override\"},"
            "\"gy30_scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional GY-30 software I2C SCL GPIO override\"},"
            "\"gy30_addr\":{\"type\":\"integer\",\"description\":\"Optional GY-30/BH1750 address, 0x23 by default or 0x5C when ADDR is high\"}},"
            "\"required\":[]}",
        .execute = tool_read_environment_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "env_history_summary",
        .description = "Summarize Sensor local environment history stored as compact JSONL under /spiffs/env. Prefer this for trend, long-running, or historical environment analysis instead of reading raw files.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"hours\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":336,\"description\":\"Lookback window in hours, capped by firmware retention\"}},"
            "\"required\":[]}",
        .execute = tool_env_history_summary_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "env_history_recent",
        .description = "Return compact recent Sensor environment samples from local /spiffs/env JSONL history. Use this when a few raw samples are needed for inspection or validation.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":48,\"description\":\"Maximum recent samples to return\"}},"
            "\"required\":[]}",
        .execute = tool_env_history_recent_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_presence",
        .description = "Read human presence from a 3-wire digital OUT human/PIR sensor, or from HC-SR05 ultrasonic proximity when Trig/Echo pins are configured. Prefer this when the user asks whether someone is nearby, whether a person is present, asks about human body sensing, proximity, obstacle distance, or Chinese phrases like '有人吗', '人体传感器', '有人靠近', '检测人体', '测一下距离', or 'HC-SR05'. Returns present=true/false, and distance_cm when ultrasonic mode is used.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"out_gpio\":{\"type\":\"integer\",\"description\":\"Optional 3-wire presence sensor OUT GPIO override\"},"
            "\"trig_gpio\":{\"type\":\"integer\",\"description\":\"Optional HC-SR05 Trig GPIO override\"},"
            "\"echo_gpio\":{\"type\":\"integer\",\"description\":\"Optional HC-SR05 Echo GPIO override; protect ESP32 input from 5V Echo\"},"
            "\"threshold_cm\":{\"type\":\"integer\",\"description\":\"Optional presence threshold in centimeters, defaults to configured threshold\"},"
            "\"samples\":{\"type\":\"integer\",\"description\":\"Optional sample count 1-7, defaults to 5\"}},"
            "\"required\":[]}",
        .execute = tool_read_presence_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "hc_sr05_read_distance",
        .description = "Low-level direct HC-SR05 ultrasonic distance read. Use this for explicit HC-SR05 diagnostics, Trig/Echo wiring checks, threshold tuning, or direct distance measurement requests.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"trig_gpio\":{\"type\":\"integer\",\"description\":\"Optional HC-SR05 Trig GPIO override\"},"
            "\"echo_gpio\":{\"type\":\"integer\",\"description\":\"Optional HC-SR05 Echo GPIO override; protect ESP32 input from 5V Echo\"},"
            "\"threshold_cm\":{\"type\":\"integer\",\"description\":\"Optional presence threshold in centimeters, defaults to configured threshold\"},"
            "\"samples\":{\"type\":\"integer\",\"description\":\"Optional sample count 1-7, defaults to 5\"}},"
            "\"required\":[]}",
        .execute = tool_hc_sr05_read_distance_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with " ESPAGENT_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " ESPAGENT_SPIFFS_BASE "/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with " ESPAGENT_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " ESPAGENT_SPIFFS_BASE "/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"},"
            "\"confirmed\":{\"type\":\"boolean\",\"description\":\"Required only for changing skill files under /spiffs/skills after explicit user confirmation\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces the first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " ESPAGENT_SPIFFS_BASE "/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"},"
            "\"confirmed\":{\"type\":\"boolean\",\"description\":\"Required only for changing skill files under /spiffs/skills after explicit user confirmation\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " ESPAGENT_SPIFFS_BASE "/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "gpio_write",
        .description = "Set an ESP32 GPIO output pin high or low. Use this for relays, digital outputs, or simple LEDs. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"ESP32 GPIO number to drive as output\"},"
            "\"state\":{\"type\":\"integer\",\"description\":\"0 for LOW, 1 for HIGH\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[\"pin\",\"state\"]}",
        .execute = tool_gpio_write_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "copper_gpio_write",
        .description = "Set one of the dedicated copper-fill GPIO pads on the third/control role high or low. Use this when the user explicitly asks GPIO4, GPIO5, or GPIO6 to be pulled high or pulled low. This tool only accepts pins 4, 5, and 6; each pin may have a different external meaning in the hardware wiring. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"enum\":[4,5,6],\"description\":\"Dedicated copper GPIO pad number\"},"
            "\"state\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"0 for LOW/pull down, 1 for HIGH/pull up\"},"
            "\"level\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"Alias for state\"},"
            "\"value\":{\"type\":\"string\",\"enum\":[\"high\",\"low\",\"on\",\"off\",\"拉高\",\"拉低\"],\"description\":\"Optional natural-language level alias\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[\"pin\"]}",
        .execute = tool_copper_gpio_write_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "set_humidifier",
        .description = "Turn the humidifier on or off. The humidifier is wired to GPIO4 on the third/control role and is active-high: logical ON drives HIGH, logical OFF drives LOW. Prefer this over generic GPIO tools when the user says humidifier, 加湿器, or humidification. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"state\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"0 for OFF/LOW, 1 for ON/HIGH\"},"
            "\"level\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"Alias for state\"},"
            "\"value\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"open\",\"close\",\"high\",\"low\",\"打开\",\"关闭\",\"拉高\",\"拉低\"],\"description\":\"Natural-language alias. on/off controls logical device state; high/low requests physical GPIO level and is converted for this active-high device\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_set_humidifier_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "set_fan",
        .description = "Legacy fan control. The current demo profile leaves the fan GPIO disabled because GPIO5 is reserved for the curtain servo. Do not use this for current showcase tests unless a non-servo fan GPIO is explicitly configured. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"state\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"0 for OFF/LOW, 1 for ON/HIGH\"},"
            "\"level\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"Alias for state\"},"
            "\"value\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"open\",\"close\",\"high\",\"low\",\"打开\",\"关闭\",\"拉高\",\"拉低\"],\"description\":\"Natural-language on/off alias\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_set_fan_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "set_device_led",
        .description = "Turn the onboard WS2812 status light on or off as a white/off alias. Prefer set_status_light when the user names a color. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"state\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"0 for OFF, 1 for ON/white\"},"
            "\"level\":{\"type\":\"integer\",\"enum\":[0,1],\"description\":\"Alias for state\"},"
            "\"value\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"open\",\"close\",\"high\",\"low\",\"打开\",\"关闭\",\"拉高\",\"拉低\"],\"description\":\"Natural-language on/off alias\"},"
            "\"pin\":{\"type\":\"integer\",\"description\":\"Optional WS2812 GPIO override; defaults to the configured onboard WS2812 pin\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_set_device_led_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "gpio_read",
        .description = "Read a GPIO pin state. Returns HIGH or LOW. Use for buttons, switches, and digital inputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"}},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_read_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "gpio_read_all",
        .description = "Read all allowed GPIO pin states in a single call.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_gpio_read_all_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "ws2812_set",
        .description = "Low-level WS2812/NeoPixel single-wire LED control. Use set_status_light for named colors; use this when explicit RGB values are needed. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"r\":{\"type\":\"integer\",\"description\":\"Red value 0-255\"},"
            "\"g\":{\"type\":\"integer\",\"description\":\"Green value 0-255\"},"
            "\"b\":{\"type\":\"integer\",\"description\":\"Blue value 0-255\"},"
            "\"brightness\":{\"type\":\"integer\",\"description\":\"Optional brightness 0-255, defaults to 255\"},"
            "\"pin\":{\"type\":\"integer\",\"description\":\"Optional GPIO override. Defaults to the configured onboard WS2812 pin.\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[\"r\",\"g\",\"b\"]}",
        .execute = tool_ws2812_set_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "set_status_light",
        .description = "Set the third/control role onboard WS2812 status light with a natural-language-friendly color. Prefer this when the user asks to turn the status light or WS2812 red, green, blue, white, yellow, purple, cyan, orange, or off. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"color\":{\"type\":\"string\",\"description\":\"Named color such as red, green, blue, white, yellow, orange, purple, cyan, or off\"},"
            "\"brightness\":{\"type\":\"integer\",\"description\":\"Optional brightness 0-255, defaults to 255\"},"
            "\"pin\":{\"type\":\"integer\",\"description\":\"Optional WS2812 GPIO override; defaults to the configured onboard WS2812 pin\"},"
            "\"r\":{\"type\":\"integer\",\"description\":\"Optional red value 0-255 when using explicit RGB\"},"
            "\"g\":{\"type\":\"integer\",\"description\":\"Optional green value 0-255 when using explicit RGB\"},"
            "\"b\":{\"type\":\"integer\",\"description\":\"Optional blue value 0-255 when using explicit RGB\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_set_status_light_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "servo_write",
        .description = "Low-level PWM servo control on the configured servo GPIO. Use set_curtain for normal curtain open/close requests; use servo_write only when the user asks for an explicit angle or pulse width. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"angle\":{\"type\":\"integer\",\"description\":\"Target angle 0-180 degrees\"},"
            "\"pulse_us\":{\"type\":\"integer\",\"description\":\"Pulse width in microseconds (typically 500-2500 for standard servos)\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_servo_write_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "set_curtain",
        .description = "Open or close the curtain actuator driven by the GPIO5 PWM servo on the third/control role. Prefer this when the user says curtain, 窗帘, curtain opener, open curtain, close curtain, 打开窗帘, or 关闭窗帘. Defaults are closed=0 degrees and open=90 degrees; optional angle can be used for calibration. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"state\":{\"type\":\"string\",\"enum\":[\"open\",\"closed\",\"on\",\"off\",\"打开\",\"关闭\"],\"description\":\"Curtain state: open/打开 or closed/关闭\"},"
            "\"position\":{\"type\":\"string\",\"enum\":[\"open\",\"closed\",\"打开\",\"关闭\"],\"description\":\"Alias for state\"},"
            "\"angle\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":180,\"description\":\"Optional calibration angle override\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_set_curtain_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "gree_ac_control",
        .description = "Control a Gree air-conditioner by IR transmit only. Supports common actions such as power on/off, cool/heat/dry/fan/auto mode, set or adjust temperature, common fan speed, and swing on/off. The tool keeps a bounded local cached AC state and resends a full Gree frame each time. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"power\":{\"type\":\"string\",\"enum\":[\"on\",\"off\"],\"description\":\"Turn the Gree AC on or off\"},"
            "\"mode\":{\"type\":\"string\",\"enum\":[\"auto\",\"cool\",\"dry\",\"fan\",\"heat\"],\"description\":\"Optional operating mode\"},"
            "\"temp_c\":{\"type\":\"integer\",\"minimum\":16,\"maximum\":30,\"description\":\"Optional target temperature in Celsius\"},"
            "\"temp_delta\":{\"type\":\"integer\",\"minimum\":-5,\"maximum\":5,\"description\":\"Optional relative temperature adjustment such as +1 or -1\"},"
            "\"fan\":{\"type\":\"string\",\"enum\":[\"auto\",\"low\",\"medium\",\"high\"],\"description\":\"Optional fan speed\"},"
            "\"swing\":{\"type\":\"string\",\"enum\":[\"on\",\"off\"],\"description\":\"Vertical swing auto on/off\"},"
            "\"tx_gpio\":{\"type\":\"integer\",\"description\":\"Optional IR TX GPIO override; defaults to configured Gree IR pin\"},"
            "\"repeat_count\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":4,\"description\":\"Optional extra retransmit count for reliability\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_gree_ac_control_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "max98357_play_tone",
        .description = "Play a short test tone through a MAX98357 I2S audio amplifier / speaker. Use this when the user asks to test audio output, a speaker, an audio amplifier, a beep, or MAX98357 wiring. The fixed wiring is BCLK=GPIO1, WS/LRCLK=GPIO2, DIN=GPIO3; SD is optional.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"frequency_hz\":{\"type\":\"integer\",\"description\":\"Tone frequency in Hz, defaults to 440\"},"
            "\"duration_ms\":{\"type\":\"integer\",\"description\":\"Tone duration in milliseconds, defaults to 400\"},"
            "\"volume_pct\":{\"type\":\"integer\",\"description\":\"Output volume percentage 0-100, defaults to 25\"},"
            "\"bclk_gpio\":{\"type\":\"integer\",\"description\":\"Optional I2S BCLK GPIO override\"},"
            "\"ws_gpio\":{\"type\":\"integer\",\"description\":\"Optional I2S WS/LRCLK GPIO override\"},"
            "\"din_gpio\":{\"type\":\"integer\",\"description\":\"Optional I2S DATA/DIN GPIO override\"},"
            "\"sd_gpio\":{\"type\":\"integer\",\"description\":\"Optional amplifier shutdown GPIO override, if wired\"},"
            "\"i2s_port\":{\"type\":\"integer\",\"description\":\"Optional I2S port override, defaults to configured port\"},"
            "\"sample_rate_hz\":{\"type\":\"integer\",\"description\":\"Optional sample rate override in Hz\"}},"
            "\"required\":[]}",
        .execute = tool_max98357_play_tone_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "sgp30_read_air_quality",
        .description = "Read eCO2 and TVOC from an SGP30 air-quality sensor over I2C. Use this for direct SGP30 reads, VOC/TVOC checks, or explicit sensor diagnostics. Chinese requests like '读取SGP30', '查看TVOC', or '检测空气数据' map here. Optional SDA/SCL GPIO overrides can be provided if board defaults are not configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SDA GPIO override\"},"
            "\"scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SCL GPIO override\"},"
            "\"i2c_port\":{\"type\":\"integer\",\"description\":\"Optional I2C port override; -1 means auto-select\"},"
            "\"scl_hz\":{\"type\":\"integer\",\"description\":\"Optional I2C clock speed in Hz, defaults to 100000\"}},"
            "\"required\":[]}",
        .execute = tool_sgp30_read_air_quality_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_air_quality",
        .description = "Read air-quality telemetry from the onboard or attached sensor. Prefer this high-level tool when the user asks about air quality, TVOC, eCO2, VOC, indoor air conditions, or Chinese phrases such as '空气质量', '空气怎么样', '检测气体', 'VOC多少', or '读取传感器数据'. Currently backed by SGP30.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SDA GPIO override\"},"
            "\"scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SCL GPIO override\"},"
            "\"i2c_port\":{\"type\":\"integer\",\"description\":\"Optional I2C port override; -1 means auto-select\"},"
            "\"scl_hz\":{\"type\":\"integer\",\"description\":\"Optional I2C clock speed in Hz, defaults to 100000\"}},"
            "\"required\":[]}",
        .execute = tool_sgp30_read_air_quality_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_light_level",
        .description = "Read ambient light level in lux from a GY-30/BH1750 I2C light sensor. Prefer this when the user asks about light level, ambient light, illuminance, lux, GY-30, BH1750, 光照, 光线亮度, 照度, or 勒克斯. Defaults to the sensor role's software I2C path because hardware I2C0/I2C1 are already used by AHT20 and SGP30; set hardware_i2c=true only for explicit bus diagnostics.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SDA GPIO override\"},"
            "\"scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SCL GPIO override\"},"
            "\"i2c_port\":{\"type\":\"integer\",\"description\":\"Optional I2C port override\"},"
            "\"scl_hz\":{\"type\":\"integer\",\"description\":\"Optional I2C clock speed in Hz, defaults to 100000\"},"
            "\"address\":{\"type\":\"integer\",\"description\":\"Optional BH1750 I2C address, 0x23 by default or 0x5C when ADDR is high\"},"
            "\"software_i2c\":{\"type\":\"boolean\",\"description\":\"Use software I2C; defaults true for the current sensor board wiring\"},"
            "\"hardware_i2c\":{\"type\":\"boolean\",\"description\":\"Force ESP-IDF hardware I2C path for diagnostics; may fail if the port is already acquired\"}},"
            "\"required\":[]}",
        .execute = tool_bh1750_read_light_execute,
    });

    build_tools_json();
    tool_subagent_init();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

const char *tool_registry_get_tools_json_compact_coordinator(void)
{
    return s_tools_json_compact_coordinator;
}

const char *tool_registry_get_tools_json_mesh_only(void)
{
    return s_tools_json_mesh_only;
}

void tool_registry_get_tools(const espagent_tool_t **tools, int *count)
{
    if (tools) {
        *tools = s_tools;
    }
    if (count) {
        *count = s_tool_count;
    }
}

esp_err_t tool_registry_execute_as(const char *name,
                                   const char *input_json,
                                   espagent_capability_caller_t caller,
                                   char *output,
                                   size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            char sandbox_reason[192] = {0};
            esp_err_t sandbox_err = tool_sandbox_check(name,
                                                       input_json,
                                                       sandbox_reason,
                                                       sizeof(sandbox_reason));
            if (sandbox_err != ESP_OK) {
                ESP_LOGW(TAG, "Sandbox blocked tool %s: %s", name, sandbox_reason);
                snprintf(output, output_size, "Error: %s",
                         sandbox_reason[0] ? sandbox_reason : "sandbox denied tool call");
                publish_sandbox_denial_observability(name, sandbox_err,
                                                     sandbox_reason);
                espagent_event_emit_simple("capability.error",
                                           "tool_registry",
                                           "",
                                           "",
                                           name,
                                           output ? output : "");
                return sandbox_err;
            }
            ESP_LOGI(TAG, "Executing tool: %s", name);
            espagent_event_emit_simple("capability.call",
                                       "tool_registry",
                                       "",
                                       "",
                                       name,
                                       input_json ? input_json : "{}");
            esp_err_t err = espagent_capability_execute(name,
                                                        input_json,
                                                        caller,
                                                        output,
                                                        output_size);
            espagent_event_emit_simple(err == ESP_OK ? "capability.result" : "capability.error",
                                       "tool_registry",
                                       "",
                                       "",
                                       name,
                                       output ? output : "");
            return err;
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    return tool_registry_execute_as(name,
                                    input_json,
                                    ESPAGENT_CAP_CALLER_AGENT,
                                    output,
                                    output_size);
}

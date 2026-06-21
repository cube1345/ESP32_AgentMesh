#include "tools/tool_sandbox.h"

#include "espagent_config.h"
#include "roles/role_config.h"

#include "cJSON.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

typedef enum {
    TOOL_CAP_READ = 1 << 0,
    TOOL_CAP_NETWORK = 1 << 1,
    TOOL_CAP_SENSOR = 1 << 2,
    TOOL_CAP_CONTROL = 1 << 3,
    TOOL_CAP_MESH = 1 << 4,
    TOOL_CAP_MEMORY_WRITE = 1 << 5,
    TOOL_CAP_FILE_WRITE = 1 << 6,
    TOOL_CAP_AUTOMATION = 1 << 7,
    TOOL_CAP_SCHEDULE = 1 << 8,
    TOOL_CAP_PRIVACY = 1 << 9,
    TOOL_CAP_SYSTEM = 1 << 10,
} tool_capability_t;

typedef struct {
    const char *name;
    espagent_tool_risk_t risk;
    unsigned caps;
} tool_sandbox_rule_t;

#ifndef ESPAGENT_SANDBOX_ENABLED
#define ESPAGENT_SANDBOX_ENABLED 1
#endif

#define ESPAGENT_SANDBOX_MAX_TTL_MS              30000
#define ESPAGENT_SANDBOX_MIN_AUTOMATION_INTERVAL_S 5
#define ESPAGENT_SANDBOX_MIN_AUTOMATION_COOLDOWN_S 5
#define ESPAGENT_SANDBOX_MAX_AUTOMATION_INTERVAL_S 86400
#define ESPAGENT_SANDBOX_MAX_AUTOMATION_COOLDOWN_S 86400
#define ESPAGENT_SANDBOX_MAX_CRON_INTERVAL_S     60
#define ESPAGENT_SANDBOX_MAX_TONE_DURATION_MS    5000
#define ESPAGENT_SANDBOX_MAX_TONE_VOLUME_PCT     80

static const tool_sandbox_rule_t s_rules[] = {
    {"web_search", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_NETWORK},
    {"get_weather", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_NETWORK},
    {"get_current_time", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"lua_runtime_info", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"lua_list_modules", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"lua_list_scripts", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"lua_run_source", ESPAGENT_TOOL_RISK_SYSTEM, TOOL_CAP_SYSTEM},
    {"lua_run_script", ESPAGENT_TOOL_RISK_SYSTEM, TOOL_CAP_SYSTEM},
    {"lua_run_script_async", ESPAGENT_TOOL_RISK_SYSTEM, TOOL_CAP_SYSTEM},
    {"lua_list_jobs", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"lua_get_job", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"lua_stop_job", ESPAGENT_TOOL_RISK_SYSTEM, TOOL_CAP_SYSTEM},
    {"memory_profile_set", ESPAGENT_TOOL_RISK_PRIVACY, TOOL_CAP_MEMORY_WRITE},
    {"skill_observation_add", ESPAGENT_TOOL_RISK_PRIVACY, TOOL_CAP_MEMORY_WRITE},
    {"read_temperature_humidity", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_SENSOR | TOOL_CAP_MESH},
    {"virtual_device_read", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_SENSOR | TOOL_CAP_MESH},
    {"read_environment", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_SENSOR},
    {"read_air_quality", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_SENSOR},
    {"sgp30_read_air_quality", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_SENSOR},
    {"read_light_level", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_SENSOR},
    {"read_presence", ESPAGENT_TOOL_RISK_PRIVACY, TOOL_CAP_SENSOR | TOOL_CAP_PRIVACY},
    {"hc_sr05_read_distance", ESPAGENT_TOOL_RISK_PRIVACY, TOOL_CAP_SENSOR | TOOL_CAP_PRIVACY},
    {"gpio_read", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"gpio_read_all", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"set_status_light", ESPAGENT_TOOL_RISK_LOW_CONTROL, TOOL_CAP_CONTROL | TOOL_CAP_MESH},
    {"ws2812_set", ESPAGENT_TOOL_RISK_LOW_CONTROL, TOOL_CAP_CONTROL | TOOL_CAP_MESH},
    {"virtual_device_control", ESPAGENT_TOOL_RISK_MEDIUM_CONTROL, TOOL_CAP_CONTROL | TOOL_CAP_MESH},
    {"gpio_write", ESPAGENT_TOOL_RISK_MEDIUM_CONTROL, TOOL_CAP_CONTROL | TOOL_CAP_MESH},
    {"servo_write", ESPAGENT_TOOL_RISK_MEDIUM_CONTROL, TOOL_CAP_CONTROL | TOOL_CAP_MESH},
    {"max98357_play_tone", ESPAGENT_TOOL_RISK_LOW_CONTROL, TOOL_CAP_CONTROL},
    {"mesh_send_command", ESPAGENT_TOOL_RISK_MEDIUM_CONTROL, TOOL_CAP_MESH},
    {"automation_create_workflow", ESPAGENT_TOOL_RISK_MEDIUM_CONTROL, TOOL_CAP_AUTOMATION | TOOL_CAP_MESH},
    {"automation_create_rule", ESPAGENT_TOOL_RISK_HIGH_CONTROL, TOOL_CAP_AUTOMATION | TOOL_CAP_MESH},
    {"automation_list", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"automation_remove", ESPAGENT_TOOL_RISK_MEDIUM_CONTROL, TOOL_CAP_AUTOMATION},
    {"cron_add", ESPAGENT_TOOL_RISK_MEDIUM_CONTROL, TOOL_CAP_SCHEDULE},
    {"cron_list", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"cron_remove", ESPAGENT_TOOL_RISK_MEDIUM_CONTROL, TOOL_CAP_SCHEDULE},
    {"spawn_subagent", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_NETWORK | TOOL_CAP_READ},
    {"read_file", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"list_dir", ESPAGENT_TOOL_RISK_READ_ONLY, TOOL_CAP_READ},
    {"write_file", ESPAGENT_TOOL_RISK_SYSTEM, TOOL_CAP_FILE_WRITE | TOOL_CAP_MEMORY_WRITE | TOOL_CAP_SYSTEM},
    {"edit_file", ESPAGENT_TOOL_RISK_SYSTEM, TOOL_CAP_FILE_WRITE | TOOL_CAP_MEMORY_WRITE | TOOL_CAP_SYSTEM},
};

static const tool_sandbox_rule_t *find_rule(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_rules) / sizeof(s_rules[0]); i++) {
        if (strcmp(s_rules[i].name, name) == 0) {
            return &s_rules[i];
        }
    }
    return NULL;
}

const char *tool_sandbox_risk_name(espagent_tool_risk_t risk)
{
    switch (risk) {
    case ESPAGENT_TOOL_RISK_READ_ONLY: return "read_only";
    case ESPAGENT_TOOL_RISK_LOW_CONTROL: return "low_control";
    case ESPAGENT_TOOL_RISK_MEDIUM_CONTROL: return "medium_control";
    case ESPAGENT_TOOL_RISK_HIGH_CONTROL: return "high_control";
    case ESPAGENT_TOOL_RISK_PRIVACY: return "privacy";
    case ESPAGENT_TOOL_RISK_SYSTEM: return "system";
    default: return "unknown";
    }
}

static bool json_bool_value(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    if (!item) {
        return fallback;
    }
    return cJSON_IsTrue(item);
}

static int json_int_value(cJSON *root, const char *key, int fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static const char *json_string_value(cJSON *root, const char *key)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void deny(char *reason, size_t reason_size, const char *fmt, ...)
{
    if (!reason || reason_size == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reason, reason_size, fmt, ap);
    va_end(ap);
}

static bool path_is_sensitive(const char *path)
{
    if (!path) {
        return false;
    }
    return strstr(path, "/secrets") != NULL ||
           strstr(path, "espagent_secrets") != NULL ||
           strstr(path, "/config/") != NULL;
}

static bool path_is_skill(const char *path)
{
    return path && strstr(path, "/skills/") != NULL;
}

static bool tool_is_lua_system_action(const char *name)
{
    return name &&
           (strcmp(name, "lua_run_source") == 0 ||
            strcmp(name, "lua_run_script") == 0 ||
            strcmp(name, "lua_run_script_async") == 0 ||
            strcmp(name, "lua_stop_job") == 0);
}

static bool role_allows_caps(const char *name, unsigned caps)
{
    if (espagent_role_is_edge()) {
        return true;
    }

    if ((caps & TOOL_CAP_FILE_WRITE) && !espagent_role_is_coordinator()) {
        return false;
    }
    if ((caps & TOOL_CAP_SYSTEM) &&
        !espagent_role_is_coordinator() &&
        !tool_is_lua_system_action(name)) {
        return false;
    }
    if ((caps & TOOL_CAP_MEMORY_WRITE) &&
        !(espagent_role_is_coordinator() || espagent_role_runs_guardian())) {
        return false;
    }
    if ((caps & TOOL_CAP_AUTOMATION) && !espagent_role_is_coordinator()) {
        return false;
    }
    if ((caps & TOOL_CAP_SCHEDULE) && !espagent_role_runs_scheduler()) {
        return false;
    }
    if ((caps & TOOL_CAP_MESH) && !(espagent_role_is_coordinator() || espagent_role_runs_sensor_sampling() || espagent_role_runs_control_outputs())) {
        return false;
    }
    if ((caps & TOOL_CAP_CONTROL) && !espagent_role_runs_control_outputs() && !espagent_role_is_coordinator()) {
        return false;
    }
    if ((caps & TOOL_CAP_SENSOR) && !espagent_role_runs_sensor_sampling() && !espagent_role_is_coordinator()) {
        return false;
    }
    if ((caps & TOOL_CAP_PRIVACY) && !(espagent_role_runs_sensor_sampling() || espagent_role_runs_guardian())) {
        return false;
    }
    return true;
}

static bool sandbox_check_path_tool(const char *name, cJSON *root, char *reason, size_t reason_size)
{
    const char *path = json_string_value(root, "path");
    if ((strcmp(name, "write_file") == 0 || strcmp(name, "edit_file") == 0) && path_is_sensitive(path)) {
        deny(reason, reason_size, "sandbox denied %s: writing config or secrets paths is not allowed", name);
        return false;
    }
    if ((strcmp(name, "write_file") == 0 || strcmp(name, "edit_file") == 0) &&
        path_is_skill(path) && !json_bool_value(root, "confirmed", false)) {
        deny(reason, reason_size, "sandbox denied %s: skill changes require explicit confirmed=true", name);
        return false;
    }
    return true;
}

static bool sandbox_check_cron(cJSON *root, char *reason, size_t reason_size)
{
    const char *schedule_type = json_string_value(root, "schedule_type");
    if (schedule_type && strcmp(schedule_type, "every") == 0) {
        int interval_s = json_int_value(root, "interval_s", 0);
        if (interval_s > 0 && interval_s < ESPAGENT_SANDBOX_MAX_CRON_INTERVAL_S) {
            deny(reason, reason_size, "sandbox denied cron_add: interval_s must be >= %d", ESPAGENT_SANDBOX_MAX_CRON_INTERVAL_S);
            return false;
        }
    }
    return true;
}

static bool sandbox_check_mesh(cJSON *root, char *reason, size_t reason_size)
{
    int ttl_ms = json_int_value(root, "ttl_ms", 30000);
    if (ttl_ms > ESPAGENT_SANDBOX_MAX_TTL_MS) {
        deny(reason, reason_size, "sandbox denied mesh_send_command: ttl_ms must be <= %d", ESPAGENT_SANDBOX_MAX_TTL_MS);
        return false;
    }
    int safety_level = json_int_value(root, "safety_level", 1);
    if (safety_level > 1) {
        deny(reason, reason_size, "sandbox denied mesh_send_command: high-risk safety_level requires an explicit confirmation flow");
        return false;
    }
    const char *action = json_string_value(root, "action");
    if (action &&
        strcmp(action, "gpio_write") != 0 &&
        strcmp(action, "servo_write") != 0 &&
        strcmp(action, "ws2812_set") != 0 &&
        strcmp(action, "set_status_light") != 0 &&
        strcmp(action, "virtual_device_control") != 0 &&
        strcmp(action, "control_state") != 0 &&
        strcmp(action, "control_emergency_stop") != 0 &&
        strcmp(action, "read_temperature_humidity") != 0 &&
        strcmp(action, "virtual_device_read") != 0) {
        deny(reason, reason_size, "sandbox denied mesh_send_command: unsupported action=%s", action);
        return false;
    }
    return true;
}

static bool sandbox_check_automation_rule(cJSON *root, char *reason, size_t reason_size)
{
    int interval_s = json_int_value(root, "interval_s", 10);
    int cooldown_s = json_int_value(root, "cooldown_s", 30);
    if (interval_s < ESPAGENT_SANDBOX_MIN_AUTOMATION_INTERVAL_S ||
        interval_s > ESPAGENT_SANDBOX_MAX_AUTOMATION_INTERVAL_S) {
        deny(reason, reason_size, "sandbox denied automation_create_rule: interval_s must be %d..%d",
             ESPAGENT_SANDBOX_MIN_AUTOMATION_INTERVAL_S,
             ESPAGENT_SANDBOX_MAX_AUTOMATION_INTERVAL_S);
        return false;
    }
    if (cooldown_s < ESPAGENT_SANDBOX_MIN_AUTOMATION_COOLDOWN_S ||
        cooldown_s > ESPAGENT_SANDBOX_MAX_AUTOMATION_COOLDOWN_S) {
        deny(reason, reason_size, "sandbox denied automation_create_rule: cooldown_s must be %d..%d",
             ESPAGENT_SANDBOX_MIN_AUTOMATION_COOLDOWN_S,
             ESPAGENT_SANDBOX_MAX_AUTOMATION_COOLDOWN_S);
        return false;
    }
    return true;
}

static bool sandbox_check_audio(cJSON *root, char *reason, size_t reason_size)
{
    int duration_ms = json_int_value(root, "duration_ms", 400);
    int volume_pct = json_int_value(root, "volume_pct", 25);
    if (duration_ms > ESPAGENT_SANDBOX_MAX_TONE_DURATION_MS) {
        deny(reason, reason_size, "sandbox denied max98357_play_tone: duration_ms must be <= %d", ESPAGENT_SANDBOX_MAX_TONE_DURATION_MS);
        return false;
    }
    if (volume_pct > ESPAGENT_SANDBOX_MAX_TONE_VOLUME_PCT) {
        deny(reason, reason_size, "sandbox denied max98357_play_tone: volume_pct must be <= %d", ESPAGENT_SANDBOX_MAX_TONE_VOLUME_PCT);
        return false;
    }
    return true;
}

esp_err_t tool_sandbox_check(const char *name,
                             const char *input_json,
                             char *reason,
                             size_t reason_size)
{
    if (reason && reason_size > 0) {
        reason[0] = '\0';
    }
#if !ESPAGENT_SANDBOX_ENABLED
    (void)name;
    (void)input_json;
    return ESP_OK;
#endif

    const tool_sandbox_rule_t *rule = find_rule(name);
    if (!rule) {
        deny(reason, reason_size, "sandbox denied unknown tool=%s", name ? name : "(null)");
        return ESP_ERR_NOT_FOUND;
    }

    if (!role_allows_caps(name, rule->caps)) {
        deny(reason, reason_size, "sandbox denied %s: role is not allowed to use %s capability",
             name, tool_sandbox_risk_name(rule->risk));
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        deny(reason, reason_size, "sandbox denied %s: input must be a JSON object", name);
        return ESP_ERR_INVALID_ARG;
    }

    bool ok = true;
    if (strcmp(name, "write_file") == 0 || strcmp(name, "edit_file") == 0 || strcmp(name, "read_file") == 0) {
        ok = sandbox_check_path_tool(name, root, reason, reason_size);
    } else if (strcmp(name, "cron_add") == 0) {
        ok = sandbox_check_cron(root, reason, reason_size);
    } else if (strcmp(name, "mesh_send_command") == 0) {
        ok = sandbox_check_mesh(root, reason, reason_size);
    } else if (strcmp(name, "automation_create_rule") == 0) {
        ok = sandbox_check_automation_rule(root, reason, reason_size);
    } else if (strcmp(name, "max98357_play_tone") == 0) {
        ok = sandbox_check_audio(root, reason, reason_size);
    }

    if (ok && rule->risk >= ESPAGENT_TOOL_RISK_HIGH_CONTROL &&
        strcmp(name, "write_file") != 0 &&
        strcmp(name, "edit_file") != 0) {
        bool confirmed = json_bool_value(root, "confirmed", false);
        if (!confirmed) {
            deny(reason, reason_size, "sandbox denied %s: %s action requires explicit confirmed=true",
                 name, tool_sandbox_risk_name(rule->risk));
            ok = false;
        }
    }

    cJSON_Delete(root);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

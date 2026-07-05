#include "capability/role_capability_profile.h"

#include "roles/role_config.h"
#include "node/node_profile.h"

#include <stdio.h>
#include <string.h>

static bool is_name(const espagent_capability_descriptor_t *cap, const char *name)
{
    return cap && cap->name && strcmp(cap->name, name) == 0;
}

static bool is_family(const espagent_capability_descriptor_t *cap, const char *family)
{
    return cap && cap->family && strcmp(cap->family, family) == 0;
}

static bool is_control_tool(const espagent_capability_descriptor_t *cap)
{
    return is_family(cap, "control") ||
           is_name(cap, "set_status_light") ||
           is_name(cap, "ws2812_set") ||
           is_name(cap, "servo_write") ||
           is_name(cap, "gree_ac_control") ||
           is_name(cap, "gpio_write") ||
           is_name(cap, "virtual_device_control") ||
           is_name(cap, "max98357_play_tone");
}

static bool is_sensor_tool(const espagent_capability_descriptor_t *cap)
{
    return is_family(cap, "sensor") ||
           is_name(cap, "read_temperature_humidity") ||
           is_name(cap, "read_environment") ||
           is_name(cap, "read_air_quality") ||
           is_name(cap, "sgp30_read_air_quality") ||
           is_name(cap, "read_light_level") ||
           is_name(cap, "read_presence") ||
           is_name(cap, "hc_sr05_read_distance") ||
           is_name(cap, "virtual_device_read") ||
           is_name(cap, "gpio_read") ||
           is_name(cap, "gpio_read_all");
}

static bool is_coordinator_tool(const espagent_capability_descriptor_t *cap)
{
    return is_family(cap, "coordinator") ||
           is_family(cap, "network") ||
           is_family(cap, "gateway") ||
           is_family(cap, "time") ||
           is_family(cap, "automation") ||
           is_family(cap, "mesh") ||
           is_family(cap, "subagent") ||
           is_name(cap, "web_search") ||
           is_name(cap, "get_weather") ||
           is_name(cap, "get_current_time") ||
           is_name(cap, "mesh_send_command") ||
           is_name(cap, "spawn_subagent") ||
           is_name(cap, "cron_add") ||
           is_name(cap, "cron_list") ||
           is_name(cap, "cron_remove") ||
           is_name(cap, "read_file") ||
           is_name(cap, "list_dir") ||
           is_name(cap, "write_file") ||
           is_name(cap, "edit_file") ||
           is_family(cap, "memory") ||
           is_family(cap, "script");
}

bool espagent_capability_visible_to_current_role(const espagent_capability_descriptor_t *cap)
{
    if (!cap || !(cap->flags & ESPAGENT_CAP_FLAG_LLM_VISIBLE)) {
        return false;
    }

    if (espagent_role_is_edge()) {
        return true;
    }

    if (espagent_role_is_coordinator()) {
        return is_coordinator_tool(cap) || is_control_tool(cap) || is_sensor_tool(cap);
    }

    if (espagent_role_is_sensor()) {
        return is_sensor_tool(cap) ||
               is_family(cap, "script") ||
               is_name(cap, "get_current_time") ||
               is_name(cap, "read_file") ||
               is_name(cap, "list_dir");
    }

    if (espagent_role_is_control()) {
        return is_control_tool(cap) ||
               is_family(cap, "script") ||
               is_name(cap, "control_state") ||
               is_name(cap, "control_emergency_stop") ||
               is_name(cap, "get_current_time") ||
               is_name(cap, "read_file") ||
               is_name(cap, "list_dir");
    }

    if (espagent_role_is_guardian()) {
        return is_family(cap, "guardian") ||
               is_family(cap, "memory") ||
               is_family(cap, "script") ||
               is_name(cap, "get_current_time") ||
               is_name(cap, "read_file") ||
               is_name(cap, "list_dir");
    }

    return false;
}

bool espagent_capability_callable_by_current_role(const espagent_capability_descriptor_t *cap,
                                                 espagent_capability_caller_t caller)
{
    if (!cap) {
        return false;
    }

    if (caller == ESPAGENT_CAP_CALLER_MESH || caller == ESPAGENT_CAP_CALLER_EVENT ||
        caller == ESPAGENT_CAP_CALLER_CLI || caller == ESPAGENT_CAP_CALLER_SYSTEM) {
        return true;
    }

    if (caller == ESPAGENT_CAP_CALLER_SUBAGENT &&
        !(cap->flags & ESPAGENT_CAP_FLAG_SUBAGENT_ALLOWED)) {
        return false;
    }

    return espagent_capability_visible_to_current_role(cap);
}

void espagent_capability_profile_describe(char *buf, size_t size)
{
    if (!buf || size == 0) {
        return;
    }
    snprintf(buf, size,
             "node_id=%s role=%s capabilities=%s profile=%s",
             espagent_node_id(),
             espagent_node_role(),
             espagent_node_capabilities(),
             espagent_role_is_coordinator() ? "coordinator" :
             espagent_role_is_sensor() ? "sensor" :
             espagent_role_is_control() ? "control" :
             espagent_role_is_guardian() ? "guardian" :
             espagent_role_is_display() ? "display" : "edge");
}

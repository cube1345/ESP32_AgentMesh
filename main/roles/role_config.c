#include "roles/role_config.h"

#include "node/node_profile.h"

bool espagent_role_is_edge(void)
{
    return espagent_node_is_role(ESPAGENT_ROLE_EDGE);
}

bool espagent_role_is_coordinator(void)
{
    return espagent_node_is_role(ESPAGENT_ROLE_COORDINATOR) ||
           espagent_node_has_capability("coordinator");
}

bool espagent_role_is_sensor(void)
{
    return espagent_node_is_role(ESPAGENT_ROLE_SENSOR) ||
           espagent_node_has_capability("sensor");
}

bool espagent_role_is_control(void)
{
    return espagent_node_is_role(ESPAGENT_ROLE_CONTROL) ||
           espagent_node_has_capability("control");
}

bool espagent_role_is_display(void)
{
    return espagent_node_is_role(ESPAGENT_ROLE_DISPLAY) ||
           espagent_node_has_capability("display");
}

bool espagent_role_runs_llm(void)
{
    return espagent_role_is_edge() ||
           espagent_role_is_coordinator() ||
           espagent_node_has_capability("llm");
}

bool espagent_role_runs_chat_channels(void)
{
    return espagent_role_is_edge() ||
           espagent_role_is_coordinator() ||
           espagent_node_has_capability("communication");
}

bool espagent_role_runs_scheduler(void)
{
    return espagent_role_is_edge() ||
           espagent_role_is_coordinator() ||
           espagent_node_has_capability("cron") ||
           espagent_node_has_capability("proactive");
}

bool espagent_role_runs_sensor_sampling(void)
{
    return espagent_role_is_edge() ||
           espagent_role_is_sensor() ||
           espagent_node_has_capability("telemetry");
}

bool espagent_role_runs_control_outputs(void)
{
    return espagent_role_is_edge() ||
           espagent_role_is_control() ||
           espagent_node_has_capability("actuator");
}

bool espagent_role_runs_display_outputs(void)
{
    return espagent_role_is_edge() ||
           espagent_role_is_display() ||
           espagent_node_has_capability("state") ||
           espagent_node_has_capability("watchdog");
}

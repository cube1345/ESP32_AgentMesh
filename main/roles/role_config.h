#pragma once

#include <stdbool.h>

#define ESPAGENT_ROLE_EDGE          "edge_agent"
#define ESPAGENT_ROLE_COORDINATOR   "coordinator_agent"
#define ESPAGENT_ROLE_SENSOR        "sensor_agent"
#define ESPAGENT_ROLE_CONTROL       "control_agent"
#define ESPAGENT_ROLE_DISPLAY       "display_agent"

bool espagent_role_is_edge(void);
bool espagent_role_is_coordinator(void);
bool espagent_role_is_sensor(void);
bool espagent_role_is_control(void);
bool espagent_role_is_display(void);

bool espagent_role_runs_llm(void);
bool espagent_role_runs_chat_channels(void);
bool espagent_role_runs_scheduler(void);
bool espagent_role_runs_sensor_sampling(void);
bool espagent_role_runs_control_outputs(void);
bool espagent_role_runs_display_outputs(void);

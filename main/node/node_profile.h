#pragma once

#include <stdbool.h>

const char *espagent_node_id(void);
const char *espagent_node_role(void);
const char *espagent_node_location(void);
const char *espagent_node_capabilities(void);
const char *espagent_node_responsibilities(void);

bool espagent_node_is_role(const char *role);
bool espagent_node_has_capability(const char *capability);
bool espagent_node_should_publish_sensor_telemetry(void);

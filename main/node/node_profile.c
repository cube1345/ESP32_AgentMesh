#include "node/node_profile.h"

#include "espagent_config.h"

#include <string.h>

static bool token_equals(const char *start, size_t len, const char *expected)
{
    return strlen(expected) == len && strncmp(start, expected, len) == 0;
}

static bool token_list_contains(const char *list, const char *token)
{
    if (!list || !token || token[0] == '\0') {
        return false;
    }

    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '|') {
            p++;
        }

        const char *start = p;
        while (*p && *p != ',' && *p != ';' && *p != '|') {
            p++;
        }

        const char *end = p;
        while (end > start && end[-1] == ' ') {
            end--;
        }

        if (end > start && token_equals(start, (size_t)(end - start), token)) {
            return true;
        }
    }

    return false;
}

const char *espagent_node_id(void)
{
    return ESPAGENT_NODE_ID;
}

const char *espagent_node_role(void)
{
    return ESPAGENT_NODE_ROLE;
}

const char *espagent_node_location(void)
{
    return ESPAGENT_NODE_LOCATION;
}

const char *espagent_node_capabilities(void)
{
    return ESPAGENT_NODE_CAPABILITIES;
}

const char *espagent_node_responsibilities(void)
{
    return ESPAGENT_NODE_RESPONSIBILITIES;
}

bool espagent_node_is_role(const char *role)
{
    return role && strcmp(ESPAGENT_NODE_ROLE, role) == 0;
}

bool espagent_node_has_capability(const char *capability)
{
    return token_list_contains(ESPAGENT_NODE_CAPABILITIES, capability);
}

bool espagent_node_should_publish_sensor_telemetry(void)
{
    return espagent_node_has_capability("sensor") ||
           espagent_node_has_capability("telemetry") ||
           espagent_node_is_role("edge_agent") ||
           espagent_node_is_role("sensor_agent");
}

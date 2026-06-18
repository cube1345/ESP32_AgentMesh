#pragma once

#include <stdbool.h>

#define ESPAGENT_MESH_ID_MAX          40
#define ESPAGENT_MESH_NODE_MAX        32
#define ESPAGENT_MESH_ROLE_MAX        32
#define ESPAGENT_MESH_ACTION_MAX      32
#define ESPAGENT_MESH_TRACE_MAX       48
#define ESPAGENT_MESH_ARGS_JSON_MAX   256

typedef enum {
    ESPAGENT_MESH_SAFETY_LOW = 0,
    ESPAGENT_MESH_SAFETY_MEDIUM = 1,
    ESPAGENT_MESH_SAFETY_HIGH = 2,
} espagent_mesh_safety_level_t;

typedef struct {
    char command_id[ESPAGENT_MESH_ID_MAX];
    char trace_id[ESPAGENT_MESH_TRACE_MAX];
    char target_node[ESPAGENT_MESH_NODE_MAX];
    char target_role[ESPAGENT_MESH_ROLE_MAX];
    char action[ESPAGENT_MESH_ACTION_MAX];
    char args_json[ESPAGENT_MESH_ARGS_JSON_MAX];
    int ttl_ms;
    int safety_level;
    bool require_ack;
} espagent_mesh_command_t;

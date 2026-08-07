#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESPAGENT_MESH_ID_MAX          40
#define ESPAGENT_MESH_NODE_MAX        32
#define ESPAGENT_MESH_ROLE_MAX        32
#define ESPAGENT_MESH_ACTION_MAX      32
#define ESPAGENT_MESH_TRACE_MAX       48
#define ESPAGENT_MESH_ARGS_JSON_MAX   256
#define ESPAGENT_MESH_SIGNATURE_MAX   65
#define ESPAGENT_MESH_NONCE_MAX       40
#define ESPAGENT_MESH_CHANNEL_MAX     16
#define ESPAGENT_MESH_CHAT_MAX        96
#define ESPAGENT_MESH_TASK_ID_MAX     48

typedef enum {
    ESPAGENT_MESH_SAFETY_LOW = 0,
    ESPAGENT_MESH_SAFETY_MEDIUM = 1,
    ESPAGENT_MESH_SAFETY_HIGH = 2,
} espagent_mesh_safety_level_t;

typedef enum {
    ESPAGENT_TASK_CREATED = 0,
    ESPAGENT_TASK_POLICY_PENDING,
    ESPAGENT_TASK_QUEUED,
    ESPAGENT_TASK_RUNNING,
    ESPAGENT_TASK_WAITING_RESULT,
    ESPAGENT_TASK_SUCCEEDED,
    ESPAGENT_TASK_FAILED,
    ESPAGENT_TASK_EXPIRED,
    ESPAGENT_TASK_CANCELLED,
} espagent_task_status_t;

typedef struct {
    char task_id[ESPAGENT_MESH_TASK_ID_MAX];
    char parent_task_id[ESPAGENT_MESH_ID_MAX];
    char command_id[ESPAGENT_MESH_ID_MAX];
    char trace_id[ESPAGENT_MESH_TRACE_MAX];
    char source_channel[ESPAGENT_MESH_CHANNEL_MAX];
    char source_chat_id[ESPAGENT_MESH_CHAT_MAX];
    char target_role[ESPAGENT_MESH_ROLE_MAX];
    int64_t deadline_ms;
    uint16_t retry_count;
    espagent_task_status_t status;
} espagent_task_meta_t;

typedef struct {
    char command_id[ESPAGENT_MESH_ID_MAX];
    char trace_id[ESPAGENT_MESH_TRACE_MAX];
    char target_node[ESPAGENT_MESH_NODE_MAX];
    char target_role[ESPAGENT_MESH_ROLE_MAX];
    char action[ESPAGENT_MESH_ACTION_MAX];
    char args_json[ESPAGENT_MESH_ARGS_JSON_MAX];
    char signature[ESPAGENT_MESH_SIGNATURE_MAX];
    char nonce[ESPAGENT_MESH_NONCE_MAX];
    int64_t ts_ms;
    int ttl_ms;
    int safety_level;
    bool require_ack;
    espagent_task_meta_t task;
} espagent_mesh_command_t;

const char *espagent_task_status_name(espagent_task_status_t status);

#pragma once

#include "esp_err.h"
#include "mesh/mesh_types.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ESPAGENT_AGENT_STAGE_ROUTE = 0,
    ESPAGENT_AGENT_STAGE_BUILD_CONTEXT,
    ESPAGENT_AGENT_STAGE_MODEL_CALL,
    ESPAGENT_AGENT_STAGE_TOOL_VALIDATE,
    ESPAGENT_AGENT_STAGE_POLICY_CHECK,
    ESPAGENT_AGENT_STAGE_TOOL_EXECUTE,
    ESPAGENT_AGENT_STAGE_RESULT_VALIDATE,
    ESPAGENT_AGENT_STAGE_FINALIZE,
} espagent_agent_stage_t;

typedef struct {
    char trace_id[ESPAGENT_MESH_TRACE_MAX];
    espagent_agent_stage_t stage;
    uint32_t stage_timeout_ms;
    size_t memory_budget;
    espagent_task_meta_t task;
    esp_err_t last_error;
    int64_t stage_started_ms;
} espagent_agent_state_t;

void espagent_agent_state_init(espagent_agent_state_t *state,
                               const char *trace_id,
                               const char *source_channel,
                               const char *source_chat_id,
                               size_t memory_budget);
esp_err_t espagent_agent_state_transition(espagent_agent_state_t *state,
                                           espagent_agent_stage_t next,
                                           uint32_t timeout_ms);
const char *espagent_agent_stage_name(espagent_agent_stage_t stage);
bool espagent_agent_state_timed_out(const espagent_agent_state_t *state,
                                    int64_t now_ms);

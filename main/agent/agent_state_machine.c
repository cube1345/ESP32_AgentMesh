#include "agent/agent_state_machine.h"

#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

const char *espagent_agent_stage_name(espagent_agent_stage_t stage)
{
    switch (stage) {
    case ESPAGENT_AGENT_STAGE_ROUTE: return "route";
    case ESPAGENT_AGENT_STAGE_BUILD_CONTEXT: return "build_context";
    case ESPAGENT_AGENT_STAGE_MODEL_CALL: return "model_call";
    case ESPAGENT_AGENT_STAGE_TOOL_VALIDATE: return "tool_validate";
    case ESPAGENT_AGENT_STAGE_POLICY_CHECK: return "policy_check";
    case ESPAGENT_AGENT_STAGE_TOOL_EXECUTE: return "tool_execute";
    case ESPAGENT_AGENT_STAGE_RESULT_VALIDATE: return "result_validate";
    case ESPAGENT_AGENT_STAGE_FINALIZE: return "finalize";
    default: return "unknown";
    }
}

void espagent_agent_state_init(espagent_agent_state_t *state,
                               const char *trace_id,
                               const char *source_channel,
                               const char *source_chat_id,
                               size_t memory_budget)
{
    if (!state) return;
    memset(state, 0, sizeof(*state));
    snprintf(state->trace_id, sizeof(state->trace_id), "%s", trace_id ? trace_id : "");
    snprintf(state->task.trace_id, sizeof(state->task.trace_id), "%s", state->trace_id);
    snprintf(state->task.source_channel, sizeof(state->task.source_channel), "%s",
             source_channel ? source_channel : "");
    snprintf(state->task.source_chat_id, sizeof(state->task.source_chat_id), "%s",
             source_chat_id ? source_chat_id : "");
    state->task.status = ESPAGENT_TASK_CREATED;
    state->stage = ESPAGENT_AGENT_STAGE_ROUTE;
    state->stage_timeout_ms = 3000;
    state->memory_budget = memory_budget;
    state->stage_started_ms = esp_timer_get_time() / 1000;
}

esp_err_t espagent_agent_state_transition(espagent_agent_state_t *state,
                                           espagent_agent_stage_t next,
                                           uint32_t timeout_ms)
{
    if (!state || next < ESPAGENT_AGENT_STAGE_ROUTE ||
        next > ESPAGENT_AGENT_STAGE_FINALIZE || timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    state->stage = next;
    state->stage_timeout_ms = timeout_ms;
    state->stage_started_ms = esp_timer_get_time() / 1000;
    return ESP_OK;
}

bool espagent_agent_state_timed_out(const espagent_agent_state_t *state,
                                    int64_t now_ms)
{
    return state && state->stage_timeout_ms > 0 &&
           now_ms - state->stage_started_ms > (int64_t)state->stage_timeout_ms;
}

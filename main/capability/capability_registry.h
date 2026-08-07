#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef esp_err_t (*espagent_capability_execute_fn)(const char *input_json,
                                                    char *output,
                                                    size_t output_size);

typedef enum {
    ESPAGENT_CAP_CALLER_SYSTEM = 0,
    ESPAGENT_CAP_CALLER_AGENT,
    ESPAGENT_CAP_CALLER_MESH,
    ESPAGENT_CAP_CALLER_EVENT,
    ESPAGENT_CAP_CALLER_CLI,
    ESPAGENT_CAP_CALLER_SUBAGENT,
} espagent_capability_caller_t;

typedef enum {
    ESPAGENT_CAP_RISK_READ_ONLY = 0,
    ESPAGENT_CAP_RISK_WRITE_FILE,
    ESPAGENT_CAP_RISK_CONTROL,
    ESPAGENT_CAP_RISK_NETWORK,
    ESPAGENT_CAP_RISK_PRIVACY,
    ESPAGENT_CAP_RISK_SYSTEM,
} espagent_capability_risk_t;

enum {
    ESPAGENT_CAP_FLAG_LLM_VISIBLE = 1U << 0,
    ESPAGENT_CAP_FLAG_RESTRICTED = 1U << 1,
    ESPAGENT_CAP_FLAG_ROOT_ONLY = 1U << 2,
    ESPAGENT_CAP_FLAG_SUBAGENT_ALLOWED = 1U << 3,
    ESPAGENT_CAP_FLAG_EVENT_SOURCE = 1U << 4,
    ESPAGENT_CAP_FLAG_DYNAMIC = 1U << 5,
};

typedef struct {
    const char *id;
    const char *name;
    const char *version;
    const char *role;
    const char *family;
    const char *description;
    const char *input_schema_json;
    uint32_t timeout_ms;
    bool idempotent;
    bool requires_guardian;
    uint32_t flags;
    espagent_capability_risk_t risk;
    espagent_capability_execute_fn execute;
} espagent_capability_descriptor_t;

esp_err_t espagent_capability_registry_init(void);

esp_err_t espagent_capability_register(const espagent_capability_descriptor_t *descriptor);

esp_err_t espagent_capability_register_legacy_tool(const char *name,
                                                   const char *description,
                                                   const char *input_schema_json,
                                                   espagent_capability_execute_fn execute);

const espagent_capability_descriptor_t *espagent_capability_find(const char *name_or_id);

esp_err_t espagent_capability_execute(const char *name_or_id,
                                      const char *input_json,
                                      espagent_capability_caller_t caller,
                                      char *output,
                                      size_t output_size);

char *espagent_capability_build_llm_tools_json(void);

void espagent_capability_list(const espagent_capability_descriptor_t **items, int *count);

const char *espagent_capability_risk_name(espagent_capability_risk_t risk);

/* Mesh, Lua, sandbox and tool callers use this same action contract. */
bool espagent_capability_mesh_action_allowed(const char *action,
                                             const char *target_role);
bool espagent_capability_requires_guardian(const char *name_or_id);

#pragma once

#include "esp_err.h"
#include "mesh/mesh_types.h"

#include <stddef.h>

typedef esp_err_t (*control_command_executor_t)(const espagent_mesh_command_t *cmd,
                                                void *ctx,
                                                char *output,
                                                size_t output_size);

esp_err_t control_command_queue_init(void);

esp_err_t control_command_queue_submit(const espagent_mesh_command_t *cmd,
                                       control_command_executor_t executor,
                                       void *ctx,
                                       char *output,
                                       size_t output_size);

esp_err_t control_command_queue_emergency_stop(char *output, size_t output_size);
esp_err_t control_command_queue_clear_emergency_stop(char *output, size_t output_size);

esp_err_t control_command_queue_state_json(char *output, size_t output_size);

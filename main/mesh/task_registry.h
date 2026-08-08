#pragma once

#include "esp_err.h"
#include "mesh/mesh_types.h"

#include <stddef.h>

/*
 * Bounded live task index for correlating Mesh dispatch, policy, and results.
 * It deliberately stays in RAM: the command protocol remains the source of
 * truth across nodes and a reboot must not replay a stale actuator command.
 */
esp_err_t espagent_task_registry_upsert(const espagent_task_meta_t *task,
                                        const char *summary);
esp_err_t espagent_task_registry_update(const char *command_id,
                                        espagent_task_status_t status,
                                        const char *summary);
esp_err_t espagent_task_registry_write_json(const char *task_or_command_id,
                                            char *output,
                                            size_t output_size);

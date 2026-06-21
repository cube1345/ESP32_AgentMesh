#pragma once

#include "esp_err.h"
#include "mesh/mesh_types.h"

#include <stdbool.h>
#include <stddef.h>

bool espagent_mesh_auth_enabled(void);

esp_err_t espagent_mesh_auth_sign_command(const espagent_mesh_command_t *cmd,
                                          char *signature,
                                          size_t signature_size);

esp_err_t espagent_mesh_auth_verify_command(const espagent_mesh_command_t *cmd,
                                            char *reason,
                                            size_t reason_size);

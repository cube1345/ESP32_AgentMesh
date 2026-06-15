#pragma once

#include "esp_err.h"
#include "mesh/mesh_types.h"

#include <stddef.h>

esp_err_t espagent_mesh_build_node_topic(const char *node_id,
                                         const char *suffix,
                                         char *out,
                                         size_t out_size);

esp_err_t espagent_mesh_build_role_topic(const char *role,
                                         const char *suffix,
                                         char *out,
                                         size_t out_size);

esp_err_t espagent_mesh_parse_command_json(const char *json,
                                           size_t json_len,
                                           espagent_mesh_command_t *out,
                                           char *err_buf,
                                           size_t err_buf_size);

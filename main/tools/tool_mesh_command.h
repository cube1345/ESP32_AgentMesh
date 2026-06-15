#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_mesh_send_command_execute(const char *input_json,
                                         char *output,
                                         size_t output_size);

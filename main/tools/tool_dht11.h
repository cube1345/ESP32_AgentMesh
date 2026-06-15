#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_read_temperature_humidity_execute(const char *input_json, char *output, size_t output_size);

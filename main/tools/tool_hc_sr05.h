#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_read_presence_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_hc_sr05_read_distance_execute(const char *input_json, char *output, size_t output_size);

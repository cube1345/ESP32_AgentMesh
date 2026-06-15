#pragma once

#include "esp_err.h"

#include <stddef.h>

esp_err_t tool_max98357_play_tone_execute(const char *input_json, char *output, size_t output_size);

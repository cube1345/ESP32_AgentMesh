#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t ir_emergency_stop_init(void);
esp_err_t ir_emergency_stop_start(void);
esp_err_t ir_emergency_stop_status(char *output, size_t output_size);

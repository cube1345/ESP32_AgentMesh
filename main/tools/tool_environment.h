#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int temperature_c_x10;
    int humidity_percent_x10;
    int co2eq_ppm;
    int tvoc_ppb;
    int light_lux_x10;
    int light_raw;
    bool sgp30_warming_up;
} tool_environment_values_t;

esp_err_t tool_read_environment_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_environment_read_values(tool_environment_values_t *values,
                                       char *status,
                                       size_t status_size);
esp_err_t tool_environment_read_bh1750_values(int sda_gpio,
                                              int scl_gpio,
                                              int address,
                                              float *lux,
                                              uint16_t *raw,
                                              char *status,
                                              size_t status_size);

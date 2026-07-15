#pragma once

#include "esp_err.h"
#include "tools/tool_environment.h"

#include <stddef.h>
#include <stdint.h>

esp_err_t sensor_history_init(void);

esp_err_t sensor_history_maybe_append(const tool_environment_values_t *values,
                                      const char *status);

esp_err_t sensor_history_build_recent(uint32_t limit,
                                      char *out,
                                      size_t out_size);

esp_err_t sensor_history_build_summary(uint32_t hours,
                                       char *out,
                                       size_t out_size);

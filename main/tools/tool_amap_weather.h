#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_amap_weather_init(void);
esp_err_t tool_amap_weather_execute(const char *input_json, char *output,
                                    size_t output_size);
esp_err_t tool_amap_weather_set_key(const char *api_key);
esp_err_t tool_amap_weather_set_default_location(const char *location,
                                                 const char *adcode);

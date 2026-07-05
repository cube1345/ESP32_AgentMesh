#pragma once

#include "esp_err.h"

#include <stddef.h>

esp_err_t tool_gateway_status_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_gateway_register_ble_mesh_device_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_gateway_ble_mesh_send_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_ota_gateway_plan_execute(const char *input_json, char *output, size_t output_size);


#pragma once

#include "esp_err.h"

#include <stddef.h>

esp_err_t espagent_ble_mesh_bridge_init(void);

esp_err_t espagent_ble_mesh_bridge_status_json(char *buf, size_t buf_size);

esp_err_t espagent_ble_mesh_bridge_register_device(const char *input_json,
                                                   char *output,
                                                   size_t output_size);

esp_err_t espagent_ble_mesh_bridge_send(const char *input_json,
                                        char *output,
                                        size_t output_size);


#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

esp_err_t espagent_time_sync_start(void);
esp_err_t espagent_time_sync_wait(uint32_t timeout_ms);
bool espagent_time_is_valid(void);

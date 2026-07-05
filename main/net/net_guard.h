#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t espagent_net_guard_take(uint32_t timeout_ms);
void espagent_net_guard_give(void);
void espagent_net_guard_defer_background(uint32_t defer_ms);
bool espagent_net_guard_background_allowed(size_t min_internal_free, size_t min_largest_internal);
esp_err_t espagent_net_guard_take_background(uint32_t timeout_ms,
                                             size_t min_internal_free,
                                             size_t min_largest_internal);

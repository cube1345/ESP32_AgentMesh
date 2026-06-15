#pragma once

#include "esp_err.h"
#include <stddef.h>

/* Set a servo motor angle or pulse width.
 * Input JSON: {"angle":<0-180>} or {"pulse_us":<500-2500>}
 * The servo GPIO is fixed by firmware configuration. */
esp_err_t tool_servo_write_execute(const char *input_json, char *output, size_t output_size);

/* Direct helpers for non-agent startup or internal control paths. */
esp_err_t tool_servo_set_angle(int angle);
esp_err_t tool_servo_set_pulse_us(int pulse_us);

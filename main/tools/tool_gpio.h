#pragma once

#include "esp_err.h"

#include <stddef.h>

/* Initialize GPIO and WS2812 helpers. */
esp_err_t tool_gpio_init(void);

/* Write a GPIO pin HIGH or LOW.
 * Input JSON: {"pin":<int>,"state":<0|1>}
 * Also accepts {"level":<0|1>} for compatibility. */
esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size);

/* Read a single GPIO pin state.
 * Input JSON: {"pin":<int>} */
esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size);

/* Read all allowed GPIO pin states.
 * Input JSON: {} */
esp_err_t tool_gpio_read_all_execute(const char *input_json, char *output, size_t output_size);

/* Set the onboard or specified WS2812 RGB LED.
 * Input JSON: {"r":<0-255>,"g":<0-255>,"b":<0-255>,"brightness"?:<0-255>,"pin"?:<int>} */
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size);

/* High-level chat-friendly status light alias.
 * Input JSON: {"color"?:<string>,"brightness"?:<0-255>,"pin"?:<int>,"r"?:<0-255>,"g"?:<0-255>,"b"?:<0-255>} */
esp_err_t tool_set_status_light_execute(const char *input_json, char *output, size_t output_size);

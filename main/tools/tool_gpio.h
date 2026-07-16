#pragma once

#include "esp_err.h"

#include <stddef.h>

/* Initialize GPIO outputs and WS2812 status-light helpers. */
esp_err_t tool_gpio_init(void);

/* Write a GPIO pin HIGH or LOW.
 * Input JSON: {"pin":<int>,"state":<0|1>}
 * Also accepts {"level":<0|1>} for compatibility. */
esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size);

/* Write one dedicated copper-fill GPIO pad on the control agent.
 * Input JSON: {"pin":4|5|6,"state":0|1}
 * Also accepts {"level":0|1} and {"value":"high"|"low"}. */
esp_err_t tool_copper_gpio_write_execute(const char *input_json, char *output, size_t output_size);

/* High-level fixed device controls on the control agent.
 * GPIO4 = humidifier active-high, GPIO5 = fan active-high by default.
 * set_device_led is kept as an on/off alias for the onboard WS2812 status light.
 * Input JSON: {"state":0|1}; also accepts {"level":0|1} and
 * logical {"value":"on"|"off"|"open"|"close"|"打开"|"关闭"}.
 * Physical {"value":"high"|"low"|"拉高"|"拉低"} is also accepted and converted
 * to the matching logical state for each device polarity.
 * Use gpio_write/copper_gpio_write when the user explicitly asks for physical HIGH/LOW. */
esp_err_t tool_set_humidifier_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_set_fan_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_set_device_led_execute(const char *input_json, char *output, size_t output_size);

/* Read a single GPIO pin state.
 * Input JSON: {"pin":<int>} */
esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size);

/* Read all allowed GPIO pin states.
 * Input JSON: {} */
esp_err_t tool_gpio_read_all_execute(const char *input_json, char *output, size_t output_size);

/* Set the onboard or specified WS2812 RGB LED.
 * Input JSON: {"r":<0-255>,"g":<0-255>,"b":<0-255>,"brightness"?:<0-255>,"pin"?:<int>} */
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size);

/* High-level chat-friendly WS2812 status light alias.
 * Defaults to ESPAGENT_WS2812_DEFAULT_GPIO.
 * Input JSON: {"color"?:<string>,"brightness"?:<0-255>,
 * "pin"?:<int>,
 * "r"?:<0-255>,"g"?:<0-255>,"b"?:<0-255>} */
esp_err_t tool_set_status_light_execute(const char *input_json, char *output, size_t output_size);

/* Start / stop internal rapid thinking animation on configured ordinary GPIO LEDs.
 * This is for local firmware state indication, not LLM-facing tool use. */
esp_err_t tool_status_indicator_thinking_start(void);
esp_err_t tool_status_indicator_thinking_stop(void);

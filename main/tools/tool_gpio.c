#include "tools/tool_gpio.h"

#include "tools/gpio_policy.h"
#include "espagent_config.h"

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "tool_gpio";

#define WS2812_RESOLUTION_HZ 10000000
#define WS2812_RESET_US      50

static const rmt_symbol_word_t s_ws2812_zero = {
    .level0 = 1,
    .duration0 = 3,
    .level1 = 0,
    .duration1 = 9,
};

static const rmt_symbol_word_t s_ws2812_one = {
    .level0 = 1,
    .duration0 = 9,
    .level1 = 0,
    .duration1 = 3,
};

static const rmt_symbol_word_t s_ws2812_reset = {
    .level0 = 0,
    .duration0 = (WS2812_RESOLUTION_HZ / 1000000 * WS2812_RESET_US) / 2,
    .level1 = 0,
    .duration1 = (WS2812_RESOLUTION_HZ / 1000000 * WS2812_RESET_US) / 2,
};

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    int gpio_num;
} ws2812_state_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t brightness;
    bool valid;
} ws2812_base_color_t;

typedef struct {
    int pins[3];
    size_t count;
    int active_level;
    bool warned_invalid;
} status_led_config_t;

static ws2812_state_t s_ws2812 = {
    .channel = NULL,
    .encoder = NULL,
    .gpio_num = -1,
};
static ws2812_base_color_t s_ws2812_base = {
    .r = 0,
    .g = 0,
    .b = 0,
    .brightness = 255,
    .valid = false,
};
static SemaphoreHandle_t s_indicator_mutex = NULL;
static TaskHandle_t s_indicator_task = NULL;
static int s_indicator_refcount = 0;
static int s_indicator_generation = 0;
static status_led_config_t s_status_led = {
    .pins = {ESPAGENT_THINK_LED_GPIO_1, ESPAGENT_THINK_LED_GPIO_2, ESPAGENT_THINK_LED_GPIO_3},
    .count = 0,
    .active_level = ESPAGENT_THINK_LED_ACTIVE_LEVEL ? 1 : 0,
    .warned_invalid = false,
};

static bool get_required_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

static bool get_optional_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

static const char *get_optional_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static esp_err_t validate_allowed_gpio(int pin, char *output, size_t output_size)
{
    if (!gpio_policy_pin_is_allowed(pin)) {
        if (gpio_policy_pin_forbidden_hint(pin, output, output_size)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (ESPAGENT_GPIO_ALLOWED_CSV[0] != '\0') {
            snprintf(output, output_size, "Error: pin %d is not in allowed list", pin);
        } else {
            snprintf(output, output_size, "Error: pin must be %d-%d",
                     ESPAGENT_GPIO_MIN_PIN, ESPAGENT_GPIO_MAX_PIN);
        }
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t configure_output_gpio(int pin, bool enable_pulldown)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = enable_pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&cfg);
}

static bool is_fixed_device_gpio(int pin)
{
    return pin == ESPAGENT_HUMIDIFIER_GPIO || pin == ESPAGENT_FAN_GPIO;
}

static esp_err_t ensure_output_gpio(int pin)
{
    if (!is_fixed_device_gpio(pin)) {
        return configure_output_gpio(pin, false);
    }

    /* These pins are reserved during startup; do not reserve them again per call. */
    esp_err_t err = gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_pull_mode((gpio_num_t)pin,
                              ESPAGENT_FIXED_OUTPUT_PULLDOWN
                                  ? GPIO_PULLDOWN_ONLY
                                  : GPIO_FLOATING);
}

static esp_err_t fixed_device_configure_safe_off(int pin, const char *device_name, int active_level)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        ESP_LOGW(TAG, "%s safe-off skipped: GPIO%d is not a valid output", device_name, pin);
        return ESP_ERR_INVALID_ARG;
    }

    int inactive_level = active_level ? 0 : 1;
    bool enable_pulldown = ESPAGENT_FIXED_OUTPUT_PULLDOWN != 0;

    /* Configure the output and bias first, then apply the safe inactive level. */
    esp_err_t err = configure_output_gpio(pin, enable_pulldown);
    if (err == ESP_OK) {
        err = gpio_set_level((gpio_num_t)pin, inactive_level);
    }

    ESP_LOGI(TAG, "%s safe-off GPIO%d active_%s idle=%s pulldown=%s -> %s",
             device_name,
             pin,
             active_level ? "high" : "low",
             inactive_level ? "HIGH" : "LOW",
             enable_pulldown ? "enabled" : "disabled",
             esp_err_to_name(err));
    return err;
}

static bool is_copper_gpio_pin(int pin)
{
    return pin == 4 || pin == 5 || pin == 6;
}

static const char *copper_gpio_meaning(int pin)
{
    switch (pin) {
    case 4:
        return "copper_gpio_4";
    case 5:
        return "copper_gpio_5";
    case 6:
        return "copper_gpio_6";
    default:
        return "unknown";
    }
}

static bool parse_gpio_level(cJSON *root, int *state)
{
    if (!root || !state) {
        return false;
    }
    if (get_optional_int(root, "state", state) ||
        get_optional_int(root, "level", state)) {
        return true;
    }

    const char *value = get_optional_string(root, "value");
    if (!value) {
        value = get_optional_string(root, "state");
    }
    if (!value) {
        value = get_optional_string(root, "level");
    }
    if (!value) {
        return false;
    }
    if (strcasecmp(value, "high") == 0 ||
        strcasecmp(value, "on") == 0 ||
        strcmp(value, "open") == 0 ||
        strcmp(value, "enable") == 0 ||
        strcmp(value, "打开") == 0 ||
        strcmp(value, "开启") == 0 ||
        strcmp(value, "拉高") == 0 ||
        strcmp(value, "高电平") == 0) {
        *state = 1;
        return true;
    }
    if (strcasecmp(value, "low") == 0 ||
        strcasecmp(value, "off") == 0 ||
        strcmp(value, "close") == 0 ||
        strcmp(value, "disable") == 0 ||
        strcmp(value, "关闭") == 0 ||
        strcmp(value, "关掉") == 0 ||
        strcmp(value, "拉低") == 0 ||
        strcmp(value, "低电平") == 0) {
        *state = 0;
        return true;
    }
    return false;
}

static bool parse_fixed_device_state(cJSON *root, int active_level, int *state, int *physical_level)
{
    if (!root || !state || !physical_level) {
        return false;
    }

    int value = -1;
    if (get_optional_int(root, "state", &value) ||
        get_optional_int(root, "level", &value)) {
        if (value != 0 && value != 1) {
            return false;
        }
        *state = value;
        *physical_level = value ? active_level : (active_level ? 0 : 1);
        return true;
    }

    const char *text = get_optional_string(root, "value");
    if (!text) {
        text = get_optional_string(root, "state");
    }
    if (!text) {
        text = get_optional_string(root, "level");
    }
    if (!text) {
        return false;
    }

    if (strcasecmp(text, "on") == 0 ||
        strcmp(text, "open") == 0 ||
        strcmp(text, "enable") == 0 ||
        strcmp(text, "打开") == 0 ||
        strcmp(text, "开启") == 0) {
        *state = 1;
        *physical_level = active_level;
        return true;
    }
    if (strcasecmp(text, "off") == 0 ||
        strcmp(text, "close") == 0 ||
        strcmp(text, "disable") == 0 ||
        strcmp(text, "关闭") == 0 ||
        strcmp(text, "关掉") == 0) {
        *state = 0;
        *physical_level = active_level ? 0 : 1;
        return true;
    }
    if (strcasecmp(text, "high") == 0 ||
        strcmp(text, "拉高") == 0 ||
        strcmp(text, "高电平") == 0) {
        *physical_level = 1;
        *state = active_level ? 1 : 0;
        return true;
    }
    if (strcasecmp(text, "low") == 0 ||
        strcmp(text, "拉低") == 0 ||
        strcmp(text, "低电平") == 0) {
        *physical_level = 0;
        *state = active_level ? 0 : 1;
        return true;
    }
    return false;
}

static int status_led_idle_level(void)
{
    return s_status_led.active_level ? 0 : 1;
}

static esp_err_t status_led_apply_mask(uint32_t mask)
{
    if (s_status_led.count == 0) {
        return ESP_OK;
    }

    for (size_t i = 0; i < s_status_led.count; i++) {
        int pin = s_status_led.pins[i];
        int level = ((mask >> i) & 0x1U) ? s_status_led.active_level : status_led_idle_level();
        esp_err_t err = gpio_set_level((gpio_num_t)pin, level);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

static void status_led_off_no_output(void)
{
    (void)status_led_apply_mask(0);
}

static uint32_t status_led_mask_for_step(size_t step)
{
    if (s_status_led.count == 0) {
        return 0;
    }
    if (s_status_led.count == 1) {
        return (step % 2 == 0) ? 0x1U : 0x0U;
    }

    return (1U << (step % s_status_led.count));
}

static void status_led_configure_once(void)
{
    if (s_status_led.count > 0 || s_status_led.warned_invalid) {
        return;
    }

    for (size_t i = 0; i < sizeof(s_status_led.pins) / sizeof(s_status_led.pins[0]); i++) {
        int pin = s_status_led.pins[i];
        if (pin < 0) {
            continue;
        }
        if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || !gpio_policy_pin_is_allowed(pin)) {
            if (!s_status_led.warned_invalid) {
                ESP_LOGW(TAG, "thinking LED GPIO %d ignored: invalid or forbidden by policy", pin);
            }
            s_status_led.warned_invalid = true;
            continue;
        }
        if (ensure_output_gpio(pin) != ESP_OK) {
            if (!s_status_led.warned_invalid) {
                ESP_LOGW(TAG, "thinking LED GPIO %d ignored: failed to configure output", pin);
            }
            s_status_led.warned_invalid = true;
            continue;
        }
        s_status_led.pins[s_status_led.count++] = pin;
    }

    if (s_status_led.count > 0) {
        status_led_off_no_output();
    }
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static uint8_t scale_component(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * brightness + 127U) / 255U);
}

static size_t ws2812_encoder_callback(const void *data, size_t data_size,
                                      size_t symbols_written, size_t symbols_free,
                                      rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;

    if (symbols_free < 8) {
        return 0;
    }

    size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = (const uint8_t *)data;
    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (int bitmask = 0x80; bitmask != 0; bitmask >>= 1) {
            symbols[symbol_pos++] = (bytes[data_pos] & bitmask) ? s_ws2812_one : s_ws2812_zero;
        }
        return symbol_pos;
    }

    symbols[0] = s_ws2812_reset;
    *done = true;
    return 1;
}

static void ws2812_release(void)
{
    if (s_ws2812.channel) {
        rmt_disable(s_ws2812.channel);
    }
    if (s_ws2812.encoder) {
        rmt_del_encoder(s_ws2812.encoder);
        s_ws2812.encoder = NULL;
    }
    if (s_ws2812.channel) {
        rmt_del_channel(s_ws2812.channel);
        s_ws2812.channel = NULL;
    }
    s_ws2812.gpio_num = -1;
}

static esp_err_t ws2812_ensure_ready(int pin)
{
    esp_err_t err;

    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ws2812.channel && s_ws2812.encoder && s_ws2812.gpio_num == pin) {
        return ESP_OK;
    }

    if (s_ws2812.channel || s_ws2812.encoder) {
        ws2812_release();
    }

    rmt_tx_channel_config_t tx_chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = pin,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    err = rmt_new_tx_channel(&tx_chan_cfg, &s_ws2812.channel);
    if (err != ESP_OK) {
        ws2812_release();
        return err;
    }

    rmt_simple_encoder_config_t encoder_cfg = {
        .callback = ws2812_encoder_callback,
        .min_chunk_size = 8,
    };
    err = rmt_new_simple_encoder(&encoder_cfg, &s_ws2812.encoder);
    if (err != ESP_OK) {
        ws2812_release();
        return err;
    }

    err = rmt_enable(s_ws2812.channel);
    if (err != ESP_OK) {
        ws2812_release();
        return err;
    }

    s_ws2812.gpio_num = pin;
    return ESP_OK;
}

static esp_err_t ws2812_apply_color(int pin, int red, int green, int blue, int brightness,
                                    char *output, size_t output_size, const char *label)
{
    if (brightness < 0 || brightness > 255) {
        snprintf(output, output_size, "Error: brightness must be between 0 and 255");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ws2812_ensure_ready(pin);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            snprintf(output, output_size, "Error: GPIO %d is not a valid WS2812 output pin", pin);
        } else {
            snprintf(output, output_size, "Error: failed to initialize WS2812 on GPIO %d (%s)",
                     pin, esp_err_to_name(err));
        }
        return err;
    }

    uint8_t rgb[3] = {
        scale_component(clamp_u8(green), (uint8_t)brightness),
        scale_component(clamp_u8(red), (uint8_t)brightness),
        scale_component(clamp_u8(blue), (uint8_t)brightness),
    };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    err = rmt_transmit(s_ws2812.channel, s_ws2812.encoder, rgb, sizeof(rgb), &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_ws2812.channel, 100);
    }

    if (err == ESP_OK) {
        if (label && label[0] != '\0') {
            snprintf(output, output_size,
                     "OK: status light on GPIO %d set to %s (RGB=%u,%u,%u brightness=%d)",
                     pin, label,
                     (unsigned)clamp_u8(red), (unsigned)clamp_u8(green), (unsigned)clamp_u8(blue),
                     brightness);
        } else {
            snprintf(output, output_size,
                     "OK: WS2812 on GPIO %d set to RGB(%u,%u,%u) brightness=%d",
                     pin,
                     (unsigned)clamp_u8(red), (unsigned)clamp_u8(green), (unsigned)clamp_u8(blue),
                     brightness);
        }
    } else {
        snprintf(output, output_size, "Error: failed to update WS2812 on GPIO %d (%s)",
                 pin, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "ws2812 pin=%d rgb=(%d,%d,%d) brightness=%d -> %s",
             pin, red, green, blue, brightness, esp_err_to_name(err));
    return err;
}

static void ws2812_note_base_color(int red, int green, int blue, int brightness)
{
    s_ws2812_base.r = clamp_u8(red);
    s_ws2812_base.g = clamp_u8(green);
    s_ws2812_base.b = clamp_u8(blue);
    s_ws2812_base.brightness = clamp_u8(brightness);
    s_ws2812_base.valid = true;
}

static void status_indicator_task(void *arg)
{
    int generation = (int)(intptr_t)arg;
    size_t step = 0;
    const TickType_t period = pdMS_TO_TICKS(80);

    while (1) {
        if (!s_indicator_mutex) {
            break;
        }
        if (xSemaphoreTake(s_indicator_mutex, portMAX_DELAY) == pdTRUE) {
            bool should_run = (s_indicator_refcount > 0 && generation == s_indicator_generation);
            xSemaphoreGive(s_indicator_mutex);
            if (!should_run) {
                break;
            }
        }

        (void)status_led_apply_mask(status_led_mask_for_step(step++));
        vTaskDelay(period);
    }

    if (s_indicator_mutex &&
        xSemaphoreTake(s_indicator_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (generation == s_indicator_generation) {
            s_indicator_task = NULL;
        }
        xSemaphoreGive(s_indicator_mutex);
    }
    vTaskDelete(NULL);
}

static esp_err_t ensure_indicator_mutex(void)
{
    if (s_indicator_mutex) {
        return ESP_OK;
    }
    s_indicator_mutex = xSemaphoreCreateMutex();
    return s_indicator_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

static bool resolve_named_color(const char *color, int *r, int *g, int *b)
{
    if (!color) {
        return false;
    }

    if (strcasecmp(color, "off") == 0 || strcmp(color, "关闭") == 0 || strcmp(color, "熄灭") == 0) {
        *r = 0; *g = 0; *b = 0;
        return true;
    }
    if (strcasecmp(color, "red") == 0 || strcmp(color, "红") == 0 || strcmp(color, "红色") == 0) {
        *r = 255; *g = 0; *b = 0;
        return true;
    }
    if (strcasecmp(color, "green") == 0 || strcmp(color, "绿") == 0 || strcmp(color, "绿色") == 0) {
        *r = 0; *g = 255; *b = 0;
        return true;
    }
    if (strcasecmp(color, "blue") == 0 || strcmp(color, "蓝") == 0 || strcmp(color, "蓝色") == 0) {
        *r = 0; *g = 0; *b = 255;
        return true;
    }
    if (strcasecmp(color, "white") == 0 || strcmp(color, "白") == 0 || strcmp(color, "白色") == 0) {
        *r = 255; *g = 255; *b = 255;
        return true;
    }
    if (strcasecmp(color, "yellow") == 0 || strcmp(color, "黄") == 0 || strcmp(color, "黄色") == 0) {
        *r = 255; *g = 180; *b = 0;
        return true;
    }
    if (strcasecmp(color, "orange") == 0 || strcmp(color, "橙") == 0 || strcmp(color, "橙色") == 0) {
        *r = 255; *g = 96; *b = 0;
        return true;
    }
    if (strcasecmp(color, "purple") == 0 || strcasecmp(color, "magenta") == 0 ||
        strcmp(color, "紫") == 0 || strcmp(color, "紫色") == 0) {
        *r = 180; *g = 0; *b = 255;
        return true;
    }
    if (strcasecmp(color, "cyan") == 0 || strcmp(color, "青") == 0 || strcmp(color, "青色") == 0) {
        *r = 0; *g = 180; *b = 255;
        return true;
    }

    return false;
}

esp_err_t tool_gpio_init(void)
{
    status_led_configure_once();
    (void)fixed_device_configure_safe_off(ESPAGENT_HUMIDIFIER_GPIO,
                                          "humidifier",
                                          ESPAGENT_HUMIDIFIER_ACTIVE_LEVEL ? 1 : 0);
    (void)fixed_device_configure_safe_off(ESPAGENT_FAN_GPIO,
                                          "fan",
                                          ESPAGENT_FAN_ACTIVE_LEVEL ? 1 : 0);
    (void)ensure_indicator_mutex();
    ESP_LOGI(TAG,
             "GPIO tools ready (humidifier GPIO%d active_%s, fan GPIO%d active_%s, status WS2812 GPIO=%d, thinking_leds=%u)",
             ESPAGENT_HUMIDIFIER_GPIO,
             ESPAGENT_HUMIDIFIER_ACTIVE_LEVEL ? "high" : "low",
             ESPAGENT_FAN_GPIO,
             ESPAGENT_FAN_ACTIVE_LEVEL ? "high" : "low",
             ESPAGENT_WS2812_DEFAULT_GPIO,
             (unsigned)s_status_led.count);
    return ESP_OK;
}

esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int pin = -1;
    int state = -1;
    if (!get_required_int(root, "pin", &pin)) {
        snprintf(output, output_size, "Error: 'pin' required (integer)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!get_optional_int(root, "state", &state) && !get_optional_int(root, "level", &state)) {
        snprintf(output, output_size, "Error: 'state' required (0 or 1)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (state != 0 && state != 1) {
        snprintf(output, output_size, "Error: state must be 0 or 1");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = validate_allowed_gpio(pin, output, output_size);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    err = ensure_output_gpio(pin);
    if (err == ESP_OK) {
        err = gpio_set_level(pin, state);
    }

    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: GPIO %d set %s", pin, state ? "HIGH" : "LOW");
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(output, output_size, "Error: GPIO %d is not a valid output pin", pin);
    } else {
        snprintf(output, output_size, "Error: failed to set GPIO %d (%s)", pin, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "gpio_write pin=%d state=%d -> %s", pin, state, esp_err_to_name(err));
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_copper_gpio_write_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int pin = -1;
    int state = -1;
    if (!get_required_int(root, "pin", &pin)) {
        snprintf(output, output_size, "Error: 'pin' required and must be one of 4, 5, 6");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!is_copper_gpio_pin(pin)) {
        snprintf(output, output_size, "Error: copper_gpio_write only allows GPIO4, GPIO5, or GPIO6");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_gpio_level(root, &state) || (state != 0 && state != 1)) {
        snprintf(output, output_size, "Error: state/level/value required, use 0/1 or high/low");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_output_gpio(pin);
    if (err == ESP_OK) {
        err = gpio_set_level(pin, state);
    }

    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: %s GPIO%d set %s",
                 copper_gpio_meaning(pin), pin, state ? "HIGH" : "LOW");
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(output, output_size, "Error: GPIO%d is not a valid output pin", pin);
    } else {
        snprintf(output, output_size, "Error: failed to set GPIO%d (%s)",
                 pin, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "copper_gpio_write pin=%d state=%d -> %s",
             pin, state, esp_err_to_name(err));
    cJSON_Delete(root);
    return err;
}

static esp_err_t fixed_device_gpio_write(const char *input_json,
                                         char *output,
                                         size_t output_size,
                                         int pin,
                                         const char *device_name,
                                         int active_level)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int state = -1;
    int physical_level = -1;
    if (!parse_fixed_device_state(root, active_level, &state, &physical_level)) {
        snprintf(output, output_size,
                 "Error: state/level/value required, use on/off for logical device state or high/low for physical GPIO level");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_output_gpio(pin);
    if (err == ESP_OK) {
        err = gpio_set_level(pin, physical_level);
    }

    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: %s on GPIO%d turned %s (active_%s, level=%s)",
                 device_name, pin, state ? "ON" : "OFF",
                 active_level ? "high" : "low",
                 physical_level ? "HIGH" : "LOW");
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(output, output_size, "Error: GPIO%d is not a valid output pin", pin);
    } else {
        snprintf(output, output_size, "Error: failed to set %s GPIO%d (%s)",
                 device_name, pin, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "%s pin=%d state=%d active_level=%d level=%d -> %s",
             device_name, pin, state, active_level, physical_level, esp_err_to_name(err));
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_set_humidifier_execute(const char *input_json, char *output, size_t output_size)
{
    return fixed_device_gpio_write(input_json,
                                   output,
                                   output_size,
                                   ESPAGENT_HUMIDIFIER_GPIO,
                                   "humidifier",
                                   ESPAGENT_HUMIDIFIER_ACTIVE_LEVEL ? 1 : 0);
}

esp_err_t tool_set_fan_execute(const char *input_json, char *output, size_t output_size)
{
    return fixed_device_gpio_write(input_json,
                                   output,
                                   output_size,
                                   ESPAGENT_FAN_GPIO,
                                   "fan",
                                   ESPAGENT_FAN_ACTIVE_LEVEL ? 1 : 0);
}

esp_err_t tool_set_device_led_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int state = -1;
    if (!parse_gpio_level(root, &state) || (state != 0 && state != 1)) {
        snprintf(output, output_size,
                 "Error: state/level/value required, use 1/on/open or 0/off/close");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int pin = ESPAGENT_WS2812_DEFAULT_GPIO;
    (void)get_optional_int(root, "pin", &pin);
    esp_err_t err = ws2812_apply_color(pin,
                                       state ? 255 : 0,
                                       state ? 255 : 0,
                                       state ? 255 : 0,
                                       255,
                                       output,
                                       output_size,
                                       state ? "white" : "off");
    if (err == ESP_OK && pin == ESPAGENT_WS2812_DEFAULT_GPIO) {
        ws2812_note_base_color(state ? 255 : 0,
                               state ? 255 : 0,
                               state ? 255 : 0,
                               255);
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int pin = -1;
    if (!get_required_int(root, "pin", &pin)) {
        snprintf(output, output_size, "Error: 'pin' required (integer)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = validate_allowed_gpio(pin, output, output_size);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to read GPIO %d (%s)", pin, esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size, "OK: GPIO %d is %s", pin, gpio_get_level(pin) ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "gpio_read pin=%d", pin);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_gpio_read_all_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    cJSON_Delete(root);

    char *cursor = output;
    size_t remaining = output_size;
    int written = snprintf(cursor, remaining, "GPIO states: ");
    if (written < 0 || (size_t)written >= remaining) {
        output[0] = '\0';
        return ESP_FAIL;
    }
    cursor += (size_t)written;
    remaining -= (size_t)written;

    int count = 0;
    if (ESPAGENT_GPIO_ALLOWED_CSV[0] != '\0') {
        const char *csv_cursor = ESPAGENT_GPIO_ALLOWED_CSV;
        while (*csv_cursor != '\0') {
            char *endptr = NULL;
            long value;

            while (*csv_cursor == ' ' || *csv_cursor == '\t' || *csv_cursor == ',') {
                csv_cursor++;
            }
            if (*csv_cursor == '\0') {
                break;
            }

            value = strtol(csv_cursor, &endptr, 10);
            if (endptr == csv_cursor) {
                while (*csv_cursor != '\0' && *csv_cursor != ',') {
                    csv_cursor++;
                }
                continue;
            }

            if (gpio_policy_pin_is_allowed((int)value)) {
                gpio_config_t cfg = {
                    .pin_bit_mask = 1ULL << (int)value,
                    .mode = GPIO_MODE_INPUT,
                    .pull_up_en = GPIO_PULLUP_DISABLE,
                    .pull_down_en = GPIO_PULLDOWN_DISABLE,
                    .intr_type = GPIO_INTR_DISABLE,
                };
                if (gpio_config(&cfg) == ESP_OK) {
                    written = snprintf(cursor, remaining, "%s%d=%s",
                                       count == 0 ? "" : ", ",
                                       (int)value,
                                       gpio_get_level((int)value) ? "HIGH" : "LOW");
                    if (written < 0 || (size_t)written >= remaining) {
                        break;
                    }
                    cursor += (size_t)written;
                    remaining -= (size_t)written;
                    count++;
                }
            }

            csv_cursor = endptr;
        }
    } else {
        for (int pin = ESPAGENT_GPIO_MIN_PIN; pin <= ESPAGENT_GPIO_MAX_PIN; pin++) {
            if (!gpio_policy_pin_is_allowed(pin)) {
                continue;
            }

            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << pin,
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            if (gpio_config(&cfg) != ESP_OK) {
                continue;
            }

            written = snprintf(cursor, remaining, "%s%d=%s",
                               count == 0 ? "" : ", ",
                               pin,
                               gpio_get_level(pin) ? "HIGH" : "LOW");
            if (written < 0 || (size_t)written >= remaining) {
                break;
            }
            cursor += (size_t)written;
            remaining -= (size_t)written;
            count++;
        }
    }

    if (count == 0) {
        snprintf(output, output_size, "Error: no allowed GPIO pins configured");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "gpio_read_all count=%d", count);
    return ESP_OK;
}

esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int red = -1;
    int green = -1;
    int blue = -1;
    if (!get_required_int(root, "r", &red) ||
        !get_required_int(root, "g", &green) ||
        !get_required_int(root, "b", &blue)) {
        snprintf(output, output_size, "Error: missing required numeric fields 'r', 'g', and 'b'");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    int pin = ESPAGENT_WS2812_DEFAULT_GPIO;
    int brightness = 255;
    (void)get_optional_int(root, "pin", &pin);
    (void)get_optional_int(root, "brightness", &brightness);

    esp_err_t err = ws2812_apply_color(pin, red, green, blue, brightness, output, output_size, NULL);
    if (err == ESP_OK && pin == ESPAGENT_WS2812_DEFAULT_GPIO) {
        ws2812_note_base_color(red, green, blue, brightness);
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_set_status_light_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int brightness = 255;
    int red = -1;
    int green = -1;
    int blue = -1;
    const char *color = get_optional_string(root, "color");

    int pin = ESPAGENT_WS2812_DEFAULT_GPIO;
    (void)get_optional_int(root, "pin", &pin);
    (void)get_optional_int(root, "brightness", &brightness);

    bool has_rgb = get_optional_int(root, "r", &red) &&
                   get_optional_int(root, "g", &green) &&
                   get_optional_int(root, "b", &blue);

    if (!has_rgb) {
        if (!resolve_named_color(color, &red, &green, &blue)) {
            snprintf(output, output_size,
                     "Error: provide either color ('red', 'green', 'blue', 'white', 'yellow', 'orange', 'purple', 'cyan', 'off') or numeric r/g/b");
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t err = ws2812_apply_color(pin, red, green, blue, brightness, output, output_size,
                                       color && color[0] != '\0' ? color : "custom");
    if (err == ESP_OK && pin == ESPAGENT_WS2812_DEFAULT_GPIO) {
        ws2812_note_base_color(red, green, blue, brightness);
    }
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_status_indicator_thinking_start(void)
{
    status_led_configure_once();
    if (s_status_led.count == 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_indicator_mutex(), TAG, "indicator mutex");

    if (xSemaphoreTake(s_indicator_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_indicator_refcount++;
    if (!s_indicator_task) {
        s_indicator_generation++;
        BaseType_t ok = xTaskCreate(status_indicator_task,
                                    "status_indicator",
                                    3072,
                                    (void *)(intptr_t)s_indicator_generation,
                                    3,
                                    &s_indicator_task);
        if (ok != pdPASS) {
            s_indicator_refcount--;
            xSemaphoreGive(s_indicator_mutex);
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(s_indicator_mutex);
    return ESP_OK;
}

esp_err_t tool_status_indicator_thinking_stop(void)
{
    if (s_status_led.count == 0) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_indicator_mutex(), TAG, "indicator mutex");

    if (xSemaphoreTake(s_indicator_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_indicator_refcount > 0) {
        s_indicator_refcount--;
    }
    bool should_restore = (s_indicator_refcount == 0);
    if (should_restore) {
        s_indicator_generation++;
    }
    xSemaphoreGive(s_indicator_mutex);

    if (should_restore) {
        status_led_off_no_output();
    }
    return ESP_OK;
}

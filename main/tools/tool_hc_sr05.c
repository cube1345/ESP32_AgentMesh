#include "tools/tool_hc_sr05.h"

#include "espagent_config.h"
#include "tools/gpio_policy.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdio.h>

static const char *TAG = "tool_hc_sr05";

#define HC_SR05_TRIGGER_US          10
#define HC_SR05_IDLE_US             2
#define HC_SR05_TIMEOUT_US          30000
#define HC_SR05_DEFAULT_SAMPLES     5
#define HC_SR05_MAX_SAMPLES         7
#define HC_SR05_SAMPLE_INTERVAL_MS  60
#define HC_SR05_MAX_DISTANCE_CM     450.0f

static bool get_optional_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

static bool hc_sr05_pin_allowed(int pin, char *output, size_t output_size)
{
    if (!gpio_policy_pin_is_allowed(pin)) {
        if (gpio_policy_pin_forbidden_hint(pin, output, output_size)) {
            return false;
        }
        snprintf(output, output_size, "Error: GPIO%d is not allowed for HC-SR05 wiring", pin);
        return false;
    }
    return true;
}

static bool presence_pin_allowed(int pin, char *output, size_t output_size)
{
    if (!gpio_policy_pin_is_allowed(pin)) {
        if (gpio_policy_pin_forbidden_hint(pin, output, output_size)) {
            return false;
        }
        snprintf(output, output_size, "Error: GPIO%d is not allowed for presence sensor OUT", pin);
        return false;
    }
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)pin)) {
        snprintf(output, output_size, "Error: GPIO%d is not a valid presence sensor input pin", pin);
        return false;
    }
    return true;
}

static esp_err_t read_digital_presence(int out_gpio, char *output, size_t output_size)
{
    if (out_gpio < 0) {
        snprintf(output, output_size,
                 "Error: 3-wire presence sensor OUT pin is not configured. Set ESPAGENT_SECRET_PRESENCE_GPIO, or call with {\"out_gpio\":x}.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!presence_pin_allowed(out_gpio, output, output_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << out_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to configure presence OUT GPIO%d (%s)",
                 out_gpio, esp_err_to_name(err));
        return err;
    }

    int level = gpio_get_level((gpio_num_t)out_gpio);
    snprintf(output, output_size,
             "OK: presence sensor on OUT=GPIO%d -> present=%s level=%s source=digital_out",
             out_gpio, level ? "true" : "false", level ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "presence OUT gpio=%d level=%d", out_gpio, level);
    return ESP_OK;
}

static esp_err_t hc_sr05_configure_pins(int trig_gpio, int echo_gpio,
                                        char *output, size_t output_size)
{
    if (trig_gpio < 0 || echo_gpio < 0) {
        snprintf(output, output_size,
                 "Error: HC-SR05 pins are not configured. Set ESPAGENT_SECRET_HC_SR05_TRIG_GPIO and ESPAGENT_SECRET_HC_SR05_ECHO_GPIO, or call with {\"trig_gpio\":x,\"echo_gpio\":y}.");
        return ESP_ERR_INVALID_STATE;
    }
    if (trig_gpio == echo_gpio) {
        snprintf(output, output_size, "Error: HC-SR05 trig_gpio and echo_gpio must be different");
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)trig_gpio)) {
        snprintf(output, output_size, "Error: GPIO%d is not a valid HC-SR05 trigger output pin", trig_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)echo_gpio)) {
        snprintf(output, output_size, "Error: GPIO%d is not a valid HC-SR05 echo input pin", echo_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (!hc_sr05_pin_allowed(trig_gpio, output, output_size) ||
        !hc_sr05_pin_allowed(echo_gpio, output, output_size)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t trig_cfg = {
        .pin_bit_mask = 1ULL << trig_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&trig_cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to configure HC-SR05 trigger GPIO%d (%s)",
                 trig_gpio, esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)trig_gpio, 0);

    gpio_config_t echo_cfg = {
        .pin_bit_mask = 1ULL << echo_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&echo_cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to configure HC-SR05 echo GPIO%d (%s)",
                 echo_gpio, esp_err_to_name(err));
    }
    return err;
}

static int wait_level_timeout(int pin, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)pin) != level) {
        if ((esp_timer_get_time() - start) >= timeout_us) {
            return -1;
        }
    }
    return 0;
}

static esp_err_t hc_sr05_read_once(int trig_gpio, int echo_gpio, float *distance_cm)
{
    gpio_set_level((gpio_num_t)trig_gpio, 0);
    esp_rom_delay_us(HC_SR05_IDLE_US);
    gpio_set_level((gpio_num_t)trig_gpio, 1);
    esp_rom_delay_us(HC_SR05_TRIGGER_US);
    gpio_set_level((gpio_num_t)trig_gpio, 0);

    if (wait_level_timeout(echo_gpio, 1, HC_SR05_TIMEOUT_US) != 0) {
        return ESP_ERR_TIMEOUT;
    }

    int64_t pulse_start = esp_timer_get_time();
    if (wait_level_timeout(echo_gpio, 0, HC_SR05_TIMEOUT_US) != 0) {
        return ESP_ERR_TIMEOUT;
    }

    int64_t high_us = esp_timer_get_time() - pulse_start;
    float cm = (float)high_us / 58.0f;
    if (cm <= 0.0f || cm > HC_SR05_MAX_DISTANCE_CM) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *distance_cm = cm;
    return ESP_OK;
}

/// @brief 读取HC-SR05传感器距离并判断是否有人存在
/// @param input_json 
/// @param output 
/// @param output_size 
/// @return 
esp_err_t tool_read_presence_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int out_gpio = ESPAGENT_PRESENCE_DEFAULT_GPIO;
    int trig_gpio = ESPAGENT_HC_SR05_DEFAULT_TRIG_GPIO;
    int echo_gpio = ESPAGENT_HC_SR05_DEFAULT_ECHO_GPIO;

    bool has_out = get_optional_int(root, "out_gpio", &out_gpio);
    (void)get_optional_int(root, "trig_gpio", &trig_gpio);
    (void)get_optional_int(root, "echo_gpio", &echo_gpio);
    cJSON_Delete(root);

    if (has_out || out_gpio >= 0) {
        return read_digital_presence(out_gpio, output, output_size);
    }

    if (trig_gpio >= 0 || echo_gpio >= 0) {
        return tool_hc_sr05_read_distance_execute(input_json, output, output_size);
    }

    snprintf(output, output_size,
             "Error: presence sensor is not configured. For a 3-wire sensor set ESPAGENT_SECRET_PRESENCE_GPIO to the OUT pin; for HC-SR05 set Trig/Echo pins.");
    return ESP_ERR_INVALID_STATE;
}

static void sort_float(float *values, int count)
{
    for (int i = 1; i < count; i++) {
        float v = values[i];
        int j = i - 1;
        while (j >= 0 && values[j] > v) {
            values[j + 1] = values[j];
            j--;
        }
        values[j + 1] = v;
    }
}

esp_err_t tool_hc_sr05_read_distance_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int trig_gpio = ESPAGENT_HC_SR05_DEFAULT_TRIG_GPIO;
    int echo_gpio = ESPAGENT_HC_SR05_DEFAULT_ECHO_GPIO;
    int threshold_cm = ESPAGENT_HC_SR05_PRESENT_THRESHOLD_CM;
    int samples = HC_SR05_DEFAULT_SAMPLES;

    (void)get_optional_int(root, "trig_gpio", &trig_gpio);
    (void)get_optional_int(root, "echo_gpio", &echo_gpio);
    (void)get_optional_int(root, "threshold_cm", &threshold_cm);
    (void)get_optional_int(root, "samples", &samples);
    cJSON_Delete(root);

    if (samples < 1) {
        samples = 1;
    } else if (samples > HC_SR05_MAX_SAMPLES) {
        samples = HC_SR05_MAX_SAMPLES;
    }
    if (threshold_cm <= 0) {
        threshold_cm = ESPAGENT_HC_SR05_PRESENT_THRESHOLD_CM;
    }

    esp_err_t err = hc_sr05_configure_pins(trig_gpio, echo_gpio, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    float readings[HC_SR05_MAX_SAMPLES] = {0};
    int valid_count = 0;
    int timeout_count = 0;

    for (int i = 0; i < samples; i++) {
        float cm = 0.0f;
        err = hc_sr05_read_once(trig_gpio, echo_gpio, &cm);
        if (err == ESP_OK) {
            readings[valid_count++] = cm;
        } else if (err == ESP_ERR_TIMEOUT) {
            timeout_count++;
        }

        if (i + 1 < samples) {
            vTaskDelay(pdMS_TO_TICKS(HC_SR05_SAMPLE_INTERVAL_MS));
        }
    }

    if (valid_count == 0) {
        snprintf(output, output_size,
                 "Error: HC-SR05 timed out on trig=GPIO%d echo=GPIO%d. Check 5V power, common GND, Trig/Echo wiring, and protect ESP32 Echo input with a divider/level shifter.",
                 trig_gpio, echo_gpio);
        ESP_LOGW(TAG, "HC-SR05 timeout trig=%d echo=%d samples=%d", trig_gpio, echo_gpio, samples);
        return ESP_ERR_TIMEOUT;
    }

    sort_float(readings, valid_count);
    float distance_cm = readings[valid_count / 2];
    bool present = distance_cm <= (float)threshold_cm;

    snprintf(output, output_size,
             "OK: HC-SR05 on trig=GPIO%d echo=GPIO%d -> present=%s distance_cm=%.1f threshold_cm=%d valid_samples=%d/%d%s",
             trig_gpio, echo_gpio,
             present ? "true" : "false",
             (double)distance_cm,
             threshold_cm,
             valid_count,
             samples,
             timeout_count > 0 ? " some_timeouts=true" : "");
    ESP_LOGI(TAG, "HC-SR05 trig=%d echo=%d distance=%.1fcm threshold=%d present=%d valid=%d/%d",
             trig_gpio, echo_gpio, (double)distance_cm, threshold_cm, present, valid_count, samples);
    return ESP_OK;
}

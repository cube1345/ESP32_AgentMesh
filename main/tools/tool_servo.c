#include "tools/tool_servo.h"

#include "espagent_config.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tool_servo";

static bool s_initialized = false;

/* Initialize LEDC PWM on GPIO5 for the servo.
 * Uses LEDC_TIMER_13_BIT for fine granularity at 50 Hz. */
static esp_err_t servo_ensure_ready(void)
{
    if (s_initialized) return ESP_OK;

    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = ESPAGENT_SERVO_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t chan_cfg = {
        .gpio_num = ESPAGENT_SERVO_DEFAULT_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    err = ledc_channel_config(&chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Servo PWM initialized on GPIO %d", ESPAGENT_SERVO_DEFAULT_GPIO);
    return ESP_OK;
}

/* Set servo to a specific pulse width in microseconds. */
esp_err_t tool_servo_set_pulse_us(int pulse_us)
{
    if (pulse_us < 0) pulse_us = 0;
    if (pulse_us > 20000) pulse_us = 20000;

    esp_err_t err = servo_ensure_ready();
    if (err != ESP_OK) return err;

    /* 13-bit resolution (8192 steps) at 50 Hz (20 ms period):
     * duty = pulse_us / 20000 * 8192 */
    uint32_t duty = ((uint32_t)pulse_us * 8191 + 10000) / 20000;
    err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC set duty failed on GPIO %d: %s",
                 ESPAGENT_SERVO_DEFAULT_GPIO, esp_err_to_name(err));
        return err;
    }

    err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC update duty failed on GPIO %d: %s",
                 ESPAGENT_SERVO_DEFAULT_GPIO, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Servo PWM updated on GPIO %d: pulse=%dus duty=%lu/8191",
             ESPAGENT_SERVO_DEFAULT_GPIO, pulse_us, (unsigned long)duty);
    return ESP_OK;
}

esp_err_t tool_servo_set_angle(int angle)
{
    if (angle < 0 || angle > 180) {
        return ESP_ERR_INVALID_ARG;
    }

    int pulse_us = ESPAGENT_SERVO_MIN_PULSE_US
                   + (angle * (ESPAGENT_SERVO_MAX_PULSE_US - ESPAGENT_SERVO_MIN_PULSE_US)) / 180;
    return tool_servo_set_pulse_us(pulse_us);
}

esp_err_t tool_servo_write_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    /* angle mode */
    cJSON *angle_item = cJSON_GetObjectItem(root, "angle");
    if (angle_item && cJSON_IsNumber(angle_item)) {
        int angle = angle_item->valueint;
        if (angle < 0 || angle > 180) {
            snprintf(output, output_size,
                     "Error: angle must be 0-180, got %d", angle);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        int pulse_us = ESPAGENT_SERVO_MIN_PULSE_US
                       + (angle * (ESPAGENT_SERVO_MAX_PULSE_US - ESPAGENT_SERVO_MIN_PULSE_US)) / 180;
        esp_err_t err = tool_servo_set_pulse_us(pulse_us);
        if (err == ESP_OK) {
            snprintf(output, output_size,
                     "OK: servo on GPIO%d set to %d degrees (pulse=%dus)", ESPAGENT_SERVO_DEFAULT_GPIO, angle, pulse_us);
        } else {
            snprintf(output, output_size,
                     "Error: servo write failed (%s)", esp_err_to_name(err));
        }

        ESP_LOGI(TAG, "servo_write angle=%d pulse=%dus -> %s",
                 angle, pulse_us, esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    /* pulse_us mode */
    cJSON *pulse_item = cJSON_GetObjectItem(root, "pulse_us");
    if (pulse_item && cJSON_IsNumber(pulse_item)) {
        int pulse_us = pulse_item->valueint;
        if (pulse_us < 0 || pulse_us > 20000) {
            snprintf(output, output_size,
                     "Error: pulse_us must be 0-20000, got %d", pulse_us);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }

        esp_err_t err = tool_servo_set_pulse_us(pulse_us);
        if (err == ESP_OK) {
            snprintf(output, output_size,
                     "OK: servo on GPIO%d set to %dus pulse width", ESPAGENT_SERVO_DEFAULT_GPIO, pulse_us);
        } else {
            snprintf(output, output_size,
                     "Error: servo write failed (%s)", esp_err_to_name(err));
        }

        ESP_LOGI(TAG, "servo_write pulse=%dus -> %s",
                 pulse_us, esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size,
             "Error: provide 'angle' (0-180) or 'pulse_us' (microseconds)");
    cJSON_Delete(root);
    return ESP_ERR_INVALID_ARG;
}

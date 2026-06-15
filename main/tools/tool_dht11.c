#include "tools/tool_dht11.h"

#include "espagent_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char *TAG = "tool_dht11";
static portMUX_TYPE s_dht11_mux = portMUX_INITIALIZER_UNLOCKED;

static int wait_level(int pin, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)pin) != level) {
        if ((esp_timer_get_time() - start) >= timeout_us) {
            return -1;
        }
    }
    return 0;
}

static esp_err_t dht11_read_raw(int pin, uint8_t data[5])
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_pullup_en((gpio_num_t)pin);
    gpio_pulldown_dis((gpio_num_t)pin);

    gpio_set_level((gpio_num_t)pin, 0);
    esp_rom_delay_us(20000);
    gpio_set_level((gpio_num_t)pin, 1);
    esp_rom_delay_us(40);

    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    gpio_pullup_en((gpio_num_t)pin);
    gpio_pulldown_dis((gpio_num_t)pin);

    portENTER_CRITICAL(&s_dht11_mux);

    if (wait_level(pin, 0, 1000) != 0) {
        portEXIT_CRITICAL(&s_dht11_mux);
        return ESP_ERR_TIMEOUT;
    }
    if (wait_level(pin, 1, 1000) != 0) {
        portEXIT_CRITICAL(&s_dht11_mux);
        return ESP_ERR_TIMEOUT;
    }
    if (wait_level(pin, 0, 1000) != 0) {
        portEXIT_CRITICAL(&s_dht11_mux);
        return ESP_ERR_TIMEOUT;
    }

    for (int byte = 0; byte < 5; byte++) {
        uint8_t value = 0;
        for (int bit = 0; bit < 8; bit++) {
            if (wait_level(pin, 1, 1000) != 0) {
                portEXIT_CRITICAL(&s_dht11_mux);
                return ESP_ERR_TIMEOUT;
            }

            int64_t t0 = esp_timer_get_time();
            if (wait_level(pin, 0, 1000) != 0) {
                portEXIT_CRITICAL(&s_dht11_mux);
                return ESP_ERR_TIMEOUT;
            }
            int high_us = (int)(esp_timer_get_time() - t0);
            value <<= 1;
            if (high_us > 50) {
                value |= 1;
            }
        }
        data[byte] = value;
    }

    portEXIT_CRITICAL(&s_dht11_mux);

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    return ESP_OK;
}

esp_err_t tool_read_temperature_humidity_execute(const char *input_json, char *output, size_t output_size)
{
    const int pin = ESPAGENT_DHT11_DEFAULT_GPIO;

    if (input_json && input_json[0] != '\0') {
        cJSON *root = cJSON_Parse(input_json);
        if (root) {
            cJSON_Delete(root);
        }
    }

    uint8_t data[5] = {0};
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = dht11_read_raw(pin, data);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    if (err != ESP_OK) {
        if (err == ESP_ERR_TIMEOUT) {
            snprintf(output, output_size,
                     "Error: DHT11 timed out on GPIO%d. Check DATA wiring, GND common ground, VCC, and pull-up.",
                     pin);
        } else {
            snprintf(output, output_size, "Error: failed to read DHT11 on GPIO%d (%s)", pin, esp_err_to_name(err));
        }
        ESP_LOGW(TAG, "DHT11 read failed on GPIO%d: %s", pin, esp_err_to_name(err));
        return err;
    }

    int humidity = data[0];
    int temperature = data[2];

    snprintf(output, output_size,
             "OK: DHT11 on GPIO%d -> temperature=%d C, humidity=%d%%",
             pin, temperature, humidity);
    ESP_LOGI(TAG, "DHT11 read on GPIO%d -> temp=%dC humidity=%d%%", pin, temperature, humidity);
    return ESP_OK;
}

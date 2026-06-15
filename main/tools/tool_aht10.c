#include "tools/tool_aht10.h"
#include "drivers/aht10.h"
#include "espagent_config.h"

#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_aht10";

static bool get_optional_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

esp_err_t tool_aht10_read_temperature_humidity_execute(const char *input_json,
                                                       char *output,
                                                       size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int sda_gpio = ESPAGENT_AHT10_DEFAULT_SDA_GPIO;
    int scl_gpio = ESPAGENT_AHT10_DEFAULT_SCL_GPIO;
    int i2c_port = ESPAGENT_AHT10_DEFAULT_I2C_PORT;
    int scl_hz = ESPAGENT_AHT10_DEFAULT_SCL_HZ;
    int address = ESPAGENT_AHT10_DEFAULT_ADDR;

    (void)get_optional_int(root, "sda_gpio", &sda_gpio);
    (void)get_optional_int(root, "scl_gpio", &scl_gpio);
    (void)get_optional_int(root, "i2c_port", &i2c_port);
    (void)get_optional_int(root, "scl_hz", &scl_hz);
    (void)get_optional_int(root, "address", &address);
    (void)get_optional_int(root, "addr", &address);

    if (!aht10_valid_gpio_pair(sda_gpio, scl_gpio)) {
        snprintf(output, output_size,
                 "Error: AHT10 SDA/SCL GPIOs are not configured or not output-capable. Set ESPAGENT_SECRET_AHT10_SDA_GPIO and ESPAGENT_SECRET_AHT10_SCL_GPIO, or call with {\"sda_gpio\":x,\"scl_gpio\":y}.");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    if (!aht10_valid_address(address)) {
        snprintf(output, output_size, "Error: AHT10 I2C address must be 0x38");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    aht10_dev_t dev;
    aht10_dev_init_default(&dev);

    aht10_config_t cfg = {
        .sda_gpio = sda_gpio,
        .scl_gpio = scl_gpio,
        .i2c_port = i2c_port,
        .scl_hz = scl_hz,
        .address = (uint8_t)address,
        .enable_internal_pullup = true,
    };

    esp_err_t err = aht10_init(&dev, &cfg);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            snprintf(output, output_size,
                     "Error: AHT10 not found at I2C address 0x%02x on SDA=%d SCL=%d",
                     address, sda_gpio, scl_gpio);
        } else if (err == ESP_ERR_INVALID_STATE) {
            snprintf(output, output_size,
                     "Error: AHT10 on SDA=%d SCL=%d addr=0x%02x did not report calibrated state",
                     sda_gpio, scl_gpio, address);
        } else {
            snprintf(output, output_size,
                     "Error: failed to initialize AHT10 on SDA=%d SCL=%d addr=0x%02x (%s)",
                     sda_gpio, scl_gpio, address, esp_err_to_name(err));
        }
        aht10_deinit(&dev);
        cJSON_Delete(root);
        return err;
    }

    aht10_reading_t reading = {0};
    err = aht10_read_temperature_humidity(&dev, &reading);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: failed to read AHT10 sample (%s)", esp_err_to_name(err));
        aht10_deinit(&dev);
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size,
             "OK: AHT10 on SDA=%d SCL=%d addr=0x%02x -> temperature=%.1f C, humidity=%.1f%%",
             sda_gpio, scl_gpio, address,
             (double)reading.temperature_c,
             (double)reading.humidity_percent);

    ESP_LOGI(TAG, "read_temperature_humidity sda=%d scl=%d port=%d addr=0x%02x freq=%d -> %s",
             sda_gpio, scl_gpio, i2c_port, address, scl_hz, output);
    aht10_deinit(&dev);
    cJSON_Delete(root);
    return ESP_OK;
}

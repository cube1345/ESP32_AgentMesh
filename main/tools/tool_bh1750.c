#include "tools/tool_bh1750.h"
#include "drivers/bh1750.h"
#include "espagent_config.h"

#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_bh1750";

static bool get_optional_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

esp_err_t tool_bh1750_read_light_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int sda_gpio = ESPAGENT_BH1750_DEFAULT_SDA_GPIO;
    int scl_gpio = ESPAGENT_BH1750_DEFAULT_SCL_GPIO;
    int i2c_port = ESPAGENT_BH1750_DEFAULT_I2C_PORT;
    int scl_hz = ESPAGENT_BH1750_DEFAULT_SCL_HZ;
    int address = ESPAGENT_BH1750_DEFAULT_ADDR;

    (void)get_optional_int(root, "sda_gpio", &sda_gpio);
    (void)get_optional_int(root, "scl_gpio", &scl_gpio);
    (void)get_optional_int(root, "i2c_port", &i2c_port);
    (void)get_optional_int(root, "scl_hz", &scl_hz);
    (void)get_optional_int(root, "address", &address);
    (void)get_optional_int(root, "addr", &address);

    if (!bh1750_valid_gpio_pair(sda_gpio, scl_gpio)) {
        snprintf(output, output_size,
                 "Error: GY-30/BH1750 SDA/SCL GPIOs are not configured or not output-capable. Set ESPAGENT_SECRET_BH1750_SDA_GPIO and ESPAGENT_SECRET_BH1750_SCL_GPIO, or call with {\"sda_gpio\":x,\"scl_gpio\":y}.");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    if (!bh1750_valid_address(address)) {
        snprintf(output, output_size,
                 "Error: GY-30/BH1750 address must be 0x23 or 0x5C");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    bh1750_dev_t dev;
    bh1750_dev_init_default(&dev);

    bh1750_config_t cfg = {
        .sda_gpio = sda_gpio,
        .scl_gpio = scl_gpio,
        .i2c_port = i2c_port,
        .scl_hz = scl_hz,
        .address = (uint8_t)address,
        .enable_internal_pullup = true,
    };

    esp_err_t err = bh1750_init(&dev, &cfg);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            snprintf(output, output_size,
                     "Error: GY-30/BH1750 not found at I2C address 0x%02x on SDA=%d SCL=%d. Try address=0x5C if ADDR is tied high.",
                     address, sda_gpio, scl_gpio);
        } else {
            snprintf(output, output_size,
                     "Error: failed to initialize GY-30/BH1750 on SDA=%d SCL=%d addr=0x%02x (%s)",
                     sda_gpio, scl_gpio, address, esp_err_to_name(err));
        }
        bh1750_deinit(&dev);
        cJSON_Delete(root);
        return err;
    }

    bh1750_reading_t reading = {0};
    err = bh1750_read_lux(&dev, &reading);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: failed to read GY-30/BH1750 sample (%s)", esp_err_to_name(err));
        bh1750_deinit(&dev);
        cJSON_Delete(root);
        return err;
    }

    snprintf(output, output_size,
             "OK: GY-30/BH1750 light on SDA=%d SCL=%d addr=0x%02x -> %.1f lux (raw=%u)",
             sda_gpio, scl_gpio, address, (double)reading.lux, (unsigned)reading.raw);

    ESP_LOGI(TAG, "read_light_level sda=%d scl=%d port=%d addr=0x%02x freq=%d -> %s",
             sda_gpio, scl_gpio, i2c_port, address, scl_hz, output);
    bh1750_deinit(&dev);
    cJSON_Delete(root);
    return ESP_OK;
}

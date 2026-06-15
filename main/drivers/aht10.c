#include "drivers/aht10.h"

#include <string.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aht10";

#define AHT10_CMD_INIT            0xE1
#define AHT10_CMD_TRIGGER         0xAC
#define AHT10_CMD_SOFT_RESET      0xBA
#define AHT10_INIT_ARG0           0x08
#define AHT10_INIT_ARG1           0x00
#define AHT10_TRIGGER_ARG0        0x33
#define AHT10_TRIGGER_ARG1        0x00
#define AHT10_STATUS_BUSY         0x80
#define AHT10_STATUS_CALIBRATED   0x08
#define AHT10_XFER_TIMEOUT_MS     100
#define AHT10_RESET_WAIT_MS       20
#define AHT10_INIT_WAIT_MS        40
#define AHT10_MEASURE_WAIT_MS     90
#define AHT10_MAX_BUSY_POLLS      3

bool aht10_valid_gpio_pair(int sda_gpio, int scl_gpio)
{
    return sda_gpio != scl_gpio &&
           GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)sda_gpio) &&
           GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)scl_gpio);
}

bool aht10_valid_address(int address)
{
    return address == AHT10_I2C_ADDRESS;
}

void aht10_dev_init_default(aht10_dev_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->sda_gpio = -1;
    dev->scl_gpio = -1;
    dev->i2c_port = -1;
    dev->address = AHT10_I2C_ADDRESS;
}

void aht10_deinit(aht10_dev_t *dev)
{
    if (dev->dev) {
        i2c_master_bus_rm_device(dev->dev);
        dev->dev = NULL;
    }
    if (dev->bus) {
        i2c_del_master_bus(dev->bus);
        dev->bus = NULL;
    }
    dev->sda_gpio = -1;
    dev->scl_gpio = -1;
    dev->i2c_port = -1;
    dev->scl_hz = 0;
    dev->address = AHT10_I2C_ADDRESS;
    dev->initialized = false;
}

static esp_err_t aht10_send_cmd(aht10_dev_t *dev, uint8_t cmd)
{
    return i2c_master_transmit(dev->dev, &cmd, sizeof(cmd), AHT10_XFER_TIMEOUT_MS);
}

static esp_err_t aht10_initialize_sensor(aht10_dev_t *dev)
{
    esp_err_t err = aht10_send_cmd(dev, AHT10_CMD_SOFT_RESET);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(AHT10_RESET_WAIT_MS));

    uint8_t init_cmd[3] = {
        AHT10_CMD_INIT,
        AHT10_INIT_ARG0,
        AHT10_INIT_ARG1,
    };
    err = i2c_master_transmit(dev->dev, init_cmd, sizeof(init_cmd), AHT10_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(AHT10_INIT_WAIT_MS));

    uint8_t status = 0;
    err = i2c_master_receive(dev->dev, &status, sizeof(status), AHT10_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if ((status & AHT10_STATUS_CALIBRATED) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    dev->initialized = true;
    return ESP_OK;
}

esp_err_t aht10_init(aht10_dev_t *dev, const aht10_config_t *cfg)
{
    if (!dev || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!aht10_valid_gpio_pair(cfg->sda_gpio, cfg->scl_gpio) ||
        !aht10_valid_address(cfg->address) ||
        cfg->scl_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->bus && dev->dev &&
        dev->sda_gpio == cfg->sda_gpio &&
        dev->scl_gpio == cfg->scl_gpio &&
        dev->i2c_port == cfg->i2c_port &&
        dev->scl_hz == cfg->scl_hz &&
        dev->address == cfg->address &&
        dev->initialized) {
        return ESP_OK;
    }

    if (dev->bus || dev->dev) {
        aht10_deinit(dev);
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = cfg->i2c_port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = cfg->enable_internal_pullup ? 1 : 0,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &dev->bus);
    if (err != ESP_OK) {
        aht10_deinit(dev);
        return err;
    }

    err = i2c_master_probe(dev->bus, cfg->address, AHT10_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        aht10_deinit(dev);
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = (uint32_t)cfg->scl_hz,
        .scl_wait_us = 0,
    };

    err = i2c_master_bus_add_device(dev->bus, &dev_cfg, &dev->dev);
    if (err != ESP_OK) {
        aht10_deinit(dev);
        return err;
    }

    dev->sda_gpio = cfg->sda_gpio;
    dev->scl_gpio = cfg->scl_gpio;
    dev->i2c_port = cfg->i2c_port;
    dev->scl_hz = cfg->scl_hz;
    dev->address = cfg->address;
    dev->initialized = false;

    err = aht10_initialize_sensor(dev);
    if (err != ESP_OK) {
        aht10_deinit(dev);
        return err;
    }

    ESP_LOGI(TAG, "initialized on SDA=%d SCL=%d port=%d addr=0x%02x freq=%d",
             cfg->sda_gpio, cfg->scl_gpio, cfg->i2c_port, cfg->address, cfg->scl_hz);
    return ESP_OK;
}

esp_err_t aht10_read_temperature_humidity(aht10_dev_t *dev, aht10_reading_t *reading)
{
    if (!dev || !reading || !dev->dev || !dev->initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t trigger_cmd[3] = {
        AHT10_CMD_TRIGGER,
        AHT10_TRIGGER_ARG0,
        AHT10_TRIGGER_ARG1,
    };
    esp_err_t err = i2c_master_transmit(dev->dev, trigger_cmd, sizeof(trigger_cmd), AHT10_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(AHT10_MEASURE_WAIT_MS));

    uint8_t data[6] = {0};
    for (int attempt = 0; attempt < AHT10_MAX_BUSY_POLLS; attempt++) {
        err = i2c_master_receive(dev->dev, data, sizeof(data), AHT10_XFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        if ((data[0] & AHT10_STATUS_BUSY) == 0) {
            break;
        }
        if (attempt == AHT10_MAX_BUSY_POLLS - 1) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint32_t raw_humidity = ((uint32_t)data[1] << 12) |
                            ((uint32_t)data[2] << 4) |
                            ((uint32_t)data[3] >> 4);
    uint32_t raw_temperature = (((uint32_t)data[3] & 0x0F) << 16) |
                               ((uint32_t)data[4] << 8) |
                               (uint32_t)data[5];

    reading->raw_humidity = raw_humidity;
    reading->raw_temperature = raw_temperature;
    reading->humidity_percent = ((float)raw_humidity * 100.0f) / 1048576.0f;
    reading->temperature_c = (((float)raw_temperature * 200.0f) / 1048576.0f) - 50.0f;
    return ESP_OK;
}

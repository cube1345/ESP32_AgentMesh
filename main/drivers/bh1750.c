#include "drivers/bh1750.h"

#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "bh1750";

#define BH1750_CMD_POWER_ON          0x01
#define BH1750_CMD_RESET             0x07
#define BH1750_CMD_ONE_TIME_H_RES    0x20
#define BH1750_CONVERSION_WAIT_MS    180
#define BH1750_XFER_TIMEOUT_MS       100

bool bh1750_valid_gpio_pair(int sda_gpio, int scl_gpio)
{
    return sda_gpio != scl_gpio &&
           GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)sda_gpio) &&
           GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)scl_gpio);
}

bool bh1750_valid_address(int address)
{
    return address == BH1750_I2C_ADDRESS_LOW || address == BH1750_I2C_ADDRESS_HIGH;
}

void bh1750_dev_init_default(bh1750_dev_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->sda_gpio = -1;
    dev->scl_gpio = -1;
    dev->i2c_port = -1;
    dev->address = BH1750_I2C_ADDRESS_LOW;
}

void bh1750_deinit(bh1750_dev_t *dev)
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
    dev->address = BH1750_I2C_ADDRESS_LOW;
}

esp_err_t bh1750_init(bh1750_dev_t *dev, const bh1750_config_t *cfg)
{
    if (!dev || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bh1750_valid_gpio_pair(cfg->sda_gpio, cfg->scl_gpio) ||
        !bh1750_valid_address(cfg->address) ||
        cfg->scl_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->bus && dev->dev &&
        dev->sda_gpio == cfg->sda_gpio &&
        dev->scl_gpio == cfg->scl_gpio &&
        dev->i2c_port == cfg->i2c_port &&
        dev->scl_hz == cfg->scl_hz &&
        dev->address == cfg->address) {
        return ESP_OK;
    }

    if (dev->bus || dev->dev) {
        bh1750_deinit(dev);
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
        bh1750_deinit(dev);
        return err;
    }

    err = i2c_master_probe(dev->bus, cfg->address, BH1750_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        bh1750_deinit(dev);
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
        bh1750_deinit(dev);
        return err;
    }

    uint8_t cmd = BH1750_CMD_POWER_ON;
    err = i2c_master_transmit(dev->dev, &cmd, sizeof(cmd), BH1750_XFER_TIMEOUT_MS);
    if (err == ESP_OK) {
        cmd = BH1750_CMD_RESET;
        err = i2c_master_transmit(dev->dev, &cmd, sizeof(cmd), BH1750_XFER_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        bh1750_deinit(dev);
        return err;
    }

    dev->sda_gpio = cfg->sda_gpio;
    dev->scl_gpio = cfg->scl_gpio;
    dev->i2c_port = cfg->i2c_port;
    dev->scl_hz = cfg->scl_hz;
    dev->address = cfg->address;

    ESP_LOGI(TAG, "initialized on SDA=%d SCL=%d port=%d addr=0x%02x freq=%d",
             cfg->sda_gpio, cfg->scl_gpio, cfg->i2c_port, cfg->address, cfg->scl_hz);
    return ESP_OK;
}

esp_err_t bh1750_read_lux(bh1750_dev_t *dev, bh1750_reading_t *reading)
{
    if (!dev || !reading || !dev->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t cmd = BH1750_CMD_ONE_TIME_H_RES;
    esp_err_t err = i2c_master_transmit(dev->dev, &cmd, sizeof(cmd), BH1750_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(BH1750_CONVERSION_WAIT_MS));

    uint8_t resp[2] = {0};
    err = i2c_master_receive(dev->dev, resp, sizeof(resp), BH1750_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    reading->raw = (uint16_t)((resp[0] << 8) | resp[1]);
    reading->lux = (float)reading->raw / 1.2f;
    return ESP_OK;
}

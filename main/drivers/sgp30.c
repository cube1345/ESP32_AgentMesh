#include "drivers/sgp30.h"

#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "sgp30";

#define SGP30_WORD_CRC_POLY        0x31
#define SGP30_WORD_CRC_INIT        0xFF
#define SGP30_XFER_TIMEOUT_MS      100
#define SGP30_CMD_IAQ_INIT         0x2003
#define SGP30_CMD_MEASURE_IAQ      0x2008
#define SGP30_MEASURE_WAIT_MS      20

static uint8_t sgp30_crc_word(const uint8_t *data)
{
    uint8_t crc = SGP30_WORD_CRC_INIT;
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ SGP30_WORD_CRC_POLY);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static esp_err_t sgp30_send_command(sgp30_dev_t *dev, uint16_t command)
{
    uint8_t cmd[2] = {
        (uint8_t)(command >> 8),
        (uint8_t)(command & 0xFF),
    };
    return i2c_master_transmit(dev->dev, cmd, sizeof(cmd), SGP30_XFER_TIMEOUT_MS);
}

static esp_err_t sgp30_measure_once(sgp30_dev_t *dev, uint16_t *co2eq_ppm, uint16_t *tvoc_ppb)
{
    uint8_t cmd[2] = {
        (uint8_t)(SGP30_CMD_MEASURE_IAQ >> 8),
        (uint8_t)(SGP30_CMD_MEASURE_IAQ & 0xFF),
    };
    uint8_t resp[6] = {0};

    esp_err_t err = i2c_master_transmit(dev->dev, cmd, sizeof(cmd), SGP30_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(SGP30_MEASURE_WAIT_MS));

    err = i2c_master_receive(dev->dev, resp, sizeof(resp), SGP30_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    if (sgp30_crc_word(&resp[0]) != resp[2] || sgp30_crc_word(&resp[3]) != resp[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    *co2eq_ppm = (uint16_t)((resp[0] << 8) | resp[1]);
    *tvoc_ppb = (uint16_t)((resp[3] << 8) | resp[4]);
    return ESP_OK;
}

bool sgp30_valid_gpio_pair(int sda_gpio, int scl_gpio)
{
    return GPIO_IS_VALID_GPIO(sda_gpio) && GPIO_IS_VALID_GPIO(scl_gpio);
}

void sgp30_dev_init_default(sgp30_dev_t *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->sda_gpio = -1;
    dev->scl_gpio = -1;
    dev->i2c_port = -1;
}

void sgp30_deinit(sgp30_dev_t *dev)
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
    dev->init_us = 0;
    dev->last_measure_us = 0;
    dev->last_co2eq_ppm = 0;
    dev->last_tvoc_ppb = 0;
    dev->has_sample = false;
}

esp_err_t sgp30_init(sgp30_dev_t *dev, const sgp30_config_t *cfg)
{
    if (!dev || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!sgp30_valid_gpio_pair(cfg->sda_gpio, cfg->scl_gpio) || cfg->scl_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (dev->bus && dev->dev &&
        dev->sda_gpio == cfg->sda_gpio &&
        dev->scl_gpio == cfg->scl_gpio &&
        dev->i2c_port == cfg->i2c_port &&
        dev->scl_hz == cfg->scl_hz) {
        return ESP_OK;
    }

    if (dev->bus || dev->dev) {
        sgp30_deinit(dev);
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
        sgp30_deinit(dev);
        return err;
    }

    err = i2c_master_probe(dev->bus, SGP30_I2C_ADDRESS, SGP30_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        sgp30_deinit(dev);
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SGP30_I2C_ADDRESS,
        .scl_speed_hz = (uint32_t)cfg->scl_hz,
        .scl_wait_us = 0,
    };

    err = i2c_master_bus_add_device(dev->bus, &dev_cfg, &dev->dev);
    if (err != ESP_OK) {
        sgp30_deinit(dev);
        return err;
    }

    err = sgp30_send_command(dev, SGP30_CMD_IAQ_INIT);
    if (err != ESP_OK) {
        sgp30_deinit(dev);
        return err;
    }

    dev->sda_gpio = cfg->sda_gpio;
    dev->scl_gpio = cfg->scl_gpio;
    dev->i2c_port = cfg->i2c_port;
    dev->scl_hz = cfg->scl_hz;
    dev->init_us = esp_timer_get_time();
    dev->last_measure_us = 0;
    dev->last_co2eq_ppm = 0;
    dev->last_tvoc_ppb = 0;
    dev->has_sample = false;

    ESP_LOGI(TAG, "initialized on SDA=%d SCL=%d port=%d freq=%d",
             cfg->sda_gpio, cfg->scl_gpio, cfg->i2c_port, cfg->scl_hz);
    return ESP_OK;
}

esp_err_t sgp30_read_air_quality(sgp30_dev_t *dev, sgp30_reading_t *reading)
{
    if (!dev || !reading || !dev->dev) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now_us = esp_timer_get_time();
    bool cached = false;

    if (dev->has_sample && (now_us - dev->last_measure_us) < SGP30_MEASURE_INTERVAL_US) {
        cached = true;
    } else {
        if (!dev->has_sample) {
            int64_t first_measure_wait_us = SGP30_MEASURE_INTERVAL_US - (now_us - dev->init_us);
            if (first_measure_wait_us > 0) {
                vTaskDelay(pdMS_TO_TICKS((first_measure_wait_us + 999) / 1000));
                now_us = esp_timer_get_time();
            }
        }

        uint16_t co2eq_ppm = 0;
        uint16_t tvoc_ppb = 0;
        esp_err_t err = sgp30_measure_once(dev, &co2eq_ppm, &tvoc_ppb);
        if (err != ESP_OK) {
            return err;
        }

        dev->last_co2eq_ppm = co2eq_ppm;
        dev->last_tvoc_ppb = tvoc_ppb;
        dev->last_measure_us = now_us;
        dev->has_sample = true;
    }

    int64_t warmup_remaining_us = SGP30_WARMUP_US - (now_us - dev->init_us);
    reading->co2eq_ppm = dev->last_co2eq_ppm;
    reading->tvoc_ppb = dev->last_tvoc_ppb;
    reading->warming_up = warmup_remaining_us > 0;
    reading->cached = cached;
    reading->warmup_remaining_us = warmup_remaining_us > 0 ? warmup_remaining_us : 0;
    return ESP_OK;
}

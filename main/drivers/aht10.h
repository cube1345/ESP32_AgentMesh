#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#define AHT10_I2C_ADDRESS 0x38

typedef struct {
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int scl_hz;
    uint8_t address;
    bool enable_internal_pullup;
} aht10_config_t;

typedef struct {
    float temperature_c;
    float humidity_percent;
    uint32_t raw_temperature;
    uint32_t raw_humidity;
} aht10_reading_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int scl_hz;
    uint8_t address;
    bool initialized;
} aht10_dev_t;

bool aht10_valid_gpio_pair(int sda_gpio, int scl_gpio);
bool aht10_valid_address(int address);
void aht10_dev_init_default(aht10_dev_t *dev);
esp_err_t aht10_init(aht10_dev_t *dev, const aht10_config_t *cfg);
void aht10_deinit(aht10_dev_t *dev);
esp_err_t aht10_read_temperature_humidity(aht10_dev_t *dev, aht10_reading_t *reading);

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#define BH1750_I2C_ADDRESS_LOW      0x23
#define BH1750_I2C_ADDRESS_HIGH     0x5C

typedef struct {
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int scl_hz;
    uint8_t address;
    bool enable_internal_pullup;
} bh1750_config_t;

typedef struct {
    float lux;
    uint16_t raw;
} bh1750_reading_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int scl_hz;
    uint8_t address;
} bh1750_dev_t;

bool bh1750_valid_gpio_pair(int sda_gpio, int scl_gpio);
bool bh1750_valid_address(int address);
void bh1750_dev_init_default(bh1750_dev_t *dev);
esp_err_t bh1750_init(bh1750_dev_t *dev, const bh1750_config_t *cfg);
void bh1750_deinit(bh1750_dev_t *dev);
esp_err_t bh1750_read_lux(bh1750_dev_t *dev, bh1750_reading_t *reading);

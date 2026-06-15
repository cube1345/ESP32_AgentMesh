#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#define SGP30_I2C_ADDRESS          0x58
#define SGP30_WARMUP_US            (15LL * 1000 * 1000)
#define SGP30_MEASURE_INTERVAL_US  (1LL * 1000 * 1000)

typedef struct {
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int scl_hz;
    bool enable_internal_pullup;
} sgp30_config_t;

typedef struct {
    uint16_t co2eq_ppm;
    uint16_t tvoc_ppb;
    bool warming_up;
    bool cached;
    int64_t warmup_remaining_us;
} sgp30_reading_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int scl_hz;
    int64_t init_us;
    int64_t last_measure_us;
    uint16_t last_co2eq_ppm;
    uint16_t last_tvoc_ppb;
    bool has_sample;
} sgp30_dev_t;

bool sgp30_valid_gpio_pair(int sda_gpio, int scl_gpio);
void sgp30_dev_init_default(sgp30_dev_t *dev);
esp_err_t sgp30_init(sgp30_dev_t *dev, const sgp30_config_t *cfg);
void sgp30_deinit(sgp30_dev_t *dev);
esp_err_t sgp30_read_air_quality(sgp30_dev_t *dev, sgp30_reading_t *reading);

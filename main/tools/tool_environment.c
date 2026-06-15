#include "tools/tool_environment.h"

#include "espagent_config.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "tool_environment";

#define ENV_AHT20_ADDR              0x38
#define ENV_SGP30_ADDR              0x58
#define ENV_BH1750_ADDR_LOW         0x23
#define ENV_BH1750_ADDR_HIGH        0x5C

#define ENV_I2C_TIMEOUT_MS          100
#define ENV_AHT20_RESET_WAIT_MS     20
#define ENV_AHT20_INIT_WAIT_MS      20
#define ENV_AHT20_MEASURE_WAIT_MS   90
#define ENV_BH1750_WAIT_MS          180
#define ENV_SGP30_MEASURE_WAIT_MS   20
#define ENV_SGP30_INTERVAL_US       (1000LL * 1000)
#define ENV_SGP30_WARMUP_US         (15LL * 1000 * 1000)
#define ENV_SOFT_I2C_DELAY_US       5

#define AHT20_CMD_SOFT_RESET        0xBA
#define AHT20_CMD_INIT              0xBE
#define AHT20_CMD_TRIGGER           0xAC
#define AHT20_STATUS_BUSY           0x80
#define AHT20_STATUS_CALIBRATED     0x08

#define SGP30_CMD_IAQ_INIT          0x2003
#define SGP30_CMD_MEASURE_IAQ       0x2008
#define SGP30_CRC_POLY              0x31
#define SGP30_CRC_INIT              0xFF

#define BH1750_CMD_POWER_ON         0x01
#define BH1750_CMD_RESET            0x07
#define BH1750_CMD_ONE_TIME_H_RES   0x20

typedef struct {
    int sda_gpio;
    int scl_gpio;
    int delay_us;
} soft_i2c_bus_t;

typedef struct {
    i2c_master_bus_handle_t aht_bus;
    i2c_master_dev_handle_t aht20;
    i2c_master_bus_handle_t sgp_bus;
    i2c_master_dev_handle_t sgp30;
    soft_i2c_bus_t gy30_soft;
    int aht_sda_gpio;
    int aht_scl_gpio;
    int aht_i2c_port;
    int aht_scl_hz;
    int sgp_sda_gpio;
    int sgp_scl_gpio;
    int sgp_i2c_port;
    int sgp_scl_hz;
    int gy30_sda_gpio;
    int gy30_scl_gpio;
    int gy30_addr;
    int64_t sgp30_init_us;
    int64_t sgp30_last_measure_us;
    uint16_t sgp30_last_co2eq_ppm;
    uint16_t sgp30_last_tvoc_ppb;
    bool sgp30_has_sample;
    bool gy30_ready;
} env_state_t;

typedef struct {
    float temperature_c;
    float humidity_percent;
} env_aht20_reading_t;

typedef struct {
    uint16_t co2eq_ppm;
    uint16_t tvoc_ppb;
    bool warming_up;
    bool cached;
    int64_t warmup_remaining_us;
} env_sgp30_reading_t;

static env_state_t s_env = {
    .aht_sda_gpio = -1,
    .aht_scl_gpio = -1,
    .aht_i2c_port = -1,
    .sgp_sda_gpio = -1,
    .sgp_scl_gpio = -1,
    .sgp_i2c_port = -1,
    .gy30_sda_gpio = -1,
    .gy30_scl_gpio = -1,
};
static SemaphoreHandle_t s_env_mutex;

static bool get_optional_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valueint;
    return true;
}

static uint8_t sgp30_crc_word(const uint8_t *data)
{
    uint8_t crc = SGP30_CRC_INIT;
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ SGP30_CRC_POLY) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t hw_send_u16(i2c_master_dev_handle_t dev, uint16_t value)
{
    uint8_t cmd[2] = {
        (uint8_t)(value >> 8),
        (uint8_t)(value & 0xFF),
    };
    return i2c_master_transmit(dev, cmd, sizeof(cmd), ENV_I2C_TIMEOUT_MS);
}

static bool valid_i2c_gpio_pair(int sda_gpio, int scl_gpio)
{
    return sda_gpio != scl_gpio &&
           GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)sda_gpio) &&
           GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)scl_gpio);
}

static void soft_i2c_delay(const soft_i2c_bus_t *bus)
{
    esp_rom_delay_us((uint32_t)bus->delay_us);
}

static void soft_i2c_set_sda(const soft_i2c_bus_t *bus, int level)
{
    gpio_set_level((gpio_num_t)bus->sda_gpio, level ? 1 : 0);
    soft_i2c_delay(bus);
}

static void soft_i2c_set_scl(const soft_i2c_bus_t *bus, int level)
{
    gpio_set_level((gpio_num_t)bus->scl_gpio, level ? 1 : 0);
    soft_i2c_delay(bus);
}

static int soft_i2c_read_sda(const soft_i2c_bus_t *bus)
{
    soft_i2c_delay(bus);
    return gpio_get_level((gpio_num_t)bus->sda_gpio);
}

static esp_err_t soft_i2c_init(soft_i2c_bus_t *bus, int sda_gpio, int scl_gpio)
{
    if (!valid_i2c_gpio_pair(sda_gpio, scl_gpio)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << sda_gpio) | (1ULL << scl_gpio),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    bus->sda_gpio = sda_gpio;
    bus->scl_gpio = scl_gpio;
    bus->delay_us = ENV_SOFT_I2C_DELAY_US;
    soft_i2c_set_sda(bus, 1);
    soft_i2c_set_scl(bus, 1);
    return ESP_OK;
}

static void soft_i2c_start(const soft_i2c_bus_t *bus)
{
    soft_i2c_set_sda(bus, 1);
    soft_i2c_set_scl(bus, 1);
    soft_i2c_set_sda(bus, 0);
    soft_i2c_set_scl(bus, 0);
}

static void soft_i2c_stop(const soft_i2c_bus_t *bus)
{
    soft_i2c_set_sda(bus, 0);
    soft_i2c_set_scl(bus, 1);
    soft_i2c_set_sda(bus, 1);
}

static esp_err_t soft_i2c_write_byte(const soft_i2c_bus_t *bus, uint8_t value)
{
    for (int bit = 7; bit >= 0; bit--) {
        soft_i2c_set_sda(bus, (value >> bit) & 0x01);
        soft_i2c_set_scl(bus, 1);
        soft_i2c_set_scl(bus, 0);
    }

    soft_i2c_set_sda(bus, 1);
    soft_i2c_set_scl(bus, 1);
    int ack = soft_i2c_read_sda(bus) == 0;
    soft_i2c_set_scl(bus, 0);
    return ack ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static uint8_t soft_i2c_read_byte(const soft_i2c_bus_t *bus, bool ack)
{
    uint8_t value = 0;
    soft_i2c_set_sda(bus, 1);

    for (int bit = 7; bit >= 0; bit--) {
        soft_i2c_set_scl(bus, 1);
        if (soft_i2c_read_sda(bus)) {
            value |= (uint8_t)(1U << bit);
        }
        soft_i2c_set_scl(bus, 0);
    }

    soft_i2c_set_sda(bus, ack ? 0 : 1);
    soft_i2c_set_scl(bus, 1);
    soft_i2c_set_scl(bus, 0);
    soft_i2c_set_sda(bus, 1);
    return value;
}

static esp_err_t soft_i2c_write_cmd(const soft_i2c_bus_t *bus, uint8_t addr, uint8_t cmd)
{
    soft_i2c_start(bus);
    esp_err_t err = soft_i2c_write_byte(bus, (uint8_t)(addr << 1));
    if (err == ESP_OK) {
        err = soft_i2c_write_byte(bus, cmd);
    }
    soft_i2c_stop(bus);
    return err;
}

static esp_err_t soft_i2c_read_two(const soft_i2c_bus_t *bus, uint8_t addr, uint8_t out[2])
{
    soft_i2c_start(bus);
    esp_err_t err = soft_i2c_write_byte(bus, (uint8_t)((addr << 1) | 0x01));
    if (err == ESP_OK) {
        out[0] = soft_i2c_read_byte(bus, true);
        out[1] = soft_i2c_read_byte(bus, false);
    }
    soft_i2c_stop(bus);
    return err;
}

static void env_deinit_aht20(void)
{
    if (s_env.aht20) {
        i2c_master_bus_rm_device(s_env.aht20);
        s_env.aht20 = NULL;
    }
    if (s_env.aht_bus) {
        i2c_del_master_bus(s_env.aht_bus);
        s_env.aht_bus = NULL;
    }
    s_env.aht_sda_gpio = -1;
    s_env.aht_scl_gpio = -1;
    s_env.aht_i2c_port = -1;
}

static void env_deinit_sgp30(void)
{
    if (s_env.sgp30) {
        i2c_master_bus_rm_device(s_env.sgp30);
        s_env.sgp30 = NULL;
    }
    if (s_env.sgp_bus) {
        i2c_del_master_bus(s_env.sgp_bus);
        s_env.sgp_bus = NULL;
    }
    s_env.sgp_sda_gpio = -1;
    s_env.sgp_scl_gpio = -1;
    s_env.sgp_i2c_port = -1;
    s_env.sgp30_has_sample = false;
}

static void env_deinit_gy30(void)
{
    s_env.gy30_sda_gpio = -1;
    s_env.gy30_scl_gpio = -1;
    s_env.gy30_ready = false;
}

static void env_deinit(void)
{
    if (s_env.aht20 || s_env.aht_bus) {
        env_deinit_aht20();
    }
    if (s_env.sgp30 || s_env.sgp_bus) {
        env_deinit_sgp30();
    }
    if (s_env.gy30_ready) {
        env_deinit_gy30();
    }
}

static esp_err_t add_hw_device(i2c_master_bus_handle_t bus, uint8_t addr,
                               int scl_hz, i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = (uint32_t)scl_hz,
        .scl_wait_us = 0,
    };
    return i2c_master_bus_add_device(bus, &cfg, dev);
}

static esp_err_t create_hw_bus(int sda_gpio, int scl_gpio, int i2c_port,
                               int scl_hz, uint8_t addr,
                               i2c_master_bus_handle_t *bus,
                               i2c_master_dev_handle_t *dev)
{
    if (!valid_i2c_gpio_pair(sda_gpio, scl_gpio) || scl_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = i2c_port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 1,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, bus);
    if (err != ESP_OK) {
        return err;
    }
    err = i2c_master_probe(*bus, addr, ENV_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return add_hw_device(*bus, addr, scl_hz, dev);
}

static esp_err_t env_aht20_init(void)
{
    uint8_t reset = AHT20_CMD_SOFT_RESET;
    esp_err_t err = i2c_master_transmit(s_env.aht20, &reset, sizeof(reset), ENV_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ENV_AHT20_RESET_WAIT_MS));

    uint8_t init_cmd[3] = {AHT20_CMD_INIT, 0x08, 0x00};
    err = i2c_master_transmit(s_env.aht20, init_cmd, sizeof(init_cmd), ENV_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ENV_AHT20_INIT_WAIT_MS));

    uint8_t status = 0;
    err = i2c_master_receive(s_env.aht20, &status, sizeof(status), ENV_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    return (status & AHT20_STATUS_CALIBRATED) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t env_sgp30_init(void)
{
    esp_err_t err = hw_send_u16(s_env.sgp30, SGP30_CMD_IAQ_INIT);
    if (err != ESP_OK) {
        return err;
    }
    s_env.sgp30_init_us = esp_timer_get_time();
    s_env.sgp30_last_measure_us = 0;
    s_env.sgp30_last_co2eq_ppm = 0;
    s_env.sgp30_last_tvoc_ppb = 0;
    s_env.sgp30_has_sample = false;
    return ESP_OK;
}

static esp_err_t env_bh1750_init(void)
{
    esp_err_t err = soft_i2c_write_cmd(&s_env.gy30_soft, (uint8_t)s_env.gy30_addr, BH1750_CMD_POWER_ON);
    if (err == ESP_OK) {
        err = soft_i2c_write_cmd(&s_env.gy30_soft, (uint8_t)s_env.gy30_addr, BH1750_CMD_RESET);
    }
    return err;
}

static esp_err_t env_ensure_ready(int aht_sda, int aht_scl, int aht_port, int aht_hz,
                                  int sgp_sda, int sgp_scl, int sgp_port, int sgp_hz,
                                  int gy30_sda, int gy30_scl, int gy30_addr,
                                  char *output, size_t output_size)
{
    if (gy30_addr != ENV_BH1750_ADDR_LOW && gy30_addr != ENV_BH1750_ADDR_HIGH) {
        snprintf(output, output_size, "Error: GY-30/BH1750 address must be 0x23 or 0x5C");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_env.aht_bus && s_env.aht20 &&
        s_env.sgp_bus && s_env.sgp30 &&
        s_env.gy30_ready &&
        s_env.aht_sda_gpio == aht_sda &&
        s_env.aht_scl_gpio == aht_scl &&
        s_env.aht_i2c_port == aht_port &&
        s_env.aht_scl_hz == aht_hz &&
        s_env.sgp_sda_gpio == sgp_sda &&
        s_env.sgp_scl_gpio == sgp_scl &&
        s_env.sgp_i2c_port == sgp_port &&
        s_env.sgp_scl_hz == sgp_hz &&
        s_env.gy30_sda_gpio == gy30_sda &&
        s_env.gy30_scl_gpio == gy30_scl &&
        s_env.gy30_addr == gy30_addr) {
        return ESP_OK;
    }

    env_deinit();

    esp_err_t err = create_hw_bus(aht_sda, aht_scl, aht_port, aht_hz,
                                  ENV_AHT20_ADDR, &s_env.aht_bus, &s_env.aht20);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: AHT20 hardware I2C failed on SDA=%d SCL=%d port=%d (%s)",
                 aht_sda, aht_scl, aht_port, esp_err_to_name(err));
        env_deinit();
        return err;
    }
    err = env_aht20_init();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to initialize AHT20 at 0x38 (%s)", esp_err_to_name(err));
        env_deinit();
        return err;
    }

    err = create_hw_bus(sgp_sda, sgp_scl, sgp_port, sgp_hz,
                        ENV_SGP30_ADDR, &s_env.sgp_bus, &s_env.sgp30);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: SGP30 hardware I2C failed on SDA=%d SCL=%d port=%d (%s)",
                 sgp_sda, sgp_scl, sgp_port, esp_err_to_name(err));
        env_deinit();
        return err;
    }
    err = env_sgp30_init();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to initialize SGP30 at 0x58 (%s)", esp_err_to_name(err));
        env_deinit();
        return err;
    }

    err = soft_i2c_init(&s_env.gy30_soft, gy30_sda, gy30_scl);
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: GY-30 software I2C GPIO config failed SDA=%d SCL=%d (%s)",
                 gy30_sda, gy30_scl, esp_err_to_name(err));
        env_deinit();
        return err;
    }
    s_env.gy30_addr = gy30_addr;
    err = env_bh1750_init();
    if (err != ESP_OK) {
        snprintf(output, output_size,
                 "Error: GY-30/BH1750 software I2C failed at 0x%02x SDA=%d SCL=%d (%s)",
                 gy30_addr, gy30_sda, gy30_scl, esp_err_to_name(err));
        env_deinit();
        return err;
    }

    s_env.aht_sda_gpio = aht_sda;
    s_env.aht_scl_gpio = aht_scl;
    s_env.aht_i2c_port = aht_port;
    s_env.aht_scl_hz = aht_hz;
    s_env.sgp_sda_gpio = sgp_sda;
    s_env.sgp_scl_gpio = sgp_scl;
    s_env.sgp_i2c_port = sgp_port;
    s_env.sgp_scl_hz = sgp_hz;
    s_env.gy30_sda_gpio = gy30_sda;
    s_env.gy30_scl_gpio = gy30_scl;
    s_env.gy30_ready = true;

    ESP_LOGI(TAG,
             "environment buses ready: AHT20 HW%d SDA=%d SCL=%d; SGP30 HW%d SDA=%d SCL=%d; GY-30 soft SDA=%d SCL=%d",
             aht_port, aht_sda, aht_scl, sgp_port, sgp_sda, sgp_scl, gy30_sda, gy30_scl);
    return ESP_OK;
}

static esp_err_t env_ensure_aht20_ready(int sda_gpio, int scl_gpio, int i2c_port, int scl_hz)
{
    if (s_env.aht_bus && s_env.aht20 &&
        s_env.aht_sda_gpio == sda_gpio &&
        s_env.aht_scl_gpio == scl_gpio &&
        s_env.aht_i2c_port == i2c_port &&
        s_env.aht_scl_hz == scl_hz) {
        return ESP_OK;
    }

    env_deinit_aht20();

    esp_err_t err = create_hw_bus(sda_gpio, scl_gpio, i2c_port, scl_hz,
                                  ENV_AHT20_ADDR, &s_env.aht_bus, &s_env.aht20);
    if (err == ESP_OK) {
        err = env_aht20_init();
    }
    if (err != ESP_OK) {
        env_deinit_aht20();
        return err;
    }

    s_env.aht_sda_gpio = sda_gpio;
    s_env.aht_scl_gpio = scl_gpio;
    s_env.aht_i2c_port = i2c_port;
    s_env.aht_scl_hz = scl_hz;
    return ESP_OK;
}

static esp_err_t env_ensure_sgp30_ready(int sda_gpio, int scl_gpio, int i2c_port, int scl_hz)
{
    if (s_env.sgp_bus && s_env.sgp30 &&
        s_env.sgp_sda_gpio == sda_gpio &&
        s_env.sgp_scl_gpio == scl_gpio &&
        s_env.sgp_i2c_port == i2c_port &&
        s_env.sgp_scl_hz == scl_hz) {
        return ESP_OK;
    }

    env_deinit_sgp30();

    esp_err_t err = create_hw_bus(sda_gpio, scl_gpio, i2c_port, scl_hz,
                                  ENV_SGP30_ADDR, &s_env.sgp_bus, &s_env.sgp30);
    if (err == ESP_OK) {
        err = env_sgp30_init();
    }
    if (err != ESP_OK) {
        env_deinit_sgp30();
        return err;
    }

    s_env.sgp_sda_gpio = sda_gpio;
    s_env.sgp_scl_gpio = scl_gpio;
    s_env.sgp_i2c_port = i2c_port;
    s_env.sgp_scl_hz = scl_hz;
    return ESP_OK;
}

static esp_err_t env_ensure_gy30_ready(int sda_gpio, int scl_gpio, int addr)
{
    if (addr != ENV_BH1750_ADDR_LOW && addr != ENV_BH1750_ADDR_HIGH) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_env.gy30_ready &&
        s_env.gy30_sda_gpio == sda_gpio &&
        s_env.gy30_scl_gpio == scl_gpio &&
        s_env.gy30_addr == addr) {
        return ESP_OK;
    }

    env_deinit_gy30();

    esp_err_t err = soft_i2c_init(&s_env.gy30_soft, sda_gpio, scl_gpio);
    if (err == ESP_OK) {
        s_env.gy30_addr = addr;
        err = env_bh1750_init();
    }
    if (err != ESP_OK) {
        env_deinit_gy30();
        return err;
    }

    s_env.gy30_sda_gpio = sda_gpio;
    s_env.gy30_scl_gpio = scl_gpio;
    s_env.gy30_ready = true;
    return ESP_OK;
}

static esp_err_t env_read_aht20(env_aht20_reading_t *reading)
{
    uint8_t trigger[3] = {AHT20_CMD_TRIGGER, 0x33, 0x00};
    esp_err_t err = i2c_master_transmit(s_env.aht20, trigger, sizeof(trigger), ENV_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ENV_AHT20_MEASURE_WAIT_MS));

    uint8_t data[7] = {0};
    err = i2c_master_receive(s_env.aht20, data, sizeof(data), ENV_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    if (data[0] & AHT20_STATUS_BUSY) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t raw_humidity = ((uint32_t)data[1] << 12) |
                            ((uint32_t)data[2] << 4) |
                            ((uint32_t)data[3] >> 4);
    uint32_t raw_temperature = (((uint32_t)data[3] & 0x0F) << 16) |
                               ((uint32_t)data[4] << 8) |
                               (uint32_t)data[5];

    reading->humidity_percent = ((float)raw_humidity * 100.0f) / 1048576.0f;
    reading->temperature_c = (((float)raw_temperature * 200.0f) / 1048576.0f) - 50.0f;
    return ESP_OK;
}

static esp_err_t env_read_sgp30(env_sgp30_reading_t *reading)
{
    int64_t now_us = esp_timer_get_time();
    bool cached = false;

    if (s_env.sgp30_has_sample && (now_us - s_env.sgp30_last_measure_us) < ENV_SGP30_INTERVAL_US) {
        cached = true;
    } else {
        if (!s_env.sgp30_has_sample) {
            int64_t wait_us = ENV_SGP30_INTERVAL_US - (now_us - s_env.sgp30_init_us);
            if (wait_us > 0) {
                vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));
                now_us = esp_timer_get_time();
            }
        }

        esp_err_t err = hw_send_u16(s_env.sgp30, SGP30_CMD_MEASURE_IAQ);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(ENV_SGP30_MEASURE_WAIT_MS));

        uint8_t resp[6] = {0};
        err = i2c_master_receive(s_env.sgp30, resp, sizeof(resp), ENV_I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
        if (sgp30_crc_word(&resp[0]) != resp[2] || sgp30_crc_word(&resp[3]) != resp[5]) {
            return ESP_ERR_INVALID_CRC;
        }

        s_env.sgp30_last_co2eq_ppm = (uint16_t)((resp[0] << 8) | resp[1]);
        s_env.sgp30_last_tvoc_ppb = (uint16_t)((resp[3] << 8) | resp[4]);
        s_env.sgp30_last_measure_us = now_us;
        s_env.sgp30_has_sample = true;
    }

    int64_t warmup_remaining_us = ENV_SGP30_WARMUP_US - (now_us - s_env.sgp30_init_us);
    reading->co2eq_ppm = s_env.sgp30_last_co2eq_ppm;
    reading->tvoc_ppb = s_env.sgp30_last_tvoc_ppb;
    reading->warming_up = warmup_remaining_us > 0;
    reading->cached = cached;
    reading->warmup_remaining_us = warmup_remaining_us > 0 ? warmup_remaining_us : 0;
    return ESP_OK;
}

static esp_err_t env_read_bh1750(float *lux, uint16_t *raw)
{
    esp_err_t err = soft_i2c_write_cmd(&s_env.gy30_soft, (uint8_t)s_env.gy30_addr, BH1750_CMD_ONE_TIME_H_RES);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ENV_BH1750_WAIT_MS));

    uint8_t resp[2] = {0};
    err = soft_i2c_read_two(&s_env.gy30_soft, (uint8_t)s_env.gy30_addr, resp);
    if (err != ESP_OK) {
        return err;
    }

    *raw = (uint16_t)((resp[0] << 8) | resp[1]);
    *lux = (float)(*raw) / 1.2f;
    return ESP_OK;
}

esp_err_t tool_read_environment_execute(const char *input_json, char *output, size_t output_size)
{
    if (!s_env_mutex) {
        s_env_mutex = xSemaphoreCreateMutex();
        if (!s_env_mutex) {
            snprintf(output, output_size, "Error: failed to create environment sensor mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int aht_sda = ESPAGENT_AHT10_DEFAULT_SDA_GPIO;
    int aht_scl = ESPAGENT_AHT10_DEFAULT_SCL_GPIO;
    int aht_port = ESPAGENT_AHT10_DEFAULT_I2C_PORT;
    int aht_hz = ESPAGENT_AHT10_DEFAULT_SCL_HZ;
    int sgp_sda = ESPAGENT_SGP30_DEFAULT_SDA_GPIO;
    int sgp_scl = ESPAGENT_SGP30_DEFAULT_SCL_GPIO;
    int sgp_port = ESPAGENT_SGP30_DEFAULT_I2C_PORT;
    int sgp_hz = ESPAGENT_SGP30_DEFAULT_SCL_HZ;
    int gy30_sda = ESPAGENT_BH1750_DEFAULT_SDA_GPIO;
    int gy30_scl = ESPAGENT_BH1750_DEFAULT_SCL_GPIO;
    int gy30_addr = ESPAGENT_BH1750_DEFAULT_ADDR;

    (void)get_optional_int(root, "aht_sda_gpio", &aht_sda);
    (void)get_optional_int(root, "aht_scl_gpio", &aht_scl);
    (void)get_optional_int(root, "aht_i2c_port", &aht_port);
    (void)get_optional_int(root, "aht_scl_hz", &aht_hz);
    (void)get_optional_int(root, "sgp30_sda_gpio", &sgp_sda);
    (void)get_optional_int(root, "sgp30_scl_gpio", &sgp_scl);
    (void)get_optional_int(root, "sgp30_i2c_port", &sgp_port);
    (void)get_optional_int(root, "sgp30_scl_hz", &sgp_hz);
    (void)get_optional_int(root, "gy30_sda_gpio", &gy30_sda);
    (void)get_optional_int(root, "gy30_scl_gpio", &gy30_scl);
    (void)get_optional_int(root, "gy30_addr", &gy30_addr);
    (void)get_optional_int(root, "bh1750_addr", &gy30_addr);
    cJSON_Delete(root);

    if (xSemaphoreTake(s_env_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        snprintf(output, output_size, "Error: environment sensor buses are busy");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = env_ensure_ready(aht_sda, aht_scl, aht_port, aht_hz,
                                     sgp_sda, sgp_scl, sgp_port, sgp_hz,
                                     gy30_sda, gy30_scl, gy30_addr,
                                     output, output_size);
    if (err != ESP_OK) {
        xSemaphoreGive(s_env_mutex);
        return err;
    }

    env_aht20_reading_t aht = {0};
    env_sgp30_reading_t sgp = {0};
    float lux = 0.0f;
    uint16_t lux_raw = 0;

    err = env_read_aht20(&aht);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to read AHT20 sample (%s)", esp_err_to_name(err));
        xSemaphoreGive(s_env_mutex);
        return err;
    }

    err = env_read_sgp30(&sgp);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to read SGP30 sample (%s)", esp_err_to_name(err));
        xSemaphoreGive(s_env_mutex);
        return err;
    }

    err = env_read_bh1750(&lux, &lux_raw);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to read GY-30/BH1750 software I2C sample (%s)",
                 esp_err_to_name(err));
        xSemaphoreGive(s_env_mutex);
        return err;
    }

    snprintf(output, output_size,
             "OK: env 3-I2C -> AHT20[HW%d SDA=%d SCL=%d] temperature=%.1f C humidity=%.1f%%; SGP30[HW%d SDA=%d SCL=%d] CO2eq=%u ppm TVOC=%u ppb%s; GY-30[SW SDA=%d SCL=%d] light=%.1f lux raw=%u",
             aht_port, aht_sda, aht_scl,
             (double)aht.temperature_c,
             (double)aht.humidity_percent,
             sgp_port, sgp_sda, sgp_scl,
             (unsigned)sgp.co2eq_ppm,
             (unsigned)sgp.tvoc_ppb,
             sgp.warming_up ? " warming_up=true" : "",
             gy30_sda, gy30_scl,
             (double)lux,
             (unsigned)lux_raw);

    ESP_LOGI(TAG, "read_environment -> %s", output);
    xSemaphoreGive(s_env_mutex);
    return ESP_OK;
}

static int env_float_x10(float value)
{
    return (int)((value * 10.0f) + (value >= 0.0f ? 0.5f : -0.5f));
}

esp_err_t tool_environment_read_values(tool_environment_values_t *values,
                                       char *status,
                                       size_t status_size)
{
    if (!values) {
        if (status && status_size) {
            snprintf(status, status_size, "Error: values output is NULL");
        }
        return ESP_ERR_INVALID_ARG;
    }

    *values = (tool_environment_values_t) {
        .temperature_c_x10 = -1,
        .humidity_percent_x10 = -1,
        .co2eq_ppm = -1,
        .tvoc_ppb = -1,
        .light_lux_x10 = -1,
        .light_raw = -1,
        .sgp30_warming_up = false,
    };

    if (!s_env_mutex) {
        s_env_mutex = xSemaphoreCreateMutex();
        if (!s_env_mutex) {
            if (status && status_size) {
                snprintf(status, status_size, "Error: failed to create environment sensor mutex");
            }
            return ESP_ERR_NO_MEM;
        }
    }

    if (xSemaphoreTake(s_env_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        if (status && status_size) {
            snprintf(status, status_size, "Error: environment sensor buses are busy");
        }
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t aht_err = env_ensure_aht20_ready(ESPAGENT_AHT10_DEFAULT_SDA_GPIO,
                                                ESPAGENT_AHT10_DEFAULT_SCL_GPIO,
                                                ESPAGENT_AHT10_DEFAULT_I2C_PORT,
                                                ESPAGENT_AHT10_DEFAULT_SCL_HZ);
    if (aht_err == ESP_OK) {
        env_aht20_reading_t aht = {0};
        aht_err = env_read_aht20(&aht);
        if (aht_err == ESP_OK) {
            values->temperature_c_x10 = env_float_x10(aht.temperature_c);
            values->humidity_percent_x10 = env_float_x10(aht.humidity_percent);
        } else {
            env_deinit_aht20();
        }
    }

    esp_err_t sgp_err = env_ensure_sgp30_ready(ESPAGENT_SGP30_DEFAULT_SDA_GPIO,
                                                ESPAGENT_SGP30_DEFAULT_SCL_GPIO,
                                                ESPAGENT_SGP30_DEFAULT_I2C_PORT,
                                                ESPAGENT_SGP30_DEFAULT_SCL_HZ);
    if (sgp_err == ESP_OK) {
        env_sgp30_reading_t sgp = {0};
        sgp_err = env_read_sgp30(&sgp);
        if (sgp_err == ESP_OK) {
            values->co2eq_ppm = (int)sgp.co2eq_ppm;
            values->tvoc_ppb = (int)sgp.tvoc_ppb;
            values->sgp30_warming_up = sgp.warming_up;
        } else {
            env_deinit_sgp30();
        }
    }

    esp_err_t gy30_err = env_ensure_gy30_ready(ESPAGENT_BH1750_DEFAULT_SDA_GPIO,
                                                ESPAGENT_BH1750_DEFAULT_SCL_GPIO,
                                                ESPAGENT_BH1750_DEFAULT_ADDR);
    if (gy30_err == ESP_OK) {
        float lux = 0.0f;
        uint16_t raw = 0;
        gy30_err = env_read_bh1750(&lux, &raw);
        if (gy30_err == ESP_OK) {
            values->light_lux_x10 = env_float_x10(lux);
            values->light_raw = (int)raw;
        } else {
            env_deinit_gy30();
        }
    }

    if (status && status_size) {
        snprintf(status, status_size,
                 "aht=%s sgp30=%s gy30=%s",
                 esp_err_to_name(aht_err),
                 esp_err_to_name(sgp_err),
                 esp_err_to_name(gy30_err));
    }

    ESP_LOGI(TAG,
             "env values -> temp_x10=%d hum_x10=%d co2=%d tvoc=%d lux_x10=%d raw=%d status=%s",
             values->temperature_c_x10,
             values->humidity_percent_x10,
             values->co2eq_ppm,
             values->tvoc_ppb,
             values->light_lux_x10,
             values->light_raw,
             status ? status : "");

    xSemaphoreGive(s_env_mutex);
    return ESP_OK;
}

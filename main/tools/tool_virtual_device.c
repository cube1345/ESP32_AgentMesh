#include "tools/tool_virtual_device.h"

#include "espagent_config.h"
#include "roles/role_config.h"
#include "tools/gpio_policy.h"
#include "tools/tool_mesh_command.h"

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "tool_virtual_device";

#define DEVICE_NAME_MAX 48
#define MANIFEST_MAX_SIZE 4096
#define I2C_OP_MAX 8
#define I2C_BYTES_MAX 32
#define I2C_TIMEOUT_DEFAULT_MS 100
#define I2C_DELAY_MAX_MS 2000
#define UART_BYTES_MAX 128
#define UART_COMMAND_MAX 64
#define UART_TIMEOUT_DEFAULT_MS 500
#define UART_DELAY_MAX_MS 2000
#define MODBUS_REGISTERS_MAX 16
#define MODBUS_TIMEOUT_DEFAULT_MS 800
#define SPI_BYTES_MAX 64
#define SPI_TIMEOUT_DEFAULT_MS 1000
#define ADC_SAMPLES_MAX 32
#define VIRTUAL_CONTROL_MAX_DURATION_MS 30000
#define VIRTUAL_CONTROL_DEFAULT_MAX_DURATION_MS 5000
#define VIRTUAL_CONTROL_DEFAULT_COOLDOWN_MS 1000
#define VIRTUAL_CONTROL_COOLDOWN_SLOTS 8
#define VIRTUAL_DEVICE_MANIFEST_VERSION 1
#define AHT20_STATUS_BUSY 0x80

typedef struct {
    char device[DEVICE_NAME_MAX];
    char field[32];
    char unit[16];
    char decode_type[24];
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int scl_hz;
    int address;
    int timeout_ms;
    double scale;
    double offset;
    bool internal_pullup;
} virtual_i2c_device_t;

typedef struct {
    char device[DEVICE_NAME_MAX];
    int tx_gpio;
    int rx_gpio;
    int uart_port;
    int baud;
    int timeout_ms;
    int read_length;
    int post_write_delay_ms;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
} virtual_uart_device_t;

typedef struct {
    virtual_uart_device_t uart;
    int de_re_gpio;
    int slave_id;
    int function_code;
    int register_address;
    int register_count;
    char field[32];
    char unit[16];
    double scale;
    double offset;
} virtual_modbus_device_t;

typedef struct {
    char device[DEVICE_NAME_MAX];
    int mosi_gpio;
    int miso_gpio;
    int sclk_gpio;
    int cs_gpio;
    int host_id;
    int mode;
    int frequency_hz;
    int read_length;
    int dummy_bytes;
    int timeout_ms;
} virtual_spi_device_t;

typedef struct {
    char device[DEVICE_NAME_MAX];
    int unit;
    int channel;
    int atten_db;
    int samples;
    char field[32];
    char unit_name[16];
    double scale;
    double offset;
} virtual_adc_device_t;

typedef struct {
    char device[DEVICE_NAME_MAX];
    int pin;
    bool pullup;
    bool pulldown;
    bool invert;
    char field[32];
} virtual_gpio_input_t;

typedef struct {
    char device[DEVICE_NAME_MAX];
    char protocol[24];
    int pin;
    int active_level;
    int safe_level;
    int default_level;
    int max_duration_ms;
    int cooldown_ms;
    int frequency_hz;
    int duty_pct;
    int ledc_channel;
    int ledc_timer;
    bool invert;
    bool high_impact;
} virtual_control_device_t;

typedef struct {
    char device[DEVICE_NAME_MAX];
    int64_t last_us;
} virtual_control_cooldown_t;

typedef struct {
    bool pwm;
    int pin;
    int safe_level;
    int ledc_channel;
    int duration_ms;
    char device[DEVICE_NAME_MAX];
} virtual_control_restore_t;

static virtual_control_cooldown_t s_control_cooldowns[VIRTUAL_CONTROL_COOLDOWN_SLOTS];

static bool valid_device_name(const char *name)
{
    if (!name || !name[0] || strlen(name) >= DEVICE_NAME_MAX) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) {
            return false;
        }
    }
    return true;
}

static const char *json_string(cJSON *root, const char *key)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_int(cJSON *root, const char *key, int fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static double json_double(cJSON *root, const char *key, double fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool json_bool(cJSON *root, const char *key, bool fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    if (!item) {
        return fallback;
    }
    return cJSON_IsTrue(item);
}

static int parse_address(cJSON *root, const char *key, int fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        return (int)strtol(item->valuestring, NULL, 0);
    }
    return fallback;
}

static esp_err_t read_manifest(const char *device,
                               char *buf,
                               size_t buf_size,
                               char *path,
                               size_t path_size)
{
    if (!valid_device_name(device)) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(path, path_size, ESPAGENT_SPIFFS_BASE "/devices/%s.json", device);

    FILE *f = fopen(path, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t n = fread(buf, 1, buf_size - 1, f);
    bool too_large = !feof(f);
    fclose(f);
    buf[n] = '\0';
    return too_large ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_size)
{
    static const char hex[] = "0123456789abcdef";
    if (!out || out_size < len * 2 + 1) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hex[(bytes[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

static esp_err_t verify_manifest_sha256(const char *device,
                                        const char *manifest_buf,
                                        bool required,
                                        char *reason,
                                        size_t reason_size)
{
    char sig_path[128] = {0};
    snprintf(sig_path, sizeof(sig_path), ESPAGENT_SPIFFS_BASE "/devices/%s.json.sha256", device);
    FILE *f = fopen(sig_path, "r");
    if (!f) {
        if (required) {
            snprintf(reason, reason_size, "missing required manifest signature sidecar at %s", sig_path);
            return ESP_ERR_NOT_FOUND;
        }
        snprintf(reason, reason_size, "signature sidecar not present");
        return ESP_OK;
    }

    char expected[80] = {0};
    size_t n = fread(expected, 1, sizeof(expected) - 1, f);
    fclose(f);
    expected[n] = '\0';
    char normalized[65] = {0};
    size_t pos = 0;
    for (size_t i = 0; expected[i] && pos < 64; i++) {
        if (isxdigit((unsigned char)expected[i])) {
            normalized[pos++] = (char)tolower((unsigned char)expected[i]);
        }
    }
    if (pos != 64) {
        snprintf(reason, reason_size, "manifest signature sidecar must contain 64 hex chars");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[32] = {0};
    size_t digest_len = 0;
    psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS) {
        status = psa_hash_compute(PSA_ALG_SHA_256,
                                  (const uint8_t *)manifest_buf,
                                  strlen(manifest_buf),
                                  digest,
                                  sizeof(digest),
                                  &digest_len);
    }
    if (status != PSA_SUCCESS || digest_len != sizeof(digest)) {
        snprintf(reason, reason_size, "sha256 calculation failed status=%ld len=%u",
                 (long)status, (unsigned)digest_len);
        return ESP_FAIL;
    }
    char actual[65] = {0};
    bytes_to_hex(digest, sizeof(digest), actual, sizeof(actual));
    if (strcmp(actual, normalized) != 0) {
        snprintf(reason, reason_size, "manifest sha256 mismatch expected=%s actual=%s", normalized, actual);
        return ESP_ERR_INVALID_CRC;
    }
    snprintf(reason, reason_size, "manifest sha256 verified");
    return ESP_OK;
}

static bool json_string_array_contains(cJSON *arr, const char *value)
{
    if (!cJSON_IsArray(arr) || !value) {
        return false;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && item->valuestring &&
            strcmp(item->valuestring, value) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t validate_manifest_header(cJSON *manifest,
                                          const char *device,
                                          const char *expected_role,
                                          const char *required_permission,
                                          char *reason,
                                          size_t reason_size)
{
    const char *name = json_string(manifest, "name");
    const char *role = json_string(manifest, "role");
    int version = json_int(manifest, "manifest_version", -1);
    cJSON *permissions = cJSON_GetObjectItem(manifest, "permissions");

    if (!name || strcmp(name, device) != 0) {
        snprintf(reason, reason_size, "manifest name must match requested device");
        return ESP_ERR_INVALID_ARG;
    }
    if (version != VIRTUAL_DEVICE_MANIFEST_VERSION) {
        snprintf(reason, reason_size,
                 "manifest_version must be %d", VIRTUAL_DEVICE_MANIFEST_VERSION);
        return ESP_ERR_INVALID_VERSION;
    }
    if (!role || strcmp(role, expected_role) != 0) {
        snprintf(reason, reason_size, "manifest role must be %s", expected_role);
        return ESP_ERR_INVALID_ARG;
    }
    if (!json_string_array_contains(permissions, required_permission)) {
        snprintf(reason, reason_size,
                 "manifest permissions must include %s", required_permission);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void virtual_control_restore_task(void *arg)
{
    virtual_control_restore_t *restore = (virtual_control_restore_t *)arg;
    if (!restore) {
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(restore->duration_ms));
    if (restore->pwm) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)restore->ledc_channel, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)restore->ledc_channel);
        ESP_LOGI(TAG, "virtual_device_control restored PWM device=%s channel=%d to duty=0",
                 restore->device, restore->ledc_channel);
    } else {
        gpio_set_level((gpio_num_t)restore->pin, restore->safe_level);
        ESP_LOGI(TAG, "virtual_device_control restored GPIO device=%s pin=%d safe_level=%d",
                 restore->device, restore->pin, restore->safe_level);
    }
    free(restore);
    vTaskDelete(NULL);
}

static esp_err_t schedule_control_restore(const virtual_control_device_t *cfg,
                                          bool pwm,
                                          int duration_ms,
                                          char *reason,
                                          size_t reason_size)
{
    if (duration_ms <= 0) {
        return ESP_OK;
    }

    virtual_control_restore_t *restore = calloc(1, sizeof(*restore));
    if (!restore) {
        snprintf(reason, reason_size, "out of memory scheduling safe restore");
        return ESP_ERR_NO_MEM;
    }
    restore->pwm = pwm;
    restore->pin = cfg->pin;
    restore->safe_level = cfg->safe_level;
    restore->ledc_channel = cfg->ledc_channel;
    restore->duration_ms = duration_ms;
    snprintf(restore->device, sizeof(restore->device), "%s", cfg->device);

    BaseType_t ok = xTaskCreate(virtual_control_restore_task,
                                "vdev_restore",
                                2048,
                                restore,
                                tskIDLE_PRIORITY + 1,
                                NULL);
    if (ok != pdPASS) {
        free(restore);
        snprintf(reason, reason_size, "failed to create safe restore task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t parse_i2c_manifest(cJSON *root,
                                    const char *device,
                                    virtual_i2c_device_t *cfg,
                                    char *reason,
                                    size_t reason_size)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->device, sizeof(cfg->device), "%s", device);
    snprintf(cfg->field, sizeof(cfg->field), "value");
    snprintf(cfg->unit, sizeof(cfg->unit), "raw");
    snprintf(cfg->decode_type, sizeof(cfg->decode_type), "raw_u16_be");
    cfg->i2c_port = 0;
    cfg->scl_hz = 100000;
    cfg->address = -1;
    cfg->timeout_ms = I2C_TIMEOUT_DEFAULT_MS;
    cfg->scale = 1.0;
    cfg->offset = 0.0;
    cfg->internal_pullup = true;

    const char *protocol = json_string(root, "protocol");
    const char *risk = json_string(root, "risk");
    if (!protocol || strcmp(protocol, "i2c") != 0) {
        snprintf(reason, reason_size, "manifest protocol must be i2c");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (risk && strcmp(risk, "read_only") != 0) {
        snprintf(reason, reason_size, "manifest risk must be read_only for virtual_device_read");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pins = cJSON_GetObjectItem(root, "pins");
    cfg->sda_gpio = json_int(pins, "sda", -1);
    cfg->scl_gpio = json_int(pins, "scl", -1);
    cfg->i2c_port = json_int(root, "i2c_port", json_int(root, "port", cfg->i2c_port));
    cfg->scl_hz = json_int(root, "scl_hz", json_int(root, "frequency_hz", cfg->scl_hz));
    cfg->address = parse_address(root, "address", -1);
    cfg->timeout_ms = json_int(root, "timeout_ms", cfg->timeout_ms);
    cfg->internal_pullup = json_bool(root, "internal_pullup", true);

    cJSON *decode = cJSON_GetObjectItem(root, "decode");
    const char *type = json_string(decode, "type");
    const char *field = json_string(decode, "field");
    const char *unit = json_string(decode, "unit");
    if (type) {
        snprintf(cfg->decode_type, sizeof(cfg->decode_type), "%s", type);
    }
    if (field) {
        snprintf(cfg->field, sizeof(cfg->field), "%s", field);
    }
    if (unit) {
        snprintf(cfg->unit, sizeof(cfg->unit), "%s", unit);
    }
    cfg->scale = json_double(decode, "scale", cfg->scale);
    cfg->offset = json_double(decode, "offset", cfg->offset);

    if (cfg->sda_gpio == cfg->scl_gpio ||
        !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->sda_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->scl_gpio)) {
        snprintf(reason, reason_size, "invalid I2C SDA/SCL pins");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->i2c_port < 0 || cfg->i2c_port > 1) {
        snprintf(reason, reason_size, "i2c_port must be 0 or 1");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->scl_hz <= 0 || cfg->scl_hz > 400000) {
        snprintf(reason, reason_size, "scl_hz must be 1..400000");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->address < 0x03 || cfg->address > 0x77) {
        snprintf(reason, reason_size, "I2C address must be a 7-bit user address");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->timeout_ms <= 0 || cfg->timeout_ms > 1000) {
        snprintf(reason, reason_size, "timeout_ms must be 1..1000");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t parse_bytes(cJSON *arr, uint8_t *bytes, size_t *len)
{
    if (!cJSON_IsArray(arr)) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = cJSON_GetArraySize(arr);
    if (n <= 0 || n > I2C_BYTES_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 255) {
            return ESP_ERR_INVALID_ARG;
        }
        bytes[i] = (uint8_t)item->valueint;
    }
    *len = (size_t)n;
    return ESP_OK;
}

static esp_err_t parse_bytes_limited(cJSON *arr, uint8_t *bytes, size_t *len, size_t max_len)
{
    if (!cJSON_IsArray(arr)) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = cJSON_GetArraySize(arr);
    if (n <= 0 || (size_t)n > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsNumber(item) || item->valueint < 0 || item->valueint > 255) {
            return ESP_ERR_INVALID_ARG;
        }
        bytes[i] = (uint8_t)item->valueint;
    }
    *len = (size_t)n;
    return ESP_OK;
}

static esp_err_t decode_reading(const virtual_i2c_device_t *cfg,
                                const uint8_t *data,
                                size_t len,
                                char *decoded,
                                size_t decoded_size)
{
    if (strcmp(cfg->decode_type, "aht20_temp_humidity") == 0) {
        if (len < 6) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (data[0] & AHT20_STATUS_BUSY) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        uint32_t raw_humidity = ((uint32_t)data[1] << 12) |
                                ((uint32_t)data[2] << 4) |
                                ((uint32_t)data[3] >> 4);
        uint32_t raw_temperature = (((uint32_t)data[3] & 0x0f) << 16) |
                                   ((uint32_t)data[4] << 8) |
                                   (uint32_t)data[5];
        double humidity_percent = ((double)raw_humidity * 100.0) / 1048576.0;
        double temperature_c = (((double)raw_temperature * 200.0) / 1048576.0) - 50.0;
        snprintf(decoded, decoded_size,
                 "temperature=%.2f C humidity=%.2f %%RH raw_h=%lu raw_t=%lu status=0x%02x",
                 temperature_c,
                 humidity_percent,
                 (unsigned long)raw_humidity,
                 (unsigned long)raw_temperature,
                 (unsigned)data[0]);
        return ESP_OK;
    }

    uint32_t raw = 0;
    if (strcmp(cfg->decode_type, "raw_u8") == 0) {
        if (len < 1) {
            return ESP_ERR_INVALID_SIZE;
        }
        raw = data[0];
    } else if (strcmp(cfg->decode_type, "raw_u16_le") == 0) {
        if (len < 2) {
            return ESP_ERR_INVALID_SIZE;
        }
        raw = ((uint32_t)data[1] << 8) | data[0];
    } else {
        if (len < 2) {
            return ESP_ERR_INVALID_SIZE;
        }
        raw = ((uint32_t)data[0] << 8) | data[1];
    }

    double value = (double)raw * cfg->scale + cfg->offset;
    snprintf(decoded, decoded_size, "%s=%.3f %s raw=%lu",
             cfg->field, value, cfg->unit, (unsigned long)raw);
    return ESP_OK;
}

static esp_err_t execute_i2c_manifest(cJSON *manifest,
                                      const virtual_i2c_device_t *cfg,
                                      char *output,
                                      size_t output_size)
{
    cJSON *operations = cJSON_GetObjectItem(manifest, "operations");
    if (!cJSON_IsArray(operations)) {
        snprintf(output, output_size, "Error: manifest missing operations array");
        return ESP_ERR_INVALID_ARG;
    }
    int op_count = cJSON_GetArraySize(operations);
    if (op_count <= 0 || op_count > I2C_OP_MAX) {
        snprintf(output, output_size, "Error: operations count must be 1..%d", I2C_OP_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = cfg->i2c_port,
        .sda_io_num = cfg->sda_gpio,
        .scl_io_num = cfg->scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = cfg->internal_pullup ? 1 : 0,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: create I2C bus SDA=%d SCL=%d port=%d failed (%s)",
                 cfg->sda_gpio, cfg->scl_gpio, cfg->i2c_port, esp_err_to_name(err));
        return err;
    }

    err = i2c_master_probe(bus, cfg->address, cfg->timeout_ms);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: device=%s not found at I2C addr=0x%02x SDA=%d SCL=%d (%s)",
                 cfg->device, cfg->address, cfg->sda_gpio, cfg->scl_gpio, esp_err_to_name(err));
        i2c_del_master_bus(bus);
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->address,
        .scl_speed_hz = (uint32_t)cfg->scl_hz,
        .scl_wait_us = 0,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: add I2C device=0x%02x failed (%s)",
                 cfg->address, esp_err_to_name(err));
        i2c_del_master_bus(bus);
        return err;
    }

    uint8_t last_read[I2C_BYTES_MAX] = {0};
    size_t last_read_len = 0;
    for (int i = 0; i < op_count; i++) {
        cJSON *op = cJSON_GetArrayItem(operations, i);
        const char *type = json_string(op, "type");
        if (!type) {
            err = ESP_ERR_INVALID_ARG;
            snprintf(output, output_size, "Error: operation %d missing type", i + 1);
            break;
        }
        if (strcmp(type, "write") == 0) {
            uint8_t bytes[I2C_BYTES_MAX] = {0};
            size_t len = 0;
            err = parse_bytes(cJSON_GetObjectItem(op, "bytes"), bytes, &len);
            if (err != ESP_OK) {
                snprintf(output, output_size, "Error: operation %d invalid write bytes", i + 1);
                break;
            }
            err = i2c_master_transmit(dev, bytes, len, cfg->timeout_ms);
            if (err != ESP_OK) {
                snprintf(output, output_size, "Error: I2C write op %d failed (%s)", i + 1, esp_err_to_name(err));
                break;
            }
        } else if (strcmp(type, "delay_ms") == 0) {
            int delay_ms = json_int(op, "value", json_int(op, "delay_ms", 0));
            if (delay_ms < 0 || delay_ms > I2C_DELAY_MAX_MS) {
                err = ESP_ERR_INVALID_ARG;
                snprintf(output, output_size, "Error: operation %d delay_ms must be 0..%d", i + 1, I2C_DELAY_MAX_MS);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        } else if (strcmp(type, "read") == 0) {
            int len = json_int(op, "length", 0);
            if (len <= 0 || len > I2C_BYTES_MAX) {
                err = ESP_ERR_INVALID_SIZE;
                snprintf(output, output_size, "Error: operation %d read length must be 1..%d", i + 1, I2C_BYTES_MAX);
                break;
            }
            err = i2c_master_receive(dev, last_read, (size_t)len, cfg->timeout_ms);
            if (err != ESP_OK) {
                snprintf(output, output_size, "Error: I2C read op %d failed (%s)", i + 1, esp_err_to_name(err));
                break;
            }
            last_read_len = (size_t)len;
        } else {
            err = ESP_ERR_NOT_SUPPORTED;
            snprintf(output, output_size, "Error: operation %d unsupported type=%s", i + 1, type);
            break;
        }
    }

    if (err == ESP_OK && last_read_len == 0) {
        err = ESP_ERR_INVALID_RESPONSE;
        snprintf(output, output_size, "Error: manifest executed but no read operation produced data");
    }
    if (err == ESP_OK) {
        char decoded[160] = {0};
        err = decode_reading(cfg, last_read, last_read_len, decoded, sizeof(decoded));
        if (err == ESP_OK) {
            snprintf(output, output_size,
                     "OK: virtual_device_read device=%s protocol=i2c addr=0x%02x SDA=%d SCL=%d -> %s",
                     cfg->device, cfg->address, cfg->sda_gpio, cfg->scl_gpio, decoded);
        } else {
            snprintf(output, output_size, "Error: decode failed for device=%s (%s)",
                     cfg->device, esp_err_to_name(err));
        }
    }

    i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
    ESP_LOGI(TAG, "virtual_device_read status=%s result=%s", esp_err_to_name(err), output);
    return err;
}

static uart_parity_t parse_uart_parity(const char *value)
{
    if (value && strcmp(value, "even") == 0) {
        return UART_PARITY_EVEN;
    }
    if (value && strcmp(value, "odd") == 0) {
        return UART_PARITY_ODD;
    }
    return UART_PARITY_DISABLE;
}

static uart_stop_bits_t parse_uart_stop_bits(int value)
{
    if (value == 2) {
        return UART_STOP_BITS_2;
    }
    return UART_STOP_BITS_1;
}

static uart_word_length_t parse_uart_data_bits(int value)
{
    switch (value) {
    case 5: return UART_DATA_5_BITS;
    case 6: return UART_DATA_6_BITS;
    case 7: return UART_DATA_7_BITS;
    default: return UART_DATA_8_BITS;
    }
}

static esp_err_t parse_uart_manifest(cJSON *root,
                                     const char *device,
                                     virtual_uart_device_t *cfg,
                                     char *reason,
                                     size_t reason_size)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->device, sizeof(cfg->device), "%s", device);
    cfg->tx_gpio = -1;
    cfg->rx_gpio = -1;
    cfg->uart_port = 1;
    cfg->baud = 9600;
    cfg->timeout_ms = UART_TIMEOUT_DEFAULT_MS;
    cfg->read_length = 64;
    cfg->post_write_delay_ms = 50;
    cfg->data_bits = UART_DATA_8_BITS;
    cfg->parity = UART_PARITY_DISABLE;
    cfg->stop_bits = UART_STOP_BITS_1;

    const char *protocol = json_string(root, "protocol");
    const char *risk = json_string(root, "risk");
    if (!protocol || strcmp(protocol, "uart") != 0) {
        snprintf(reason, reason_size, "manifest protocol must be uart");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (risk && strcmp(risk, "read_only") != 0) {
        snprintf(reason, reason_size, "manifest risk must be read_only for virtual_device_read");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pins = cJSON_GetObjectItem(root, "pins");
    cfg->tx_gpio = json_int(pins, "tx", -1);
    cfg->rx_gpio = json_int(pins, "rx", -1);
    cfg->uart_port = json_int(root, "uart_port", json_int(root, "port", cfg->uart_port));
    cfg->baud = json_int(root, "baud", cfg->baud);
    cfg->timeout_ms = json_int(root, "timeout_ms", cfg->timeout_ms);
    cfg->read_length = json_int(root, "read_length", cfg->read_length);
    cfg->post_write_delay_ms = json_int(root, "post_write_delay_ms", cfg->post_write_delay_ms);
    cfg->data_bits = parse_uart_data_bits(json_int(root, "data_bits", 8));
    cfg->stop_bits = parse_uart_stop_bits(json_int(root, "stop_bits", 1));
    cfg->parity = parse_uart_parity(json_string(root, "parity"));

    if (cfg->uart_port <= 0 || cfg->uart_port >= UART_NUM_MAX) {
        snprintf(reason, reason_size, "uart_port must be 1..%d; UART0 is reserved for console", UART_NUM_MAX - 1);
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->tx_gpio) ||
        !GPIO_IS_VALID_GPIO((gpio_num_t)cfg->rx_gpio) ||
        cfg->tx_gpio == cfg->rx_gpio) {
        snprintf(reason, reason_size, "invalid UART tx/rx pins");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->baud < 1200 || cfg->baud > 921600) {
        snprintf(reason, reason_size, "baud must be 1200..921600");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->timeout_ms <= 0 || cfg->timeout_ms > 3000) {
        snprintf(reason, reason_size, "timeout_ms must be 1..3000");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->read_length <= 0 || cfg->read_length > UART_BYTES_MAX) {
        snprintf(reason, reason_size, "read_length must be 1..%d", UART_BYTES_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->post_write_delay_ms < 0 || cfg->post_write_delay_ms > UART_DELAY_MAX_MS) {
        snprintf(reason, reason_size, "post_write_delay_ms must be 0..%d", UART_DELAY_MAX_MS);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t parse_modbus_manifest(cJSON *root,
                                       const char *device,
                                       virtual_modbus_device_t *cfg,
                                       char *reason,
                                       size_t reason_size)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->uart.device, sizeof(cfg->uart.device), "%s", device);
    cfg->uart.tx_gpio = -1;
    cfg->uart.rx_gpio = -1;
    cfg->uart.uart_port = 1;
    cfg->uart.baud = 9600;
    cfg->uart.timeout_ms = MODBUS_TIMEOUT_DEFAULT_MS;
    cfg->uart.read_length = 64;
    cfg->uart.post_write_delay_ms = 20;
    cfg->uart.data_bits = UART_DATA_8_BITS;
    cfg->uart.parity = UART_PARITY_DISABLE;
    cfg->uart.stop_bits = UART_STOP_BITS_1;
    cfg->de_re_gpio = -1;
    cfg->slave_id = 1;
    cfg->function_code = 4;
    cfg->register_address = -1;
    cfg->register_count = 1;
    cfg->scale = 1.0;
    cfg->offset = 0.0;
    snprintf(cfg->field, sizeof(cfg->field), "register");
    snprintf(cfg->unit, sizeof(cfg->unit), "raw");

    const char *protocol = json_string(root, "protocol");
    const char *risk = json_string(root, "risk");
    if (!protocol || strcmp(protocol, "modbus_rtu") != 0) {
        snprintf(reason, reason_size, "manifest protocol must be modbus_rtu");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (risk && strcmp(risk, "read_only") != 0) {
        snprintf(reason, reason_size, "manifest risk must be read_only for virtual_device_read");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pins = cJSON_GetObjectItem(root, "pins");
    cfg->uart.tx_gpio = json_int(pins, "tx", -1);
    cfg->uart.rx_gpio = json_int(pins, "rx", -1);
    cfg->de_re_gpio = json_int(pins, "de_re", json_int(pins, "de", -1));
    cfg->uart.uart_port = json_int(root, "uart_port", json_int(root, "port", cfg->uart.uart_port));
    cfg->uart.baud = json_int(root, "baud", cfg->uart.baud);
    cfg->uart.timeout_ms = json_int(root, "timeout_ms", cfg->uart.timeout_ms);
    cfg->uart.data_bits = parse_uart_data_bits(json_int(root, "data_bits", 8));
    cfg->uart.stop_bits = parse_uart_stop_bits(json_int(root, "stop_bits", 1));
    cfg->uart.parity = parse_uart_parity(json_string(root, "parity"));
    cfg->slave_id = json_int(root, "slave_id", cfg->slave_id);
    cfg->function_code = json_int(root, "function_code", cfg->function_code);
    cfg->register_address = parse_address(root, "register", parse_address(root, "register_address", -1));
    cfg->register_count = json_int(root, "register_count", cfg->register_count);

    cJSON *decode = cJSON_GetObjectItem(root, "decode");
    const char *field = json_string(decode, "field");
    const char *unit = json_string(decode, "unit");
    if (field) {
        snprintf(cfg->field, sizeof(cfg->field), "%s", field);
    }
    if (unit) {
        snprintf(cfg->unit, sizeof(cfg->unit), "%s", unit);
    }
    cfg->scale = json_double(decode, "scale", cfg->scale);
    cfg->offset = json_double(decode, "offset", cfg->offset);

    if (cfg->uart.uart_port <= 0 || cfg->uart.uart_port >= UART_NUM_MAX) {
        snprintf(reason, reason_size, "uart_port must be 1..%d; UART0 is reserved for console", UART_NUM_MAX - 1);
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->uart.tx_gpio) ||
        !GPIO_IS_VALID_GPIO((gpio_num_t)cfg->uart.rx_gpio) ||
        cfg->uart.tx_gpio == cfg->uart.rx_gpio) {
        snprintf(reason, reason_size, "invalid Modbus UART tx/rx pins");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->de_re_gpio >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->de_re_gpio)) {
        snprintf(reason, reason_size, "invalid Modbus de_re pin");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->uart.baud < 1200 || cfg->uart.baud > 921600) {
        snprintf(reason, reason_size, "baud must be 1200..921600");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->uart.timeout_ms <= 0 || cfg->uart.timeout_ms > 3000) {
        snprintf(reason, reason_size, "timeout_ms must be 1..3000");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->slave_id <= 0 || cfg->slave_id > 247) {
        snprintf(reason, reason_size, "slave_id must be 1..247");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->function_code != 3 && cfg->function_code != 4) {
        snprintf(reason, reason_size, "function_code must be read-only 3 or 4");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->register_address < 0 || cfg->register_address > 0xffff) {
        snprintf(reason, reason_size, "register must be 0..0xffff");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->register_count <= 0 || cfg->register_count > MODBUS_REGISTERS_MAX) {
        snprintf(reason, reason_size, "register_count must be 1..%d", MODBUS_REGISTERS_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    cfg->uart.read_length = 5 + cfg->register_count * 2;
    return ESP_OK;
}

static spi_host_device_t parse_spi_host(int value)
{
    return value == 3 ? SPI3_HOST : SPI2_HOST;
}

static esp_err_t parse_spi_manifest(cJSON *root,
                                    const char *device,
                                    virtual_spi_device_t *cfg,
                                    char *reason,
                                    size_t reason_size)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->device, sizeof(cfg->device), "%s", device);
    cfg->mosi_gpio = -1;
    cfg->miso_gpio = -1;
    cfg->sclk_gpio = -1;
    cfg->cs_gpio = -1;
    cfg->host_id = 2;
    cfg->mode = 0;
    cfg->frequency_hz = 1000000;
    cfg->read_length = 1;
    cfg->dummy_bytes = 0;
    cfg->timeout_ms = SPI_TIMEOUT_DEFAULT_MS;

    const char *protocol = json_string(root, "protocol");
    const char *risk = json_string(root, "risk");
    if (!protocol || strcmp(protocol, "spi") != 0) {
        snprintf(reason, reason_size, "manifest protocol must be spi");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (risk && strcmp(risk, "read_only") != 0) {
        snprintf(reason, reason_size, "manifest risk must be read_only for virtual_device_read");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *pins = cJSON_GetObjectItem(root, "pins");
    cfg->mosi_gpio = json_int(pins, "mosi", -1);
    cfg->miso_gpio = json_int(pins, "miso", -1);
    cfg->sclk_gpio = json_int(pins, "sclk", -1);
    cfg->cs_gpio = json_int(pins, "cs", -1);
    cfg->host_id = json_int(root, "host", json_int(root, "spi_host", cfg->host_id));
    cfg->mode = json_int(root, "mode", cfg->mode);
    cfg->frequency_hz = json_int(root, "frequency_hz", cfg->frequency_hz);
    cfg->read_length = json_int(root, "read_length", cfg->read_length);
    cfg->dummy_bytes = json_int(root, "dummy_bytes", cfg->dummy_bytes);
    cfg->timeout_ms = json_int(root, "timeout_ms", cfg->timeout_ms);

    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->mosi_gpio) ||
        !GPIO_IS_VALID_GPIO((gpio_num_t)cfg->miso_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->sclk_gpio) ||
        !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->cs_gpio)) {
        snprintf(reason, reason_size, "invalid SPI pins");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->host_id != 2 && cfg->host_id != 3) {
        snprintf(reason, reason_size, "host must be 2 or 3");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->mode < 0 || cfg->mode > 3) {
        snprintf(reason, reason_size, "mode must be 0..3");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->frequency_hz <= 0 || cfg->frequency_hz > 10000000) {
        snprintf(reason, reason_size, "frequency_hz must be 1..10000000");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->read_length <= 0 || cfg->read_length > SPI_BYTES_MAX) {
        snprintf(reason, reason_size, "read_length must be 1..%d", SPI_BYTES_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->dummy_bytes < 0 || cfg->dummy_bytes > SPI_BYTES_MAX) {
        snprintf(reason, reason_size, "dummy_bytes must be 0..%d", SPI_BYTES_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->timeout_ms <= 0 || cfg->timeout_ms > 3000) {
        snprintf(reason, reason_size, "timeout_ms must be 1..3000");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static adc_atten_t parse_adc_atten(int atten_db)
{
    switch (atten_db) {
    case 0: return ADC_ATTEN_DB_0;
    case 2: return ADC_ATTEN_DB_2_5;
    case 6: return ADC_ATTEN_DB_6;
    default: return ADC_ATTEN_DB_12;
    }
}

static esp_err_t parse_adc_manifest(cJSON *root,
                                    const char *device,
                                    virtual_adc_device_t *cfg,
                                    char *reason,
                                    size_t reason_size)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->device, sizeof(cfg->device), "%s", device);
    cfg->unit = 1;
    cfg->channel = -1;
    cfg->atten_db = 12;
    cfg->samples = 4;
    cfg->scale = 1.0;
    cfg->offset = 0.0;
    snprintf(cfg->field, sizeof(cfg->field), "adc");
    snprintf(cfg->unit_name, sizeof(cfg->unit_name), "raw");

    const char *protocol = json_string(root, "protocol");
    const char *risk = json_string(root, "risk");
    if (!protocol || strcmp(protocol, "adc") != 0) {
        snprintf(reason, reason_size, "manifest protocol must be adc");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (risk && strcmp(risk, "read_only") != 0) {
        snprintf(reason, reason_size, "manifest risk must be read_only for virtual_device_read");
        return ESP_ERR_INVALID_ARG;
    }

    cfg->unit = json_int(root, "adc_unit", json_int(root, "unit", cfg->unit));
    cfg->channel = json_int(root, "channel", cfg->channel);
    cfg->atten_db = json_int(root, "atten_db", cfg->atten_db);
    cfg->samples = json_int(root, "samples", cfg->samples);
    cJSON *decode = cJSON_GetObjectItem(root, "decode");
    const char *field = json_string(decode, "field");
    const char *unit = json_string(decode, "unit");
    if (field) {
        snprintf(cfg->field, sizeof(cfg->field), "%s", field);
    }
    if (unit) {
        snprintf(cfg->unit_name, sizeof(cfg->unit_name), "%s", unit);
    }
    cfg->scale = json_double(decode, "scale", cfg->scale);
    cfg->offset = json_double(decode, "offset", cfg->offset);

    if (cfg->unit != 1 && cfg->unit != 2) {
        snprintf(reason, reason_size, "adc_unit must be 1 or 2");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->channel < 0 || cfg->channel > 9) {
        snprintf(reason, reason_size, "channel must be 0..9 on ESP32-S3");
        return ESP_ERR_INVALID_ARG;
    }
    if (!(cfg->atten_db == 0 || cfg->atten_db == 2 || cfg->atten_db == 6 || cfg->atten_db == 12)) {
        snprintf(reason, reason_size, "atten_db must be 0, 2, 6, or 12");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->samples <= 0 || cfg->samples > ADC_SAMPLES_MAX) {
        snprintf(reason, reason_size, "samples must be 1..%d", ADC_SAMPLES_MAX);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t parse_gpio_input_manifest(cJSON *root,
                                           const char *device,
                                           virtual_gpio_input_t *cfg,
                                           char *reason,
                                           size_t reason_size)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->device, sizeof(cfg->device), "%s", device);
    cfg->pin = -1;
    snprintf(cfg->field, sizeof(cfg->field), "level");

    const char *protocol = json_string(root, "protocol");
    const char *risk = json_string(root, "risk");
    if (!protocol || strcmp(protocol, "gpio_input") != 0) {
        snprintf(reason, reason_size, "manifest protocol must be gpio_input");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (risk && strcmp(risk, "read_only") != 0) {
        snprintf(reason, reason_size, "manifest risk must be read_only for virtual_device_read");
        return ESP_ERR_INVALID_ARG;
    }

    cfg->pin = json_int(root, "pin", -1);
    cfg->pullup = json_bool(root, "pullup", false);
    cfg->pulldown = json_bool(root, "pulldown", false);
    cfg->invert = json_bool(root, "invert", false);
    const char *field = json_string(root, "field");
    if (field) {
        snprintf(cfg->field, sizeof(cfg->field), "%s", field);
    }

    if (!GPIO_IS_VALID_GPIO((gpio_num_t)cfg->pin)) {
        snprintf(reason, reason_size, "invalid GPIO input pin");
        return ESP_ERR_INVALID_ARG;
    }
    if (!gpio_policy_pin_is_allowed(cfg->pin)) {
        snprintf(reason, reason_size, "GPIO pin is outside ESPAgent allowlist");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->pullup && cfg->pulldown) {
        snprintf(reason, reason_size, "pullup and pulldown cannot both be true");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool risk_is_control(const char *risk)
{
    return risk &&
           (strcmp(risk, "low_control") == 0 ||
            strcmp(risk, "medium_control") == 0 ||
            strcmp(risk, "high_control") == 0);
}

static esp_err_t parse_control_manifest(cJSON *root,
                                        const char *device,
                                        virtual_control_device_t *cfg,
                                        char *reason,
                                        size_t reason_size)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->device, sizeof(cfg->device), "%s", device);
    cfg->pin = -1;
    cfg->active_level = 1;
    cfg->safe_level = 0;
    cfg->default_level = 0;
    cfg->max_duration_ms = VIRTUAL_CONTROL_DEFAULT_MAX_DURATION_MS;
    cfg->cooldown_ms = VIRTUAL_CONTROL_DEFAULT_COOLDOWN_MS;
    cfg->frequency_hz = 1000;
    cfg->duty_pct = 0;
    cfg->ledc_channel = 1;
    cfg->ledc_timer = 1;

    const char *protocol = json_string(root, "protocol");
    const char *risk = json_string(root, "risk");
    if (!protocol ||
        (strcmp(protocol, "gpio_output") != 0 &&
         strcmp(protocol, "relay_control") != 0 &&
         strcmp(protocol, "pwm_output") != 0 &&
         strcmp(protocol, "ledc_pwm") != 0)) {
        snprintf(reason, reason_size, "manifest protocol must be gpio_output, relay_control, pwm_output, or ledc_pwm");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!risk_is_control(risk)) {
        snprintf(reason, reason_size, "control manifest risk must be low_control, medium_control, or high_control");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(cfg->protocol, sizeof(cfg->protocol), "%s", protocol);
    cfg->high_impact = strcmp(risk, "high_control") == 0 || strcmp(protocol, "relay_control") == 0;
    cfg->pin = json_int(root, "pin", -1);
    cfg->active_level = json_int(root, "active_level", cfg->active_level);
    cfg->safe_level = json_int(root, "safe_level", cfg->safe_level);
    cfg->default_level = json_int(root, "default_level", cfg->default_level);
    cfg->max_duration_ms = json_int(root, "max_duration_ms", cfg->max_duration_ms);
    cfg->cooldown_ms = json_int(root, "cooldown_ms", cfg->cooldown_ms);
    cfg->frequency_hz = json_int(root, "frequency_hz", cfg->frequency_hz);
    cfg->duty_pct = json_int(root, "default_duty_pct", cfg->duty_pct);
    cfg->ledc_channel = json_int(root, "ledc_channel", cfg->ledc_channel);
    cfg->ledc_timer = json_int(root, "ledc_timer", cfg->ledc_timer);
    cfg->invert = json_bool(root, "invert", false);

    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)cfg->pin) || !gpio_policy_pin_is_allowed(cfg->pin)) {
        snprintf(reason, reason_size, "control pin is invalid or outside ESPAgent allowlist");
        return ESP_ERR_INVALID_ARG;
    }
    if ((cfg->active_level != 0 && cfg->active_level != 1) ||
        (cfg->safe_level != 0 && cfg->safe_level != 1) ||
        (cfg->default_level != 0 && cfg->default_level != 1)) {
        snprintf(reason, reason_size, "active_level, safe_level, and default_level must be 0 or 1");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->max_duration_ms < 0 || cfg->max_duration_ms > VIRTUAL_CONTROL_MAX_DURATION_MS) {
        snprintf(reason, reason_size, "max_duration_ms must be 0..%d", VIRTUAL_CONTROL_MAX_DURATION_MS);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->cooldown_ms < 0 || cfg->cooldown_ms > 60000) {
        snprintf(reason, reason_size, "cooldown_ms must be 0..60000");
        return ESP_ERR_INVALID_ARG;
    }
    if ((strcmp(protocol, "pwm_output") == 0 || strcmp(protocol, "ledc_pwm") == 0) &&
        (cfg->frequency_hz <= 0 || cfg->frequency_hz > 40000)) {
        snprintf(reason, reason_size, "frequency_hz must be 1..40000 for PWM");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->duty_pct < 0 || cfg->duty_pct > 100) {
        snprintf(reason, reason_size, "default_duty_pct must be 0..100");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->ledc_channel < 0 || cfg->ledc_channel >= LEDC_CHANNEL_MAX ||
        cfg->ledc_timer < 0 || cfg->ledc_timer >= LEDC_TIMER_MAX) {
        snprintf(reason, reason_size, "invalid LEDC channel or timer");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t control_cooldown_check_and_mark(const char *device,
                                                 int cooldown_ms,
                                                 char *reason,
                                                 size_t reason_size)
{
    if (cooldown_ms <= 0) {
        return ESP_OK;
    }
    int64_t now = esp_timer_get_time();
    int free_slot = -1;
    for (int i = 0; i < VIRTUAL_CONTROL_COOLDOWN_SLOTS; i++) {
        if (s_control_cooldowns[i].device[0] == '\0' && free_slot < 0) {
            free_slot = i;
            continue;
        }
        if (strcmp(s_control_cooldowns[i].device, device) == 0) {
            int64_t elapsed_ms = (now - s_control_cooldowns[i].last_us) / 1000;
            if (elapsed_ms < cooldown_ms) {
                snprintf(reason, reason_size,
                         "cooldown active for device=%s, wait %lldms",
                         device,
                         (long long)(cooldown_ms - elapsed_ms));
                return ESP_ERR_INVALID_STATE;
            }
            s_control_cooldowns[i].last_us = now;
            return ESP_OK;
        }
    }
    int slot = free_slot >= 0 ? free_slot : 0;
    snprintf(s_control_cooldowns[slot].device, sizeof(s_control_cooldowns[slot].device), "%s", device);
    s_control_cooldowns[slot].last_us = now;
    return ESP_OK;
}

static esp_err_t parse_uart_command(cJSON *request,
                                    cJSON *manifest,
                                    uint8_t *cmd,
                                    size_t *cmd_len)
{
    cJSON *bytes = cJSON_GetObjectItem(request, "command_bytes");
    if (!bytes) {
        bytes = cJSON_GetObjectItem(manifest, "command_bytes");
    }
    if (bytes) {
        return parse_bytes_limited(bytes, cmd, cmd_len, UART_COMMAND_MAX);
    }

    const char *ascii = json_string(request, "command_ascii");
    if (!ascii) {
        ascii = json_string(manifest, "command_ascii");
    }
    if (!ascii) {
        cJSON *command = cJSON_GetObjectItem(request, "command");
        if (!command) {
            command = cJSON_GetObjectItem(manifest, "command");
        }
        ascii = json_string(command, "ascii");
        bytes = cJSON_GetObjectItem(command, "bytes");
        if (bytes) {
            return parse_bytes_limited(bytes, cmd, cmd_len, UART_COMMAND_MAX);
        }
    }
    if (!ascii) {
        *cmd_len = 0;
        return ESP_OK;
    }

    size_t len = strlen(ascii);
    const char *terminator = json_string(request, "terminator");
    if (!terminator) {
        terminator = json_string(manifest, "terminator");
    }
    size_t term_len = terminator ? strlen(terminator) : 0;
    if (len + term_len > UART_COMMAND_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(cmd, ascii, len);
    if (term_len > 0) {
        memcpy(cmd + len, terminator, term_len);
    }
    *cmd_len = len + term_len;
    return ESP_OK;
}

static uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x0001) {
                crc = (uint16_t)((crc >> 1) ^ 0xa001);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void format_ascii_preview(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < out_size; i++) {
        unsigned char ch = data[i];
        out[pos++] = (ch >= 32 && ch <= 126) ? (char)ch : '.';
    }
    out[pos] = '\0';
}

static void format_hex_preview(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 4 < out_size; i++) {
        pos += snprintf(out + pos, out_size - pos, "%02X%s", data[i], (i + 1 < len) ? " " : "");
    }
}

static esp_err_t execute_uart_manifest(cJSON *manifest,
                                       cJSON *request,
                                       const virtual_uart_device_t *cfg,
                                       char *output,
                                       size_t output_size)
{
    uint8_t command[UART_COMMAND_MAX] = {0};
    size_t command_len = 0;
    esp_err_t err = parse_uart_command(request, manifest, command, &command_len);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: invalid UART command for device=%s (%s)",
                 cfg->device, esp_err_to_name(err));
        return err;
    }
    bool expect_response = json_bool(request, "expect_response", true);

    uart_config_t uart_cfg = {
        .baud_rate = cfg->baud,
        .data_bits = cfg->data_bits,
        .parity = cfg->parity,
        .stop_bits = cfg->stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_port_t uart_num = (uart_port_t)cfg->uart_port;
    err = uart_driver_install(uart_num, UART_BYTES_MAX * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: install UART%d failed for device=%s (%s)",
                 cfg->uart_port, cfg->device, esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(uart_num, &uart_cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(uart_num,
                           cfg->tx_gpio,
                           cfg->rx_gpio,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: configure UART%d TX=%d RX=%d failed (%s)",
                 cfg->uart_port, cfg->tx_gpio, cfg->rx_gpio, esp_err_to_name(err));
        uart_driver_delete(uart_num);
        return err;
    }

    uart_flush_input(uart_num);
    if (command_len > 0) {
        int written = uart_write_bytes(uart_num, (const char *)command, command_len);
        if (written != (int)command_len) {
            snprintf(output, output_size, "Error: UART%d wrote %d/%u bytes",
                     cfg->uart_port, written, (unsigned)command_len);
            uart_driver_delete(uart_num);
            return ESP_FAIL;
        }
        if (cfg->post_write_delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cfg->post_write_delay_ms));
        }
    }

    if (!expect_response) {
        uart_wait_tx_done(uart_num, pdMS_TO_TICKS(cfg->timeout_ms));
        uart_driver_delete(uart_num);
        char ascii[96] = {0};
        char hex[160] = {0};
        format_ascii_preview(command, command_len, ascii, sizeof(ascii));
        format_hex_preview(command, command_len, hex, sizeof(hex));
        snprintf(output, output_size,
                 "OK: virtual_device_read device=%s protocol=uart uart=%d baud=%d TX=%d RX=%d sent_bytes=%u ascii=\"%s\" hex=%s response=skipped",
                 cfg->device,
                 cfg->uart_port,
                 cfg->baud,
                 cfg->tx_gpio,
                 cfg->rx_gpio,
                 (unsigned)command_len,
                 ascii,
                 hex[0] ? hex : "(none)");
        ESP_LOGI(TAG, "virtual_device_read UART send-only status=ESP_OK result=%s", output);
        return ESP_OK;
    }

    uint8_t response[UART_BYTES_MAX] = {0};
    int n = uart_read_bytes(uart_num,
                            response,
                            (uint32_t)cfg->read_length,
                            pdMS_TO_TICKS(cfg->timeout_ms));
    uart_driver_delete(uart_num);
    if (n <= 0) {
        snprintf(output, output_size,
                 "Error: UART%d device=%s no response within %dms TX=%d RX=%d",
                 cfg->uart_port, cfg->device, cfg->timeout_ms, cfg->tx_gpio, cfg->rx_gpio);
        return ESP_ERR_TIMEOUT;
    }

    char ascii[96] = {0};
    char hex[160] = {0};
    format_ascii_preview(response, (size_t)n, ascii, sizeof(ascii));
    format_hex_preview(response, (size_t)n, hex, sizeof(hex));
    snprintf(output, output_size,
             "OK: virtual_device_read device=%s protocol=uart uart=%d baud=%d TX=%d RX=%d bytes=%d ascii=\"%s\" hex=%s",
             cfg->device,
             cfg->uart_port,
             cfg->baud,
             cfg->tx_gpio,
             cfg->rx_gpio,
             n,
             ascii,
             hex);
    ESP_LOGI(TAG, "virtual_device_read UART status=ESP_OK result=%s", output);
    return ESP_OK;
}

static esp_err_t execute_modbus_manifest(const virtual_modbus_device_t *cfg,
                                         char *output,
                                         size_t output_size)
{
    uint8_t request[8] = {
        (uint8_t)cfg->slave_id,
        (uint8_t)cfg->function_code,
        (uint8_t)((cfg->register_address >> 8) & 0xff),
        (uint8_t)(cfg->register_address & 0xff),
        (uint8_t)((cfg->register_count >> 8) & 0xff),
        (uint8_t)(cfg->register_count & 0xff),
        0,
        0,
    };
    uint16_t req_crc = modbus_crc16(request, 6);
    request[6] = (uint8_t)(req_crc & 0xff);
    request[7] = (uint8_t)((req_crc >> 8) & 0xff);

    uart_config_t uart_cfg = {
        .baud_rate = cfg->uart.baud,
        .data_bits = cfg->uart.data_bits,
        .parity = cfg->uart.parity,
        .stop_bits = cfg->uart.stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_port_t uart_num = (uart_port_t)cfg->uart.uart_port;
    esp_err_t err = uart_driver_install(uart_num, UART_BYTES_MAX * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: install UART%d for Modbus device=%s failed (%s)",
                 cfg->uart.uart_port, cfg->uart.device, esp_err_to_name(err));
        return err;
    }
    err = uart_param_config(uart_num, &uart_cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(uart_num,
                           cfg->uart.tx_gpio,
                           cfg->uart.rx_gpio,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: configure Modbus UART%d TX=%d RX=%d failed (%s)",
                 cfg->uart.uart_port, cfg->uart.tx_gpio, cfg->uart.rx_gpio, esp_err_to_name(err));
        uart_driver_delete(uart_num);
        return err;
    }

    if (cfg->de_re_gpio >= 0) {
        gpio_set_direction((gpio_num_t)cfg->de_re_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)cfg->de_re_gpio, 0);
    }

    uart_flush_input(uart_num);
    if (cfg->de_re_gpio >= 0) {
        gpio_set_level((gpio_num_t)cfg->de_re_gpio, 1);
    }
    int written = uart_write_bytes(uart_num, (const char *)request, sizeof(request));
    uart_wait_tx_done(uart_num, pdMS_TO_TICKS(cfg->uart.timeout_ms));
    if (cfg->de_re_gpio >= 0) {
        gpio_set_level((gpio_num_t)cfg->de_re_gpio, 0);
    }
    if (written != (int)sizeof(request)) {
        snprintf(output, output_size, "Error: Modbus UART%d wrote %d/%u bytes",
                 cfg->uart.uart_port, written, (unsigned)sizeof(request));
        uart_driver_delete(uart_num);
        return ESP_FAIL;
    }
    if (cfg->uart.post_write_delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(cfg->uart.post_write_delay_ms));
    }

    uint8_t response[UART_BYTES_MAX] = {0};
    int expected = cfg->uart.read_length;
    int n = uart_read_bytes(uart_num, response, expected, pdMS_TO_TICKS(cfg->uart.timeout_ms));
    uart_driver_delete(uart_num);
    if (n < expected) {
        snprintf(output, output_size,
                 "Error: Modbus device=%s no complete response on UART%d, got %d/%d bytes",
                 cfg->uart.device, cfg->uart.uart_port, n, expected);
        return ESP_ERR_TIMEOUT;
    }
    uint16_t actual_crc = ((uint16_t)response[n - 1] << 8) | response[n - 2];
    uint16_t calc_crc = modbus_crc16(response, (size_t)n - 2);
    if (actual_crc != calc_crc) {
        snprintf(output, output_size,
                 "Error: Modbus CRC mismatch device=%s actual=0x%04x expected=0x%04x",
                 cfg->uart.device, actual_crc, calc_crc);
        return ESP_ERR_INVALID_CRC;
    }
    if (response[0] != (uint8_t)cfg->slave_id || response[1] != (uint8_t)cfg->function_code) {
        snprintf(output, output_size,
                 "Error: Modbus unexpected slave/function device=%s slave=%u function=%u",
                 cfg->uart.device, response[0], response[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (response[2] != (uint8_t)(cfg->register_count * 2)) {
        snprintf(output, output_size,
                 "Error: Modbus byte count mismatch device=%s got=%u expected=%d",
                 cfg->uart.device, response[2], cfg->register_count * 2);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char regs[144] = {0};
    size_t pos = 0;
    uint16_t first_raw = 0;
    for (int i = 0; i < cfg->register_count; i++) {
        uint16_t value = ((uint16_t)response[3 + i * 2] << 8) | response[4 + i * 2];
        if (i == 0) {
            first_raw = value;
        }
        pos += snprintf(regs + pos,
                        sizeof(regs) - pos,
                        "%s%u",
                        i == 0 ? "" : ",",
                        value);
        if (pos >= sizeof(regs)) {
            break;
        }
    }
    double decoded = (double)first_raw * cfg->scale + cfg->offset;
    snprintf(output,
             output_size,
             "OK: virtual_device_read device=%s protocol=modbus_rtu uart=%d slave=%d fc=%d register=0x%04x count=%d %s=%.3f %s raw=[%s]",
             cfg->uart.device,
             cfg->uart.uart_port,
             cfg->slave_id,
             cfg->function_code,
             cfg->register_address,
             cfg->register_count,
             cfg->field,
             decoded,
             cfg->unit,
             regs);
    ESP_LOGI(TAG, "virtual_device_read Modbus status=ESP_OK result=%s", output);
    return ESP_OK;
}

static esp_err_t execute_spi_manifest(cJSON *manifest,
                                      const virtual_spi_device_t *cfg,
                                      char *output,
                                      size_t output_size)
{
    uint8_t tx[SPI_BYTES_MAX * 2] = {0};
    uint8_t rx[SPI_BYTES_MAX * 2] = {0};
    size_t command_len = 0;
    cJSON *command = cJSON_GetObjectItem(manifest, "command_bytes");
    esp_err_t err = ESP_OK;
    if (command) {
        err = parse_bytes_limited(command, tx, &command_len, SPI_BYTES_MAX);
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: invalid SPI command_bytes for device=%s", cfg->device);
            return err;
        }
    }

    size_t skip_len = command_len + (size_t)cfg->dummy_bytes;
    size_t total_len = skip_len + (size_t)cfg->read_length;
    if (total_len > sizeof(tx)) {
        snprintf(output, output_size, "Error: SPI transfer too long for device=%s", cfg->device);
        return ESP_ERR_INVALID_SIZE;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = cfg->mosi_gpio,
        .miso_io_num = cfg->miso_gpio,
        .sclk_io_num = cfg->sclk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = (int)total_len,
    };
    spi_device_interface_config_t dev_cfg = {
        .mode = (uint8_t)cfg->mode,
        .clock_speed_hz = cfg->frequency_hz,
        .spics_io_num = cfg->cs_gpio,
        .queue_size = 1,
    };
    spi_host_device_t host = parse_spi_host(cfg->host_id);
    spi_device_handle_t handle = NULL;

    err = spi_bus_initialize(host, &bus_cfg, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: initialize SPI host=%d failed for device=%s (%s)",
                 cfg->host_id, cfg->device, esp_err_to_name(err));
        return err;
    }
    err = spi_bus_add_device(host, &dev_cfg, &handle);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: add SPI device=%s failed (%s)", cfg->device, esp_err_to_name(err));
        spi_bus_free(host);
        return err;
    }

    spi_transaction_t transaction = {
        .length = total_len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    err = spi_device_transmit(handle, &transaction);
    spi_bus_remove_device(handle);
    spi_bus_free(host);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: SPI transfer failed for device=%s (%s)",
                 cfg->device, esp_err_to_name(err));
        return err;
    }

    char hex[192] = {0};
    format_hex_preview(rx + skip_len, (size_t)cfg->read_length, hex, sizeof(hex));
    snprintf(output,
             output_size,
             "OK: virtual_device_read device=%s protocol=spi host=%d mode=%d hz=%d CS=%d bytes=%d hex=%s",
             cfg->device,
             cfg->host_id,
             cfg->mode,
             cfg->frequency_hz,
             cfg->cs_gpio,
             cfg->read_length,
             hex);
    ESP_LOGI(TAG, "virtual_device_read SPI status=ESP_OK result=%s", output);
    return ESP_OK;
}

static esp_err_t execute_adc_manifest(const virtual_adc_device_t *cfg,
                                      char *output,
                                      size_t output_size)
{
    adc_oneshot_unit_handle_t handle = NULL;
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = cfg->unit == 1 ? ADC_UNIT_1 : ADC_UNIT_2,
        .clk_src = ADC_RTC_CLK_SRC_DEFAULT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &handle);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: create ADC unit=%d failed for device=%s (%s)",
                 cfg->unit, cfg->device, esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = parse_adc_atten(cfg->atten_db),
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(handle, (adc_channel_t)cfg->channel, &chan_cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: configure ADC unit=%d channel=%d failed (%s)",
                 cfg->unit, cfg->channel, esp_err_to_name(err));
        adc_oneshot_del_unit(handle);
        return err;
    }

    int raw = 0;
    int64_t sum = 0;
    for (int i = 0; i < cfg->samples; i++) {
        err = adc_oneshot_read(handle, (adc_channel_t)cfg->channel, &raw);
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: read ADC unit=%d channel=%d failed (%s)",
                     cfg->unit, cfg->channel, esp_err_to_name(err));
            adc_oneshot_del_unit(handle);
            return err;
        }
        sum += raw;
    }
    adc_oneshot_del_unit(handle);

    double avg = (double)sum / (double)cfg->samples;
    double value = avg * cfg->scale + cfg->offset;
    snprintf(output,
             output_size,
             "OK: virtual_device_read device=%s protocol=adc unit=%d channel=%d samples=%d raw_avg=%.2f %s=%.3f %s",
             cfg->device,
             cfg->unit,
             cfg->channel,
             cfg->samples,
             avg,
             cfg->field,
             value,
             cfg->unit_name);
    ESP_LOGI(TAG, "virtual_device_read ADC status=ESP_OK result=%s", output);
    return ESP_OK;
}

static esp_err_t execute_gpio_input_manifest(const virtual_gpio_input_t *cfg,
                                             char *output,
                                             size_t output_size)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << cfg->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = cfg->pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = cfg->pulldown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: configure GPIO input pin=%d failed (%s)",
                 cfg->pin, esp_err_to_name(err));
        return err;
    }
    int level = gpio_get_level((gpio_num_t)cfg->pin);
    int value = cfg->invert ? !level : level;
    snprintf(output,
             output_size,
             "OK: virtual_device_read device=%s protocol=gpio_input pin=%d %s=%d raw_level=%d",
             cfg->device,
             cfg->pin,
             cfg->field,
             value,
             level);
    ESP_LOGI(TAG, "virtual_device_read GPIO status=ESP_OK result=%s", output);
    return ESP_OK;
}

static esp_err_t execute_control_manifest(const virtual_control_device_t *cfg,
                                          cJSON *args,
                                          char *output,
                                          size_t output_size)
{
    bool confirmed = json_bool(args, "confirmed", false);
    int duration_ms = json_int(args, "duration_ms", 0);
    if (duration_ms < 0) {
        snprintf(output, output_size, "Error: duration_ms must be >= 0");
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms > cfg->max_duration_ms) {
        snprintf(output, output_size, "Error: duration_ms=%d exceeds manifest max_duration_ms=%d",
                 duration_ms, cfg->max_duration_ms);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->high_impact && duration_ms == 0 && !confirmed) {
        snprintf(output,
                 output_size,
                 "Error: persistent high-impact control device=%s requires confirmed=true or bounded duration_ms",
                 cfg->device);
        return ESP_ERR_INVALID_STATE;
    }

    char cooldown_reason[128] = {0};
    esp_err_t err = control_cooldown_check_and_mark(cfg->device,
                                                    cfg->cooldown_ms,
                                                    cooldown_reason,
                                                    sizeof(cooldown_reason));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: %s", cooldown_reason);
        return err;
    }

    if (strcmp(cfg->protocol, "pwm_output") == 0 || strcmp(cfg->protocol, "ledc_pwm") == 0) {
        int duty_pct = json_int(args, "duty_pct", cfg->duty_pct);
        if (duty_pct < 0 || duty_pct > 100) {
            snprintf(output, output_size, "Error: duty_pct must be 0..100");
            return ESP_ERR_INVALID_ARG;
        }
        ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = (ledc_timer_t)cfg->ledc_timer,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .freq_hz = (uint32_t)cfg->frequency_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        err = ledc_timer_config(&timer_cfg);
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: LEDC timer config failed for device=%s (%s)",
                     cfg->device, esp_err_to_name(err));
            return err;
        }
        uint32_t duty = (uint32_t)((duty_pct * 1023) / 100);
        ledc_channel_config_t channel_cfg = {
            .gpio_num = cfg->pin,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)cfg->ledc_channel,
            .timer_sel = (ledc_timer_t)cfg->ledc_timer,
            .duty = duty,
            .hpoint = 0,
            .flags.output_invert = cfg->invert ? 1 : 0,
        };
        err = ledc_channel_config(&channel_cfg);
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: LEDC channel config failed for device=%s (%s)",
                     cfg->device, esp_err_to_name(err));
            return err;
        }
        char restore_reason[96] = {0};
        err = schedule_control_restore(cfg, true, duration_ms, restore_reason, sizeof(restore_reason));
        if (err != ESP_OK) {
            ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)cfg->ledc_channel, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)cfg->ledc_channel);
            snprintf(output, output_size, "Error: %s", restore_reason);
            return err;
        }
        snprintf(output,
                 output_size,
                 "OK: virtual_device_control device=%s protocol=%s pin=%d duty_pct=%d duration_ms=%d restore=%s",
                 cfg->device,
                 cfg->protocol,
                 cfg->pin,
                 duty_pct,
                 duration_ms,
                 duration_ms > 0 ? "scheduled" : "none");
        return ESP_OK;
    }

    int level = json_int(args, "level", cfg->default_level);
    cJSON *active_item = cJSON_GetObjectItem(args, "active");
    if (cJSON_IsBool(active_item)) {
        level = cJSON_IsTrue(active_item) ? cfg->active_level : cfg->safe_level;
    }
    if (level != 0 && level != 1) {
        snprintf(output, output_size, "Error: level must be 0 or 1");
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << cfg->pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: configure GPIO output pin=%d failed (%s)",
                 cfg->pin, esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)cfg->pin, level);
    char restore_reason[96] = {0};
    err = schedule_control_restore(cfg, false, duration_ms, restore_reason, sizeof(restore_reason));
    if (err != ESP_OK) {
        gpio_set_level((gpio_num_t)cfg->pin, cfg->safe_level);
        snprintf(output, output_size, "Error: %s", restore_reason);
        return err;
    }
    snprintf(output,
             output_size,
             "OK: virtual_device_control device=%s protocol=%s pin=%d level=%d duration_ms=%d safe_level=%d restore=%s",
             cfg->device,
             cfg->protocol,
             cfg->pin,
             level,
             duration_ms,
             cfg->safe_level,
             duration_ms > 0 ? "scheduled" : "none");
    ESP_LOGI(TAG, "virtual_device_control status=ESP_OK result=%s", output);
    return ESP_OK;
}

static esp_err_t route_to_sensor_agent(cJSON *root, char *output, size_t output_size)
{
    cJSON *cmd = cJSON_CreateObject();
    cJSON *args = cJSON_Duplicate(root, true);
    if (!cmd || !args) {
        cJSON_Delete(cmd);
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(args, "local");
    cJSON_AddStringToObject(cmd, "target_role", "sensor_agent");
    cJSON_AddStringToObject(cmd, "action", "virtual_device_read");
    cJSON_AddItemToObject(cmd, "args", args);

    char *payload = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    if (!payload) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = tool_mesh_send_command_execute(payload, output, output_size);
    cJSON_free(payload);
    return err;
}

static esp_err_t route_to_control_agent(cJSON *root, char *output, size_t output_size)
{
    cJSON *cmd = cJSON_CreateObject();
    cJSON *args = cJSON_Duplicate(root, true);
    if (!cmd || !args) {
        cJSON_Delete(cmd);
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(args, "local");
    cJSON_AddStringToObject(cmd, "target_role", "control_agent");
    cJSON_AddStringToObject(cmd, "action", "virtual_device_control");
    cJSON_AddItemToObject(cmd, "args", args);

    char *payload = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    if (!payload) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = tool_mesh_send_command_execute(payload, output, output_size);
    cJSON_free(payload);
    return err;
}

esp_err_t tool_virtual_device_read_execute(const char *input_json,
                                           char *output,
                                           size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    bool local = json_bool(root, "local", false);
    if (espagent_role_is_coordinator() && !local) {
        esp_err_t routed = route_to_sensor_agent(root, output, output_size);
        cJSON_Delete(root);
        return routed;
    }

    const char *device = json_string(root, "device");
    char device_name[DEVICE_NAME_MAX] = {0};
    if (!valid_device_name(device)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: device must be 1..47 chars using letters, numbers, '_' or '-'");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(device_name, sizeof(device_name), "%s", device);

    char manifest_path[128] = {0};
    char manifest_buf[MANIFEST_MAX_SIZE] = {0};
    esp_err_t err = read_manifest(device_name,
                                  manifest_buf,
                                  sizeof(manifest_buf),
                                  manifest_path,
                                  sizeof(manifest_path));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: cannot read manifest for device=%s at %s (%s)",
                 device_name, manifest_path[0] ? manifest_path : ESPAGENT_SPIFFS_BASE "/devices/<device>.json",
                 esp_err_to_name(err));
        return err;
    }

    char signature_reason[192] = {0};
    err = verify_manifest_sha256(device_name, manifest_buf, false, signature_reason, sizeof(signature_reason));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: manifest trust check failed for device=%s: %s",
                 device_name, signature_reason);
        return err;
    }

    cJSON *manifest = cJSON_Parse(manifest_buf);
    if (!manifest || !cJSON_IsObject(manifest)) {
        cJSON_Delete(root);
        cJSON_Delete(manifest);
        snprintf(output, output_size, "Error: invalid manifest JSON for device=%s", device);
        return ESP_ERR_INVALID_ARG;
    }

    const char *protocol = json_string(manifest, "protocol");
    char reason[128] = {0};
    err = validate_manifest_header(manifest, device_name, "sensor_agent", "read", reason, sizeof(reason));
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: manifest rejected for device=%s: %s", device, reason);
        cJSON_Delete(manifest);
        cJSON_Delete(root);
        return err;
    }
    if (protocol && strcmp(protocol, "i2c") == 0) {
        virtual_i2c_device_t cfg;
        err = parse_i2c_manifest(manifest, device_name, &cfg, reason, sizeof(reason));
        if (err == ESP_OK) {
            err = execute_i2c_manifest(manifest, &cfg, output, output_size);
        }
    } else if (protocol && strcmp(protocol, "uart") == 0) {
        virtual_uart_device_t cfg;
        err = parse_uart_manifest(manifest, device_name, &cfg, reason, sizeof(reason));
        if (err == ESP_OK) {
            err = execute_uart_manifest(manifest, root, &cfg, output, output_size);
        }
    } else if (protocol && strcmp(protocol, "modbus_rtu") == 0) {
        virtual_modbus_device_t cfg;
        err = parse_modbus_manifest(manifest, device_name, &cfg, reason, sizeof(reason));
        if (err == ESP_OK) {
            err = execute_modbus_manifest(&cfg, output, output_size);
        }
    } else if (protocol && strcmp(protocol, "spi") == 0) {
        virtual_spi_device_t cfg;
        err = parse_spi_manifest(manifest, device_name, &cfg, reason, sizeof(reason));
        if (err == ESP_OK) {
            err = execute_spi_manifest(manifest, &cfg, output, output_size);
        }
    } else if (protocol && strcmp(protocol, "adc") == 0) {
        virtual_adc_device_t cfg;
        err = parse_adc_manifest(manifest, device_name, &cfg, reason, sizeof(reason));
        if (err == ESP_OK) {
            err = execute_adc_manifest(&cfg, output, output_size);
        }
    } else if (protocol && strcmp(protocol, "gpio_input") == 0) {
        virtual_gpio_input_t cfg;
        err = parse_gpio_input_manifest(manifest, device_name, &cfg, reason, sizeof(reason));
        if (err == ESP_OK) {
            err = execute_gpio_input_manifest(&cfg, output, output_size);
        }
    } else {
        err = ESP_ERR_NOT_SUPPORTED;
        snprintf(reason,
                 sizeof(reason),
                 "protocol must be i2c, uart, modbus_rtu, spi, adc, or gpio_input for this phase");
    }
    if (err != ESP_OK && output[0] == '\0') {
        snprintf(output, output_size, "Error: manifest rejected for device=%s: %s",
                 device_name, reason[0] ? reason : esp_err_to_name(err));
    }

    cJSON_Delete(manifest);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_virtual_device_control_execute(const char *input_json,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    bool local = json_bool(root, "local", false);
    if (espagent_role_is_coordinator() && !local) {
        esp_err_t routed = route_to_control_agent(root, output, output_size);
        cJSON_Delete(root);
        return routed;
    }

    const char *device = json_string(root, "device");
    char device_name[DEVICE_NAME_MAX] = {0};
    if (!valid_device_name(device)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: device must be 1..47 chars using letters, numbers, '_' or '-'");
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(device_name, sizeof(device_name), "%s", device);

    char manifest_path[128] = {0};
    char manifest_buf[MANIFEST_MAX_SIZE] = {0};
    esp_err_t err = read_manifest(device_name,
                                  manifest_buf,
                                  sizeof(manifest_buf),
                                  manifest_path,
                                  sizeof(manifest_path));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: cannot read control manifest for device=%s at %s (%s)",
                 device_name, manifest_path[0] ? manifest_path : ESPAGENT_SPIFFS_BASE "/devices/<device>.json",
                 esp_err_to_name(err));
        return err;
    }

    char signature_reason[192] = {0};
    err = verify_manifest_sha256(device_name, manifest_buf, true, signature_reason, sizeof(signature_reason));
    if (err != ESP_OK) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: control manifest trust check failed for device=%s: %s",
                 device_name, signature_reason);
        return err;
    }

    cJSON *manifest = cJSON_Parse(manifest_buf);
    if (!manifest || !cJSON_IsObject(manifest)) {
        cJSON_Delete(root);
        cJSON_Delete(manifest);
        snprintf(output, output_size, "Error: invalid manifest JSON for device=%s", device);
        return ESP_ERR_INVALID_ARG;
    }

    char reason[160] = {0};
    virtual_control_device_t cfg;
    err = validate_manifest_header(manifest, device_name, "control_agent", "control", reason, sizeof(reason));
    if (err == ESP_OK) {
        err = parse_control_manifest(manifest, device_name, &cfg, reason, sizeof(reason));
    }
    if (err == ESP_OK) {
        err = execute_control_manifest(&cfg, root, output, output_size);
    }
    if (err != ESP_OK && output[0] == '\0') {
        snprintf(output, output_size, "Error: control manifest rejected for device=%s: %s",
                 device_name, reason[0] ? reason : esp_err_to_name(err));
    }

    cJSON_Delete(manifest);
    cJSON_Delete(root);
    return err;
}

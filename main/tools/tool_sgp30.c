#include "tools/tool_sgp30.h"
#include "drivers/sgp30.h"
#include "espagent_config.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_sgp30";

static sgp30_dev_t s_sgp30;
/**
 * @brief 从工具输入 JSON 中读取一个可选的整数参数。
 *
 * @param root 已解析完成的 cJSON 根对象。
 * @param key 要读取的字段名。
 * @param value 用于输出解析结果的整数指针。
 *
 * @return 若字段存在且为数值类型则返回 true，否则返回 false。
 */
static bool get_optional_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}


/**
 * @brief 执行 ESPAgent 的 SGP30 空气质量读取工具。
 *
 * 该函数会从 JSON 输入中解析可选的总线覆盖参数，校验 GPIO 配置，
 * 在首次使用时初始化 I2C 总线，缓存总线/设备句柄以及最近一次采样结果，
 * 最终返回预热状态文本或正常空气质量读数文本。
 *
 * @param input_json 包含可选 `sda_gpio`、`scl_gpio`、`i2c_port` 和 `scl_hz`
 * 覆盖参数的 JSON 字符串。
 * @param output 调用方提供的字符串缓冲区，用于接收最终工具返回文本。
 * @param output_size @p output 缓冲区大小，单位为字节。
 *
 * @retval ESP_OK 工具执行成功，@p output 中包含状态文本。
 * @retval ESP_ERR_INVALID_ARG 输入 JSON 或总线参数非法。
 * @retval ESP_ERR_INVALID_STATE 没有可用的 SDA/SCL 配置。
 * @retval ESP_ERR_NOT_FOUND 请求的总线上未发现 SGP30。
 * @retval ESP_ERR_INVALID_CRC 传感器返回数据的 CRC 校验失败。
 * @retval 其他 ESP-IDF 错误码 初始化或传输过程失败。
 */
esp_err_t tool_sgp30_read_air_quality_execute(const char *input_json, char *output, size_t output_size)
{
    if (!s_sgp30.bus && !s_sgp30.dev) {
        sgp30_dev_init_default(&s_sgp30);
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int sda_gpio = ESPAGENT_SGP30_DEFAULT_SDA_GPIO;
    int scl_gpio = ESPAGENT_SGP30_DEFAULT_SCL_GPIO;
    int i2c_port = ESPAGENT_SGP30_DEFAULT_I2C_PORT;
    int scl_hz = ESPAGENT_SGP30_DEFAULT_SCL_HZ;

    (void)get_optional_int(root, "sda_gpio", &sda_gpio);
    (void)get_optional_int(root, "scl_gpio", &scl_gpio);
    (void)get_optional_int(root, "i2c_port", &i2c_port);
    (void)get_optional_int(root, "scl_hz", &scl_hz);

    if (!sgp30_valid_gpio_pair(sda_gpio, scl_gpio)) {
        snprintf(output, output_size,
                 "Error: SGP30 SDA/SCL GPIOs are not configured. Set ESPAGENT_SECRET_SGP30_SDA_GPIO and ESPAGENT_SECRET_SGP30_SCL_GPIO, or call with {\"sda_gpio\":x,\"scl_gpio\":y}.");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    sgp30_config_t cfg = {
        .sda_gpio = sda_gpio,
        .scl_gpio = scl_gpio,
        .i2c_port = i2c_port,
        .scl_hz = scl_hz,
        .enable_internal_pullup = true,
    };

    esp_err_t err = sgp30_init(&s_sgp30, &cfg);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            snprintf(output, output_size,
                     "Error: SGP30 not found at I2C address 0x58 on SDA=%d SCL=%d", sda_gpio, scl_gpio);
        } else {
            snprintf(output, output_size,
                     "Error: failed to initialize SGP30 on SDA=%d SCL=%d (%s)",
                     sda_gpio, scl_gpio, esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return err;
    }

    sgp30_reading_t reading = {0};
    err = sgp30_read_air_quality(&s_sgp30, &reading);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to read SGP30 sample (%s)", esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    if (reading.warming_up) {
        snprintf(output, output_size,
                 "OK: SGP30 warming up on SDA=%d SCL=%d. CO2eq=%u ppm, TVOC=%u ppb, %.1f s remaining%s",
                 sda_gpio, scl_gpio,
                 (unsigned)reading.co2eq_ppm,
                 (unsigned)reading.tvoc_ppb,
                 (double)reading.warmup_remaining_us / 1000000.0,
                 reading.cached ? " (cached)" : "");
    } else {
        snprintf(output, output_size,
                 "OK: SGP30 air quality on SDA=%d SCL=%d -> CO2eq=%u ppm, TVOC=%u ppb%s",
                 sda_gpio, scl_gpio,
                 (unsigned)reading.co2eq_ppm,
                 (unsigned)reading.tvoc_ppb,
                 reading.cached ? " (cached)" : "");
    }

    ESP_LOGI(TAG, "sgp30_read_air_quality sda=%d scl=%d port=%d freq=%d -> %s",
             sda_gpio, scl_gpio, i2c_port, scl_hz, output);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_sgp30_monitor_start(void)
{
    /* Auto-monitor removed: SGP30 is read on-demand via agent tools only. */
    return ESP_OK;
}

#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * @brief 通过 I2C 读取 SGP30 空气质量传感器数据。
 *
 * 该工具支持通过 JSON 可选覆盖 I2C 引脚和总线参数：
 * `{ "sda_gpio"?, "scl_gpio"?, "i2c_port"?, "scl_hz"? }`。
 *
 * @param input_json 包含可选 GPIO 和 I2C 参数覆盖项的 JSON 字符串。
 * @param output 调用方提供的输出缓冲区，用于接收简短的人类可读结果。
 * @param output_size @p output 缓冲区大小，单位为字节。
 *
 * @retval ESP_OK 传感器读取成功，@p output 中包含读数结果。
 * @retval ESP_ERR_INVALID_ARG 输入 JSON 格式非法。
 * @retval ESP_ERR_INVALID_STATE 默认引脚未配置，且本次调用也未提供覆盖参数。
 * @retval ESP_ERR_NOT_FOUND 地址 `0x58` 上未发现 SGP30 设备。
 * @retval 其他 ESP-IDF 错误码 I2C 总线创建、探测或传输失败。
 */
esp_err_t tool_sgp30_read_air_quality_execute(const char *input_json, char *output, size_t output_size);

/* Start periodic SGP30 polling to print readings on the serial monitor. */
esp_err_t tool_sgp30_monitor_start(void);

#pragma once

#include "driver/gpio.h"
#include "driver/i2s_common.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int i2s_port;
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t sd_gpio;
    uint32_t sample_rate_hz;
    bool right_channel;
} ina_mic_config_t;

typedef struct {
    size_t samples;
    size_t nonzero_samples;
    size_t clipped_samples;
    int32_t mean;
    int32_t peak_abs;
    uint32_t rms;
} ina_mic_level_t;

typedef struct {
    ina_mic_config_t cfg;
    i2s_chan_handle_t rx_chan;
    bool active;
    size_t raw_bytes_in;
} ina_mic_stream_t;

void ina_mic_default_config(ina_mic_config_t *cfg);

esp_err_t ina_mic_stream_open(ina_mic_stream_t *stream,
                              const ina_mic_config_t *cfg,
                              char *diag,
                              size_t diag_size);

esp_err_t ina_mic_stream_read_level(ina_mic_stream_t *stream,
                                    uint32_t duration_ms,
                                    ina_mic_level_t *level,
                                    char *diag,
                                    size_t diag_size);

esp_err_t ina_mic_stream_close(ina_mic_stream_t *stream,
                               char *diag,
                               size_t diag_size);

esp_err_t ina_mic_capture_level(const ina_mic_config_t *cfg,
                                uint32_t duration_ms,
                                ina_mic_level_t *level,
                                char *diag,
                                size_t diag_size);

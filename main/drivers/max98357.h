#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/i2s_common.h"

typedef struct {
    int i2s_port;
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t din_gpio;
    gpio_num_t sd_gpio;
    uint32_t sample_rate_hz;
} max98357_config_t;

typedef struct {
    max98357_config_t cfg;
    i2s_chan_handle_t tx_chan;
    bool active;
    bool has_pending_byte;
    uint8_t pending_byte;
    size_t pcm_bytes_in;
    size_t i2s_bytes_out;
} max98357_stream_t;

void max98357_default_config(max98357_config_t *cfg);

esp_err_t max98357_play_tone(const max98357_config_t *cfg,
                             uint32_t frequency_hz,
                             uint32_t duration_ms,
                             uint8_t volume_pct,
                             char *diag,
                             size_t diag_size);

esp_err_t max98357_stream_open(max98357_stream_t *stream,
                               const max98357_config_t *cfg,
                               char *diag,
                               size_t diag_size);
esp_err_t max98357_stream_write_pcm_mono16le(max98357_stream_t *stream,
                                             const uint8_t *data,
                                             size_t len,
                                             char *diag,
                                             size_t diag_size);
esp_err_t max98357_stream_close(max98357_stream_t *stream,
                                char *diag,
                                size_t diag_size);

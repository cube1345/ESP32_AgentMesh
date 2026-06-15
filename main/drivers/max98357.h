#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int i2s_port;
    gpio_num_t bclk_gpio;
    gpio_num_t ws_gpio;
    gpio_num_t din_gpio;
    gpio_num_t sd_gpio;
    uint32_t sample_rate_hz;
} max98357_config_t;

void max98357_default_config(max98357_config_t *cfg);

esp_err_t max98357_play_tone(const max98357_config_t *cfg,
                             uint32_t frequency_hz,
                             uint32_t duration_ms,
                             uint8_t volume_pct,
                             char *diag,
                             size_t diag_size);

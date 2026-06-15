#include "drivers/max98357.h"

#include "espagent_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "max98357";

#define MAX98357_CHUNK_FRAMES 256
#define MAX98357_MAX_DURATION_MS 10000
#define MAX98357_MIN_SAMPLE_RATE_HZ 8000
#define MAX98357_MAX_SAMPLE_RATE_HZ 48000
#define MAX98357_PI 3.14159265358979323846f

void max98357_default_config(max98357_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    cfg->i2s_port = ESPAGENT_MAX98357_DEFAULT_I2S_PORT;
    cfg->bclk_gpio = (gpio_num_t)ESPAGENT_MAX98357_DEFAULT_BCLK_GPIO;
    cfg->ws_gpio = (gpio_num_t)ESPAGENT_MAX98357_DEFAULT_WS_GPIO;
    cfg->din_gpio = (gpio_num_t)ESPAGENT_MAX98357_DEFAULT_DIN_GPIO;
    cfg->sd_gpio = (gpio_num_t)ESPAGENT_MAX98357_DEFAULT_SD_GPIO;
    cfg->sample_rate_hz = ESPAGENT_MAX98357_DEFAULT_SAMPLE_RATE_HZ;
}

static bool valid_output_gpio(gpio_num_t gpio)
{
    return gpio >= 0 && gpio < GPIO_NUM_MAX;
}

static esp_err_t validate_config(const max98357_config_t *cfg, char *diag, size_t diag_size)
{
    if (!cfg) {
        snprintf(diag, diag_size, "Error: missing MAX98357 config");
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_output_gpio(cfg->bclk_gpio) ||
        !valid_output_gpio(cfg->ws_gpio) ||
        !valid_output_gpio(cfg->din_gpio)) {
        snprintf(diag, diag_size,
                 "Error: configure MAX98357 pins first: bclk_gpio, ws_gpio, din_gpio are required");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->sd_gpio != GPIO_NUM_NC && cfg->sd_gpio != (gpio_num_t)-1 &&
        !valid_output_gpio(cfg->sd_gpio)) {
        snprintf(diag, diag_size, "Error: invalid MAX98357 sd_gpio=%d", (int)cfg->sd_gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->i2s_port < I2S_NUM_AUTO || cfg->i2s_port > I2S_NUM_2) {
        snprintf(diag, diag_size, "Error: invalid I2S port %d", cfg->i2s_port);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->sample_rate_hz < MAX98357_MIN_SAMPLE_RATE_HZ ||
        cfg->sample_rate_hz > MAX98357_MAX_SAMPLE_RATE_HZ) {
        snprintf(diag, diag_size,
                 "Error: sample_rate_hz must be %d-%d, got %lu",
                 MAX98357_MIN_SAMPLE_RATE_HZ,
                 MAX98357_MAX_SAMPLE_RATE_HZ,
                 (unsigned long)cfg->sample_rate_hz);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t configure_shutdown_pin(gpio_num_t sd_gpio, bool enabled)
{
    if (sd_gpio == GPIO_NUM_NC || sd_gpio == (gpio_num_t)-1) {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << sd_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(sd_gpio, enabled ? 1 : 0);
}

static esp_err_t init_i2s_tx(const max98357_config_t *cfg, i2s_chan_handle_t *tx_chan)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(cfg->i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = MAX98357_CHUNK_FRAMES;

    esp_err_t err = i2s_new_channel(&chan_cfg, tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = cfg->bclk_gpio,
            .ws = cfg->ws_gpio,
            .dout = cfg->din_gpio,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(*tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(*tx_chan);
        *tx_chan = NULL;
        return err;
    }

    return ESP_OK;
}

static void fill_tone_chunk(int16_t *samples,
                            size_t frames,
                            uint32_t sample_rate_hz,
                            uint32_t frequency_hz,
                            uint8_t volume_pct,
                            float *phase)
{
    if (volume_pct > 100) {
        volume_pct = 100;
    }

    const float step = (2.0f * MAX98357_PI * (float)frequency_hz) / (float)sample_rate_hz;
    const int amplitude = (32767 * (int)volume_pct) / 100;

    for (size_t i = 0; i < frames; ++i) {
        int16_t sample = (int16_t)(sinf(*phase) * (float)amplitude);
        samples[i * 2] = sample;
        samples[i * 2 + 1] = sample;

        *phase += step;
        if (*phase >= 2.0f * MAX98357_PI) {
            *phase -= 2.0f * MAX98357_PI;
        }
    }
}

esp_err_t max98357_play_tone(const max98357_config_t *cfg,
                             uint32_t frequency_hz,
                             uint32_t duration_ms,
                             uint8_t volume_pct,
                             char *diag,
                             size_t diag_size)
{
    if (!diag || diag_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    diag[0] = '\0';

    esp_err_t err = validate_config(cfg, diag, diag_size);
    if (err != ESP_OK) {
        return err;
    }

    if (frequency_hz == 0) {
        frequency_hz = ESPAGENT_MAX98357_DEFAULT_TONE_HZ;
    }
    if (duration_ms == 0) {
        duration_ms = ESPAGENT_MAX98357_DEFAULT_DURATION_MS;
    }
    if (duration_ms > MAX98357_MAX_DURATION_MS) {
        duration_ms = MAX98357_MAX_DURATION_MS;
    }
    if (volume_pct > 100) {
        volume_pct = 100;
    }

    err = configure_shutdown_pin(cfg->sd_gpio, true);
    if (err != ESP_OK) {
        snprintf(diag, diag_size, "Error: MAX98357 SD GPIO init failed (%s)", esp_err_to_name(err));
        return err;
    }

    i2s_chan_handle_t tx_chan = NULL;
    err = init_i2s_tx(cfg, &tx_chan);
    if (err != ESP_OK) {
        configure_shutdown_pin(cfg->sd_gpio, false);
        snprintf(diag, diag_size, "Error: MAX98357 I2S init failed (%s)", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(tx_chan);
    if (err != ESP_OK) {
        i2s_del_channel(tx_chan);
        configure_shutdown_pin(cfg->sd_gpio, false);
        snprintf(diag, diag_size, "Error: MAX98357 I2S enable failed (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG,
             "Playing tone: freq=%luHz duration=%lums volume=%u%% sample_rate=%lu bclk=GPIO%d ws=GPIO%d din=GPIO%d sd=GPIO%d i2s=%d",
             (unsigned long)frequency_hz,
             (unsigned long)duration_ms,
             (unsigned)volume_pct,
             (unsigned long)cfg->sample_rate_hz,
             (int)cfg->bclk_gpio,
             (int)cfg->ws_gpio,
             (int)cfg->din_gpio,
             (int)cfg->sd_gpio,
             cfg->i2s_port);

    int16_t samples[MAX98357_CHUNK_FRAMES * 2];
    uint32_t total_frames = (cfg->sample_rate_hz * duration_ms) / 1000;
    uint32_t frames_written = 0;
    size_t total_bytes = 0;
    float phase = 0.0f;

    while (frames_written < total_frames) {
        uint32_t remain = total_frames - frames_written;
        size_t frames = remain < MAX98357_CHUNK_FRAMES ? remain : MAX98357_CHUNK_FRAMES;
        fill_tone_chunk(samples, frames, cfg->sample_rate_hz, frequency_hz, volume_pct, &phase);

        size_t bytes_written = 0;
        err = i2s_channel_write(tx_chan, samples, frames * 2 * sizeof(int16_t),
                                &bytes_written, 1000);
        if (err != ESP_OK) {
            break;
        }
        total_bytes += bytes_written;
        frames_written += frames;
    }

    memset(samples, 0, sizeof(samples));
    size_t silence_written = 0;
    i2s_channel_write(tx_chan, samples, sizeof(samples), &silence_written, 1000);
    vTaskDelay(pdMS_TO_TICKS(20));

    i2s_channel_disable(tx_chan);
    i2s_del_channel(tx_chan);
    configure_shutdown_pin(cfg->sd_gpio, false);

    if (err != ESP_OK) {
        snprintf(diag, diag_size,
                 "Error: MAX98357 tone write failed after %lu bytes (%s)",
                 (unsigned long)total_bytes,
                 esp_err_to_name(err));
        return err;
    }

    snprintf(diag, diag_size,
             "OK: MAX98357 tone %luHz %lums volume=%u%% sample_rate=%luHz bytes=%lu BCLK=GPIO%d WS=GPIO%d DIN=GPIO%d",
             (unsigned long)frequency_hz,
             (unsigned long)duration_ms,
             (unsigned)volume_pct,
             (unsigned long)cfg->sample_rate_hz,
             (unsigned long)total_bytes,
             (int)cfg->bclk_gpio,
             (int)cfg->ws_gpio,
             (int)cfg->din_gpio);
    ESP_LOGI(TAG, "%s", diag);
    return ESP_OK;
}

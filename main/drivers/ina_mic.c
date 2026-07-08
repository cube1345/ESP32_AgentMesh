#include "drivers/ina_mic.h"

#include "espagent_config.h"

#include <math.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ina_mic";

#define INA_MIC_CHUNK_SAMPLES 256
#define INA_MIC_MIN_SAMPLE_RATE_HZ 8000
#define INA_MIC_MAX_SAMPLE_RATE_HZ 48000
#define INA_MIC_MIN_DURATION_MS 50
#define INA_MIC_MAX_DURATION_MS 10000

static bool valid_input_gpio(gpio_num_t gpio)
{
    return gpio >= 0 && gpio < GPIO_NUM_MAX;
}

static esp_err_t validate_config(const ina_mic_config_t *cfg, char *diag, size_t diag_size)
{
    if (!cfg) {
        snprintf(diag, diag_size, "Error: missing INA mic config");
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_input_gpio(cfg->bclk_gpio) ||
        !valid_input_gpio(cfg->ws_gpio) ||
        !valid_input_gpio(cfg->sd_gpio)) {
        snprintf(diag, diag_size,
                 "Error: configure INA mic pins first: bclk_gpio, ws_gpio, sd_gpio are required");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->i2s_port < I2S_NUM_AUTO || cfg->i2s_port > I2S_NUM_2) {
        snprintf(diag, diag_size, "Error: invalid I2S port %d", cfg->i2s_port);
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->sample_rate_hz < INA_MIC_MIN_SAMPLE_RATE_HZ ||
        cfg->sample_rate_hz > INA_MIC_MAX_SAMPLE_RATE_HZ) {
        snprintf(diag, diag_size,
                 "Error: sample_rate_hz must be %d-%d, got %lu",
                 INA_MIC_MIN_SAMPLE_RATE_HZ,
                 INA_MIC_MAX_SAMPLE_RATE_HZ,
                 (unsigned long)cfg->sample_rate_hz);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t init_i2s_rx(const ina_mic_config_t *cfg, i2s_chan_handle_t *rx_chan)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(cfg->i2s_port, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = INA_MIC_CHUNK_SAMPLES;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(cfg->sample_rate_hz),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = cfg->bclk_gpio,
            .ws = cfg->ws_gpio,
            .dout = I2S_GPIO_UNUSED,
            .din = cfg->sd_gpio,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(*rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(*rx_chan);
        *rx_chan = NULL;
        return err;
    }

    return ESP_OK;
}

static void level_reset(ina_mic_level_t *level)
{
    if (level) {
        memset(level, 0, sizeof(*level));
    }
}

static void level_accumulate(ina_mic_level_t *level,
                             int32_t sample24,
                             int64_t *sum,
                             uint64_t *sum_sq)
{
    int64_t abs_sample;

    if (!level || !sum || !sum_sq) {
        return;
    }

    abs_sample = sample24 >= 0 ? (int64_t)sample24 : -(int64_t)sample24;
    level->samples++;
    if (sample24 != 0) {
        level->nonzero_samples++;
    }
    if (abs_sample > level->peak_abs) {
        level->peak_abs = (int32_t)abs_sample;
    }
    if (abs_sample >= 0x7FFFFF) {
        level->clipped_samples++;
    }
    *sum += sample24;
    *sum_sq += (uint64_t)(abs_sample * abs_sample);
}

static void level_finalize(ina_mic_level_t *level, int64_t sum, uint64_t sum_sq)
{
    if (!level || level->samples == 0) {
        return;
    }

    level->mean = (int32_t)(sum / (int64_t)level->samples);
    level->rms = (uint32_t)((double)sum_sq / (double)level->samples > 0.0
                                ? sqrt((double)sum_sq / (double)level->samples)
                                : 0.0);
}

void ina_mic_default_config(ina_mic_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    cfg->i2s_port = ESPAGENT_INA_MIC_DEFAULT_I2S_PORT;
    cfg->bclk_gpio = (gpio_num_t)ESPAGENT_INA_MIC_DEFAULT_BCLK_GPIO;
    cfg->ws_gpio = (gpio_num_t)ESPAGENT_INA_MIC_DEFAULT_WS_GPIO;
    cfg->sd_gpio = (gpio_num_t)ESPAGENT_INA_MIC_DEFAULT_SD_GPIO;
    cfg->sample_rate_hz = ESPAGENT_INA_MIC_DEFAULT_SAMPLE_RATE_HZ;
    cfg->right_channel = ESPAGENT_INA_MIC_DEFAULT_RIGHT_CHANNEL != 0;
}

esp_err_t ina_mic_stream_open(ina_mic_stream_t *stream,
                              const ina_mic_config_t *cfg,
                              char *diag,
                              size_t diag_size)
{
    esp_err_t err;

    if (!diag || diag_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    diag[0] = '\0';

    if (!stream || !cfg) {
        snprintf(diag, diag_size, "Error: missing INA mic stream config");
        return ESP_ERR_INVALID_ARG;
    }

    memset(stream, 0, sizeof(*stream));
    stream->cfg = *cfg;

    err = validate_config(&stream->cfg, diag, diag_size);
    if (err != ESP_OK) {
        return err;
    }

    err = init_i2s_rx(&stream->cfg, &stream->rx_chan);
    if (err != ESP_OK) {
        snprintf(diag, diag_size, "Error: INA mic I2S init failed (%s)", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(stream->rx_chan);
    if (err != ESP_OK) {
        i2s_del_channel(stream->rx_chan);
        stream->rx_chan = NULL;
        snprintf(diag, diag_size, "Error: INA mic I2S enable failed (%s)", esp_err_to_name(err));
        return err;
    }

    stream->active = true;
    snprintf(diag, diag_size,
             "OK: INA mic stream ready sample_rate=%luHz BCLK=GPIO%d WS=GPIO%d SD=GPIO%d channel=%s i2s=%d",
             (unsigned long)stream->cfg.sample_rate_hz,
             (int)stream->cfg.bclk_gpio,
             (int)stream->cfg.ws_gpio,
             (int)stream->cfg.sd_gpio,
             stream->cfg.right_channel ? "right" : "left",
             stream->cfg.i2s_port);
    ESP_LOGI(TAG, "%s", diag);
    return ESP_OK;
}

esp_err_t ina_mic_stream_read_level(ina_mic_stream_t *stream,
                                    uint32_t duration_ms,
                                    ina_mic_level_t *level,
                                    char *diag,
                                    size_t diag_size)
{
    int32_t *raw_buf = NULL;
    int64_t sum = 0;
    uint64_t sum_sq = 0;
    uint32_t elapsed_ms = 0;
    const uint32_t window_ms = 40;
    esp_err_t err = ESP_OK;

    if (!diag || diag_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    diag[0] = '\0';

    if (!stream || !stream->active || !stream->rx_chan) {
        snprintf(diag, diag_size, "Error: INA mic stream is not active");
        return ESP_ERR_INVALID_STATE;
    }

    if (!level) {
        snprintf(diag, diag_size, "Error: missing INA mic level output");
        return ESP_ERR_INVALID_ARG;
    }

    if (duration_ms < INA_MIC_MIN_DURATION_MS || duration_ms > INA_MIC_MAX_DURATION_MS) {
        snprintf(diag, diag_size,
                 "Error: duration_ms must be %d-%d, got %lu",
                 INA_MIC_MIN_DURATION_MS,
                 INA_MIC_MAX_DURATION_MS,
                 (unsigned long)duration_ms);
        return ESP_ERR_INVALID_ARG;
    }

    raw_buf = heap_caps_calloc(INA_MIC_CHUNK_SAMPLES * 2, sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!raw_buf) {
        snprintf(diag, diag_size, "Error: no memory for INA mic capture buffer");
        return ESP_ERR_NO_MEM;
    }

    level_reset(level);

    while (elapsed_ms < duration_ms) {
        size_t bytes_read = 0;
        size_t sample_count = 0;

        err = i2s_channel_read(stream->rx_chan,
                               raw_buf,
                               INA_MIC_CHUNK_SAMPLES * 2 * sizeof(int32_t),
                               &bytes_read,
                               pdMS_TO_TICKS(window_ms));
        if (err == ESP_ERR_TIMEOUT) {
            elapsed_ms += window_ms;
            continue;
        }
        if (err != ESP_OK) {
            snprintf(diag, diag_size, "Error: INA mic read failed (%s)", esp_err_to_name(err));
            free(raw_buf);
            return err;
        }

        stream->raw_bytes_in += bytes_read;
        sample_count = bytes_read / sizeof(int32_t);
        for (size_t i = stream->cfg.right_channel ? 1 : 0; i < sample_count; i += 2) {
            int32_t sample24 = raw_buf[i] >> 8;
            level_accumulate(level, sample24, &sum, &sum_sq);
        }
        elapsed_ms += window_ms;
    }

    level_finalize(level, sum, sum_sq);
    snprintf(diag, diag_size,
             "OK: INA mic level samples=%u nonzero=%u rms=%lu peak=%ld mean=%ld clipped=%u raw_bytes=%u",
             (unsigned)level->samples,
             (unsigned)level->nonzero_samples,
             (unsigned long)level->rms,
             (long)level->peak_abs,
             (long)level->mean,
             (unsigned)level->clipped_samples,
             (unsigned)stream->raw_bytes_in);
    ESP_LOGI(TAG, "%s", diag);
    free(raw_buf);
    return ESP_OK;
}

esp_err_t ina_mic_stream_capture_pcm16(ina_mic_stream_t *stream,
                                       uint32_t duration_ms,
                                       ina_mic_pcm16_writer_t writer,
                                       void *ctx,
                                       size_t *samples_written,
                                       char *diag,
                                       size_t diag_size)
{
    int32_t *raw_buf = NULL;
    int16_t *pcm_buf = NULL;
    uint32_t elapsed_ms = 0;
    const uint32_t window_ms = 40;
    size_t total_samples = 0;
    esp_err_t err = ESP_OK;

    if (!diag || diag_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    diag[0] = '\0';

    if (!stream || !stream->active || !stream->rx_chan) {
        snprintf(diag, diag_size, "Error: INA mic stream is not active");
        return ESP_ERR_INVALID_STATE;
    }
    if (!writer) {
        snprintf(diag, diag_size, "Error: missing INA mic PCM writer");
        return ESP_ERR_INVALID_ARG;
    }
    if (duration_ms < INA_MIC_MIN_DURATION_MS || duration_ms > INA_MIC_MAX_DURATION_MS) {
        snprintf(diag, diag_size,
                 "Error: duration_ms must be %d-%d, got %lu",
                 INA_MIC_MIN_DURATION_MS,
                 INA_MIC_MAX_DURATION_MS,
                 (unsigned long)duration_ms);
        return ESP_ERR_INVALID_ARG;
    }

    raw_buf = heap_caps_calloc(INA_MIC_CHUNK_SAMPLES * 2, sizeof(int32_t), MALLOC_CAP_SPIRAM);
    pcm_buf = heap_caps_calloc(INA_MIC_CHUNK_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!raw_buf || !pcm_buf) {
        free(raw_buf);
        free(pcm_buf);
        snprintf(diag, diag_size, "Error: no memory for INA mic PCM capture");
        return ESP_ERR_NO_MEM;
    }

    while (elapsed_ms < duration_ms) {
        size_t bytes_read = 0;
        size_t pcm_count = 0;
        size_t sample_count = 0;

        err = i2s_channel_read(stream->rx_chan,
                               raw_buf,
                               INA_MIC_CHUNK_SAMPLES * 2 * sizeof(int32_t),
                               &bytes_read,
                               pdMS_TO_TICKS(window_ms));
        if (err == ESP_ERR_TIMEOUT) {
            elapsed_ms += window_ms;
            continue;
        }
        if (err != ESP_OK) {
            snprintf(diag, diag_size, "Error: INA mic read failed (%s)", esp_err_to_name(err));
            free(raw_buf);
            free(pcm_buf);
            return err;
        }

        stream->raw_bytes_in += bytes_read;
        sample_count = bytes_read / sizeof(int32_t);
        for (size_t i = stream->cfg.right_channel ? 1 : 0; i < sample_count; i += 2) {
            int32_t sample24 = raw_buf[i] >> 8;
            pcm_buf[pcm_count++] = (int16_t)(sample24 >> 8);
        }

        if (pcm_count > 0) {
            err = writer(pcm_buf, pcm_count, ctx);
            if (err != ESP_OK) {
                snprintf(diag, diag_size, "Error: INA mic PCM writer failed (%s)",
                         esp_err_to_name(err));
                free(raw_buf);
                free(pcm_buf);
                return err;
            }
            total_samples += pcm_count;
        }
        elapsed_ms += window_ms;
    }

    if (samples_written) {
        *samples_written = total_samples;
    }
    snprintf(diag, diag_size,
             "OK: INA mic PCM captured samples=%u raw_bytes=%u sample_rate=%lu",
             (unsigned)total_samples,
             (unsigned)stream->raw_bytes_in,
             (unsigned long)stream->cfg.sample_rate_hz);
    ESP_LOGI(TAG, "%s", diag);
    free(raw_buf);
    free(pcm_buf);
    return ESP_OK;
}

esp_err_t ina_mic_stream_close(ina_mic_stream_t *stream,
                               char *diag,
                               size_t diag_size)
{
    if (!diag || diag_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    diag[0] = '\0';

    if (!stream) {
        snprintf(diag, diag_size, "Error: missing INA mic stream");
        return ESP_ERR_INVALID_ARG;
    }

    if (stream->rx_chan) {
        (void)i2s_channel_disable(stream->rx_chan);
        i2s_del_channel(stream->rx_chan);
        stream->rx_chan = NULL;
    }
    stream->active = false;

    snprintf(diag, diag_size,
             "OK: INA mic stream closed raw_bytes=%u",
             (unsigned)stream->raw_bytes_in);
    ESP_LOGI(TAG, "%s", diag);
    return ESP_OK;
}

esp_err_t ina_mic_capture_level(const ina_mic_config_t *cfg,
                                uint32_t duration_ms,
                                ina_mic_level_t *level,
                                char *diag,
                                size_t diag_size)
{
    ina_mic_stream_t stream;
    char close_diag[128] = {0};
    esp_err_t err = ina_mic_stream_open(&stream, cfg, diag, diag_size);
    if (err != ESP_OK) {
        return err;
    }

    err = ina_mic_stream_read_level(&stream, duration_ms, level, diag, diag_size);
    (void)ina_mic_stream_close(&stream, close_diag, sizeof(close_diag));
    return err;
}

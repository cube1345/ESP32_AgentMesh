#include "drivers/gree_ir.h"

#include "espagent_config.h"

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "gree_ir";

#define GREE_FRAME_BYTES 8
#define GREE_TOTAL_SYMBOLS 69

#define GREE_HDR_MARK_US 9000
#define GREE_HDR_SPACE_US 4500
#define GREE_BIT_MARK_US 620
#define GREE_ONE_SPACE_US 1600
#define GREE_ZERO_SPACE_US 540
#define GREE_MSG_SPACE_US 19980

#define GREE_MODE_MASK 0x07
#define GREE_POWER_MASK 0x08
#define GREE_FAN_MASK 0x30
#define GREE_SWING_AUTO_MASK 0x40
#define GREE_TEMP_MASK 0x0F
#define GREE_MODEL_A_MASK 0x40
#define GREE_SWING_V_MASK 0x0F
#define GREE_ECONO_MASK 0x04

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    int gpio_num;
} gree_ir_runtime_t;

static gree_ir_runtime_t s_runtime = {
    .channel = NULL,
    .encoder = NULL,
    .gpio_num = -1,
};

static const rmt_symbol_word_t s_header_symbol = {
    .level0 = 1,
    .duration0 = GREE_HDR_MARK_US,
    .level1 = 0,
    .duration1 = GREE_HDR_SPACE_US,
};

static const rmt_symbol_word_t s_zero_symbol = {
    .level0 = 1,
    .duration0 = GREE_BIT_MARK_US,
    .level1 = 0,
    .duration1 = GREE_ZERO_SPACE_US,
};

static const rmt_symbol_word_t s_one_symbol = {
    .level0 = 1,
    .duration0 = GREE_BIT_MARK_US,
    .level1 = 0,
    .duration1 = GREE_ONE_SPACE_US,
};

static const rmt_symbol_word_t s_end_symbol = {
    .level0 = 1,
    .duration0 = GREE_BIT_MARK_US,
    .level1 = 0,
    .duration1 = GREE_MSG_SPACE_US,
};

void gree_ir_default_config(gree_ir_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    cfg->tx_gpio = ESPAGENT_GREE_IR_TX_GPIO;
    cfg->carrier_hz = ESPAGENT_GREE_IR_CARRIER_HZ;
    cfg->carrier_duty_cycle = ESPAGENT_GREE_IR_CARRIER_DUTY_CYCLE;
    cfg->resolution_hz = ESPAGENT_GREE_IR_RMT_RESOLUTION_HZ;
    cfg->repeat_count = ESPAGENT_GREE_IR_REPEAT_COUNT;
}

void gree_ir_default_state(gree_ir_state_t *state)
{
    if (!state) {
        return;
    }

    state->power_on = false;
    state->mode = GREE_IR_MODE_COOL;
    state->temp_c = 26;
    state->fan = GREE_IR_FAN_AUTO;
    state->swing_on = false;
}

void gree_ir_normalize_state(gree_ir_state_t *state)
{
    if (!state) {
        return;
    }

    if (state->temp_c < 16) {
        state->temp_c = 16;
    } else if (state->temp_c > 30) {
        state->temp_c = 30;
    }

    if (state->mode == GREE_IR_MODE_AUTO) {
        state->temp_c = 25;
    }
    if (state->mode == GREE_IR_MODE_DRY) {
        state->fan = GREE_IR_FAN_LOW;
    }
}

static uint8_t gree_checksum(const uint8_t frame[GREE_FRAME_BYTES])
{
    uint8_t sum = 10;
    for (int i = 0; i < 4; i++) {
        sum = (uint8_t)(sum + (frame[i] & 0x0F));
    }
    for (int i = 4; i < GREE_FRAME_BYTES - 1; i++) {
        sum = (uint8_t)(sum + (frame[i] >> 4));
    }
    return sum & 0x0F;
}

static void gree_build_frame(const gree_ir_state_t *state, uint8_t frame[GREE_FRAME_BYTES])
{
    gree_ir_state_t normalized = *state;
    gree_ir_normalize_state(&normalized);

    memset(frame, 0, GREE_FRAME_BYTES);
    frame[1] = 0x09;
    frame[2] = 0x20;
    frame[3] = 0x50;
    frame[5] = 0x20;

    frame[0] = (uint8_t)(frame[0] & ~(GREE_MODE_MASK | GREE_POWER_MASK |
                                      GREE_FAN_MASK | GREE_SWING_AUTO_MASK));
    frame[0] |= (uint8_t)(normalized.mode & GREE_MODE_MASK);
    frame[0] |= (uint8_t)((normalized.fan & 0x03U) << 4);
    if (normalized.power_on) {
        frame[0] |= GREE_POWER_MASK;
    }
    if (normalized.swing_on) {
        frame[0] |= GREE_SWING_AUTO_MASK;
    }

    frame[1] = (uint8_t)((frame[1] & ~GREE_TEMP_MASK) |
                         ((normalized.temp_c - 16) & GREE_TEMP_MASK));

    if (normalized.power_on) {
        frame[2] |= GREE_MODEL_A_MASK;
    } else {
        frame[2] &= (uint8_t)~GREE_MODEL_A_MASK;
    }

    frame[4] = (uint8_t)(frame[4] & ~GREE_SWING_V_MASK);
    frame[4] |= normalized.swing_on ? 0x01 : 0x00;

    frame[7] &= (uint8_t)~0xF0U;
    frame[7] |= (uint8_t)(gree_checksum(frame) << 4);
    frame[7] &= (uint8_t)~GREE_ECONO_MASK;
}

static void format_frame_hex(const uint8_t frame[GREE_FRAME_BYTES], char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return;
    }

    snprintf(buf, buf_size,
             "%02X %02X %02X %02X %02X %02X %02X %02X",
             frame[0], frame[1], frame[2], frame[3],
             frame[4], frame[5], frame[6], frame[7]);
}

static void gree_append_bits_lsb(rmt_symbol_word_t *symbols, size_t *index, uint8_t value, int bit_count)
{
    for (int bit = 0; bit < bit_count; bit++) {
        symbols[(*index)++] = (value & (1U << bit)) ? s_one_symbol : s_zero_symbol;
    }
}

static void gree_frame_to_symbols(const uint8_t frame[GREE_FRAME_BYTES],
                                  rmt_symbol_word_t symbols[GREE_TOTAL_SYMBOLS])
{
    size_t index = 0;
    symbols[index++] = s_header_symbol;
    for (int i = 0; i < 4; i++) {
        gree_append_bits_lsb(symbols, &index, frame[i], 8);
    }
    gree_append_bits_lsb(symbols, &index, 0x02, 3);
    for (int i = 4; i < GREE_FRAME_BYTES; i++) {
        gree_append_bits_lsb(symbols, &index, frame[i], 8);
    }
    symbols[index++] = s_end_symbol;
}

static size_t gree_encoder_callback(const void *data,
                                    size_t data_size,
                                    size_t symbols_written,
                                    size_t symbols_free,
                                    rmt_symbol_word_t *symbols,
                                    bool *done,
                                    void *arg)
{
    (void)arg;

    const rmt_symbol_word_t *src = (const rmt_symbol_word_t *)data;
    size_t total_symbols = data_size / sizeof(rmt_symbol_word_t);
    if (symbols_written >= total_symbols) {
        *done = true;
        return 0;
    }

    size_t remaining = total_symbols - symbols_written;
    size_t chunk = remaining < symbols_free ? remaining : symbols_free;
    memcpy(symbols, src + symbols_written, chunk * sizeof(rmt_symbol_word_t));
    if (chunk == remaining) {
        *done = true;
    }
    return chunk;
}

static void gree_ir_release(void)
{
    if (s_runtime.channel) {
        rmt_disable(s_runtime.channel);
    }
    if (s_runtime.encoder) {
        rmt_del_encoder(s_runtime.encoder);
        s_runtime.encoder = NULL;
    }
    if (s_runtime.channel) {
        rmt_del_channel(s_runtime.channel);
        s_runtime.channel = NULL;
    }
    s_runtime.gpio_num = -1;
}

static esp_err_t gree_ir_ensure_ready(const gree_ir_config_t *cfg, char *diag, size_t diag_size)
{
    if (!cfg) {
        snprintf(diag, diag_size, "Error: missing Gree IR config");
        return ESP_ERR_INVALID_ARG;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(cfg->tx_gpio)) {
        snprintf(diag, diag_size,
                 "Error: configure ESPAGENT_SECRET_GREE_IR_TX_GPIO or provide a valid tx_gpio override");
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->carrier_hz == 0 || cfg->resolution_hz == 0 ||
        cfg->carrier_duty_cycle <= 0.0f || cfg->carrier_duty_cycle >= 1.0f) {
        snprintf(diag, diag_size, "Error: invalid Gree IR carrier or RMT configuration");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_runtime.channel && s_runtime.encoder && s_runtime.gpio_num == cfg->tx_gpio) {
        return ESP_OK;
    }

    if (s_runtime.channel || s_runtime.encoder) {
        gree_ir_release();
    }

    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = cfg->tx_gpio,
        .mem_block_symbols = 128,
        .resolution_hz = cfg->resolution_hz,
        .trans_queue_depth = 1,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_cfg, &s_runtime.channel);
    if (err != ESP_OK) {
        snprintf(diag, diag_size, "Error: RMT TX channel init failed (%s)", esp_err_to_name(err));
        gree_ir_release();
        return err;
    }

    rmt_carrier_config_t carrier_cfg = {
        .frequency_hz = cfg->carrier_hz,
        .duty_cycle = cfg->carrier_duty_cycle,
        .flags.polarity_active_low = false,
        .flags.always_on = false,
    };
    err = rmt_apply_carrier(s_runtime.channel, &carrier_cfg);
    if (err != ESP_OK) {
        snprintf(diag, diag_size, "Error: RMT carrier apply failed (%s)", esp_err_to_name(err));
        gree_ir_release();
        return err;
    }

    rmt_simple_encoder_config_t encoder_cfg = {
        .callback = gree_encoder_callback,
        .min_chunk_size = 1,
    };
    err = rmt_new_simple_encoder(&encoder_cfg, &s_runtime.encoder);
    if (err != ESP_OK) {
        snprintf(diag, diag_size, "Error: RMT encoder init failed (%s)", esp_err_to_name(err));
        gree_ir_release();
        return err;
    }

    err = rmt_enable(s_runtime.channel);
    if (err != ESP_OK) {
        snprintf(diag, diag_size, "Error: RMT enable failed (%s)", esp_err_to_name(err));
        gree_ir_release();
        return err;
    }

    s_runtime.gpio_num = cfg->tx_gpio;
    return ESP_OK;
}

esp_err_t gree_ir_send(const gree_ir_config_t *cfg,
                       const gree_ir_state_t *state,
                       char *diag,
                       size_t diag_size)
{
    if (!diag || diag_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    diag[0] = '\0';

    if (!state) {
        snprintf(diag, diag_size, "Error: missing Gree IR state");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = gree_ir_ensure_ready(cfg, diag, diag_size);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t frame[GREE_FRAME_BYTES] = {0};
    gree_build_frame(state, frame);

    rmt_symbol_word_t symbols[GREE_TOTAL_SYMBOLS] = {0};
    gree_frame_to_symbols(frame, symbols);

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };

    for (uint8_t i = 0; i <= cfg->repeat_count; i++) {
        err = rmt_transmit(s_runtime.channel,
                           s_runtime.encoder,
                           symbols,
                           sizeof(symbols),
                           &tx_cfg);
        if (err == ESP_OK) {
            err = rmt_tx_wait_all_done(s_runtime.channel, 200);
        }
        if (err != ESP_OK) {
            snprintf(diag, diag_size, "Error: Gree IR transmit failed (%s)", esp_err_to_name(err));
            return err;
        }
    }

    char frame_hex[3 * GREE_FRAME_BYTES] = {0};
    format_frame_hex(frame, frame_hex, sizeof(frame_hex));
    snprintf(diag, diag_size,
             "OK: Gree IR sent on GPIO%d frame=%s repeat=%u",
             cfg->tx_gpio,
             frame_hex,
             (unsigned)cfg->repeat_count);
    ESP_LOGI(TAG, "%s", diag);
    return ESP_OK;
}

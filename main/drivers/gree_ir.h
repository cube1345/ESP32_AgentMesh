#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GREE_IR_MODE_AUTO = 0,
    GREE_IR_MODE_COOL = 1,
    GREE_IR_MODE_DRY = 2,
    GREE_IR_MODE_FAN = 3,
    GREE_IR_MODE_HEAT = 4,
} gree_ir_mode_t;

typedef enum {
    GREE_IR_FAN_AUTO = 0,
    GREE_IR_FAN_LOW = 1,
    GREE_IR_FAN_MEDIUM = 2,
    GREE_IR_FAN_HIGH = 3,
} gree_ir_fan_t;

typedef struct {
    bool power_on;
    gree_ir_mode_t mode;
    uint8_t temp_c;
    gree_ir_fan_t fan;
    bool swing_on;
} gree_ir_state_t;

typedef struct {
    int tx_gpio;
    uint32_t carrier_hz;
    float carrier_duty_cycle;
    uint32_t resolution_hz;
    uint8_t repeat_count;
} gree_ir_config_t;

void gree_ir_default_config(gree_ir_config_t *cfg);
void gree_ir_default_state(gree_ir_state_t *state);
void gree_ir_normalize_state(gree_ir_state_t *state);

esp_err_t gree_ir_send(const gree_ir_config_t *cfg,
                       const gree_ir_state_t *state,
                       char *diag,
                       size_t diag_size);

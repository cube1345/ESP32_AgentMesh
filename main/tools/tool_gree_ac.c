#include "tools/tool_gree_ac.h"

#include "drivers/gree_ir.h"

#include "cJSON.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "tool_gree_ac";

static bool s_state_ready = false;
static gree_ir_state_t s_cached_state;

static const char *mode_name(gree_ir_mode_t mode)
{
    switch (mode) {
    case GREE_IR_MODE_AUTO: return "auto";
    case GREE_IR_MODE_COOL: return "cool";
    case GREE_IR_MODE_DRY: return "dry";
    case GREE_IR_MODE_FAN: return "fan";
    case GREE_IR_MODE_HEAT: return "heat";
    default: return "unknown";
    }
}

static const char *fan_name(gree_ir_fan_t fan)
{
    switch (fan) {
    case GREE_IR_FAN_AUTO: return "auto";
    case GREE_IR_FAN_LOW: return "low";
    case GREE_IR_FAN_MEDIUM: return "medium";
    case GREE_IR_FAN_HIGH: return "high";
    default: return "unknown";
    }
}

static bool parse_on_off_item(cJSON *item, bool *value)
{
    if (!item || !value) {
        return false;
    }
    if (cJSON_IsBool(item)) {
        *value = cJSON_IsTrue(item);
        return true;
    }
    if (!cJSON_IsString(item)) {
        return false;
    }

    const char *text = item->valuestring;
    if (!text) {
        return false;
    }
    if (strcasecmp(text, "on") == 0 || strcasecmp(text, "true") == 0) {
        *value = true;
        return true;
    }
    if (strcasecmp(text, "off") == 0 || strcasecmp(text, "false") == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool parse_mode_item(cJSON *item, gree_ir_mode_t *mode)
{
    if (!item || !mode || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    const char *text = item->valuestring;
    if (strcasecmp(text, "auto") == 0) {
        *mode = GREE_IR_MODE_AUTO;
        return true;
    }
    if (strcasecmp(text, "cool") == 0) {
        *mode = GREE_IR_MODE_COOL;
        return true;
    }
    if (strcasecmp(text, "dry") == 0) {
        *mode = GREE_IR_MODE_DRY;
        return true;
    }
    if (strcasecmp(text, "fan") == 0) {
        *mode = GREE_IR_MODE_FAN;
        return true;
    }
    if (strcasecmp(text, "heat") == 0) {
        *mode = GREE_IR_MODE_HEAT;
        return true;
    }
    return false;
}

static bool parse_fan_item(cJSON *item, gree_ir_fan_t *fan)
{
    if (!item || !fan || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    const char *text = item->valuestring;
    if (strcasecmp(text, "auto") == 0) {
        *fan = GREE_IR_FAN_AUTO;
        return true;
    }
    if (strcasecmp(text, "low") == 0) {
        *fan = GREE_IR_FAN_LOW;
        return true;
    }
    if (strcasecmp(text, "medium") == 0 || strcasecmp(text, "mid") == 0) {
        *fan = GREE_IR_FAN_MEDIUM;
        return true;
    }
    if (strcasecmp(text, "high") == 0) {
        *fan = GREE_IR_FAN_HIGH;
        return true;
    }
    return false;
}

static int json_int_or_default(cJSON *root, const char *key, int default_value)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsNumber(item) ? item->valueint : default_value;
}

static void ensure_cached_state(void)
{
    if (s_state_ready) {
        return;
    }
    gree_ir_default_state(&s_cached_state);
    s_state_ready = true;
}

esp_err_t tool_gree_ac_control_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    ensure_cached_state();
    gree_ir_state_t next = s_cached_state;
    bool changed = false;
    bool has_non_power_change = false;
    bool power_present = false;

    cJSON *power_item = cJSON_GetObjectItem(root, "power");
    if (power_item) {
        bool power_on = false;
        if (!parse_on_off_item(power_item, &power_on)) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: power must be on/off or true/false");
            return ESP_ERR_INVALID_ARG;
        }
        next.power_on = power_on;
        power_present = true;
        changed = true;
    }

    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    if (mode_item) {
        gree_ir_mode_t mode = GREE_IR_MODE_COOL;
        if (!parse_mode_item(mode_item, &mode)) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: mode must be auto/cool/dry/fan/heat");
            return ESP_ERR_INVALID_ARG;
        }
        next.mode = mode;
        changed = true;
        has_non_power_change = true;
    }

    cJSON *temp_item = cJSON_GetObjectItem(root, "temp_c");
    if (temp_item) {
        if (!cJSON_IsNumber(temp_item)) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: temp_c must be an integer 16-30");
            return ESP_ERR_INVALID_ARG;
        }
        next.temp_c = (uint8_t)temp_item->valueint;
        changed = true;
        has_non_power_change = true;
    }

    cJSON *delta_item = cJSON_GetObjectItem(root, "temp_delta");
    if (delta_item) {
        if (!cJSON_IsNumber(delta_item)) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: temp_delta must be an integer");
            return ESP_ERR_INVALID_ARG;
        }
        int next_temp = (int)next.temp_c + delta_item->valueint;
        if (next_temp < 16) {
            next_temp = 16;
        } else if (next_temp > 30) {
            next_temp = 30;
        }
        next.temp_c = (uint8_t)next_temp;
        changed = true;
        has_non_power_change = true;
    }

    cJSON *fan_item = cJSON_GetObjectItem(root, "fan");
    if (fan_item) {
        gree_ir_fan_t fan = GREE_IR_FAN_AUTO;
        if (!parse_fan_item(fan_item, &fan)) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: fan must be auto/low/medium/high");
            return ESP_ERR_INVALID_ARG;
        }
        next.fan = fan;
        changed = true;
        has_non_power_change = true;
    }

    cJSON *swing_item = cJSON_GetObjectItem(root, "swing");
    if (swing_item) {
        bool swing_on = false;
        if (!parse_on_off_item(swing_item, &swing_on)) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: swing must be on/off or true/false");
            return ESP_ERR_INVALID_ARG;
        }
        next.swing_on = swing_on;
        changed = true;
        has_non_power_change = true;
    }

    if (!changed) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "Error: provide at least one of power, mode, temp_c, temp_delta, fan, or swing");
        return ESP_ERR_INVALID_ARG;
    }

    if (!power_present && has_non_power_change) {
        next.power_on = true;
    }

    int tx_gpio = json_int_or_default(root, "tx_gpio", -1);
    int repeat_count = json_int_or_default(root, "repeat_count", -1);
    cJSON_Delete(root);

    gree_ir_normalize_state(&next);

    gree_ir_config_t cfg;
    gree_ir_default_config(&cfg);
    if (tx_gpio >= 0) {
        cfg.tx_gpio = tx_gpio;
    }
    if (repeat_count >= 0) {
        if (repeat_count > 4) {
            repeat_count = 4;
        }
        cfg.repeat_count = (uint8_t)repeat_count;
    }

    esp_err_t err = gree_ir_send(&cfg, &next, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    s_cached_state = next;
    char driver_status[192] = {0};
    snprintf(driver_status, sizeof(driver_status), "%s", output);
    snprintf(output, output_size,
             "OK: gree_ac_control power=%s mode=%s temp=%uC fan=%s swing=%s tx_gpio=%d repeat=%u [%s]",
             next.power_on ? "on" : "off",
             mode_name(next.mode),
             (unsigned)next.temp_c,
             fan_name(next.fan),
             next.swing_on ? "on" : "off",
             cfg.tx_gpio,
             (unsigned)cfg.repeat_count,
             driver_status);
    ESP_LOGI(TAG, "%s", output);
    return ESP_OK;
}

#include "tools/tool_max98357.h"

#include "drivers/max98357.h"
#include "espagent_config.h"

#include <stdio.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "tool_max98357";

static int json_int_or_default(cJSON *root, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return default_value;
}

esp_err_t tool_max98357_play_tone_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    max98357_config_t cfg;
    max98357_default_config(&cfg);

    cfg.bclk_gpio = (gpio_num_t)json_int_or_default(root, "bclk_gpio", cfg.bclk_gpio);
    cfg.ws_gpio = (gpio_num_t)json_int_or_default(root, "ws_gpio", cfg.ws_gpio);
    cfg.din_gpio = (gpio_num_t)json_int_or_default(root, "din_gpio", cfg.din_gpio);
    cfg.sd_gpio = (gpio_num_t)json_int_or_default(root, "sd_gpio", cfg.sd_gpio);
    cfg.i2s_port = json_int_or_default(root, "i2s_port", cfg.i2s_port);
    cfg.sample_rate_hz = (uint32_t)json_int_or_default(root, "sample_rate_hz", cfg.sample_rate_hz);

    uint32_t frequency_hz = (uint32_t)json_int_or_default(root, "frequency_hz",
                                                          ESPAGENT_MAX98357_DEFAULT_TONE_HZ);
    uint32_t duration_ms = (uint32_t)json_int_or_default(root, "duration_ms",
                                                         ESPAGENT_MAX98357_DEFAULT_DURATION_MS);
    uint8_t volume_pct = (uint8_t)json_int_or_default(root, "volume_pct",
                                                      ESPAGENT_MAX98357_DEFAULT_VOLUME_PCT);

    esp_err_t err = max98357_play_tone(&cfg, frequency_hz, duration_ms, volume_pct,
                                       output, output_size);
    ESP_LOGI(TAG, "max98357_play_tone -> %s", esp_err_to_name(err));
    cJSON_Delete(root);
    return err;
}

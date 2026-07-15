#include "tools/tool_sensor_history.h"

#include "espagent_config.h"
#include "sensors/sensor_history.h"

#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>

static uint32_t json_u32(cJSON *root, const char *key, uint32_t fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return fallback;
    }
    return (uint32_t)item->valuedouble;
}

esp_err_t tool_env_history_recent_execute(const char *input_json,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t limit = json_u32(root, "limit", 12);
    cJSON_Delete(root);
    return sensor_history_build_recent(limit, output, output_size);
}

esp_err_t tool_env_history_summary_execute(const char *input_json,
                                           char *output,
                                           size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t hours = json_u32(root, "hours", 24);
    cJSON_Delete(root);
    return sensor_history_build_summary(hours, output, output_size);
}

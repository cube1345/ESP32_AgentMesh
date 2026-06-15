#include "roles/sensor_node.h"

#include "esp_log.h"
#include "node/node_profile.h"
#include "roles/role_config.h"

static const char *TAG = "sensor_node";

esp_err_t sensor_node_init(void)
{
    if (!espagent_role_runs_sensor_sampling()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Sensor role enabled: node=%s role=%s capabilities=%s",
             espagent_node_id(), espagent_node_role(), espagent_node_capabilities());
    return ESP_OK;
}

esp_err_t sensor_node_start(void)
{
    if (!espagent_role_runs_sensor_sampling()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Sensor service boundary active: sampling, telemetry, local thresholds");
    return ESP_OK;
}

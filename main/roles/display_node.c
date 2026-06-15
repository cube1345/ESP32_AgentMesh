#include "roles/display_node.h"

#include "esp_log.h"
#include "node/node_profile.h"
#include "roles/role_config.h"

static const char *TAG = "display_node";

esp_err_t display_node_init(void)
{
    if (!espagent_role_runs_display_outputs()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Display role enabled: node=%s role=%s capabilities=%s",
             espagent_node_id(), espagent_node_role(), espagent_node_capabilities());
    return ESP_OK;
}

esp_err_t display_node_start(void)
{
    if (!espagent_role_runs_display_outputs()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Display service boundary active: state, alerts, timeline, watchdog");
    return ESP_OK;
}

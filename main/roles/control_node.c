#include "roles/control_node.h"

#include "esp_log.h"
#include "node/node_profile.h"
#include "roles/role_config.h"

static const char *TAG = "control_node";

esp_err_t control_node_init(void)
{
    if (!espagent_role_runs_control_outputs()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Control role enabled: node=%s role=%s capabilities=%s",
             espagent_node_id(), espagent_node_role(), espagent_node_capabilities());
    return ESP_OK;
}

esp_err_t control_node_start(void)
{
    if (!espagent_role_runs_control_outputs()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Control service boundary active: command queue and safety interlock are required before remote execution");
    return ESP_OK;
}

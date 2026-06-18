#include "roles/guardian_node.h"

#include "esp_log.h"
#include "node/node_profile.h"
#include "roles/role_config.h"

static const char *TAG = "guardian_node";

esp_err_t guardian_node_init(void)
{
    if (!espagent_role_runs_guardian()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Guardian role enabled: node=%s role=%s capabilities=%s",
             espagent_node_id(), espagent_node_role(), espagent_node_capabilities());
    return ESP_OK;
}

esp_err_t guardian_node_start(void)
{
    if (!espagent_role_runs_guardian()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Guardian service boundary active: policy, privacy, audit, stateboard, watchdog");
    return ESP_OK;
}

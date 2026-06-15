#include "roles/coordinator_node.h"

#include "esp_log.h"
#include "node/node_profile.h"
#include "roles/role_config.h"

static const char *TAG = "coordinator_node";

esp_err_t coordinator_node_init(void)
{
    if (!espagent_role_runs_llm()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Coordinator role enabled: node=%s role=%s capabilities=%s",
             espagent_node_id(), espagent_node_role(), espagent_node_capabilities());
    return ESP_OK;
}

esp_err_t coordinator_node_start(void)
{
    if (!espagent_role_runs_llm()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Coordinator service boundary active: LLM, chat channels, dispatch, timeline");
    return ESP_OK;
}

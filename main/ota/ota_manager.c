#include "ota_manager.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_partition.h"
#include "esp_system.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "ota";

static void log_partition(const char *label, const esp_partition_t *partition)
{
    if (!partition) {
        ESP_LOGW(TAG, "%s: unavailable", label);
        printf("%s: unavailable\n", label);
        return;
    }

    ESP_LOGI(TAG, "%s: label=%s subtype=0x%02x addr=0x%06lx size=0x%06lx",
             label,
             partition->label,
             partition->subtype,
             (unsigned long)partition->address,
             (unsigned long)partition->size);
    printf("%s: label=%s subtype=0x%02x addr=0x%06lx size=0x%06lx\n",
           label,
           partition->label,
           partition->subtype,
           (unsigned long)partition->address,
           (unsigned long)partition->size);
}

esp_err_t ota_print_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    log_partition("running", running);
    log_partition("configured_boot", boot);
    log_partition("next_update", next);

    if (!running || !boot || !next) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ota_update_from_url(const char *url)
{
    if (!url || url[0] == '\0') {
        ESP_LOGE(TAG, "OTA URL is empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (strncmp(url, "https://", 8) != 0) {
        ESP_LOGE(TAG, "OTA requires an HTTPS firmware URL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Starting OTA from: %s", url);
    ota_print_info();

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 120000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA successful, restarting...");
        ota_print_info();
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app/espagent_app.h"

static const char *TAG = "ESPAgent";

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESPAgent firmware for ESP32-S3");
    ESP_LOGI(TAG, "========================================");

    ESP_LOGI(TAG, "Internal free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    ESP_ERROR_CHECK(espagent_app_init_subsystems());
    ESP_ERROR_CHECK(espagent_app_start_local_services());
    if (espagent_app_connect_wifi_or_onboard() != ESP_OK) {
        return;
    }
    ESP_ERROR_CHECK(espagent_app_start_network_services());

    ESP_LOGI(TAG, "ESPAgent is ready. Type 'help' for CLI commands.");
}

#include "time_sync/time_sync.h"

#include "espagent_config.h"

#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "time_sync";

static bool s_started = false;

bool espagent_time_is_valid(void)
{
    time_t now = time(NULL);
    return now >= (time_t)ESPAGENT_TIME_VALID_AFTER_EPOCH;
}

static void log_local_time(const char *prefix)
{
    time_t now = time(NULL);
    struct tm local = {0};
    char buf[64] = {0};

    localtime_r(&now, &local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &local);
    ESP_LOGI(TAG, "%s: %s", prefix, buf);
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    log_local_time("SNTP synchronized");
}

esp_err_t espagent_time_sync_start(void)
{
    setenv("TZ", ESPAGENT_TIMEZONE, 1);
    tzset();

    if (s_started) {
        return ESP_OK;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(ESPAGENT_SNTP_SERVER);
    config.sync_cb = time_sync_cb;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_started = true;
        ESP_LOGI(TAG, "SNTP started: server=%s timezone=%s", ESPAGENT_SNTP_SERVER, ESPAGENT_TIMEZONE);
    }
    return err;
}

esp_err_t espagent_time_sync_wait(uint32_t timeout_ms)
{
    esp_err_t err = espagent_time_sync_start();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK && espagent_time_is_valid()) {
        log_local_time("System time ready");
    }
    return err;
}

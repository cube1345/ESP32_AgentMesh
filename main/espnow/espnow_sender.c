#include "espnow/espnow_sender.h"

#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "espnow_sender";

#define ESPNOW_DEFAULT_CHANNEL 1

static const uint8_t ESPNOW_BROADCAST_MAC[ESP_NOW_ETH_ALEN] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static bool s_initialized;
static bool s_peer_added;

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        ESP_LOGW(TAG, "send failed: %d", status);
    }
}

static esp_err_t ensure_wifi_radio_started(void)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_get_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    if (mode == WIFI_MODE_NULL) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_mode(STA) failed: %s", esp_err_to_name(err));
            return err;
        }
    } else if (mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
        ESP_LOGW(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_channel(ESPNOW_DEFAULT_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK &&
        err != ESP_ERR_WIFI_STATE &&
        err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_set_channel(%d) failed: %s",
                 ESPNOW_DEFAULT_CHANNEL, esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t espnow_sender_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = ensure_wifi_radio_started();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_initialized = true;
    err = esp_now_register_send_cb(espnow_send_cb);
    if (err != ESP_OK) {
        s_initialized = false;
        ESP_LOGW(TAG, "esp_now_register_send_cb failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ESP-NOW sender initialized (broadcast, channel=%d when not connected to an AP)",
             ESPNOW_DEFAULT_CHANNEL);
    return ESP_OK;
}

static esp_err_t ensure_broadcast_peer(void)
{
    if (s_peer_added) {
        return ESP_OK;
    }

    if (esp_now_is_peer_exist(ESPNOW_BROADCAST_MAC)) {
        s_peer_added = true;
        return ESP_OK;
    }

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, ESPNOW_BROADCAST_MAC, ESP_NOW_ETH_ALEN);
    peer.ifidx = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;

    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) {
        s_peer_added = true;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "failed to add broadcast peer: %s", esp_err_to_name(err));
    return err;
}

esp_err_t espnow_sender_send_text(const char *topic, const char *text)
{
    if (!topic || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = espnow_sender_init();
    if (err != ESP_OK) {
        return err;
    }

    err = ensure_broadcast_peer();
    if (err != ESP_OK) {
        return err;
    }

    char payload[ESP_NOW_MAX_DATA_LEN] = {0};
    int written = snprintf(payload, sizeof(payload), "%s:%s", topic, text);
    size_t len = strnlen(payload, sizeof(payload));
    if (written < 0) {
        return ESP_FAIL;
    }
    if ((size_t)written >= sizeof(payload)) {
        payload[sizeof(payload) - 1] = '\0';
        len = sizeof(payload) - 1;
        ESP_LOGW(TAG, "payload truncated to %u bytes", (unsigned)len);
    }

    err = esp_now_send(ESPNOW_BROADCAST_MAC, (const uint8_t *)payload, len + 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "sent %u bytes: %.96s", (unsigned)(len + 1), payload);
    }
    return err;
}

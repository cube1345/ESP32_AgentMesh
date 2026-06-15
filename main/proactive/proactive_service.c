#include "proactive/proactive_service.h"

#include "bus/message_bus.h"
#include "espagent_config.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "proactive";

static TaskHandle_t s_task = NULL;
static char s_channel[16] = {0};
static char s_chat_id[96] = {0};
static bool s_has_target = false;

static const char *PROACTIVE_PROMPT =
    "This is an internal proactive check. Decide whether ESPAgent should "
    "send the user a short proactive message now.\n"
    "Use tools if useful, especially get_current_time, memory files, "
    "get_weather, or local sensors. Send a message only if it is genuinely useful, "
    "timely, or caring. Keep it concise and natural in the user's language.\n"
    "Good reasons include unusual weather, a useful reminder, checking in at a "
    "reasonable time, or a sensor state worth mentioning.\n"
    "If there is no useful reason to contact the user, reply exactly: "
    ESPAGENT_PROACTIVE_NO_MESSAGE;

static void proactive_load_target(void)
{
    nvs_handle_t nvs;
    if (nvs_open(ESPAGENT_NVS_PROACTIVE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    char channel[sizeof(s_channel)] = {0};
    char chat_id[sizeof(s_chat_id)] = {0};
    size_t channel_len = sizeof(channel);
    size_t chat_len = sizeof(chat_id);

    if (nvs_get_str(nvs, ESPAGENT_NVS_KEY_CHANNEL, channel, &channel_len) == ESP_OK &&
        nvs_get_str(nvs, ESPAGENT_NVS_KEY_CHAT_ID, chat_id, &chat_len) == ESP_OK &&
        channel[0] && chat_id[0]) {
        strncpy(s_channel, channel, sizeof(s_channel) - 1);
        strncpy(s_chat_id, chat_id, sizeof(s_chat_id) - 1);
        s_has_target = true;
        ESP_LOGI(TAG, "Loaded proactive target %s:%s", s_channel, s_chat_id);
    }

    nvs_close(nvs);
}

static void proactive_save_target(void)
{
    nvs_handle_t nvs;
    if (nvs_open(ESPAGENT_NVS_PROACTIVE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open proactive NVS namespace");
        return;
    }

    nvs_set_str(nvs, ESPAGENT_NVS_KEY_CHANNEL, s_channel);
    nvs_set_str(nvs, ESPAGENT_NVS_KEY_CHAT_ID, s_chat_id);
    nvs_commit(nvs);
    nvs_close(nvs);
}

esp_err_t proactive_service_note_contact(const char *channel,
                                         const char *chat_id)
{
    if (!channel || !chat_id || !channel[0] || !chat_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
        strcmp(channel, ESPAGENT_CHAN_CLI) == 0) {
        return ESP_OK;
    }

    if (s_has_target && strcmp(s_channel, channel) == 0 &&
        strcmp(s_chat_id, chat_id) == 0) {
        return ESP_OK;
    }

    memset(s_channel, 0, sizeof(s_channel));
    memset(s_chat_id, 0, sizeof(s_chat_id));
    strncpy(s_channel, channel, sizeof(s_channel) - 1);
    strncpy(s_chat_id, chat_id, sizeof(s_chat_id) - 1);
    s_has_target = true;
    proactive_save_target();

    ESP_LOGI(TAG, "Updated proactive target to %s:%s", s_channel, s_chat_id);
    return ESP_OK;
}

esp_err_t proactive_service_trigger_now(void)
{
#if !ESPAGENT_PROACTIVE_ENABLED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_has_target) {
        ESP_LOGW(TAG, "No proactive target yet; wait for a Feishu/WebSocket message");
        return ESP_ERR_INVALID_STATE;
    }

    espagent_msg_t msg = {0};
    strncpy(msg.channel, s_channel, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, s_chat_id, sizeof(msg.chat_id) - 1);
    msg.flags = ESPAGENT_MSG_FLAG_PROACTIVE;
    msg.content = strdup(PROACTIVE_PROMPT);
    if (!msg.content) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        free(msg.content);
        return err;
    }

    ESP_LOGI(TAG, "Queued proactive check for %s:%s", s_channel, s_chat_id);
    return ESP_OK;
#endif
}

static void proactive_task_main(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(ESPAGENT_PROACTIVE_INITIAL_DELAY_MS));

    while (1) {
        proactive_service_trigger_now();
        vTaskDelay(pdMS_TO_TICKS(ESPAGENT_PROACTIVE_INTERVAL_MS));
    }
}

esp_err_t proactive_service_init(void)
{
    proactive_load_target();
    ESP_LOGI(TAG, "Proactive service initialized (enabled=%d interval=%ds)",
             ESPAGENT_PROACTIVE_ENABLED,
             ESPAGENT_PROACTIVE_INTERVAL_MS / 1000);
    return ESP_OK;
}

esp_err_t proactive_service_start(void)
{
#if !ESPAGENT_PROACTIVE_ENABLED
    ESP_LOGI(TAG, "Proactive service disabled");
    return ESP_OK;
#else
    if (s_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(proactive_task_main,
                                "proactive",
                                ESPAGENT_PROACTIVE_STACK,
                                NULL,
                                ESPAGENT_PROACTIVE_PRIO,
                                &s_task);
    if (ok != pdPASS || !s_task) {
        ESP_LOGE(TAG, "Failed to create proactive task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Proactive service started");
    return ESP_OK;
#endif
}

void proactive_service_stop(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
        ESP_LOGI(TAG, "Proactive service stopped");
    }
}

void proactive_service_status(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    snprintf(out, out_size,
             "enabled=%d running=%s interval_s=%d target=%s:%s",
             ESPAGENT_PROACTIVE_ENABLED,
             s_task ? "yes" : "no",
             ESPAGENT_PROACTIVE_INTERVAL_MS / 1000,
             s_has_target ? s_channel : "(none)",
             s_has_target ? s_chat_id : "(none)");
}

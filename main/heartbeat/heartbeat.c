#include "heartbeat/heartbeat.h"
#include "espagent_config.h"
#include "bus/message_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_log.h"

static const char *TAG = "heartbeat";

#define HEARTBEAT_WORKER_STACK 4096
#define HEARTBEAT_WORKER_PRIO  (tskIDLE_PRIORITY + 1)

#define HEARTBEAT_PROMPT \
    "Read " ESPAGENT_HEARTBEAT_FILE " and follow any instructions or tasks listed there. " \
    "If nothing needs attention, reply with just: HEARTBEAT_OK"

static TimerHandle_t s_heartbeat_timer = NULL;
static portMUX_TYPE s_heartbeat_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_heartbeat_worker_running = false;

/* ── Content check ────────────────────────────────────────────── */

/**
 * Check if HEARTBEAT.md has actionable content.
 * Returns true if any line is NOT:
 *   - empty / whitespace-only
 *   - a markdown header (starts with #)
 *   - a completed checkbox (- [x] or * [x])
 */
static bool heartbeat_has_tasks(void)
{
    FILE *f = fopen(ESPAGENT_HEARTBEAT_FILE, "r");
    if (!f) {
        return false;
    }

    char line[256];
    bool found_task = false;

    while (fgets(line, sizeof(line), f)) {
        /* Skip leading whitespace */
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        /* Skip empty lines */
        if (*p == '\0') {
            continue;
        }

        /* Skip markdown headers */
        if (*p == '#') {
            continue;
        }

        /* Skip completed checkboxes: "- [x]" or "* [x]" */
        if ((*p == '-' || *p == '*') && *(p + 1) == ' ' && *(p + 2) == '[') {
            char mark = *(p + 3);
            if ((mark == 'x' || mark == 'X') && *(p + 4) == ']') {
                continue;
            }
        }

        /* Found an actionable line */
        found_task = true;
        break;
    }

    fclose(f);
    return found_task;
}

/* ── Send heartbeat to agent ──────────────────────────────────── */

static bool heartbeat_send(void)
{
    if (!heartbeat_has_tasks()) {
        ESP_LOGD(TAG, "No actionable tasks in HEARTBEAT.md");
        return false;
    }

    espagent_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, ESPAGENT_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "heartbeat", sizeof(msg.chat_id) - 1);
    msg.content = strdup(HEARTBEAT_PROMPT);

    if (!msg.content) {
        ESP_LOGE(TAG, "Failed to allocate heartbeat prompt");
        return false;
    }

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to push heartbeat message: %s", esp_err_to_name(err));
        free(msg.content);
        return false;
    }

    ESP_LOGI(TAG, "Triggered agent check");
    return true;
}

static void heartbeat_worker_task(void *arg)
{
    (void)arg;
    heartbeat_send();

    portENTER_CRITICAL(&s_heartbeat_lock);
    s_heartbeat_worker_running = false;
    portEXIT_CRITICAL(&s_heartbeat_lock);

    vTaskDelete(NULL);
}

/* ── Timer callback ───────────────────────────────────────────── */

static void heartbeat_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;

    portENTER_CRITICAL(&s_heartbeat_lock);
    if (s_heartbeat_worker_running) {
        portEXIT_CRITICAL(&s_heartbeat_lock);
        return;
    }
    s_heartbeat_worker_running = true;
    portEXIT_CRITICAL(&s_heartbeat_lock);

    BaseType_t ok = xTaskCreate(heartbeat_worker_task,
                                "heartbeat_worker",
                                HEARTBEAT_WORKER_STACK,
                                NULL,
                                HEARTBEAT_WORKER_PRIO,
                                NULL);
    if (ok != pdPASS) {
        portENTER_CRITICAL(&s_heartbeat_lock);
        s_heartbeat_worker_running = false;
        portEXIT_CRITICAL(&s_heartbeat_lock);
        ESP_LOGW(TAG, "Failed to create heartbeat worker");
    }
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t heartbeat_init(void)
{
    ESP_LOGI(TAG, "Heartbeat service initialized (file: %s, interval: %ds)",
             ESPAGENT_HEARTBEAT_FILE, ESPAGENT_HEARTBEAT_INTERVAL_MS / 1000);
    return ESP_OK;
}

esp_err_t heartbeat_start(void)
{
    if (s_heartbeat_timer) {
        ESP_LOGW(TAG, "Heartbeat timer already running");
        return ESP_OK;
    }

    s_heartbeat_timer = xTimerCreate(
        "heartbeat",
        pdMS_TO_TICKS(ESPAGENT_HEARTBEAT_INTERVAL_MS),
        pdTRUE,    /* auto-reload */
        NULL,
        heartbeat_timer_callback
    );

    if (!s_heartbeat_timer) {
        ESP_LOGE(TAG, "Failed to create heartbeat timer");
        return ESP_FAIL;
    }

    if (xTimerStart(s_heartbeat_timer, pdMS_TO_TICKS(1000)) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start heartbeat timer");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Heartbeat started (every %d min)", ESPAGENT_HEARTBEAT_INTERVAL_MS / 60000);
    return ESP_OK;
}

void heartbeat_stop(void)
{
    if (s_heartbeat_timer) {
        xTimerStop(s_heartbeat_timer, pdMS_TO_TICKS(1000));
        xTimerDelete(s_heartbeat_timer, pdMS_TO_TICKS(1000));
        s_heartbeat_timer = NULL;
        ESP_LOGI(TAG, "Heartbeat stopped");
    }
}

bool heartbeat_trigger(void)
{
    return heartbeat_send();
}

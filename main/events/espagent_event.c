#include "events/espagent_event.h"

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "memory/session_mgr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "event_router";

#define ESPAGENT_EVENT_RING_SIZE 32

static espagent_event_t s_events[ESPAGENT_EVENT_RING_SIZE];
static int s_next;
static int s_count;
static SemaphoreHandle_t s_lock;

static void copy_str(char *dst, size_t size, const char *src)
{
    if (!dst || size == 0) {
        return;
    }
    snprintf(dst, size, "%s", src ? src : "");
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

esp_err_t espagent_event_router_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_next = 0;
    s_count = 0;
    ESP_LOGI(TAG, "Event router initialized, ring=%d", ESPAGENT_EVENT_RING_SIZE);
    return ESP_OK;
}

esp_err_t espagent_event_emit(const espagent_event_t *event)
{
    if (!event || !event->type[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    espagent_event_t copy = *event;
    if (!copy.schema[0]) {
        copy_str(copy.schema, sizeof(copy.schema), "espagent.event.v1");
    }
    if (!copy.event_id[0]) {
        snprintf(copy.event_id, sizeof(copy.event_id), "ev-%08llx",
                 (unsigned long long)(esp_timer_get_time() & 0xffffffffULL));
    }
    if (copy.ts_ms <= 0) {
        copy.ts_ms = now_ms();
    }

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_events[s_next] = copy;
    s_next = (s_next + 1) % ESPAGENT_EVENT_RING_SIZE;
    if (s_count < ESPAGENT_EVENT_RING_SIZE) {
        s_count++;
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    ESP_LOGI(TAG, "event type=%s source=%s channel=%s corr=%s payload=%.96s",
             copy.type, copy.source, copy.channel, copy.correlation_id, copy.payload);

    if (copy.chat_id[0]) {
        char raw[512];
        snprintf(raw, sizeof(raw),
                 "{\"schema\":\"%s\",\"event_id\":\"%s\",\"event\":\"%s\","
                 "\"source\":\"%s\",\"channel\":\"%s\",\"correlation_id\":\"%s\","
                 "\"payload\":\"%.180s\"}",
                 copy.schema, copy.event_id, copy.type, copy.source,
                 copy.channel, copy.correlation_id, copy.payload);
        session_append_trace(copy.chat_id, copy.type, copy.payload, raw);
    }
    return ESP_OK;
}

esp_err_t espagent_event_emit_simple(const char *type,
                                     const char *source,
                                     const char *channel,
                                     const char *chat_id,
                                     const char *correlation_id,
                                     const char *payload)
{
    espagent_event_t event = {0};
    copy_str(event.schema, sizeof(event.schema), "espagent.event.v1");
    copy_str(event.type, sizeof(event.type), type);
    copy_str(event.source, sizeof(event.source), source);
    copy_str(event.channel, sizeof(event.channel), channel);
    copy_str(event.chat_id, sizeof(event.chat_id), chat_id);
    copy_str(event.correlation_id, sizeof(event.correlation_id), correlation_id);
    copy_str(event.payload, sizeof(event.payload), payload);
    return espagent_event_emit(&event);
}

esp_err_t espagent_event_recent_json(char *buf, size_t size, int max_events)
{
    if (!buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (max_events <= 0 || max_events > ESPAGENT_EVENT_RING_SIZE) {
        max_events = ESPAGENT_EVENT_RING_SIZE;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.event_recent.v1");
    cJSON_AddItemToObject(root, "events", arr);

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    int count = s_count < max_events ? s_count : max_events;
    int start = (s_next - count + ESPAGENT_EVENT_RING_SIZE) % ESPAGENT_EVENT_RING_SIZE;
    for (int i = 0; i < count; i++) {
        const espagent_event_t *ev = &s_events[(start + i) % ESPAGENT_EVENT_RING_SIZE];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddStringToObject(obj, "event_id", ev->event_id);
        cJSON_AddStringToObject(obj, "type", ev->type);
        cJSON_AddStringToObject(obj, "source", ev->source);
        cJSON_AddStringToObject(obj, "channel", ev->channel);
        cJSON_AddStringToObject(obj, "chat_id", ev->chat_id);
        cJSON_AddStringToObject(obj, "correlation_id", ev->correlation_id);
        cJSON_AddStringToObject(obj, "payload", ev->payload);
        cJSON_AddNumberToObject(obj, "ts_ms", (double)ev->ts_ms);
        cJSON_AddItemToArray(arr, obj);
    }
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(buf, size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

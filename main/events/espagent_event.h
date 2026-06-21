#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define ESPAGENT_EVENT_TYPE_MAX 48
#define ESPAGENT_EVENT_SOURCE_MAX 48
#define ESPAGENT_EVENT_CHANNEL_MAX 16
#define ESPAGENT_EVENT_CHAT_MAX 96
#define ESPAGENT_EVENT_CORRELATION_MAX 64
#define ESPAGENT_EVENT_PAYLOAD_MAX 256

typedef struct {
    char schema[24];
    char event_id[32];
    char type[ESPAGENT_EVENT_TYPE_MAX];
    char source[ESPAGENT_EVENT_SOURCE_MAX];
    char channel[ESPAGENT_EVENT_CHANNEL_MAX];
    char chat_id[ESPAGENT_EVENT_CHAT_MAX];
    char correlation_id[ESPAGENT_EVENT_CORRELATION_MAX];
    char payload[ESPAGENT_EVENT_PAYLOAD_MAX];
    int64_t ts_ms;
} espagent_event_t;

esp_err_t espagent_event_router_init(void);

esp_err_t espagent_event_emit(const espagent_event_t *event);

esp_err_t espagent_event_emit_simple(const char *type,
                                     const char *source,
                                     const char *channel,
                                     const char *chat_id,
                                     const char *correlation_id,
                                     const char *payload);

esp_err_t espagent_event_recent_json(char *buf, size_t size, int max_events);

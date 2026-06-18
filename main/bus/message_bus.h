#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>

/* Channel identifiers */
#define ESPAGENT_CHAN_FEISHU     "feishu"
#define ESPAGENT_CHAN_WEBSOCKET  "websocket"
#define ESPAGENT_CHAN_CLI        "cli"
#define ESPAGENT_CHAN_SYSTEM     "system"

/* Message flags */
#define ESPAGENT_MSG_FLAG_PROACTIVE  (1U << 0)
#define ESPAGENT_MSG_FLAG_INTERNAL_RESULT  (1U << 1)

/* Message types on the bus */
typedef struct {
    char channel[16];       /* "feishu", "websocket", "cli" */
    char chat_id[96];       /* Feishu chat_id/open_id, or WS client id */
    uint32_t flags;         /* ESPAGENT_MSG_FLAG_* */
    char *content;          /* Heap-allocated message text (caller must free) */
} espagent_msg_t;

/**
 * Initialize the message bus (inbound + outbound FreeRTOS queues).
 */
esp_err_t message_bus_init(void);

/**
 * Push a message to the inbound queue (towards Agent Loop).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_inbound(const espagent_msg_t *msg);

/**
 * Pop a message from the inbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_inbound(espagent_msg_t *msg, uint32_t timeout_ms);

/**
 * Push a message to the outbound queue (towards channels).
 * The bus takes ownership of msg->content.
 */
esp_err_t message_bus_push_outbound(const espagent_msg_t *msg);

/**
 * Pop a message from the outbound queue (blocking).
 * Caller must free msg->content when done.
 */
esp_err_t message_bus_pop_outbound(espagent_msg_t *msg, uint32_t timeout_ms);

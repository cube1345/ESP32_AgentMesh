#include "message_bus.h"
#include "espagent_config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "bus";

static QueueHandle_t s_inbound_queue;
static QueueHandle_t s_outbound_queue;

esp_err_t message_bus_init(void)
{
    s_inbound_queue = xQueueCreate(ESPAGENT_BUS_QUEUE_LEN, sizeof(espagent_msg_t));
    s_outbound_queue = xQueueCreate(ESPAGENT_BUS_QUEUE_LEN, sizeof(espagent_msg_t));

    if (!s_inbound_queue || !s_outbound_queue) {
        ESP_LOGE(TAG, "Failed to create message queues");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Message bus initialized (queue depth %d)", ESPAGENT_BUS_QUEUE_LEN);
    return ESP_OK;
}

/*
 * 这个函数的意义是将消息推送到消息总线的入站队列中，供系统的其他部分（如处理器或通道模块）消费。它接受一个指向espagent_msg_t结构的指针，该结构包含了消息的相关信息，如频道、聊天ID和消息内容。
 * 函数内部使用FreeRTOS的xQueueSend函数将消息发送到s_inbound_queue队列中。如果队列已满，函数会返回ESP_ERR_NO_MEM错误码，并在日志中记录警告信息。如果消息成功发送到队列，函数返回ESP_OK表示成功。
*/
esp_err_t message_bus_push_inbound(const espagent_msg_t *msg)
{
    if (xQueueSend(s_inbound_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Inbound queue full, dropping message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * 这个函数的意义是从消息总线的入站队列中弹出一条消息，供系统的其他部分（如处理器或通道模块）处理。它接受一个指向espagent_msg_t结构的指针和一个超时时间（以毫秒为单位）。
 * 如果在指定的超时时间内成功从队列中获取到一条消息，函数会将消息内容填充到提供的espagent_msg_t结构中，并返回ESP_OK表示成功。
 * 如果在超时时间内没有消息可用，函数会返回ESP_ERR_TIMEOUT错误码。函数内部使用FreeRTOS的xQueueReceive函数从s_inbound_queue队列中接收消息，并根据结果返回相应的状态码。
*/
esp_err_t message_bus_pop_inbound(espagent_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_inbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/*
 * 这个函数的意义是将消息推送到消息总线的出站队列中，供系统的其他部分（如处理器或通道模块）消费。它接受一个指向espagent_msg_t结构的指针，该结构包含了消息的相关信息，如频道、聊天ID和消息内容。
 * 函数内部使用FreeRTOS的xQueueSend函数将消息发送到s_outbound_queue队列中。如果队列已满，函数会返回ESP_ERR_NO_MEM错误码，并在日志中记录警告信息。如果消息成功发送到队列，函数返回ESP_OK表示成功。
*/
esp_err_t message_bus_push_outbound(const espagent_msg_t *msg)
{
    if (xQueueSend(s_outbound_queue, msg, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Outbound queue full, dropping message");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t message_bus_pop_outbound(espagent_msg_t *msg, uint32_t timeout_ms)
{
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xQueueReceive(s_outbound_queue, msg, ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

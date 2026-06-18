#include "sensors/sensor_mqtt.h"

#include "espagent_config.h"
#include "mesh/mesh_protocol.h"
#include "node/node_profile.h"
#include "roles/role_config.h"
#include "tools/tool_environment.h"
#include "tools/tool_gpio.h"
#include "tools/tool_servo.h"

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "sensor_mqtt";

#define DHT22_MAX_RETRIES      3
#define MQTT_BUF_SIZE          1024
#define MQTT_CLIENT_ID         "ESPAgent-" ESPAGENT_NODE_ID
#define MQTT_KEEPALIVE_S       30
#define MHZ19_CMD_LEN          9
#define MHZ19_UART_BUF_SIZE    128
#define MQTT_PUB_TOPIC_SIZE    160
#define MQTT_PUB_PAYLOAD_SIZE   768
#define MQTT_PUB_QUEUE_DEPTH    8
#define MQTT_OUTPUT_CACHE_DEPTH 8
#define MQTT_OUTPUT_JSON_SIZE   768
#define MQTT_POLICY_CACHE_DEPTH 8
#define MQTT_POLICY_JSON_SIZE   512
#define MQTT_STATEBOARD_DEPTH   12
#define MQTT_STATEBOARD_JSON_SIZE 384

typedef struct {
    char topic[MQTT_PUB_TOPIC_SIZE];
    char payload[MQTT_PUB_PAYLOAD_SIZE];
} mqtt_pub_item_t;

typedef struct {
    bool used;
    int64_t ts_ms;
    char command_id[ESPAGENT_MESH_ID_MAX];
    char output_json[MQTT_OUTPUT_JSON_SIZE];
} output_cache_item_t;

typedef struct {
    bool used;
    int64_t ts_ms;
    char command_id[ESPAGENT_MESH_ID_MAX];
    char decision_json[MQTT_POLICY_JSON_SIZE];
} policy_cache_item_t;

typedef struct {
    bool used;
    int64_t ts_ms;
    char event_type[32];
    char status[16];
    char command_id[ESPAGENT_MESH_ID_MAX];
    char summary[MQTT_STATEBOARD_JSON_SIZE];
} stateboard_item_t;

static portMUX_TYPE s_dht22_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_mhz19_uart_ready = false;
static QueueHandle_t s_pub_queue = NULL;
static SemaphoreHandle_t s_output_mutex = NULL;
static SemaphoreHandle_t s_policy_mutex = NULL;
static output_cache_item_t s_output_cache[MQTT_OUTPUT_CACHE_DEPTH];
static policy_cache_item_t s_policy_cache[MQTT_POLICY_CACHE_DEPTH];
static stateboard_item_t s_stateboard[MQTT_STATEBOARD_DEPTH];
static uint32_t s_output_cache_next = 0;
static uint32_t s_policy_cache_next = 0;
static uint32_t s_stateboard_next = 0;

static void json_add_optional_string(cJSON *root, const char *key, const char *value);
static void stateboard_note_event(const char *event_type,
                                  const char *status,
                                  const char *summary,
                                  const char *command_id);

static int wait_level_timeout(int pin, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)pin) != level) {
        if ((esp_timer_get_time() - start) >= timeout_us) {
            return -1;
        }
    }
    return 0;
}

static esp_err_t dht22_read(float *temperature, float *humidity)
{
    const int pin = ESPAGENT_SENSOR_DHT22_GPIO;
    uint8_t data[5] = {0};

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure DHT22 output failed");

    gpio_set_level((gpio_num_t)pin, 0);
    esp_rom_delay_us(2000);
    gpio_set_level((gpio_num_t)pin, 1);
    esp_rom_delay_us(40);

    cfg.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure DHT22 input failed");

    portENTER_CRITICAL(&s_dht22_mux);

    if (wait_level_timeout(pin, 0, 1000) != 0 ||
        wait_level_timeout(pin, 1, 1000) != 0 ||
        wait_level_timeout(pin, 0, 1000) != 0) {
        portEXIT_CRITICAL(&s_dht22_mux);
        return ESP_ERR_TIMEOUT;
    }

    for (int byte = 0; byte < 5; byte++) {
        uint8_t value = 0;
        for (int bit = 0; bit < 8; bit++) {
            if (wait_level_timeout(pin, 1, 1000) != 0) {
                portEXIT_CRITICAL(&s_dht22_mux);
                return ESP_ERR_TIMEOUT;
            }
            int64_t t0 = esp_timer_get_time();
            if (wait_level_timeout(pin, 0, 1000) != 0) {
                portEXIT_CRITICAL(&s_dht22_mux);
                return ESP_ERR_TIMEOUT;
            }
            value <<= 1;
            if ((esp_timer_get_time() - t0) > 50) {
                value |= 1;
            }
        }
        data[byte] = value;
    }

    portEXIT_CRITICAL(&s_dht22_mux);

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_humidity = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_temp = ((uint16_t)data[2] << 8) | data[3];
    float temp = (float)(raw_temp & 0x7FFF) / 10.0f;
    if (raw_temp & 0x8000) {
        temp = -temp;
    }

    *humidity = (float)raw_humidity / 10.0f;
    *temperature = temp;
    return ESP_OK;
}

static __attribute__((unused)) esp_err_t read_dht22_with_retry(float *temperature, float *humidity)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < DHT22_MAX_RETRIES; i++) {
        err = dht22_read(temperature, humidity);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return err;
}

static uint8_t mhz19_checksum(const uint8_t *packet)
{
    uint8_t sum = 0;
    for (int i = 1; i < 8; i++) {
        sum = (uint8_t)(sum + packet[i]);
    }
    return (uint8_t)(0xFF - sum + 1);
}

static esp_err_t mhz19_uart_init(void)
{
    if (s_mhz19_uart_ready) {
        return ESP_OK;
    }

    if (ESPAGENT_SENSOR_MHZ19_RX_GPIO < 0 || ESPAGENT_SENSOR_MHZ19_TX_GPIO < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uart_config_t cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_port_t uart_num = (uart_port_t)ESPAGENT_SENSOR_MHZ19_UART_NUM;
    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num, MHZ19_UART_BUF_SIZE, 0, 0, NULL, 0),
                        TAG, "install MH-Z19 UART failed");
    ESP_RETURN_ON_ERROR(uart_param_config(uart_num, &cfg), TAG, "configure MH-Z19 UART failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(uart_num,
                                     ESPAGENT_SENSOR_MHZ19_TX_GPIO,
                                     ESPAGENT_SENSOR_MHZ19_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "set MH-Z19 UART pins failed");

    s_mhz19_uart_ready = true;
    return ESP_OK;
}

static __attribute__((unused)) esp_err_t mhz19_read_co2(int *co2_ppm)
{
    ESP_RETURN_ON_ERROR(mhz19_uart_init(), TAG, "MH-Z19 UART unavailable");

    static const uint8_t cmd[MHZ19_CMD_LEN] = {0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0x79};
    uint8_t resp[MHZ19_CMD_LEN] = {0};
    uart_port_t uart_num = (uart_port_t)ESPAGENT_SENSOR_MHZ19_UART_NUM;

    uart_flush_input(uart_num);
    int written = uart_write_bytes(uart_num, (const char *)cmd, sizeof(cmd));
    if (written != sizeof(cmd)) {
        return ESP_FAIL;
    }

    int n = uart_read_bytes(uart_num, resp, sizeof(resp), pdMS_TO_TICKS(1000));
    if (n != sizeof(resp)) {
        return ESP_ERR_TIMEOUT;
    }
    if (resp[0] != 0xFF || resp[1] != 0x86 || mhz19_checksum(resp) != resp[8]) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *co2_ppm = ((int)resp[2] << 8) | resp[3];
    return ESP_OK;
}

static int write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static size_t mqtt_encode_remaining_len(uint8_t *out, size_t value)
{
    size_t count = 0;
    do {
        uint8_t byte = (uint8_t)(value % 128);
        value /= 128;
        if (value > 0) {
            byte |= 0x80;
        }
        out[count++] = byte;
    } while (value > 0 && count < 4);
    return count;
}

static uint8_t *mqtt_write_string(uint8_t *p, const char *s)
{
    size_t len = strlen(s);
    *p++ = (uint8_t)(len >> 8);
    *p++ = (uint8_t)(len & 0xFF);
    memcpy(p, s, len);
    return p + len;
}

static int mqtt_read_remaining_len(int fd, size_t *out)
{
    size_t multiplier = 1;
    size_t value = 0;

    for (int i = 0; i < 4; i++) {
        uint8_t byte = 0;
        int n = recv(fd, &byte, 1, MSG_WAITALL);
        if (n != 1) {
            return -1;
        }

        value += (size_t)(byte & 0x7F) * multiplier;
        if ((byte & 0x80) == 0) {
            *out = value;
            return 0;
        }
        multiplier *= 128;
    }

    return -1;
}

static int mqtt_send_connect(int fd)
{
    uint8_t payload[128];
    uint8_t *p = payload;
    p = mqtt_write_string(p, "MQTT");
    *p++ = 4;
    *p++ = 0x02;
    *p++ = 0;
    *p++ = MQTT_KEEPALIVE_S;
    p = mqtt_write_string(p, MQTT_CLIENT_ID);

    uint8_t packet[160];
    packet[0] = 0x10;
    size_t rem_len_len = mqtt_encode_remaining_len(&packet[1], (size_t)(p - payload));
    memcpy(packet + 1 + rem_len_len, payload, (size_t)(p - payload));
    return write_all(fd, packet, 1 + rem_len_len + (size_t)(p - payload));
}

static int mqtt_read_connack(int fd)
{
    uint8_t resp[4] = {0};
    int n = recv(fd, resp, sizeof(resp), MSG_WAITALL);
    if (n != sizeof(resp)) {
        return -1;
    }
    return (resp[0] == 0x20 && resp[1] == 0x02 && resp[2] == 0x00 && resp[3] == 0x00) ? 0 : -1;
}

static int mqtt_subscribe(int fd, const char *topic, uint16_t packet_id)
{
    uint8_t payload[160];
    uint8_t *p = payload;
    *p++ = (uint8_t)(packet_id >> 8);
    *p++ = (uint8_t)(packet_id & 0xFF);
    p = mqtt_write_string(p, topic);
    *p++ = 0;

    uint8_t packet[192];
    packet[0] = 0x82;
    size_t rem_len_len = mqtt_encode_remaining_len(&packet[1], (size_t)(p - payload));
    memcpy(packet + 1 + rem_len_len, payload, (size_t)(p - payload));
    int ret = write_all(fd, packet, 1 + rem_len_len + (size_t)(p - payload));
    if (ret == 0) {
        ESP_LOGI(TAG, "MQTT subscribe %s", topic);
    }
    return ret;
}

static int mqtt_publish(int fd, const char *topic, const char *payload)
{
    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t rem_len = 2 + topic_len + payload_len;
    if (rem_len + 5 > MQTT_BUF_SIZE) {
        return -1;
    }

    uint8_t packet[MQTT_BUF_SIZE];
    packet[0] = 0x30;
    size_t rem_len_len = mqtt_encode_remaining_len(&packet[1], rem_len);
    uint8_t *p = packet + 1 + rem_len_len;
    p = mqtt_write_string(p, topic);
    memcpy(p, payload, payload_len);
    return write_all(fd, packet, 1 + rem_len_len + 2 + topic_len + payload_len);
}

static bool mqtt_topic_equals(const uint8_t *topic, size_t topic_len, const char *expected)
{
    size_t expected_len = strlen(expected);
    return topic_len == expected_len && memcmp(topic, expected, expected_len) == 0;
}

static void mqtt_publish_node_event_json(char *json, size_t json_size,
                                         const char *event_type, const char *detail)
{
    int64_t ts_ms = esp_timer_get_time() / 1000;
    snprintf(json, json_size,
             "{\"node_id\":\"%s\",\"role\":\"%s\",\"location\":\"%s\","
             "\"capabilities\":\"%s\","
             "\"type\":\"event\",\"event\":\"%s\",\"detail\":\"%s\","
             "\"ts_ms\":%lld}",
             espagent_node_id(), espagent_node_role(), espagent_node_location(),
             espagent_node_capabilities(),
             event_type ? event_type : "",
             detail ? detail : "",
             (long long)ts_ms);
}

static esp_err_t mqtt_queue_publish(const char *topic, const char *payload)
{
    if (!topic || !topic[0] || !payload || !payload[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ESPAGENT_SENSOR_MQTT_BROKER[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_pub_queue) {
        s_pub_queue = xQueueCreate(MQTT_PUB_QUEUE_DEPTH, sizeof(mqtt_pub_item_t));
        if (!s_pub_queue) {
            return ESP_ERR_NO_MEM;
        }
    }

    mqtt_pub_item_t item = {0};
    snprintf(item.topic, sizeof(item.topic), "%s", topic);
    snprintf(item.payload, sizeof(item.payload), "%s", payload);

    if (xQueueSend(s_pub_queue, &item, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void mqtt_flush_queued_publishes(int fd)
{
    if (!s_pub_queue) {
        return;
    }

    mqtt_pub_item_t item;
    while (xQueueReceive(s_pub_queue, &item, 0) == pdPASS) {
        if (mqtt_publish(fd, item.topic, item.payload) != 0) {
            ESP_LOGW(TAG, "Queued MQTT publish failed: topic=%s", item.topic);
            continue;
        }
        ESP_LOGI(TAG, "MQTT publish %s: %s", item.topic, item.payload);
    }
}

esp_err_t sensor_mqtt_publish_text(const char *topic, const char *payload)
{
    return mqtt_queue_publish(topic, payload);
}

static esp_err_t output_cache_ensure(void)
{
    if (!s_output_mutex) {
        s_output_mutex = xSemaphoreCreateMutex();
        if (!s_output_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t policy_cache_ensure(void)
{
    if (!s_policy_mutex) {
        s_policy_mutex = xSemaphoreCreateMutex();
        if (!s_policy_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static bool json_field_equals(cJSON *root, const char *key, const char *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) && strcmp(item->valuestring, value) == 0;
}

static bool output_payload_is_result(cJSON *root)
{
    return json_field_equals(root, "schema", "espagent.output.v1") ||
           json_field_equals(root, "type", "output") ||
           json_field_equals(root, "event", "mesh_command_result");
}

static bool policy_payload_is_decision(cJSON *root)
{
    return json_field_equals(root, "schema", "espagent.policy_decision.v1") ||
           json_field_equals(root, "event", "policy_decision");
}

static const char *json_optional_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

static int json_optional_int(cJSON *root, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : default_value;
}

static bool guardian_should_audit_event(cJSON *root)
{
    const char *event = json_optional_string(root, "event");
    const char *type = json_optional_string(root, "type");

    return output_payload_is_result(root) ||
           strcmp(event, "tool_use") == 0 ||
           strcmp(event, "tool_result") == 0 ||
           strcmp(event, "mesh_command_queued") == 0 ||
           strcmp(event, "final_reply") == 0 ||
           strcmp(event, "error") == 0 ||
           strcmp(type, "output") == 0;
}

static void guardian_audit_timeline_payload(const char *payload, size_t payload_len)
{
    if (!espagent_role_is_guardian() || !payload || payload_len == 0) {
        return;
    }

    cJSON *observed = cJSON_ParseWithLength(payload, payload_len);
    if (!observed || !cJSON_IsObject(observed)) {
        cJSON_Delete(observed);
        return;
    }
    if (!guardian_should_audit_event(observed)) {
        cJSON_Delete(observed);
        return;
    }

    const char *status = json_optional_string(observed, "status");
    const bool is_error = strcmp(status, "error") == 0 ||
                          json_field_equals(observed, "type", "error");

    cJSON *audit = cJSON_CreateObject();
    if (!audit) {
        cJSON_Delete(observed);
        return;
    }

    int64_t ts_ms = esp_timer_get_time() / 1000;
    char audit_id[96] = {0};
    snprintf(audit_id, sizeof(audit_id), "audit-%s-%lld",
             espagent_node_id(), (long long)ts_ms);

    cJSON_AddStringToObject(audit, "schema", "espagent.guardian.audit.v1");
    cJSON_AddStringToObject(audit, "audit_id", audit_id);
    cJSON_AddStringToObject(audit, "event", "guardian_audit");
    cJSON_AddStringToObject(audit, "auditor_node", espagent_node_id());
    cJSON_AddStringToObject(audit, "auditor_role", espagent_node_role());
    cJSON_AddStringToObject(audit, "decision", is_error ? "watch" : "observe");
    cJSON_AddStringToObject(audit, "privacy_mode", "metadata_only");
    cJSON_AddStringToObject(audit, "observed_event", json_optional_string(observed, "event"));
    cJSON_AddStringToObject(audit, "observed_type", json_optional_string(observed, "type"));
    cJSON_AddStringToObject(audit, "observed_status", status);
    cJSON_AddStringToObject(audit, "observed_node", json_optional_string(observed, "node_id"));
    cJSON_AddStringToObject(audit, "observed_role", json_optional_string(observed, "role"));
    json_add_optional_string(audit, "command_id", json_optional_string(observed, "command_id"));
    json_add_optional_string(audit, "trace_id", json_optional_string(observed, "trace_id"));
    json_add_optional_string(audit, "action", json_optional_string(observed, "action"));
    cJSON_AddStringToObject(audit, "summary", is_error ? "Guardian observed an error event" : "Guardian observed a mesh event");
    cJSON_AddNumberToObject(audit, "ts_ms", (double)ts_ms);

    char *json = cJSON_PrintUnformatted(audit);
    cJSON_Delete(audit);
    cJSON_Delete(observed);
    if (!json) {
        return;
    }

    (void)mqtt_queue_publish(ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json);
    if (is_error) {
        (void)mqtt_queue_publish(ESPAGENT_MESH_TOPIC_ALERTS, json);
    }
    ESP_LOGI(TAG, "Guardian audited timeline event: %s", json);
    cJSON_free(json);
}

static void output_cache_store_json(cJSON *root)
{
    if (!root || output_cache_ensure() != ESP_OK) {
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command_id");
    if (!cJSON_IsString(cmd) || cmd->valuestring[0] == '\0') {
        return;
    }

    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return;
    }

    if (xSemaphoreTake(s_output_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        output_cache_item_t *slot = &s_output_cache[s_output_cache_next % MQTT_OUTPUT_CACHE_DEPTH];
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->ts_ms = esp_timer_get_time() / 1000;
        snprintf(slot->command_id, sizeof(slot->command_id), "%s", cmd->valuestring);
        snprintf(slot->output_json, sizeof(slot->output_json), "%s", json);
        s_output_cache_next++;
        xSemaphoreGive(s_output_mutex);
        ESP_LOGI(TAG, "Cached OutputMessage command_id=%s", cmd->valuestring);
    }

    cJSON_free(json);
}

static void output_cache_maybe_store_payload(const char *payload, size_t payload_len)
{
    if (!payload || payload_len == 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return;
    }

    if (output_payload_is_result(root)) {
        output_cache_store_json(root);
    }

    cJSON_Delete(root);
}

static void policy_cache_store_json(cJSON *root)
{
    if (!root || policy_cache_ensure() != ESP_OK) {
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command_id");
    if (!cJSON_IsString(cmd) || cmd->valuestring[0] == '\0') {
        return;
    }

    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        return;
    }

    if (xSemaphoreTake(s_policy_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        policy_cache_item_t *slot = &s_policy_cache[s_policy_cache_next % MQTT_POLICY_CACHE_DEPTH];
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->ts_ms = esp_timer_get_time() / 1000;
        snprintf(slot->command_id, sizeof(slot->command_id), "%s", cmd->valuestring);
        snprintf(slot->decision_json, sizeof(slot->decision_json), "%s", json);
        s_policy_cache_next++;
        xSemaphoreGive(s_policy_mutex);
        ESP_LOGI(TAG, "Cached PolicyDecision command_id=%s", cmd->valuestring);
    }

    cJSON_free(json);
}

static void policy_cache_maybe_store_payload(const char *payload, size_t payload_len)
{
    if (!payload || payload_len == 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    if (policy_payload_is_decision(root)) {
        policy_cache_store_json(root);
    }

    cJSON_Delete(root);
}

esp_err_t sensor_mqtt_wait_output_message(const char *command_id,
                                          char *output_json,
                                          size_t output_json_size,
                                          uint32_t timeout_ms)
{
    if (!command_id || command_id[0] == '\0' || !output_json || output_json_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (output_cache_ensure() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    const int64_t start_ms = esp_timer_get_time() / 1000;
    const int64_t deadline_ms = start_ms + timeout_ms;
    while ((esp_timer_get_time() / 1000) <= deadline_ms) {
        if (xSemaphoreTake(s_output_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (size_t i = 0; i < MQTT_OUTPUT_CACHE_DEPTH; i++) {
                const output_cache_item_t *item = &s_output_cache[i];
                if (item->used && strcmp(item->command_id, command_id) == 0) {
                    snprintf(output_json, output_json_size, "%s", item->output_json);
                    xSemaphoreGive(s_output_mutex);
                    return ESP_OK;
                }
            }
            xSemaphoreGive(s_output_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t sensor_mqtt_wait_policy_decision(const char *command_id,
                                           char *decision_json,
                                           size_t decision_json_size,
                                           uint32_t timeout_ms)
{
    if (!command_id || command_id[0] == '\0' || !decision_json || decision_json_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (policy_cache_ensure() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    const int64_t start_ms = esp_timer_get_time() / 1000;
    const int64_t deadline_ms = start_ms + timeout_ms;
    while ((esp_timer_get_time() / 1000) <= deadline_ms) {
        if (xSemaphoreTake(s_policy_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (size_t i = 0; i < MQTT_POLICY_CACHE_DEPTH; i++) {
                const policy_cache_item_t *item = &s_policy_cache[i];
                if (item->used && strcmp(item->command_id, command_id) == 0) {
                    snprintf(decision_json, decision_json_size, "%s", item->decision_json);
                    xSemaphoreGive(s_policy_mutex);
                    return ESP_OK;
                }
            }
            xSemaphoreGive(s_policy_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t sensor_mqtt_publish_node_event(const char *event_type, const char *detail)
{
    char json[512];
    mqtt_publish_node_event_json(json, sizeof(json), event_type, detail);
    return mqtt_queue_publish(ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json);
}

static void stateboard_note_event(const char *event_type,
                                  const char *status,
                                  const char *summary,
                                  const char *command_id)
{
    stateboard_item_t *slot = &s_stateboard[s_stateboard_next % MQTT_STATEBOARD_DEPTH];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->ts_ms = esp_timer_get_time() / 1000;
    snprintf(slot->event_type, sizeof(slot->event_type), "%s", event_type ? event_type : "event");
    snprintf(slot->status, sizeof(slot->status), "%s", status ? status : "");
    snprintf(slot->command_id, sizeof(slot->command_id), "%s", command_id ? command_id : "");
    snprintf(slot->summary, sizeof(slot->summary), "%s", summary ? summary : "");
    s_stateboard_next++;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.stateboard.v1");
    cJSON_AddStringToObject(root, "event", "stateboard_update");
    cJSON_AddStringToObject(root, "node_id", espagent_node_id());
    cJSON_AddStringToObject(root, "role", espagent_node_role());
    cJSON_AddStringToObject(root, "last_event", slot->event_type);
    cJSON_AddStringToObject(root, "last_status", slot->status);
    json_add_optional_string(root, "command_id", slot->command_id);
    cJSON_AddStringToObject(root, "summary", slot->summary);
    cJSON_AddNumberToObject(root, "ts_ms", (double)slot->ts_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return;
    }
    (void)mqtt_queue_publish(ESPAGENT_MESH_TOPIC_GUARDIAN_STATEBOARD, json);
    cJSON_free(json);
}

static void json_add_optional_string(cJSON *root, const char *key, const char *value)
{
    if (value && value[0]) {
        cJSON_AddStringToObject(root, key, value);
    }
}

esp_err_t sensor_mqtt_stateboard_json(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "schema", "espagent.stateboard.v1");
    cJSON_AddStringToObject(root, "node_id", espagent_node_id());
    cJSON_AddStringToObject(root, "role", espagent_node_role());

    uint32_t count = s_stateboard_next < MQTT_STATEBOARD_DEPTH
                         ? s_stateboard_next
                         : MQTT_STATEBOARD_DEPTH;
    uint32_t start = s_stateboard_next > count ? s_stateboard_next - count : 0;
    for (uint32_t i = 0; i < count; i++) {
        const stateboard_item_t *slot = &s_stateboard[(start + i) % MQTT_STATEBOARD_DEPTH];
        if (!slot->used) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "event", slot->event_type);
        cJSON_AddStringToObject(item, "status", slot->status);
        json_add_optional_string(item, "command_id", slot->command_id);
        cJSON_AddStringToObject(item, "summary", slot->summary);
        cJSON_AddNumberToObject(item, "ts_ms", (double)slot->ts_ms);
        cJSON_AddItemToArray(items, item);
    }
    cJSON_AddItemToObject(root, "items", items);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(buf, buf_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t sensor_mqtt_publish_output_message(const char *event,
                                             const char *command_id,
                                             const char *trace_id,
                                             const char *action,
                                             const char *recipient,
                                             esp_err_t result_err,
                                             const char *summary,
                                             const char *result_text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    int64_t ts_ms = esp_timer_get_time() / 1000;
    char msg_id[96] = {0};
    snprintf(msg_id, sizeof(msg_id), "out-%s-%lld",
             command_id && command_id[0] ? command_id : espagent_node_id(),
             (long long)ts_ms);

    cJSON_AddStringToObject(root, "schema", "espagent.output.v1");
    cJSON_AddStringToObject(root, "msg_id", msg_id);
    cJSON_AddStringToObject(root, "node_id", espagent_node_id());
    cJSON_AddStringToObject(root, "role", espagent_node_role());
    cJSON_AddStringToObject(root, "sender", espagent_node_role());
    cJSON_AddStringToObject(root, "sender_node", espagent_node_id());
    cJSON_AddStringToObject(root, "recipient", recipient ? recipient : "coordinator_agent");
    cJSON_AddStringToObject(root, "location", espagent_node_location());
    cJSON_AddStringToObject(root, "type", "output");
    cJSON_AddStringToObject(root, "event", event ? event : "output");
    json_add_optional_string(root, "command_id", command_id);
    json_add_optional_string(root, "trace_id", trace_id);
    json_add_optional_string(root, "action", action);
    cJSON_AddStringToObject(root, "status", result_err == ESP_OK ? "ok" : "error");
    cJSON_AddStringToObject(root, "esp_err", esp_err_to_name(result_err));
    cJSON_AddStringToObject(root, "summary", summary ? summary : "");

    cJSON *result_obj = cJSON_CreateObject();
    if (result_obj) {
        cJSON_AddStringToObject(result_obj, "text", result_text ? result_text : "");
        cJSON_AddItemToObject(root, "result", result_obj);
    }

    if (result_err == ESP_OK) {
        cJSON_AddNullToObject(root, "error");
    } else {
        cJSON *err_obj = cJSON_CreateObject();
        if (err_obj) {
            cJSON_AddStringToObject(err_obj, "code", esp_err_to_name(result_err));
            cJSON_AddStringToObject(err_obj, "message", result_text ? result_text : "");
            cJSON_AddItemToObject(root, "error", err_obj);
        }
    }
    cJSON_AddNumberToObject(root, "ts_ms", (double)ts_ms);

    output_cache_store_json(root);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sensor_mqtt_publish_text(ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json);
    esp_err_t timeline_err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_TIMELINE, json);
    stateboard_note_event(event ? event : "output",
                          result_err == ESP_OK ? "ok" : "error",
                          summary ? summary : "",
                          command_id);
    cJSON_free(json);
    return err == ESP_OK ? timeline_err : err;
}

esp_err_t sensor_mqtt_publish_timeline_event(const char *phase,
                                             const char *event_type,
                                             const char *status,
                                             const char *summary,
                                             const char *command_id,
                                             const char *target_role,
                                             const char *target_node,
                                             const char *action)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    int64_t ts_ms = esp_timer_get_time() / 1000;
    char event_id[96] = {0};
    snprintf(event_id, sizeof(event_id), "evt-%s-%lld",
             espagent_node_id(), (long long)ts_ms);

    cJSON_AddStringToObject(root, "event_id", event_id);
    cJSON_AddStringToObject(root, "node_id", espagent_node_id());
    cJSON_AddStringToObject(root, "role", espagent_node_role());
    cJSON_AddStringToObject(root, "source_node", espagent_node_id());
    cJSON_AddStringToObject(root, "source_role", espagent_node_role());
    cJSON_AddStringToObject(root, "location", espagent_node_location());
    cJSON_AddStringToObject(root, "type", event_type ? event_type : "event");
    cJSON_AddStringToObject(root, "event", event_type ? event_type : "event");
    cJSON_AddStringToObject(root, "phase", phase ? phase : "");
    cJSON_AddStringToObject(root, "status", status ? status : "");
    cJSON_AddStringToObject(root, "summary", summary ? summary : "");
    json_add_optional_string(root, "command_id", command_id);
    json_add_optional_string(root, "target_role", target_role);
    json_add_optional_string(root, "target_node", target_node);
    json_add_optional_string(root, "action", action);
    cJSON_AddNumberToObject(root, "ts_ms", (double)ts_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = mqtt_queue_publish(ESPAGENT_MESH_TOPIC_TIMELINE, json);
    stateboard_note_event(event_type,
                          status,
                          summary,
                          command_id);
    cJSON_free(json);
    return err;
}

static bool policy_is_sensor_action(const char *action)
{
    return action && strcmp(action, "read_temperature_humidity") == 0;
}

static bool policy_is_control_action(const char *action)
{
    return action &&
           (strcmp(action, "set_status_light") == 0 ||
            strcmp(action, "ws2812_set") == 0 ||
            strcmp(action, "servo_write") == 0 ||
            strcmp(action, "gpio_write") == 0);
}

static void handle_guardian_policy_check(const char *payload, size_t payload_len)
{
    if (!espagent_role_is_guardian() || !payload || payload_len == 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, payload_len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Guardian rejected malformed policy_check: %.*s", (int)payload_len, payload);
        return;
    }

    const char *command_id = json_optional_string(root, "command_id");
    const char *trace_id = json_optional_string(root, "trace_id");
    const char *action = json_optional_string(root, "action");
    const char *target_role = json_optional_string(root, "target_role");
    const char *target_node = json_optional_string(root, "target_node");
    int safety_level = json_optional_int(root, "safety_level", ESPAGENT_MESH_SAFETY_MEDIUM);

    char command_id_copy[ESPAGENT_MESH_ID_MAX] = {0};
    char trace_id_copy[ESPAGENT_MESH_TRACE_MAX] = {0};
    char action_copy[ESPAGENT_MESH_ACTION_MAX] = {0};
    char target_role_copy[ESPAGENT_MESH_ROLE_MAX] = {0};
    char target_node_copy[ESPAGENT_MESH_NODE_MAX] = {0};
    snprintf(command_id_copy, sizeof(command_id_copy), "%s", command_id);
    snprintf(trace_id_copy, sizeof(trace_id_copy), "%s", trace_id);
    snprintf(action_copy, sizeof(action_copy), "%s", action);
    snprintf(target_role_copy, sizeof(target_role_copy), "%s", target_role);
    snprintf(target_node_copy, sizeof(target_node_copy), "%s", target_node);

    const char *decision = "deny";
    const char *reason = "deny by default";
    bool allowed = false;

    if (command_id_copy[0] == '\0') {
        reason = "missing command_id";
    } else if (action_copy[0] == '\0') {
        reason = "missing action";
    } else if (safety_level > ESPAGENT_MESH_SAFETY_MEDIUM) {
        reason = "safety_level requires human confirmation";
    } else if (policy_is_sensor_action(action_copy)) {
        if (strcmp(target_role_copy, ESPAGENT_ROLE_SENSOR) == 0) {
            decision = "allow";
            reason = "allowed low-risk sensor read";
            allowed = true;
        } else {
            reason = "sensor action must target sensor_agent";
        }
    } else if (policy_is_control_action(action_copy)) {
        if (strcmp(target_role_copy, ESPAGENT_ROLE_CONTROL) == 0) {
            decision = "allow";
            reason = "allowed whitelisted low/medium-risk control action";
            allowed = true;
        } else {
            reason = "control action must target control_agent";
        }
    } else {
        reason = "unsupported action";
    }

    cJSON *out = cJSON_CreateObject();
    if (!out) {
        cJSON_Delete(root);
        return;
    }

    int64_t ts_ms = esp_timer_get_time() / 1000;
    char decision_id[96] = {0};
    snprintf(decision_id, sizeof(decision_id), "decision-%s-%lld",
             command_id_copy[0] ? command_id_copy : "none",
             (long long)ts_ms);

    cJSON_AddStringToObject(out, "schema", "espagent.policy_decision.v1");
    cJSON_AddStringToObject(out, "event", "policy_decision");
    cJSON_AddStringToObject(out, "decision_id", decision_id);
    cJSON_AddStringToObject(out, "guardian_node", espagent_node_id());
    cJSON_AddStringToObject(out, "guardian_role", espagent_node_role());
    cJSON_AddStringToObject(out, "command_id", command_id_copy);
    cJSON_AddStringToObject(out, "trace_id", trace_id_copy);
    cJSON_AddStringToObject(out, "action", action_copy);
    cJSON_AddStringToObject(out, "target_role", target_role_copy);
    cJSON_AddStringToObject(out, "target_node", target_node_copy);
    cJSON_AddStringToObject(out, "decision", decision);
    cJSON_AddBoolToObject(out, "allowed", allowed);
    cJSON_AddStringToObject(out, "reason", reason);
    cJSON_AddNumberToObject(out, "safety_level", safety_level);
    cJSON_AddNumberToObject(out, "ts_ms", (double)ts_ms);

    char *json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(root);
    if (!json) {
        return;
    }

    (void)mqtt_queue_publish(ESPAGENT_MESH_TOPIC_POLICY_DECISION, json);
    (void)mqtt_queue_publish(ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json);
    (void)sensor_mqtt_publish_timeline_event("policy",
                                             "policy_decision",
                                             allowed ? "ok" : "blocked",
                                             reason,
                                             command_id_copy,
                                             target_role_copy,
                                             target_node_copy,
                                             action_copy);
    ESP_LOGI(TAG, "Guardian policy decision: %s", json);
    cJSON_free(json);
}

static void publish_mesh_command_result(const espagent_mesh_command_t *cmd,
                                        esp_err_t result_err,
                                        const char *result)
{
    if (!cmd) {
        return;
    }

    (void)sensor_mqtt_publish_output_message("mesh_command_result",
                                             cmd->command_id,
                                             cmd->trace_id,
                                             cmd->action,
                                             "coordinator_agent",
                                             result_err,
                                             result ? result : "",
                                             result ? result : "");
    (void)sensor_mqtt_publish_timeline_event("result",
                                             "mesh_command_result",
                                             result_err == ESP_OK ? "ok" : "error",
                                             result ? result : "",
                                             cmd->command_id,
                                             cmd->target_role,
                                             cmd->target_node,
                                             cmd->action);
}

static esp_err_t read_temperature_humidity_result(char *result, size_t result_size)
{
    tool_environment_values_t values = {0};
    char status[96] = {0};
    esp_err_t err = tool_environment_read_values(&values, status, sizeof(status));
    if (err == ESP_OK &&
        values.temperature_c_x10 != -1 &&
        values.humidity_percent_x10 != -1) {
        snprintf(result, result_size,
                 "OK: AHT20 on SDA=%d SCL=%d addr=0x%02x -> temperature=%.1f C, humidity=%.1f%% [%s]",
                 ESPAGENT_AHT10_DEFAULT_SDA_GPIO,
                 ESPAGENT_AHT10_DEFAULT_SCL_GPIO,
                 ESPAGENT_AHT10_DEFAULT_ADDR,
                 (double)values.temperature_c_x10 / 10.0,
                 (double)values.humidity_percent_x10 / 10.0,
                 status);
        return ESP_OK;
    }

    snprintf(result, result_size,
             "Error: AHT20 not readable on SDA=%d SCL=%d addr=0x%02x [%s]",
             ESPAGENT_AHT10_DEFAULT_SDA_GPIO,
             ESPAGENT_AHT10_DEFAULT_SCL_GPIO,
             ESPAGENT_AHT10_DEFAULT_ADDR,
             status[0] ? status : esp_err_to_name(err));
    return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
}

static bool handle_sensor_mesh_command(const espagent_mesh_command_t *cmd)
{
    if (!espagent_role_is_sensor()) {
        return false;
    }
    if (strcmp(cmd->action, "read_temperature_humidity") != 0) {
        return false;
    }

    char result[384] = {0};
    esp_err_t err = read_temperature_humidity_result(result, sizeof(result));
    ESP_LOGI(TAG, "Mesh sensor command executed: id=%s action=%s status=%s result=%s",
             cmd->command_id[0] ? cmd->command_id : "(none)",
             cmd->action,
             esp_err_to_name(err),
             result);
    publish_mesh_command_result(cmd, err, result);
    return true;
}

static bool control_command_has_guardian_allow(const espagent_mesh_command_t *cmd,
                                               char *reason,
                                               size_t reason_size)
{
    if (!cmd || !cmd->command_id[0]) {
        snprintf(reason, reason_size, "missing command_id for Guardian decision lookup");
        return false;
    }

    char decision_json[MQTT_POLICY_JSON_SIZE] = {0};
    esp_err_t err = sensor_mqtt_wait_policy_decision(cmd->command_id,
                                                     decision_json,
                                                     sizeof(decision_json),
                                                     300);
    if (err != ESP_OK) {
        snprintf(reason, reason_size,
                 "missing Guardian allow decision for command_id=%s",
                 cmd->command_id);
        return false;
    }

    cJSON *root = cJSON_Parse(decision_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(reason, reason_size, "Guardian decision is not valid JSON");
        return false;
    }

    const char *decision = json_optional_string(root, "decision");
    const char *action = json_optional_string(root, "action");
    const char *target_role = json_optional_string(root, "target_role");
    const char *target_node = json_optional_string(root, "target_node");

    bool allowed = strcmp(decision, "allow") == 0;
    if (allowed && action[0] && strcmp(action, cmd->action) != 0) {
        allowed = false;
        snprintf(reason, reason_size,
                 "Guardian decision action mismatch: decision=%s command=%s",
                 action, cmd->action);
    } else if (allowed && target_role[0] && strcmp(target_role, espagent_node_role()) != 0) {
        allowed = false;
        snprintf(reason, reason_size,
                 "Guardian decision target_role mismatch: decision=%s local=%s",
                 target_role, espagent_node_role());
    } else if (allowed && target_node[0] && strcmp(target_node, espagent_node_id()) != 0) {
        allowed = false;
        snprintf(reason, reason_size,
                 "Guardian decision target_node mismatch: decision=%s local=%s",
                 target_node, espagent_node_id());
    } else if (allowed) {
        snprintf(reason, reason_size, "Guardian allow decision verified");
    } else {
        snprintf(reason, reason_size,
                 "Guardian decision is not allow: %s",
                 decision[0] ? decision : "(empty)");
    }

    cJSON_Delete(root);
    return allowed;
}

static bool handle_control_mesh_command(const espagent_mesh_command_t *cmd)
{
    if (!espagent_role_is_control()) {
        return false;
    }

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    char result[384] = {0};
    const char *args = cmd->args_json[0] ? cmd->args_json : "{}";

    char policy_reason[192] = {0};
    if (!control_command_has_guardian_allow(cmd, policy_reason, sizeof(policy_reason))) {
        snprintf(result, sizeof(result),
                 "Error: control command denied before local execution: %s",
                 policy_reason);
        err = ESP_ERR_INVALID_STATE;
    } else if (cmd->safety_level > ESPAGENT_MESH_SAFETY_MEDIUM) {
        snprintf(result, sizeof(result),
                 "Error: command safety_level=%d requires a future safety interlock",
                 cmd->safety_level);
        err = ESP_ERR_INVALID_ARG;
    } else if (strcmp(cmd->action, "set_status_light") == 0) {
        err = tool_set_status_light_execute(args, result, sizeof(result));
    } else if (strcmp(cmd->action, "ws2812_set") == 0) {
        err = tool_ws2812_set_execute(args, result, sizeof(result));
    } else if (strcmp(cmd->action, "servo_write") == 0) {
        err = tool_servo_write_execute(args, result, sizeof(result));
    } else if (strcmp(cmd->action, "gpio_write") == 0) {
        err = tool_gpio_write_execute(args, result, sizeof(result));
    } else {
        return false;
    }

    ESP_LOGI(TAG, "Mesh control command executed: id=%s action=%s status=%s result=%s",
             cmd->command_id[0] ? cmd->command_id : "(none)",
             cmd->action,
             esp_err_to_name(err),
             result);
    publish_mesh_command_result(cmd, err, result);
    return true;
}

static void handle_mesh_command(const char *source, const char *payload, size_t payload_len)
{
    espagent_mesh_command_t cmd;
    char err[128] = {0};
    esp_err_t parse_err = espagent_mesh_parse_command_json(payload, payload_len, &cmd, err, sizeof(err));
    if (parse_err != ESP_OK) {
        ESP_LOGW(TAG, "Mesh %s command rejected before execution: %s payload=%.*s",
                 source, err, (int)payload_len, payload);
        return;
    }

    ESP_LOGI(TAG,
             "Mesh %s command validated in dry-run mode: id=%s action=%s target_node=%s target_role=%s safety=%d ttl_ms=%d args=%s",
             source,
             cmd.command_id[0] ? cmd.command_id : "(none)",
             cmd.action,
             cmd.target_node[0] ? cmd.target_node : "(any)",
             cmd.target_role[0] ? cmd.target_role : "(any)",
             cmd.safety_level,
             cmd.ttl_ms,
             cmd.args_json);

    if (handle_sensor_mesh_command(&cmd)) {
        return;
    }
    if (handle_control_mesh_command(&cmd)) {
        return;
    }

    ESP_LOGI(TAG, "Mesh command execution remains disabled until command_queue and safety_interlock are implemented");
}

static int mqtt_connect_tcp(void)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", ESPAGENT_SENSOR_MQTT_PORT);

    int err = getaddrinfo(ESPAGENT_SENSOR_MQTT_BROKER, port_str, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGW(TAG, "MQTT broker resolve failed: %s", ESPAGENT_SENSOR_MQTT_BROKER);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = {
        .tv_sec = 3,
        .tv_usec = 0,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "MQTT TCP connect failed: errno=%d", errno);
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void mqtt_poll_inbound(int fd)
{
    uint8_t header = 0;
    int n = recv(fd, &header, 1, MSG_DONTWAIT);
    if (n <= 0) {
        return;
    }

    uint8_t packet_type = header & 0xF0;
    size_t remaining = 0;
    if (mqtt_read_remaining_len(fd, &remaining) != 0) {
        ESP_LOGW(TAG, "MQTT packet remaining length decode failed");
        return;
    }

    uint8_t payload[MQTT_BUF_SIZE];
    if (remaining >= sizeof(payload)) {
        ESP_LOGW(TAG, "MQTT inbound packet too large: %d bytes", (int)remaining);
        return;
    }
    n = recv(fd, payload, remaining, MSG_WAITALL);
    if (n != (int)remaining) {
        return;
    }

    if (packet_type == 0x30 && remaining >= 2) {
        size_t topic_len = ((size_t)payload[0] << 8) | payload[1];
        if (2 + topic_len < remaining) {
            const uint8_t *topic = &payload[2];
            const char *msg = (const char *)&payload[2 + topic_len];
            size_t msg_len = remaining - 2 - topic_len;
            if (mqtt_topic_equals(topic, topic_len, ESPAGENT_SENSOR_MQTT_TOPIC_COMMAND)) {
                ESP_LOGI(TAG, "Mesh command received for %s: %.*s",
                         ESPAGENT_NODE_ID, (int)msg_len, msg);
                handle_mesh_command("node", msg, msg_len);
            } else if (mqtt_topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_ROLE_COMMAND)) {
                ESP_LOGI(TAG, "Mesh role command received for %s: %.*s",
                         ESPAGENT_NODE_ROLE, (int)msg_len, msg);
                handle_mesh_command("role", msg, msg_len);
            } else if (mqtt_topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_DISPATCH)) {
                ESP_LOGI(TAG, "Mesh dispatch received: %.*s", (int)msg_len, msg);
            } else if (mqtt_topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_ALERTS)) {
                ESP_LOGI(TAG, "Mesh alert received: %.*s", (int)msg_len, msg);
            } else if (mqtt_topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_TIMELINE)) {
                output_cache_maybe_store_payload(msg, msg_len);
                guardian_audit_timeline_payload(msg, msg_len);
                ESP_LOGI(TAG, "Mesh timeline received: %.*s", (int)msg_len, msg);
            } else if (mqtt_topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_POLICY_CHECK)) {
                ESP_LOGI(TAG, "Policy check received: %.*s", (int)msg_len, msg);
                handle_guardian_policy_check(msg, msg_len);
            } else if (mqtt_topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_POLICY_DECISION)) {
                policy_cache_maybe_store_payload(msg, msg_len);
                ESP_LOGI(TAG, "Policy decision received: %.*s", (int)msg_len, msg);
            } else {
                ESP_LOGI(TAG, "MQTT inbound %.*s: %.*s",
                         (int)topic_len, (const char *)topic,
                         (int)msg_len, msg);
            }
        }
    }
}

static esp_err_t publish_node_state(int fd, const char *state)
{
    char json[640];
    int64_t ts_ms = esp_timer_get_time() / 1000;
    snprintf(json, sizeof(json),
             "{\"node_id\":\"%s\",\"role\":\"%s\",\"location\":\"%s\","
             "\"capabilities\":\"%s\",\"responsibilities\":\"%s\","
             "\"type\":\"state\",\"state\":\"%s\",\"ts_ms\":%lld}",
             espagent_node_id(), espagent_node_role(), espagent_node_location(),
             espagent_node_capabilities(), espagent_node_responsibilities(),
             state, (long long)ts_ms);

    if (mqtt_publish(fd, ESPAGENT_SENSOR_MQTT_TOPIC_STATE, json) != 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT publish %s: %s", ESPAGENT_SENSOR_MQTT_TOPIC_STATE, json);
    return ESP_OK;
}

static esp_err_t publish_node_event(int fd, const char *event_type, const char *detail)
{
    char json[512];
    mqtt_publish_node_event_json(json, sizeof(json), event_type, detail);

    if (mqtt_publish(fd, ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json) != 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT publish %s: %s", ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json);
    return ESP_OK;
}

static esp_err_t publish_sensor_data(int fd)
{
    if (!espagent_node_should_publish_sensor_telemetry()) {
        ESP_LOGD(TAG, "skip sensor telemetry for role=%s capabilities=%s",
                 espagent_node_role(), espagent_node_capabilities());
        return publish_node_state(fd, "online");
    }

    tool_environment_values_t values = {0};
    char env_status[96] = {0};
    esp_err_t env_err = tool_environment_read_values(&values, env_status, sizeof(env_status));
    if (env_err != ESP_OK ||
        values.temperature_c_x10 == -1 ||
        values.humidity_percent_x10 == -1) {
        ESP_LOGW(TAG, "skip MQTT telemetry: AHT20 unavailable (%s, %s)",
                 esp_err_to_name(env_err), env_status);
        return publish_node_state(fd, "online");
    }

    char json[384];
    int64_t ts_ms = esp_timer_get_time() / 1000;
    double temp = (double)values.temperature_c_x10 / 10.0;
    double humidity = (double)values.humidity_percent_x10 / 10.0;
    snprintf(json, sizeof(json),
             "{\"node_id\":\"%s\",\"role\":\"%s\",\"location\":\"%s\","
             "\"capabilities\":\"%s\","
             "\"type\":\"telemetry\",\"temp\":%.1f,\"humidity\":%.1f,"
             "\"co2\":%d,\"tvoc\":%d,\"light_lux\":%.1f,"
             "\"sensor\":\"AHT20\",\"status\":\"%s\",\"ts_ms\":%lld}",
             espagent_node_id(), espagent_node_role(), espagent_node_location(),
             espagent_node_capabilities(),
             temp, humidity,
             values.co2eq_ppm,
             values.tvoc_ppb,
             (double)values.light_lux_x10 / 10.0,
             env_status,
             (long long)ts_ms);

    if (mqtt_publish(fd, ESPAGENT_SENSOR_MQTT_TOPIC_TELEMETRY, json) != 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT publish %s: %s", ESPAGENT_SENSOR_MQTT_TOPIC_TELEMETRY, json);
    return ESP_OK;
}

static void sensor_mqtt_task(void *arg)
{
    (void)arg;
    const bool publish_telemetry = espagent_role_runs_sensor_sampling();

    while (1) {
        int fd = mqtt_connect_tcp();
        if (fd < 0 || mqtt_send_connect(fd) != 0 || mqtt_read_connack(fd) != 0) {
            ESP_LOGW(TAG, "MQTT connect failed");
            if (fd >= 0) {
                close(fd);
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "MQTT connected to %s:%d", ESPAGENT_SENSOR_MQTT_BROKER, ESPAGENT_SENSOR_MQTT_PORT);
        publish_node_state(fd, "online");
        publish_node_event(fd, "mqtt_connected", "sensor_mqtt connected");
        mqtt_subscribe(fd, ESPAGENT_SENSOR_MQTT_TOPIC_COMMAND, 1);
        mqtt_subscribe(fd, ESPAGENT_MESH_TOPIC_ROLE_COMMAND, 2);
        mqtt_subscribe(fd, ESPAGENT_MESH_TOPIC_DISPATCH, 3);
        mqtt_subscribe(fd, ESPAGENT_MESH_TOPIC_ALERTS, 4);
        mqtt_subscribe(fd, ESPAGENT_MESH_TOPIC_TIMELINE, 5);
        mqtt_subscribe(fd, ESPAGENT_MESH_TOPIC_POLICY_CHECK, 6);
        mqtt_subscribe(fd, ESPAGENT_MESH_TOPIC_POLICY_DECISION, 7);

        int64_t last_periodic_publish_ms = 0;
        while (1) {
            mqtt_poll_inbound(fd);
            mqtt_flush_queued_publishes(fd);

            int64_t now_ms = esp_timer_get_time() / 1000;
            if (last_periodic_publish_ms == 0 ||
                now_ms - last_periodic_publish_ms >= ESPAGENT_SENSOR_MQTT_PUBLISH_INTERVAL_MS) {
                last_periodic_publish_ms = now_ms;
                if (publish_telemetry) {
                    esp_err_t telemetry_err = publish_sensor_data(fd);
                    if (telemetry_err == ESP_FAIL) {
                        ESP_LOGW(TAG, "MQTT publish failed, reconnecting");
                        publish_node_event(fd, "mqtt_reconnect", "telemetry publish failed");
                        close(fd);
                        break;
                    }
                } else if (publish_node_state(fd, "online") != ESP_OK) {
                    ESP_LOGW(TAG, "MQTT state publish failed, reconnecting");
                    close(fd);
                    break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

esp_err_t sensor_mqtt_start(void)
{
    if (ESPAGENT_SENSOR_MQTT_BROKER[0] == '\0') {
        ESP_LOGI(TAG, "Sensor MQTT disabled: ESPAGENT_SECRET_SENSOR_MQTT_BROKER is empty");
        return ESP_OK;
    }

    if (!s_pub_queue) {
        s_pub_queue = xQueueCreate(MQTT_PUB_QUEUE_DEPTH, sizeof(mqtt_pub_item_t));
        if (!s_pub_queue) {
            ESP_LOGE(TAG, "Failed to create MQTT publish queue");
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ok = xTaskCreatePinnedToCore(sensor_mqtt_task, "sensor_mqtt",
                                           ESPAGENT_SENSOR_MQTT_STACK, NULL,
                                           ESPAGENT_SENSOR_MQTT_PRIO, NULL,
                                           ESPAGENT_SENSOR_MQTT_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

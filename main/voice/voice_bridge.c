#include "voice/voice_bridge.h"

#include "bus/message_bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espagent_config.h"
#include "sensors/sensor_mqtt.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "voice_bridge";

static bool topic_equals(const char *topic,
                         size_t topic_len,
                         const char *expected)
{
    return topic && expected &&
           strlen(expected) == topic_len &&
           strncmp(topic, expected, topic_len) == 0;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void json_add_string_if(cJSON *root, const char *key, const char *value)
{
    if (root && key && value && value[0]) {
        cJSON_AddStringToObject(root, key, value);
    }
}

esp_err_t espagent_voice_publish_tts_request(const char *text,
                                             const char *source_channel,
                                             const char *chat_id,
                                             const char *trace_id)
{
    if (!text || !text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    char request_id[48];
    snprintf(request_id, sizeof(request_id), "tts-%lld", (long long)now_ms());

    cJSON_AddStringToObject(root, "schema", "espagent.voice.tts_request.v1");
    cJSON_AddStringToObject(root, "event", "tts_request");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "device_id", ESPAGENT_VOICE_DEFAULT_DEVICE);
    cJSON_AddStringToObject(root, "text", text);
    cJSON_AddBoolToObject(root, "auto_play", true);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());
    json_add_string_if(root, "source_channel", source_channel);
    json_add_string_if(root, "chat_id", chat_id);
    json_add_string_if(root, "trace_id", trace_id);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_VOICE_TTS_REQUEST, json);
    if (err == ESP_OK) {
        (void)sensor_mqtt_publish_timeline_event("voice",
                                                 "tts_request",
                                                 "queued",
                                                 text,
                                                 request_id,
                                                 "display_agent",
                                                 ESPAGENT_VOICE_DEFAULT_DEVICE,
                                                 "tts_request");
    }
    cJSON_free(json);
    return err;
}

esp_err_t espagent_voice_publish_stt_request(const char *session_id,
                                             const char *reply_channel,
                                             const char *reply_chat_id,
                                             const char *hint_text,
                                             bool auto_route_reply)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    char request_id[48];
    snprintf(request_id, sizeof(request_id), "%s",
             (session_id && session_id[0]) ? session_id : "stt-session");

    cJSON_AddStringToObject(root, "schema", "espagent.voice.stt_request.v1");
    cJSON_AddStringToObject(root, "event", "stt_request");
    cJSON_AddStringToObject(root, "request_id", request_id);
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "device_id", ESPAGENT_VOICE_DEFAULT_DEVICE);
    cJSON_AddBoolToObject(root, "auto_route_reply", auto_route_reply);
    cJSON_AddNumberToObject(root, "ts_ms", (double)now_ms());
    json_add_string_if(root, "reply_channel", reply_channel);
    json_add_string_if(root, "reply_chat_id", reply_chat_id);
    json_add_string_if(root, "hint_text", hint_text);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_VOICE_STT_REQUEST, json);
    if (err == ESP_OK) {
        (void)sensor_mqtt_publish_timeline_event("voice",
                                                 "stt_request",
                                                 "queued",
                                                 hint_text && hint_text[0] ? hint_text : "Voice capture requested",
                                                 request_id,
                                                 "display_agent",
                                                 ESPAGENT_VOICE_DEFAULT_DEVICE,
                                                 "stt_request");
    }
    cJSON_free(json);
    return err;
}

esp_err_t espagent_voice_build_status_json(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "schema", "espagent.voice.status.v1");
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "device_id", ESPAGENT_VOICE_DEFAULT_DEVICE);
    cJSON_AddBoolToObject(root, "auto_tts_enabled", ESPAGENT_VOICE_AUTO_TTS != 0);
    cJSON_AddStringToObject(root, "tts_request_topic", ESPAGENT_MESH_TOPIC_VOICE_TTS_REQUEST);
    cJSON_AddStringToObject(root, "tts_status_topic", ESPAGENT_MESH_TOPIC_VOICE_TTS_STATUS);
    cJSON_AddStringToObject(root, "stt_request_topic", ESPAGENT_MESH_TOPIC_VOICE_STT_REQUEST);
    cJSON_AddStringToObject(root, "stt_result_topic", ESPAGENT_MESH_TOPIC_VOICE_STT_RESULT);
    cJSON_AddBoolToObject(root, "mqtt_connected", sensor_mqtt_is_connected());

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

static bool handle_stt_result(const char *payload, size_t payload_len)
{
    char json_buf[1024];
    size_t copy_len = payload_len >= sizeof(json_buf) ? sizeof(json_buf) - 1 : payload_len;
    memcpy(json_buf, payload, copy_len);
    json_buf[copy_len] = '\0';

    cJSON *root = cJSON_Parse(json_buf);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *transcript = cJSON_GetObjectItem(root, "transcript");
    const cJSON *status = cJSON_GetObjectItem(root, "status");
    const cJSON *request_id = cJSON_GetObjectItem(root, "request_id");
    const cJSON *reply_channel = cJSON_GetObjectItem(root, "reply_channel");
    const cJSON *reply_chat_id = cJSON_GetObjectItem(root, "reply_chat_id");

    if (!cJSON_IsString(transcript) || transcript->valuestring[0] == '\0') {
        ESP_LOGI(TAG, "STT result ignored: no transcript. status=%s",
                 cJSON_IsString(status) ? status->valuestring : "unknown");
        cJSON_Delete(root);
        return true;
    }

    espagent_msg_t msg = {0};
    snprintf(msg.channel, sizeof(msg.channel), "%s",
             cJSON_IsString(reply_channel) && reply_channel->valuestring[0]
                 ? reply_channel->valuestring
                 : ESPAGENT_CHAN_VOICE);
    snprintf(msg.chat_id, sizeof(msg.chat_id), "%s",
             cJSON_IsString(reply_chat_id) && reply_chat_id->valuestring[0]
                 ? reply_chat_id->valuestring
                 : (cJSON_IsString(request_id) ? request_id->valuestring : "voice-session"));
    msg.content = strdup(transcript->valuestring);
    if (!msg.content) {
        cJSON_Delete(root);
        return true;
    }

    esp_err_t err = message_bus_push_inbound(&msg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Injected voice transcript into agent_loop: channel=%s chat_id=%s text=%s",
                 msg.channel, msg.chat_id, transcript->valuestring);
        (void)sensor_mqtt_publish_timeline_event("voice",
                                                 "stt_result",
                                                 "ok",
                                                 transcript->valuestring,
                                                 cJSON_IsString(request_id) ? request_id->valuestring : NULL,
                                                 ESPAGENT_NODE_ROLE,
                                                 ESPAGENT_NODE_ID,
                                                 "stt_result");
    } else {
        free(msg.content);
        ESP_LOGW(TAG, "Failed to inject voice transcript: %s", esp_err_to_name(err));
    }

    cJSON_Delete(root);
    return true;
}

bool espagent_voice_handle_mqtt_message(const char *topic,
                                        size_t topic_len,
                                        const char *payload,
                                        size_t payload_len)
{
    if (topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_VOICE_STT_RESULT)) {
        return handle_stt_result(payload, payload_len);
    }

    if (topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_VOICE_TTS_STATUS) ||
        topic_equals(topic, topic_len, ESPAGENT_MESH_TOPIC_VOICE_EVENTS)) {
        char detail[192];
        size_t copy_len = payload_len >= sizeof(detail) ? sizeof(detail) - 1 : payload_len;
        memcpy(detail, payload, copy_len);
        detail[copy_len] = '\0';
        ESP_LOGI(TAG, "Voice MQTT event on %.*s: %s", (int)topic_len, topic, detail);
        return true;
    }

    return false;
}

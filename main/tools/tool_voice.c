#include "tools/tool_voice.h"

#include "voice/voice_bridge.h"

#include "cJSON.h"

#include <stdbool.h>
#include <stdio.h>

static const char *json_string(cJSON *root, const char *key)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool json_bool(cJSON *root, const char *key, bool default_value)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    if (!item) {
        return default_value;
    }
    return cJSON_IsTrue(item);
}

esp_err_t tool_voice_tts_request_execute(const char *input_json,
                                         char *output,
                                         size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *text = json_string(root, "text");
    const char *source_channel = json_string(root, "source_channel");
    const char *chat_id = json_string(root, "chat_id");
    const char *trace_id = json_string(root, "trace_id");
    if (!text || !text[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: text is required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = espagent_voice_publish_tts_request(text, source_channel, chat_id, trace_id);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: published TTS request to display_agent");
    } else {
        snprintf(output, output_size, "Error: failed to publish TTS request: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t tool_voice_stt_request_execute(const char *input_json,
                                         char *output,
                                         size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *session_id = json_string(root, "session_id");
    const char *reply_channel = json_string(root, "reply_channel");
    const char *reply_chat_id = json_string(root, "reply_chat_id");
    const char *hint_text = json_string(root, "hint_text");
    bool auto_route_reply = json_bool(root, "auto_route_reply", true);

    esp_err_t err = espagent_voice_publish_stt_request(session_id,
                                                       reply_channel,
                                                       reply_chat_id,
                                                       hint_text,
                                                       auto_route_reply);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: published STT capture request to display_agent");
    } else {
        snprintf(output, output_size, "Error: failed to publish STT request: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t tool_voice_status_execute(const char *input_json,
                                    char *output,
                                    size_t output_size)
{
    (void)input_json;
    return espagent_voice_build_status_json(output, output_size);
}

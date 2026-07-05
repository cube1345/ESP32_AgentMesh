#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t espagent_voice_publish_tts_request(const char *text,
                                             const char *source_channel,
                                             const char *chat_id,
                                             const char *trace_id);

esp_err_t espagent_voice_publish_stt_request(const char *session_id,
                                             const char *reply_channel,
                                             const char *reply_chat_id,
                                             const char *hint_text,
                                             bool auto_route_reply);

esp_err_t espagent_voice_build_status_json(char *output, size_t output_size);

bool espagent_voice_handle_mqtt_message(const char *topic,
                                        size_t topic_len,
                                        const char *payload,
                                        size_t payload_len);

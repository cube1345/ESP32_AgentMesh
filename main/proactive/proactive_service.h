#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t proactive_service_init(void);
esp_err_t proactive_service_start(void);
void proactive_service_stop(void);

esp_err_t proactive_service_note_contact(const char *channel,
                                         const char *chat_id);
esp_err_t proactive_service_trigger_now(void);
void proactive_service_status(char *out, size_t out_size);

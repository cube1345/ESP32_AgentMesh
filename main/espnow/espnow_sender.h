#pragma once

#include "esp_err.h"

esp_err_t espnow_sender_init(void);
esp_err_t espnow_sender_send_text(const char *topic, const char *text);


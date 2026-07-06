#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

bool espagent_voice_local_tts_enabled(void);
bool espagent_voice_local_tts_configured(void);
esp_err_t espagent_voice_local_tts_speak(const char *text,
                                         char *diag,
                                         size_t diag_size);

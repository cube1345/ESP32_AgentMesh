#include "voice/local_tts.h"

#include "esp_log.h"
#include "espagent_config.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "voice_local_tts";

bool espagent_voice_local_tts_enabled(void)
{
    return ESPAGENT_VOICE_LOCAL_TTS_ENABLE != 0;
}

bool espagent_voice_local_tts_configured(void)
{
    return ESPAGENT_VOLC_TTS_APP_ID[0] != '\0' &&
           ESPAGENT_VOLC_TTS_API_KEY[0] != '\0' &&
           ESPAGENT_VOLC_TTS_RESOURCE_ID[0] != '\0' &&
           ESPAGENT_VOLC_TTS_SPEAKER[0] != '\0';
}

esp_err_t espagent_voice_local_tts_speak(const char *text,
                                         char *diag,
                                         size_t diag_size)
{
    if (diag && diag_size > 0) {
        diag[0] = '\0';
    }

    if (!text || !text[0]) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: empty TTS text");
        }
        return ESP_ERR_INVALID_ARG;
    }

    if (!espagent_voice_local_tts_enabled()) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size, "Error: local S3 TTS is disabled");
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!espagent_voice_local_tts_configured()) {
        if (diag && diag_size > 0) {
            snprintf(diag, diag_size,
                     "Error: local S3 TTS missing Volc config (appid/api_key/resource_id/speaker)");
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (diag && diag_size > 0) {
        snprintf(diag, diag_size,
                 "Error: local S3 TTS protocol client is not linked yet; text_len=%u appid=%s resource=%s speaker=%s",
                 (unsigned)strlen(text),
                 ESPAGENT_VOLC_TTS_APP_ID,
                 ESPAGENT_VOLC_TTS_RESOURCE_ID,
                 ESPAGENT_VOLC_TTS_SPEAKER);
    }
    ESP_LOGW(TAG, "%s", diag && diag[0] ? diag : "local S3 TTS protocol client is not linked yet");
    return ESP_ERR_NOT_SUPPORTED;
}

#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t memory_v2_init(void);

esp_err_t memory_v2_upsert_profile_fact(const char *key,
                                        const char *value,
                                        const char *source,
                                        float confidence);

esp_err_t memory_v2_append_skill_observation(const char *skill,
                                             const char *status,
                                             const char *summary);

esp_err_t memory_v2_build_profile_summary(char *buf, size_t size);

esp_err_t memory_v2_build_skill_summary(char *buf, size_t size);

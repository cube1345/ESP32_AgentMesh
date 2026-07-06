#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t memory_v2_init(void);

void memory_v2_normalize_profile_key(const char *raw_key,
                                     char *normalized_key,
                                     size_t normalized_key_size);

esp_err_t memory_v2_upsert_profile_fact(const char *key,
                                        const char *value,
                                        const char *source,
                                        float confidence,
                                        char *change_note,
                                        size_t change_note_size);

esp_err_t memory_v2_append_skill_observation(const char *skill,
                                             const char *status,
                                             const char *summary);

esp_err_t memory_v2_build_profile_summary(char *buf, size_t size);
esp_err_t memory_v2_build_relevant_profile_summary(const char *query,
                                                   char *buf,
                                                   size_t size);
esp_err_t memory_v2_build_relevant_profile_conflict_summary(const char *query,
                                                            char *buf,
                                                            size_t size);

esp_err_t memory_v2_build_skill_summary(char *buf, size_t size);
esp_err_t memory_v2_build_relevant_skill_summary(const char *query,
                                                 char *buf,
                                                 size_t size);

esp_err_t memory_v2_clear_all(void);

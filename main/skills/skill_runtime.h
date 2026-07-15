#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t skill_runtime_list_json(char *buf, size_t size);

esp_err_t skill_runtime_get_json(const char *name, char **json_out);

esp_err_t skill_runtime_upsert(const char *name,
                               const char *content,
                               bool confirmed,
                               char *message,
                               size_t message_size);

esp_err_t skill_runtime_delete(const char *name,
                               bool confirmed,
                               char *message,
                               size_t message_size);

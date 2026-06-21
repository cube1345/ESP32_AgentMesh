#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

esp_err_t guardian_approval_queue_init(void);

esp_err_t guardian_approval_add(const char *command_id,
                                const char *trace_id,
                                const char *action,
                                const char *target_role,
                                const char *args_json,
                                const char *reason,
                                char *approval_id,
                                size_t approval_id_size);

esp_err_t guardian_approval_list_json(char *output, size_t output_size);

esp_err_t guardian_approval_resolve(const char *approval_id,
                                    bool approve,
                                    char *output,
                                    size_t output_size);

esp_err_t guardian_approval_consume(const char *approval_id,
                                    const char *action,
                                    const char *target_role,
                                    char *reason,
                                    size_t reason_size);

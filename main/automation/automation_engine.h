#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t automation_engine_init(void);
esp_err_t automation_engine_start(void);
void automation_engine_stop(void);

esp_err_t automation_engine_create_workflow(const char *input_json,
                                           char *output,
                                           size_t output_size);

esp_err_t automation_engine_create_rule(const char *input_json,
                                        char *output,
                                        size_t output_size);

esp_err_t automation_engine_list(char *output, size_t output_size);

esp_err_t automation_engine_remove(const char *id,
                                   char *output,
                                   size_t output_size);

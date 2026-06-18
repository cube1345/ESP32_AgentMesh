#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_automation_create_workflow_execute(const char *input_json,
                                                  char *output,
                                                  size_t output_size);

esp_err_t tool_automation_create_rule_execute(const char *input_json,
                                             char *output,
                                             size_t output_size);

esp_err_t tool_automation_list_execute(const char *input_json,
                                      char *output,
                                      size_t output_size);

esp_err_t tool_automation_remove_execute(const char *input_json,
                                         char *output,
                                         size_t output_size);

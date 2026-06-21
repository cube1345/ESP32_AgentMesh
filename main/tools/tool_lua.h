#pragma once

#include "esp_err.h"
#include <stddef.h>

esp_err_t tool_lua_runtime_info_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_lua_list_modules_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_lua_list_scripts_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_lua_run_source_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_lua_run_script_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_lua_run_script_async_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_lua_list_jobs_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_lua_get_job_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_lua_stop_job_execute(const char *input_json, char *output, size_t output_size);

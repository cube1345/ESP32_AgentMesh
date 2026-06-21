#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ESPAGENT_LUA_JOB_ID_MAX 24

bool espagent_lua_runtime_available(void);

const char *espagent_lua_runtime_status(void);

bool espagent_lua_script_path_is_valid(const char *path);

esp_err_t espagent_lua_modules_json(char *output, size_t output_size);

esp_err_t espagent_lua_scripts_json(char *output, size_t output_size);

esp_err_t espagent_lua_run_source(const char *source,
                                  const char *args_json,
                                  uint32_t timeout_ms,
                                  char *output,
                                  size_t output_size);

esp_err_t espagent_lua_run_source_with_stop(const char *source,
                                            const char *args_json,
                                            uint32_t timeout_ms,
                                            volatile bool *stop_requested,
                                            char *output,
                                            size_t output_size);

esp_err_t espagent_lua_run_file(const char *path,
                                const char *args_json,
                                uint32_t timeout_ms,
                                char *output,
                                size_t output_size);

esp_err_t espagent_lua_run_file_with_stop(const char *path,
                                          const char *args_json,
                                          uint32_t timeout_ms,
                                          volatile bool *stop_requested,
                                          char *output,
                                          size_t output_size);

esp_err_t espagent_lua_start_async(const char *path,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   char *job_id,
                                   size_t job_id_size);

esp_err_t espagent_lua_jobs_json(char *output, size_t output_size);

esp_err_t espagent_lua_job_get_json(const char *job_id, char *output, size_t output_size);

esp_err_t espagent_lua_job_stop(const char *job_id, char *output, size_t output_size);

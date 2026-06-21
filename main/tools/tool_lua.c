#include "tools/tool_lua.h"

#include "cJSON.h"
#include "esp_err.h"
#include "espagent_config.h"
#include "lua/espagent_lua_runtime.h"

#include <stdio.h>

static const char *json_string_value(cJSON *root, const char *key)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_int_value(cJSON *root, const char *key, int fallback)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

esp_err_t tool_lua_runtime_info_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    snprintf(output, output_size,
             "{\"schema\":\"espagent.lua_runtime.v1\",\"available\":%s,"
             "\"status\":\"%s\",\"scripts_root\":\"%s/scripts\","
             "\"skills_root\":\"%s/skills\",\"max_script_bytes\":%d,"
             "\"default_timeout_ms\":%d,\"max_timeout_ms\":%d}",
             espagent_lua_runtime_available() ? "true" : "false",
             espagent_lua_runtime_status(),
             ESPAGENT_SPIFFS_BASE,
             ESPAGENT_SPIFFS_BASE,
             ESPAGENT_LUA_MAX_SCRIPT_BYTES,
             ESPAGENT_LUA_DEFAULT_TIMEOUT_MS,
             ESPAGENT_LUA_MAX_TIMEOUT_MS);
    return ESP_OK;
}

esp_err_t tool_lua_list_modules_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    return espagent_lua_modules_json(output, output_size);
}

esp_err_t tool_lua_list_scripts_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    return espagent_lua_scripts_json(output, output_size);
}

esp_err_t tool_lua_run_source_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *source = json_string_value(root, "source");
    const char *args_json = json_string_value(root, "args_json");
    cJSON *args = cJSON_GetObjectItem(root, "args");
    char *printed_args = NULL;
    if (!args_json && args && cJSON_IsObject(args)) {
        printed_args = cJSON_PrintUnformatted(args);
        args_json = printed_args;
    }
    uint32_t timeout_ms = (uint32_t)json_int_value(root,
                                                   "timeout_ms",
                                                   ESPAGENT_LUA_DEFAULT_TIMEOUT_MS);
    if (!source || !source[0]) {
        cJSON_free(printed_args);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: source is required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = espagent_lua_run_source(source,
                                            args_json ? args_json : "{}",
                                            timeout_ms,
                                            output,
                                            output_size);
    cJSON_free(printed_args);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_lua_run_script_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = json_string_value(root, "path");
    const char *args_json = json_string_value(root, "args_json");
    cJSON *args = cJSON_GetObjectItem(root, "args");
    char *printed_args = NULL;
    if (!args_json && args && cJSON_IsObject(args)) {
        printed_args = cJSON_PrintUnformatted(args);
        args_json = printed_args;
    }
    uint32_t timeout_ms = (uint32_t)json_int_value(root,
                                                   "timeout_ms",
                                                   ESPAGENT_LUA_DEFAULT_TIMEOUT_MS);
    if (!path || !path[0]) {
        cJSON_free(printed_args);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path is required");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = espagent_lua_run_file(path,
                                          args_json ? args_json : "{}",
                                          timeout_ms,
                                          output,
                                          output_size);
    cJSON_free(printed_args);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_lua_run_script_async_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = json_string_value(root, "path");
    const char *args_json = json_string_value(root, "args_json");
    cJSON *args = cJSON_GetObjectItem(root, "args");
    char *printed_args = NULL;
    if (!args_json && args && cJSON_IsObject(args)) {
        printed_args = cJSON_PrintUnformatted(args);
        args_json = printed_args;
    }
    uint32_t timeout_ms = (uint32_t)json_int_value(root,
                                                   "timeout_ms",
                                                   ESPAGENT_LUA_DEFAULT_TIMEOUT_MS);
    if (!path || !path[0]) {
        cJSON_free(printed_args);
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: path is required");
        return ESP_ERR_INVALID_ARG;
    }
    char path_copy[160];
    snprintf(path_copy, sizeof(path_copy), "%s", path);

    char job_id[ESPAGENT_LUA_JOB_ID_MAX] = {0};
    esp_err_t err = espagent_lua_start_async(path,
                                             args_json ? args_json : "{}",
                                             timeout_ms,
                                             job_id,
                                             sizeof(job_id));
    cJSON_free(printed_args);
    cJSON_Delete(root);
    if (err == ESP_OK) {
        snprintf(output, output_size,
                 "{\"schema\":\"espagent.lua_async_started.v1\",\"job_id\":\"%s\",\"path\":\"%s\"}",
                 job_id, path_copy);
    } else {
        snprintf(output, output_size, "Error: Lua async start failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t tool_lua_list_jobs_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;
    return espagent_lua_jobs_json(output, output_size);
}

esp_err_t tool_lua_get_job_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    const char *job_id = json_string_value(root, "job_id");
    if (!job_id || !job_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: job_id is required");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = espagent_lua_job_get_json(job_id, output, output_size);
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_lua_stop_job_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    const char *job_id = json_string_value(root, "job_id");
    if (!job_id || !job_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: job_id is required");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = espagent_lua_job_stop(job_id, output, output_size);
    cJSON_Delete(root);
    return err;
}

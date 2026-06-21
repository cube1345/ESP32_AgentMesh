#include "lua/espagent_lua_runtime.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "espagent_config.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if ESPAGENT_ENABLE_LUA_RUNTIME && defined(__has_include)
#if __has_include("lua.h") && __has_include("lauxlib.h") && __has_include("lualib.h")
#define ESPAGENT_LUA_HAS_INTERPRETER 1
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#endif
#endif

#ifndef ESPAGENT_LUA_HAS_INTERPRETER
#define ESPAGENT_LUA_HAS_INTERPRETER 0
#endif

bool espagent_lua_runtime_available(void)
{
    return ESPAGENT_LUA_HAS_INTERPRETER != 0;
}

const char *espagent_lua_runtime_status(void)
{
#if ESPAGENT_LUA_HAS_INTERPRETER
    return "available";
#elif ESPAGENT_ENABLE_LUA_RUNTIME
    return "enabled_but_lua_headers_missing";
#else
    return "disabled_build_flag";
#endif
}

static bool has_lua_suffix(const char *path)
{
    if (!path) {
        return false;
    }
    size_t len = strlen(path);
    return len > 4 && strcmp(path + len - 4, ".lua") == 0;
}

bool espagent_lua_script_path_is_valid(const char *path)
{
    if (!path || path[0] != '/' || !has_lua_suffix(path) || strstr(path, "..")) {
        return false;
    }
    return strncmp(path, ESPAGENT_SPIFFS_BASE "/scripts/", strlen(ESPAGENT_SPIFFS_BASE "/scripts/")) == 0 ||
           strncmp(path, ESPAGENT_SPIFFS_BASE "/skills/", strlen(ESPAGENT_SPIFFS_BASE "/skills/")) == 0;
}

static void add_module_json(cJSON *arr,
                            const char *name,
                            const char *status,
                            const char *description)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "name", name);
    cJSON_AddStringToObject(obj, "status", status);
    cJSON_AddStringToObject(obj, "description", description);
    cJSON_AddItemToArray(arr, obj);
}

esp_err_t espagent_lua_modules_json(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.lua_modules.v1");
    cJSON_AddStringToObject(root, "runtime_status", espagent_lua_runtime_status());
    cJSON_AddStringToObject(root, "module_model", "espagent_safe_capability_bridge");
    cJSON_AddItemToObject(root, "modules", arr);
    add_module_json(arr, "espagent", "available",
                    "Safe ESPAgent bridge: call_capability(name,args_json), delay_ms(ms), now_ms(), runtime_info().");
    add_module_json(arr, "_G", "available", "Lua base library with dofile/loadfile removed.");
    add_module_json(arr, "table", "available", "Lua table library.");
    add_module_json(arr, "string", "available", "Lua string library.");
    add_module_json(arr, "math", "available", "Lua math library.");
    add_module_json(arr, "package", "restricted", "require() is limited to /spiffs/scripts and /spiffs/skills; loadlib/cpath disabled.");
    add_module_json(arr, "gpio/i2c/adc/pwm/rmt/ble/display/camera/audio", "not_linked",
                    "Use espagent.call_capability or manifest tools instead of direct hardware modules in this firmware.");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

static void add_script_path_json(cJSON *arr, const char *path, const struct stat *st)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "path", path);
    if (st) {
        cJSON_AddNumberToObject(obj, "size", (double)st->st_size);
    }
    cJSON_AddItemToArray(arr, obj);
}

static void scan_script_dir(cJSON *arr, const char *dir_path, int depth, int *count)
{
    if (!arr || !dir_path || depth > 4 || !count || *count >= 64) {
        return;
    }
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return;
    }
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL && *count < 64) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char path[192];
        int written = snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            scan_script_dir(arr, path, depth + 1, count);
        } else if (S_ISREG(st.st_mode) && espagent_lua_script_path_is_valid(path)) {
            add_script_path_json(arr, path, &st);
            (*count)++;
        }
    }
    closedir(dir);
}

esp_err_t espagent_lua_scripts_json(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.lua_scripts.v1");
    cJSON_AddStringToObject(root, "runtime_status", espagent_lua_runtime_status());
    cJSON_AddStringToObject(root, "scripts_root", ESPAGENT_SPIFFS_BASE "/scripts");
    cJSON_AddStringToObject(root, "skills_root", ESPAGENT_SPIFFS_BASE "/skills");
    cJSON_AddItemToObject(root, "scripts", arr);
    int count = 0;
    scan_script_dir(arr, ESPAGENT_SPIFFS_BASE "/scripts", 0, &count);
    scan_script_dir(arr, ESPAGENT_SPIFFS_BASE "/skills", 0, &count);
    cJSON_AddNumberToObject(root, "count", count);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

#if ESPAGENT_LUA_HAS_INTERPRETER
#include "tools/tool_registry.h"
static const char *TAG = "lua_runtime";

typedef struct {
    int64_t deadline_us;
    volatile bool *stop_requested;
    char *output;
    size_t output_size;
    size_t output_len;
    bool output_truncated;
} lua_exec_guard_t;

static void lua_output_append(lua_exec_guard_t *guard, const char *text, size_t len)
{
    if (!guard || !guard->output || guard->output_size == 0 || !text || len == 0) {
        return;
    }
    if (guard->output_len >= guard->output_size - 1) {
        guard->output_truncated = true;
        return;
    }
    size_t room = guard->output_size - 1 - guard->output_len;
    size_t copy = len < room ? len : room;
    memcpy(guard->output + guard->output_len, text, copy);
    guard->output_len += copy;
    guard->output[guard->output_len] = '\0';
    if (copy < len) {
        guard->output_truncated = true;
    }
}

static lua_exec_guard_t *lua_current_guard(lua_State *L)
{
    lua_exec_guard_t **slot = (lua_exec_guard_t **)lua_getextraspace(L);
    return slot ? *slot : NULL;
}

static void lua_timeout_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    lua_exec_guard_t *guard = lua_current_guard(L);
    if (guard && guard->stop_requested && *guard->stop_requested) {
        luaL_error(L, "Lua script stop requested");
    }
    if (guard && esp_timer_get_time() > guard->deadline_us) {
        luaL_error(L, "Lua script timeout");
    }
}

static int lua_capture_print(lua_State *L)
{
    lua_exec_guard_t *guard = lua_current_guard(L);
    int top = lua_gettop(L);
    for (int i = 1; i <= top; i++) {
        size_t len = 0;
        const char *text = luaL_tolstring(L, i, &len);
        if (i > 1) {
            lua_output_append(guard, "\t", 1);
        }
        lua_output_append(guard, text, len);
        lua_pop(L, 1);
    }
    lua_output_append(guard, "\n", 1);
    return 0;
}

static int lua_espagent_call_capability(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *args_json = luaL_optstring(L, 2, "{}");
    char output[1024] = {0};
    esp_err_t err = tool_registry_execute_as(name,
                                             args_json,
                                             ESPAGENT_CAP_CALLER_EVENT,
                                             output,
                                             sizeof(output));
    lua_pushboolean(L, err == ESP_OK);
    lua_pushstring(L, output);
    lua_pushinteger(L, err);
    return 3;
}

static int lua_espagent_delay_ms(lua_State *L)
{
    lua_Integer requested = luaL_checkinteger(L, 1);
    lua_exec_guard_t *guard = lua_current_guard(L);
    if (requested < 0 || requested > ESPAGENT_LUA_MAX_TIMEOUT_MS) {
        return luaL_error(L, "delay_ms must be 0..%d", ESPAGENT_LUA_MAX_TIMEOUT_MS);
    }
    if (guard && guard->deadline_us > 0) {
        int64_t remaining_ms = (guard->deadline_us - esp_timer_get_time()) / 1000;
        if (requested > remaining_ms) {
            return luaL_error(L, "delay_ms exceeds remaining timeout");
        }
    }
    vTaskDelay(pdMS_TO_TICKS((uint32_t)requested));
    lua_pushboolean(L, true);
    return 1;
}

static int lua_espagent_now_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int lua_espagent_runtime_info(lua_State *L)
{
    lua_newtable(L);
    lua_pushboolean(L, espagent_lua_runtime_available());
    lua_setfield(L, -2, "available");
    lua_pushstring(L, espagent_lua_runtime_status());
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, ESPAGENT_LUA_MAX_SCRIPT_BYTES);
    lua_setfield(L, -2, "max_script_bytes");
    lua_pushinteger(L, ESPAGENT_LUA_DEFAULT_TIMEOUT_MS);
    lua_setfield(L, -2, "default_timeout_ms");
    lua_pushinteger(L, ESPAGENT_LUA_MAX_TIMEOUT_MS);
    lua_setfield(L, -2, "max_timeout_ms");
    return 1;
}

static int luaopen_espagent(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"call_capability", lua_espagent_call_capability},
        {"delay_ms", lua_espagent_delay_ms},
        {"now_ms", lua_espagent_now_ms},
        {"runtime_info", lua_espagent_runtime_info},
        {NULL, NULL},
    };
    luaL_newlib(L, funcs);
    return 1;
}

static void open_espagent_lib(lua_State *L)
{
    luaL_requiref(L, "espagent", luaopen_espagent, 1);
    lua_setglobal(L, "espagent");
}

static void open_safe_libs(lua_State *L)
{
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_LOADLIBNAME, luaopen_package, 1);
    lua_pushstring(L,
                   ESPAGENT_SPIFFS_BASE "/scripts/?.lua;"
                   ESPAGENT_SPIFFS_BASE "/scripts/?/init.lua;"
                   ESPAGENT_SPIFFS_BASE "/skills/?.lua;"
                   ESPAGENT_SPIFFS_BASE "/skills/?/init.lua");
    lua_setfield(L, -2, "path");
    lua_pushliteral(L, "");
    lua_setfield(L, -2, "cpath");
    lua_pushnil(L);
    lua_setfield(L, -2, "loadlib");
    lua_pop(L, 1);
    open_espagent_lib(L);
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushcfunction(L, lua_capture_print);
    lua_setglobal(L, "print");
}

static esp_err_t push_json_value(lua_State *L, const cJSON *item, int depth)
{
    if (depth > 32) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!item || cJSON_IsNull(item)) {
        lua_pushnil(L);
        return ESP_OK;
    }
    if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
        return ESP_OK;
    }
    if (cJSON_IsNumber(item)) {
        lua_pushnumber(L, item->valuedouble);
        return ESP_OK;
    }
    if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring ? item->valuestring : "");
        return ESP_OK;
    }
    if (cJSON_IsArray(item)) {
        int index = 1;
        cJSON *child = NULL;
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            esp_err_t err = push_json_value(L, child, depth + 1);
            if (err != ESP_OK) {
                lua_pop(L, 1);
                return err;
            }
            lua_rawseti(L, -2, index++);
        }
        return ESP_OK;
    }
    if (cJSON_IsObject(item)) {
        cJSON *child = NULL;
        lua_newtable(L);
        cJSON_ArrayForEach(child, item) {
            esp_err_t err = push_json_value(L, child, depth + 1);
            if (err != ESP_OK) {
                lua_pop(L, 1);
                return err;
            }
            lua_setfield(L, -2, child->string);
        }
        return ESP_OK;
    }
    lua_pushnil(L);
    return ESP_OK;
}

static esp_err_t set_args_global(lua_State *L, const char *args_json)
{
    const char *payload = args_json && args_json[0] ? args_json : "{}";
    cJSON *root = cJSON_Parse(payload);
    if (root) {
        esp_err_t err = push_json_value(L, root, 0);
        cJSON_Delete(root);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        lua_newtable(L);
    }
    lua_setglobal(L, "args");
    lua_pushstring(L, payload);
    lua_setglobal(L, "ESPAGENT_ARGS_JSON");
    return ESP_OK;
}

static esp_err_t finish_lua_result(lua_State *L, lua_exec_guard_t *guard, const char *ok_message)
{
    int top = lua_gettop(L);
    if (top > 0 && lua_isstring(L, top)) {
        size_t len = 0;
        const char *ret = lua_tolstring(L, top, &len);
        if (guard->output_len > 0 &&
            guard->output[guard->output_len - 1] != '\n') {
            lua_output_append(guard, "\n", 1);
        }
        lua_output_append(guard, ret, len);
    } else if (guard->output_len == 0 && ok_message) {
        lua_output_append(guard, ok_message, strlen(ok_message));
    }
    if (guard->output_truncated) {
        lua_output_append(guard, "\n[output truncated]", strlen("\n[output truncated]"));
    }
    return ESP_OK;
}

static esp_err_t lua_prepare_state(lua_State **state_out,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   volatile bool *stop_requested,
                                   char *output,
                                   size_t output_size,
                                   lua_exec_guard_t *guard)
{
    if (!state_out || !guard || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *state_out = NULL;
    output[0] = '\0';
    lua_State *L = luaL_newstate();
    if (!L) {
        snprintf(output, output_size, "Error: cannot allocate Lua state");
        return ESP_ERR_NO_MEM;
    }
    open_safe_libs(L);
    esp_err_t err = set_args_global(L, args_json);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: invalid Lua args_json");
        lua_close(L);
        return err;
    }

    *guard = (lua_exec_guard_t){
        .deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000,
        .stop_requested = stop_requested,
        .output = output,
        .output_size = output_size,
        .output_len = 0,
        .output_truncated = false,
    };
    lua_exec_guard_t **slot = (lua_exec_guard_t **)lua_getextraspace(L);
    if (slot) {
        *slot = guard;
    }
    lua_sethook(L, lua_timeout_hook, LUA_MASKCOUNT, 10000);
    *state_out = L;
    return ESP_OK;
}
#endif

esp_err_t espagent_lua_run_file_with_stop(const char *path,
                                          const char *args_json,
                                          uint32_t timeout_ms,
                                          volatile bool *stop_requested,
                                          char *output,
                                          size_t output_size)
{
    if (!path || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!espagent_lua_script_path_is_valid(path)) {
        snprintf(output, output_size,
                 "Error: Lua path must be an absolute .lua file under %s/scripts/ or %s/skills/ with no '..'",
                 ESPAGENT_SPIFFS_BASE, ESPAGENT_SPIFFS_BASE);
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = ESPAGENT_LUA_DEFAULT_TIMEOUT_MS;
    }
    if (timeout_ms > ESPAGENT_LUA_MAX_TIMEOUT_MS) {
        snprintf(output, output_size, "Error: timeout_ms must be <= %d", ESPAGENT_LUA_MAX_TIMEOUT_MS);
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(output, output_size, "Error: Lua script not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > ESPAGENT_LUA_MAX_SCRIPT_BYTES) {
            fclose(f);
            snprintf(output, output_size, "Error: Lua script too large: %ld > %d",
                     sz, ESPAGENT_LUA_MAX_SCRIPT_BYTES);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    fclose(f);

#if ESPAGENT_LUA_HAS_INTERPRETER
    lua_State *L = NULL;
    lua_exec_guard_t guard = {0};
    esp_err_t prep_err = lua_prepare_state(&L,
                                           args_json,
                                           timeout_ms,
                                           stop_requested,
                                           output,
                                           output_size,
                                           &guard);
    if (prep_err != ESP_OK) {
        return prep_err;
    }

    int rc = luaL_loadfile(L, path);
    if (rc == LUA_OK) {
        rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    }

    if (rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        snprintf(output, output_size, "Error: Lua failed: %s", err ? err : "unknown");
        lua_close(L);
        return ESP_FAIL;
    }

    char ok_message[192];
    snprintf(ok_message, sizeof(ok_message), "OK: Lua script completed path=%s", path);
    finish_lua_result(L, &guard, ok_message);
    lua_close(L);
    ESP_LOGI(TAG, "Lua script completed: %s", path);
    return ESP_OK;
#else
    snprintf(output, output_size,
             "Error: Lua runtime is %s. Add georgik/lua and set ESPAGENT_ENABLE_LUA_RUNTIME=1 to execute %s.",
             espagent_lua_runtime_status(), path);
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t espagent_lua_run_file(const char *path,
                                const char *args_json,
                                uint32_t timeout_ms,
                                char *output,
                                size_t output_size)
{
    return espagent_lua_run_file_with_stop(path,
                                           args_json,
                                           timeout_ms,
                                           NULL,
                                           output,
                                           output_size);
}

esp_err_t espagent_lua_run_source_with_stop(const char *source,
                                            const char *args_json,
                                            uint32_t timeout_ms,
                                            volatile bool *stop_requested,
                                            char *output,
                                            size_t output_size)
{
    if (!source || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t source_len = strlen(source);
    if (source_len == 0 || source_len > ESPAGENT_LUA_MAX_SCRIPT_BYTES) {
        snprintf(output, output_size, "Error: Lua source size must be 1..%d bytes",
                 ESPAGENT_LUA_MAX_SCRIPT_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    if (timeout_ms == 0) {
        timeout_ms = ESPAGENT_LUA_DEFAULT_TIMEOUT_MS;
    }
    if (timeout_ms > ESPAGENT_LUA_MAX_TIMEOUT_MS) {
        snprintf(output, output_size, "Error: timeout_ms must be <= %d", ESPAGENT_LUA_MAX_TIMEOUT_MS);
        return ESP_ERR_INVALID_ARG;
    }

#if ESPAGENT_LUA_HAS_INTERPRETER
    lua_State *L = NULL;
    lua_exec_guard_t guard = {0};
    esp_err_t prep_err = lua_prepare_state(&L,
                                           args_json,
                                           timeout_ms,
                                           stop_requested,
                                           output,
                                           output_size,
                                           &guard);
    if (prep_err != ESP_OK) {
        return prep_err;
    }

    int rc = luaL_loadbuffer(L, source, source_len, "espagent_inline_lua");
    if (rc == LUA_OK) {
        rc = lua_pcall(L, 0, LUA_MULTRET, 0);
    }
    if (rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        snprintf(output, output_size, "Error: Lua failed: %s", err ? err : "unknown");
        lua_close(L);
        return ESP_FAIL;
    }
    finish_lua_result(L, &guard, "OK: Lua source completed");
    lua_close(L);
    ESP_LOGI(TAG, "Lua source completed");
    return ESP_OK;
#else
    snprintf(output, output_size,
             "Error: Lua runtime is %s. Add georgik/lua and set ESPAGENT_ENABLE_LUA_RUNTIME=1 to execute inline source.",
             espagent_lua_runtime_status());
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t espagent_lua_run_source(const char *source,
                                  const char *args_json,
                                  uint32_t timeout_ms,
                                  char *output,
                                  size_t output_size)
{
    return espagent_lua_run_source_with_stop(source,
                                             args_json,
                                             timeout_ms,
                                             NULL,
                                             output,
                                             output_size);
}

typedef enum {
    LUA_JOB_EMPTY = 0,
    LUA_JOB_RUNNING,
    LUA_JOB_DONE,
    LUA_JOB_FAILED,
    LUA_JOB_STOPPED,
} lua_job_state_t;

typedef struct {
    lua_job_state_t state;
    char job_id[ESPAGENT_LUA_JOB_ID_MAX];
    char path[160];
    char args_json[384];
    char output[768];
    uint32_t timeout_ms;
    int64_t started_ms;
    int64_t finished_ms;
    volatile bool stop_requested;
} lua_job_t;

static lua_job_t s_jobs[ESPAGENT_LUA_MAX_JOBS];
static SemaphoreHandle_t s_jobs_lock;

static void jobs_lock(void)
{
    if (!s_jobs_lock) {
        s_jobs_lock = xSemaphoreCreateMutex();
    }
    if (s_jobs_lock) {
        xSemaphoreTake(s_jobs_lock, portMAX_DELAY);
    }
}

static void jobs_unlock(void)
{
    if (s_jobs_lock) {
        xSemaphoreGive(s_jobs_lock);
    }
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *job_state_name(lua_job_state_t state)
{
    switch (state) {
    case LUA_JOB_RUNNING:
        return "running";
    case LUA_JOB_DONE:
        return "done";
    case LUA_JOB_FAILED:
        return "failed";
    case LUA_JOB_STOPPED:
        return "stopped";
    default:
        return "empty";
    }
}

static void gen_job_id(char *buf, size_t size)
{
    snprintf(buf, size, "lua-%08llx",
             (unsigned long long)(esp_timer_get_time() & 0xffffffffULL));
}

static int find_job_locked(const char *job_id)
{
    if (!job_id || !job_id[0]) {
        return -1;
    }
    for (int i = 0; i < ESPAGENT_LUA_MAX_JOBS; i++) {
        if (s_jobs[i].state != LUA_JOB_EMPTY && strcmp(s_jobs[i].job_id, job_id) == 0) {
            return i;
        }
    }
    return -1;
}

static int alloc_job_locked(void)
{
    int oldest_done = -1;
    int64_t oldest_ms = INT64_MAX;
    for (int i = 0; i < ESPAGENT_LUA_MAX_JOBS; i++) {
        if (s_jobs[i].state == LUA_JOB_EMPTY) {
            return i;
        }
        if (s_jobs[i].state != LUA_JOB_RUNNING && s_jobs[i].finished_ms < oldest_ms) {
            oldest_ms = s_jobs[i].finished_ms;
            oldest_done = i;
        }
    }
    return oldest_done;
}

static void lua_job_task(void *arg)
{
    lua_job_t *job = (lua_job_t *)arg;
    char local_path[sizeof(job->path)];
    char local_args[sizeof(job->args_json)];
    uint32_t timeout_ms;

    jobs_lock();
    snprintf(local_path, sizeof(local_path), "%s", job->path);
    snprintf(local_args, sizeof(local_args), "%s", job->args_json);
    timeout_ms = job->timeout_ms;
    jobs_unlock();

    char result[sizeof(job->output)] = {0};
    esp_err_t err = espagent_lua_run_file_with_stop(local_path,
                                                    local_args,
                                                    timeout_ms,
                                                    &job->stop_requested,
                                                    result,
                                                    sizeof(result));

    jobs_lock();
    snprintf(job->output, sizeof(job->output), "%s", result);
    job->finished_ms = now_ms();
    if (job->stop_requested && err != ESP_OK) {
        job->state = LUA_JOB_STOPPED;
    } else {
        job->state = (err == ESP_OK) ? LUA_JOB_DONE : LUA_JOB_FAILED;
    }
    jobs_unlock();

    vTaskDelete(NULL);
}

esp_err_t espagent_lua_start_async(const char *path,
                                   const char *args_json,
                                   uint32_t timeout_ms,
                                   char *job_id,
                                   size_t job_id_size)
{
    if (!path || !job_id || job_id_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!espagent_lua_script_path_is_valid(path)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = ESPAGENT_LUA_DEFAULT_TIMEOUT_MS;
    }
    if (timeout_ms > ESPAGENT_LUA_MAX_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    jobs_lock();
    int slot = alloc_job_locked();
    if (slot < 0) {
        jobs_unlock();
        return ESP_ERR_NO_MEM;
    }

    lua_job_t *job = &s_jobs[slot];
    memset(job, 0, sizeof(*job));
    job->state = LUA_JOB_RUNNING;
    gen_job_id(job->job_id, sizeof(job->job_id));
    snprintf(job->path, sizeof(job->path), "%s", path);
    snprintf(job->args_json, sizeof(job->args_json), "%s", args_json && args_json[0] ? args_json : "{}");
    job->timeout_ms = timeout_ms;
    job->started_ms = now_ms();
    snprintf(job_id, job_id_size, "%s", job->job_id);

    BaseType_t ok = xTaskCreatePinnedToCore(lua_job_task,
                                            "lua_job",
                                            ESPAGENT_LUA_JOB_STACK,
                                            job,
                                            4,
                                            NULL,
                                            0);
    if (ok != pdPASS) {
        job->state = LUA_JOB_FAILED;
        snprintf(job->output, sizeof(job->output), "Error: failed to create lua_job task");
        job->finished_ms = now_ms();
        jobs_unlock();
        return ESP_ERR_NO_MEM;
    }
    jobs_unlock();
    return ESP_OK;
}

static void add_job_json(cJSON *arr, const lua_job_t *job)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return;
    }
    cJSON_AddStringToObject(obj, "job_id", job->job_id);
    cJSON_AddStringToObject(obj, "state", job_state_name(job->state));
    cJSON_AddStringToObject(obj, "path", job->path);
    cJSON_AddNumberToObject(obj, "timeout_ms", job->timeout_ms);
    cJSON_AddNumberToObject(obj, "started_ms", (double)job->started_ms);
    cJSON_AddNumberToObject(obj, "finished_ms", (double)job->finished_ms);
    cJSON_AddStringToObject(obj, "output", job->output);
    cJSON_AddItemToArray(arr, obj);
}

esp_err_t espagent_lua_jobs_json(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (!root || !arr) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "schema", "espagent.lua_jobs.v1");
    cJSON_AddStringToObject(root, "runtime_status", espagent_lua_runtime_status());
    cJSON_AddItemToObject(root, "jobs", arr);

    jobs_lock();
    for (int i = 0; i < ESPAGENT_LUA_MAX_JOBS; i++) {
        if (s_jobs[i].state != LUA_JOB_EMPTY) {
            add_job_json(arr, &s_jobs[i]);
        }
    }
    jobs_unlock();

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t espagent_lua_job_get_json(const char *job_id, char *output, size_t output_size)
{
    if (!job_id || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return ESP_ERR_NO_MEM;
    }

    jobs_lock();
    int idx = find_job_locked(job_id);
    if (idx >= 0) {
        add_job_json(arr, &s_jobs[idx]);
    }
    jobs_unlock();

    if (idx < 0) {
        cJSON_Delete(arr);
        snprintf(output, output_size, "Error: Lua job not found: %s", job_id);
        return ESP_ERR_NOT_FOUND;
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

esp_err_t espagent_lua_job_stop(const char *job_id, char *output, size_t output_size)
{
    if (!job_id || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    jobs_lock();
    int idx = find_job_locked(job_id);
    if (idx < 0) {
        jobs_unlock();
        snprintf(output, output_size, "Error: Lua job not found: %s", job_id);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_jobs[idx].state != LUA_JOB_RUNNING) {
        snprintf(output, output_size, "OK: Lua job %s is already %s",
                 job_id, job_state_name(s_jobs[idx].state));
        jobs_unlock();
        return ESP_OK;
    }
    s_jobs[idx].stop_requested = true;
    snprintf(output, output_size, "OK: stop requested for Lua job %s", job_id);
    jobs_unlock();
    return ESP_OK;
}

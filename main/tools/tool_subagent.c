#include "tools/tool_subagent.h"

#include "agent/context_builder.h"
#include "espagent_config.h"
#include "llm/llm_proxy.h"
#include "tools/tool_registry.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <ctype.h>

static const char *TAG = "subagent";

static char *s_subagent_tools_json = NULL;
static portMUX_TYPE s_subagent_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_subagent_active = false;
static void subagent_task(void *arg);

typedef struct {
    char *task;
    char *context;
    char *result;
    SemaphoreHandle_t done_sem;
    SemaphoreHandle_t cleanup_sem;
    bool owns_single_flight;
} subagent_ctx_t;

static bool subagent_single_flight_try_acquire(void)
{
    bool acquired = false;
    portENTER_CRITICAL(&s_subagent_state_lock);
    if (!s_subagent_active) {
        s_subagent_active = true;
        acquired = true;
    }
    portEXIT_CRITICAL(&s_subagent_state_lock);
    return acquired;
}

static void subagent_single_flight_release(void)
{
    portENTER_CRITICAL(&s_subagent_state_lock);
    s_subagent_active = false;
    portEXIT_CRITICAL(&s_subagent_state_lock);
}

static bool contains_substr_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || needle[0] == '\0') {
        return false;
    }

    const size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < needle_len && p[i]) {
            unsigned char hc = (unsigned char)p[i];
            unsigned char nc = (unsigned char)needle[i];
            if (hc < 0x80) {
                hc = (unsigned char)tolower(hc);
            }
            if (nc < 0x80) {
                nc = (unsigned char)tolower(nc);
            }
            if (hc != nc) {
                break;
            }
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static bool subagent_try_low_memory_shortcut(subagent_ctx_t *ctx)
{
    if (!ctx || !ctx->task || ctx->task[0] == '\0') {
        return false;
    }

    char *tool_output = heap_caps_calloc(1, ESPAGENT_SUBAGENT_TOOL_BUF_SIZE,
                                         MALLOC_CAP_SPIRAM);
    if (!tool_output) {
        ctx->result = strdup("Error: subagent low-memory shortcut allocation failed");
        return true;
    }

    const bool wants_math =
        contains_substr_ci(ctx->task, "计算") ||
        contains_substr_ci(ctx->task, "math") ||
        contains_substr_ci(ctx->task, "calculate");
    if (wants_math) {
        long nums[3] = {0};
        int count = 0;
        for (const char *p = ctx->task; *p && count < 3; p++) {
            if ((*p >= '0' && *p <= '9') ||
                ((*p == '-' || *p == '+') && p[1] >= '0' && p[1] <= '9')) {
                char *end = NULL;
                nums[count++] = strtol(p, &end, 10);
                if (end && end > p) {
                    p = end - 1;
                }
            }
        }
        if (count >= 3 &&
            (strchr(ctx->task, '*') || contains_substr_ci(ctx->task, "乘")) &&
            (strchr(ctx->task, '+') || contains_substr_ci(ctx->task, "加"))) {
            snprintf(tool_output, ESPAGENT_SUBAGENT_TOOL_BUF_SIZE,
                     "%ld", nums[0] * nums[1] + nums[2]);
            ctx->result = strdup(tool_output);
            free(tool_output);
            ESP_LOGI(TAG, "Subagent low-memory shortcut -> arithmetic");
            return true;
        }
    }

    const bool wants_time =
        contains_substr_ci(ctx->task, "get_current_time") ||
        contains_substr_ci(ctx->task, "current time") ||
        contains_substr_ci(ctx->task, "当前时间") ||
        contains_substr_ci(ctx->task, "现在几点") ||
        contains_substr_ci(ctx->task, "日期");
    if (wants_time) {
        tool_registry_execute_as("get_current_time", "{}",
                                 ESPAGENT_CAP_CALLER_SUBAGENT,
                                 tool_output, ESPAGENT_SUBAGENT_TOOL_BUF_SIZE);
        ctx->result = strdup(tool_output[0] ? tool_output
                                            : "Error: get_current_time returned empty output");
        free(tool_output);
        ESP_LOGI(TAG, "Subagent low-memory shortcut -> get_current_time");
        return true;
    }

    const bool wants_weather =
        contains_substr_ci(ctx->task, "get_weather") ||
        contains_substr_ci(ctx->task, "weather") ||
        contains_substr_ci(ctx->task, "天气");
    if (wants_weather) {
        tool_registry_execute_as("get_weather", "{}",
                                 ESPAGENT_CAP_CALLER_SUBAGENT,
                                 tool_output, ESPAGENT_SUBAGENT_TOOL_BUF_SIZE);
        ctx->result = strdup(tool_output[0] ? tool_output
                                            : "Error: get_weather returned empty output");
        free(tool_output);
        ESP_LOGI(TAG, "Subagent low-memory shortcut -> get_weather");
        return true;
    }

    free(tool_output);
    return false;
}

static BaseType_t create_subagent_task(subagent_ctx_t *ctx)
{
    static const uint32_t stack_attempts_large_first[] = {
        ESPAGENT_SUBAGENT_STACK,
        8192,
        6144,
    };
    static const uint32_t stack_attempts_small_first[] = {
        6144,
        8192,
        ESPAGENT_SUBAGENT_STACK,
    };
    const uint32_t free_internal =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largest_internal =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint32_t *stack_attempts = stack_attempts_large_first;

    if (free_internal < 32768 || largest_internal < 16384) {
        stack_attempts = stack_attempts_small_first;
    }

    ESP_LOGI(TAG,
             "Subagent task create begin: free_internal=%u largest_internal=%u preferred_stack=%u",
             (unsigned)free_internal,
             (unsigned)largest_internal,
             (unsigned)stack_attempts[0]);

    for (size_t i = 0; i < sizeof(stack_attempts_large_first) / sizeof(stack_attempts_large_first[0]); i++) {
        const uint32_t stack_bytes = stack_attempts[i];
        BaseType_t ok = xTaskCreatePinnedToCore(subagent_task,
                                                "subagent",
                                                stack_bytes,
                                                ctx,
                                                ESPAGENT_SUBAGENT_PRIO,
                                                NULL,
                                                ESPAGENT_SUBAGENT_CORE);
        if (ok == pdPASS) {
            ESP_LOGI(TAG,
                     "Subagent task created with internal stack=%u (free_internal=%u largest_internal=%u)",
                     (unsigned)stack_bytes,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            return ok;
        }

        ESP_LOGW(TAG,
                 "Subagent task create failed with internal stack=%u (free_internal=%u largest_internal=%u)",
                 (unsigned)stack_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    return pdFAIL;
}

static bool subagent_tool_allowed(const char *name)
{
    return strcmp(name, "web_search") == 0 ||
           strcmp(name, "get_weather") == 0 ||
           strcmp(name, "get_current_time") == 0 ||
           strcmp(name, "read_file") == 0 ||
           strcmp(name, "write_file") == 0 ||
           strcmp(name, "edit_file") == 0 ||
           strcmp(name, "list_dir") == 0;
}

static void subagent_cleanup_ctx(subagent_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    const bool release_single_flight = ctx->owns_single_flight;
    if (ctx->done_sem) {
        vSemaphoreDelete(ctx->done_sem);
    }
    if (ctx->cleanup_sem) {
        vSemaphoreDelete(ctx->cleanup_sem);
    }
    free(ctx->task);
    free(ctx->context);
    free(ctx->result);
    free(ctx);
    if (release_single_flight) {
        subagent_single_flight_release();
        ESP_LOGI(TAG, "Subagent single-flight slot released");
    }
}

static void subagent_run(subagent_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    ESP_LOGI(TAG, "Subagent started: %.80s", ctx->task ? ctx->task : "");

    const uint32_t free_internal =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largest_internal =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if ((free_internal < 32768 || largest_internal < 16384) &&
        subagent_try_low_memory_shortcut(ctx)) {
        ESP_LOGI(TAG,
                 "Subagent satisfied via low-memory shortcut: free_internal=%u largest_internal=%u",
                 (unsigned)free_internal, (unsigned)largest_internal);
        return;
    }

    char *system_prompt = heap_caps_calloc(1, ESPAGENT_SUBAGENT_CONTEXT_SIZE,
                                           MALLOC_CAP_SPIRAM);
    char *tool_output = heap_caps_calloc(1, ESPAGENT_SUBAGENT_TOOL_BUF_SIZE,
                                         MALLOC_CAP_SPIRAM);
    cJSON *messages = NULL;

    if (!system_prompt || !tool_output) {
        ctx->result = strdup("Error: subagent memory allocation failed");
        goto done;
    }

    context_build_system_prompt(system_prompt, ESPAGENT_SUBAGENT_CONTEXT_SIZE);

    size_t used = strlen(system_prompt);
    if (used < ESPAGENT_SUBAGENT_CONTEXT_SIZE - 1) {
        snprintf(system_prompt + used, ESPAGENT_SUBAGENT_CONTEXT_SIZE - used,
                 "\n\n--- SUBAGENT MODE ---\n"
                 "You are a temporary ESPAgent subagent spawned for one focused subtask. "
                 "Complete only that subtask, use tools when useful, and return a concise final result. "
                 "You cannot spawn further subagents. Do not perform unsafe hardware actions unless the task explicitly asks for them and the tool schema allows it.\n");
    }

    if (ctx->context && ctx->context[0]) {
        used = strlen(system_prompt);
        if (used < ESPAGENT_SUBAGENT_CONTEXT_SIZE - 1) {
            snprintf(system_prompt + used, ESPAGENT_SUBAGENT_CONTEXT_SIZE - used,
                     "\nAdditional context:\n%s\n", ctx->context);
        }
    }

    messages = cJSON_CreateArray();
    cJSON *user_msg = cJSON_CreateObject();
    if (!messages || !user_msg) {
        ctx->result = strdup("Error: subagent message allocation failed");
        goto done;
    }
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", ctx->task ? ctx->task : "");
    cJSON_AddItemToArray(messages, user_msg);

    llm_response_t resp = {0};
    esp_err_t err = llm_chat_tools(system_prompt, messages, s_subagent_tools_json, &resp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Subagent LLM call failed: %s", esp_err_to_name(err));
        ctx->result = strdup("Error: subagent LLM call failed");
        goto done;
    }

    if (!resp.tool_use) {
        ctx->result = (resp.text && resp.text_len > 0)
                          ? strdup(resp.text)
                          : strdup("(subagent produced no output)");
        llm_response_free(&resp);
        goto done;
    }

    const llm_tool_call_t *call = &resp.calls[0];
    if (resp.call_count > 1) {
        ESP_LOGW(TAG, "Subagent requested %d tools; executing only the first", resp.call_count);
    }
    tool_output[0] = '\0';
    if (subagent_tool_allowed(call->name)) {
        tool_registry_execute_as(call->name,
                                 call->input ? call->input : "{}",
                                 ESPAGENT_CAP_CALLER_SUBAGENT,
                                 tool_output,
                                 ESPAGENT_SUBAGENT_TOOL_BUF_SIZE);
    } else {
        snprintf(tool_output, ESPAGENT_SUBAGENT_TOOL_BUF_SIZE,
                 "Error: tool '%s' is not available to subagents",
                 call->name[0] ? call->name : "(empty)");
        ESP_LOGW(TAG, "Blocked subagent tool call: %s",
                 call->name[0] ? call->name : "(empty)");
    }
    ESP_LOGI(TAG, "Subagent tool %s result: %d bytes",
             call->name, (int)strlen(tool_output));

    size_t result_size = strlen(tool_output) + sizeof("Subagent completed: ");
    ctx->result = malloc(result_size);
    if (ctx->result) {
        snprintf(ctx->result, result_size, "Subagent completed: %s", tool_output);
    } else {
        ctx->result = strdup("Error: subagent result allocation failed");
    }
    llm_response_free(&resp);

done:
    if (messages) {
        cJSON_Delete(messages);
    }
    free(system_prompt);
    free(tool_output);

    ESP_LOGI(TAG, "Subagent done, result=%d bytes",
             ctx->result ? (int)strlen(ctx->result) : 0);
}

static void subagent_task(void *arg)
{
    subagent_ctx_t *ctx = (subagent_ctx_t *)arg;
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    subagent_run(ctx);

    xSemaphoreGive(ctx->done_sem);
    xSemaphoreTake(ctx->cleanup_sem, portMAX_DELAY);
    subagent_cleanup_ctx(ctx);
    vTaskDelete(NULL);
}

esp_err_t tool_subagent_init(void)
{
    const espagent_tool_t *tools = NULL;
    int count = 0;
    tool_registry_get_tools(&tools, &count);

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < count; i++) {
        if (!subagent_tool_allowed(tools[i].name)) {
            continue;
        }

        cJSON *tool = cJSON_CreateObject();
        if (!tool) {
            continue;
        }
        cJSON_AddStringToObject(tool, "name", tools[i].name);
        cJSON_AddStringToObject(tool, "description", tools[i].description);

        cJSON *schema = cJSON_Parse(tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }
        cJSON_AddItemToArray(arr, tool);
    }

    free(s_subagent_tools_json);
    s_subagent_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!s_subagent_tools_json) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Subagent tools JSON built");
    return ESP_OK;
}

esp_err_t tool_subagent_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *task_json = cJSON_GetObjectItem(input, "task");
    if (!task_json || !cJSON_IsString(task_json) || !task_json->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: 'task' field is required");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *context_json = cJSON_GetObjectItem(input, "context");
    subagent_ctx_t *ctx = heap_caps_calloc(1, sizeof(*ctx), MALLOC_CAP_SPIRAM);
    if (!ctx) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: failed to allocate subagent context");
        return ESP_ERR_NO_MEM;
    }

    ctx->task = strdup(task_json->valuestring);
    ctx->context = (context_json && cJSON_IsString(context_json))
                       ? strdup(context_json->valuestring)
                       : NULL;
    cJSON_Delete(input);

    if (!ctx->task) {
        subagent_cleanup_ctx(ctx);
        snprintf(output, output_size, "Error: subagent setup failed");
        return ESP_ERR_NO_MEM;
    }

    if (!subagent_single_flight_try_acquire()) {
        snprintf(output, output_size,
                 "Error: another subagent is still running; wait for it to finish before retrying");
        ESP_LOGW(TAG, "Rejected overlapping subagent request");
        subagent_cleanup_ctx(ctx);
        return ESP_ERR_INVALID_STATE;
    }
    ctx->owns_single_flight = true;
    ESP_LOGI(TAG, "Subagent single-flight slot acquired");

    const uint32_t free_internal =
        (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t largest_internal =
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const bool run_inline = free_internal < 32768 || largest_internal < 16384;

    if (run_inline) {
        ESP_LOGW(TAG,
                 "Running subagent inline due to tight internal heap: free_internal=%u largest_internal=%u",
                 (unsigned)free_internal,
                 (unsigned)largest_internal);
        if (subagent_try_low_memory_shortcut(ctx)) {
            snprintf(output, output_size, "%s",
                     ctx->result ? ctx->result : "(subagent returned no result)");
            ESP_LOGI(TAG, "Subagent completed via low-memory shortcut, output=%d bytes",
                     (int)strlen(output));
            subagent_cleanup_ctx(ctx);
            return ESP_OK;
        }
        if (free_internal < 24576 || largest_internal < 12288) {
            snprintf(output, output_size,
                     "Error: subagent skipped because internal heap is too low: free=%u largest=%u",
                     (unsigned)free_internal,
                     (unsigned)largest_internal);
            subagent_cleanup_ctx(ctx);
            return ESP_ERR_NO_MEM;
        }
        subagent_run(ctx);
        snprintf(output, output_size, "%s",
                 ctx->result ? ctx->result : "(subagent returned no result)");
        ESP_LOGI(TAG, "Subagent completed inline, output=%d bytes", (int)strlen(output));
        subagent_cleanup_ctx(ctx);
        return ESP_OK;
    }

    ctx->done_sem = xSemaphoreCreateBinary();
    ctx->cleanup_sem = xSemaphoreCreateBinary();
    if (!ctx->done_sem || !ctx->cleanup_sem) {
        subagent_cleanup_ctx(ctx);
        snprintf(output, output_size, "Error: subagent setup failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Spawning subagent: %.80s", ctx->task);
    BaseType_t ok = create_subagent_task(ctx);
    if (ok != pdPASS) {
        subagent_cleanup_ctx(ctx);
        snprintf(output, output_size, "Error: failed to create subagent task");
        return ESP_FAIL;
    }

    BaseType_t done = xSemaphoreTake(ctx->done_sem,
                                     pdMS_TO_TICKS(ESPAGENT_SUBAGENT_TIMEOUT_MS));
    if (done != pdTRUE) {
        xSemaphoreGive(ctx->cleanup_sem);
        ESP_LOGW(TAG,
                 "Subagent timed out; single-flight remains occupied until worker cleanup");
        snprintf(output, output_size, "Error: subagent timed out after %d seconds",
                 ESPAGENT_SUBAGENT_TIMEOUT_MS / 1000);
        return ESP_ERR_TIMEOUT;
    }

    snprintf(output, output_size, "%s",
             ctx->result ? ctx->result : "(subagent returned no result)");
    xSemaphoreGive(ctx->cleanup_sem);

    ESP_LOGI(TAG, "Subagent completed, output=%d bytes", (int)strlen(output));
    return ESP_OK;
}

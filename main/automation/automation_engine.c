#include "automation/automation_engine.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "espagent_config.h"
#include "mesh/mesh_protocol.h"
#include "sensors/sensor_mqtt.h"
#include "tools/tool_mesh_command.h"

#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "automation";

#define AUTOMATION_MAX_WORKFLOWS ESPAGENT_AUTOMATION_MAX_WORKFLOWS
#define AUTOMATION_MAX_RULES     ESPAGENT_AUTOMATION_MAX_RULES
#define AUTOMATION_MAX_STEPS     ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS
#define AUTOMATION_WORKFLOW_OUTPUT_SIZE 768

typedef struct {
    char action[ESPAGENT_MESH_ACTION_MAX];
    char target_role[ESPAGENT_MESH_ROLE_MAX];
    char target_node[ESPAGENT_MESH_NODE_MAX];
    char args_json[320];
    uint32_t delay_ms;
} automation_step_t;

typedef struct {
    bool used;
    char id[24];
    char name[32];
    bool enabled;
    uint8_t step_count;
    automation_step_t steps[AUTOMATION_MAX_STEPS];
} automation_workflow_t;

typedef enum {
    AUTOMATION_METRIC_TEMPERATURE = 0,
    AUTOMATION_METRIC_HUMIDITY = 1,
    AUTOMATION_METRIC_LIGHT_LUX = 2,
} automation_metric_t;

typedef struct {
    bool used;
    char id[24];
    char name[32];
    char source_skill[49];
    bool enabled;
    automation_metric_t metric;
    float threshold;
    float hysteresis;
    uint32_t interval_s;
    uint32_t cooldown_s;
    int64_t last_check_ms;
    int64_t last_action_ms;
    int last_branch; /* -1 below, 0 unknown, 1 above */
    char sensor_args_json[256];
    automation_step_t above;
    automation_step_t below;
} automation_rule_t;

static automation_workflow_t s_workflows[AUTOMATION_MAX_WORKFLOWS];
static automation_rule_t s_rules[AUTOMATION_MAX_RULES];
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;

static void lock(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static BaseType_t create_runtime_task(TaskFunction_t task_func,
                                      const char *task_name,
                                      uint32_t stack_bytes,
                                      void *task_arg,
                                      TaskHandle_t *task_handle)
{
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(task_func,
                                                    task_name,
                                                    stack_bytes,
                                                    task_arg,
                                                    4,
                                                    task_handle,
                                                    0,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) {
        ESP_LOGI(TAG, "Automation runtime task %s created with PSRAM stack=%u", task_name, (unsigned)stack_bytes);
        return ok;
    }

    ESP_LOGW(TAG,
             "Automation runtime task %s PSRAM stack create failed (stack=%u, free_internal=%u, largest_internal=%u), retrying internal RAM",
             task_name,
             (unsigned)stack_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#endif

    return xTaskCreatePinnedToCore(task_func,
                                   task_name,
                                   stack_bytes,
                                   task_arg,
                                   4,
                                   task_handle,
                                   0);
}

static void gen_id(char *buf, size_t size, const char *prefix)
{
    snprintf(buf, size, "%s-%08llx",
             prefix,
             (unsigned long long)(esp_timer_get_time() & 0xffffffffULL));
}

static cJSON *step_to_json(const automation_step_t *step)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    cJSON_AddStringToObject(obj, "action", step->action);
    cJSON_AddStringToObject(obj, "target_role", step->target_role);
    if (step->target_node[0]) {
        cJSON_AddStringToObject(obj, "target_node", step->target_node);
    }
    if (step->args_json[0]) {
        cJSON *args = cJSON_Parse(step->args_json);
        if (args && cJSON_IsObject(args)) {
            cJSON_AddItemToObject(obj, "args", args);
        } else {
            cJSON_Delete(args);
            cJSON_AddStringToObject(obj, "args_json", step->args_json);
        }
    }
    cJSON_AddNumberToObject(obj, "delay_ms", step->delay_ms);
    return obj;
}

static bool json_get_string(cJSON *root, const char *key, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

static int json_get_int(cJSON *root, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return default_value;
    }
    return item->valueint;
}

static double json_get_double(cJSON *root, const char *key, double default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return default_value;
    }
    return item->valuedouble;
}

static bool json_get_bool(cJSON *root, const char *key, bool default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item) {
        return default_value;
    }
    return cJSON_IsTrue(item);
}

static void json_copy_object_string(cJSON *obj, const char *key, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item) && item->valuestring) {
        snprintf(out, out_size, "%s", item->valuestring);
    }
}

static const char *default_role_for_action(const char *action, const char *fallback)
{
    if (action &&
        (strcmp(action, "read_temperature_humidity") == 0 ||
         strcmp(action, "read_environment") == 0 ||
         strcmp(action, "read_light_level") == 0 ||
         strcmp(action, "virtual_device_read") == 0)) {
        return "sensor_agent";
    }
    if (action &&
        (strcmp(action, "set_status_light") == 0 ||
         strcmp(action, "ws2812_set") == 0 ||
         strcmp(action, "virtual_device_control") == 0 ||
         strcmp(action, "set_curtain") == 0 ||
         strcmp(action, "set_humidifier") == 0 ||
         strcmp(action, "set_fan") == 0 ||
         strcmp(action, "set_device_led") == 0 ||
         strcmp(action, "servo_write") == 0 ||
         strcmp(action, "copper_gpio_write") == 0 ||
         strcmp(action, "gree_ac_control") == 0 ||
         strcmp(action, "gpio_write") == 0)) {
        return "control_agent";
    }
    return fallback;
}

static const char *automation_metric_name(automation_metric_t metric)
{
    switch (metric) {
    case AUTOMATION_METRIC_HUMIDITY:
        return "humidity_percent";
    case AUTOMATION_METRIC_LIGHT_LUX:
        return "light_lux";
    case AUTOMATION_METRIC_TEMPERATURE:
    default:
        return "temperature_c";
    }
}

static automation_metric_t automation_metric_from_name(const char *metric)
{
    if (metric && strcmp(metric, "humidity_percent") == 0) {
        return AUTOMATION_METRIC_HUMIDITY;
    }
    if (metric && strcmp(metric, "light_lux") == 0) {
        return AUTOMATION_METRIC_LIGHT_LUX;
    }
    return AUTOMATION_METRIC_TEMPERATURE;
}

static void fill_step_from_json(automation_step_t *step, cJSON *obj, const char *default_role)
{
    memset(step, 0, sizeof(*step));
    if (!obj || !cJSON_IsObject(obj)) {
        return;
    }

    json_copy_object_string(obj, "action", step->action, sizeof(step->action));
    json_copy_object_string(obj, "target_role", step->target_role, sizeof(step->target_role));
    json_copy_object_string(obj, "target_node", step->target_node, sizeof(step->target_node));
    if (!step->target_role[0] && default_role) {
        snprintf(step->target_role, sizeof(step->target_role), "%s",
                 default_role_for_action(step->action, default_role));
    } else if (!step->target_role[0]) {
        const char *role = default_role_for_action(step->action, NULL);
        if (role) {
            snprintf(step->target_role, sizeof(step->target_role), "%s", role);
        }
    }

    cJSON *args = cJSON_GetObjectItem(obj, "args");
    if (args && cJSON_IsObject(args)) {
        char *json = cJSON_PrintUnformatted(args);
        if (json) {
            snprintf(step->args_json, sizeof(step->args_json), "%s", json);
            cJSON_free(json);
        }
    } else {
        json_copy_object_string(obj, "args_json", step->args_json, sizeof(step->args_json));
    }
    step->delay_ms = (uint32_t)json_get_int(obj, "delay_ms", 0);
}

static esp_err_t step_execute_mesh(const automation_step_t *step, char *output, size_t output_size)
{
    cJSON *payload = cJSON_CreateObject();
    if (!payload) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(payload, "action", step->action);
    if (step->target_role[0]) {
        cJSON_AddStringToObject(payload, "target_role", step->target_role);
    }
    if (step->target_node[0]) {
        cJSON_AddStringToObject(payload, "target_node", step->target_node);
    }
    cJSON_AddBoolToObject(payload, "async", true);
    cJSON_AddNumberToObject(payload, "safety_level", 1);
    cJSON_AddNumberToObject(payload, "ttl_ms", 30000);
    cJSON_AddBoolToObject(payload, "require_ack", true);

    if (step->args_json[0]) {
        cJSON *args = cJSON_Parse(step->args_json);
        if (args && cJSON_IsObject(args)) {
            cJSON_AddItemToObject(payload, "args", args);
        } else {
            cJSON_Delete(args);
            cJSON_AddStringToObject(payload, "args_json", step->args_json);
        }
    } else {
        cJSON_AddItemToObject(payload, "args", cJSON_CreateObject());
    }

    char *json = cJSON_PrintUnformatted(payload);
    cJSON_Delete(payload);
    if (!json) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = tool_mesh_send_command_execute(json, output, output_size);
    cJSON_free(json);
    return err;
}

static bool extract_output_message_json(const char *text, char *json_buf, size_t json_buf_size)
{
    if (!text || !json_buf || json_buf_size == 0) {
        return false;
    }

    const char *marker = strstr(text, "output_message=");
    if (!marker) {
        return false;
    }
    marker += strlen("output_message=");
    while (*marker == ' ') {
        marker++;
    }
    snprintf(json_buf, json_buf_size, "%s", marker);
    return json_buf[0] != '\0';
}

static bool parse_temperature_humidity(const char *text, float *temperature_c, float *humidity_pct)
{
    if (!text) {
        return false;
    }
    const char *temp = strstr(text, "temperature=");
    const char *hum = strstr(text, "humidity=");
    if (!temp || !hum) {
        return false;
    }
    float t = 0.0f;
    float h = 0.0f;
    if (sscanf(temp, "temperature=%f", &t) != 1) {
        return false;
    }
    if (sscanf(hum, "humidity=%f", &h) != 1) {
        return false;
    }
    if (temperature_c) *temperature_c = t;
    if (humidity_pct) *humidity_pct = h;
    return true;
}

static bool parse_light_lux(const char *text, float *light_lux)
{
    if (!text || !light_lux) {
        return false;
    }
    const char *light = strstr(text, "light_lux=");
    if (!light) {
        light = strstr(text, "light=");
    }
    if (!light) {
        return false;
    }
    const char *eq = strchr(light, '=');
    if (!eq) {
        return false;
    }
    float lux = 0.0f;
    if (sscanf(eq + 1, "%f", &lux) != 1) {
        return false;
    }
    *light_lux = lux;
    return true;
}

static bool parse_sensor_output_metric(const char *output_text, automation_metric_t metric, float *value)
{
    float t = 0.0f;
    float h = 0.0f;
    float lux = 0.0f;
    if (metric == AUTOMATION_METRIC_LIGHT_LUX &&
        parse_light_lux(output_text, &lux)) {
        *value = lux;
        return true;
    }
    if (parse_temperature_humidity(output_text, &t, &h)) {
        *value = (metric == AUTOMATION_METRIC_HUMIDITY) ? h : t;
        return true;
    }

    char output_json[1024] = {0};
    if (!extract_output_message_json(output_text, output_json, sizeof(output_json))) {
        return false;
    }

    cJSON *root = cJSON_Parse(output_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (!result || !cJSON_IsObject(result) || (error && !cJSON_IsNull(error))) {
        cJSON_Delete(root);
        return false;
    }

    cJSON *text = cJSON_GetObjectItem(result, "text");
    const char *result_text = cJSON_IsString(text) ? text->valuestring : NULL;
    if (metric == AUTOMATION_METRIC_LIGHT_LUX &&
        parse_light_lux(result_text, &lux)) {
        *value = lux;
        cJSON_Delete(root);
        return true;
    }
    if (!parse_temperature_humidity(result_text, &t, &h)) {
        cJSON_Delete(root);
        return false;
    }
    *value = (metric == AUTOMATION_METRIC_HUMIDITY) ? h : t;
    cJSON_Delete(root);
    return true;
}

static esp_err_t persist_rules_locked(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *rules = cJSON_CreateArray();
    if (!root || !rules) {
        cJSON_Delete(root);
        cJSON_Delete(rules);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < AUTOMATION_MAX_RULES; i++) {
        automation_rule_t *rule = &s_rules[i];
        if (!rule->used) continue;
        cJSON *item = cJSON_CreateObject();
        if (!item) continue;

        cJSON_AddStringToObject(item, "id", rule->id);
        cJSON_AddStringToObject(item, "name", rule->name);
        if (rule->source_skill[0]) {
            cJSON_AddStringToObject(item, "source_skill", rule->source_skill);
        }
        cJSON_AddBoolToObject(item, "enabled", rule->enabled);
        cJSON_AddStringToObject(item, "metric", automation_metric_name(rule->metric));
        cJSON_AddNumberToObject(item, "threshold", rule->threshold);
        cJSON_AddNumberToObject(item, "hysteresis", rule->hysteresis);
        cJSON_AddNumberToObject(item, "interval_s", (double)rule->interval_s);
        cJSON_AddNumberToObject(item, "cooldown_s", (double)rule->cooldown_s);
        if (rule->sensor_args_json[0]) {
            cJSON *sensor_args = cJSON_Parse(rule->sensor_args_json);
            if (sensor_args && cJSON_IsObject(sensor_args)) {
                cJSON_AddItemToObject(item, "sensor_args", sensor_args);
            } else {
                cJSON_Delete(sensor_args);
            }
        }
        cJSON *above = step_to_json(&rule->above);
        cJSON *below = step_to_json(&rule->below);
        if (above) cJSON_AddItemToObject(item, "above", above);
        if (below) cJSON_AddItemToObject(item, "below", below);
        cJSON_AddItemToArray(rules, item);
    }

    cJSON_AddItemToObject(root, "rules", rules);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(ESPAGENT_AUTOMATION_FILE, "w");
    if (!f) {
        cJSON_free(json);
        return ESP_FAIL;
    }
    fputs(json, f);
    fclose(f);
    cJSON_free(json);
    ESP_LOGI(TAG, "Saved automation rules to %s", ESPAGENT_AUTOMATION_FILE);
    return ESP_OK;
}

static esp_err_t load_rules_locked(void)
{
    FILE *f = fopen(ESPAGENT_AUTOMATION_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "No automation file found, starting fresh");
        return ESP_OK;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return ESP_OK;
    }

    char *buf = calloc(1, (size_t)len + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    fread(buf, 1, (size_t)len, f);
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "Failed to parse automation file");
        return ESP_OK;
    }

    cJSON *rules = cJSON_GetObjectItem(root, "rules");
    if (rules && cJSON_IsArray(rules)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, rules) {
            if (!cJSON_IsObject(item)) continue;
            int slot = -1;
            for (int i = 0; i < AUTOMATION_MAX_RULES; i++) {
                if (!s_rules[i].used) {
                    slot = i;
                    break;
                }
            }
            if (slot < 0) break;

            automation_rule_t *rule = &s_rules[slot];
            memset(rule, 0, sizeof(*rule));
            rule->used = true;
            json_get_string(item, "id", rule->id, sizeof(rule->id));
            json_get_string(item, "name", rule->name, sizeof(rule->name));
            json_get_string(item, "source_skill", rule->source_skill,
                            sizeof(rule->source_skill));
            rule->enabled = json_get_bool(item, "enabled", true);
            char metric[32] = {0};
            json_get_string(item, "metric", metric, sizeof(metric));
            rule->metric = automation_metric_from_name(metric);
            rule->threshold = (float)json_get_double(item, "threshold", 0.0);
            rule->hysteresis = (float)json_get_double(item, "hysteresis", 1.0);
            rule->interval_s = (uint32_t)json_get_int(item, "interval_s", 10);
            rule->cooldown_s = (uint32_t)json_get_int(item, "cooldown_s", 30);
            cJSON *sensor_args = cJSON_GetObjectItem(item, "sensor_args");
            if (sensor_args && cJSON_IsObject(sensor_args)) {
                char *json = cJSON_PrintUnformatted(sensor_args);
                if (json) {
                    snprintf(rule->sensor_args_json, sizeof(rule->sensor_args_json), "%s", json);
                    cJSON_free(json);
                }
            }

            cJSON *above = cJSON_GetObjectItem(item, "above");
            cJSON *below = cJSON_GetObjectItem(item, "below");
            fill_step_from_json(&rule->above, above, "control_agent");
            fill_step_from_json(&rule->below, below, "control_agent");
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded automation rules");
    return ESP_OK;
}

static void publish_rule_event(const automation_rule_t *rule,
                              const char *phase,
                              const char *status,
                              const char *summary)
{
    if (!rule) return;
    (void)sensor_mqtt_publish_timeline_event(phase,
                                             "automation_rule",
                                             status,
                                             summary,
                                             rule->id,
                                             "control_agent",
                                             "",
                                             rule->name);
}

static bool workflow_still_active(const char *id)
{
    bool active = false;
    lock();
    for (int i = 0; i < AUTOMATION_MAX_WORKFLOWS; i++) {
        if (s_workflows[i].used && strcmp(s_workflows[i].id, id) == 0) {
            active = true;
            break;
        }
    }
    unlock();
    return active;
}

static void workflow_task(void *arg)
{
    automation_workflow_t *workflow = (automation_workflow_t *)arg;
    if (!workflow) {
        vTaskDelete(NULL);
        return;
    }

    char summary[192];
    char *output = calloc(1, AUTOMATION_WORKFLOW_OUTPUT_SIZE);
    if (!output) {
        (void)sensor_mqtt_publish_timeline_event("workflow",
                                                 "automation_workflow_error",
                                                 "error",
                                                 "workflow runtime out of memory",
                                                 workflow->id,
                                                 "control_agent",
                                                 "",
                                                 workflow->name);
        lock();
        for (int i = 0; i < AUTOMATION_MAX_WORKFLOWS; i++) {
            if (s_workflows[i].used && strcmp(s_workflows[i].id, workflow->id) == 0) {
                s_workflows[i].used = false;
                break;
            }
        }
        unlock();
        free(workflow);
        vTaskDelete(NULL);
        return;
    }

    for (int i = 0; i < workflow->step_count; i++) {
        if (!workflow_still_active(workflow->id)) {
            snprintf(summary, sizeof(summary), "workflow=%s canceled before step=%d", workflow->id, i + 1);
            (void)sensor_mqtt_publish_timeline_event("workflow",
                                                     "automation_workflow_canceled",
                                                     "canceled",
                                                     summary,
                                                     workflow->id,
                                                     "control_agent",
                                                     "",
                                                     workflow->name);
            break;
        }
        automation_step_t *step = &workflow->steps[i];
        if (step->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(step->delay_ms));
        }
        if (!workflow_still_active(workflow->id)) {
            snprintf(summary, sizeof(summary), "workflow=%s canceled after delay before step=%d", workflow->id, i + 1);
            (void)sensor_mqtt_publish_timeline_event("workflow",
                                                     "automation_workflow_canceled",
                                                     "canceled",
                                                     summary,
                                                     workflow->id,
                                                     "control_agent",
                                                     "",
                                                     workflow->name);
            break;
        }

        output[0] = '\0';
        esp_err_t err = step_execute_mesh(step, output, AUTOMATION_WORKFLOW_OUTPUT_SIZE);
        snprintf(summary, sizeof(summary),
                 "workflow=%s step=%d action=%s status=%s",
                 workflow->id, i + 1, step->action, esp_err_to_name(err));
        (void)sensor_mqtt_publish_timeline_event("workflow",
                                                 "automation_workflow_step",
                                                 err == ESP_OK ? "ok" : "error",
                                                 output[0] ? output : summary,
                                                 workflow->id,
                                                 step->target_role,
                                                 step->target_node,
                                                 step->action);
        if (err != ESP_OK) {
            break;
        }
    }

    lock();
    for (int i = 0; i < AUTOMATION_MAX_WORKFLOWS; i++) {
        if (s_workflows[i].used && strcmp(s_workflows[i].id, workflow->id) == 0) {
            s_workflows[i].used = false;
            break;
        }
    }
    unlock();
    free(output);
    free(workflow);
    vTaskDelete(NULL);
}

static void rule_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ESPAGENT_AUTOMATION_CHECK_INTERVAL_MS));

        lock();
        for (int i = 0; i < AUTOMATION_MAX_RULES; i++) {
            automation_rule_t *rule = &s_rules[i];
            if (!rule->used || !rule->enabled) {
                continue;
            }

            int64_t now_ms = esp_timer_get_time() / 1000;
            if (rule->last_check_ms > 0 && (now_ms - rule->last_check_ms) < (int64_t)rule->interval_s * 1000LL) {
                continue;
            }
            rule->last_check_ms = now_ms;
            automation_rule_t snapshot = *rule;
            unlock();

            char sensor_payload[512] = {0};
            snprintf(sensor_payload, sizeof(sensor_payload),
                     "{\"target_role\":\"sensor_agent\",\"action\":\"read_environment\",\"async\":false,\"require_ack\":true,\"ttl_ms\":30000,\"safety_level\":1%s%s}",
                     snapshot.sensor_args_json[0] ? ",\"args\":" : "",
                     snapshot.sensor_args_json[0] ? snapshot.sensor_args_json : "");
            char sensor_output[1024] = {0};
            esp_err_t read_err = tool_mesh_send_command_execute(sensor_payload, sensor_output, sizeof(sensor_output));

            float metric_value = 0.0f;
            bool metric_ok = (read_err == ESP_OK) && parse_sensor_output_metric(sensor_output, snapshot.metric, &metric_value);
            if (!metric_ok) {
                publish_rule_event(&snapshot, "observe", "error", "automation rule sensor read failed");
                lock();
                continue;
            }

            bool above = metric_value > snapshot.threshold;
            int current_branch = above ? 1 : -1;
            if (snapshot.last_branch > 0) {
                if (!above && metric_value > (snapshot.threshold - snapshot.hysteresis)) {
                    lock();
                    continue;
                }
            } else if (snapshot.last_branch < 0) {
                if (above && metric_value < (snapshot.threshold + snapshot.hysteresis)) {
                    lock();
                    continue;
                }
            }
            if (snapshot.last_branch == current_branch) {
                lock();
                continue;
            }

            if (snapshot.cooldown_s > 0 && snapshot.last_action_ms > 0 &&
                (now_ms - snapshot.last_action_ms) < (int64_t)snapshot.cooldown_s * 1000LL) {
                lock();
                continue;
            }

            automation_step_t *step = above ? &snapshot.above : &snapshot.below;
            if (!step->action[0]) {
                lock();
                continue;
            }

            char action_output[768] = {0};
            esp_err_t action_err = step_execute_mesh(step, action_output, sizeof(action_output));
            publish_rule_event(&snapshot,
                               "act",
                               action_err == ESP_OK ? "ok" : "error",
                               action_output[0] ? action_output : "automation rule action executed");
            lock();
            if (s_rules[i].used && strcmp(s_rules[i].id, snapshot.id) == 0) {
                s_rules[i].last_branch = current_branch;
                if (action_err == ESP_OK) {
                    s_rules[i].last_action_ms = now_ms;
                }
            }
            unlock();
            lock();
        }
        unlock();
    }
}

static esp_err_t ensure_lock(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t automation_engine_init(void)
{
    esp_err_t lock_err = ensure_lock();
    if (lock_err != ESP_OK) {
        return lock_err;
    }
    lock();
    memset(s_workflows, 0, sizeof(s_workflows));
    memset(s_rules, 0, sizeof(s_rules));
    esp_err_t err = load_rules_locked();
    unlock();
    return err;
}

esp_err_t automation_engine_start(void)
{
    if (s_task) {
        return ESP_OK;
    }
    BaseType_t ok = create_runtime_task(rule_task, "automation",
                                        ESPAGENT_AUTOMATION_STACK,
                                        NULL, &s_task);
    if (ok != pdPASS || !s_task) {
        s_task = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Automation engine started");
    return ESP_OK;
}

void automation_engine_stop(void)
{
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
}

static esp_err_t add_workflow_locked(cJSON *root, char *output, size_t output_size)
{
    char name[32] = {0};
    if (!json_get_string(root, "name", name, sizeof(name))) {
        snprintf(output, output_size, "Error: missing required field name");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *steps = cJSON_GetObjectItem(root, "steps");
    if (!steps || !cJSON_IsArray(steps)) {
        snprintf(output, output_size, "Error: missing required field steps");
        return ESP_ERR_INVALID_ARG;
    }

    int count = cJSON_GetArraySize(steps);
    if (count <= 0 || count > AUTOMATION_MAX_STEPS) {
        snprintf(output, output_size, "Error: steps must be 1-%d", AUTOMATION_MAX_STEPS);
        return ESP_ERR_INVALID_ARG;
    }

    int slot = -1;
    for (int i = 0; i < AUTOMATION_MAX_WORKFLOWS; i++) {
        if (!s_workflows[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        snprintf(output, output_size, "Error: workflow limit reached");
        return ESP_ERR_NO_MEM;
    }

    automation_workflow_t *wf = &s_workflows[slot];
    memset(wf, 0, sizeof(*wf));
    wf->used = true;
    wf->enabled = true;
    gen_id(wf->id, sizeof(wf->id), "wf");
    snprintf(wf->name, sizeof(wf->name), "%s", name);
    wf->step_count = (uint8_t)count;

    for (int i = 0; i < count; i++) {
        cJSON *step_json = cJSON_GetArrayItem(steps, i);
        fill_step_from_json(&wf->steps[i], step_json, "control_agent");
        if (!wf->steps[i].action[0]) {
            snprintf(output, output_size, "Error: step %d missing action", i + 1);
            memset(wf, 0, sizeof(*wf));
            return ESP_ERR_INVALID_ARG;
        }
    }

    char *snapshot = cJSON_PrintUnformatted(root);
    if (snapshot) {
        ESP_LOGI(TAG, "Created workflow: %s", snapshot);
        cJSON_free(snapshot);
    }

    automation_workflow_t *runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        snprintf(output, output_size, "Error: out of memory");
        wf->used = false;
        return ESP_ERR_NO_MEM;
    }
    *runtime = *wf;
    if (create_runtime_task(workflow_task,
                            "workflow",
                            ESPAGENT_AUTOMATION_WORKFLOW_STACK,
                            runtime,
                            NULL) != pdPASS) {
        free(runtime);
        wf->used = false;
        snprintf(output, output_size, "Error: failed to start workflow task");
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "OK: workflow %s created with %u steps", wf->id, (unsigned)wf->step_count);
    (void)sensor_mqtt_publish_timeline_event("workflow",
                                             "automation_workflow_created",
                                             "ok",
                                             output,
                                             wf->id,
                                             "control_agent",
                                             "",
                                             wf->name);
    return ESP_OK;
}

static esp_err_t add_rule_locked(cJSON *root, char *output, size_t output_size)
{
    char name[32] = {0};
    if (!json_get_string(root, "name", name, sizeof(name))) {
        snprintf(output, output_size, "Error: missing required field name");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *above = cJSON_GetObjectItem(root, "above");
    cJSON *below = cJSON_GetObjectItem(root, "below");
    if (!above || !cJSON_IsObject(above) || !below || !cJSON_IsObject(below)) {
        snprintf(output, output_size, "Error: above and below branches are required");
        return ESP_ERR_INVALID_ARG;
    }

    int slot = -1;
    for (int i = 0; i < AUTOMATION_MAX_RULES; i++) {
        if (!s_rules[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        snprintf(output, output_size, "Error: rule limit reached");
        return ESP_ERR_NO_MEM;
    }

    automation_rule_t *rule = &s_rules[slot];
    memset(rule, 0, sizeof(*rule));
    rule->used = true;
    rule->enabled = json_get_bool(root, "enabled", true);
    gen_id(rule->id, sizeof(rule->id), "rule");
    snprintf(rule->name, sizeof(rule->name), "%s", name);
    json_get_string(root, "source_skill", rule->source_skill,
                    sizeof(rule->source_skill));
    rule->interval_s = (uint32_t)json_get_int(root, "interval_s", 10);
    if (rule->interval_s < 1) rule->interval_s = 1;
    if (rule->interval_s > 86400) rule->interval_s = 86400;
    rule->cooldown_s = (uint32_t)json_get_int(root, "cooldown_s", 30);
    if (rule->cooldown_s > 300) rule->cooldown_s = 300;
    rule->hysteresis = (float)json_get_double(root, "hysteresis_c", 1.0);
    if (rule->hysteresis < 0.0f) rule->hysteresis = 0.0f;

    char metric[32] = {0};
    json_get_string(root, "metric", metric, sizeof(metric));
    rule->metric = automation_metric_from_name(metric);
    rule->threshold = (float)json_get_double(root, "threshold", 35.0);
    cJSON *sensor_args = cJSON_GetObjectItem(root, "sensor_args");
    if (sensor_args && cJSON_IsObject(sensor_args)) {
        char *json = cJSON_PrintUnformatted(sensor_args);
        if (json) {
            snprintf(rule->sensor_args_json, sizeof(rule->sensor_args_json), "%s", json);
            cJSON_free(json);
        }
    }
    fill_step_from_json(&rule->above, above, "control_agent");
    fill_step_from_json(&rule->below, below, "control_agent");
    if (!rule->above.action[0] || !rule->below.action[0]) {
        snprintf(output, output_size, "Error: above/below actions are required");
        rule->used = false;
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t save_err = persist_rules_locked();
    if (save_err != ESP_OK) {
        memset(rule, 0, sizeof(*rule));
        snprintf(output, output_size, "Error: failed to persist rule (%s)", esp_err_to_name(save_err));
        return save_err;
    } else {
        snprintf(output, output_size,
                 "OK: rule %s created metric=%s threshold=%.2f interval=%us cooldown=%us",
                 rule->id,
                 automation_metric_name(rule->metric),
                 rule->threshold,
                 (unsigned)rule->interval_s,
                 (unsigned)rule->cooldown_s);
    }
    publish_rule_event(rule, "policy", "ok", output);
    return ESP_OK;
}

esp_err_t automation_engine_create_workflow(const char *input_json,
                                           char *output,
                                           size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = add_workflow_locked(root, output, output_size);
    unlock();
    cJSON_Delete(root);
    return err;
}

esp_err_t automation_engine_create_rule(const char *input_json,
                                        char *output,
                                        size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = add_rule_locked(root, output, output_size);
    unlock();
    cJSON_Delete(root);
    return err;
}

esp_err_t automation_engine_list(char *output, size_t output_size)
{
    lock();
    size_t off = 0;
    off += snprintf(output + off, output_size - off, "Workflows:\n");
    for (int i = 0; i < AUTOMATION_MAX_WORKFLOWS && off < output_size; i++) {
        automation_workflow_t *wf = &s_workflows[i];
        if (!wf->used) continue;
        off += snprintf(output + off, output_size - off,
                        "- %s [%s] steps=%u enabled=%s\n",
                        wf->id, wf->name, (unsigned)wf->step_count, wf->enabled ? "yes" : "no");
    }
    off += snprintf(output + off, output_size - off, "Rules:\n");
    for (int i = 0; i < AUTOMATION_MAX_RULES && off < output_size; i++) {
        automation_rule_t *rule = &s_rules[i];
        if (!rule->used) continue;
        off += snprintf(output + off, output_size - off,
                        "- %s [%s] metric=%s threshold=%.2f interval=%us cooldown=%us enabled=%s\n",
                        rule->id,
                        rule->name,
                        automation_metric_name(rule->metric),
                        rule->threshold,
                        (unsigned)rule->interval_s,
                        (unsigned)rule->cooldown_s,
                        rule->enabled ? "yes" : "no");
    }
    unlock();
    return ESP_OK;
}

esp_err_t automation_engine_remove(const char *id,
                                   char *output,
                                   size_t output_size)
{
    if (!id || !id[0]) {
        snprintf(output, output_size, "Error: id is required");
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    for (int i = 0; i < AUTOMATION_MAX_WORKFLOWS; i++) {
        if (s_workflows[i].used && strcmp(s_workflows[i].id, id) == 0) {
            s_workflows[i].used = false;
            unlock();
            snprintf(output, output_size, "OK: removed workflow %s", id);
            return ESP_OK;
        }
    }
    for (int i = 0; i < AUTOMATION_MAX_RULES; i++) {
        if (s_rules[i].used && strcmp(s_rules[i].id, id) == 0) {
            s_rules[i].used = false;
            esp_err_t save_err = persist_rules_locked();
            unlock();
            if (save_err == ESP_OK) {
                snprintf(output, output_size, "OK: removed rule %s", id);
            } else {
                snprintf(output, output_size, "OK: removed rule %s (persist failed: %s)", id, esp_err_to_name(save_err));
            }
            return ESP_OK;
        }
    }
    unlock();
    snprintf(output, output_size, "Error: automation item %s not found", id);
    return ESP_ERR_NOT_FOUND;
}

static bool condition_value(const char *line, const char *key,
                            char *out, size_t out_size)
{
    char pattern[48] = {0};
    if (!line || !key || !out || out_size == 0) {
        return false;
    }
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *p = strstr(line, pattern);
    if (!p) {
        return false;
    }
    p += strlen(pattern);
    size_t len = 0;
    while (p[len] && !isspace((unsigned char)p[len])) {
        len++;
    }
    if (len == 0 || len >= out_size) {
        return false;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool automation_condition_action_allowed(const char *action)
{
    return action &&
           (strcmp(action, "set_status_light") == 0 ||
            strcmp(action, "ws2812_set") == 0 ||
            strcmp(action, "set_curtain") == 0 ||
            strcmp(action, "servo_write") == 0 ||
            strcmp(action, "set_humidifier") == 0 ||
            strcmp(action, "set_fan") == 0 ||
            strcmp(action, "set_device_led") == 0 ||
            strcmp(action, "gpio_write") == 0 ||
            strcmp(action, "virtual_device_control") == 0);
}

static esp_err_t remove_skill_condition_locked(const char *skill_name,
                                               char *output,
                                               size_t output_size)
{
    bool removed = false;
    for (int i = 0; i < AUTOMATION_MAX_RULES; i++) {
        if (!s_rules[i].used ||
            strcmp(s_rules[i].source_skill, skill_name) != 0) {
            continue;
        }
        memset(&s_rules[i], 0, sizeof(s_rules[i]));
        removed = true;
    }
    if (removed) {
        esp_err_t err = persist_rules_locked();
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: skill condition removed but persist failed: %s",
                     esp_err_to_name(err));
            return err;
        }
    }
    if (output && output_size > 0) {
        snprintf(output, output_size, "%s",
                 removed ? "OK: previous skill condition removed"
                         : "OK: no previous skill condition");
    }
    return ESP_OK;
}

esp_err_t automation_engine_remove_skill_condition(const char *skill_name,
                                                   char *output,
                                                   size_t output_size)
{
    if (!skill_name || !skill_name[0]) {
        snprintf(output, output_size, "Error: skill name is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (ensure_lock() != ESP_OK) {
        snprintf(output, output_size, "Error: automation lock unavailable");
        return ESP_ERR_NO_MEM;
    }
    lock();
    esp_err_t err = remove_skill_condition_locked(skill_name, output, output_size);
    unlock();
    return err;
}

esp_err_t automation_engine_sync_skill_condition(const char *skill_name,
                                                 const char *content,
                                                 char *output,
                                                 size_t output_size)
{
    if (!skill_name || !skill_name[0] || strlen(skill_name) >= 32 || !content) {
        snprintf(output, output_size, "Error: invalid skill condition identity");
        return ESP_ERR_INVALID_ARG;
    }

    const char *directive = strstr(content, "@condition");
    if (!directive) {
        return automation_engine_remove_skill_condition(skill_name, output, output_size);
    }
    const char *line_end = strpbrk(directive, "\r\n");
    size_t line_len = line_end ? (size_t)(line_end - directive) : strlen(directive);
    if (line_len == 0 || line_len >= 1024) {
        snprintf(output, output_size, "Error: @condition line is too long");
        return ESP_ERR_INVALID_SIZE;
    }

    char line[1024] = {0};
    memcpy(line, directive, line_len);
    line[line_len] = '\0';
    char metric[32] = {0};
    char threshold[24] = {0};
    char hysteresis[24] = {0};
    char interval[24] = {0};
    char cooldown[24] = {0};
    char above_action[48] = {0};
    char below_action[48] = {0};
    char above_args[320] = {0};
    char below_args[320] = {0};
    if (!condition_value(line, "metric", metric, sizeof(metric)) ||
        !condition_value(line, "threshold", threshold, sizeof(threshold)) ||
        !condition_value(line, "above_action", above_action, sizeof(above_action)) ||
        !condition_value(line, "above_args", above_args, sizeof(above_args)) ||
        !condition_value(line, "below_action", below_action, sizeof(below_action)) ||
        !condition_value(line, "below_args", below_args, sizeof(below_args))) {
        snprintf(output, output_size,
                 "Error: @condition requires metric, threshold, above_action, above_args, below_action, below_args");
        return ESP_ERR_INVALID_ARG;
    }
    (void)condition_value(line, "hysteresis", hysteresis, sizeof(hysteresis));
    (void)condition_value(line, "interval_s", interval, sizeof(interval));
    (void)condition_value(line, "cooldown_s", cooldown, sizeof(cooldown));

    char *end = NULL;
    double threshold_value = strtod(threshold, &end);
    if (!end || *end || !isfinite(threshold_value) ||
        (strcmp(metric, "humidity_percent") == 0 &&
         (threshold_value < 0.0 || threshold_value > 100.0)) ||
        (strcmp(metric, "humidity_percent") != 0 &&
         strcmp(metric, "temperature_c") != 0 &&
         strcmp(metric, "light_lux") != 0)) {
        snprintf(output, output_size, "Error: unsupported or invalid condition metric/threshold");
        return ESP_ERR_INVALID_ARG;
    }
    char *interval_end = NULL;
    char *cooldown_end = NULL;
    long interval_value = interval[0] ? strtol(interval, &interval_end, 10) : 30;
    long cooldown_value = cooldown[0] ? strtol(cooldown, &cooldown_end, 10) : 300;
    char *hysteresis_end = NULL;
    double hysteresis_value = hysteresis[0] ? strtod(hysteresis, &hysteresis_end) : 1.0;
    if ((interval[0] && (!interval_end || *interval_end)) ||
        (cooldown[0] && (!cooldown_end || *cooldown_end)) ||
        (hysteresis[0] && (!hysteresis_end || *hysteresis_end)) ||
        interval_value < 5 || interval_value > 86400 ||
        cooldown_value < 5 || cooldown_value > 300 ||
        !isfinite(hysteresis_value) || hysteresis_value < 0.0 || hysteresis_value > 100.0) {
        snprintf(output, output_size, "Error: interval/cooldown/hysteresis is outside safe bounds");
        return ESP_ERR_INVALID_ARG;
    }
    if (!automation_condition_action_allowed(above_action) ||
        !automation_condition_action_allowed(below_action)) {
        snprintf(output, output_size, "Error: condition action is not allowed");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *above = cJSON_Parse(above_args);
    cJSON *below = cJSON_Parse(below_args);
    if (!above || !below || !cJSON_IsObject(above) || !cJSON_IsObject(below)) {
        cJSON_Delete(above);
        cJSON_Delete(below);
        snprintf(output, output_size, "Error: condition args must be JSON objects");
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_Delete(above);
    cJSON_Delete(below);

    if (ensure_lock() != ESP_OK) {
        snprintf(output, output_size, "Error: automation lock unavailable");
        return ESP_ERR_NO_MEM;
    }
    lock();
    esp_err_t remove_err = remove_skill_condition_locked(skill_name, output, output_size);
    unlock();
    if (remove_err != ESP_OK) {
        return remove_err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *above_obj = cJSON_CreateObject();
    cJSON *below_obj = cJSON_CreateObject();
    if (!root || !above_obj || !below_obj) {
        cJSON_Delete(root);
        cJSON_Delete(above_obj);
        cJSON_Delete(below_obj);
        snprintf(output, output_size, "Error: condition rule out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "name", skill_name);
    cJSON_AddStringToObject(root, "source_skill", skill_name);
    cJSON_AddStringToObject(root, "metric", metric);
    cJSON_AddNumberToObject(root, "threshold", threshold_value);
    cJSON_AddNumberToObject(root, "hysteresis_c", hysteresis_value);
    cJSON_AddNumberToObject(root, "interval_s", interval_value);
    cJSON_AddNumberToObject(root, "cooldown_s", cooldown_value);
    cJSON_AddStringToObject(above_obj, "action", above_action);
    cJSON_AddStringToObject(above_obj, "target_role", "control_agent");
    cJSON_AddItemToObject(above_obj, "args", cJSON_Parse(above_args));
    cJSON_AddStringToObject(below_obj, "action", below_action);
    cJSON_AddStringToObject(below_obj, "target_role", "control_agent");
    cJSON_AddItemToObject(below_obj, "args", cJSON_Parse(below_args));
    cJSON_AddItemToObject(root, "above", above_obj);
    cJSON_AddItemToObject(root, "below", below_obj);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        snprintf(output, output_size, "Error: condition rule serialization failed");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = automation_engine_create_rule(json, output, output_size);
    cJSON_free(json);
    return err;
}

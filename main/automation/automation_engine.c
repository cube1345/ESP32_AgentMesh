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
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "automation";

#define AUTOMATION_MAX_WORKFLOWS ESPAGENT_AUTOMATION_MAX_WORKFLOWS
#define AUTOMATION_MAX_RULES     ESPAGENT_AUTOMATION_MAX_RULES
#define AUTOMATION_MAX_STEPS     ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS

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
} automation_metric_t;

typedef struct {
    bool used;
    char id[24];
    char name[32];
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
    if (action && strcmp(action, "read_temperature_humidity") == 0) {
        return "sensor_agent";
    }
    if (action &&
        (strcmp(action, "set_status_light") == 0 ||
         strcmp(action, "ws2812_set") == 0 ||
         strcmp(action, "servo_write") == 0 ||
         strcmp(action, "gpio_write") == 0)) {
        return "control_agent";
    }
    return fallback;
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
    cJSON_AddBoolToObject(payload, "async", false);
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

static bool parse_sensor_output_metric(const char *output_text, automation_metric_t metric, float *value)
{
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
    float t = 0.0f;
    float h = 0.0f;
    if (!parse_temperature_humidity(result_text, &t, &h)) {
        cJSON_Delete(root);
        return false;
    }
    if (metric == AUTOMATION_METRIC_TEMPERATURE) {
        *value = t;
    } else {
        *value = h;
    }
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
        cJSON_AddBoolToObject(item, "enabled", rule->enabled);
        cJSON_AddStringToObject(item, "metric", rule->metric == AUTOMATION_METRIC_HUMIDITY ? "humidity_percent" : "temperature_c");
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
            rule->enabled = json_get_bool(item, "enabled", true);
            char metric[32] = {0};
            json_get_string(item, "metric", metric, sizeof(metric));
            rule->metric = (strcmp(metric, "humidity_percent") == 0) ? AUTOMATION_METRIC_HUMIDITY : AUTOMATION_METRIC_TEMPERATURE;
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

static void workflow_task(void *arg)
{
    automation_workflow_t *workflow = (automation_workflow_t *)arg;
    if (!workflow) {
        vTaskDelete(NULL);
        return;
    }

    char summary[192];
    for (int i = 0; i < workflow->step_count; i++) {
        automation_step_t *step = &workflow->steps[i];
        if (step->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(step->delay_ms));
        }

        char output[768] = {0};
        esp_err_t err = step_execute_mesh(step, output, sizeof(output));
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
                     "{\"target_role\":\"sensor_agent\",\"action\":\"read_temperature_humidity\",\"async\":false,\"require_ack\":true,\"ttl_ms\":30000,\"safety_level\":1%s%s}",
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
                s_rules[i].last_action_ms = now_ms;
                s_rules[i].last_branch = above ? 1 : -1;
            }
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
    BaseType_t ok = xTaskCreatePinnedToCore(rule_task, "automation",
                                           ESPAGENT_AUTOMATION_STACK,
                                           NULL, 4, &s_task, 0);
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
    snprintf(output, output_size, "OK: workflow %s created with %u steps", wf->id, (unsigned)wf->step_count);
    (void)sensor_mqtt_publish_timeline_event("workflow",
                                             "automation_workflow_created",
                                             "ok",
                                             output,
                                             wf->id,
                                             "control_agent",
                                             "",
                                             wf->name);
    if (xTaskCreate(workflow_task, "workflow", 6144, runtime, 4, NULL) != pdPASS) {
        free(runtime);
        wf->used = false;
        snprintf(output, output_size, "Error: failed to start workflow task");
        return ESP_ERR_NO_MEM;
    }
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
    rule->interval_s = (uint32_t)json_get_int(root, "interval_s", 10);
    if (rule->interval_s < 1) rule->interval_s = 1;
    rule->cooldown_s = (uint32_t)json_get_int(root, "cooldown_s", 30);
    if (rule->cooldown_s > 300) rule->cooldown_s = 300;
    rule->hysteresis = (float)json_get_double(root, "hysteresis_c", 1.0);
    if (rule->hysteresis < 0.0f) rule->hysteresis = 0.0f;

    char metric[32] = {0};
    json_get_string(root, "metric", metric, sizeof(metric));
    if (strcmp(metric, "humidity_percent") == 0) {
        rule->metric = AUTOMATION_METRIC_HUMIDITY;
    } else {
        rule->metric = AUTOMATION_METRIC_TEMPERATURE;
    }
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
        snprintf(output, output_size, "Error: created rule but failed to persist (%s)", esp_err_to_name(save_err));
    } else {
        snprintf(output, output_size,
                 "OK: rule %s created metric=%s threshold=%.2f interval=%us cooldown=%us",
                 rule->id,
                 rule->metric == AUTOMATION_METRIC_HUMIDITY ? "humidity_percent" : "temperature_c",
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
                        rule->metric == AUTOMATION_METRIC_HUMIDITY ? "humidity_percent" : "temperature_c",
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

#include "control/command_queue.h"

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "espagent_config.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "control_queue";

#define CONTROL_RECENT_DEPTH 8
#define CONTROL_STATE_DEPTH  8
#define CONTROL_RECENT_TTL_MS (2 * 60 * 1000)

typedef struct {
    bool used;
    char command_id[ESPAGENT_MESH_ID_MAX];
    int64_t ts_ms;
} recent_command_t;

typedef struct {
    bool used;
    char command_id[ESPAGENT_MESH_ID_MAX];
    char action[ESPAGENT_MESH_ACTION_MAX];
    char status[16];
    char summary[160];
    int64_t started_ms;
    int64_t finished_ms;
} actuator_state_t;

static SemaphoreHandle_t s_lock;
static bool s_busy;
static bool s_emergency_stop;
static uint32_t s_recent_next;
static uint32_t s_state_next;
static recent_command_t s_recent[CONTROL_RECENT_DEPTH];
static actuator_state_t s_state[CONTROL_STATE_DEPTH];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
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

esp_err_t control_command_queue_init(void)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    lock();
    memset(s_recent, 0, sizeof(s_recent));
    memset(s_state, 0, sizeof(s_state));
    s_busy = false;
    s_emergency_stop = false;
    s_recent_next = 0;
    s_state_next = 0;
    unlock();
    return ESP_OK;
}

static bool seen_command_locked(const char *command_id)
{
    if (!command_id || !command_id[0]) {
        return false;
    }
    int64_t now = now_ms();
    for (int i = 0; i < CONTROL_RECENT_DEPTH; i++) {
        if (s_recent[i].used && now - s_recent[i].ts_ms > CONTROL_RECENT_TTL_MS) {
            memset(&s_recent[i], 0, sizeof(s_recent[i]));
            continue;
        }
        if (s_recent[i].used && strcmp(s_recent[i].command_id, command_id) == 0) {
            return true;
        }
    }
    return false;
}

static void remember_command_locked(const char *command_id)
{
    if (!command_id || !command_id[0]) {
        return;
    }
    int64_t now = now_ms();
    for (int i = 0; i < CONTROL_RECENT_DEPTH; i++) {
        if (s_recent[i].used && now - s_recent[i].ts_ms > CONTROL_RECENT_TTL_MS) {
            memset(&s_recent[i], 0, sizeof(s_recent[i]));
        }
    }
    recent_command_t *slot = &s_recent[s_recent_next % CONTROL_RECENT_DEPTH];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    snprintf(slot->command_id, sizeof(slot->command_id), "%s", command_id);
    slot->ts_ms = now;
    s_recent_next++;
}

static actuator_state_t *start_state_locked(const espagent_mesh_command_t *cmd)
{
    actuator_state_t *slot = &s_state[s_state_next % CONTROL_STATE_DEPTH];
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    snprintf(slot->command_id, sizeof(slot->command_id), "%s", cmd->command_id);
    snprintf(slot->action, sizeof(slot->action), "%s", cmd->action);
    snprintf(slot->status, sizeof(slot->status), "running");
    slot->started_ms = now_ms();
    s_state_next++;
    return slot;
}

static void finish_state_locked(actuator_state_t *slot,
                                esp_err_t err,
                                const char *summary)
{
    if (!slot) {
        return;
    }
    snprintf(slot->status, sizeof(slot->status), "%s", err == ESP_OK ? "ok" : "error");
    snprintf(slot->summary, sizeof(slot->summary), "%s", summary ? summary : "");
    slot->finished_ms = now_ms();
}

static esp_err_t admission_check_locked(const espagent_mesh_command_t *cmd,
                                        char *output,
                                        size_t output_size)
{
    if (!cmd || !cmd->command_id[0]) {
        snprintf(output, output_size, "Error: control command missing command_id");
        return ESP_ERR_INVALID_ARG;
    }
    bool is_status = strcmp(cmd->action, "control_state") == 0;
    bool is_stop = strcmp(cmd->action, "control_emergency_stop") == 0;
    if (s_emergency_stop && !is_status && !is_stop) {
        snprintf(output, output_size, "Error: control command blocked by emergency_stop");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy && !is_status && !is_stop) {
        snprintf(output, output_size, "Error: control command queue busy; actuator interlock active");
        return ESP_ERR_INVALID_STATE;
    }
    int interlock_gpio = ESPAGENT_CONTROL_INTERLOCK_GPIO;
    if (!is_status && !is_stop && interlock_gpio >= 0) {
        if (!GPIO_IS_VALID_GPIO((gpio_num_t)interlock_gpio)) {
            snprintf(output, output_size,
                     "Error: configured control interlock GPIO=%d is invalid",
                     interlock_gpio);
            return ESP_ERR_INVALID_ARG;
        }
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << (unsigned)interlock_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t gpio_err = gpio_config(&cfg);
        if (gpio_err != ESP_OK) {
            snprintf(output, output_size,
                     "Error: failed to configure control interlock GPIO=%d (%s)",
                     interlock_gpio,
                     esp_err_to_name(gpio_err));
            return gpio_err;
        }
        int level = gpio_get_level((gpio_num_t)interlock_gpio);
        if (level != ESPAGENT_CONTROL_INTERLOCK_ACTIVE_LEVEL) {
            snprintf(output, output_size,
                     "Error: hardware interlock open GPIO=%d level=%d expected=%d",
                     interlock_gpio,
                     level,
                     ESPAGENT_CONTROL_INTERLOCK_ACTIVE_LEVEL);
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (!is_status && seen_command_locked(cmd->command_id)) {
        snprintf(output, output_size, "Error: duplicate control command_id=%s rejected", cmd->command_id);
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * Mesh command ts_ms is generated from the coordinator node uptime, while
     * this queue runs on the control node uptime. These monotonic clocks do not
     * share an epoch, so comparing them here rejects valid cross-node commands
     * as "from the future". TTL is enforced by the sender wait path and kept in
     * the signed command envelope; the actuator side only validates schema,
     * Guardian decision, duplicate IDs, emergency stop, and hardware interlock.
     */
    return ESP_OK;
}

esp_err_t control_command_queue_submit(const espagent_mesh_command_t *cmd,
                                       control_command_executor_t executor,
                                       void *ctx,
                                       char *output,
                                       size_t output_size)
{
    if (!cmd || !executor || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: control queue unavailable");
        return err;
    }

    lock();
    err = admission_check_locked(cmd, output, output_size);
    if (err != ESP_OK) {
        unlock();
        return err;
    }
    bool bypass_busy = strcmp(cmd->action, "control_state") == 0 ||
                       strcmp(cmd->action, "control_emergency_stop") == 0;
    if (bypass_busy) {
        unlock();
        return executor(cmd, ctx, output, output_size);
    }
    s_busy = true;
    remember_command_locked(cmd->command_id);
    actuator_state_t *slot = start_state_locked(cmd);
    unlock();

    ESP_LOGI(TAG, "Control command admitted: id=%s action=%s", cmd->command_id, cmd->action);
    err = executor(cmd, ctx, output, output_size);

    lock();
    finish_state_locked(slot, err, output);
    s_busy = false;
    unlock();

    return err;
}

esp_err_t control_command_queue_emergency_stop(char *output, size_t output_size)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    lock();
    s_emergency_stop = true;
    s_busy = false;
    unlock();
    if (output && output_size) {
        snprintf(output, output_size, "OK: control emergency_stop latched; future control commands are blocked");
    }
    return ESP_OK;
}

esp_err_t control_command_queue_clear_emergency_stop(char *output, size_t output_size)
{
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        return err;
    }
    lock();
    s_emergency_stop = false;
    unlock();
    if (output && output_size) {
        snprintf(output, output_size, "OK: control emergency_stop cleared; new control commands are allowed");
    }
    return ESP_OK;
}

esp_err_t control_command_queue_state_json(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_lock();
    if (err != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"control queue unavailable\"}");
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        return ESP_ERR_NO_MEM;
    }

    lock();
    cJSON_AddStringToObject(root, "schema", "espagent.control_state.v1");
    cJSON_AddBoolToObject(root, "busy", s_busy);
    cJSON_AddBoolToObject(root, "emergency_stop", s_emergency_stop);
    cJSON_AddNumberToObject(root, "interlock_gpio", ESPAGENT_CONTROL_INTERLOCK_GPIO);
    cJSON_AddNumberToObject(root, "interlock_active_level", ESPAGENT_CONTROL_INTERLOCK_ACTIVE_LEVEL);
    uint32_t count = s_state_next < CONTROL_STATE_DEPTH ? s_state_next : CONTROL_STATE_DEPTH;
    uint32_t start = s_state_next > count ? s_state_next - count : 0;
    for (uint32_t i = 0; i < count; i++) {
        actuator_state_t *slot = &s_state[(start + i) % CONTROL_STATE_DEPTH];
        if (!slot->used) {
            continue;
        }
        cJSON *obj = cJSON_CreateObject();
        if (!obj) {
            continue;
        }
        cJSON_AddStringToObject(obj, "command_id", slot->command_id);
        cJSON_AddStringToObject(obj, "action", slot->action);
        cJSON_AddStringToObject(obj, "status", slot->status);
        cJSON_AddStringToObject(obj, "summary", slot->summary);
        cJSON_AddNumberToObject(obj, "started_ms", (double)slot->started_ms);
        cJSON_AddNumberToObject(obj, "finished_ms", (double)slot->finished_ms);
        cJSON_AddItemToArray(items, obj);
    }
    unlock();
    cJSON_AddItemToObject(root, "items", items);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "%s", json);
    cJSON_free(json);
    return ESP_OK;
}

#include "control/ir_emergency_stop.h"

#include "control/command_queue.h"
#include "espagent_config.h"
#include "roles/role_config.h"
#include "sensors/sensor_mqtt.h"
#include "tools/tool_gpio.h"
#include "tools/tool_servo.h"

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ir_stop";

#define IR_STOP_RX_SYMBOLS 80
#define IR_STOP_NEC_BITS   32
#define IR_STOP_TOLERANCE_US 260

typedef struct {
    TaskHandle_t task_to_notify;
    volatile size_t received_symbols;
} ir_stop_rx_ctx_t;

static TaskHandle_t s_task;
static rmt_channel_handle_t s_rx_channel;
static rmt_symbol_word_t *s_rx_symbols;
static ir_stop_rx_ctx_t s_rx_ctx;
static int64_t s_last_trigger_ms;
static uint32_t s_rx_jobs;
static uint32_t s_rx_done_events;
static uint32_t s_nec_frames;
static uint32_t s_trigger_count;
static size_t s_last_symbol_count;
static uint16_t s_last_addr;
static uint8_t s_last_cmd;
static bool s_last_parse_ok;

static bool in_range(uint32_t value, uint32_t target, uint32_t tolerance)
{
    return value >= target - tolerance && value <= target + tolerance;
}

static bool nec_symbol_is_bit(const rmt_symbol_word_t *s, int *bit)
{
    if (!s || !bit) {
        return false;
    }

    bool mark_first = s->level0 == 0 && s->level1 == 1;
    uint32_t mark = mark_first ? s->duration0 : s->duration1;
    uint32_t space = mark_first ? s->duration1 : s->duration0;

    if (!in_range(mark, 560, IR_STOP_TOLERANCE_US)) {
        return false;
    }
    if (in_range(space, 560, IR_STOP_TOLERANCE_US)) {
        *bit = 0;
        return true;
    }
    if (in_range(space, 1690, IR_STOP_TOLERANCE_US * 2)) {
        *bit = 1;
        return true;
    }
    return false;
}

static bool nec_symbol_is_header(const rmt_symbol_word_t *s)
{
    if (!s) {
        return false;
    }
    uint32_t mark = s->level0 == 0 ? s->duration0 : s->duration1;
    uint32_t space = s->level0 == 0 ? s->duration1 : s->duration0;
    return in_range(mark, 9000, 1200) && in_range(space, 4500, 900);
}

static bool parse_nec_frame(const rmt_symbol_word_t *symbols,
                            size_t count,
                            uint16_t *addr_out,
                            uint8_t *cmd_out)
{
    if (!symbols || count < IR_STOP_NEC_BITS + 1 || !addr_out || !cmd_out) {
        return false;
    }

    size_t start = 0;
    for (; start < count; start++) {
        if (nec_symbol_is_header(&symbols[start])) {
            break;
        }
    }
    if (start >= count || start + IR_STOP_NEC_BITS >= count) {
        return false;
    }

    uint32_t raw = 0;
    for (int i = 0; i < IR_STOP_NEC_BITS; i++) {
        int bit = 0;
        if (!nec_symbol_is_bit(&symbols[start + 1 + i], &bit)) {
            return false;
        }
        raw |= ((uint32_t)bit) << i;
    }

    uint8_t addr = raw & 0xff;
    uint8_t addr_inv = (raw >> 8) & 0xff;
    uint8_t cmd = (raw >> 16) & 0xff;
    uint8_t cmd_inv = (raw >> 24) & 0xff;
    if ((uint8_t)(addr ^ addr_inv) != 0xff || (uint8_t)(cmd ^ cmd_inv) != 0xff) {
        return false;
    }

    *addr_out = addr;
    *cmd_out = cmd;
    return true;
}

static bool nec_code_matches(uint16_t addr, uint8_t cmd)
{
#if ESPAGENT_IR_STOP_ANY_NEC
    (void)addr;
    (void)cmd;
    return true;
#else
    return addr == (uint16_t)ESPAGENT_IR_STOP_NEC_ADDR &&
           cmd == (uint8_t)ESPAGENT_IR_STOP_NEC_CMD;
#endif
}

static void append_safe_output_status(char *summary,
                                      size_t summary_size,
                                      const char *label,
                                      esp_err_t err)
{
    if (!summary || summary_size == 0 || !label) {
        return;
    }
    size_t off = strnlen(summary, summary_size);
    if (off >= summary_size - 1) {
        return;
    }
    snprintf(summary + off, summary_size - off, "%s%s=%s",
             off ? "," : "", label, err == ESP_OK ? "off" : "err");
}

static void force_local_safe_outputs(char *summary, size_t summary_size)
{
    if (summary && summary_size) {
        summary[0] = '\0';
    }

    char tmp[160] = {0};
    esp_err_t err = tool_set_status_light_execute("{\"color\":\"off\"}", tmp, sizeof(tmp));
    append_safe_output_status(summary, summary_size, "status_light", err);

    tmp[0] = '\0';
    err = tool_servo_set_pulse_us(0);
    append_safe_output_status(summary, summary_size, "servo_pwm", err);

    tmp[0] = '\0';
    err = tool_set_humidifier_execute("{\"state\":0}", tmp, sizeof(tmp));
    append_safe_output_status(summary, summary_size, "humidifier", err);

    tmp[0] = '\0';
    err = tool_set_fan_execute("{\"state\":0}", tmp, sizeof(tmp));
    append_safe_output_status(summary, summary_size, "fan", err);
}

static void trigger_ir_emergency_stop(uint16_t addr, uint8_t cmd)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_last_trigger_ms < ESPAGENT_IR_STOP_COOLDOWN_MS) {
        return;
    }
    s_last_trigger_ms = now_ms;

    char stop_out[160] = {0};
    char safe_summary[128] = {0};
    (void)control_command_queue_emergency_stop(stop_out, sizeof(stop_out));
    force_local_safe_outputs(safe_summary, sizeof(safe_summary));

    char payload[256];
    snprintf(payload, sizeof(payload),
             "IR emergency stop triggered addr=0x%02x cmd=0x%02x safe_off=%s",
             addr, cmd, safe_summary[0] ? safe_summary : "none");
    ESP_LOGW(TAG, "%s", payload);
    (void)sensor_mqtt_publish_timeline_event("control",
                                             "ir_emergency_stop",
                                             "warn",
                                             payload,
                                             ESPAGENT_NODE_ID,
                                             "control_agent",
                                             NULL,
                                             NULL);
}

static bool ir_stop_rx_done_callback(rmt_channel_handle_t channel,
                                     const rmt_rx_done_event_data_t *edata,
                                     void *user_data)
{
    (void)channel;
    BaseType_t high_task_wakeup = pdFALSE;
    ir_stop_rx_ctx_t *ctx = (ir_stop_rx_ctx_t *)user_data;
    if (ctx && ctx->task_to_notify && edata && edata->flags.is_last) {
        ctx->received_symbols = edata->num_symbols;
        vTaskNotifyGiveFromISR(ctx->task_to_notify, &high_task_wakeup);
    }
    return high_task_wakeup == pdTRUE;
}

static esp_err_t start_receive_job(void)
{
    rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = 120000,
        .signal_range_max_ns = 12000000,
    };
    esp_err_t err = rmt_receive(s_rx_channel,
                                s_rx_symbols,
                                IR_STOP_RX_SYMBOLS * sizeof(rmt_symbol_word_t),
                                &rx_cfg);
    if (err == ESP_OK) {
        s_rx_jobs++;
    }
    return err;
}

static void ir_stop_task(void *arg)
{
    (void)arg;
    s_rx_ctx.task_to_notify = xTaskGetCurrentTaskHandle();
    ESP_LOGI(TAG, "IR emergency stop receiver started on GPIO%d",
             ESPAGENT_IR_STOP_RX_GPIO);

    ESP_ERROR_CHECK_WITHOUT_ABORT(start_receive_job());
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        size_t count = s_rx_ctx.received_symbols;
        s_rx_done_events++;
        s_last_symbol_count = count;
        uint16_t addr = 0;
        uint8_t cmd = 0;
        if (parse_nec_frame(s_rx_symbols, count, &addr, &cmd)) {
            s_last_parse_ok = true;
            s_last_addr = addr;
            s_last_cmd = cmd;
            s_nec_frames++;
            ESP_LOGI(TAG, "IR NEC frame addr=0x%02x cmd=0x%02x symbols=%u",
                     addr, cmd, (unsigned)count);
            if (nec_code_matches(addr, cmd)) {
                s_trigger_count++;
                trigger_ir_emergency_stop(addr, cmd);
            }
        } else if (count > 0) {
            s_last_parse_ok = false;
            ESP_LOGI(TAG, "Ignored non-NEC IR frame symbols=%u", (unsigned)count);
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(start_receive_job());
    }
}

esp_err_t ir_emergency_stop_init(void)
{
    if (!espagent_role_is_control()) {
        ESP_LOGI(TAG, "IR emergency stop skipped for role=%s", ESPAGENT_NODE_ROLE);
        return ESP_OK;
    }
    if (ESPAGENT_IR_STOP_RX_GPIO < 0) {
        ESP_LOGI(TAG, "IR emergency stop disabled by GPIO=%d", ESPAGENT_IR_STOP_RX_GPIO);
        return ESP_OK;
    }
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)ESPAGENT_IR_STOP_RX_GPIO)) {
        ESP_LOGW(TAG, "IR emergency stop GPIO%d invalid", ESPAGENT_IR_STOP_RX_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    if (ESPAGENT_IR_STOP_RX_GPIO == 0) {
        ESP_LOGW(TAG,
                 "IR receiver uses GPIO0 boot strap; keep IR output idle-high during reset/power-on");
    }

    gpio_config_t gpio_cfg = {
        .pin_bit_mask = 1ULL << ESPAGENT_IR_STOP_RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gpio_cfg), TAG, "IR GPIO config failed");

    s_rx_symbols = heap_caps_calloc(IR_STOP_RX_SYMBOLS,
                                    sizeof(rmt_symbol_word_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    if (!s_rx_symbols) {
        return ESP_ERR_NO_MEM;
    }

    rmt_rx_channel_config_t rx_chan_cfg = {
        .gpio_num = ESPAGENT_IR_STOP_RX_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = ESPAGENT_IR_STOP_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .flags.invert_in = false,
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(rmt_new_rx_channel(&rx_chan_cfg, &s_rx_channel),
                        TAG, "create IR RMT RX channel failed");

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = ir_stop_rx_done_callback,
    };
    ESP_RETURN_ON_ERROR(rmt_rx_register_event_callbacks(s_rx_channel, &cbs, &s_rx_ctx),
                        TAG, "register IR RMT callback failed");
    ESP_RETURN_ON_ERROR(rmt_enable(s_rx_channel), TAG, "enable IR RMT RX failed");
    return ESP_OK;
}

esp_err_t ir_emergency_stop_start(void)
{
    if (!espagent_role_is_control() || ESPAGENT_IR_STOP_RX_GPIO < 0) {
        return ESP_OK;
    }
    if (!s_rx_channel || !s_rx_symbols) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(ir_stop_task,
                                            "ir_stop",
                                            ESPAGENT_IR_STOP_RX_STACK,
                                            NULL,
                                            ESPAGENT_IR_STOP_RX_PRIO,
                                            &s_task,
                                            ESPAGENT_IR_STOP_RX_CORE);
    if (ok != pdPASS || !s_task) {
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t ir_emergency_stop_status(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(output,
             output_size,
             "{\"role_is_control\":%s,\"gpio\":%d,\"enabled\":%s,"
             "\"task_running\":%s,\"rx_ready\":%s,\"any_nec\":%d,"
             "\"rx_jobs\":%lu,\"rx_done_events\":%lu,\"nec_frames\":%lu,"
             "\"trigger_count\":%lu,\"last_symbols\":%u,"
             "\"last_parse_ok\":%s,\"last_addr\":\"0x%02x\",\"last_cmd\":\"0x%02x\"}",
             espagent_role_is_control() ? "true" : "false",
             ESPAGENT_IR_STOP_RX_GPIO,
             ESPAGENT_IR_STOP_RX_GPIO >= 0 ? "true" : "false",
             s_task ? "true" : "false",
             (s_rx_channel && s_rx_symbols) ? "true" : "false",
             ESPAGENT_IR_STOP_ANY_NEC,
             (unsigned long)s_rx_jobs,
             (unsigned long)s_rx_done_events,
             (unsigned long)s_nec_frames,
             (unsigned long)s_trigger_count,
             (unsigned)s_last_symbol_count,
             s_last_parse_ok ? "true" : "false",
             s_last_addr,
             s_last_cmd);
    return ESP_OK;
}

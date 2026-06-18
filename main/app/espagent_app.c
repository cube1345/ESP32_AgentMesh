#include "espagent_app.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cJSON.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "espagent_config.h"
#include "agent/agent_loop.h"
#include "automation/automation_engine.h"
#include "bus/message_bus.h"
#include "cache/cache_store.h"
#include "channels/feishu/feishu_bot.h"
#include "cli/serial_cli.h"
#include "cron/cron_service.h"
#include "gateway/ws_server.h"
#include "heartbeat/heartbeat.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "onboard/wifi_onboard.h"
#include "proactive/proactive_service.h"
#include "proxy/http_proxy.h"
#include "roles/control_node.h"
#include "roles/coordinator_node.h"
#include "roles/display_node.h"
#include "roles/guardian_node.h"
#include "roles/role_config.h"
#include "roles/sensor_node.h"
#include "sensors/sensor_mqtt.h"
#include "skills/skill_loader.h"
#include "time_sync/time_sync.h"
#include "tools/tool_environment.h"
#include "tools/tool_hc_sr05.h"
#include "tools/tool_registry.h"
#include "tools/tool_servo.h"
#include "tools/tool_sgp30.h"
#include "wifi/wifi_manager.h"
#include "espnow/espnow_sender.h"

static const char *TAG = "ESPAgent";

#define ESPAGENT_BOOT_SERVO_STACK                 3072
#define ESPAGENT_BOOT_SERVO_PRIO                  4
#define ESPAGENT_BOOT_SERVO_CORE                  0
#define ESPAGENT_PRESENCE_MONITOR_STACK           3072
#define ESPAGENT_PRESENCE_MONITOR_PRIO            4
#define ESPAGENT_PRESENCE_MONITOR_CORE            0
#define ESPAGENT_PRESENCE_MONITOR_INTERVAL_MS     1000
#define ESPAGENT_ENVIRONMENT_MONITOR_STACK        5120
#define ESPAGENT_ENVIRONMENT_MONITOR_PRIO         4
#define ESPAGENT_ENVIRONMENT_MONITOR_CORE         0
#define ESPAGENT_ENVIRONMENT_MONITOR_INTERVAL_MS  3000

static esp_err_t create_pinned_task(TaskFunction_t task_func,
                                    const char *task_name,
                                    uint32_t stack_bytes,
                                    UBaseType_t priority,
                                    BaseType_t core_id)
{
    BaseType_t ok = xTaskCreatePinnedToCore(
        task_func,
        task_name,
        stack_bytes,
        NULL,
        priority,
        NULL,
        core_id);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static void publish_feishu_outbound_event(const char *chat_id,
                                          const char *text,
                                          const char *status)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    char preview[192] = {0};
    if (text && text[0]) {
        size_t text_len = strlen(text);
        size_t copy_len = text_len > 128 ? 128 : text_len;
        snprintf(preview, sizeof(preview), "%.*s%s", (int)copy_len, text,
                 text_len > copy_len ? "..." : "");
    }

    int64_t ts_ms = esp_timer_get_time() / 1000;
    cJSON_AddStringToObject(root, "node_id", ESPAGENT_NODE_ID);
    cJSON_AddStringToObject(root, "role", ESPAGENT_NODE_ROLE);
    cJSON_AddStringToObject(root, "channel", ESPAGENT_CHAN_FEISHU);
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", "feishu_outbound");
    cJSON_AddStringToObject(root, "status", status ? status : "");
    if (chat_id && chat_id[0]) {
        cJSON_AddStringToObject(root, "chat_id", chat_id);
    }
    if (preview[0]) {
        cJSON_AddStringToObject(root, "text_preview", preview);
        cJSON_AddNumberToObject(root, "text_len", (double)strlen(text));
    }
    cJSON_AddNumberToObject(root, "ts_ms", (double)ts_ms);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return;
    }

    (void)sensor_mqtt_publish_text(ESPAGENT_SENSOR_MQTT_TOPIC_EVENTS, json);
    (void)sensor_mqtt_publish_text(ESPAGENT_MESH_TOPIC_TIMELINE, json);
    cJSON_free(json);
}

static void boot_servo_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG, "Boot servo demo: moving servo on GPIO%d", ESPAGENT_SERVO_DEFAULT_GPIO);
    if (tool_servo_set_angle(90) != ESP_OK) {
        ESP_LOGW(TAG, "Boot servo demo failed at 90 degrees");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(600));

    if (tool_servo_set_angle(0) != ESP_OK) {
        ESP_LOGW(TAG, "Boot servo demo failed at 0 degrees");
    }

    ESP_LOGI(TAG, "Boot servo demo complete");
    vTaskDelete(NULL);
}

static void environment_monitor_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG,
             "Environment monitor started: AHT20 HW I2C SDA=GPIO%d SCL=GPIO%d, SGP30 HW I2C SDA=GPIO%d SCL=GPIO%d, GY-30 software I2C SDA=GPIO%d SCL=GPIO%d, interval=%dms, bypassing LLM",
             ESPAGENT_AHT10_DEFAULT_SDA_GPIO,
             ESPAGENT_AHT10_DEFAULT_SCL_GPIO,
             ESPAGENT_SGP30_DEFAULT_SDA_GPIO,
             ESPAGENT_SGP30_DEFAULT_SCL_GPIO,
             ESPAGENT_BH1750_DEFAULT_SDA_GPIO,
             ESPAGENT_BH1750_DEFAULT_SCL_GPIO,
             ESPAGENT_ENVIRONMENT_MONITOR_INTERVAL_MS);

    while (1) {
        tool_environment_values_t values = {
            .temperature_c_x10 = -1,
            .humidity_percent_x10 = -1,
            .co2eq_ppm = -1,
            .tvoc_ppb = -1,
            .light_lux_x10 = -1,
            .light_raw = -1,
            .sgp30_warming_up = false,
        };
        char status[128] = {0};
        esp_err_t err = tool_environment_read_values(&values, status, sizeof(status));
        if (err == ESP_OK) {
            char payload[256] = {0};
            snprintf(payload, sizeof(payload),
                     "co2=%d,temp_x10=%d,hum_x10=%d,lux_x10=%d,raw=%d,warmup=%d",
                     values.co2eq_ppm,
                     values.temperature_c_x10,
                     values.humidity_percent_x10,
                     values.light_lux_x10,
                     values.light_raw,
                     values.sgp30_warming_up ? 1 : 0);
            ESP_LOGI(TAG, "Environment monitor: %s [%s]", payload, status);
            esp_err_t send_err = espnow_sender_send_text("env", payload);
            if (send_err != ESP_OK) {
                ESP_LOGW(TAG, "Environment ESP-NOW send failed: %s", esp_err_to_name(send_err));
            }
        } else {
            char payload[256] = {0};
            snprintf(payload, sizeof(payload),
                     "co2=-1,temp_x10=-1,hum_x10=-1,lux_x10=-1,raw=-1,warmup=-1");
            ESP_LOGW(TAG, "Environment monitor failed: %s (%s), send fallback payload", esp_err_to_name(err), status);
            esp_err_t send_err = espnow_sender_send_text("env", payload);
            if (send_err != ESP_OK) {
                ESP_LOGW(TAG, "Environment fallback ESP-NOW send failed: %s", esp_err_to_name(send_err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(ESPAGENT_ENVIRONMENT_MONITOR_INTERVAL_MS));
    }
}

static void presence_monitor_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Presence monitor started on GPIO%d, interval=%dms",
             ESPAGENT_PRESENCE_DEFAULT_GPIO, ESPAGENT_PRESENCE_MONITOR_INTERVAL_MS);

    while (1) {
        char output[256] = {0};
        esp_err_t err = tool_read_presence_execute("{}", output, sizeof(output));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Presence monitor: %s", output);
        } else {
            ESP_LOGW(TAG, "Presence monitor failed: %s (%s)", esp_err_to_name(err), output);
        }
        vTaskDelay(pdMS_TO_TICKS(ESPAGENT_PRESENCE_MONITOR_INTERVAL_MS));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = ESPAGENT_SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

static void outbound_dispatch_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        espagent_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) {
            continue;
        }

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if (strcmp(msg.channel, ESPAGENT_CHAN_FEISHU) == 0) {
            esp_err_t send_err = feishu_send_message(msg.chat_id, msg.content);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Feishu send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
                publish_feishu_outbound_event(msg.chat_id, msg.content, "send_failed");
            } else {
                ESP_LOGI(TAG, "Feishu send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
                publish_feishu_outbound_event(msg.chat_id, msg.content, "send_ok");
            }
        } else if (strcmp(msg.channel, ESPAGENT_CHAN_WEBSOCKET) == 0) {
            esp_err_t ws_err = ws_server_send(msg.chat_id, msg.content);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            }
        } else if (strcmp(msg.channel, ESPAGENT_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

esp_err_t espagent_app_init_subsystems(void)
{
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "init_nvs failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default failed");
    ESP_RETURN_ON_ERROR(init_spiffs(), TAG, "init_spiffs failed");

    ESP_RETURN_ON_ERROR(message_bus_init(), TAG, "message_bus_init failed");
    ESP_RETURN_ON_ERROR(memory_store_init(), TAG, "memory_store_init failed");
    ESP_RETURN_ON_ERROR(cache_store_init(), TAG, "cache_store_init failed");
    ESP_RETURN_ON_ERROR(skill_loader_init(), TAG, "skill_loader_init failed");
    ESP_RETURN_ON_ERROR(session_mgr_init(), TAG, "session_mgr_init failed");
    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "wifi_manager_init failed");
    ESP_RETURN_ON_ERROR(http_proxy_init(), TAG, "http_proxy_init failed");

    if (espagent_role_runs_chat_channels()) {
        ESP_RETURN_ON_ERROR(feishu_bot_init(), TAG, "feishu_bot_init failed");
    } else {
        ESP_LOGI(TAG, "Feishu channel init skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    if (espagent_role_runs_llm()) {
        ESP_RETURN_ON_ERROR(llm_proxy_init(), TAG, "llm_proxy_init failed");
    } else {
        ESP_LOGI(TAG, "LLM proxy init skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    ESP_RETURN_ON_ERROR(tool_registry_init(), TAG, "tool_registry_init failed");

    if (espagent_role_runs_scheduler()) {
        ESP_RETURN_ON_ERROR(automation_engine_init(), TAG, "automation_engine_init failed");
        ESP_RETURN_ON_ERROR(cron_service_init(), TAG, "cron_service_init failed");
        ESP_RETURN_ON_ERROR(heartbeat_init(), TAG, "heartbeat_init failed");
        ESP_RETURN_ON_ERROR(proactive_service_init(), TAG, "proactive_service_init failed");
    } else {
        ESP_LOGI(TAG, "Scheduler/proactive init skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    if (espagent_role_runs_llm()) {
        ESP_RETURN_ON_ERROR(agent_loop_init(), TAG, "agent_loop_init failed");
    } else {
        ESP_LOGI(TAG, "Agent loop init skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    ESP_RETURN_ON_ERROR(coordinator_node_init(), TAG, "coordinator_node_init failed");
    ESP_RETURN_ON_ERROR(sensor_node_init(), TAG, "sensor_node_init failed");
    ESP_RETURN_ON_ERROR(control_node_init(), TAG, "control_node_init failed");
    ESP_RETURN_ON_ERROR(display_node_init(), TAG, "display_node_init failed");
    ESP_RETURN_ON_ERROR(guardian_node_init(), TAG, "guardian_node_init failed");

    return ESP_OK;
}

esp_err_t espagent_app_start_local_services(void)
{
    ESP_RETURN_ON_ERROR(serial_cli_init(), TAG, "serial_cli_init failed");

    if (espagent_role_runs_control_outputs()) {
        ESP_RETURN_ON_ERROR(create_pinned_task(boot_servo_task, "boot_servo",
                                              ESPAGENT_BOOT_SERVO_STACK,
                                              ESPAGENT_BOOT_SERVO_PRIO,
                                              ESPAGENT_BOOT_SERVO_CORE),
                            TAG, "boot_servo task failed");
    } else {
        ESP_LOGI(TAG, "Boot servo demo skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    if (espagent_role_runs_sensor_sampling()) {
        ESP_RETURN_ON_ERROR(create_pinned_task(environment_monitor_task, "env_mon",
                                              ESPAGENT_ENVIRONMENT_MONITOR_STACK,
                                              ESPAGENT_ENVIRONMENT_MONITOR_PRIO,
                                              ESPAGENT_ENVIRONMENT_MONITOR_CORE),
                            TAG, "env_mon task failed");
        ESP_RETURN_ON_ERROR(create_pinned_task(presence_monitor_task, "presence_mon",
                                              ESPAGENT_PRESENCE_MONITOR_STACK,
                                              ESPAGENT_PRESENCE_MONITOR_PRIO,
                                              ESPAGENT_PRESENCE_MONITOR_CORE),
                            TAG, "presence_mon task failed");

        if (tool_sgp30_monitor_start() != ESP_OK) {
            ESP_LOGW(TAG, "SGP30 auto monitor unavailable");
        }
    } else {
        ESP_LOGI(TAG, "Local sensor monitors skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    return ESP_OK;
}

esp_err_t espagent_app_connect_wifi_or_onboard(void)
{
    esp_err_t wifi_err = wifi_manager_start();
    bool wifi_ok = false;
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            wifi_ok = true;
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials configured");
    }

    if (!wifi_ok) {
        ESP_LOGW(TAG, "Entering WiFi onboarding mode...");
        wifi_onboard_start(WIFI_ONBOARD_MODE_CAPTIVE);
        return ESP_ERR_INVALID_STATE;
    }

#if ESPAGENT_ONBOARD_ADMIN_AFTER_WIFI
    if (wifi_onboard_start(WIFI_ONBOARD_MODE_ADMIN) != ESP_OK) {
        ESP_LOGW(TAG, "Local admin portal unavailable; continuing without config hotspot");
    }
#else
    ESP_LOGI(TAG, "Local admin portal disabled after WiFi connect; keeping STA-only mode");
#endif

    esp_err_t time_err = espagent_time_sync_start();
    if (time_err == ESP_OK) {
        time_err = espagent_time_sync_wait(ESPAGENT_SNTP_SYNC_WAIT_MS);
        if (time_err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP time sync not ready yet: %s", esp_err_to_name(time_err));
        }
    } else {
        ESP_LOGW(TAG, "SNTP time sync start failed: %s", esp_err_to_name(time_err));
    }

    return ESP_OK;
}

esp_err_t espagent_app_start_network_services(void)
{
    ESP_RETURN_ON_ERROR(create_pinned_task(outbound_dispatch_task, "outbound",
                                          ESPAGENT_OUTBOUND_STACK,
                                          ESPAGENT_OUTBOUND_PRIO,
                                          ESPAGENT_OUTBOUND_CORE),
                        TAG, "outbound task failed");

    ESP_RETURN_ON_ERROR(coordinator_node_start(), TAG, "coordinator_node_start failed");
    ESP_RETURN_ON_ERROR(sensor_node_start(), TAG, "sensor_node_start failed");
    ESP_RETURN_ON_ERROR(control_node_start(), TAG, "control_node_start failed");
    ESP_RETURN_ON_ERROR(display_node_start(), TAG, "display_node_start failed");
    ESP_RETURN_ON_ERROR(guardian_node_start(), TAG, "guardian_node_start failed");

    if (espagent_role_runs_llm()) {
        ESP_RETURN_ON_ERROR(agent_loop_start(), TAG, "agent_loop_start failed");
    } else {
        ESP_LOGI(TAG, "Agent loop start skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    if (espagent_role_runs_chat_channels()) {
        ESP_RETURN_ON_ERROR(feishu_bot_start(), TAG, "feishu_bot_start failed");
    } else {
        ESP_LOGI(TAG, "Feishu channel start skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    ESP_RETURN_ON_ERROR(sensor_mqtt_start(), TAG, "sensor_mqtt_start failed");

    if (espagent_role_runs_scheduler()) {
        esp_err_t automation_err = automation_engine_start();
        if (automation_err != ESP_OK) {
            ESP_LOGW(TAG, "Automation engine start failed: %s", esp_err_to_name(automation_err));
        }

        esp_err_t cron_err = cron_service_start();
        if (cron_err != ESP_OK) {
            ESP_LOGW(TAG, "Cron service start failed: %s", esp_err_to_name(cron_err));
        }

        esp_err_t heartbeat_err = heartbeat_start();
        if (heartbeat_err != ESP_OK) {
            ESP_LOGW(TAG, "Heartbeat start failed: %s", esp_err_to_name(heartbeat_err));
        }

        esp_err_t proactive_err = proactive_service_start();
        if (proactive_err != ESP_OK) {
            ESP_LOGW(TAG, "Proactive service start failed: %s", esp_err_to_name(proactive_err));
        }
    } else {
        ESP_LOGI(TAG, "Scheduler/proactive start skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    if (espagent_role_runs_chat_channels()) {
        ESP_RETURN_ON_ERROR(ws_server_start(), TAG, "ws_server_start failed");
    } else {
        ESP_LOGI(TAG, "WebSocket chat gateway skipped for role=%s", ESPAGENT_NODE_ROLE);
    }

    ESP_LOGI(TAG, "All services started!");
    return ESP_OK;
}

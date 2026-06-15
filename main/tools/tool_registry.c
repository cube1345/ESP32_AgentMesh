#include "tool_registry.h"

#include "espagent_config.h"
#include "tools/tool_cron.h"
#include "tools/tool_files.h"
#include "tools/tool_get_time.h"
#include "tools/tool_mesh_command.h"
#include "tools/tool_subagent.h"
#include "tools/tool_gpio.h"
#include "tools/tool_aht10.h"
#include "tools/tool_environment.h"
#include "tools/tool_hc_sr05.h"
#include "tools/tool_servo.h"
#include "tools/tool_max98357.h"
#include "tools/tool_sgp30.h"
#include "tools/tool_bh1750.h"
#include "tools/tool_web_search.h"
#include "tools/tool_amap_weather.h"
#include "roles/role_config.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 30

static espagent_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;

static bool json_has_key(cJSON *root, const char *key)
{
    return root && cJSON_GetObjectItem(root, key) != NULL;
}

static bool json_bool_value(cJSON *root, const char *key, bool default_value)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : NULL;
    if (!item) {
        return default_value;
    }
    return cJSON_IsTrue(item);
}

static esp_err_t tool_read_temperature_humidity_execute(const char *input_json,
                                                        char *output,
                                                        size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    bool local = false;
    bool has_local_diagnostics = false;
    if (root && cJSON_IsObject(root)) {
        local = json_bool_value(root, "local", false);
        has_local_diagnostics = json_has_key(root, "sda_gpio") ||
                                json_has_key(root, "scl_gpio") ||
                                json_has_key(root, "i2c_port") ||
                                json_has_key(root, "scl_hz") ||
                                json_has_key(root, "address");
    }
    if (root) {
        cJSON_Delete(root);
    }

    if (espagent_role_is_coordinator() && !local && !has_local_diagnostics) {
        return tool_mesh_send_command_execute(
            "{\"target_role\":\"sensor_agent\",\"action\":\"read_temperature_humidity\",\"args\":{}}",
            output,
            output_size);
    }

    return tool_aht10_read_temperature_humidity_execute(input_json, output, output_size);
}

typedef esp_err_t (*tool_exec_fn_t)(const char *input_json, char *output, size_t output_size);

static esp_err_t route_control_action_to_mesh(const char *action,
                                              const char *input_json,
                                              char *output,
                                              size_t output_size)
{
    cJSON *args = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    if (!args || !cJSON_IsObject(args)) {
        if (args) {
            cJSON_Delete(args);
        }
        args = cJSON_CreateObject();
    }
    if (!args) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_DeleteItemFromObject(args, "local");

    cJSON *cmd = cJSON_CreateObject();
    if (!cmd) {
        cJSON_Delete(args);
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(cmd, "target_role", "control_agent");
    cJSON_AddStringToObject(cmd, "action", action);
    cJSON_AddItemToObject(cmd, "args", args);

    char *payload = cJSON_PrintUnformatted(cmd);
    cJSON_Delete(cmd);
    if (!payload) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = tool_mesh_send_command_execute(payload, output, output_size);
    cJSON_free(payload);
    return err;
}

static esp_err_t coordinator_control_or_local(const char *action,
                                              const char *input_json,
                                              char *output,
                                              size_t output_size,
                                              tool_exec_fn_t local_execute)
{
    cJSON *root = cJSON_Parse(input_json && input_json[0] ? input_json : "{}");
    bool local = false;
    if (root && cJSON_IsObject(root)) {
        local = json_bool_value(root, "local", false);
    }
    if (root) {
        cJSON_Delete(root);
    }

    if (espagent_role_is_coordinator() && !local) {
        return route_control_action_to_mesh(action, input_json, output, output_size);
    }

    return local_execute(input_json, output, output_size);
}

static esp_err_t tool_gpio_write_routed_execute(const char *input_json,
                                                char *output,
                                                size_t output_size)
{
    return coordinator_control_or_local("gpio_write", input_json, output, output_size,
                                        tool_gpio_write_execute);
}

static esp_err_t tool_ws2812_set_routed_execute(const char *input_json,
                                                char *output,
                                                size_t output_size)
{
    return coordinator_control_or_local("ws2812_set", input_json, output, output_size,
                                        tool_ws2812_set_execute);
}

static esp_err_t tool_set_status_light_routed_execute(const char *input_json,
                                                      char *output,
                                                      size_t output_size)
{
    return coordinator_control_or_local("set_status_light", input_json, output, output_size,
                                        tool_set_status_light_execute);
}

static esp_err_t tool_servo_write_routed_execute(const char *input_json,
                                                 char *output,
                                                 size_t output_size)
{
    return coordinator_control_or_local("servo_write", input_json, output, output_size,
                                        tool_servo_write_execute);
}

static void register_tool(const espagent_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }

    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    tool_web_search_init();
    tool_amap_weather_init();
    tool_gpio_init();

    register_tool(&(espagent_tool_t){
        .name = "web_search",
        .description = "Search the web for current information via Tavily (preferred) or Brave when configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "get_weather",
        .description = "Get structured current or forecast weather from Amap WebService. Prefer this over web_search for weather, temperature, rain, wind, forecast, 出门建议, 天气, 气温, 下雨, 降温, 穿衣, or daily proactive weather. Defaults to the configured home location when no location/adcode is provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"location\":{\"type\":\"string\",\"description\":\"Optional city/district/address such as 南京市栖霞区. If it is not an adcode, Amap geocoding resolves it first.\"},"
            "\"adcode\":{\"type\":\"string\",\"description\":\"Optional 6-digit Amap adcode. Overrides location when provided.\"},"
            "\"extensions\":{\"type\":\"string\",\"description\":\"'base' for live weather or 'all' for forecast. Defaults to 'base'.\"},"
            "\"type\":{\"type\":\"string\",\"description\":\"Optional alias: 'live' maps to base, 'forecast' maps to all.\"}},"
            "\"required\":[]}",
        .execute = tool_amap_weather_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_get_time_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_temperature_humidity",
        .description = "Read temperature and humidity from this board's local AHT10/AHT20 I2C sensor. On a coordinator_agent, do not use this for ordinary Feishu room temperature/humidity requests; route those through mesh_send_command to sensor_agent unless the user explicitly asks for this board, local sensor, or I2C wiring diagnostics. Optional SDA/SCL GPIO overrides can be provided for wiring diagnostics.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SDA GPIO override\"},"
            "\"scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SCL GPIO override\"},"
            "\"i2c_port\":{\"type\":\"integer\",\"description\":\"Optional I2C port override\"},"
            "\"scl_hz\":{\"type\":\"integer\",\"description\":\"Optional I2C clock speed in Hz, defaults to 100000\"},"
            "\"address\":{\"type\":\"integer\",\"description\":\"Optional AHT10 I2C address, normally 0x38\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly testing this board's local AHT10/AHT20 sensor instead of routing through sensor_agent\"}},"
            "\"required\":[]}",
        .execute = tool_read_temperature_humidity_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "mesh_send_command",
        .description = "Publish a standard MQTT Mesh command to another ESPAgent node or role. Use this when the coordinator should route a user request to another ESP32. For ordinary temperature/humidity requests such as '读取温湿度', use action=read_temperature_humidity and target_role=sensor_agent; target_node is optional. For remote control/status-light requests, use target_role=control_agent with action=set_status_light or ws2812_set and color/RGB args. Do not claim a Mesh command was sent unless this tool returns OK.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"target_node\":{\"type\":\"string\",\"description\":\"Optional target node id such as esp32s3-sensor-01. Overrides target_role when set.\"},"
            "\"target_role\":{\"type\":\"string\",\"description\":\"Optional target role such as sensor_agent or control_agent\"},"
            "\"action\":{\"type\":\"string\",\"description\":\"Command action such as read_temperature_humidity\"},"
            "\"args\":{\"type\":\"object\",\"description\":\"Optional JSON arguments for the command\"},"
            "\"args_json\":{\"type\":\"string\",\"description\":\"Optional raw JSON object string for arguments\"},"
            "\"command_id\":{\"type\":\"string\",\"description\":\"Optional command id. Auto-generated when omitted.\"},"
            "\"ttl_ms\":{\"type\":\"integer\",\"description\":\"Command time-to-live in milliseconds, defaults to 30000\"},"
            "\"safety_level\":{\"type\":\"integer\",\"description\":\"Safety level hint, defaults to 1\"},"
            "\"require_ack\":{\"type\":\"boolean\",\"description\":\"Whether the remote node should acknowledge, defaults to true\"}},"
            "\"required\":[\"action\"]}",
        .execute = tool_mesh_send_command_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "spawn_subagent",
        .description = "Delegate an independent subtask to a temporary ESPAgent subagent. The subagent runs its own short ReAct tool loop, cannot spawn nested subagents, and returns a concise result. Use this for separable work such as searching, reading files, or summarizing a focused subtask before the main answer.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"task\":{\"type\":\"string\",\"description\":\"The focused subtask for the subagent to complete\"},"
            "\"context\":{\"type\":\"string\",\"description\":\"Optional context, constraints, or relevant prior information for the subagent\"}},"
            "\"required\":[\"task\"]}",
        .execute = tool_subagent_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_environment",
        .description = "Read the 3-I2C environment sensor set in one call: AHT20/AHT10 temperature and humidity on hardware I2C0, SGP30 eCO2/TVOC on hardware I2C1, and GY-30/BH1750 light level on software I2C. Prefer this when the user asks for a combined environment test, comprehensive sensor test, AHT20+SGP30+GY30, or Chinese phrases like '综合测试', '环境数据', '读取全部传感器', '温湿度空气质量光照'.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"aht_sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional AHT20 hardware I2C SDA GPIO override\"},"
            "\"aht_scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional AHT20 hardware I2C SCL GPIO override\"},"
            "\"aht_i2c_port\":{\"type\":\"integer\",\"description\":\"Optional AHT20 hardware I2C port override\"},"
            "\"sgp30_sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SGP30 hardware I2C SDA GPIO override\"},"
            "\"sgp30_scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SGP30 hardware I2C SCL GPIO override\"},"
            "\"sgp30_i2c_port\":{\"type\":\"integer\",\"description\":\"Optional SGP30 hardware I2C port override\"},"
            "\"gy30_sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional GY-30 software I2C SDA GPIO override\"},"
            "\"gy30_scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional GY-30 software I2C SCL GPIO override\"},"
            "\"gy30_addr\":{\"type\":\"integer\",\"description\":\"Optional GY-30/BH1750 address, 0x23 by default or 0x5C when ADDR is high\"}},"
            "\"required\":[]}",
        .execute = tool_read_environment_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_presence",
        .description = "Read human presence from a 3-wire digital OUT human/PIR sensor, or from HC-SR05 ultrasonic proximity when Trig/Echo pins are configured. Prefer this when the user asks whether someone is nearby, whether a person is present, asks about human body sensing, proximity, obstacle distance, or Chinese phrases like '有人吗', '人体传感器', '有人靠近', '检测人体', '测一下距离', or 'HC-SR05'. Returns present=true/false, and distance_cm when ultrasonic mode is used.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"out_gpio\":{\"type\":\"integer\",\"description\":\"Optional 3-wire presence sensor OUT GPIO override\"},"
            "\"trig_gpio\":{\"type\":\"integer\",\"description\":\"Optional HC-SR05 Trig GPIO override\"},"
            "\"echo_gpio\":{\"type\":\"integer\",\"description\":\"Optional HC-SR05 Echo GPIO override; protect ESP32 input from 5V Echo\"},"
            "\"threshold_cm\":{\"type\":\"integer\",\"description\":\"Optional presence threshold in centimeters, defaults to configured threshold\"},"
            "\"samples\":{\"type\":\"integer\",\"description\":\"Optional sample count 1-7, defaults to 5\"}},"
            "\"required\":[]}",
        .execute = tool_read_presence_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "hc_sr05_read_distance",
        .description = "Low-level direct HC-SR05 ultrasonic distance read. Use this for explicit HC-SR05 diagnostics, Trig/Echo wiring checks, threshold tuning, or direct distance measurement requests.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"trig_gpio\":{\"type\":\"integer\",\"description\":\"Optional HC-SR05 Trig GPIO override\"},"
            "\"echo_gpio\":{\"type\":\"integer\",\"description\":\"Optional HC-SR05 Echo GPIO override; protect ESP32 input from 5V Echo\"},"
            "\"threshold_cm\":{\"type\":\"integer\",\"description\":\"Optional presence threshold in centimeters, defaults to configured threshold\"},"
            "\"samples\":{\"type\":\"integer\",\"description\":\"Optional sample count 1-7, defaults to 5\"}},"
            "\"required\":[]}",
        .execute = tool_hc_sr05_read_distance_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with " ESPAGENT_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " ESPAGENT_SPIFFS_BASE "/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with " ESPAGENT_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " ESPAGENT_SPIFFS_BASE "/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces the first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " ESPAGENT_SPIFFS_BASE "/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " ESPAGENT_SPIFFS_BASE "/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "gpio_write",
        .description = "Set an ESP32 GPIO output pin high or low. Use this for relays, digital outputs, or simple LEDs. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"ESP32 GPIO number to drive as output\"},"
            "\"state\":{\"type\":\"integer\",\"description\":\"0 for LOW, 1 for HIGH\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[\"pin\",\"state\"]}",
        .execute = tool_gpio_write_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "gpio_read",
        .description = "Read a GPIO pin state. Returns HIGH or LOW. Use for buttons, switches, and digital inputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"}},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_read_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "gpio_read_all",
        .description = "Read all allowed GPIO pin states in a single call.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_gpio_read_all_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "ws2812_set",
        .description = "Set a single WS2812/NeoPixel RGB LED color. Useful for the onboard RGB LED on ESP32-S3 boards. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"r\":{\"type\":\"integer\",\"description\":\"Red value 0-255\"},"
            "\"g\":{\"type\":\"integer\",\"description\":\"Green value 0-255\"},"
            "\"b\":{\"type\":\"integer\",\"description\":\"Blue value 0-255\"},"
            "\"brightness\":{\"type\":\"integer\",\"description\":\"Optional brightness 0-255, defaults to 255\"},"
            "\"pin\":{\"type\":\"integer\",\"description\":\"Optional GPIO override. Defaults to the configured onboard WS2812 pin.\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[\"r\",\"g\",\"b\"]}",
        .execute = tool_ws2812_set_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "set_status_light",
        .description = "Set the onboard RGB status light with a natural-language-friendly color. Prefer this when the user asks to turn the board light red, green, blue, white, yellow, purple, cyan, orange, or off. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"color\":{\"type\":\"string\",\"description\":\"Named color such as red, green, blue, white, yellow, orange, purple, cyan, or off\"},"
            "\"brightness\":{\"type\":\"integer\",\"description\":\"Optional brightness 0-255, defaults to 255\"},"
            "\"pin\":{\"type\":\"integer\",\"description\":\"Optional GPIO override. Defaults to the configured onboard WS2812 pin.\"},"
            "\"r\":{\"type\":\"integer\",\"description\":\"Optional red value 0-255 when using explicit RGB\"},"
            "\"g\":{\"type\":\"integer\",\"description\":\"Optional green value 0-255 when using explicit RGB\"},"
            "\"b\":{\"type\":\"integer\",\"description\":\"Optional blue value 0-255 when using explicit RGB\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_set_status_light_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "servo_write",
        .description = "Control the servo motor on GPIO5. Set the angle in degrees (0-180) or pulse width in microseconds. For requests like opening, starting, or testing the servo without a specific angle, prefer angle=90 to produce a visible motion. On a coordinator_agent, this defaults to the remote control_agent unless local=true is explicitly provided.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"angle\":{\"type\":\"integer\",\"description\":\"Target angle 0-180 degrees\"},"
            "\"pulse_us\":{\"type\":\"integer\",\"description\":\"Pulse width in microseconds (typically 500-2500 for standard servos)\"},"
            "\"local\":{\"type\":\"boolean\",\"description\":\"Set true only when explicitly controlling this coordinator board locally\"}},"
            "\"required\":[]}",
        .execute = tool_servo_write_routed_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "max98357_play_tone",
        .description = "Play a short test tone through a MAX98357 I2S audio amplifier / speaker. Use this when the user asks to test audio output, a speaker, an audio amplifier, a beep, or MAX98357 wiring. The fixed wiring is BCLK=GPIO1, WS/LRCLK=GPIO2, DIN=GPIO3; SD is optional.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"frequency_hz\":{\"type\":\"integer\",\"description\":\"Tone frequency in Hz, defaults to 440\"},"
            "\"duration_ms\":{\"type\":\"integer\",\"description\":\"Tone duration in milliseconds, defaults to 400\"},"
            "\"volume_pct\":{\"type\":\"integer\",\"description\":\"Output volume percentage 0-100, defaults to 25\"},"
            "\"bclk_gpio\":{\"type\":\"integer\",\"description\":\"Optional I2S BCLK GPIO override\"},"
            "\"ws_gpio\":{\"type\":\"integer\",\"description\":\"Optional I2S WS/LRCLK GPIO override\"},"
            "\"din_gpio\":{\"type\":\"integer\",\"description\":\"Optional I2S DATA/DIN GPIO override\"},"
            "\"sd_gpio\":{\"type\":\"integer\",\"description\":\"Optional amplifier shutdown GPIO override, if wired\"},"
            "\"i2s_port\":{\"type\":\"integer\",\"description\":\"Optional I2S port override, defaults to configured port\"},"
            "\"sample_rate_hz\":{\"type\":\"integer\",\"description\":\"Optional sample rate override in Hz\"}},"
            "\"required\":[]}",
        .execute = tool_max98357_play_tone_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "sgp30_read_air_quality",
        .description = "Read eCO2 and TVOC from an SGP30 air-quality sensor over I2C. Use this for direct SGP30 reads, VOC/TVOC checks, or explicit sensor diagnostics. Chinese requests like '读取SGP30', '查看TVOC', or '检测空气数据' map here. Optional SDA/SCL GPIO overrides can be provided if board defaults are not configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SDA GPIO override\"},"
            "\"scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SCL GPIO override\"},"
            "\"i2c_port\":{\"type\":\"integer\",\"description\":\"Optional I2C port override; -1 means auto-select\"},"
            "\"scl_hz\":{\"type\":\"integer\",\"description\":\"Optional I2C clock speed in Hz, defaults to 100000\"}},"
            "\"required\":[]}",
        .execute = tool_sgp30_read_air_quality_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_air_quality",
        .description = "Read air-quality telemetry from the onboard or attached sensor. Prefer this high-level tool when the user asks about air quality, TVOC, eCO2, VOC, indoor air conditions, or Chinese phrases such as '空气质量', '空气怎么样', '检测气体', 'VOC多少', or '读取传感器数据'. Currently backed by SGP30.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SDA GPIO override\"},"
            "\"scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SCL GPIO override\"},"
            "\"i2c_port\":{\"type\":\"integer\",\"description\":\"Optional I2C port override; -1 means auto-select\"},"
            "\"scl_hz\":{\"type\":\"integer\",\"description\":\"Optional I2C clock speed in Hz, defaults to 100000\"}},"
            "\"required\":[]}",
        .execute = tool_sgp30_read_air_quality_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "read_light_level",
        .description = "Read ambient light level in lux from a GY-30/BH1750 I2C light sensor. Prefer this when the user asks about light level, ambient light, illuminance, lux, GY-30, BH1750, 光照, 光线亮度, 照度, or 勒克斯. Optional SDA/SCL GPIO and address overrides can be provided for wiring diagnostics.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"sda_gpio\":{\"type\":\"integer\",\"description\":\"Optional SDA GPIO override\"},"
            "\"scl_gpio\":{\"type\":\"integer\",\"description\":\"Optional SCL GPIO override\"},"
            "\"i2c_port\":{\"type\":\"integer\",\"description\":\"Optional I2C port override\"},"
            "\"scl_hz\":{\"type\":\"integer\",\"description\":\"Optional I2C clock speed in Hz, defaults to 100000\"},"
            "\"address\":{\"type\":\"integer\",\"description\":\"Optional BH1750 I2C address, 0x23 by default or 0x5C when ADDR is high\"}},"
            "\"required\":[]}",
        .execute = tool_bh1750_read_light_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "cron_add",
        .description = "Schedule a recurring, daily, or one-shot proactive task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval, 'at' for one-shot at a unix timestamp, or 'daily' for a local-time daily task\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"hour\":{\"type\":\"integer\",\"description\":\"Local hour 0-23 (required for 'daily')\"},"
            "\"minute\":{\"type\":\"integer\",\"description\":\"Local minute 0-59 (required for 'daily')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'feishu' or 'websocket'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. If omitted during an active turn, current chat_id is used\"}},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
        .execute = tool_cron_list_execute,
    });

    register_tool(&(espagent_tool_t){
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    });

    build_tools_json();
    tool_subagent_init();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

void tool_registry_get_tools(const espagent_tool_t **tools, int *count)
{
    if (tools) {
        *tools = s_tools;
    }
    if (count) {
        *count = s_tool_count;
    }
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}

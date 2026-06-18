#include "agent_loop.h"
#include "agent/context_builder.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/session_mgr.h"
#include "proactive/proactive_service.h"
#include "sensors/sensor_mqtt.h"
#include "espagent_config.h"
#include "tools/tool_registry.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE (8 * 1024)

static size_t utf8_expected_len(unsigned char c) {
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    return 2;
  }
  if ((c & 0xF0) == 0xE0) {
    return 3;
  }
  if ((c & 0xF8) == 0xF0) {
    return 4;
  }
  return 0;
}

static size_t utf8_safe_prefix_len(const char *buf, size_t len) {
  size_t i = 0;
  size_t last_good = 0;

  while (i < len) {
    unsigned char c = (unsigned char)buf[i];
    size_t need = utf8_expected_len(c);
    if (need == 0 || i + need > len) {
      break;
    }

    bool valid = true;
    for (size_t j = 1; j < need; j++) {
      unsigned char cc = (unsigned char)buf[i + j];
      if ((cc & 0xC0) != 0x80) {
        valid = false;
        break;
      }
    }
    if (!valid) {
      break;
    }

    i += need;
    last_good = i;
  }

  return last_good;
}

static size_t terminate_at_utf8_boundary(char *buf, size_t len) {
  size_t safe = utf8_safe_prefix_len(buf, len);
  buf[safe] = '\0';
  return safe;
}

static bool text_is_proactive_no_message(const char *text) {
  if (!text) {
    return false;
  }

  while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') {
    text++;
  }

  size_t len = strlen(text);
  while (len > 0 &&
         (text[len - 1] == ' ' || text[len - 1] == '\n' ||
          text[len - 1] == '\r' || text[len - 1] == '\t')) {
    len--;
  }

  return len == strlen(ESPAGENT_PROACTIVE_NO_MESSAGE) &&
         strncmp(text, ESPAGENT_PROACTIVE_NO_MESSAGE, len) == 0;
}

static bool contains_substr_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || needle[0] == '\0') {
    return false;
  }

  const size_t needle_len = strlen(needle);
  for (const char *p = haystack; *p; p++) {
    size_t i = 0;
    while (i < needle_len && p[i]) {
      unsigned char hc = (unsigned char)p[i];
      unsigned char nc = (unsigned char)needle[i];

      if (hc >= 'A' && hc <= 'Z')
        hc = (unsigned char)(hc - 'A' + 'a');
      if (nc >= 'A' && nc <= 'Z')
        nc = (unsigned char)(nc - 'A' + 'a');
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

static bool message_has_any_keyword(const char *message,
                                    const char *const *keywords,
                                    size_t keyword_count) {
  if (!message || !keywords) {
    return false;
  }

  for (size_t i = 0; i < keyword_count; i++) {
    if (contains_substr_ci(message, keywords[i])) {
      return true;
    }
  }

  return false;
}

static bool tool_guard_match_light_sensor_request(const char *message) {
  static const char *const topic_keywords[] = {
      "light level", "ambient light", "illuminance",
      "lux",         "gy-30",         "gy30",
      "bh1750",      "light sensor",  "brightness sensor",
      "光照",        "光线",          "亮度",
      "照度",        "勒克斯",        "光传感器",
      "gy-30",       "gy30",          "bh1750",
  };
  static const char *const intent_keywords[] = {
      "read",       "check",    "measure", "detect", "show",   "status",
      "how bright", "how much", "读取",    "检测",   "测量",   "查看",
      "读一下",     "多少",     "数值",    "状态",   "亮不亮",
  };

  if (contains_substr_ci(message, "gy-30") ||
      contains_substr_ci(message, "gy30") ||
      contains_substr_ci(message, "bh1750") ||
      contains_substr_ci(message, "lux") ||
      contains_substr_ci(message, "光照") ||
      contains_substr_ci(message, "照度")) {
    return true;
  }

  return message_has_any_keyword(message, topic_keywords,
                                 sizeof(topic_keywords) /
                                     sizeof(topic_keywords[0])) &&
         message_has_any_keyword(message, intent_keywords,
                                 sizeof(intent_keywords) /
                                     sizeof(intent_keywords[0]));
}

static bool tool_guard_match_environment_request(const char *message) {
  static const char *const keywords[] = {
      "environment",
      "all sensors",
      "combined sensor",
      "combined test",
      "sensor suite",
      "aht20",
      "aht10",
      "sgp30",
      "gy-30",
      "gy30",
      "bh1750",
      "综合测试",
      "环境数据",
      "环境传感器",
      "全部传感器",
      "所有传感器",
      "温湿度空气质量光照",
      "温湿度",
      "空气质量",
      "光照",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_light_request(const char *message) {
  static const char *const keywords[] = {
      "light",       "led",    "rgb",   "ws2812", "neopixel", "status light",
      "board light", "颜色",   "灯",    "灯光",   "亮灯",     "板载灯",
      "彩灯",        "状态灯", "rgb灯", "ws2812",
  };
  if (tool_guard_match_light_sensor_request(message)) {
    return false;
  }
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_air_quality_request(const char *message) {
  static const char *const topic_keywords[] = {
      "air quality", "tvoc",       "voc",        "eco2",     "co2",
      "sgp30",       "indoor air", "gas sensor", "空气质量", "空气传感器",
      "气体传感器",  "气体数据",   "voc",        "tvoc",     "eco2",
      "co2",         "sgp30",
  };
  static const char *const intent_keywords[] = {
      "read",   "check",   "measure", "detect", "show", "status",
      "how is", "what is", "读取",    "检测",   "测量", "查看",
      "读一下", "怎么样",  "多少",    "数值",   "状态",
  };

  if (contains_substr_ci(message, "sgp30")) {
    return true;
  }

  return message_has_any_keyword(message, topic_keywords,
                                 sizeof(topic_keywords) /
                                     sizeof(topic_keywords[0])) &&
         message_has_any_keyword(message, intent_keywords,
                                 sizeof(intent_keywords) /
                                     sizeof(intent_keywords[0]));
}

static bool tool_guard_match_presence_request(const char *message) {
  static const char *const topic_keywords[] = {
      "presence",   "human sensor", "person",     "someone",
      "anyone",     "nearby",       "proximity",  "distance sensor",
      "ultrasonic", "hc-sr05",      "hcsr05",     "hc sr05",
      "trig",       "echo",         "人体传感器", "人体",
      "有人",       "人靠近",       "靠近",       "距离",
      "测距",       "超声波",       "障碍",       "hc-sr05",
      "hcsr05",
  };
  static const char *const intent_keywords[] = {
      "read",   "check",    "measure",   "detect",     "show",
      "status", "is there", "is anyone", "is someone", "读取",
      "检测",   "测量",     "查看",      "读一下",     "有没有",
      "有人吗", "靠近吗",   "多少",      "状态",
  };

  if (contains_substr_ci(message, "hc-sr05") ||
      contains_substr_ci(message, "hcsr05") ||
      contains_substr_ci(message, "人体传感器")) {
    return true;
  }

  return message_has_any_keyword(message, topic_keywords,
                                 sizeof(topic_keywords) /
                                     sizeof(topic_keywords[0])) &&
         message_has_any_keyword(message, intent_keywords,
                                 sizeof(intent_keywords) /
                                     sizeof(intent_keywords[0]));
}

static bool tool_guard_match_gpio_write_request(const char *message) {
  static const char *const keywords[] = {
      "gpio",           "pin",    "io",     "output",    "relay",
      "mosfet",         "high",   "low",    "pull high", "pull low",
      "digital output", "引脚",   "脚位",   "io口",      "输出",
      "继电器",         "高电平", "低电平", "拉高",      "拉低",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_gpio_read_request(const char *message) {
  static const char *const keywords[] = {
      "gpio",     "pin",   "io",    "read pin", "button", "switch",
      "input",    "state", "level", "引脚",     "脚位",   "io口",
      "读取引脚", "按钮",  "开关",  "输入",     "状态",   "电平",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_cron_request(const char *message) {
  static const char *const keywords[] = {
      "cron",     "schedule", "scheduled", "timer", "timed",    "remind",
      "reminder", "every",    "later",     "定时",  "计划任务", "提醒",
      "定时任务", "稍后",     "每隔",      "到点",  "每天",     "每日",
      "早上",     "上午",     "中午",      "晚上",  "主动",     "关心",
      "问候",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_weather_request(const char *message) {
  static const char *const keywords[] = {
      "weather", "temperature", "forecast", "rain", "wind", "cold", "hot",
      "umbrella", "coat", "天气", "气温", "温度", "预报", "下雨", "降雨",
      "风力", "降温", "升温", "冷不冷", "热不热", "带伞", "穿衣", "出门",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_servo_request(const char *message) {
  static const char *const keywords[] = {
      "servo",      "angle",       "rotate",      "rotation",   "turn servo",
      "open servo", "start servo", "move servo",  "test servo", "steer",
      "steering",   "pwm",         "pulse width", "舵机",       "角度",
      "旋转",       "转动",        "顺时针",      "逆时针",     "脉宽",
      "打开舵机",   "开舵机",      "启动舵机",    "舵机打开",   "舵机动一下",
      "让舵机动",   "测试舵机",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool tool_guard_match_temperature_humidity_request(const char *message) {
  static const char *const keywords[] = {
      "temperature", "humidity", "temp", "hum", "aht20", "aht10",
      "温度",        "湿度",     "温湿度", "气温", "室温",  "环境温度",
      "读取温湿度",  "读一下温度", "湿度多少",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool text_claims_mesh_dispatched(const char *text) {
  if (!text) {
    return false;
  }

  const bool mentions_mesh =
      contains_substr_ci(text, "mqtt") || contains_substr_ci(text, "mesh") ||
      contains_substr_ci(text, "control_agent") ||
      contains_substr_ci(text, "sensor_agent") ||
      contains_substr_ci(text, "已通过 MQTT") ||
      contains_substr_ci(text, "通过 MQTT") ||
      contains_substr_ci(text, "MQTT Mesh");

  const bool claims_sent =
      contains_substr_ci(text, "sent") || contains_substr_ci(text, "queued") ||
      contains_substr_ci(text, "published") ||
      contains_substr_ci(text, "已发送") || contains_substr_ci(text, "已发出") ||
      contains_substr_ci(text, "已下发") || contains_substr_ci(text, "已发布") ||
      contains_substr_ci(text, "已通过") || contains_substr_ci(text, "发送到") ||
      contains_substr_ci(text, "下发到");

  return mentions_mesh && claims_sent;
}

static bool message_should_have_used_mesh(const char *message) {
  return tool_guard_match_temperature_humidity_request(message) ||
         tool_guard_match_light_request(message) ||
         tool_guard_match_gpio_write_request(message) ||
         tool_guard_match_servo_request(message);
}

static bool final_text_is_unexecuted_mesh_claim(const char *text,
                                                const espagent_msg_t *msg) {
  return msg && message_should_have_used_mesh(msg->content) &&
         text_claims_mesh_dispatched(text);
}

static bool message_has_local_marker(const char *message) {
  static const char *const keywords[] = {
      "local", "this board", "coordinator", "usb0", "本机", "本地",
      "这块板", "第一角色", "协调器",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static const char *detect_status_light_color(const char *message) {
  if (!message) {
    return NULL;
  }
  if (contains_substr_ci(message, "blue") || contains_substr_ci(message, "蓝")) {
    return "blue";
  }
  if (contains_substr_ci(message, "green") || contains_substr_ci(message, "绿")) {
    return "green";
  }
  if (contains_substr_ci(message, "red") || contains_substr_ci(message, "红")) {
    return "red";
  }
  if (contains_substr_ci(message, "white") || contains_substr_ci(message, "白")) {
    return "white";
  }
  if (contains_substr_ci(message, "yellow") || contains_substr_ci(message, "黄")) {
    return "yellow";
  }
  if (contains_substr_ci(message, "purple") || contains_substr_ci(message, "紫")) {
    return "purple";
  }
  if (contains_substr_ci(message, "cyan") || contains_substr_ci(message, "青")) {
    return "cyan";
  }
  if (contains_substr_ci(message, "orange") || contains_substr_ci(message, "橙")) {
    return "orange";
  }
  if (contains_substr_ci(message, "off") || contains_substr_ci(message, "关闭") ||
      contains_substr_ci(message, "关灯") || contains_substr_ci(message, "熄灭")) {
    return "off";
  }
  return NULL;
}

typedef struct {
  const char *name;
  const char *const *aliases;
  size_t alias_count;
} status_light_color_alias_t;

static const char *const color_alias_blue[] = {"blue", "蓝"};
static const char *const color_alias_green[] = {"green", "绿"};
static const char *const color_alias_red[] = {"red", "红"};
static const char *const color_alias_white[] = {"white", "白"};
static const char *const color_alias_yellow[] = {"yellow", "黄"};
static const char *const color_alias_purple[] = {"purple", "violet", "紫"};
static const char *const color_alias_cyan[] = {"cyan", "青"};
static const char *const color_alias_orange[] = {"orange", "橙"};
static const char *const color_alias_off[] = {"off", "关闭", "关灯", "熄灭"};

static const status_light_color_alias_t status_light_colors[] = {
    {"blue", color_alias_blue, sizeof(color_alias_blue) / sizeof(color_alias_blue[0])},
    {"green", color_alias_green, sizeof(color_alias_green) / sizeof(color_alias_green[0])},
    {"red", color_alias_red, sizeof(color_alias_red) / sizeof(color_alias_red[0])},
    {"white", color_alias_white, sizeof(color_alias_white) / sizeof(color_alias_white[0])},
    {"yellow", color_alias_yellow, sizeof(color_alias_yellow) / sizeof(color_alias_yellow[0])},
    {"purple", color_alias_purple, sizeof(color_alias_purple) / sizeof(color_alias_purple[0])},
    {"cyan", color_alias_cyan, sizeof(color_alias_cyan) / sizeof(color_alias_cyan[0])},
    {"orange", color_alias_orange, sizeof(color_alias_orange) / sizeof(color_alias_orange[0])},
    {"off", color_alias_off, sizeof(color_alias_off) / sizeof(color_alias_off[0])},
};

static const char *find_substr_ci_ascii(const char *haystack, const char *needle) {
  if (!haystack || !needle || !needle[0]) {
    return NULL;
  }

  size_t needle_len = strlen(needle);
  for (const char *p = haystack; *p; p++) {
    size_t i = 0;
    while (i < needle_len && p[i]) {
      unsigned char a = (unsigned char)p[i];
      unsigned char b = (unsigned char)needle[i];
      if (a < 0x80) {
        a = (unsigned char)tolower(a);
      }
      if (b < 0x80) {
        b = (unsigned char)tolower(b);
      }
      if (a != b) {
        break;
      }
      i++;
    }
    if (i == needle_len) {
      return p;
    }
  }
  return NULL;
}

static bool starts_with_ci_ascii(const char *text, const char *prefix) {
  if (!text || !prefix) {
    return false;
  }
  while (*prefix) {
    unsigned char a = (unsigned char)*text++;
    unsigned char b = (unsigned char)*prefix++;
    if (a < 0x80) {
      a = (unsigned char)tolower(a);
    }
    if (b < 0x80) {
      b = (unsigned char)tolower(b);
    }
    if (a != b) {
      return false;
    }
    if (!*text && *prefix) {
      return false;
    }
  }
  return true;
}

static int extract_status_light_color_sequence(const char *message,
                                               const char **out,
                                               size_t out_count) {
  if (!message || !out || out_count == 0) {
    return 0;
  }

  const char *cursor = message;
  int count = 0;
  while (*cursor && count < (int)out_count) {
    const char *best_pos = NULL;
    const char *best_color = NULL;
    size_t best_len = 0;
    for (size_t i = 0; i < sizeof(status_light_colors) / sizeof(status_light_colors[0]); i++) {
      const status_light_color_alias_t *color = &status_light_colors[i];
      for (size_t j = 0; j < color->alias_count; j++) {
        const char *alias = color->aliases[j];
        const char *pos = find_substr_ci_ascii(cursor, alias);
        if (pos && (!best_pos || pos < best_pos)) {
          best_pos = pos;
          best_color = color->name;
          best_len = strlen(alias);
        }
      }
    }
    if (!best_pos || !best_color || best_len == 0) {
      break;
    }
    if (count == 0 || strcmp(out[count - 1], best_color) != 0) {
      out[count++] = best_color;
    }
    cursor = best_pos + best_len;
  }
  return count;
}

static bool message_has_sequence_marker(const char *message) {
  static const char *const keywords[] = {
      "then", "after", "wait", "sequence", "workflow", "first", "next",
      "先", "再", "然后", "之后", "以后", "后", "等待", "延迟", "过", "秒", "分钟",
      "闪", "切换", "变成", "改为", "再亮",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static uint32_t parse_light_sequence_delay_ms(const char *message) {
  if (!message) {
    return 1000;
  }

  for (const char *p = message; *p; p++) {
    if (!isdigit((unsigned char)*p)) {
      continue;
    }
    unsigned value = 0;
    const char *q = p;
    while (isdigit((unsigned char)*q)) {
      value = value * 10U + (unsigned)(*q - '0');
      q++;
    }
    while (*q && isspace((unsigned char)*q)) {
      q++;
    }
    if (strncmp(q, "秒", strlen("秒")) == 0 ||
        *q == 's' || *q == 'S' || starts_with_ci_ascii(q, "sec") ||
        starts_with_ci_ascii(q, "second")) {
      if (value == 0) {
        return 1000;
      }
      if (value > 600) {
        value = 600;
      }
      return value * 1000U;
    }
    if (strncmp(q, "分钟", strlen("分钟")) == 0 ||
        strncmp(q, "分", strlen("分")) == 0 ||
        *q == 'm' || *q == 'M' || starts_with_ci_ascii(q, "minute")) {
      if (value == 0) {
        return 60000;
      }
      if (value > 30) {
        value = 30;
      }
      return value * 60000U;
    }
    p = q - 1;
  }

  if (contains_substr_ci(message, "十秒")) {
    return 10000;
  }
  if (contains_substr_ci(message, "一秒")) {
    return 1000;
  }
  if (contains_substr_ci(message, "两秒") || contains_substr_ci(message, "二秒")) {
    return 2000;
  }
  if (contains_substr_ci(message, "三秒")) {
    return 3000;
  }
  return 1000;
}

static bool try_execute_deterministic_light_workflow(const espagent_msg_t *msg,
                                                     char *tool_output,
                                                     size_t tool_output_size,
                                                     char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      !tool_guard_match_light_request(msg->content) ||
      !message_has_sequence_marker(msg->content)) {
    return false;
  }

  const char *colors[ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS] = {0};
  int color_count = extract_status_light_color_sequence(
      msg->content, colors, ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS);
  if (color_count < 2) {
    return false;
  }

  uint32_t delay_ms = parse_light_sequence_delay_ms(msg->content);
  cJSON *root = cJSON_CreateObject();
  cJSON *steps = cJSON_CreateArray();
  if (!root || !steps) {
    cJSON_Delete(root);
    cJSON_Delete(steps);
    return false;
  }

  cJSON_AddStringToObject(root, "name", "light_sequence");
  cJSON_AddItemToObject(root, "steps", steps);
  for (int i = 0; i < color_count; i++) {
    cJSON *step = cJSON_CreateObject();
    cJSON *args = cJSON_CreateObject();
    if (!step || !args) {
      cJSON_Delete(step);
      cJSON_Delete(args);
      cJSON_Delete(root);
      return false;
    }
    cJSON_AddNumberToObject(step, "delay_ms", i == 0 ? 0 : (double)delay_ms);
    cJSON_AddStringToObject(step, "target_role", "control_agent");
    cJSON_AddStringToObject(step, "action", "set_status_light");
    cJSON_AddStringToObject(args, "color", colors[i]);
    cJSON_AddItemToObject(step, "args", args);
    cJSON_AddItemToArray(steps, step);
  }

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!payload) {
    return false;
  }

  tool_output[0] = '\0';
  tool_registry_execute("automation_create_workflow", payload, tool_output, tool_output_size);
  cJSON_free(payload);
  ESP_LOGI(TAG, "=== CONV === Deterministic workflow route => %s", tool_output);

  char reply_buf[512];
  if (strncmp(tool_output, "OK:", 3) == 0) {
    snprintf(reply_buf, sizeof(reply_buf),
             "已创建灯光多步流程：先设置为%s，随后按顺序切换，共%d步。%s",
             colors[0], color_count, tool_output);
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "灯光多步流程创建失败：%s", tool_output);
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool is_mesh_related_tool_name(const char *name) {
  return name &&
         (strcmp(name, "mesh_send_command") == 0 ||
          strcmp(name, "read_temperature_humidity") == 0 ||
          strcmp(name, "set_status_light") == 0 ||
          strcmp(name, "ws2812_set") == 0 ||
          strcmp(name, "gpio_write") == 0 ||
          strcmp(name, "servo_write") == 0);
}

static bool try_execute_deterministic_mesh_request(const espagent_msg_t *msg,
                                                   char *tool_output,
                                                   size_t tool_output_size,
                                                   char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content)) {
    return false;
  }

  char reply_buf[512];
  cJSON *payload_root = NULL;

  if (tool_guard_match_temperature_humidity_request(msg->content) &&
      !tool_guard_match_weather_request(msg->content)) {
    payload_root = cJSON_CreateObject();
    if (payload_root) {
      cJSON_AddStringToObject(payload_root, "target_role", "sensor_agent");
      cJSON_AddStringToObject(payload_root, "action", "read_temperature_humidity");
      cJSON_AddItemToObject(payload_root, "args", cJSON_CreateObject());
    }
  } else if (tool_guard_match_light_request(msg->content)) {
    const char *color = detect_status_light_color(msg->content);
    if (!color) {
      return false;
    }
    payload_root = cJSON_CreateObject();
    if (payload_root) {
      cJSON_AddStringToObject(payload_root, "target_role", "control_agent");
      cJSON_AddStringToObject(payload_root, "action", "set_status_light");
      cJSON *args = cJSON_CreateObject();
      if (args) {
        cJSON_AddStringToObject(args, "color", color);
        cJSON_AddItemToObject(payload_root, "args", args);
      }
    }
  } else {
    return false;
  }

  if (!payload_root) {
    return false;
  }
  cJSON_AddBoolToObject(payload_root, "async", true);
  cJSON_AddStringToObject(payload_root, "reply_channel", msg->channel);
  cJSON_AddStringToObject(payload_root, "reply_chat_id", msg->chat_id);

  char *payload = cJSON_PrintUnformatted(payload_root);
  cJSON_Delete(payload_root);
  if (!payload) {
    return false;
  }

  tool_output[0] = '\0';
  tool_registry_execute("mesh_send_command", payload, tool_output, tool_output_size);
  cJSON_free(payload);
  ESP_LOGI(TAG, "=== CONV === Deterministic Mesh route => %s", tool_output);

  if (strncmp(tool_output, "OK:", 3) == 0) {
    snprintf(reply_buf, sizeof(reply_buf), "已通过 MQTT Mesh 下发命令：%s", tool_output);
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "MQTT Mesh 命令下发失败：%s", tool_output);
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool tool_guard_check(const llm_tool_call_t *call, const espagent_msg_t *msg,
                             char *output, size_t output_size) {
  if (!call || !msg || !msg->content || call->name[0] == '\0') {
    return true;
  }

  const char *tool_name = call->name;
  const char *message = msg->content;
  bool allowed = true;
  const char *expected = NULL;

  if (strcmp(tool_name, "read_environment") == 0) {
    allowed = tool_guard_match_environment_request(message);
    expected = "combined AHT20/SGP30/GY-30 environment sensor reading";
  } else if (strcmp(tool_name, "read_light_level") == 0) {
    allowed = tool_guard_match_light_sensor_request(message);
    expected =
        "ambient-light, illuminance, lux, or GY-30/BH1750 sensor reading";
  } else if (strcmp(tool_name, "set_status_light") == 0 ||
             strcmp(tool_name, "ws2812_set") == 0) {
    allowed = tool_guard_match_light_request(message);
    expected = "board light or LED control";
  } else if (strcmp(tool_name, "servo_write") == 0) {
    allowed = tool_guard_match_servo_request(message);
    expected = "servo angle or servo pulse-width control";
  } else if (strcmp(tool_name, "read_air_quality") == 0 ||
             strcmp(tool_name, "sgp30_read_air_quality") == 0) {
    allowed = tool_guard_match_air_quality_request(message);
    expected = "air-quality or gas-sensor reading";
  } else if (strcmp(tool_name, "read_presence") == 0 ||
             strcmp(tool_name, "hc_sr05_read_distance") == 0) {
    allowed = tool_guard_match_presence_request(message);
    expected = "HC-SR05 presence, proximity, or distance reading";
  } else if (strcmp(tool_name, "gpio_write") == 0) {
    allowed = tool_guard_match_gpio_write_request(message);
    expected = "explicit GPIO or digital output control";
  } else if (strcmp(tool_name, "gpio_read") == 0 ||
             strcmp(tool_name, "gpio_read_all") == 0) {
    allowed = tool_guard_match_gpio_read_request(message);
    expected = "explicit GPIO, button, switch, or input-state reading";
  } else if (strcmp(tool_name, "cron_add") == 0 ||
             strcmp(tool_name, "cron_list") == 0 ||
             strcmp(tool_name, "cron_remove") == 0) {
    allowed = tool_guard_match_cron_request(message);
    expected = "scheduling or reminder management";
  } else if (strcmp(tool_name, "get_weather") == 0) {
    allowed = tool_guard_match_weather_request(message);
    expected = "weather, temperature, forecast, rain, wind, or clothing advice";
  }

  if (allowed) {
    return true;
  }

  snprintf(
      output, output_size,
      "Guard blocked tool '%s': the user's request does not clearly ask for "
      "%s. "
      "Do not substitute a nearby capability. Reply that this capability is "
      "not currently supported, "
      "or ask a brief clarification question if the user intent is ambiguous.",
      tool_name, expected ? expected : "this tool");
  ESP_LOGW(TAG, "Tool guard blocked %s for message: %s", tool_name, message);
  return false;
}

/* Build the assistant content array from llm_response_t for the messages
 * history. Returns a cJSON array with text and tool_use blocks. */
static cJSON *build_assistant_content(const llm_response_t *resp) {
  cJSON *content = cJSON_CreateArray();

  /* Text block */
  if (resp->text && resp->text_len > 0) {
    cJSON *text_block = cJSON_CreateObject();
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", resp->text);
    cJSON_AddItemToArray(content, text_block);
  }

  /* Tool use blocks */
  for (int i = 0; i < resp->call_count; i++) {
    const llm_tool_call_t *call = &resp->calls[i];
    cJSON *tool_block = cJSON_CreateObject();
    cJSON_AddStringToObject(tool_block, "type", "tool_use");
    cJSON_AddStringToObject(tool_block, "id", call->id);
    cJSON_AddStringToObject(tool_block, "name", call->name);

    cJSON *input = cJSON_Parse(call->input);
    if (input) {
      cJSON_AddItemToObject(tool_block, "input", input);
    } else {
      cJSON_AddItemToObject(tool_block, "input", cJSON_CreateObject());
    }

    cJSON_AddItemToArray(content, tool_block);
  }

  return content;
}

static void json_set_string(cJSON *obj, const char *key, const char *value) {
  if (!obj || !key || !value) {
    return;
  }
  cJSON_DeleteItemFromObject(obj, key);
  cJSON_AddStringToObject(obj, key, value);
}

static void append_turn_context_prompt(char *prompt, size_t size,
                                       const espagent_msg_t *msg) {
  if (!prompt || size == 0 || !msg) {
    return;
  }

  size_t off = strnlen(prompt, size - 1);
  if (off >= size - 1) {
    return;
  }

  int n = snprintf(prompt + off, size - off,
                   "\n## Current Turn Context\n"
                   "- source_channel: %s\n"
                   "- source_chat_id: %s\n",
                   msg->channel[0] ? msg->channel : "(unknown)",
                   msg->chat_id[0] ? msg->chat_id : "(empty)");

  if (n < 0 || (size_t)n >= (size - off)) {
    terminate_at_utf8_boundary(prompt, size - 1);
  }
}

static size_t append_prompt_format(char *prompt, size_t size, const char *fmt,
                                   ...) {
  if (!prompt || size == 0) {
    return 0;
  }

  size_t off = strnlen(prompt, size - 1);
  if (off >= size - 1) {
    prompt[size - 1] = '\0';
    return off;
  }

  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(prompt + off, size - off, fmt, ap);
  va_end(ap);

  if (n < 0 || (size_t)n >= size - off) {
    return terminate_at_utf8_boundary(prompt, size - 1);
  }
  return off + (size_t)n;
}

static bool text_looks_like_question(const char *text) {
  if (!text || text[0] == '\0') {
    return false;
  }

  if (strstr(text, "?") || strstr(text, "？")) {
    return true;
  }

  static const char *const markers[] = {
      "吗", "么", "哪", "哪个", "哪些", "是否", "要不要",
      "还是", "请问", "确认", "选择", "需要我",
  };
  return message_has_any_keyword(text, markers,
                                 sizeof(markers) / sizeof(markers[0]));
}

static void copy_compact_text(char *out, size_t out_size, const char *text) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!text) {
    return;
  }

  size_t j = 0;
  bool last_space = false;
  for (size_t i = 0; text[i] && j + 1 < out_size; i++) {
    char ch = text[i];
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      ch = ' ';
    }
    if (ch == ' ') {
      if (last_space) {
        continue;
      }
      last_space = true;
    } else {
      last_space = false;
    }
    out[j++] = ch;
  }
  out[j] = '\0';
}

static bool history_last_assistant_question(const char *history_json, char *out,
                                            size_t out_size) {
  if (!history_json || !out || out_size == 0) {
    return false;
  }
  out[0] = '\0';

  cJSON *messages = cJSON_Parse(history_json);
  if (!messages || !cJSON_IsArray(messages)) {
    cJSON_Delete(messages);
    return false;
  }

  bool found = false;
  int count = cJSON_GetArraySize(messages);
  for (int i = count - 1; i >= 0; i--) {
    cJSON *msg = cJSON_GetArrayItem(messages, i);
    cJSON *role = cJSON_GetObjectItem(msg, "role");
    if (!cJSON_IsString(role) || strcmp(role->valuestring, "assistant") != 0) {
      continue;
    }

    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (!cJSON_IsString(content) || !content->valuestring) {
      break;
    }

    if (text_looks_like_question(content->valuestring)) {
      copy_compact_text(out, out_size, content->valuestring);
      found = out[0] != '\0';
    }
    break;
  }

  cJSON_Delete(messages);
  return found;
}

static void append_pending_clarification_prompt(char *prompt, size_t size,
                                                const char *history_json) {
  char question[384];
  if (!history_last_assistant_question(history_json, question,
                                       sizeof(question))) {
    return;
  }

  append_prompt_format(
      prompt, size,
      "\n## Pending Clarification\n"
      "The previous assistant message appears to be a clarification question:\n"
      "\"%s\"\n"
      "If the current user message is relevant, treat it as the user's answer "
      "to that question and continue the pending task. Do not restart the task "
      "unless the user clearly changes topic.\n",
      question);
}

static char *patch_tool_input_with_context(const llm_tool_call_t *call,
                                           const espagent_msg_t *msg) {
  if (!call || !msg ||
      (strcmp(call->name, "cron_add") != 0 &&
       strcmp(call->name, "mesh_send_command") != 0)) {
    return NULL;
  }

  cJSON *root = cJSON_Parse(call->input ? call->input : "{}");
  if (!root || !cJSON_IsObject(root)) {
    cJSON_Delete(root);
    root = cJSON_CreateObject();
  }
  if (!root) {
    return NULL;
  }

  bool changed = false;

  if (strcmp(call->name, "mesh_send_command") == 0) {
    cJSON *async_item = cJSON_GetObjectItem(root, "async");
    if (!async_item) {
      cJSON_AddBoolToObject(root, "async", true);
      changed = true;
    }
    cJSON *reply_channel = cJSON_GetObjectItem(root, "reply_channel");
    if (!cJSON_IsString(reply_channel) && msg->channel[0] != '\0') {
      json_set_string(root, "reply_channel", msg->channel);
      changed = true;
    }
    cJSON *reply_chat = cJSON_GetObjectItem(root, "reply_chat_id");
    if (!cJSON_IsString(reply_chat) && msg->chat_id[0] != '\0') {
      json_set_string(root, "reply_chat_id", msg->chat_id);
      changed = true;
    }

    char *patched = NULL;
    if (changed) {
      patched = cJSON_PrintUnformatted(root);
      if (patched) {
        ESP_LOGI(TAG, "Patched mesh_send_command reply target to %s:%s",
                 msg->channel, msg->chat_id);
      }
    }
    cJSON_Delete(root);
    return patched;
  }

  cJSON *channel_item = cJSON_GetObjectItem(root, "channel");
  const char *channel =
      cJSON_IsString(channel_item) ? channel_item->valuestring : NULL;

  if ((!channel || channel[0] == '\0') && msg->channel[0] != '\0') {
    json_set_string(root, "channel", msg->channel);
    channel = msg->channel;
    changed = true;
  }

  if (channel && strcmp(channel, msg->channel) == 0 &&
      msg->chat_id[0] != '\0') {
    cJSON *chat_item = cJSON_GetObjectItem(root, "chat_id");
    const char *chat_id =
        cJSON_IsString(chat_item) ? chat_item->valuestring : NULL;
    if (!chat_id || chat_id[0] == '\0' || strcmp(chat_id, "cron") == 0) {
      json_set_string(root, "chat_id", msg->chat_id);
      changed = true;
    }
  }

  char *patched = NULL;
  if (changed) {
    patched = cJSON_PrintUnformatted(root);
    if (patched) {
      ESP_LOGI(TAG, "Patched cron_add target to %s:%s", msg->channel,
               msg->chat_id);
    }
  }
  cJSON_Delete(root);
  return patched;
}

static void publish_tool_timeline(const llm_tool_call_t *call,
                                  const char *tool_input,
                                  const char *event_type,
                                  const char *status,
                                  const char *result)
{
  char summary[192] = {0};
  char command_id[64] = {0};
  char target_role[64] = {0};
  char target_node[64] = {0};
  char action[64] = {0};

  snprintf(summary, sizeof(summary), "%s %s",
           event_type ? event_type : "tool",
           call ? call->name : "(unknown)");
  if (result && result[0]) {
    size_t len = strlen(result);
    snprintf(summary, sizeof(summary), "%s: %.*s%s",
             call ? call->name : "tool",
             (int)(len > 96 ? 96 : len), result, len > 96 ? "..." : "");
  }

  if (call && tool_input && strcmp(call->name, "mesh_send_command") == 0) {
    cJSON *root = cJSON_Parse(tool_input);
    if (root && cJSON_IsObject(root)) {
      cJSON *item = cJSON_GetObjectItem(root, "command_id");
      if (cJSON_IsString(item)) {
        snprintf(command_id, sizeof(command_id), "%s", item->valuestring);
      }
      item = cJSON_GetObjectItem(root, "target_role");
      if (cJSON_IsString(item)) {
        snprintf(target_role, sizeof(target_role), "%s", item->valuestring);
      }
      item = cJSON_GetObjectItem(root, "target_node");
      if (cJSON_IsString(item)) {
        snprintf(target_node, sizeof(target_node), "%s", item->valuestring);
      }
      item = cJSON_GetObjectItem(root, "action");
      if (cJSON_IsString(item)) {
        snprintf(action, sizeof(action), "%s", item->valuestring);
      }
    }
    cJSON_Delete(root);
  }

  (void)sensor_mqtt_publish_timeline_event(
      strcmp(event_type ? event_type : "", "tool_use") == 0 ? "reasoning" : "result",
      event_type,
      status,
      summary,
      command_id,
      target_role,
      target_node,
      action);
}

static void compact_for_trace(const char *in, char *out, size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  if (!in) {
    return;
  }
  size_t len = strlen(in);
  size_t copy = len > out_size - 1 ? out_size - 1 : len;
  memcpy(out, in, copy);
  out[copy] = '\0';
  terminate_at_utf8_boundary(out, strlen(out));
}

static void append_tool_trace(const espagent_msg_t *msg,
                              const char *event_type,
                              const llm_tool_call_t *call,
                              const char *payload) {
  if (!msg || !event_type || !call) {
    return;
  }

  char compact[512] = {0};
  compact_for_trace(payload, compact, sizeof(compact));

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    return;
  }
  cJSON_AddStringToObject(root, "event", event_type);
  cJSON_AddStringToObject(root, "tool_call_id", call->id);
  cJSON_AddStringToObject(root, "tool", call->name);
  cJSON_AddStringToObject(root, "payload", compact);
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    return;
  }

  char summary[128] = {0};
  snprintf(summary, sizeof(summary), "%s %s", event_type, call->name);
  (void)session_append_trace(msg->chat_id, event_type, summary, json);
  cJSON_free(json);
}

static void publish_local_tool_output(const llm_tool_call_t *call,
                                      const char *tool_output,
                                      esp_err_t result_err) {
  if (!call) {
    return;
  }

  char summary[192] = {0};
  char result_text[384] = {0};
  compact_for_trace(tool_output, result_text, sizeof(result_text));
  snprintf(summary, sizeof(summary), "%s: %s",
           call->name,
           result_err == ESP_OK ? "local tool result" : "local tool error");
  (void)sensor_mqtt_publish_output_message("tool_result",
                                           NULL,
                                           NULL,
                                           call->name,
                                           "coordinator_agent",
                                           result_err,
                                           summary,
                                           result_text);
}

/* Build the user message with tool_result blocks */
static cJSON *build_tool_results(const llm_response_t *resp,
                                 const espagent_msg_t *msg, char *tool_output,
                                 size_t tool_output_size, char *tool_fallback,
                                 size_t tool_fallback_size) {
  cJSON *content = cJSON_CreateArray();

  for (int i = 0; i < resp->call_count; i++) {
    const llm_tool_call_t *call = &resp->calls[i];
    const char *tool_input = call->input ? call->input : "{}";
    char *patched_input = patch_tool_input_with_context(call, msg);
    if (patched_input) {
      tool_input = patched_input;
    }

    publish_tool_timeline(call, tool_input, "tool_use", "pending", NULL);
    append_tool_trace(msg, "tool_use", call, tool_input);

    tool_output[0] = '\0';
    if (!tool_guard_check(call, msg, tool_output, tool_output_size)) {
      ESP_LOGI(TAG, "=== CONV === Tool[%s] => %s", call->name, tool_output);
      publish_tool_timeline(call, tool_input, "tool_result", "blocked",
                            tool_output);
      append_tool_trace(msg, "tool_result", call, tool_output);
      publish_local_tool_output(call, tool_output, ESP_ERR_INVALID_STATE);
      free(patched_input);

      cJSON *result_block = cJSON_CreateObject();
      cJSON_AddStringToObject(result_block, "type", "tool_result");
      cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
      cJSON_AddStringToObject(result_block, "content", tool_output);
      cJSON_AddItemToArray(content, result_block);
      continue;
    }

    /* Execute tool */
    tool_registry_execute(call->name, tool_input, tool_output,
                          tool_output_size);

    ESP_LOGI(TAG, "=== CONV === Tool[%s] => %s", call->name, tool_output);
    const bool tool_error = strncmp(tool_output, "Error:", 6) == 0;
    publish_tool_timeline(call, tool_input, "tool_result",
                          tool_error ? "error" : "ok", tool_output);
    append_tool_trace(msg, "tool_result", call, tool_output);
    publish_local_tool_output(call, tool_output,
                              tool_error ? ESP_FAIL : ESP_OK);
    free(patched_input);

    if (tool_fallback && tool_fallback_size > 0 &&
        strcmp(call->name, "web_search") == 0 && tool_output[0] != '\0') {
      snprintf(tool_fallback, tool_fallback_size,
               "我已完成联网搜索，但整理结果时遇到临时错误。先把搜索结果发给你：\n\n%s",
               tool_output);
    }

    /* Build tool_result block */
    cJSON *result_block = cJSON_CreateObject();
    cJSON_AddStringToObject(result_block, "type", "tool_result");
    cJSON_AddStringToObject(result_block, "tool_use_id", call->id);
    cJSON_AddStringToObject(result_block, "content", tool_output);
    cJSON_AddItemToArray(content, result_block);
  }

  return content;
}

static void agent_loop_task(void *arg) {
  ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

  /* Allocate large buffers from PSRAM */
  char *system_prompt =
      heap_caps_calloc(1, ESPAGENT_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
  char *history_json =
      heap_caps_calloc(1, ESPAGENT_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
  char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);
  char *tool_fallback =
      heap_caps_calloc(1, TOOL_OUTPUT_SIZE + 512, MALLOC_CAP_SPIRAM);

  if (!system_prompt || !history_json || !tool_output || !tool_fallback) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
    vTaskDelete(NULL);
    return;
  }

  const char *tools_json = tool_registry_get_tools_json();

  while (1) {
    espagent_msg_t msg;
    esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
    if (err != ESP_OK)
      continue;

    ESP_LOGI(TAG, "Processing message from %s:%s", msg.channel, msg.chat_id);
    ESP_LOGI(TAG,
             "=== CONV ==================================================");
    ESP_LOGI(TAG, "=== CONV === [%s/%s] >> USER: %s", msg.channel, msg.chat_id,
             msg.content);

    bool proactive_turn = (msg.flags & ESPAGENT_MSG_FLAG_PROACTIVE) != 0;
    bool internal_result_turn =
        (msg.flags & ESPAGENT_MSG_FLAG_INTERNAL_RESULT) != 0;
    if (!proactive_turn && !internal_result_turn) {
      proactive_service_note_contact(msg.channel, msg.chat_id);
    }

    /* 1. Build system prompt */
    context_build_system_prompt(system_prompt, ESPAGENT_CONTEXT_BUF_SIZE);
    append_turn_context_prompt(system_prompt, ESPAGENT_CONTEXT_BUF_SIZE, &msg);
    ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel,
             msg.chat_id);

    /* 2. Load session history into cJSON array */
    session_get_history_json(msg.chat_id, history_json,
                             ESPAGENT_LLM_STREAM_BUF_SIZE, ESPAGENT_AGENT_MAX_HISTORY);
    append_pending_clarification_prompt(system_prompt,
                                        ESPAGENT_CONTEXT_BUF_SIZE,
                                        history_json);

    cJSON *messages = cJSON_Parse(history_json);
    if (!messages)
      messages = cJSON_CreateArray();

    /* 3. Append current user message */
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", msg.content);
    cJSON_AddItemToArray(messages, user_msg);

    /* 4. ReAct loop */
    char *final_text = NULL;
    int iteration = 0;
    bool sent_working_status = false;
    bool mesh_related_tool_seen = false;
    tool_fallback[0] = '\0';

    if (!try_execute_deterministic_light_workflow(&msg, tool_output,
                                                  TOOL_OUTPUT_SIZE,
                                                  &final_text)) {
      try_execute_deterministic_mesh_request(&msg, tool_output, TOOL_OUTPUT_SIZE,
                                             &final_text);
    }

    while (!final_text && iteration < ESPAGENT_AGENT_MAX_TOOL_ITER) {
      /* Send "working" indicator before each API call */
#if ESPAGENT_AGENT_SEND_WORKING_STATUS
      if (!proactive_turn && !sent_working_status &&
          strcmp(msg.channel, ESPAGENT_CHAN_SYSTEM) != 0) {
        espagent_msg_t status = {0};
        strncpy(status.channel, msg.channel, sizeof(status.channel) - 1);
        strncpy(status.chat_id, msg.chat_id, sizeof(status.chat_id) - 1);
        status.content = strdup("ESPAgent is processing your request...");
        if (status.content) {
          if (message_bus_push_outbound(&status) != ESP_OK) {
            ESP_LOGW(TAG, "Outbound queue full, drop working status");
            free(status.content);
          } else {
            sent_working_status = true;
          }
        }
      }
#endif

      llm_response_t resp;
      err = llm_chat_tools(system_prompt, messages, tools_json, &resp);

      if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
        break;
      }

      if (!resp.tool_use) {
        /* Normal completion — save final text and break */
        if (resp.text && resp.text_len > 0) {
          final_text = strdup(resp.text);
          ESP_LOGI(TAG, "=== CONV === << LLM: %.*s", (int)resp.text_len,
                   resp.text);
          if (!mesh_related_tool_seen && final_text &&
              final_text_is_unexecuted_mesh_claim(final_text, &msg)) {
            ESP_LOGW(TAG,
                     "LLM claimed Mesh dispatch without a tool call; replacing "
                     "final response");
            free(final_text);
            final_text = strdup("我没有拿到实际的 MQTT Mesh 工具执行结果，不能声称已经下发。请再发一次明确命令，例如：读取温湿度，或：把控制板状态灯设为蓝色。");
          }
        }
        llm_response_free(&resp);
        break;
      }

      ESP_LOGI(TAG, "Tool use iteration %d: %d calls", iteration + 1,
               resp.call_count);
      for (int ci = 0; ci < resp.call_count; ci++) {
        ESP_LOGI(TAG, "=== CONV === << LLM tool[%d]: %s(%s)", ci,
                 resp.calls[ci].name,
                 resp.calls[ci].input ? resp.calls[ci].input : "{}");
        if (is_mesh_related_tool_name(resp.calls[ci].name)) {
          mesh_related_tool_seen = true;
        }
      }

      /* Append assistant message with content array */
      cJSON *asst_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(asst_msg, "role", "assistant");
      cJSON_AddItemToObject(asst_msg, "content",
                            build_assistant_content(&resp));
      cJSON_AddItemToArray(messages, asst_msg);

      /* Execute tools and append results */
      cJSON *tool_results =
          build_tool_results(&resp, &msg, tool_output, TOOL_OUTPUT_SIZE,
                             tool_fallback, TOOL_OUTPUT_SIZE + 512);
      cJSON *result_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(result_msg, "role", "user");
      cJSON_AddItemToObject(result_msg, "content", tool_results);
      cJSON_AddItemToArray(messages, result_msg);

      llm_response_free(&resp);
      iteration++;
    }

    cJSON_Delete(messages);

    /* 5. Send response */
    if (final_text && final_text[0]) {
      if (proactive_turn && text_is_proactive_no_message(final_text)) {
        ESP_LOGI(TAG, "Proactive check chose no message");
        free(final_text);
        final_text = NULL;
        free(msg.content);
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        continue;
      }

      /* Save to session (only user text + final assistant text) */
      esp_err_t save_user =
          (proactive_turn || internal_result_turn) ? ESP_OK : session_append(msg.chat_id, "user", msg.content);
      esp_err_t save_asst = session_append(msg.chat_id, "assistant", final_text);
      if (internal_result_turn) {
        (void)session_append_trace(msg.chat_id, "async_result_input",
                                   "Async Mesh result injected", msg.content);
      }
      if (save_user != ESP_OK || save_asst != ESP_OK) {
        ESP_LOGW(TAG, "Session save failed for chat %s (user=%s, assistant=%s)",
                 msg.chat_id, esp_err_to_name(save_user),
                 esp_err_to_name(save_asst));
      } else {
        ESP_LOGI(TAG, "Session saved for chat %s", msg.chat_id);
      }

      /* Push response to outbound */
      espagent_msg_t out = {0};
      strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
      strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
      out.content = final_text; /* transfer ownership */
      ESP_LOGI(TAG, "Queue final response to %s:%s (%d bytes)", out.channel,
               out.chat_id, (int)strlen(final_text));
      ESP_LOGI(TAG, "=== CONV === >> RESPONSE [%s/%s]: %s", out.channel,
               out.chat_id, out.content);
      (void)sensor_mqtt_publish_timeline_event("final", "final_reply", "ok",
                                               out.content, NULL, NULL, NULL,
                                               NULL);
      (void)sensor_mqtt_publish_output_message("final_reply",
                                               NULL,
                                               NULL,
                                               "final_reply",
                                               msg.channel,
                                               ESP_OK,
                                               out.content,
                                               out.content);
      (void)session_append_trace(msg.chat_id, "final_reply",
                                 "Assistant final reply", out.content);
      if (message_bus_push_outbound(&out) != ESP_OK) {
        ESP_LOGW(TAG, "Outbound queue full, drop final response");
        free(final_text);
      } else {
        final_text = NULL;
      }
    } else {
      /* Error or empty response */
      free(final_text);
      espagent_msg_t out = {0};
      strncpy(out.channel, msg.channel, sizeof(out.channel) - 1);
      strncpy(out.chat_id, msg.chat_id, sizeof(out.chat_id) - 1);
      out.content = strdup(tool_fallback[0] ? tool_fallback
                                            : "抱歉，我这次处理请求时遇到了错误。");
      if (out.content) {
        (void)sensor_mqtt_publish_timeline_event("final", "error", "error",
                                                 out.content, NULL, NULL, NULL,
                                                 NULL);
        (void)sensor_mqtt_publish_output_message("final_reply",
                                                 NULL,
                                                 NULL,
                                                 "final_reply",
                                                 msg.channel,
                                                 ESP_FAIL,
                                                 out.content,
                                                 out.content);
        (void)session_append_trace(msg.chat_id, "final_error",
                                   "Assistant error reply", out.content);
        if (message_bus_push_outbound(&out) != ESP_OK) {
          ESP_LOGW(TAG, "Outbound queue full, drop error response");
          free(out.content);
        }
      }
    }

    /* Free inbound message content */
    free(msg.content);

    /* Log memory status */
    ESP_LOGI(TAG, "Free PSRAM: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
}

esp_err_t agent_loop_init(void) {
  ESP_LOGI(TAG, "Agent loop initialized");
  return ESP_OK;
}

esp_err_t agent_loop_start(void) {
  const uint32_t stack_candidates[] = {
      ESPAGENT_AGENT_STACK, 20 * 1024, 16 * 1024, 14 * 1024, 12 * 1024,
  };

  for (size_t i = 0;
       i < (sizeof(stack_candidates) / sizeof(stack_candidates[0])); i++) {
    uint32_t stack_size = stack_candidates[i];
    BaseType_t ret =
        xTaskCreatePinnedToCore(agent_loop_task, "agent_loop", stack_size, NULL,
                                ESPAGENT_AGENT_PRIO, NULL, ESPAGENT_AGENT_CORE);

    if (ret == pdPASS) {
      ESP_LOGI(TAG, "agent_loop task created with stack=%u bytes",
               (unsigned)stack_size);
      return ESP_OK;
    }

    ESP_LOGW(TAG,
             "agent_loop create failed (stack=%u, free_internal=%u, "
             "largest_internal=%u), retrying...",
             (unsigned)stack_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }

  return ESP_FAIL;
}

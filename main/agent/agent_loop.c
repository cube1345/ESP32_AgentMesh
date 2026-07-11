#include "agent_loop.h"
#include "agent/context_builder.h"
#include "agent/slash_command.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "memory/memory_store.h"
#include "memory/memory_v2.h"
#include "memory/session_mgr.h"
#include "net/net_guard.h"
#include "proactive/proactive_service.h"
#include "roles/role_config.h"
#include "sensors/sensor_mqtt.h"
#include "skills/skill_loader.h"
#include "espagent_config.h"
#include "tools/tool_gpio.h"
#include "tools/tool_registry.h"
#include "voice/voice_bridge.h"

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
#include <time.h>

static const char *TAG = "agent";

#define TOOL_OUTPUT_SIZE (8 * 1024)
#define TOOL_SUMMARY_SIZE 4096
#define AGENT_LLM_BACKGROUND_DEFER_MS 20000

static bool message_prefers_direct_reply_no_tools(const char *message);
static size_t append_prompt_format(char *prompt, size_t size, const char *fmt,
                                   ...);
static bool message_requests_light_turn_on_without_color(const char *message);
static bool message_explicitly_requests_subagent(const char *message);
static bool extract_explicit_subagent_task(const char *message,
                                           char *out,
                                           size_t out_size);
static const char *find_substr_ci_ascii(const char *haystack,
                                        const char *needle);
static bool starts_with_ci_ascii(const char *text, const char *prefix);

static bool agent_should_persist_trace(void) {
  return !espagent_role_is_coordinator();
}

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

static void build_lightweight_direct_reply_prompt(char *prompt,
                                                  size_t size,
                                                  const espagent_msg_t *msg) {
  if (!prompt || size == 0) {
    return;
  }

  snprintf(
      prompt, size,
      "# ESPAgent Direct Reply\n\n"
      "You are ESPAgent, a careful and concise assistant.\n"
      "This turn is a direct question-answer turn, not a device orchestration turn.\n"
      "Prioritize correctness over speed.\n"
      "For numbers, ordering, dates, units, and logic questions, reason carefully before answering.\n"
      "Keep the final reply concise, but do not sacrifice correctness.\n"
      "If the user asks for only the final answer, return only the final answer.\n"
      "Do not mention tools, mesh, policy, skills, memory, or hardware unless the user explicitly asks about them.\n"
      "If you are uncertain, say so plainly instead of guessing.\n");

  if (msg && msg->channel[0]) {
    append_prompt_format(prompt, size,
                         "\nTurn channel: %s\n"
                         "Respond naturally for this chat surface.\n",
                         msg->channel);
  }
}

static void build_project_explanation_prompt(char *prompt, size_t size,
                                             const espagent_msg_t *msg) {
  if (!prompt || size == 0) {
    return;
  }

  snprintf(
      prompt, size,
      "# ESPAgent Project Explanation\n\n"
      "You are ESPAgent, explaining the current project to a developer or operator.\n"
      "This turn is a project explanation turn, not a live hardware orchestration turn.\n"
      "Answer concretely and structurally, but keep the response concise.\n"
      "Focus on architecture, node roles, data flow, runtime behavior, prompt routing, tools, memory, skills, gateway, and integration boundaries when relevant.\n"
      "Do not pretend a hardware action, MQTT command, mesh dispatch, or tool execution already happened unless the current turn explicitly contains a real execution result.\n"
      "If the question is about current implementation state, answer conservatively and distinguish code behavior from future design intent.\n"
      "Do not mention policy, mesh, skills, or hardware unless they help answer the user's question.\n"
      "If the user asks for comparison, reasoning, or explanation, prioritize correctness and clarity over speed.\n\n"
      "## Current Node Identity\n"
      "Node ID: " ESPAGENT_NODE_ID "\n"
      "Node role: " ESPAGENT_NODE_ROLE "\n"
      "Node capabilities: " ESPAGENT_NODE_CAPABILITIES "\n"
      "Node responsibilities: " ESPAGENT_NODE_RESPONSIBILITIES "\n");

  if (msg && msg->channel[0]) {
    append_prompt_format(prompt, size,
                         "\nTurn channel: %s\n"
                         "Respond naturally for this chat surface.\n",
                         msg->channel);
  }
}

static void build_tight_coordinator_prompt(char *prompt, size_t size,
                                           const espagent_msg_t *msg) {
  if (!prompt || size == 0) {
    return;
  }

  snprintf(
      prompt, size,
      "# ESPAgent Tight Coordinator\n\n"
      "You are ESPAgent running on the coordinator node.\n"
      "The current turn is under tight memory pressure, so keep reasoning compact and tool use minimal.\n"
      "Be accurate and concise.\n"
      "Use tools only when they are necessary to complete the user's request.\n"
      "For ordinary sensing requests, prefer mesh_send_command to sensor_agent.\n"
      "For remote actuator or device control requests, prefer mesh_send_command to control_agent.\n"
      "For policy or audit requests, prefer mesh_send_command to guardian_agent.\n"
      "Never claim a tool or mesh action succeeded unless the result says it succeeded.\n"
      "If the request is ambiguous, ask one short follow-up question.\n");

  if (msg && msg->channel[0]) {
    append_prompt_format(prompt, size,
                         "\nTurn channel: %s\n"
                         "Respond naturally for this chat surface.\n",
                         msg->channel);
  }
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

static bool is_ascii_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static bool contains_ascii_word_ci(const char *haystack, const char *needle) {
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
      char prev = (p == haystack) ? '\0' : p[-1];
      char next = p[i];
      if (!is_ascii_word_char(prev) && !is_ascii_word_char(next)) {
        return true;
      }
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

static void build_relevance_query(const char *input, char *out,
                                  size_t out_size) {
  if (!out || out_size == 0) {
    return;
  }
  out[0] = '\0';
  snprintf(out, out_size, "%s", input ? input : "");

  const char *suffix = "";
  if (espagent_role_is_sensor()) {
    suffix =
        " sensor telemetry temperature humidity air quality light presence i2c uart adc";
  } else if (espagent_role_is_control()) {
    suffix =
        " control actuator gpio ws2812 servo relay pwm ir uart emergency stop";
  } else if (espagent_role_is_guardian()) {
    suffix =
        " guardian security policy privacy audit approval sandbox watchdog stateboard";
  } else if (espagent_role_is_coordinator()) {
    suffix = " coordinator mesh dispatch timeline gateway voice status";
  }

  size_t off = strnlen(out, out_size - 1);
  if (suffix[0] && off < out_size - 1) {
    snprintf(out + off, out_size - off, "%s", suffix);
  }
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

static bool tool_guard_match_copper_gpio_write_request(const char *message) {
  return tool_guard_match_gpio_write_request(message) &&
         (contains_substr_ci(message, "gpio4") ||
          contains_substr_ci(message, "gpio 4") ||
          contains_substr_ci(message, "io4") ||
          contains_substr_ci(message, "io 4") ||
          contains_substr_ci(message, "gpio5") ||
          contains_substr_ci(message, "gpio 5") ||
          contains_substr_ci(message, "io5") ||
          contains_substr_ci(message, "io 5") ||
          contains_substr_ci(message, "gpio6") ||
          contains_substr_ci(message, "gpio 6") ||
          contains_substr_ci(message, "io6") ||
          contains_substr_ci(message, "io 6"));
}

static bool tool_guard_match_fixed_gpio_device_request(const char *message) {
  static const char *const keywords[] = {
      "humidifier", "fan", "device led", "gpio6 led", "加湿器",
      "风扇",      "普通led", "单色led",    "独立led",   "gpio6灯",
      "gpio 6灯",  "gpio6 led",
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

static bool message_has_condition_rule_marker(const char *message) {
  static const char *const keywords[] = {
      "if",       "when",     "otherwise", "above", "below",
      "greater",  "less",     "threshold", "rule",  "condition",
      "monitor",  "条件",     "规则",      "如果",  "当",
      "否则",     "反之",     "大于",      "小于",  "高于",  "低于",
      "超过",     "不超过",   "阈值",      "监测",  "监听",
      "持续",     "温度大于", "温度小于",
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

static bool tool_guard_match_current_time_request(const char *message) {
  static const char *const keywords[] = {
      "current time", "what time", "time now", "current date", "today date",
      "date today",   "当前时间",   "现在时间",  "现在几点",     "几点了",
      "当前日期",     "今天几号",   "今天日期",  "现在几号",     "当前几点",
      "实时日期",     "实时时间",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool message_requests_today_schedule(const char *message) {
  static const char *const keywords[] = {
      "today", "this evening", "tonight", "今天", "今日", "今天晚上", "今晚",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool message_requests_daily_schedule(const char *message) {
  static const char *const keywords[] = {
      "daily", "every day", "每天", "每日",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool message_has_explicit_schedule_time(const char *message) {
  if (!message || message[0] == '\0') {
    return false;
  }

  const char *scan = message;
  const char *prefix_sep = strstr(message, ": ");
  if (prefix_sep && (prefix_sep - message) < 48) {
    bool prefix_like = true;
    for (const char *p = message; p < prefix_sep; p++) {
      if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) {
        prefix_like = false;
        break;
      }
    }
    if (prefix_like) {
      scan = prefix_sep + 2;
    }
  }

  bool has_digit = false;
  for (const char *p = scan; *p; p++) {
    if (isdigit((unsigned char)*p)) {
      has_digit = true;
      const char *q = p;
      while (isdigit((unsigned char)*q)) {
        q++;
      }

      if ((*q == ':' || *q == '.') && isdigit((unsigned char)q[1])) {
        return true;
      }

      if (strncmp(q, "am", 2) == 0 || strncmp(q, "pm", 2) == 0 ||
          strncmp(q, "AM", 2) == 0 || strncmp(q, "PM", 2) == 0 ||
          strncmp(q, "秒", strlen("秒")) == 0 ||
          strncmp(q, "分钟", strlen("分钟")) == 0 ||
          strncmp(q, "分", strlen("分")) == 0 ||
          strncmp(q, "小时", strlen("小时")) == 0 ||
          strncmp(q, "时", strlen("时")) == 0 ||
          strncmp(q, "点", strlen("点")) == 0 ||
          strncmp(q, "天", strlen("天")) == 0 ||
          strncmp(q, "周", strlen("周")) == 0 ||
          strncmp(q, "月", strlen("月")) == 0) {
        return true;
      }
    }
  }

  if (has_digit &&
      (contains_substr_ci(scan, "every ") || contains_substr_ci(scan, "after ") ||
       contains_substr_ci(scan, "later ") || contains_substr_ci(scan, "minutes") ||
       contains_substr_ci(scan, "minute ") || contains_substr_ci(scan, "hours") ||
       contains_substr_ci(scan, "hour "))) {
    return true;
  }

  return false;
}

static bool parse_schedule_time_hhmm(const char *message, int *hour,
                                     int *minute) {
  if (!message || !hour || !minute) {
    return false;
  }

  for (const char *p = message; *p; p++) {
    if (!isdigit((unsigned char)*p)) {
      continue;
    }

    int h = 0;
    const char *q = p;
    while (isdigit((unsigned char)*q)) {
      h = h * 10 + (int)(*q - '0');
      q++;
      if (h > 999) {
        break;
      }
    }

    int m = 0;
    bool matched = false;
    if (*q == ':' && isdigit((unsigned char)q[1])) {
      q++;
      while (isdigit((unsigned char)*q)) {
        m = m * 10 + (int)(*q - '0');
        q++;
      }
      matched = true;
    } else if (strncmp(q, "点", strlen("点")) == 0) {
      q += strlen("点");
      if (strncmp(q, "半", strlen("半")) == 0) {
        m = 30;
        matched = true;
      } else if (isdigit((unsigned char)*q)) {
        while (isdigit((unsigned char)*q)) {
          m = m * 10 + (int)(*q - '0');
          q++;
        }
        if (strncmp(q, "分", strlen("分")) == 0) {
          matched = true;
        }
      } else {
        m = 0;
        matched = true;
      }
    }

    if (matched && h >= 0 && h <= 23 && m >= 0 && m <= 59) {
      *hour = h;
      *minute = m;
      return true;
    }
  }

  return false;
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
         tool_guard_match_environment_request(message) ||
         tool_guard_match_light_sensor_request(message) ||
         tool_guard_match_light_request(message) ||
         tool_guard_match_air_quality_request(message) ||
         tool_guard_match_presence_request(message) ||
         tool_guard_match_gpio_read_request(message) ||
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

static bool message_explicitly_requests_subagent(const char *message) {
  static const char *const intent_keywords[] = {
      "spawn_subagent", "/subagent", "subagent", "子代理",
  };
  static const char *const action_keywords[] = {
      "请调用", "调用", "使用", "启动", "运行", "交给", "委托", "让",
      "call", "use", "spawn", "delegate", "run",
  };

  if (!message || message[0] == '\0') {
    return false;
  }

  if (!message_has_any_keyword(message, intent_keywords,
                               sizeof(intent_keywords) /
                                   sizeof(intent_keywords[0]))) {
    return false;
  }

  return message_has_any_keyword(message, action_keywords,
                                 sizeof(action_keywords) /
                                     sizeof(action_keywords[0]));
}

static const char *trim_inline_separators(const char *text) {
  if (!text) {
    return NULL;
  }
  while (*text) {
    if (!isspace((unsigned char)*text) && *text != ':' && *text != ',' &&
        *text != ';') {
      break;
    }
    text++;
  }
  return text;
}

static bool extract_explicit_subagent_task(const char *message,
                                           char *out,
                                           size_t out_size) {
  static const char *const prefixes[] = {
      "让子代理",        "让 subagent",       "让 spawn_subagent",
      "请让子代理",      "请让 subagent",     "请子代理",
      "子代理",          "subagent",         "spawn_subagent",
      "由子代理",        "由 subagent",      "通过子代理",
      "通过 subagent",   "使用子代理",       "使用 subagent",
      "调用子代理",      "调用 subagent",     "调用 spawn_subagent",
  };

  if (!message || !out || out_size == 0) {
    return false;
  }
  out[0] = '\0';

  const char *scan = message;
  const char *prefix_sep = strstr(message, ": ");
  if (prefix_sep && (prefix_sep - message) < 48) {
    bool prefix_like = true;
    for (const char *p = message; p < prefix_sep; p++) {
      if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_')) {
        prefix_like = false;
        break;
      }
    }
    if (prefix_like) {
      scan = prefix_sep + 2;
    }
  }

  const char *mentions[] = {
      strstr(scan, "spawn_subagent"),
      strstr(scan, "/subagent"),
      strstr(scan, "subagent"),
      strstr(scan, "子代理"),
  };
  const char *mention = NULL;
  for (size_t i = 0; i < sizeof(mentions) / sizeof(mentions[0]); i++) {
    if (mentions[i] && (!mention || mentions[i] < mention)) {
      mention = mentions[i];
    }
  }

  if (mention) {
    const char *cursor = mention;
    while (*cursor) {
      if (*cursor == ',' || *cursor == ':' || *cursor == ';') {
        scan = cursor + 1;
        break;
      }
      if ((unsigned char)*cursor == 0xEF &&
          (unsigned char)cursor[1] == 0xBC &&
          ((unsigned char)cursor[2] == 0x8C ||
           (unsigned char)cursor[2] == 0x9A ||
           (unsigned char)cursor[2] == 0x9B)) {
        scan = cursor + 3;
        break;
      }
      cursor++;
    }
  }

  scan = trim_inline_separators(scan);
  if (!scan || scan[0] == '\0') {
    scan = message;
  }

  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    size_t len = strlen(prefixes[i]);
    if (contains_substr_ci(scan, prefixes[i]) && starts_with_ci_ascii(scan, prefixes[i])) {
      scan += len;
      scan = trim_inline_separators(scan);
      break;
    }
  }

  if (!scan || scan[0] == '\0') {
    scan = message;
  }

  snprintf(out, out_size, "%s", scan);
  return out[0] != '\0';
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
  if (message_requests_light_turn_on_without_color(message)) {
    return "white";
  }
  return NULL;
}

static bool message_requests_light_turn_on_without_color(const char *message) {
  static const char *const on_keywords[] = {
      "turn on", "switch on", "power on", "open light", "点亮",
      "亮起",    "打开灯",    "开灯",     "点灯",       "亮灯",
  };
  static const char *const color_keywords[] = {
      "blue",  "green", "red",   "white", "yellow", "purple", "violet",
      "cyan",  "orange","off",   "蓝",    "绿",     "红",
      "白",    "黄",    "紫",    "青",    "橙",     "关闭",
      "关灯",  "熄灭",
  };

  if (!message || !tool_guard_match_light_request(message)) {
    return false;
  }

  if (!message_has_any_keyword(message, on_keywords,
                               sizeof(on_keywords) / sizeof(on_keywords[0]))) {
    return false;
  }

  if (message_has_any_keyword(message, color_keywords,
                              sizeof(color_keywords) / sizeof(color_keywords[0]))) {
    return false;
  }

  return true;
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

typedef enum {
  FIXED_DEVICE_NONE = 0,
  FIXED_DEVICE_HUMIDIFIER,
  FIXED_DEVICE_FAN,
  FIXED_DEVICE_LED,
} fixed_gpio_device_t;

static fixed_gpio_device_t detect_fixed_gpio_device(const char *message) {
  if (!message) {
    return FIXED_DEVICE_NONE;
  }
  if (contains_substr_ci(message, "humidifier") ||
      contains_substr_ci(message, "加湿器")) {
    return FIXED_DEVICE_HUMIDIFIER;
  }
  if (contains_substr_ci(message, "fan") ||
      contains_substr_ci(message, "风扇")) {
    return FIXED_DEVICE_FAN;
  }
  if (contains_substr_ci(message, "device led") ||
      contains_substr_ci(message, "gpio6 led") ||
      contains_substr_ci(message, "gpio6灯") ||
      contains_substr_ci(message, "gpio 6灯") ||
      contains_substr_ci(message, "普通led") ||
      contains_substr_ci(message, "单色led") ||
      contains_substr_ci(message, "独立led")) {
    return FIXED_DEVICE_LED;
  }
  return FIXED_DEVICE_NONE;
}

static const char *fixed_gpio_device_action(fixed_gpio_device_t device) {
  switch (device) {
  case FIXED_DEVICE_HUMIDIFIER:
    return "set_humidifier";
  case FIXED_DEVICE_FAN:
    return "set_fan";
  case FIXED_DEVICE_LED:
    return "set_device_led";
  default:
    return "";
  }
}

static const char *fixed_gpio_device_label_zh(fixed_gpio_device_t device) {
  switch (device) {
  case FIXED_DEVICE_HUMIDIFIER:
    return "加湿器";
  case FIXED_DEVICE_FAN:
    return "风扇";
  case FIXED_DEVICE_LED:
    return "GPIO6 独立 LED";
  default:
    return "设备";
  }
}

static bool message_requests_on_then_off(const char *message) {
  if (!message) {
    return false;
  }
  const bool has_on =
      contains_substr_ci(message, "turn on") ||
      contains_substr_ci(message, "open") ||
      contains_substr_ci(message, "enable") ||
      contains_substr_ci(message, "打开") ||
      contains_substr_ci(message, "开启") ||
      contains_substr_ci(message, "拉高");
  const bool has_off =
      contains_substr_ci(message, "turn off") ||
      contains_substr_ci(message, "close") ||
      contains_substr_ci(message, "disable") ||
      contains_substr_ci(message, "关闭") ||
      contains_substr_ci(message, "关掉") ||
      contains_substr_ci(message, "拉低");
  return has_on && has_off && message_has_sequence_marker(message);
}

static bool message_requests_task_removal(const char *message) {
  static const char *const keywords[] = {
      "remove",       "delete",       "cancel",      "disable",
      "stop",         "clear",        "invalidate",  "invalid",
      "失效",         "取消",         "删除",        "清除",
      "移除",         "停止",         "停用",        "禁用",
      "不要执行",     "不再执行",     "作废",        "关闭任务",
      "删掉",         "去掉",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool message_requests_task_list(const char *message) {
  static const char *const keywords[] = {
      "list", "show", "inspect", "current", "列出", "查看", "当前",
      "有哪些", "任务列表", "规则列表", "定时列表",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool message_mentions_managed_task(const char *message) {
  static const char *const keywords[] = {
      "task",       "job-",      "cron",      "schedule",
      "rule-",      "wf-",       "workflow",  "rule",
      "任务",       "定时任务",  "计划任务",  "规则任务",
      "规则",       "条件规则",  "工作流",
  };
  return message_has_any_keyword(message, keywords,
                                 sizeof(keywords) / sizeof(keywords[0]));
}

static bool is_hex_ascii_token(const char *text, size_t len) {
  if (!text || len == 0) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)text[i];
    if (!((c >= '0' && c <= '9') ||
          (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

static bool extract_task_id_token(const char *message,
                                  char *out,
                                  size_t out_size,
                                  bool *looks_cron,
                                  bool *looks_automation) {
  static const char *const prefixes[] = {
      "job-", "cron-", "rule-", "wf-", "workflow-",
  };

  if (!message || !out || out_size < 2) {
    return false;
  }
  out[0] = '\0';
  if (looks_cron) {
    *looks_cron = false;
  }
  if (looks_automation) {
    *looks_automation = false;
  }

  const char *best = NULL;
  const char *best_prefix = NULL;
  for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
    const char *pos = find_substr_ci_ascii(message, prefixes[i]);
    if (pos && (!best || pos < best)) {
      best = pos;
      best_prefix = prefixes[i];
    }
  }
  if (!best || !best_prefix) {
    const bool cron_context =
        contains_substr_ci(message, "job") ||
        contains_substr_ci(message, "cron") ||
        contains_substr_ci(message, "schedule") ||
        contains_substr_ci(message, "定时") ||
        contains_substr_ci(message, "计划任务") ||
        contains_substr_ci(message, "提醒");
    if (!cron_context) {
      return false;
    }

    for (const char *p = message; *p;) {
      while (*p && !isalnum((unsigned char)*p)) {
        p++;
      }
      const char *start = p;
      while (*p && isalnum((unsigned char)*p)) {
        p++;
      }
      size_t len = (size_t)(p - start);
      if ((len == 8 || len == 16) && is_hex_ascii_token(start, len)) {
        if (len >= out_size) {
          len = out_size - 1;
        }
        memcpy(out, start, len);
        out[len] = '\0';
        if (looks_cron) {
          *looks_cron = true;
        }
        return true;
      }
    }
    return false;
  }

  size_t len = 0;
  while (best[len] &&
         (isalnum((unsigned char)best[len]) || best[len] == '-' ||
          best[len] == '_')) {
    len++;
  }
  if (len == 0) {
    return false;
  }
  if (len >= out_size) {
    len = out_size - 1;
  }
  memcpy(out, best, len);
  out[len] = '\0';

  if (looks_cron) {
    *looks_cron = starts_with_ci_ascii(best_prefix, "job-") ||
                  starts_with_ci_ascii(best_prefix, "cron-");
  }
  if (looks_automation) {
    *looks_automation = starts_with_ci_ascii(best_prefix, "rule-") ||
                        starts_with_ci_ascii(best_prefix, "wf-") ||
                        starts_with_ci_ascii(best_prefix, "workflow-");
  }
  return true;
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

static bool extract_next_number_token(const char **cursor,
                                      double *out_value,
                                      char *token_buf,
                                      size_t token_buf_size) {
  if (!cursor || !*cursor || !out_value || !token_buf || token_buf_size < 2) {
    return false;
  }

  const char *p = *cursor;
  while (*p) {
    const bool signed_number =
        ((*p == '+' || *p == '-') &&
         (isdigit((unsigned char)p[1]) ||
          (p[1] == '.' && isdigit((unsigned char)p[2]))));
    const bool unsigned_number =
        isdigit((unsigned char)*p) ||
        (*p == '.' && isdigit((unsigned char)p[1]));
    if (signed_number || unsigned_number) {
      break;
    }
    p++;
  }

  if (!*p) {
    *cursor = p;
    return false;
  }

  char *endptr = NULL;
  double value = strtod(p, &endptr);
  if (endptr == p) {
    *cursor = p + 1;
    return false;
  }

  size_t copy_len = (size_t)(endptr - p);
  if (copy_len >= token_buf_size) {
    copy_len = token_buf_size - 1;
  }
  memcpy(token_buf, p, copy_len);
  token_buf[copy_len] = '\0';

  *out_value = value;
  *cursor = endptr;
  return true;
}

static bool try_execute_deterministic_number_compare(const espagent_msg_t *msg,
                                                     char **final_text) {
  if (!msg || !msg->content || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content)) {
    return false;
  }

  const bool ask_larger =
      contains_substr_ci(msg->content, "谁大") ||
      contains_substr_ci(msg->content, "哪个大") ||
      contains_substr_ci(msg->content, "哪个更大");
  const bool ask_smaller =
      contains_substr_ci(msg->content, "谁小") ||
      contains_substr_ci(msg->content, "哪个小") ||
      contains_substr_ci(msg->content, "哪个更小");

  if (!ask_larger && !ask_smaller) {
    return false;
  }

  const char *cursor = msg->content;
  char lhs_token[32] = {0};
  char rhs_token[32] = {0};
  double lhs = 0.0;
  double rhs = 0.0;
  if (!extract_next_number_token(&cursor, &lhs, lhs_token, sizeof(lhs_token)) ||
      !extract_next_number_token(&cursor, &rhs, rhs_token, sizeof(rhs_token))) {
    return false;
  }

  const char *winner_token = NULL;
  const char *loser_token = NULL;
  if (lhs > rhs) {
    winner_token = lhs_token;
    loser_token = rhs_token;
  } else if (rhs > lhs) {
    winner_token = rhs_token;
    loser_token = lhs_token;
  } else {
    winner_token = lhs_token;
    loser_token = rhs_token;
  }

  char reply_buf[160] = {0};
  const bool concise_only =
      message_prefers_direct_reply_no_tools(msg->content) ||
      contains_substr_ci(msg->content, "最终答案") ||
      contains_substr_ci(msg->content, "不要解释");

  if (lhs == rhs) {
    if (concise_only) {
      snprintf(reply_buf, sizeof(reply_buf), "%s", lhs_token);
    } else {
      snprintf(reply_buf, sizeof(reply_buf), "%s 和 %s 一样大。", lhs_token,
               rhs_token);
    }
  } else if (ask_smaller) {
    if (concise_only) {
      snprintf(reply_buf, sizeof(reply_buf), "%s", loser_token);
    } else {
      snprintf(reply_buf, sizeof(reply_buf), "%s 更小。", loser_token);
    }
  } else {
    if (concise_only) {
      snprintf(reply_buf, sizeof(reply_buf), "%s", winner_token);
    } else {
      snprintf(reply_buf, sizeof(reply_buf), "%s 更大。", winner_token);
    }
  }

  ESP_LOGI(TAG, "=== CONV === Deterministic number compare => %s", reply_buf);
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool try_execute_deterministic_task_list_request(
    const espagent_msg_t *msg, char *tool_output, size_t tool_output_size,
    char **final_text) {
  (void)tool_output;
  (void)tool_output_size;

  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      !message_requests_task_list(msg->content) ||
      !message_mentions_managed_task(msg->content)) {
    return false;
  }

  const bool cron_marker =
      tool_guard_match_cron_request(msg->content) ||
      contains_substr_ci(msg->content, "job") ||
      contains_substr_ci(msg->content, "cron") ||
      contains_substr_ci(msg->content, "定时") ||
      contains_substr_ci(msg->content, "计划任务") ||
      contains_substr_ci(msg->content, "提醒");
  const bool automation_marker =
      contains_substr_ci(msg->content, "rule-") ||
      contains_substr_ci(msg->content, "wf-") ||
      contains_substr_ci(msg->content, "workflow") ||
      contains_substr_ci(msg->content, "规则") ||
      contains_substr_ci(msg->content, "条件") ||
      contains_substr_ci(msg->content, "工作流");
  const bool general_task =
      contains_substr_ci(msg->content, "任务") ||
      contains_substr_ci(msg->content, "task");

  const bool include_cron = cron_marker || general_task;
  const bool include_automation = automation_marker || general_task;
  if (!include_cron && !include_automation) {
    return false;
  }

  char cron_output[2048] = {0};
  char automation_output[2048] = {0};
  if (include_cron) {
    tool_registry_execute("cron_list", "{}", cron_output,
                          sizeof(cron_output));
  }
  if (include_automation) {
    tool_registry_execute("automation_list", "{}", automation_output,
                          sizeof(automation_output));
  }

  ESP_LOGI(TAG, "=== CONV === Deterministic task list route");
  char reply_buf[4096] = {0};
  if (include_cron && include_automation) {
    snprintf(reply_buf, sizeof(reply_buf),
             "当前定时任务：\n%.1800s\n\n当前规则/工作流任务：\n%.1800s",
             cron_output[0] ? cron_output : "(cron_list returned empty)",
             automation_output[0] ? automation_output
                                  : "(automation_list returned empty)");
  } else if (include_cron) {
    snprintf(reply_buf, sizeof(reply_buf), "当前定时任务：\n%.3600s",
             cron_output[0] ? cron_output : "(cron_list returned empty)");
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "当前规则/工作流任务：\n%.3600s",
             automation_output[0] ? automation_output
                                  : "(automation_list returned empty)");
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool try_execute_deterministic_task_remove_request(
    const espagent_msg_t *msg, char *tool_output, size_t tool_output_size,
    char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      !message_requests_task_removal(msg->content)) {
    return false;
  }

  bool looks_cron = false;
  bool looks_automation = false;
  char task_id[48] = {0};
  const bool has_id = extract_task_id_token(msg->content, task_id,
                                            sizeof(task_id), &looks_cron,
                                            &looks_automation);

  const bool cron_marker =
      looks_cron || tool_guard_match_cron_request(msg->content) ||
      contains_substr_ci(msg->content, "job-") ||
      contains_substr_ci(msg->content, "定时") ||
      contains_substr_ci(msg->content, "提醒");
  const bool automation_marker =
      looks_automation || message_has_condition_rule_marker(msg->content) ||
      contains_substr_ci(msg->content, "rule-") ||
      contains_substr_ci(msg->content, "wf-") ||
      contains_substr_ci(msg->content, "workflow") ||
      contains_substr_ci(msg->content, "规则") ||
      contains_substr_ci(msg->content, "条件") ||
      contains_substr_ci(msg->content, "工作流");

  if (!has_id) {
    if (!cron_marker && !automation_marker &&
        !message_mentions_managed_task(msg->content)) {
      return false;
    }
    *final_text = strdup(
        "请告诉我要清除的任务 ID。定时任务 ID 通常是 cron_list 中括号里的 8 位 ID，规则/工作流任务 ID 通常是 rule-... 或 wf-...。如果不确定，请先让我列出当前定时任务或规则任务。");
    return *final_text != NULL;
  }

  const bool remove_cron = looks_cron || (!looks_automation && cron_marker &&
                                         !automation_marker);
  const char *tool_name = remove_cron ? "cron_remove" : "automation_remove";
  char payload[96] = {0};
  if (remove_cron) {
    snprintf(payload, sizeof(payload), "{\"job_id\":\"%s\"}", task_id);
  } else {
    snprintf(payload, sizeof(payload), "{\"id\":\"%s\"}", task_id);
  }

  tool_output[0] = '\0';
  tool_registry_execute(tool_name, payload, tool_output, tool_output_size);
  ESP_LOGI(TAG, "=== CONV === Deterministic task remove route %s => %s",
           task_id, tool_output);

  char reply_buf[512] = {0};
  if (strncmp(tool_output, "OK:", 3) == 0) {
    snprintf(reply_buf, sizeof(reply_buf), "已清除%s %s：%s",
             remove_cron ? "定时任务" : "规则/工作流任务", task_id,
             tool_output);
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "清除%s %s 失败：%s",
             remove_cron ? "定时任务" : "规则/工作流任务", task_id,
             tool_output[0] ? tool_output : "unknown error");
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool try_execute_deterministic_condition_rule_request(
    const espagent_msg_t *msg, char *tool_output, size_t tool_output_size,
    char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      !message_has_condition_rule_marker(msg->content)) {
    return false;
  }

  const bool is_humidity =
      contains_substr_ci(msg->content, "humidity") ||
      contains_substr_ci(msg->content, "湿度");
  const bool is_temperature =
      contains_substr_ci(msg->content, "temperature") ||
      contains_substr_ci(msg->content, "temp") ||
      contains_substr_ci(msg->content, "温度") ||
      contains_substr_ci(msg->content, "气温") ||
      contains_substr_ci(msg->content, "室温");
  if (!is_temperature && !is_humidity) {
    return false;
  }

  const bool has_above =
      contains_substr_ci(msg->content, "above") ||
      contains_substr_ci(msg->content, "greater") ||
      contains_substr_ci(msg->content, "over") ||
      contains_substr_ci(msg->content, "大于") ||
      contains_substr_ci(msg->content, "高于") ||
      contains_substr_ci(msg->content, "超过");
  const bool has_below =
      contains_substr_ci(msg->content, "below") ||
      contains_substr_ci(msg->content, "less") ||
      contains_substr_ci(msg->content, "otherwise") ||
      contains_substr_ci(msg->content, "else") ||
      contains_substr_ci(msg->content, "否则") ||
      contains_substr_ci(msg->content, "反之") ||
      contains_substr_ci(msg->content, "小于") ||
      contains_substr_ci(msg->content, "低于") ||
      contains_substr_ci(msg->content, "不超过");
  if (!has_above || !has_below) {
    return false;
  }

  const char *cursor = msg->content;
  char threshold_token[32] = {0};
  double threshold = 0.0;
  if (!extract_next_number_token(&cursor, &threshold, threshold_token,
                                 sizeof(threshold_token))) {
    return false;
  }

  const char *colors[ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS] = {0};
  int color_count = extract_status_light_color_sequence(
      msg->content, colors, ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS);
  if (color_count < 2) {
    return false;
  }

  const char *above_color = colors[0];
  const char *below_color = colors[1];
  if (contains_substr_ci(msg->content, "小于") ||
      contains_substr_ci(msg->content, "低于") ||
      contains_substr_ci(msg->content, "below") ||
      contains_substr_ci(msg->content, "less")) {
    below_color = colors[0];
    above_color = colors[1];
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *above = cJSON_CreateObject();
  cJSON *below = cJSON_CreateObject();
  cJSON *above_args = cJSON_CreateObject();
  cJSON *below_args = cJSON_CreateObject();
  if (!root || !above || !below || !above_args || !below_args) {
    cJSON_Delete(root);
    cJSON_Delete(above);
    cJSON_Delete(below);
    cJSON_Delete(above_args);
    cJSON_Delete(below_args);
    return false;
  }

  cJSON_AddStringToObject(root, "name",
                          is_humidity ? "humidity_light_rule"
                                      : "temperature_light_rule");
  cJSON_AddStringToObject(root, "metric",
                          is_humidity ? "humidity_percent" : "temperature_c");
  cJSON_AddNumberToObject(root, "threshold", threshold);
  cJSON_AddNumberToObject(root, "interval_s", 10);
  cJSON_AddNumberToObject(root, "cooldown_s", 30);
  cJSON_AddNumberToObject(root, "hysteresis_c", is_humidity ? 2.0 : 0.5);
  cJSON_AddBoolToObject(root, "confirmed", true);

  cJSON_AddStringToObject(above, "target_role", "control_agent");
  cJSON_AddStringToObject(above, "action", "set_status_light");
  cJSON_AddStringToObject(above_args, "color", above_color);
  cJSON_AddItemToObject(above, "args", above_args);

  cJSON_AddStringToObject(below, "target_role", "control_agent");
  cJSON_AddStringToObject(below, "action", "set_status_light");
  cJSON_AddStringToObject(below_args, "color", below_color);
  cJSON_AddItemToObject(below, "args", below_args);

  cJSON_AddItemToObject(root, "above", above);
  cJSON_AddItemToObject(root, "below", below);

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!payload) {
    return false;
  }

  tool_output[0] = '\0';
  tool_registry_execute("automation_create_rule", payload, tool_output,
                        tool_output_size);
  cJSON_free(payload);
  ESP_LOGI(TAG, "=== CONV === Deterministic rule route => %s", tool_output);

  char reply_buf[640] = {0};
  if (strncmp(tool_output, "OK:", 3) == 0) {
    snprintf(reply_buf, sizeof(reply_buf),
             "已创建条件规则：%s大于%s时设置为%s，否则设置为%s。%s",
             is_humidity ? "湿度" : "温度", threshold_token, above_color,
             below_color, tool_output);
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "条件规则创建失败：%s",
             tool_output);
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool try_execute_deterministic_fixed_device_duration_workflow(
    const espagent_msg_t *msg, char *tool_output, size_t tool_output_size,
    char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      !message_requests_on_then_off(msg->content)) {
    return false;
  }

  fixed_gpio_device_t device = detect_fixed_gpio_device(msg->content);
  const char *action = fixed_gpio_device_action(device);
  if (!action[0]) {
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

  cJSON_AddStringToObject(root, "name",
                          device == FIXED_DEVICE_HUMIDIFIER
                              ? "humidifier_duration"
                              : (device == FIXED_DEVICE_FAN ? "fan_duration"
                                                            : "device_led_duration"));
  cJSON_AddItemToObject(root, "steps", steps);

  for (int i = 0; i < 2; i++) {
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
    cJSON_AddStringToObject(step, "action", action);
    cJSON_AddNumberToObject(args, "state", i == 0 ? 1 : 0);
    cJSON_AddItemToObject(step, "args", args);
    cJSON_AddItemToArray(steps, step);
  }

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!payload) {
    return false;
  }

  tool_output[0] = '\0';
  tool_registry_execute("automation_create_workflow", payload, tool_output,
                        tool_output_size);
  cJSON_free(payload);
  ESP_LOGI(TAG, "=== CONV === Deterministic fixed device workflow => %s",
           tool_output);

  char reply_buf[560] = {0};
  if (strncmp(tool_output, "OK:", 3) == 0) {
    snprintf(reply_buf, sizeof(reply_buf),
             "已创建%s限时流程：立即打开，%.1f秒后自动关闭。%s",
             fixed_gpio_device_label_zh(device), (double)delay_ms / 1000.0,
             tool_output);
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "%s限时流程创建失败：%s",
             fixed_gpio_device_label_zh(device), tool_output);
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool try_execute_deterministic_temperature_fan_rule(
    const espagent_msg_t *msg, char *tool_output, size_t tool_output_size,
    char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      detect_fixed_gpio_device(msg->content) != FIXED_DEVICE_FAN ||
      !message_has_condition_rule_marker(msg->content)) {
    return false;
  }

  const bool is_temperature =
      contains_substr_ci(msg->content, "temperature") ||
      contains_substr_ci(msg->content, "temp") ||
      contains_substr_ci(msg->content, "温度") ||
      contains_substr_ci(msg->content, "气温") ||
      contains_substr_ci(msg->content, "室温");
  if (!is_temperature) {
    return false;
  }

  const bool has_above =
      contains_substr_ci(msg->content, "above") ||
      contains_substr_ci(msg->content, "greater") ||
      contains_substr_ci(msg->content, "over") ||
      contains_substr_ci(msg->content, "大于") ||
      contains_substr_ci(msg->content, "高于") ||
      contains_substr_ci(msg->content, "超过");
  const bool has_below =
      contains_substr_ci(msg->content, "below") ||
      contains_substr_ci(msg->content, "less") ||
      contains_substr_ci(msg->content, "otherwise") ||
      contains_substr_ci(msg->content, "else") ||
      contains_substr_ci(msg->content, "否则") ||
      contains_substr_ci(msg->content, "反之") ||
      contains_substr_ci(msg->content, "小于") ||
      contains_substr_ci(msg->content, "低于") ||
      contains_substr_ci(msg->content, "不超过");
  if (!has_above || !has_below) {
    return false;
  }

  const char *cursor = msg->content;
  char threshold_token[32] = {0};
  double threshold = 0.0;
  if (!extract_next_number_token(&cursor, &threshold, threshold_token,
                                 sizeof(threshold_token))) {
    return false;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *above = cJSON_CreateObject();
  cJSON *below = cJSON_CreateObject();
  cJSON *above_args = cJSON_CreateObject();
  cJSON *below_args = cJSON_CreateObject();
  if (!root || !above || !below || !above_args || !below_args) {
    cJSON_Delete(root);
    cJSON_Delete(above);
    cJSON_Delete(below);
    cJSON_Delete(above_args);
    cJSON_Delete(below_args);
    return false;
  }

  cJSON_AddStringToObject(root, "name", "temperature_fan_rule");
  cJSON_AddStringToObject(root, "metric", "temperature_c");
  cJSON_AddNumberToObject(root, "threshold", threshold);
  cJSON_AddNumberToObject(root, "interval_s", 10);
  cJSON_AddNumberToObject(root, "cooldown_s", 15);
  cJSON_AddNumberToObject(root, "hysteresis_c", 0.5);
  cJSON_AddBoolToObject(root, "confirmed", true);

  cJSON_AddStringToObject(above, "target_role", "control_agent");
  cJSON_AddStringToObject(above, "action", "set_fan");
  cJSON_AddNumberToObject(above_args, "state", 1);
  cJSON_AddItemToObject(above, "args", above_args);

  cJSON_AddStringToObject(below, "target_role", "control_agent");
  cJSON_AddStringToObject(below, "action", "set_fan");
  cJSON_AddNumberToObject(below_args, "state", 0);
  cJSON_AddItemToObject(below, "args", below_args);

  cJSON_AddItemToObject(root, "above", above);
  cJSON_AddItemToObject(root, "below", below);

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!payload) {
    return false;
  }

  tool_output[0] = '\0';
  tool_registry_execute("automation_create_rule", payload, tool_output,
                        tool_output_size);
  cJSON_free(payload);
  ESP_LOGI(TAG, "=== CONV === Deterministic temperature fan rule => %s",
           tool_output);

  char reply_buf[640] = {0};
  if (strncmp(tool_output, "OK:", 3) == 0) {
    snprintf(reply_buf, sizeof(reply_buf),
             "已创建温度风扇规则：温度大于%s°C时打开风扇，否则关闭风扇。%s",
             threshold_token, tool_output);
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "温度风扇规则创建失败：%s",
             tool_output);
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool try_execute_deterministic_light_workflow(const espagent_msg_t *msg,
                                                     char *tool_output,
                                                     size_t tool_output_size,
                                                     char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      message_has_condition_rule_marker(msg->content) ||
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

static bool try_execute_deterministic_subagent_request(
    const espagent_msg_t *msg, char *tool_output, size_t tool_output_size,
    char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      !espagent_role_is_coordinator() ||
      !message_explicitly_requests_subagent(msg->content)) {
    return false;
  }

  char task[384] = {0};
  if (!extract_explicit_subagent_task(msg->content, task, sizeof(task))) {
    return false;
  }

  cJSON *payload_root = cJSON_CreateObject();
  if (!payload_root) {
    return false;
  }
  cJSON_AddStringToObject(payload_root, "task", task);
  cJSON_AddStringToObject(
      payload_root, "context",
      "The coordinator explicitly routed this request to spawn_subagent. "
      "Complete only the focused subtask and return one concise final result.");

  char *payload = cJSON_PrintUnformatted(payload_root);
  cJSON_Delete(payload_root);
  if (!payload) {
    return false;
  }

  tool_output[0] = '\0';
  tool_registry_execute("spawn_subagent", payload, tool_output, tool_output_size);
  cJSON_free(payload);
  ESP_LOGI(TAG, "=== CONV === Deterministic subagent route => %s", tool_output);

  char reply_buf[768] = {0};
  if (strncmp(tool_output, "Error:", 6) == 0 || tool_output[0] == '\0') {
    snprintf(reply_buf, sizeof(reply_buf), "子代理执行失败：%s",
             tool_output[0] ? tool_output : "unknown error");
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "子代理结果：%s", tool_output);
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool try_execute_deterministic_time_weather_request(
    const espagent_msg_t *msg, char *tool_output, size_t tool_output_size,
    char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      !tool_guard_match_current_time_request(msg->content)) {
    return false;
  }

  char time_output[192] = {0};
  tool_registry_execute("get_current_time", "{}", time_output,
                        sizeof(time_output));
  if (strncmp(time_output, "Error:", 6) == 0 || time_output[0] == '\0') {
    snprintf(tool_output, tool_output_size, "%s",
             time_output[0] ? time_output : "Error: failed to get current time");
    return false;
  }

  if (tool_guard_match_weather_request(msg->content)) {
    char weather_output[512] = {0};
    tool_registry_execute("get_weather", "{}", weather_output,
                          sizeof(weather_output));
    if (strncmp(weather_output, "Error:", 6) == 0 || weather_output[0] == '\0') {
      snprintf(tool_output, tool_output_size, "%s",
               weather_output[0] ? weather_output
                                 : "Error: failed to get current weather");
      return false;
    }

    char reply_buf[768] = {0};
    snprintf(reply_buf, sizeof(reply_buf), "当前时间：%s\n%s", time_output,
             weather_output);
    *final_text = strdup(reply_buf);
    return *final_text != NULL;
  }

  *final_text = strdup(time_output);
  return *final_text != NULL;
}

static bool try_execute_deterministic_scheduled_light_request(
    const espagent_msg_t *msg, char *tool_output, size_t tool_output_size,
    char **final_text) {
  if (!msg || !msg->content || !tool_output || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      !tool_guard_match_light_request(msg->content)) {
    return false;
  }

  const char *colors[ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS] = {0};
  if (message_has_sequence_marker(msg->content) &&
      extract_status_light_color_sequence(msg->content,
                                          colors,
                                          ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS) >= 2) {
    return false;
  }

  const bool today_schedule = message_requests_today_schedule(msg->content);
  const bool daily_schedule = message_requests_daily_schedule(msg->content);
  const bool has_time = message_has_explicit_schedule_time(msg->content);

  if (!has_time) {
    return false;
  }

  const char *color = detect_status_light_color(msg->content);
  if (!color) {
    *final_text = strdup(
        "你希望 WS2812 亮成什么颜色？请补充颜色，并说明是今天一次执行，还是每天重复执行。");
    return *final_text != NULL;
  }

  int hour = 0;
  int minute = 0;
  if (!parse_schedule_time_hhmm(msg->content, &hour, &minute)) {
    *final_text = strdup(
        "我识别到你想创建灯光定时任务，但没有读出明确时间。请用“今天 17:00”或“每天 17:00”这种格式再说一次。");
    return *final_text != NULL;
  }

  if (today_schedule == daily_schedule) {
    char reply_buf[256] = {0};
    snprintf(reply_buf, sizeof(reply_buf),
             "你是要“今天 %02d:%02d”执行一次，还是“每天 %02d:%02d”重复执行？另外我会把 WS2812 设置为 %s。",
             hour, minute, hour, minute, color);
    *final_text = strdup(reply_buf);
    return *final_text != NULL;
  }

  char trigger_message[192] = {0};
  snprintf(trigger_message, sizeof(trigger_message),
           "请把远程控制板的 WS2812 状态灯设置为%s。", color);

  cJSON *root = cJSON_CreateObject();
  if (!root) {
    return false;
  }
  cJSON_AddStringToObject(root, "name", "scheduled_ws2812");
  cJSON_AddStringToObject(root, "message", trigger_message);
  cJSON_AddStringToObject(root, "channel", msg->channel);
  cJSON_AddStringToObject(root, "chat_id", msg->chat_id);

  if (daily_schedule) {
    cJSON_AddStringToObject(root, "schedule_type", "daily");
    cJSON_AddNumberToObject(root, "hour", hour);
    cJSON_AddNumberToObject(root, "minute", minute);
  } else {
    time_t now = time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);
    local_tm.tm_hour = hour;
    local_tm.tm_min = minute;
    local_tm.tm_sec = 0;
    time_t at_epoch = mktime(&local_tm);
    if (at_epoch <= now) {
      char reply_buf[256] = {0};
      snprintf(reply_buf, sizeof(reply_buf),
               "今天 %02d:%02d 已经过了。请改成稍后的今天时间，或者明确说明“明天 %02d:%02d”/“每天 %02d:%02d”。",
               hour, minute, hour, minute, hour, minute);
      cJSON_Delete(root);
      *final_text = strdup(reply_buf);
      return *final_text != NULL;
    }
    cJSON_AddStringToObject(root, "schedule_type", "at");
    cJSON_AddNumberToObject(root, "at_epoch", (double)at_epoch);
    cJSON_AddBoolToObject(root, "delete_after_run", true);
  }

  char *payload = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!payload) {
    return false;
  }

  tool_output[0] = '\0';
  tool_registry_execute("cron_add", payload, tool_output, tool_output_size);
  cJSON_free(payload);

  char reply_buf[512] = {0};
  if (strncmp(tool_output, "OK:", 3) == 0) {
    snprintf(reply_buf, sizeof(reply_buf),
             daily_schedule
                 ? "已创建每日定时任务：每天 %02d:%02d 自动通知你，并点亮 WS2812 为 %s。%s"
                 : "已创建一次性定时任务：今天 %02d:%02d 自动通知你，并点亮 WS2812 为 %s。%s",
             hour, minute, color, tool_output);
  } else {
    snprintf(reply_buf, sizeof(reply_buf), "创建 WS2812 定时任务失败：%s",
             tool_output);
  }
  *final_text = strdup(reply_buf);
  return *final_text != NULL;
}

static bool try_execute_deterministic_cron_clarification(
    const espagent_msg_t *msg, char **final_text) {
  if (!msg || !msg->content || !final_text ||
      strcmp(msg->channel, ESPAGENT_CHAN_SYSTEM) == 0 ||
      message_has_local_marker(msg->content) ||
      !tool_guard_match_cron_request(msg->content) ||
      message_has_explicit_schedule_time(msg->content)) {
    return false;
  }

  const char *colors[ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS] = {0};
  if (tool_guard_match_light_request(msg->content) &&
      message_has_sequence_marker(msg->content) &&
      extract_status_light_color_sequence(msg->content,
                                          colors,
                                          ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS) >= 2) {
    return false;
  }

  *final_text = strdup(
      "要创建定时任务，请先给出明确触发时间，例如“10分钟后提醒我浇花”、“今天18:30提醒我关灯”或“每天08:00播报天气”。");
  return *final_text != NULL;
}

static bool is_mesh_related_tool_name(const char *name) {
  return name &&
         (strcmp(name, "mesh_send_command") == 0 ||
          strcmp(name, "read_temperature_humidity") == 0 ||
          strcmp(name, "set_status_light") == 0 ||
          strcmp(name, "ws2812_set") == 0 ||
          strcmp(name, "set_humidifier") == 0 ||
          strcmp(name, "set_fan") == 0 ||
          strcmp(name, "set_device_led") == 0 ||
          strcmp(name, "copper_gpio_write") == 0 ||
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
  } else if (tool_guard_match_light_request(msg->content) &&
             !tool_guard_match_fixed_gpio_device_request(msg->content)) {
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
    allowed = tool_guard_match_light_request(message) &&
              !tool_guard_match_fixed_gpio_device_request(message);
    expected = "board light or LED control";
  } else if (strcmp(tool_name, "set_humidifier") == 0) {
    allowed = contains_substr_ci(message, "humidifier") ||
              contains_substr_ci(message, "加湿器");
    expected = "humidifier control on GPIO4";
  } else if (strcmp(tool_name, "set_fan") == 0) {
    allowed = contains_substr_ci(message, "fan") ||
              contains_substr_ci(message, "风扇");
    expected = "fan control on GPIO5";
  } else if (strcmp(tool_name, "set_device_led") == 0) {
    allowed = contains_substr_ci(message, "device led") ||
              contains_substr_ci(message, "gpio6 led") ||
              contains_substr_ci(message, "gpio6灯") ||
              contains_substr_ci(message, "gpio 6灯") ||
              contains_substr_ci(message, "普通led") ||
              contains_substr_ci(message, "单色led") ||
              contains_substr_ci(message, "独立led");
    expected = "discrete GPIO6 LED control";
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
  } else if (strcmp(tool_name, "copper_gpio_write") == 0) {
    allowed = tool_guard_match_copper_gpio_write_request(message);
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

static bool message_prefers_direct_reply_no_tools(const char *message) {
  static const char *const direct_markers[] = {
      "请用一句话回复", "用一句话回复", "一句话回复", "用一句话回答", "简短回复",
      "直接回复", "直接回答", "只需要回复", "只回复", "不要调用工具",
      "不用调用工具", "不要使用工具", "just reply", "reply only",
      "answer only", "no tools",
  };

  if (!message || message[0] == '\0') {
    return false;
  }

  return message_has_any_keyword(
      message, direct_markers,
      sizeof(direct_markers) / sizeof(direct_markers[0]));
}

static bool message_is_simple_greeting_or_smalltalk(const char *message) {
  static const char *const markers[] = {
      "你好", "您好", "早上好", "中午好", "晚上好",
      "在吗", "在不在", "收到吗", "谢谢", "感谢", "再见",
  };
  static const char *const ascii_markers[] = {
      "hello", "hi", "hey", "bye",
  };

  if (!message || message[0] == '\0') {
    return false;
  }

  if (strlen(message) > 48) {
    return false;
  }

  if (message_has_any_keyword(message, markers,
                              sizeof(markers) / sizeof(markers[0]))) {
    return true;
  }

  for (size_t i = 0; i < sizeof(ascii_markers) / sizeof(ascii_markers[0]);
       i++) {
    if (contains_ascii_word_ci(message, ascii_markers[i])) {
      return true;
    }
  }

  return false;
}

static bool message_is_general_qa_turn(const char *message) {
  static const char *const hardware_or_agent_keywords[] = {
      "esp32",       "esp-agent",     "espagent",     "agent",
      "mesh",        "mqtt",          "ws2812",       "gpio",
      "i2c",         "uart",          "servo",        "relay",
      "sensor",      "telemetry",     "skill",        "skills",
      "memory",      "cache",         "mcp",          "workflow",
      "automation",  "subagent",      "sub-agent",    "board",
      "node",        "coordinator",   "guardian",     "control",
      "feishu",      "websocket",     "bluetooth",    "ble",
      "wifi",        "红外",          "空调",         "灯",
      "温度",        "湿度",          "光照",         "传感器",
      "舵机",        "麦克风",        "扬声器",       "喇叭",
      "屏幕",        "节点",          "开发板",       "设备",
      "联网",        "控制",          "状态灯",       "流水灯",
      "技能",        "记忆",          "网关",         "硬件",
  };

  if (!message || message[0] == '\0') {
    return false;
  }

  if (!text_looks_like_question(message)) {
    return false;
  }

  if (message_has_any_keyword(message, hardware_or_agent_keywords,
                              sizeof(hardware_or_agent_keywords) /
                                  sizeof(hardware_or_agent_keywords[0]))) {
    return false;
  }

  if (message_has_sequence_marker(message)) {
    return false;
  }

  return true;
}

static bool message_is_project_explanation_turn(const char *message) {
  static const char *const project_keywords[] = {
      "项目",         "系统",          "架构",          "原理",
      "流程",         "方案",          "设计",          "实现",
      "prompt",       "路由",          "意图",          "多agent",
      "multi-agent",  "agent",         "mesh",          "gateway",
      "skills",       "memory",        "cache",         "mcp",
      "feishu",       "websocket",     "mqtt",          "esp32",
      "coordinator",  "guardian",      "sensor_agent",  "control_agent",
      "project",      "architecture",  "design",        "implementation",
      "prompt",       "routing",       "orchestration", "context",
      "why",          "meaning",       "how to",        "explain",
      "introduce",    "overview",      "what does",     "为什么",
      "什么意思",      "怎么做",        "如何修改",      "如何优化",
      "介绍一下",      "解释一下",      "说一下",
  };

  if (!message || message[0] == '\0') {
    return false;
  }

  if (!text_looks_like_question(message)) {
    return false;
  }

  if (!message_has_any_keyword(message, project_keywords,
                               sizeof(project_keywords) /
                                   sizeof(project_keywords[0]))) {
    return false;
  }

  if (message_should_have_used_mesh(message) ||
      tool_guard_match_cron_request(message) ||
      tool_guard_match_weather_request(message)) {
    return false;
  }

  return true;
}

static int history_limit_for_turn(bool smalltalk_turn, bool prefer_direct_reply,
                                  bool project_explanation_turn) {
  if (espagent_role_is_coordinator()) {
    if (smalltalk_turn) {
      return 2;
    }
    if (project_explanation_turn) {
      return 3;
    }
    if (prefer_direct_reply) {
      return 2;
    }
    return 6;
  }
  return ESPAGENT_AGENT_MAX_HISTORY;
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

static void append_tool_summary_line(char *summary,
                                     size_t summary_size,
                                     const char *tool_name,
                                     const char *tool_output) {
  if (!summary || summary_size == 0 || !tool_name || !tool_output) {
    return;
  }

  char compact[256] = {0};
  compact_for_trace(tool_output, compact, sizeof(compact));
  size_t off = strnlen(summary, summary_size - 1);
  if (off >= summary_size - 1) {
    return;
  }
  snprintf(summary + off, summary_size - off, "- %s: %s\n", tool_name,
           compact);
}

static cJSON *build_compact_finalization_messages(const char *user_text,
                                                  const char *tool_summary) {
  if (!user_text || !tool_summary || tool_summary[0] == '\0') {
    return NULL;
  }

  cJSON *messages = cJSON_CreateArray();
  cJSON *user_msg = cJSON_CreateObject();
  if (!messages || !user_msg) {
    cJSON_Delete(messages);
    cJSON_Delete(user_msg);
    return NULL;
  }

  char *content = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM);
  if (!content) {
    cJSON_Delete(user_msg);
    cJSON_Delete(messages);
    return NULL;
  }

  snprintf(content, 2048,
           "Original user request:\n%s\n\n"
           "Executed tool results:\n%s\n"
           "Write the final answer using only these confirmed results. "
           "Do not request more tools.",
           user_text, tool_summary);
  cJSON_AddStringToObject(user_msg, "role", "user");
  cJSON_AddStringToObject(user_msg, "content", content);
  cJSON_AddItemToArray(messages, user_msg);
  free(content);
  return messages;
}

static char *build_direct_tool_summary_reply(const char *tool_summary) {
  if (!tool_summary || tool_summary[0] == '\0') {
    return NULL;
  }

  char *reply = heap_caps_calloc(1, 3072, MALLOC_CAP_SPIRAM);
  if (!reply) {
    return NULL;
  }

  snprintf(reply, 3072,
           "本轮工具已经执行完成。当前 coordinator 可用内存过低，为避免第二次模型请求失败，先直接返回确认结果：\n\n%s",
           tool_summary);
  return reply;
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
  if (agent_should_persist_trace()) {
    (void)session_append_trace(msg->chat_id, event_type, summary, json);
  }
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
                                 size_t tool_output_size, char *tool_summary,
                                 size_t tool_summary_size, char *tool_fallback,
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
      append_tool_summary_line(tool_summary, tool_summary_size, call->name,
                               tool_output);
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
    append_tool_summary_line(tool_summary, tool_summary_size, call->name,
                             tool_output);
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

static bool send_direct_text_reply(const espagent_msg_t *msg, const char *text) {
  if (!msg || !text) {
    return false;
  }

  espagent_msg_t out = {0};
  strncpy(out.channel, msg->channel, sizeof(out.channel) - 1);
  strncpy(out.chat_id, msg->chat_id, sizeof(out.chat_id) - 1);
  out.content = strdup(text);
  if (!out.content) {
    return false;
  }
  if (message_bus_push_outbound(&out) != ESP_OK) {
    free(out.content);
    return false;
  }
  return true;
}

static bool handle_slash_action(const espagent_msg_t *msg,
                                const espagent_slash_result_t *slash) {
  if (!msg || !slash || slash->type != ESPAGENT_SLASH_ACTION) {
    return false;
  }

  char reply[512] = {0};
  bool any_removed = false;

  if (strcmp(slash->command, "clear") == 0) {
    if (session_clear_all_context(msg->chat_id) == ESP_OK) {
      snprintf(reply, sizeof(reply),
               "已清除当前 chat_id 的 session/history/brief/trace。");
    } else {
      snprintf(reply, sizeof(reply),
               "当前 chat_id 没有可清除的 session/history/brief/trace。");
    }
    return send_direct_text_reply(msg, reply);
  }

  if (strcmp(slash->command, "clear_all_memory") == 0) {
    if (session_clear_all_context(msg->chat_id) == ESP_OK) {
      any_removed = true;
    }
    if (session_clear_all_sessions_and_traces() == ESP_OK) {
      any_removed = true;
    }
    if (memory_clear_all() == ESP_OK) {
      any_removed = true;
    }
    if (memory_v2_clear_all() == ESP_OK) {
      any_removed = true;
    }

    snprintf(reply, sizeof(reply),
             any_removed
                 ? "已清除当前上下文，并删除 MEMORY/profile/skills/trace/session/brief 持久数据。"
                 : "没有发现可清除的 MEMORY/profile/skills/trace/session 数据。");
    return send_direct_text_reply(msg, reply);
  }

  if (strcmp(slash->command, "context_status") == 0) {
    if (session_context_status_text(msg->chat_id, reply, sizeof(reply)) != ESP_OK) {
      snprintf(reply, sizeof(reply), "无法读取当前 chat_id 的 context 状态。");
    }
    return send_direct_text_reply(msg, reply);
  }

  if (strcmp(slash->command, "skills_list") == 0) {
    char *skills_text = heap_caps_calloc(1, 3072, MALLOC_CAP_SPIRAM);
    if (!skills_text) {
      snprintf(reply, sizeof(reply), "内存不足，无法列出 skills。");
      return send_direct_text_reply(msg, reply);
    }

    esp_err_t err =
        skill_loader_build_index_text(skills_text, 3072);
    bool ok = false;
    if (err == ESP_OK && skills_text[0]) {
      ok = send_direct_text_reply(msg, skills_text);
    } else {
      snprintf(reply, sizeof(reply), "当前没有可用的 skills。");
      ok = send_direct_text_reply(msg, reply);
    }
    free(skills_text);
    return ok;
  }

  if (strcmp(slash->command, "skills_show") == 0) {
    char *skill_text = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM);
    char title[128] = {0};
    if (!skill_text) {
      snprintf(reply, sizeof(reply), "内存不足，无法读取 skill 内容。");
      return send_direct_text_reply(msg, reply);
    }

    esp_err_t err = skill_loader_read_skill_by_name(
        slash->text, skill_text, 4096, title, sizeof(title));
    bool ok = false;
    if (err == ESP_OK && skill_text[0]) {
      char *full_reply = heap_caps_calloc(1, 4608, MALLOC_CAP_SPIRAM);
      if (full_reply) {
        snprintf(full_reply, 4608, "# %s\n\n%s",
                 title[0] ? title : slash->text, skill_text);
        ok = send_direct_text_reply(msg, full_reply);
        free(full_reply);
      } else {
        ok = send_direct_text_reply(msg, skill_text);
      }
    } else {
      snprintf(reply, sizeof(reply), "未找到 skill: %.320s", slash->text);
      ok = send_direct_text_reply(msg, reply);
    }
    free(skill_text);
    return ok;
  }

  snprintf(reply, sizeof(reply), "Unsupported slash action: /%s",
           slash->command);
  return send_direct_text_reply(msg, reply);
}

static void agent_loop_task(void *arg) {
  ESP_LOGI(TAG, "Agent loop started on core %d", xPortGetCoreID());

  /* Allocate large buffers from PSRAM */
  char *system_prompt =
      heap_caps_calloc(1, ESPAGENT_CONTEXT_BUF_SIZE, MALLOC_CAP_SPIRAM);
  char *history_json =
      heap_caps_calloc(1, ESPAGENT_LLM_STREAM_BUF_SIZE, MALLOC_CAP_SPIRAM);
  char *tool_output = heap_caps_calloc(1, TOOL_OUTPUT_SIZE, MALLOC_CAP_SPIRAM);
  char *tool_summary =
      heap_caps_calloc(1, TOOL_SUMMARY_SIZE, MALLOC_CAP_SPIRAM);
  char *tool_fallback =
      heap_caps_calloc(1, TOOL_OUTPUT_SIZE + 512, MALLOC_CAP_SPIRAM);
  char *relevance_query = heap_caps_calloc(1, 1024, MALLOC_CAP_SPIRAM);
  char *turn_buf = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM);

  if (!system_prompt || !history_json || !tool_output || !tool_summary ||
      !tool_fallback ||
      !relevance_query || !turn_buf) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
    vTaskDelete(NULL);
    return;
  }

  const char *tools_json = tool_registry_get_tools_json();
  const char *tools_json_compact_coordinator =
      tool_registry_get_tools_json_compact_coordinator();
  const char *tools_json_mesh_only =
      tool_registry_get_tools_json_mesh_only();

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
    (void)tool_status_indicator_thinking_start();

    bool proactive_turn = (msg.flags & ESPAGENT_MSG_FLAG_PROACTIVE) != 0;
    bool internal_result_turn =
        (msg.flags & ESPAGENT_MSG_FLAG_INTERNAL_RESULT) != 0;
    if (!proactive_turn && !internal_result_turn) {
      proactive_service_note_contact(msg.channel, msg.chat_id);
    }

    espagent_slash_result_t slash = {0};
    if (!proactive_turn && !internal_result_turn &&
        espagent_slash_try_handle(msg.content, &slash)) {
      if (slash.type == ESPAGENT_SLASH_HELP || slash.type == ESPAGENT_SLASH_ERROR) {
        ESP_LOGI(TAG, "Slash command /%s produced direct reply",
                 slash.command[0] ? slash.command : "help");
        (void)send_direct_text_reply(&msg, slash.text);
        (void)tool_status_indicator_thinking_stop();
        free(msg.content);
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        continue;
      }

      if (slash.type == ESPAGENT_SLASH_ACTION) {
        ESP_LOGI(TAG, "Slash command /%s executing action", slash.command);
        (void)handle_slash_action(&msg, &slash);
        (void)tool_status_indicator_thinking_stop();
        free(msg.content);
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        continue;
      }

      if (slash.type == ESPAGENT_SLASH_REWRITE) {
        char *rewritten = strdup(slash.text);
        if (rewritten) {
          ESP_LOGI(TAG, "Slash command /%s rewrote request", slash.command);
          free(msg.content);
          msg.content = rewritten;
          ESP_LOGI(TAG, "=== CONV === [%s/%s] >> SLASH-REWRITE: %s",
                   msg.channel, msg.chat_id, msg.content);
        }
      }
    }

    bool smalltalk_turn = message_is_simple_greeting_or_smalltalk(msg.content);
    bool project_explanation_turn =
        espagent_role_is_coordinator() &&
        message_is_project_explanation_turn(msg.content);
    bool general_qa_turn =
        espagent_role_is_coordinator() &&
        !project_explanation_turn &&
        message_is_general_qa_turn(msg.content);
    bool coordinator_transport_tight =
        espagent_role_is_coordinator() && llm_transport_heap_is_tight();
    bool prefer_direct_reply = message_prefers_direct_reply_no_tools(
                                   msg.content) ||
                               smalltalk_turn || general_qa_turn ||
                               project_explanation_turn;
    bool use_lightweight_direct_prompt =
        espagent_role_is_coordinator() && prefer_direct_reply;
    int history_limit =
        history_limit_for_turn(smalltalk_turn, prefer_direct_reply,
                               project_explanation_turn);
    if (coordinator_transport_tight && history_limit > 3) {
      history_limit = 3;
    }
    if (espagent_role_is_coordinator()) {
      espagent_net_guard_defer_background(AGENT_LLM_BACKGROUND_DEFER_MS);
    }

    /* 1. Build system prompt */
    if (project_explanation_turn) {
      build_project_explanation_prompt(system_prompt,
                                       ESPAGENT_CONTEXT_BUF_SIZE,
                                       &msg);
    } else if (use_lightweight_direct_prompt) {
      build_lightweight_direct_reply_prompt(system_prompt,
                                           ESPAGENT_CONTEXT_BUF_SIZE,
                                           &msg);
    } else if (coordinator_transport_tight) {
      build_tight_coordinator_prompt(system_prompt,
                                     ESPAGENT_CONTEXT_BUF_SIZE,
                                     &msg);
    } else {
      context_build_system_prompt(system_prompt, ESPAGENT_CONTEXT_BUF_SIZE);
      append_turn_context_prompt(system_prompt, ESPAGENT_CONTEXT_BUF_SIZE, &msg);
    }
    ESP_LOGI(TAG, "LLM turn context: channel=%s chat_id=%s", msg.channel,
             msg.chat_id);

    /* 2. Load session history into cJSON array */
    session_get_history_json(msg.chat_id, history_json,
                             ESPAGENT_LLM_STREAM_BUF_SIZE, history_limit);
    ESP_LOGI(TAG,
             "History loaded: limit=%d bytes=%u direct_reply=%d smalltalk=%d general_qa=%d project_explain=%d tight=%d",
             history_limit,
             (unsigned)strlen(history_json),
             prefer_direct_reply ? 1 : 0,
             smalltalk_turn ? 1 : 0,
             general_qa_turn ? 1 : 0,
             project_explanation_turn ? 1 : 0,
             coordinator_transport_tight ? 1 : 0);
    if (!use_lightweight_direct_prompt && !coordinator_transport_tight) {
      memset(relevance_query, 0, 1024);
      build_relevance_query(msg.content, relevance_query, 1024);
      if (!espagent_role_is_coordinator()) {
        memset(turn_buf, 0, 2048);
        if (memory_v2_build_relevant_profile_summary(relevance_query,
                                                     turn_buf,
                                                     2048) == ESP_OK &&
            turn_buf[0]) {
            append_prompt_format(system_prompt,
                                 ESPAGENT_CONTEXT_BUF_SIZE,
                                 "\n## Relevant User Profile For This Turn\n\n%s\n",
                                 turn_buf);
        }
        memset(turn_buf, 0, 2048);
        if (memory_v2_build_relevant_profile_conflict_summary(relevance_query,
                                                              turn_buf,
                                                              2048) == ESP_OK &&
            turn_buf[0]) {
            append_prompt_format(system_prompt,
                                 ESPAGENT_CONTEXT_BUF_SIZE,
                                 "\n## Relevant Profile Changes For This Turn\n\n%s\n",
                                 turn_buf);
        }
        memset(turn_buf, 0, 2048);
        if (memory_v2_build_relevant_skill_summary(relevance_query,
                                                   turn_buf,
                                                   2048) == ESP_OK &&
            turn_buf[0]) {
            append_prompt_format(system_prompt,
                                 ESPAGENT_CONTEXT_BUF_SIZE,
                                 "\n## Relevant Skill Notes For This Turn\n\n%s\n",
                                 turn_buf);
        }
      }
      memset(turn_buf, 0, 2048);
      if (skill_loader_build_relevant_details(relevance_query,
                                              turn_buf,
                                              2048,
                                              2) == ESP_OK &&
          turn_buf[0]) {
        append_prompt_format(system_prompt,
                             ESPAGENT_CONTEXT_BUF_SIZE,
                             "\n## Relevant Skill Details For This Turn\n\n%s\n",
                             turn_buf);
      }
        append_pending_clarification_prompt(system_prompt,
                                          ESPAGENT_CONTEXT_BUF_SIZE,
                                          history_json);
    } else if (coordinator_transport_tight) {
      ESP_LOGW(TAG,
               "Coordinator transport heap is tight; skipping skill/memory context expansion for this turn");
    }

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
    esp_err_t last_llm_err = ESP_OK;
    tool_summary[0] = '\0';
    tool_fallback[0] = '\0';

    if (!try_execute_deterministic_number_compare(&msg, &final_text) &&
        !try_execute_deterministic_task_list_request(
            &msg, tool_output, TOOL_OUTPUT_SIZE, &final_text) &&
        !try_execute_deterministic_task_remove_request(
            &msg, tool_output, TOOL_OUTPUT_SIZE, &final_text) &&
        !try_execute_deterministic_fixed_device_duration_workflow(
            &msg, tool_output, TOOL_OUTPUT_SIZE, &final_text) &&
        !try_execute_deterministic_temperature_fan_rule(
            &msg, tool_output, TOOL_OUTPUT_SIZE, &final_text) &&
        !try_execute_deterministic_condition_rule_request(
            &msg, tool_output, TOOL_OUTPUT_SIZE, &final_text) &&
        !try_execute_deterministic_light_workflow(&msg, tool_output,
                                                  TOOL_OUTPUT_SIZE,
                                                  &final_text) &&
        !try_execute_deterministic_scheduled_light_request(
            &msg, tool_output, TOOL_OUTPUT_SIZE, &final_text) &&
        !try_execute_deterministic_cron_clarification(&msg, &final_text) &&
        !try_execute_deterministic_subagent_request(&msg, tool_output,
                                                    TOOL_OUTPUT_SIZE,
                                                    &final_text) &&
        !try_execute_deterministic_time_weather_request(&msg, tool_output,
                                                        TOOL_OUTPUT_SIZE,
                                                        &final_text)) {
      try_execute_deterministic_mesh_request(&msg, tool_output, TOOL_OUTPUT_SIZE,
                                             &final_text);
    }

    while (!final_text && iteration < ESPAGENT_AGENT_MAX_TOOL_ITER) {
      /* Send "working" indicator before each API call */
#if ESPAGENT_AGENT_SEND_WORKING_STATUS
      if (!proactive_turn && !sent_working_status && !prefer_direct_reply &&
          strcmp(msg.channel, ESPAGENT_CHAN_SYSTEM) != 0 &&
          strcmp(msg.channel, ESPAGENT_CHAN_FEISHU) != 0 &&
          strcmp(msg.channel, ESPAGENT_CHAN_WEBSOCKET) != 0) {
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
      cJSON *llm_messages = messages;
      cJSON *compact_messages = NULL;
      const char *selected_tools_json = tools_json;
      if (espagent_role_is_coordinator()) {
        if (message_should_have_used_mesh(msg.content) && tools_json_mesh_only) {
          selected_tools_json = tools_json_mesh_only;
        } else if (tools_json_compact_coordinator) {
          selected_tools_json = tools_json_compact_coordinator;
        }
      }
      const char *active_tools_json = prefer_direct_reply ? NULL : selected_tools_json;
      if (!prefer_direct_reply &&
          espagent_role_is_coordinator() &&
          iteration > 0 &&
          llm_transport_heap_is_tight()) {
        active_tools_json = NULL;
        compact_messages =
            build_compact_finalization_messages(msg.content, tool_summary);
        if (compact_messages) {
          llm_messages = compact_messages;
          ESP_LOGW(
              TAG,
              "Coordinator follow-up LLM round switched to compact no-tools finalization due to tight transport heap");
        } else {
          ESP_LOGW(TAG,
                   "Coordinator follow-up LLM round forced to no-tools finalization due to tight transport heap");
        }
      }
      if (prefer_direct_reply && iteration == 0) {
        ESP_LOGI(TAG,
                 "Direct-reply heuristic enabled for this turn; first LLM call runs without tools (smalltalk=%d general_qa=%d project_explain=%d)",
                 smalltalk_turn ? 1 : 0, general_qa_turn ? 1 : 0,
                 project_explanation_turn ? 1 : 0);
      }
      if (espagent_role_is_coordinator()) {
        espagent_net_guard_defer_background(AGENT_LLM_BACKGROUND_DEFER_MS);
      }
      err = llm_chat_tools(system_prompt, llm_messages, active_tools_json, &resp);
      if (compact_messages) {
        cJSON_Delete(compact_messages);
        compact_messages = NULL;
      }

      if (err != ESP_OK) {
        last_llm_err = err;
        ESP_LOGE(TAG, "LLM call failed: %s", esp_err_to_name(err));
        if (iteration == 0 && active_tools_json && !prefer_direct_reply) {
          ESP_LOGW(TAG,
                   "Retrying first LLM turn without tools after tool-enabled failure");
          if (espagent_role_is_coordinator()) {
            espagent_net_guard_defer_background(AGENT_LLM_BACKGROUND_DEFER_MS);
          }
          err = llm_chat_tools(system_prompt, messages, NULL, &resp);
          if (err == ESP_OK) {
            prefer_direct_reply = true;
            last_llm_err = ESP_OK;
          } else {
            last_llm_err = err;
            ESP_LOGE(TAG, "Fallback no-tools LLM retry failed: %s",
                     esp_err_to_name(err));
          }
        }
        if (err != ESP_OK) {
          break;
        }
      }

      if (!resp.tool_use && (!resp.text || resp.text_len == 0) &&
          iteration == 0 && active_tools_json && !prefer_direct_reply) {
        ESP_LOGW(TAG,
                 "First LLM turn returned empty non-tool response with tools; retrying without tools");
        llm_response_free(&resp);
        if (espagent_role_is_coordinator()) {
          espagent_net_guard_defer_background(AGENT_LLM_BACKGROUND_DEFER_MS);
        }
        err = llm_chat_tools(system_prompt, messages, NULL, &resp);
        if (err == ESP_OK) {
          prefer_direct_reply = true;
          last_llm_err = ESP_OK;
        } else {
          last_llm_err = err;
          ESP_LOGE(TAG, "No-tools retry after empty response failed: %s",
                   esp_err_to_name(err));
          break;
        }
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
                             tool_summary, TOOL_SUMMARY_SIZE,
                             tool_fallback, TOOL_OUTPUT_SIZE + 512);
      cJSON *result_msg = cJSON_CreateObject();
      cJSON_AddStringToObject(result_msg, "role", "user");
      cJSON_AddItemToObject(result_msg, "content", tool_results);
      cJSON_AddItemToArray(messages, result_msg);

      llm_response_free(&resp);
      iteration++;

      if (!final_text &&
          espagent_role_is_coordinator() &&
          iteration > 0 &&
          llm_transport_heap_is_tight() &&
          tool_summary[0]) {
        final_text = build_direct_tool_summary_reply(tool_summary);
        if (final_text) {
          ESP_LOGW(TAG,
                   "Coordinator follow-up finalized locally from tool summary due to tight transport heap");
          break;
        }
      }
    }

    cJSON_Delete(messages);

    if (!final_text) {
      if (tool_fallback[0]) {
        final_text = strdup(tool_fallback);
      } else if (tool_output[0]) {
        char fallback_text[TOOL_OUTPUT_SIZE + 128];
        snprintf(fallback_text, sizeof(fallback_text),
                 "我已经执行了相关步骤，但整理最终回复时出了问题。先把当前结果直接发给你：\n\n%s",
                 tool_output);
        final_text = strdup(fallback_text);
      } else if (iteration >= ESPAGENT_AGENT_MAX_TOOL_ITER) {
        final_text = strdup("我已经开始处理这个问题，但工具调用轮次达到上限，没能整理出最终答复。请把问题再缩小一点，或者分步问我。");
      } else if (last_llm_err != ESP_OK) {
        char llm_error[192] = {0};
        char err_text[320];
        if (llm_get_last_error(llm_error, sizeof(llm_error))) {
          snprintf(err_text, sizeof(err_text),
                   "模型服务这次调用失败了：%s。请稍后重试。",
                   llm_error);
        } else {
          snprintf(err_text, sizeof(err_text),
                   "模型服务这次调用失败了：%s。请稍后重试。",
                   esp_err_to_name(last_llm_err));
        }
        final_text = strdup(err_text);
      }
    }

    /* 5. Send response */
    if (final_text && final_text[0]) {
      if (proactive_turn && text_is_proactive_no_message(final_text)) {
        ESP_LOGI(TAG, "Proactive check chose no message");
        free(final_text);
        final_text = NULL;
        free(msg.content);
        (void)tool_status_indicator_thinking_stop();
        ESP_LOGI(TAG, "Free PSRAM: %d bytes",
                 (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        continue;
      }

      /* Save to session (only user text + final assistant text) */
      esp_err_t save_user =
          (proactive_turn || internal_result_turn) ? ESP_OK : session_append(msg.chat_id, "user", msg.content);
      esp_err_t save_asst = session_append(msg.chat_id, "assistant", final_text);
      if (internal_result_turn && agent_should_persist_trace()) {
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
      if (agent_should_persist_trace()) {
        (void)session_append_trace(msg.chat_id, "final_reply",
                                   "Assistant final reply", out.content);
      }
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
        if (agent_should_persist_trace()) {
          (void)session_append_trace(msg.chat_id, "final_error",
                                     "Assistant error reply", out.content);
        }
        if (message_bus_push_outbound(&out) != ESP_OK) {
          ESP_LOGW(TAG, "Outbound queue full, drop error response");
          free(out.content);
        }
      }
    }

    /* Free inbound message content */
    free(msg.content);
    (void)tool_status_indicator_thinking_stop();

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
  const uint32_t coordinator_stack_candidates[] = {
      24 * 1024, 20 * 1024, 18 * 1024, 16 * 1024,
  };
  const uint32_t default_stack_candidates[] = {
      ESPAGENT_AGENT_STACK, 20 * 1024, 16 * 1024, 14 * 1024, 12 * 1024,
  };

  const uint32_t *stack_candidates = default_stack_candidates;
  size_t stack_candidate_count =
      sizeof(default_stack_candidates) / sizeof(default_stack_candidates[0]);

  if (espagent_role_is_coordinator()) {
    stack_candidates = coordinator_stack_candidates;
    stack_candidate_count = sizeof(coordinator_stack_candidates) /
                            sizeof(coordinator_stack_candidates[0]);
  }

  for (size_t i = 0; i < stack_candidate_count; i++) {
    uint32_t stack_size = stack_candidates[i];
    BaseType_t ret =
        xTaskCreatePinnedToCore(agent_loop_task, "agent_loop", stack_size, NULL,
                                ESPAGENT_AGENT_PRIO, NULL,
                                ESPAGENT_AGENT_CORE);

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

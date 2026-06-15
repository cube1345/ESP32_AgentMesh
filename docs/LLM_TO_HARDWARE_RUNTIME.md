# ESPAgent 从 LLM 请求到硬件执行的实现链路

本文基于当前工程代码说明 ESPAgent 是如何把聊天消息交给 LLM，再把 LLM 返回的结构化调用落到硬件上的。项目不是把 LLM 返回的一段普通文字用关键词硬解析成动作，而是把硬件能力注册成 tool，让 LLM 在需要时返回 tool call，固件再按工具名调用对应的 C 函数。

项目启动入口在 `main/espagent.c`。`app_main()` 先初始化 SPIFFS、消息总线、memory、cache、skills、LLM 和工具注册表。关键代码在 `main/espagent.c:237-248`：

```c
ESP_ERROR_CHECK(message_bus_init());
ESP_ERROR_CHECK(memory_store_init());
ESP_ERROR_CHECK(cache_store_init());
ESP_ERROR_CHECK(skill_loader_init());
ESP_ERROR_CHECK(session_mgr_init());
ESP_ERROR_CHECK(wifi_manager_init());
ESP_ERROR_CHECK(http_proxy_init());
ESP_ERROR_CHECK(feishu_bot_init());
ESP_ERROR_CHECK(feishu_bot_init());
ESP_ERROR_CHECK(llm_proxy_init());
ESP_ERROR_CHECK(tool_registry_init());
```

这里的顺序很重要。`cache_store_init()` 在 `skill_loader_init()` 前面，因为 skills 的摘要会用 cache 缓存。`tool_registry_init()` 会把所有可以让 LLM 调用的能力注册进去，例如 GPIO、RGB 灯、舵机、SGP30、GY-30、人体传感器、文件读写等。

这些模块被编进固件的位置在 `main/CMakeLists.txt`。`cache/cache_store.c` 在 `main/CMakeLists.txt:12`，`agent/context_builder.c` 在 `main/CMakeLists.txt:11`，`tools/tool_registry.c` 在 `main/CMakeLists.txt:22`，`skills/skill_loader.c` 在 `main/CMakeLists.txt:40`，`espnow/espnow_sender.c` 在 `main/CMakeLists.txt:20`。也就是说，cache、skills、Agent、工具系统和 ESP-NOW 都属于 `main` 组件，随固件一起编译。

SPIFFS 的初始化发生在 `init_spiffs()` 之后，`app_main()` 在 `main/espagent.c:235` 调用它。skills、memory、cron、heartbeat 等文件都依赖 `/spiffs` 挂载成功。cache 本身不依赖 SPIFFS，因为它是 RAM 中的 KV 缓存，但 skills summary 的内容来自 SPIFFS，所以实际运行时必须先有 SPIFFS，再能扫描 skills 文件。

飞书接入在 `main/channels/feishu/feishu_bot.c`。项目使用飞书 WebSocket 模式。WebSocket 收到事件后，`feishu_handle_ws_frame()` 在 `feishu_bot.c:541-570` 解析 frame，并把事件 JSON 交给 `feishu_process_ws_event_json()`。真正处理文本消息的是 `handle_message_event()`，位置在 `feishu_bot.c:688`。它从飞书事件里取出 `chat_id`、`message_type` 和 `content`，在 `feishu_bot.c:720-732` 解析飞书消息里的文本内容，然后在 `feishu_bot.c:772-779` 构造 `espagent_msg_t` 并推入消息总线：

```c
espagent_msg_t msg = {0};
strncpy(msg.channel, ESPAGENT_CHAN_FEISHU, sizeof(msg.channel) - 1);
strncpy(msg.chat_id, route_id, sizeof(msg.chat_id) - 1);
msg.content = strdup(cleaned);

if (msg.content) {
    if (message_bus_push_inbound(&msg) != ESP_OK) {
        ESP_LOGW(TAG, "Inbound queue full, dropping feishu message");
        free(msg.content);
    }
}
```

消息总线本身很简单，在 `main/bus/message_bus.c`。`message_bus_push_inbound()` 位于 `message_bus.c:25-32`，内部就是把消息放进 FreeRTOS queue。Agent 侧再通过 `message_bus_pop_inbound()` 从队列里取消息，代码在 `message_bus.c:34-41`。

飞书回复的方向也经过同一个总线。Agent 生成最终回复后，会在 `agent_loop.c:535-544` 构造 outbound 消息，并调用 `message_bus_push_outbound()`。真正发回飞书的位置在 `main/espagent.c:195-201`。这里根据 `msg.channel` 判断是飞书消息，就调用 `feishu_send_message(msg.chat_id, msg.content)`。因此飞书通道只负责收发消息，不直接碰硬件；硬件动作统一交给 Agent 的 tool 执行链路。

Agent 主循环在 `main/agent/agent_loop.c`。`agent_loop_task()` 在 `agent_loop.c:408` 开始运行。它在 `agent_loop.c:423` 先取出工具定义 JSON：

```c
const char *tools_json = tool_registry_get_tools_json();
```

然后在 `agent_loop.c:427` 等待用户消息：

```c
espagent_msg_t msg;
esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
```

拿到消息后，Agent 在 `agent_loop.c:436` 调用 `context_build_system_prompt()` 构建 system prompt，在 `agent_loop.c:444-451` 把历史消息和当前用户消息整理成 `messages`。真正请求 LLM 的位置是 `agent_loop.c:477-478`：

```c
llm_response_t resp;
err = llm_chat_tools(system_prompt, messages, tools_json, &resp);
```

Agent 这里采用多轮工具调用流程。`agent_loop.c:458` 开始的 while 循环会最多执行 `ESPAGENT_AGENT_MAX_TOOL_ITER` 次。每一轮都会请求 LLM，如果 LLM 没有返回工具调用，就把普通文本作为最终回复；如果 LLM 返回了工具调用，就执行工具，把工具结果作为下一条 user 消息重新放回 `messages`，再请求一次 LLM 生成最终回答。这个流程在 `agent_loop.c:502-513` 很清楚：先把 LLM 的 tool use 作为 assistant message 加进去，再用 `build_tool_results()` 生成 tool result，再把 result 作为 user message 加进去。

`build_tool_results()` 在 `agent_loop.c:365-405`。它遍历 `resp->calls`，每个 call 里有 `name` 和 `input`。在执行工具前，`agent_loop.c:373` 会调用 `patch_tool_input_with_context()`，这个函数会给需要上下文的工具补充 channel 或 chat_id，例如 cron 任务需要知道从哪个聊天通道回复。然后 `agent_loop.c:379` 会走 `tool_guard_check()`，用于阻止一些不合适的工具调用。通过检查后，`agent_loop.c:392` 才真正执行工具。

LLM 请求的构造在 `main/llm/llm_proxy.c`。`llm_chat_tools()` 位于 `llm_proxy.c:550`。如果使用 OpenAI 风格接口，代码会在 `llm_proxy.c:568-576` 把 messages、tools 和 `tool_choice=auto` 放进请求体：

```c
cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
cJSON_AddItemToObject(body, "messages", openai_msgs);

if (tools_json) {
    cJSON *tools = convert_tools_openai(tools_json);
    if (tools) {
        cJSON_AddItemToObject(body, "tools", tools);
        cJSON_AddStringToObject(body, "tool_choice", "auto");
    }
}
```

LLM 返回后，`llm_proxy.c:629-630` 解析 HTTP 返回的 JSON。OpenAI 风格的 `tool_calls` 在 `llm_proxy.c:659-687` 解析，工具名来自 `function.name`，参数来自 `function.arguments`。也就是说，LLM 返回的不是“把灯打开”这种自然语言，而是类似 `set_status_light({"color":"red"})` 这样的结构化调用。

工程也兼容另一种 content block 风格的 tool use。非 OpenAI 分支在 `llm_proxy.c:691` 之后处理，`llm_proxy.c:731-759` 会遍历 `content` 数组，找到 `type == "tool_use"` 的 block，然后读取 `id`、`name` 和 `input`。其中 `input` 会通过 `cJSON_PrintUnformatted()` 转成字符串，存入 `call->input`。这样 Agent 后面不用关心 LLM 供应商差异，只需要读取统一的 `llm_response_t`。

`llm_response_t` 的定义在 `main/llm/llm_proxy.h`。它里面有普通文本 `text`，也有工具调用数组 `calls[ESPAGENT_MAX_TOOL_CALLS]`。每个 `llm_tool_call_t` 保存 `id`、`name`、`input` 和 `input_len`。所以项目内部从 LLM 返回到工具执行之间，已经从外部 API 的 JSON 格式转换成了自己的 C 结构体。

工具的注册表在 `main/tools/tool_registry.c` 和 `main/tools/tool_registry.h`。`espagent_tool_t` 的定义在 `tool_registry.h:7-13`：

```c
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    esp_err_t (*execute)(const char *input_json, char *output, size_t output_size);
} espagent_tool_t;
```

每个工具都有名字、描述、输入 schema 和一个真正执行的函数指针。`tool_registry_init()` 位于 `tool_registry.c:65`。例如 `gpio_write` 在 `tool_registry.c:192-200` 注册，`set_status_light` 在 `tool_registry.c:235-247` 注册，`servo_write` 在 `tool_registry.c:250-259` 注册，`read_environment` 在 `tool_registry.c:104-119` 注册。注册完成后，`build_tools_json()` 在 `tool_registry.c:41-63` 把工具表转换成 JSON，后面发给 LLM。

工具描述对 LLM 的选择很关键。比如 `set_status_light` 的描述在 `tool_registry.c:236-237` 写明了 “Prefer this when the user asks to turn the board light red, green, blue...” 这类自然语言触发方式。`read_environment` 的描述在 `tool_registry.c:105-106` 明确写了 AHT20、SGP30、GY-30 组合测试，以及中文短语“综合测试”“环境数据”“温湿度空气质量光照”。这些描述会被放进 `tools_json`，所以 LLM 能根据用户话语选择合适的工具。

工具 schema 限制了 LLM 可以生成哪些参数。比如 `gpio_write` 的 schema 在 `tool_registry.c:195-199`，要求 `pin` 和 `state`；`servo_write` 的 schema 在 `tool_registry.c:253-258`，允许 `angle` 或 `pulse_us`；`read_environment` 的 schema 在 `tool_registry.c:107-118`，允许覆盖 AHT20、SGP30、GY-30 的 SDA/SCL 引脚。LLM 生成的参数仍然会在 C 函数里再次检查，schema 不是唯一保护。

LLM 决定调用工具后，Agent 在 `agent_loop.c:496-499` 打印 LLM 要调用的工具，然后在 `agent_loop.c:508-509` 进入工具执行流程。真正执行的位置在 `agent_loop.c:391-392`：

```c
tool_registry_execute(call->name, tool_input, tool_output, tool_output_size);
```

`tool_registry_execute()` 位于 `tool_registry.c:365-378`。它按工具名查找注册表，找到后调用对应的 `execute()` 函数：

```c
if (strcmp(s_tools[i].name, name) == 0) {
    ESP_LOGI(TAG, "Executing tool: %s", name);
    return s_tools[i].execute(input_json, output, output_size);
}
```

硬件动作就在这些 `tool_xxx_execute()` 里完成。比如普通 GPIO 输出在 `main/tools/tool_gpio.c`。`tool_gpio_write_execute()` 位于 `tool_gpio.c:336`，它先用 cJSON 解析 LLM 给的 JSON 参数，在 `tool_gpio.c:344-360` 取出 `pin` 和 `state`，然后在 `tool_gpio.c:362` 做 GPIO 安全检查，在 `tool_gpio.c:368-370` 配置并输出电平：

```c
err = ensure_output_gpio(pin);
if (err == ESP_OK) {
    err = gpio_set_level(pin, state);
}
```

GPIO 的安全检查不是直接写在 `tool_gpio_write_execute()` 里，而是通过 `validate_allowed_gpio()` 和 `gpio_policy_pin_is_allowed()` 完成。`validate_allowed_gpio()` 在 `tool_gpio.c:85-101`，它会检查该 pin 是否在允许范围内。如果不允许，会返回错误文本。这样 LLM 即使生成了危险引脚，工具层也不会直接执行。

`ensure_output_gpio()` 在 `tool_gpio.c:103-118`，它会用 `GPIO_IS_VALID_OUTPUT_GPIO()` 检查这个 pin 是否真的能作为输出，然后调用 `gpio_config()` 设置为 `GPIO_MODE_OUTPUT`。所以一个 GPIO 写入请求至少要经过三步：LLM 生成 JSON，工具解析并检查 pin，最后 ESP-IDF GPIO 驱动输出电平。

RGB 状态灯也是工具。`tool_set_status_light_execute()` 位于 `tool_gpio.c:556`，它支持 `red`、`green`、`blue`、`off` 等颜色名。颜色解析在 `tool_gpio.c:578-584`，真正写 WS2812 的函数是 `ws2812_apply_color()`。底层用的是 ESP-IDF RMT，`rmt_transmit()` 在 `tool_gpio.c:254` 被调用。

RGB 灯工具有两层入口。`ws2812_set` 是低层工具，要求 LLM 给出 `r/g/b` 数字，入口在 `tool_gpio.c:527`。`set_status_light` 是更适合自然语言的入口，入口在 `tool_gpio.c:556`，它可以接收 `color` 字符串。颜色名到 RGB 的转换在 `resolve_named_color()`，位置是 `tool_gpio.c:283-328`，里面支持英文和中文颜色名，例如 `red`、`红色`、`off`、`关闭`。转换完成后还是调用同一个 `ws2812_apply_color()`。

WS2812 的 RMT 初始化在 `ws2812_ensure_ready()`，位置是 `tool_gpio.c:177-224`。它会创建 RMT TX channel、创建 encoder，然后 `rmt_enable()`。发送时会把 RGB 转成 WS2812 需要的 GRB 顺序，代码在 `tool_gpio.c:245-249`。这说明 LLM 不需要知道 WS2812 的时序，也不需要知道 GRB 顺序，只需要选择工具并给出颜色。

舵机控制在 `main/tools/tool_servo.c`。`tool_servo_write_execute()` 位于 `tool_servo.c:99`。它可以接收 `angle` 或 `pulse_us`。如果传入角度，`tool_servo.c:118-120` 会把角度换算成 PWM 脉宽，然后调用 `tool_servo_set_pulse_us()`。底层在 `tool_servo.c:69` 调用 `ledc_set_duty()`，在 `tool_servo.c:76` 调用 `ledc_update_duty()`。

舵机的 PWM 初始化在 `servo_ensure_ready()`，位置是 `tool_servo.c:19-55`。它配置 LEDC timer，频率来自 `ESPAGENT_SERVO_FREQ_HZ`，通道固定为 `LEDC_CHANNEL_0`，GPIO 使用 `ESPAGENT_SERVO_DEFAULT_GPIO`。`tool_servo_set_angle()` 在 `tool_servo.c:88-97`，它把 0-180 度映射到 `ESPAGENT_SERVO_MIN_PULSE_US` 和 `ESPAGENT_SERVO_MAX_PULSE_US` 之间。这里也体现了项目的做法：LLM 只负责说 angle，硬件细节由工具函数处理。

传感器读取也是同样的结构。SGP30 的工具入口在 `main/tools/tool_sgp30.c:54`，函数名是 `tool_sgp30_read_air_quality_execute()`。它在 `tool_sgp30.c:66-74` 读取 SDA、SCL、I2C port 等参数，在 `tool_sgp30.c:91` 初始化 SGP30，在 `tool_sgp30.c:106` 调用 `sgp30_read_air_quality()`。底层 I2C 驱动在 `main/drivers/sgp30.c`，其中 `sgp30.c:52` 调 `i2c_master_transmit()`，`sgp30.c:59` 调 `i2c_master_receive()`。

SGP30 驱动层还做了 CRC 检查。`sgp30_measure_once()` 在 `main/drivers/sgp30.c:44-71`，它发送 `SGP30_CMD_MEASURE_IAQ`，等待 20 ms，再读取 6 字节返回值。`sgp30.c:64-66` 会检查两个 word 的 CRC，如果失败就返回 `ESP_ERR_INVALID_CRC`。工具层拿到错误后，在 `tool_sgp30.c:107-110` 把错误转成文字返回给 Agent。

综合环境读取在 `main/tools/tool_environment.c`。给 LLM 调用的工具入口是 `tool_read_environment_execute()`，位置在 `tool_environment.c:689`。它会解析可选 JSON 参数，默认使用 `espagent_config.h` 里的 AHT20、SGP30、GY-30 引脚配置。AHT20 的读取在 `tool_environment.c:594-621`，SGP30 的读取在 `tool_environment.c:624-668`，GY-30/BH1750 的读取在 `tool_environment.c:670-687`。这个工具返回一整段状态文本，适合用户问“综合测试”“读取全部环境数据”时使用。

后来为了 ESP-NOW 定时发送，又加了结构化读取接口 `tool_environment_read_values()`，位置在 `tool_environment.c:795`。这个函数和 LLM tool 不完全一样，它不是返回一句自然语言，而是填充 `tool_environment_values_t`。默认值在 `tool_environment.c:806-814` 设置为 `-1`。每个传感器独立尝试读取，失败就保持 `-1`，成功才写入真实值。这是为了让 ESP-NOW 接收端更容易解析。

人体传感器在 `main/tools/tool_hc_sr05.c`。`tool_read_presence_execute()` 位于 `tool_hc_sr05.c:197`。如果是三线 OUT 人体传感器，会走 `read_digital_presence()`，在 `tool_hc_sr05.c:91` 用 `gpio_get_level()` 读取 OUT 引脚。如果是 HC-SR05 超声波模式，则在 `tool_hc_sr05.c:167-171` 拉高 Trig 发脉冲，在 `tool_hc_sr05.c:177-183` 计算 Echo 高电平时间。

人体传感器工具也有两种入口。`read_presence` 是高层工具，适合“有人吗”“人体传感器”“有人靠近吗”这类请求。`hc_sr05_read_distance` 是低层工具，适合明确要求 HC-SR05 距离或 Trig/Echo 调试。注册位置分别在 `tool_registry.c:122-133` 和 `tool_registry.c:136-146`。高层工具内部会先判断是否有 OUT 引脚，如果有就读数字输入；如果没有 OUT 但有 Trig/Echo，就切到超声波读取。

cache 机制在 `main/cache/cache_store.c`。它是内存里的 KV cache，不是持久化文件缓存。结构体在 `cache_store.c:18-26`，里面有 key、value、过期时间、最近访问时间和命中次数。初始化函数 `cache_store_init()` 在 `cache_store.c:143-157`，主要创建 mutex 并打印容量。容量配置在 `main/espagent_config.h:224-230`，默认最多 32 个条目、单值 4096 字节、总大小 24KB、默认 TTL 300 秒。

cache 的 value 优先分配到 PSRAM。`alloc_value()` 在 `cache_store.c:58-65`，先调用 `heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`，如果失败再退回普通 `malloc()`。这适合 ESP32-S3 这类带 PSRAM 的板子，可以减少内部 RAM 压力。

读取 cache 的函数是 `cache_get()`，在 `cache_store.c:159-197`。它会查 key，检查是否过期，命中后复制 value 并更新 hits。写入函数是 `cache_put()`，在 `cache_store.c:199-256`。如果容量不够，会调用 `evict_until_fits()`，位置在 `cache_store.c:131-140`，优先清过期项，然后按最近访问时间淘汰旧数据。

cache 删除有两种方式。`cache_delete()` 在 `cache_store.c:258-274`，按完整 key 删除。`cache_delete_prefix()` 在 `cache_store.c:276-291`，按前缀删除一批 key。调试时可以通过 CLI 看 cache 状态，`cache_stats` 命令对应 `main/cli/serial_cli.c:310-331`，会打印 entries、bytes、hits、misses、evictions、expired 和 truncated。`cache_dump()` 在 `cache_store.c:319-377`，可以列出每个 key 的大小、剩余 TTL、命中次数和最近访问时间。

skills 机制在 `main/skills/skill_loader.c`。skills 是 SPIFFS 里的 Markdown 文件，路径由 `main/espagent_config.h:243` 定义：

```c
#define ESPAGENT_SKILLS_PREFIX ESPAGENT_SPIFFS_BASE "/skills/"
```

启动时 `skill_loader_init()` 在 `skill_loader.c:18-41` 扫描 SPIFFS，统计 `skills/*.md` 文件数量。这里不会把所有 skill 全文塞进 prompt，只是确认可用。

skills 文件会随 SPIFFS 数据一起烧录，也可以运行时通过 `write_file` 写入。`skill_loader_init()` 打开的是 `ESPAGENT_SPIFFS_BASE`，也就是 `/spiffs`，然后匹配目录项名字是否以 `skills/` 开头、是否以 `.md` 结尾。工程这里没有递归扫描多级目录，只匹配 SPIFFS 返回的 `skills/xxx.md` 这种路径。

真正进入 LLM 上下文的是 skills summary。`context_build_system_prompt()` 在 `main/agent/context_builder.c:34`。它在 `context_builder.c:108-111` 告诉 LLM skills 放在 `/spiffs/skills/`，需要时用 `read_file` 读取全文。然后在 `context_builder.c:130-137` 调用 `skill_loader_build_summary()`，把技能摘要加入 system prompt：

```c
char skills_buf[2048];
size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
if (skills_len > 0) {
    off += snprintf(buf + off, size - off,
                    "\n## Available Skills\n\n"
                    "Available skills (use read_file to load full instructions):\n%s\n",
                    skills_buf);
}
```

`skill_loader_build_summary()` 位于 `skill_loader.c:166-186`。它先在 `skill_loader.c:172` 查 cache，key 是 `prompt:skills_summary`。如果没命中，就调用 `build_summary_uncached()`，位置在 `skill_loader.c:103-164`。这个函数扫描 `skills/*.md`，读取每个文件第一行作为标题，再读取后面的描述，最后生成类似这样的摘要：

```text
- **某个技能**: 技能描述 (read with: read_file /spiffs/skills/xxx.md)
```

生成完成后，`skill_loader.c:180` 会把 summary 写入 cache，TTL 使用 `ESPAGENT_CACHE_SKILLS_TTL_S`，也就是 24 小时。这样每轮对话构建 prompt 时不用反复扫 SPIFFS。

summary 的提取规则也在代码里。`extract_title()` 位于 `skill_loader.c:50-66`，它期望第一行是 `# Title`，会去掉前面的 `# `。`extract_description()` 位于 `skill_loader.c:71-101`，它从第二行开始读取描述，遇到空行或二级标题 `##` 就停止。因此一个 skill 文件开头最好写成：

```md
# 技能名称
这一段是简短描述，会进入 system prompt 的 skills summary。

## 详细说明
这里是完整使用说明，需要 LLM 通过 read_file 读取后才会看到。
```

这样做的原因是控制 prompt 大小。system prompt 里只放所有 skills 的摘要和路径，不放全文。只有当 LLM 判断任务匹配某个 skill 时，才会使用 `read_file` 工具读取 `/spiffs/skills/xxx.md` 全文。`read_file` 工具的入口在 `main/tools/tool_files.c:43-75`，路径检查在 `tool_files.c:21-31`，要求路径必须以 `/spiffs` 开头并且不能包含 `..`。

如果用户通过工具修改了 skill 文件，项目会自动让 skills cache 失效。逻辑在 `main/tools/tool_files.c:34-39`。当路径以 `/spiffs/skills/` 开头时，会调用 `skill_loader_invalidate_cache()`。`write_file` 成功后在 `tool_files.c:120` 调用这个失效逻辑，`edit_file` 成功后在 `tool_files.c:224` 也会调用。真正删除缓存 key 的位置是 `skill_loader.c:188-191`。

串口 CLI 也能查看 skills。`skill_loader_build_summary()` 被 CLI 的 `skill_list` 命令调用，代码在 `main/cli/serial_cli.c:451-464`。`skill_show` 可以按名字打开某个 skill 文件，路径构造在 `serial_cli.c:479-490`，读取和打印在 `serial_cli.c:493-520`。`skill_search` 在 `serial_cli.c:545-610`，会在 skills 文件名和内容里搜索关键字。这些 CLI 命令主要用于开发和现场调试。

ESP-NOW 用于把设备采集的数据发给另一块 ESP。代码在 `main/espnow/espnow_sender.c`。发送端初始化在 `espnow_sender.c:70`。为了不依赖路由器连接，`ensure_wifi_radio_started()` 在 `espnow_sender.c:30-68` 会先确保 WiFi radio 启动，并默认尝试设为 channel 1。广播 peer 在 `espnow_sender.c:100-125` 添加，目标 MAC 是 `FF:FF:FF:FF:FF:FF`。真正发送在 `espnow_sender.c:127-163`，格式是：

```text
topic:text
```

`espnow_sender_init()` 会先调用 `ensure_wifi_radio_started()`。如果 WiFi mode 是 `WIFI_MODE_NULL`，就设成 `WIFI_MODE_STA`；如果当前是 AP，就切到 `WIFI_MODE_APSTA`。随后调用 `esp_wifi_start()`，再尝试 `esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)`。这意味着它不要求设备已经连上路由器，但接收端必须在同一信道上。

ESP-NOW 初始化后会注册发送回调，回调函数是 `espnow_send_cb()`，位置在 `espnow_sender.c:22-28`。当前 ESP-IDF 6.1 的签名是 `const esp_now_send_info_t *tx_info`，不是旧版本的 `const uint8_t *mac_addr`。如果发送失败，回调会打印 warning。发送函数 `espnow_sender_send_text()` 会把 topic 和 text 拼成一个 payload，代码在 `espnow_sender.c:145-156`。如果超过 `ESP_NOW_MAX_DATA_LEN`，会截断并打印 warning。

环境数据的定时采集和发送在 `main/espagent.c` 的 `environment_monitor_task()`，函数从 `espagent.c:65` 开始。它在 `espagent.c:92` 调用 `tool_environment_read_values()` 读取 AHT20/SGP30/GY-30 数据，然后在 `espagent.c:95-102` 拼出固定字段：

```c
snprintf(payload, sizeof(payload),
         "co2=%d,temp_x10=%d,hum_x10=%d,lux_x10=%d,raw=%d,warmup=%d",
         values.co2eq_ppm,
         values.temperature_c_x10,
         values.humidity_percent_x10,
         values.light_lux_x10,
         values.light_raw,
         values.sgp30_warming_up ? 1 : 0);
```

随后在 `espagent.c:104` 调用：

```c
espnow_sender_send_text("env", payload);
```

所以 ESP-NOW 发出去的数据类似：

```text
env:co2=400,temp_x10=253,hum_x10=612,lux_x10=1234,raw=1481,warmup=0
```

如果某个传感器读取失败，`tool_environment_read_values()` 会把对应字段保留为 `-1`。这个默认值在 `main/tools/tool_environment.c:806-814` 设置。

这个环境监控任务不是由 LLM 触发的，而是上电后由 `app_main()` 创建。创建位置在 `main/espagent.c:261-264`：

```c
ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
    environment_monitor_task, "env_mon",
    5120, NULL, 4, NULL, 0) == pdPASS)
    ? ESP_OK : ESP_FAIL);
```

所以它会按照 `ESPAGENT_ENVIRONMENT_MONITOR_INTERVAL_MS` 定时读取并发送。当前 payload 使用整数表示小数，`temp_x10=253` 表示 25.3 摄氏度，`hum_x10=612` 表示 61.2%，`lux_x10=1234` 表示 123.4 lux。这样接收端不用处理浮点字符串，直接解析整数即可。

接收端如果是 ESP32-P4，需要注意一点：ESP32-P4 本体没有 WiFi 射频，必须使用带无线协处理器的 P4 板，或者外接一块 ESP32-C6/S3/C3 接收 ESP-NOW 后再转发给 P4。如果接收端本身有 ESP-NOW 能力，需要把 WiFi channel 设成 1，并注册 `esp_now_register_recv_cb()`。接收回调拿到的字符串会以 `env:` 开头，后面就是 `co2=...` 这些字段。

整体看，本项目的执行链路是：

```text
飞书/Feishu/WebSocket 收到用户文字
-> message_bus 放入 inbound queue
-> agent_loop 取消息并构建 prompt
-> llm_chat_tools 带 tools 请求 LLM
-> LLM 返回 tool_call
-> agent_loop 调 tool_registry_execute
-> tool_registry 按名字找 execute 函数
-> tool_xxx_execute 解析 JSON 参数
-> ESP-IDF GPIO/I2C/LEDC/RMT/ESP-NOW 驱动执行
```

如果用一句更贴近代码的话描述，就是：聊天通道只负责把文字转成 `espagent_msg_t`；Agent 负责把 `espagent_msg_t`、历史记录、system prompt 和工具表发给 LLM；LLM 返回 `llm_response_t`；Agent 把 `llm_response_t.calls[]` 交给 `tool_registry_execute()`；工具注册表按名字找到 `execute` 函数；具体工具再调用 GPIO、I2C、LEDC、RMT 或 ESP-NOW。

这个流程也解释了为什么新增硬件能力时一般要改三类代码。第一类是硬件实现，例如 `main/tools/tool_xxx.c` 或 `main/drivers/xxx.c`。第二类是工具注册，在 `main/tools/tool_registry.c` 加 name、description、schema 和 execute 函数。第三类是 prompt 指引，在 `main/agent/context_builder.c` 里告诉 LLM 什么时候应该使用这个工具。如果希望长期保存某个工具的使用方法，也可以写成 `/spiffs/skills/xxx.md`，让 skills 机制提供额外说明。

这种实现里，LLM 负责理解用户意图并选择工具，固件负责检查参数、调用驱动和返回结果。cache 用来减少重复构建上下文的成本，skills 用 Markdown 文件扩展 Agent 的操作说明，ESP-NOW 用来把采集到的数据发给其他 ESP 设备。

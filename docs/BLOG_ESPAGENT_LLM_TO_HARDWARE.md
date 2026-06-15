# ESPAgent：从 LLM 请求到硬件执行的一次完整链路

ESPAgent 是一个运行在 ESP32-S3 上的 AI Agent 项目。它不是单纯把聊天内容发给大模型，然后把大模型回复显示出来。它更重要的一点是：把设备上的硬件能力整理成工具，交给 LLM 选择。当 LLM 判断用户想读传感器、控制 GPIO、点亮 RGB 灯、转动舵机时，它返回的是结构化的工具调用。固件收到这个工具调用后，再执行真正的硬件驱动代码。

这篇文章按当前项目代码说明这条链路。主要会讲四部分：LLM 信息的请求、LLM 返回数据到硬件驱动的链路、Skills 与 cache 机制、飞书机器人的接入。

## 第一章：LLM 信息的请求

ESPAgent 的启动入口在 `main/espagent.c`。设备上电后，`app_main()` 会初始化一批基础模块，其中和 LLM 请求相关的主要是消息总线、cache、skills、会话记录、LLM 代理和工具注册表。对应代码在 `main/espagent.c` 里：

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

这段初始化说明了一个事实：LLM 请求不是孤立发生的。它前面有消息来源，旁边有上下文构建，后面还有工具注册表。`message_bus_init()` 负责准备消息队列，`session_mgr_init()` 负责历史记录，`skill_loader_init()` 负责加载技能摘要，`tool_registry_init()` 负责准备可以交给 LLM 的工具列表。

真正处理用户消息的是 `main/agent/agent_loop.c`。`agent_loop_task()` 会一直从消息总线里等待新消息：

```c
espagent_msg_t msg;
esp_err_t err = message_bus_pop_inbound(&msg, UINT32_MAX);
```

`espagent_msg_t` 里保存了三个核心字段：消息来自哪个通道、应该回复给哪个聊天对象、用户说了什么。例如飞书通道进来的消息，`channel` 会是 `feishu`，`chat_id` 是飞书里的会话或用户 ID，`content` 是用户发来的文本。

拿到用户消息后，Agent 会构建 system prompt：

```c
context_build_system_prompt(system_prompt, ESPAGENT_CONTEXT_BUF_SIZE);
append_turn_context_prompt(system_prompt, ESPAGENT_CONTEXT_BUF_SIZE, &msg);
```

`context_build_system_prompt()` 在 `main/agent/context_builder.c`。这里会把项目身份、可用工具说明、memory、skills summary 等内容拼成一段系统提示。`append_turn_context_prompt()` 会补充当前回合的信息，例如当前消息来自哪个 channel、chat_id 是什么。这样 LLM 不只是看到用户的一句话，也能知道自己运行在什么设备上、有哪些工具、当前应该往哪里回复。

随后 Agent 会加载当前会话历史。代码在 `agent_loop.c` 中调用：

```c
session_get_history_json(msg.chat_id, history_json,
                         ESPAGENT_LLM_STREAM_BUF_SIZE, ESPAGENT_AGENT_MAX_HISTORY);
```

历史记录会被解析成 cJSON 数组，然后当前用户消息会被追加进去：

```c
cJSON *user_msg = cJSON_CreateObject();
cJSON_AddStringToObject(user_msg, "role", "user");
cJSON_AddStringToObject(user_msg, "content", msg.content);
cJSON_AddItemToArray(messages, user_msg);
```

在请求 LLM 前，Agent 还会取出工具注册表生成的工具 JSON：

```c
const char *tools_json = tool_registry_get_tools_json();
```

最终请求 LLM 的函数是：

```c
err = llm_chat_tools(system_prompt, messages, tools_json, &resp);
```

这个函数位于 `main/llm/llm_proxy.c`。它会把 system prompt、messages 和 tools 组装成 HTTP 请求。如果当前使用的是 OpenAI 风格接口，代码会加入 `messages`、`tools`，并设置 `tool_choice` 为 `auto`：

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

这就是 ESPAgent 请求 LLM 的基本形式。它不是只发一句用户文本，而是发：系统提示、历史记录、当前用户输入、工具列表。LLM 看到这些信息后，可以选择直接回答，也可以返回工具调用。

## 第二章：LLM 返回数据到硬件驱动的链路

LLM 返回后，`llm_chat_tools()` 会解析返回 JSON。OpenAI 风格的 `tool_calls` 会被解析成项目内部的 `llm_response_t`。工具名来自 `function.name`，参数来自 `function.arguments`：

```c
cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
...
cJSON *name = cJSON_GetObjectItem(func, "name");
cJSON *args = cJSON_GetObjectItem(func, "arguments");
```

项目内部并不直接把原始 JSON 到处传。它会把返回内容整理成统一结构：普通回答放在 `resp.text`，工具调用放在 `resp.calls[]`。每个工具调用里有工具名和输入 JSON。这样后面的 Agent 逻辑不用关心 LLM 供应商返回格式，只要看 `resp.tool_use` 和 `resp.calls`。

如果 `resp.tool_use` 为 false，说明 LLM 没有要求调用工具，Agent 就把 `resp.text` 当作最终回复。如果 `resp.tool_use` 为 true，Agent 会进入工具执行流程。执行入口在 `main/agent/agent_loop.c` 的 `build_tool_results()`：

```c
tool_registry_execute(call->name, tool_input, tool_output,
                      tool_output_size);
```

这里的 `call->name` 是 LLM 返回的工具名，例如 `gpio_write`、`set_status_light`、`servo_write`、`read_air_quality`。`tool_input` 是 LLM 返回的 JSON 参数，例如：

```json
{"color":"red","brightness":80}
```

工具注册表在 `main/tools/tool_registry.c`。每个工具都有名字、描述、输入 schema 和执行函数。结构定义在 `main/tools/tool_registry.h`：

```c
typedef struct {
    const char *name;
    const char *description;
    const char *input_schema_json;
    esp_err_t (*execute)(const char *input_json, char *output, size_t output_size);
} espagent_tool_t;
```

注册工具时，项目会明确告诉 LLM 这个工具叫什么、能做什么、需要哪些参数。比如 `gpio_write` 的注册内容说明它用于设置 GPIO 高低电平，并要求输入 `pin` 和 `state`。`set_status_light` 的描述会告诉 LLM：当用户说把板载灯调成红色、绿色、蓝色或关闭时，优先使用这个工具。`servo_write` 的描述说明它控制 GPIO5 上的舵机，可以用角度或脉宽。

`tool_registry_execute()` 的逻辑很直接：按工具名查找注册表，找到后调用对应函数指针：

```c
if (strcmp(s_tools[i].name, name) == 0) {
    ESP_LOGI(TAG, "Executing tool: %s", name);
    return s_tools[i].execute(input_json, output, output_size);
}
```

也就是说，从 LLM 到硬件之间，最关键的转换点就是这里。LLM 只负责返回“调用哪个工具”和“传什么参数”，真正驱动硬件的是 `tool_xxx_execute()`。

以 GPIO 为例，工具入口是 `main/tools/tool_gpio.c` 里的 `tool_gpio_write_execute()`。它先解析 JSON：

```c
cJSON *root = cJSON_Parse(input_json);
```

然后取出 `pin` 和 `state`，检查 state 是否只能是 0 或 1。接着会检查这个 GPIO 是否允许被用户控制。检查通过后，才会配置输出并设置电平：

```c
err = ensure_output_gpio(pin);
if (err == ESP_OK) {
    err = gpio_set_level(pin, state);
}
```

这说明 LLM 不能绕过工具层直接写 GPIO。即使 LLM 生成了不合适的 pin，工具层也会检查并返回错误。

RGB 灯也是类似的流程。`set_status_light` 的入口是 `tool_set_status_light_execute()`。它可以接收 `color` 字符串，也可以接收 `r/g/b` 数值。如果是颜色名，会通过 `resolve_named_color()` 转成 RGB 值。真正发送 WS2812 数据的是 `ws2812_apply_color()`，底层使用 ESP-IDF 的 RMT：

```c
rmt_transmit(s_ws2812.channel, s_ws2812.encoder, rgb, sizeof(rgb), &tx_cfg);
```

舵机控制走 `main/tools/tool_servo.c`。LLM 可以返回 `{"angle":90}`，工具函数会把 90 度换算成 PWM 脉宽，再调用 LEDC 驱动：

```c
ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
```

传感器读取同样走工具。SGP30 的工具入口是 `tool_sgp30_read_air_quality_execute()`。它从 JSON 里读取可选的 SDA、SCL、I2C port，然后调用驱动层初始化并读取数据。底层驱动在 `main/drivers/sgp30.c`，核心读写是：

```c
i2c_master_transmit(dev->dev, cmd, sizeof(cmd), SGP30_XFER_TIMEOUT_MS);
i2c_master_receive(dev->dev, resp, sizeof(resp), SGP30_XFER_TIMEOUT_MS);
```

如果是综合环境数据，工具入口是 `tool_read_environment_execute()`，会一次读取 AHT20/AHT10 温湿度、SGP30 空气质量、GY-30/BH1750 光照。这个工具适合用户说“综合测试”“读取环境数据”“温湿度空气质量光照”这类请求。

Agent 执行完工具后，会把工具输出包装成 `tool_result`，再放回 messages 里给 LLM 看。也就是说，LLM 不是调用工具后就结束，而是会再看一次工具结果，然后生成适合用户阅读的最终回答。这样用户最终看到的是自然语言结果，而硬件动作已经在工具执行阶段完成了。

## 第三章：Skills 与 cache 机制

ESPAgent 的 skills 是存放在 SPIFFS 里的 Markdown 文件。路径由 `main/espagent_config.h` 定义：

```c
#define ESPAGENT_SKILLS_PREFIX ESPAGENT_SPIFFS_BASE "/skills/"
```

启动时，`skill_loader_init()` 会扫描 `/spiffs` 下的 `skills/*.md` 文件，统计当前安装了多少 skill。它不会在启动时把所有 skill 全文读进内存，也不会把所有全文都塞进 prompt。这样做是为了控制内存和 prompt 长度。

真正进入 system prompt 的是 skills summary。`context_build_system_prompt()` 会调用：

```c
size_t skills_len = skill_loader_build_summary(skills_buf, sizeof(skills_buf));
```

`skill_loader_build_summary()` 的工作方式是：先查 cache，看有没有已经生成过的 `prompt:skills_summary`。如果 cache 命中，就直接返回缓存内容。如果没有命中，就扫描 skills 目录，打开每个 Markdown 文件，提取标题和简介，再生成一份简短列表。

一个 skill 文件通常可以这样写：

```md
# 技能名称
这一段是简短描述，会进入 system prompt 的 skills summary。

详细说明：
这里写完整操作规则。只有当 LLM 需要时，才通过 read_file 读取全文。
```

标题提取函数是 `extract_title()`，它会读取第一行并去掉 `# `。简介提取函数是 `extract_description()`，它会读取标题后面的几行，遇到空行或 `##` 就停止。最后生成的 summary 类似：

```text
- **技能名称**: 简短描述 (read with: read_file /spiffs/skills/xxx.md)
```

这份 summary 会加入 system prompt。LLM 看到的是“有哪些 skill、每个 skill 大概做什么、需要时应该用 read_file 读取哪个文件”。如果任务真的匹配某个 skill，LLM 可以调用 `read_file` 工具读取完整内容。

cache 机制在 `main/cache/cache_store.c`。它是 RAM 里的 KV cache，不是文件缓存。每个 cache entry 里有 key、value、过期时间、最近访问时间和命中次数：

```c
typedef struct {
    bool used;
    char key[ESPAGENT_CACHE_MAX_KEY_BYTES];
    char *value;
    size_t value_len;
    int64_t expires_at_us;
    int64_t last_access_us;
    uint32_t hits;
} cache_entry_t;
```

cache 初始化函数是 `cache_store_init()`。它主要创建 mutex，并准备后续读写。缓存容量由 `main/espagent_config.h` 控制，当前默认是 32 个条目、单个 value 最大 4096 字节、总大小 24KB、默认 TTL 300 秒。skills summary 使用的 TTL 是 `ESPAGENT_CACHE_SKILLS_TTL_S`，也就是 24 小时。

读取 cache 用 `cache_get()`。它会检查 key 是否存在，是否过期。如果命中，会复制 value，并更新 `last_access_us` 和命中次数。写入 cache 用 `cache_put()`。如果容量不够，会清理过期项，或者按最近访问时间淘汰旧项。

skills 和 cache 的关系很明确：skills 文件放在 SPIFFS，summary 放在 cache。这样每一轮对话构建 prompt 时，不需要反复扫描 SPIFFS 和读取所有 skill 文件。

如果通过 `write_file` 或 `edit_file` 修改了 `/spiffs/skills/` 下的文件，项目会自动清掉 skills summary 的 cache。相关逻辑在 `main/tools/tool_files.c`：

```c
if (path && strncmp(path, ESPAGENT_SKILLS_PREFIX, strlen(ESPAGENT_SKILLS_PREFIX)) == 0) {
    skill_loader_invalidate_cache();
}
```

`skill_loader_invalidate_cache()` 内部会删除 `prompt:skills_summary`。下一次构建 system prompt 时，就会重新扫描 skills 文件并生成新的 summary。

## 第四章：飞书机器人的接入

飞书机器人接入在 `main/channels/feishu/feishu_bot.c`。项目使用的是飞书长连接 WebSocket 模式。启动时，`feishu_bot_init()` 会从 NVS 读取 `app_id` 和 `app_secret`。如果没有配置，会打印提示，让用户用串口命令设置：

```text
set_feishu_creds <APP_ID> <APP_SECRET>
```

`feishu_bot_start()` 会检查凭证是否存在。如果凭证存在，就创建 `feishu_ws_task`：

```c
xTaskCreatePinnedToCore(
    feishu_ws_task,
    "feishu_ws",
    ESPAGENT_FEISHU_POLL_STACK,
    NULL,
    ESPAGENT_FEISHU_POLL_PRIO,
    &s_ws_task,
    ESPAGENT_FEISHU_POLL_CORE);
```

`feishu_ws_task()` 会先向飞书接口拉取 WebSocket 配置，然后启动 `esp_websocket_client`。收到 WebSocket 数据后，事件处理函数 `feishu_ws_event_handler()` 会把分片数据拼起来，拼完后交给 `feishu_handle_ws_frame()`。

飞书发来的数据是一个带 frame 的事件。`feishu_handle_ws_frame()` 会解析 frame，确认是 event 类型后，把 payload 交给 `feishu_process_ws_event_json()`。如果事件类型是 `im.message.receive_v1`，就会进入 `handle_message_event()`。

`handle_message_event()` 会从飞书事件里取出消息体。它只处理文本消息。飞书文本内容本身还是一个 JSON 字符串，所以代码会再 parse 一次：

```c
cJSON *content_obj = cJSON_Parse(content_j->valuestring);
cJSON *text_j = cJSON_GetObjectItem(content_obj, "text");
```

拿到文本后，代码会处理群聊里可能带上的机器人 mention 前缀，然后决定 route id。单聊时，项目会使用发送者的 `open_id` 作为会话 ID；群聊时使用 `chat_id`。最后把消息包装成 `espagent_msg_t`：

```c
espagent_msg_t msg = {0};
strncpy(msg.channel, ESPAGENT_CHAN_FEISHU, sizeof(msg.channel) - 1);
strncpy(msg.chat_id, route_id, sizeof(msg.chat_id) - 1);
msg.content = strdup(cleaned);
```

然后推入 inbound 消息总线：

```c
message_bus_push_inbound(&msg);
```

从这里开始，飞书消息就进入了前面讲过的 Agent 流程：构建 prompt、请求 LLM、执行工具、生成回复。

回复飞书时，Agent 不会直接调用飞书接口。Agent 会把最终回复放进 outbound queue。`main/espagent.c` 里的 outbound dispatch 任务会根据 channel 判断应该发到哪里。如果 channel 是 `feishu`，就调用：

```c
feishu_send_message(msg.chat_id, msg.content);
```

`feishu_send_message()` 会判断 `chat_id` 是群聊 ID 还是 open_id。如果以 `ou_` 开头，就按 `open_id` 发送，否则按 `chat_id` 发送。发送前它会构造飞书消息体：

```c
cJSON_AddStringToObject(body, "receive_id", chat_id);
cJSON_AddStringToObject(body, "msg_type", "text");
cJSON_AddStringToObject(body, "content", content_str);
```

最后通过 `feishu_api_call()` 调用飞书 HTTP API。这个函数内部会先确保 tenant token 可用，再发 POST 请求。

飞书接入在整个项目里只负责通道功能：收消息、转成 `espagent_msg_t`、把回复发回飞书。它不会直接控制 GPIO，也不会直接读传感器。真正是否调用硬件，由 Agent 和 LLM tool calling 决定。这样的结构让飞书和 WebSocket 可以共用同一套 Agent 和工具系统。

完整来看，本项目的链路可以写成：

```text
飞书收到用户消息
-> feishu_bot.c 解析文本
-> message_bus_push_inbound()
-> agent_loop.c 取消息并构建上下文
-> llm_proxy.c 请求 LLM
-> LLM 返回普通文本或 tool call
-> tool_registry.c 按工具名分发
-> tool_gpio.c / tool_servo.c / tool_sgp30.c 等模块执行硬件操作
-> Agent 根据工具结果生成回复
-> message_bus_push_outbound()
-> feishu_send_message() 发回飞书
```

ESPAgent 的核心做法是把通道、LLM、工具和硬件驱动拆开。通道只管消息进出，LLM 只负责理解意图和选择工具，工具层负责参数检查和执行，硬件驱动负责真正操作设备。Skills 用来给 Agent 增加可扩展说明，cache 用来减少重复读取和重复构建上下文。整体结构不复杂，但每一层职责比较清楚，后续要加新的传感器或执行器时，也能沿着同样的方式接入。

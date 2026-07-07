# Feishu LLM Failure Troubleshooting

## 现象分层

`模型服务这次调用失败了：ERROR。请稍后重试。`

这类报错以前有两个常见来源，不能混为一谈：

1. 飞书消息已经进到 coordinator，但 coordinator 在组 prompt / skills / history 时 panic 或重启。
2. coordinator 本身没崩，真正失败点在 LLM HTTP 调用、上游模型返回非 200、返回体异常，最后被统一折叠成 `ESP_FAIL`。

这次两类问题都出现过。

## 本次实际根因

### 1. 第一层故障：skills 相关栈占用过大

在 `main/skills/skill_loader.c` 中，`skill_loader_build_index_text()`、`skill_loader_read_skill_by_name()`、`skill_loader_build_relevant_details()` 使用了较大的局部数组。  
coordinator 收到飞书消息后，走到 `History loaded` 之后继续拼 skills 上下文时，容易触发栈/内存破坏，最终 `Guru Meditation`。

处理方式：

- 把大块局部数组从 stack 挪到 heap
- 保持原有逻辑不变，只减小单次栈压力

### 2. 第二层故障：LLM 失败细节被吞掉

即使 panic 修好后，如果上游模型调用失败，`agent_loop` 之前只会把：

- HTTP 连接失败
- TLS/内存不足
- 上游 401/402/403/429/5xx
- JSON 解析失败

统一显示成：

- `模型服务这次调用失败了：ERROR。请稍后重试。`

这会让真实故障点被隐藏，导致看起来像“飞书又坏了”，但实际上常常是上游 LLM、网络或内存问题。

## 这次做的修复

### 代码修复

1. `main/skills/skill_loader.c`
   - 大数组改为 heap 分配，修 coordinator 因 skills/context 扩展导致的 panic。

2. `main/llm/llm_proxy.c`
   - 新增最近一次 LLM 失败摘要缓存。
   - 记录以下失败细节：
     - API key 为空
     - 请求体/响应缓冲区分配失败
     - TLS/HTTP transport 失败
     - 上游非 200 响应
     - 返回体非 JSON 或 JSON 截断

3. `main/agent/agent_loop.c`
   - LLM 调用失败时，优先把 `llm_proxy` 的最近一次真实失败摘要返回给飞书，而不是只回 `ERROR`。

4. `main/cli/serial_cli.c`
   - 新增 `llm_last_error` 命令，便于串口直接查看最近一次 LLM 失败摘要。

## 为什么 MCU 端更容易出现这类错误

### 1. RAM / stack 边界更紧

ESP32-S3 上即使带 PSRAM，很多 TLS、HTTP、JSON、日志、任务栈仍依赖 internal RAM。  
prompt、history、skills、tools 一旦一起变大，最先暴露的往往不是“慢”，而是异常重启或 HTTP 失败。

### 2. 错误链路跨层

一条飞书消息会经过：

- Feishu WebSocket / polling
- message bus
- agent loop
- context / history / skills
- HTTP/TLS
- 上游 LLM
- tool / mesh / outbound reply

任何一层失败，如果最后只映射成一个通用 `ESP_FAIL`，表面上就会变成“飞书不回复”。

### 3. 串口日志噪声大

当前系统同时还在刷：

- MQTT state
- telemetry
- timeline
- guardian/stateboard

如果没有单独的错误摘要接口，想从串口里肉眼抓住一次 LLM 真错误会比较费劲。

## 现在怎么排查

推荐顺序：

1. 先看是不是 panic / reboot
   - 串口是否出现 `Guru Meditation`、`Backtrace`、异常重启

2. 看 Wi-Fi 是否正常
   - 串口执行：`wifi_status`

3. 看最近一次 LLM 失败摘要
   - 串口执行：`llm_last_error`

4. 再区分失败类型
   - `LLM API key is empty`：未配置 key
   - `LLM transport failed: ...`：网络、TLS、内存或代理问题
   - `LLM upstream HTTP 401/403/...`：上游鉴权/额度/接口错误
   - `LLM returned non-JSON or truncated JSON response`：返回体损坏或协议不兼容

## 建议的实机调试命令

### 1. 看网络

```text
wifi_status
```

### 2. 看最近一次 LLM 错误

```text
llm_last_error
```

### 3. 本地注入一条消息绕过真实飞书输入

```text
inject_msg feishu debug_chat hello
```

如果这条能通，而真实飞书会话不通，说明飞书入口不是唯一问题，需要继续看：

- 真实 chat_id
- 飞书 outbound 参数
- 真实会话上下文大小
- 用户消息内容是否触发更大 prompt / tool 路径

## 本次验证结论

已验证：

1. 修复后的 coordinator 不再在 `History loaded` 后 panic。
2. 本地注入 `inject_msg feishu debug_chat hello` 可以成功调用 LLM 并生成回复。
3. 目前新增的 `llm_last_error` 可以用于后续定位真实上游失败。

注意：

- `debug_chat` 只是本地注入测试 chat_id，发送飞书回复时会因为 `invalid receive_id` 失败，这是正常现象，不代表 LLM 失败。
- 真实飞书会话仍应继续用真实 `chat_id` 复测。

## 后续建议

1. 保持 skills/context 相关代码尽量少用大栈对象。
2. 继续保留并扩展 `llm_last_error` 能力。
3. 后续可以再补一个 `llm_status`：
   - provider
   - model
   - key 是否存在
   - 最近一次 HTTP status
   - 最近一次失败时间

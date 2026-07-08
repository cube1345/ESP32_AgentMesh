# 前端 AI 通信入口现状

本文档说明当前 React 前端已经完成的“像飞书一样发送消息给 AI”的能力，以及 ESP32 端如果要进一步增强这条链路，还需要补哪些能力。

## 1. 当前前端已经完成的内容

当前前端已经新增一个 **AI 通信面板**，位置在页面右上角按钮：

```text
AI 通信
```

点击后会打开一个对话窗口，用于直接通过 ESP32 的 WebSocket 网关把消息送进 `agent_loop`。

### 1.1 当前前端支持的能力

前端现在已经支持：

- 输入 ESP32 WebSocket 地址
- 自定义 `chat_id`
- 手动连接 / 断开 WebSocket
- 查看连接状态
- 发送用户消息
- 接收 ESP32 返回的 AI 回复
- 在前端对话框里保留本次会话消息

### 1.2 当前前端复用的协议

前端没有自造协议，而是直接复用了当前 ESP32 已有的 WebSocket 协议。

发送格式：

```json
{"type":"message","content":"你好","chat_id":"web_console_01"}
```

返回格式：

```json
{"type":"response","content":"你好，我在。","chat_id":"web_console_01"}
```

这意味着当前前端已经可以在“通信入口”这件事上模拟飞书：

```text
前端 WebSocket
-> ESP32 ws_server
-> message_bus inbound
-> agent_loop
-> LLM / tools
-> outbound
-> WebSocket response
```

## 2. 当前前端这条链路的边界

虽然通信已经可用，但它目前仍然是一个“调试型 / 控制台型”的入口，不是完整 IM 客户端。

### 2.1 当前前端已经做到

- 能给 ESP32 发消息
- 能收到 AI 回复
- 能模拟一个像飞书一样的文本对话入口
- 能作为 dashboard 与 AI 通信的统一控制台入口

### 2.2 当前前端还没有做到

- 历史会话持久化
- 多会话切换
- 消息 ack / request_id 跟踪
- 工具调用过程的对话内展开
- 把 timeline 事件自动关联到某条聊天消息
- 文件/语音/图片消息
- 流式回复

也就是说，当前更像：

```text
Web 调试聊天终端
```

而不是：

```text
完整的飞书替代客户端
```

## 3. 当前 ESP32 端已经具备的能力

这条链路之所以能做，是因为 ESP32 侧已经具备以下能力。

### 3.1 已有 WebSocket Server

ESP32 端已有本地 WebSocket 网关：

- `main/gateway/ws_server.c`

它已经支持：

- 浏览器/前端连接
- 接收 JSON 消息
- 解析 `type=message`
- 推送到 `message_bus`
- 把 AI 回复再发回 WebSocket 客户端

### 3.2 已有消息总线与 agent_loop

当前 WebSocket 消息并不是特殊旁路，而是进入标准主链路：

- `message_bus`
- `agent_loop`
- `tool_registry`
- outbound

所以前端聊天与飞书聊天在核心处理路径上是一致的。

## 4. 当前 ESP32 端如果要继续增强，需要补什么

如果你希望这个前端通信入口变得更像飞书或更像完整上位机 IM 终端，ESP32 端建议补下面这些内容。

### 4.1 结构化消息 ID / trace_id 回传

当前前端虽然能收到回复，但如果要把：

- 用户消息
- tool_use
- tool_result
- mesh_command
- final_reply

串成一条完整链路，最好让 WebSocket 返回里明确带上：

- `msg_id`
- `trace_id`
- `task_id`

这样前端就能把聊天窗口和 timeline 关联起来。

### 4.2 WebSocket 侧主动推送 timeline

当前前端聊天收到的是 `response`，不是完整的实时事件流。

如果要更像飞书式“过程可见通信终端”，建议 ESP32 端补：

- WebSocket 主动推送 `tool_use`
- 主动推送 `tool_result`
- 主动推送 `mesh_command_result`
- 主动推送 `final_reply`

这样聊天窗口可以直接显示：

```text
用户消息
-> AI 开始思考
-> 调用工具
-> 下发控制
-> 收到结果
-> 最终回复
```

### 4.3 连接注册信息增强

当前 WebSocket 客户端主要靠：

- fd
- chat_id

跟踪连接。

如果后面前端终端变多，建议增加：

- `client_type`
- `device_id`
- `display_name`
- `session_id`

便于区分：

- Web 前端
- ESP32-P4
- Android

### 4.4 可选的流式回复

如果后面要更接近现代聊天产品体验，ESP32 端可以考虑：

- 支持分段 response
- 支持流式 token / chunk 推送

不过这不是当前第一优先级。

## 5. 当前结论

一句话总结：

### 前端现在已经可以做到

**像飞书一样给 AI 发文本消息，并接收 AI 回复。**

### 当前前端还只是

**一个基于 ESP32 WebSocket 网关的调试/控制台型聊天入口。**

### ESP32 当前已经足够支持

- 单轮消息发送
- AI 回复返回

### ESP32 如果还要增强

建议继续补：

- `trace_id/msg_id/task_id`
- timeline 主动推送
- 更丰富的客户端注册信息
- 可选流式回复

## 6. 推荐下一步

如果下一步继续只做“前端就能受益”的增强，我建议优先做：

### 聊天窗口与 timeline 联动

也就是：

- 发送一条消息后
- 自动在右侧同步显示相关 timeline
- 按 `trace_id` 或时间窗口进行归并

这样这个前端就不只是“能聊天”，而是真正开始像一个多 Agent 调度终端。

## 7. 当前 STT 新状态

当前语音输入已经分成两条链路：

### 7.1 浏览器麦克风链路

```text
浏览器 SpeechRecognition
-> frontend WebSocket
-> MQTT voice/stt/result
-> coordinator agent_loop
```

这条链路已经可用，适合快速演示。

### 7.2 ESP32 麦克风链路

```text
INMP441 / INA-class Mic
-> control_agent I2S capture
-> MQTT voice/stt/audio_chunk
-> frontend/server 音频重组
-> 上游云端 STT
-> MQTT voice/stt/result
-> coordinator agent_loop
```

这条链路已经完成了：

- `control_agent` 收到 `voice/stt/request` 后启动本地麦克风录音
- 录音以 `pcm_s16le` 分片方式通过 MQTT 发布
- `frontend/server/index.mjs` 已能重组分片并调用上游 STT
- 识别结果继续复用既有 `voice/stt/result` 回灌链

## 8. 启用 ESP32 麦克风 STT 需要的网关配置

当前 `frontend/server/index.mjs` 支持一个最小通用的 HTTP JSON STT 上游。

启动前设置环境变量：

```bash
export ESPAGENT_STT_PROVIDER=http_json
export ESPAGENT_STT_UPSTREAM_URL="https://your-stt-endpoint"
export ESPAGENT_STT_UPSTREAM_APP_ID="your_app_id"
export ESPAGENT_STT_UPSTREAM_TOKEN="your_token"
export ESPAGENT_STT_UPSTREAM_APP_ID_HEADER="X-Appid"
export ESPAGENT_STT_UPSTREAM_TOKEN_MODE="bearer"
export ESPAGENT_STT_UPSTREAM_TOKEN_HEADER="Authorization"
```

当前网关发给上游的请求体格式是：

```json
{
  "request_id": "stt-xxx",
  "format": "pcm_s16le",
  "channels": 1,
  "sample_rate_hz": 16000,
  "language": "zh-CN",
  "audio_b64": "base64..."
}
```

期望上游返回至少包含：

```json
{
  "transcript": "识别结果",
  "provider": "your_stt_provider"
}
```

如果后续你确认豆包/火山语音的真实 STT HTTP 或 WebSocket 协议，我再把这个通用 `http_json` 适配改成你当前账号的专用 provider。

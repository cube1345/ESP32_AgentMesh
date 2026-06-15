# WebSocket Chat Client

这是一个最小上位机网页示例，用浏览器直接连接 ESP32 上的 ESPAgent WebSocket 网关。

## 文件位置

- `assets/ws-chat-client/index.html`
- `assets/ws-chat-client/style.css`
- `assets/ws-chat-client/app.js`

## 现有后端能力

本仓库已经具备以下链路，无需再单独写 HTTP 聊天接口：

1. WebSocket 客户端发消息到 ESP32
   - 协议入口：`main/gateway/ws_server.c`
2. ESP32 把消息推入 inbound message bus
3. agent loop 调用大模型
   - 入口：`main/agent/agent_loop.c`
4. AI 回复进入 outbound message bus
5. WebSocket 通道把结果发回浏览器

当前协议：

```json
{"type":"message","content":"你好","chat_id":"pc_demo"}
```

返回：

```json
{"type":"response","content":"你好，我在。","chat_id":"pc_demo"}
```

## 使用方法

1. 先让 ESP32 连上 WiFi，并确认串口日志里显示本机 IP。
2. 在电脑上直接打开 `assets/ws-chat-client/index.html`。
3. 填入 ESP32 的 IP，默认端口 `18789`。
4. 点击“连接 ESP32”。
5. 输入文本并发送，或使用浏览器语音输入。

## 说明

- 这个页面是纯 HTML、CSS、JavaScript，不依赖任何前端框架。
- 语音输入依赖浏览器的 Web Speech API；如果浏览器不支持，仍然可以正常使用文本输入。
- 页面不会帮你配置 WiFi；WiFi 配置仍使用当前仓库已有的 onboarding / admin portal。

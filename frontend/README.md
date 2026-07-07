# ESPAgent Console

项目内新增的 React + TypeScript 前端控制台，面向 ESPAgent 多节点协同、环境数据、技能编辑、用户偏好和通信链路展示。

## 技术栈

- React
- TypeScript
- Vite
- Ant Design
- Tailwind CSS v4
- axios

## 启动

```bash
cd frontend
npm install
npm run server
npm run dev
```

默认开发地址：

```text
http://localhost:4173/
```

默认聚合服务地址：

```text
http://localhost:4175/
```

## 构建

```bash
npm run build
```

## 当前页面内容

- 多 Agent 节点总览
- 能力目录表
- 环境数据卡片
- 调度时间线与通信流程
- Skills Studio
- 用户偏好与策略配置

## 数据接入

当前使用 `axios` 请求 `/api/*`。开发模式下，Vite 会把 `/api` 代理到本地聚合服务；当接口不存在时会自动回退到本地 mock 数据。

前端聊天与控制入口现在也统一走本地 Gateway：

- 浏览器连接 `/ws`
- Vite 将 `/ws` 代理到本地聚合服务 `:4175`
- 本地聚合服务再转发到 ESP32 WebSocket 网关

这样页面不再直接依赖 `ws://<ESP32_IP>:18789/`。

预留接口：

- `GET /api/dashboard`
- `POST /api/skills`
- `POST /api/preferences`

## MQTT 真实数据聚合

`npm run server` 会启动一个轻量 Node 服务：

- 订阅 `${TOPIC_PREFIX}/nodes/+/telemetry`
- 订阅 `${TOPIC_PREFIX}/nodes/+/state`
- 订阅 `${TOPIC_PREFIX}/agent/timeline`
- 订阅 `${TOPIC_PREFIX}/guardian/stateboard`

可配置环境变量：

```bash
ESPAGENT_MQTT_HOST=broker.emqx.io
ESPAGENT_MQTT_PORT=1883
ESPAGENT_MQTT_PROTOCOL=mqtt
ESPAGENT_TOPIC_PREFIX=espagent/cube1345
ESPAGENT_DASHBOARD_PORT=4175
```

后续可以继续接：

- MQTT over WebSocket
- 原生 WebSocket timeline push
- ESP32-P4 / Android 终端共用的数据结构

## Web Chat Gateway

本地聚合服务除了 `HTTP /api/*` 外，还提供：

- `WS /ws`

默认行为：

1. 前端只连本地 `/ws`
2. 本地服务根据 `ESPAGENT_AGENT_WS_URL` 连接上游 ESP32
3. 浏览器消息原样转发到板端
4. 板端回复再回推给浏览器

可配置环境变量：

```bash
ESPAGENT_AGENT_WS_URL=ws://172.29.231.55:18789/
```

如果未显式配置，且 `ESPAGENT_SKILLS_API_BASE` 已设置为 `http://<ESP32_IP>`，服务会自动推导为：

```text
ws://<ESP32_IP>:18789/
```

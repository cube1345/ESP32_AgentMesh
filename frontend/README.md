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

如果要从同一局域网的其他设备通过 HTTPS 打开页面，可生成局域网自签名证书。

生成局域网自签名证书：

```bash
cd frontend
bash scripts/create-lan-cert.sh
```

启动 HTTPS 前端：

```bash
npm run dev:https
```

然后在其他设备打开脚本输出的地址，例如：

```text
https://<上位机IP>:4173/
```

第一次访问自签名证书时，浏览器会提示证书不受信任；按浏览器提示信任后即可继续访问。

默认聚合服务地址：

```text
http://localhost:4175/
```

## 构建

```bash
npm run build
```

## 当前页面内容

- 四角色实时拓扑与节点状态
- 环境指标、最近执行和能力边界
- 可筛选的消息与执行时间线
- Sandbox 策略、风险摘要和审计事件
- Skills Studio 草稿编辑与 Runtime 状态
- 用户偏好与策略配置
- 响应式 Agent 通信 Drawer

控制台采用桌面侧边导航和移动端横向导航。MQTT 实时数据、演示数据、
Runtime 串口不可用和通信网关空闲会使用不同状态提示，避免把 mock 或
不可用数据误认为真实设备状态。

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

## Runtime Skills Gateway

Skills Studio 的“安装到 Runtime”会把草稿转换为 Markdown，并写入板端：

```text
/spiffs/skills/<normalized-name>.md
```

默认上位机服务通过 MQTT Mesh 与 coordinator 通信，不需要 USB 串口连接：

```bash
npm run server
```

Runtime Skill 的列表、正文读取、安装、更新和删除均使用
`espagent/<mesh>/runtime/skills/request` 与 `runtime/skills/reply`。列表和正文采用
分页拉取，避免较长 Markdown 占满 coordinator 的 MQTT 发布队列。

MQTT 可配置环境变量：

```bash
ESPAGENT_SKILLS_MQTT_ENABLED=1
ESPAGENT_SKILLS_MQTT_TIMEOUT_MS=15000
ESPAGENT_SKILLS_MQTT_MAX_CONTENT_BYTES=1100
```

只有需要兼容旧固件时才显式启用串口回退：

```bash
ESPAGENT_SKILLS_SERIAL_ENABLED=1
ESPAGENT_SKILLS_SERIAL_PORT=/dev/ttyUSB0
ESPAGENT_SKILLS_SERIAL_TIMEOUT_MS=20000
ESPAGENT_SKILLS_SERIAL_LIST_CACHE_MS=30000
ESPAGENT_SKILLS_SERIAL_MAX_CONTENT_BYTES=4096
```

串口回退安装会执行等价命令：

```bash
python3 tools/serial_cmd.py /dev/ttyUSB0 'tool_exec write_file {"path":"/spiffs/skills/<name>.md","content":"...","confirmed":true}' --timeout 20
```

串口回退默认不启用。启用后，列表读取带 30 秒缓存，单个 Skill Markdown
写入限制为 4096 bytes，避免超长单行 JSON 命令给板端 console 带来额外内存压力。

如果需要外接独立 gateway，可设置：

```bash
ESPAGENT_SKILLS_API_BASE=http://<gateway-host>:<port>
```

此时前端服务会改为代理 `GET/POST <base>/api/skills`，不再直连串口。

# LingShu Agent Mesh

本文记录 ESPAgent 当前固件与后续多节点 Agent Mesh 方案的边界。

## 项目名称

灵枢 Agent Mesh

英文名：LingShu Agent Mesh

当前固件节点名：ESPAgent Edge Agent Node

## 关键字

ESP32-S3、Edge AI Agent、LLM Tool Calling、IoT、MQTT、ESP-NOW、Feishu、WebSocket、Amap Weather、Proactive Agent、Multi-Agent Collaboration、Smart Home、Sensor Fusion

## 应用领域

- 智能家居：灯光、风扇、继电器、加湿器、环境传感器联动。
- 环境监测：温湿度、空气质量、光照、人体存在、距离检测。
- 个人助理：天气提醒、日程提醒、主动问候、异常状态通知。
- 边缘智能控制：让 MCU 节点执行真实硬件动作，云端只做调度和扩展。
- 教学展示：展示 LLM 如何通过工具调用理解任务、读取传感器、控制硬件。

## 作品创意

灵枢 Agent Mesh 的核心想法是把“能聊天的 AI”推进到“能感知、能判断、能执行、能主动提醒”的边缘设备网络。

当前 ESP32-S3 固件已经实现单节点轻量 Agent runtime：用户可以通过飞书或 WebSocket 发消息，Agent 调用 LLM 判断是否需要工具，再通过 GPIO、传感器、天气、搜索、定时任务等工具完成动作。第一阶段 Mesh 改造把这个单节点明确为 Edge Agent Node，并通过 MQTT 发布节点状态、遥测和事件，为后续多节点协作打底。

规划中的完整系统不是简单复制多个聊天机器人，而是按职责拆分 Agent：

- Coordinator Agent：理解任务、拆解步骤、选择执行节点。
- Sensor Agent：负责温湿度、空气质量、光照、CO2、人体存在等数据采集。
- Control Agent：负责 GPIO、RGB、舵机、继电器、风扇、水泵等硬件控制。
- Memory Agent：负责长期记忆、用户偏好、会话摘要、skills 管理。
- Communication Agent：负责 Feishu、WebSocket、MQTT、MCP Gateway 等入口。
- Display Agent：运行在 ESP32-P4 或 Android 上，展示状态、工具调用和调度时间线。

## 当前已实现基础

当前固件运行在单 ESP32-S3 上，核心链路是：

```text
Feishu / WebSocket / Cron / Proactive
        |
        v
message_bus inbound queue
        |
        v
agent_loop
        |
        v
LLM tool calling
        |
        v
tool_registry
        |
        v
传感器 / GPIO / RGB / 舵机 / 文件 / 天气 / 搜索 / 定时任务
        |
        v
message_bus outbound queue
        |
        v
Feishu / WebSocket 回复
```

Phase 1 新增的 Mesh 能力：

- 编译期节点身份：`node_id`、`role`、`location`、`mesh_topic_prefix`。
- 编译期节点 profile：`capabilities` 和 `responsibilities`。
- system prompt 中明确当前设备是 Edge Agent Node。
- MQTT telemetry/state/event payload 带节点身份、能力和职责。
- MQTT 订阅 node command、role command、dispatch、timeline、alerts 主题。
- Sensor 角色对白名单 `read_temperature_humidity` 支持受限执行并发布结构化 `espagent.output.v1` OutputMessage。
- Control 角色对白名单状态灯/WS2812/舵机/GPIO 命令支持低中风险执行并发布结构化 OutputMessage。
- Coordinator `mesh_send_command` 可以等待相同 `command_id` 的 OutputMessage 并回灌给下一轮 LLM，形成跨节点 ReAct 闭环第一版。
- Guardian 角色订阅 timeline 并生成 `espagent.guardian.audit.v1` 观察式审计事件。
- Automation runtime 已支持把自然语言转换为确定性多步 workflow 或持久化 condition-action rule；后台 FreeRTOS task 会周期读取 Sensor 数据并通过 Guardian-gated Mesh 触发 Control 动作。
- 条件 rule 由一个常驻 `rule_task` 串行扫描执行；多步 workflow 由独立临时 `workflow_task` 执行。当前默认支持 8 条规则、8 个 workflow 槽位、每个 workflow 8 步。
- Coordinator 可以通过 LLM tool calling 将自然语言请求路由到 `sensor_agent` 或 `control_agent`，用户正常对话时不需要手写 MQTT node id。
- 串口 `config_show` 展示节点身份和关键 MQTT topic。
- Feishu WebSocket 接入已修复 ACK 栈溢出问题，当前 Coordinator 能稳定收到飞书消息并回复。

## 技术架构

### 感知层

当前 ESPAgent 已支持或预留：

- AHT10/AHT20：温湿度。
- SGP30：eCO2、TVOC 空气质量。
- BH1750/GY-30：光照 lux。
- HC-SR05：超声波距离/存在检测。
- 3-wire presence/PIR：人体存在输入。
- DHT22/MH-Z19：后台 MQTT 温湿度/CO2 发布链路。

### 传输层

当前已实现：

- Wi-Fi STA。
- Feishu/Lark WebSocket 长连接。
- 本地 WebSocket gateway，端口 `18789`。
- ESP-NOW 广播环境遥测。
- MQTT 裸 TCP publisher/subscriber。

Phase 1 Mesh MQTT topic：

```text
espagent/nodes/<node_id>/state
espagent/nodes/<node_id>/telemetry
espagent/nodes/<node_id>/events
espagent/nodes/<node_id>/command
espagent/roles/<role>/command
espagent/agent/dispatch
espagent/agent/timeline
espagent/alerts
```

四块 ESP32-S3 可以使用同一套固件，通过 `main/espagent_secrets.h` 配置不同 profile：

```text
esp32s3-coordinator-01  coordinator_agent  coordinator,communication,llm,dispatch,timeline,alerts
esp32s3-sensor-01       sensor_agent       sensor,telemetry,environment,air_quality,light,presence
esp32s3-control-01      control_agent      control,gpio,rgb,servo,relay,actuator
esp32s3-guardian-01     guardian_agent     guardian,security,policy,privacy,audit,watchdog,stateboard
```

详细配置见 `docs/ESP32_ROLE_PROFILES.md`。

2026-06-17 development 推荐状态：

```text
/dev/ttyUSB0  esp32s3-coordinator-01  coordinator_agent  Feishu / LLM / dispatch
/dev/ttyUSB1  esp32s3-sensor-01       sensor_agent       sensing / telemetry
/dev/ttyUSB2  esp32s3-control-01      control_agent      RGB / GPIO / actuator boundary
/dev/ttyUSB3  esp32s3-guardian-01     guardian_agent     audit / policy / privacy / watchdog
```

已验证：

- 四个节点均可通过串口看到 `state online`。
- 飞书 P2P bot `咕咕嘎嘎！` 已恢复端到端回复。
- `读取温湿度` 会被 Coordinator 路由到 `sensor_agent`。
- `点亮WS2812为蓝色` 会被 Coordinator 路由到 `control_agent`。
- USB1 `sensor_agent` 已实测 AHT20，典型读数 `27.x C / 45-46%RH`，并已发布到 `espagent/cube1345/nodes/esp32s3-sensor-01/telemetry`。
- 湿度条件联动已验证：USB0 后台 automation rule 读取 USB1 AHT20 湿度，判断 `humidity_percent > 40` 后经 Guardian 放行，由 USB2 点亮 WS2812 红色。

仍需补充的验证：

- 抓取 USB0 发布 MQTT command、USB1/USB2 接收 command、下游发布 OutputMessage、USB3 Guardian 发布 audit 的完整串口证据。
- Control 角色已经能执行低/中风险白名单动作；更高风险或长期运行的执行器动作前，还需要 command queue、safety interlock、actuator state 和更完整审计事件。

### 控制层

当前已实现：

- `agent_loop` 串行处理所有 LLM 回合。
- `tool_registry` 注册 AI-callable tools。
- `tool_guard` 阻止明显不匹配或不支持的硬件动作。
- GPIO、WS2812、舵机、MAX98357、传感器读取、文件、天气、搜索、cron 等工具。
- Cron 和 proactive service 可主动注入 Agent turn。

后续规划：

- Coordinator Agent 在云端或更高算力终端运行。
- ESP32-S3 节点保持窄工具、窄权限、强约束。
- 远程命令必须经过 schema、鉴权、审计、消息总线和 tool guard。

### 软件及开发环境

- MCU：ESP32-S3。
- SDK：ESP-IDF 6.1-dev。
- OS：FreeRTOS。
- 语言：C。
- 存储：SPIFFS、NVS。
- 外部服务：LLM provider、Tavily/Brave Search、Amap WebService、Feishu Open Platform。
- 调试入口：USB serial CLI、WebSocket、日志、`idf.py build/flash/monitor`。

### 云应用

当前固件已对接：

- LLM API：负责自然语言理解和 tool calling。
- Amap Weather API：结构化天气查询。
- Tavily/Brave Search：联网搜索。
- Feishu：用户聊天入口和主动消息出口。

后续云端 Coordinator/MCP 可扩展：

- MQTT Broker：多节点状态同步和调度事件流。
- MCP Gateway：数据库、知识库、Home Assistant、云服务等外部工具。
- Timeline Service：记录用户指令、任务拆解、工具调用、执行结果。
- Android/ESP32-P4 Display Agent：订阅 MQTT/WebSocket，展示状态和调度过程。

## 典型协作流程

用户说：“如果空气质量差，就打开风扇并提醒我”

```text
Coordinator Agent 理解任务
        |
        v
读取 Sensor Agent 空气质量
        |
        v
判断 eCO2/TVOC 是否超阈值
        |
        v
调用 Control Agent 打开风扇或继电器
        |
        v
Communication Agent 发送飞书/Android 通知
        |
        v
Display Agent 展示完整调度时间线
```

终端可展示：

```text
[用户指令] 读取空气质量并联动风扇
[任务拆解] 需要 Sensor Agent + Control Agent
[工具调用] read_environment
[结果] eCO2=900ppm, TVOC=...
[决策] 空气质量偏差，触发通风
[工具调用] relay_write / fan_on
[执行结果] 风扇已打开
[通知] 已同步到飞书和 Android App
```

## 当前限制

- 当前固件不是完整多 Agent 系统，只有一个 ESP32-S3 `agent_loop`。
- MQTT 使用裸 TCP MQTT 实现，尚未实现 TLS、认证、retain、QoS 完整语义。
- MQTT command/dispatch 当前仍以受限能力为主：Sensor `read_temperature_humidity` 是白名单执行路径，Control 已开放 WS2812/status-light/servo/GPIO 的低中风险白名单路径；更高风险执行器仍需 command queue、人工确认和 safety interlock 后再开放。
- 节点 profile 当前用于身份、能力声明、prompt 和 MQTT payload；还没有根据 role 自动裁剪工具列表。
- ESP32-P4 本体没有 Wi-Fi 射频，作为 Display Agent 时需要带无线协处理器的开发板或通过 Android/网关接入。
- MCP、云端 Coordinator、Android App、ESP32-P4 UI 还属于后续扩展。
- Automation 已能覆盖温湿度条件和顺序/延迟动作，但还没有完善自然语言暂停/恢复/删除规则、规则冲突检测、复杂多条件表达式和可查询规则状态面板。

## 下一阶段建议

1. 定义 MQTT command schema 和 timeline event schema。
2. 给 MQTT 增加认证/TLS 或放在可信内网/VPN 中。
3. 将 command/dispatch 安全接入 `message_bus`，而不是直接调用硬件。
4. 增加 `tool_result`/timeline 事件发布，便于 ESP32-P4/Android 展示调度过程。
5. 云端实现最小 Coordinator，先只做任务路由和只读状态查询，再逐步开放控制命令。

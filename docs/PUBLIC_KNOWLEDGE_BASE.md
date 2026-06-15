# ESPAgent Public Knowledge Base

本文是 ESPAgent 项目的公共知识库，面向后续开发者和 AI 协作者。它汇总项目定位、总体架构、文件分工、当前功能、细节边界、进度和未来规划。

`public_knowledge.md` 继续作为工作协议和进度日志入口；本文作为更完整的项目认知总览。

## 项目定位

ESPAgent 是运行在 ESP32-S3 上的轻量 AI Agent 固件。它不是 Linux 多进程框架，而是基于 ESP-IDF 和 FreeRTOS 的 MCU 程序。

私有 GitHub 仓库：

- HTTPS: `https://github.com/cube1345/ESP32_AgentMesh.git`
- SSH: `git@github.com:cube1345/ESP32_AgentMesh.git`

当前固件能力可以概括为：

- 通过 Feishu / WebSocket / Serial CLI 接收人类输入。
- 使用 LLM tool calling 理解任务和选择工具。
- 支持 `spawn_subagent`：主 Agent 可为独立的信息检索、文件读取/总结等子任务临时创建一个受限子 Agent。
- 通过 `tool_registry` 执行真实硬件、文件、天气、搜索、定时任务等工具。
- 使用 SPIFFS 保存记忆、会话、skills 和 cron 配置。
- 使用 MQTT / ESP-NOW / WebSocket 作为后续多节点协作链路。
- 以 ESP32-S3 Edge Agent Node 身份接入规划中的 LingShu Agent Mesh。

当前必须清楚区分三层：

- 当前已实现：单 ESP32-S3 上的轻量 Agent runtime，一个 `agent_loop` 串行处理所有 LLM 回合。
- 当前新增：`spawn_subagent` 可在同一 ESP32-S3 上临时启动一个 FreeRTOS 子任务执行短 ReAct loop，但它仍是同一固件内的受限工具型子代理，不是独立设备上的完整 LLM Agent。
- 规划中：多个 ESP32-S3 节点 + ESP32-P4/Android 终端 + 云端 Coordinator/MCP 的多 Agent 协作系统。

## 核心链路

```text
Feishu / WebSocket / Cron / Proactive / CLI Inject
        |
        v
message_bus inbound queue
        |
        v
agent_loop
        |
        v
context_builder + session history + memory + skills
        |
        v
LLM provider API
        |
        v
tool_use?
        |
        v
tool_registry
        |
        v
GPIO / RGB / Servo / Sensors / Files / Weather / Search / Cron
        |
        v
message_bus outbound queue
        |
        v
Feishu / WebSocket reply
```

关键边界：

- 通道只负责收发消息，不直接碰硬件。
- `agent_loop` 决定是否调用工具。
- 工具是 C 函数，LLM 只能通过注册过的 JSON schema 调用。
- 硬件动作必须经过工具实现和 tool guard。
- cron/proactive 只是注入消息，不是独立 LLM Agent。

## 当前 Agent Mesh 状态

当前固件已经支持 ESP32-S3 作为 LingShu Agent Mesh 的 Edge Agent Node：

- `ESPAGENT_NODE_ID`
- `ESPAGENT_NODE_ROLE`
- `ESPAGENT_NODE_LOCATION`
- `ESPAGENT_NODE_CAPABILITIES`
- `ESPAGENT_NODE_RESPONSIBILITIES`
- `ESPAGENT_MESH_TOPIC_PREFIX`

四块 ESP32-S3 可以使用同一套固件，通过 `espagent_secrets.h` 设置不同 profile：

```text
esp32s3-coordinator-01  coordinator_agent  coordinator,communication,llm,dispatch,timeline,alerts
esp32s3-sensor-01       sensor_agent       sensor,telemetry,environment,air_quality,light,presence
esp32s3-control-01      control_agent      control,gpio,rgb,servo,relay,actuator
esp32s3-display-01      display_agent      display,timeline,alerts,state,watchdog
```

当前实物映射：

```text
/dev/ttyUSB0  esp32s3-coordinator-01  coordinator_agent
/dev/ttyUSB1  esp32s3-sensor-01       sensor_agent
/dev/ttyUSB2  esp32s3-control-01      control_agent
/dev/ttyUSB3  esp32s3-display-01      display_agent
```

当前 MQTT 联调状态：

- USB0 `coordinator_agent` 已烧录，串口确认 Coordinator role 启动，并且 Feishu P2P bot `咕咕嘎嘎！` 已完成端到端回复验证。
- USB1 `sensor_agent` 已烧录，串口确认 Sensor role 启动，presence/environment monitor 已启动，并周期发布 `state online`。
- USB2 `control_agent` 已烧录，串口确认 Control role 周期发布 `state online`。
- USB3 `display_agent` 已烧录，串口确认 Display role 周期发布 `state online`。
- 2026-06-15 联调确认：四个串口 `/dev/ttyUSB0-3` 均可读取，但当前工具环境读串口需要提权；非提权 `/dev` 扫描可能短暂看不到设备。
- Coordinator 通过飞书自然语言测试已经能把 `读取温湿度` 路由到 `sensor_agent`，把 `点亮WS2812为蓝色` 路由到 `control_agent`。
- Sensor 节点当前日志中可见 `DHT22=ESP_ERR_TIMEOUT` 和 `MH-Z19=ESP_FAIL`，表示节点在线但这些具体传感器在当前接线/配置下未读到数据。
- 当前控制类远程执行仍需补齐 command queue、safety interlock、actuator state 和 result/timeline 审计后再完全开放；sensor 只有 `read_temperature_humidity` 白名单例外。
- 联调用 broker 暂为 `broker.emqx.io:1883`，topic prefix 暂为 `espagent/cube1345`；这是调试配置，不是生产配置。
- `mesh_send_command` 已加入 LLM tool registry，Coordinator 可以把跨节点请求发布为 MQTT Mesh command。
- Sensor 角色已补充 `read_temperature_humidity` command 白名单：收到命令后可调用 AHT10/AHT20 工具并发布 `mesh_command_result` 到 events/timeline。

MQTT Mesh topic：

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

当前 MQTT command / dispatch 的执行边界是：Sensor 角色只对白名单 `read_temperature_humidity` 做受限执行并发布 `mesh_command_result`；其它 node/role command 仍以校验和 dry-run 日志为主，不直接执行硬件动作。Control 角色真正执行前必须补 command queue、鉴权、审计、message_bus/tool_guard 转发和 safety interlock。

Feishu/LLM 通信板的 MQTT 桥接已经进入可编译状态：

- Feishu 入站消息会发布到本节点 `events`、全局 `agent/dispatch` 和全局 `agent/timeline`。
- AI 通过 Feishu 发出的出站回复会发布到本节点 `events` 和全局 `agent/timeline`。
- MQTT 发布走 FreeRTOS queue，允许 Feishu 事件先入队，等 MQTT 连接成功后再 flush。
- MQTT 接收端已支持标准 remaining length 解析，避免 payload 超过 127 字节时误读包体。
- 这些事件当前用于审计、展示和后续调度输入；还不会直接驱动其他 ESP32 的硬件动作。

Subagent 状态：

- 已在当前 `main` 架构上实现 `spawn_subagent`，没有合并远程 `subagent` 分支，也没有采用该分支回退旧架构的改动。
- 行为模式参考远程分支：主 Agent 调用工具后创建 `subagent` FreeRTOS task，子任务独立调用 LLM 和工具，完成后通过 semaphore 把结果交回主 Agent。
- 子代理工具面被严格限制为：`web_search`、`get_weather`、`get_current_time`、`read_file`、`write_file`、`edit_file`、`list_dir`。
- 子代理不能调用硬件、传感器、GPIO、WS2812、舵机、Mesh command，也不能递归创建子代理。
- 当前是同步等待模式，超时由 `ESPAGENT_SUBAGENT_TIMEOUT_MS` 控制；后续更合理的 Mesh 版本应演进成 async task_id + `message_bus`/timeline 回注结果。

Feishu WebSocket 稳定性状态：

- 2026-06-15 复现过一次 `feishu_ack` 任务栈溢出：飞书 WebSocket 收到消息后，ACK 小任务因 4KB 栈不足重启 Coordinator。
- 已新增 `ESPAGENT_FEISHU_ACK_STACK`，当前配置为 8KB，并把 `feishu_ack` 任务改为使用该配置。
- 修复后重新烧录 USB0，飞书消息 `测试第一角色修复后是否恢复：请回复收到。` 已正常得到 `ESPAgent is processing your request...` 和 `收到。` 两条回复。

## 四角色公共认知

四个角色不是四个彼此孤立的聊天机器人，而是同一套 ESPAgent runtime 在不同 ESP32-S3 上按职责裁剪后的节点。当前工程推荐继续保持“同仓库、同固件、不同 build-time profile”的方式，先靠 `ESPAGENT_NODE_ROLE` 和 `ESPAGENT_NODE_CAPABILITIES` 控制启动服务，后续再拆出更细的 role service。

### Coordinator / Communication Agent

地位：

- 这是当前接入 Feishu 的主 MCU，也是用户入口和 LLM 入口。
- 在四节点系统中处于“任务理解与调度核心”位置，但不应该直接承担全部传感器采样和高风险执行器控制。
- 当前本地私有配置已按该角色设置为 `esp32s3-coordinator-01` / `coordinator_agent` / `coordinator,communication,llm,dispatch,timeline,alerts`。

当前职责：

- 接入 Feishu WebSocket 和 WebSocket chat gateway。
- 运行 `agent_loop`，调用 LLM provider，管理 session/context/skills。
- 调用工具：天气、搜索、时间、cron/proactive、文件、部分硬件工具。
- 发布 MQTT state/events，并把 Feishu 入站发布到 `espagent/agent/dispatch` 和 `espagent/agent/timeline`。
- 通过 Feishu 把最终回复发回用户。

输入：

- Feishu 用户消息。
- WebSocket 用户消息。
- Cron/proactive 注入消息。
- 未来来自 MQTT 的 sensor telemetry、node state、alerts、tool result。

输出：

- Feishu/WebSocket 回复。
- `espagent/nodes/<coordinator_id>/events`
- `espagent/agent/dispatch`
- `espagent/agent/timeline`
- 未来面向 sensor/control/display 的正式 Mesh command。

已完成进度：

- Feishu/LLM 主链路已跑通。
- `feishu_inbound` 和 `feishu_outbound` MQTT 事件桥接已完成并通过构建。
- USB0 作为 Coordinator 已验证 MQTT state/events 发布到公网测试 broker。
- Coordinator 已新增 `mesh_send_command` 工具，可向 `sensor_agent`、`control_agent` 或指定 node 发布标准 MQTT Mesh command。
- UTF-8 safe prompt truncation 已修复，避免 LLM API 因截断中文而返回 HTTP 400。
- 启动后 SNTP 校时已接入，默认 `ntp.aliyun.com`，`get_current_time` 会优先使用已同步系统时间。
- 高德 `get_weather` 工具已注册，默认南京市栖霞区。

当前限制：

- 还没有把自然语言任务自动转换成正式 Mesh command。
- 还没有等待远端 `mesh_command_result`、关联 `command_id` 并把结果主动汇总回复飞书。
- 还没有完整 tool_use/tool_result timeline。
- 还没有 role-based tool exposure，Coordinator 仍能看到较多本地工具。
- MQTT broker 不可达时，事件只能在本地队列中等待，无法被其他节点看到。
- SNTP/天气修复已通过构建，但还没有在真实 Feishu 消息上完成板端运行验证。

下一步：

- 将 LLM 产出的跨节点动作转为 `espagent_mesh_command_t`。
- 对 dispatch 结果增加 command_id、target_role、safety_level、ttl_ms。
- 把天气、时间、主动提醒结果同步到 timeline，供 Display Agent 展示。

### Sensor Agent

地位：

- Sensor 是多节点系统的感知层，负责把真实环境变成可靠、短小、带时间戳的数据。
- 它不跑主 LLM，也不直接执行高风险控制动作。
- 它的价值在于长时间稳定采样、滤波、缓存和上报。

当前职责：

- 读取 AHT10/AHT20 温湿度、SGP30 eCO2/TVOC、BH1750/GY-30 光照、人体存在/距离等环境数据。
- 周期发布 telemetry。
- 通过 MQTT/ESP-NOW 把传感器快照给 Coordinator 和 Display。
- 后续做阈值判断，例如湿度过低、空气质量变差、光照异常。

输入：

- 本地 I2C/UART/GPIO 传感器。
- 未来的 MQTT read/query command。

输出：

- `espagent/nodes/<sensor_id>/telemetry`
- `espagent/nodes/<sensor_id>/state`
- `espagent/nodes/<sensor_id>/events`
- ESP-NOW 环境数据包。

已完成进度：

- role/capability 已能让 sensor profile 启动本地 sensor monitor。
- 环境监测任务已存在，能读取综合环境数据并通过 ESP-NOW 发送。
- MQTT telemetry/state/event 框架已存在。
- MQTT 收到 `read_temperature_humidity` command 时，Sensor 角色可执行 AHT10/AHT20 温湿度读取，并发布 `mesh_command_result`。

当前限制：

- `sensor_mqtt.c` 仍混合了通用 MQTT transport 和 DHT22/MH-Z19 telemetry，后续应拆为 `mesh_mqtt` 与 `sensor_telemetry`。
- 传感器数据还没有统一 sensor cache、质量标记、采样时间戳和异常阈值规则。
- Sensor 节点当前只对白名单 `read_temperature_humidity` command 做直接响应，其它 Mesh command 仍不执行。

下一步：

- 建立 `sensor_cache`，统一保存最近一次温湿度、空气质量、光照、存在检测。
- telemetry payload 标准化为 JSON schema。
- 增加环境阈值事件，例如 `air_quality_bad`、`humidity_low`、`presence_changed`。

### Control Agent

地位：

- Control 是执行层，负责真实 GPIO/PWM/I2S/继电器/舵机/灯光动作。
- 它必须最保守：只执行白名单动作，所有远程动作必须可校验、可限流、可审计。
- 它不应该直接相信 MQTT payload，更不应该让 MQTT callback 直接调用硬件工具。

当前职责：

- 运行 RGB、GPIO、舵机、继电器、风扇、水泵、加湿器等执行器驱动。
- 后续接收 Coordinator 下发的 Mesh command。
- 做本地 safety interlock、命令去重、TTL 检查、状态机和结果上报。

输入：

- 未来的 `espagent/nodes/<control_id>/command`
- 未来的 `espagent/roles/control_agent/command`
- 本地串口调试命令。

输出：

- `espagent/nodes/<control_id>/state`
- `espagent/nodes/<control_id>/events`
- 未来的 command result / actuator state / timeline event。

已完成进度：

- role/capability 已能让 control profile 启动控制边界。
- 本地舵机、WS2812、GPIO 等工具已存在。
- MQTT command 已能被解析并 dry-run 校验 target_node/target_role/action。
- 历史验证：USB1 之前作为 Control 验证过连接 MQTT、发布 state/events、订阅 role command，并收到一条测试 `gpio_write` command。当前 USB1 已改烧为 Sensor，Control 是后续第三角色目标。

当前限制：

- 还没有 command queue。
- 还没有 safety interlock。
- 还没有 actuator registry 和 actuator state。
- 还没有把 MQTT command 安全转成硬件动作。

下一步：

- 新增 `control/command_queue.c/.h`，先入队、去重、检查 TTL。
- 新增 `control/safety_interlock.c/.h`，控制危险动作确认、限流和互锁。
- 新增 `control/actuator_state.c/.h`，记录执行器状态并发布 result event。

### Display / Watchdog Agent

地位：

- Display 是可视化与状态监督层，可运行在 ESP32-S3 简易显示节点，也可以迁移到 ESP32-P4 或 Android App。
- 它不承担主 LLM，也不直接控制危险硬件。
- 它负责让多 Agent 调度过程“看得见”：谁发起、谁执行、结果如何、哪里异常。

当前职责：

- 订阅 state、telemetry、timeline、alerts。
- 展示节点在线状态、环境数据、任务拆解、工具调用、执行结果。
- 后续做 watchdog：节点离线、telemetry 过期、命令超时、异常告警。

输入：

- `espagent/nodes/+/state`
- `espagent/nodes/+/telemetry`
- `espagent/nodes/+/events`
- `espagent/agent/timeline`
- `espagent/alerts`

输出：

- 本地屏幕/串口/状态灯展示。
- 未来可发布 watchdog alert 或 display ack。

已完成进度：

- display role boundary 已接入启动流程。
- role gating 已修正，`timeline`/`alerts` capability 不会误启动 display service，必须具备 display/state/watchdog/display_agent/edge_agent。
- Feishu inbound/outbound 基础 timeline 事件已经由 Coordinator 发布。

当前限制：

- 还没有真实屏幕 UI。
- 还没有 timeline store。
- 还没有 MQTT wildcard 订阅聚合和 watchdog 规则。

下一步：

- 增加 `display/timeline_store.c/.h`。
- 增加 state/telemetry 聚合缓存。
- ESP32-P4 或 Android 作为更完整的 Display Agent 终端。

## 四节点资源压榨与代码设计

四块 ESP32-S3 不建议拆成四个互不相干的工程。推荐保持同一仓库、同一公共 runtime，再按 role 编译/配置出不同节点：

```text
common runtime + mesh protocol + role-specific service
```

代码层应逐步演进为：

```text
main/
├── mesh/
│   ├── mesh_protocol.c/.h     MQTT/ESP-NOW payload schema、topic builder、JSON encode/decode
│   ├── mesh_mqtt.c/.h         通用 MQTT transport、订阅、发布、重连
│   └── mesh_types.h           node_state、telemetry、command、result、timeline 类型
├── roles/
│   ├── role_config.h          基于 node_profile 的 role/capability 判断
│   ├── coordinator_node.c/.h  LLM、Feishu/WebSocket、dispatch、timeline
│   ├── sensor_node.c/.h       多传感器采样、滤波、缓存、告警、遥测
│   ├── control_node.c/.h      执行器队列、安全互锁、状态机、结果事件
│   └── display_node.c/.h      timeline/state/alerts 订阅与本地展示
├── sensors/
│   ├── sensor_sampler.c/.h    周期采样和驱动调度
│   ├── sensor_cache.c/.h      短期快照、统计、阈值判断
│   └── sensor_analyzer.c/.h   环境状态分析和异常事件
├── control/
│   ├── actuator_registry.c/.h 执行器能力表
│   ├── command_queue.c/.h     远程命令排队、去重、TTL
│   ├── safety_interlock.c/.h  危险动作确认、限流、互锁
│   └── actuator_state.c/.h    执行器当前状态和审计
└── display/
    ├── display_state.c/.h     节点状态聚合
    ├── timeline_store.c/.h    调度时间线缓存
    └── display_service.c/.h   屏幕/串口/状态灯展示
```

启动结构建议保持公共初始化和角色启动分离：

```c
common_init();

if (role_is_coordinator()) coordinator_node_init();
if (role_is_sensor()) sensor_node_init();
if (role_is_control()) control_node_init();
if (role_is_display()) display_node_init();

common_start();

if (role_is_coordinator()) coordinator_node_start();
if (role_is_sensor()) sensor_node_start();
if (role_is_control()) control_node_start();
if (role_is_display()) display_node_start();
```

每个节点的资源使用方向：

| 节点 | 主要吃满的资源 | 设计重点 |
|------|----------------|----------|
| Coordinator | Wi-Fi/HTTPS、TLS buffer、LLM JSON、PSRAM、会话上下文 | 保留 Feishu/WebSocket/LLM/search/weather/cron/proactive，负责理解、拆解、调度、通知。 |
| Sensor | I2C/UART/GPIO 输入、采样任务、滤波缓存、MQTT/ESP-NOW 上报 | 持续采集环境数据，做本地阈值判断和短期统计，不跑 LLM。 |
| Control | GPIO/PWM/I2S/继电器/舵机、命令队列、安全互锁、状态机 | 只执行白名单动作，所有远程命令先入队、校验、限流、审计，不直接从 MQTT 调 GPIO。 |
| Display | PSRAM/屏幕 buffer、timeline 缓存、状态订阅、watchdog | 订阅 state/telemetry/timeline/alerts，展示调度过程和异常状态，不承担主 LLM。 |

远程控制的安全路径必须是：

```text
MQTT command
        |
        v
mesh_protocol validate
        |
        v
command_queue
        |
        v
safety_interlock
        |
        v
actuator_state
        |
        v
driver/tool
        |
        v
result event + timeline event
```

禁止让 MQTT command 直接调用 `gpio_write`、`servo_write`、`ws2812_set` 等硬件工具。这样做虽然快，但会绕过权限、审计、互锁和用户确认边界。

建议的 command 类型：

```c
typedef struct {
    char command_id[40];
    char target_node[32];
    char target_role[32];
    char action[32];
    char args_json[256];
    int ttl_ms;
    int safety_level;
    bool require_ack;
} espagent_mesh_command_t;
```

实现顺序建议：

1. 先补 `mesh_types` 和 `mesh_protocol`，稳定 topic 与 JSON schema。
2. 再把现有 `sensor_mqtt.c` 拆成通用 `mesh_mqtt` 和传感器 telemetry service。
3. 再加 `roles/*_node` 骨架，让不同 profile 启动不同服务。
4. 最后才开放 command 执行，并强制经过 command queue、safety interlock、tool guard 和 timeline event。

当前执行状态：

- 已完成：`mesh_types`、`mesh_protocol`、`roles/*_node` 骨架、role-gated startup、MQTT command dry-run validation。
- 下一步：拆分 `sensor_mqtt.c` 为通用 `mesh_mqtt` 与 sensor telemetry service。
- 暂不开放：真实远程 command 执行，必须等 command queue、safety interlock、审计和 timeline event 完成后再接。

## 主要目录

```text
ESPAgent/
├── main/                       ESP-IDF application component
├── spiffs_data/                SPIFFS 初始文件
├── docs/                       架构、方案、集成、路线图
├── scripts/                    构建和环境脚本
├── skills/deploy/              部署辅助 skill
├── partitions.csv              Flash 分区表
├── sdkconfig.defaults          公共 ESP-IDF 默认配置
├── sdkconfig.defaults.esp32s3  ESP32-S3 默认配置
├── public_knowledge.md         AI 工作协议和进度日志
└── README.md                   项目入口说明
```

## main/ 文件分工

### 启动与全局配置

| 文件 | 意义 |
|------|------|
| `main/espagent.c` | ESP-IDF `app_main()` 入口，打印启动横幅并调用 app 启动阶段。 |
| `main/app/espagent_app.c/.h` | 真正的启动编排：NVS、SPIFFS、message bus、memory、cache、skills、Wi-Fi、LLM、tools、cron、proactive、CLI、网络服务。 |
| `main/espagent_config.h` | 全局编译期配置、默认值、分区路径、任务栈、GPIO、天气、MQTT、Mesh node profile、NVS namespace/key。 |
| `main/espagent_secrets.h.example` | build-time secrets 模板。真实 `espagent_secrets.h` 被 gitignore，不能提交。 |
| `main/CMakeLists.txt` | main 组件源文件和 ESP-IDF 依赖列表。 |

### 消息与 Agent

| 文件 | 意义 |
|------|------|
| `main/bus/message_bus.c/.h` | inbound/outbound FreeRTOS queue，统一传递 `espagent_msg_t`。 |
| `main/agent/agent_loop.c/.h` | 核心 ReAct loop：读取消息、构造 LLM 请求、执行 tool_use、保存会话、推送回复。 |
| `main/agent/context_builder.c/.h` | 构建 system prompt，包含工具说明、硬件边界、node profile、memory、recent notes、skills summary。 |
| `main/llm/llm_proxy.c/.h` | LLM provider HTTP 调用，支持 Anthropic 和 OpenAI-compatible tool-use 解析。 |
| `main/node/node_profile.c/.h` | 当前节点身份、角色、能力、职责和 capability 检查。用于四 ESP32 分工。 |
| `main/roles/role_config.c/.h` | 基于 node profile 判断当前节点应运行 LLM、聊天、scheduler、sensor、control、display 哪些服务。 |
| `main/roles/*_node.c/.h` | coordinator、sensor、control、display 四类 role service 骨架，作为后续职责拆分入口。 |
| `main/mesh/mesh_types.h` | Mesh command 等公共协议类型定义。 |
| `main/mesh/mesh_protocol.c/.h` | Mesh topic 构造和 MQTT command JSON 解析/目标校验。 |

### 通道与网关

| 文件 | 意义 |
|------|------|
| `main/channels/feishu/feishu_bot.c/.h` | Feishu/Lark WebSocket 接入、事件解析、消息入队、回复发送。 |
| `main/gateway/ws_server.c/.h` | 本地 WebSocket chat gateway，默认端口 `18789`。 |
| `main/cli/serial_cli.c/.h` | USB serial CLI，提供配置、诊断、工具直调、注入消息、搜索测试、proactive 测试等命令。 |
| `main/onboard/wifi_onboard.c/.h` | Wi-Fi onboarding/admin AP。 |
| `main/onboard/onboard_html.h` | 内嵌 onboarding HTML 页面。 |

### 存储、记忆、缓存、技能

| 文件 | 意义 |
|------|------|
| `main/memory/memory_store.c/.h` | 长期记忆 `MEMORY.md` 和 daily notes 读写。 |
| `main/memory/session_mgr.c/.h` | 每个 chat 的 JSONL 会话历史，文件名使用 hash，避免长 chat_id 破坏 SPIFFS 路径。 |
| `main/cache/cache_store.c/.h` | RAM KV cache，目前主要缓存 skills summary，后续可缓存 MCP tools list、搜索摘要、传感器快照。 |
| `main/skills/skill_loader.c/.h` | 从 SPIFFS skills markdown 中构建 prompt summary。 |

### 后台行为

| 文件 | 意义 |
|------|------|
| `main/cron/cron_service.c/.h` | 定时任务服务，支持 every、at、daily，任务触发后注入 agent turn。 |
| `main/proactive/proactive_service.c/.h` | 周期性主动检查，必要时向最近联系人发起消息；`PROACTIVE_NO_MESSAGE` 表示无需打扰。 |
| `main/heartbeat/heartbeat.c/.h` | 周期性读取 `HEARTBEAT.md` 并按需注入任务。 |

### 硬件、传感器和通信

| 文件 | 意义 |
|------|------|
| `main/drivers/*.c/.h` | 底层驱动：SGP30、AHT10/AHT20、BH1750、MAX98357 等。 |
| `main/tools/*.c/.h` | AI-callable 工具实现。每个工具做参数解析、边界检查和具体硬件/服务调用。 |
| `main/tools/tool_registry.c/.h` | 工具注册表、JSON schema 构建、按名字分发执行。 |
| `main/tools/tool_subagent.c/.h` | `spawn_subagent` 工具：创建受限 FreeRTOS 子代理，执行独立短 ReAct loop 后同步返回结果。 |
| `main/tools/gpio_policy.c/.h` | GPIO allowlist 和安全策略。 |
| `main/sensors/sensor_mqtt.c/.h` | MQTT state/event/telemetry 发布，订阅 node/role command、dispatch、alerts。 |
| `main/espnow/espnow_sender.c/.h` | ESP-NOW 广播文本遥测。 |
| `main/wifi/wifi_manager.c/.h` | Wi-Fi STA 生命周期、事件处理、重连退避。 |
| `main/proxy/http_proxy.c/.h` | HTTP CONNECT 代理，用于 Feishu/LLM/search 等 HTTPS 出口。 |
| `main/ota/ota_manager.c/.h` | HTTPS OTA 更新封装。 |

## SPIFFS 初始文件

| 路径 | 意义 |
|------|------|
| `spiffs_data/config/SOUL.md` | AI 个性和基本行为设定。 |
| `spiffs_data/config/USER.md` | 用户信息 bootstrap。 |
| `spiffs_data/memory/MEMORY.md` | 长期记忆初始文件。 |
| `spiffs_data/skills/*.md` | skills，运行时由 `skill_loader` 汇总进 prompt。 |

运行期还会使用：

- `/spiffs/memory/<YYYY-MM-DD>.md`: daily notes。
- `/spiffs/sessions/session_<hash>.jsonl`: 每个 chat 的短期会话历史。
- `/spiffs/cron.json`: cron 任务配置。
- `/spiffs/HEARTBEAT.md`: heartbeat 待办。

## 当前功能总览

### 通信入口

- Feishu/Lark WebSocket：主要用户聊天入口。
- 本地 WebSocket gateway：端口 `18789`，可用于上位机或调试网页。
- Serial CLI：无 Wi-Fi 时也可进行配置、诊断和工具测试。
- Cron / proactive / heartbeat：内部主动注入 agent turn。

### LLM 与工具调用

- `agent_loop` 使用非流式 LLM 请求。
- 支持 Anthropic Messages API 和 OpenAI-compatible provider。
- 支持多轮 tool_use，最大迭代由 `ESPAGENT_AGENT_MAX_TOOL_ITER` 控制，当前为 10。
- 工具 schema 由 `tool_registry` 统一生成。
- 工具结果回填给 LLM，再生成最终自然语言回复。
- `spawn_subagent` 也是普通工具，但内部会新建一个受限 FreeRTOS task；子代理使用单独过滤后的 tools JSON，且执行侧再次校验白名单，防止绕过硬件和 Mesh 安全边界。

### 已注册工具

| 工具 | 作用 |
|------|------|
| `web_search` | Tavily 优先、Brave fallback 的联网搜索。 |
| `get_weather` | 高德 Amap WebService 天气，默认南京市栖霞区。 |
| `get_current_time` | 获取当前时间并设置系统时钟。 |
| `spawn_subagent` | 为独立搜索、天气、时间、SPIFFS 文件读取/总结等任务创建受限临时子代理。 |
| `read_temperature_humidity` | AHT10/AHT20 温湿度读取。 |
| `read_environment` | AHT10/AHT20 + SGP30 + BH1750/GY-30 综合环境读取。 |
| `read_presence` | 人体存在或 HC-SR05 超声波存在判断。 |
| `hc_sr05_read_distance` | HC-SR05 低层距离读取。 |
| `read_file` / `write_file` / `edit_file` / `list_dir` | SPIFFS 文件工具。 |
| `gpio_write` / `gpio_read` / `gpio_read_all` | GPIO 控制和读取。 |
| `ws2812_set` / `set_status_light` | WS2812 RGB 状态灯控制。 |
| `servo_write` | GPIO5 舵机控制，角度或脉宽。 |
| `max98357_play_tone` | MAX98357 I2S 扬声器测试音。 |
| `sgp30_read_air_quality` / `read_air_quality` | SGP30 空气质量 eCO2/TVOC。 |
| `read_light_level` | BH1750/GY-30 光照 lux。 |
| `cron_add` / `cron_list` / `cron_remove` | 定时任务管理。 |

### 硬件能力

- WS2812 RGB：默认 GPIO48。
- 舵机：GPIO5，LEDC PWM，50Hz，500-2500us。
- AHT10/AHT20：温湿度。
- SGP30：空气质量 eCO2/TVOC。
- BH1750/GY-30：光照 lux。
- HC-SR05：超声波距离/存在检测，Echo 必须做电平保护。
- 3-wire presence/PIR：数字人体存在输入。
- MAX98357：I2S 音频测试。
- GPIO：仅允许安全 allowlist，避免破坏 USB/boot。

### 主动性

- `cron_add` 支持 recurring、one-shot、daily。
- `proactive_service` 可周期性触发自检，必要时主动发消息。
- 天气主动提醒应优先使用 `get_weather`。
- 如果 LLM 返回 `PROACTIVE_NO_MESSAGE`，proactive turn 不打扰用户。

## 配置方式

核心配置位于 `main/espagent_config.h`，真实部署值放在 `main/espagent_secrets.h`。

重要规则：

- `main/espagent_secrets.h` 是私有文件，不能提交。
- `main/espagent_secrets.h.example` 是公开模板。
- 部分配置可通过 Serial CLI 存入 NVS，但 node profile 当前主要依赖 build-time macros。
- Wi-Fi、LLM、搜索、高德、Feishu、代理、传感器引脚、Mesh 节点身份都通过宏或 NVS 管理。

常用 node profile 宏：

```c
#define ESPAGENT_SECRET_NODE_ID "esp32s3-sensor-01"
#define ESPAGENT_SECRET_NODE_ROLE "sensor_agent"
#define ESPAGENT_SECRET_NODE_LOCATION "南京市栖霞区"
#define ESPAGENT_SECRET_NODE_CAPABILITIES "sensor,telemetry,environment,air_quality,light,presence"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "read environment sensors and publish telemetry"
```

## 存储与分区

Flash 配置为 16MB，自定义分区表：

| 分区 | 大小 | 作用 |
|------|------|------|
| `nvs` | 24KB | Wi-Fi、LLM、Feishu、搜索、proactive、Amap 等 NVS 配置。 |
| `ota_0` | 2MB | OTA app slot A。 |
| `ota_1` | 2MB | OTA app slot B。 |
| `spiffs` | 约 11.8MB | memory、sessions、skills、cron、config 文件。 |
| `coredump` | 64KB | 预留 coredump，当前 sdkconfig 禁用 flash coredump。 |

最近构建结果：

- `build/ESPAgent.bin` size `0x149420`。
- 最小 app 分区剩余 `0xb6be0`，约 36%。

## 运行时任务

核心 FreeRTOS 任务：

- `agent_loop`: Core 1，处理 LLM 和工具调用。
- `feishu_ws`: Core 0，Feishu WebSocket。
- `outbound`: Core 0，统一发送 Feishu/WebSocket 回复。
- `serial_cli`: Core 0，串口 REPL。
- `cron`: 定时任务轮询。
- `proactive`: 主动检查。
- `sensor_mqtt`: MQTT state/event/telemetry。
- ESP-IDF 内部 Wi-Fi/httpd/TLS 任务。

设计原则：

- Core 0 偏 I/O。
- Core 1 偏 Agent/JSON/HTTPS。
- 大 buffer 优先 PSRAM。
- FreeRTOS queue 传递消息所有权，pop 后由消费者释放 `content`。

## 当前进度

已经完成：

- 项目重命名和文档体系统一为 ESPAgent。
- ESP-IDF 6.1-dev 构建适配。
- Feishu、WebSocket、Serial CLI 三类入口。
- 单 agent loop + tool calling + tool registry。
- SGP30、AHT10/AHT20、BH1750、HC-SR05、WS2812、舵机、MAX98357 等工具或驱动。
- 高德天气 `get_weather`，默认南京市栖霞区。
- SNTP 启动校时，默认 `ntp.aliyun.com`；`get_current_time` 优先使用已同步系统时间，HTTP Date 仅作为兜底。
- Tavily/Brave 搜索。
- prompt 安全边界和 execution-side tool guard。
- SPIFFS memory、daily notes、sessions、skills。
- Cron daily proactive 和 periodic proactive service。
- 反问承接的启发式上下文提示。
- Agent Mesh Phase 1：node identity、capabilities、responsibilities、MQTT state/telemetry/event、node/role command topic。
- 四 ESP32 role profile 文档：coordinator、sensor、control、display。
- Agent Mesh Phase 1.5：新增 `main/mesh` 协议层、`main/roles` 角色服务骨架，并让 `espagent_app` 根据 role/capability 选择性启动 LLM/聊天、scheduler、sensor monitor、control demo、display 边界服务。
- MQTT node/role command 已接入 `mesh_protocol` 做 JSON schema 解析、`action` 必填校验、`target_node`/`target_role` 匹配校验；除 Sensor `read_temperature_humidity` 白名单外，当前仍是 dry-run 日志，不执行硬件动作。
- Feishu 通信板 MQTT 桥接已完成第一版：`feishu_inbound` 发布到 node events、`agent/dispatch`、`agent/timeline`，`feishu_outbound` 发布到 node events 和 `agent/timeline`；MQTT queue 支持连接前事件暂存，MQTT packet remaining length 解析已修正。
- Coordinator 已注册 `mesh_send_command` 工具，可向指定 node 或 role 发布标准 MQTT Mesh command。
- Sensor 角色已支持白名单 `read_temperature_humidity` Mesh command，并将 AHT10/AHT20 执行结果发布为 `mesh_command_result` 到本节点 events 和全局 timeline。
- Feishu 通信板时间同步已补齐：Wi-Fi 连接后启动 SNTP 校时，`get_current_time` 不再优先依赖 Google Date 头；天气工具仍使用高德 `get_weather`，默认南京市栖霞区。
- 本地私有配置当前已设置为 Feishu/LLM 入口板：`esp32s3-coordinator-01` / `coordinator_agent` / `coordinator,communication,llm,dispatch,timeline,alerts`。
- 已烧录 coordinator 固件到 `/dev/ttyUSB0`，目标 ESP32-S3 MAC 为 `14:c1:9f:2d:76:20`；串口日志确认 Feishu、LLM、agent_loop 和 coordinator role 均启动，本地 sensor monitor 与 boot servo demo 已按角色跳过。
- 已修复飞书消息统一回复 `抱歉，我这次处理请求时遇到了错误。` 的根因：system prompt 旧 16KB buffer 被截断到 UTF-8 多字节字符中间，导致 OpenAI-compatible LLM API 返回 HTTP 400 `invalid unicode code point`。现在 prompt buffer 为 24KB，`context_builder` 和 `agent_loop` 都做 UTF-8-safe truncation。
- 修复后已烧录并通过串口 `inject_msg system debug hello` 验证：LLM API 正常返回，最终回复成功进入 outbound。
- GitHub 远端：
  - HTTPS: `https://github.com/cube1345/ESP32_AgentMesh.git`
  - SSH: `git@github.com:cube1345/ESP32_AgentMesh.git`
  - 仓库可见性：private
  - 当前项目已重新以 `ESP32_AgentMesh` 私有仓库承载，保留 ESPAgent 固件名称和 Agent Mesh 代码架构。
  - `main` 已推送；初始固件快照提交为 `b6cae3f`（`Initialize ESP32 AgentMesh firmware`）。

## 当前限制

- 当前仍是单 ESP32-S3 的单 `agent_loop`，不是完整多 Agent 运行时。
- MQTT command 现在已做基础 schema/目标校验；除 Sensor `read_temperature_humidity` 白名单外，dispatch 仍只打印日志，不执行硬件动作。
- Sensor 角色只有 `read_temperature_humidity` 白名单执行路径；Control 角色仍不会从 MQTT command 直接执行硬件动作。
- 还没有 MQTT command queue、鉴权、审计事件、结果关联和完整安全执行链路。
- 还没有根据 role 自动裁剪工具列表。
- Coordinator 现在会把 Feishu 入站广播到 `agent/dispatch`，但还没有把自然语言任务转成面向 sensor/control/display 节点的正式 Mesh command。
- timeline topic 已有 Feishu inbound/outbound 基础事件，但还没有完整 tool_use/tool_result 流。
- Memory 写入依赖模型主动调用文件工具，没有固件侧强制 consolidation。
- session 只保存最终对话，不保存完整 tool_use/tool_result 轨迹。
- system prompt 仍集中在 C 字符串中，后续可以拆成 SPIFFS prompt fragments。
- NVS 只有 24KB，后续如果存节点表、证书或更多运行时配置，建议扩容。
- 本地工作区 `.git` 在当前工具环境里是只读 tmpfs 占位，普通 `git status` 不可用；当前私有仓库推送使用 `/tmp/ESP32_AgentMesh.git` 作为临时 Git 元数据目录完成。

## 未来规划

### P0

- 实现 Coordinator 对 `mesh_command_result` 的 `command_id` 等待、关联和 Feishu 汇总回复。
- 将非 sensor 白名单的 MQTT command 安全转入 command queue / `message_bus`，由安全互锁、`agent_loop` 和 `tool_guard` 处理。
- 发布 timeline events：用户指令、任务拆解、工具调用、工具结果、最终回复。
- 增加节点 heartbeat / discovery。
- 增加 `storage_info` CLI，打印 SPIFFS/NVS/session 状态。

### P1

- Role-based tool exposure，根据 node capabilities 裁剪工具表和 prompt。
- Coordinator Agent 最小实现：只做任务路由和只读查询，不直接绕过安全层。
- Display Agent 支持 ESP32-P4 或 Android 订阅 timeline。
- MQTT TLS/认证或内网/VPN 部署规范。
- 完善 session/tool trace 持久化，便于调试和展示。

### P2

- MCP Gateway：数据库、知识库、Home Assistant、云服务。
- Memory Agent：长期记忆压缩、用户偏好、skills 管理。
- Android App：移动查看、调试、控制、调度时间线展示。
- ESP32-P4 中控屏：现场可视化和触控操作。
- 多节点策略：传感器冗余、控制器仲裁、异常告警。

## 开发注意事项

- 修改文件前先读 `public_knowledge.md`。
- 不要提交 `main/espagent_secrets.h`、`sdkconfig`、`dependencies.lock`、`build/`。
- 硬件工具要窄、可解释、可测试。
- 新硬件能力先做底层工具，再注册 tool schema，再补 prompt guidance，再用 CLI 测试。
- 通道模块不要直接控制硬件。
- 远程控制必须经过 schema、鉴权、message_bus、agent_loop、tool_guard。
- 对 ESP-IDF API 不确定时先查项目用法或官方文档。

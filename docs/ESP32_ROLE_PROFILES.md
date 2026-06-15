# ESP32 Role Profiles

本文说明如何用同一套 ESPAgent 固件，让四块 ESP32-S3 在 LingShu Agent Mesh 中各司其职。

核心原则：

- 四块板可以使用同一套代码。
- 每块板通过 `main/espagent_secrets.h` 设置不同的 `NODE_ID`、`NODE_ROLE`、`NODE_CAPABILITIES` 和 `NODE_RESPONSIBILITIES`。
- MQTT state/telemetry/event payload 会带上这些字段。
- 每块板会订阅自己的节点命令 topic 和角色命令 topic。
- 远程 command 当前已经有基础 schema/目标校验；Sensor 角色只对白名单 `read_temperature_humidity` 做受限执行并回传 `mesh_command_result`，Control 等其它硬件动作仍不直接执行。真正开放控制前必须接入 command queue、message_bus、schema 校验、tool_guard 和 safety interlock。

## 当前板端进度

当前实物联调进度：

| USB 口 | 节点 | 当前角色 | 当前状态 |
|--------|------|----------|----------|
| `/dev/ttyUSB0` | `esp32s3-coordinator-01` | `coordinator_agent` | 已烧录，串口确认 `state online`；Feishu bot `咕咕嘎嘎！` 端到端回复已恢复 |
| `/dev/ttyUSB1` | `esp32s3-sensor-01` | `sensor_agent` | 已烧录，串口确认 `state online`；presence/environment monitor 已启动；当前 DHT22/MH-Z19 未读到 |
| `/dev/ttyUSB2` | `esp32s3-control-01` | `control_agent` | 已烧录，串口确认 `state online`；用于接收控制类 Mesh command |
| `/dev/ttyUSB3` | `esp32s3-display-01` | `display_agent` | 已烧录，串口确认 `state online`；用于展示 state/timeline/alerts |

串口监视说明：

- 当前四个串口都应使用 `/dev/ttyUSB0-3`。
- 在本工作环境中，读取串口监视器时需要提权；非提权扫描可能短暂看不到 `/dev/ttyUSB*`，但提权读取可看到四块板日志。
- 建议联调时一次发送一条飞书命令，再并行观察 USB0/USB1/USB2/USB3，避免多条 LLM 回合交错。

当前联调用的 MQTT broker 是公网测试 broker：

```c
#define ESPAGENT_SECRET_SENSOR_MQTT_BROKER "broker.emqx.io"
#define ESPAGENT_SECRET_SENSOR_MQTT_PORT 1883
#define ESPAGENT_SECRET_MESH_TOPIC_PREFIX "espagent/cube1345"
```

该 broker 只适合临时验证，不适合长期生产使用。正式方案应换成自建 broker，并补用户名/密码、TLS、ACL 和 topic 隔离。

## 通用配置项

```c
#define ESPAGENT_SECRET_NODE_ID "esp32s3-edge-01"
#define ESPAGENT_SECRET_NODE_ROLE "edge_agent"
#define ESPAGENT_SECRET_NODE_LOCATION "南京市栖霞区"
#define ESPAGENT_SECRET_MESH_TOPIC_PREFIX "espagent"
#define ESPAGENT_SECRET_NODE_CAPABILITIES "coordinator,communication,sensor,control,display,telemetry,timeline,alerts"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "single-node development profile; can chat, sense, control, publish telemetry, and display mesh state"
```

## ESP32-1: Coordinator / Communication Node

职责：

- 接收 Feishu / WebSocket 用户消息。
- 调用 LLM 做任务理解。
- 后续负责生成 dispatch 和 timeline。
- 负责面向用户的时间/天气能力：启动后 SNTP 校时，天气查询使用高德 `get_weather`。
- 作为用户入口，不直接承担所有硬件控制。

建议配置：

```c
#define ESPAGENT_SECRET_NODE_ID "esp32s3-coordinator-01"
#define ESPAGENT_SECRET_NODE_ROLE "coordinator_agent"
#define ESPAGENT_SECRET_NODE_CAPABILITIES "coordinator,communication,llm,dispatch,timeline,alerts"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "receive user messages, call LLM, plan dispatch, publish timeline, and notify users"
```

## ESP32-2: Sensor Node

职责：

- 采集温湿度、空气质量、光照、人体存在等数据。
- 周期发布 telemetry。
- 不直接控制高风险执行器。

建议配置：

```c
#define ESPAGENT_SECRET_NODE_ID "esp32s3-sensor-01"
#define ESPAGENT_SECRET_NODE_ROLE "sensor_agent"
#define ESPAGENT_SECRET_NODE_CAPABILITIES "sensor,telemetry,environment,air_quality,light,presence"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "read environment sensors and publish telemetry for coordinator and display nodes"
```

## ESP32-3: Control Node

职责：

- 控制 RGB、GPIO、舵机、继电器、风扇、水泵、加湿器等。
- 后续接收安全 command 后执行硬件动作。
- 保持工具权限窄、动作可审计。

建议配置：

```c
#define ESPAGENT_SECRET_NODE_ID "esp32s3-control-01"
#define ESPAGENT_SECRET_NODE_ROLE "control_agent"
#define ESPAGENT_SECRET_NODE_CAPABILITIES "control,gpio,rgb,servo,relay,actuator"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "execute whitelisted hardware actions after schema validation and tool guard checks"
```

## ESP32-4: Display / Watchdog Node

职责：

- 订阅状态、告警和 timeline。
- 展示节点在线状态、环境数据、任务过程。
- 可做状态灯、蜂鸣提示或本地 watchdog。

建议配置：

```c
#define ESPAGENT_SECRET_NODE_ID "esp32s3-display-01"
#define ESPAGENT_SECRET_NODE_ROLE "display_agent"
#define ESPAGENT_SECRET_NODE_CAPABILITIES "display,timeline,alerts,state,watchdog"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "display mesh state, timeline, telemetry, alerts, and watchdog status"
```

## MQTT Topics

每个节点都会使用：

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

示例：

```text
espagent/nodes/esp32s3-control-01/command
espagent/roles/control_agent/command
```

## 当前实现边界

已经实现：

- 节点身份、能力和职责配置。
- `config_show` 展示节点 profile。
- MQTT state/event/telemetry 带节点 profile。
- 按节点和按角色订阅 command topic。
- 非 sensor profile 不会因为缺少 DHT22/MH-Z19 而反复重连。
- USB0 `coordinator_agent` 已烧录并通过串口确认 role 启动和 `state online`。
- USB1 `sensor_agent` 已烧录并通过串口确认 role 启动和 `state online`。
- USB2 `control_agent` 已烧录并通过串口确认 `state online`。
- USB3 `display_agent` 已烧录并通过串口确认 `state online`。
- Feishu WebSocket ACK 栈溢出已修复：`feishu_ack` 从硬编码 4KB 改为 `ESPAGENT_FEISHU_ACK_STACK`，当前 8KB。
- Feishu P2P 端到端回复恢复：测试消息收到 `ESPAgent is processing your request...` 和 `收到。`。
- Coordinator 可在自然语言中自动选择目标角色：
  - `读取温湿度` -> 回复已向 `sensor_agent` 发送读取指令。
  - `点亮WS2812为蓝色` -> 回复已转发给 `control_agent`。
- 历史进度：USB0 `coordinator_agent` 和 USB1 `control_agent` 曾连接到同一个 MQTT broker，串口验证了各自 state/events 发布；USB1 `control_agent` 曾验证接收 `espagent/cube1345/roles/control_agent/command` 并 dry-run 校验。
- 2026-06-15 四板串口确认均在线；下一步仍需要抓取 Coordinator MQTT publish、Sensor/Control MQTT receive、result event 三段日志，形成完整 MQTT 执行证据。
- `mesh_send_command` 工具已加入 LLM tool registry，Coordinator 可以通过 MQTT 向指定 node/role 发布标准 Mesh command。
- Sensor 角色收到 `read_temperature_humidity` command 时，已支持执行 AHT10/AHT20 温湿度读取，并把 `mesh_command_result` 发布到本节点 events 和全局 timeline。
- `main/roles/role_config.c/.h` 根据 role/capability 判断节点应该运行哪些服务。
- `main/roles/coordinator_node.c/.h`、`sensor_node.c/.h`、`control_node.c/.h`、`display_node.c/.h` 已作为四类职责入口接入启动流程。
- `main/app/espagent_app.c` 已根据 role/capability 选择性启动 LLM/聊天入口、scheduler/proactive、sensor monitor、control boot demo、display 边界服务。
- `main/mesh/mesh_types.h` 和 `main/mesh/mesh_protocol.c/.h` 已定义 Mesh command 类型，并解析/校验 MQTT command。
- MQTT node/role command 现在会进入 `mesh_protocol` 校验 `action`、`target_node`、`target_role`；除 Sensor `read_temperature_humidity` 白名单外，仍保持 dry-run，不执行硬件。
- Feishu 通信板 MQTT 桥接已完成第一版：
  - `feishu_inbound` 发布到 `espagent/nodes/<coordinator_id>/events`、`espagent/agent/dispatch`、`espagent/agent/timeline`。
  - `feishu_outbound` 发布到 `espagent/nodes/<coordinator_id>/events`、`espagent/agent/timeline`。
  - MQTT publish queue 支持连接前事件暂存，连接成功后 flush。
  - MQTT inbound packet 已支持标准 remaining length 解析。
- Feishu/LLM 通信板已接入启动后 SNTP 校时，`get_current_time` 优先返回同步后的本地系统时间，天气仍使用高德 `get_weather`。

尚未实现：

- MQTT command queue。
- MQTT command 到 message_bus 的安全转发。
- MQTT command 鉴权、审计事件、safety interlock。
- Coordinator 等待远端 `mesh_command_result`，关联 `command_id` 并汇总回复用户。
- 根据 role 自动裁剪工具列表。
- timeline tool_use/tool_result 事件流。

## 资源使用设计

目标不是让四块板都跑同样的满功能固件，而是让每块 ESP32-S3 吃满自己擅长的硬件资源。

| 节点 | 资源侧重点 | 不建议承担 |
|------|------------|------------|
| `coordinator_agent` | LLM HTTPS、Feishu/WebSocket、JSON 解析、session/context、dispatch/timeline | 长周期传感器采样、高风险执行器直控 |
| `sensor_agent` | I2C/UART/GPIO 采样、滤波、短期缓存、MQTT/ESP-NOW telemetry | LLM、用户聊天入口、继电器/电机控制 |
| `control_agent` | GPIO、PWM、I2S、继电器、舵机、RGB、动作队列、安全互锁 | 任务理解、天气搜索、跨节点规划 |
| `display_agent` | 屏幕/状态灯、PSRAM buffer、timeline cache、alerts/watchdog | 主 LLM、重型采样、危险控制 |

当前 profile 已经不只是声明能力：`espagent_app` 会根据 role/capability 选择性启动服务。推荐结构仍继续保持：

```text
common runtime
  Wi-Fi / NVS / SPIFFS / message_bus / node_profile / serial_cli

mesh protocol
  topic builder / JSON schema / MQTT transport / ESP-NOW framing

role service
  coordinator_node / sensor_node / control_node / display_node
```

推荐启动方式：

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

## 远程命令安全路径

后续允许 Coordinator 调用 Control Node 时，命令必须走固定链路：

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

不要让 MQTT callback 直接调用 `gpio_write`、`servo_write` 或 `ws2812_set`。原因是 MQTT 只是传输入口，不应该绕过 schema 校验、TTL、权限、互锁、审计和 timeline。

建议 command 数据结构：

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

## 模块拆分路线

下一轮代码改造建议按这个顺序做：

1. 已完成：新增 `main/mesh/mesh_types.h`，定义 Mesh command 类型。
2. 已完成：新增 `main/mesh/mesh_protocol.c/.h`，处理 topic 构造和 command JSON 解析/校验。
3. 已完成：新增 `main/roles/role_config.c/.h`，封装 `node_profile` role/capability 判断。
4. 已完成：新增四个 role service 骨架：`coordinator_node`、`sensor_node`、`control_node`、`display_node`。
5. 已完成：修改 `main/app/espagent_app.c`，按 role/capability 启动对应服务。
6. 下一步：将 `main/sensors/sensor_mqtt.c` 拆成通用 `mesh_mqtt.c` 和 sensor telemetry service。
7. 下一步：增加 `command_queue`、`safety_interlock`、`actuator_state`，确认安全链路后再接入真实执行。

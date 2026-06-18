# ESP32 Role Profiles

本文说明如何用同一套 ESPAgent 固件，让四块 ESP32-S3 在 LingShu Agent Mesh 中各司其职。

核心原则：

- 四块板可以使用同一套代码。
- 每块板通过 `main/espagent_secrets.h` 设置不同的 `NODE_ID`、`NODE_ROLE`、`NODE_CAPABILITIES` 和 `NODE_RESPONSIBILITIES`。
- MQTT state/telemetry/event payload 会带上这些字段。
- 每块板会订阅自己的节点命令 topic 和角色命令 topic。
- 远程 command 当前已经有基础 schema/目标校验；Sensor 角色只对白名单 `read_temperature_humidity` 做受限执行并回传结构化 `OutputMessage`。Control 角色对 `set_status_light`、`ws2812_set`、`servo_write`、`gpio_write` 已有低/中安全级执行路径并回传结构化 `OutputMessage`。Coordinator 下发 Mesh command 前会先发布 `policy_check`，USB3 Guardian 返回 `policy_decision=allow/deny` 后才继续下发；完整 command queue / safety interlock 仍是下一阶段。

## 当前板端进度

当前实物联调进度：

| USB 口 | 节点 | 当前角色 | 当前状态 |
|--------|------|----------|----------|
| `/dev/ttyUSB0` | `esp32s3-coordinator-01` | `coordinator_agent` | 已烧录，串口确认 `state online`；Feishu bot `咕咕嘎嘎！` 端到端回复已恢复 |
| `/dev/ttyUSB1` | `esp32s3-sensor-01` | `sensor_agent` | 已烧录，串口确认 `state online`；AHT20 已实测读取 `27.x C / 45-46%RH` 并发布 telemetry |
| `/dev/ttyUSB2` | `esp32s3-control-01` | `control_agent` | 已烧录，串口确认 `state online`；已验证接收控制类 Mesh command 并执行 WS2812 状态灯 |
| `/dev/ttyUSB3` | `esp32s3-guardian-01` | `guardian_agent` | 已烧录并参与 policy/audit；用于审计 OutputMessage、观察 timeline、发布 Guardian audit/alerts |

串口监视说明：

- 当前四个串口都应使用 `/dev/ttyUSB0-3`。
- 批量烧录脚本为 `tools/flash_roles_usb0_3.sh`，固定只使用 `/dev/ttyUSB0`、`/dev/ttyUSB1`、`/dev/ttyUSB2`、`/dev/ttyUSB3`，并按 Coordinator、Sensor、Control、Guardian 顺序烧录。
- 四角色压力测试脚本为 `tools/stress_mesh_usb0_3.py`，同样固定只使用 `/dev/ttyUSB0-3`；它会先执行四板 `config_show`，再由 USB0 连续发 Mesh command，并监听 USB1/USB2/USB3 的接收、执行、结果发布和崩溃日志。
- 飞书入口压力测试脚本为 `tools/stress_feishu_usb0_3.py`，同样只监听 `/dev/ttyUSB0-3`；它会真实向 Feishu bot `咕咕嘎嘎！` 发送温湿度和控制灯请求，再统计 USB0 的 Mesh 下发、USB1/USB2 的接收执行和四板崩溃情况。
- `/dev/ttyACM*` 不作为四角色烧录端口；如果某个 `/dev/ttyUSB0-3` 不存在，就视为对应 ESP32-S3 未检测到，不要改烧 ACM。
- 在本工作环境中，读取串口监视器时需要提权；非提权扫描可能短暂看不到 `/dev/ttyUSB*`，但提权读取可看到四块板日志。
- 建议联调时一次发送一条飞书命令，再并行观察 USB0/USB1/USB2/USB3，避免多条 LLM 回合交错。
- 2026-06-17 起固件已加入串口 OTA 维护命令：`ota_info` 查看当前 OTA 分区，`ota_update <https_url_to_ESPAgent.bin>` 从 HTTPS app bin 升级 inactive slot。当前不要通过飞书/LLM 触发 OTA；四角色仍是 build-time profile，OTA 镜像必须按目标角色构建，或后续把 role profile 迁移到 NVS。
- OTA 在四角色系统中的长期定位是运维编排：Coordinator 负责发现版本和发起升级请求，Guardian 负责策略/来源/角色/人工确认校验，目标节点执行 OTA，ESP32-P4/Android 展示进度。当前固件只完成目标节点执行层的串口 HTTPS OTA。

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
#define ESPAGENT_SECRET_NODE_CAPABILITIES "coordinator,communication,sensor,control,guardian,telemetry,timeline,alerts"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "single-node development profile; can chat, sense, control, publish telemetry, and audit mesh state"
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
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "read environment sensors and publish telemetry for coordinator and display terminals"
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

## ESP32-4: Guardian / Security Node

职责：

- 订阅全局 timeline 和节点事件。
- 审计 `OutputMessage`、工具调用结果、Mesh command 和最终回复。
- 处理 `policy_check` 并发布 `policy_decision`，维护轻量 StateBoard。
- 对错误事件发布 alerts，后续继续扩展人工确认、隐私脱敏和 watchdog。
- 不直接执行硬件动作，不替代 P4/Android 显示终端。

建议配置：

```c
#define ESPAGENT_SECRET_NODE_ID "esp32s3-guardian-01"
#define ESPAGENT_SECRET_NODE_ROLE "guardian_agent"
#define ESPAGENT_SECRET_NODE_CAPABILITIES "guardian,security,policy,privacy,audit,watchdog,stateboard"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "enforce policy decisions, audit OutputMessages, watch node health, and protect private data"
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
espagent/security/policy_check
espagent/security/decision
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
- USB3 推荐改为 `guardian_agent`；启动后会打印 Guardian boundary，收到 `policy_check` 后发布 `espagent.policy_decision.v1`，并在收到关键 timeline 事件时发布 `espagent.guardian.audit.v1` 审计事件。
- 2026-06-15 已新增并验证 `tools/flash_roles_usb0_3.sh`：脚本会临时切换 `main/espagent_secrets.h` 的节点 profile，按 USB0-USB3 顺序烧录四个角色，最后恢复为 Coordinator profile；脚本明确拒绝使用 `/dev/ttyACM*`。
- 2026-06-15 已新增并验证 `tools/stress_mesh_usb0_3.py`：
  - 基线测试 `--rounds 5 --interval 6 --settle 40 --quiet`：USB0 入队 10/10，USB1 sensor 接收/执行 5/5，USB2 control 接收/执行 5/5，0 崩溃，PASS。
  - 突发测试 `--rounds 5 --interval 1.5 --settle 60 --quiet`：USB0 入队 10/10，USB1 sensor 接收/执行 5/5，USB2 control 接收/执行 5/5，0 崩溃，PASS。
  - 测试中的 AHT10/DHT22/MH-Z19 错误来自当前物理传感器未接入或不可用，不代表 Mesh 链路失败。
  - 旧 Display role 已由 ESP32-P4/Android 承担主显示职责；USB3 S3 推荐转为 Guardian。
- Feishu WebSocket ACK 栈溢出已修复：`feishu_ack` 从硬编码 4KB 改为 `ESPAGENT_FEISHU_ACK_STACK`，当前 8KB。
- Feishu P2P 端到端回复恢复：测试消息收到 `ESPAgent is processing your request...` 和 `收到。`。
- Feishu 入口压测中复现过 USB0 `Tmr Svc` 栈溢出；已把 heartbeat 文件读取/消息注入移出 timer callback，改由 `heartbeat_worker` 执行，并把 `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH` 提升到 4096。
- Coordinator 可在自然语言中自动选择目标角色：
  - `读取温湿度` -> 回复已向 `sensor_agent` 发送读取指令。
  - `点亮WS2812为蓝色` -> 回复已转发给 `control_agent`。
- Coordinator 对上述两类常见飞书指令已加入确定性 Mesh 路由和假成功保护，不再完全依赖 LLM 自己选择 `mesh_send_command`。
- `mesh_send_command` 默认异步：返回 `async_task_id` 后由后台 task 等待相同 `command_id` 的结构化 `OutputMessage`，再通过 `message_bus` 注入内部结果消息，让 LLM 在下一回合总结远端真实执行结果。
- `mesh_send_command` 在发布真正的 node/role command 前会先向 `espagent/security/policy_check` 发布 `espagent.policy_check.v1`，等待 Guardian 在 `espagent/security/decision` 上返回 `espagent.policy_decision.v1`。只有 `decision=allow` 才继续下发。
- 2026-06-15 飞书入口压力测试 `tools/stress_feishu_usb0_3.py --rounds 2 --interval 30 --settle 220 --quiet` 通过：飞书发送 4/4，USB1 sensor 接收/执行 2/2，USB2 control 接收/执行 2/2，0 崩溃，PASS。
- 历史进度：USB0 `coordinator_agent` 和 USB1 `control_agent` 曾连接到同一个 MQTT broker，串口验证了各自 state/events 发布；USB1 `control_agent` 曾验证接收 `espagent/cube1345/roles/control_agent/command` 并 dry-run 校验。
- 2026-06-15 四板串口确认均在线；Coordinator MQTT publish、Sensor/Control MQTT receive、result event 三段日志已经在串口/MQTT 压测与飞书入口压测中形成基础证据。
- `mesh_send_command` 工具已加入 LLM tool registry，Coordinator 可以通过 MQTT 向指定 node/role 发布标准 Mesh command。
- Sensor 角色收到 `read_temperature_humidity` command 时，已支持执行 AHT10/AHT20 温湿度读取，并把 `mesh_command_result` 发布到本节点 events 和全局 timeline。
- 2026-06-18 USB1 AHT20 已完成实物验证：I2C `0x38` 正常响应，`read_temperature_humidity` 和周期 telemetry 均返回 AHT20 温湿度；典型串口读数 `temperature=27.4 C, humidity=45.2%`。
- `main/roles/role_config.c/.h` 根据 role/capability 判断节点应该运行哪些服务。
- `main/roles/coordinator_node.c/.h`、`sensor_node.c/.h`、`control_node.c/.h`、`guardian_node.c/.h`、`display_node.c/.h` 已作为职责入口接入启动流程。
- `main/app/espagent_app.c` 已根据 role/capability 选择性启动 LLM/聊天入口、scheduler/proactive、sensor monitor、control boot demo、guardian/display 边界服务。
- `main/mesh/mesh_types.h` 和 `main/mesh/mesh_protocol.c/.h` 已定义 Mesh command 类型，并解析/校验 MQTT command。
- MQTT node/role command 现在会进入 `mesh_protocol` 校验 `action`、`target_node`、`target_role`；Sensor `read_temperature_humidity` 已有白名单执行路径；Control 对 `set_status_light`、`ws2812_set`、`servo_write`、`gpio_write` 已有直接执行路径，并会在执行前校验本机缓存的 Guardian allow decision；后续仍需 command queue、签名认证、人工确认和 safety interlock。
- Feishu 通信板 MQTT 桥接已完成第一版：
  - `feishu_inbound` 发布到 `espagent/nodes/<coordinator_id>/events`、`espagent/agent/dispatch`、`espagent/agent/timeline`。
  - `feishu_outbound` 发布到 `espagent/nodes/<coordinator_id>/events`、`espagent/agent/timeline`。
  - MQTT publish queue 支持连接前事件暂存，连接成功后 flush。
  - MQTT inbound packet 已支持标准 remaining length 解析。
- Feishu/LLM 通信板已接入启动后 SNTP 校时，`get_current_time` 优先返回同步后的本地系统时间，天气仍使用高德 `get_weather`。
- Automation runtime 已接入工具层和后台任务：`automation_create_workflow` 处理顺序/延迟动作，`automation_create_rule` 处理持久化条件监控，规则保存到 `/spiffs/automation.json`。
- Automation 支持多个任务但有明确上限：条件规则由单个 `rule_task` 轮询，最多 8 条；多步 workflow 每个启动一个临时 `workflow_task`，最多 8 个 workflow 槽位，每个最多 8 步。
- 2026-06-18 已验证湿度条件联动：USB0 创建 `humidity_percent > 40` 规则，USB1 AHT20 返回约 `46.0%RH`，USB2 执行 `set_status_light` 后 WS2812 输出红色 `rgb=(255,0,0)`；该测试规则已删除，说明机制可用于默认后台任务，但不会留下测试副作用。

尚未完善：

- MQTT command queue。
- MQTT command 到 message_bus 的安全转发。
- 完整 command queue、签名认证、人工确认和 safety interlock。
- Coordinator 已能通过异步 task_id 等待远端 `OutputMessage` 并回灌给 LLM；更完整的超时恢复、trace 查询和任务树聚合仍待实现。
- Automation 目前支持创建、列出和删除；自然语言暂停/恢复、规则状态面板、复杂多条件表达式、规则冲突检测、运行中 workflow 取消和 workflow 重启恢复仍待完善。
- 根据 role 自动裁剪工具列表。
- 更完整的 timeline 持久化、trace 聚合、Guardian StateBoard API 和 watchdog。

## 资源使用设计

目标不是让四块板都跑同样的满功能固件，而是让每块 ESP32-S3 吃满自己擅长的硬件资源。

当前需要分清“固件体积”和“运行时启用服务”：

- Flash 还没有按角色裁剪。四块板使用同一套 app 镜像；加入 OTA CLI 后最新验证构建 `ESPAgent.bin` 为 `0x156bf0`，2MB app 分区剩余 `0xa9410`，约 33%。
- 运行时已经按 role/capability 裁剪服务。Coordinator 最重，Sensor/Control 中等，Guardian 目前较轻但已承担审计入口。
- USB0 Coordinator 启动日志显示 PSRAM 约 8MB 可用，完成一次 ReAct 验证后 PSRAM 仍约 8.25MB 可用；说明当前并未真正把硬件资源吃满。

| 节点 | 资源侧重点 | 不建议承担 |
|------|------------|------------|
| `coordinator_agent` | LLM HTTPS、Feishu/WebSocket、JSON 解析、session/context、dispatch/timeline | 长周期传感器采样、高风险执行器直控 |
| `sensor_agent` | I2C/UART/GPIO 采样、滤波、短期缓存、MQTT/ESP-NOW telemetry | LLM、用户聊天入口、继电器/电机控制 |
| `control_agent` | GPIO、PWM、I2S、继电器、舵机、RGB、动作队列、安全互锁 | 任务理解、天气搜索、跨节点规划 |
| `guardian_agent` | policy_check/policy_decision、audit/privacy/watchdog/stateboard、OutputMessage 审计 | 主 LLM、传感器采样、危险控制、复杂 UI |

当前实际资源利用判断：

| 节点 | 当前实际启用 | 当前资源饱和度 |
|------|--------------|----------------|
| `coordinator_agent` | LLM、Feishu、WebSocket、MQTT、SNTP、cron/proactive、session/context、临时 subagent | 四者中最高，但仍有较大 PSRAM/Flash 余量 |
| `sensor_agent` | sensor sampling、environment/presence monitor、MQTT telemetry、serial CLI | 中等，尚缺 sensor cache、滤波统计、阈值事件 |
| `control_agent` | control boundary、MQTT command receiver、本地 actuator tools、boot servo demo | 中等偏低，尚缺 command queue、safety interlock、actuator_state |
| `guardian_agent` | guardian boundary、MQTT policy_check/decision、timeline 订阅、OutputMessage audit、错误 alerts、StateBoard | 中等，尚缺人工确认、watchdog 聚合 |

当前 profile 已经不只是声明能力：`espagent_app` 会根据 role/capability 选择性启动服务。推荐结构仍继续保持：

```text
common runtime
  Wi-Fi / NVS / SPIFFS / message_bus / node_profile / serial_cli

mesh protocol
  topic builder / JSON schema / MQTT transport / ESP-NOW framing

role service
  coordinator_node / sensor_node / control_node / guardian_node / display_node
```

推荐启动方式：

```c
common_init();

if (role_is_coordinator()) coordinator_node_init();
if (role_is_sensor()) sensor_node_init();
if (role_is_control()) control_node_init();
if (role_is_guardian()) guardian_node_init();
if (role_is_display()) display_node_init();

common_start();

if (role_is_coordinator()) coordinator_node_start();
if (role_is_sensor()) sensor_node_start();
if (role_is_control()) control_node_start();
if (role_is_guardian()) guardian_node_start();
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

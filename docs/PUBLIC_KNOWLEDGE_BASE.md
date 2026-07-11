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
- 支持一层轻量 `/命令` 前缀路由，用固定命令约束后续自然语言执行方向。
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
- slash 命令层先于 prompt 构建运行；它只做 direct reply 或 prompt rewrite，不是新的 shell/runtime。
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
esp32s3-guardian-01     guardian_agent     guardian,security,policy,privacy,audit,watchdog,stateboard
```

当前实物映射：

```text
/dev/ttyUSB0  esp32s3-coordinator-01  coordinator_agent
/dev/ttyUSB1  esp32s3-sensor-01       sensor_agent
/dev/ttyUSB2  esp32s3-control-01      control_agent
/dev/ttyUSB3  esp32s3-guardian-01     guardian_agent
```

当前 MQTT 联调状态：

- USB0 `coordinator_agent` 已烧录，串口确认 Coordinator role 启动，并且 Feishu P2P bot `咕咕嘎嘎！` 已完成端到端回复验证。
- USB1 `sensor_agent` 已烧录，串口确认 Sensor role 启动，presence/environment monitor 已启动，并周期发布 `state online`。
- USB2 `control_agent` 已烧录，串口确认 Control role 周期发布 `state online`。
- USB3 推荐角色已调整为 `guardian_agent`。Guardian 不是显示终端，而是安全/数据治理节点；ESP32-P4 和 Android 继续承担可视化 Display Terminal。
- 2026-06-15 联调确认：四个串口 `/dev/ttyUSB0-3` 均可读取，但当前工具环境读串口需要提权；非提权 `/dev` 扫描可能短暂看不到设备。
- Coordinator 通过飞书自然语言测试已经能把 `读取温湿度` 路由到 `sensor_agent`，把 `点亮WS2812为蓝色` 路由到 `control_agent`。
- Coordinator 现在对常见飞书 Mesh 指令有确定性路由：普通 `读取温湿度` 直接转 `sensor_agent/read_temperature_humidity`，远程/控制板 WS2812 状态灯颜色请求直接转 `control_agent/set_status_light`，不再完全依赖 LLM 自己选择工具。
- Coordinator 新增一层显式 slash command：
  - `/help`
  - `/sensor <task>`
  - `/control <task>`
  - `/guardian <task>`
  - `/subagent <task>`
  - `/workflow <task>`
  - `/rule <task>`
  - `/local <task>`
- 其中 `/help`、未知命令、缺少任务参数会直接返回文本；其它 slash 命令会把后续自然语言改写成更强的角色/执行约束，再进入原有 ReAct/tool 流程。
- 2026-07-05 USB0 板端已验证 slash 闭环：`/help` 直接回复成功；`/control set status light blue` 已确认走 `mesh_send_command -> policy_check -> policy_decision -> control_agent -> OutputMessage -> final reply`。
- Sensor 节点当前日志中可见 `DHT22=ESP_ERR_TIMEOUT` 和 `MH-Z19=ESP_FAIL`，表示节点在线但这些具体传感器在当前接线/配置下未读到数据。
- 当前控制类远程执行已支持 WS2812/status-light/servo/GPIO/virtual_device_control 白名单验证；Coordinator 下发前会先经过 Guardian `policy_check/policy_decision`，执行结果会发布结构化 `espagent.output.v1` OutputMessage。Control Agent 已新增轻量 command queue，支持重复 command_id 拦截、TTL 检查、单执行器互锁、emergency stop latch 和 actuator state snapshot。Guardian 已具备人工确认队列，`approval_confirm` 后可通过一次性 `approval_id` 让同 action/role 的高风险控制重试获得 allow。固件也支持可选硬件 interlock GPIO 和 Mesh command HMAC 当前/previous key 验证；生产 broker ACL/TLS、真实互锁接线和密钥轮换流程仍需部署验证。
- 2026-06-21 实机压测发现 Guardian 在高频 state/telemetry + StateBoard 更新下会拖慢 `policy_decision` 回流，导致 Coordinator 偶发等待超时但 Guardian 随后才 allow。已修复：Guardian 对普通 watchdog StateBoard 更新按 state/telemetry 分别 30s 节流，MQTT critical 队列只让 command / policy_check / policy_decision 插队，Coordinator 的 policy decision 等待调整为 3-15s 并增加同 `command_id` 的 3 次幂等 `policy_check` 重试。
- 联调用 broker 暂为 `broker.emqx.io:1883`，topic prefix 暂为 `espagent/cube1345`；这是调试配置，不是生产配置。
- `mesh_send_command` 已加入 LLM tool registry，Coordinator 可以把跨节点请求发布为 MQTT Mesh command。
- Sensor 角色已补充 `read_temperature_humidity` command 白名单：收到命令后可调用 AHT10/AHT20 工具并发布结构化 `espagent.output.v1` OutputMessage 到 events/timeline。
- 2026-06-18 实测 USB1 `sensor_agent` 上 AHT20 已正确识别并读取，典型读数为 `temperature=27.4 C`、`humidity=45.2%`；Sensor telemetry 已发布到 `espagent/cube1345/nodes/esp32s3-sensor-01/telemetry`，payload 包含 `temp`、`humidity`、`sensor:"AHT20"` 和 `status`。
- Coordinator 的 `mesh_send_command` 工具现在会先向 Guardian 发起 `policy_check`，收到 `decision=allow` 后才发布真正的 Sensor/Control command；随后在 `require_ack=true` 时等待同一 `command_id` 的 OutputMessage。等待成功时，工具结果会包含 `output_message={...}` 并进入下一轮 LLM 上下文，让跨节点 ReAct 从“只下发命令”推进到“裁决、下发、等待、观察结果、再推理”。
- Coordinator 已新增 automation runtime：`automation_create_workflow` 用于顺序/延迟动作，`automation_create_rule` 用于持续条件监控，规则持久化到 `/spiffs/automation.json`，由 FreeRTOS `automation` task 后台轮询执行，不依赖当前对话回合持续占用 `agent_loop`。
- Automation 当前分两条执行路径：条件规则由一个常驻 `rule_task` 串行扫描 `s_rules[]`，多步/延迟任务由每个 workflow 自己启动一个临时 `workflow_task`。默认上限为 8 条规则、8 个 workflow 槽位、每个 workflow 8 步。
- 2026-06-18 已完成湿度条件自动化验证：USB0 创建 `humidity_percent > 40` 规则，USB1 AHT20 返回湿度约 `46.0%`，USB0 经 Guardian policy 下发 `set_status_light`，USB2 `control_agent` 执行 WS2812 `rgb=(255,0,0)`；测试规则随后已通过 `automation_remove` 删除。
- Guardian 角色已接入 policy 第二层：启动时声明 policy/privacy/audit/stateboard/watchdog 边界；订阅 `espagent/cube1345/security/policy_check` 后按白名单、安全等级和部分参数级规则返回 `espagent.policy_decision.v1` 到 `espagent/cube1345/security/decision`；对 `virtual_device_control` 会检查 `device`、`duration_ms` 和持久 relay `confirmed=true` 要求；订阅 timeline 后对 `tool_use`、`tool_result`、`mesh_command_queued`、`mesh_command_result`、`final_reply`、`error` 等关键事件生成 `espagent.guardian.audit.v1` 审计事件，错误事件会同步发布到 alerts。

Gateway / 管理后台状态：

- 2026-06-21 已参考 Ai-Thinker BLE Mesh Gateway 的思路，把“网关”作为 ESPAgent 的能力层接入，而不是把外部项目整套替换进来。
- 新增 `device_registry`：本地维护已知 MQTT Mesh 节点和外部网关设备，持久化到 `/spiffs/device_registry.json`；本节点启动时自动注册自己，收到 `nodes/+/state` 或 `nodes/+/telemetry` 后会更新远端节点在线状态、角色、能力和最近出现时间。
- 新增本地管理 API：`GET /status`、`GET /devices`、`POST /gateway/ble_mesh/register`、`POST /ota/plan`；`onboard_html` 已加入 Gateway Status、BLE Mesh Device、OTA Gateway Plan 三个管理区块。
- 新增 LLM/CLI 可调用工具：`gateway_status`、`gateway_register_ble_mesh_device`、`gateway_ble_mesh_send`、`ota_gateway_plan`，并接入 sandbox、capability family、role-visible profile 和 `context_builder` 提示词。
- BLE Mesh 目前是 bridge boundary：可以注册设备、生成事件、把 BLE Mesh command 意图发布到 node events 和全局 timeline；但真实 BLE Mesh Provisioner / model send 后端尚未链接，工具会返回 `ESP_ERR_NOT_SUPPORTED` / `not_linked`，不能声称已经物理控制 BLE Mesh 设备。
- OTA Gateway 当前是 plan-only：Coordinator/管理页/CLI 可以生成结构化升级计划并发布到 timeline，便于 P4/Android 展示；真实升级仍通过串口 `ota_update <https_url>`，远程 OTA 执行还需要 Guardian 审批、镜像来源校验、目标角色校验和进度回传后再开放。

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

当前 MQTT command / dispatch 的执行边界是：Sensor 角色只对白名单 `read_temperature_humidity` 做受限执行并发布 `mesh_command_result`；Control 角色已支持 WS2812/status-light 这类低风险白名单命令并发布结果。Gateway 侧 BLE Mesh command 当前只登记/发布意图，不做真实 BLE 执行。其它 node/role command 仍以校验和 dry-run 日志为主，不直接执行硬件动作。

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
- 当前 subagent 仍是同步等待模式，超时由 `ESPAGENT_SUBAGENT_TIMEOUT_MS` 控制；Mesh command 已演进为默认异步 `async_task_id` + 后台等待 OutputMessage + `message_bus`/timeline 回注结果。
- 2026-06-15 已在 USB0 coordinator 板端完成真实验证：烧录当前固件后启动日志显示 `Registered tool: spawn_subagent`、`Tools JSON built (26 tools)`、`Subagent tools JSON built`；串口执行 `tool_exec spawn_subagent {"task":"Call_get_current_time_and_return_one_sentence"}` 后，子代理完成 LLM tool loop，调用 `get_current_time`，并以 `ESP_OK` 返回当前时间。
- 2026-06-15 同一 USB0 板端也完成主 `agent_loop` ReAct 验证：串口 `inject_msg system react_test 请调用get_current_time并回复当前时间` 触发 `Tool use iteration 1`，模型调用 `get_current_time({})`，工具返回系统时间后第二轮 LLM 生成最终回复。

Slash command 状态：

- 实现文件：`main/agent/slash_command.c/.h`
- 接入点：`main/agent/agent_loop.c`
- 提示词认知：`main/agent/context_builder.c`
- 当前设计不是给 MCU 做一个通用 shell，而是在 `agent_loop` 前增加一层固定命令语义入口。
- 已支持的固定命令：
  - `/help`
  - `/sensor`
  - `/control`
  - `/guardian`
  - `/subagent`
  - `/workflow`
  - `/rule`
  - `/local`
- 当前行为：
  - `/help` 和错误命令直接回复
  - 其它命令把后续自然语言重写成强约束提示，再复用现有 LLM ReAct/tool/runtime

四角色资源占用快照：

- Flash 尚未按角色裁剪，四个角色仍使用同一固件镜像，但会在烧录前写入不同 `node_id` / `role` / capabilities；2026-06-21 加入 Gateway/device_registry/管理 API 后构建 app 二进制约 `0x195920`，2MB app 分区剩余 `0x6a6e0`，约 21%。
- Coordinator 是当前最重角色，承担 LLM、Feishu WebSocket、WebSocket server、MQTT、SNTP、cron/proactive、session/context 和临时 subagent。USB0 启动日志显示 PSRAM 约 8MB 可用；完成一次 ReAct 验证后 PSRAM 仍约 8.25MB 可用。
- Sensor 当前承担 sensor sampling、environment/presence monitor、MQTT telemetry 和串口 CLI，不运行 LLM/Feishu。
- Control 当前承担控制边界、MQTT command 接收、本地执行器工具和 boot servo demo，不运行 LLM/Feishu。
- Display 当前由 ESP32-P4+C6 和 Android 方向承担；USB3 ESP32-S3 从旧 Display 方案调整为 Guardian。ESP32-P4+C6 工程 `/home/cube/WorkSpace/ESP/lvgl_traffic_control` 已新增真实 LVGL `AgentMesh` 页面和 MQTT timeline/state/telemetry/alerts 订阅，并已烧录到 `/dev/ttyACM0`。当前 P4 板端已确认 Wi-Fi 和 MQTT 订阅成功，四板联动与屏幕动态刷新仍需继续实机验证。
- P4 已订阅 `nodes/+/telemetry`，因此 AHT20 温湿度 telemetry 能进入 Display Terminal 数据流；但现有 Environment Monitor 温湿度卡片仍需进一步做动态绑定，当前不能夸大为所有 UI 卡片都已实时刷新。
- Android Display Agent 交接方案已新增到 `docs/ANDROID_AGENTMESH_HANDOFF.md`：定位为 ESP32-P4+C6 的移动增强版展示终端，使用 Java Android + MQTT 订阅 `timeline/state/telemetry/alerts`，在本地维护类似 Stage `StateBoard` 的状态源，用于展示 AI 推理、Mesh 通信、节点状态、传感器数据和最终用户结果。
- 当前状态不是硬件资源完全拉满，而是按角色裁剪服务并保留较大 RAM/PSRAM/Flash 余量，便于继续加入 Sensor cache、Control command queue/safety interlock、Display timeline/UI。

Feishu WebSocket 稳定性状态：

- 2026-06-15 复现过一次 `feishu_ack` 任务栈溢出：飞书 WebSocket 收到消息后，ACK 小任务因 4KB 栈不足重启 Coordinator。
- 已新增 `ESPAGENT_FEISHU_ACK_STACK`，当前配置为 8KB，并把 `feishu_ack` 任务改为使用该配置。
- 修复后重新烧录 USB0，飞书消息 `测试第一角色修复后是否恢复：请回复收到。` 已正常得到 `ESPAgent is processing your request...` 和 `收到。` 两条回复。
- 2026-06-15 飞书入口压测又复现过一次 USB0 `Tmr Svc` FreeRTOS timer service 栈溢出。根因方向是 timer callback 中承载了 heartbeat 文件读取和消息注入等重型工作，同时 `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048` 偏小。
- 已将 heartbeat timer callback 改为只启动 `heartbeat_worker` task，真正的 SPIFFS 读取和 message_bus 注入在 worker 中执行；同时把 `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH` 提升到 4096，并写入 `sdkconfig.defaults`。
- 压测还暴露出 LLM 偶发“口头声称已通过 MQTT Mesh 发送、但没有工具调用”的问题。`agent_loop` 现在会拦截这种假成功回复，并对温湿度和远程/控制板状态灯请求优先走确定性 Mesh 路由。

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
- 未来面向 sensor/control/guardian/display terminal 的正式 Mesh command。

已完成进度：

- Feishu/LLM 主链路已跑通。
- `feishu_inbound` 和 `feishu_outbound` MQTT 事件桥接已完成并通过构建。
- USB0 作为 Coordinator 已验证 MQTT state/events 发布到公网测试 broker。
- Coordinator 已新增 `mesh_send_command` 工具，可向 `sensor_agent`、`control_agent` 或指定 node 发布标准 MQTT Mesh command。
- Coordinator 已新增确定性 Mesh 快速路由：飞书中常见的 `读取温湿度` 和远程/控制板 WS2812 状态灯颜色请求会直接生成 `mesh_send_command`，减少 LLM 漏工具调用导致的假成功。
- Coordinator `mesh_send_command` 已能等待相同 `command_id` 的结构化 OutputMessage，并把结果回灌给下一轮 LLM。
- UTF-8 safe prompt truncation 已修复，避免 LLM API 因截断中文而返回 HTTP 400。
- 启动后 SNTP 校时已接入，默认 `ntp.aliyun.com`，`get_current_time` 会优先使用已同步系统时间。
- 高德 `get_weather` 工具已注册，默认南京市栖霞区。

当前限制：

- 简单自然语言任务已经能自动转换成正式 Mesh command：普通温湿度读取和远程/控制板 WS2812 状态灯颜色请求已验证。复杂跨节点任务仍需要继续通过 LLM/tool planning 转换。
- 远端结果关联已有默认异步回注第一版：`mesh_send_command` 返回 `async_task_id`，后台等待 OutputMessage 后注入内部消息。
- 基础 `tool_use`/`tool_result`/`mesh_command_queued`/结构化 `OutputMessage`/Guardian audit/`final_reply` timeline 已实现；`trace_*.jsonl`、串口 `trace_show`、`trace_index`、`task_tree` 和 Guardian `stateboard_show` 已落地。当前 task tree 是按近期 trace 的 `trace_id/command_id` 轻量聚合，不是完整 DAG 调度器；watchdog 聚合仍待补齐。
- 还没有 role-based tool exposure，Coordinator 仍能看到较多本地工具。
- MQTT broker 不可达时，事件只能在本地队列中等待，无法被其他节点看到。
- SNTP/天气修复已通过构建；天气真实 Feishu 场景仍需单独板端验证。

下一步：

- 继续实机验证 command queue、人工确认和硬件 safety interlock，把 Guardian decision、一次性 approval token、interlock GPIO 和 Control 本地执行闸门串成可演示闭环。
- 把天气、时间、主动提醒结果同步到 timeline，供 ESP32-P4/Android 展示。

四板压力验证：

- `tools/flash_roles_usb0_3.sh` 固定只烧录 `/dev/ttyUSB0-3`，不使用 `/dev/ttyACM*`。
- `tools/stress_mesh_usb0_3.py` 固定只测试 `/dev/ttyUSB0-3`，先确认四板 `config_show` role，再由 USB0 连续发送 `mesh_send_command`，并监视 USB1/USB2/USB3 日志。
- 2026-06-15 基线测试 `--rounds 5 --interval 6 --settle 40 --quiet` 通过：USB0 入队 10/10，USB1 sensor 接收/执行 5/5，USB2 control 接收/执行 5/5，0 崩溃。
- 2026-06-15 突发测试 `--rounds 5 --interval 1.5 --settle 60 --quiet` 通过：USB0 入队 10/10，USB1 sensor 接收/执行 5/5，USB2 control 接收/执行 5/5，0 崩溃。
- 2026-06-15 飞书入口压力测试 `tools/stress_feishu_usb0_3.py --rounds 2 --interval 30 --settle 220 --quiet` 通过：飞书发送 4/4，USB1 sensor 接收/执行 2/2，USB2 control 接收/执行 2/2，`mesh_command_result_lines=8`，0 崩溃；日志在 `artifacts/feishu_stress/feishu_stress_205412_ttyUSB*.log`。
- 2026-06-21 最新四板复测：按 `/dev/ttyUSB0-3` 顺序重刷 Coordinator/Sensor/Control/Guardian 后，`tools/verify_roles_usb0_3.py --echo` 四角色身份 PASS；`tools/stress_mesh_usb0_3.py --rounds 5 --interval 1.5 --settle 60 --quiet` 连续两轮 PASS。两轮均为 `sent=10`、`queued_ok=10`、Sensor 接收/执行 `5/5`、Control 接收/执行 `5/5`、Guardian `policy_checks=10`、`policy_decisions=10`、`guardian_audits=10`、`warnings=0`、`errors=0`、`crashes=0`。当前判断：在该压测强度下四角色 MQTT Mesh / Guardian policy / OutputMessage 链路正常。
- 2026-07-05 USB0 一度因刷写链路异常掉入 bootloader 循环，现象为 `invalid segment length 0xffffffff`、`OTA app partition slot 0 is not bootable`、`No bootable app partitions in the partition table`。已确认不是 slash 功能代码本身造成编译失败，而是增量压缩刷写后的 app 镜像损坏。恢复方式为：跳过 `idf.py flash` 的增量压缩路径，改用原始 `esptool` 对 `/dev/ttyUSB0` 做整板擦除和全量无压缩写入；恢复后 USB0 已重新启动 app，并再次看到 Feishu WS 启动日志。
- 压测中出现的 AHT10/DHT22/MH-Z19 错误来自当前物理传感器未接入或不可用，不代表 MQTT Mesh 链路失败。
- Display role 当前可确认 profile 与 state 在线；完整 timeline 订阅、缓存和可视化仍未完成。

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
- 2026-06-18 USB1 AHT20 已实测通过，I2C 地址 `0x38`，典型读数 `27.4 C / 45.2%RH`；Sensor Agent 的周期 telemetry 已改为优先发布 AHT20 温湿度 JSON，而不是继续依赖未接入的 DHT22/MH-Z19。

当前限制：

- `sensor_mqtt.c` 仍混合了通用 MQTT transport 和 DHT22/MH-Z19 telemetry，后续应拆为 `mesh_mqtt` 与 `sensor_telemetry`。
- 传感器数据还没有统一 sensor cache、质量标记、采样时间戳和异常阈值规则。
- Sensor 节点当前只对白名单 `read_temperature_humidity` command 做直接响应，其它 Mesh command 仍不执行；AHT20 正常接线后已不再把“未找到温湿度设备”视为当前主要问题。

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
- S3 coordinator 现在会把 ReAct 工具调用、工具结果、Mesh command queued、Mesh command result、最终回复和错误路径发布到 `espagent/cube1345/agent/timeline`。
- ESP32-P4+C6 侧已在现有 LVGL 工程 `/home/cube/WorkSpace/ESP/lvgl_traffic_control` 中新增 `AgentMesh` tab：四角色状态卡、timeline 列表、最终结果区域、裸 TCP MQTT 订阅任务和 FreeRTOS 事件队列。
- P4 侧为避免外网 component manager 依赖，没有使用 `esp-mqtt`，而是沿用与 S3 类似的裸 TCP MQTT 订阅器。
- P4 工程已用当前 `/home/cube/WorkSpace/ESP/esp-idf` v6.1-dev 构建通过，输出 `build/lvgl_template.bin`，大小 `0x150910`，8MB app 分区剩余约 84%。
- P4 固件已烧录到 `/dev/ttyACM0`；最终串口日志显示 Wi-Fi connected，IP `10.176.79.81`，并已连接 `broker.emqx.io:1883`，订阅 `agent/timeline`、`nodes/+/state`、`nodes/+/telemetry`、`alerts`，且 `agent/timeline` 收到 `SUBACK id=1 code=0x00`。主机侧 broker loopback 已确认 topic 可转发；但一次 host-published timeline 测试消息没有在 P4 串口收包日志中出现，因此 P4 收包日志和屏幕 timeline 动态刷新还需要结合实时 S3 流量继续验证。
- 2026-06-18 S3 侧已经能持续发布 AHT20 telemetry，因此 P4/Android Display 的下一步重点从“有没有数据源”转为“UI 卡片和 timeline 是否正确绑定这条实时数据流”。

当前限制：

- P4 真实屏幕 UI 已有基础实现并已烧录；当前已验证到 Wi-Fi/MQTT 连接和订阅，尚未完成实时收包/屏幕刷新验证。
- 还没有持久化 timeline store；当前 P4 只保留内存中的最近事件。
- 还没有 watchdog 规则、命令超时判断和 display ack。

下一步：

- 增加 `display/timeline_store.c/.h`。
- 增加 state/telemetry 聚合缓存。
- 用实时 S3 timeline/state/telemetry 流量验证 P4 收包日志、LVGL timeline 刷新和节点状态卡更新。
- 后续再把摄像头等高带宽感知能力接入 AgentMesh 展示链路。

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

- 已完成：`mesh_types`、`mesh_protocol`、`roles/*_node` 骨架、role-gated startup、MQTT command validation、Sensor/Control 低中风险白名单执行、Guardian policy gate 和 OutputMessage result。
- 下一步：拆分 `sensor_mqtt.c` 为通用 `mesh_mqtt` 与 sensor telemetry service，并补 command queue、safety interlock、actuator state、人工确认和更完整审计。
- 当前只开放受控白名单动作；不要把任意 MQTT command 直接接到硬件工具。

## 主要目录

```text
ESPAgent/
├── main/                       ESP-IDF application component
├── spiffs_data/                SPIFFS 初始文件
├── docs/                       架构、方案、集成、路线图
├── tools/                      主机侧烧录、验证、压力测试和 benchmark runner
├── benchmarks/                 benchmark 数据集和期望行为用例
├── scripts/                    构建和环境脚本
├── skills/deploy/              部署辅助 skill
├── artifacts/                  测试日志和运行摘要，生成物，不进源码管理
├── build*/                     ESP-IDF 构建产物，生成物，不进源码管理
├── partitions.csv              Flash 分区表
├── sdkconfig.defaults          公共 ESP-IDF 默认配置
├── sdkconfig.defaults.esp32s3  ESP32-S3 默认配置
├── public_knowledge.md         AI 工作协议和进度日志
└── README.md                   项目入口说明
```

目录治理规则：

- 固件源码只放在 `main/`，由 `main/CMakeLists.txt` 显式加入编译。
- SPIFFS 初始文件只放在 `spiffs_data/`，由顶层 `CMakeLists.txt` 打包。
- 串口烧录、四角色验证、飞书压力测试、skills benchmark 等主机工具放在 `tools/`。
- benchmark 数据集放在 `benchmarks/`，runner 放在 `tools/`。
- `artifacts/`、`build*/`、`tmp*/`、`__pycache__/`、`*.bin`、`*.elf`、`*.map`、`*.log` 都是生成物，已由 `.gitignore` 忽略。
- 详细目录职责和新增文件放置规则见 `docs/PROJECT_STRUCTURE.md`。

当前结构优化边界：

- 现在不推荐拆成四个 ESP-IDF 工程；继续使用一个固件工程，通过 node profile 区分 Coordinator、Sensor、Control、Guardian。
- 未来真正值得拆的是 `main/sensors/sensor_mqtt.c`：它目前同时承担 MQTT lifecycle、Mesh command routing、telemetry、Guardian decision cache、OutputMessage 等职责。推荐后续拆为 `main/mesh/mesh_mqtt_client.*`、`main/mesh/mesh_command_router.*`、`main/mesh/mesh_policy_cache.*` 和 `main/sensors/sensor_telemetry.*`。

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
| `main/roles/role_config.c/.h` | 基于 node profile 判断当前节点应运行 LLM、聊天、scheduler、sensor、control、guardian、display 哪些服务。 |
| `main/roles/*_node.c/.h` | coordinator、sensor、control、guardian、display role service 骨架，作为后续职责拆分入口。 |
| `main/mesh/mesh_types.h` | Mesh command 公共协议类型定义：`command_id`、`trace_id`、`target_node`、`target_role`、`action`、`args_json`、`ttl_ms`、`safety_level`、`require_ack`。 |
| `main/mesh/mesh_protocol.c/.h` | Mesh topic 构造和 MQTT command JSON 解析/目标校验。 |

### 通道与网关

| 文件 | 意义 |
|------|------|
| `main/channels/feishu/feishu_bot.c/.h` | Feishu/Lark WebSocket 接入、事件解析、消息入队、回复发送。 |
| `main/gateway/ws_server.c/.h` | 本地 WebSocket chat gateway，默认端口 `18789`。 |
| `main/gateway/ble_mesh_bridge.c/.h` | BLE Mesh bridge boundary：注册外部 BLE Mesh 设备、发布 BLE command 意图到 events/timeline；真实 BLE 后端未链接时返回 `not_linked`。 |
| `main/device/device_registry.c/.h` | 本地 device registry：持久化已知 MQTT Mesh 节点和外部网关设备，供管理页、Gateway tool、P4/Android 展示使用。 |
| `main/cli/serial_cli.c/.h` | USB serial CLI，提供配置、诊断、工具直调、注入消息、搜索测试、proactive 测试等命令。 |
| `main/onboard/wifi_onboard.c/.h` | Wi-Fi onboarding/admin AP；当前也提供 `/status`、`/devices`、`/gateway/ble_mesh/register`、`/ota/plan` 管理 API。 |
| `main/onboard/onboard_html.h` | 内嵌 onboarding/admin HTML 页面，包含 Wi-Fi/LLM/Feishu/Search/Gateway/BLE/OTA Plan 配置区。 |

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
| `main/drivers/*.c/.h` | 底层驱动：SGP30、AHT10/AHT20、BH1750、MAX98357、INA I2S Mic 等。 |
| `main/tools/*.c/.h` | AI-callable 工具实现。每个工具做参数解析、边界检查和具体硬件/服务调用。 |
| `main/tools/tool_registry.c/.h` | 工具注册表、JSON schema 构建、按名字分发执行。 |
| `main/tools/tool_gateway.c/.h` | Gateway 相关 AI-callable 工具：`gateway_status`、`gateway_register_ble_mesh_device`、`gateway_ble_mesh_send`、`ota_gateway_plan`。 |
| `main/tools/tool_subagent.c/.h` | `spawn_subagent` 工具：创建受限 FreeRTOS 子代理，执行独立短 ReAct loop 后同步返回结果。 |
| `main/tools/gpio_policy.c/.h` | GPIO allowlist 和安全策略。 |
| `main/sensors/sensor_mqtt.c/.h` | MQTT state/event/telemetry 发布，订阅 node/role command、dispatch、timeline、alerts、policy_check/decision；缓存 OutputMessage 和 policy decision，供 Coordinator 异步回注与 Control 本地校验使用。 |
| `main/control/command_queue.c/.h` | Control Agent 执行闸门：command_id 去重、TTL 检查、单执行器互锁、emergency stop latch、actuator state snapshot，并包装真实硬件工具执行。 |
| `main/mesh/mesh_auth.c/.h` | 可选 MQTT Mesh command HMAC-SHA256 签名/验证；`ESPAGENT_SECRET_MESH_AUTH_KEY` 为空时保持开发兼容，非空时接收端强制校验 `signature`。 |
| `main/espnow/espnow_sender.c/.h` | ESP-NOW 广播文本遥测。 |
| `main/wifi/wifi_manager.c/.h` | Wi-Fi STA 生命周期、事件处理、重连退避。 |
| `main/proxy/http_proxy.c/.h` | HTTP CONNECT 代理，用于 Feishu/LLM/search 等 HTTPS 出口。 |
| `main/ota/ota_manager.c/.h` | HTTPS OTA 更新封装；当前通过 Serial CLI 的 `ota_info` / `ota_update` 使用。 |
| `benchmarks/skills/espagent_skill_benchmark.jsonl` | ESP32 运行时 skill benchmark 数据集，用 JSONL 描述 skill loading、Mesh routing、sandbox、privacy、prompt-injection 和 workflow 验证用例。 |
| `tools/benchmark_skills_usb0_3.py` | ESP32-S3 四板 benchmark runner，默认只使用 `/dev/ttyUSB0-3`，通过串口 CLI / `inject_msg` 执行用例并基于日志打分。 |
| `tools/manifest_lint.py` | 新硬件 manifest 开发者工具链，支持本地 lint 和 `--dry-run` 路由预演，不依赖外部 Python 包。 |
| `schemas/device_manifest.schema.json` | device manifest schema 参考，记录当前支持的协议、角色、风险和基础字段。 |

## SPIFFS 初始文件

| 路径 | 意义 |
|------|------|
| `spiffs_data/config/SOUL.md` | AI 个性和基本行为设定。 |
| `spiffs_data/config/USER.md` | 用户信息 bootstrap。 |
| `spiffs_data/memory/MEMORY.md` | 长期记忆初始文件。 |
| `spiffs_data/skills/*.md` | skills，运行时由 `skill_loader` 汇总进 prompt。 |
| `spiffs_data/devices/*.json` | 新硬件 runtime manifest。当前支持 `virtual_device_read` 读取 read-only I2C、bounded UART query、read-only Modbus RTU register、SPI transfer-read、ADC one-shot、GPIO input manifest。 |

当前运行时 skills 已覆盖：

- `weather.md`: 天气请求优先使用高德 `get_weather`。
- `proactive-care.md`: 定时天气、主动关心、daily briefing。
- `agent-cache-engineering.md` / `esp32-kv-cache.md`: prompt/cache/KV cache 边界和优化。
- `gpio-control.md`: GPIO 安全控制指导。
- `agent-mesh-coordination.md`: 四角色自然语言路由，避免让用户手填 MQTT node id。
- `mqtt-mesh-operations.md`: MQTT Mesh topic、command/result、开发与生产安全边界。
- `mcu-edge-ai-boundaries.md`: MCU 端 AI 能力边界，明确当前不是 ESP32-S3 本地 LLM。
- `mesh-resource-planning.md`: 四块 ESP32-S3 的资源使用方向和后续补强路径。
- `user-profile-adaptation.md`: 用户画像慢速更新规则，记录偏好、习惯、环境舒适阈值、证据、置信度和冲突修正策略。
- `agent-sandbox-permissions.md`: Agent 行为沙箱、最小权限、风险分级、dry-run、TTL、确认和硬件互锁规则。
- `privacy-data-minimization.md`: 家居隐私数据分级、本地脱敏、最小上下文、MQTT/Display/Memory 隐私边界。
- `tool-integrity-and-prompt-injection.md`: 防 prompt injection、工具输出不可信、工具完整性、外部内容不得覆盖安全策略。
- `hardware-protocol-extension.md`: 新硬件通用协议扩展认知，覆盖 I2C/IIC、SPI、UART、RS485/Modbus RTU、CAN/TWAI、GPIO、PWM、ADC、1-Wire、I2S/PDM、RMT/红外、USB CDC、SDIO/SDMMC、BLE GATT；要求 Agent 先判断是否存在安全通用原语/类驱动和硬件 manifest，不能假装未知硬件已经有专用驱动。
- `spiffs_data/devices/bh1750_manifest.json`: 通用 I2C manifest 示例，不依赖专用 BH1750 工具；流程为写 `0x20`、等待 180ms、读 2 字节、按 `raw_u16_be * 0.833333` 解码为 lux。
- `spiffs_data/devices/uart_at_example.json`: 通用 UART read-only query manifest 示例，使用 UART2、固定 TX/RX、固定 AT 查询命令、固定读取长度和超时；UART0 保留给串口控制台，不能被 manifest 占用。
- `spiffs_data/devices/modbus_rtu_temp_example.json`: 通用 Modbus RTU read-only manifest 示例，生成 function 3/4 读寄存器帧并校验 CRC，支持可选 `de_re` 方向脚；不支持写线圈/写寄存器。
- `spiffs_data/devices/spi_jedec_id_example.json`: 通用 SPI transfer-read manifest 示例，适合 JEDEC ID、简单寄存器读取等短帧场景，返回 hex 字节。
- `spiffs_data/devices/adc_input_example.json`: 通用 ADC one-shot manifest 示例，支持有限采样平均和 scale/offset 转换。
- `spiffs_data/devices/gpio_input_example.json`: 通用 GPIO input manifest 示例，只读取 allowlist 内引脚，不驱动输出。
- `spiffs_data/devices/gpio_output_example.json`: 通用 GPIO output manifest 示例，经 `virtual_device_control`、Guardian policy、Control Agent 本地校验后执行。
- `spiffs_data/devices/relay_control_example.json`: 通用 relay manifest 示例，声明 `high_control`，持久吸合必须 `confirmed=true`，优先使用 `duration_ms` 自动回到 safe state。
- `spiffs_data/devices/pwm_output_example.json`: 通用 PWM manifest 示例，限制 duty、frequency、duration 和 cooldown。
- `spiffs_data/devices/ledc_pwm_example.json`: 显式 `ledc_pwm` manifest 示例，语义贴近 ESP-IDF LEDC，行为与 bounded PWM 输出一致。
- Manifest 工具链：`python3 tools/manifest_lint.py spiffs_data/devices --dry-run` 可在烧录前检查 name/file、manifest_version、permissions、protocol/role/risk、GPIO allowlist、duration/cooldown、只读/控制边界，并输出将使用的 tool 与 target role。`--support-matrix` 输出当前可执行协议和规划中但不可执行协议，`--init-template <protocol> --name <device>` 可生成开发者起始模板。
- Manifest 可信来源：每个示例 manifest 都带有 `<device>.json.sha256` sidecar；固件会校验已存在的 sidecar，控制类 manifest 缺少或不匹配 SHA-256 sidecar 会被 `virtual_device_control` 拒绝执行。runtime header 还会强制 `manifest_version=1`、`permissions=["read"|"control"]` 和 role 边界。开发者修改 manifest 后用 `python3 tools/manifest_lint.py spiffs_data/devices --write-signatures` 更新 sidecar。
- 控制类 manifest 的 `duration_ms` 已改为非阻塞安全恢复：Control Agent 先设置 GPIO/PWM 输出并立即返回，再由后台 `vdev_restore` task 在 duration 到期后恢复 safe state；如果恢复任务创建失败，固件立即回滚到安全态并返回错误。

当前 skills 的有效性不只靠人工阅读判断，已新增 ESP32 平台 benchmark：

- 静态可用性：通过 `skill_list` / `skill_show` 确认 SPIFFS skills 被加载并可读。
- 全量 skill 可读性：runner 会自动为 `spiffs_data/skills/*.md` 生成 `skill_show` 用例，确保每个运行时 skill 都能从 SPIFFS 被读取。
- 固件/运行时策略：通过行为用例验证 sandbox 对受保护路径、高风险动作、确认参数的拦截。
- 行为级 Agent 验证：通过 `inject_msg` 进入 Coordinator 的 ReAct loop，检查自然语言是否触发 Mesh routing、Guardian policy、OutputMessage、workflow、privacy 和 prompt-injection 相关行为。
- 通用协议扩展验证：`protocol_extension_001` 检查 Agent 面对未实现的新 I2C 传感器时，会回答 manifest/通用原语/驱动边界，而不是编造已有工具或直接调用硬件；`protocol_boundary_advanced_001` 检查 Agent 能区分 RS485/Modbus、CAN/TWAI、I2S、RMT、USB CDC、SDIO、BLE GATT 中哪些适合 manifest，哪些必须走固件驱动。
- 运行资源快照：benchmark 在用例前后采集四块 S3 的 `heap_info` 和 `cache_stats`，同时记录 crash pattern，便于判断技能验证期间的资源压力。
- benchmark 数据集在 `benchmarks/skills/espagent_skill_benchmark.jsonl`，runner 为 `tools/benchmark_skills_usb0_3.py`，使用说明见 `docs/SKILL_BENCHMARK.md`。

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

#### Tool call 结构化实现

当前 tool call 是 LLM provider 返回的“模型要调用哪个工具”的结构化对象，固件内部统一抽象为 `llm_tool_call_t`：

```c
typedef struct {
    char id[64];
    char name[32];
    char *input;
    size_t input_len;
} llm_tool_call_t;
```

字段意义：

| 字段 | 意义 |
|------|------|
| `id` | provider 生成的 tool call id，例如 Anthropic 的 `toolu_xxx` 或 OpenAI-compatible 的 `call_xxx`。 |
| `name` | 工具名，例如 `get_weather`、`mesh_send_command`、`ws2812_set`。 |
| `input` | 工具输入 JSON 字符串，由模型根据工具 schema 生成。 |
| `input_len` | `input` 字节长度。 |

不同 provider 的解析路径：

- OpenAI-compatible：`finish_reason == "tool_calls"`，从 `message.tool_calls[]` 读取 `id`、`function.name`、`function.arguments`。
- Anthropic：`stop_reason == "tool_use"`，从 `content[]` 中读取 `type=tool_use` block 的 `id`、`name`、`input`。

`agent_loop` 会把 provider 差异统一成内部 `llm_response_t`：

```c
typedef struct {
    char *text;
    size_t text_len;
    llm_tool_call_t calls[ESPAGENT_MAX_TOOL_CALLS];
    int call_count;
    bool tool_use;
} llm_response_t;
```

ReAct 回合中，如果 `tool_use=true`：

```text
LLM response
  -> llm_proxy 解析为 llm_tool_call_t[]
  -> agent_loop 执行 tool_guard_check
  -> tool_registry_execute(name, input)
  -> 构造 tool_result
  -> 追加到 messages
  -> 下一轮 LLM
```

工具 schema 不是靠 prompt 口头约束，而是由 `tool_registry` 注册后统一构造 tools JSON。每个工具包含：

```json
{
  "name": "mesh_send_command",
  "description": "...",
  "input_schema": {
    "type": "object",
    "properties": {}
  }
}
```

OpenAI-compatible 请求会再转换成 `{"type":"function","function":{...}}` 格式；Anthropic 请求保留 `name`、`description`、`input_schema` 风格。

#### `mesh_send_command` Tool schema

`mesh_send_command` 是 Coordinator 发起跨节点动作的核心工具，当前 schema 约束如下：

```json
{
  "target_node": "optional string",
  "target_role": "sensor_agent | control_agent",
  "action": "read_temperature_humidity | set_status_light | ws2812_set | servo_write | gpio_write",
  "args": "optional object",
  "args_json": "optional raw JSON object string",
  "command_id": "optional string",
  "trace_id": "optional string",
  "ttl_ms": "1000..30000",
  "safety_level": "0 low | 1 medium | 2 high",
  "require_ack": "boolean",
  "async": "boolean, default true",
  "reply_channel": "optional channel for async callback",
  "reply_chat_id": "optional chat id for async callback"
}
```

关键行为：

- `action` 必填。
- `target_node` 存在时优先按节点下发；否则按 `target_role` 下发。
- `require_ack` 默认 true；`async` 默认 true。Coordinator 会立即返回 `async_task_id`，后台 task 等待同一 `command_id` 的 OutputMessage，再把结果作为内部消息回注到 `message_bus`。
- 当前目标角色只开放 `sensor_agent` 和 `control_agent`，避免 LLM 任意向 Guardian 或其它角色下发动作。
- Coordinator 上的 `read_temperature_humidity`、`ws2812_set`、`set_status_light`、`servo_write`、`gpio_write` 会在非 `local=true` 时自动路由到 Mesh，避免用户必须手写 MQTT node id。

#### Tool result 回填

工具执行完成后，`agent_loop` 构造：

```json
{
  "type": "tool_result",
  "tool_use_id": "<tool call id>",
  "content": "<tool output text>"
}
```

对于 `mesh_send_command`，默认异步返回会包含：

```text
OK: queued MQTT mesh command action=<action> ... async_task_id=mesh-task-<command_id>; result will be injected when OutputMessage arrives
```

后台等待 task 收到远端 OutputMessage 后，会向原 `reply_channel` / `reply_chat_id` 注入内部消息：

```text
Internal async Mesh result. task_id=<id> command_id=<id> action=<action> output_message={...espagent.output.v1...}
```

这让下一轮 LLM 能在独立回合“观察”远端节点真实执行结果，而不是只知道 MQTT 已发布。

#### OutputMessage v1

OutputMessage 是 ESP32 节点之间的结构化结果消息，不是 LLM provider 的原生 message。当前由 Sensor/Control 在执行 Mesh command 后发布，也覆盖 Coordinator 本地工具结果和最终回复，schema 为 `espagent.output.v1`。

当前字段：

```json
{
  "schema": "espagent.output.v1",
  "msg_id": "out-<command_id>-<ts_ms>",
  "node_id": "esp32s3-sensor-01",
  "role": "sensor_agent",
  "sender": "sensor_agent",
  "sender_node": "esp32s3-sensor-01",
  "recipient": "coordinator_agent",
  "location": "南京市栖霞区",
  "type": "output",
  "event": "mesh_command_result",
  "command_id": "...",
  "trace_id": "...",
  "action": "read_temperature_humidity",
  "status": "ok",
  "esp_err": "ESP_OK",
  "summary": "...",
  "result": {
    "text": "..."
  },
  "error": null,
  "ts_ms": 123456
}
```

错误时：

```json
{
  "status": "error",
  "esp_err": "ESP_FAIL",
  "error": {
    "code": "ESP_FAIL",
    "message": "..."
  }
}
```

发布路径：

- `espagent/<prefix>/nodes/<node_id>/events`
- `espagent/<prefix>/agent/timeline`

缓存与等待：

- `sensor_mqtt` 收到 `schema=espagent.output.v1`、`type=output` 或 `event=mesh_command_result` 时，会按 `command_id` 缓存。
- Coordinator 的 `mesh_send_command` 在 `require_ack=true` 且 `async=true` 时启动后台等待 task；等待成功后，OutputMessage 原文通过内部消息回注给 `agent_loop`，由 LLM 总结后发给用户。
- 本地普通工具和 `final_reply` 也会发布 OutputMessage，方便 ESP32-P4/Android 按统一 schema 展示工具结果与用户可见结论。

#### Guardian policy gate

Coordinator 发布真正 Mesh command 前，会先发布 `espagent.policy_check.v1`：

```json
{
  "schema": "espagent.policy_check.v1",
  "event": "policy_check",
  "command_id": "...",
  "trace_id": "...",
  "source_role": "coordinator_agent",
  "target_role": "control_agent",
  "target_node": "",
  "action": "set_status_light",
  "safety_level": 1,
  "ttl_ms": 30000,
  "ts_ms": 123456
}
```

Guardian 返回 `espagent.policy_decision.v1`，当前 Coordinator 只在 `decision=allow` 时继续下发真实 command。等待 policy decision 的单次超时限制在 3-15 秒之间；若未收到 decision，Coordinator 会用同一 `command_id` 幂等重发 `policy_check`，最多 3 次，避免 MQTT/Guardian 瞬时拥塞导致真实允许的命令被误判为失败。

当前安全边界：

- Coordinator 侧已经强制先等 Guardian decision。
- Guardian 当前是第一版白名单/安全等级裁决和 metadata-only audit。
- Control 侧已在本机缓存中校验对应 `command_id` 的 Guardian allow decision，action/target_role/target_node 不匹配或无 allow 时拒绝本地执行。
- 直接伪造 Control topic 的防护已经有第一层本地互锁；后续仍需 command queue、人工确认、签名/认证和更完整的 safety interlock。

#### 跨节点 ReAct 闭环

当前跨节点 ReAct 闭环已经达到第一版异步回注形态：

```text
用户自然语言
  -> Coordinator agent_loop
  -> LLM tool_call(mesh_send_command)
  -> policy_check
  -> Guardian policy_decision
  -> MQTT Mesh command
  -> Sensor/Control 执行
  -> OutputMessage espagent.output.v1
  -> Coordinator 后台等待 task 匹配 command_id
  -> message_bus 注入 Internal async Mesh result
  -> agent_loop 触发 LLM 再推理
  -> final_reply / OutputMessage
```

这说明项目已经不是“只发 MQTT 命令”，而是具备“推理 -> 执行 -> 观察结果 -> 再推理”的 ReAct 闭环雏形。

当前仍不是完美闭环：

- 仍是单 `agent_loop` 串行处理用户回合和内部回注，不是独立多 LLM Agent 进程。
- session 已有 `trace_*.jsonl` 保存 tool_use/tool_result/final_reply/async_result_input；串口 `trace_show` 可查单会话 trace，`trace_index` 可列持久 trace 文件，`task_tree` 可按 `trace_id/command_id` 做轻量任务树聚合。
- timeline 已有基础结构化事件和 Guardian StateBoard；Guardian 具备人工确认队列和一次性 approval token。仍缺 watchdog 聚合、跨节点长期 trace 查询 API 和完整任务 DAG。
- Control 已做本地 Guardian decision 校验、command queue、签名/认证、人工确认重试路径和可选硬件 interlock GPIO。生产级安全仍需要私有 broker ACL/TLS、真实互锁接线验证、决策消息认证和密钥轮换流程。

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
| `automation_create_workflow` | 创建并启动确定性多步工作流，适合“先红色，等 10 秒，再蓝色”这类顺序/延迟动作。 |
| `automation_create_rule` | 创建持久化条件动作规则，适合“湿度低于阈值就开灯/打开加湿器”这类后台监控。 |
| `automation_list` / `automation_remove` | 查看或删除 workflow/rule。 |

### Agent Sandbox

ESP32-S3 上不能实现 Linux/Docker 容器，但当前固件已经加入第一版 capability sandbox：

- 新增 `main/tools/tool_sandbox.c/.h`。
- `tool_registry_execute()` 在执行任何工具前先调用 `tool_sandbox_check()`；`tool_mesh_send_command_execute()` 自身也会调用同一检查，避免 automation runtime 直接调用 Mesh 工具时绕过沙箱。
- 沙箱按工具风险分为 `read_only`、`low_control`、`medium_control`、`high_control`、`privacy`、`system`。
- 沙箱会检查当前 role/capability、受保护路径、Mesh TTL/safety/action、automation interval/cooldown、音频时长/音量，以及高影响动作是否带 `confirmed=true`。
- `write_file` / `edit_file` 写 `/spiffs/config`、secrets 相关路径会被拒绝；修改 `/spiffs/skills` 需要显式 `confirmed=true`。
- `automation_create_rule` 被视为高影响持久动作，默认需要 `confirmed=true`，避免 LLM 在没有用户确认时创建长期后台规则。
- 该 sandbox 不是替代 Guardian，而是 Guardian 前面的本地确定性工具边界；当前已补人工确认 token 和 Mesh command HMAC，后续还应继续补决策消息认证、生产密钥轮换流程和更细的 per-tool capability token。

### 硬件能力

- WS2812 RGB：默认 GPIO48。
- 舵机：GPIO5，LEDC PWM，50Hz，500-2500us。
- AHT10/AHT20：温湿度。
- SGP30：空气质量 eCO2/TVOC。
- BH1750/GY-30：光照 lux。
- HC-SR05：超声波距离/存在检测，Echo 必须做电平保护。
- 3-wire presence/PIR：数字人体存在输入。
- MAX98357：I2S 音频测试。
- INA / INMP-class I2S Mic：原硬件规划为 `BCLK=GPIO6`、`WS=GPIO7`、`LR=GND`、`SD=GPIO8`；当前分支已暂缓该采集链，避免挤占 MCU 内存与 I2S 资源。
- GPIO：仅允许安全 allowlist，避免破坏 USB/boot。

### 主动性

- `cron_add` 支持 recurring、one-shot、daily。
- `automation_create_rule` 支持把温湿度阈值联动注册为后台默认任务，规则存入 `/spiffs/automation.json`，即使当前对话切换到其它任务，automation task 仍会按 interval/cooldown 继续监控和触发。
- `automation_create_workflow` 支持多个一次性顺序/延迟任务；每个 workflow 创建独立临时 `workflow_task`，执行完成后释放，不属于重启恢复型持久任务。
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
| `spiffs` | 约 11.8MB | memory、sessions、skills、cron、automation、config 文件。 |
| `coredump` | 64KB | 预留 coredump，当前 sdkconfig 禁用 flash coredump。 |

最近构建结果：

- 2026-06-21 加入 Gateway/device_registry/管理 API 后，`build/ESPAgent.bin` 约 `0x195920`。
- 最小 app 分区剩余约 `0x6a6e0`，约 21%。
- `0x1902c0` - `0x191d40` / 约 22% 剩余是 Lua runtime 和多节点必要性增强阶段的历史构建结果，不代表 Gateway 合入后的当前体积。

OTA 状态：

- 分区表已经是双 app slot：`ota_0` / `ota_1` 各 2MB，`otadata` 记录启动状态。
- 2026-06-17 已把 `main/ota/ota_manager.c` 编入固件，并在串口 CLI 增加 `ota_info` 和 `ota_update <https_url_to_ESPAgent.bin>`。
- 2026-06-21 已新增 `ota_gateway_plan` 工具和 `/ota/plan` 管理 API，用于创建结构化 OTA 运维计划并发布到 timeline，供 P4/Android 展示和 Guardian 后续审批链路使用。
- `ota_update` 只接受 HTTPS URL，使用 ESP-IDF `esp_https_ota` 和系统证书包下载 app `.bin`，成功后自动重启到新分区。
- OTA 当前不暴露为 LLM/Feishu tool。后续如果要远程触发，必须经过 Guardian policy、人工确认、镜像来源校验和版本/角色校验。
- Agent 在 OTA 中的定位不是“写新固件代码”或“在 MCU 上编译固件”，而是升级运维编排：开发者或 CI 先准备好 `ESPAgent.bin`，Agent 后续可以负责发现版本、匹配角色、请求 Guardian 审批、询问用户确认、下发 OTA 任务、观察重启和汇总升级结果。
- 如果没有公网服务器，当前代码仍需要 ESP32 可访问的 HTTPS app bin URL；局域网 HTTP、本机上传、串口传输和自签名 HTTPS 还没有实现。
- 当前四个 S3 角色仍使用同一代码但不同 build-time profile；如果 OTA 镜像内写死了另一个 `NODE_ID` / `NODE_ROLE`，升级后会改变板子的角色。后续更推荐把角色身份迁移到 NVS，再让同一个 OTA 镜像适配四块板。

## 运行时任务

核心 FreeRTOS 任务：

- `agent_loop`: Core 1，处理 LLM 和工具调用。
- `feishu_ws`: Core 0，Feishu WebSocket。
- `outbound`: Core 0，统一发送 Feishu/WebSocket 回复。
- `serial_cli`: Core 0，串口 REPL。
- `cron`: 定时任务轮询。
- `proactive`: 主动检查。
- `automation`: Core 0，后台执行持久化条件规则，周期读取 Sensor 并触发 Control。
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
- esp-claw-like 单板 runtime 第一版：新增 capability registry、role capability profile、统一 event router、Memory v2 和 dynamic extension catalog；旧 `tool_registry` 保持兼容，LLM 工具列表开始按当前 ESP32 角色过滤。
- SGP30、AHT10/AHT20、BH1750、HC-SR05、WS2812、舵机、MAX98357 等工具或驱动。
- 高德天气 `get_weather`，默认南京市栖霞区。
- SNTP 启动校时，默认 `ntp.aliyun.com`；`get_current_time` 优先使用已同步系统时间，HTTP Date 仅作为兜底。
- Tavily/Brave 搜索。
- prompt 安全边界和 execution-side tool guard。
- SPIFFS memory、daily notes、sessions、skills。
- Cron daily proactive 和 periodic proactive service。
- Automation runtime：`automation_create_workflow` 支持顺序/延迟 Mesh 动作，`automation_create_rule` 支持持久化条件联动，`automation_list`/`automation_remove` 支持查询和删除。
- Automation 并发边界已明确：`rule_task` 只负责条件规则，所有规则在一个后台 task 中串行轮询；workflow 由独立 `workflow_task` 执行，当前不做重启恢复，且运行中 workflow 的强制取消能力还需要补。
- 反问承接的启发式上下文提示。
- Agent Mesh Phase 1：node identity、capabilities、responsibilities、MQTT state/telemetry/event、node/role command topic。
- 四 ESP32 role profile 文档：coordinator、sensor、control、guardian。
- Agent Mesh Phase 1.5：新增 `main/mesh` 协议层、`main/roles` 角色服务骨架，并让 `espagent_app` 根据 role/capability 选择性启动 LLM/聊天、scheduler、sensor monitor、control demo、guardian/display 边界服务。
- MQTT node/role command 已接入 `mesh_protocol` 做 JSON schema 解析、`action` 必填校验、`target_node`/`target_role` 匹配校验；Sensor `read_temperature_humidity` 和 Control WS2812/status-light 已有白名单执行路径，其它命令当前仍是 dry-run 日志，不执行硬件动作。
- Feishu 通信板 MQTT 桥接已完成第一版：`feishu_inbound` 发布到 node events、`agent/dispatch`、`agent/timeline`，`feishu_outbound` 发布到 node events 和 `agent/timeline`；MQTT queue 支持连接前事件暂存，MQTT packet remaining length 解析已修正。
- Coordinator 已注册 `mesh_send_command` 工具，可向指定 node 或 role 发布标准 MQTT Mesh command。
- Coordinator 已加入确定性 Mesh 路由和假成功保护：常见温湿度/控制板状态灯飞书请求会直接下发 MQTT Mesh；如果本轮没有实际 Mesh/routed tool 执行，固件不会允许最终回复声称“已发送”。
- Sensor 角色已支持白名单 `read_temperature_humidity` Mesh command，并将 AHT10/AHT20 执行结果发布为 `mesh_command_result` 到本节点 events 和全局 timeline。
- Control 角色已支持 WS2812/status-light 白名单 Mesh command，并将执行结果发布为 `mesh_command_result` 到本节点 events 和全局 timeline。
- S3 timeline 已补齐基础结构化事件流：`tool_use`、`tool_result`、`mesh_command_queued`、`mesh_command_result`、`final_reply` 和 error 会发布到 `espagent/cube1345/agent/timeline`，供 P4/Android/Display Agent 观察。
- 2026-06-17 development 分支推进 Guardian + OutputMessage + policy gate：USB3 推荐改为 `esp32s3-guardian-01` / `guardian_agent`；Coordinator `mesh_send_command` 下发前先发布 `schema=espagent.policy_check.v1`，Guardian 返回 `schema=espagent.policy_decision.v1` 后才继续；下游 Mesh 执行结果升级为 `schema=espagent.output.v1`；Coordinator 默认异步等待远端 OutputMessage 并通过内部消息回注给 LLM；Guardian 会对关键 timeline 事件发布 `schema=espagent.guardian.audit.v1` 审计事件。
- 2026-06-17 已完成闭环增强第一版：`mesh_send_command` 默认异步，返回 `async_task_id` 后由后台 task 等待 OutputMessage 并通过 `message_bus` 回注；本地普通工具和最终回复也发布 `espagent.output.v1`；session 新增 `trace_*.jsonl` 记录 tool_use/tool_result/final_reply/async_result_input；Guardian StateBoard 可通过 `stateboard_show` 查询；Control 侧已强制校验本机缓存的 Guardian allow decision。
- 2026-06-17 已完成 USB0-USB3 四板验证：`tools/flash_roles_usb0_3.sh` 依次烧录 Coordinator/Sensor/Control/Guardian；`tools/stress_mesh_usb0_3.py --rounds 3 --interval 1.5 --settle 25 --quiet` 通过，6/6 command queued，Sensor 3/3，Control 3/3，Guardian policy 6/6，audit 18，无 crash；`stateboard_show` 在 USB3 返回最近 policy_decision 列表。
- 2026-06-17 已完成 Feishu 真实入口验证：`tools/stress_feishu_usb0_3.py --rounds 1 --interval 35 --settle 80 --quiet` 通过，飞书发送 2/2，异步 OutputMessage 回注 2/2，Sensor 1/1，Control 1/1，最终 Feishu 回复 4 次，无 crash。当时 Sensor 读温湿度返回 AHT10 未找到，这是当时硬件/接线状态，不是 MQTT Mesh 链路故障。
- 2026-06-18 已完成 AHT20 实物验证：USB1 `esp32s3-sensor-01` 识别 AHT20，读数恢复到 `27.x C / 45-46%RH`；`read_temperature_humidity` 和 Sensor MQTT telemetry 均优先使用 AHT20 数据，topic 为 `espagent/cube1345/nodes/esp32s3-sensor-01/telemetry`。
- 2026-06-18 已完成湿度条件自动化验证：创建 `humidity_percent > 40` 的 `automation_create_rule`，后台 `automation` task 周期读取 USB1 AHT20，触发 USB2 `control_agent` 执行 `set_status_light`，串口确认 WS2812 GPIO48 输出红色 `rgb=(255,0,0)`；验证后测试规则已删除。
- 2026-06-18 已修复 automation task 栈溢出：`ESPAGENT_AUTOMATION_STACK` 提升到 `12 * 1024`，`automation_engine_start()` 创建 `automation` task 时使用该配置。
- 2026-06-17 已新增 OTA 固件升级能力：`ota_manager` 正式参与构建，串口 CLI 支持 `ota_info` 查看 OTA 分区，`ota_update <https_url_to_ESPAgent.bin>` 从 HTTPS app bin 更新 inactive OTA slot 并成功后重启。该能力当前仅用于本地维护，不进入 LLM tool registry。
- OTA 的 Agent 化规划已明确：不是让 ESP32 Agent 生成代码，而是把 OTA 作为多 Agent 运维闭环，由 Coordinator 发现/调度、Guardian 审批、安全确认、目标节点执行、P4/Android 展示进度和结果。
- ESP32-P4+C6 显示端已在 `/home/cube/WorkSpace/ESP/lvgl_traffic_control` 中实现基础 Display Terminal：新增 LVGL `AgentMesh` tab、裸 TCP MQTT 订阅、事件队列、内存 timeline buffer、节点状态卡和最终结果显示；P4 构建通过并已烧录到 `/dev/ttyACM0`，Wi-Fi/MQTT connect/subscribe 已验证，实时收包和屏幕 timeline 实机验证仍需继续。
- 新增 Agent Mesh / MCU edge AI 运行时 skills：`agent-mesh-coordination.md`、`mqtt-mesh-operations.md`、`mcu-edge-ai-boundaries.md`、`mesh-resource-planning.md`。这些文件把联网调研得到的边界固化进运行时 prompt：角色化 Agent Mesh 调度、MQTT pub/sub 协作、MCU 端 TinyML/小模型推理方向、以及“当前 ESP32-S3 不运行本地完整 LLM”的能力边界。
- 2026-06-19 已根据 OWASP LLM Top 10、NIST AI RMF、MCP/Agent 工具安全和 Agentic AI 安全资料补充三类安全运行时 skills：`agent-sandbox-permissions.md` 约束工具权限、风险分级、dry-run、TTL 和硬件互锁；`privacy-data-minimization.md` 约束家居隐私数据分级、本地脱敏、最小上下文和 MQTT/Display/Memory 边界；`tool-integrity-and-prompt-injection.md` 约束外部内容作为数据而非指令，防止 prompt injection、tool poisoning 和工具输出越权。
- 2026-06-19 已完善 ESP32 平台 skills benchmark：`benchmarks/skills/espagent_skill_benchmark.jsonl` 定义运行时行为用例，`tools/benchmark_skills_usb0_3.py` 负责 `/dev/ttyUSB0-3` 四板串口执行、role 检查、自动生成全量 SPIFFS skill 可读性用例、剔除输入回显后的日志打分、资源快照和 artifact 输出；当前支持离线 `--validate-only` / `--list-cases`，也支持板端 skill loading、Mesh routing、sandbox、privacy、prompt-injection、workflow 和全量 skill 文件读取测试。本轮完整四板 run `fixed_full_20260619_210709` 为 23/24，唯一失败来自 prompt-injection `must_not` 规则过宽；修正后单项 run `fixed_prompt_20260619_212052` 为 1/1，串口确认无 `read_file` 执行、无 memory 泄露。
- 2026-06-21 软件侧补齐 runtime hardware manifest 体系：固件强制 `manifest_version=1`、`permissions`、role/risk 边界和 SHA-256 sidecar；控制类 manifest 的 `duration_ms` 改为非阻塞后台 safe-state restore；`tools/manifest_lint.py` 增加 `--support-matrix`、`--init-template` 和签名更新；SPIFFS 示例扩展到 10 个 manifest，覆盖 read-only I2C/UART/Modbus/SPI/ADC/GPIO input 与 control GPIO/relay/PWM/LEDC。离线验证结果：manifest dry-run 10/10，benchmark validate-only 40/40，`idf.py build` PASS，`ESPAgent.bin=0x16f020`，app 分区剩余约 28%。
- 2026-06-21 继续补 Control/Guardian 安全闭环：新增 `main/control/command_queue.c/.h`，Control Mesh 命令进入 admission queue 后再执行，具备 duplicate command_id 拦截、TTL 检查、单执行器互锁、emergency stop latch 和 actuator state JSON；新增 `main/mesh/mesh_auth.c/.h`，在 `ESPAGENT_SECRET_MESH_AUTH_KEY` 非空时对 Mesh command 做 HMAC-SHA256 签名/验证，并支持 `ESPAGENT_SECRET_MESH_AUTH_PREVIOUS_KEY` 作为轮换期验证 key；`policy_check` 携带 `args_json`，Guardian 对 `virtual_device_control` 做参数级检查；Guardian approval queue 支持 `approval_list`、`approval_confirm`、`approval_deny`，已确认的 `approval_id` 可被同 action/role 的重试一次性消费；`automation_remove` 现在能让运行中的 workflow 在下一步前停止；新增串口 `trace_show <chat_id> [max_events]`、`trace_index` 和 `task_tree <chat_id> [max_events]` 查询 per-session ReAct/tool trace 与轻量任务树。
- 2026-06-21 参考 `esp-claw` 完成单板 Agent runtime 升级第一版：新增 `main/capability/`，把现有工具镜像成 capability descriptor，包含 family、risk、flags 和 execute；新增 `role_capability_profile`，让不同 ESP32 角色只向 LLM 暴露当前角色允许的工具集合；新增 `main/events/` 统一事件层，`message_bus` inbound/outbound 和 tool/capability 调用会产生 `espagent.event.v1` 事件并写入 session trace；新增 `memory_v2`，结构化保存用户画像 fact 和 skill observation；新增 `dynamic_extension` catalog，汇总当前可执行 manifest primitive 与计划中的 Lua/CAN/BLE/1-Wire/I2S/RMT/USB/SDIO 扩展边界。构建验证通过，`ESPAgent.bin=0x171d40`，app 分区剩余 `0x8e2c0`。
- 2026-06-21 继续优化 capability 执行面：`tool_registry_execute()` 改为 capability-first，保留 sandbox 和 legacy tool fallback；新增 `tool_registry_execute_as()`，让 Agent、Serial CLI、subagent 使用不同 caller 类型，避免权限边界混淆。Lua 脚本能力已从“可选规划”推进到真实固件能力：`ESPAGENT_ENABLE_LUA_RUNTIME=1`，`georgik/lua (5.5.0~7)` 已链接进固件；`main/lua/espagent_lua_runtime.c/.h` 提供路径校验、脚本大小限制、timeout、stop flag、print 捕获、`args` table、`ESPAGENT_ARGS_JSON`、`require('espagent')`、受限 `package.path` 和 runtime 状态。`lua_runtime_info`、`lua_list_modules`、`lua_list_scripts`、`lua_run_source`、`lua_run_script`、`lua_run_script_async`、`lua_list_jobs`、`lua_get_job`、`lua_stop_job` 已进入 tool/capability/sandbox/profile 体系。最新构建通过，`ESPAgent.bin` 约 `0x190310`，app 分区剩余约 `0x6fcf0`。
- 2026-06-21 阶段性结论更新：软件侧不再只是工具堆叠，而是进入“每个 ESP32 角色都有 esp-claw-like 单板 Agent runtime，角色间再通过 MQTT Mesh 协作”的架构。当前没有直接把 esp-claw 全量硬件 Lua module 裸接入 ESPAgent；采用的是兼容 ESPAgent 现有 Mesh/Guardian/OutputMessage 的安全重写：Lua 通过 `espagent.call_capability(name, args_json)` 调用现有 capability，仍经过 sandbox、角色权限、Guardian/Mesh/硬件互锁边界；`gpio/i2c/adc/pwm/rmt/ble/display/camera/audio` 等 esp-claw 式直接硬件 module 当前在 `lua_list_modules` 中标记为 `not_linked`，开发者应通过 manifest primitive 或 ESPAgent capability 访问硬件。
- 2026-06-21 实机部署验证：按 USB0-3 顺序烧录四个 ESP32-S3，USB0=`coordinator_agent`，USB1=`sensor_agent`，USB2=`control_agent`，USB3=`guardian_agent`；`tools/verify_roles_usb0_3.py --echo` 四角色身份检查 PASS。USB0 执行 `tools/test_lua_usb0.py --echo`，在 SNTP 同步后 Lua runtime 完整 smoke 7/7 PASS，覆盖 runtime info、module list、script list、同步脚本、inline source、async job、job list。`tools/test_lua_roles_usb0_3.py` 对四个角色执行 `lua_runtime_info` 与 `lua_run_source`，结果 8/8 PASS，证明 Lua runtime 不只部署在 coordinator。
- 2026-06-21 esp-claw 优势吸收策略：后续值得迁移的是工程化能力，而不是替换 ESPAgent 架构。优先级包括：Board Descriptor/Board Profile，用结构化方式记录每块 S3/P4 的 pins、sensors、actuators、safe ranges、dangerous actions 和 validation status；Lua package/layout，把 `/spiffs/scripts/builtin/`、`/spiffs/scripts/user/`、`/spiffs/skills/<skill>/scripts/` 区分为内置可信、用户上传和 skill 绑定脚本；Lua module/docs/skill 生成思想，形成“Lua script + manifest primitive + capability schema + skill 文档 + benchmark case”的开发者工具链；Capability lifecycle，为每个 capability 增加 init/start/health_check/stop/benchmark 状态；P4/Android Script Console，用于显示 Lua job、capability list、Guardian decision、benchmark 和 trace。禁止方向：不允许 Lua 裸控 GPIO/I2C/ADC/PWM/RMT/BLE/display/camera/audio 绕过 tool_registry/capability_registry；不允许脚本跳过 sandbox、Guardian、Mesh policy 或 Control interlock；不把 ESPAgent 做成 esp-claw 复制版，项目特色仍是多 ESP32 Agent Mesh、Guardian 权限治理、OutputMessage/ReAct 闭环和可视化推理链路。
- 2026-06-21 多节点必要性增强第一轮已落地并完成四板实机回归：Sensor 不再只是“读一下传感器”，MQTT telemetry 增加最近采样 ring cache 与 EWMA 滤波输出 `temp_avg`、`humidity_avg`、`light_lux_avg`、`sample_count`，并基于默认阈值 `temperature>35.0C`、`humidity<35%`、`humidity>70%`、`light<10lux` 在状态变化时发布 `espagent.sensor_event.v1` 到 node events、全局 alerts 和 timeline；Control 在远程 actuator 命令执行后发布 `espagent.control_state.v1`，包含 busy、emergency_stop、interlock 和最近动作状态，便于 Android/P4 展示执行器状态；Guardian 的 `policy_decision` 增加 `risk_score` 与 `privacy_mode=metadata_only`，同时订阅 `espagent.../nodes/+/state` 和 `espagent.../nodes/+/telemetry` 聚合 `watchdog_node_state` / `watchdog_node_telemetry` 到 StateBoard。针对实机压测中暴露的 policy 回流拥塞，已补 Guardian watchdog StateBoard 节流、MQTT critical 队列调整和 Coordinator `policy_check` 幂等重试。最新 app 约 `0x191d40`，2MB app 分区剩余约 22%；四角色身份检查 PASS，连续两轮 5 轮 Mesh 压测 PASS。
- 2026-06-21 参考 Ai-Thinker BLE Mesh Gateway 的强项完成 ESPAgent Gateway 软件侧第一版：新增 `main/device/device_registry.*` 持久化 MQTT Mesh 节点和外部设备；新增 `main/gateway/ble_mesh_bridge.*` 作为 BLE Mesh bridge boundary；新增 `main/tools/tool_gateway.*` 并注册 `gateway_status`、`gateway_register_ble_mesh_device`、`gateway_ble_mesh_send`、`ota_gateway_plan`；`wifi_onboard` 增加 `/status`、`/devices`、`/gateway/ble_mesh/register`、`/ota/plan` API，`onboard_html` 增加 Gateway/BLE/OTA 管理区。该轮 `idf.py build` PASS，`ESPAgent.bin=0x195920`，app 分区剩余 `0x6a6e0`，约 21%。当前 BLE Mesh 只完成设备登记和命令意图发布，未链接真实 BLE Provisioner 后端；OTA Gateway 只生成计划并发 timeline，未开放远程刷机执行。
- Feishu 通信板时间同步已补齐：Wi-Fi 连接后启动 SNTP 校时，`get_current_time` 不再优先依赖 Google Date 头；天气工具仍使用高德 `get_weather`，默认南京市栖霞区。
- 本地私有配置当前已设置为 Feishu/LLM 入口板：`esp32s3-coordinator-01` / `coordinator_agent` / `coordinator,communication,llm,dispatch,timeline,alerts`。
- 已烧录 coordinator 固件到 `/dev/ttyUSB0`，目标 ESP32-S3 MAC 为 `14:c1:9f:2d:76:20`；串口日志确认 Feishu、LLM、agent_loop 和 coordinator role 均启动，本地 sensor monitor 与 boot servo demo 已按角色跳过。
- 已修复飞书消息统一回复 `抱歉，我这次处理请求时遇到了错误。` 的根因：system prompt 旧 16KB buffer 被截断到 UTF-8 多字节字符中间，导致 OpenAI-compatible LLM API 返回 HTTP 400 `invalid unicode code point`。现在 prompt buffer 为 24KB，`context_builder` 和 `agent_loop` 都做 UTF-8-safe truncation。
- 修复后已烧录并通过串口 `inject_msg system debug hello` 验证：LLM API 正常返回，最终回复成功进入 outbound。
- 已修复飞书入口压测中的 `Tmr Svc` 栈溢出：heartbeat timer callback 不再直接读 SPIFFS/注入消息，而是启动 `heartbeat_worker`；FreeRTOS timer service task stack 提升到 4096。
- 已完成飞书入口 2 轮压力测试：`tools/stress_feishu_usb0_3.py --rounds 2 --interval 30 --settle 220 --quiet`，飞书发送 4/4，sensor 2/2，control 2/2，0 崩溃，结果 `PASS`。
- 2026-07-07 已继续修 coordinator 的普通问答 / 项目说明 / 内存压力路径，并完成 USB0 实机验证：
  - 新增三层意图路由：`general_qa`、`project_explanation`、`hardware/orchestration`。普通问答不再默认走 coordinator 的重型设备编排 prompt；项目介绍/架构说明类问题使用单独 explanation prompt；真实硬件控制继续走原有 Mesh/tool 路线。
  - 新增 deterministic 数值比较 fast-path，并已在 USB0 串口 `inject_msg system qa_test4 "Which is larger, 13.8 or 13.11?"` 上验证：日志显示 `smalltalk=0 general_qa=1 project_explain=0`，最终回复 `13.8 is larger.`。
  - 修正英文 smalltalk 误判：原先 `Which` 中的 `hi` 会误触发 smalltalk；现已改为 ASCII 单词边界匹配，避免普通英文问句被错判为寒暄。
  - 已验证 explanation route：USB0 串口 `inject_msg system explain_test5 "Explain project architecture?"` 日志显示 `direct_reply=1 smalltalk=0 general_qa=0 project_explain=1`，最终回复以 `# ESPAgent Project Architecture` 开头。
  - 已确认当前瓶颈不是“PSRAM 不够”，而是 coordinator 在 LLM TLS/HTTP 路径上的 internal RAM 过低且碎片化严重。真实日志中失败前常见状态约为 `internal_free≈21KB`、`largest_internal=7680`，而 `llm_proxy.c` 的 tight 阈值为 `96KB / 32KB`。因此问题本质是 coordinator 过重 + 长 prompt/history/tools body + internal heap 碎片化，而不是总 RAM 用尽。
  - 已新增 tight-memory 首轮减载策略：当 `llm_transport_heap_is_tight()` 为真时，coordinator 首轮改用更短的 tight prompt，历史上限压到 3，并跳过 skills/memory/clarification 扩展；同时 `LLM_COORDINATOR_MAX_TOKENS_TIGHT` 从 `768` 下调到 `512`。
  - 该减载策略已在 USB0 实机上验证有效：对之前容易触发 `LLM transport failed` 的长 explanation 问句 `Explain the project architecture and why normal QA should not use the device orchestration context.`，日志显示 `tight=1`、`skipping skill/memory context expansion`、`max_tokens: 512`、请求体约 `3586 bytes`，最终成功返回 `## ESPAgent Project Architecture`；在修复前，同类请求体约 `8786 bytes`，并多次在 transport 阶段失败。
  - 本轮相关代码变更已推送到 GitHub `development` 分支，关键 commit 包括：
    - `1262f14` `Improve coordinator direct replies and LLM diagnostics`
    - `2075f8d` `Route general QA away from orchestration prompt`
    - `b3539f3` `Split project explanation from general QA routing`
    - `5569604` `Tighten QA and project explanation intent matching`
    - `1981843` `Reduce coordinator LLM pressure under tight memory`
- GitHub 远端：
  - HTTPS: `https://github.com/cube1345/ESP32_AgentMesh.git`
  - SSH: `git@github.com:cube1345/ESP32_AgentMesh.git`
  - 仓库可见性：private
  - 当前项目已重新以 `ESP32_AgentMesh` 私有仓库承载，保留 ESPAgent 固件名称和 Agent Mesh 代码架构。
  - `main` 已推送；初始固件快照提交为 `b6cae3f`（`Initialize ESP32 AgentMesh firmware`）。
  - Feishu 入口 Mesh 压测修复、确定性 Mesh 路由、压测脚本、skills 和文档更新已推送到 `main`：`dbe3c41`（`Stabilize Feishu mesh pressure path`）。
  - 本地 `artifacts/` 仅保存压测串口日志，未纳入远程提交。

## 当前限制

- 当前 Coordinator 仍是单 ESP32-S3 的单 `agent_loop`，不是多个独立 LLM Agent 进程；四板协作主要通过 MQTT Mesh command、OutputMessage 和 timeline 完成。
- MQTT command 现在已做基础 schema/目标校验；Sensor `read_temperature_humidity` 和 Control `set_status_light`、`ws2812_set`、`servo_write`、`gpio_write` 已有白名单执行路径。
- Sensor 角色目前主要开放 `read_temperature_humidity` 和 `virtual_device_read` Mesh 执行路径；Control 角色开放低/中风险白名单执行路径，高风险应进入 Guardian approval queue，并在人工确认后通过一次性 `approval_id` 重试。
- MQTT command queue、人工确认、Mesh command HMAC 和可选 interlock GPIO 已有固件实现；仍不是最终生产安全系统，因为私有 broker ACL/TLS、决策消息认证、真实互锁接线和密钥轮换 SOP 还需要部署验证。
- 已有 role-visible capability profile，LLM 可见工具会按 `coordinator_agent`、`sensor_agent`、`control_agent`、`guardian_agent` 过滤；但这仍是固件内的静态 profile 规则，后续还需要把角色身份迁移到 NVS，并让工具可见性、Lua 脚本包和 manifest 权限共同受统一 profile 管理。
- Coordinator 现在会把 Feishu 入站广播到 `agent/dispatch`，并且能把普通温湿度读取、远程/控制板状态灯颜色请求转成正式 Mesh command；复杂自然语言任务的通用 Mesh planning 仍未完成。
- Automation 已能覆盖顺序/延迟动作和温湿度条件联动，`automation_remove` 可让运行中的 workflow 在下一步前停止；但自然语言暂停/恢复/删除默认任务、规则状态面板、更复杂的多传感器条件表达式、workflow 持久化恢复和冲突检测还没有完善。
- timeline topic 已有 Feishu inbound/outbound、基础 ReAct 工具调用/结果、policy_check/policy_decision、Mesh 下发/结构化 OutputMessage、Guardian audit、StateBoard 更新和最终回复事件；串口侧已有 trace 持久化查询和轻量任务树聚合，watchdog 聚合还不完整。
- Memory 写入依赖模型主动调用文件工具，没有固件侧强制 consolidation。
- session 已新增 `trace_*.jsonl` 保存 tool_use/tool_result/final_reply/async_result_input；已有 `trace_index` 和 `task_tree` 串口查询，还缺跨节点长期关联查询、压缩归档和远程 API。
- system prompt 仍集中在 C 字符串中，后续可以拆成 SPIFFS prompt fragments。
- Coordinator 当前的主要内存瓶颈仍是 internal RAM，而不是 PSRAM：即使 PSRAM 仍约 8MB 可用，LLM 的 TLS/HTTP/AES 路径仍可能因为 internal heap 太小或连续块太碎而失败。当前已通过“短 prompt / 短 history / 跳过 skills-memory 扩展 / tighter max_tokens”把最容易炸的路径压下去，但这不等于 coordinator 已经拥有充裕 internal RAM。
- 2026-07-07 已继续做一轮保守的 Wi-Fi/LWIP 内存收缩，并完成 build + USB0 实机验证：
  - 配置侧已下调 `CONFIG_LWIP_MAX_SOCKETS=12`、`CONFIG_LWIP_MAX_ACTIVE_TCP=12`、`CONFIG_LWIP_MAX_LISTENING_TCP=12`、`CONFIG_LWIP_MAX_UDP_PCBS=12`、`CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=8`、`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=16`、`CONFIG_ESP_WIFI_MGMT_SBUF_NUM=16`。
  - 这轮改动已通过完整 `idf.py build`，随后已烧录 `/dev/ttyUSB0` coordinator。
  - 对同一条长 explanation 压力问句再次实测，日志仍显示 `tight=1`，说明 coordinator 的 internal RAM 依旧紧张；但请求继续稳定成功，`Calling LLM API ... body: 3714 bytes`，且 `Before LLM HTTP heap: internal_free=20331 largest_internal=8192 psram_free=8186724`。
  - 与上一轮常见的 `largest_internal=7680` 相比，这轮至少把连续 internal block 稍微抬高到 `8192`，说明 Wi-Fi/LWIP 收缩对 TLS/HTTP 路径有一定帮助，但还没有把 coordinator 从 tight-memory 区间拉出来。
- 2026-07-07 又补了一轮 coordinator 背景 MQTT 降噪，目标是让 LLM 调用窗口少受 housekeeping 干扰：
  - `agent_loop.c` 在 coordinator turn 开始和每次 `llm_chat_tools()` 前都会调用 `espagent_net_guard_defer_background(20000)`，把当前对话标记为高优先级网络窗口。
  - `sensor_mqtt.c` 对 coordinator 新增 `mqtt_coordinator_background_housekeeping_allowed()`：当 net guard 处于 defer 窗口时，暂停周期性 `nodes/.../state` 发布，并跳过 `nodes/+/state`、`nodes/+/telemetry` 的 watchdog/device_registry 处理和高频日志。
  - 这轮改动已完成 build、烧录 `/dev/ttyUSB0` 和实机复测。热机后的 explanation 压测里，可以直接看到 `Coordinator MQTT housekeeping deferred during active LLM window: periodic_state/node_state/node_telemetry`，说明静默窗口已经在 LLM 前生效。
  - 最新热机样本下，长 explanation 请求仍稳定成功；`Before LLM HTTP heap` 约为 `internal_free=23119 largest_internal=7680 psram_free=8186720`。这说明背景噪声被压下去了，但 `largest_internal` 仍然卡在 7.5KB~8KB 档，internal RAM 紧张问题还没有根治。
- 后续如果继续优化 internal RAM，应优先压背景 MQTT state/telemetry/timeline 噪声和并发窗口，再评估是否需要进一步下调 socket/buffer 或动更高风险的 TLS 参数。
- NVS 只有 24KB，后续如果存节点表、证书或更多运行时配置，建议扩容。
- 本地工作区 `.git` 在当前工具环境里是只读 tmpfs 占位，普通 `git status` 不可用；当前私有仓库推送使用 `/tmp/ESP32_AgentMesh.git` 作为临时 Git 元数据目录完成。

## esp-claw 吸收边界

当前已经吸收的是 esp-claw 值得保留的“单板 Agent runtime 工程思想”，不是直接替换 ESPAgent：

- 已落地：capability registry、role-visible capability profile、统一事件流、Memory v2、dynamic extension catalog、受控 Lua runtime。
- 已验证：四个 ESP32-S3 角色均可运行 Lua runtime；USB0 Lua smoke 7/7 PASS，USB0-3 跨角色 Lua smoke 8/8 PASS。
- 保留特色：四角色 Agent Mesh、Guardian policy、OutputMessage/ReAct 闭环、MQTT Mesh、P4/Android 可视化推理链路。
- 安全红线：Lua 只能通过 `espagent.call_capability(name, args_json)` 进入 ESPAgent capability/tool 路径，不能裸控 GPIO/I2C/ADC/PWM/RMT/BLE/display/camera/audio，不能绕过 sandbox、role profile、Guardian、Mesh policy 或 Control interlock。
- 后续优先吸收：Board Descriptor/Board Profile、Lua package/source metadata、capability lifecycle、脚本/manifest/skill/benchmark 一体化开发者工具链、P4/Android Script Console。

## 未来规划

### P0

- 持续回归当前软件侧闭环：USB0 Coordinator、USB1 Sensor、USB2 Control、USB3 Guardian、ESP32-P4 Display Terminal。
- 继续回归本轮多节点自治增强：USB1 telemetry 的 EWMA/threshold 事件、USB2 control_state 快照、USB3 watchdog StateBoard 聚合和 Mesh policy 链路已经通过四板压力测试；下一步重点转向 Android/P4 是否能订阅并稳定显示。
- 继续实机验证 Control command queue、一次性 `approval_id`、可选 interlock GPIO、emergency stop 和 actuator state 在真实板端的串口/MQTT表现。
- 验证 ESP32-P4/Android 是否能稳定展示 `policy_check`、`policy_decision`、`mesh_command_queued`、`OutputMessage`、final reply 和 sensor telemetry。
- 完善 automation rule 管理：自然语言暂停/恢复/删除、状态查询、规则命名、冲突检测和默认任务可视化。
- 丰富 timeline events：任务拆解、远端等待/超时、结果关联、watchdog 状态和跨节点 trace 关联。
- 新增 Board Descriptor / Board Profile，用结构化数据记录四块 S3 与 P4 的 pins、sensors、actuators、safe ranges、risk、validation status。
- 增加节点 heartbeat / discovery。
- 增加 `storage_info` CLI，打印 SPIFFS/NVS/session 状态。

### P1

- 完善 role/capability/profile 的运行时管理：把角色身份迁移到 NVS，支持统一 OTA 镜像，并让 capability、Lua package 和 manifest 权限共享同一 profile。
- Coordinator Agent 最小实现：只做任务路由和只读查询，不直接绕过安全层。
- 继续用四块 S3 的实时 MQTT 流量验证 ESP32-P4+C6 Display Terminal，确认屏幕能展示四个 S3 角色的 MQTT 状态、推理过程、数据通信过程和最终结果。
- MQTT TLS/认证或内网/VPN 部署规范。
- 完善 session/tool trace 持久化，便于调试和展示。
- 增加 Lua package layout 和 metadata：区分 `/spiffs/scripts/builtin/`、`/spiffs/scripts/user/`、`/spiffs/skills/<skill>/scripts/`，记录 source、version、risk 和 confirmation requirement。

### P2

- MCP Gateway：数据库、知识库、Home Assistant、云服务。
- Memory Agent：长期记忆压缩、用户偏好、skills 管理。
- MCU edge AI：评估 TinyML / LiteRT Micro / ESP-DL / ESP-SR 类小模型路径，只在模型文件、驱动、工具 schema 和资源预算明确后再声明为已实现能力。
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

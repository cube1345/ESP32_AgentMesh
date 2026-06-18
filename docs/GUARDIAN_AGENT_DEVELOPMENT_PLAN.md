# Guardian Agent Development Plan

本文档记录 ESPAgent 在 `development` 分支上推进的新方案。项目名称不变，仍然使用 ESPAgent / LingShu Agent Mesh 的既有命名；本方案只调整第四块 ESP32-S3 的职责。

## 分支策略

- `legacy-display-agent`：旧方案保守分支，保留当前四角色设计：Coordinator / Sensor / Control / Display。
- `development`：新方案开发分支，用于推进 Guardian Agent、权限治理、数据隔离、审计和可观测性。

如果 Guardian 方案实施过程中遇到结构性问题，可以回到 `legacy-display-agent` 继续使用旧方案。

## 背景判断

当前系统已经有 ESP32-P4+C6 和 Android 上位机规划，用于展示 AI 推理过程、MQTT 通信过程、节点状态、传感器数据和最终结果。因此，第四块 ESP32-S3 继续作为主 Display Agent 的收益下降。

更合理的定位是让第四块 ESP32-S3 成为边缘治理节点：

```text
USB0  coordinator_agent   飞书 / LLM / 任务理解 / 调度 / 最终回复
USB1  sensor_agent        传感器采集 / telemetry / 本地阈值
USB2  control_agent       GPIO / WS2812 / 舵机 / 继电器 / 执行器状态机
USB3  guardian_agent      权限裁决 / 数据治理 / 审计 / StateBoard / Watchdog
```

ESP32-P4+C6 和 Android 保持 Display Terminal 定位：

```text
ESP32-P4+C6   现场中控屏 / LVGL 展示 / 触控
Android App   移动上位机 / 复杂 UI / Debug / 人工确认
```

## Stage 项目参考点

`~/WorkSpace/DLML/Stage` 的核心架构是：

```text
Director          统筹调度
Agent             执行职责
StateBoard        全局状态板，唯一事实源
StructuredMessage 结构化消息
TraceContext      跨 Agent 链路追踪
Security          身份、能力 token、审计
Monitor           心跳、超时、异常检测
```

迁移到 ESPAgent 时不直接照搬 Python 多进程/async 运行时，而是迁移思想：

- StateBoard：Guardian 在 MCU 侧维护轻量状态板。
- StructuredMessage：MQTT Mesh command/event 统一结构化。
- TraceContext：一次飞书请求跨 Coordinator、Guardian、Sensor/Control、P4/Android 的链路统一 trace。
- Security：节点身份、能力、风险等级、策略裁决、审计事件。
- Monitor：心跳、命令超时、telemetry 过期、异常告警。

## 目标架构

旧链路：

```text
Coordinator -> Sensor / Control
```

新链路：

```text
Feishu / WebSocket / Android / P4
        |
        v
Coordinator Agent
        |
        v
Guardian Agent policy_check
        |
        +--> deny
        +--> require_confirm -> Android/P4/Feishu
        +--> sanitize
        +--> allow
                |
                v
        Sensor Agent / Control Agent
                |
                v
        Guardian audit + StateBoard + timeline
                |
                v
        Coordinator final reply + P4/Android visualization
```

关键原则：

- Guardian 做策略裁决、审计和数据治理。
- Control 仍做本地强制安全校验，不能无条件相信 Guardian。
- Sensor 原始隐私数据不默认进入 Coordinator/LLM，只输出脱敏摘要或必要数据。
- P4/Android 主要展示 Guardian StateBoard 和 trace，也可以承载人工确认入口。

## Guardian Agent 职责

### 权限裁决

Guardian 接收 `policy_check` 请求，输出：

```text
allow
deny
require_confirm
sanitize
limit
```

示例风险分级：

| 风险 | 示例 | 默认策略 |
|------|------|----------|
| low | 读取温湿度、点亮状态灯 | 可自动 allow |
| medium | 舵机动作、继电器短时开关、蜂鸣器 | 需要参数限制和审计 |
| high | 长时间运行风扇/水泵/门锁/高功率继电器 | 默认 require_confirm |
| restricted | 未知 GPIO、摄像头原始图像、语音原文、用户隐私数据外发 | 默认 deny 或 sanitize |

### 数据治理

Sensor 数据分级：

| 数据 | 原始处理 | 对 Coordinator/LLM 输出 |
|------|----------|-------------------------|
| 温湿度 | 可缓存 | 可直接输出 |
| 光照 | 可缓存 | 可直接输出 |
| 空气质量 | 可缓存 | 可直接输出 |
| 人体存在 | 原始事件内部保存 | 输出“有人/无人/活动异常”摘要 |
| 摄像头 | 原始数据不进 S3 Guardian | 只允许 P4/Android/RPi 本地摘要 |
| 语音 | 原文敏感 | 默认摘要化或本地处理 |

### 审计日志

Guardian 记录：

- 谁发起：`actor`
- 谁请求：`sender`
- 请求动作：`action`
- 目标节点：`target_node` / `target_role`
- 风险等级：`risk_level`
- 裁决结果：`decision`
- 执行结果：`result`
- trace：`trace_id`
- 时间：`ts_ms`

### StateBoard

Guardian 维护轻量状态：

```text
nodes
  node_id -> role, online, last_seen_ms, capabilities

commands
  command_id -> trace_id, action, target, status, decision, deadline_ms

telemetry
  node_id -> latest sanitized telemetry

alerts
  recent alert ring buffer

audit
  recent audit ring buffer

traces
  trace_id -> compact event chain
```

### Watchdog

Guardian 周期检查：

- 节点心跳超时。
- command 超过 `ttl_ms` 未完成。
- telemetry 过期。
- Control 连续执行失败。
- Sensor 连续采样失败。
- Coordinator 长时间无最终回复。

输出：

```text
espagent/cube1345/alerts
espagent/cube1345/security/audit
espagent/cube1345/security/decision
espagent/cube1345/agent/timeline
```

## MQTT 协议规划

保留现有 topic：

```text
espagent/cube1345/nodes/<node_id>/state
espagent/cube1345/nodes/<node_id>/telemetry
espagent/cube1345/nodes/<node_id>/events
espagent/cube1345/nodes/<node_id>/command
espagent/cube1345/roles/<role>/command
espagent/cube1345/agent/dispatch
espagent/cube1345/agent/timeline
espagent/cube1345/alerts
```

新增 Guardian topic：

```text
espagent/cube1345/security/policy_check
espagent/cube1345/security/decision
espagent/cube1345/security/audit
espagent/cube1345/privacy/sanitized
espagent/cube1345/guardian/stateboard
espagent/cube1345/guardian/watchdog
```

建议消息结构：

```json
{
  "schema": "espagent.mesh.v1",
  "msg_id": "msg_...",
  "trace_id": "trace_...",
  "parent_id": "msg_...",
  "sender": "coordinator_agent",
  "recipient": "guardian_agent",
  "type": "policy_check",
  "priority": "high",
  "ttl_ms": 5000,
  "created_ms": 0,
  "payload": {
    "action": "set_status_light",
    "target_role": "control_agent",
    "risk_level": "low",
    "args": {
      "color": "blue"
    }
  }
}
```

裁决消息：

```json
{
  "schema": "espagent.security.v1",
  "msg_id": "msg_...",
  "trace_id": "trace_...",
  "sender": "guardian_agent",
  "recipient": "coordinator_agent",
  "type": "policy_decision",
  "decision": "allow",
  "reason": "low risk status light command",
  "constraints": {
    "max_duration_ms": 30000,
    "requires_local_guard": true
  }
}
```

## 开发阶段

### Phase 0: 文档和分支

- 建立 `legacy-display-agent` 分支，保存旧方案。
- 建立 `development` 分支，推进 Guardian 方案。
- 写入本计划文档。

### Phase 1: 角色 profile 和文档

- 状态：已落地。
- 在 `espagent_secrets.h.example` 增加 `guardian_agent` profile。
- 在 `role_config` 中增加 guardian role 判定。
- 保留 `display_agent` 兼容旧方案，但将 USB3 推荐角色改为 `guardian_agent`。
- 更新 `docs/ESP32_ROLE_PROFILES.md` 和 `docs/PUBLIC_KNOWLEDGE_BASE.md`。

### Phase 2: Guardian 最小运行时

- 状态：部分落地。
- 已新增 `main/roles/guardian_node.c/.h`。
- Guardian 启动后会声明 `policy/privacy/audit/stateboard/watchdog` 边界。
- MQTT runtime 已订阅全局 `agent/timeline`，Guardian 会对关键事件生成 `espagent.guardian.audit.v1` 审计事件；错误事件会同步到 alerts。
- 尚未维护完整内存版 StateBoard。
- 尚未发布专用 guardian stateboard/watchdog topic。
- 不执行硬件动作。

### Phase 2.5: OutputMessage / ReAct 结果闭环

- 状态：已落地第一版。
- 下游 Sensor/Control 的 `mesh_command_result` 已升级为结构化 `schema=espagent.output.v1`。
- OutputMessage 字段包含 `msg_id`、`node_id`、`role`、`sender`、`recipient`、`command_id`、`trace_id`、`action`、`status`、`summary`、`result`、`error`、`ts_ms`。
- Coordinator `mesh_send_command` 在 `require_ack=true` 时等待相同 `command_id` 的 OutputMessage。
- 等待成功后，工具结果返回 `output_message={...}`，进入下一轮 LLM tool_result，从而支持“推理 -> 下发 -> 观察远端结果 -> 再推理”的跨节点 ReAct 闭环。
- 当前等待是同步阻塞，后续应演进为 async task_id + message_bus 回注。

### Phase 3: Policy Check 链路

- 状态：已落地第一版。
- Coordinator 在高风险或远程硬件命令前发布 `policy_check`。
- Guardian 返回 `policy_decision`。
- Coordinator 根据裁决：
  - allow：继续发给 Sensor/Control。
  - deny：回复用户拒绝原因。
  - require_confirm：请求 Feishu/Android/P4 二次确认。
  - sanitize：只把脱敏数据交给 LLM。

当前第一版已对低/中风险白名单动作自动 allow，对高风险或未知动作 deny；`require_confirm` 和 `sanitize` 仍是后续扩展。

### Phase 4: Control 双层安全

- Control 接收命令时检查是否有 Guardian decision。
- 对 medium/high 风险命令，没有 approval 不执行。
- Control 本地仍检查 GPIO 白名单、动作时长、冷却时间和危险设备限制。
- 所有执行结果发布 audit/timeline。

### Phase 5: Sensor 数据治理

- Sensor 继续采集原始 telemetry。
- Guardian 订阅或接收原始事件后生成 sanitized telemetry。
- Coordinator/LLM 优先使用 sanitized topic。
- 人体存在、摄像头、语音类数据默认只输出摘要。

### Phase 6: P4/Android 展示

- P4/Android 订阅 Guardian StateBoard、security decision、audit 和 trace。
- 页面展示：
  - 用户请求
  - Coordinator 拆解
  - Guardian 裁决
  - Sensor/Control 执行
  - Guardian 审计
  - 最终回复

## 验收标准

最小验收：

- USB3 可以烧录为 `guardian_agent` 并发布 state online。
- Guardian 能收到节点 state/telemetry/timeline。
- Coordinator 发起 `读取温湿度` 时，低风险 policy 自动 allow。
- Coordinator 发起 `点亮WS2812为蓝色` 时，低风险 policy 自动 allow。
- 未知 GPIO 或高风险 relay 命令返回 deny 或 require_confirm。
- P4/Android 能看到 Guardian 的 decision 和 audit event。

稳定性验收：

- Guardian 离线时，低风险命令可按降级策略执行，高风险命令默认拒绝。
- Control 即使收到伪造 allow，也必须本地校验参数。
- command 超时会出现在 Guardian watchdog alert。
- trace_id 能串起一次飞书请求的完整链路。

## 风险与降级

风险：

- Guardian 变成单点依赖，导致系统可用性下降。
- MQTT 消息往返增加延迟。
- ESP32-S3 内存不足以保存过多 StateBoard 数据。
- policy/decision 协议复杂后调试难度上升。

降级策略：

- Guardian 离线时，low-risk command 可以按本地策略执行。
- medium/high-risk command 在 Guardian 离线时默认拒绝或 require_confirm。
- StateBoard 使用 ring buffer，限制内存。
- 先做同步 request/decision，后续再做完整 async trace。

## 不做的事

- 不改项目名称。
- 不让 Guardian 直接控制 GPIO/继电器/舵机。
- 不把所有硬件控制都强制同步经过 Guardian 后才执行低风险动作。
- 不把摄像头/语音原始隐私数据发给云端 LLM。
- 不把 ESP32-S3 当作完整本地大模型推理节点。

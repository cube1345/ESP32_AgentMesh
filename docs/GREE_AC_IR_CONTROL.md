# Gree AC IR Control

## 这次做了什么

我在当前 ESPAgent 固件里补了一条专用的格力空调红外控制链路，范围只覆盖 `Gree-only`、`send-only` 的常用控制：

- 开机 / 关机
- `cool` / `heat` / `dry` / `fan` / `auto`
- 设定温度 `temp_c`
- 温度增减 `temp_delta`
- 风速 `auto` / `low` / `medium` / `high`
- 摆风 `on` / `off`

实现入口是新工具 `gree_ac_control`。它会维护一份本地缓存空调状态，每次控制时重新组完整的 Gree IR frame 并发送。

## 涉及文件

新增：

- `main/drivers/gree_ir.h`
- `main/drivers/gree_ir.c`
- `main/tools/tool_gree_ac.h`
- `main/tools/tool_gree_ac.c`

接入修改：

- `main/CMakeLists.txt`
- `main/espagent_config.h`
- `main/espagent_secrets.h.example`
- `main/tools/tool_registry.c`
- `main/tools/tool_mesh_command.c`
- `main/tools/tool_sandbox.c`
- `main/capability/capability_registry.c`
- `main/capability/role_capability_profile.c`
- `main/sensors/sensor_mqtt.c`
- `main/agent/context_builder.c`

## 架构接入方式

### 当前四节点映射

按你现在的串口顺序，四个节点是：

- `/dev/ttyUSB0` -> `coordinator_agent`
- `/dev/ttyUSB1` -> `sensor_agent`
- `/dev/ttyUSB2` -> `control_agent`
- `/dev/ttyUSB3` -> `guardian_agent`

这意味着：

- 用户聊天入口、主调度、Mesh 下发主要在 `coordinator_agent`
- 传感器采集主要在 `sensor_agent`
- 舵机、GPIO、WS2812、格力空调 IR 发射应接在 `control_agent`
- 安全审批与策略决策在 `guardian_agent`

### 1. 本地控制

`gree_ac_control` 在本机上直接调用 `drivers/gree_ir.c`，通过 ESP-IDF `RMT TX + 38kHz carrier` 发红外码。

### 2. Coordinator 路由

如果当前节点是 `coordinator_agent`，默认不会在本板直接发空调红外，而是按现有控制类工具模式，把请求路由到 `control_agent`：

- action: `gree_ac_control`
- target_role: `control_agent`

只有传入 `local=true` 时，coordinator 才在本地执行。

按你当前节点顺序，默认远程执行目标就是 `/dev/ttyUSB2` 那块控制板。

### 3. Guardian / Sandbox / Capability

我把这个工具同步加进了：

- tool registry
- Mesh action 白名单
- sandbox 风险控制
- capability registry
- control role 可见工具集
- Guardian 控制动作判断

这样 coordinator、control、guardian 三条链路是一致的，不会出现“能看见工具但不能下发/不能执行”的断层。

## 配置项

在 `espagent_secrets.h` 里新增：

```c
#define ESPAGENT_SECRET_GREE_IR_TX_GPIO (-1)
```

默认是 `-1`，表示未配置。要启用必须改成实际 IR 发射 GPIO。

如果你采用四节点部署，这个配置应该优先写在 `control_agent`，也就是 `/dev/ttyUSB2` 对应那块板子的配置里。

同时新增了这些固件常量：

```c
#define ESPAGENT_GREE_IR_TX_GPIO             ESPAGENT_SECRET_GREE_IR_TX_GPIO
#define ESPAGENT_GREE_IR_CARRIER_HZ          38000U
#define ESPAGENT_GREE_IR_CARRIER_DUTY_CYCLE  0.33f
#define ESPAGENT_GREE_IR_RMT_RESOLUTION_HZ   1000000U
#define ESPAGENT_GREE_IR_REPEAT_COUNT        0
```

## 接线方式

不要把红外发射二极管直接硬顶在 ESP32 GPIO 上。建议这样接：

- `ESP32 GPIO` -> `1k~2.2k` 电阻 -> `NPN 三极管基极`
- 三极管发射极 -> `GND`
- 三极管集电极 -> `IR LED 阴极`
- `IR LED 阳极` -> `3.3V 或 5V` 串限流电阻
- `ESP32 GND` 与发射电路 `GND` 共地

如果你用的是现成的 3-pin 红外发射模块，一般接：

- `VCC`
- `GND`
- `SIG -> ESPAGENT_SECRET_GREE_IR_TX_GPIO`

## 工具调用示例

串口 CLI / 工具调用可以这样用：

```json
{"power":"on"}
```

```json
{"mode":"cool","temp_c":26}
```

```json
{"mode":"heat","temp_c":28,"fan":"high"}
```

```json
{"temp_delta":1}
```

```json
{"temp_delta":-1}
```

```json
{"swing":"on"}
```

```json
{"power":"off"}
```

如果你用串口工具执行，形式类似：

```sh
tool_exec gree_ac_control '{"mode":"cool","temp_c":26}'
```

## Agent 触发词

我在 prompt 里补了这类映射，模型会优先选 `gree_ac_control`：

- 格力空调开机 / 关机
- 制冷 / 制热 / 除湿 / 送风 / 自动
- 26度 / 调高一度 / 调低一度
- 风速自动 / 低 / 中 / 高
- 打开摆风 / 关闭摆风

## 当前行为特性

### 1. 有状态，不是“学习型遥控”

这是 `stateful resend` 方案，不是录码回放：

- 固件本地保存一份缓存状态
- 温度加一度 / 减一度 是改缓存，再发完整 frame

### 2. 首次默认状态

首次没有历史状态时，默认缓存为：

- `power=off`
- `mode=cool`
- `temp=26C`
- `fan=auto`
- `swing=off`

所以用户如果只说“开空调”，首帧会按这组默认状态开机。

### 3. 模式归一化

为了贴合 Gree 协议做了两条归一化：

- `auto` 模式固定按 `25C`
- `dry` 模式风速归一到 `low`

## 已知限制

- 只支持 `Gree`
- 只支持 `发送`，不支持 `学习`
- 不支持自动识别当前空调真实状态
- 如果用户手动用原厂遥控器改过状态，固件缓存可能漂移
- 目前不支持 `turbo` / `sleep` / `timer` / `iFeel` / `WiFi` 等扩展功能
- 目前不是通用家电红外框架，只是专用 `Gree AC` 控制路径

## 建议的下一步

如果你后面要继续扩展，建议按这个顺序：

1. 加 `Midea/Haier/Hisense` 等独立协议工具，而不是混成一个大而泛的 IR 工具
2. 给 `gree_ac_control` 增加 `status` / `reset_cached_state`
3. 把空调状态同步显示到 ESP32-P4 或 Android 终端
4. 如果需要评审演示，再补一页“空调控制流程图 + Mesh 路由图”

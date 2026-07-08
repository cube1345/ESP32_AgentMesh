# ESPAgent Hardware Plan

本文档描述当前 ESPAgent 四节点方案下的硬件分工、默认接线、供电建议和后续扩展边界。

文档目标：

- 以当前代码和实际四节点部署为准
- 区分“已经在固件里支持”和“只是预留规划”
- 避免把单板满配方案和多节点分工混在一起

## 1. 当前四节点映射

当前串口与角色顺序固定为：

| 串口 | 节点 ID | 角色 | 主要职责 |
|---|---|---|---|
| `/dev/ttyUSB0` | `esp32s3-coordinator-01` | `coordinator_agent` | 飞书/WebSocket 入口、LLM 调度、Mesh 下发、timeline |
| `/dev/ttyUSB1` | `esp32s3-sensor-01` | `sensor_agent` | 温湿度、空气质量、光照、存在感知、telemetry |
| `/dev/ttyUSB2` | `esp32s3-control-01` | `control_agent` | WS2812、GPIO、舵机、音频、红外等执行器 |
| `/dev/ttyUSB3` | `esp32s3-guardian-01` | `guardian_agent` | policy_check、审批、审计、watchdog |

结论：

- 传感器优先挂在 `sensor_agent`
- 执行器优先挂在 `control_agent`
- `coordinator_agent` 不建议承担复杂硬件直控
- `guardian_agent` 不建议承担普通传感器采样和执行器控制

## 2. 当前固件已支持的硬件能力

以下内容以当前代码配置为准，默认值来自 `main/espagent_secrets.h.example` 和 `main/espagent_config.h`。

| 模块 | 默认引脚 | 建议角色 | 固件状态 | 说明 |
|---|---|---|---|---|
| WS2812 / 板载 RGB | `GPIO48` | `control_agent` | 已支持 | 工具：`set_status_light` / `ws2812_set` |
| 舵机 1 | `GPIO5` | `control_agent` | 已支持 | 工具：`servo_write`，当前只有 1 路 |
| AHT10 / AHT20 | `SDA=GPIO21`, `SCL=GPIO18` | `sensor_agent` | 已支持 | 工具：`read_temperature_humidity` / `read_environment` |
| SGP30 | `SDA=GPIO14`, `SCL=GPIO15` | `sensor_agent` | 已支持 | 工具：`sgp30_read_air_quality` / `read_air_quality` |
| BH1750 / GY-30 | `SDA=GPIO11`, `SCL=GPIO12` | `sensor_agent` | 已支持 | 工具：`read_light_level` / `read_environment` |
| 3-wire PIR / 存在传感器 | `GPIO13` | `sensor_agent` | 已支持 | 工具：`read_presence` |
| HC-SR05 | 默认未固定 | `sensor_agent` | 已支持但未默认配置 | 需单独设置 Trig / Echo |
| MAX98357 I2S 功放 | `BCLK=GPIO1`, `WS=GPIO2`, `DIN=GPIO3`, `SD` 可选 | `control_agent` | 已支持 | 工具：`max98357_play_tone` |
| INA I2S 麦克风 | `SCK=GPIO6`, `WS=GPIO7`, `LR=GND`, `SD=GPIO8` | `control_agent` | 硬件方案已确定，固件已接入 I2S 采样与 MQTT 音频上送，云端 STT 仍需网关配置 | I2S 麦克风输入 |
| 格力空调 IR 发射 | `ESPAGENT_SECRET_GREE_IR_TX_GPIO`，默认 `-1` | `control_agent` | 已支持 | 工具：`gree_ac_control`，Gree-only，send-only |
| 通用 GPIO | `1-18, 21, 38, 46` | `control_agent` | 已支持 | 工具：`gpio_write` / `gpio_read` |

## 3. 当前推荐接线分布

### 3.1 sensor_agent `/dev/ttyUSB1`

建议把采集类模块集中到 `sensor_agent`：

| 模块 | 接线 |
|---|---|
| AHT20 | `SDA -> GPIO21`, `SCL -> GPIO18`, `VCC -> 3V3`, `GND -> GND` |
| SGP30 | `SDA -> GPIO14`, `SCL -> GPIO15`, `VCC -> 3V3`, `GND -> GND` |
| BH1750 | `SDA -> GPIO11`, `SCL -> GPIO12`, `VCC -> 3V3`, `GND -> GND` |
| PIR / 3-wire 存在 | `OUT -> GPIO13`, `VCC/GND` 按模块规格接 |
| HC-SR05 | `Trig/Echo` 后续按实际配置接入，`Echo` 必须分压或电平转换 |

说明：

- 当前代码已经把 AHT20、SGP30、BH1750 分成三组默认引脚，不再使用旧文档里 `SGP30 GPIO17/18` 的冲突方案。
- 如果你后面继续接 `ICM42688`，建议单独规划，优先考虑 `SPI`，不要直接复用当前默认 I2C 设计。

### 3.2 control_agent `/dev/ttyUSB2`

建议把执行类模块集中到 `control_agent`：

| 模块 | 接线 |
|---|---|
| WS2812 | `DIN -> GPIO48`, `VCC/GND` 按灯带规格供电 |
| 舵机 1 | `Signal -> GPIO5`, `VCC -> 外部 5V`, `GND -> 共地` |
| MAX98357 | `BCLK -> GPIO1`, `WS/LRCLK -> GPIO2`, `DIN -> GPIO3`, `SD` 可选 |
| INA 麦克风 | `SCK -> GPIO6`, `WS -> GPIO7`, `LR -> GND`, `SD -> GPIO8`, `VDD -> 3V3`, `GND -> GND` |
| 格力空调 IR | `SIG -> ESPAGENT_SECRET_GREE_IR_TX_GPIO`，建议经三极管驱动 |
| 通用继电器 / MOSFET / 风扇 | 走 `gpio_write` 的允许引脚，按具体模块分配 |

说明：

- 当前固件只有 **1 路舵机**，第二路舵机还没有实现。
- `gree_ac_control` 默认由 `coordinator_agent` 路由到 `control_agent` 执行，因此空调红外发射模块应接在 `/dev/ttyUSB2`。

### 3.3 coordinator_agent `/dev/ttyUSB0`

建议保持轻载：

- 只保留 Wi-Fi、Feishu、WebSocket、LLM、Mesh 调度
- 不建议接大电流执行器
- 最多保留串口调试和必要状态指示

### 3.4 guardian_agent `/dev/ttyUSB3`

建议只承担：

- policy_check / approval / audit
- watchdog / timeline 观察

不建议接复杂传感器和执行器。

## 4. 关键接线方式

### 4.1 电源与共地

所有外设必须与 ESP32-S3 共地：

```text
外设 GND -> ESP32 GND
```

注意：

- 舵机、风扇、水泵、继电器、音频功放、红外发射增强电路不要直接吃 ESP32 板载 3V3
- 推荐外部独立供电，并与 ESP32 共地

### 4.2 AHT20

```text
AHT20 SDA -> GPIO21
AHT20 SCL -> GPIO18
AHT20 VCC -> 3V3
AHT20 GND -> GND
```

地址通常为 `0x38`。

### 4.3 SGP30

```text
SGP30 SDA -> GPIO14
SGP30 SCL -> GPIO15
SGP30 VCC -> 3V3
SGP30 GND -> GND
```

地址通常为 `0x58`。

### 4.4 BH1750 / GY-30

```text
BH1750 SDA -> GPIO11
BH1750 SCL -> GPIO12
BH1750 VCC -> 3V3
BH1750 GND -> GND
```

地址通常为 `0x23`，部分模块为 `0x5C`。

### 4.5 舵机

```text
Servo Signal -> GPIO5
Servo VCC    -> 外部 5V
Servo GND    -> ESP32 GND 共地
```

说明：

- 当前 `tool_servo.c` 只支持 1 路舵机
- 第二路舵机属于后续扩展，不要在文档里当成已实现能力

### 4.6 MAX98357 I2S 音频功放

```text
MAX98357 BCLK      -> GPIO1
MAX98357 WS/LRCLK  -> GPIO2
MAX98357 DIN       -> GPIO3
MAX98357 SD        -> 可选 GPIO，或上拉使能
MAX98357 VIN       -> 5V 优先
MAX98357 GND       -> GND
```

说明：

- 当前是播放测试音输出链路
- 不是麦克风输入链路
- 麦克风输入已单独规划为 `GPIO6/7/8`，不要与 MAX98357 的 `GPIO1/2/3` 并线

### 4.7 INA I2S 麦克风

```text
INA SCK/BCLK -> GPIO6
INA WS       -> GPIO7
INA LR       -> GND
INA SD/DOUT  -> GPIO8
INA VDD      -> 3V3
INA GND      -> GND
```

说明：

- 当前不仅接线方案已确定，固件也已补入 I2S 采样、音量检测和 MQTT 音频分片上送；但真正的云端 STT 识别仍依赖网关侧配置上游 provider
- `LR` 接地表示固定使用单侧声道
- 这一路是 I2S 数字麦克风输入，和 MAX98357 的 I2S 功放输出是两套独立链路
- 按当前项目 GPIO 策略，ESP32-S3 侧不会因为 `GPIO6/7/8` 直接被策略拦截；当前明确保留的是 `GPIO19/20` USB Serial/JTAG

### 4.8 WS2812

```text
WS2812 DIN -> GPIO48
WS2812 VCC -> 5V 或按模块规格
WS2812 GND -> GND
```

说明：

- 多颗 WS2812 串联时，供电不要只靠 ESP32 板载 5V/3V3
- 建议单独 5V 供电并共地

### 4.9 PIR / 3-wire 存在传感器

```text
PIR VCC -> 3V3 或 5V，按模块规格
PIR GND -> GND
PIR OUT -> GPIO13
```

### 4.10 HC-SR05

```text
HC-SR05 VCC  -> 5V
HC-SR05 GND  -> GND
HC-SR05 Trig -> 运行配置指定 GPIO
HC-SR05 Echo -> 运行配置指定 GPIO，经分压/电平转换后进入 ESP32
```

Echo 保护示例：

```text
HC-SR05 Echo -- 2kΩ --+-- ESP32 GPIO
                      |
                     3.3kΩ
                      |
                     GND
```

### 4.11 格力空调 IR 发射

建议不要让 GPIO 直接硬推红外 LED，推荐三极管驱动：

```text
ESP32 GPIO -> 1k~2.2k -> NPN 基极
NPN 发射极 -> GND
NPN 集电极 -> IR LED 阴极
IR LED 阳极 -> 3.3V/5V 串限流电阻
```

如果使用现成 3-pin 红外发射模块：

```text
VCC -> 3V3 / 5V
GND -> GND
SIG -> ESPAGENT_SECRET_GREE_IR_TX_GPIO
```

说明：

- 当前仅支持 `Gree`
- 当前仅支持 `send-only`
- 不支持 IR 学习和通用家电码库

## 5. 当前与旧方案的差异

以下内容是旧文档里容易误导的点，现统一修正：

| 旧说法 | 当前应以什么为准 |
|---|---|
| `SGP30 SDA=GPIO17, SCL=GPIO18` | 当前默认是 `SDA=GPIO14, SCL=GPIO15` |
| `DHT22 + MH-Z19B` 是当前主传感器链路 | 当前多节点主传感器链路应以 `AHT20 + SGP30 + BH1750` 为准 |
| 双舵机已纳入当前方案 | 当前代码只支持 1 路舵机 |
| 所有模块都尽量堆在一块 ESP32-S3 上 | 当前方案是四节点分工，不是单板满配 |
| RMT/IR 仍只是规划能力 | 当前已接入专用 `gree_ac_control` 红外发送路径 |
| `GPIO6/7/8` 不能用于当前 S3 麦克风规划 | 按当前项目 GPIO 策略，ESP32-S3 不会把 `GPIO6/7/8` 作为禁止引脚；当前麦克风规划就是 `SCK=6, WS=7, SD=8` |

## 6. 当前建议修改的配置项

如果按当前四节点方案落地，建议优先在对应角色板子的 `espagent_secrets.h` 中配置：

### sensor_agent

```c
#define ESPAGENT_SECRET_AHT10_SDA_GPIO  21
#define ESPAGENT_SECRET_AHT10_SCL_GPIO  18
#define ESPAGENT_SECRET_AHT10_I2C_PORT  0
#define ESPAGENT_SECRET_AHT10_SCL_HZ    100000
#define ESPAGENT_SECRET_AHT10_ADDR      0x38

#define ESPAGENT_SECRET_SGP30_SDA_GPIO  14
#define ESPAGENT_SECRET_SGP30_SCL_GPIO  15
#define ESPAGENT_SECRET_SGP30_I2C_PORT  1
#define ESPAGENT_SECRET_SGP30_SCL_HZ    100000

#define ESPAGENT_SECRET_BH1750_SDA_GPIO 11
#define ESPAGENT_SECRET_BH1750_SCL_GPIO 12
#define ESPAGENT_SECRET_BH1750_ADDR     0x23

#define ESPAGENT_SECRET_PRESENCE_GPIO   13
```

### control_agent

```c
#define ESPAGENT_SECRET_WS2812_GPIO     48
#define ESPAGENT_SECRET_SERVO_GPIO      5

#define ESPAGENT_SECRET_MAX98357_BCLK_GPIO 1
#define ESPAGENT_SECRET_MAX98357_WS_GPIO   2
#define ESPAGENT_SECRET_MAX98357_DIN_GPIO  3
#define ESPAGENT_SECRET_MAX98357_SD_GPIO   (-1)
#define ESPAGENT_SECRET_INA_MIC_BCLK_GPIO  6
#define ESPAGENT_SECRET_INA_MIC_WS_GPIO    7
#define ESPAGENT_SECRET_INA_MIC_SD_GPIO    8
#define ESPAGENT_SECRET_INA_MIC_I2S_PORT   1
#define ESPAGENT_SECRET_INA_MIC_SAMPLE_RATE_HZ 16000
#define ESPAGENT_SECRET_INA_MIC_RIGHT_CHANNEL 0

#define ESPAGENT_SECRET_GREE_IR_TX_GPIO <你的实际红外发射GPIO>
```

## 7. 后续扩展建议

建议继续扩展的方向：

| 项目 | 当前状态 | 建议 |
|---|---|---|
| 第二路舵机 | 未实现 | 增加第二路 servo tool 或支持 `index` |
| ICM42688 | 未接入 | 优先 SPI 独立驱动 |
| 麦克风输入 | 已接入固件采样链，待联通网关 STT | 当前规划：`SCK=6, WS=7, LR=GND, SD=8`，板端已支持 I2S 采样和 MQTT 音频上送 |
| HC-05 蓝牙网关 | 未完整接入 | 作为 UART/蓝牙网关单独设计 |
| 通用家电 IR | 未接入 | 不要和 Gree AC 专用控制混成一个大工具 |
| ESP32-P4 / Android 终端联动 | 有数据通路 | 下一步重点做 UI 绑定和调度展示 |

## 8. 一页式总览

```text
/dev/ttyUSB0  coordinator_agent
  - Feishu / WebSocket / LLM / Mesh dispatch

/dev/ttyUSB1  sensor_agent
  - AHT20   SDA21 SCL18
  - SGP30   SDA14 SCL15
  - BH1750  SDA11 SCL12
  - PIR     GPIO13
  - HC-SR05 optional

/dev/ttyUSB2  control_agent
  - WS2812      GPIO48
  - Servo 1     GPIO5
  - MAX98357    GPIO1/2/3
  - INA Mic     GPIO6/7/8 (LR->GND)
  - Gree IR TX  custom GPIO

/dev/ttyUSB3  guardian_agent
  - policy / approval / audit / watchdog
```

# ESPAgent Hardware GPIO Plan

本文档描述 ESPAgent 当前硬件能力、目标 GPIO 分配、接线方式和固件改动清单。

目标原则：

- 尽量使用当前项目允许的 ESP32-S3 可用 GPIO。
- 只保留 2 个 GPIO 作为唯一 UART 串口对。
- 舵机规划 2 路。
- GPIO19/GPIO20 保留给 ESP32-S3 USB Serial/JTAG，不做外设。
- GPIO0 是 boot 按键相关脚，不纳入外设规划。

## 1. 当前固件已支持的外设

| 外设 | 当前 GPIO | 固件状态 | 说明 |
|---|---:|---|---|
| WS2812 / 板载 RGB | GPIO48 | 已支持 | `set_status_light` / `ws2812_set` |
| 舵机 1 | GPIO5 | 已支持 | 当前只有 1 路 `servo_write` |
| AHT10 | I2C SDA=GPIO21, SCL=GPIO18 | 已支持 | AI 工具读取温湿度 |
| DHT22 | GPIO4 | 已支持 | 后台 MQTT 上报温湿度 |
| MH-Z19B | UART1 RX=GPIO16, TX=GPIO17 | 已支持 | 后台 MQTT 上报 CO2 |
| 3-wire PIR / 人体存在 | GPIO13 | 已支持 | OUT 数字输入 |
| HC-SR05 | 未固定 | 已支持但未配置 | 需要 Trig/Echo 两个 GPIO |
| MAX98357 I2S 音频放大器 | GPIO1/BCLK, GPIO2/WS, GPIO3/DIN | 已支持 | SD 可选 |
| SGP30 | 当前 SDA=GPIO17, SCL=GPIO18 | 已支持但有冲突 | GPIO17 与 MH-Z19B UART 冲突 |
| 通用 GPIO | 1-18, 21, 38, 46 | 已支持 | GPIO46 建议只做输入 |

## 2. 目标 GPIO 分配总表

该表按“尽量占满 GPIO，只保留 2 个串口 GPIO”的目标规划。这里的“串口”专指 UART1 的 RX/TX。

| GPIO | 目标用途 | 方向 | 接线 / 说明 | 固件状态 |
|---:|---|---|---|---|
| GPIO1 | MAX98357 BCLK | OUT | MAX98357 BCLK | 已支持 |
| GPIO2 | MAX98357 WS/LRCLK | OUT | MAX98357 WS/LRCLK | 已支持 |
| GPIO3 | MAX98357 DIN | OUT | MAX98357 DIN | 已支持 |
| GPIO4 | DHT22 DATA | IN/OD | DHT22 data，上拉到 3V3 | 已支持 |
| GPIO5 | 舵机 1 PWM | OUT | Servo 1 signal | 已支持 |
| GPIO6 | 舵机 2 PWM | OUT | Servo 2 signal | 需扩展第二路舵机 |
| GPIO7 | 蜂鸣器 | OUT/PWM | 有源蜂鸣器可直接 GPIO；无源蜂鸣器用 PWM | 需新增工具 |
| GPIO8 | 本地按钮 1 | IN | 按钮到 GND，内部上拉 | 需新增按键任务 |
| GPIO9 | 本地按钮 2 | IN | 按钮到 GND，内部上拉 | 需新增按键任务 |
| GPIO10 | 风扇 / MOSFET 1 | OUT | MOSFET gate，驱动风扇或负载 | 可用通用 GPIO，建议新增专用工具 |
| GPIO11 | 水泵 / MOSFET 2 | OUT | MOSFET gate，驱动水泵或负载 | 可用通用 GPIO，建议新增专用工具 |
| GPIO12 | HC-SR05 Trig | OUT | 超声波 Trig | 需写入默认配置 |
| GPIO13 | PIR OUT | IN | 人体/PIR OUT | 已支持 |
| GPIO14 | HC-SR05 Echo | IN | 超声波 Echo，必须分压到 3.3V | 需写入默认配置 |
| GPIO15 | 预留数字输入 / 门磁 / 火焰传感器 | IN | 数字 OUT 传感器输入 | 可用通用 GPIO |
| GPIO16 | UART1 RX | IN | 接 MH-Z19B TX；这是保留的 2 个串口 GPIO 之一 | 已支持 |
| GPIO17 | UART1 TX | OUT | 接 MH-Z19B RX；这是保留的 2 个串口 GPIO 之一 | 已支持 |
| GPIO18 | I2C SCL | OUT/OD | SGP30/OLED/BH1750/AHT10 共用 SCL | 已支持多个 I2C 传感器，建议共用 I2C |
| GPIO21 | I2C SDA | IN/OUT/OD | SGP30/OLED/BH1750/AHT10 共用 SDA | 需把 SGP30 SDA 从 GPIO17 移到 GPIO21 |
| GPIO38 | 状态输入 / 编码器 A | IN | 旋钮编码器 A、限位开关或额外按钮 | 可用通用 GPIO |
| GPIO46 | 状态输入 / 编码器 B | IN | 旋钮编码器 B、限位开关或额外按钮；不建议输出 | 可用通用 GPIO 输入 |
| GPIO48 | WS2812 RGB | OUT | 板载 RGB LED DIN | 已支持 |

不分配：

| GPIO | 原因 |
|---:|---|
| GPIO0 | 启动/下载模式相关，不建议接普通外设 |
| GPIO19/GPIO20 | ESP32-S3 USB Serial/JTAG，保留给烧录、monitor、console |
| 其他 GPIO | 当前项目未列入安全可用表，除非确认开发板原理图再使用 |

## 3. 关键接线方式

### 3.1 电源与共地

所有外设必须与 ESP32-S3 共地：

```text
外设 GND -> ESP32 GND
```

舵机、继电器、水泵、风扇、蜂鸣器等感性或大电流负载不要直接吃 ESP32 的 3V3。推荐单独 5V 电源，并共地。

### 3.2 两路舵机

```text
Servo 1 Signal -> GPIO5
Servo 2 Signal -> GPIO6
Servo VCC      -> 外部 5V
Servo GND      -> ESP32 GND 共地
```

固件注意：

- 当前 `tool_servo.c` 只支持 1 路舵机 GPIO5。
- 目标需要扩展为 `servo_write` 支持 `index` 或新增 `servo1_write` / `servo2_write`。
- 建议使用 LEDC 两个 channel，同一个 50Hz timer。

### 3.3 AHT10 与 DHT22

```text
AHT10 SDA  -> GPIO21
AHT10 SCL  -> GPIO18
AHT10 VCC  -> 3V3
AHT10 GND  -> GND

DHT22 DATA -> GPIO4
VCC        -> 3V3
GND        -> GND
DHT22 DATA -> 4.7k~10k 上拉到 3V3
```

说明：

- AHT10 当前用于 AI 工具读取温湿度，I2C 地址通常为 `0x38`。
- DHT22 当前用于后台 MQTT 上报到树莓派。

### 3.4 MH-Z19B CO2 传感器

```text
MH-Z19B VCC -> 5V
MH-Z19B GND -> GND
MH-Z19B TX  -> GPIO16  // ESP32 UART1 RX
MH-Z19B RX  -> GPIO17  // ESP32 UART1 TX
```

说明：

- GPIO16/GPIO17 是本规划中仅保留的 UART 串口对。
- 不再给其他 UART 传感器预留 GPIO。

### 3.5 I2C 传感器总线

```text
I2C SDA -> GPIO21
I2C SCL -> GPIO18
VCC     -> 3V3
GND     -> GND
```

可挂设备：

| 设备 | 地址 | 用途 |
|---|---:|---|
| SGP30 | 0x58 | eCO2 / TVOC |
| SSD1306 OLED | 0x3C 或 0x3D | 本地状态显示 |
| BH1750 | 0x23 或 0x5C | 光照 lux |

固件注意：

- 当前真实配置里 SGP30 SDA 是 GPIO17，与 MH-Z19B UART TX 冲突。
- 目标规划要求把 `ESPAGENT_SECRET_SGP30_SDA_GPIO` 改为 GPIO21。

### 3.6 HC-SR05 超声波

```text
HC-SR05 VCC  -> 5V
HC-SR05 GND  -> GND
HC-SR05 Trig -> GPIO12
HC-SR05 Echo -> GPIO14，经分压/电平转换后进入 ESP32
```

Echo 保护：

```text
HC-SR05 Echo -- 2kΩ --+-- GPIO14
                      |
                     3.3kΩ
                      |
                     GND
```

说明：

- HC-SR05 Echo 常见为 5V，不能直接进 ESP32。
- 目标配置建议：

```c
#define ESPAGENT_SECRET_HC_SR05_TRIG_GPIO 12
#define ESPAGENT_SECRET_HC_SR05_ECHO_GPIO 14
```

### 3.7 PIR / 3-wire 人体存在传感器

```text
PIR VCC -> 3V3 或 5V，按模块规格
PIR GND -> GND
PIR OUT -> GPIO13
```

当前固件会定时读取 GPIO13。

### 3.8 继电器 / MOSFET 输出

推荐分配：

```text
GPIO10 -> Relay IN1 / MOSFET gate 1
GPIO11 -> Relay IN2 / MOSFET gate 2
```

注意：

- 继电器模块注意高/低电平触发类型。
- 风扇、水泵、电磁铁等感性负载需要续流二极管或使用成品驱动模块。
- 不要让 ESP32 GPIO 直接给负载供电，GPIO 只输出控制信号。

### 3.9 MAX98357 I2S 音频放大器

```text
MAX98357 BCLK   -> GPIO1
MAX98357 WS/LRCLK -> GPIO2
MAX98357 DIN    -> GPIO3
MAX98357 SD     -> 可选 GPIO，或直接上拉到 3V3 使能
MAX98357 GAIN   -> 可悬空
MAX98357 GND    -> ESP32-S3 GND
MAX98357 VIN    -> 5V 优先，3V3 也可低功率测试
```

说明：

- 这是 I2S 音频放大器，不是普通 GPIO 蜂鸣器。
- 固件里按需申请 I2S TX 通道，播放测试音后释放。
- 这组引脚已固定在固件默认配置里，串口命令可直接执行 `max98357_test`。
- 如果日志显示 `ESP_OK` 但听不到声音，优先把 `SD` 接到 `3V3`，确认模块没有处于 shutdown。
- 更明显的排障音：`max98357_test 1000 3000 80 1 2 3`。

### 3.10 蜂鸣器

```text
Buzzer + / IN -> GPIO7
Buzzer -      -> GND
```

建议：

- 有源蜂鸣器：GPIO 高低电平控制即可。
- 无源蜂鸣器：用 PWM 输出固定频率。

### 3.11 按钮 / 编码器 / 状态输入

```text
Button 1 -> GPIO8  -> GND
Button 2 -> GPIO9  -> GND
Input 1  -> GPIO15 -> GND/传感器 OUT
Input 2  -> GPIO38 -> GND/传感器 OUT
Input 3  -> GPIO46 -> GND/传感器 OUT
```

建议固件启用内部上拉，按下或触发时读取 LOW。

## 4. MQTT 数据链路

当前后台链路：

```text
ESP32 DHT22 + MH-Z19B -> MQTT -> RPi 3
```

当前 broker：

```text
192.168.101.74:1883
```

Topic：

```text
ESP32 -> RPi: sensor/data
RPi -> ESP32: sensor/analysis
RPi -> ESP32: sensor/alert
```

Payload：

```json
{"temp":25.6,"humidity":68.3,"co2":450}
```

## 5. 需要同步修改的固件配置

目标硬件规划落地时，建议调整 `main/espagent_secrets.h`：

```c
/* Two-servo target */
#define ESPAGENT_SECRET_SERVO_GPIO      5
/* TODO: add ESPAGENT_SECRET_SERVO2_GPIO 6 after firmware supports servo 2 */

/* HC-SR05 */
#define ESPAGENT_SECRET_HC_SR05_TRIG_GPIO 12
#define ESPAGENT_SECRET_HC_SR05_ECHO_GPIO 14

/* Move I2C away from UART GPIO17 */
#define ESPAGENT_SECRET_SGP30_SDA_GPIO  21
#define ESPAGENT_SECRET_SGP30_SCL_GPIO  18
#define ESPAGENT_SECRET_SGP30_I2C_PORT  0
#define ESPAGENT_SECRET_SGP30_SCL_HZ    100000

/* AHT10 temperature/humidity */
#define ESPAGENT_SECRET_AHT10_SDA_GPIO  21
#define ESPAGENT_SECRET_AHT10_SCL_GPIO  18
#define ESPAGENT_SECRET_AHT10_I2C_PORT  0
#define ESPAGENT_SECRET_AHT10_SCL_HZ    100000
#define ESPAGENT_SECRET_AHT10_ADDR      0x38

/* MQTT sensor pipe */
#define ESPAGENT_SECRET_SENSOR_DHT22_GPIO 4
#define ESPAGENT_SECRET_SENSOR_MHZ19_UART_NUM 1
#define ESPAGENT_SECRET_SENSOR_MHZ19_RX_GPIO 16
#define ESPAGENT_SECRET_SENSOR_MHZ19_TX_GPIO 17

/* MAX98357 I2S amplifier */
#define ESPAGENT_SECRET_MAX98357_BCLK_GPIO 1
#define ESPAGENT_SECRET_MAX98357_WS_GPIO   2
#define ESPAGENT_SECRET_MAX98357_DIN_GPIO  3
#define ESPAGENT_SECRET_MAX98357_SD_GPIO   (-1)
#define ESPAGENT_SECRET_MAX98357_I2S_PORT  0
#define ESPAGENT_SECRET_MAX98357_SAMPLE_RATE_HZ 16000
#define ESPAGENT_SECRET_MAX98357_DEFAULT_TONE_HZ 440
#define ESPAGENT_SECRET_MAX98357_DEFAULT_DURATION_MS 2000
#define ESPAGENT_SECRET_MAX98357_DEFAULT_VOLUME_PCT 70
```

## 6. 固件待办清单

| 项目 | 原因 |
|---|---|
| 增加第二路舵机 | 当前只有 GPIO5 一路 servo |
| 把 SGP30 SDA 从 GPIO17 移到 GPIO21 | 避免与 MH-Z19B UART 冲突 |
| 给 Relay/MOSFET 建专用工具 | 比通用 `gpio_write` 更适合自然语言控制 |
| 给蜂鸣器建 `beep` / `set_buzzer` 工具 | 用于 CO2/温度告警 |
| 增加 OLED 显示任务 | 显示 WiFi、MQTT、温湿度、CO2、告警 |
| 增加按钮任务 | 本地静音、手动上报、重连 MQTT |
| 将 GPIO 占用表写入 prompt | 防止 Agent 误调用已占用 GPIO |

## 7. 最终占用概览

```text
GPIO1   MAX98357 BCLK
GPIO2   MAX98357 WS/LRCLK
GPIO3   MAX98357 DIN
GPIO4   DHT22
GPIO5   Servo 1
GPIO6   Servo 2
GPIO7   Buzzer
GPIO8   Button 1
GPIO9   Button 2
GPIO10  MOSFET 1 / Fan
GPIO11  MOSFET 2 / Pump
GPIO12  HC-SR05 Trig
GPIO13  PIR OUT
GPIO14  HC-SR05 Echo, level shifted
GPIO15  Digital input
GPIO16  UART1 RX, MH-Z19B TX
GPIO17  UART1 TX, MH-Z19B RX
GPIO18  I2C SCL
GPIO21  I2C SDA
GPIO38  Digital input / encoder A
GPIO46  Digital input / encoder B
GPIO48  WS2812 RGB
GPIO19  Reserved USB Serial/JTAG
GPIO20  Reserved USB Serial/JTAG
```

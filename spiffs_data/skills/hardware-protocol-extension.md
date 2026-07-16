# Hardware Protocol Extension

Help ESPAgent reason about new hardware that can be described through common
embedded communication protocols instead of a dedicated C driver.

## When to use

Use this when the user or developer asks whether ESPAgent can support a new
sensor, actuator, module, or board without writing a new firmware tool first.
Also use it when a request mentions I2C/IIC, SPI, UART, RS485, Modbus, CAN,
TWAI, GPIO, PWM, ADC, 1-Wire, I2S, PDM, RMT, infrared, USB CDC, SDIO, SDMMC,
BLE GATT, register maps, bus addresses, command frames, or hardware manifests.

## Core idea

The LLM must not pretend that unsupported hardware is already implemented. It
may only propose a runtime extension when the firmware has a safe generic
primitive for the required protocol.

Correct reasoning chain:

```text
hardware request
  -> identify protocol and risk
  -> check whether a generic primitive exists
  -> ask for or read a hardware manifest
  -> validate pins, address, timing, and bounds
  -> route read-only work to sensor_agent
  -> route control work to control_agent through Guardian
  -> return OutputMessage with resolved protocol details
```

Current executable primitive:

- `virtual_device_read` can execute read-only I2C manifests.
  I2C decoders are explicit firmware whitelist entries: `raw_u8`,
  `raw_u16_be`, `raw_u16_le`, and `aht20_temp_humidity`.
- `virtual_device_read` can execute bounded UART query manifests.
- `virtual_device_read` can execute read-only Modbus RTU function 3/4 register
  manifests over UART/RS485.
- `virtual_device_read` can execute bounded SPI transfer-read manifests.
- `virtual_device_read` can execute ADC one-shot read manifests.
- `virtual_device_read` can execute GPIO input read manifests.
- `virtual_device_control` can execute bounded GPIO output, relay_control,
  pwm_output, and ledc_pwm manifests through Control Agent, Guardian policy,
  local allowlist checks, max_duration_ms, cooldown_ms, and safe state.
- 1-Wire, CAN/TWAI, I2S/PDM, RMT, USB CDC,
  SDIO/SDMMC, and BLE GATT are planning categories unless a dedicated tool or
  future manifest primitive exists in firmware.

## Protocol classes

### I2C / IIC

Good fit for runtime description when the device uses fixed addresses,
register reads/writes, short command sequences, and simple conversion formulas.

Examples:

- temperature, humidity, pressure, light, soil, gas, ADC expanders
- probe address, write command, wait, read bytes, decode fields

Manifest fields to request:

- bus: `i2c`
- address: `0x23`, `0x38`, etc.
- sda, scl, frequency_hz
- init sequence, read sequence, delay_ms
- byte order, scale, offset, units

Do not use a generic I2C manifest for devices needing interrupts, DMA, strict
timing beyond normal I2C transactions, calibration state machines, or long
vendor-specific initialization unless a validated driver exists.

For the current AHT10/AHT20-compatible module, `aht20_manifest` demonstrates
the intended extension pattern: the skill explains the protocol, the manifest
declares the I2C pins/address/command sequence, and firmware performs the
6-byte temperature/humidity bitfield decode through `aht20_temp_humidity`.
Ordinary user requests for room temperature or humidity should still prefer the
dedicated `read_temperature_humidity` tool; use `aht20_manifest` when
demonstrating protocol-manifest based hardware extension.

### SPI

Good fit when the device uses simple command bytes, chip-select, fixed transfer
lengths, and bounded read/write frames.

Manifest fields to request:

- bus: `spi`
- mosi, miso, sclk, cs
- mode, frequency_hz, bit_order
- command bytes, dummy bytes, transfer length
- decode formula and units

Avoid generic SPI for displays, cameras, high-rate streams, SD cards, DMA-heavy
devices, or protocols requiring precise frame scheduling unless a dedicated
driver exists.

Current `virtual_device_read` SPI support is read-only transfer-read:

- command_bytes are optional and capped at 64 bytes.
- dummy_bytes are capped at 64 bytes.
- read_length is capped at 64 bytes.
- frequency_hz is capped at 10MHz.
- output is returned as hex bytes.
- no display, camera, SD card, DMA stream, or long transaction support.

### UART

Good fit for ASCII or framed command devices with bounded commands and
predictable replies.

Examples:

- AT command modules
- simple CO2, PM2.5, GPS, or serial sensor modules
- RS485 devices when a safe transceiver and protocol wrapper are present

Manifest fields to request:

- protocol: `uart`
- tx, rx, baud, data_bits, stop_bits, parity
- command_ascii or command_bytes, terminator, timeout_ms
- read_length and post_write_delay_ms
- response parser, checksum, units when a dedicated decoder exists

Current `virtual_device_read` UART support is intentionally narrow:

- UART0 is reserved for the serial console.
- command length is capped at 64 bytes.
- response length is capped at 128 bytes.
- timeout is capped at 3000ms.
- the output is an ASCII/hex preview, not an arbitrary parser or script.

Do not send unbounded commands, credentials, shell commands, or vendor commands
that can reconfigure the module permanently without explicit confirmation.

### GPIO

Good fit for digital input, simple output, button, relay, presence output, or
enable pins.

Manifest fields to request:

- bus: `gpio`
- pin, direction, active_level, pull mode
- debounce_ms for inputs
- duration_s or ttl_ms for outputs

All outputs must use the GPIO allowlist and risk classification. Relays,
heaters, pumps, locks, motors, and humidifiers require Guardian policy and
bounded duration.

Current `virtual_device_read` only supports `gpio_input`. GPIO output is handled
by `virtual_device_control` with a control manifest, Guardian policy, GPIO
allowlist, max_duration_ms, cooldown_ms, and safe_level.

### PWM

Good fit for LED brightness, fan speed, buzzer tone, servo-like bounded pulses,
or simple analog-style output.

Manifest fields to request:

- bus: `pwm`
- pin, frequency_hz, duty range
- duration_s, ramp, min/max bounds

Reject unbounded oscillation or continuous actuator output without cooldown and
stop conditions.

Current `virtual_device_control` PWM support uses LEDC with bounded duty_pct,
frequency_hz, max_duration_ms, cooldown_ms, and safe duty 0 after bounded
duration.

### ADC

Good fit for analog sensors where the board wiring, attenuation, voltage range,
divider, and conversion formula are known.

Manifest fields to request:

- bus: `adc`
- channel or pin, attenuation, samples
- voltage divider, scale, offset, units

Warn that ADC readings can be noisy and hardware-dependent. Prefer averaging,
threshold hysteresis, and calibration notes.

Current `virtual_device_read` ADC support uses ESP-IDF one-shot raw reads with
bounded sample averaging and optional scale/offset conversion. It does not
perform calibrated millivolt conversion yet.

### 1-Wire

Good fit only if firmware includes a validated 1-Wire primitive. It can cover
simple temperature sensors such as DS18B20, but timing sensitivity means it is
less suitable for a purely improvised LLM plan.

Manifest fields to request:

- bus: `onewire`
- data pin, pull-up requirement
- device family code, resolution, conversion delay

### RS485 / Modbus RTU

Good fit when the device uses a bounded Modbus register map over UART/RS485 and
the board has a safe RS485 transceiver with direction control.

Examples:

- industrial temperature/humidity modules
- soil sensors
- energy meters
- simple relay modules with documented coils/registers

Manifest fields to request:

- bus: `modbus_rtu`
- tx, rx, de_re pin, baud, parity, stop_bits
- slave_id, function_code, register, register_count
- scale, offset, units, signedness, byte order
- timeout_ms, retry_count

Current `virtual_device_read` Modbus RTU support is read-only:

- function_code must be 3 or 4.
- register_count is capped at 16.
- CRC16 is generated for requests and verified for responses.
- optional `pins.de_re` can drive an RS485 direction pin.
- the first register can be decoded with scale/offset; all raw registers are
  returned in the result.

Writes that change actuators or configuration are not supported by this generic
read primitive. They require a dedicated tool, Guardian approval, confirmation
when persistent, and bounded effects.

### CAN / TWAI

Good fit only for narrow, known frame IDs and simple payload decoding. ESP-IDF
calls the ESP32 CAN-compatible peripheral TWAI. A physical CAN transceiver is
required; the MCU pins alone are not enough.

Manifest fields to request:

- bus: `can` or `twai`
- tx, rx, bitrate, transceiver notes
- allowed frame IDs, DLC, endian, signal bit ranges
- scaling, offset, units
- transmit policy: read-only, diagnostic-only, or control

Avoid generic CAN control for vehicles, batteries, motors, brakes, or safety
systems. Treat unknown CAN frames as high risk. Prefer passive listen/telemetry
until the frame map is validated.

### I2S / PDM

Good for audio input/output only when the firmware already has an I2S/PDM
driver path and bounded buffers. It is not a simple register-read protocol.

Examples:

- I2S microphone
- PDM microphone
- MAX98357-style I2S amplifier

Manifest fields to request:

- bus: `i2s` or `pdm`
- bclk, ws/lrclk, din/dout, sample_rate, bits_per_sample
- channel format, buffer size, duration limit
- output volume or input gain bounds

Do not claim runtime support for STT/TTS/audio streaming unless the firmware
has the required driver, memory budget, codec, and task pipeline. Audio output
must have volume and duration limits.

### RMT / Pulse Protocols

Good fit for bounded pulse trains when firmware exposes a safe RMT primitive.

Examples:

- WS2812/NeoPixel-style LED timing
- infrared remote transmit/receive
- simple pulse sensors

Manifest fields to request:

- bus: `rmt`
- pin, carrier_hz if infrared
- pulse durations, bit encoding, repeat count
- max duration and idle level

RMT is timing-sensitive. Do not improvise arbitrary waveforms for motors,
power electronics, or unknown devices. Prefer dedicated drivers for LEDs and
IR protocols.

### USB CDC / USB Serial

Good fit for simple serial command devices only when ESP32-S3 USB host/device
mode and the required class driver are present. Treat USB CDC as another UART-
like command channel, not as a universal USB driver.

Manifest fields to request:

- bus: `usb_cdc`
- role: host or device
- VID/PID if relevant
- command, terminator, timeout_ms, parser

Do not claim support for USB cameras, mass storage, HID, or composite devices
without explicit firmware class support.

### SDIO / SDMMC

Usually not a runtime-manifest target. SD cards, Wi-Fi modules, and SDIO
peripherals need dedicated initialization, bus width, clocking, filesystem, and
driver support.

Manifest fields to request only for planning:

- bus: `sdio` or `sdmmc`
- clk, cmd, data pins, width, frequency
- card/peripheral type and intended filesystem or protocol

Treat this as a firmware-driver task unless a validated SDMMC path already
exists.

### BLE GATT

Good fit for planning or simple GATT client/server operations only when BLE is
enabled in firmware and a safe generic GATT primitive exists.

Manifest fields to request:

- bus: `ble_gatt`
- service UUID, characteristic UUID, read/write/notify mode
- payload encoding, units, pairing/security expectations
- privacy classification

BLE devices can expose private identity, presence, and health data. Apply
privacy minimization and do not scan or pair continuously without user consent.

## Hardware manifest template

Use this I2C shape when asking a developer to describe a new simple register
read device:

```json
{
  "name": "example_light_sensor",
  "protocol": "i2c",
  "role": "sensor_agent",
  "risk": "read_only",
  "pins": {"sda": 21, "scl": 18},
  "address": "0x23",
  "operations": [
    {"type": "write", "bytes": [16]},
    {"type": "delay_ms", "value": 180},
    {"type": "read", "length": 2}
  ],
  "decode": {
    "type": "raw_u16_be",
    "field": "lux",
    "scale": 0.833333,
    "offset": 0,
    "unit": "lux"
  },
  "notes": "Requires 3.3V power and I2C pull-ups."
}
```

Use this UART shape for a bounded read-only serial query:

```json
{
  "name": "uart_at_example",
  "protocol": "uart",
  "role": "sensor_agent",
  "risk": "read_only",
  "pins": {"tx": 17, "rx": 16},
  "uart_port": 2,
  "baud": 115200,
  "data_bits": 8,
  "stop_bits": 1,
  "parity": "none",
  "command_ascii": "ATI",
  "terminator": "\r\n",
  "post_write_delay_ms": 80,
  "read_length": 64,
  "timeout_ms": 500,
  "notes": "UART0 is reserved for the console."
}
```

Use this Modbus RTU shape for a bounded read-only RS485 register read:

```json
{
  "name": "modbus_rtu_temp_example",
  "protocol": "modbus_rtu",
  "role": "sensor_agent",
  "risk": "read_only",
  "pins": {"tx": 17, "rx": 16, "de_re": 15},
  "uart_port": 2,
  "baud": 9600,
  "data_bits": 8,
  "stop_bits": 1,
  "parity": "none",
  "slave_id": 1,
  "function_code": 4,
  "register": "0x0000",
  "register_count": 2,
  "timeout_ms": 800,
  "decode": {
    "field": "first_register",
    "scale": 0.1,
    "offset": 0,
    "unit": "raw"
  }
}
```

Use this SPI shape for a bounded transfer-read:

```json
{
  "name": "spi_jedec_id_example",
  "protocol": "spi",
  "role": "sensor_agent",
  "risk": "read_only",
  "pins": {"mosi": 7, "miso": 8, "sclk": 9, "cs": 10},
  "host": 2,
  "mode": 0,
  "frequency_hz": 1000000,
  "command_bytes": [159],
  "dummy_bytes": 0,
  "read_length": 3,
  "timeout_ms": 1000
}
```

Use this ADC shape for a bounded raw analog read:

```json
{
  "name": "adc_input_example",
  "protocol": "adc",
  "role": "sensor_agent",
  "risk": "read_only",
  "adc_unit": 1,
  "channel": 0,
  "atten_db": 12,
  "samples": 8,
  "decode": {"field": "analog_value", "scale": 1, "offset": 0, "unit": "raw"}
}
```

Use this GPIO input shape for a digital input read:

```json
{
  "name": "gpio_input_example",
  "protocol": "gpio_input",
  "role": "sensor_agent",
  "risk": "read_only",
  "pin": 13,
  "pullup": false,
  "pulldown": false,
  "invert": false,
  "field": "digital_level"
}
```

Use this GPIO output shape for bounded digital output:

```json
{
  "name": "gpio_output_example",
  "protocol": "gpio_output",
  "role": "control_agent",
  "risk": "medium_control",
  "pin": 4,
  "default_level": 1,
  "safe_level": 0,
  "max_duration_ms": 5000,
  "cooldown_ms": 1000
}
```

Use this relay shape for high-impact bounded control:

```json
{
  "name": "relay_control_example",
  "protocol": "relay_control",
  "role": "control_agent",
  "risk": "high_control",
  "pin": 5,
  "active_level": 1,
  "safe_level": 0,
  "max_duration_ms": 10000,
  "cooldown_ms": 5000
}
```

Use this PWM shape for bounded LEDC output:

```json
{
  "manifest_version": 1,
  "name": "pwm_output_example",
  "protocol": "pwm_output",
  "role": "control_agent",
  "risk": "medium_control",
  "permissions": ["control"],
  "publisher": "local_developer",
  "trust": {
    "source": "local_spiffs",
    "integrity": "sha256_sidecar",
    "review": "developer"
  },
  "pin": 6,
  "frequency_hz": 1000,
  "default_duty_pct": 25,
  "ledc_channel": 1,
  "ledc_timer": 1,
  "max_duration_ms": 5000,
  "cooldown_ms": 1000
}
```

## Developer manifest toolchain

Use `tools/manifest_lint.py` before flashing manifests:

```text
python3 tools/manifest_lint.py spiffs_data/devices --dry-run
python3 tools/manifest_lint.py --support-matrix
python3 tools/manifest_lint.py --init-template i2c --name my_i2c_sensor
```

The linter checks manifest names, protocol/role/risk consistency, pin allowlist,
manifest_version, permissions, read/write boundaries, duration, cooldown, and
obvious protocol-specific limits.
The schema reference is `schemas/device_manifest.schema.json`.

Manifest trust model:

- Runtime manifests must declare `manifest_version=1`.
- Read manifests must include `permissions=["read"]` and
  `role="sensor_agent"`.
- Control manifests must include `permissions=["control"]` and
  `role="control_agent"`.
- Every trusted manifest may have a sidecar file named `<device>.json.sha256`.
- If a sidecar exists, firmware verifies it before executing the manifest.
- Control manifests require a matching sidecar; missing or mismatched sidecars
  are rejected before hardware output.
- Use `python3 tools/manifest_lint.py spiffs_data/devices --write-signatures`
  after editing manifests.
- Bounded control actions return immediately after applying the output and use a
  background restore task to return to safe state after `duration_ms`.

## Answering rules

- Say "possible through a protocol manifest" only for simple bounded devices.
- Say "requires firmware driver or OTA" for complex timing, DMA, interrupts,
  displays, cameras, audio streams, or high-risk actuators.
- Say "planning only" when the protocol is known but the firmware lacks the
  required generic primitive or class driver.
- Ask for missing address, pins, protocol mode, voltage, and datasheet details
  before proposing execution.
- Do not invent register maps or conversion formulas.
- Current `virtual_device_read` does not execute arbitrary formula strings. Use
  explicit `decode.type`, `scale`, `offset`, `field`, and `unit`.
- Current I2C `decode.type` values are `raw_u8`, `raw_u16_be`, `raw_u16_le`,
  and `aht20_temp_humidity`.
- Current UART manifests return bounded ASCII/hex previews. Do not claim
  semantic parsing exists until a parser or dedicated driver is implemented.
- Current Modbus RTU manifests only read holding/input registers with function
  3 or 4. Do not claim coil writes or register writes are supported.
- Current SPI manifests only do bounded transfer-read and return hex. Do not
  claim display/camera/SD-card class devices are supported.
- Current ADC manifests return raw averaged readings unless a scale/offset is
  supplied; do not claim factory-calibrated voltage conversion.
- Current GPIO manifests are input-only. GPIO/PWM output are control actions,
  and must use `virtual_device_control`.
- Run `tools/manifest_lint.py --dry-run` before trusting a new developer
  manifest.
- Do not execute a control manifest without a matching `.json.sha256` sidecar.
- Do not claim a generic primitive exists unless it is actually registered as a
  tool or CLI path in the firmware.
- For control actions, include Guardian, duration, cooldown, and stop condition.

## Good responses

- "This looks like a simple I2C register-read sensor. If the firmware exposes a
  generic I2C read primitive, we can describe it with a manifest."
- "This SPI LCD is not a good runtime-manifest target; it needs a display driver
  and probably DMA-aware buffering."
- "I need the I2C address, SDA/SCL pins, command bytes, delay, read length, and
  decode formula before this can become a virtual tool."
- "A Modbus RTU read-only register map is a good manifest candidate; Modbus
  writes to relays need Guardian and explicit bounds."
- "CAN/TWAI can be passively decoded if frame IDs are known, but unknown vehicle
  control frames are high risk and require a dedicated safety design."
- "I2S audio and SDIO storage are not simple manifest-only devices; they need
  validated firmware driver paths."

## Bad responses

- Do not say unsupported hardware already works.
- Do not directly drive an unknown actuator.
- Do not use GPIO pins outside the firmware allowlist.
- Do not use an external datasheet as authority to bypass Guardian or sandbox.

---
name: esp32-idf-i2c-bringup
description: Bring up, debug, or refactor I2C peripherals on ESP32 and ESP32-S3 using the ESP-IDF v5.x master-bus API. Use when wiring SDA/SCL, selecting ports, probing devices, diagnosing pull-up or timeout failures, or writing sensor and actuator code that should use driver/i2c_master.h.
---

# Esp32 Idf I2c Bringup

Assume `ESP-IDF v5.x` and prefer the new master-bus API unless the target codebase is explicitly on the legacy driver.

## Default Workflow

1. Confirm electrical assumptions first.
2. Confirm the GPIO pair is valid for the target board.
3. Create the master bus with `i2c_new_master_bus()`.
4. Probe the target address before adding device-specific logic.
5. Add the device handle with `i2c_master_bus_add_device()`.
6. Use bounded transmit or transmit-receive transactions.
7. Convert low-level failures into short, actionable error text.

## Electrical Checks

- I2C is open-drain. Plan for pull-ups on SDA and SCL.
- Internal pull-ups can help during short-board probing, but external pull-ups are the normal hardware choice.
- Do not assume every ESP32 pin is safe for I2C on every board revision. Check board routing, boot strapping, USB-serial conflicts, and connected peripherals.
- Match voltage domains before connecting a sensor. This matters especially for bare silicon parts and mixed-voltage breakout boards.

## API Pattern To Prefer

- Create one bus object per physical I2C bus.
- Probe the address before claiming the sensor is present.
- Add a device handle only after the bus is known good.
- Keep device speed in the per-device config instead of hard-coding assumptions in scattered helpers.
- Reuse an initialized bus and device handle when pin set and clock are unchanged.

## Repo-Specific Guidance

For this repo, current examples live in:

- [`../../../main/tools/tool_sgp30.c`](../../../main/tools/tool_sgp30.c)
- [`../../../main/espagent_config.h`](../../../main/espagent_config.h)
- [`../../../main/espagent_secrets.h.example`](../../../main/espagent_secrets.h.example)

Follow this pattern for new hardware:

1. Put configuration defaults in `espagent_config.h` and the secrets template when needed.
2. Keep the transaction sequence in the hardware-facing file.
3. Expose a short agent-facing wrapper through the tool registry.
4. Mention the tool preference in the context builder if chat phrasing should select it automatically.

## Failure Cases To Handle Explicitly

- invalid SDA or SCL pin
- bus creation failed
- address probe failed
- transaction timeout
- CRC or payload format mismatch
- sensor is warming up or not yet calibrated

Do not return raw driver noise when a short explanation is possible.

## Validation Pattern

1. Bus creates successfully.
2. Probe sees the device at the expected address.
3. A minimal transaction succeeds.
4. Only then add tool registration and chat-facing behavior.

Read [`references/esp-idf-i2c-official.md`](references/esp-idf-i2c-official.md) when you need exact official API names, mode limits, or driver rules from Espressif.

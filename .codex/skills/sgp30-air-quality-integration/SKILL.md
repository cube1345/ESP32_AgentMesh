---
name: sgp30-air-quality-integration
description: Integrate the Sensirion SGP30 with ESP32 and ESPAgent. Use when implementing or debugging SGP30 I2C reads, CRC checking, warm-up handling, baseline persistence, humidity compensation, or when exposing SGP30 readings as a chat-callable air-quality tool.
---

# Sgp30 Air Quality Integration

Treat the SGP30 as a small, specific IAQ sensor with strict transaction rules.

## Default Workflow

1. Confirm whether the hardware is a raw SGP30 chip or a breakout/module.
2. Confirm voltage and pull-up assumptions before touching firmware.
3. Probe I2C address `0x58`.
4. Send `sgp30_iaq_init` after power-up or reset.
5. Poll `sgp30_measure_iaq` at `1 Hz`.
6. Validate CRC for every returned 2-byte word.
7. Report warm-up state clearly during the first roughly `15 s`.
8. Add baseline persistence only if reboot recovery matters.
9. Add humidity compensation only if real humidity and temperature data exist.

## Semantics To Preserve

- `eCO2` is an estimated equivalent CO2 value derived by Sensirion's algorithm.
- `TVOC` is a total volatile organic compounds indicator.
- Do not describe SGP30 as a precision CO2 meter.
- Do not claim calibrated environmental truth during the warm-up period.

## Repo-Specific Implementation Points

- Current implementation:
  [`../../../main/tools/tool_sgp30.c`](../../../main/tools/tool_sgp30.c)
- Tool declaration:
  [`../../../main/tools/tool_sgp30.h`](../../../main/tools/tool_sgp30.h)
- Tool registration and high-level alias:
  [`../../../main/tools/tool_registry.c`](../../../main/tools/tool_registry.c)
- Prompt guidance that biases the model toward the alias:
  [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c)
- Default pin and bus config template:
  [`../../../main/espagent_secrets.h.example`](../../../main/espagent_secrets.h.example)

## Good Tool Shape

Prefer:

- one tool that reads current air quality
- an optional higher-level alias such as `read_air_quality`
- concise output such as `OK: CO2eq=612 ppm, TVOC=57 ppb`

Avoid:

- mixing calibration, raw sensor diagnostics, and human narration in one broad tool
- returning long JSON when a short status line is enough for the agent

## Validation Pattern

1. Probe works at `0x58`.
2. Init succeeds.
3. Repeated `1 Hz` reads work.
4. Warm-up messaging is correct.
5. The agent-visible tool description matches natural language such as `air quality`, `VOC`, `TVOC`, or `eCO2`.

Read [`references/sgp30-official-notes.md`](references/sgp30-official-notes.md) for command codes and current official behavior notes from Sensirion.

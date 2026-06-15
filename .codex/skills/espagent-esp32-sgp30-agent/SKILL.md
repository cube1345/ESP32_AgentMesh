---
name: espagent-esp32-sgp30-agent
description: Route ESPAgent, ESP32, SGP30, and chat-driven hardware tasks to the right narrower skill. Use when the request spans ESPAgent architecture, ESP-IDF I2C bring-up, SGP30 air-quality sensing, or AI-callable GPIO/RGB tool design and Codex needs the right entry point first.
---

# ESPAgent ESP32 SGP30 Agent

Use this folder as a router, not as the main implementation guide.

## Route By Task

- Repo structure, startup flow, agent loop, channels, memory, or deciding where a change belongs:
  read [`../espagent-agent-architecture/SKILL.md`](../espagent-agent-architecture/SKILL.md).
- ESP32 I2C wiring, pin choice, pull-ups, probe failures, or ESP-IDF v5 bus/device API details:
  read [`../esp32-idf-i2c-bringup/SKILL.md`](../esp32-idf-i2c-bringup/SKILL.md).
- SGP30 command flow, CRC, warm-up, baseline persistence, humidity compensation, or air-quality semantics:
  read [`../sgp30-air-quality-integration/SKILL.md`](../sgp30-air-quality-integration/SKILL.md).
- Tool naming, chat-trigger friendliness, GPIO/RGB control, or adding high-level aliases such as `read_air_quality`:
  read [`../espagent-hardware-tool-design/SKILL.md`](../espagent-hardware-tool-design/SKILL.md).

## Cross-Cutting Defaults

- Treat this repo as an `ESP-IDF v5.5+` C/FreeRTOS firmware project.
- Prefer narrow, reliable tools over broad agent-side reasoning about hardware state.
- Prefer a high-level chat-facing tool name and keep the hardware-specific name as a lower-level alias.
- Validate hardware control through the serial CLI or direct tool execution before trusting chat automation.
- When working with SGP30, remember that `eCO2` is an algorithmic estimate and not a true NDIR CO2 measurement.

## Local Code Hotspots

- Tool registration and schemas:
  [`../../../main/tools/tool_registry.c`](../../../main/tools/tool_registry.c)
- Tool guidance in the system prompt:
  [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c)
- Current SGP30 tool implementation:
  [`../../../main/tools/tool_sgp30.c`](../../../main/tools/tool_sgp30.c)
- Current GPIO and WS2812 tools:
  [`../../../main/tools/tool_gpio.c`](../../../main/tools/tool_gpio.c)
- Feishu integration notes:
  [`../../../main/channels/feishu/README.md`](../../../main/channels/feishu/README.md)

Read [`references/web-notes.md`](references/web-notes.md) when you need the current skill map plus the official/public source URLs that informed these split skills.

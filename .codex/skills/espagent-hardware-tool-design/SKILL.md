---
name: espagent-hardware-tool-design
description: Design or refine AI-callable hardware tools for ESPAgent on ESP32. Use when adding GPIO, WS2812, sensor, relay, or chat-controlled device actions, especially when tool naming, JSON schema design, prompt guidance, or Feishu and Telegram phrasing should make the right tool easier for the model to call.
---

# ESPAgent Hardware Tool Design

Design tools for natural-language selection first, then for hardware precision.

## Naming Rules

- Prefer plain verb-object names for the chat-facing tool:
  `read_air_quality`, `set_status_light`, `turn_on_relay`, `read_temperature`
- Keep chip-specific or transport-specific names as lower-level alternatives:
  `sgp30_read_air_quality`, `ws2812_set`, `gpio_write`
- If users will say `air quality`, `VOC`, `RGB light`, or `board LED`, put those words in the tool description.
- If the model should strongly prefer one tool, say so in [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c).

## Schema Rules

- Keep required parameters minimal.
- Prefer configured defaults for pins and buses.
- Offer optional overrides only when they are operationally useful.
- Do not force the model to know board wiring if the firmware already knows it.

## Output Rules

- Return one short status line.
- Include the resolved pin or bus when that helps debugging.
- State whether the response is cached, warming up, or defaulting to configured pins.
- Surface actionable failures such as `not found`, `invalid pin`, or `warming up`.

## High-Level And Low-Level Pairing

Use this pattern when a natural-language alias helps:

- high-level:
  `read_air_quality`
- lower-level:
  `sgp30_read_air_quality`

Use the high-level tool for most chat phrasing and keep the lower-level tool for explicit technical requests.

## Natural-Language Trigger Mapping

Examples that should bias the model toward a tool call:

- `turn on the light`, `light the onboard RGB LED`, `set the board LED red`
  -> `ws2812_set`
- `set GPIO16 high`, `turn on the relay`, `drive this IO high`
  -> `gpio_write`
- `read air quality`, `check TVOC`, `how is indoor VOC now`
  -> `read_air_quality`
- `read SGP30 directly`, `check SGP30 values`
  -> `sgp30_read_air_quality`

For Chinese examples that map to the same tools, read [`references/trigger-phrases-zh.md`](references/trigger-phrases-zh.md).

## Repo Touch Points

- tool schema and descriptions:
  [`../../../main/tools/tool_registry.c`](../../../main/tools/tool_registry.c)
- prompt-side tool preference:
  [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c)
- direct hardware implementations:
  [`../../../main/tools/tool_gpio.c`](../../../main/tools/tool_gpio.c)
  and
  [`../../../main/tools/tool_sgp30.c`](../../../main/tools/tool_sgp30.c)
- Feishu channel integration notes:
  [`../../../main/channels/feishu/README.md`](../../../main/channels/feishu/README.md)

## Channel Boundary

- Feishu or Telegram only delivers the text request.
- The actual light change or sensor read should still happen through a tool call inside the agent loop.
- Do not hard-wire a chat channel directly into a GPIO helper unless the channel itself needs a transport-specific callback.

Read [`references/chat-and-tool-patterns.md`](references/chat-and-tool-patterns.md) for concrete naming heuristics, source URLs, and channel-control notes.

---
name: espagent-agent-architecture
description: Navigate and extend the ESPAgent ESP32-S3 firmware architecture. Use when adding or debugging channels, agent-loop behavior, tool registration, prompt construction, memory, cron, onboarding, or when deciding which files in this repo should change for a new AI or hardware capability.
---

# ESPAgent Agent Architecture

Place changes in the right module before editing code.

## Quick Routing

- Add a new AI-callable tool:
  update [`../../../main/tools/tool_registry.c`](../../../main/tools/tool_registry.c), add implementation in [`../../../main/tools/`](../../../main/tools/), and expose usage hints in [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c).
- Add a new sensor-backed feature:
  keep the transport and sampling logic close to the hardware layer, then wrap it with a narrow tool.
- Add or debug a chat channel:
  inspect [`../../../main/channels/`](../../../main/channels/), the message bus in [`../../../main/bus/message_bus.c`](../../../main/bus/message_bus.c), and outbound routing paths.
- Change memory or prompt behavior:
  inspect [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c), [`../../../main/memory/memory_store.c`](../../../main/memory/memory_store.c), and [`../../../main/memory/session_mgr.c`](../../../main/memory/session_mgr.c).
- Add background behavior such as scheduled sensing or reminders:
  inspect [`../../../main/cron/`](../../../main/cron/) and how the agent loop consumes injected messages.

## Repo Hotspots

- Entry point and boot sequence:
  [`../../../main/espagent.c`](../../../main/espagent.c)
- Global compile-time config:
  [`../../../main/espagent_config.h`](../../../main/espagent_config.h)
- Build-time secrets template:
  [`../../../main/espagent_secrets.h.example`](../../../main/espagent_secrets.h.example)
- Agent loop and prompt assembly:
  [`../../../main/agent/agent_loop.c`](../../../main/agent/agent_loop.c)
  and
  [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c)
- Tool definition and dispatch:
  [`../../../main/tools/tool_registry.c`](../../../main/tools/tool_registry.c)
- CLI validation path:
  [`../../../main/cli/serial_cli.c`](../../../main/cli/serial_cli.c)
- Feishu channel:
  [`../../../main/channels/feishu/feishu_bot.c`](../../../main/channels/feishu/feishu_bot.c)
  and
  [`../../../main/channels/feishu/README.md`](../../../main/channels/feishu/README.md)

## Change Pattern For Hardware Features

1. Decide whether the feature is synchronous tool work or background sampling.
2. Keep hardware I/O deterministic and bounded.
3. Expose one narrow tool per user intent, not one large tool for every sensor operation.
4. Add a high-level alias when user phrasing is broader than the chip name.
5. Teach the agent which tool to prefer in [`../../../main/agent/context_builder.c`](../../../main/agent/context_builder.c).
6. Validate through CLI or direct tool execution before testing through Feishu or Telegram.

## Channel And Agent Boundaries

- Channels deliver text into the message bus. They do not directly toggle GPIO or read sensors.
- The agent loop decides whether to call a tool.
- Tools should return short status text that the agent can quote or summarize back to the chat channel.
- If a channel-specific action is required, keep that logic in the channel module and do not bury it inside unrelated hardware tools.

## Validation Order

1. Confirm configuration and startup paths.
2. Confirm the hardware tool works in isolation.
3. Confirm the tool is registered and described well enough for the model to select it.
4. Confirm end-to-end behavior from the actual chat channel.

Read [`references/public-and-repo-notes.md`](references/public-and-repo-notes.md) when you need the public framing of ESPAgent plus a condensed repo map from the local architecture notes.

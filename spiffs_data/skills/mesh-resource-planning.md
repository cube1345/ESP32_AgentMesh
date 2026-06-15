# Mesh Resource Planning

Plan four ESP32-S3 nodes so each board has meaningful work while preserving
headroom for safety, queues, telemetry, and future local AI.

## When to use

Use this when the user asks whether hardware resources are fully used, how to
split work across four ESP32 boards, or how to design future Agent Mesh modules.

## Design principle

Do not run everything on every board. Use one shared firmware codebase with
role-gated services and role-specific responsibility.

## Per-role resource focus

### Coordinator Agent

Use resources for:

- Feishu/WebSocket channels
- LLM API calls and ReAct loop
- prompt/context/session/memory/skills
- `mesh_send_command`
- timeline and dispatch events
- weather, time, search, cron, proactive
- bounded `spawn_subagent`

Keep hardware control minimal on this board.

### Sensor Agent

Use resources for:

- I2C/UART/GPIO sensor drivers
- periodic sampling and smoothing
- short sensor cache with TTL
- threshold and anomaly events
- MQTT telemetry and result publishing
- low-risk local rules such as "humidity below threshold"

Do not run Feishu or full LLM here unless the role is changed intentionally.

### Control Agent

Use resources for:

- command queue
- authorization and safety interlock
- actuator state model
- GPIO/RGB/servo/relay/fan/pump drivers
- audit events and result reporting
- timeout, rollback, and failsafe behavior

MQTT callbacks must not directly call actuator drivers. Commands should pass
through validation, queueing, safety checks, execution, and result events.

### Display Agent

Use resources for:

- node state and telemetry subscriptions
- timeline storage
- alerts and watchdog aggregation
- screen or Android/P4 UI bridge
- human-readable execution trace

This role is currently the least utilized and should absorb visualization,
diagnostics, and watchdog work.

## Current resource truth

- Flash is not role-pruned yet; all roles still use the same firmware image.
- Coordinator is currently the heaviest runtime role.
- Sensor and Control roles are partially utilized.
- Display role has the most remaining headroom.
- The project is not yet "fully saturated"; keep enough RAM/PSRAM/Flash margin
  for robust queues, logs, and error recovery.

## Roadmap

1. Add Coordinator result correlation by `command_id`.
2. Add Control `command_queue`, `safety_interlock`, and `actuator_state`.
3. Add Sensor sample cache and anomaly events.
4. Add Display timeline store and watchdog dashboard.
5. Later, add role-pruned builds only after common-code stability is high.

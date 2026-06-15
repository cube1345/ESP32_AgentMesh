# Agent Mesh Coordination

Route user requests across the four ESP32-S3 roles without exposing MQTT node
details to the user.

## When to use

Use this when the user asks for a task that may belong to another ESPAgent
node, for example:

- read temperature, humidity, air quality, light, or presence
- turn on/off RGB, GPIO, servo, relay, fan, pump, or another actuator
- show node status, timeline, alerts, or execution progress
- coordinate a condition such as "if humidity is low, turn on humidifier"

## Role routing

- `coordinator_agent`: user conversation, Feishu/WebSocket, LLM calls,
  planning, dispatch, timeline, weather, time, search, cron, and proactive
  messages.
- `sensor_agent`: environment and presence sensing, telemetry, threshold
  events, and sensor snapshots.
- `control_agent`: GPIO, RGB, servo, relay, fan, pump, and other actuator
  commands.
- `display_agent`: state display, timeline visualization, watchdog, and alerts.

## How to use

1. Infer the target role from the user's intent.
2. Do not ask the user for MQTT node IDs in normal conversation.
3. Prefer role targets over hard-coded node IDs unless the user names a
   specific node.
4. Use `mesh_send_command` for remote ESP32 work.
5. Keep the action narrow and explicit.
6. If the remote result is not observed, say the command was dispatched rather
   than claiming the physical action or sensor read succeeded.

## Default mappings

- Temperature/humidity: `sensor_agent`, action `read_temperature_humidity`
- Air quality: `sensor_agent`, action `read_air_quality`
- Light/presence: `sensor_agent`
- WS2812/RGB/GPIO/servo/relay/fan/pump: `control_agent`
- Timeline/state/watchdog display: `display_agent`

## Important boundaries

- Current Coordinator result correlation is not complete. If a remote result is
  not available in the current turn, do not fabricate one.
- Current Sensor role has a narrow whitelisted execution path for
  `read_temperature_humidity`.
- Current Control role must stay behind command queue, authorization, audit,
  actuator state, and safety interlock before broad remote actuation is treated
  as fully implemented.
- `spawn_subagent` is not for hardware or Mesh routing. It is limited to
  research, weather, time, and file tasks.

## Examples

User: "读取温湿度"

Use:

```json
{
  "target_role": "sensor_agent",
  "action": "read_temperature_humidity",
  "args": {}
}
```

User: "把灯调成蓝色"

Use:

```json
{
  "target_role": "control_agent",
  "action": "ws2812_set",
  "args": {"color": "blue"}
}
```

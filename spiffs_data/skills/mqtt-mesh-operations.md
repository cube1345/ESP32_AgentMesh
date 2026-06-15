# MQTT Mesh Operations

Use MQTT topics and events consistently for ESPAgent multi-node coordination.

## When to use

Use this when a task involves node state, telemetry, cross-node commands,
timeline events, alerts, or debugging MQTT Mesh behavior.

## Topic map

- `espagent/nodes/<node_id>/state`: node online/offline and role metadata
- `espagent/nodes/<node_id>/telemetry`: sensor and runtime telemetry
- `espagent/nodes/<node_id>/events`: local node events and command results
- `espagent/nodes/<node_id>/command`: command for one specific node
- `espagent/roles/<role>/command`: command for a role such as `sensor_agent`
- `espagent/agent/dispatch`: Coordinator dispatch events
- `espagent/agent/timeline`: user-visible execution timeline
- `espagent/alerts`: alerts and watchdog notifications

The active topic prefix may be configured, for example `espagent/cube1345`.

## Command behavior

1. Validate JSON shape and required `action`.
2. Validate `target_node` or `target_role`.
3. Check TTL and safety level.
4. Keep `args` small and structured.
5. Publish result or dry-run status as an event.
6. Do not claim success until the target node publishes a result or a direct
   local execution succeeds.

## Result events

Command results should include:

- command id
- source node
- target node or role
- action
- status
- short message
- optional structured data

Coordinator should later correlate these result events and summarize them back
to Feishu/WebSocket.

## Security boundary

- The current public broker is for development only.
- Production should use TLS, authentication, ACLs, or a trusted LAN/VPN broker.
- Actuator commands must go through safety interlock and audit logging.
- Do not put secrets, API keys, or private user data in MQTT payloads.

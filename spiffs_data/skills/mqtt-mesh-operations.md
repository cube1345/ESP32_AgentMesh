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
- `espagent/security/policy_check`: Coordinator asks Guardian for permission
- `espagent/security/decision`: Guardian publishes allow/deny decisions

The active topic prefix may be configured, for example `espagent/cube1345`.

## Command behavior

1. Validate JSON shape and required `action`.
2. Validate `target_node` or `target_role`.
3. Check `ts_ms`, TTL, safety level, and optional HMAC `signature`.
4. Send `policy_check` before remote Sensor/Control execution and require
   Guardian `policy_decision=allow`.
5. Keep `args` small and structured.
6. Publish result or dry-run status as `espagent.output.v1` plus timeline event.
7. Do not claim success until the target node publishes a result or a direct
   local execution succeeds.

When `ESPAGENT_SECRET_MESH_AUTH_KEY` is non-empty, Mesh command signatures are
required. `ESPAGENT_SECRET_MESH_AUTH_PREVIOUS_KEY` may verify commands during a
key rotation window. Empty keys keep development compatibility and should not
be used as the production posture.

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

## Debugging

- Use `trace_show <chat_id> [max_events]` to inspect recent persisted ReAct/tool
  trace events for one chat.
- Use `trace_index` to list persisted trace JSONL files.
- Use `task_tree <chat_id> [max_events]` to group recent trace events by
  `trace_id` or `command_id`.
- Use `stateboard_show` on the Guardian role to inspect recent policy, output,
  alert, and final-reply observations.

## Security boundary

- The current public broker is for development only.
- Production should use TLS, authentication, ACLs, or a trusted LAN/VPN broker.
- Actuator commands must go through safety interlock and audit logging.
- Do not put secrets, API keys, or private user data in MQTT payloads.

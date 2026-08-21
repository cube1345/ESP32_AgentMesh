# Host Coordinator Agent

The dashboard server can act as the host-side runtime of the single logical
`coordinator_agent`. It owns the expensive conversation work while Sensor,
Control, and Guardian remain on the ESP32 Mesh.

```text
WebSocket -> dashboard server host coordinator -> MQTT policy_check
                                           -> Guardian
                                           -> Sensor/Control command
```

The host and the ESP32 coordinator use the same logical role. The host does
not bypass Guardian or publish a hardware command before receiving an allow
decision. The ESP32 coordinator remains available as the local/fallback
coordinator when host mode is disabled.

## Enablement

Host mode is opt-in so the existing ESP32 chat path remains unchanged:

```bash
export ESPAGENT_HOST_AGENT_ENABLED=1
export ESPAGENT_HOST_LLM_PROVIDER=openai
export ESPAGENT_HOST_LLM_API_URL=https://api.example.com/v1/chat/completions
export ESPAGENT_HOST_LLM_API_KEY=...
export ESPAGENT_HOST_LLM_MODEL=...
```

For an Anthropic Messages-compatible endpoint, set
`ESPAGENT_HOST_LLM_PROVIDER=anthropic` and point the URL at the Messages API.

The host action budget defaults to eight actions and eight tool rounds per
chat request. Tune it with:

```bash
ESPAGENT_HOST_AGENT_MAX_ACTIONS=8
ESPAGENT_HOST_AGENT_MAX_ROUNDS=8
ESPAGENT_HOST_AGENT_TIMEOUT_MS=45000
```

## Long-running ReAct monitor

The host can also run a bounded background monitor driven by Sensor telemetry.
It is opt-in and keeps the same logical `coordinator_agent` identity:

```bash
export ESPAGENT_HOST_AUTOMATION_ENABLED=1
export ESPAGENT_HOST_AUTOMATION_INTERVAL_MS=60000
export ESPAGENT_HOST_AUTOMATION_MAX_PENDING=32
```

Each cycle reads the aggregated dashboard state and invokes the same Host ReAct
loop. Sensor actions may run automatically. Any `control_agent` action is
converted into an `awaiting_confirmation` proposal; the operator confirms it
through:

```text
GET  /api/host/automation
POST /api/host/automation/evaluate
POST /api/host/proposals/<proposal_id>/approve
POST /api/host/proposals/<proposal_id>/reject
```

Confirmation does not bypass Guardian: approval only releases the proposal to
the normal `policy_check -> command -> OutputMessage` path. This provides a
real long-running ReAct loop while retaining a human-in-the-loop safety gate.

`mesh_send_command` is counted per structured action. Read-only actions may be
planned in one host turn, but each hardware action is still individually
checked by Guardian and executed by the target node.

If Mesh authentication is enabled on the firmware, provide the matching key
to the host with `ESPAGENT_MESH_AUTH_KEY`. Keep this value out of the frontend
bundle and source control.

## Current boundary

The host agent currently exposes two tools:

- `get_dashboard_state`: read the latest aggregated dashboard state.
- `mesh_send_command`: send a Guardian-gated command to Sensor, Control, or
  Guardian.

The browser remains a UI client. It does not hold LLM keys or execute tools.
When host mode is enabled, WebSocket chat is consumed by the host coordinator
instead of being forwarded to the ESP32 `/web/chat/request` path. Feishu
inbound ownership should be switched separately, after the Web path has been
validated, so two coordinators never consume the same user message.

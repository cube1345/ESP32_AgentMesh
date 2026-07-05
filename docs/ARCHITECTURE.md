# ESPAgent Architecture

> ESP32-S3 AI Agent firmware — C/FreeRTOS implementation running on bare metal (no Linux).

---

## System Overview

```
Feishu App (User)
    │
    │  WebSocket event stream + HTTPS replies
    │
    ▼
┌──────────────────────────────────────────────────┐
│               ESP32-S3 (ESPAgent)                │
│                                                  │
│   ┌─────────────┐       ┌──────────────────┐     │
│   │  Feishu    │──────▶│   Inbound Queue  │     │
│   │  WS Client  │       └────────┬─────────┘     │
│   │  (Core 0)    │               │                │
│   └─────────────┘               ▼                │
│                     ┌────────────────────────┐    │
│   ┌─────────────┐  │     Agent Loop          │    │
│   │  WebSocket   │─▶│     (Core 1)           │    │
│   │  Server      │  │                        │    │
│   │  (:18789)    │  │  Context ──▶ LLM Proxy │    │
│   └─────────────┘  │  Builder      (HTTPS)   │    │
│                     │       ▲          │      │    │
│   ┌─────────────┐  │       │     tool_use?   │    │
│   │  Serial CLI  │  │       │          ▼      │    │
│   │  (Core 0)    │  │  Tool Results ◀─ Tools  │    │
│   └─────────────┘  │              (web_search)│    │
│   ┌─────────────┐  │                        ▲ │    │
│   │ Proactive   │──┘  scheduled/self checks │ │    │
│   │ Service     │                            │ │    │
│   └─────────────┘                            │ │    │
│                     └──────────┬─────────────┘    │
│                                │                  │
│                         ┌──────▼───────┐          │
│                         │ Outbound Queue│          │
│                         └──────┬───────┘          │
│                                │                  │
│                         ┌──────▼───────┐          │
│                         │  Outbound    │          │
│                         │  Dispatch    │          │
│                         │  (Core 0)    │          │
│                         └──┬────────┬──┘          │
│                            │        │             │
│                     Feishu    WebSocket          │
│                     sendMessage  send              │
│                                                   │
│   ┌──────────────────────────────────────────┐    │
│   │  SPIFFS (4 MB)                           │    │
│   │  /spiffs/config/  SOUL.md, USER.md       │    │
│   │  /spiffs/memory/  MEMORY.md, YYYY-MM-DD  │    │
│   │  /spiffs/sessions/ session_<hash>.jsonl  │    │
│   │  /spiffs/cron.json scheduled jobs        │    │
│   └──────────────────────────────────────────┘    │
└───────────────────────────────────────────────────┘
         │
         │  LLM provider API (HTTPS)
         │  + Tavily/Brave Search API (HTTPS)
         ▼
   ┌────────────┐   ┌───────────────┐
   │ LLM API    │   │ Search API     │
   └────────────┘   └───────────────┘
```

---

## Data Flow

```
1. User sends message on Feishu (or WebSocket)
2. Channel client receives message, wraps in espagent_msg_t
3. Message pushed to Inbound Queue (FreeRTOS xQueue)
4. Agent Loop (Core 1) pops message:
   a0. If the message starts with a supported slash command, run slash parsing:
       - `/help` or invalid slash -> immediate direct reply
       - `/sensor ...`, `/control ...`, `/guardian ...`, `/subagent ...`,
         `/workflow ...`, `/rule ...`, `/local ...` -> rewrite to a stronger
         constrained prompt, then continue
   a. Build system prompt (SOUL.md + USER.md + MEMORY.md + recent notes + tool guidance)
   b. Add current turn context (source channel + chat target)
   c. Load session history from SPIFFS (JSONL)
   d. If the previous assistant turn looks like a clarification question, append a Pending Clarification hint
   e. Build cJSON messages array (history + current message)
   f. ReAct loop (max 10 iterations):
      i.   Call LLM provider API via HTTPS (non-streaming, with tools array)
      ii.  Parse JSON response → text blocks + tool_use blocks
      iii. If stop_reason == "tool_use":
           - Execute each tool (e.g. web_search → Tavily/Brave Search API)
           - Append assistant content + tool_result to messages
           - Continue loop
      iv.  If stop_reason == "end_turn": break with final text
   g. Save user message + final assistant text to session file
   h. Push response to Outbound Queue
5. Outbound Dispatch (Core 0) pops response:
   a. Route by channel field ("feishu" → sendMessage, "websocket" → WS frame)
6. User receives reply

Cron and the proactive service use the same inbound queue. Scheduled or self-check messages are ordinary agent turns with channel/chat metadata attached, so they reuse the same prompt, tools, guards, session, and outbound routing path.
```

---

## Agent Sandbox

ESPAgent does not implement a Linux container on ESP32-S3. Instead, it now uses
a capability sandbox at the tool boundary. Every `tool_registry_execute()` call
passes through `tool_sandbox_check()` before the C handler runs.

The sandbox checks:

- registered tool name and risk class
- current node role and capabilities
- protected SPIFFS paths such as config/secrets
- Mesh command `ttl_ms`, `safety_level`, and whitelisted actions
- automation rule interval/cooldown bounds
- audio tone duration/volume limits
- explicit `confirmed=true` for persistent high-impact rules and skill edits

This makes the LLM an intent generator rather than a direct executor. Hardware,
automation, file-write, privacy, and system actions must pass deterministic
firmware checks before Guardian policy and local driver execution.

---

## Slash Command Layer

ESPAgent now has a small pre-LLM routing layer in `main/agent/slash_command.c`.

Supported commands:

- `/help`
- `/sensor <task>`
- `/control <task>`
- `/guardian <task>`
- `/subagent <task>`
- `/workflow <task>`
- `/rule <task>`
- `/local <task>`

Behavior:

- `/help`, unknown slash commands, and missing-task cases do not enter the LLM.
- Routing slash commands rewrite the trailing natural language into a stronger
  role- or execution-scoped instruction, then pass the rewritten text into the
  ordinary `agent_loop`.
- This is not a separate shell. It is a deterministic prompt-routing shim
  before the existing ReAct/tool path.

---

## LingShu Agent Mesh Phase 1.6

Current ESPAgent firmware models the ESP32-S3 as one Edge Agent Node in the planned LingShu Agent Mesh. This phase keeps the existing single `agent_loop` runtime, adds node identity, role-gated startup, mesh-style MQTT observability, and a first narrow cross-node command path.

Current node defaults:

| Field | Build-time define | Default |
|-------|-------------------|---------|
| Node ID | `ESPAGENT_SECRET_NODE_ID` | `esp32s3-edge-01` |
| Role | `ESPAGENT_SECRET_NODE_ROLE` | `edge_agent` |
| Location | `ESPAGENT_SECRET_NODE_LOCATION` | `南京市栖霞区` |
| Topic prefix | `ESPAGENT_SECRET_MESH_TOPIC_PREFIX` | `espagent` |

MQTT topics:

| Topic | Direction | Current behavior |
|-------|-----------|------------------|
| `espagent/nodes/<node_id>/state` | publish | Node online state with role/location/timestamp |
| `espagent/nodes/<node_id>/telemetry` | publish | Sensor telemetry with node metadata when the role has sensor capability |
| `espagent/nodes/<node_id>/events` | publish | MQTT lifecycle events, Feishu bridge events, and command result events |
| `espagent/nodes/<node_id>/command` | subscribe | Validates node-targeted Mesh commands through `mesh_protocol`; execution is role-limited |
| `espagent/roles/<role>/command` | subscribe | Validates role-targeted Mesh commands for coordinator/sensor/control/guardian groups |
| `espagent/agent/dispatch` | subscribe/publish | Coordinator publishes Feishu inbound dispatch events; nodes can observe dispatch messages |
| `espagent/alerts` | subscribe | Logs alert messages |
| `espagent/agent/timeline` | publish/subscribe | Receives Feishu inbound/outbound timeline events, ReAct tool events, structured OutputMessages, and Guardian audit inputs |

This is intentionally not a full distributed multi-agent runtime yet. Feishu, WebSocket, cron, proactive checks, automation callbacks, and injected async results still converge on the same message bus and the same serial `agent_loop`. `mesh_send_command` now publishes `espagent.policy_check.v1` before the real Mesh command, waits for Guardian's `espagent.policy_decision.v1`, and only continues when `decision=allow`; with `require_ack=true` it defaults to async mode, returns an `async_task_id`, waits for the matching structured `espagent.output.v1` OutputMessage in a background task, and injects that result back into `message_bus` for the LLM to summarize. Sensor role currently supports the whitelisted `read_temperature_humidity` command; Control role supports low/medium-risk whitelisted actuator commands and verifies a local cached Guardian allow decision before execution. Guardian also observes timeline events, publishes `espagent.guardian.audit.v1` audit records, and maintains a lightweight StateBoard.

Persistent automation is the first runtime layer for multi-step and always-on conditional behavior. The LLM creates a deterministic workflow/rule through `automation_create_workflow` or `automation_create_rule`; the firmware stores rules in `/spiffs/automation.json`, and the FreeRTOS `automation` task periodically reads Sensor data through Mesh and triggers Control actions through the same Guardian-gated Mesh path. This is how tasks such as "red now, blue after 10 seconds" or "if humidity is above 40%, set the WS2812 red, otherwise blue" continue without keeping the LLM turn open.

Automation has two execution paths. Condition-action rules are handled by one long-lived `rule_task`, which scans up to `ESPAGENT_AUTOMATION_MAX_RULES` active rules, respects each rule's `interval_s`, `cooldown_s`, and hysteresis, then runs Sensor/Control Mesh calls serially. Ordered/delayed workflows do not run inside `rule_task`: every accepted workflow starts a temporary `workflow_task`, executes up to `ESPAGENT_AUTOMATION_WORKFLOW_MAX_STEPS` steps in order, then marks itself complete. Current limits are 8 rules, 8 workflow slots, and 8 steps per workflow. Rules are persisted across reboot; workflow tasks are currently one-shot runtime work and are not restored after reboot.

## esp-claw-Inspired Runtime Direction

ESPAgent adopts the useful engineering ideas from esp-claw without replacing the project identity or bypassing ESPAgent's security model. The migrated layer is a managed single-board runtime: capability descriptors, role-visible capability profiles, event/trace records, Memory v2, dynamic extension discovery, and a bounded Lua runtime. Lua is linked through `georgik/lua` and exposed through AI-callable tools such as `lua_runtime_info`, `lua_list_modules`, `lua_list_scripts`, `lua_run_source`, `lua_run_script`, and async job management tools.

Lua scripts run as system-level extensions, but they do not get raw unrestricted hardware authority. Scripts use `require("espagent")` or the global `espagent` table and call `espagent.call_capability(name, args_json)`. That call enters the same ESPAgent capability/tool path used by Serial CLI, LLM tools, Mesh, and automation, so sandbox checks, role policy, Guardian policy, Mesh validation, and Control local interlocks remain authoritative. Direct esp-claw-style hardware modules such as raw GPIO, I2C, ADC, PWM, RMT, BLE, display, camera, or audio are intentionally not linked unless they can be wrapped behind ESPAgent capability/manifest policy.

Current verification status: the latest Lua runtime build fits the 2 MB app slot with about 22% free space, `/dev/ttyUSB0-3` verify as Coordinator/Sensor/Control/Guardian, USB0 Lua smoke passes 7/7 after SNTP sync, and the same Lua runtime info/source smoke passes 8/8 across all four S3 roles. This verifies the managed runtime layer on every role, but it does not imply all physical sensors, actuators, manifest primitives, or display UI bindings have been hardware-tested.

2026-07-05 update: USB0 Coordinator also completed board-side slash command
verification. `/help` produced a direct reply from the slash layer, and
`/control set status light blue` rewrote into a constrained control request,
called `mesh_send_command`, passed Guardian policy, triggered Control execution,
and returned a structured final reply through OutputMessage reinjection.

The next safe esp-claw-inspired improvements are:

- Board descriptors / board profiles for each ESP32-S3 role and ESP32-P4 display terminal, including pins, sensors, actuators, safe ranges, risky actions, and validation status.
- Lua package layout under `/spiffs/scripts/builtin/`, `/spiffs/scripts/user/`, and `/spiffs/skills/<skill>/scripts/`, with source, version, risk, and confirmation metadata.
- Developer tooling that can generate a Lua script, manifest primitive, capability schema, skill document, and benchmark case together.
- Capability lifecycle fields such as `init`, `start`, `health_check`, `stop`, and `benchmark`, so tools are not only registered but also observable.
- P4/Android debug surfaces for script jobs, capability lists, Guardian decisions, benchmark results, and timeline traces.

The project should not become a direct esp-claw copy. ESPAgent's differentiator remains multi-ESP32 Agent Mesh coordination, Guardian permission management, structured OutputMessage/ReAct closure, MQTT Mesh communication, P4/Android reasoning visualization, and Lua/manifest as a safe extension layer.

For four ESP32-S3 boards, use the same firmware and assign different node profiles in `espagent_secrets.h`: `coordinator_agent`, `sensor_agent`, `control_agent`, and `guardian_agent`. ESP32-P4/Android carry the display-terminal role. See `docs/ESP32_ROLE_PROFILES.md`.

---

## Repository Structure Rule

The source tree is intentionally kept as one ESP-IDF firmware project. Role
specialization happens through node profiles and role-gated startup, not through
four separate firmware directories.

Use these placement boundaries:

- `main/`: firmware source compiled by `main/CMakeLists.txt`.
- `spiffs_data/`: files bundled into the SPIFFS partition at flash time.
- `tools/`: host-side flashing, verification, pressure test, and benchmark tools.
- `benchmarks/`: benchmark datasets and expected behavior cases.
- `docs/`: architecture, setup, handoff, roadmap, and current-state documents.
- `artifacts/`, `build*/`, `tmp*/`: generated output, ignored by git.

The detailed structure contract is in `docs/PROJECT_STRUCTURE.md`.

Current structural debt: `main/sensors/sensor_mqtt.c` still mixes general MQTT
Mesh transport, command routing, telemetry publishing, policy decision cache,
OutputMessage waiting, and display-facing timeline topics. The next source-level
architecture split should move generic MQTT/Mesh code into `main/mesh/` and keep
only sensor sampling/telemetry in `main/sensors/`.

---

## Module Map

```
main/
├── espagent.c                  Entry point — app_main() banner + startup phases
├── espagent_config.h           All compile-time constants + build-time secrets include
├── espagent_secrets.h          Build-time credentials (gitignored, highest priority)
├── espagent_secrets.h.example  Template for espagent_secrets.h
│
├── app/
│   ├── espagent_app.h      Application orchestration API
│   └── espagent_app.c      Subsystem init, local services, WiFi/onboarding, network services
│
├── bus/
│   ├── message_bus.h       espagent_msg_t struct, queue API
│   └── message_bus.c       Two FreeRTOS queues: inbound + outbound
│
├── wifi/
│   ├── wifi_manager.h      WiFi STA lifecycle API
│   └── wifi_manager.c      Event handler, exponential backoff
│
├── channels/
│   └── feishu/
│       ├── feishu_bot.h    Bot init/start, send_message API
│       └── feishu_bot.c    WebSocket receive loop, JSON parsing, replies
│
├── llm/
│   ├── llm_proxy.h         llm_chat() + llm_chat_tools() API, tool_use types
│   └── llm_proxy.c         Anthropic/OpenAI-compatible chat APIs, tool_use parsing
│
├── agent/
│   ├── agent_loop.h        Agent task init/start
│   ├── agent_loop.c        ReAct loop: LLM call → tool execution → repeat
│   ├── context_builder.h   System prompt + messages builder API
│   └── context_builder.c   Reads bootstrap files + memory + tool guidance
│
├── automation/
│   ├── automation_engine.h Persistent workflow/rule runtime API
│   └── automation_engine.c FreeRTOS automation task, rule persistence, Mesh execution
│
├── tools/
│   ├── tool_registry.h     Tool definition struct, register/dispatch API
│   ├── tool_registry.c     Tool registration, JSON schema builder, dispatch by name
│   ├── tool_subagent.h     spawn_subagent tool API
│   ├── tool_subagent.c     Temporary bounded subagent FreeRTOS task + short ReAct loop
│   ├── tool_web_search.h   Web search tool API
│   ├── tool_web_search.c   Tavily/Brave Search API via HTTPS
│   ├── tool_amap_weather.c Amap geocode + structured weather tool
│   ├── tool_gpio.c         GPIO / WS2812 status light tools
│   ├── tool_servo.c        Servo control tool
│   ├── tool_environment.c  Combined environment sensor tool
│   └── tool_*.c            Sensor, file, cron, audio, and time tools
│
├── memory/
│   ├── memory_store.h      Long-term + daily memory API
│   ├── memory_store.c      MEMORY.md read/write, daily .md append/read
│   ├── session_mgr.h       Per-chat session API
│   └── session_mgr.c       JSONL session files, ring buffer history
│
├── gateway/
│   ├── ws_server.h         WebSocket server API
│   └── ws_server.c         ESP HTTP server with WS upgrade, client tracking
│
├── proxy/
│   ├── http_proxy.h        Proxy connection API
│   └── http_proxy.c        HTTP CONNECT tunnel + TLS via esp_tls
│
├── cache/
│   ├── cache_store.h       Runtime KV cache API
│   └── cache_store.c       Prompt and skill summary cache
│
├── skills/
│   ├── skill_loader.h      SPIFFS skill summary API
│   └── skill_loader.c      Load markdown skills into prompt summaries
│
├── cron/
│   ├── cron_service.h      Scheduled trigger API
│   └── cron_service.c      FreeRTOS timer backed agent triggers
│
├── proactive/
│   ├── proactive_service.h Periodic self-check API
│   └── proactive_service.c NVS target storage + proactive agent injection
│
├── heartbeat/
│   ├── heartbeat.h         Heartbeat service API
│   └── heartbeat.c         Periodic background checks
│
├── drivers/
│   ├── sgp30.c             Air-quality sensor driver
│   ├── aht10.c             Temperature/humidity sensor driver
│   ├── bh1750.c            Light sensor driver
│   └── max98357.c          I2S audio output driver
│
├── espnow/
│   ├── espnow_sender.h     ESP-NOW telemetry API
│   └── espnow_sender.c     Peer setup and payload send path
│
├── mesh/
│   ├── mesh_types.h        Mesh command and protocol data types
│   ├── mesh_protocol.h     Topic builder and command parser API
│   └── mesh_protocol.c     MQTT command JSON validation and target checks
│
├── roles/
│   ├── role_config.h       Role/capability service-gating API
│   ├── role_config.c       Coordinator/sensor/control/guardian/display runtime policy
│   ├── coordinator_node.c  Coordinator role boundary
│   ├── sensor_node.c       Sensor role boundary
│   ├── control_node.c      Control role boundary
│   └── display_node.c      Display role boundary
│
├── sensors/
│   ├── sensor_mqtt.h       Sensor publishing API
│   └── sensor_mqtt.c       MQTT state/event/telemetry publishing, Mesh command validation, and sensor result publishing
│
├── onboard/
│   ├── wifi_onboard.h      Local setup/admin AP API
│   ├── wifi_onboard.c      Captive portal and admin hotspot
│   └── onboard_html.h      Embedded setup page HTML
│
├── cli/
│   ├── serial_cli.h        CLI init API
│   └── serial_cli.c        esp_console REPL with debug/maintenance commands
│
└── ota/
    ├── ota_manager.h       OTA update API
    └── ota_manager.c       HTTPS-only esp_https_ota wrapper + partition info
```

---

## FreeRTOS Task Layout

| Task               | Core | Priority | Stack  | Description                          |
|--------------------|------|----------|--------|--------------------------------------|
| `feishu_ws`        | 0    | 5        | 12 KB  | Feishu WebSocket receive loop        |
| `agent_loop`       | 1    | 6        | 12 KB  | Message processing + LLM API call    |
| `subagent`         | 1    | 5        | 12 KB  | Temporary bounded subagent ReAct loop |
| `automation`       | 0    | 4        | 12 KB  | Persistent workflow/rule polling and Mesh actions |
| `outbound`         | 0    | 5        | 8 KB   | Route responses to Feishu / WS     |
| `proactive`        | any  | 4        | 5 KB   | Periodic proactive agent checks      |
| `cron`             | any  | 4        | 5 KB   | Scheduled job polling and injection  |
| `serial_cli`       | 0    | 3        | 4 KB   | USB serial console REPL              |
| httpd (internal)   | 0    | 5        | —      | WebSocket server (esp_http_server)   |
| wifi_event (IDF)   | 0    | 8        | —      | WiFi event handling (ESP-IDF)        |

**Core allocation strategy**: Core 0 handles I/O (network, serial, WiFi). Core 1 is dedicated to the agent loop (CPU-bound JSON building + waiting on HTTPS).

---

## Memory Budget

| Purpose                            | Location       | Size     |
|------------------------------------|----------------|----------|
| FreeRTOS task stacks               | Internal SRAM  | ~40 KB   |
| WiFi buffers                       | Internal SRAM  | ~30 KB   |
| TLS connections x2 (Feishu + LLM/search) | PSRAM  | ~120 KB  |
| JSON parse buffers                 | PSRAM          | ~32 KB   |
| Session history cache              | PSRAM          | ~32 KB   |
| System prompt buffer               | PSRAM          | ~16 KB   |
| Subagent prompt/tool buffers        | PSRAM          | ~24 KB per active `spawn_subagent` |
| LLM response stream buffer         | PSRAM          | ~32 KB   |
| Remaining available                | PSRAM          | ~7.7 MB  |

Large buffers (32 KB+) are allocated from PSRAM via `heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM)`.

---

## Flash Partition Layout

```
Offset      Size      Name        Purpose
─────────────────────────────────────────────
0x009000    24 KB     nvs         ESP-IDF internal use (WiFi calibration etc.)
0x00F000     8 KB     otadata     OTA boot state
0x011000     4 KB     phy_init    WiFi PHY calibration
0x020000     2 MB     ota_0       Firmware slot A
0x220000     2 MB     ota_1       Firmware slot B
0x420000     4 MB     spiffs      Markdown memory, sessions, config
0x820000    64 KB     coredump    Crash dump storage

The remaining flash space is intentionally left unallocated for future data
partitions or growth, while keeping the default role firmware burn path much
faster than a 12 MB SPIFFS image.
```

Total: 16 MB flash.

Current OTA behavior:

- `main/ota/ota_manager.c` uses ESP-IDF `esp_https_ota` with the bundled root CA store.
- `ota_update` rejects empty URLs and non-HTTPS URLs.
- The target URL must be an app image such as `ESPAgent.bin`, not a full flash image.
- The current trigger surface is Serial CLI only: `ota_info` and `ota_update <HTTPS_BIN_URL>`.
- The Agent does not write firmware source code or compile firmware on the MCU. OTA is an operations path: a developer or CI prepares `ESPAgent.bin`, then the device downloads and installs it.
- Future Agent-driven OTA should be treated as orchestration only: version discovery, role matching, Guardian approval, human confirmation, command dispatch, reboot observation, and health reporting.
- Four S3 roles currently use the same firmware image with build-time profile in `espagent_secrets.h`; OTAing a generic image can change that role profile unless the uploaded `.bin` was built for the intended role. A future improvement is to move role identity to NVS so one app image can update all roles safely.

---

## Storage Layout (SPIFFS)

SPIFFS is a flat filesystem — no real directories. Files use path-like names.

```
/spiffs/config/SOUL.md          AI personality definition
/spiffs/config/USER.md          User profile
/spiffs/memory/MEMORY.md        Long-term persistent memory
/spiffs/memory/2026-02-05.md    Daily notes (one file per day)
/spiffs/sessions/session_<fnv64>.jsonl Session history (hash of channel chat id)
/spiffs/cron.json               Persistent scheduled jobs
/spiffs/automation.json         Persistent automation workflows/rules
```

Session files are JSONL (one JSON object per line):
```json
{"role":"user","content":"Hello","ts":1738764800}
{"role":"assistant","content":"Hi there!","ts":1738764802}
```

New writes use a stable FNV-1a 64-bit hash of `chat_id` in the filename. Safe legacy names such as `session_12345.jsonl` are still read and removed by `session_clear` for migration compatibility.

---

## Configuration

Configuration uses build-time defaults from `espagent_secrets.h`, with selected runtime overrides stored in NVS through the serial CLI.

| Define                       | Description                             |
|------------------------------|-----------------------------------------|
| `ESPAGENT_SECRET_WIFI_SSID`     | WiFi SSID                               |
| `ESPAGENT_SECRET_WIFI_PASS`     | WiFi password                           |
| `ESPAGENT_SECRET_FEISHU_APP_ID`      | Feishu App ID                         |
| `ESPAGENT_SECRET_FEISHU_APP_SECRET`  | Feishu App Secret                     |
| `ESPAGENT_SECRET_API_KEY`       | LLM provider API key                    |
| `ESPAGENT_SECRET_MODEL`         | Model ID (default: claude-opus-4-6)     |
| `ESPAGENT_SECRET_PROXY_HOST`    | HTTP proxy hostname/IP (optional)       |
| `ESPAGENT_SECRET_PROXY_PORT`    | HTTP proxy port (optional)              |
| `ESPAGENT_SECRET_SEARCH_KEY`    | Brave Search API key (optional)         |
| `ESPAGENT_SECRET_TAVILY_KEY`    | Tavily Search API key (optional)        |

NVS is also used for runtime overrides such as Wi-Fi, Feishu credentials, LLM settings, proxy settings, and search API keys.

---

## Message Bus Protocol

The internal message bus uses two FreeRTOS queues carrying `espagent_msg_t`:

```c
typedef struct {
    char channel[16];   // "feishu", "websocket", "cli", "system"
    char chat_id[96];   // Feishu chat_id/open_id, or WS client id
    uint32_t flags;     // ESPAGENT_MSG_FLAG_*
    char *content;      // Heap-allocated text (ownership transferred)
} espagent_msg_t;
```

- **Inbound queue**: channels → agent loop (depth: 8)
- **Outbound queue**: agent loop → dispatch → channels (depth: 8)
- Content string ownership is transferred on push; receiver must `free()`.
- `ESPAGENT_MSG_FLAG_PROACTIVE` marks internally generated proactive checks. The agent loop suppresses the "working" status for these turns and drops the final outbound message if the model returns `PROACTIVE_NO_MESSAGE`.

---

## WebSocket Protocol

Port: **18789**. Max clients: **4**.

**Client → Server:**
```json
{"type": "message", "content": "Hello", "chat_id": "ws_client1"}
```

**Server → Client:**
```json
{"type": "response", "content": "Hi there!", "chat_id": "ws_client1"}
```

Client `chat_id` is auto-assigned on connection (`ws_<fd>`) but can be overridden in the first message.

---

## LLM API Integration

Primary Anthropic endpoint: `POST https://api.anthropic.com/v1/messages`

OpenAI-compatible providers are normalized inside `llm_proxy.c`; the agent loop consumes the same `llm_response_t` regardless of provider.

Request format (Anthropic-native, non-streaming, with tools):
```json
{
  "model": "claude-opus-4-6",
  "max_tokens": 4096,
  "system": "<system prompt>",
  "tools": [
    {
      "name": "web_search",
      "description": "Search the web for current information.",
      "input_schema": {"type": "object", "properties": {"query": {"type": "string"}}, "required": ["query"]}
    }
  ],
  "messages": [
    {"role": "user", "content": "Hello"},
    {"role": "assistant", "content": "Hi!"},
    {"role": "user", "content": "What's the weather today?"}
  ]
}
```

Key difference from OpenAI: `system` is a top-level field, not inside the `messages` array.

Non-streaming JSON response:
```json
{
  "id": "msg_xxx",
  "type": "message",
  "role": "assistant",
  "content": [
    {"type": "text", "text": "Let me search for that."},
    {"type": "tool_use", "id": "toolu_xxx", "name": "web_search", "input": {"query": "weather today"}}
  ],
  "stop_reason": "tool_use"
}
```

When `stop_reason` is `"tool_use"`, the agent loop executes each tool and sends results back:
```json
{"role": "assistant", "content": [<text + tool_use blocks>]}
{"role": "user", "content": [{"type": "tool_result", "tool_use_id": "toolu_xxx", "content": "..."}]}
```

The loop repeats until `stop_reason` is `"end_turn"` (max 10 iterations).

### Subagent Tool

`spawn_subagent` is registered as a normal LLM-callable tool, but its implementation creates a temporary FreeRTOS task:

```text
main agent tool_use
  -> tool_subagent_execute()
  -> xTaskCreatePinnedToCore("subagent")
  -> subagent calls llm_chat_tools() with a filtered tools JSON
  -> subagent executes allowed tool calls
  -> xSemaphoreGive(done_sem)
  -> main agent receives a concise tool result
```

This was implemented on the current `main` branch by referencing the remote `subagent` branch pattern, without merging that branch or replacing the current Agent Mesh code.

Current boundaries:

- Allowed subagent tools: `web_search`, `get_weather`, `get_current_time`, `read_file`, `write_file`, `edit_file`, `list_dir`.
- Blocked subagent tools: hardware, sensor, GPIO, RGB, servo, MQTT Mesh command, and nested `spawn_subagent`.
- The main caller waits synchronously up to `ESPAGENT_SUBAGENT_TIMEOUT_MS`.
- Future Mesh direction: return an async task id, publish progress/results to `message_bus` and `espagent/agent/timeline`, then let the Coordinator summarize the result later.

---

## Startup Sequence

```
app_main()
  ├── init_nvs()                    NVS flash init (erase if corrupted)
  ├── esp_event_loop_create_default()
  ├── init_spiffs()                 Mount SPIFFS at /spiffs
  ├── message_bus_init()            Create inbound + outbound queues
  ├── memory_store_init()           Verify SPIFFS paths
  ├── cache_store_init()
  ├── skill_loader_init()
  ├── session_mgr_init()
  ├── wifi_manager_init()           Init WiFi STA mode + event handlers
  ├── http_proxy_init()             Load proxy config from build-time secrets
  ├── [if coordinator/communication]
  │   └── feishu_bot_init()         Load Feishu app credentials from build-time secrets
  ├── [if coordinator/llm]
  │   └── llm_proxy_init()          Load API key + model from build-time secrets
  ├── tool_registry_init()          Register tools, build tools JSON
  ├── automation_engine_init()      Load /spiffs/automation.json rules
  ├── [if coordinator/scheduler]
  │   ├── cron_service_init()
  │   ├── heartbeat_init()
  │   └── proactive_service_init()
  ├── [if coordinator/llm]
  │   └── agent_loop_init()
  ├── coordinator_node_init()
  ├── sensor_node_init()
  ├── control_node_init()
  ├── display_node_init()
  ├── serial_cli_init()             Start REPL (works without WiFi)
  ├── [if control]
  │   └── boot_servo task
  ├── [if sensor]
  │   ├── env_mon task
  │   ├── presence_mon task
  │   └── SGP30 monitor
  │
  ├── wifi_manager_start()          Connect using build-time credentials
  │   └── wifi_manager_wait_connected(30s)
  │
  └── [if WiFi connected]
      ├── outbound_dispatch task    Launch outbound task (Core 0)
      ├── coordinator_node_start()
      ├── sensor_node_start()
      ├── control_node_start()
      ├── display_node_start()
      ├── [if coordinator/llm] agent_loop_start()
      ├── automation_engine_start()
      ├── [if coordinator/communication] feishu_bot_start()
      ├── sensor_mqtt_start()       Mesh MQTT state/event/telemetry for all roles
      ├── [if coordinator/scheduler]
      │   ├── cron_service_start()
      │   ├── heartbeat_start()
      │   └── proactive_service_start()
      └── [if coordinator/communication] ws_server_start()
```

If WiFi credentials are missing or connection times out, the CLI remains available for diagnostics.

---

## Serial CLI Commands

The CLI provides debug and maintenance commands only. All configuration is done via `espagent_secrets.h`.

| Command                        | Description                          |
|--------------------------------|--------------------------------------|
| `wifi_status`                  | Show connection status and IP        |
| `memory_read`                  | Print MEMORY.md contents             |
| `memory_write <CONTENT>`       | Overwrite MEMORY.md                  |
| `session_list`                 | List all session files               |
| `session_clear <CHAT_ID>`      | Delete a session file                |
| `heap_info`                    | Show internal + PSRAM free bytes     |
| `ota_info`                     | Show running, boot, and next OTA partitions |
| `ota_update <HTTPS_BIN_URL>`   | Download HTTPS app `.bin`, write inactive OTA slot, reboot on success |
| `restart`                      | Reboot the device                    |
| `help`                         | List all available commands           |

OTA is currently a local maintenance capability only. It is not registered as an LLM tool and should not be exposed through Feishu until it is gated by Guardian policy, explicit human confirmation, image provenance checks, and role/profile handling.

The intended higher-level OTA flow is:

```text
developer/CI builds ESPAgent.bin
        |
        v
publish firmware + manifest
        |
        v
Coordinator compares node versions and roles
        |
        v
Guardian checks source, role, version, and risk
        |
        v
user confirms
        |
        v
target ESP32 downloads and applies OTA
        |
        v
node reboots, reports version and health
```

---

## ESPAgent Module Responsibilities

| Area | Modules | Responsibility |
|------|---------|----------------|
| Boot and lifecycle | `espagent.c`, `espagent_config.h` | Initialize storage, Wi-Fi, channels, agent services, hardware monitors, and CLI |
| Message routing | `bus/`, `channels/feishu/`, `gateway/` | Normalize Feishu/WebSocket/system messages into queues and route outbound replies |
| Agent runtime | `agent/`, `llm/`, `tools/` | Build prompts, call the LLM, parse tool-use blocks, execute tools, and return final text |
| Persistence | `memory/`, `cache/`, `skills/`, `spiffs_data/` | Store sessions, long-term memory, daily notes, skill summaries, and cached prompt fragments |
| Hardware access | `drivers/`, `tools/`, `sensors/`, `espnow/` | Keep sensor/peripheral I/O bounded and expose narrow AI-callable tools |
| Operations | `cli/`, `onboard/`, `ota/`, `proxy/` | Support local setup, diagnostics, OTA updates, and proxied HTTPS access |

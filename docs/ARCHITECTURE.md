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
│   │  SPIFFS (12 MB)                          │    │
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
| `espagent/roles/<role>/command` | subscribe | Validates role-targeted Mesh commands for coordinator/sensor/control/display groups |
| `espagent/agent/dispatch` | subscribe/publish | Coordinator publishes Feishu inbound dispatch events; nodes can observe dispatch messages |
| `espagent/alerts` | subscribe | Logs alert messages |
| `espagent/agent/timeline` | publish/subscribe | Receives Feishu inbound/outbound timeline events and sensor `mesh_command_result` events |

This is intentionally not a full distributed multi-agent runtime yet. Feishu, WebSocket, cron, proactive checks, and future injected messages still converge on the same message bus and the same serial `agent_loop`. `mesh_send_command` can publish a standard MQTT Mesh command from the Coordinator to a target node or role. Sensor role currently supports the whitelisted `read_temperature_humidity` command and publishes `mesh_command_result`; control-role hardware execution remains disabled until command queue, authorization, safety interlock, audit, and result correlation are implemented.

For four ESP32-S3 boards, use the same firmware and assign different node profiles in `espagent_secrets.h`: `coordinator_agent`, `sensor_agent`, `control_agent`, and `display_agent`. See `docs/ESP32_ROLE_PROFILES.md`.

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
│   ├── role_config.c       Coordinator/sensor/control/display runtime policy
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
    └── ota_manager.c       esp_https_ota wrapper
```

---

## FreeRTOS Task Layout

| Task               | Core | Priority | Stack  | Description                          |
|--------------------|------|----------|--------|--------------------------------------|
| `feishu_ws`        | 0    | 5        | 12 KB  | Feishu WebSocket receive loop        |
| `agent_loop`       | 1    | 6        | 12 KB  | Message processing + LLM API call    |
| `subagent`         | 1    | 5        | 12 KB  | Temporary bounded subagent ReAct loop |
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
0x420000    12 MB     spiffs      Markdown memory, sessions, config
0xFF0000    64 KB     coredump    Crash dump storage
```

Total: 16 MB flash.

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
| `restart`                      | Reboot the device                    |
| `help`                         | List all available commands           |

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

# ESPAgent Roadmap

> ESPAgent is tracked as its own ESP32-S3 firmware project. This roadmap lists native ESPAgent capabilities, gaps, and implementation priorities without tying the project identity to any external codebase.
> Priority: P0 = core missing, P1 = important enhancement, P2 = nice to have.

---

## P0 — Core Agent Capabilities

### [x] ~~Tool Use Loop (multi-turn agent iteration)~~
- Implemented: `agent_loop.c` ReAct loop with `llm_chat_tools()`, max 10 iterations, non-streaming JSON parsing.

### [ ] Memory Write via Tool Use (agent-driven memory persistence)
- **Target**: The agent can persist important information into `MEMORY.md` and daily memory files through explicit tool calls.
- **Current**: `memory_write_long_term` and `memory_append_today` exist but are only called from CLI.
- **Scope**: Expose memory write/append tools through `tool_registry.c`; add prompt guidance on when to persist memory; optionally trigger memory flush when session history approaches `ESPAGENT_SESSION_MAX_MSGS`.
- **Depends on**: Tool Use Loop.

### [x] ~~Tool Registry + web_search Tool~~
- Implemented: `tools/tool_registry.c` — tool registration, JSON schema builder, dispatch by name.
- Implemented: `tools/tool_web_search.c` — Tavily/Brave Search API via HTTPS with direct and proxy modes.

### [ ] More Built-in Tools
- **Target**: Add a practical SPIFFS-oriented tool set for embedded use: `read_file`, `write_file`, `list_dir`, `message`, and `memory_write`.
- **Current**: Core registry exists; file tools are present but should be reviewed for prompt guidance and safety boundaries.
- **Recommendation**: Keep each tool narrow, bounded, and explicit about writable paths.

### [ ] Background Work / Long-running Tasks
- **Target**: Support background jobs that can run outside the immediate chat turn and inject results back into the message bus.
- **Current**: Dedicated cron, heartbeat, proactive, sensor monitor, environment monitor, automation engine, and a bounded `spawn_subagent` worker pattern exist. `cron_add` supports recurring, one-shot, and daily jobs; `proactive_service` can periodically inject an internal self-check. `automation_create_workflow` runs deterministic ordered/delayed Mesh actions in temporary `workflow_task` workers, and `automation_create_rule` persists condition-action rules in `/spiffs/automation.json` for one FreeRTOS `rule_task` to poll Sensor data and trigger Control actions. The current automation limits are 8 rules, 8 workflow slots, and 8 steps per workflow. `spawn_subagent` can run one focused research/file/time/weather subtask in a temporary FreeRTOS task, but it is synchronous from the caller's perspective and is not a generic arbitrary background job runner.
- **Recommendation**: Use one bounded FreeRTOS worker pattern before adding broader scheduling abstractions.

### [x] ~~Bounded Subagent Tool~~
- Implemented: `tools/tool_subagent.c` registers `spawn_subagent`, creates a temporary FreeRTOS task, runs a short independent ReAct loop, and returns the result through a semaphore.
- Implemented: subagent tool exposure is filtered to search/weather/time/SPIFFS file tools, with execution-side whitelist enforcement.
- Remaining enhancement: convert long-running subagent work to async `task_id` plus `message_bus`/timeline result injection.

---

## P1 — Important Features

### [ ] Feishu User Allowlist
- **Target**: Restrict which Feishu users/chats may consume the agent and API credits.
- **Current**: No allowlist check in the Feishu inbound path.
- **Recommendation**: Store the allowlist in `espagent_secrets.h` or NVS, then filter before pushing messages into the inbound queue.

### [ ] Feishu Markdown to HTML Conversion
- **Target**: Send richer formatted Feishu replies without parse failures.
- **Current**: Reply formatting is intentionally simple; special characters can still require plain-text fallback.
- **Recommendation**: Implement a small Markdown-to-Feishu converter for code blocks, inline code, bold, lists, and links.

### [ ] Feishu /start Command
- **Target**: Handle `/start` locally with a short ESPAgent welcome/status message.
- **Current**: `/start` is treated as normal user text.
- **Recommendation**: Intercept it in `channels/feishu/feishu_bot.c` before enqueueing to the agent loop.

### [ ] Feishu Media Handling (photos/voice/files)
- **Target**: Handle image, voice, audio, and document messages.
- **Current**: Text messages are supported; media payloads are ignored.
- **Recommendation**: Start with image download and base64 forwarding to a vision-capable LLM; add voice transcription only after the HTTPS memory impact is measured.

### [ ] Skills System Expansion
- **Target**: SPIFFS skills can be always-loaded or loaded on demand, with concise metadata.
- **Current**: `skill_loader.c` builds a prompt summary from Markdown skill files.
- **Recommendation**: Add frontmatter parsing only if the current summary becomes too noisy.

### [ ] Bootstrap File Completion
- **Target**: Keep behavior guidelines, user preferences, tool notes, and identity data as explicit SPIFFS bootstrap files.
- **Current**: `SOUL.md` and `USER.md` are loaded; skills and memory are summarized separately.
- **Recommendation**: Add only the files that materially improve prompt quality, then cache their summaries.

### [ ] Longer Memory Lookback
- **Target**: Make daily memory lookback configurable.
- **Current**: `context_builder.c` uses a short lookback to control token and RAM pressure.
- **Recommendation**: Add a compile-time default and optional runtime setting, with a hard maximum.

### [x] ~~System Prompt Tool Guidance~~
- Implemented: `context_builder.c` includes tool usage guidance in the system prompt.

### [ ] Message Metadata
- **Target**: Carry media references, reply targets, and channel metadata through `espagent_msg_t`.
- **Current**: `espagent_msg_t` contains `channel`, `chat_id`, and `content`.
- **Recommendation**: Extend the struct carefully and audit every producer/consumer before changing ownership rules.

### [ ] Outbound Channel Registration
- **Target**: Let channels register outbound handlers instead of hardcoding every route in `espagent.c`.
- **Current**: Outbound dispatch uses a small if/else chain for Feishu, WebSocket, and system messages.
- **Recommendation**: Current code is simple and acceptable; defer until more channels exist.

---

## P2 — Advanced Features

### [x] ~~Cron Scheduled Task Service~~
- Implemented: `cron_service.c` persists jobs in `/spiffs/cron.json` and injects scheduled messages into the agent loop.
- Implemented: `cron_add` supports `schedule_type="every"`, `schedule_type="at"`, and `schedule_type="daily"` with local hour/minute.
- Remaining enhancement: full cron syntax is intentionally deferred; current simple schedule types are easier to validate on an MCU.

### [ ] Heartbeat Service
- **Target**: Periodically inspect heartbeat instructions and trigger the agent when work is pending.
- **Current**: Heartbeat service exists.
- **Recommendation**: Add better observability and storage guards before increasing frequency.

### [ ] Multi-LLM Provider Support
- **Target**: Support OpenAI-compatible APIs and a small provider selection layer.
- **Current**: `llm_proxy.c` supports Anthropic-style and OpenAI-compatible tool-use parsing, but provider selection/configuration remains intentionally small.
- **Recommendation**: Keep the current normalization layer until more provider-specific behavior is needed.

### [ ] Voice Transcription
- **Target**: Convert Feishu voice messages into text before entering the agent loop.
- **Current**: Not implemented.
- **Recommendation**: Measure heap/TLS impact for download plus transcription API before adding it to default builds.

### [x] ~~Build-time Config File + Runtime NVS Override~~
- Implemented: `espagent_secrets.h` as build-time defaults, NVS as runtime override via CLI.
- Two-layer config: build-time secrets -> NVS fallback, CLI commands to set/show/reset.

### [ ] WebSocket Gateway Protocol Enhancement
- **Target**: Add richer client protocol events such as streaming tokens and structured tool status.
- **Current**: Basic JSON request/reply protocol on port `18789`.
- **Recommendation**: Add versioned message types before introducing streaming.

### [ ] LingShu Agent Mesh Coordinator
- **Target**: Expand the current ESP32-S3 Edge Agent Node into a multi-node system with Coordinator Agent, Sensor Agent, Control Agent, Memory Agent, Communication Agent, and Display Agent.
- **Current**: Phase 1.7 is implemented and board-verified in firmware: node identity and role profile exist; MQTT publishes state/telemetry/events under `espagent/nodes/<node_id>/...`; node/role command topics are subscribed; `main/mesh` parses and validates command JSON; `main/roles` provides coordinator/sensor/control/guardian service boundaries; `espagent_app` starts LLM/chat, scheduler, sensor monitors, control outputs, and guardian/display boundaries according to role/capability. `mesh_send_command` is registered as an LLM-callable Coordinator tool, publishes Guardian `policy_check`, defaults to async `async_task_id`, waits for structured `espagent.output.v1` in a background task, and injects the result back through `message_bus`. `sensor_agent` and `control_agent` can execute their whitelisted Mesh commands; Control locally verifies cached Guardian allow decisions before actuator execution. Local tools, remote Mesh results, and final replies now publish structured OutputMessage; session trace JSONL and Guardian StateBoard exist.
- **Recommendation**: Keep high-risk hardware execution disabled until command queue, authorization/signature checks, human confirmation, richer audit events, safety interlock, and message_bus/tool_guard routing are implemented. Next priority is trace/stateboard queryability and watchdog aggregation for ESP32-P4/Android display.

### [ ] MCU Edge AI / TinyML Path
- **Target**: Add optional local inference for small, bounded tasks such as sensor anomaly detection, wake word/command spotting, or low-dimensional classification.
- **Current**: ESP32-S3 runs the agent runtime, tools, Mesh, cache, memory, and deterministic edge logic; full LLM reasoning still runs through remote APIs. Runtime skill `mcu-edge-ai-boundaries.md` documents this boundary so the agent does not overclaim local LLM capability.
- **Recommendation**: Treat TinyML / LiteRT Micro / ESP-DL / ESP-SR as future integration paths. Do not expose a local inference tool until model size, operators, RAM/PSRAM budget, latency, driver input path, and validation data are defined.

### [ ] Multi-Channel Manager
- **Target**: Centralize channel lifecycle if Feishu, WebSocket, serial, and future channels need common management.
- **Current**: Startup is orchestrated directly in `app_main()`.
- **Recommendation**: Defer until lifecycle duplication becomes real.

### [ ] Additional IM Channels
- **Target**: Add more user-facing channels only when there is a clear deployment need.
- **Current**: Feishu and WebSocket are the supported chat paths.
- **Recommendation**: Keep channel implementations isolated under `main/channels/`.

### [x] ~~Feishu Proxy Support (HTTP CONNECT)~~
- Implemented: HTTP CONNECT tunnel via `proxy/http_proxy.c`, configurable via `espagent_secrets.h` (`ESPAGENT_SECRET_PROXY_HOST`/`ESPAGENT_SECRET_PROXY_PORT`).

### [ ] Session Metadata Persistence
- **Target**: Store session metadata such as created/updated timestamps.
- **Current**: JSONL session files store role/content/ts records only. File names now use a stable FNV-1a hash of `chat_id`, with read/clear compatibility for safe legacy names.
- **Recommendation**: Low priority; add only if UI or maintenance tooling needs it.

---

## Completed ESPAgent Capabilities

- [x] Feishu WebSocket channel
- [x] Message Bus (inbound/outbound queues)
- [x] Agent Loop with ReAct tool use (multi-turn, max 10 iterations)
- [x] LLM API integration (Anthropic Messages API plus OpenAI-compatible tool-call parsing)
- [x] Tool Registry + web_search tool (Tavily/Brave Search API)
- [x] Structured Weather Tool (`get_weather` via Amap WebService, default Nanjing Qixia District)
- [x] Context Builder (system prompt + bootstrap files + memory + tool guidance)
- [x] Context Builder bounded append safety (prevents prompt offset overflow after truncation)
- [x] Memory Store (MEMORY.md + daily notes)
- [x] Session Manager (hashed JSONL file names, ring buffer history, legacy-safe read/clear)
- [x] Pending clarification carry-over (previous assistant question is injected as likely pending context)
- [x] Proactive Service (periodic internal checks, NVS target, `PROACTIVE_NO_MESSAGE` suppression)
- [x] Daily Cron Scheduling (hour/minute local-time jobs for reminders, weather, and check-ins)
- [x] LingShu Agent Mesh Phase 1 identity and MQTT observability topics
- [x] LingShu Agent Mesh Phase 1.5 role-gated startup and command dry-run validation
- [x] Mesh command publish tool (`mesh_send_command`) for Coordinator-to-node/role MQTT commands
- [x] Sensor-side whitelisted Mesh command result path for `read_temperature_humidity`
- [x] AHT20-backed Sensor telemetry path verified on USB1 (`27.x C / 45-46%RH`)
- [x] Automation workflow/rule tools (`automation_create_workflow`, `automation_create_rule`, `automation_list`, `automation_remove`)
- [x] Humidity condition automation verified from USB0 -> USB1 AHT20 -> Guardian -> USB2 WS2812
- [x] Bounded Subagent Tool (`spawn_subagent`) with filtered search/weather/time/file tool access
- [x] Four-ESP32 role profile configuration (`coordinator_agent`, `sensor_agent`, `control_agent`, `guardian_agent`)
- [x] WebSocket Gateway (port 18789, JSON protocol)
- [x] Serial CLI (esp_console, debug/maintenance commands)
- [x] HTTP CONNECT Proxy (Feishu + LLM API + search APIs via proxy tunnel)
- [x] OTA Update (Serial CLI HTTPS app-bin update; Agent orchestration planned)
- [x] WiFi Manager (build-time credentials, exponential backoff)
- [x] SPIFFS storage
- [x] Build-time config (`espagent_secrets.h`) + runtime NVS override via CLI

---

## Suggested Implementation Order

```text
1. [done] Tool Use Loop + Tool Registry + web_search
2. Memory Write via Tool Use
3. Built-in Tools (read_file, write_file, message)
4. Feishu Allowlist
5. Bootstrap File Completion
6. [partial] Background Work / Long-running Tasks
7. Feishu Markdown -> HTML
8. Media Handling
9. Heartbeat/proactive observability
10. [done] Mesh Phase 1.5 role-gated startup and command dry-run validation
11. [done] Coordinator `mesh_send_command` publish tool
12. [done] Sensor whitelisted `read_temperature_humidity` command result event
13. [done] Bounded `spawn_subagent` tool on current `main` architecture
14. Coordinator result wait/correlation by `command_id` and Feishu summary reply
15. [done] Flash and verify a real `sensor_agent` board with AHT20 over MQTT
16. [done] Add deterministic automation workflow/rule tools for delayed actions and temperature/humidity conditions
17. Natural-language automation pause/resume/remove, status board, conflict detection, workflow cancellation/recovery, and richer multi-condition rules
18. Mesh command queue + authorization + safety interlock + actuator state for `control_agent`
19. Async subagent task_id + `message_bus`/timeline result injection
20. Full timeline events for tool_use/tool_result/remote result
21. Other enhancements
```

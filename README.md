# ESPAgent

ESPAgent is an ESP32-S3 Agent Mesh firmware project built on ESP-IDF and FreeRTOS. It runs a lightweight LLM Agent runtime on MCU hardware, connects Feishu/Lark chat, local WebSocket access, Serial CLI, SPIFFS memory, ReAct-style tool calling, MQTT Mesh communication, and real hardware tools into one embedded multi-node control system.

The project is maintained as ESPAgent. Public documentation, runtime logs, generated binaries, and directory structure should use the ESPAgent name consistently.

The current implementation targets a four-ESP32-S3 Agent Mesh:

- `coordinator_agent`: Feishu/WebSocket entry, LLM ReAct loop, task dispatch, timeline, and user replies.
- `sensor_agent`: AHT10/AHT20, SGP30, BH1750/GY-30, presence, and environment telemetry.
- `control_agent`: WS2812, GPIO, servo, relay/actuator boundary, and whitelisted hardware execution.
- `guardian_agent`: `policy_check`, `policy_decision`, audit, privacy boundary, watchdog, and lightweight StateBoard.

ESP32-P4+C6 and Android are Display Terminals. They subscribe to Mesh state, telemetry, timeline, alerts, and structured results to visualize reasoning, communication, sensor data, and final user-facing outcomes. They are not the fourth ESP32-S3 role.

This is still an MCU-oriented runtime, not a Linux multi-process agent framework. The Coordinator currently owns the main serial `agent_loop`; cross-node collaboration is implemented through MQTT Mesh commands, Guardian policy gates, structured `OutputMessage`, timeline events, and background automation tasks.

## Capabilities

- ESP32-S3 firmware built with ESP-IDF 6.1
- Feishu/Lark WebSocket channel for chat input and replies
- Local WebSocket gateway on port `18789`
- Local Wi-Fi onboarding/admin portal with device status, device registry, and runtime skills API
- Serial CLI for diagnostics and local maintenance
- Slash command layer for direct diagnostics and explicit routing: `/help`,
  `/init`, `/doctor`, `/mcp`, `/compact`, `/skills_list`,
  `/skills_show`, `/sensor`, `/control`, `/guardian`, `/subagent`,
  `/workflow`, `/rule`, `/local`, `/mesh`, `/status`, `/stop`, `/resume`,
  `/device`, `/profile`, `/skills`, `/privacy`, `/lua`, and `/trace`
- ReAct-style agent loop with LLM tool use
- Bounded `spawn_subagent` tool for focused search, weather/time, and SPIFFS file subtasks
- esp-claw-like single-board runtime layer: capability registry, role-visible capability profiles, event/trace flow, Memory v2, dynamic extension catalog, and managed Lua scripting
- Managed Lua runtime linked through `georgik/lua`: `lua_runtime_info`, `lua_list_modules`, `lua_list_scripts`, `lua_run_source`, `lua_run_script`, `lua_run_script_async`, `lua_list_jobs`, `lua_get_job`, and `lua_stop_job`
- Lua scripts can use `require("espagent")` / global `espagent` to call existing capabilities through `espagent.call_capability(name, args_json)`; hardware access still passes through sandbox, role policy, Guardian/Mesh, and local interlocks
- MQTT Mesh command routing with node/role targets
- Guardian-gated `policy_check` / `policy_decision` before remote Sensor/Control execution
- Control Agent command queue with duplicate command rejection, TTL check, single-actuator interlock, emergency stop latch, and actuator state snapshot
- Optional MQTT Mesh command HMAC-SHA256 signing when `ESPAGENT_SECRET_MESH_AUTH_KEY` is configured
- Guardian parameter-level checks for virtual control manifests, including persistent relay confirmation and duration bounds
- Structured `espagent.output.v1` OutputMessage for Mesh results, local tool results, and final replies
- Async `mesh_send_command` result reinjection: `async_task_id` first, later OutputMessage injected back into `message_bus`
- Timeline events for `tool_use`, `tool_result`, `mesh_command_queued`, `mesh_command_result`, `final_reply`, `error`, and Guardian audit
- Session trace JSONL for tool and async-result history
- Serial `trace_show <chat_id> [max_events]` for querying recent ReAct/tool trace events
- Serial `trace_index` and `task_tree <chat_id> [max_events]` for persisted trace discovery and lightweight task aggregation
- Guardian manual approval queue with `approval_list`, `approval_confirm`, `approval_deny`, and one-shot `approval_id` consumption for high-risk control retries
- Lightweight Guardian StateBoard exposed through Serial CLI
- Runtime skill benchmark over `/dev/ttyUSB0-3` for all SPIFFS skill readability, Mesh routing, sandbox, privacy, prompt-injection, workflow, and heap/cache snapshots
- SPIFFS-backed memory, sessions, skills, and config files
- Structured weather lookup through Amap WebService, with Nanjing Qixia District as the default location when configured
- Hardware tools for GPIO, WS2812, servo, MAX98357, AHT10/AHT20, SGP30, BH1750/GY-30, HC-SR05, and environment readings
- Runtime skill guidance for protocol-manifest based hardware extension over I2C/IIC, SPI, UART, RS485/Modbus RTU, CAN/TWAI, GPIO, PWM, ADC, 1-Wire, I2S/PDM, RMT/IR, USB CDC, SDIO/SDMMC, and BLE GATT, with clear driver-vs-manifest boundaries
- `virtual_device_read` runtime extension: read-only I2C, UART query, Modbus RTU register-read, SPI transfer-read, ADC one-shot, and GPIO input manifests under `/spiffs/devices/*.json`
- `virtual_device_control` runtime extension: bounded GPIO output, relay, `pwm_output`, and `ledc_pwm` manifests with allowlist, cooldown, duration, background safe-state restore, Mesh Guardian policy, and Control Agent local verification
- Developer manifest toolchain: `schemas/device_manifest.schema.json` and `tools/manifest_lint.py --dry-run`, `--support-matrix`, `--init-template`, and `--write-signatures`
- Manifest trust check: `manifest_version=1`, `permissions`, role/risk consistency, and `<device>.json.sha256` sidecar verification; control manifests require a matching SHA-256 sidecar before execution
- ESP-NOW environment telemetry sender
- MQTT state, telemetry, events, dispatch, timeline, alerts, and security topics
- SPIFFS-backed `device_registry` for MQTT Mesh nodes
- Automation runtime for delayed workflows and one-shot condition-action rules
- Four ESP32-S3 role profiles: `coordinator_agent`, `sensor_agent`, `control_agent`, and `guardian_agent`
- Wi-Fi onboarding/admin AP under the `ESPAgent-XXXX` network name

Current verified highlights:

- USB0 `coordinator_agent` has recovered from a bootloader loop through a raw
  full-device `esptool` reflash; the board now boots normally and Feishu WS
  startup has been re-verified.
- Feishu entry can route common natural-language requests to Sensor or Control without requiring the user to name an MQTT node id.
- Slash commands have been board-verified on USB0: `/help` returns directly
  without LLM, and `/control set status light blue` rewrites into a constrained
  control request, then completes the Mesh -> Guardian -> Control ->
  OutputMessage -> final reply chain.
- AHT20 on the Sensor role has been verified, with typical readings around `27.x C / 45-46%RH`.
- Sensor telemetry publishes AHT20 data on `espagent/cube1345/nodes/esp32s3-sensor-01/telemetry`.
- Sensor telemetry now includes local EWMA fields (`temp_avg`, `humidity_avg`, `light_lux_avg`) and sample counts, and Sensor can publish threshold events to `events`, `alerts`, and `agent/timeline`.
- Sensor role now keeps bounded local environment history under `/spiffs/env/*.jsonl` at a 5-minute cadence, with `env_history_summary` / `env_history_recent` tools for role-local LLM trend analysis without loading raw files.
- A humidity rule has been verified end to end: Coordinator automation reads Sensor AHT20 humidity, Guardian allows the action, and Control sets the WS2812 status light.
- Control publishes `espagent.control_state.v1` snapshots after remote actuator commands, so Display Terminals can show busy state, emergency stop state, interlock config, and recent actuator results.
- Guardian policy decisions include `risk_score` and `privacy_mode=metadata_only`; Guardian also subscribes to `nodes/+/state` and `nodes/+/telemetry` to build lightweight watchdog StateBoard updates.
- Managed Lua runtime has been deployed to all four ESP32-S3 roles. `tools/test_lua_usb0.py --echo` passed 7/7 on USB0 after SNTP sync, and `tools/test_lua_roles_usb0_3.py` passed 8/8 across USB0-3.
- ESP32-P4 display firmware has verified Wi-Fi/MQTT connect and topic subscription; full live UI binding should still be treated as in-progress.
- Local admin mode exposes `/status`, `/devices`, and `/api/skills` for board
  status, node registry inspection, and runtime skill management. The host
  console compares edited skill content with SPIFFS content and skips writes
  when the SHA-256 hash is unchanged.

## Runtime Flow

```text
Feishu / WebSocket / CLI / Cron / Proactive / Automation
        |
        v
message_bus inbound queue
        |
        v
coordinator agent_loop
        |
        v
LLM ReAct tool use
        |
        v
tool_registry
        |
        +--> local tools
        |
        +--> mesh_send_command
                 |
                 v
          Guardian policy_check / policy_decision
                 |
                 v
          Sensor or Control role command
                 |
                 v
          structured OutputMessage
                 |
                 v
          async result reinjection + timeline + Feishu/WebSocket reply
```

The ReAct loop is now a first-version cross-node loop: the Coordinator can reason, call a Mesh tool, wait asynchronously for the remote result, observe the structured OutputMessage, and then produce a user-facing answer.

Before prompt construction, ordinary user text now passes through a lightweight
slash-command parser in `main/agent/slash_command.c`. `/help`, `/init`,
`/doctor`, `/mcp`, `/compact`, `/context_status`, `/skills_list`,
`/skills_show`, and invalid slash commands return immediate text replies.
Routing commands such as `/control ...`, `/workflow ...`, or `/sensor ...` are
rewritten into stronger role-constrained natural-language prompts and then
continue through the normal `agent_loop`.

## Automation Runtime

ESPAgent has a deterministic automation layer for requests that should not depend on a single open LLM turn.

- `automation_create_workflow`: creates one-shot ordered or delayed workflows, such as "turn red now, then blue after 10 seconds".
- `automation_create_rule`: creates one-shot condition-action rules, such as "if humidity is above 40%, set the light red"; the rule auto-removes after its first branch action attempt.
- `automation_list`: lists active workflows and rules.
- `automation_remove`: removes a workflow or rule.

Pending rules are stored in `/spiffs/automation.json` only while waiting for their first trigger. A background `rule_task` scans conditions and removes each rule after one branch action attempt, while ordered workflows run in temporary `workflow_task` instances and release their slot after completion. The current default limits are 8 rules, 8 workflow slots, and 8 steps per workflow.

## Repository Layout

```text
ESPAgent/
├── main/                       ESP-IDF application component
│   ├── espagent.c              app_main() banner and startup phase calls
│   ├── espagent_config.h       compile-time project constants
│   ├── espagent_secrets.h.example
│   │                           build-time credentials template
│   ├── app/                    application startup and service orchestration
│   ├── agent/                  agent loop and system prompt construction
│   ├── automation/             persistent rules and one-shot workflows
│   ├── bus/                    FreeRTOS inbound/outbound message queues
│   ├── cache/                  local runtime cache for prompt fragments
│   ├── channels/feishu/        Feishu/Lark WebSocket channel
│   ├── cli/                    USB serial CLI
│   ├── cron/                   scheduled agent trigger service
│   ├── device/                 device registry for Mesh nodes
│   ├── drivers/                sensor and peripheral drivers
│   ├── espnow/                 ESP-NOW telemetry sender
│   ├── gateway/                local WebSocket chat gateway
│   ├── heartbeat/              heartbeat-driven background checks
│   ├── llm/                    HTTPS LLM provider client and tool-use parser
│   ├── memory/                 long-term memory and per-chat JSONL sessions
│   ├── mesh/                   MQTT Mesh command, policy, and protocol validation
│   ├── node/                   node identity, role, capabilities, responsibilities
│   ├── onboard/                Wi-Fi onboarding/admin portal
│   ├── proxy/                  HTTP CONNECT proxy support
│   ├── roles/                  coordinator/sensor/control/guardian/display boundaries
│   ├── sensors/                periodic sensor publishing integrations
│   ├── skills/                 SPIFFS skill summary loader
│   ├── time_sync/              SNTP-backed local time support
│   ├── wifi/                   Wi-Fi connection helpers
│   └── tools/                  AI-callable tool registry and tool handlers
├── spiffs_data/                files bundled into the SPIFFS partition
│   ├── config/                 SOUL.md and USER.md bootstrap files
│   ├── memory/                 MEMORY.md and daily notes
│   └── skills/                 markdown skills loaded at runtime
├── docs/                       architecture, setup, and integration notes
├── tools/                      host-side flash, verification, and test runners
├── benchmarks/                 benchmark datasets and expected behavior cases
├── skills/deploy/              local deployment helper skill and scripts
├── scripts/                    host setup/build helper scripts
├── artifacts/                  generated test logs and summaries, ignored
├── build*/                     ESP-IDF build output, ignored
├── partitions.csv              flash partition table
├── sdkconfig.defaults          shared ESP-IDF defaults
└── sdkconfig.defaults.esp32s3  ESP32-S3-specific defaults
```

Detailed placement rules are maintained in
[`docs/PROJECT_STRUCTURE.md`](docs/PROJECT_STRUCTURE.md). In short: firmware code
belongs under `main/`, runtime SPIFFS seed files under `spiffs_data/`, host test
and flashing tools under `tools/`, and generated build/test output under ignored
`build*/` or `artifacts/` directories.

## Build

Load the ESP-IDF environment, then build:

```bash
idf.py build
```

The default firmware artifact is:

```text
build/ESPAgent.bin
```

Flash with:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

For the current four-S3 setup, the recommended order is:

```text
/dev/ttyUSB0  coordinator_agent
/dev/ttyUSB1  sensor_agent
/dev/ttyUSB2  control_agent
/dev/ttyUSB3  guardian_agent
```

Use `/dev/ttyUSB0-3` for ESP32-S3 flashing. Do not flash ESP32-S3 role firmware to `/dev/ttyACM*`; the ESP32-P4+C6 display terminal commonly appears as `/dev/ttyACM0` and has its own project.

Normal iteration should prefer app-only flashing through `tools/flash_roles_usb0_3.sh`.
If a board falls into a bootloader loop such as `invalid segment length 0xffffffff`
or `No bootable app partitions`, recover it with a raw full-device reflash
instead of another incremental `idf.py flash`.

In AP/admin mode, the same management surface is available through:

```text
GET  /status
GET  /devices
GET  /api/skills
GET  /api/skills?name=<runtime_skill>
POST /api/skills
DELETE /api/skills
```

## Configuration

Copy the secrets template when build-time credentials are needed:

```bash
cp main/espagent_secrets.h.example main/espagent_secrets.h
```

Credentials can also be set through the serial CLI and stored in NVS. The local setup portal appears as an `ESPAgent-XXXX` Wi-Fi network when onboarding/admin AP mode is active.

Node identity defaults can be set in `main/espagent_secrets.h`:

```c
#define ESPAGENT_SECRET_NODE_ID "esp32s3-edge-01"
#define ESPAGENT_SECRET_NODE_ROLE "edge_agent"
#define ESPAGENT_SECRET_NODE_LOCATION "南京市栖霞区"
#define ESPAGENT_SECRET_MESH_TOPIC_PREFIX "espagent"
#define ESPAGENT_SECRET_NODE_CAPABILITIES "coordinator,communication,sensor,control,guardian,telemetry,timeline,alerts"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "single-node development profile; can chat, sense, control, publish telemetry, and audit mesh state"
```

For a four-ESP32 setup, use the same codebase and assign each board a different node profile:

```text
esp32s3-coordinator-01  coordinator_agent  coordinator,communication,llm,dispatch,timeline,alerts
esp32s3-sensor-01       sensor_agent       sensor,telemetry,environment,air_quality,light,presence
esp32s3-control-01      control_agent      control,gpio,rgb,servo,relay,actuator
esp32s3-guardian-01     guardian_agent     guardian,security,policy,privacy,audit,watchdog,stateboard
```

At the moment, role identity is still mainly a build-time profile. The focused
Agent collaboration branch does not use MCU-side OTA as a showcase path; firmware
updates should be handled from the development host until role identity is moved
to NVS and a single image can safely serve all four S3 roles.

## MQTT Mesh Topics

Typical topics under the configured prefix:

```text
espagent/nodes/<node_id>/state
espagent/nodes/<node_id>/telemetry
espagent/nodes/<node_id>/events
espagent/nodes/<node_id>/command
espagent/roles/<role>/command
espagent/agent/dispatch
espagent/agent/timeline
espagent/alerts
espagent/security/policy_check
espagent/security/decision
```

The current lab prefix is `espagent/cube1345`, and the temporary public broker used during bring-up is `broker.emqx.io:1883`. That broker is only for development validation. A production deployment should use a private broker with authentication, TLS, ACLs, and topic isolation.

## Current Boundaries

- The Coordinator still uses one main `agent_loop`; the four S3 boards are not four independent full LLM agents.
- `spawn_subagent` is a bounded local FreeRTOS task, not another physical device. It can use only a small whitelist of non-hardware tools.
- Managed Lua is a capability extension layer, not a raw hardware escape hatch. Scripts must call existing ESPAgent capabilities through `espagent.call_capability()`, so sandbox, role profile, Guardian/Mesh policy, and Control interlocks remain authoritative.
- Direct esp-claw-style raw hardware modules such as GPIO/I2C/ADC/PWM/RMT/BLE/display/camera/audio are intentionally not exposed to Lua unless wrapped by ESPAgent manifest/capability policy.
- Sensor and Control execute only whitelisted Mesh commands.
- Control checks a cached Guardian allow decision, passes actuator commands through a lightweight command queue/interlock, and can verify HMAC-signed Mesh commands when `ESPAGENT_SECRET_MESH_AUTH_KEY` is configured.
- Manual approval, configurable hardware interlock GPIO, HMAC current/previous-key rotation hooks, trace index, lightweight task-tree aggregation, Sensor threshold events, Control state snapshots, and Guardian watchdog aggregation are implemented in firmware. Production broker ACL/TLS, real interlock wiring validation, key rotation operations, and full multi-board watchdog stress testing still need deployment-level validation.
- ESP32-P4/Android Display Terminals should consume the existing MQTT data stream; not every UI card should be described as fully live until verified on hardware.

Next esp-claw-inspired work should focus on Board Descriptor / Board Profile, Lua package metadata, capability lifecycle health checks, a script/manifest/skill/benchmark generator, and P4/Android script-capability debug surfaces.

## Documentation

- [Competition defense guide (Chinese)](docs/defense/README.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Public knowledge base](docs/PUBLIC_KNOWLEDGE_BASE.md)
- [Project structure](docs/PROJECT_STRUCTURE.md)
- [LingShu Agent Mesh](docs/LINGSHU_AGENT_MESH.md)
- [ESP32 role profiles](docs/ESP32_ROLE_PROFILES.md)
- [Android Agent Mesh handoff](docs/ANDROID_AGENTMESH_HANDOFF.md)
- [LLM to hardware runtime](docs/LLM_TO_HARDWARE_RUNTIME.md)
- [Skill benchmark](docs/SKILL_BENCHMARK.md)
- [Feishu setup](docs/im-integration/FEISHU_SETUP.md)
- [Wi-Fi onboarding AP](docs/WIFI_ONBOARDING_AP.md)
- [Tavily setup](docs/tool-setup/TAVILY_SETUP.md)
- [Amap weather setup](docs/tool-setup/AMAP_WEATHER_SETUP.md)
- [Roadmap](docs/TODO.md)

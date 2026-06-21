# ESPAgent Project Structure

This document defines the intended file architecture for ESPAgent. Keep it in
sync with `README.md`, `docs/ARCHITECTURE.md`, and `docs/PUBLIC_KNOWLEDGE_BASE.md`
when moving modules or adding major features.

## Top-Level Layout

```text
ESPAgent/
├── main/                       ESP-IDF application component
├── spiffs_data/                Initial SPIFFS filesystem image
├── docs/                       Architecture, setup, planning, and handoff docs
├── tools/                      Host-side flashing, validation, and test runners
├── scripts/                    Host setup/build helper scripts
├── benchmarks/                 Benchmark datasets and expected behavior cases
├── skills/                     Codex/local operator skills, not firmware skills
├── artifacts/                  Generated test logs and run summaries, ignored
├── build*/                     ESP-IDF build output, ignored
├── partitions.csv              Flash partition table
├── sdkconfig.defaults          Shared ESP-IDF defaults
├── sdkconfig.defaults.esp32s3  ESP32-S3 defaults
├── CMakeLists.txt              ESP-IDF project entry
└── README.md                   Project entry document
```

`artifacts/`, `build*/`, `tmp*/`, Python cache directories, binaries, maps, and
logs are generated output. They should not be treated as source files.

## Firmware Source Boundaries

`main/` is the only firmware component compiled into the ESP32-S3 app by
`main/CMakeLists.txt`. Keep source modules in this component unless a true
reusable ESP-IDF component is introduced later.

```text
main/
├── app/            Boot orchestration and service startup order
├── agent/          ReAct loop, prompt construction, OutputMessage handling
├── automation/     Persistent rules and delayed workflows
├── bus/            FreeRTOS inbound/outbound message queues
├── capability/     esp-claw-like capability registry and role-visible profiles
├── cache/          Runtime cache for prompt fragments and skill summaries
├── channels/       External chat channels such as Feishu/Lark
├── cli/            USB serial diagnostics and maintenance commands
├── cron/           Scheduled injected agent turns
├── drivers/        Deterministic low-level peripheral drivers
├── dynamic/        Runtime extension catalog for manifests/workflows/future Lua
├── events/         Unified lightweight event router and recent event ring
├── espnow/         ESP-NOW telemetry path
├── gateway/        Local WebSocket chat gateway
├── heartbeat/      Background heartbeat checks
├── llm/            LLM provider HTTP client and tool-use parsing
├── lua/            Optional bounded Lua runtime adapter
├── memory/         Long-term memory and session persistence
├── mesh/           MQTT Mesh protocol structs, topics, and validation
├── node/           Node identity, role, and capability model
├── onboard/        Wi-Fi onboarding/admin portal
├── ota/            HTTPS OTA maintenance primitive
├── proxy/          HTTP CONNECT proxy support
├── roles/          Coordinator/Sensor/Control/Guardian/Display role services
├── sensors/        Periodic sensor telemetry and MQTT integrations
├── skills/         Runtime skill summary loader for SPIFFS markdown
├── time_sync/      SNTP time synchronization
├── tools/          AI-callable tool handlers and tool registry
└── wifi/           Wi-Fi station lifecycle
```

### Placement Rules

- Chat ingress belongs in `main/channels/<channel>/`, not in hardware tools.
- The agent loop decides intent; tools execute bounded actions.
- Hardware bus code and chip protocols belong in `main/drivers/`.
- AI-callable wrappers belong in `main/tools/`; capability metadata, role-visible
  tool catalogs, and esp-claw-like callable grouping belong in `main/capability/`.
- Structured events from message bus, tools, Mesh, automation, or future channels
  belong in `main/events/`; channels should emit events instead of inventing
  private trace formats.
- Runtime hardware extension discovery belongs in `main/dynamic/`; actual
  deterministic primitive execution remains in `main/tools/tool_virtual_device.c`
  and low-level bus drivers.
- Lua script execution belongs in `main/lua/`; AI-facing Lua commands belong in
  `main/tools/tool_lua.c` and must still pass sandbox/capability policy. Lua is
  linked through `georgik/lua`; scripts use `require('espagent')` or global
  `espagent` to call existing capabilities instead of bypassing Mesh/Guardian.
- Cross-node command schema and topic validation belong in `main/mesh/`.
- Role-specific startup and local service ownership belong in `main/roles/`.
- Long-running deterministic behavior belongs in `main/automation/`, `main/cron/`,
  `main/proactive/`, or `main/heartbeat/`, not inside one LLM turn.
- Files intended to ship in the ESP32 SPIFFS partition belong under
  `spiffs_data/`.
- Host-only validation and flashing code belongs under `tools/`; keep it out of
  `main/`.

## SPIFFS Source Layout

```text
spiffs_data/
├── config/         Bootstrap persona and user context files
├── memory/         Initial long-term memory files
└── skills/         Runtime markdown skills loaded by skill_loader
```

`spiffs_data/` is packaged by the top-level `spiffs_create_partition_image()`
call. Moving this directory requires changing `CMakeLists.txt` and all docs or
scripts that refer to `/spiffs/...` runtime paths.

## Host Tool Layout

```text
tools/
├── flash_roles_usb0_3.sh              Flash USB0-USB3 role profiles
├── flash_roles_usb0_3_verify.sh       Flash and verify role identity
├── verify_roles_usb0_3.py             Serial role checker
├── benchmark_skills_usb0_3.py         Runtime skill benchmark runner
├── stress_mesh_usb0_3.py              Mesh stress runner
├── stress_feishu_usb0_3.py            Feishu entry stress runner
├── test_feishu_full_agent_usb0_3.py   Full Feishu capability suite
└── test_feishu_multistep_usb0_3.py    Multi-step workflow regression runner
```

These tools intentionally use `/dev/ttyUSB0-3` for ESP32-S3 role boards. Do not
fall back to `/dev/ttyACM*`, because ESP32-P4 display hardware commonly appears
there and uses a different project.

## Documentation Layout

Use the following ownership rules for docs:

- `README.md`: concise project entry, capabilities, build, flash, and links.
- `docs/ARCHITECTURE.md`: firmware architecture and runtime flow.
- `docs/PUBLIC_KNOWLEDGE_BASE.md`: comprehensive project memory and current state.
- `docs/ESP32_ROLE_PROFILES.md`: four ESP32-S3 role identities and flash order.
- `docs/SKILL_BENCHMARK.md`: benchmark dataset and runner instructions.
- `docs/PROJECT_STRUCTURE.md`: this file, directory ownership and placement rules.
- `docs/im-integration/`: Feishu/Lark integration.
- `docs/tool-setup/`: external service setup such as Amap and Tavily.

## Current Refactor Boundary

The project is already split by firmware responsibility inside `main/`, so the
next high-value structure improvement is not to create four separate ESP-IDF
projects. Keep one firmware project and assign roles by node profile until role
identity is moved fully into NVS.

The main structural debt is `main/sensors/sensor_mqtt.c`: it currently combines
general MQTT Mesh lifecycle, command handling, telemetry publishing, policy
decision caching, OutputMessage waiting, and display-facing timeline topics.
When this is split, use this target layout:

```text
main/mesh/
├── mesh_protocol.c/.h          Existing command schema and topic helpers
├── mesh_mqtt_client.c/.h       MQTT connect, subscribe, publish primitives
├── mesh_command_router.c/.h    Command dispatch, result, OutputMessage path
└── mesh_policy_cache.c/.h      Guardian decision cache and validation helpers

main/sensors/
└── sensor_telemetry.c/.h       Periodic environment sampling and telemetry
```

Do that split only with a build and four-role smoke test, because
`sensor_mqtt.c` is currently on the critical Feishu-to-Mesh path.

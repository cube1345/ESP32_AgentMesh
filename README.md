# ESPAgent

ESPAgent is an ESP32-S3 firmware project that runs a lightweight AI agent directly on ESP-IDF and FreeRTOS. It connects messaging channels, local WebSocket access, persistent SPIFFS memory, LLM tool use, and hardware-control tools into one embedded agent runtime.

The project is maintained as ESPAgent. Public documentation, runtime logs, generated binaries, and directory structure should use the ESPAgent name consistently.

The current firmware is the first edge-node phase of the planned LingShu Agent Mesh: one ESP32-S3 runs the local agent runtime, publishes node state and telemetry over MQTT mesh topics, and keeps Feishu/WebSocket/Cron/Proactive input flowing through the same guarded `agent_loop`. It is not yet a cloud Coordinator or multiple independent LLM agent processes.

## Capabilities

- ESP32-S3 firmware built with ESP-IDF 6.1
- Feishu/Lark WebSocket channel for chat input and replies
- Local WebSocket gateway on port `18789`
- Serial CLI for diagnostics and local maintenance
- ReAct-style agent loop with LLM tool use
- Bounded `spawn_subagent` tool for focused search, weather/time, and SPIFFS file subtasks
- SPIFFS-backed memory, sessions, skills, and config files
- Structured weather lookup through Amap WebService, with Nanjing Qixia District as the default location when configured
- Hardware tools for GPIO, WS2812, servo, MAX98357, AHT10/AHT20, SGP30, BH1750/GY-30, HC-SR05, and environment readings
- ESP-NOW environment telemetry sender
- MQTT mesh telemetry/state topics under `espagent/nodes/<node_id>/...`
- MQTT Mesh command publishing through `mesh_send_command`
- Four ESP32-S3 role profiles: `coordinator_agent`, `sensor_agent`, `control_agent`, and `display_agent`
- Wi-Fi onboarding/admin AP under the `ESPAgent-XXXX` network name

Current Mesh boundary: the Coordinator can publish a standard MQTT Mesh command to another node or role, and the Sensor role has a whitelisted `read_temperature_humidity` result path. Control-role hardware execution is still intentionally disabled until command queue, authorization, safety interlock, audit events, and result correlation are implemented.

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
│   ├── bus/                    FreeRTOS inbound/outbound message queues
│   ├── cache/                  local runtime cache for prompt fragments
│   ├── channels/feishu/        Feishu/Lark WebSocket channel
│   ├── cli/                    USB serial CLI
│   ├── cron/                   scheduled agent trigger service
│   ├── drivers/                sensor and peripheral drivers
│   ├── espnow/                 ESP-NOW telemetry sender
│   ├── gateway/                local WebSocket chat gateway
│   ├── heartbeat/              heartbeat-driven background checks
│   ├── llm/                    HTTPS LLM provider client and tool-use parser
│   ├── memory/                 long-term memory and per-chat JSONL sessions
│   ├── mesh/                   MQTT Mesh command types and protocol validation
│   ├── node/                   node identity, role, capabilities, responsibilities
│   ├── onboard/                Wi-Fi onboarding/admin portal
│   ├── ota/                    HTTPS OTA update support
│   ├── proxy/                  HTTP CONNECT proxy support
│   ├── roles/                  coordinator/sensor/control/display role boundaries
│   ├── sensors/                periodic sensor publishing integrations
│   ├── skills/                 SPIFFS skill summary loader
│   └── tools/                  AI-callable tool registry and tool handlers
├── spiffs_data/                files bundled into the SPIFFS partition
│   ├── config/                 SOUL.md and USER.md bootstrap files
│   ├── memory/                 MEMORY.md and daily notes
│   └── skills/                 markdown skills loaded at runtime
├── docs/                       architecture, setup, and integration notes
├── skills/deploy/              local deployment helper skill and scripts
├── scripts/                    host setup/build helper scripts
├── partitions.csv              flash partition table
├── sdkconfig.defaults          shared ESP-IDF defaults
└── sdkconfig.defaults.esp32s3  ESP32-S3-specific defaults
```

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
#define ESPAGENT_SECRET_NODE_CAPABILITIES "coordinator,communication,sensor,control,display,telemetry,timeline,alerts"
#define ESPAGENT_SECRET_NODE_RESPONSIBILITIES "single-node development profile; can chat, sense, control, publish telemetry, and display mesh state"
```

For a four-ESP32 setup, use the same firmware and assign each board a different node profile: `coordinator_agent`, `sensor_agent`, `control_agent`, and `display_agent`.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Public knowledge base](docs/PUBLIC_KNOWLEDGE_BASE.md)
- [LingShu Agent Mesh](docs/LINGSHU_AGENT_MESH.md)
- [ESP32 role profiles](docs/ESP32_ROLE_PROFILES.md)
- [Feishu setup](docs/im-integration/FEISHU_SETUP.md)
- [Wi-Fi onboarding AP](docs/WIFI_ONBOARDING_AP.md)
- [Tavily setup](docs/tool-setup/TAVILY_SETUP.md)
- [Amap weather setup](docs/tool-setup/AMAP_WEATHER_SETUP.md)
- [Roadmap](docs/TODO.md)

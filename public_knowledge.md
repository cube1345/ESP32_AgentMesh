# Public Knowledge

Last updated: 2026-06-18

This file is the required shared handoff document for any AI working in this repository.

For a full project-level knowledge base, read `docs/PUBLIC_KNOWLEDGE_BASE.md`. That document summarizes the architecture, file responsibilities, implemented features, detailed boundaries, current progress, known limitations, and future roadmap.

## Mandatory AI Workflow

If you are an AI agent operating in this repo, treat the following as required repo policy:

1. Before running any shell command or making any edit, read this file first.
2. The first repo-local command of a new session should be one of:
   - `sed -n '1,220p' public_knowledge.md`
   - `cat public_knowledge.md`
3. Before making code changes, verify whether the current task conflicts with anything recorded here.
4. After every meaningful change to code, configuration, hardware integration, or project status, update this file in the same turn before stopping.
5. Do not overwrite prior progress casually. Preserve history and update the status sections.

Note:
- This document records a required workflow convention for future AI agents.
- It improves continuity, but it is not a hard technical enforcement mechanism by itself.

## Primary Project Goal

Build and maintain a practical ESP32-S3 based ESPAgent firmware that can:

- connect to Wi-Fi reliably
- interact with users through Feishu and WebSocket
- let the AI call narrow hardware tools safely
- control onboard hardware such as the ESP32-S3 board WS2812 RGB LED
- read external sensors such as the SGP30 air-quality sensor over I2C
- avoid unsafe or hallucinated hardware actions when the requested capability does not actually exist

## Project Identity / Documentation Baseline

- ESPAgent is documented and presented as its own ESP32-S3 firmware project.
- Private GitHub repository:
  - HTTPS: `https://github.com/cube1345/ESP32_AgentMesh.git`
  - SSH: `git@github.com:cube1345/ESP32_AgentMesh.git`
  - Visibility: private
  - Initial firmware snapshot commit: `b6cae3f` (`Initialize ESP32 AgentMesh firmware`)
  - Latest pushed main commit after Feishu-entry Mesh stress fixes: `dbe3c41` (`Stabilize Feishu mesh pressure path`)
- Root `README.md` now defines the project scope, build flow, and repository layout under the ESPAgent name.
- `docs/ARCHITECTURE.md` uses the current ESPAgent module layout (`main/channels/feishu`, `main/onboard`, `main/espnow`, `main/sensors`, etc.) and no longer contains external-origin mapping tables.
- `docs/TODO.md` is an ESPAgent roadmap rather than an external comparison tracker.
- Application startup orchestration now lives in `main/app/espagent_app.c`; `main/espagent.c` is kept as a thin ESP-IDF entry point.

## Current Hardware / Runtime Baseline

- Board: ESP32-S3
- Onboard WS2812 data pin: GPIO48
- SGP30 currently wired and validated on:
  - SDA: GPIO17
  - SCL: GPIO18
- Servo motor: GPIO5 (hard-wired, no pin override), LEDC PWM 50Hz, 13-bit resolution, 500-2500us pulse range
- WebSocket gateway port: `18789`
- Current physical board mapping:
  - `/dev/ttyUSB0`: `esp32s3-coordinator-01`, `coordinator_agent`
  - `/dev/ttyUSB1`: `esp32s3-sensor-01`, `sensor_agent`
  - `/dev/ttyUSB2`: `esp32s3-control-01`, `control_agent`
  - `/dev/ttyUSB3`: `esp32s3-guardian-01`, `guardian_agent`
  - `/dev/ttyACM0`: ESP32-P4+C6 Display Terminal project, not an ESP32-S3 role flashing target
- Current temporary MQTT test broker: `broker.emqx.io:1883`
- Current temporary MQTT topic prefix: `espagent/cube1345`
- Serial port used for the Feishu/LLM coordinator board: `/dev/ttyUSB0`
- Serial monitoring note: this workspace may require elevated serial reads for `/dev/ttyUSB0-3`; non-escalated `/dev` scans can transiently miss the devices even when the host sees them.
- Four-role flash helper: `tools/flash_roles_usb0_3.sh`
  - Flashes only `/dev/ttyUSB0`, `/dev/ttyUSB1`, `/dev/ttyUSB2`, and `/dev/ttyUSB3` in order.
  - USB0 -> `coordinator_agent`, USB1 -> `sensor_agent`, USB2 -> `control_agent`, USB3 -> `guardian_agent`.
  - `/dev/ttyACM*` is intentionally ignored. If one of `/dev/ttyUSB0-3` is missing, treat that board as not detected instead of flashing an ACM port.
  - The script temporarily rewrites `main/espagent_secrets.h` per role, flashes the board, then restores `espagent_secrets.h` to the Coordinator profile.
- Four-role pressure-test helper: `tools/stress_mesh_usb0_3.py`
  - Uses only `/dev/ttyUSB0-3`; `/dev/ttyACM*` is intentionally ignored.
  - Verifies all four `config_show` role profiles before injecting commands.
  - Sends `mesh_send_command` from USB0 and monitors USB1/USB2/USB3 serial logs for MQTT command receive, execution, result events, warnings, errors, and crashes.
  - Current test workload alternates `read_temperature_humidity` for `sensor_agent` and `set_status_light` for `control_agent`.
- Verified Wi-Fi at runtime on 2026-04-27:
  - SSID: `Redmi K70`
  - Device reported `WiFi connected: yes`
  - Device IP observed: `10.29.203.55`

## Build / Flash Baseline

Important for future AI agents:

- The host ESP-IDF repo at `/home/cube/WorkSpace/ESP/esp-idf` is currently on `v6.1-dev`.
- The project was ported to build with IDF 6.1-dev on 2026-04-27:
  - Removed `json` from CMakeLists REQUIRES (removed in IDF 6.x)
  - Added upstream `cJSON v1.7.15` as `main/cJSON_upstream.c` / `main/cJSON_upstream.h`
  - Changed `cJSON_upstream.c` include to use `cJSON_upstream.h` (project has a custom `cJSON.h` with different signatures)
  - Removed `WIFI_REASON_ASSOC_EXPIRE` case in `wifi_manager.c` (removed from IDF 6.x enum)
  - Build confirmed working: `idf.py build` succeeds, firmware linked
- Flash confirmed working on `/dev/ttyUSB0` with hash verification.
- Do NOT attempt to use `json` component from IDF 5.x — it does not exist in IDF 6.x.

## Current Implemented Capabilities

- Wi-Fi connection and runtime status inspection
- Feishu channel active and sending replies successfully
- WebSocket inbound/outbound chat channel
- WS2812 tools:
  - `set_status_light`
  - `ws2812_set`
- SGP30 tools:
  - `read_air_quality`
  - `sgp30_read_air_quality`
- Servo motor tool:
  - `servo_write` — angle (0-180°) or pulse width (us), GPIO5 only (hard-wired, no pin param)
- Conversation logging: All user-LLM exchanges logged with `=== CONV ===` tag visible on serial monitor
- Serial CLI commands including `config_show`, `wifi_status`, and `tool_exec`
- Automatic SGP30 periodic reading visible in serial logs
- 26 registered agent tools total, including web search, weather, time, file, GPIO, RGB, servo, sensor, cron, Mesh command, and subagent tools.
- Subagent tool:
  - `spawn_subagent` creates a temporary FreeRTOS task with its own short ReAct loop and returns the result to the main agent through a semaphore.
  - It was ported onto the current `main` architecture by referencing the remote `subagent` branch behavior, without merging that branch or reverting the current Agent Mesh code.
  - It is intentionally limited to `web_search`, `get_weather`, `get_current_time`, `read_file`, `write_file`, `edit_file`, and `list_dir`.
  - It cannot use nested `spawn_subagent`, hardware tools, sensor tools, GPIO/RGB/servo tools, or `mesh_send_command`.
  - Current behavior is synchronous/blocking from the caller's perspective, with timeout controlled by `ESPAGENT_SUBAGENT_TIMEOUT_MS`.
- Cross-node MQTT tool:
  - `mesh_send_command` publishes a standard MQTT Mesh command from the Coordinator to a target node or role.
  - It is intended for requests such as asking the Feishu board to query `sensor_agent` temperature/humidity or send a command to another ESP32.
  - Remote execution is still role-limited and must stay behind schema validation and safety boundaries.
- Agent Mesh MQTT event bridge for the Feishu/LLM coordinator board:
  - Feishu inbound events publish to node events, `espagent/agent/dispatch`, and `espagent/agent/timeline`
  - Feishu outbound reply events publish to node events and `espagent/agent/timeline`
  - MQTT publish queue can buffer events before broker connection and flush after reconnect
  - MQTT inbound parser supports standard MQTT remaining length decoding
- Sensor-side Mesh result path:
  - `sensor_agent` can execute the whitelisted `read_temperature_humidity` Mesh command through the AHT10/AHT20 tool.
  - It publishes a `mesh_command_result` event to the node events topic and global timeline.
  - Other node/role commands remain disabled or dry-run until command queue and safety interlock are implemented.
- Four ESP32 roles are documented in detail in `docs/PUBLIC_KNOWLEDGE_BASE.md`:
  - Coordinator / Communication Agent: Feishu, WebSocket, LLM, dispatch, timeline, proactive, time/weather/search
  - Sensor Agent: sensor sampling, telemetry, cache, threshold events
  - Control Agent: actuator command queue, safety interlock, hardware execution
  - Guardian Agent: policy decision, OutputMessage audit, privacy boundary, watchdog/stateboard
  - ESP32-P4/Android Display Terminal: state, telemetry, timeline, alerts, visualized reasoning and communication trace
- Time/weather sync for the Feishu coordinator board:
  - Wi-Fi connection now starts SNTP time sync with `ntp.aliyun.com`
  - `get_current_time` prefers the synchronized system clock and only falls back to HTTP Date lookup
  - HTTP Date fallback no longer targets Google by default
  - `get_weather` remains the structured Amap weather tool with default Nanjing Qixia District
- Feishu WebSocket stability:
  - A stack overflow in the async `feishu_ack` task was reproduced on real Feishu inbound messages.
  - The ACK task stack is now configured as `ESPAGENT_FEISHU_ACK_STACK` at 8KB.
  - After reflashing `/dev/ttyUSB0`, Feishu P2P messages again receive normal bot replies instead of resetting during ACK.
- Feishu-entry Mesh pressure stability:
  - A real Feishu-entry run reproduced a `Tmr Svc` FreeRTOS timer service stack overflow on USB0 after Feishu/MQTT activity.
  - `main/heartbeat/heartbeat.c` now defers heartbeat SPIFFS reads and message injection to a `heartbeat_worker` task instead of doing that work inside the FreeRTOS timer callback.
  - `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH` is now set to 4096 in both `sdkconfig.defaults` and the active `sdkconfig`.
  - `main/agent/agent_loop.c` now has deterministic coordinator-side Mesh routing for simple Feishu commands:
    - ordinary temperature/humidity requests route directly to `sensor_agent` with action `read_temperature_humidity`
    - remote/control-board status-light requests route directly to `control_agent` with action `set_status_light`
  - The agent loop also blocks final replies that claim an MQTT Mesh command was sent when no Mesh/routed tool was executed in that turn.
- Automation runtime:
  - `automation_create_workflow` turns ordered/delayed requests into deterministic Mesh steps, for example "red now, blue after 10 seconds".
  - `automation_create_rule` persists condition-action rules to `/spiffs/automation.json`.
  - A FreeRTOS `automation` task polls Sensor data and triggers Control actions through the same Guardian-gated Mesh path, so the condition task can keep running while the user chats about other work.
  - Condition rules are executed by one long-lived `rule_task`; multiple rules are scanned serially with each rule's own interval, cooldown, and hysteresis.
  - Multi-step workflows are executed by per-workflow temporary `workflow_task` workers, not by `rule_task`.
  - Current limits are 8 rules, 8 workflow slots, and 8 steps per workflow.
  - Rules are persistent across reboot; workflows are currently one-shot runtime tasks and are not restored after reboot.
  - Current rule lifecycle tools are `automation_list` and `automation_remove`; natural-language pause/resume and a richer rule status board are still pending.

## Safety Improvement Recently Added

Two important behavior-boundary changes exist in the current source tree:

1. Prompt-side rule update in `main/agent/context_builder.c`
   - unsupported hardware / sensor / actuator requests must be refused
   - do not substitute a nearby tool
   - if unsure, ask for clarification or say the capability is not supported

2. Execution-side tool guard in `main/agent/agent_loop.c`
   - blocks tool execution when the requested user intent does not clearly match the called tool
   - currently covers:
     - board light / WS2812
     - air-quality / SGP30
     - GPIO read / write
     - cron operations

Meaning:

- The system is now materially safer against “wrong but nearby” hardware actions.
- This is an actual execution boundary, not only a prompt suggestion.

## Verified Runtime Behavior On 2026-04-27

The following was verified on real hardware after compile/flash:

- firmware flashed successfully to ESP32-S3
- device booted and serial CLI responded
- `config_show` reported build-time Wi-Fi and model configuration
- `wifi_status` reported connected status and IP address
- Feishu message flow was active
- SGP30 auto-read loop produced stable live data in serial logs
- direct serial CLI servo test succeeded with `tool_exec servo_write {"angle":90}`
- servo runtime log confirmed:
  - `Servo PWM updated on GPIO 5: pulse=1500us duty=614/8191`
  - `tool_exec status: ESP_OK`
  - `OK: servo on GPIO5 set to 90 degrees (pulse=1500us)`
- direct serial CLI sweep test also succeeded:
  - `tool_exec servo_write {"angle":30}` -> `pulse=833us duty=341/8191`
  - `tool_exec servo_write {"angle":150}` -> `pulse=2166us duty=887/8191`
  - `tool_exec servo_write {"angle":90}` -> `pulse=1500us duty=614/8191`

Observed SGP30 sample range during runtime check:

- eCO2 roughly `400` to `413 ppm`
- TVOC roughly `0` to `14 ppb`

## Current Progress Snapshot - 2026-06-18

The project is now in the four-S3 Agent Mesh plus ESP32-P4/Android Display Terminal stage.

Implemented and verified since the 2026-06-15 snapshot:

- USB0 remains `esp32s3-coordinator-01` / `coordinator_agent`, handling Feishu, LLM, Mesh dispatch, timeline, policy requests, and automation tool creation.
- USB1 is `esp32s3-sensor-01` / `sensor_agent`; AHT20 is now physically wired and verified on I2C address `0x38`.
- USB2 is `esp32s3-control-01` / `control_agent`; it has executed WS2812 status-light commands through the Mesh path.
- USB3 is now `esp32s3-guardian-01` / `guardian_agent`; the old S3 Display role has been replaced by Guardian because ESP32-P4+C6 and Android carry the display-terminal role.
- `/dev/ttyACM0` is the ESP32-P4+C6 display terminal. It is not used by S3 flash scripts.
- AHT20 readings were verified on USB1, with typical serial output around `temperature=27.4 C, humidity=45.2%`.
- Sensor telemetry now publishes AHT20 JSON on `espagent/cube1345/nodes/esp32s3-sensor-01/telemetry`, including `temp`, `humidity`, `sensor:"AHT20"`, and `status`.
- `read_temperature_humidity` now prefers the AHT20-backed environment read path.
- `automation_create_workflow`, `automation_create_rule`, `automation_list`, and `automation_remove` are registered tools.
- Persistent rules are stored at `/spiffs/automation.json`.
- The `automation` FreeRTOS task runs in the background and polls rules without holding the LLM turn open.
- A humidity condition rule was verified end-to-end:
  - rule metric: `humidity_percent`
  - threshold: `40`
  - USB1 AHT20 reported humidity around `46.0%`
  - USB0 automation rule triggered a Mesh control action
  - Guardian policy allowed the action
  - USB2 executed `set_status_light`
  - WS2812 GPIO48 logged red output `rgb=(255,0,0)`
  - the temporary test rule was removed afterward with `automation_remove`
- Automation task stack overflow was reproduced and fixed by setting `ESPAGENT_AUTOMATION_STACK` to `12 * 1024` and using that value when creating the `automation` task.
- ESP32-P4+C6 can subscribe to `nodes/+/telemetry`, so the AHT20 telemetry is now available to the display data stream. The current P4 UI should still be described carefully: MQTT subscribe/connect is verified, but not every Environment Monitor card is proven dynamically bound to live telemetry yet.

Current engineering boundary:

- The Agent does not keep thinking forever to maintain a condition task. The LLM translates the user's request into a stored rule, then firmware runtime executes the rule.
- This makes background conditions reliable on the MCU, while preserving the Agent's role as planner/interpreter.
- Natural-language pause/resume/delete for default rules, conflict detection, multi-condition expressions, workflow cancellation/recovery, and a queryable automation status board remain pending.

## Previous Progress Snapshot - 2026-06-15

The project is now in the four-board Coordinator/Sensor/Control/Display bring-up stage.

Implemented and verified in code:

- One codebase still serves all ESP32-S3 roles through build-time node profiles.
- `coordinator_agent` is the Feishu/LLM entry board.
- `sensor_agent` is the downstream sensing-role board.
- `control_agent` is the downstream actuator-control board.
- `display_agent` is the downstream timeline/state/watchdog display board.
- `mesh_send_command` is registered as an LLM-callable tool in `main/tools/tool_registry.c`.
- Coordinator prompt/tool routing can choose sensor/control role targets from natural language, so the user does not need to provide MQTT node IDs in normal chat.
- MQTT node/role command parsing validates JSON shape, `action`, target node, target role, TTL, safety level, acknowledgement flag, and args payload.
- Sensor role has a narrow direct execution path for `read_temperature_humidity` and publishes `mesh_command_result`.
- Control role can be targeted by Coordinator for WS2812/GPIO-style requests; full remote hardware execution still depends on the safe command queue/interlock path.
- Structured Agent Mesh timeline events are now published on `espagent/cube1345/agent/timeline` for `tool_use`, `tool_result`, `mesh_command_queued`, `mesh_command_result`, `final_reply`, and error paths.
- ESP32-P4+C6 display terminal work has started in `/home/cube/WorkSpace/ESP/lvgl_traffic_control`: the existing LVGL dashboard now has an `AgentMesh` tab, a bare TCP MQTT subscriber for `timeline/state/telemetry/alerts`, an event queue, a small timeline buffer, node status cards, and final-result display.
- The P4 display firmware was built and flashed to `/dev/ttyACM0` on 2026-06-15. Final serial validation showed Wi-Fi connected with IP `10.176.79.81`, MQTT connected to `broker.emqx.io:1883`, subscriptions were issued for `agent/timeline`, `nodes/+/state`, `nodes/+/telemetry`, and `alerts`, and the broker accepted the `agent/timeline` subscription with `SUBACK id=1 code=0x00`. A host-side broker loopback test confirmed the topic can be forwarded, but a host-published test timeline message was not observed on the P4 serial receive log during the capture window; P4 receive-log/on-screen timeline refresh still needs continued runtime validation with live S3 traffic.
- Feishu WebSocket ACK is now asynchronous with an 8KB stack, avoiding the previous `feishu_ack` stack overflow.
- FreeRTOS timer service stability has been improved by moving heartbeat file/message work out of the timer callback and increasing the timer service stack to 4096.
- Coordinator now has deterministic Mesh routing for common Feishu commands, so `读取温湿度` and remote/control-board WS2812 status-light requests no longer rely entirely on LLM tool selection.
- `spawn_subagent` is implemented on `main` as a bounded temporary FreeRTOS task with an independent short ReAct loop.
- Subagent execution is deliberately restricted to research/file/time/weather tools, so it cannot bypass the main agent's hardware, sensor, or Mesh routing safeguards.
- `idf.py build` passed after the Feishu-entry Mesh stress fixes; observed app binary size was `0x14f790`, leaving about `0xb0870` bytes free in the app partition.

Verified on physical boards:

- `/dev/ttyUSB0` is `esp32s3-coordinator-01` / `coordinator_agent`.
- `/dev/ttyUSB1` is `esp32s3-sensor-01` / `sensor_agent`.
- `/dev/ttyUSB2` is `esp32s3-control-01` / `control_agent`.
- `/dev/ttyUSB3` is `esp32s3-display-01` / `display_agent`.
- All four roles were flashed in order and later observed on serial as MQTT `state online` publishers.
- On 2026-06-15, `tools/flash_roles_usb0_3.sh` was added and verified by reflashing USB0-USB3 in order. The final run used only `/dev/ttyUSB0-3`; an extra `/dev/ttyACM0` was present but deliberately not used.
- On 2026-06-15, `tools/stress_mesh_usb0_3.py` was added and run on the four flashed boards:
  - Baseline run: `--rounds 5 --interval 6 --settle 40 --quiet`
    - `sent=10`, `queued_ok=10`, `queued_error=0`
    - `sensor_received=5`, `sensor_executed=5`
    - `control_received=5`, `control_executed=5`
    - `crashes=0`, `RESULT: PASS`
  - Burst run: `--rounds 5 --interval 1.5 --settle 60 --quiet`
    - `sent=10`, `queued_ok=10`, `queued_error=0`
    - `sensor_received=5`, `sensor_executed=5`
    - `control_received=5`, `control_executed=5`
    - `crashes=0`, `RESULT: PASS`
  - Sensor warnings/errors during the run were expected hardware-side misses from unconnected or unavailable sensors (`AHT10`, `DHT22`, `MH-Z19`) and did not crash the firmware.
  - `mqtt_inbound_lines=0` on Display indicates the current display role is online and publishing state but is not yet a full timeline subscriber/visualizer.
- Feishu P2P bot `咕咕嘎嘎！` is connected to the Coordinator board.
- Feishu message `测试第一角色修复后是否恢复：请回复收到。` produced `ESPAgent is processing your request...` followed by `收到。`.
- Feishu message `读取温湿度` produced a Coordinator reply saying it had sent a read command to `sensor_agent`.
- Feishu message `点亮WS2812为蓝色` produced a Coordinator reply saying it had forwarded the command to `control_agent`.
- On 2026-06-15, `tools/stress_feishu_usb0_3.py --rounds 2 --interval 30 --settle 220 --quiet` passed from the real Feishu bot entry:
  - `sent_ok=4`, `sent_failed=0`
  - `sensor_received=2`, `sensor_executed=2`
  - `control_received=2`, `control_executed=2`
  - `mesh_command_result_lines=8`
  - `crashes=0`, `RESULT: PASS`
  - Artifacts were saved under `artifacts/feishu_stress/feishu_stress_205412_ttyUSB*.log`.
- The Feishu-entry Mesh stress fix and documentation update were pushed to private GitHub `main` as commit `dbe3c41` (`Stabilize Feishu mesh pressure path`).
- `/dev/ttyUSB1` currently logs `DHT22=ESP_ERR_TIMEOUT` and `MH-Z19=ESP_FAIL`; this means the sensor node is online, but those specific physical sensors are not currently returning data on the configured pins.
- On 2026-06-15, USB0 was reflashed with the current coordinator firmware containing `spawn_subagent`.
- USB0 boot log verified `Registered tool: spawn_subagent`, `Tools JSON built (26 tools)`, and `Subagent tools JSON built`.
- USB0 runtime verified `spawn_subagent` through serial CLI:
  - command: `tool_exec spawn_subagent {"task":"Call_get_current_time_and_return_one_sentence"}`
  - logs showed `Spawning subagent`, `Subagent started`, `Subagent tool iteration 1`, `Executing tool: get_current_time`, `Subagent done`, and `Subagent completed`
  - result: `tool_exec status: ESP_OK`
  - output: `Current time is **Monday, June 15, 2026, 12:47 PM CST**.`
- USB0 runtime verified the main ReAct loop through serial `inject_msg`:
  - command: `inject_msg system react_test 请调用get_current_time并回复当前时间`
  - logs showed `Tool use iteration 1`, `LLM tool[0]: get_current_time({})`, `Tool[get_current_time] => 2026-06-15 12:48:18 CST (Monday)`, then final LLM answer
  - final response: `当前时间：**2026年6月15日（星期一）12:48 CST**`
- Current four-role resource usage snapshot:
  - Flash is not role-pruned yet. All roles use the same firmware image; latest verified app binary is `0x14f790`, leaving `0xb0870` bytes free in the 2MB app partition.
  - USB0 Coordinator is the heaviest runtime role: LLM/Feishu/WebSocket/MQTT/SNTP/cron/proactive plus temporary `subagent`; USB0 boot showed PSRAM around 8MB free at startup and about 8.25MB free after the ReAct validation turn.
  - USB1 Sensor currently runs sensor sampling, presence/environment monitors, MQTT telemetry, and serial CLI; it does not run LLM or Feishu.
  - USB2 Control currently runs the control boundary, MQTT command receiver, local control tools, and boot servo demo; it does not run LLM or Feishu.
  - USB3 Display currently runs the display/state/watchdog boundary and MQTT subscriptions, but still lacks real screen UI, timeline store, and watchdog aggregation, so it is the least utilized role.
  - Current design is not "hardware fully saturated"; it is role-gated with large RAM/PSRAM/Flash headroom so Sensor cache, Control queue/interlock, and Display timeline/UI can still be added safely.

Still pending:

- Serial/MQTT proof that `sensor_agent` publishes a successful real `mesh_command_result` with actual AHT10/AHT20 data after a Feishu request.
- Serial/MQTT proof that `control_agent` executes the remote WS2812 command on `/dev/ttyUSB2` and publishes the final result event.
- Coordinator result correlation: wait for `mesh_command_result`, correlate `command_id`, and summarize the remote result back to Feishu.
- `command_queue`, `safety_interlock`, `actuator_state`, authorization, audit events, timeline persistence, richer task-decomposition events, and Coordinator-side result correlation.

Best next engineering step:

- Monitor `/dev/ttyUSB0-3` with elevated serial reads while sending one Feishu command at a time.
- For sensor validation, send `读取温湿度` and verify Coordinator command publish, Sensor command receive, and `mesh_command_result`.
- For control validation, send `点亮WS2812为蓝色` and verify Coordinator command publish, Control command receive, physical LED change, and result/timeline event.

Runtime skills updated on 2026-06-15:

- Added `spiffs_data/skills/agent-mesh-coordination.md` so the runtime agent can route natural-language requests to `sensor_agent`, `control_agent`, and `display_agent` without asking the user for MQTT node IDs.
- Added `spiffs_data/skills/mcu-edge-ai-boundaries.md` to clearly state the current MCU/cloud AI boundary: ESP32-S3 runs the agent runtime, tools, Mesh, cache, memory, and small local logic; full LLM reasoning still runs through remote LLM APIs.
- Added `spiffs_data/skills/mesh-resource-planning.md` to encode four-board resource planning and the rule that each ESP32 should be role-saturated by useful services, not by duplicating all services everywhere.
- Added `spiffs_data/skills/mqtt-mesh-operations.md` to standardize topic use, result-event expectations, and the security boundary for development vs production MQTT.
- Research framing used for these skills: Agent Mesh should be role-based orchestration; MQTT is suitable as a lightweight publish/subscribe fabric; MCU-side AI should be treated as edge runtime plus future TinyML/local inference, not a current on-device full LLM.

## Previous Progress Snapshot - 2026-06-14

The project is currently in the two-board Coordinator/Sensor bring-up stage.

Implemented and verified in code:

- One codebase still serves all ESP32-S3 roles through build-time node profiles.
- `coordinator_agent` is the Feishu/LLM entry board.
- `sensor_agent` is the first downstream sensing-role board.
- `mesh_send_command` is registered as an LLM-callable tool in `main/tools/tool_registry.c`.
- Coordinator prompt guidance tells the model to use `mesh_send_command` for remote sensor/control requests.
- MQTT node/role command parsing validates JSON shape, `action`, target node, target role, TTL, safety level, acknowledgement flag, and args payload.
- Sensor role has a narrow direct execution path for `read_temperature_humidity` and publishes `mesh_command_result`.
- Control role still validates remote commands in dry-run mode and does not directly control hardware from the MQTT callback.

Verified on physical boards:

- `/dev/ttyUSB0` has been used as `esp32s3-coordinator-01` / `coordinator_agent`.
- `/dev/ttyUSB1` has now been flashed as `esp32s3-sensor-01` / `sensor_agent`.
- USB0 and USB1 flash/hash verification succeeded on 2026-06-14.
- USB0 boot logs confirmed `coordinator_node: Coordinator role enabled`.
- USB1 boot logs confirmed `sensor_node: Sensor role enabled`; Feishu, LLM, scheduler/proactive, and boot servo demo were skipped by role policy.
- USB1 started presence and environment monitor tasks.
- Latest known coordinator build size: `0x14be80`; latest known sensor build size: `0x14be50`; both leave roughly 35% free in the smallest app partition.
- Current runtime caveat: both boards reported `NO_AP_FOUND` for the configured Wi-Fi SSID during this flash validation, so MQTT state/events were not revalidated in this pass.

Still pending:

- Physical `sensor_agent` boot is now verified, but AHT10/AHT20-backed `read_temperature_humidity` over MQTT is not yet end-to-end verified.
- Coordinator does not yet wait for `mesh_command_result`, correlate `command_id`, and summarize the remote result back to Feishu.
- Control role is not currently flashed on USB1 after this pass; it remains the third-role target.
- Control role does not yet have `command_queue`, `safety_interlock`, `actuator_state`, authorization, or audit-driven hardware execution.
- Basic `tool_use`/`tool_result`/final-reply timeline publication is implemented; persistent trace storage, richer task-decomposition events, and physical P4 display validation are still pending.

Best next engineering step:

- Bring the configured Wi-Fi network online, or update Wi-Fi credentials.
- Send `mesh_send_command` from the Feishu/Coordinator board with `target_role="sensor_agent"` and `action="read_temperature_humidity"`.
- Monitor serial logs and MQTT topics to verify command publication, sensor execution, and `mesh_command_result`.
- After that, implement Coordinator-side result waiting/correlation before enabling real control execution.

## Current Repo State To Be Aware Of

As of 2026-06-15:

- The current working directory still does not expose a normal Git worktree to `git status`; `.git` exists as an empty read-only directory and `git status` reports that this path is not a Git repository.
- In this environment `.git` is mounted as a read-only tmpfs placeholder, so normal in-place `git init` cannot replace it from the sandbox.
- Current push work uses temporary Git metadata outside the project tree: `/tmp/ESP32_AgentMesh.git` with `GIT_DIR=/tmp/ESP32_AgentMesh.git` and `GIT_WORK_TREE=/home/cube/WorkSpace/ESP/ESPAgent`.
- `main` has been pushed to the private `cube1345/ESP32_AgentMesh` repository. The initial firmware snapshot commit is `b6cae3f`.
- Treat source files as the source of truth and avoid relying on plain `git status` in the project root unless the host-side `.git` mount is removed outside this sandbox.
- Do not commit or publish private `main/espagent_secrets.h`.
- Do not commit build output directories.
- Do not revert source changes unless explicitly asked.

## Project Status Summary

- WS2812 control: implemented
- Feishu-triggered hardware control path: implemented
- SGP30 driver and AI-callable read path: implemented
- SGP30 live serial monitoring: implemented and verified
- Servo motor control (GPIO5, LEDC PWM): implemented and registered
- Servo LLM call path: corrected in prompt/schema/guard on 2026-04-27, pending board-side runtime retest
- Servo direct tool path on GPIO5: verified on hardware at 90 degrees
- Wi-Fi reliability for current configured network: working in current validation
- “If unsupported, say unsupported” prompt rule: implemented
- execution-side tool guard: implemented and flashed
- real negative end-to-end unsupported-request test: partially superseded by `inject_msg` availability, but still should be re-run after every prompt/tool change
- GPIO pin allowlist documented in system prompt (prevents LLM hallucination)
- Build ported to ESP-IDF 6.1-dev (cJSON upstream, fixed wifi_manager)
- Mesh command publishing: implemented through `mesh_send_command`
- Four-board role bring-up: `/dev/ttyUSB0-3` map to coordinator, sensor, control, and guardian roles; ESP32-P4/Android now carry the display-terminal role
- Feishu coordinator runtime: restored after the `feishu_ack` stack fix; P2P bot `咕咕嘎嘎！` replies normally again
- Natural-language role routing: verified through Feishu for `读取温湿度` -> `sensor_agent` and `点亮WS2812为蓝色` -> `control_agent`
- Sensor Mesh result path for `read_temperature_humidity`: implemented and verified with real AHT20 readings on USB1
- Coordinator result correlation and async OutputMessage reinjection: implemented as first version for Mesh results
- Guardian policy gate and OutputMessage audit: implemented as first version on USB3
- Automation workflow/rule runtime: implemented; humidity condition rule verified from USB0 -> USB1 AHT20 -> Guardian -> USB2 WS2812
- ESP32-P4 display data stream: MQTT connect/subscribe verified; full live UI binding still needs more runtime proof

## Update Log

### 2026-06-18

- Updated current documentation for the post-Guardian four-role layout:
  - USB0 Coordinator
  - USB1 Sensor
  - USB2 Control
  - USB3 Guardian
  - ESP32-P4/Android Display Terminal
- Verified AHT20 on USB1 `sensor_agent`; typical readings are around `27.x C / 45-46%RH`.
- Updated Sensor telemetry documentation to record AHT20 JSON publishing on `espagent/cube1345/nodes/esp32s3-sensor-01/telemetry`.
- Documented automation runtime:
  - `automation_create_workflow`
  - `automation_create_rule`
  - `automation_list`
  - `automation_remove`
  - persistent storage at `/spiffs/automation.json`
  - background FreeRTOS `automation` task
- Verified humidity condition automation:
  - metric `humidity_percent`
  - threshold `40`
  - Sensor AHT20 humidity around `46.0%`
  - Control WS2812 executed red `rgb=(255,0,0)`
  - temporary test rule removed afterward
- Fixed and documented the automation task stack issue by using `ESPAGENT_AUTOMATION_STACK = 12 * 1024`.

### 2026-06-14

- Built and flashed first-role Coordinator firmware to `/dev/ttyUSB0`:
  - node: `esp32s3-coordinator-01`
  - role: `coordinator_agent`
  - build size: `0x14be80`, smallest app partition free: about 35%
  - esptool connected to ESP32-S3 MAC `28:84:85:54:20:64`
  - bootloader, partition table, OTA data, app, and SPIFFS were written and hash-verified
  - serial boot log confirmed `coordinator_node: Coordinator role enabled`
- Built and flashed second-role Sensor firmware to `/dev/ttyUSB1`:
  - node: `esp32s3-sensor-01`
  - role: `sensor_agent`
  - build size: `0x14be50`, smallest app partition free: about 35%
  - esptool connected to ESP32-S3 MAC `28:84:85:54:d7:f4`
  - bootloader, partition table, OTA data, app, and SPIFFS were written and hash-verified
  - serial boot log confirmed `sensor_node: Sensor role enabled`
  - role policy skipped Feishu, LLM, scheduler/proactive, agent loop, and boot servo demo
  - presence and environment monitor tasks started
- Runtime caveat from this pass:
  - both boards reported `NO_AP_FOUND` for the configured Wi-Fi SSID during serial validation
  - MQTT connection, `state/events`, and `mesh_command_result` were therefore not revalidated after this role reassignment
- Local build-time node profile in `main/espagent_secrets.h` is currently left as `esp32s3-sensor-01` / `sensor_agent`, matching the last firmware built and flashed to USB1.

### 2026-06-09

- Completed the first MQTT bridge for the Feishu communication ESP32:
  - `main/channels/feishu/feishu_bot.c` now publishes `feishu_inbound` audit events to node events, Mesh dispatch, and timeline topics.
  - `main/app/espagent_app.c` now publishes `feishu_outbound` reply events to node events and timeline topics.
  - `main/sensors/sensor_mqtt.c/.h` now exposes `sensor_mqtt_publish_node_event`, lazily creates the publish queue before MQTT connects, and flushes queued publishes once connected.
  - MQTT inbound packet handling now decodes standard multi-byte remaining length instead of assuming one-byte payload lengths.
- Verified with ESP-IDF 6.1-dev `idf.py build`; `build/ESPAgent.bin` generated successfully.
- Added detailed four-role public knowledge to `docs/PUBLIC_KNOWLEDGE_BASE.md`, covering each role's position, responsibilities, inputs, outputs, current progress, limits, and next steps.
- Added SNTP-based time sync for the Feishu/LLM coordinator MCU:
  - new `main/time_sync/time_sync.c/.h`
  - startup calls SNTP after Wi-Fi connects
  - `get_current_time` now returns the synchronized local system clock first
  - fallback HTTP Date source changed away from Google
- Verified again with ESP-IDF 6.1-dev `idf.py build`; `build/ESPAgent.bin` generated successfully.

### 2026-06-01

- Added structured Amap weather support:
  - New `get_weather` tool implemented in `main/tools/tool_amap_weather.c/.h`.
  - Tool supports default-location live weather, forecast weather, direct adcode lookup, and city/district/address geocoding before weather lookup.
  - Default location is configured as `南京市栖霞区` with adcode `320113`.
  - Private Amap WebService key was placed in local `main/espagent_secrets.h`; public examples/docs only contain empty placeholders.
  - Added runtime CLI commands `set_amap_key <key>` and `set_amap_location <location> <adcode>`.
  - `config_show` now reports masked Amap key and default weather location/adcode.
  - `config_reset` now clears the Amap NVS namespace too.
  - System prompt and weather/proactive skills now prefer `get_weather` over generic `web_search` for weather.
  - Verified with `idf.py build`; `build/ESPAgent.bin` generated successfully with size `0x1470f0`.
- Added docs for Amap weather setup in `docs/tool-setup/AMAP_WEATHER_SETUP.md` and linked it from README/tool setup docs.

### 2026-06-05

- Archived the pre-Mesh Phase 1 source state to `tmp/archives/ESPAgent_pre_mesh_phase1_20260605_123752.tar.gz`.
- Added LingShu Agent Mesh Phase 1 node identity defaults:
  - `ESPAGENT_NODE_ID` / `ESPAGENT_SECRET_NODE_ID`, default `esp32s3-edge-01`.
  - `ESPAGENT_NODE_ROLE` / `ESPAGENT_SECRET_NODE_ROLE`, default `edge_agent`.
  - `ESPAGENT_NODE_LOCATION` / `ESPAGENT_SECRET_NODE_LOCATION`, default `南京市栖霞区`.
  - `ESPAGENT_MESH_TOPIC_PREFIX` / `ESPAGENT_SECRET_MESH_TOPIC_PREFIX`, default `espagent`.
- Changed the background MQTT sensor publisher from legacy `sensor/data` framing to Mesh-style topics:
  - `espagent/nodes/<node_id>/state`
  - `espagent/nodes/<node_id>/telemetry`
  - `espagent/nodes/<node_id>/events`
  - `espagent/nodes/<node_id>/command`
  - `espagent/agent/dispatch`
  - `espagent/agent/timeline`
  - `espagent/alerts`
- MQTT telemetry payloads now include `node_id`, `role`, `location`, `type`, and `ts_ms`.
- MQTT publishes node online state and lifecycle events after connect.
- MQTT subscribes command/dispatch/alerts topics for observability only; remote commands are logged and are not executed yet.
- `config_show` now prints node identity and key MQTT Mesh topics; `config_reset` clears the reserved node NVS namespace.
- System prompt now identifies the device as an ESPAgent Edge Agent Node in the planned LingShu Agent Mesh, while explicitly stating the current firmware is still a single ESP32-S3 `agent_loop`, not a full cloud Coordinator or multiple independent LLM agent processes.
- Added `docs/LINGSHU_AGENT_MESH.md` with project name, keywords, application fields, creative concept, architecture layers, current implementation boundary, and next-stage roadmap.
- Updated `README.md`, `docs/ARCHITECTURE.md`, `docs/AGENT_ANALYSIS.md`, and `docs/TODO.md` for Mesh Phase 1.
- Verified with `idf.py build`; `build/ESPAgent.bin` generated successfully with size `0x1478d0`, leaving `0xb8730` bytes free in the smallest app partition.

### 2026-06-09

- Added ESP32 role profile support for four-node LingShu Agent Mesh deployments:
  - `coordinator_agent`
  - `sensor_agent`
  - `control_agent`
  - `display_agent`
- Added `main/node/node_profile.c/.h` to centralize node identity, role, capabilities, responsibilities, and role/capability checks.
- Added build-time profile macros:
  - `ESPAGENT_SECRET_NODE_CAPABILITIES`
  - `ESPAGENT_SECRET_NODE_RESPONSIBILITIES`
  - public aliases `ESPAGENT_NODE_CAPABILITIES` and `ESPAGENT_NODE_RESPONSIBILITIES`
- Added role-targeted MQTT command topic:
  - `espagent/roles/<role>/command`
- MQTT `state`, `event`, and `telemetry` payloads now include node capabilities; state payload also includes responsibilities.
- Non-sensor node profiles no longer reconnect repeatedly because DHT22/MH-Z19 telemetry is unavailable; they publish online state instead.
- `config_show` now prints node capabilities, responsibilities, and role command topic.
- System prompt now includes node capabilities and responsibilities so the LLM can distinguish coordinator/sensor/control/display duties.
- Added `docs/ESP32_ROLE_PROFILES.md` with concrete `espagent_secrets.h` examples for four ESP32-S3 boards.
- Updated `README.md`, `docs/ARCHITECTURE.md`, `docs/AGENT_ANALYSIS.md`, `docs/LINGSHU_AGENT_MESH.md`, and `docs/TODO.md` for four-ESP32 role profile support.
- Verified with explicit ESP-IDF Python environment:
  - `ESP_IDF_VERSION=6.1.0 IDF_PATH=/home/cube/WorkSpace/ESP/esp-idf IDF_PYTHON_ENV_PATH=/home/cube/.espressif/python_env/idf6.1_py3.13_env PATH=/home/cube/.espressif/python_env/idf6.1_py3.13_env/bin:/home/cube/WorkSpace/ESP/esp-idf/tools:$PATH /home/cube/.espressif/python_env/idf6.1_py3.13_env/bin/python /home/cube/WorkSpace/ESP/esp-idf/tools/idf.py build`
  - `build/ESPAgent.bin` generated successfully with size `0x147e40`, leaving `0xb81c0` bytes free in the smallest app partition.
- Added `docs/PUBLIC_KNOWLEDGE_BASE.md` as the comprehensive public knowledge base for the project:
  - project positioning and current single-agent boundary
  - LingShu Agent Mesh state and four-ESP32 role profiles
  - full directory and file responsibility map
  - implemented features and hardware/tool details
  - storage, runtime tasks, current progress, known limitations, and roadmap
- Linked the public knowledge base from `README.md`.
- Updated the public knowledge base with the four-ESP32 resource-usage and code-design plan:
  - recommended architecture: `common runtime + mesh protocol + role-specific service`
  - future module split: `main/mesh`, `main/roles`, `main/sensors`, `main/control`, `main/display`
  - per-node resource focus for coordinator/sensor/control/display boards
  - safe remote-control path: MQTT command -> protocol validation -> command queue -> safety interlock -> actuator state -> driver/tool -> result/timeline event
  - explicit rule that MQTT callbacks must not directly call hardware tools such as `gpio_write`, `servo_write`, or `ws2812_set`
- Updated `docs/ESP32_ROLE_PROFILES.md` with the same resource-use design, safe command path, command struct proposal, and phased module split roadmap.
- Recorded the private GitHub repository URL:
  - `https://github.com/cube1345/ESP32_AgentMesh.git`
  - SSH push URL is `git@github.com:cube1345/ESP32_AgentMesh.git`
- Implemented Agent Mesh Phase 1.5 code architecture:
  - Added `main/mesh/mesh_types.h` and `main/mesh/mesh_protocol.c/.h`.
  - Added `main/roles/role_config.c/.h`.
  - Added role service skeletons:
    - `main/roles/coordinator_node.c/.h`
    - `main/roles/sensor_node.c/.h`
    - `main/roles/control_node.c/.h`
    - `main/roles/display_node.c/.h`
  - `main/app/espagent_app.c` now starts services by role/capability:
    - coordinator/edge: LLM, Feishu/WebSocket chat, scheduler, proactive.
    - sensor/edge: local environment/presence/SGP30 monitoring.
    - control/edge: control-output boundary and boot servo demo.
    - display/edge: display/timeline/alert boundary logs.
  - `main/sensors/sensor_mqtt.c` now parses node/role MQTT command payloads through `mesh_protocol` in dry-run mode.
  - MQTT command validation now checks JSON object shape, required `action`, optional `target_node`, optional `target_role`, `ttl_ms`, `safety_level`, `require_ack`, and `args`/`args_json`.
  - Remote command execution remains disabled until command_queue, safety_interlock, audit, and result/timeline events are implemented.
  - Verified with `idf.py build`; `build/ESPAgent.bin` generated successfully with size `0x1492c0`, leaving `0xb6d40` bytes free in the smallest app partition.
- Updated `docs/PUBLIC_KNOWLEDGE_BASE.md` and `docs/ESP32_ROLE_PROFILES.md` for Mesh Phase 1.5 implementation status.
- Configured local private `main/espagent_secrets.h` for the Feishu/LLM entry board:
  - `ESPAGENT_SECRET_NODE_ID`: `esp32s3-coordinator-01`
  - `ESPAGENT_SECRET_NODE_ROLE`: `coordinator_agent`
  - capabilities: `coordinator,communication,llm,dispatch,timeline,alerts`
  - responsibilities: receive user messages, call LLM, plan dispatch, publish timeline, and notify users
- Fixed role gating so `timeline`/`alerts` capabilities alone no longer enable the display service boundary; display output now requires `display`, `state`, `watchdog`, `display_agent`, or `edge_agent`.
- Built and flashed coordinator firmware to ESP32-S3 on `/dev/ttyUSB0`:
  - detected chip: ESP32-S3, MAC `14:c1:9f:2d:76:20`
  - `build/ESPAgent.bin` size `0x1492b0`, smallest app partition free `0xb6d50`
  - bootloader, partition table, OTA data, app, and SPIFFS were flashed/verified
  - final reset completed via RTS
- Post-flash serial verification confirmed:
  - Feishu credentials loaded
  - LLM proxy initialized
  - agent loop initialized
  - coordinator role enabled as `esp32s3-coordinator-01`
  - boot servo demo skipped for `coordinator_agent`
  - local sensor monitors skipped for `coordinator_agent`
- Investigated the Feishu-visible error response `抱歉，我这次处理请求时遇到了错误。`.
  - Serial monitor showed the Feishu/Wi-Fi/coordinator startup path was healthy.
  - Direct CLI injection reproduced the failure without Feishu: `inject_msg system debug hello`.
  - Root cause: system prompt filled the old 16KB buffer and was truncated in the middle of a UTF-8 multibyte character, causing the OpenAI-compatible LLM API to reject the request with HTTP 400: `messages[0].content: invalid unicode code point`.
  - Fixed by increasing `ESPAGENT_CONTEXT_BUF_SIZE` from 16KB to 24KB and adding UTF-8-safe truncation in `main/agent/context_builder.c` and `main/agent/agent_loop.c`.
  - Built and flashed the fix to `/dev/ttyUSB0`; `build/ESPAgent.bin` size `0x149420`, smallest app partition free `0xb6be0`.
  - Post-flash CLI injection verified success:
    - `System prompt built: 18545 bytes`
    - LLM API returned HTTP 200 JSON response
    - final response queued successfully
    - system output: `你好，Echo 在线。`

### 2026-05-31

- Archived the pre-fix source state to `tmp/archives/ESPAgent_pre_context_fixes_20260531_072213.tar.gz`.
- Hardened `main/agent/context_builder.c` prompt assembly with bounded append helpers so truncated `snprintf` results cannot advance offsets past the end of the context buffer.
- Changed session storage in `main/memory/session_mgr.c` to hash `chat_id` into stable filenames such as `session_<hash>.jsonl`, avoiding long or unsafe raw chat IDs in SPIFFS paths.
- Kept compatibility for existing safe legacy session filenames during read and clear operations.
- Added bounds validation for `session_get_history_json` so `max_msgs` cannot exceed the fixed ring-buffer size.
- Added pending clarification carry-over in `main/agent/agent_loop.c`: if the latest assistant history item looks like a follow-up question, the next user message is framed as a likely answer to that pending clarification.
- Verified the changes with `idf.py build`; `build/ESPAgent.bin` generated successfully with size `0x1453b0`.
- Updated technical docs:
  - `docs/ARCHITECTURE.md` now documents proactive service, cron/proactive inbound injection, hashed session filenames, current message flags, updated startup order, and clarification carry-over.
  - `docs/AGENT_ANALYSIS.md` now explains prompt buffer hardening, session hash storage, pending clarification behavior, proactive checks, and current limitations.
  - `docs/TODO.md` now marks cron scheduling as implemented and records proactive service, session hardening, and clarification carry-over as completed capabilities.

### 2026-04-27

- Document created as the required shared progress file for future AI sessions.
- Recorded current project goals, validated hardware state, build/flash baseline, and repo workflow requirements.
- Recorded that guard-related source changes were present, flashed, and partially validated in that session.

### 2026-04-27 (second session)

- Ported project to build with ESP-IDF 6.1-dev: removed `json` REQUIRES, bundled upstream cJSON v1.7.15 as `cJSON_upstream.c/h`, fixed `wifi_manager.c` for removed enum.
- Added servo motor tool `servo_write` on GPIO5 with LEDC PWM (50Hz, 500-2500us).
- Registered servo in tool_registry (17 total tools at that time; current count is higher).

### 2026-05-30

- Added root `README.md` and updated architecture/roadmap documentation to present ESPAgent as a standalone ESP32-S3 firmware project.
- Refactored startup glue from `main/espagent.c` into `main/app/espagent_app.c/.h`.
- `main/espagent.c` now only prints the startup banner and calls application startup phases.
- Verified `idf.py build` after the refactor; `build/ESPAgent.bin` generated successfully.
- Flashed the current `build/ESPAgent.bin` and SPIFFS image to ESP32-S3 on `/dev/ttyUSB0`; esptool connected to MAC `28:84:85:54:da:5c`, verified all written hashes, and hard-reset the board.
- Fixed LLM hallucination about GPIO5 availability: added explicit allowed pin list (1-18, 21, 38, 46) to system prompt in context_builder.c.
- Build and flash verified successfully on `/dev/ttyUSB0`.
- Confirmed WiFi connection to `Redmi K70`, IP `10.29.203.55`.
- All 17 tools registered and agent loop running.
- Investigated user-reported `Sorry, I encountered an error.` when asking the agent to search online.
- Verified direct `web_search` works with Tavily, and verified full Agent tool-loop can call `web_search` and summarize results through DeepSeek/OpenAI-compatible chat completions.
- Added a `web_search` fallback in `main/agent/agent_loop.c`: if search results are available but a later LLM summarization call fails or returns empty, ESPAgent now returns the collected search results instead of the generic error.
- Added serial diagnostic command `inject_search_zh` in `main/cli/serial_cli.c` to inject a fixed Chinese web-search request without relying on serial console UTF-8 parsing.
- Built and flashed the fix to `/dev/ttyUSB0`; validation passed with `inject_search_zh`, Wi-Fi IP `10.29.63.187`, `web_search` tool calls, and final Chinese summary response.
- Added proactive daily scheduling support for Agent-initiated messages:
  - `cron_add` now supports `schedule_type="daily"` with local `hour` and `minute`.
  - Daily jobs compute the next run using the firmware timezone, now set to `CST-8` for China local time.
  - System prompt now teaches the model to use `cron_add` for daily weather, morning briefings, reminders, and caring check-ins.
  - Added SPIFFS skill `spiffs_data/skills/proactive-care.md`.
  - Verified with `idf.py build`; `build/ESPAgent.bin` generated successfully.
- Added a periodic proactive LLM check service:
  - New `main/proactive/proactive_service.c/.h` stores the latest Feishu/WebSocket contact target in NVS and periodically injects an internal proactive prompt into the Agent loop.
  - Agent loop now marks proactive turns with `ESPAGENT_MSG_FLAG_PROACTIVE`, skips "working" status for those turns, and suppresses outbound messages when the LLM returns `PROACTIVE_NO_MESSAGE`.
  - New CLI commands: `proactive_status`, `proactive_set_target <channel> <chat_id>`, and `proactive_trigger`.
  - Default proactive check interval is 1 hour with a 10 minute initial delay.
  - Verified with `idf.py build`; `build/ESPAgent.bin` generated successfully.
- Strengthened Feishu/WebSocket clarification behavior in `main/agent/context_builder.c`: if a command is missing required details, ambiguous, potentially unsafe, or depends on user preference, the model is instructed to ask one concise follow-up question in the same chat and avoid hardware-control tools until the user answers.
- Verified clarification prompt update with `idf.py build`; `build/ESPAgent.bin` generated successfully.

### 2026-04-27 (third session)

- Fixed `servo_write` tool not being callable by LLM: root cause was `oneOf` in JSON Schema — unsupported by OpenAI/DeepSeek function calling. Changed to standard `required: ["angle"]` in `tool_registry.c`.
- Added Chinese servo keywords to system prompt in `context_builder.c`: 舵机, 旋转, 转动, 角度, 顺时针, 逆时针
- Created `烧录指南.md` — comprehensive flash/burn guide in Chinese covering ESP-IDF setup, two USB ports, `--no-compress` workaround, partition layout, SPIFFS layout, and common issues.
- Built and flashed successfully.

### 2026-04-27 (fourth session)

- Investigated a new issue where the LLM reported servo success but the servo did not visibly move.
- Found prompt/schema consistency gaps:
  - `servo_write` was registered, but missing from the explicit Available Tools list in `context_builder.c`
  - `servo_write` schema still forced `angle`, which made `pulse_us` effectively unavailable to the LLM
  - the new execution-side `tool_guard` had no explicit servo matcher
- Applied fixes:
  - added `servo_write` to the system prompt tool list
  - added servo-specific guard keywords in `agent_loop.c`
  - changed `servo_write` schema to `required: []` so either `angle` or `pulse_us` can be used
  - updated servo implementation logs to include GPIO and PWM duty details
  - aligned `tool_servo.h` comments with the current fixed-pin firmware design
- Local build revalidated successfully with `idf.py build`.
- Direct serial runtime retest was attempted but initially blocked because `/dev/ttyUSB0` was busy from a stale external `screen` holder.
- The stale `screen` holder (`PID 365239`) was cleared, then firmware was rebuilt and flashed successfully to `/dev/ttyUSB0`.
- Flash completed with verified hashes and hard reset.

### 2026-04-27 (fifth session)

- Confirmed again that the servo GPIO in firmware is `GPIO5` via `ESPAGENT_SERVO_DEFAULT_GPIO`.
- Bypassed the LLM and invoked the same registered tool function through serial CLI:
  - `tool_exec servo_write {"angle":90}`
- Real device logs confirmed the tool executed and PWM was updated:
  - `Servo PWM updated on GPIO 5: pulse=1500us duty=614/8191`
  - `tool_exec status: ESP_OK`
  - `OK: servo on GPIO5 set to 90 degrees (pulse=1500us)`

### 2026-04-27 (sixth session)

- Ran a wider direct serial servo sweep to separate “same-angle no visible movement” from “no hardware drive”.
- Commands executed:
  - `tool_exec servo_write {"angle":30}`
  - `tool_exec servo_write {"angle":150}`
  - `tool_exec servo_write {"angle":90}`
- Firmware logs confirmed three distinct PWM outputs on `GPIO5`:
  - `833us` (`duty=341/8191`)
  - `2166us` (`duty=887/8191`)
  - `1500us` (`duty=614/8191`)

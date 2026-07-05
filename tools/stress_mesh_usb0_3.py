#!/usr/bin/env python3
"""USB0-USB3 ESPAgent Mesh pressure test.

This script intentionally uses only /dev/ttyUSB0-3. /dev/ttyACM* is ignored.
It sends MQTT Mesh commands through the coordinator serial CLI and watches all
four serial logs for routing, execution, result publication, and crashes.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import sys
import termios
import time
from dataclasses import dataclass, field


PORTS = [f"/dev/ttyUSB{i}" for i in range(4)]
EXPECTED_ROLES = {
    "/dev/ttyUSB0": "coordinator_agent",
    "/dev/ttyUSB1": "sensor_agent",
    "/dev/ttyUSB2": "control_agent",
    "/dev/ttyUSB3": "guardian_agent",
}
CRASH_PATTERNS = (
    "Guru Meditation",
    "panic",
    "abort()",
    "assert failed",
    "Backtrace:",
    "stack canary",
    "StoreProhibited",
    "LoadProhibited",
)


@dataclass
class PortState:
    port: str
    fd: int
    buffer: str = ""
    text: str = ""
    role: str | None = None
    node_id: str | None = None
    lines: list[str] = field(default_factory=list)


@dataclass
class Metrics:
    sent: int = 0
    queued_ok: int = 0
    queued_error: int = 0
    sensor_received: int = 0
    sensor_executed: int = 0
    control_received: int = 0
    control_executed: int = 0
    command_results: int = 0
    policy_checks: int = 0
    policy_decisions: int = 0
    guardian_audits: int = 0
    mqtt_state: int = 0
    mqtt_inbound: int = 0
    crashes: int = 0
    errors: int = 0
    warnings: int = 0


def configure_serial(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def open_ports() -> dict[int, PortState]:
    missing = [port for port in PORTS if not os.path.exists(port)]
    if missing:
        print("ERROR: missing required ESP32-S3 ttyUSB ports:", ", ".join(missing), file=sys.stderr)
        print("ERROR: this test only uses /dev/ttyUSB0-3 and will not use /dev/ttyACM*.", file=sys.stderr)
        sys.exit(2)

    states: dict[int, PortState] = {}
    for port in PORTS:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        configure_serial(fd)
        states[fd] = PortState(port=port, fd=fd)
        try:
            os.read(fd, 65535)
        except BlockingIOError:
            pass
    return states


def close_ports(states: dict[int, PortState]) -> None:
    for fd in list(states):
        try:
            os.close(fd)
        except OSError:
            pass


def write_line(state: PortState, line: str) -> None:
    os.write(state.fd, line.encode("utf-8"))


def drain(states: dict[int, PortState], seconds: float, echo: bool, metrics: Metrics | None = None) -> None:
    end = time.time() + seconds
    while time.time() < end:
        readable, _, _ = select.select(list(states), [], [], 0.2)
        for fd in readable:
            state = states[fd]
            try:
                data = os.read(fd, 8192)
            except BlockingIOError:
                data = b""
            if not data:
                continue
            chunk = data.decode("utf-8", "replace")
            state.text += chunk
            state.buffer += chunk
            complete_lines = state.buffer.splitlines(keepends=True)
            if complete_lines and not complete_lines[-1].endswith(("\n", "\r")):
                state.buffer = complete_lines.pop()
            else:
                state.buffer = ""
            for line in complete_lines:
                line = line.strip()
                if not line:
                    continue
                state.lines.append(line)
                if metrics:
                    classify_line(state, line, metrics)
                if echo and interesting(line):
                    print(f"{state.port}: {line}")


def interesting(line: str) -> bool:
    keys = (
        "Node ID",
        "Node Role",
        "MQTT publish",
        "MQTT inbound",
        "Mesh role command",
        "Mesh command received",
        "Mesh sensor command executed",
        "Mesh control command executed",
        "mesh_command_result",
        "tool_exec",
        "OK: queued MQTT mesh command",
        "Error:",
        "W (",
        "E (",
    )
    return any(key in line for key in keys) or any(key in line for key in CRASH_PATTERNS)


def classify_line(state: PortState, line: str, metrics: Metrics) -> None:
    if "OK: queued MQTT mesh command" in line:
        metrics.queued_ok += 1
    if "Error:" in line and "queued MQTT mesh command" in line:
        metrics.queued_error += 1
    if "Mesh role command received for sensor_agent" in line:
        metrics.sensor_received += 1
    if "Mesh sensor command executed" in line:
        metrics.sensor_executed += 1
    if "Mesh role command received for control_agent" in line:
        metrics.control_received += 1
    if "Mesh control command executed" in line:
        metrics.control_executed += 1
    if "Policy check received" in line:
        metrics.policy_checks += 1
        metrics.mqtt_inbound += 1
    if "Guardian policy decision:" in line:
        metrics.policy_decisions += 1
        metrics.mqtt_inbound += 1
    if "Guardian audited timeline event:" in line:
        metrics.guardian_audits += 1
    if "mesh_command_result" in line:
        metrics.command_results += 1
    if "MQTT publish" in line and "/state:" in line:
        metrics.mqtt_state += 1
    if "MQTT inbound" in line or "Mesh dispatch received" in line or "Mesh alert received" in line:
        metrics.mqtt_inbound += 1
    if "W (" in line:
        metrics.warnings += 1
    if "E (" in line:
        metrics.errors += 1
    if any(pattern in line for pattern in CRASH_PATTERNS):
        metrics.crashes += 1


def request_config(states: dict[int, PortState]) -> None:
    for state in states.values():
        write_line(state, "\nconfig_show\n")
    drain(states, 7, echo=False)
    for state in states.values():
        role_match = re.search(r"Node Role\s+:\s+([^\s]+)", state.text)
        node_match = re.search(r"Node ID\s+:\s+([^\s]+)", state.text)
        state.role = role_match.group(1) if role_match else None
        state.node_id = node_match.group(1) if node_match else None


def build_commands(rounds: int, run_id: str) -> list[str]:
    colors = ["blue", "green", "red", "cyan", "purple", "white"]
    commands: list[str] = []
    for i in range(1, rounds + 1):
        sensor_cmd = {
            "action": "read_temperature_humidity",
            "command_id": f"{run_id}-sensor-{i}",
            "args": {},
        }
        control_cmd = {
            "action": "set_status_light",
            "command_id": f"{run_id}-control-{i}",
            "args": {"color": colors[(i - 1) % len(colors)]},
            "safety_level": 1,
        }
        commands.append("tool_exec mesh_send_command " + json.dumps(sensor_cmd, separators=(",", ":")) + "\n")
        commands.append("tool_exec mesh_send_command " + json.dumps(control_cmd, separators=(",", ":")) + "\n")
    return commands


def count_ids_in_lines(lines: list[str], prefix: str, rounds: int, marker: str) -> int:
    count = 0
    for i in range(1, rounds + 1):
        command_id = f"{prefix}-{i}"
        if any(marker in line and command_id in line for line in lines):
            count += 1
    return count


def count_command_ids_in_lines(lines: list[str], command_ids: list[str], marker: str) -> int:
    return sum(1 for command_id in command_ids
               if any(marker in line and command_id in line for line in lines))


def missing_ids_in_lines(lines: list[str], command_ids: list[str], marker: str) -> list[str]:
    return [command_id for command_id in command_ids
            if not any(marker in line and command_id in line for line in lines)]


def id_near_marker(text: str, command_id: str, marker: str, window: int = 4096) -> bool:
    id_positions = [m.start() for m in re.finditer(re.escape(command_id), text)]
    marker_positions = [m.start() for m in re.finditer(re.escape(marker), text)]
    return bool(id_positions and marker_positions and
                any(abs(id_pos - marker_pos) <= window
                    for id_pos in id_positions
                    for marker_pos in marker_positions))


def command_id_from_tool_command(command: str) -> str:
    try:
        payload = command.split(" ", 2)[2]
        data = json.loads(payload)
        value = data.get("command_id", "")
        return value if isinstance(value, str) else ""
    except (IndexError, json.JSONDecodeError, TypeError):
        return ""


def coordinator_ack_seen(coordinator: PortState, command_id: str) -> bool:
    if not command_id:
        return True
    markers = (
        "OK: queued MQTT mesh command",
        "Error: Guardian policy blocked mesh command",
        "Error: MQTT is not connected for mesh dispatch",
        "Error: failed to queue MQTT mesh command",
        "tool_exec status:",
    )
    return any(id_near_marker(coordinator.text, command_id, marker) for marker in markers)


def drain_until_command_ack(states: dict[int, PortState],
                            coordinator: PortState,
                            command_id: str,
                            timeout_s: float,
                            echo: bool,
                            metrics: Metrics) -> None:
    end = time.time() + timeout_s
    while time.time() < end:
        drain(states, 0.2, echo=echo, metrics=metrics)
        if coordinator_ack_seen(coordinator, command_id):
            return


def final_metrics(states: dict[int, PortState], rounds: int, live: Metrics, run_id: str) -> Metrics:
    result = Metrics(
        sent=live.sent,
        warnings=live.warnings,
        errors=live.errors,
        crashes=live.crashes,
    )
    usb0_state = next(s for s in states.values() if s.port == "/dev/ttyUSB0")
    usb1_state = next(s for s in states.values() if s.port == "/dev/ttyUSB1")
    usb2_state = next(s for s in states.values() if s.port == "/dev/ttyUSB2")
    usb0 = usb0_state.text
    usb1 = usb1_state.text
    usb2 = usb2_state.text
    all_text = "\n".join(s.text for s in states.values())
    command_ids = (
        [f"{run_id}-sensor-{i}" for i in range(1, rounds + 1)]
        + [f"{run_id}-control-{i}" for i in range(1, rounds + 1)]
    )

    queued_markers = (
        "OK: queued MQTT mesh command",
        "OK: mesh command completed",
    )
    result.queued_ok = sum(
        1 for command_id in command_ids
        if any(marker in line and command_id in line
               for line in usb0_state.lines
               for marker in queued_markers)
    )
    result.queued_error = usb0.count("Error: failed to queue MQTT mesh command")
    result.sensor_received = count_ids_in_lines(usb1_state.lines, f"{run_id}-sensor", rounds, "Mesh role command received for sensor_agent")
    result.sensor_executed = count_ids_in_lines(usb1_state.lines, f"{run_id}-sensor", rounds, "Mesh sensor command executed")
    result.control_received = count_ids_in_lines(usb2_state.lines, f"{run_id}-control", rounds, "Mesh role command received for control_agent")
    result.control_executed = count_ids_in_lines(usb2_state.lines, f"{run_id}-control", rounds, "Mesh control command executed")
    usb3_state = next(s for s in states.values() if s.port == "/dev/ttyUSB3")
    result.policy_checks = count_command_ids_in_lines(usb3_state.lines, command_ids, "Policy check received")
    result.policy_decisions = count_command_ids_in_lines(usb3_state.lines, command_ids, "Guardian policy decision:")
    result.guardian_audits = count_command_ids_in_lines(usb3_state.lines, command_ids, "Guardian audited timeline event:")
    result.command_results = all_text.count("mesh_command_result")
    result.mqtt_state = all_text.count('"state":"online"')
    result.mqtt_inbound = all_text.count("MQTT inbound") + all_text.count("Mesh dispatch received") + all_text.count("Mesh alert received")
    return result


def print_config_summary(states: dict[int, PortState]) -> bool:
    print("===== role check =====")
    ok = True
    for port in PORTS:
        state = next(s for s in states.values() if s.port == port)
        expected = EXPECTED_ROLES[port]
        status = "OK" if state.role == expected else "MISMATCH"
        if status != "OK":
            ok = False
        print(f"{port}: node={state.node_id or 'unknown'} role={state.role or 'unknown'} expected={expected} {status}")
    return ok


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=5, help="sensor/control command rounds")
    parser.add_argument("--interval", type=float, default=1.5, help="seconds between commands")
    parser.add_argument("--settle", type=float, default=25.0, help="extra seconds after last command")
    parser.add_argument("--quiet", action="store_true", help="do not print live interesting lines")
    parser.add_argument("--run-id", default="", help="short unique command-id prefix; generated when omitted")
    args = parser.parse_args()

    if args.rounds < 1:
        print("ERROR: --rounds must be >= 1", file=sys.stderr)
        return 2

    states = open_ports()
    metrics = Metrics()
    try:
        print("===== ESPAgent USB0-USB3 Mesh pressure test =====")
        print("Ports: /dev/ttyUSB0 coordinator, /dev/ttyUSB1 sensor, /dev/ttyUSB2 control, /dev/ttyUSB3 guardian")
        print("ACM policy: ignored by design")

        request_config(states)
        roles_ok = print_config_summary(states)

        coordinator = next(s for s in states.values() if s.port == "/dev/ttyUSB0")
        run_id = args.run_id.strip() or f"st{int(time.time() * 1000) & 0xffffffff:08x}"
        commands = build_commands(args.rounds, run_id)
        command_ids = (
            [f"{run_id}-sensor-{i}" for i in range(1, args.rounds + 1)]
            + [f"{run_id}-control-{i}" for i in range(1, args.rounds + 1)]
        )
        print("===== command phase =====")
        print(f"run_id={run_id}")
        print(f"Sending {len(commands)} mesh commands from USB0: {args.rounds} sensor + {args.rounds} control")

        for index, command in enumerate(commands, start=1):
            metrics.sent += 1
            print(f"SEND {index}/{len(commands)}: {command.strip()}")
            write_line(coordinator, command)
            command_id = command_id_from_tool_command(command)
            drain_until_command_ack(states,
                                    coordinator,
                                    command_id,
                                    max(args.interval, 8.0),
                                    echo=not args.quiet,
                                    metrics=metrics)

        print(f"===== settle {args.settle:.1f}s =====")
        drain(states, args.settle, echo=not args.quiet, metrics=metrics)

        expected_each = args.rounds
        metrics = final_metrics(states, args.rounds, metrics, run_id)
        print("===== metrics =====")
        print(f"sent={metrics.sent}")
        print(f"queued_ok={metrics.queued_ok} queued_error={metrics.queued_error}")
        print(f"sensor_received={metrics.sensor_received} sensor_executed={metrics.sensor_executed} expected={expected_each}")
        print(f"control_received={metrics.control_received} control_executed={metrics.control_executed} expected={expected_each}")
        print(f"policy_checks={metrics.policy_checks} policy_decisions={metrics.policy_decisions} guardian_audits={metrics.guardian_audits}")
        print(f"mesh_command_result_lines={metrics.command_results}")
        print(f"mqtt_state_lines={metrics.mqtt_state} mqtt_inbound_lines={metrics.mqtt_inbound}")
        print(f"warnings={metrics.warnings} errors={metrics.errors} crashes={metrics.crashes}")

        missing_queued = [
            command_id for command_id in command_ids
            if not any(marker in line and command_id in line
                       for line in coordinator.lines
                       for marker in ("OK: queued MQTT mesh command", "OK: mesh command completed"))
        ]
        missing_sensor_received = missing_ids_in_lines(
            next(s for s in states.values() if s.port == "/dev/ttyUSB1").lines,
            [f"{run_id}-sensor-{i}" for i in range(1, args.rounds + 1)],
            "Mesh role command received for sensor_agent")
        missing_sensor_executed = missing_ids_in_lines(
            next(s for s in states.values() if s.port == "/dev/ttyUSB1").lines,
            [f"{run_id}-sensor-{i}" for i in range(1, args.rounds + 1)],
            "Mesh sensor command executed")
        missing_control_received = missing_ids_in_lines(
            next(s for s in states.values() if s.port == "/dev/ttyUSB2").lines,
            [f"{run_id}-control-{i}" for i in range(1, args.rounds + 1)],
            "Mesh role command received for control_agent")
        missing_control_executed = missing_ids_in_lines(
            next(s for s in states.values() if s.port == "/dev/ttyUSB2").lines,
            [f"{run_id}-control-{i}" for i in range(1, args.rounds + 1)],
            "Mesh control command executed")
        missing_policy_checks = missing_ids_in_lines(
            next(s for s in states.values() if s.port == "/dev/ttyUSB3").lines,
            command_ids,
            "Policy check received")
        missing_policy_decisions = missing_ids_in_lines(
            next(s for s in states.values() if s.port == "/dev/ttyUSB3").lines,
            command_ids,
            "Guardian policy decision:")
        missing_guardian_audits = missing_ids_in_lines(
            next(s for s in states.values() if s.port == "/dev/ttyUSB3").lines,
            command_ids,
            "Guardian audited timeline event:")
        if any((missing_queued,
                missing_sensor_received,
                missing_sensor_executed,
                missing_control_received,
                missing_control_executed,
                missing_policy_checks,
                missing_policy_decisions,
                missing_guardian_audits)):
            print("===== missing ids =====")
            print(f"queued={missing_queued}")
            print(f"sensor_received={missing_sensor_received}")
            print(f"sensor_executed={missing_sensor_executed}")
            print(f"control_received={missing_control_received}")
            print(f"control_executed={missing_control_executed}")
            print(f"policy_checks={missing_policy_checks}")
            print(f"policy_decisions={missing_policy_decisions}")
            print(f"guardian_audits={missing_guardian_audits}")

        pass_basic = (
            roles_ok
            and metrics.sent == len(commands)
            and metrics.queued_ok >= len(commands)
            and metrics.sensor_received >= expected_each
            and metrics.sensor_executed >= expected_each
            and metrics.control_received >= expected_each
            and metrics.control_executed >= expected_each
            and metrics.policy_checks >= len(commands)
            and metrics.policy_decisions >= len(commands)
            and metrics.guardian_audits >= len(commands)
            and metrics.errors == 0
            and metrics.crashes == 0
        )
        print("RESULT:", "PASS" if pass_basic else "FAIL")
        return 0 if pass_basic else 1
    finally:
        close_ports(states)


if __name__ == "__main__":
    raise SystemExit(main())

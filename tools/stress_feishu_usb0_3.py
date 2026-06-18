#!/usr/bin/env python3
"""Feishu-entry ESPAgent pressure test.

This test sends real Feishu messages to the ESPAgent bot chat and monitors
/dev/ttyUSB0-3. It intentionally ignores /dev/ttyACM*.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import subprocess
import sys
import termios
import time
import uuid
from dataclasses import dataclass, field
from pathlib import Path


PORTS = [f"/dev/ttyUSB{i}" for i in range(4)]
DEFAULT_CHAT_ID = "oc_9771f831cad2fffc28239bd313cd77e1"
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
class SentMessage:
    tag: str
    text: str
    ok: bool
    message_id: str = ""
    error: str = ""


@dataclass
class Metrics:
    sent_ok: int = 0
    sent_failed: int = 0
    feishu_user_seen: int = 0
    processing_turns: int = 0
    working_status_sent: int = 0
    final_responses: int = 0
    async_result_turns: int = 0
    feishu_send_ok: int = 0
    llm_tool_mesh: int = 0
    queued_mesh: int = 0
    sensor_received: int = 0
    sensor_executed: int = 0
    control_received: int = 0
    control_executed: int = 0
    mesh_results: int = 0
    warnings: int = 0
    errors: int = 0
    crashes: int = 0


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


def drain(states: dict[int, PortState], seconds: float, echo: bool) -> None:
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
                if echo and interesting(line):
                    print(f"{state.port}: {line}")


def interesting(line: str) -> bool:
    keys = (
        "FS-STRESS-",
        "Processing message from feishu",
        "=== CONV ===",
        "Tool use iteration",
        "mesh_send_command",
        "OK: queued MQTT mesh command",
        "Queue final response",
        "Feishu send success",
        "Feishu send failed",
        "Mesh role command received",
        "Mesh sensor command executed",
        "Mesh control command executed",
        "mesh_command_result",
        "Inbound queue full",
        "Outbound queue full",
        "LLM call failed",
        "W (",
        "E (",
    )
    return any(key in line for key in keys) or any(key in line for key in CRASH_PATTERNS)


def request_config(states: dict[int, PortState]) -> None:
    for state in states.values():
        write_line(state, "\nconfig_show\n")
    drain(states, 7, echo=False)
    for state in states.values():
        role_match = re.search(r"Node Role\s+:\s+([^\s]+)", state.text)
        node_match = re.search(r"Node ID\s+:\s+([^\s]+)", state.text)
        state.role = role_match.group(1) if role_match else None
        state.node_id = node_match.group(1) if node_match else None


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


def send_feishu(chat_id: str, text: str, tag: str, timeout: int) -> SentMessage:
    cmd = [
        "lark-cli",
        "im",
        "+messages-send",
        "--as",
        "user",
        "--chat-id",
        chat_id,
        "--text",
        text,
        "--idempotency-key",
        f"espagent-{tag}-{uuid.uuid4().hex[:8]}",
        "--json",
    ]
    try:
        proc = subprocess.run(cmd, check=False, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return SentMessage(tag=tag, text=text, ok=False, error="lark-cli timeout")

    if proc.returncode != 0:
        return SentMessage(tag=tag, text=text, ok=False, error=(proc.stderr or proc.stdout).strip())

    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return SentMessage(tag=tag, text=text, ok=False, error=f"invalid lark-cli JSON: {proc.stdout[:200]}")

    if not data.get("ok"):
        return SentMessage(tag=tag, text=text, ok=False, error=json.dumps(data.get("error", data), ensure_ascii=False))

    message_id = ""
    payload = data.get("data") or {}
    if isinstance(payload, dict):
        message = payload.get("message") or payload
        if isinstance(message, dict):
            message_id = str(message.get("message_id") or "")
    return SentMessage(tag=tag, text=text, ok=True, message_id=message_id)


def build_messages(run_id: str, rounds: int) -> list[tuple[str, str]]:
    colors = ["蓝色", "绿色", "红色", "青色", "紫色", "白色"]
    messages: list[tuple[str, str]] = []
    for i in range(1, rounds + 1):
        sensor_tag = f"FS-STRESS-{run_id}-S{i}"
        control_tag = f"FS-STRESS-{run_id}-C{i}"
        messages.append((sensor_tag, f"{sensor_tag}：读取温湿度。"))
        messages.append((control_tag, f"{control_tag}：把远程控制板的 WS2812 状态灯设置为{colors[(i - 1) % len(colors)]}。"))
    return messages


def count_tags_near(text: str, tags: list[str], marker: str, window: int = 1600) -> int:
    count = 0
    marker_positions = [m.start() for m in re.finditer(re.escape(marker), text)]
    if not marker_positions:
        return 0
    for tag in tags:
        tag_positions = [m.start() for m in re.finditer(re.escape(tag), text)]
        if any(abs(tag_pos - marker_pos) <= window for tag_pos in tag_positions for marker_pos in marker_positions):
            count += 1
    return count


def write_artifacts(states: dict[int, PortState], run_id: str, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for state in states.values():
        name = state.port.rsplit("/", 1)[-1]
        (out_dir / f"feishu_stress_{run_id}_{name}.log").write_text(state.text, encoding="utf-8", errors="replace")


def final_metrics(states: dict[int, PortState], sent: list[SentMessage], sensor_tags: list[str], control_tags: list[str]) -> Metrics:
    usb0 = next(s for s in states.values() if s.port == "/dev/ttyUSB0").text
    usb1 = next(s for s in states.values() if s.port == "/dev/ttyUSB1").text
    usb2 = next(s for s in states.values() if s.port == "/dev/ttyUSB2").text
    all_text = "\n".join(s.text for s in states.values())
    all_tags = sensor_tags + control_tags

    metrics = Metrics()
    metrics.sent_ok = sum(1 for item in sent if item.ok)
    metrics.sent_failed = sum(1 for item in sent if not item.ok)
    metrics.feishu_user_seen = sum(1 for tag in all_tags if tag in usb0 and ">> USER" in usb0)
    metrics.processing_turns = count_tags_near(usb0, all_tags, "Processing message from feishu")
    metrics.working_status_sent = usb0.count("Feishu send success") - count_tags_near(usb0, all_tags, "Queue final response")
    metrics.final_responses = usb0.count("Queue final response to feishu:")
    metrics.async_result_turns = usb0.count("Internal async Mesh result.")
    metrics.feishu_send_ok = usb0.count("Feishu send success")
    metrics.llm_tool_mesh = usb0.count("mesh_send_command")
    metrics.queued_mesh = usb0.count("OK: queued MQTT mesh command")
    metrics.sensor_received = usb1.count("Mesh role command received for sensor_agent")
    metrics.sensor_executed = usb1.count("Mesh sensor command executed")
    metrics.control_received = usb2.count("Mesh role command received for control_agent")
    metrics.control_executed = usb2.count("Mesh control command executed")
    metrics.mesh_results = all_text.count("mesh_command_result")
    metrics.warnings = all_text.count("W (")
    metrics.errors = all_text.count("E (")
    metrics.crashes = sum(all_text.count(pattern) for pattern in CRASH_PATTERNS)
    return metrics


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    parser = argparse.ArgumentParser()
    parser.add_argument("--chat-id", default=DEFAULT_CHAT_ID)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--interval", type=float, default=25.0)
    parser.add_argument("--settle", type=float, default=120.0)
    parser.add_argument("--send-timeout", type=int, default=20)
    parser.add_argument("--artifact-dir", default="artifacts/feishu_stress")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.rounds < 1:
        print("ERROR: --rounds must be >= 1", file=sys.stderr)
        return 2

    run_id = time.strftime("%H%M%S")
    messages = build_messages(run_id, args.rounds)
    sensor_tags = [tag for tag, _ in messages if "-S" in tag]
    control_tags = [tag for tag, _ in messages if "-C" in tag]

    states = open_ports()
    sent: list[SentMessage] = []
    try:
        print("===== ESPAgent Feishu-entry pressure test =====")
        print(f"chat_id={args.chat_id}")
        print("Ports: /dev/ttyUSB0 coordinator, /dev/ttyUSB1 sensor, /dev/ttyUSB2 control, /dev/ttyUSB3 guardian")
        print("ACM policy: ignored by design")

        request_config(states)
        roles_ok = print_config_summary(states)

        print("===== send phase =====")
        print(f"Sending {len(messages)} Feishu messages: {args.rounds} sensor + {args.rounds} control")
        for index, (tag, text) in enumerate(messages, start=1):
            print(f"SEND {index}/{len(messages)} {tag}: {text}")
            result = send_feishu(args.chat_id, text, tag, args.send_timeout)
            sent.append(result)
            if result.ok:
                print(f"LARK OK {tag} message_id={result.message_id or '(unknown)'}")
            else:
                print(f"LARK ERROR {tag}: {result.error}")
            drain(states, args.interval, echo=not args.quiet)

        print(f"===== settle {args.settle:.1f}s =====")
        drain(states, args.settle, echo=not args.quiet)
        write_artifacts(states, run_id, Path(args.artifact_dir))

        metrics = final_metrics(states, sent, sensor_tags, control_tags)
        expected_each = args.rounds
        expected_total = len(messages)

        print("===== metrics =====")
        print(f"sent_ok={metrics.sent_ok} sent_failed={metrics.sent_failed} expected_total={expected_total}")
        print(f"feishu_user_seen={metrics.feishu_user_seen} processing_turns={metrics.processing_turns}")
        print(f"final_responses={metrics.final_responses} async_result_turns={metrics.async_result_turns} feishu_send_ok={metrics.feishu_send_ok}")
        print(f"llm_tool_mesh_mentions={metrics.llm_tool_mesh} queued_mesh={metrics.queued_mesh}")
        print(f"sensor_received={metrics.sensor_received} sensor_executed={metrics.sensor_executed} expected={expected_each}")
        print(f"control_received={metrics.control_received} control_executed={metrics.control_executed} expected={expected_each}")
        print(f"mesh_command_result_lines={metrics.mesh_results}")
        print(f"warnings={metrics.warnings} errors={metrics.errors} crashes={metrics.crashes}")
        print(f"artifacts={args.artifact_dir}/feishu_stress_{run_id}_ttyUSB*.log")

        passed = (
            roles_ok
            and metrics.sent_failed == 0
            and metrics.sent_ok == expected_total
            and metrics.processing_turns >= expected_total
            and metrics.final_responses >= expected_total
            and metrics.async_result_turns >= expected_total
            and metrics.sensor_received >= expected_each
            and metrics.sensor_executed >= expected_each
            and metrics.control_received >= expected_each
            and metrics.control_executed >= expected_each
            and metrics.crashes == 0
        )
        print("RESULT:", "PASS" if passed else "FAIL")
        return 0 if passed else 1
    finally:
        close_ports(states)


if __name__ == "__main__":
    raise SystemExit(main())

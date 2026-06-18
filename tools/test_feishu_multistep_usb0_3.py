#!/usr/bin/env python3
"""Feishu-entry multi-step reasoning test for ESPAgent.

The script sends ordered light-control prompts through the real Feishu chat and
monitors /dev/ttyUSB0-3 only. /dev/ttyACM* is intentionally ignored.
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
        print("ERROR: missing ESP32-S3 ports:", ", ".join(missing), file=sys.stderr)
        print("ERROR: this test uses only /dev/ttyUSB0-3.", file=sys.stderr)
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


def interesting(line: str) -> bool:
    keys = (
        "MS-",
        "Processing message from feishu",
        "Tool use iteration",
        "Executing tool:",
        "automation_create_workflow",
        "automation_create_rule",
        "mesh_send_command",
        "OK: queued MQTT mesh command",
        "Queue final response",
        "Feishu send success",
        "LLM call failed",
        "Mesh role command received",
        "Mesh control command executed",
        "tool_gpio: ws2812",
        "policy_check",
        "Guardian",
        "mesh_command_result",
        "workflow",
        "Automation",
        "W (",
        "E (",
    )
    return any(key in line for key in keys) or any(key in line for key in CRASH_PATTERNS)


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
            lines = state.buffer.splitlines(keepends=True)
            if lines and not lines[-1].endswith(("\n", "\r")):
                state.buffer = lines.pop()
            else:
                state.buffer = ""
            for line in lines:
                line = line.strip()
                if not line:
                    continue
                state.lines.append(line)
                if echo and interesting(line):
                    print(f"{state.port}: {line}")


def request_config(states: dict[int, PortState]) -> bool:
    for state in states.values():
        write_line(state, "\nconfig_show\n")
    drain(states, 7, echo=False)
    ok = True
    print("===== role check =====")
    for port in PORTS:
        state = next(s for s in states.values() if s.port == port)
        role_match = re.search(r"Node Role\s+:\s+([^\s]+)", state.text)
        node_match = re.search(r"Node ID\s+:\s+([^\s]+)", state.text)
        state.role = role_match.group(1) if role_match else None
        state.node_id = node_match.group(1) if node_match else None
        expected = EXPECTED_ROLES[port]
        status = "OK" if state.role == expected else "MISMATCH"
        ok = ok and status == "OK"
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
        f"espagent-multistep-{tag}-{uuid.uuid4().hex[:8]}",
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
        return SentMessage(tag=tag, text=text, ok=False, error=f"invalid JSON: {proc.stdout[:200]}")
    if not data.get("ok"):
        return SentMessage(tag=tag, text=text, ok=False, error=json.dumps(data.get("error", data), ensure_ascii=False))

    payload = data.get("data") or {}
    message = payload.get("message") if isinstance(payload, dict) else {}
    message_id = str(message.get("message_id") or "") if isinstance(message, dict) else ""
    return SentMessage(tag=tag, text=text, ok=True, message_id=message_id)


def build_messages(run_id: str) -> list[tuple[str, str]]:
    return [
        (
            f"MS-{run_id}-SEQ",
            f"MS-{run_id}-SEQ：请把远程控制板的 WS2812 状态灯先闪紫色，再闪绿色。",
        ),
        (
            f"MS-{run_id}-DELAY",
            f"MS-{run_id}-DELAY：请把远程控制板的 WS2812 状态灯点亮红色，10秒后切换为蓝色。",
        ),
    ]


def write_artifacts(states: dict[int, PortState], run_id: str, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for state in states.values():
        name = state.port.rsplit("/", 1)[-1]
        (out_dir / f"feishu_multistep_{run_id}_{name}.log").write_text(state.text, encoding="utf-8", errors="replace")


def count_metrics(states: dict[int, PortState], sent: list[SentMessage]) -> dict[str, int]:
    usb0 = next(s for s in states.values() if s.port == "/dev/ttyUSB0").text
    usb2 = next(s for s in states.values() if s.port == "/dev/ttyUSB2").text
    usb3 = next(s for s in states.values() if s.port == "/dev/ttyUSB3").text
    all_text = "\n".join(s.text for s in states.values())
    return {
        "sent_ok": sum(1 for item in sent if item.ok),
        "sent_failed": sum(1 for item in sent if not item.ok),
        "processing_turns": usb0.count("Processing message from feishu"),
        "tool_iterations": usb0.count("Tool use iteration"),
        "automation_mentions": usb0.count("automation_create_workflow"),
        "mesh_mentions": usb0.count("mesh_send_command"),
        "queued_mesh": usb0.count("OK: queued MQTT mesh command"),
        "control_received": usb2.count("Mesh role command received for control_agent"),
        "control_executed": usb2.count("Mesh control command executed"),
        "ws2812_writes": usb2.count("tool_gpio: ws2812"),
        "guardian_policy": usb3.count("policy_decision") + usb3.count("policy_check"),
        "final_responses": usb0.count("Queue final response to feishu:"),
        "feishu_send_ok": usb0.count("Feishu send success"),
        "errors": all_text.count("E ("),
        "crashes": sum(all_text.count(pattern) for pattern in CRASH_PATTERNS),
    }


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)

    parser = argparse.ArgumentParser()
    parser.add_argument("--chat-id", default=DEFAULT_CHAT_ID)
    parser.add_argument("--send-timeout", type=int, default=20)
    parser.add_argument("--interval", type=float, default=35.0)
    parser.add_argument("--settle", type=float, default=90.0)
    parser.add_argument("--artifact-dir", default="artifacts/feishu_multistep")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    run_id = time.strftime("%H%M%S")
    messages = build_messages(run_id)
    states = open_ports()
    sent: list[SentMessage] = []
    try:
        print("===== ESPAgent Feishu multi-step test =====")
        print(f"chat_id={args.chat_id}")
        print("Ports: /dev/ttyUSB0 coordinator, /dev/ttyUSB1 sensor, /dev/ttyUSB2 control, /dev/ttyUSB3 guardian")
        print("ACM policy: ignored by design")
        roles_ok = request_config(states)

        print("===== send phase =====")
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

        metrics = count_metrics(states, sent)
        print("===== metrics =====")
        for key, value in metrics.items():
            print(f"{key}={value}")
        print(f"artifacts={args.artifact_dir}/feishu_multistep_{run_id}_ttyUSB*.log")

        passed = (
            roles_ok
            and metrics["sent_failed"] == 0
            and metrics["sent_ok"] == len(messages)
            and metrics["processing_turns"] >= len(messages)
            and metrics["automation_mentions"] >= 1
            and metrics["control_received"] >= 2
            and metrics["ws2812_writes"] >= 2
            and metrics["crashes"] == 0
        )
        print("RESULT:", "PASS" if passed else "FAIL")
        return 0 if passed else 1
    finally:
        close_ports(states)


if __name__ == "__main__":
    raise SystemExit(main())

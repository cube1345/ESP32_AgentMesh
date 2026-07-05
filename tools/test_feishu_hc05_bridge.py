#!/usr/bin/env python3
"""Feishu -> coordinator -> sensor_agent -> HC-05 UART bridge smoke test."""

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
from dataclasses import dataclass


PORTS = ["/dev/ttyUSB0", "/dev/ttyUSB1"]
DEFAULT_CHAT_ID = "oc_9771f831cad2fffc28239bd313cd77e1"
PROMPT_TEMPLATE = (
    "HC05-BRIDGE-{tag}: "
    "请把文本 {payload} 通过 HC-05 蓝牙发送到我的手机上位机。"
    "请使用 virtual_device_read，device=hc05_uart_bridge，"
    "command_ascii={payload}，expect_response=false。"
    "完成后简短回复已发送。"
)


@dataclass
class PortState:
    port: str
    fd: int
    text: str = ""
    buffer: str = ""


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
        print(f"ERROR: missing ports: {', '.join(missing)}", file=sys.stderr)
        return {}
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


def drain(states: dict[int, PortState], seconds: float, echo: bool) -> str:
    end = time.time() + seconds
    start = {fd: len(state.text) for fd, state in states.items()}
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
            if echo:
                for line in chunk.splitlines():
                    if any(key in line for key in (
                        "Processing message from feishu",
                        "mesh_send_command",
                        "Mesh role command received",
                        "Executing tool: virtual_device_read",
                        "virtual_device_read UART send-only status=ESP_OK",
                        "Queue final response to feishu",
                        "Feishu send success",
                        "hc05_uart_bridge",
                    )):
                        print(f"{state.port}: {line}")
    return "\n".join(state.text[start[fd]:] for fd, state in states.items())


def send_feishu(chat_id: str, text: str, timeout_s: int) -> tuple[bool, str]:
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
        f"espagent-hc05-{uuid.uuid4().hex[:12]}",
        "--json",
    ]
    try:
        proc = subprocess.run(cmd, check=False, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired:
        return False, "lark-cli timeout"
    if proc.returncode != 0:
        return False, (proc.stderr or proc.stdout).strip()
    try:
        data = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return False, f"invalid lark-cli JSON: {proc.stdout[:200]}"
    if not data.get("ok"):
        return False, json.dumps(data, ensure_ascii=False)
    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chat-id", default=DEFAULT_CHAT_ID)
    parser.add_argument("--payload", default=f"HC05_FROM_FEISHU_{int(time.time())}")
    parser.add_argument("--timeout-seconds", type=float, default=70.0)
    parser.add_argument("--send-timeout", type=int, default=20)
    parser.add_argument("--echo", action="store_true")
    args = parser.parse_args()

    states = open_ports()
    if not states:
        return 2
    try:
        drain(states, 2.0, echo=False)
        tag = uuid.uuid4().hex[:8].upper()
        prompt = PROMPT_TEMPLATE.format(tag=tag, payload=args.payload)
        print(f"===== HC05 bridge Feishu test =====")
        print(f"payload={args.payload}")
        print(f"tag={tag}")
        print("sending Feishu message...")
        ok, err = send_feishu(args.chat_id, prompt, args.send_timeout)
        if not ok:
            print(f"FAIL: Feishu send failed: {err}")
            return 1
        print("Feishu send: OK")

        text = drain(states, args.timeout_seconds, echo=args.echo)
        text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)

        checks = {
            "usb0_feishu_inbound": "Processing message from feishu" in text,
            "usb0_mesh_dispatch": "mesh_send_command" in text and "hc05_uart_bridge" in text,
            "usb1_uart_send": (
                "virtual_device_read UART send-only status=ESP_OK" in text and
                args.payload in text and
                "hc05_uart_bridge" in text
            ),
            "usb0_feishu_reply": (
                "Queue final response to feishu" in text or
                "Feishu send success" in text
            ),
        }

        print("===== result =====")
        for key, passed in checks.items():
            print(f"{key}={'PASS' if passed else 'FAIL'}")
        success = all(checks.values())
        print(f"overall={'PASS' if success else 'FAIL'}")
        return 0 if success else 1
    finally:
        close_ports(states)


if __name__ == "__main__":
    raise SystemExit(main())

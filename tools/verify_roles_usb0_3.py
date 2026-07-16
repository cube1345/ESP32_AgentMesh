#!/usr/bin/env python3
"""Verify ESPAgent role identity on /dev/ttyUSB0-3.

The script intentionally checks only ttyUSB ports. ttyACM devices are ignored
because ESP32-P4 display boards commonly appear there.
"""

from __future__ import annotations

import os
import re
import select
import sys
import termios
import time
from dataclasses import dataclass


EXPECTED = [
    {
        "port": "/dev/ttyUSB0",
        "node_id": "esp32s3-coordinator-01",
        "role": "coordinator_agent",
        "capability": "coordinator",
    },
    {
        "port": "/dev/ttyUSB1",
        "node_id": "esp32s3-sensor-01",
        "role": "sensor_agent",
        "capability": "sensor",
    },
    {
        "port": "/dev/ttyUSB2",
        "node_id": "esp32s3-control-01",
        "role": "control_agent",
        "capability": "control",
    },
    {
        "port": "/dev/ttyUSB3",
        "node_id": "esp32s3-guardian-01",
        "role": "guardian_agent",
        "capability": "guardian",
    },
]


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
    missing = [item["port"] for item in EXPECTED if not os.path.exists(item["port"])]
    if missing:
        print("ERROR: missing required ports:", ", ".join(missing), file=sys.stderr)
        print("ERROR: this verifier uses only /dev/ttyUSB0-3, not /dev/ttyACM*.", file=sys.stderr)
        sys.exit(2)

    states: dict[int, PortState] = {}
    for item in EXPECTED:
        port = item["port"]
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
    if not line.endswith("\n"):
        line += "\n"
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
            lines = state.buffer.splitlines(keepends=True)
            if lines and not lines[-1].endswith(("\n", "\r")):
                state.buffer = lines.pop()
            else:
                state.buffer = ""
            if echo:
                for line in lines:
                    stripped = line.strip()
                    if ("Node " in stripped or "Caps" in stripped or
                            "ESPAgent ready" in stripped or "role=" in stripped):
                        print(f"{state.port}: {stripped}")


def find_field(text: str, label: str) -> str | None:
    value = None
    for line in text.splitlines():
        line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line).strip()
        if not line.startswith(label) or ":" not in line:
            continue
        value = line.split(":", 1)[1].strip()
        value = re.sub(r"\s+\[[^\]]+\]$", "", value).strip()
    return value


def find_identity_fallback(text: str) -> tuple[str | None, str | None, str | None]:
    node_id = None
    role = None
    caps = None
    for match in re.finditer(r"node=([A-Za-z0-9_-]+)\s+role=([A-Za-z0-9_]+)\s+capabilities=([^\s\r\n]+)", text):
        node_id, role, caps = match.groups()
    for match in re.finditer(r'"node_id":"([^"]+)".*?"role":"([^"]+)".*?"capabilities":"([^"]+)"', text):
        node_id, role, caps = match.groups()
    return node_id, role, caps


def state_for_port(states: dict[int, PortState], port: str) -> PortState:
    for state in states.values():
        if state.port == port:
            return state
    raise KeyError(port)


def verify(states: dict[int, PortState]) -> bool:
    ok = True
    print("===== ESPAgent role identity check =====")
    for item in EXPECTED:
        state = state_for_port(states, item["port"])
        node_id = find_field(state.text, "Node ID")
        role = find_field(state.text, "Node Role")
        caps = find_field(state.text, "Node Caps") or ""
        fallback_node, fallback_role, fallback_caps = find_identity_fallback(state.text)
        node_id = node_id or fallback_node
        role = role or fallback_role
        caps = caps or (fallback_caps or "")

        port_ok = (
            node_id == item["node_id"] and
            role == item["role"] and
            item["capability"] in caps
        )
        status = "PASS" if port_ok else "FAIL"
        if not port_ok:
            ok = False
        print(
            f"{item['port']}: {status} "
            f"node_id={node_id or 'unknown'} expected={item['node_id']} "
            f"role={role or 'unknown'} expected={item['role']} "
            f"caps={caps or 'unknown'}"
        )
    return ok


def main() -> int:
    echo = "--echo" in sys.argv
    states = open_ports()
    try:
        drain(states, 2.0, echo=echo)
        for state in states.values():
            write_line(state, "config_show")
        drain(states, 8.0, echo=echo)
        return 0 if verify(states) else 1
    finally:
        close_ports(states)


if __name__ == "__main__":
    raise SystemExit(main())

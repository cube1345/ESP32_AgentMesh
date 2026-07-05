#!/usr/bin/env python3
"""Verify coordinator gateway capability from /dev/ttyUSB0.

This test intentionally targets only the ESP32-S3 coordinator on /dev/ttyUSB0.
It ignores /dev/ttyACM* because ESP32-P4 boards commonly enumerate there.
"""

from __future__ import annotations

import json
import os
import re
import select
import socket
import sys
import termios
import time
import urllib.request
from dataclasses import dataclass


PORT = "/dev/ttyUSB0"
BAUD = termios.B115200


@dataclass
class SerialState:
    fd: int
    buffer: str = ""
    text: str = ""


def configure_serial(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = BAUD
    attrs[5] = BAUD
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def open_serial() -> SerialState:
    last_error: OSError | None = None
    deadline = time.time() + 20.0
    while time.time() < deadline:
        try:
            fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            break
        except OSError as exc:
            last_error = exc
        time.sleep(0.5)
    else:
        detail = f": {last_error}" if last_error else ""
        raise SystemExit(f"ERROR: cannot open {PORT}{detail}; this verifier does not use /dev/ttyACM*")
    configure_serial(fd)
    try:
        os.read(fd, 65535)
    except BlockingIOError:
        pass
    return SerialState(fd=fd)


def close_serial(state: SerialState) -> None:
    try:
        os.close(state.fd)
    except OSError:
        pass


def drain(state: SerialState, seconds: float, echo: bool = True) -> str:
    end = time.time() + seconds
    start_len = len(state.text)
    while time.time() < end:
        readable, _, _ = select.select([state.fd], [], [], 0.2)
        if not readable:
            continue
        try:
            data = os.read(state.fd, 8192)
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
                if stripped:
                    print(f"{PORT}: {stripped}")
    return state.text[start_len:]


def send_command(state: SerialState, command: str, wait: float = 5.0) -> str:
    print(f"===== SEND {command} =====")
    os.write(state.fd, (command + "\n").encode("utf-8"))
    out = drain(state, wait)
    print("===== END =====")
    return out


def extract_ip(text: str) -> str | None:
    patterns = [
        r"\bip(?:_addr)?[=: ]+([0-9]{1,3}(?:\.[0-9]{1,3}){3})",
        r"\bSTA(?: |_)IP[=: ]+([0-9]{1,3}(?:\.[0-9]{1,3}){3})",
        r"\bgot ip[: ]+([0-9]{1,3}(?:\.[0-9]{1,3}){3})",
        r"\b([0-9]{1,3}(?:\.[0-9]{1,3}){3})\b",
    ]
    for pattern in patterns:
        for match in re.finditer(pattern, text, re.IGNORECASE):
            ip = match.group(1)
            parts = [int(p) for p in ip.split(".")]
            if all(0 <= p <= 255 for p in parts) and not ip.startswith("0."):
                return ip
    return None


def http_get(ip: str, path: str, timeout: float = 5.0) -> tuple[bool, str]:
    url = f"http://{ip}{path}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            body = response.read(4096).decode("utf-8", "replace")
            return True, f"HTTP {response.status} {url}\n{body}"
    except (OSError, socket.timeout, urllib.error.URLError) as exc:
        return False, f"HTTP FAIL {url}: {exc}"


def main() -> int:
    state = open_serial()
    try:
        drain(state, 1.0, echo=False)
        wifi = send_command(state, "wifi_status", wait=4.0)
        status1 = send_command(state, "tool_exec gateway_status {}", wait=5.0)
        reg = send_command(
            state,
            'tool_exec gateway_register_ble_mesh_device {"device_id":"blemesh_lamp_01","address":"0005","name":"Lamp","capabilities":"light,onoff"}',
            wait=5.0,
        )
        status2 = send_command(state, "tool_exec gateway_status {}", wait=5.0)
        ota_block = send_command(
            state,
            'tool_exec ota_gateway_plan {"target_role":"control_agent","url":"https://example.com/ESPAgent.bin","version":"test","confirmed":false}',
            wait=5.0,
        )
        ota_ok = send_command(
            state,
            'tool_exec ota_gateway_plan {"target_role":"control_agent","url":"https://example.com/ESPAgent.bin","version":"test","confirmed":true}',
            wait=5.0,
        )

        combined = "\n".join([wifi, status1, reg, status2, ota_block, ota_ok])
        ip = extract_ip(combined)
        print(f"===== HTTP TARGET ip={ip or 'unknown'} =====")
        http_results: list[tuple[str, bool, str]] = []
        if ip:
            for path in ("/", "/status", "/devices"):
                ok, body = http_get(ip, path)
                http_results.append((path, ok, body))
                print(f"===== HTTP {path} ok={ok} =====")
                print(body[:2000])

        checks = {
            "gateway_status": "gateway" in status1.lower() and "tool_exec status: ESP_OK" in status1,
            "ble_virtual_register": "blemesh_lamp_01" in reg and "tool_exec status: ESP_OK" in reg,
            "ble_virtual_visible": "blemesh_lamp_01" in status2,
            "ota_requires_confirm": "requires explicit confirmed=true" in ota_block or "denied" in ota_block.lower(),
            "ota_confirmed_plan": "tool_exec status: ESP_OK" in ota_ok and ("OTA" in ota_ok or "ota" in ota_ok),
            "http_root": any(path == "/" and ok for path, ok, _ in http_results),
            "http_status": any(path == "/status" and ok for path, ok, _ in http_results),
            "http_devices": any(path == "/devices" and ok for path, ok, _ in http_results),
        }

        print("===== GATEWAY CHECK SUMMARY =====")
        for name, ok in checks.items():
            print(f"{name}: {'PASS' if ok else 'FAIL'}")
        return 0 if all(checks.values()) else 1
    finally:
        close_serial(state)


if __name__ == "__main__":
    raise SystemExit(main())

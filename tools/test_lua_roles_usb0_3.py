#!/usr/bin/env python3
"""Verify Lua runtime on all ESPAgent S3 roles over /dev/ttyUSB0-3."""

from __future__ import annotations

import json
import os
import select
import sys
import termios
import time


PORTS = ["/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2", "/dev/ttyUSB3"]
ROLES = ["coordinator", "sensor", "control", "guardian"]


def configure(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def open_port(port: str) -> int:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)
    return fd


def drain(fd: int, seconds: float) -> str:
    end = time.time() + seconds
    out: list[str] = []
    while time.time() < end:
        readable, _, _ = select.select([fd], [], [], 0.2)
        if not readable:
            continue
        try:
            data = os.read(fd, 8192)
        except BlockingIOError:
            data = b""
        if data:
            out.append(data.decode("utf-8", "replace"))
    return "".join(out)


def write_line(fd: int, line: str) -> None:
    os.write(fd, (line.rstrip("\n") + "\n").encode("utf-8"))


def tool_command(name: str, payload: dict) -> str:
    return f"tool_exec {name} {json.dumps(payload, separators=(',', ':'))}"


def run_case(fd: int, port: str, command: str, expect: list[str], timeout_s: float) -> bool:
    write_line(fd, command)
    text = drain(fd, timeout_s)
    ok = all(item in text for item in expect) and "Guru Meditation" not in text
    print(f"{port}: {'PASS' if ok else 'FAIL'}")
    if not ok:
        print(f"{port}: missing {[item for item in expect if item not in text]}")
    return ok


def main() -> int:
    missing = [port for port in PORTS if not os.path.exists(port)]
    if missing:
        print("ERROR: missing ports: " + ", ".join(missing), file=sys.stderr)
        return 2

    total = 0
    passed = 0
    for port, role in zip(PORTS, ROLES):
        fd = open_port(port)
        try:
            drain(fd, 1.0)
            total += 1
            if run_case(fd,
                        port,
                        tool_command("lua_runtime_info", {}),
                        ['"available":true', '"status":"available"', "tool_exec status: ESP_OK"],
                        4.0):
                passed += 1
            total += 1
            if run_case(fd,
                        port,
                        tool_command("lua_run_source", {
                            "source": "return('role_lua_ok_'..tostring(args.role))",
                            "args": {"role": role},
                            "confirmed": True,
                            "timeout_ms": 1000,
                        }),
                        [f"role_lua_ok_{role}", "tool_exec status: ESP_OK"],
                        5.0):
                passed += 1
        finally:
            os.close(fd)

    print(f"===== all-role lua summary: {passed}/{total} passed =====")
    return 0 if passed == total else 1


if __name__ == "__main__":
    raise SystemExit(main())

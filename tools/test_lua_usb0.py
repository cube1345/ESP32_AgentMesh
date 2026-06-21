#!/usr/bin/env python3
"""Smoke-test ESPAgent Lua runtime on coordinator /dev/ttyUSB0."""

from __future__ import annotations

import json
import os
import select
import sys
import termios
import time


PORT = "/dev/ttyUSB0"


def configure(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def drain(fd: int, seconds: float, echo: bool) -> str:
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
        if not data:
            continue
        text = data.decode("utf-8", "replace")
        out.append(text)
        if echo:
            sys.stdout.write(text)
            sys.stdout.flush()
    return "".join(out)


def write_line(fd: int, line: str) -> None:
    os.write(fd, (line.rstrip("\n") + "\n").encode("utf-8"))


def tool_command(name: str, payload: dict) -> str:
    return f"tool_exec {name} {json.dumps(payload, separators=(',', ':'))}"


def run_case(fd: int,
             case_id: str,
             command: str,
             expect: list[str],
             timeout_s: float,
             echo: bool) -> bool:
    print(f"===== {case_id} =====")
    write_line(fd, command)
    text = drain(fd, timeout_s, echo)
    ok = all(item in text for item in expect) and "Guru Meditation" not in text
    print(f"{case_id}: {'PASS' if ok else 'FAIL'}")
    if not ok:
        print("missing:", [item for item in expect if item not in text])
    return ok


def main() -> int:
    echo = "--echo" in sys.argv
    if not os.path.exists(PORT):
        print(f"ERROR: missing {PORT}", file=sys.stderr)
        return 2
    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(fd)
        drain(fd, 1.0, echo)
        cases = [
            (
                "lua_runtime_info",
                tool_command("lua_runtime_info", {}),
                ['"available":true', '"status":"available"'],
                4.0,
            ),
            (
                "lua_list_modules",
                tool_command("lua_list_modules", {}),
                ['"name":"espagent"', '"status":"available"', '"package"', '"restricted"'],
                4.0,
            ),
            (
                "lua_list_scripts",
                tool_command("lua_list_scripts", {}),
                ["/spiffs/scripts/lua_smoke.lua", '"schema":"espagent.lua_scripts.v1"'],
                4.0,
            ),
            (
                "lua_run_script",
                tool_command("lua_run_script", {
                    "path": "/spiffs/scripts/lua_smoke.lua",
                    "args": {"label": "usb0"},
                    "confirmed": True,
                    "timeout_ms": 3000,
                }),
                ["lua_smoke label=usb0", "lua_smoke ok:", "tool_exec status: ESP_OK"],
                8.0,
            ),
            (
                "lua_run_source",
                tool_command("lua_run_source", {
                    "source": (
                        "ea=require('espagent');"
                        "print('inline_label='..tostring(args.label));"
                        "ea.delay_ms(10);"
                        "return('inline_ok_'..tostring(ea.now_ms()))"
                    ),
                    "args": {"label": "dev"},
                    "confirmed": True,
                    "timeout_ms": 2000,
                }),
                ["inline_label=dev", "inline_ok_", "tool_exec status: ESP_OK"],
                6.0,
            ),
            (
                "lua_run_script_async",
                tool_command("lua_run_script_async", {
                    "path": "/spiffs/scripts/lua_smoke.lua",
                    "args": {"label": "async"},
                    "confirmed": True,
                    "timeout_ms": 3000,
                }),
                ['"schema":"espagent.lua_async_started.v1"', '"job_id":"lua-', "tool_exec status: ESP_OK"],
                5.0,
            ),
            (
                "lua_list_jobs",
                tool_command("lua_list_jobs", {}),
                ['"schema":"espagent.lua_jobs.v1"', '"state":"done"', "lua_smoke ok:"],
                6.0,
            ),
        ]
        passed = 0
        for case_id, command, expect, timeout_s in cases:
            if run_case(fd, case_id, command, expect, timeout_s, echo):
                passed += 1
        print(f"===== lua summary: {passed}/{len(cases)} passed =====")
        return 0 if passed == len(cases) else 1
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())

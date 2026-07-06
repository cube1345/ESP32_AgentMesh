#!/usr/bin/env python3
"""Smoke-test local Volc TTS playback on coordinator /dev/ttyUSB0."""

from __future__ import annotations

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


def main() -> int:
    echo = "--echo" in sys.argv
    text = "hello from espagent local tts test"
    if len(sys.argv) > 1 and not sys.argv[1].startswith("--"):
        text = " ".join(arg for arg in sys.argv[1:] if not arg.startswith("--"))

    if not os.path.exists(PORT):
        print(f"ERROR: missing {PORT}", file=sys.stderr)
        return 2

    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(fd)
        drain(fd, 1.0, echo)
        command = f"local_tts_speak {text}"
        print(f"===== local_tts command =====\n{command}")
        write_line(fd, command)
        result = drain(fd, 18.0, echo)
        ok = (
            "local_tts_speak status: ESP_OK" in result
            and "OK: local TTS played" in result
            and "Guru Meditation" not in result
        )
        print(f"===== local_tts summary: {'PASS' if ok else 'FAIL'} =====")
        return 0 if ok else 1
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())

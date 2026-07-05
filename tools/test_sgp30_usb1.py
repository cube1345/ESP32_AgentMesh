#!/usr/bin/env python3

import argparse
import os
import re
import select
import sys
import termios
import time


PORT = "/dev/ttyUSB1"
PROMPT = "ESPAgent> "
COMMANDS = [
    "tool_exec sgp30_read_air_quality {}",
    "tool_exec read_air_quality {}",
    "tool_exec read_environment {}",
]


def configure_serial(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def normalize(text: str) -> str:
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)
    return text.replace("\r", "")


def read_until_prompt(fd: int, timeout_s: float, echo: bool) -> str:
    end = time.time() + timeout_s
    buf = ""
    while time.time() < end:
        readable, _, _ = select.select([fd], [], [], 0.2)
        if not readable:
            continue
        chunk = os.read(fd, 8192).decode("utf-8", "replace")
        if not chunk:
            continue
        buf += chunk
        if echo:
            sys.stdout.write(chunk)
            sys.stdout.flush()
        if PROMPT in normalize(buf):
            break
    return buf


def run_command(fd: int, line: str, timeout_s: float, echo: bool) -> str:
    try:
        while True:
            if not os.read(fd, 4096):
                break
    except BlockingIOError:
        pass
    os.write(fd, b"\n")
    time.sleep(0.1)
    read_until_prompt(fd, 2.0, echo=False)
    os.write(fd, (line + "\n").encode("utf-8"))
    return read_until_prompt(fd, timeout_s, echo=echo)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--echo", action="store_true")
    parser.add_argument("--timeout", type=float, default=6.0)
    args = parser.parse_args()

    if not os.path.exists(PORT):
        print(f"ERROR: missing {PORT}", file=sys.stderr)
        return 2

    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure_serial(fd)
        try:
            os.read(fd, 65535)
        except BlockingIOError:
            pass

        ok = True
        for command in COMMANDS:
            print(f"===== {command} =====")
            result = normalize(run_command(fd, command, args.timeout, echo=args.echo))
            if "tool_exec status: ESP_OK" not in result:
                ok = False
                print("RESULT: FAIL")
            else:
                print("RESULT: PASS")
        return 0 if ok else 1
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())

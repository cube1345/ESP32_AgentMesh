#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import select
import sys
import termios
import time


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Send one CLI command to an ESPAgent serial console")
    parser.add_argument("port")
    parser.add_argument("command", nargs="+")
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--settle", type=float, default=1.0)
    parser.add_argument("--echo", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(args.port):
        print(f"ERROR: missing {args.port}", file=sys.stderr)
        return 2

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(fd)
        drain(fd, args.settle, args.echo)
        line = " ".join(args.command)
        print(f"===== serial command =====\n{args.port}: {line}")
        os.write(fd, (line + "\n").encode("utf-8"))
        output = drain(fd, args.timeout, args.echo)
        print("===== serial output end =====")
        if not args.echo:
            print(output)
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())

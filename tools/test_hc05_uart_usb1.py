#!/usr/bin/env python3

import argparse
import json
import os
import select
import re
import sys
import termios
import time


PORT = "/dev/ttyUSB1"


def configure_serial(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def read_until(fd: int, timeout_s: float, echo: bool = True) -> str:
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
    return buf


def normalize(text: str) -> str:
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)
    return text.replace("\r", "")


def wait_for_prompt(fd: int, timeout_s: float, echo: bool = True) -> str:
    end = time.time() + timeout_s
    buf = ""
    while time.time() < end:
        chunk = read_until(fd, 0.4, echo=echo)
        if chunk:
            buf += chunk
            if "ESPAgent> " in normalize(buf):
                return buf
        else:
            os.write(fd, b"\n")
            time.sleep(0.1)
    return buf


def run_command(fd: int, line: str, timeout_s: float, echo: bool = True) -> str:
    try:
        while True:
            if not os.read(fd, 4096):
                break
    except BlockingIOError:
        pass
    wait_for_prompt(fd, 2.0, echo=echo)
    write_line(fd, line)
    return wait_for_prompt(fd, timeout_s, echo=echo)


def write_line(fd: int, line: str) -> None:
    if not line.endswith("\n"):
        line += "\n"
    os.write(fd, line.encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="hc05_uart_sniff")
    parser.add_argument("--listen-seconds", type=float, default=7.0)
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

        print("== role check ==")
        run_command(fd, "config_show", 4.0, echo=True)

        print("\n== manifest presence ==")
        list_result = run_command(fd,
                                  'tool_exec list_dir {"prefix":"/spiffs/devices"}',
                                  4.0,
                                  echo=True)
        read_result = run_command(fd,
                                  f'tool_exec read_file {json.dumps({"path": f"/spiffs/devices/{args.device}.json"}, separators=(",", ":"))}',
                                  4.0,
                                  echo=True)

        payload = {"device": args.device}
        cmd = f"tool_exec virtual_device_read {json.dumps(payload, separators=(',', ':'))}"
        print("\n== test window ==")
        print(f"Immediately send data from the paired phone app within {args.listen_seconds:.1f}s")
        print(f"CLI: {cmd}")
        result = run_command(fd, cmd, args.listen_seconds, echo=True)

        ok = (
            args.device in normalize(list_result) and
            args.device in normalize(read_result) and
            "tool_exec status: ESP_OK" in normalize(result)
        )
        print("\n== summary ==")
        print(f"pass={ok}")
        return 0 if ok else 1
    finally:
        os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())

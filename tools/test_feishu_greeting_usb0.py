#!/usr/bin/env python3
"""Send a real Feishu greeting and verify coordinator USB0 LLM reply path."""

from __future__ import annotations

import argparse
import json
import os
import select
import subprocess
import sys
import termios
import time
import uuid


DEFAULT_CHAT_ID = "oc_9771f831cad2fffc28239bd313cd77e1"
PORT = "/dev/ttyUSB0"


def configure_serial(fd: int) -> None:
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
    chunks: list[str] = []
    while time.time() < end:
        readable, _, _ = select.select([fd], [], [], 0.2)
        if fd not in readable:
            continue
        try:
            data = os.read(fd, 8192)
        except BlockingIOError:
            data = b""
        if not data:
            continue
        text = data.decode("utf-8", "replace")
        chunks.append(text)
        if echo:
            sys.stdout.write(text)
            sys.stdout.flush()
    return "".join(chunks)


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
        f"espagent-greeting-{uuid.uuid4().hex[:12]}",
        "--json",
    ]
    proc = subprocess.run(cmd, check=False, capture_output=True, text=True, timeout=timeout_s)
    if proc.returncode != 0:
        return False, (proc.stderr or proc.stdout).strip()
    try:
        payload = json.loads(proc.stdout)
    except json.JSONDecodeError:
        return False, f"invalid JSON: {proc.stdout[:200]}"
    if not payload.get("ok"):
        return False, json.dumps(payload, ensure_ascii=False)
    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--chat-id", default=DEFAULT_CHAT_ID)
    parser.add_argument("--text", default="你好")
    parser.add_argument("--port", default=PORT)
    parser.add_argument("--send-timeout", type=int, default=25)
    parser.add_argument("--wait", type=float, default=35.0)
    parser.add_argument("--echo", action="store_true")
    args = parser.parse_args()

    if not os.path.exists(args.port):
        print(f"ERROR: missing {args.port}", file=sys.stderr)
        return 2

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure_serial(fd)
        try:
            os.read(fd, 65535)
        except BlockingIOError:
            pass

        ok, err = send_feishu(args.chat_id, args.text, args.send_timeout)
        if not ok:
            print(f"ERROR: Feishu send failed: {err}", file=sys.stderr)
            return 1

        text = drain(fd, args.wait, args.echo)
    finally:
        os.close(fd)

    common_expected = [
        "Processing message from feishu",
        "Queue final response to feishu",
        "Feishu send success",
    ]
    llm_expected = [
        "Direct-reply heuristic enabled for this turn; first LLM call runs without tools",
        "Calling LLM API with tools",
    ]
    deterministic_expected = [
        "Deterministic runtime skill route",
    ]
    forbidden = [
        "抱歉，我这次处理请求时遇到了错误。",
        "Local smalltalk reply",
        "Using local smalltalk fallback",
        "LLM call failed",
    ]

    path = "deterministic" if all(item in text for item in deterministic_expected) else "llm"
    expected = common_expected + (deterministic_expected if path == "deterministic" else llm_expected)
    missing = [item for item in expected if item not in text]
    bad = [item for item in forbidden if item in text]

    print("===== greeting verification =====")
    print(f"prompt={args.text}")
    print(f"path={path}")
    print(f"missing={missing}")
    print(f"forbidden={bad}")
    if missing or bad:
        print("RESULT: FAIL")
        return 1

    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

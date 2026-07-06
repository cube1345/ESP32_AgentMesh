#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import select
import subprocess
import sys
import termios
import time


PORTS = {
    "usb0": "/dev/ttyUSB0",
    "usb2": "/dev/ttyUSB2",
    "usb3": "/dev/ttyUSB3",
}


def configure(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def drain(fds: dict[str, int], deadline: float, echo: bool) -> dict[str, str]:
    buffers = {name: [] for name in fds}
    fd_to_name = {fd: name for name, fd in fds.items()}
    while time.time() < deadline:
        readable, _, _ = select.select(list(fds.values()), [], [], 0.2)
        for fd in readable:
            try:
                data = os.read(fd, 8192)
            except BlockingIOError:
                data = b""
            if not data:
                continue
            text = data.decode("utf-8", "replace").replace("\x00", "")
            name = fd_to_name[fd]
            buffers[name].append(text)
            if echo:
                sys.stdout.write(f"[{name}] {text}")
                sys.stdout.flush()
    return {name: "".join(parts) for name, parts in buffers.items()}


def drain_until(
    fds: dict[str, int],
    deadline: float,
    echo: bool,
    rules: dict[str, list[str]],
) -> tuple[dict[str, str], bool]:
    buffers = {name: [] for name in fds}
    fd_to_name = {fd: name for name, fd in fds.items()}
    matched: dict[str, set[str]] = {
        name: set() for name in rules
    }

    while time.time() < deadline:
        readable, _, _ = select.select(list(fds.values()), [], [], 0.2)
        for fd in readable:
            try:
                data = os.read(fd, 8192)
            except BlockingIOError:
                data = b""
            if not data:
                continue

            text = data.decode("utf-8", "replace").replace("\x00", "")
            name = fd_to_name[fd]
            buffers[name].append(text)
            if len(buffers[name]) > 64:
                buffers[name] = buffers[name][-64:]

            if echo:
                sys.stdout.write(f"[{name}] {text}")
                sys.stdout.flush()

            for pattern in rules.get(name, []):
                if pattern in text or pattern in "".join(buffers[name]):
                    matched.setdefault(name, set()).add(pattern)

        all_ok = True
        for name, patterns in rules.items():
            if not set(patterns).issubset(matched.get(name, set())):
                all_ok = False
                break
        if all_ok:
            return ({name: "".join(parts) for name, parts in buffers.items()}, True)

    return ({name: "".join(parts) for name, parts in buffers.items()}, False)


def write_line(fd: int, line: str) -> None:
    os.write(fd, (line.rstrip("\n") + "\n").encode("utf-8"))


def run_serial_cmd(port: str, command: list[str], timeout: float) -> subprocess.CompletedProcess[str]:
    argv = [
        sys.executable,
        "tools/serial_cmd.py",
        port,
        *command,
        "--timeout",
        str(timeout),
    ]
    return subprocess.run(
        argv,
        cwd=os.getcwd(),
        text=True,
        capture_output=True,
        check=False,
    )


def expect(logs: dict[str, str], rules: dict[str, list[str]]) -> tuple[bool, list[str]]:
    missing = []
    for name, patterns in rules.items():
        text = logs.get(name, "")
        for pattern in patterns:
            if pattern not in text:
                missing.append(f"{name}:{pattern}")
    return (len(missing) == 0, missing)


def main() -> int:
    parser = argparse.ArgumentParser(description="Verify coordinator -> control auto-reply TTS flow")
    parser.add_argument("--echo", action="store_true")
    parser.add_argument("--mesh-timeout", type=float, default=18.0)
    parser.add_argument("--ai-timeout", type=float, default=45.0)
    parser.add_argument("--skip-ai", action="store_true")
    parser.add_argument("--skip-mesh", action="store_true")
    parser.add_argument("--prompt", default="auto tts outbound reply test passed")
    args = parser.parse_args()

    missing_ports = [port for port in PORTS.values() if not os.path.exists(port)]
    if missing_ports:
        print("ERROR: missing ports:", ", ".join(missing_ports), file=sys.stderr)
        return 2

    fds = {
        name: os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        for name, path in PORTS.items()
    }
    try:
        for fd in fds.values():
            configure(fd)

        drain(fds, time.time() + 1.0, args.echo)

        if not args.skip_mesh:
            mesh_payload = json.dumps({
                "target_role": "control_agent",
                "action": "tts_speak",
                "args": {"text": "ok"},
                "async": True,
                "safety_level": 1,
            }, separators=(",", ":"))
            mesh_cmd = f"tool_exec mesh_send_command {mesh_payload}"
            print("===== mesh command =====")
            print(mesh_cmd)
            write_line(fds["usb0"], mesh_cmd)
            mesh_rules = {
                "usb0": ["tool_exec status: ESP_OK"],
                "usb2": ["voice_local_tts: OK: local TTS played"],
                "usb3": ["policy_decision"],
            }
            mesh_logs, mesh_done = drain_until(fds,
                                               time.time() + args.mesh_timeout,
                                               args.echo,
                                               mesh_rules)
            mesh_ok, mesh_missing = expect(mesh_logs, mesh_rules)
            mesh_ok = mesh_ok and mesh_done
            print(f"===== mesh summary: {'PASS' if mesh_ok else 'FAIL'} =====")
            if not mesh_ok:
                print("missing=" + ",".join(mesh_missing))
                return 1

        if args.skip_ai:
            return 0

        drain({"usb2": fds["usb2"]}, time.time() + 1.0, args.echo)
        inject_cmd = ["inject_outbound", "voice", "demo", args.prompt]
        print("===== outbound command =====")
        print(" ".join(inject_cmd))
        ai_rules = {
            "usb0": [
                "inject_outbound status: ESP_OK",
                "Dispatching response to voice:demo",
                "Voice outbound handled via MQTT TTS bridge",
                "Control-agent TTS on voice channel -> ESP_OK",
            ],
            "usb2": [
                "Local voice TTS request handled:",
                "voice_local_tts: OK: local TTS played",
            ],
        }
        proc = subprocess.Popen(
            [
                sys.executable,
                "tools/serial_cmd.py",
                PORTS["usb0"],
                *inject_cmd,
                "--timeout",
                str(min(args.ai_timeout, 12.0)),
            ],
            cwd=os.getcwd(),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        usb2_logs, ai_done = drain_until({"usb2": fds["usb2"]},
                                         time.time() + args.ai_timeout,
                                         args.echo,
                                         {"usb2": ai_rules["usb2"]})
        stdout, stderr = proc.communicate(timeout=max(2.0, min(args.ai_timeout, 15.0)))
        ai_logs = {
            "usb0": stdout + ("\nSTDERR:\n" + stderr if stderr else ""),
            "usb2": usb2_logs.get("usb2", ""),
        }
        ai_ok, ai_missing = expect(ai_logs, ai_rules)
        ai_ok = ai_ok and ai_done
        print(f"===== outbound summary: {'PASS' if ai_ok else 'FAIL'} =====")
        if not ai_ok:
            print("missing=" + ",".join(ai_missing))
            print("----- usb0 output -----")
            print(ai_logs["usb0"])
            print("----- usb2 output tail -----")
            print(ai_logs["usb2"][-2000:])
            return 1

        return 0
    finally:
        for fd in fds.values():
            os.close(fd)


if __name__ == "__main__":
    raise SystemExit(main())

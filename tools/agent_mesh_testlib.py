#!/usr/bin/env python3
"""Shared network-only helpers for ESPAgent Agent Mesh verification."""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import time
import uuid
from dataclasses import dataclass
from typing import Any, Callable, Iterable
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


DEFAULT_CHAT_ID = "oc_9771f831cad2fffc28239bd313cd77e1"
DEFAULT_DASHBOARD_URL = "http://127.0.0.1:4175/api/dashboard"
DEFAULT_MQTT_HOST = "broker.emqx.io"
DEFAULT_MQTT_PORT = 1883
DEFAULT_TOPIC_PREFIX = "espagent/cube1345"


class TestFailure(RuntimeError):
    """Expected test precondition or behavior was not observed."""


@dataclass(frozen=True)
class SentMessage:
    message_id: str
    baseline_ids: frozenset[str]


@dataclass(frozen=True)
class FeishuReply:
    message_id: str
    content: str
    position: int


def run_id(prefix: str) -> str:
    return f"{prefix}-{time.strftime('%H%M%S')}-{uuid.uuid4().hex[:4]}"


def contains_any(text: str, patterns: Iterable[str]) -> bool:
    return any(re.search(pattern, text, re.IGNORECASE | re.MULTILINE) for pattern in patterns)


def print_result(capability: str, passed: bool, **details: Any) -> int:
    print(json.dumps({"capability": capability, "passed": passed, **details},
                     ensure_ascii=False, indent=2))
    return 0 if passed else 1


class FeishuClient:
    def __init__(self, chat_id: str, command_timeout_s: float = 25.0) -> None:
        if not shutil.which("lark-cli"):
            raise TestFailure("lark-cli is not installed or not on PATH")
        self.chat_id = chat_id
        self.command_timeout_s = command_timeout_s
        self.env = os.environ.copy()
        self.env["LARKSUITE_CLI_NO_UPDATE_NOTIFIER"] = "1"
        self.env["LARKSUITE_CLI_NO_SKILLS_NOTIFIER"] = "1"

    def _run(self, args: list[str]) -> dict[str, Any]:
        command = ["lark-cli", "im", *args]
        try:
            proc = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=self.command_timeout_s,
                env=self.env,
            )
        except subprocess.TimeoutExpired as exc:
            raise TestFailure(f"lark-cli timed out: {' '.join(command[:4])}") from exc
        if proc.returncode != 0:
            raise TestFailure((proc.stderr or proc.stdout or "lark-cli failed").strip())
        try:
            payload = json.loads(proc.stdout)
        except json.JSONDecodeError as exc:
            raise TestFailure(f"invalid lark-cli JSON: {proc.stdout[:240]}") from exc
        if not payload.get("ok"):
            raise TestFailure(json.dumps(payload.get("error", payload), ensure_ascii=False))
        data = payload.get("data")
        return data if isinstance(data, dict) else {}

    def list_messages(self, page_size: int = 50) -> list[dict[str, Any]]:
        data = self._run([
            "+chat-messages-list",
            "--as", "user",
            "--chat-id", self.chat_id,
            "--order", "desc",
            "--page-size", str(page_size),
            "--no-reactions",
            "--format", "json",
        ])
        messages = data.get("messages")
        return messages if isinstance(messages, list) else []

    def send(self, text: str, key_prefix: str) -> SentMessage:
        baseline = frozenset(
            str(item.get("message_id", "")) for item in self.list_messages()
            if item.get("message_id")
        )
        data = self._run([
            "+messages-send",
            "--as", "user",
            "--chat-id", self.chat_id,
            "--text", text,
            "--idempotency-key", f"espagent-{key_prefix}-{uuid.uuid4().hex[:10]}",
            "--json",
        ])
        message = data.get("message") if isinstance(data.get("message"), dict) else data
        message_id = str(message.get("message_id", ""))
        if not message_id:
            raise TestFailure(f"Feishu send response has no message_id: {data}")
        return SentMessage(message_id=message_id, baseline_ids=baseline)

    def wait_for_reply(
        self,
        sent: SentMessage,
        predicate: Callable[[str], bool],
        timeout_s: float,
        poll_s: float = 2.5,
    ) -> FeishuReply:
        deadline = time.monotonic() + timeout_s
        observed: dict[str, FeishuReply] = {}
        while time.monotonic() < deadline:
            messages = self.list_messages()
            sent_position = -1
            for item in messages:
                if str(item.get("message_id", "")) == sent.message_id:
                    sent_position = _message_position(item)
                    break

            for item in messages:
                message_id = str(item.get("message_id", ""))
                sender = item.get("sender") if isinstance(item.get("sender"), dict) else {}
                sender_type = str(sender.get("sender_type", ""))
                position = _message_position(item)
                if not message_id or message_id in sent.baseline_ids:
                    continue
                if sender_type not in {"app", "bot"}:
                    continue
                if sent_position >= 0 and position >= 0 and position <= sent_position:
                    continue
                reply = FeishuReply(message_id, str(item.get("content", "")), position)
                observed[message_id] = reply

            ordered = sorted(observed.values(), key=lambda item: item.position)
            for reply in ordered:
                if predicate(reply.content):
                    return reply
            time.sleep(poll_s)

        preview = " | ".join(reply.content[:180] for reply in observed.values())
        raise TestFailure(f"no matching Agent reply within {timeout_s:.0f}s; observed={preview or '(none)'}")

    def send_and_wait(
        self,
        text: str,
        key_prefix: str,
        predicate: Callable[[str], bool],
        timeout_s: float,
    ) -> FeishuReply:
        print(f"SEND: {text}")
        sent = self.send(text, key_prefix)
        reply = self.wait_for_reply(sent, predicate, timeout_s)
        print(f"REPLY: {reply.content}")
        return reply


def _message_position(message: dict[str, Any]) -> int:
    try:
        return int(message.get("message_position", -1))
    except (TypeError, ValueError):
        return -1


def http_json(url: str, timeout_s: float = 10.0) -> dict[str, Any]:
    try:
        with urlopen(Request(url, headers={"Accept": "application/json"}), timeout=timeout_s) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as exc:
        raise TestFailure(f"HTTP JSON request failed for {url}: {exc}") from exc
    if not isinstance(payload, dict):
        raise TestFailure(f"HTTP response is not an object: {url}")
    return payload


def dashboard_events(url: str) -> list[dict[str, Any]]:
    payload = http_json(url)
    events = payload.get("timeline")
    return events if isinstance(events, list) else []


def event_key(event: dict[str, Any]) -> str:
    return json.dumps(event, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def wait_for_dashboard_events(
    url: str,
    baseline: set[str],
    requirements: dict[str, Callable[[dict[str, Any]], bool]],
    timeout_s: float,
    poll_s: float = 1.5,
) -> tuple[dict[str, dict[str, Any]], list[dict[str, Any]]]:
    deadline = time.monotonic() + timeout_s
    matched: dict[str, dict[str, Any]] = {}
    observed: dict[str, dict[str, Any]] = {}
    while time.monotonic() < deadline:
        for event in dashboard_events(url):
            key = event_key(event)
            if key in baseline:
                continue
            observed[key] = event
            for label, predicate in requirements.items():
                if label not in matched and predicate(event):
                    matched[label] = event
        if len(matched) == len(requirements):
            return matched, list(observed.values())
        time.sleep(poll_s)
    return matched, list(observed.values())


def assert_mesh_roles_online(url: str) -> None:
    payload = http_json(url)
    nodes = payload.get("nodes") if isinstance(payload.get("nodes"), list) else []
    online = {str(node.get("role")) for node in nodes if node.get("status") == "online"}
    required = {"coordinator_agent", "sensor_agent", "control_agent", "guardian_agent"}
    missing = required - online
    if missing:
        raise TestFailure(f"Agent Mesh roles are not all online: {sorted(missing)}")


def mqtt_runtime_skill_upsert(
    name: str,
    content: str,
    host: str = DEFAULT_MQTT_HOST,
    port: int = DEFAULT_MQTT_PORT,
    topic_prefix: str = DEFAULT_TOPIC_PREFIX,
    mesh_auth_key: str = "",
    timeout_s: float = 20.0,
) -> dict[str, Any]:
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,48}", name):
        raise TestFailure("runtime skill name must match [A-Za-z0-9_-]{1,48}")
    if not content.lstrip().startswith("# "):
        raise TestFailure("runtime skill content must start with '# '")
    if len(content.encode("utf-8")) > 1100:
        raise TestFailure("runtime skill exceeds the current 1100-byte MQTT limit")

    request_id = f"py-skill-{int(time.time() * 1000):x}-{uuid.uuid4().hex[:6]}"
    digest = hashlib.sha256(content.encode("utf-8")).hexdigest()
    request: dict[str, Any] = {
        "request_id": request_id,
        "operation": "upsert",
        "name": name,
        "content": content,
        "sha256": digest,
        "confirmed": True,
        "ts_ms": int(time.time() * 1000),
    }
    if mesh_auth_key:
        canonical = "\n".join(str(request[field]) for field in (
            "request_id", "operation", "name", "sha256", "ts_ms"
        ))
        request["signature"] = hmac.new(
            mesh_auth_key.encode("utf-8"), canonical.encode("utf-8"), hashlib.sha256
        ).hexdigest()

    request_topic = f"{topic_prefix.rstrip('/')}/runtime/skills/request"
    reply_topic = f"{topic_prefix.rstrip('/')}/runtime/skills/reply"
    client = _MqttClient(host, port, timeout_s)
    try:
        client.connect()
        client.subscribe(reply_topic)
        client.publish(request_topic, json.dumps(request, ensure_ascii=False, separators=(",", ":")))
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            topic, body = client.receive_publish(min(2.0, deadline - time.monotonic()))
            if topic != reply_topic:
                continue
            try:
                reply = json.loads(body)
            except json.JSONDecodeError:
                continue
            if reply.get("request_id") == request_id:
                if not reply.get("ok"):
                    raise TestFailure(str(reply.get("message") or "runtime skill MQTT update failed"))
                return reply
    finally:
        client.close()
    raise TestFailure(f"no Runtime Skill MQTT reply within {timeout_s:.0f}s")


class _MqttClient:
    def __init__(self, host: str, port: int, timeout_s: float) -> None:
        self.host = host
        self.port = port
        self.timeout_s = timeout_s
        self.sock: socket.socket | None = None
        self.packet_id = 1

    def connect(self) -> None:
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout_s)
        variable = _mqtt_string("MQTT") + bytes((4, 2)) + struct.pack("!H", 30)
        payload = _mqtt_string(f"espagent-python-{uuid.uuid4().hex[:12]}")
        self._send_packet(0x10, variable + payload)
        packet_type, body = self._read_packet(self.timeout_s)
        if packet_type != 2 or len(body) != 2 or body[1] != 0:
            raise TestFailure(f"MQTT CONNACK rejected: type={packet_type} body={body.hex()}")

    def subscribe(self, topic: str) -> None:
        packet_id = self._next_packet_id()
        self._send_packet(0x82, struct.pack("!H", packet_id) + _mqtt_string(topic) + b"\x00")
        packet_type, body = self._read_packet(self.timeout_s)
        if packet_type != 9 or len(body) < 3 or struct.unpack("!H", body[:2])[0] != packet_id:
            raise TestFailure("MQTT SUBACK was not received")

    def publish(self, topic: str, payload: str) -> None:
        self._send_packet(0x30, _mqtt_string(topic) + payload.encode("utf-8"))

    def receive_publish(self, timeout_s: float) -> tuple[str, str]:
        if timeout_s <= 0:
            return "", ""
        try:
            packet_type, body = self._read_packet(timeout_s)
        except socket.timeout:
            return "", ""
        if packet_type != 3 or len(body) < 2:
            return "", ""
        topic_len = struct.unpack("!H", body[:2])[0]
        if len(body) < 2 + topic_len:
            return "", ""
        topic = body[2:2 + topic_len].decode("utf-8", "replace")
        return topic, body[2 + topic_len:].decode("utf-8", "replace")

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.sendall(b"\xe0\x00")
            except OSError:
                pass
            self.sock.close()
            self.sock = None

    def _send_packet(self, header: int, body: bytes) -> None:
        if not self.sock:
            raise TestFailure("MQTT socket is not connected")
        self.sock.sendall(bytes((header,)) + _mqtt_varint(len(body)) + body)

    def _read_packet(self, timeout_s: float) -> tuple[int, bytes]:
        if not self.sock:
            raise TestFailure("MQTT socket is not connected")
        self.sock.settimeout(timeout_s)
        first = _recv_exact(self.sock, 1)[0]
        multiplier = 1
        remaining = 0
        for _ in range(4):
            digit = _recv_exact(self.sock, 1)[0]
            remaining += (digit & 0x7F) * multiplier
            if not digit & 0x80:
                break
            multiplier *= 128
        return first >> 4, _recv_exact(self.sock, remaining)

    def _next_packet_id(self) -> int:
        value = self.packet_id
        self.packet_id = 1 if value >= 65535 else value + 1
        return value


def _recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise TestFailure("MQTT connection closed unexpectedly")
        data.extend(chunk)
    return bytes(data)


def _mqtt_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("!H", len(encoded)) + encoded


def _mqtt_varint(value: int) -> bytes:
    encoded = bytearray()
    while True:
        digit = value % 128
        value //= 128
        if value:
            digit |= 0x80
        encoded.append(digit)
        if not value:
            return bytes(encoded)

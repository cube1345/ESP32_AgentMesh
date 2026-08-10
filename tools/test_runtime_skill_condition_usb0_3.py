#!/usr/bin/env python3
"""Verify a bounded Runtime Skill condition from MQTT write to Control output."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import time
import uuid

from agent_mesh_testlib import (
    DEFAULT_MQTT_HOST,
    DEFAULT_MQTT_PORT,
    DEFAULT_TOPIC_PREFIX,
    TestFailure,
    _MqttClient,
    print_result,
)


SKILL_NAME = "humidity-guard-test"


def runtime_skill_request(name: str, content: str) -> tuple[str, str]:
    request_id = f"condition-{int(time.time() * 1000):x}-{uuid.uuid4().hex[:6]}"
    request = {
        "request_id": request_id,
        "operation": "upsert",
        "name": name,
        "content": content,
        "sha256": hashlib.sha256(content.encode("utf-8")).hexdigest(),
        "confirmed": True,
        "ts_ms": int(time.time() * 1000),
    }
    return request_id, json.dumps(request, ensure_ascii=False, separators=(",", ":"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-host", default=os.getenv("ESPAGENT_MQTT_HOST", DEFAULT_MQTT_HOST))
    parser.add_argument("--mqtt-port", type=int, default=int(os.getenv("ESPAGENT_MQTT_PORT", DEFAULT_MQTT_PORT)))
    parser.add_argument("--topic-prefix", default=os.getenv("ESPAGENT_TOPIC_PREFIX", DEFAULT_TOPIC_PREFIX))
    parser.add_argument("--threshold", type=float, default=100.0)
    parser.add_argument("--timeout", type=float, default=35.0)
    args = parser.parse_args()

    content = (
        "# Humidity Guard Test\n\n"
        f"@condition metric=humidity_percent threshold={args.threshold:g} hysteresis=1 "
        "interval_s=5 cooldown_s=60 above_action=set_status_light "
        "above_args={\"color\":\"red\"} below_action=set_status_light "
        "below_args={\"color\":\"off\"}\n"
    )
    request_id, payload = runtime_skill_request(SKILL_NAME, content)
    prefix = args.topic_prefix.rstrip("/")
    client = _MqttClient(args.mqtt_host, args.mqtt_port, args.timeout)
    received: list[dict[str, object]] = []
    skill_saved = False
    sensor_read = False
    guardian_allowed = False
    control_executed = False
    control_command_ids: set[str] = set()
    first_success_at: float | None = None

    try:
        client.connect()
        client.subscribe(f"{prefix}/runtime/skills/reply")
        client.subscribe(f"{prefix}/agent/timeline")
        client.subscribe(f"{prefix}/nodes/esp32s3-control-01/events")
        client.publish(f"{prefix}/runtime/skills/request", payload)

        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            topic, body = client.receive_publish(min(2.0, deadline - time.monotonic()))
            if not topic:
                continue
            try:
                event = json.loads(body)
            except json.JSONDecodeError:
                continue
            if not isinstance(event, dict):
                continue
            received.append({"topic": topic, "event": event})
            if topic.endswith("/runtime/skills/reply"):
                skill_saved = bool(event.get("ok")) and event.get("request_id") == request_id
                continue
            action = str(event.get("action", ""))
            role = str(event.get("role", ""))
            status = str(event.get("status", ""))
            event_name = str(event.get("event", ""))
            if action == "read_environment" and status in {"ok", "running"}:
                sensor_read = True
            if action == "set_status_light" and role == "guardian_agent" and event_name == "policy_decision" and status == "ok":
                guardian_allowed = True
            if action == "set_status_light" and event_name == "mesh_command_result" and status == "ok":
                control_executed = True
                command_id = str(event.get("command_id", ""))
                if command_id:
                    control_command_ids.add(command_id)
                if first_success_at is None:
                    first_success_at = time.monotonic()
            if (first_success_at is not None and
                    time.monotonic() - first_success_at >= 12.0):
                break
    except (OSError, TestFailure) as exc:
        return print_result("runtime_skill_condition", False, error=str(exc))
    finally:
        client.close()

    return print_result(
        "runtime_skill_condition",
        skill_saved and sensor_read and guardian_allowed and control_executed and len(control_command_ids) == 1,
        request_id=request_id,
        checks={
            "skill_saved": skill_saved,
            "sensor_read": sensor_read,
            "guardian_allowed": guardian_allowed,
            "control_executed": control_executed,
            "unique_control_command_ids": sorted(control_command_ids),
        },
        observed_count=len(received),
    )


if __name__ == "__main__":
    raise SystemExit(main())

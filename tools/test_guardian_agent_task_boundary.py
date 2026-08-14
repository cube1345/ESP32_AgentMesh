#!/usr/bin/env python3
"""Verify Guardian rejects control work disguised as an agent_task."""

from __future__ import annotations

import argparse
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mqtt-host", default=os.getenv("ESPAGENT_MQTT_HOST", DEFAULT_MQTT_HOST))
    parser.add_argument("--mqtt-port", type=int, default=int(os.getenv("ESPAGENT_MQTT_PORT", DEFAULT_MQTT_PORT)))
    parser.add_argument("--topic-prefix", default=os.getenv("ESPAGENT_TOPIC_PREFIX", DEFAULT_TOPIC_PREFIX))
    parser.add_argument("--timeout", type=float, default=12.0)
    args = parser.parse_args()

    command_id = f"guardian-boundary-{uuid.uuid4().hex[:12]}"
    payload = {
        "schema": "espagent.policy_check.v1",
        "event": "policy_check",
        "command_id": command_id,
        "trace_id": f"trace-{command_id}",
        "source_role": "coordinator_agent",
        "target_role": "guardian_agent",
        "target_node": "",
        "action": "agent_task",
        "args_json": json.dumps({"task": "请执行第三角色的任务，打开状态灯"}, ensure_ascii=False),
        "nonce": f"nonce-{uuid.uuid4().hex[:12]}",
        "source_type": "coordinator",
        "source_id": "coordinator_agent",
        "safety_level": 1,
        "ttl_ms": 30000,
        "ts_ms": int(time.time() * 1000),
    }
    prefix = args.topic_prefix.rstrip("/")
    decision: dict[str, object] | None = None
    client = _MqttClient(args.mqtt_host, args.mqtt_port, args.timeout)
    try:
        client.connect()
        client.subscribe(f"{prefix}/security/decision")
        client.publish(f"{prefix}/security/policy_check", json.dumps(payload, separators=(",", ":")))
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            topic, body = client.receive_publish(min(2.0, deadline - time.monotonic()))
            if topic != f"{prefix}/security/decision":
                continue
            event = json.loads(body)
            if isinstance(event, dict) and event.get("command_id") == command_id:
                decision = event
                break
    except (OSError, ValueError, TestFailure) as exc:
        return print_result("guardian_agent_task_boundary", False, error=str(exc))
    finally:
        client.close()

    reason = str(decision.get("reason", "")) if decision else ""
    passed = (bool(decision) and decision.get("allowed") is False and
              decision.get("reason_code") == "guardian_role_boundary" and
              "cannot execute control" in reason)
    return print_result(
        "guardian_agent_task_boundary",
        passed,
        command_id=command_id,
        decision=decision or {},
    )


if __name__ == "__main__":
    raise SystemExit(main())

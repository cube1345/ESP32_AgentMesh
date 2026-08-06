#!/usr/bin/env python3
"""Verify Runtime Skill @rule trigger -> one Mesh tool action -> WS2812 result."""

from __future__ import annotations

import argparse
import os

from agent_mesh_testlib import (
    DEFAULT_CHAT_ID,
    DEFAULT_DASHBOARD_URL,
    DEFAULT_MQTT_HOST,
    DEFAULT_MQTT_PORT,
    DEFAULT_TOPIC_PREFIX,
    FeishuClient,
    TestFailure,
    assert_mesh_roles_online,
    contains_any,
    dashboard_events,
    event_key,
    mqtt_runtime_skill_upsert,
    print_result,
    run_id,
    wait_for_dashboard_events,
)


SKILL_NAME = "tool-status-light-rule-test"


def control_result(color: str, rgb: str):
    def matches(event: dict[str, object]) -> bool:
        stage = str(event.get("stage", ""))
        payload = str(event.get("payload", ""))
        return stage in {"mesh_command_result", "mesh_async_reply", "final_reply"} and (
            f"set to {color}" in payload.lower() or rgb in payload
        )

    return matches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chat-id", default=os.getenv("ESPAGENT_FEISHU_CHAT_ID", DEFAULT_CHAT_ID))
    parser.add_argument("--dashboard-url", default=os.getenv("ESPAGENT_DASHBOARD_URL", DEFAULT_DASHBOARD_URL))
    parser.add_argument("--mqtt-host", default=os.getenv("ESPAGENT_MQTT_HOST", DEFAULT_MQTT_HOST))
    parser.add_argument("--mqtt-port", type=int, default=int(os.getenv("ESPAGENT_MQTT_PORT", DEFAULT_MQTT_PORT)))
    parser.add_argument("--topic-prefix", default=os.getenv("ESPAGENT_TOPIC_PREFIX", DEFAULT_TOPIC_PREFIX))
    parser.add_argument("--mesh-auth-key", default=os.getenv("ESPAGENT_MESH_AUTH_KEY", ""))
    parser.add_argument("--reply-timeout", type=float, default=100.0)
    parser.add_argument("--execution-timeout", type=float, default=45.0)
    args = parser.parse_args()

    marker = run_id("SKILL-RULE")
    trigger = f"技能蓝灯{marker[-4:]}"
    content = (
        "# Tool Status Light Rule Test\n\n"
        "This Runtime Skill verifies deterministic one-shot skill rules.\n\n"
        f"@rule trigger=\"{trigger}\" target_role=control_agent action=set_status_light args={{\"color\":\"blue\"}}\n"
    )

    try:
        assert_mesh_roles_online(args.dashboard_url)
        baseline = {event_key(event) for event in dashboard_events(args.dashboard_url)}
        mqtt_reply = mqtt_runtime_skill_upsert(
            SKILL_NAME,
            content,
            host=args.mqtt_host,
            port=args.mqtt_port,
            topic_prefix=args.topic_prefix,
            mesh_auth_key=args.mesh_auth_key,
        )
        print(f"MQTT REPLY: {mqtt_reply}")

        client = FeishuClient(args.chat_id)
        show_reply = client.send_and_wait(
            f"/skills_show {SKILL_NAME}",
            "skill-rule-show",
            lambda text: trigger in text or contains_any(text, [r"未找到", r"not found"]),
            55.0,
        )
        prompt = f"{marker}：{trigger}"
        reply = client.send_and_wait(
            prompt,
            "skill-rule-trigger",
            lambda text: contains_any(
                text,
                [
                    r"已命中 skill 规则",
                    r"queued MQTT mesh command",
                    r"MQTT Mesh",
                    r"失败",
                    r"Error",
                ],
            ),
            args.reply_timeout,
        )
        requirements = {
            "blue_action": control_result("blue", "RGB=0,0,255"),
        }
        matched, observed = wait_for_dashboard_events(
            args.dashboard_url, baseline, requirements, args.execution_timeout
        )
    except (TestFailure, OSError) as exc:
        return print_result("skill_rule", False, skill=SKILL_NAME, marker=marker, error=str(exc))

    persisted = trigger in show_reply.content
    acknowledged = contains_any(reply.content, [r"已命中 skill 规则", r"queued MQTT mesh command", r"MQTT Mesh"]) and not contains_any(
        reply.content, [r"失败", r"Error"]
    )
    executed = "blue_action" in matched
    return print_result(
        "skill_rule",
        persisted and acknowledged and executed,
        skill=SKILL_NAME,
        marker=marker,
        trigger=trigger,
        mqtt_reply=mqtt_reply,
        show_reply=show_reply.content,
        reply=reply.content,
        checks={
            "persisted": persisted,
            "acknowledged": acknowledged,
            "executed": executed,
        },
        matched_events=matched,
        observed_event_count=len(observed),
    )


if __name__ == "__main__":
    raise SystemExit(main())

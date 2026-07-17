#!/usr/bin/env python3
"""Verify a humidity-triggered WS2812 condition rule without serial access."""

from __future__ import annotations

import argparse
import os

from agent_mesh_testlib import (
    DEFAULT_CHAT_ID,
    DEFAULT_DASHBOARD_URL,
    FeishuClient,
    TestFailure,
    assert_mesh_roles_online,
    contains_any,
    dashboard_events,
    event_key,
    print_result,
    run_id,
    wait_for_dashboard_events,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chat-id", default=os.getenv("ESPAGENT_FEISHU_CHAT_ID", DEFAULT_CHAT_ID))
    parser.add_argument("--dashboard-url", default=os.getenv("ESPAGENT_DASHBOARD_URL", DEFAULT_DASHBOARD_URL))
    parser.add_argument("--reply-timeout", type=float, default=110.0)
    parser.add_argument("--execution-timeout", type=float, default=40.0)
    args = parser.parse_args()

    marker = run_id("RULE")
    client = FeishuClient(args.chat_id)
    cleanup_reply = "not attempted"
    try:
        assert_mesh_roles_online(args.dashboard_url)
        baseline = {event_key(event) for event in dashboard_events(args.dashboard_url)}
        prompt = f"/rule {marker}：湿度大于0%时把第三角色板载 WS2812 设置为红色，否则关闭。"
        reply = client.send_and_wait(
            prompt,
            "rule",
            lambda text: contains_any(text, [r"OK:\s*rule", r"已创建条件规则", r"规则创建失败", r"Error"]),
            args.reply_timeout,
        )
        requirements = {
            "sensor_telemetry": lambda event: str(event.get("stage", "")) == "telemetry" and "humidity=" in str(event.get("payload", "")),
            "guardian_allowed": lambda event: str(event.get("stage", "")) in {"policy_check", "policy_decision"} and "allowed" in str(event.get("payload", "")).lower(),
            "red_action": lambda event: str(event.get("stage", "")) in {"mesh_command_result", "mesh_async_reply", "final_reply"} and (
                "set to red" in str(event.get("payload", "")).lower() or "RGB=255,0,0" in str(event.get("payload", ""))
            ),
        }
        matched, observed = wait_for_dashboard_events(
            args.dashboard_url, baseline, requirements, args.execution_timeout
        )
        try:
            cleanup = client.send_and_wait(
                "/control 请把第三角色板载 WS2812 关闭。",
                "rule-cleanup",
                lambda text: contains_any(text, [r"off", r"关闭", r"RGB=0,0,0", r"失败", r"Error"]),
                70.0,
            )
            cleanup_reply = cleanup.content
        except TestFailure as cleanup_error:
            cleanup_reply = f"cleanup warning: {cleanup_error}"
    except TestFailure as exc:
        return print_result("rule", False, marker=marker, error=str(exc), cleanup_reply=cleanup_reply)

    created = contains_any(reply.content, [r"OK:\s*rule", r"已创建条件规则"]) and not contains_any(
        reply.content, [r"失败", r"Error"]
    )
    return print_result(
        "rule",
        created and len(matched) == 3,
        marker=marker,
        reply=reply.content,
        cleanup_reply=cleanup_reply,
        checks={"created": created, "observed": sorted(matched)},
        matched_events=matched,
        observed_event_count=len(observed),
    )


if __name__ == "__main__":
    raise SystemExit(main())

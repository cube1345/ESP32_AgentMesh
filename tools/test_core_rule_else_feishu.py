#!/usr/bin/env python3
"""Verify the else branch of a humidity-to-WS2812 condition rule."""

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
    http_json,
    print_result,
    run_id,
    wait_for_dashboard_events,
)


def current_humidity(payload: dict[str, object]) -> float:
    metrics = payload.get("environment")
    if not isinstance(metrics, list):
        raise TestFailure("Dashboard has no environment metrics")
    for metric in metrics:
        if isinstance(metric, dict) and str(metric.get("label", "")) in {"湿度", "Humidity"}:
            return float(metric.get("value", 0.0))
    raise TestFailure("Dashboard has no humidity value")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chat-id", default=os.getenv("ESPAGENT_FEISHU_CHAT_ID", DEFAULT_CHAT_ID))
    parser.add_argument("--dashboard-url", default=os.getenv("ESPAGENT_DASHBOARD_URL", DEFAULT_DASHBOARD_URL))
    parser.add_argument("--reply-timeout", type=float, default=110.0)
    parser.add_argument("--execution-timeout", type=float, default=40.0)
    args = parser.parse_args()

    marker = run_id("RULE-ELSE")
    client = FeishuClient(args.chat_id)
    cleanup_reply = "not attempted"
    try:
        assert_mesh_roles_online(args.dashboard_url)
        dashboard = http_json(args.dashboard_url)
        humidity = current_humidity(dashboard)
        threshold = min(100.0, humidity + 5.0)
        baseline = {event_key(event) for event in dashboard_events(args.dashboard_url)}
        prompt = (
            f"/rule {marker}：湿度大于{threshold:.1f}%时把第三角色板载 WS2812 设置为红色，"
            "否则设置为蓝色。"
        )
        reply = client.send_and_wait(
            prompt,
            "rule-else",
            lambda text: contains_any(text, [r"OK:\s*rule", r"已创建条件规则", r"失败", r"Error"]),
            args.reply_timeout,
        )
        requirements = {
            "guardian_allowed": lambda event: str(event.get("stage", "")) in {"policy_check", "policy_decision"} and "allowed" in str(event.get("payload", "")).lower(),
            "blue_else_action": lambda event: str(event.get("stage", "")) in {"mesh_command_result", "mesh_async_reply", "final_reply"} and (
                "set to blue" in str(event.get("payload", "")).lower() or "RGB=0,0,255" in str(event.get("payload", ""))
            ),
        }
        matched, observed = wait_for_dashboard_events(
            args.dashboard_url, baseline, requirements, args.execution_timeout
        )
        try:
            cleanup = client.send_and_wait(
                "/control 请把第三角色板载 WS2812 关闭。",
                "rule-else-cleanup",
                lambda text: contains_any(text, [r"off", r"关闭", r"RGB=0,0,0", r"失败", r"Error"]),
                70.0,
            )
            cleanup_reply = cleanup.content
        except TestFailure as cleanup_error:
            cleanup_reply = f"cleanup warning: {cleanup_error}"
    except (TestFailure, TypeError, ValueError) as exc:
        return print_result("rule_else", False, marker=marker, error=str(exc), cleanup_reply=cleanup_reply)

    created = contains_any(reply.content, [r"OK:\s*rule", r"已创建条件规则"]) and not contains_any(
        reply.content, [r"失败", r"Error"]
    )
    return print_result(
        "rule_else",
        created and len(matched) == 2,
        marker=marker,
        measured_humidity=humidity,
        threshold=threshold,
        reply=reply.content,
        cleanup_reply=cleanup_reply,
        checks={"created": created, "observed": sorted(matched)},
        matched_events=matched,
        observed_event_count=len(observed),
    )


if __name__ == "__main__":
    raise SystemExit(main())

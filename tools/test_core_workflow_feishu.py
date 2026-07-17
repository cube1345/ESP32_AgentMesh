#!/usr/bin/env python3
"""Verify a timed multi-step WS2812 workflow without serial access."""

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
    parser.add_argument("--reply-timeout", type=float, default=110.0)
    parser.add_argument("--execution-timeout", type=float, default=45.0)
    args = parser.parse_args()

    marker = run_id("WORKFLOW")
    try:
        assert_mesh_roles_online(args.dashboard_url)
        baseline = {event_key(event) for event in dashboard_events(args.dashboard_url)}
        prompt = (
            f"/workflow {marker}：将第三角色板载 WS2812 先设置为蓝色，"
            "3秒后设置为绿色，再过3秒关闭。"
        )
        reply = FeishuClient(args.chat_id).send_and_wait(
            prompt,
            "workflow",
            lambda text: contains_any(text, [r"OK:\s*workflow", r"已创建.*流程", r"流程创建失败", r"Error"]),
            args.reply_timeout,
        )
        requirements = {
            "blue": control_result("blue", "RGB=0,0,255"),
            "green": control_result("green", "RGB=0,255,0"),
            "off": control_result("off", "RGB=0,0,0"),
        }
        matched, observed = wait_for_dashboard_events(
            args.dashboard_url, baseline, requirements, args.execution_timeout
        )
    except TestFailure as exc:
        return print_result("workflow", False, marker=marker, error=str(exc))

    created = contains_any(reply.content, [r"OK:\s*workflow", r"已创建.*流程"]) and not contains_any(
        reply.content, [r"失败", r"Error"]
    )
    return print_result(
        "workflow",
        created and len(matched) == 3,
        marker=marker,
        reply=reply.content,
        checks={"created": created, "executed_steps": sorted(matched)},
        matched_events=matched,
        observed_event_count=len(observed),
    )


if __name__ == "__main__":
    raise SystemExit(main())

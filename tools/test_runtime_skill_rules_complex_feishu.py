#!/usr/bin/env python3
"""Verify one Runtime Skill can route several deterministic one-shot rules.

The test intentionally keeps each Feishu prompt to one hardware/tool action.
It installs the skill through the running dashboard gateway so it exercises the
same wireless Runtime Skills path used by Skills Studio.
"""

from __future__ import annotations

import argparse
import json
import os
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

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


SKILL_NAME = "complex-one-shot-rules"


def post_json(url: str, payload: dict[str, Any], timeout_s: float = 45.0) -> dict[str, Any]:
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = Request(
        url,
        data=data,
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        method="POST",
    )
    try:
        with urlopen(request, timeout=timeout_s) as response:
            body = response.read().decode("utf-8")
    except (HTTPError, URLError, TimeoutError) as exc:
        raise TestFailure(f"dashboard POST failed for {url}: {exc}") from exc
    try:
        result = json.loads(body)
    except json.JSONDecodeError as exc:
        raise TestFailure(f"dashboard POST returned invalid JSON: {body[:240]}") from exc
    if not isinstance(result, dict) or not result.get("ok"):
        raise TestFailure(json.dumps(result, ensure_ascii=False))
    return result


def install_runtime_skill(dashboard_url: str, content: str) -> dict[str, Any]:
    base = dashboard_url.rsplit("/api/dashboard", 1)[0]
    return post_json(
        f"{base}/api/skills/runtime",
        {
            "confirmed": True,
            "skill": {
                "runtimeName": SKILL_NAME,
                "title": "Complex One Shot Rules",
                "scope": "runtime",
                "enabled": True,
                "content": content,
            },
        },
    )


def payload_contains(*needles: str) -> Callable[[dict[str, object]], bool]:
    lowered_needles = [needle.lower() for needle in needles]

    def matches(event: dict[str, object]) -> bool:
        stage = str(event.get("stage", ""))
        payload = str(event.get("payload", "")).lower()
        return stage in {"mesh_command_result", "mesh_async_reply", "final_reply"} and any(
            needle in payload for needle in lowered_needles
        )

    return matches


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chat-id", default=os.getenv("ESPAGENT_FEISHU_CHAT_ID", DEFAULT_CHAT_ID))
    parser.add_argument("--dashboard-url", default=os.getenv("ESPAGENT_DASHBOARD_URL", DEFAULT_DASHBOARD_URL))
    parser.add_argument("--reply-timeout", type=float, default=100.0)
    parser.add_argument("--execution-timeout", type=float, default=50.0)
    args = parser.parse_args()

    marker = run_id("COMPLEX-SKILL")
    suffix = marker[-4:]
    triggers = {
        "blue": f"复杂蓝灯{suffix}",
        "red": f"复杂红灯{suffix}",
        "curtain_open": f"复杂窗帘打开{suffix}",
        "curtain_close": f"复杂窗帘关闭{suffix}",
    }
    content = (
        "# Complex One Shot Rules\n\n"
        "This Runtime Skill verifies several deterministic single-tool rules.\n"
        "Each trigger must route exactly one mesh action.\n\n"
        f"@rule trigger=\"{triggers['blue']}|蓝色复杂别名{suffix}\" target_role=control_agent action=set_status_light args={{\"color\":\"blue\"}}\n"
        f"@rule trigger=\"{triggers['red']}|红色复杂别名{suffix}\" target_role=control_agent action=set_status_light args={{\"color\":\"red\"}}\n"
        f"@rule trigger=\"{triggers['curtain_open']}\" target_role=control_agent action=set_curtain args={{\"state\":\"open\"}}\n"
        f"@rule trigger=\"{triggers['curtain_close']}\" target_role=control_agent action=set_curtain args={{\"state\":\"closed\"}}\n"
    )

    cases = [
        {
            "name": "blue_alias",
            "prompt": f"{marker}：请执行蓝色复杂别名{suffix}",
            "reply_action": "set_status_light",
            "requirements": {"blue_action": payload_contains("set to blue", "rgb=0,0,255")},
        },
        {
            "name": "red_direct",
            "prompt": f"{marker}：{triggers['red']}",
            "reply_action": "set_status_light",
            "requirements": {"red_action": payload_contains("set to red", "rgb=255,0,0")},
        },
        {
            "name": "curtain_open",
            "prompt": f"{marker}：{triggers['curtain_open']}",
            "reply_action": "set_curtain",
            "requirements": {"curtain_open": payload_contains("curtain", "open")},
        },
        {
            "name": "curtain_close",
            "prompt": f"{marker}：{triggers['curtain_close']}",
            "reply_action": "set_curtain",
            "requirements": {"curtain_close": payload_contains("curtain", "closed", "close")},
        },
    ]

    try:
        assert_mesh_roles_online(args.dashboard_url)
        install_reply = install_runtime_skill(args.dashboard_url, content)
        client = FeishuClient(args.chat_id)
        show_reply = client.send_and_wait(
            f"/skills_show {SKILL_NAME}",
            "complex-skill-show",
            lambda text: suffix in text or contains_any(text, [r"未找到", r"not found"]),
            60.0,
        )

        case_results: list[dict[str, Any]] = []
        for case in cases:
            baseline = {event_key(event) for event in dashboard_events(args.dashboard_url)}
            reply = client.send_and_wait(
                case["prompt"],
                f"complex-skill-{case['name']}",
                lambda text, action=case["reply_action"]: contains_any(
                    text,
                    [
                        r"已命中 skill 规则",
                        action,
                        r"MQTT Mesh",
                        r"失败",
                        r"Error",
                    ],
                ),
                args.reply_timeout,
            )
            matched, observed = wait_for_dashboard_events(
                args.dashboard_url,
                baseline,
                case["requirements"],
                args.execution_timeout,
            )
            acknowledged = contains_any(reply.content, [r"已命中 skill 规则", r"MQTT Mesh"]) and not contains_any(
                reply.content, [r"失败", r"Error"]
            )
            executed = set(case["requirements"]) <= set(matched)
            case_results.append(
                {
                    "name": case["name"],
                    "passed": acknowledged and executed,
                    "reply": reply.content,
                    "matched_events": matched,
                    "observed_event_count": len(observed),
                }
            )
    except (TestFailure, OSError) as exc:
        return print_result("complex_skill_rules", False, skill=SKILL_NAME, marker=marker, error=str(exc))

    persisted = suffix in show_reply.content
    passed = persisted and all(item["passed"] for item in case_results)
    return print_result(
        "complex_skill_rules",
        passed,
        skill=SKILL_NAME,
        marker=marker,
        install_reply=install_reply,
        show_reply=show_reply.content,
        checks={
            "persisted": persisted,
            "case_count": len(case_results),
            "passed_cases": sum(1 for item in case_results if item["passed"]),
        },
        cases=case_results,
    )


if __name__ == "__main__":
    raise SystemExit(main())

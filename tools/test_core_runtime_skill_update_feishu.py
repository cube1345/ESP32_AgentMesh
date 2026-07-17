#!/usr/bin/env python3
"""Verify same-name Runtime Skill hot update and cache invalidation."""

from __future__ import annotations

import argparse
import os
import time

from agent_mesh_testlib import (
    DEFAULT_CHAT_ID,
    DEFAULT_MQTT_HOST,
    DEFAULT_MQTT_PORT,
    DEFAULT_TOPIC_PREFIX,
    FeishuClient,
    TestFailure,
    contains_any,
    mqtt_runtime_skill_upsert,
    print_result,
    run_id,
)


SKILL_NAME = "runtime-verify-update"


def skill_content(marker: str, value: str) -> str:
    return (
        "# Runtime Verify Update\n\n"
        f"UPDATE_MARKER={marker}\n"
        f"UPDATE_VALUE={value}\n\n"
        "This is a runtime-only cache invalidation probe. Return UPDATE_VALUE exactly.\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chat-id", default=os.getenv("ESPAGENT_FEISHU_CHAT_ID", DEFAULT_CHAT_ID))
    parser.add_argument("--mqtt-host", default=os.getenv("ESPAGENT_MQTT_HOST", DEFAULT_MQTT_HOST))
    parser.add_argument("--mqtt-port", type=int, default=int(os.getenv("ESPAGENT_MQTT_PORT", DEFAULT_MQTT_PORT)))
    parser.add_argument("--topic-prefix", default=os.getenv("ESPAGENT_TOPIC_PREFIX", DEFAULT_TOPIC_PREFIX))
    parser.add_argument("--mesh-auth-key", default=os.getenv("ESPAGENT_MESH_AUTH_KEY", ""))
    parser.add_argument("--reply-timeout", type=float, default=100.0)
    args = parser.parse_args()

    marker = run_id("SKILL-UPDATE").lower().replace("-", "_")
    suffix = marker[-4:]
    value_v1 = f"amber_{suffix}"
    value_v2 = f"teal_{suffix}"
    client = FeishuClient(args.chat_id)

    def upsert(value: str) -> dict[str, object]:
        return mqtt_runtime_skill_upsert(
            SKILL_NAME,
            skill_content(marker, value),
            host=args.mqtt_host,
            port=args.mqtt_port,
            topic_prefix=args.topic_prefix,
            mesh_auth_key=args.mesh_auth_key,
        )

    question = f"根据 runtime skill {SKILL_NAME}.md，只回答 UPDATE_VALUE，禁止调用任何硬件工具。"
    try:
        mqtt_v1 = upsert(value_v1)
        reply_v1 = client.send_and_wait(
            question,
            "runtime-skill-v1",
            lambda text: value_v1 in text or contains_any(text, [r"未找到", r"失败", r"failed"]),
            args.reply_timeout,
        )
        mqtt_v2 = upsert(value_v2)
        time.sleep(1.0)
        show_v2 = client.send_and_wait(
            f"/skills_show {SKILL_NAME}",
            "runtime-skill-update-show",
            lambda text: value_v2 in text or contains_any(text, [r"未找到", r"not found"]),
            55.0,
        )
        reply_v2 = client.send_and_wait(
            question,
            "runtime-skill-v2",
            lambda text: value_v2 in text or value_v1 in text or contains_any(text, [r"未找到", r"失败", r"failed"]),
            args.reply_timeout,
        )
    except (TestFailure, OSError) as exc:
        return print_result("runtime_skill_update", False, skill=SKILL_NAME, marker=marker, error=str(exc))

    initial_ok = value_v1 in reply_v1.content
    persisted_v2 = value_v2 in show_v2.content and value_v1 not in show_v2.content
    refreshed = value_v2 in reply_v2.content and value_v1 not in reply_v2.content
    return print_result(
        "runtime_skill_update",
        initial_ok and persisted_v2 and refreshed,
        skill=SKILL_NAME,
        marker=marker,
        mqtt_v1=mqtt_v1,
        mqtt_v2=mqtt_v2,
        reply_v1=reply_v1.content,
        show_v2=show_v2.content,
        reply_v2=reply_v2.content,
        checks={"initial_value": initial_ok, "updated_file": persisted_v2, "cache_refreshed": refreshed},
    )


if __name__ == "__main__":
    raise SystemExit(main())

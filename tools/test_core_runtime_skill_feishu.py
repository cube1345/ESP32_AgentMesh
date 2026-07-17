#!/usr/bin/env python3
"""Verify wireless Runtime Skill injection and Agent understanding."""

from __future__ import annotations

import argparse
import os

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


SKILL_NAME = "runtime-verify-basic"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chat-id", default=os.getenv("ESPAGENT_FEISHU_CHAT_ID", DEFAULT_CHAT_ID))
    parser.add_argument("--mqtt-host", default=os.getenv("ESPAGENT_MQTT_HOST", DEFAULT_MQTT_HOST))
    parser.add_argument("--mqtt-port", type=int, default=int(os.getenv("ESPAGENT_MQTT_PORT", DEFAULT_MQTT_PORT)))
    parser.add_argument("--topic-prefix", default=os.getenv("ESPAGENT_TOPIC_PREFIX", DEFAULT_TOPIC_PREFIX))
    parser.add_argument("--mesh-auth-key", default=os.getenv("ESPAGENT_MESH_AUTH_KEY", ""))
    parser.add_argument("--reply-timeout", type=float, default=100.0)
    args = parser.parse_args()

    marker = run_id("SKILL").lower().replace("-", "_")
    color = f"cobalt_{marker[-4:]}"
    code = f"maple_{marker[-9:-5]}"
    content = (
        "# Runtime Verify Basic\n\n"
        f"VERIFY_MARKER={marker}\n"
        f"VERIFY_COLOR={color}\n"
        f"VERIFY_CODE={code}\n\n"
        "These values are runtime-only verification facts. Answer them exactly when this skill is named.\n"
    )

    try:
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
            "runtime-skill-show",
            lambda text: marker in text or contains_any(text, [r"未找到 skill", r"not found"]),
            55.0,
        )
        qa_prompt = (
            f"根据 runtime skill {SKILL_NAME}.md，只回答 VERIFY_COLOR 和 VERIFY_CODE 的值。"
            "禁止调用传感器或硬件工具。"
        )
        qa_reply = client.send_and_wait(
            qa_prompt,
            "runtime-skill-qa",
            lambda text: color in text or code in text or contains_any(text, [r"未找到", r"失败", r"failed"]),
            args.reply_timeout,
        )
    except (TestFailure, OSError) as exc:
        return print_result("runtime_skill", False, skill=SKILL_NAME, marker=marker, error=str(exc))

    persisted = marker in show_reply.content and color in show_reply.content and code in show_reply.content
    understood = color in qa_reply.content and code in qa_reply.content
    return print_result(
        "runtime_skill",
        persisted and understood,
        skill=SKILL_NAME,
        marker=marker,
        mqtt_reply=mqtt_reply,
        show_reply=show_reply.content,
        qa_reply=qa_reply.content,
        checks={"persisted": persisted, "understood": understood},
    )


if __name__ == "__main__":
    raise SystemExit(main())

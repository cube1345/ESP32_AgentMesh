#!/usr/bin/env python3
"""Verify deterministic reasoning delegation through the subagent route."""

from __future__ import annotations

import argparse
import os

from agent_mesh_testlib import DEFAULT_CHAT_ID, FeishuClient, TestFailure, contains_any, print_result, run_id


EXPECTED = "714"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chat-id", default=os.getenv("ESPAGENT_FEISHU_CHAT_ID", DEFAULT_CHAT_ID))
    parser.add_argument("--timeout", type=float, default=150.0)
    args = parser.parse_args()

    marker = run_id("SUBAGENT-MATH")
    prompt = (
        f"/subagent {marker}：请让子代理单独计算 (37 * 19) + 11。"
        "主Agent只能用一行返回 SUBAGENT_MATH=<子代理结果>；失败时返回 SUBAGENT_MATH_FAIL。"
    )
    try:
        reply = FeishuClient(args.chat_id).send_and_wait(
            prompt,
            "subagent-math",
            lambda text: contains_any(text, [r"SUBAGENT_MATH", EXPECTED, r"失败", r"failed"]),
            args.timeout,
        )
    except TestFailure as exc:
        return print_result("subagent_reasoning", False, marker=marker, error=str(exc))

    failed = contains_any(reply.content, [r"SUBAGENT_MATH_FAIL", r"子代理.*失败", r"subagent.*failed"])
    correct = EXPECTED in reply.content
    return print_result(
        "subagent_reasoning",
        correct and not failed,
        marker=marker,
        expected=EXPECTED,
        reply=reply.content,
        checks={"correct_result": correct, "contains_failure": failed},
    )


if __name__ == "__main__":
    raise SystemExit(main())

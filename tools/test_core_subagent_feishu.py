#!/usr/bin/env python3
"""Verify the subagent capability through Feishu without serial access."""

from __future__ import annotations

import argparse
import os
import re

from agent_mesh_testlib import DEFAULT_CHAT_ID, FeishuClient, TestFailure, contains_any, print_result, run_id


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--chat-id", default=os.getenv("ESPAGENT_FEISHU_CHAT_ID", DEFAULT_CHAT_ID))
    parser.add_argument("--timeout", type=float, default=150.0)
    args = parser.parse_args()

    marker = run_id("SUBAGENT")
    prompt = (
        f"/subagent {marker}：请调用子代理查询当前时间。主Agent收到结果后，"
        "用一句话返回查询时间；如果子代理失败，请明确回复失败原因。"
    )
    try:
        reply = FeishuClient(args.chat_id).send_and_wait(
            prompt,
            "subagent",
            lambda text: contains_any(text, [r"子代理", r"subagent", r"\d{1,2}[:：点]\d{2}", r"失败", r"failed"]),
            args.timeout,
        )
    except TestFailure as exc:
        return print_result("subagent", False, marker=marker, error=str(exc))

    failed = contains_any(reply.content, [r"子代理.*失败", r"subagent.*failed", r"LLM call failed", r"执行失败"])
    has_time = re.search(r"(?:[01]?\d|2[0-3])[:：]\d{2}|\d{1,2}点\d{1,2}分", reply.content) is not None
    return print_result(
        "subagent",
        has_time and not failed,
        marker=marker,
        reply=reply.content,
        checks={"contains_time": has_time, "contains_failure": failed},
    )


if __name__ == "__main__":
    raise SystemExit(main())

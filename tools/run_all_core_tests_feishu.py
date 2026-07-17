#!/usr/bin/env python3
"""Run all no-serial ESPAgent core capability tests sequentially."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path


TESTS = [
    "tools/test_core_subagent_feishu.py",
    "tools/test_core_subagent_reasoning_feishu.py",
    "tools/test_core_sandbox_feishu.py",
    "tools/test_core_sandbox_path_feishu.py",
    "tools/test_core_workflow_feishu.py",
    "tools/test_core_workflow_colors_feishu.py",
    "tools/test_core_rule_feishu.py",
    "tools/test_core_rule_else_feishu.py",
    "tools/test_core_runtime_skill_feishu.py",
    "tools/test_core_runtime_skill_update_feishu.py",
]


def parse_last_json(output: str) -> dict[str, object]:
    start = output.rfind("\n{")
    if start >= 0:
        start += 1
    else:
        start = output.find("{")
    if start < 0:
        return {}
    try:
        parsed = json.loads(output[start:])
    except json.JSONDecodeError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout-per-test", type=float, default=240.0)
    parser.add_argument("--stop-on-fail", action="store_true")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    results: list[dict[str, object]] = []

    for index, script in enumerate(TESTS, start=1):
        started = time.monotonic()
        print(f"===== [{index}/{len(TESTS)}] {script} =====", flush=True)
        try:
            proc = subprocess.run(
                [sys.executable, script],
                cwd=repo_root,
                text=True,
                capture_output=True,
                timeout=args.timeout_per_test,
            )
        except subprocess.TimeoutExpired as exc:
            elapsed = time.monotonic() - started
            result = {
                "script": script,
                "passed": False,
                "exit_code": 124,
                "elapsed_s": round(elapsed, 1),
                "error": f"timeout after {args.timeout_per_test:.0f}s",
                "stdout": (exc.stdout or "")[-1200:],
                "stderr": (exc.stderr or "")[-1200:],
            }
            results.append(result)
            print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
            if args.stop_on_fail:
                break
            continue

        elapsed = time.monotonic() - started
        if proc.stdout:
            print(proc.stdout.rstrip(), flush=True)
        if proc.stderr:
            print("----- stderr -----", flush=True)
            print(proc.stderr.rstrip(), flush=True)

        parsed = parse_last_json(proc.stdout)
        result = {
            "script": script,
            "passed": bool(parsed.get("passed")) and proc.returncode == 0,
            "exit_code": proc.returncode,
            "elapsed_s": round(elapsed, 1),
            "capability": parsed.get("capability", Path(script).stem),
        }
        if not result["passed"]:
            result["stdout_tail"] = proc.stdout[-1600:]
            result["stderr_tail"] = proc.stderr[-1600:]
        results.append(result)
        print(f"RESULT {script}: {'PASS' if result['passed'] else 'FAIL'} ({elapsed:.1f}s)", flush=True)
        if args.stop_on_fail and not result["passed"]:
            break

    passed = sum(1 for item in results if item["passed"])
    summary = {
        "passed": passed,
        "total": len(results),
        "failed": [item for item in results if not item["passed"]],
    }
    print("===== SUMMARY =====", flush=True)
    print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)
    return 0 if passed == len(TESTS) else 1


if __name__ == "__main__":
    raise SystemExit(main())

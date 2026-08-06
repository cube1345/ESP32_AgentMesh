#!/usr/bin/env python3
"""Analyze ESPAgent HEAP_STAGE logs from a file or a live serial port."""

from __future__ import annotations

import argparse
import json
import os
import re
import select
import sys
import termios
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path


FIELD_RE = re.compile(r"([a-zA-Z_]+)=([^\s]+)")


@dataclass
class HeapSample:
    stage: str
    internal_free: int
    internal_largest: int
    psram_free: int
    tasks: int


@dataclass
class Report:
    samples: list[HeapSample] = field(default_factory=list)
    alert_kinds: Counter[str] = field(default_factory=Counter)
    allocation_failures: int = 0
    integrity_failures: int = 0

    def consume(self, line: str) -> None:
        if "HEAP_STAGE" in line:
            fields = dict(FIELD_RE.findall(line))
            try:
                self.samples.append(
                    HeapSample(
                        stage=fields["stage"],
                        internal_free=int(fields["internal_free"]),
                        internal_largest=int(fields["internal_largest"]),
                        psram_free=int(fields["psram_free"]),
                        tasks=int(fields["tasks"]),
                    )
                )
            except (KeyError, ValueError):
                pass

        if "HEAP_ALERT" in line:
            fields = dict(FIELD_RE.findall(line))
            kind = fields.get("kind", "unknown")
            self.alert_kinds[kind] += 1
            if kind == "corruption":
                self.integrity_failures += 1

        if "HEAP_ALLOC_FAIL" in line:
            fields = dict(FIELD_RE.findall(line))
            try:
                self.allocation_failures += int(fields.get("new", "1"))
            except ValueError:
                self.allocation_failures += 1

    def summary(self) -> dict[str, object]:
        completed = [sample for sample in self.samples if sample.stage.startswith("turn_end")]
        result: dict[str, object] = {
            "samples": len(self.samples),
            "completed_turns": len(completed),
            "allocation_failures": self.allocation_failures,
            "integrity_failures": self.integrity_failures,
            "alerts": dict(self.alert_kinds),
        }
        if not completed:
            result["verdict"] = "insufficient_data"
            return result

        first = completed[0]
        last = completed[-1]
        result.update(
            {
                "internal_first": first.internal_free,
                "internal_last": last.internal_free,
                "internal_drift": last.internal_free - first.internal_free,
                "largest_first": first.internal_largest,
                "largest_last": last.internal_largest,
                "largest_drift": last.internal_largest - first.internal_largest,
                "psram_drift": last.psram_free - first.psram_free,
                "task_min": min(sample.tasks for sample in completed),
                "task_max": max(sample.tasks for sample in completed),
            }
        )

        if self.integrity_failures or self.allocation_failures:
            verdict = "failed"
        elif any(
            self.alert_kinds[kind]
            for kind in ("suspected_leak", "fragmentation", "task_retention", "low_internal")
        ):
            verdict = "warning"
        elif len(completed) < 3:
            verdict = "insufficient_data"
        else:
            verdict = "stable"
        result["verdict"] = verdict
        return result


def configure_serial(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def consume_text(report: Report, text: str, echo: bool = False) -> None:
    for line in text.splitlines():
        if echo:
            print(line)
        report.consume(line)


def read_serial(report: Report, port: str, duration: float, echo: bool) -> None:
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    pending = ""
    try:
        configure_serial(fd)
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            readable, _, _ = select.select([fd], [], [], 0.25)
            if not readable:
                continue
            data = os.read(fd, 8192)
            if not data:
                continue
            pending += data.decode("utf-8", "replace")
            lines = pending.split("\n")
            pending = lines.pop()
            consume_text(report, "\n".join(lines), echo)
        if pending:
            consume_text(report, pending, echo)
    finally:
        os.close(fd)


def print_human(summary: dict[str, object]) -> None:
    print("===== ESPAgent heap analysis =====")
    print(f"verdict:             {summary['verdict']}")
    print(f"completed turns:     {summary['completed_turns']}")
    print(f"allocation failures: {summary['allocation_failures']}")
    print(f"integrity failures:  {summary['integrity_failures']}")
    print(f"alerts:              {json.dumps(summary['alerts'], ensure_ascii=False)}")
    if summary["completed_turns"]:
        print(f"internal drift:      {summary['internal_drift']:+d} bytes")
        print(f"largest drift:       {summary['largest_drift']:+d} bytes")
        print(f"PSRAM drift:         {summary['psram_drift']:+d} bytes")
        print(f"task range:          {summary['task_min']}..{summary['task_max']}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze HEAP_STAGE/HEAP_ALERT logs from a file or serial port"
    )
    parser.add_argument("source", help="Log file path or serial device such as /dev/ttyUSB0")
    parser.add_argument("--duration", type=float, default=60.0, help="Live serial capture seconds")
    parser.add_argument("--echo", action="store_true", help="Echo live serial lines")
    parser.add_argument("--json", action="store_true", help="Print JSON summary")
    args = parser.parse_args()

    report = Report()
    source = Path(args.source)
    try:
        if args.source.startswith("/dev/"):
            if not source.exists():
                print(f"ERROR: missing serial port {source}", file=sys.stderr)
                return 2
            read_serial(report, args.source, args.duration, args.echo)
        else:
            consume_text(report, source.read_text(encoding="utf-8", errors="replace"))
    except (OSError, termios.error) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    summary = report.summary()
    if args.json:
        print(json.dumps(summary, ensure_ascii=False, indent=2))
    else:
        print_human(summary)
    return 1 if summary["verdict"] in {"warning", "failed"} else 0


if __name__ == "__main__":
    raise SystemExit(main())

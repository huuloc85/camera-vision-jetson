#!/usr/bin/env python3
"""Summarize vision and trigger-to-display timings from a production log."""

from __future__ import annotations

import argparse
import math
import re
from pathlib import Path


TIMING_RE = re.compile(
    r"Trigger timing: busy=(?P<busy>[\d.]+)ms "
    r"camera=(?P<camera>[\d.]+)ms detect=(?P<detect>[\d.]+)ms "
    r"gpio=(?P<gpio>[\d.]+)ms hmi_frame=(?P<hmi>[\d.]+)ms "
    r"critical=(?P<critical>[\d.]+)ms total=(?P<total>[\d.]+)ms"
)
DISPLAY_RE = re.compile(
    r"Trigger displayed: processing=(?P<processing>[\d.]+)ms "
    r"present=(?P<present>[\d.]+)ms total=(?P<total>[\d.]+)ms"
)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = max(0, math.ceil(len(ordered) * fraction) - 1)
    return ordered[index]


def summary(label: str, values: list[float]) -> str:
    average = sum(values) / len(values)
    return (
        f"{label}: n={len(values)} avg={average:.2f}ms "
        f"p95={percentile(values, 0.95):.2f}ms max={max(values):.2f}ms"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize jetson-inspect-v2 Trigger timing logs"
    )
    parser.add_argument("log", type=Path, help="Path to the application log")
    args = parser.parse_args()

    text = args.log.read_text(encoding="utf-8", errors="replace")
    timing = [match.groupdict() for match in TIMING_RE.finditer(text)]
    displayed = [float(match.group("total")) for match in DISPLAY_RE.finditer(text)]

    if not timing:
        print("No 'Trigger timing' samples found.")
        return 1

    detect = [float(sample["detect"]) for sample in timing]
    print(summary("Vision detect", detect))
    detect_average = sum(detect) / len(detect)
    if detect_average > 0.0:
        print(f"Vision equivalent throughput: {1000.0 / detect_average:.1f} fps")
    else:
        print("Vision equivalent throughput: >1000 fps (log rounded detect to 0ms)")
    for key, label in (
        ("camera", "Camera"),
        ("critical", "Trigger to GPIO"),
        ("total", "Vision cycle total"),
    ):
        print(summary(label, [float(sample[key]) for sample in timing]))

    if displayed:
        print(summary("Trigger displayed", displayed))
        enough_samples = len(displayed) >= 20
        passed = enough_samples and max(displayed) < 50.0
        if not enough_samples:
            print(f"Display acceptance: INSUFFICIENT ({len(displayed)}/20 samples)")
        else:
            print(f"Display acceptance: {'PASS' if passed else 'FAIL'} (max < 50ms)")
    else:
        print("No 'Trigger displayed' samples found; display acceptance unavailable.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compare median production-benchmark throughput across two revisions."""

from __future__ import annotations

import argparse
import csv
import glob
import math
import os
import statistics
from collections import defaultdict
from pathlib import Path


Key = tuple[str, str, str, str, str]


def load(pattern: str) -> dict[Key, list[float]]:
    samples: dict[Key, list[float]] = defaultdict(list)
    paths = sorted(glob.glob(pattern))
    if not paths:
        raise RuntimeError(f"no benchmark CSV files match {pattern!r}")
    for path in paths:
        with open(path, newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                throughput = float(row["throughput_per_second"])
                if throughput <= 0:
                    continue
                key = (
                    row["subsystem"], row["scenario"], row["producers"],
                    row["workers"], row["message_bytes"],
                )
                samples[key].append(throughput)
    return samples


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--minimum-dispatch-gain", type=float)
    parser.add_argument("--maximum-dispatch-scenario-regression", type=float)
    parser.add_argument("--maximum-subsystem-regression", type=float)
    args = parser.parse_args()

    baseline = load(args.baseline)
    candidate = load(args.candidate)
    rows: list[tuple[Key, float, float, float]] = []
    dispatch_ratios: list[float] = []
    for key in sorted(baseline.keys() & candidate.keys()):
        old = statistics.median(baseline[key])
        new = statistics.median(candidate[key])
        ratio = new / old
        rows.append((key, old, new, (ratio - 1.0) * 100.0))
        if key[0] == "worker" and key[1].startswith("dispatch_"):
            dispatch_ratios.append(ratio)

    if not dispatch_ratios:
        raise RuntimeError("no common WorkerPool dispatch scenarios were found")
    geometric_gain = (
        math.exp(sum(math.log(value) for value in dispatch_ratios)
                 / len(dispatch_ratios)) - 1.0
    ) * 100.0
    subsystem_ratios: dict[str, list[float]] = defaultdict(list)
    for key, old, new, _ in rows:
        subsystem_ratios[key[0]].append(new / old)
    subsystem_gains = {
        subsystem: (
            math.exp(sum(math.log(value) for value in ratios) / len(ratios)) - 1.0
        ) * 100.0
        for subsystem, ratios in subsystem_ratios.items()
    }
    worst_key, _, _, worst_delta = min(rows, key=lambda row: row[3])
    dispatch_rows = [
        row for row in rows
        if row[0][0] == "worker" and row[0][1].startswith("dispatch_")
    ]
    worst_dispatch_key, _, _, worst_dispatch_delta = min(
        dispatch_rows, key=lambda row: row[3]
    )
    worst_subsystem, worst_subsystem_gain = min(
        subsystem_gains.items(), key=lambda item: item[1]
    )

    report = [
        "# Version benchmark comparison",
        "",
        f"WorkerPool dispatch geometric-mean gain: **{geometric_gain:.1f}%**.",
        "",
        "Subsystem geometric-mean changes: " + ", ".join(
            f"**{name} {gain:+.1f}%**"
            for name, gain in sorted(subsystem_gains.items())
        ) + ".",
        "",
        f"Worst common scenario: **{worst_key[0]}/{worst_key[1]} "
        f"{worst_delta:+.1f}%**.",
        "",
        "Medians are calculated from identical scenario keys and independent CSV runs.",
        "",
        "| Subsystem | Scenario | Producers | Workers | Bytes | Baseline ops/s | Candidate ops/s | Delta |",
        "|---|---|---:|---:|---:|---:|---:|---:|",
    ]
    for key, old, new, delta in rows:
        report.append(
            f"| {key[0]} | {key[1]} | {key[2]} | {key[3]} | {key[4]} | "
            f"{old:,.0f} | {new:,.0f} | {delta:+.1f}% |"
        )
    text = "\n".join(report) + "\n"
    Path(args.output).write_text(text, encoding="utf-8")
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(text)
    print(text)

    if (args.minimum_dispatch_gain is not None
            and geometric_gain < args.minimum_dispatch_gain):
        print(
            f"required gain {args.minimum_dispatch_gain:.1f}% was not reached",
            file=os.sys.stderr,
        )
        return 1
    if (args.maximum_dispatch_scenario_regression is not None
            and worst_dispatch_delta < -args.maximum_dispatch_scenario_regression):
        print(
            f"WorkerPool dispatch scenario {worst_dispatch_key} regressed by "
            f"{worst_dispatch_delta:.1f}%; limit is "
            f"-{args.maximum_dispatch_scenario_regression:.1f}%",
            file=os.sys.stderr,
        )
        return 1
    if (args.maximum_subsystem_regression is not None
            and worst_subsystem_gain < -args.maximum_subsystem_regression):
        print(
            f"subsystem {worst_subsystem} regressed by "
            f"{worst_subsystem_gain:.1f}%; limit is "
            f"-{args.maximum_subsystem_regression:.1f}%",
            file=os.sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

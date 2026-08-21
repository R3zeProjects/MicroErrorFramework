#!/usr/bin/env python3
"""Summarize repeated benchmark samples across MEF release tags."""

from __future__ import annotations

import argparse
import csv
import glob
import math
import re
import statistics
from collections import defaultdict
from pathlib import Path


VERSION_PATTERN = re.compile(r"(v\d+\.\d+\.\d+-beta)")
MICRO_PATTERN = re.compile(
    r"^(\S+).*?(?:operations|tasks)_per_second=([0-9.eE+-]+)$"
)
Key = tuple[str, str, str, str, str]


def version_of(path: str) -> str:
    match = VERSION_PATTERN.search(Path(path).name)
    if match is None:
        raise RuntimeError(f"cannot extract a release version from {path!r}")
    return match.group(1)


def version_key(version: str) -> tuple[int, int, int]:
    numbers = re.search(r"(\d+)\.(\d+)\.(\d+)", version)
    if numbers is None:
        raise RuntimeError(f"invalid release version {version!r}")
    return tuple(int(value) for value in numbers.groups())


def geometric_mean(values: list[float]) -> float:
    return math.exp(sum(math.log(value) for value in values) / len(values))


def change(current: float, baseline: float) -> float:
    return (current / baseline - 1.0) * 100.0


def rate(value: float) -> str:
    if value >= 1_000_000:
        return f"{value / 1_000_000:.2f}M"
    if value >= 1_000:
        return f"{value / 1_000:.1f}k"
    return f"{value:.0f}"


def validate_samples(
    dataset: str,
    samples: dict[str, dict[object, list[float]]],
    expected_samples: int,
) -> None:
    for version, scenarios in samples.items():
        for scenario, values in scenarios.items():
            if len(values) != expected_samples:
                raise RuntimeError(
                    f"{dataset} {version} {scenario!r} has {len(values)} samples; "
                    f"expected {expected_samples}"
                )


def load_micro(pattern: str, expected_samples: int) -> dict[str, dict[str, float]]:
    samples: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    paths = sorted(glob.glob(pattern))
    if not paths:
        raise RuntimeError(f"no microbenchmark files match {pattern!r}")
    for path in paths:
        version = version_of(path)
        for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
            match = MICRO_PATTERN.match(line)
            if match:
                samples[version][match.group(1)].append(float(match.group(2)))
    validate_samples("micro", samples, expected_samples)
    return {
        version: {
            scenario: statistics.median(values)
            for scenario, values in scenarios.items()
        }
        for version, scenarios in samples.items()
    }


def load_production(pattern: str, expected_samples: int) -> dict[str, dict[Key, float]]:
    samples: dict[str, dict[Key, list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    paths = sorted(glob.glob(pattern))
    if not paths:
        raise RuntimeError(f"no production benchmark files match {pattern!r}")
    for path in paths:
        version = version_of(path)
        with open(path, newline="", encoding="utf-8-sig") as stream:
            for row in csv.DictReader(stream):
                throughput = float(row["throughput_per_second"])
                if throughput <= 0:
                    continue
                key = (
                    row["subsystem"], row["scenario"], row["producers"],
                    row["workers"], row["message_bytes"],
                )
                samples[version][key].append(throughput)
    validate_samples("production", samples, expected_samples)
    return {
        version: {
            key: statistics.median(values)
            for key, values in scenarios.items()
        }
        for version, scenarios in samples.items()
    }


def append_micro_report(
    report: list[str], micro: dict[str, dict[str, float]]
) -> None:
    versions = sorted(micro, key=version_key)
    scenarios = [
        "single", "multi", "async", "worker_pool", "worker_dispatch",
        "worker_bulk_dispatch",
    ]
    labels = ["Single", "Multi", "Async", "Tracked", "Dispatch", "Bulk"]
    baseline = micro[versions[0]]
    report.extend([
        "## Identical cross-release microbenchmark",
        "",
        "The benchmark source has the same Git blob in every tagged release. "
        "Values are medians of seven runs and are shown as operations/tasks per second.",
        "",
        "| Version | " + " | ".join(labels) + " | Geo vs 0.1.0 | Geo vs previous |",
        "|---|" + "---:|" * (len(labels) + 2),
    ])
    previous: dict[str, float] | None = None
    for version in versions:
        values = micro[version]
        ratios = [values[name] / baseline[name] for name in scenarios]
        baseline_gain = (geometric_mean(ratios) - 1.0) * 100.0
        previous_gain = None if previous is None else (
            geometric_mean([values[name] / previous[name] for name in scenarios]) - 1.0
        ) * 100.0
        formatted = " | ".join(rate(values[name]) for name in scenarios)
        prior = "—" if previous_gain is None else f"{previous_gain:+.1f}%"
        report.append(
            f"| {version} | {formatted} | {baseline_gain:+.1f}% | {prior} |"
        )
        previous = values
    report.append("")


def append_production_report(
    report: list[str], production: dict[str, dict[Key, float]]
) -> None:
    versions = sorted(production, key=version_key)
    common = set.intersection(*(set(production[version]) for version in versions))
    baseline_version = versions[0]
    baseline = production[baseline_version]
    subsystems = sorted({key[0] for key in common})
    report.extend([
        "## Common production scenarios",
        "",
        f"The intersection contains {len(common)} identical scenario keys across "
        f"{len(versions)} releases. Each cell is the subsystem geometric-mean "
        f"throughput change relative to {baseline_version}. The timed bodies of "
        "common scenarios are unchanged; later harness revisions only add suite "
        "selection or new scenarios.",
        "",
        "| Version | " + " | ".join(subsystems) + " | Overall |",
        "|---|" + "---:|" * (len(subsystems) + 1),
    ])
    for version in versions:
        values = production[version]
        subsystem_cells: list[str] = []
        all_ratios: list[float] = []
        for subsystem in subsystems:
            keys = sorted(key for key in common if key[0] == subsystem)
            ratios = [values[key] / baseline[key] for key in keys]
            all_ratios.extend(ratios)
            subsystem_cells.append(f"{(geometric_mean(ratios) - 1.0) * 100.0:+.1f}%")
        overall = (geometric_mean(all_ratios) - 1.0) * 100.0
        report.append(
            f"| {version} | " + " | ".join(subsystem_cells) + f" | {overall:+.1f}% |"
        )

    selected = [
        ("logger", "parallel_sharded", "1", "0", "128"),
        ("logger", "parallel_sharded", "16", "0", "128"),
        ("worker", "dispatch_q1024", "1", "4", "0"),
        ("worker", "dispatch_q1024", "4", "4", "0"),
        ("register", "same_category_add", "1", "0", "10"),
        ("register", "same_category_add", "16", "0", "10"),
    ]
    selected = [key for key in selected if key in common]
    report.extend([
        "",
        "### Selected production hot paths",
        "",
        "| Version | " + " | ".join(
            f"{key[0]}/{key[1]} p{key[2]} w{key[3]}" for key in selected
        ) + " |",
        "|---|" + "---:|" * len(selected),
    ])
    for version in versions:
        report.append(
            f"| {version} | " + " | ".join(
                rate(production[version][key]) for key in selected
            ) + " |"
        )

    newest = versions[-1]
    previous = versions[-2]
    newest_only = sorted(set(production[newest]) - set(production[previous]))
    if newest_only:
        report.extend([
            "",
            f"### Scenarios introduced in {newest}",
            "",
            "No percentage comparison is reported because the preceding release has no "
            "equivalent API scenario.",
            "",
            "| Subsystem | Scenario | Producers | Median throughput |",
            "|---|---|---:|---:|",
        ])
        for key in newest_only:
            report.append(
                f"| {key[0]} | {key[1]} | {key[2]} | {rate(production[newest][key])} |"
            )
    report.append("")


def write_csv(
    path: str,
    micro: dict[str, dict[str, float]],
    production: dict[str, dict[Key, float]],
) -> None:
    with open(path, "w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow([
            "dataset", "version", "subsystem", "scenario", "producers",
            "workers", "message_bytes", "median_throughput_per_second",
        ])
        for version in sorted(micro, key=version_key):
            for scenario, throughput in sorted(micro[version].items()):
                writer.writerow(["micro", version, "mixed", scenario, "", "", "", throughput])
        for version in sorted(production, key=version_key):
            for key, throughput in sorted(production[version].items()):
                writer.writerow(["production", version, *key, throughput])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--micro", required=True)
    parser.add_argument("--production", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--csv-output", required=True)
    parser.add_argument("--samples", type=int, default=7)
    args = parser.parse_args()

    if args.samples <= 0:
        parser.error("--samples must be positive")
    micro = load_micro(args.micro, args.samples)
    production = load_production(args.production, args.samples)
    report = [
        "# Historical release benchmark comparison",
        "",
        "All binaries were built in Release mode with Clang 22.1.6 and Ninja on "
        "the same host. Version order alternated between samples to reduce thermal "
        "and scheduling bias.",
        "",
    ]
    append_micro_report(report, micro)
    append_production_report(report, production)
    report.extend([
        "## Interpretation",
        "",
        "Throughput is hardware- and scheduler-dependent. Patch releases with no "
        "runtime changes should be treated as repeated controls; small differences "
        "between them estimate measurement noise rather than code improvement. "
        "Only identical scenario keys are used for historical percentages.",
        "",
    ])
    text = "\n".join(report)
    Path(args.output).write_text(text, encoding="utf-8")
    write_csv(args.csv_output, micro, production)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

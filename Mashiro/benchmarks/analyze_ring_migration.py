#!/usr/bin/env python3
"""Analyze paired Mashiro-to-Sora RingBuffer migration measurements."""

from __future__ import annotations

import argparse
import csv
import math
import random
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


PAIR_LABELS = {
    "mashiro_self": ("Mashiro self A", "Mashiro self B"),
    "sora_self": ("Sora self A", "Sora self B"),
    "forward": ("Mashiro forward", "Sora forward"),
    "reverse": ("Mashiro reverse", "Sora reverse"),
}
EXPECTED_QUEUES = frozenset(label for pair in PAIR_LABELS.values() for label in pair)


@dataclass(frozen=True, order=True)
class CaseKey:
    scenario: str
    payload_bytes: int
    capacity: int
    producers: int
    consumers: int


@dataclass
class CaseResult:
    key: CaseKey
    forward: list[float]
    reverse: list[float]
    mashiro_self: list[float]
    sora_self: list[float]

    def balanced_log_ratio(self) -> float:
        return (statistics.median(map(math.log, self.forward)) +
                statistics.median(map(math.log, self.reverse))) / 2.0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--bootstrap", type=int, default=20_000)
    parser.add_argument("--seed", type=int, default=0x534F5241)
    parser.add_argument("--margin", type=float, default=0.05)
    return parser.parse_args()


def percentile(sorted_values: list[float], probability: float) -> float:
    position = probability * (len(sorted_values) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return sorted_values[lower]
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def confidence_interval(values: list[float]) -> tuple[float, float]:
    values.sort()
    return percentile(values, 0.025), percentile(values, 0.975)


def load_cases(path: Path) -> list[CaseResult]:
    rows: dict[CaseKey, dict[str, dict[int, tuple[float, int, int]]]] = defaultdict(lambda: defaultdict(dict))
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            key = CaseKey(
                scenario=row["scenario"],
                payload_bytes=int(row["payload_bytes"]),
                capacity=int(row["capacity"]),
                producers=int(row["producers"]),
                consumers=int(row["consumers"]),
            )
            queue = row["queue"]
            if queue not in EXPECTED_QUEUES:
                continue
            sample = int(row["sample"])
            throughput = float(row["million_transfers_per_second"])
            transfers = int(row["transfers"])
            queue_bytes = int(row["queue_bytes"])
            if not math.isfinite(throughput) or throughput <= 0.0 or transfers <= 0:
                raise ValueError(f"invalid measurement for {key} / {queue} / sample {sample}")
            if row["thread_placement"] != "cpu-set-high-priority":
                raise ValueError("formal migration analysis requires CPU-set-high-priority placement")
            if sample in rows[key][queue]:
                raise ValueError(f"duplicate measurement for {key} / {queue} / sample {sample}")
            rows[key][queue][sample] = throughput, transfers, queue_bytes

    results: list[CaseResult] = []
    for key, queues in sorted(rows.items()):
        if set(queues) != EXPECTED_QUEUES:
            missing = EXPECTED_QUEUES - set(queues)
            raise ValueError(f"incomplete controls for {key}: missing {sorted(missing)}")

        pair_ratios: dict[str, list[float]] = {}
        for pair_name, (first, second) in PAIR_LABELS.items():
            if set(queues[first]) != set(queues[second]):
                raise ValueError(f"unpaired samples for {key} / {pair_name}")
            ratios = []
            for sample in sorted(queues[first]):
                first_rate, first_transfers, first_bytes = queues[first][sample]
                second_rate, second_transfers, second_bytes = queues[second][sample]
                if first_transfers != second_transfers or first_bytes != second_bytes:
                    raise ValueError(f"unequal work or storage for {key} / {pair_name} / sample {sample}")
                ratios.append(second_rate / first_rate)
            pair_ratios[pair_name] = ratios

        results.append(CaseResult(key=key, **pair_ratios))
    if not results:
        raise ValueError("input contains no migration measurements")
    return results


def bootstrap_case(case: CaseResult, count: int, rng: random.Random) -> tuple[float, float]:
    values = []
    for _ in range(count):
        forward = [rng.choice(case.forward) for _ in case.forward]
        reverse = [rng.choice(case.reverse) for _ in case.reverse]
        log_ratio = (statistics.median(map(math.log, forward)) +
                     statistics.median(map(math.log, reverse))) / 2.0
        values.append(math.exp(log_ratio))
    return confidence_interval(values)


def bootstrap_group(cases: list[CaseResult], count: int, rng: random.Random) -> tuple[float, float]:
    values = []
    for _ in range(count):
        sampled_cases = [rng.choice(cases) for _ in cases]
        logs = []
        for case in sampled_cases:
            forward = [rng.choice(case.forward) for _ in case.forward]
            reverse = [rng.choice(case.reverse) for _ in case.reverse]
            logs.append((statistics.median(map(math.log, forward)) +
                         statistics.median(map(math.log, reverse))) / 2.0)
        values.append(math.exp(statistics.fmean(logs)))
    return confidence_interval(values)


def geometric_mean(values: list[float]) -> float:
    return math.exp(statistics.fmean(map(math.log, values)))


def verdict(low: float, high: float, margin: float) -> str:
    if low >= 1.0 - margin and high <= 1.0 + margin:
        return f"equivalent within +/-{margin:.0%}"
    if low >= 1.0 - margin:
        return f"no >{margin:.0%} regression; improvement not bounded"
    return f"cannot exclude >{margin:.0%} regression"


def render_report(cases: list[CaseResult], bootstrap_count: int, seed: int, margin: float) -> str:
    rng = random.Random(seed)
    grouped: dict[str, list[CaseResult]] = defaultdict(list)
    for case in cases:
        grouped[case.key.scenario].append(case)

    lines = [
        "# Mashiro-to-Sora RingBuffer migration audit",
        "",
        f"Cases: {len(cases)}; hierarchical bootstrap replicates: {bootstrap_count}; equivalence margin: +/-{margin:.0%}.",
        "The ratio is Sora throughput / Mashiro throughput. Forward and reverse template placements are",
        "combined geometrically before aggregation.",
        "",
        "## Aggregate",
        "",
        "| Scope | Cases | Ratio | 95% CI | Verdict |",
        "|---|---:|---:|---:|---|",
    ]
    all_groups = [("Overall", cases), *sorted(grouped.items())]
    for name, group in all_groups:
        ratio = math.exp(statistics.fmean(case.balanced_log_ratio() for case in group))
        low, high = bootstrap_group(group, bootstrap_count, rng)
        lines.append(f"| {name} | {len(group)} | {ratio:.4f}x | [{low:.4f}, {high:.4f}] | "
                     f"{verdict(low, high, margin)} |")

    mashiro_self = geometric_mean([
        statistics.median(case.mashiro_self) for case in cases
    ])
    sora_self = geometric_mean([
        statistics.median(case.sora_self) for case in cases
    ])
    lines.extend([
        "",
        "## Self-controls",
        "",
        f"- Mashiro B/A geometric mean: {mashiro_self:.4f}x.",
        f"- Sora B/A geometric mean: {sora_self:.4f}x.",
        f"- Relative self-control bias (Sora / Mashiro): {sora_self / mashiro_self:.4f}x.",
        "",
        "## Per case",
        "",
        "| Topology | Payload | Capacity | Threads | Ratio | 95% CI |",
        "|---|---:|---:|---:|---:|---:|",
    ])
    for case in cases:
        ratio = math.exp(case.balanced_log_ratio())
        low, high = bootstrap_case(case, bootstrap_count, rng)
        threads = f"{case.key.producers}P{case.key.consumers}C"
        lines.append(f"| {case.key.scenario} | {case.key.payload_bytes} B | {case.key.capacity} | {threads} | "
                     f"{ratio:.4f}x | [{low:.4f}, {high:.4f}] |")
    return "\n".join(lines) + "\n"


def main() -> None:
    args = parse_args()
    if args.bootstrap <= 0 or not 0.0 < args.margin < 1.0:
        raise ValueError("bootstrap must be positive and margin must be between zero and one")
    report = render_report(load_cases(args.input), args.bootstrap, args.seed, args.margin)
    print(report, end="")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")


if __name__ == "__main__":
    main()

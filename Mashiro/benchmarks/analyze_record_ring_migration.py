#!/usr/bin/env python3
"""Analyze paired RecordRingBuffer migration measurements with bootstrap confidence intervals."""

from __future__ import annotations

import argparse
import csv
import math
import random
import statistics
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, order=True)
class CaseKey:
    topology: str
    payload_bytes: int
    byte_capacity: int
    producers: int
    consumers: int


@dataclass
class CaseResult:
    key: CaseKey
    ratios: list[float]

    def estimate(self) -> float:
        return statistics.median(self.ratios)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--mode", choices=("migration", "logger"), required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--bootstrap", type=int, default=20_000)
    parser.add_argument("--seed", type=int, default=0x5245434F5244)
    parser.add_argument("--margin", type=float, default=0.05)
    return parser.parse_args()


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    position = probability * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def interval(values: list[float]) -> tuple[float, float]:
    return percentile(values, 0.025), percentile(values, 0.975)


def geometric_mean(values: list[float]) -> float:
    return math.exp(statistics.fmean(map(math.log, values)))


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError("input contains no measurements")
    for row in rows:
        throughput = float(row["million_records_per_second"])
        if not math.isfinite(throughput) or throughput <= 0.0:
            raise ValueError("throughput values must be finite and positive")
        if int(row["records"]) <= 0:
            raise ValueError("record counts must be positive")
        if row["thread_placement"] != "cpu-set-high-priority":
            raise ValueError("formal analysis requires CPU-set-high-priority placement")
    return rows


def case_key(row: dict[str, str]) -> CaseKey:
    return CaseKey(
        topology=row["topology"],
        payload_bytes=int(row["payload_bytes"]),
        byte_capacity=int(row["byte_capacity"]),
        producers=int(row["producers"]),
        consumers=int(row["consumers"]),
    )


def pair_samples(rows: list[dict[str, str]], pair: str) -> dict[CaseKey, dict[int, dict[str, float]]]:
    paired: dict[CaseKey, dict[int, dict[str, float]]] = defaultdict(lambda: defaultdict(dict))
    for row in rows:
        if row["pair"] != pair:
            continue
        key = case_key(row)
        sample = int(row["sample"])
        implementation = row["implementation"]
        if implementation in paired[key][sample]:
            raise ValueError(f"duplicate row for {key}, sample {sample}, {implementation}")
        paired[key][sample][implementation] = float(row["million_records_per_second"])
    return paired


def ratio_for(sample: dict[str, float], numerator: str, denominator: str) -> float:
    if set(sample) != {numerator, denominator}:
        raise ValueError(f"unpaired implementations: expected {numerator!r} and {denominator!r}, got {sorted(sample)}")
    return sample[numerator] / sample[denominator]


def load_logger_cases(rows: list[dict[str, str]]) -> tuple[list[CaseResult], dict[str, list[float]]]:
    forward = pair_samples(rows, "logger migration forward")
    reverse = pair_samples(rows, "logger migration reverse")
    if set(forward) != set(reverse):
        raise ValueError("forward and reverse Logger migration cases differ")
    cases = []
    for key in sorted(forward):
        if set(forward[key]) != set(reverse[key]):
            raise ValueError(f"forward and reverse Logger sample sets differ for {key}")
        ratios = []
        for sample_index in sorted(forward[key]):
            forward_ratio = ratio_for(
                forward[key][sample_index], "Sora logger forward", "legacy logger forward"
            )
            reverse_ratio = ratio_for(
                reverse[key][sample_index], "Sora logger reverse", "legacy logger reverse"
            )
            ratios.append(math.sqrt(forward_ratio * reverse_ratio))
        cases.append(CaseResult(key, ratios))

    controls: dict[str, list[float]] = {}
    for pair, numerator, denominator in (
        ("logger legacy self", "legacy logger self B", "legacy logger self A"),
        ("logger Sora self", "Sora logger self B", "Sora logger self A"),
    ):
        control_rows = pair_samples(rows, pair)
        if set(control_rows) != set(forward):
            raise ValueError(f"Logger self-control cases differ for {pair}")
        controls[pair] = [
            ratio_for(sample, numerator, denominator)
            for key in sorted(control_rows)
            for _, sample in sorted(control_rows[key].items())
        ]
    if not cases:
        raise ValueError("input contains no Logger migration pairs")
    return cases, controls


def load_migration_cases(rows: list[dict[str, str]]) -> tuple[list[CaseResult], dict[str, list[float]]]:
    forward = pair_samples(rows, "migration forward")
    reverse = pair_samples(rows, "migration reverse")
    if set(forward) != set(reverse):
        raise ValueError("forward and reverse migration cases differ")

    cases = []
    for key in sorted(forward):
        if set(forward[key]) != set(reverse[key]):
            raise ValueError(f"forward and reverse sample sets differ for {key}")
        ratios = []
        for sample_index in sorted(forward[key]):
            forward_ratio = ratio_for(forward[key][sample_index], "Sora forward", "Mashiro forward")
            reverse_ratio = ratio_for(reverse[key][sample_index], "Sora reverse", "Mashiro reverse")
            ratios.append(math.sqrt(forward_ratio * reverse_ratio))
        cases.append(CaseResult(key, ratios))

    controls: dict[str, list[float]] = {}
    for pair, numerator, denominator in (
        ("migration Mashiro self", "Mashiro self B", "Mashiro self A"),
        ("migration Sora self", "Sora self B", "Sora self A"),
    ):
        control_rows = pair_samples(rows, pair)
        if set(control_rows) != set(forward):
            raise ValueError(f"self-control cases differ for {pair}")
        controls[pair] = [
            ratio_for(sample, numerator, denominator)
            for key in sorted(control_rows)
            for _, sample in sorted(control_rows[key].items())
        ]
    if not cases:
        raise ValueError("input contains no RecordRingBuffer migration pairs")
    return cases, controls


def bootstrap_case(case: CaseResult, count: int, rng: random.Random) -> tuple[float, float]:
    estimates = [statistics.median(rng.choices(case.ratios, k=len(case.ratios))) for _ in range(count)]
    return interval(estimates)


def bootstrap_aggregate(cases: list[CaseResult], count: int, rng: random.Random) -> tuple[float, float]:
    estimates = []
    for _ in range(count):
        sampled_cases = rng.choices(cases, k=len(cases))
        sampled_ratios = [statistics.median(rng.choices(case.ratios, k=len(case.ratios))) for case in sampled_cases]
        estimates.append(geometric_mean(sampled_ratios))
    return interval(estimates)


def verdict(low: float, high: float, margin: float) -> str:
    if low >= 1.0 - margin and high <= 1.0 + margin:
        return f"equivalent within +/-{margin:.0%}"
    if low >= 1.0 - margin:
        return f"no >{margin:.0%} regression"
    return f"cannot exclude >{margin:.0%} regression"


def render_report(
    mode: str,
    cases: list[CaseResult],
    controls: dict[str, list[float]],
    bootstrap_count: int,
    seed: int,
    margin: float,
) -> str:
    rng = random.Random(seed)
    title = "RecordRingBuffer Mashiro-to-Sora migration" if mode == "migration" else "StructuredLogger ring migration"
    ratio_label = "Sora / Mashiro" if mode == "migration" else "RecordRingBuffer / ThreadRing"
    estimate = geometric_mean([case.estimate() for case in cases])
    low, high = bootstrap_aggregate(cases, bootstrap_count, rng)
    lines = [
        f"# {title} audit",
        "",
        f"Ratio: {ratio_label}. Cases: {len(cases)}. Paired hierarchical bootstrap: {bootstrap_count} replicates.",
        f"Non-regression margin: {margin:.0%}.",
        "",
        "## Aggregate",
        "",
        "| Ratio | 95% CI | Verdict |",
        "|---:|---:|---|",
        f"| {estimate:.4f}x | [{low:.4f}, {high:.4f}] | {verdict(low, high, margin)} |",
    ]
    if controls:
        lines.extend(["", "## Self-controls", ""])
        for name, ratios in controls.items():
            lines.append(f"- {name}: {geometric_mean(ratios):.4f}x geometric mean.")
    lines.extend([
        "",
        "## Per case",
        "",
        "| Topology | Payload | Endpoints | Ratio | 95% CI | Verdict |",
        "|---|---:|---:|---:|---:|---|",
    ])
    for case in cases:
        case_low, case_high = bootstrap_case(case, bootstrap_count, rng)
        endpoints = f"{case.key.producers}P{case.key.consumers}C"
        lines.append(
            f"| {case.key.topology} | {case.key.payload_bytes} B | {endpoints} | {case.estimate():.4f}x | "
            f"[{case_low:.4f}, {case_high:.4f}] | {verdict(case_low, case_high, margin)} |"
        )
    return "\n".join(lines) + "\n"


def main() -> None:
    args = parse_args()
    if args.bootstrap <= 0 or not 0.0 < args.margin < 1.0:
        raise ValueError("bootstrap must be positive and margin must be between zero and one")
    rows = load_rows(args.input)
    if args.mode == "migration":
        cases, controls = load_migration_cases(rows)
    else:
        cases, controls = load_logger_cases(rows)
    report = render_report(args.mode, cases, controls, args.bootstrap, args.seed, args.margin)
    print(report, end="")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report, encoding="utf-8")


if __name__ == "__main__":
    main()

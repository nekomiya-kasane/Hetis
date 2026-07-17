"""
Plot the semantic-layered AsyncQueue benchmark.

The summary CSV contains one row per measured sample and the companion latency CSV contains every latency
observation.  The script deliberately uses the median and P10-P90 interval instead of a single best run.
"""

from __future__ import annotations

import argparse
import csv
import math
import random
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


IMPLEMENTATION_ORDER = [
    "Mashiro AsyncQueue",
    "Mashiro sync_wait",
    "Mashiro atomic terminal",
    "Mashiro spin terminal",
    "Mashiro CV terminal",
    "oneTBB bounded",
    "moodycamel blocking",
    "condition variable",
    "Mashiro storage try",
    "Mashiro AsyncQueue try",
    "Mashiro sender start",
]


def read_summary(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def read_raw(path: Path, categories: set[str]) -> dict[tuple[str, str, str], list[int]]:
    values: dict[tuple[str, str, str], list[int]] = defaultdict(list)
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            if row["category"] in categories:
                key = (row["category"], row["scenario"], row["implementation"])
                values[key].append(int(row["nanoseconds"]))
    return values


def numeric(rows: list[dict[str, str]], field: str) -> np.ndarray:
    return np.asarray([float(row[field]) for row in rows], dtype=float)


def grouped(rows: list[dict[str, str]], keys: tuple[str, ...]) -> dict[tuple[str, ...], list[dict[str, str]]]:
    result: dict[tuple[str, ...], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        result[tuple(row[key] for key in keys)].append(row)
    return result


def order_implementations(names: set[str]) -> list[str]:
    return [name for name in IMPLEMENTATION_ORDER if name in names] + sorted(names.difference(IMPLEMENTATION_ORDER))


def save_figure(figure: plt.Figure, output: Path, stem: str) -> None:
    figure.tight_layout()
    figure.savefig(output / f"{stem}.png", dpi=180)
    figure.savefig(output / f"{stem}.svg")
    plt.close(figure)


def summarize(rows: list[dict[str, str]], field: str) -> dict[str, tuple[float, float, float]]:
    result = {}
    for implementation, sample_rows in grouped(rows, ("implementation",)).items():
        values = numeric(sample_rows, field)
        result[implementation[0]] = (
            float(np.quantile(values, 0.10)),
            float(np.median(values)),
            float(np.quantile(values, 0.90)),
        )
    return result


def plot_sizes(rows: list[dict[str, str]], output: Path) -> None:
    rows = [row for row in rows if row["category"] == "size"]
    if not rows:
        return
    rows_by_name = {row["scenario"] + ":" + row["implementation"]: row for row in rows}
    labels = list(rows_by_name)
    values = [float(rows_by_name[label]["bytes"]) for label in labels]
    figure, axis = plt.subplots(figsize=(12, 6))
    bars = axis.bar(np.arange(len(labels)), values, color="#0072B2")
    axis.set_xticks(np.arange(len(labels)), labels, rotation=55, ha="right")
    axis.set_ylabel("Bytes")
    axis.set_title("Queue and operation-state storage footprint")
    axis.bar_label(bars, fmt="%.0f", padding=3)
    save_figure(figure, output, "async-queue-storage-footprint")


def plot_ready(rows: list[dict[str, str]], output: Path) -> None:
    rows = [row for row in rows if row["category"] == "ready"]
    if not rows:
        return
    summary = summarize(rows, "operations_per_second")
    names = order_implementations(set(summary))
    medians = [summary[name][1] / 1e6 for name in names]
    lower = [medians[index] - summary[name][0] / 1e6 for index, name in enumerate(names)]
    upper = [summary[name][2] / 1e6 - medians[index] for index, name in enumerate(names)]
    figure, axis = plt.subplots(figsize=(11, 6))
    axis.bar(np.arange(len(names)), medians, yerr=[lower, upper], capsize=4, color="#009E73")
    axis.set_xticks(np.arange(len(names)), names, rotation=35, ha="right")
    axis.set_ylabel("Million operations/s")
    axis.set_title("AsyncQueue ready-path layer cost, median with P10-P90")
    save_figure(figure, output, "async-queue-ready-path")


def plot_throughput(rows: list[dict[str, str]], output: Path) -> None:
    rows = [row for row in rows if row["category"] == "throughput"]
    cases = grouped(rows, ("scenario", "payload_bytes"))
    if not cases:
        return
    figure, axes = plt.subplots(2, math.ceil(len(cases) / 2), figsize=(16, 9), squeeze=False)
    for axis, (case, case_rows) in zip(axes.flat, sorted(cases.items())):
        summary = summarize(case_rows, "operations_per_second")
        names = order_implementations(set(summary))
        medians = np.asarray([summary[name][1] / 1e6 for name in names])
        lower = medians - np.asarray([summary[name][0] / 1e6 for name in names])
        upper = np.asarray([summary[name][2] / 1e6 for name in names]) - medians
        axis.bar(np.arange(len(names)), medians, yerr=[lower, upper], capsize=3, color="#56B4E9")
        axis.set_xticks(np.arange(len(names)), names, rotation=45, ha="right")
        axis.set_ylabel("Million transfers/s")
        axis.set_title(f"{case[0]}, payload {case[1]} B")
    for axis in axes.flat[len(cases):]:
        axis.set_visible(False)
    save_figure(figure, output, "async-queue-throughput-overview")


def plot_throughput_relative(rows: list[dict[str, str]], output: Path) -> None:
    rows = [row for row in rows if row["category"] == "throughput"]
    cases = grouped(rows, ("scenario", "payload_bytes"))
    values = []
    labels = []
    for case, case_rows in sorted(cases.items()):
        summary = summarize(case_rows, "operations_per_second")
        mashiro = "Mashiro sync_wait" if "Mashiro sync_wait" in summary else "Mashiro AsyncQueue"
        external = [value[1] for name, value in summary.items() if not name.startswith("Mashiro ")]
        if not external or mashiro not in summary:
            continue
        values.append(summary[mashiro][1] / max(external))
        labels.append(f"{case[0]} / {case[1]} B")
    if not values:
        return
    figure, axis = plt.subplots(figsize=(10, 6))
    colors = ["#D55E00" if value < 1 else "#009E73" for value in values]
    axis.bar(np.arange(len(values)), values, color=colors)
    axis.axhline(1.0, color="black", linewidth=1)
    axis.set_xticks(np.arange(len(values)), labels, rotation=40, ha="right")
    axis.set_ylabel("Mashiro sync_wait / best external median")
    axis.set_title("End-to-end throughput relative to the best comparable external implementation")
    save_figure(figure, output, "async-queue-throughput-relative")


def plot_latency_summary(rows: list[dict[str, str]], output: Path, category: str, stem: str, title: str) -> None:
    rows = [row for row in rows if row["category"] == category]
    if not rows:
        return
    cases = grouped(rows, ("scenario", "implementation"))
    names = order_implementations({case[1] for case in cases})
    scenarios = sorted({case[0] for case in cases})
    figure, axes = plt.subplots(1, len(scenarios), figsize=(8 * len(scenarios), 6), squeeze=False)
    for axis, scenario in zip(axes.flat, scenarios):
        medians = []
        p90s = []
        p99s = []
        labels = []
        for name in names:
            sample_rows = cases.get((scenario, name), [])
            if not sample_rows:
                continue
            labels.append(name)
            medians.append(float(np.median(numeric(sample_rows, "p50_ns"))) / 1e3)
            p90s.append(float(np.median(numeric(sample_rows, "p90_ns"))) / 1e3)
            p99s.append(float(np.median(numeric(sample_rows, "p99_ns"))) / 1e3)
        x = np.arange(len(labels))
        axis.bar(x - 0.25, medians, 0.25, label="P50")
        axis.bar(x, p90s, 0.25, label="P90")
        axis.bar(x + 0.25, p99s, 0.25, label="P99")
        axis.set_xticks(x, labels, rotation=40, ha="right")
        axis.set_ylabel("Microseconds")
        axis.set_title(scenario)
        axis.legend()
    figure.suptitle(title)
    save_figure(figure, output, stem)


def plot_fanout(rows: list[dict[str, str]], output: Path, category: str, stem: str, title: str) -> None:
    rows = [row for row in rows if row["category"] == category]
    if not rows:
        return
    grouped_rows = grouped(rows, ("implementation", "scenario"))
    figure, axis = plt.subplots(figsize=(10, 6))
    for name in order_implementations({key[0] for key in grouped_rows}):
        points = []
        for (implementation, scenario), sample_rows in grouped_rows.items():
            if implementation != name:
                continue
            waiters = int(scenario.split("-")[-1])
            median = float(np.median(numeric(sample_rows, "p50_ns"))) / 1e3
            points.append((waiters, median))
        if points:
            points.sort()
            axis.plot([point[0] for point in points], [point[1] for point in points], marker="o", label=name)
    axis.set_xlabel("Parked waiters")
    axis.set_ylabel("Abort-to-all-return P50 (microseconds)")
    axis.set_title(title)
    axis.legend()
    save_figure(figure, output, stem)


def plot_raw_ecdf(
    raw: dict[tuple[str, str, str], list[int]], output: Path, category: str, stem: str, title: str
) -> None:
    selected = {(scenario, name): values for (kind, scenario, name), values in raw.items() if kind == category}
    if not selected:
        return
    figure, axis = plt.subplots(figsize=(10, 6))
    for (scenario, name), values in sorted(selected.items()):
        values = sorted(values)
        if len(values) > 100_000:
            random.seed(0)
            values = sorted(random.sample(values, 100_000))
        probability = np.linspace(0.0, 1.0, len(values), endpoint=False)
        axis.plot(values, probability, label=f"{name} ({scenario})")
    axis.set_xscale("log")
    axis.set_xlabel("Latency (ns)")
    axis.set_ylabel("Empirical CDF")
    axis.set_title(title)
    axis.legend()
    save_figure(figure, output, stem)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("summary", type=Path, nargs="+")
    parser.add_argument("--raw", type=Path)
    parser.add_argument("--output", type=Path, default=Path("."))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rows = [row for summary in args.summary for row in read_summary(summary)]
    raw_path = args.raw or args.summary[0].with_name(args.summary[0].stem + "-latency.csv")
    raw = read_raw(raw_path, {"consumer-resume", "producer-resume", "callback-delivery", "cancellation"})
    plot_sizes(rows, args.output)
    plot_ready(rows, args.output)
    plot_throughput(rows, args.output)
    plot_throughput_relative(rows, args.output)
    plot_latency_summary(
        rows, args.output, "consumer-resume", "async-queue-consumer-resume", "Consumer park/resume latency"
    )
    plot_latency_summary(
        rows,
        args.output,
        "producer-resume",
        "async-queue-producer-resume",
        "Producer backpressure resume latency",
    )
    plot_latency_summary(
        rows,
        args.output,
        "callback-delivery",
        "async-queue-callback-delivery",
        "P2300 callback delivery latency",
    )
    plot_latency_summary(
        rows, args.output, "cancellation", "async-queue-cancellation", "P2300 cancellation delivery latency"
    )
    plot_fanout(
        rows,
        args.output,
        "abort-callback-fanout",
        "async-queue-abort-callback-fanout",
        "Abort-to-receiver callback fan-out scaling",
    )
    plot_fanout(
        rows,
        args.output,
        "abort-thread-fanout",
        "async-queue-abort-thread-fanout",
        "Abort-to-thread-return fan-out scaling",
    )
    plot_raw_ecdf(
        raw,
        args.output,
        "consumer-resume",
        "async-queue-consumer-resume-ecdf",
        "Raw consumer resume latency ECDF",
    )
    plot_raw_ecdf(
        raw,
        args.output,
        "producer-resume",
        "async-queue-producer-resume-ecdf",
        "Raw producer resume latency ECDF",
    )
    print(f"generated charts in {args.output}")


if __name__ == "__main__":
    main()

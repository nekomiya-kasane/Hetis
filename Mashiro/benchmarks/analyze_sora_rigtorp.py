"""Analyze direct Sora RingBuffer versus Rigtorp queue measurements."""

from __future__ import annotations

import argparse
import csv
import math
import random
from collections import defaultdict
from pathlib import Path

import matplotlib.pyplot as plt


def median(values: list[float]) -> float:
    values = sorted(values)
    return values[len(values) // 2]


def bootstrap_ci(values: list[float], seed: int) -> tuple[float, float]:
    rng = random.Random(seed)
    estimates = []
    for _ in range(10_000):
        sample = [values[rng.randrange(len(values))] for _ in values]
        estimates.append(math.prod(sample) ** (1.0 / len(sample)))
    estimates.sort()
    return estimates[250], estimates[9_749]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--plot", type=Path, required=True)
    args = parser.parse_args()

    rows = list(csv.DictReader(args.input.open(encoding="utf-8")))
    grouped: dict[tuple[str, str, str, str, str], dict[str, list[float]]] = defaultdict(lambda: defaultdict(list))
    for row in rows:
        key = (row["scenario"], row["payload_bytes"], row["capacity"], row["producers"], row["consumers"])
        grouped[key][row["queue"]].append(float(row["million_transfers_per_second"]))

    summaries = []
    for index, (key, implementations) in enumerate(sorted(grouped.items())):
        sora = implementations["Sora RingBuffer"]
        rigtorp = implementations["Rigtorp MPMC"] if key[0] != "SPSC" else implementations["Rigtorp SPSC"]
        ratios = [sora_value / rigtorp_value for sora_value, rigtorp_value in zip(sora, rigtorp)]
        ratio = math.prod(ratios) ** (1.0 / len(ratios))
        low, high = bootstrap_ci(ratios, index + 1)
        summaries.append((*key, ratio, low, high, median(sora), median(rigtorp)))

    args.summary.parent.mkdir(parents=True, exist_ok=True)
    with args.summary.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["scenario", "payload_bytes", "capacity", "producers", "consumers", "sora_rigtorp", "ci_low", "ci_high", "sora_mrec", "rigtorp_mrec"])
        writer.writerows(summaries)

    topology_order = ["SPSC", "MPSC", "SPMC", "MPMC"]
    fig, axes = plt.subplots(2, 2, figsize=(12, 8), sharey=True)
    for axis, topology in zip(axes.flat, topology_order):
        selected = [row for row in summaries if row[0] == topology]
        labels = [f"{row[1]}B/{row[2]}\n{row[3]}P{row[4]}C" for row in selected]
        x = list(range(len(selected)))
        ratios = [row[5] for row in selected]
        lower = [row[5] - row[6] for row in selected]
        upper = [row[7] - row[5] for row in selected]
        axis.errorbar(x, ratios, yerr=[lower, upper], fmt="o", color="#0072B2", ecolor="#0072B2", capsize=3)
        axis.axhline(1.0, color="#444444", linewidth=1.0)
        axis.axhspan(0.95, 1.05, color="#009E73", alpha=0.10)
        axis.set_title(topology)
        axis.set_xticks(x, labels, rotation=45, ha="right")
        axis.set_ylabel("Sora / Rigtorp throughput")
        axis.grid(axis="y", alpha=0.25)
    fig.suptitle("Direct Sora RingBuffer versus Rigtorp queue")
    fig.tight_layout()
    args.plot.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.plot, dpi=160)


if __name__ == "__main__":
    main()

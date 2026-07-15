from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


ROOT = Path(__file__).resolve().parent
RAW = ROOT / "results_raw.csv"
SUMMARY = ROOT / "results_summary.csv"
OVERVIEW = ROOT / "hive_bench_overview.png"
SPEEDUP = ROOT / "hive_bench_speedup_vs_vector.png"

OPERATIONS = [
    "build_insert",
    "iterate_sum",
    "erase_if_25pct",
    "churn_erase_insert_25pct",
]

TITLES = {
    "build_insert": "Build + insert",
    "iterate_sum": "Sequential iteration",
    "erase_if_25pct": "Erase 25% by predicate",
    "churn_erase_insert_25pct": "Erase 25% + refill",
}

COLORS = {
    "std::vector": "#2563eb",
    "std::deque": "#0891b2",
    "std::list": "#84cc16",
    "std::hive": "#dc2626",
    "std::pmr::vector": "#7c3aed",
    "std::pmr::deque": "#0f766e",
    "std::pmr::list": "#65a30d",
    "std::pmr::hive": "#f97316",
}


def mad(values: pd.Series) -> float:
    median = values.median()
    return float((values - median).abs().median())


def load_summary() -> pd.DataFrame:
    try:
        raw = pd.read_csv(RAW)
    except UnicodeDecodeError:
        raw = pd.read_csv(RAW, encoding="utf-16")
    summary = (
        raw.groupby(["container", "operation", "n"], as_index=False)
        .agg(
            samples=("ns_per_op", "count"),
            median_ns_per_op=("ns_per_op", "median"),
            mad_ns_per_op=("ns_per_op", mad),
            min_ns_per_op=("ns_per_op", "min"),
            max_ns_per_op=("ns_per_op", "max"),
            mean_ns_per_op=("ns_per_op", "mean"),
        )
        .sort_values(["operation", "container", "n"])
    )
    summary.to_csv(SUMMARY, index=False)
    return summary


def draw_overview(summary: pd.DataFrame) -> None:
    fig, axes = plt.subplots(2, 2, figsize=(15, 10), constrained_layout=True)

    for ax, operation in zip(axes.flat, OPERATIONS):
        subset = summary[summary["operation"] == operation]
        for container, group in subset.groupby("container", sort=False):
            group = group.sort_values("n")
            x = group["n"].to_numpy(dtype=float)
            y = group["median_ns_per_op"].to_numpy(dtype=float)
            err = group["mad_ns_per_op"].to_numpy(dtype=float)
            color = COLORS.get(container)
            ax.plot(x, y, marker="o", markersize=3.2, linewidth=1.8, label=container, color=color)
            ax.fill_between(x, np.maximum(y - err, 1e-12), y + err, color=color, alpha=0.10, linewidth=0)

        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_title(TITLES[operation])
        ax.set_xlabel("N elements")
        ax.set_ylabel("median ns/op")
        ax.grid(True, which="both", alpha=0.22)

    handles, labels = axes.flat[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside lower center", ncols=4)
    fig.suptitle("std::hive / std::pmr::hive benchmark against standard sequence containers", fontsize=15)
    fig.savefig(OVERVIEW, dpi=220)


def draw_speedup(summary: pd.DataFrame) -> None:
    fig, axes = plt.subplots(2, 2, figsize=(15, 10), constrained_layout=True)
    candidates = [
        "std::deque",
        "std::list",
        "std::hive",
        "std::pmr::vector",
        "std::pmr::deque",
        "std::pmr::list",
        "std::pmr::hive",
    ]

    for ax, operation in zip(axes.flat, OPERATIONS):
        subset = summary[summary["operation"] == operation]
        baseline = subset[subset["container"] == "std::vector"][["n", "median_ns_per_op"]].rename(
            columns={"median_ns_per_op": "vector_ns_per_op"}
        )

        for container in candidates:
            group = subset[subset["container"] == container][["n", "median_ns_per_op"]]
            merged = group.merge(baseline, on="n", how="inner").sort_values("n")
            if merged.empty:
                continue
            speedup = merged["vector_ns_per_op"] / merged["median_ns_per_op"]
            ax.plot(
                merged["n"],
                speedup,
                marker="o",
                markersize=3.2,
                linewidth=1.8,
                label=container,
                color=COLORS.get(container),
            )

        ax.axhline(1.0, color="#111827", linewidth=1.0, linestyle="--", alpha=0.65)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log", base=2)
        ax.set_title(TITLES[operation])
        ax.set_xlabel("N elements")
        ax.set_ylabel("speedup vs std::vector")
        ax.grid(True, which="both", alpha=0.22)

    handles, labels = axes.flat[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="outside lower center", ncols=4)
    fig.suptitle("Speedup relative to std::vector; values above 1 are faster", fontsize=15)
    fig.savefig(SPEEDUP, dpi=220)


def main() -> None:
    summary = load_summary()
    draw_overview(summary)
    draw_speedup(summary)
    print(SUMMARY)
    print(OVERVIEW)
    print(SPEEDUP)


if __name__ == "__main__":
    main()

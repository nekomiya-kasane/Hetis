"""Render Mashiro queue benchmark CSV files into comparison and optimization charts."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


MASHIRO_QUEUES = {"Mashiro SPSC", "Mashiro SPSC channel", "Mashiro MPSC", "Mashiro MPMC"}
COLORS = {
    "Mashiro SPSC": "#0072B2",
    "Mashiro SPSC channel": "#56B4E9",
    "Mashiro MPSC": "#009E73",
    "Mashiro MPMC": "#D55E00",
    "Rigtorp SPSC": "#CC79A7",
    "moodycamel": "#E69F00",
    "oneTBB bounded": "#7A5195",
    "mutex deque": "#777777",
    "Mashiro MPSC optional": "#75A99B",
    "Mashiro MPMC optional": "#E69F73",
    "shared-epoch SPSC channel": "#999999",
}


def load(paths: list[Path]) -> pd.DataFrame:
    frames = [pd.read_csv(path) for path in paths]
    data = pd.concat(frames, ignore_index=True)
    keys = ["label", "scenario", "queue", "payload_bytes", "capacity", "producers", "consumers"]
    summary = data.groupby(keys, as_index=False)["million_transfers_per_second"].agg(
        median="median", q10=lambda values: values.quantile(0.10), q90=lambda values: values.quantile(0.90)
    )
    return data, summary


def style_axes(axis: plt.Axes) -> None:
    axis.grid(axis="y", color="#D9D9D9", linewidth=0.7, alpha=0.8)
    axis.spines[["top", "right"]].set_visible(False)
    axis.set_axisbelow(True)


def latest_full_label(frame: pd.DataFrame) -> str:
    labels = frame.loc[frame["queue"] == "Rigtorp SPSC", "label"].drop_duplicates()
    if labels.empty:
        raise ValueError("no complete benchmark label contains the Rigtorp SPSC control")
    return labels.iloc[-1]


def overview(summary: pd.DataFrame, output: Path) -> None:
    latest = summary[summary["label"] == latest_full_label(summary)]
    capacity = latest["capacity"].max()
    payload = latest["payload_bytes"].min()
    selected = latest[(latest["capacity"] == capacity) & (latest["payload_bytes"] == payload)]
    topologies = [("SPSC", 1, 1), ("MPSC", 4, 1), ("MPMC", 4, 4)]
    fig, axes = plt.subplots(1, 3, figsize=(17, 5.5), constrained_layout=True)
    for axis, (scenario, producers, consumers) in zip(axes, topologies, strict=True):
        frame = selected[
            (selected["scenario"] == scenario)
            & (selected["producers"] == producers)
            & (selected["consumers"] == consumers)
        ].sort_values("median", ascending=False)
        axis.bar(
            frame["queue"], frame["median"],
            yerr=np.vstack([frame["median"] - frame["q10"], frame["q90"] - frame["median"]]),
            color=[COLORS.get(name, "#555555") for name in frame["queue"]], capsize=3,
        )
        axis.set_title(f"{scenario} {producers}P/{consumers}C")
        axis.set_ylabel("Million transfers / second")
        axis.tick_params(axis="x", rotation=35, labelsize=9)
        style_axes(axis)
    fig.suptitle(f"Queue throughput, payload={payload} B, capacity={capacity:,}\nmedian with P10-P90 interval")
    fig.savefig(output / "queue-throughput-overview.png", dpi=180)
    fig.savefig(output / "queue-throughput-overview.svg")
    plt.close(fig)


def scaling(summary: pd.DataFrame, output: Path) -> None:
    latest = summary[summary["label"] == latest_full_label(summary)]
    selected = latest[
        (latest["capacity"] == latest["capacity"].max()) & (latest["payload_bytes"] == latest["payload_bytes"].min())
    ]
    fig, axes = plt.subplots(1, 2, figsize=(13, 5), constrained_layout=True)
    for axis, scenario in zip(axes, ["MPSC", "MPMC"], strict=True):
        frame = selected[selected["scenario"] == scenario]
        for queue, group in frame.groupby("queue"):
            group = group.sort_values("producers")
            axis.plot(group["producers"], group["median"], marker="o", label=queue, color=COLORS.get(queue))
            axis.fill_between(group["producers"], group["q10"], group["q90"], alpha=0.13,
                              color=COLORS.get(queue))
        axis.set_title(f"{scenario} scaling")
        axis.set_xlabel("Producer threads")
        axis.set_ylabel("Million transfers / second")
        axis.set_xticks(sorted(frame["producers"].unique()))
        style_axes(axis)
    axes[-1].legend(frameon=False, fontsize=9)
    fig.savefig(output / "queue-scaling.png", dpi=180)
    fig.savefig(output / "queue-scaling.svg")
    plt.close(fig)


def optimization(summary: pd.DataFrame, output: Path) -> None:
    labels = list(summary.loc[summary["queue"] == "Rigtorp SPSC", "label"].drop_duplicates())
    if len(labels) < 2:
        return
    mashiro = summary[summary["queue"].isin(MASHIRO_QUEUES)].copy()
    mashiro["case"] = (
        mashiro["scenario"] + " " + mashiro["producers"].astype(str) + "P/" + mashiro["consumers"].astype(str)
        + "C " + mashiro["payload_bytes"].astype(str) + "B cap=" + mashiro["capacity"].astype(str)
        + "\n" + mashiro["queue"]
    )
    pivot = mashiro.pivot_table(index="case", columns="label", values="median")
    baseline, optimized = labels[0], labels[-1]
    pivot = pivot.dropna(subset=[baseline, optimized])
    pivot["speedup"] = pivot[optimized] / pivot[baseline]
    pivot = pivot.sort_values("speedup")

    fig, axis = plt.subplots(figsize=(12, max(5, 0.34 * len(pivot))), constrained_layout=True)
    colors = np.where(pivot["speedup"] >= 1.0, "#009E73", "#D55E00")
    axis.barh(pivot.index, pivot["speedup"], color=colors)
    axis.axvline(1.0, color="#333333", linewidth=1)
    axis.set_xlabel(f"Speedup: {optimized} / {baseline}")
    axis.set_title("Mashiro queue optimization effect (raw between-run ratio)")
    axis.grid(axis="x", color="#D9D9D9", linewidth=0.7)
    axis.spines[["top", "right", "left"]].set_visible(False)
    fig.savefig(output / "queue-optimization-speedup.png", dpi=180)
    fig.savefig(output / "queue-optimization-speedup.svg")
    plt.close(fig)


def variability(data: pd.DataFrame, output: Path) -> None:
    latest = data[data["label"] == latest_full_label(data)]
    selected = latest[
        (latest["capacity"] == latest["capacity"].max())
        & (latest["payload_bytes"] == latest["payload_bytes"].min())
        & (((latest["scenario"] == "MPSC") & (latest["producers"] == 4))
           | ((latest["scenario"] == "MPMC") & (latest["producers"] == 4)))
    ].copy()
    selected["case"] = selected["scenario"] + " | " + selected["queue"]
    order = selected.groupby("case")["million_transfers_per_second"].median().sort_values().index
    values = [selected[selected["case"] == case]["million_transfers_per_second"] for case in order]
    fig, axis = plt.subplots(figsize=(12, max(5, 0.45 * len(order))), constrained_layout=True)
    axis.boxplot(values, tick_labels=order, vert=False, showfliers=True, patch_artist=True,
                 boxprops={"facecolor": "#56B4E9", "alpha": 0.7})
    axis.set_xlabel("Million transfers / second")
    axis.set_title("Sample variability under contention")
    axis.grid(axis="x", color="#D9D9D9", linewidth=0.7)
    axis.spines[["top", "right", "left"]].set_visible(False)
    fig.savefig(output / "queue-sample-variability.png", dpi=180)
    fig.savefig(output / "queue-sample-variability.svg")
    plt.close(fig)


def paired_speedup(summary: pd.DataFrame, output: Path) -> None:
    pairs = [
        ("SPSC channel", "Mashiro SPSC channel", "shared-epoch SPSC channel"),
        ("MPSC storage", "Mashiro MPSC", "Mashiro MPSC optional"),
        ("MPMC storage", "Mashiro MPMC", "Mashiro MPMC optional"),
    ]
    keys = ["label", "scenario", "payload_bytes", "capacity", "producers", "consumers"]
    rows = []
    for title, direct, reference in pairs:
        selected = summary[summary["queue"].isin([direct, reference])]
        pivot = selected.pivot_table(index=keys, columns="queue", values="median").dropna(subset=[direct, reference])
        if pivot.empty:
            continue
        frame = pivot.reset_index()
        frame["group"] = title
        frame["speedup"] = frame[direct] / frame[reference]
        frame["case"] = (
            frame["scenario"] + " | " + frame["payload_bytes"].astype(str) + " B | cap "
            + frame["capacity"].map(lambda value: f"{value:,}") + " | " + frame["producers"].astype(str)
            + "P/" + frame["consumers"].astype(str) + "C"
        )
        rows.append(frame)
    if not rows:
        return

    paired = pd.concat(rows, ignore_index=True)
    groups = list(paired["group"].drop_duplicates())
    heights = [max(2.6, 0.32 * len(paired[paired["group"] == group])) for group in groups]
    fig, axes = plt.subplots(len(groups), 1, figsize=(13, sum(heights)), constrained_layout=True)
    axes = np.atleast_1d(axes)
    for axis, group in zip(axes, groups, strict=True):
        frame = paired[paired["group"] == group].sort_values("speedup")
        colors = np.where(frame["speedup"] >= 1.0, "#009E73", "#D55E00")
        axis.barh(frame["case"], frame["speedup"], color=colors)
        axis.axvline(1.0, color="#333333", linewidth=1)
        axis.set_title(group)
        axis.set_xlabel("Paired median throughput ratio: direct / reference")
        axis.grid(axis="x", color="#D9D9D9", linewidth=0.7)
        axis.spines[["top", "right", "left"]].set_visible(False)
    fig.suptitle("Same-process paired queue-path comparison")
    fig.savefig(output / "queue-paired-speedup.png", dpi=180)
    fig.savefig(output / "queue-paired-speedup.svg")
    plt.close(fig)


def storage_footprint(data: pd.DataFrame, output: Path) -> None:
    latest = data[data["label"] == latest_full_label(data)]
    selected = latest[latest["queue"].isin(MASHIRO_QUEUES)].drop_duplicates(
        subset=["queue", "payload_bytes", "capacity"]
    )
    payloads = sorted(selected["payload_bytes"].unique())
    fig, axes = plt.subplots(1, len(payloads), figsize=(13, 5), constrained_layout=True, sharey=True)
    axes = np.atleast_1d(axes)
    for axis, payload in zip(axes, payloads, strict=True):
        frame = selected[selected["payload_bytes"] == payload]
        for queue, group in frame.groupby("queue"):
            group = group.sort_values("capacity")
            axis.plot(group["capacity"], group["queue_bytes"] / 1024.0, marker="o", label=queue,
                      color=COLORS.get(queue))
        axis.set_title(f"Payload {payload} B")
        axis.set_xlabel("Capacity")
        axis.set_xscale("log", base=2)
        axis.set_yscale("log", base=2)
        axis.set_ylabel("Queue object size (KiB)")
        style_axes(axis)
    axes[-1].legend(frameon=False, fontsize=9)
    fig.suptitle("Mashiro fixed-capacity queue storage footprint")
    fig.savefig(output / "queue-storage-footprint.png", dpi=180)
    fig.savefig(output / "queue-storage-footprint.svg")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    data, summary = load(args.csv)
    summary.to_csv(args.output / "queue-summary.csv", index=False)
    overview(summary, args.output)
    scaling(summary, args.output)
    optimization(summary, args.output)
    variability(data, args.output)
    paired_speedup(summary, args.output)
    storage_footprint(data, args.output)


if __name__ == "__main__":
    main()

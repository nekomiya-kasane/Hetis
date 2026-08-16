#!/usr/bin/env python3
"""Validate paired record-ring measurements and generate publication-quality figures."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib import colors
from matplotlib.ticker import FuncFormatter
import numpy as np
import pandas as pd


MASHIRO = "Mashiro RecordRingBuffer"
METHOD_ORDER = [
    MASHIRO,
    "Aeron SPSC algorithm",
    "Aeron MPSC algorithm",
    "DPDK rte_ring classic fixed-record",
    "Rigtorp SPSCQueue fixed-record",
    "Rigtorp MPMCQueue fixed-record",
    "mutex packed RecordRingBuffer",
    "mutex allocating deque",
]
METHOD_COLORS = {
    MASHIRO: "#0072B2",
    "Aeron SPSC algorithm": "#009E73",
    "Aeron MPSC algorithm": "#009E73",
    "DPDK rte_ring classic fixed-record": "#E69F00",
    "Rigtorp SPSCQueue fixed-record": "#CC79A7",
    "Rigtorp MPMCQueue fixed-record": "#CC79A7",
    "mutex packed RecordRingBuffer": "#D55E00",
    "mutex allocating deque": "#6B6B6B",
}
METHOD_MARKERS = {
    MASHIRO: "o",
    "Aeron SPSC algorithm": "^",
    "Aeron MPSC algorithm": "^",
    "DPDK rte_ring classic fixed-record": "v",
    "Rigtorp SPSCQueue fixed-record": "P",
    "Rigtorp MPMCQueue fixed-record": "P",
    "mutex packed RecordRingBuffer": "s",
    "mutex allocating deque": "D",
}
METHOD_LINESTYLES = {
    MASHIRO: "-",
    "Aeron SPSC algorithm": "-",
    "Aeron MPSC algorithm": "-",
    "DPDK rte_ring classic fixed-record": "--",
    "Rigtorp SPSCQueue fixed-record": "--",
    "Rigtorp MPMCQueue fixed-record": "--",
    "mutex packed RecordRingBuffer": "-",
    "mutex allocating deque": ":",
}
TOPOLOGY_ORDER = ["SPSC", "MPSC", "SPMC", "MPMC"]
AGGREGATE_ORDER = ["Aeron", "DPDK classic", "Rigtorp fixed-slot", "mutex packed", "mutex allocating"]


def configure_style() -> None:
    plt.rcParams.update(
        {
            "figure.dpi": 130,
            "savefig.dpi": 240,
            "font.size": 10,
            "axes.titlesize": 12,
            "axes.labelsize": 10,
            "legend.fontsize": 9,
            "axes.grid": True,
            "grid.alpha": 0.2,
            "grid.linewidth": 0.7,
            "axes.axisbelow": True,
            "svg.fonttype": "none",
        }
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bootstrap", type=int, default=20_000)
    parser.add_argument("--seed", type=int, default=0x5245434F5244)
    return parser.parse_args()


def load_measurements(path: Path) -> pd.DataFrame:
    frame = pd.read_csv(path)
    required = {
        "pair",
        "topology",
        "implementation",
        "payload_bytes",
        "byte_capacity",
        "producers",
        "consumers",
        "sample",
        "order",
        "records",
        "seconds",
        "million_records_per_second",
        "payload_gib_per_second",
        "push_retries",
        "pop_retries",
        "storage_bytes",
        "thread_placement",
        "reference_commit",
        "capacity_semantics",
    }
    missing = required - set(frame.columns)
    if missing:
        raise ValueError(f"missing CSV columns: {sorted(missing)}")
    numeric = [
        "payload_bytes",
        "byte_capacity",
        "producers",
        "consumers",
        "sample",
        "records",
        "seconds",
        "million_records_per_second",
        "payload_gib_per_second",
        "push_retries",
        "pop_retries",
        "storage_bytes",
    ]
    if not np.isfinite(frame[numeric].to_numpy(dtype=float)).all():
        raise ValueError("benchmark CSV contains non-finite values")
    if (frame["seconds"] <= 0).any() or (frame["records"] <= 0).any():
        raise ValueError("every sample must have positive duration and record count")
    group_keys = ["pair", "topology", "payload_bytes", "byte_capacity", "producers", "consumers"]
    row_keys = [*group_keys, "implementation", "sample"]
    if frame.duplicated(row_keys).any():
        raise ValueError("benchmark CSV contains duplicate implementation samples")
    sample_counts = frame.groupby([*group_keys, "implementation"])["sample"].nunique()
    if sample_counts.nunique() != 1:
        raise ValueError("every implementation group must contain the same number of samples")
    expected_samples = list(range(int(sample_counts.iloc[0])))
    for key, group in frame.groupby([*group_keys, "implementation"], sort=False):
        if sorted(group["sample"].tolist()) != expected_samples:
            raise ValueError(f"implementation group has missing or non-contiguous samples: {key}")
        if group["records"].nunique() != 1:
            raise ValueError(f"calibrated record count changed within an implementation group: {key}")
    for key, group in frame.groupby([*group_keys, "sample"], sort=False):
        if len(group) != 2 or (group["implementation"] == MASHIRO).sum() != 1:
            raise ValueError(f"pair/sample must contain one Mashiro row and one baseline row: {key}")
        order_parts = group["order"].str.split(":", n=1, expand=True)
        if order_parts[0].nunique() != 1 or set(order_parts[1]) != {"first", "second"}:
            raise ValueError(f"pair/sample has an invalid AB/BA orientation: {key}")
    frame.attrs["source_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest().upper()
    return frame


def paired_ratios(frame: pd.DataFrame) -> pd.DataFrame:
    keys = [
        "pair",
        "topology",
        "payload_bytes",
        "byte_capacity",
        "producers",
        "consumers",
        "sample",
    ]
    rows: list[dict[str, object]] = []
    for key, group in frame.groupby(keys, sort=False):
        mashiro = group[group["implementation"] == MASHIRO]
        baseline = group[group["implementation"] != MASHIRO]
        if len(mashiro) != 1 or len(baseline) != 1:
            raise ValueError(f"pair/sample must contain one Mashiro and one baseline row: {key}")
        first = mashiro.iloc[0]
        second = baseline.iloc[0]
        rows.append(
            {
                **dict(zip(keys, key, strict=True)),
                "baseline": second["implementation"],
                "mashiro_run_position": first["order"].split(":", maxsplit=1)[1],
                "speedup": first["million_records_per_second"]
                / second["million_records_per_second"],
                "mashiro_mrps": first["million_records_per_second"],
                "baseline_mrps": second["million_records_per_second"],
            }
        )
    return pd.DataFrame(rows)


def bootstrap_interval(values: np.ndarray, iterations: int, rng: np.random.Generator) -> tuple[float, float]:
    if len(values) == 1:
        value = float(values[0])
        return value, value
    indices = rng.integers(0, len(values), size=(iterations, len(values)))
    medians = np.median(values[indices], axis=1)
    low, high = np.quantile(medians, [0.025, 0.975])
    return float(low), float(high)


def summarize_pairs(ratios: pd.DataFrame, iterations: int, seed: int) -> pd.DataFrame:
    keys = [
        "pair",
        "topology",
        "baseline",
        "payload_bytes",
        "byte_capacity",
        "producers",
        "consumers",
    ]
    rng = np.random.default_rng(seed)
    rows: list[dict[str, object]] = []
    for key, group in ratios.groupby(keys, sort=False):
        values = group["speedup"].to_numpy(dtype=float)
        low, high = bootstrap_interval(values, iterations, rng)
        rows.append(
            {
                **dict(zip(keys, key, strict=True)),
                "samples": len(values),
                "speedup_median": float(np.median(values)),
                "speedup_ci95_low": low,
                "speedup_ci95_high": high,
                "mashiro_median_mrps": float(np.median(group["mashiro_mrps"])),
                "baseline_median_mrps": float(np.median(group["baseline_mrps"])),
            }
        )
    return pd.DataFrame(rows)


def implementation_label(name: str) -> str:
    return {
        MASHIRO: "Mashiro",
        "Aeron SPSC algorithm": "Aeron",
        "Aeron MPSC algorithm": "Aeron",
        "DPDK rte_ring classic fixed-record": "DPDK classic",
        "Rigtorp SPSCQueue fixed-record": "Rigtorp fixed-slot",
        "Rigtorp MPMCQueue fixed-record": "Rigtorp fixed-slot",
        "mutex packed RecordRingBuffer": "mutex packed",
        "mutex allocating deque": "mutex allocating",
    }[name]


def geometric_mean(values: pd.Series) -> float:
    return float(np.exp(np.log(values.to_numpy(dtype=float)).mean()))


def endpoint_label(row: pd.Series) -> str:
    return f"{int(row['producers'])}P/{int(row['consumers'])}C"


def primary_endpoint(frame: pd.DataFrame, topology: str) -> tuple[int, int]:
    target = (1, 1) if topology == "SPSC" else ((4, 1) if topology == "MPSC" else (1, 4))
    if topology == "MPMC":
        target = (4, 4)
    available = frame.loc[frame["topology"] == topology, ["producers", "consumers"]]
    if not ((available["producers"] == target[0]) & (available["consumers"] == target[1])).any():
        raise ValueError(f"missing primary endpoint {target} for {topology}")
    return target


def save_figure(figure: plt.Figure, output: Path, stem: str) -> None:
    figure.savefig(output / f"{stem}.png", bbox_inches="tight")
    figure.savefig(output / f"{stem}.svg", bbox_inches="tight")
    plt.close(figure)


def payload_throughput(frame: pd.DataFrame, output: Path) -> None:
    figure, axes = plt.subplots(2, 4, figsize=(18, 8.8), sharex="col", constrained_layout=True)
    metrics = [
        ("million_records_per_second", "M records/s"),
        ("payload_gib_per_second", "Effective payload GiB/s"),
    ]
    for column, topology in enumerate(TOPOLOGY_ORDER):
        producers, consumers = primary_endpoint(frame, topology)
        selected = frame[
            (frame["topology"] == topology)
            & (frame["producers"] == producers)
            & (frame["consumers"] == consumers)
        ]
        for row, (metric, ylabel) in enumerate(metrics):
            axis = axes[row, column]
            for implementation in METHOD_ORDER:
                group = selected[selected["implementation"] == implementation]
                if group.empty:
                    continue
                summary = group.groupby("payload_bytes")[metric].agg(
                    median="median", q10=lambda values: values.quantile(0.1), q90=lambda values: values.quantile(0.9)
                )
                x = summary.index.to_numpy(dtype=float)
                axis.plot(
                    x,
                    summary["median"],
                    color=METHOD_COLORS[implementation],
                    marker=METHOD_MARKERS[implementation],
                    linestyle=METHOD_LINESTYLES[implementation],
                    linewidth=1.8,
                    markersize=5,
                    label=implementation_label(implementation),
                )
                axis.fill_between(x, summary["q10"], summary["q90"], color=METHOD_COLORS[implementation], alpha=0.1)
            axis.set_xscale("log", base=2)
            axis.set_yscale("log")
            payloads = sorted(selected["payload_bytes"].unique())
            axis.set_xticks(payloads, [f"{int(value)} B" for value in payloads], rotation=30)
            axis.spines[["top", "right"]].set_visible(False)
            if row == 0:
                axis.set_title(f"{topology}  {producers}P/{consumers}C")
            if column == 0:
                axis.set_ylabel(ylabel)
            if row == 1:
                axis.set_xlabel("Record payload")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    unique = dict(zip(labels, handles, strict=True))
    figure.legend(unique.values(), unique.keys(), loc="outside lower center", ncol=len(unique))
    figure.suptitle("Record transfer throughput across payload sizes (median with P10-P90 bands)", fontsize=15)
    save_figure(figure, output, "record-ring-payload-throughput")


def speedup_heatmaps(summary: pd.DataFrame, output: Path) -> None:
    baselines = [
        ("Aeron", ["Aeron SPSC algorithm", "Aeron MPSC algorithm"]),
        ("DPDK classic", ["DPDK rte_ring classic fixed-record"]),
        ("Rigtorp", ["Rigtorp SPSCQueue fixed-record", "Rigtorp MPMCQueue fixed-record"]),
        ("mutex packed", ["mutex packed RecordRingBuffer"]),
        ("mutex allocating", ["mutex allocating deque"]),
    ]
    payloads = sorted(summary["payload_bytes"].unique())
    panels = []
    for baseline_label, baseline_names in baselines:
        selected = summary[summary["baseline"].isin(baseline_names)].copy()
        rows = selected[["topology", "producers", "consumers"]].drop_duplicates()
        rows["order"] = rows["topology"].map({name: index for index, name in enumerate(TOPOLOGY_ORDER)})
        rows = rows.sort_values(["order", "producers", "consumers"])
        matrix = np.full((len(rows), len(payloads)), np.nan)
        for row_index, endpoint in enumerate(rows.itertuples(index=False)):
            for column_index, payload in enumerate(payloads):
                match = selected[
                    (selected["topology"] == endpoint.topology)
                    & (selected["producers"] == endpoint.producers)
                    & (selected["consumers"] == endpoint.consumers)
                    & (selected["payload_bytes"] == payload)
                ]
                if len(match) == 1:
                    matrix[row_index, column_index] = match.iloc[0]["speedup_median"]
        panels.append((baseline_label, rows, matrix))
    finite = np.concatenate([matrix[np.isfinite(matrix)] for _, _, matrix in panels])
    limit = max(1.0, float(np.max(np.abs(np.log2(finite))))) if finite.size else 1.0
    norm = colors.TwoSlopeNorm(vmin=-limit, vcenter=0, vmax=limit)
    figure, axes = plt.subplots(1, len(panels), figsize=(28, 7.5), constrained_layout=True)
    image = None
    for axis, (baseline_label, rows, matrix) in zip(axes, panels, strict=True):
        image = axis.imshow(
            np.log2(matrix), cmap="RdBu_r", norm=norm, aspect="auto"
        )
        for row_index in range(matrix.shape[0]):
            for column_index in range(matrix.shape[1]):
                value = matrix[row_index, column_index]
                if np.isfinite(value):
                    axis.text(column_index, row_index, f"{value:.2f}x", ha="center", va="center", fontsize=9)
        axis.set_xticks(range(len(payloads)), [f"{int(value)} B" for value in payloads], rotation=30)
        axis.set_yticks(
            range(len(rows)),
            [f"{row.topology} {row.producers}P/{row.consumers}C" for row in rows.itertuples(index=False)],
        )
        axis.set_title(f"vs {baseline_label}")
    colorbar = figure.colorbar(image, ax=axes, shrink=0.74)
    ticks = np.linspace(-limit, limit, 7)
    colorbar.set_ticks(ticks, labels=[f"{2 ** tick:.2f}x" for tick in ticks])
    figure.suptitle("Mashiro paired median speedup (log2 color scale)", fontsize=15)
    save_figure(figure, output, "record-ring-speedup-heatmaps")


def scaling_plot(frame: pd.DataFrame, output: Path) -> None:
    selected = frame[frame["payload_bytes"] == 64].copy()
    figure, axes = plt.subplots(1, 3, figsize=(17, 5.3), constrained_layout=True)
    for axis, topology in zip(axes, ["MPSC", "SPMC", "MPMC"], strict=True):
        topology_frame = selected[selected["topology"] == topology].copy()
        topology_frame["endpoints"] = topology_frame[["producers", "consumers"]].max(axis=1)
        for implementation in METHOD_ORDER:
            group = topology_frame[topology_frame["implementation"] == implementation]
            if group.empty:
                continue
            summary = group.groupby("endpoints")["million_records_per_second"].agg(
                median="median", q10=lambda values: values.quantile(0.1), q90=lambda values: values.quantile(0.9)
            )
            x = summary.index.to_numpy(dtype=float)
            axis.plot(
                x,
                summary["median"],
                color=METHOD_COLORS[implementation],
                marker=METHOD_MARKERS[implementation],
                linestyle=METHOD_LINESTYLES[implementation],
                linewidth=1.8,
                label=implementation_label(implementation),
            )
            axis.fill_between(x, summary["q10"], summary["q90"], color=METHOD_COLORS[implementation], alpha=0.1)
        axis.set_title(f"{topology}, 64 B payload")
        axis.set_xlabel("Competing endpoint threads")
        axis.set_ylabel("M records/s")
        axis.set_xticks(sorted(topology_frame["endpoints"].unique()))
        axis.set_yscale("log")
        axis.spines[["top", "right"]].set_visible(False)
    handles, labels = axes[0].get_legend_handles_labels()
    unique = dict(zip(labels, handles, strict=True))
    figure.legend(unique.values(), unique.keys(), loc="outside lower center", ncol=len(unique))
    figure.suptitle("Endpoint scaling (median with P10-P90 bands)", fontsize=15)
    save_figure(figure, output, "record-ring-endpoint-scaling")


def retry_pressure(frame: pd.DataFrame, output: Path) -> None:
    selected = frame[frame["payload_bytes"] == 64].copy()
    selected["retries_per_record"] = (selected["push_retries"] + selected["pop_retries"]) / selected["records"]
    figure, axes = plt.subplots(1, 3, figsize=(17, 5.3), constrained_layout=True)
    for axis, topology in zip(axes, ["MPSC", "SPMC", "MPMC"], strict=True):
        topology_frame = selected[selected["topology"] == topology].copy()
        topology_frame["endpoints"] = topology_frame[["producers", "consumers"]].max(axis=1)
        for implementation in METHOD_ORDER:
            group = topology_frame[topology_frame["implementation"] == implementation]
            if group.empty:
                continue
            summary = group.groupby("endpoints")["retries_per_record"].median()
            axis.plot(
                summary.index,
                summary,
                color=METHOD_COLORS[implementation],
                marker=METHOD_MARKERS[implementation],
                linestyle=METHOD_LINESTYLES[implementation],
                linewidth=1.8,
                label=implementation_label(implementation),
            )
        axis.set_title(topology)
        axis.set_xlabel("Competing endpoint threads")
        axis.set_ylabel("Failed non-blocking attempts / record")
        axis.set_yscale("symlog", linthresh=0.01)
        axis.spines[["top", "right"]].set_visible(False)
    handles, labels = axes[0].get_legend_handles_labels()
    unique = dict(zip(labels, handles, strict=True))
    figure.legend(unique.values(), unique.keys(), loc="outside lower center", ncol=len(unique))
    figure.suptitle("Retry pressure at 64 B payload (median)", fontsize=15)
    save_figure(figure, output, "record-ring-retry-pressure")


def forest_plot(summary: pd.DataFrame, output: Path) -> None:
    selected = summary.copy()
    selected["topology_order"] = selected["topology"].map(
        {name: index for index, name in enumerate(TOPOLOGY_ORDER)}
    )
    selected = selected.sort_values(
        ["topology_order", "baseline", "payload_bytes", "producers", "consumers"]
    ).reset_index(drop=True)
    labels = [
        f"{row.topology} {row.producers}P/{row.consumers}C | {int(row.payload_bytes):4} B | "
        f"{implementation_label(row.baseline)}"
        for row in selected.itertuples(index=False)
    ]
    positions = np.arange(len(selected))
    figure, axis = plt.subplots(figsize=(12, max(8, 0.27 * len(selected))), constrained_layout=True)
    lower = selected["speedup_median"] - selected["speedup_ci95_low"]
    upper = selected["speedup_ci95_high"] - selected["speedup_median"]
    point_colors = [METHOD_COLORS[name] for name in selected["baseline"]]
    axis.errorbar(
        selected["speedup_median"],
        positions,
        xerr=np.vstack([lower, upper]),
        fmt="none",
        ecolor="#777777",
        elinewidth=1.0,
        capsize=2,
    )
    axis.scatter(selected["speedup_median"], positions, c=point_colors, s=25, zorder=3)
    axis.axvline(1.0, color="#222222", linestyle="--", linewidth=1.1)
    axis.set_xscale("log", base=2)
    axis.xaxis.set_major_formatter(FuncFormatter(lambda value, _: f"{value:.2f}x"))
    axis.set_yticks(positions, labels)
    axis.invert_yaxis()
    axis.set_xlabel("Mashiro / baseline paired throughput ratio (median, bootstrap 95% CI)")
    axis.set_title("Paired speedup confidence intervals")
    axis.spines[["top", "right"]].set_visible(False)
    save_figure(figure, output, "record-ring-speedup-forest")


def write_report(frame: pd.DataFrame, ratios: pd.DataFrame, summary: pd.DataFrame, output: Path) -> None:
    primary = []
    for topology in TOPOLOGY_ORDER:
        producers, consumers = primary_endpoint(frame, topology)
        rows = summary[
            (summary["topology"] == topology)
            & (summary["producers"] == producers)
            & (summary["consumers"] == consumers)
        ]
        primary.append(rows)
    table = pd.concat(primary).sort_values(["topology", "payload_bytes", "baseline"])
    spsc_aeron = table[(table["topology"] == "SPSC") & table["baseline"].str.startswith("Aeron")]
    mpsc_aeron = table[(table["topology"] == "MPSC") & table["baseline"].str.startswith("Aeron")]
    dpdk = table[table["baseline"] == "DPDK rte_ring classic fixed-record"]
    rigtorp = table[table["baseline"].str.startswith("Rigtorp")]
    packed = table[table["baseline"] == "mutex packed RecordRingBuffer"]
    references = frame[
        ["implementation", "family", "reference_commit", "capacity_semantics", "storage_bytes"]
    ].drop_duplicates()
    reference_rows = []
    for key, group in references.groupby(
        ["implementation", "family", "reference_commit", "capacity_semantics"], sort=False
    ):
        implementation, family, commit, semantics = key
        storage_min = int(group["storage_bytes"].min())
        storage_max = int(group["storage_bytes"].max())
        storage = f"{storage_min:,} B" if storage_min == storage_max else f"{storage_min:,}-{storage_max:,} B"
        if implementation == "mutex allocating deque":
            storage += " + runtime heap"
        reference_rows.append((implementation, family, commit, semantics, storage))
    method_rank = {name: index for index, name in enumerate(METHOD_ORDER)}
    reference_rows.sort(key=lambda row: method_rank[row[0]])
    aggregate = summary.copy()
    aggregate["baseline_group"] = aggregate["baseline"].map(implementation_label)
    aggregate_rows = []
    for baseline, group in aggregate.groupby("baseline_group", sort=False):
        aggregate_rows.append(
            {
                "baseline": baseline,
                "cases": len(group),
                "geometric_mean": geometric_mean(group["speedup_median"]),
                "wins": int((group["speedup_median"] > 1).sum()),
                "significant_wins": int((group["speedup_ci95_low"] > 1).sum()),
                "significant_losses": int((group["speedup_ci95_high"] < 1).sum()),
            }
        )
    aggregate_rank = {name: index for index, name in enumerate(AGGREGATE_ORDER)}
    aggregate_rows.sort(key=lambda row: aggregate_rank[row["baseline"]])
    orientation_keys = [
        "pair",
        "topology",
        "baseline",
        "payload_bytes",
        "byte_capacity",
        "producers",
        "consumers",
        "mashiro_run_position",
    ]
    orientation = ratios.groupby(orientation_keys, sort=False)["speedup"].median().reset_index()
    orientation_means = orientation.groupby("mashiro_run_position")["speedup"].apply(geometric_mean)
    mashiro_first = float(orientation_means["first"])
    mashiro_second = float(orientation_means["second"])
    pair_count = len(
        frame[["pair", "topology", "payload_bytes", "byte_capacity", "producers", "consumers"]].drop_duplicates()
    )
    lines = [
        "# Record Ring Buffer Benchmark",
        "",
        f"- Matrix: {pair_count} implementation pairs, {len(frame):,} measured rows",
        f"- Samples: {int(summary['samples'].min())}-{int(summary['samples'].max())} paired observations/group",
        f"- Byte capacity: {', '.join(f'{int(value):,}' for value in sorted(frame['byte_capacity'].unique()))}",
        f"- Payloads: {', '.join(f'{int(value)} B' for value in sorted(frame['payload_bytes'].unique()))}",
        f"- Thread placement: {', '.join(sorted(frame['thread_placement'].unique()))}",
        f"- Input SHA-256: `{frame.attrs['source_sha256']}`",
        "- Intervals: nonparametric bootstrap 95% CI of the paired median throughput ratio",
        "",
        "## Method",
        "",
        "- Each implementation is independently calibrated to the same minimum sample duration.",
        "- Implementations in a pair run adjacently with alternating AB/BA order to limit temporal drift.",
        "- Every transferred record carries a global identity; record count and checksum are verified per sample.",
        "- Worker threads use deterministic Windows CPU Sets and high thread/process priority.",
        "- Aeron and DPDK baselines are source-equivalent C++ ports of pinned algorithms, not linked binaries.",
        "- Dashed curves are fixed-slot baselines. They avoid Mashiro's variable-record header, alignment, and "
        "wrap-padding costs, so their capacity and storage semantics are deliberately reported as non-identical.",
        "- Accounted storage includes fixed queue objects and arenas. The allocating mutex baseline additionally uses "
        "workload-dependent deque and vector heap blocks that cannot be represented by one constant byte count.",
        "",
        "## Findings",
        "",
        f"- SPSC versus Aeron: {spsc_aeron['speedup_median'].min():.2f}x to "
        f"{spsc_aeron['speedup_median'].max():.2f}x across measured payloads.",
        f"- MPSC 4P/1C versus Aeron: {mpsc_aeron['speedup_median'].min():.2f}x to "
        f"{mpsc_aeron['speedup_median'].max():.2f}x.",
        f"- Versus DPDK classic fixed slots, the primary matrix spans {dpdk['speedup_median'].min():.2f}x to "
        f"{dpdk['speedup_median'].max():.2f}x.",
        f"- Versus Rigtorp fixed slots, the primary matrix spans {rigtorp['speedup_median'].min():.2f}x to "
        f"{rigtorp['speedup_median'].max():.2f}x.",
        f"- Versus mutex packed storage, the complete primary matrix spans "
        f"{packed['speedup_median'].min():.2f}x to {packed['speedup_median'].max():.2f}x; "
        "small-record multi-consumer cases are not uniformly faster.",
        "- Effective payload bandwidth increases with payload size even when records/s falls, so both metrics are "
        "reported.",
        "",
        "## Aggregate Results",
        "",
        "Each case contributes one paired-median ratio, so payloads and endpoint configurations receive equal weight.",
        "",
        "| Baseline | Cases | Geometric mean | Mashiro wins | CI above 1 | CI below 1 |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for row in aggregate_rows:
        lines.append(
            f"| {row['baseline']} | {row['cases']} | {row['geometric_mean']:.3f}x | {row['wins']} | "
            f"{row['significant_wins']} | {row['significant_losses']} |"
        )
    lines.extend(
        [
            "",
            f"Mashiro-first geometric mean: {mashiro_first:.3f}x; Mashiro-second: {mashiro_second:.3f}x; "
            f"orientation quotient: {mashiro_first / mashiro_second:.3f}x.",
            "",
        "## Implementations and Capacity Semantics",
        "",
        "| Implementation | Family | Reference | Capacity semantics | Accounted fixed storage |",
        "|---|---|---|---|---:|",
        ]
    )
    for implementation, family, commit, semantics, storage in reference_rows:
        lines.append(
            f"| {implementation_label(implementation)} | {family} | `{commit}` | `{semantics}` | {storage} |"
        )
    lines.extend(
        [
            "",
        "## Primary Topologies",
        "",
        "| Topology | Endpoints | Payload | Baseline | Mashiro speedup | 95% CI |",
        "|---|---:|---:|---|---:|---:|",
        ]
    )
    for row in table.itertuples(index=False):
        lines.append(
            f"| {row.topology} | {row.producers}P/{row.consumers}C | {int(row.payload_bytes)} B | "
            f"{implementation_label(row.baseline)} | {row.speedup_median:.3f}x | "
            f"[{row.speedup_ci95_low:.3f}, {row.speedup_ci95_high:.3f}] |"
        )
    lines.extend(
        [
            "",
            "## Limits",
            "",
            "- Results describe one Windows host and one 64 KiB byte capacity; they are not cross-machine claims.",
            "- The benchmark measures sustained throughput with a minimal callback, not latency distributions.",
            "- Confidence intervals quantify within-run paired sampling variation, not host-to-host variation.",
            "- Fixed-slot baselines isolate synchronization efficiency but are not semantic substitutes for a packed "
            "variable-record arena.",
            "- SPMC/MPMC callbacks may complete out of claim order; the queue guarantees unique FIFO claims and "
            "safe reclamation, not serialized callback side effects.",
        ]
    )
    (output / "record-ring-benchmark-report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    configure_style()
    frame = load_measurements(args.input)
    ratios = paired_ratios(frame)
    summary = summarize_pairs(ratios, args.bootstrap, args.seed)
    summary.to_csv(args.output / "record-ring-paired-summary.csv", index=False)
    payload_throughput(frame, args.output)
    speedup_heatmaps(summary, args.output)
    scaling_plot(frame, args.output)
    retry_pressure(frame, args.output)
    forest_plot(summary, args.output)
    write_report(frame, ratios, summary, args.output)


if __name__ == "__main__":
    main()

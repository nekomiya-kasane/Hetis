"""Analyze paired Mashiro versus mutex RingBuffer measurements and render scientific figures."""

from __future__ import annotations

import argparse
import hashlib
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.colors as colors
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


LOCKED_QUEUE = "mutex RingBuffer"
MASHIRO_QUEUES = {
    "MPSC": "Mashiro MPSC",
    "SPMC": "Mashiro SPMC",
    "MPMC": "Mashiro MPMC",
}
CASE_KEYS = ["scenario", "payload_bytes", "capacity", "producers", "consumers"]
PAIR_KEYS = ["label", *CASE_KEYS, "sample"]
METHOD_COLORS = {"Mashiro": "#0072B2", "Mutex": "#777777"}
LINE_STYLES = ["-", "--", ":", "-."]


def configure_style() -> None:
    plt.rcParams.update(
        {
            "figure.facecolor": "#FFFFFF",
            "axes.facecolor": "#FAFAFA",
            "axes.edgecolor": "#555555",
            "axes.titleweight": "bold",
            "axes.labelcolor": "#222222",
            "font.size": 10,
            "grid.color": "#D8D8D8",
            "grid.linewidth": 0.7,
            "legend.frameon": False,
            "savefig.facecolor": "#FFFFFF",
            "svg.fonttype": "none",
        }
    )


def load_paired(paths: list[Path]) -> tuple[pd.DataFrame, str, int, float]:
    frames: list[pd.DataFrame] = []
    hashes: list[str] = []
    for path in paths:
        raw = path.read_bytes()
        hashes.append(f"{path.name}: {hashlib.sha256(raw).hexdigest()}")
        frames.append(pd.read_csv(path))
    data = pd.concat(frames, ignore_index=True)
    expected_queues = {LOCKED_QUEUE, *MASHIRO_QUEUES.values()}
    data = data[data["queue"].isin(expected_queues)].copy()
    if data.empty:
        raise ValueError("input contains no lock comparison rows")
    for scenario, queue in MASHIRO_QUEUES.items():
        invalid = data[(data["scenario"] == scenario) & (~data["queue"].isin([queue, LOCKED_QUEUE]))]
        if not invalid.empty:
            raise ValueError(f"unexpected queue in {scenario} rows")

    throughput = data.pivot(index=PAIR_KEYS, columns="queue", values="million_transfers_per_second")
    missing_columns = expected_queues.difference(throughput.columns)
    if missing_columns:
        raise ValueError(f"missing queue columns: {sorted(missing_columns)}")

    pairs: list[pd.DataFrame] = []
    for scenario, mashiro_queue in MASHIRO_QUEUES.items():
        frame = throughput.xs(scenario, level="scenario", drop_level=False).dropna(
            subset=[mashiro_queue, LOCKED_QUEUE]
        )
        frame = frame.reset_index()
        frame["mashiro_mts"] = frame[mashiro_queue]
        frame["mutex_mts"] = frame[LOCKED_QUEUE]
        frame["speedup"] = frame["mashiro_mts"] / frame["mutex_mts"]
        pairs.append(frame[[*PAIR_KEYS, "mashiro_mts", "mutex_mts", "speedup"]])
    paired = pd.concat(pairs, ignore_index=True)

    observed = paired.groupby(CASE_KEYS, observed=True).size()
    if observed.min() != observed.max():
        raise ValueError("case matrix has unequal paired sample counts")
    return (
        paired,
        "\n".join(hashes),
        int(observed.iloc[0]),
        float(data["minimum_sample_ms"].min()),
    )


def percentile(values: pd.Series, probability: float) -> float:
    return float(values.quantile(probability))


def bootstrap_median_ci(values: np.ndarray, seed: int, resamples: int = 20_000) -> tuple[float, float]:
    rng = np.random.default_rng(seed)
    indices = rng.integers(0, values.size, size=(resamples, values.size))
    medians = np.median(values[indices], axis=1)
    low, high = np.quantile(medians, [0.025, 0.975])
    return float(low), float(high)


def summarize(paired: pd.DataFrame, seed: int) -> pd.DataFrame:
    rows: list[dict[str, float | int | str]] = []
    for index, (key, group) in enumerate(paired.groupby(CASE_KEYS, sort=True, observed=True)):
        speedup = group["speedup"].to_numpy(dtype=np.float64)
        ci_low, ci_high = bootstrap_median_ci(speedup, seed + index)
        rows.append(
            {
                **dict(zip(CASE_KEYS, key, strict=True)),
                "paired_samples": len(group),
                "mashiro_median_mts": group["mashiro_mts"].median(),
                "mashiro_q10_mts": percentile(group["mashiro_mts"], 0.10),
                "mashiro_q90_mts": percentile(group["mashiro_mts"], 0.90),
                "mutex_median_mts": group["mutex_mts"].median(),
                "mutex_q10_mts": percentile(group["mutex_mts"], 0.10),
                "mutex_q90_mts": percentile(group["mutex_mts"], 0.90),
                "speedup_median": float(np.median(speedup)),
                "speedup_q10": float(np.quantile(speedup, 0.10)),
                "speedup_q90": float(np.quantile(speedup, 0.90)),
                "speedup_ci95_low": ci_low,
                "speedup_ci95_high": ci_high,
            }
        )
    return pd.DataFrame(rows)


def endpoint_label(row: pd.Series) -> str:
    return f"{int(row.producers)}P/{int(row.consumers)}C"


def save_figure(figure: plt.Figure, output: Path, stem: str) -> None:
    figure.savefig(output / f"{stem}.png", dpi=220, bbox_inches="tight")
    figure.savefig(output / f"{stem}.svg", bbox_inches="tight")
    plt.close(figure)


def speedup_heatmap(summary: pd.DataFrame, output: Path) -> None:
    row_order: list[tuple[str, int, int]] = []
    for scenario in ["MPSC", "SPMC", "MPMC"]:
        topologies = summary.loc[summary["scenario"] == scenario, ["producers", "consumers"]].drop_duplicates()
        topologies = topologies.sort_values(["producers", "consumers"])
        row_order.extend(
            (scenario, int(row.producers), int(row.consumers)) for row in topologies.itertuples(index=False)
        )
    column_order = [
        (payload, capacity)
        for payload in sorted(summary["payload_bytes"].unique())
        for capacity in sorted(summary["capacity"].unique())
    ]
    matrix = np.full((len(row_order), len(column_order)), np.nan)
    for row_index, (scenario, producers, consumers) in enumerate(row_order):
        for column_index, (payload, capacity) in enumerate(column_order):
            selected = summary[
                (summary["scenario"] == scenario)
                & (summary["producers"] == producers)
                & (summary["consumers"] == consumers)
                & (summary["payload_bytes"] == payload)
                & (summary["capacity"] == capacity)
            ]
            if len(selected) == 1:
                matrix[row_index, column_index] = selected.iloc[0]["speedup_median"]

    log_matrix = np.log2(matrix)
    limit = max(1.0, float(np.nanmax(np.abs(log_matrix))))
    norm = colors.TwoSlopeNorm(vmin=-limit, vcenter=0.0, vmax=limit)
    figure, axis = plt.subplots(figsize=(16, 7.5), constrained_layout=True)
    image = axis.imshow(log_matrix, cmap="RdBu_r", norm=norm, aspect="auto")
    for row in range(matrix.shape[0]):
        for column in range(matrix.shape[1]):
            value = matrix[row, column]
            if np.isfinite(value):
                text_color = "white" if abs(log_matrix[row, column]) > 0.55 * limit else "#111111"
                axis.text(column, row, f"{value:.2f}x", ha="center", va="center", color=text_color, fontsize=9)

    axis.set_xticks(
        np.arange(len(column_order)),
        [f"{payload} B\ncap {capacity:,}" for payload, capacity in column_order],
    )
    axis.set_yticks(
        np.arange(len(row_order)),
        [f"{scenario}  {producers}P/{consumers}C" for scenario, producers, consumers in row_order],
    )
    axis.tick_params(axis="x", rotation=30)
    axis.set_title("Mashiro sequence-cell RingBuffer speedup over one-mutex RingBuffer")
    axis.set_xlabel("Payload and bounded capacity")
    colorbar = figure.colorbar(image, ax=axis, shrink=0.88)
    ticks = np.linspace(-limit, limit, 7)
    colorbar.set_ticks(ticks, labels=[f"{2 ** tick:.2f}x" for tick in ticks])
    colorbar.set_label("Paired median throughput ratio (log2 color scale)")
    save_figure(figure, output, "locked-ring-speedup-heatmap")


def throughput_scaling(summary: pd.DataFrame, output: Path) -> None:
    payloads = sorted(summary["payload_bytes"].unique())
    scenarios = ["MPSC", "SPMC", "MPMC"]
    figure, axes = plt.subplots(
        len(payloads), len(scenarios), figsize=(16, 11), sharex=True, constrained_layout=True
    )
    axes = np.asarray(axes).reshape(len(payloads), len(scenarios))
    for row_index, payload in enumerate(payloads):
        for column_index, scenario in enumerate(scenarios):
            axis = axes[row_index, column_index]
            frame = summary[(summary["payload_bytes"] == payload) & (summary["scenario"] == scenario)].copy()
            frame["endpoints"] = np.maximum(frame["producers"], frame["consumers"])
            for capacity_index, capacity in enumerate(sorted(frame["capacity"].unique())):
                group = frame[frame["capacity"] == capacity].sort_values("endpoints")
                for method, prefix in [("Mashiro", "mashiro"), ("Mutex", "mutex")]:
                    axis.plot(
                        group["endpoints"],
                        group[f"{prefix}_median_mts"],
                        color=METHOD_COLORS[method],
                        linestyle=LINE_STYLES[capacity_index % len(LINE_STYLES)],
                        marker="o" if method == "Mashiro" else "s",
                        markersize=4,
                        linewidth=1.7,
                        label=f"{method}, cap {int(capacity):,}",
                    )
                    axis.fill_between(
                        group["endpoints"],
                        group[f"{prefix}_q10_mts"],
                        group[f"{prefix}_q90_mts"],
                        color=METHOD_COLORS[method],
                        alpha=0.07,
                    )
            axis.set_yscale("log")
            axis.set_xticks([2, 4, 8])
            axis.grid(True, which="both")
            axis.spines[["top", "right"]].set_visible(False)
            if row_index == 0:
                axis.set_title(scenario)
            if column_index == 0:
                axis.set_ylabel(f"{payload} B payload\nM transfers/s")
            if row_index == len(payloads) - 1:
                axis.set_xlabel("Competing endpoint threads")
    handles, labels = axes[0, 0].get_legend_handles_labels()
    figure.legend(handles, labels, loc="outside lower center", ncol=3)
    figure.suptitle("Absolute throughput scaling, median with P10-P90 bands")
    save_figure(figure, output, "locked-ring-throughput-scaling")


def payload_throughput(summary: pd.DataFrame, output: Path) -> None:
    scenarios = ["MPSC", "SPMC", "MPMC"]
    payloads = sorted(summary["payload_bytes"].unique())
    endpoint_counts = [
        summary.loc[summary["scenario"] == scenario, ["producers", "consumers"]].drop_duplicates().shape[0]
        for scenario in scenarios
    ]
    columns = max(endpoint_counts)
    figure, axes = plt.subplots(
        len(scenarios), columns, figsize=(16, 11), sharex=True, sharey="row", squeeze=False, constrained_layout=True
    )
    first_axis: plt.Axes | None = None
    for row_index, scenario in enumerate(scenarios):
        frame = summary[summary["scenario"] == scenario].copy()
        endpoints = frame[["producers", "consumers"]].drop_duplicates().sort_values(["producers", "consumers"])
        for column_index, axis in enumerate(axes[row_index]):
            if column_index >= len(endpoints):
                axis.set_visible(False)
                continue
            if first_axis is None:
                first_axis = axis
            endpoint = endpoints.iloc[column_index]
            producers = int(endpoint["producers"])
            consumers = int(endpoint["consumers"])
            selected = frame[(frame["producers"] == producers) & (frame["consumers"] == consumers)]
            for capacity_index, capacity in enumerate(sorted(selected["capacity"].unique())):
                group = selected[selected["capacity"] == capacity].sort_values("payload_bytes")
                for method, prefix in [("Mashiro", "mashiro"), ("Mutex", "mutex")]:
                    axis.plot(
                        group["payload_bytes"],
                        group[f"{prefix}_median_mts"],
                        color=METHOD_COLORS[method],
                        linestyle=LINE_STYLES[capacity_index % len(LINE_STYLES)],
                        marker="o" if method == "Mashiro" else "s",
                        markersize=4,
                        linewidth=1.7,
                        label=f"{method}, cap {int(capacity):,}",
                    )
                    axis.fill_between(
                        group["payload_bytes"],
                        group[f"{prefix}_q10_mts"],
                        group[f"{prefix}_q90_mts"],
                        color=METHOD_COLORS[method],
                        alpha=0.07,
                    )
            axis.set_xscale("log", base=2)
            axis.set_yscale("log")
            axis.set_xticks(payloads, [f"{int(payload)} B" for payload in payloads])
            axis.set_title(f"{producers}P/{consumers}C")
            axis.grid(True, which="both")
            axis.spines[["top", "right"]].set_visible(False)
            if column_index == 0:
                axis.set_ylabel(f"{scenario}\nM transfers/s")
            if row_index == len(scenarios) - 1:
                axis.set_xlabel("Payload size")
    if first_axis is not None:
        handles, labels = first_axis.get_legend_handles_labels()
        figure.legend(handles, labels, loc="outside lower center", ncol=3)
    figure.suptitle("Absolute throughput by payload size, median with P10-P90 bands")
    save_figure(figure, output, "locked-ring-payload-throughput")


def speedup_forest(summary: pd.DataFrame, output: Path) -> None:
    scenarios = ["MPSC", "SPMC", "MPMC"]
    figure, axes = plt.subplots(1, 3, figsize=(19, 11), constrained_layout=True, sharex=True)
    for axis, scenario in zip(axes, scenarios, strict=True):
        frame = summary[summary["scenario"] == scenario].copy()
        frame["endpoints"] = np.maximum(frame["producers"], frame["consumers"])
        frame = frame.sort_values(["payload_bytes", "capacity", "endpoints"])
        labels = [
            f"{int(row.payload_bytes):>3} B | cap {int(row.capacity):>5,} | {endpoint_label(row)}"
            for _, row in frame.iterrows()
        ]
        positions = np.arange(len(frame))
        lower = frame["speedup_median"] - frame["speedup_ci95_low"]
        upper = frame["speedup_ci95_high"] - frame["speedup_median"]
        axis.errorbar(
            frame["speedup_median"],
            positions,
            xerr=np.vstack([lower, upper]),
            fmt="o",
            color="#0072B2",
            ecolor="#7A7A7A",
            elinewidth=1.1,
            capsize=2,
            markersize=4,
        )
        axis.axvline(1.0, color="#B2182B", linewidth=1.2)
        axis.set_xscale("log", base=2)
        axis.set_yticks(positions, labels, fontsize=8)
        axis.set_title(scenario)
        axis.set_xlabel("Paired speedup, bootstrap 95% CI")
        axis.grid(axis="x", which="both")
        axis.spines[["top", "right", "left"]].set_visible(False)
        axis.invert_yaxis()
    figure.suptitle("Per-case uncertainty: Mashiro / mutex RingBuffer", y=1.02)
    save_figure(figure, output, "locked-ring-speedup-confidence")


def geometric_mean(values: pd.Series) -> float:
    return float(math.exp(np.log(values.to_numpy(dtype=np.float64)).mean()))


def write_report(
    paired: pd.DataFrame,
    summary: pd.DataFrame,
    output: Path,
    csv_hashes: str,
    machine: str,
    compiler: str,
    measured_samples: int,
    minimum_sample_ms: float,
    warmups: int,
) -> None:
    global_geomean = geometric_mean(summary["speedup_median"])
    topology = (
        summary.groupby("scenario", observed=True)["speedup_median"]
        .agg(cases="size", geomean=geometric_mean, minimum="min", maximum="max")
        .reset_index()
    )
    significance = summary.assign(
        outcome=np.select(
            [summary["speedup_ci95_low"] > 1.0, summary["speedup_ci95_high"] < 1.0],
            ["faster", "slower"],
            default="inconclusive",
        )
    )
    significance = (
        significance.groupby(["scenario", "outcome"], observed=True)
        .size()
        .unstack(fill_value=0)
        .reindex(columns=["faster", "slower", "inconclusive"], fill_value=0)
        .reset_index()
    )
    topology = topology.merge(significance, on="scenario", how="left").fillna(0)
    capacity = (
        summary.groupby("capacity", observed=True)["speedup_median"]
        .agg(geomean=geometric_mean, minimum="min", maximum="max")
        .reset_index()
    )
    grouped = (
        summary.groupby(["scenario", "payload_bytes"], observed=True)["speedup_median"]
        .agg(geomean=geometric_mean, minimum="min", maximum="max")
        .reset_index()
    )
    best = summary.loc[summary["speedup_median"].idxmax()]
    worst = summary.loc[summary["speedup_median"].idxmin()]
    order = paired.copy()
    order["orientation"] = np.where((order["sample"] & 1) == 0, "Mashiro first", "Mutex first")
    orientation = order.groupby([*CASE_KEYS, "orientation"], observed=True)["speedup"].median().unstack()
    mashiro_first_geomean = geometric_mean(orientation["Mashiro first"])
    mutex_first_geomean = geometric_mean(orientation["Mutex first"])
    orientation_ratio = mashiro_first_geomean / mutex_first_geomean
    lines = [
        "# Mashiro sequence-cell RingBuffer versus one-mutex RingBuffer",
        "",
        "## Experiment",
        "",
        f"- Machine: `{machine}`",
        f"- Compiler: `{compiler}`",
        "- Queue contract: bounded, strict FIFO, non-blocking `TryPush/TryPop`, identical fixed local RingBuffer",
        "  payload storage in the lock baseline.",
        "- Scheduling: Windows CPU Set pinning and elevated process priority.",
        f"- Sampling: paired AB/BA order, equal transfer count, {warmups} warmups,"
        f" {measured_samples} measured samples.",
        f"- Calibration: each implementation uses an independent {minimum_sample_ms:g} ms pilot target; each pair",
        "  then uses the larger calibrated transfer count.",
        "- Integrity: every sample verifies the transfer count and the checksum of all payload IDs.",
        "- Uncertainty: per-case paired median, P10/P90, and 20,000-resample bootstrap 95% CI.",
        "",
        "## Result",
        "",
        f"The equal-weight geometric mean of all {len(summary)} case medians is **{global_geomean:.3f}x**.",
        "This is a machine- and workload-specific throughput ratio, not a universal constant.",
        "",
        "| Topology | Cases | Geometric mean | Minimum | Maximum | CI > 1 | CI < 1 | CI crosses 1 |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in topology.itertuples(index=False):
        lines.append(
            f"| {row.scenario} | {int(row.cases)} | {row.geomean:.3f}x | {row.minimum:.3f}x | "
            f"{row.maximum:.3f}x | {int(row.faster)} | {int(row.slower)} | {int(row.inconclusive)} |"
        )
    lines.extend(
        [
            "",
            "### Payload by topology",
            "",
            "| Topology | Payload | Geometric mean | Minimum case | Maximum case |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for row in grouped.itertuples(index=False):
        lines.append(
            f"| {row.scenario} | {int(row.payload_bytes)} B | {row.geomean:.3f}x | "
            f"{row.minimum:.3f}x | {row.maximum:.3f}x |"
        )
    lines.extend(
        [
            "",
            "### Capacity effect",
            "",
            "| Capacity | Geometric mean | Minimum case | Maximum case |",
            "|---:|---:|---:|---:|",
        ]
    )
    for row in capacity.itertuples(index=False):
        lines.append(
            f"| {int(row.capacity):,} | {row.geomean:.3f}x | {row.minimum:.3f}x | {row.maximum:.3f}x |"
        )
    lines.extend(
        [
            "",
            "### Execution-order diagnostic",
            "",
            f"- Mashiro-first sample medians: **{mashiro_first_geomean:.3f}x** geometric mean.",
            f"- Mutex-first sample medians: **{mutex_first_geomean:.3f}x** geometric mean.",
            f"- Orientation quotient: **{orientation_ratio:.3f}x**. This measures residual time/order bias, not queue",
            "  speedup; values near 1 indicate that AB/BA alternation controlled short-term drift.",
            "",
            "## Extremes",
            "",
            f"- Fastest relative case: {best.scenario} {endpoint_label(best)}, {int(best.payload_bytes)} B, "
            f"capacity {int(best.capacity):,}: **{best.speedup_median:.3f}x** "
            f"(95% CI {best.speedup_ci95_low:.3f}-{best.speedup_ci95_high:.3f}x).",
            f"- Slowest relative case: {worst.scenario} {endpoint_label(worst)}, {int(worst.payload_bytes)} B, "
            f"capacity {int(worst.capacity):,}: **{worst.speedup_median:.3f}x** "
            f"(95% CI {worst.speedup_ci95_low:.3f}-{worst.speedup_ci95_high:.3f}x).",
            "",
            "## Artifacts",
            "",
            "- `locked-ring-summary.csv`: complete numerical summary for every case.",
            "- `locked-ring-speedup-heatmap.*`: all case medians on a centered log2 color scale.",
            "- `locked-ring-throughput-scaling.*`: absolute throughput and P10-P90 variability.",
            "- `locked-ring-payload-throughput.*`: absolute throughput across measured payload sizes.",
            "- `locked-ring-speedup-confidence.*`: bootstrap confidence intervals for every case.",
            "",
            "## Input SHA-256",
            "",
            "```text",
            csv_hashes,
            "```",
            "",
        ]
    )
    (output / "locked-ring-report.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv", nargs="+", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--machine", default="unspecified")
    parser.add_argument("--compiler", default="COCA clang-p2996")
    parser.add_argument("--seed", type=int, default=0x4D41534849524F)
    parser.add_argument("--warmups", type=int, default=2)
    args = parser.parse_args()

    configure_style()
    args.output.mkdir(parents=True, exist_ok=True)
    paired, csv_hashes, measured_samples, minimum_sample_ms = load_paired(args.csv)
    summary = summarize(paired, args.seed)
    summary.to_csv(args.output / "locked-ring-summary.csv", index=False)
    speedup_heatmap(summary, args.output)
    throughput_scaling(summary, args.output)
    payload_throughput(summary, args.output)
    speedup_forest(summary, args.output)
    write_report(
        paired,
        summary,
        args.output,
        csv_hashes,
        args.machine,
        args.compiler,
        measured_samples,
        minimum_sample_ms,
        args.warmups,
    )


if __name__ == "__main__":
    main()

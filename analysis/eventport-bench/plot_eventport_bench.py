from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter


ROOT = Path(__file__).resolve().parent
CSV_PATH = ROOT / "results" / "eventport-bench-x64-profile.csv"
OUT_PNG = ROOT / "results" / "eventport-bench-overview.png"
OUT_SVG = ROOT / "results" / "eventport-bench-overview.svg"

PATH_LABELS = {
    "kernel_extension": "Kernel extension",
    "com_adaptor_extension": "ComAdaptor extension",
    "plain_inline_member": "Plain inline member",
    "plain_external_unordered_map": "External unordered_map",
}

COLORS = {
    "kernel_extension": "#D55E00",
    "com_adaptor_extension": "#0072B2",
    "plain_inline_member": "#009E73",
    "plain_external_unordered_map": "#CC79A7",
}


def ns_formatter(value: float, _: int) -> str:
    if value >= 1000:
        return f"{value / 1000:g}k"
    return f"{value:g}"


def read_rows() -> list[dict[str, str]]:
    with CSV_PATH.open(newline="", encoding="utf-8") as file:
        return list(csv.DictReader(file))


def median(row: dict[str, str]) -> float:
    return float(row["median_ns_per_op"])


def main() -> None:
    rows = read_rows()
    lookup_rows = [row for row in rows if row["scenario"] == "port_lookup"]
    emit_rows = [row for row in rows if row["scenario"] == "lookup_plus_emit"]
    path_order = list(PATH_LABELS)
    listener_counts = [0, 1, 8, 64]

    plt.rcParams.update(
        {
            "figure.dpi": 150,
            "savefig.dpi": 220,
            "font.size": 11,
            "axes.titlesize": 14,
            "axes.labelsize": 11,
            "legend.fontsize": 10,
            "axes.spines.top": False,
            "axes.spines.right": False,
            "axes.grid": True,
            "grid.alpha": 0.25,
            "grid.linewidth": 0.8,
        }
    )

    fig = plt.figure(figsize=(14, 8), constrained_layout=True)
    grid = fig.add_gridspec(2, 2, width_ratios=[1.05, 1.35], height_ratios=[1.0, 0.95])
    ax_lookup = fig.add_subplot(grid[0, 0])
    ax_emit = fig.add_subplot(grid[0, 1])
    ax_ratio = fig.add_subplot(grid[1, :])

    lookup_by_path = {row["path"]: median(row) for row in lookup_rows}
    lookup_values = [lookup_by_path[path] for path in path_order]
    y_positions = range(len(path_order))
    ax_lookup.barh(
        y_positions,
        lookup_values,
        color=[COLORS[path] for path in path_order],
        edgecolor="white",
        linewidth=1.0,
    )
    ax_lookup.set_yticks(list(y_positions), [PATH_LABELS[path] for path in path_order])
    ax_lookup.invert_yaxis()
    ax_lookup.set_xscale("log")
    ax_lookup.set_xlabel("median lookup latency (ns/op, log scale)")
    ax_lookup.set_title("Port lookup cost")
    ax_lookup.xaxis.set_major_formatter(FuncFormatter(ns_formatter))
    for y, value in zip(y_positions, lookup_values, strict=True):
        ax_lookup.text(value * 1.12, y, f"{value:.2f}", va="center", fontsize=9)

    emit_by_path = {
        path: {
            int(row["listeners"]): median(row)
            for row in emit_rows
            if row["path"] == path
        }
        for path in path_order
    }
    for path in path_order:
        values = [emit_by_path[path][listeners] for listeners in listener_counts]
        ax_emit.plot(
            listener_counts,
            values,
            marker="o",
            linewidth=2.4,
            markersize=6,
            color=COLORS[path],
            label=PATH_LABELS[path],
        )
        ax_emit.text(listener_counts[-1] * 1.04, values[-1], f"{values[-1]:.0f}", color=COLORS[path], va="center")
    ax_emit.set_xscale("symlog", linthresh=1)
    ax_emit.set_yscale("log")
    ax_emit.set_xticks(listener_counts, [str(x) for x in listener_counts])
    ax_emit.set_xlabel("listeners")
    ax_emit.set_ylabel("median lookup + emit latency (ns/op, log scale)")
    ax_emit.set_title("Lookup + synchronous Emit cost")
    ax_emit.yaxis.set_major_formatter(FuncFormatter(ns_formatter))
    ax_emit.legend(loc="upper left", frameon=False)

    inline = emit_by_path["plain_inline_member"]
    x = range(len(listener_counts))
    width = 0.2
    offsets = {
        "kernel_extension": -1.5 * width,
        "com_adaptor_extension": -0.5 * width,
        "plain_inline_member": 0.5 * width,
        "plain_external_unordered_map": 1.5 * width,
    }
    for path in path_order:
        ratios = [emit_by_path[path][listeners] / inline[listeners] for listeners in listener_counts]
        bars = ax_ratio.bar(
            [i + offsets[path] for i in x],
            ratios,
            width=width,
            color=COLORS[path],
            edgecolor="white",
            linewidth=1.0,
            label=PATH_LABELS[path],
        )
        for bar, ratio in zip(bars, ratios, strict=True):
            if ratio >= 1.15:
                ax_ratio.text(
                    bar.get_x() + bar.get_width() / 2,
                    ratio + 0.12,
                    f"{ratio:.1f}x",
                    ha="center",
                    va="bottom",
                    fontsize=8,
                    rotation=0,
                )
    ax_ratio.axhline(1.0, color="#333333", linewidth=1.0)
    ax_ratio.set_xticks(list(x), [str(v) for v in listener_counts])
    ax_ratio.set_xlabel("listeners")
    ax_ratio.set_ylabel("relative to plain inline member")
    ax_ratio.set_title("Relative overhead in full call path")
    ax_ratio.set_ylim(0, 15)
    ax_ratio.legend(loc="upper left", ncols=4, frameon=False)

    fig.suptitle("Sora EventPort Access Paths: median ns/op, x64-profile", fontsize=16, fontweight="bold")
    OUT_PNG.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT_PNG, bbox_inches="tight")
    fig.savefig(OUT_SVG, bbox_inches="tight")
    print(OUT_PNG)
    print(OUT_SVG)


if __name__ == "__main__":
    main()

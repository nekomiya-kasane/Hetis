from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


ROOT = Path(__file__).resolve().parent
csv_path = ROOT / "results.csv"
out_path = ROOT / "flat_map_vs_unordered_map.png"

df = pd.read_csv(csv_path)
impl = ", ".join(sorted(df.loc[df["container"] == "flat_map", "implementation"].unique()))

fig, axes = plt.subplots(2, 2, figsize=(13, 9), constrained_layout=True)
operations = ["hit_lookup", "miss_lookup", "iteration", "build_insert"]
titles = {
    "hit_lookup": "Successful lookup",
    "miss_lookup": "Miss lookup",
    "iteration": "Full iteration",
    "build_insert": "Build / insert",
}

for ax, operation in zip(axes.flat, operations):
    subset = df[df["operation"] == operation]
    for container, group in subset.groupby("container"):
        group = group.sort_values("n")
        label = "flat_map" if container == "flat_map" else "unordered_map"
        ax.plot(group["n"], group["ns_per_op"], marker="o", linewidth=2, label=label)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_title(titles[operation])
    ax.set_xlabel("N")
    ax.set_ylabel("ns/op")
    ax.grid(True, which="both", alpha=0.25)
    ax.legend()

fig.suptitle(f"flat_map vs unordered_map by scale; flat implementation: {impl}", fontsize=14)
fig.savefig(out_path, dpi=180)
print(out_path)

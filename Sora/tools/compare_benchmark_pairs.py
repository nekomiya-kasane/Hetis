"""Compare paired benchmark process outputs without changing measurement scope."""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Comparison:
    key: tuple[str, ...]
    samples: int
    paired_median: float
    wins: int
    baseline_median: float
    candidate_median: float
    minimum_ratio: float
    maximum_ratio: float


def parse_runs(specification: str) -> tuple[int, ...]:
    """Parse comma-separated run numbers and inclusive ``first:last`` ranges."""
    runs: list[int] = []
    for item in specification.split(","):
        item = item.strip()
        if not item:
            raise ValueError("run specification contains an empty item")
        if ":" in item:
            first_text, last_text = item.split(":", 1)
            first, last = int(first_text), int(last_text)
            if first <= 0 or last < first:
                raise ValueError(f"invalid inclusive run range: {item}")
            runs.extend(range(first, last + 1))
        else:
            run = int(item)
            if run <= 0:
                raise ValueError(f"run number must be positive: {item}")
            runs.append(run)
    if len(set(runs)) != len(runs):
        raise ValueError("run specification contains duplicates")
    return tuple(runs)


def parse_filters(filters: Iterable[str]) -> dict[str, str]:
    """Parse repeated ``column=value`` filters."""
    result: dict[str, str] = {}
    for expression in filters:
        column, separator, value = expression.partition("=")
        if not separator or not column:
            raise ValueError(f"invalid filter: {expression}")
        if column in result:
            raise ValueError(f"duplicate filter column: {column}")
        result[column] = value
    return result


def read_values(
    path: Path,
    fieldnames: Sequence[str] | None,
    filters: dict[str, str],
    key_columns: Sequence[str],
    value_column: str,
) -> dict[tuple[str, ...], float]:
    """Read one process result and return its unique filtered benchmark values."""
    with path.open("r", encoding="utf-8", newline="") as stream:
        rows = csv.DictReader(stream, fieldnames=fieldnames)
        if rows.fieldnames is None:
            raise ValueError(f"{path}: missing CSV header")
        required = {*filters, *key_columns, value_column}
        missing = required.difference(rows.fieldnames)
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(sorted(missing))}")

        values: dict[tuple[str, ...], float] = {}
        for line, row in enumerate(rows, start=2 if fieldnames is None else 1):
            if any(row.get(column) != expected for column, expected in filters.items()):
                continue
            key_values = tuple(row.get(column) or "" for column in key_columns)
            if any(not value for value in key_values):
                raise ValueError(f"{path}:{line}: empty key field")
            text = row.get(value_column)
            if text is None:
                raise ValueError(f"{path}:{line}: missing value field")
            try:
                value = float(text)
            except ValueError as error:
                raise ValueError(f"{path}:{line}: invalid numeric value: {text}") from error
            if value <= 0.0:
                raise ValueError(f"{path}:{line}: timing value must be positive")
            if key_values in values:
                raise ValueError(f"{path}:{line}: duplicate key: {key_values}")
            values[key_values] = value
    if not values:
        raise ValueError(f"{path}: no rows matched the requested filters")
    return values


def compare_pairs(
    baseline_pattern: str,
    candidate_pattern: str,
    runs: Sequence[int],
    fieldnames: Sequence[str] | None,
    filters: dict[str, str],
    key_columns: Sequence[str],
    value_column: str,
) -> list[Comparison]:
    """Compare matching keys from independent paired process outputs."""
    samples: dict[tuple[str, ...], list[tuple[float, float]]] = {}
    expected_keys: set[tuple[str, ...]] | None = None
    for run in runs:
        baseline_path = Path(baseline_pattern.format(run=run))
        candidate_path = Path(candidate_pattern.format(run=run))
        baseline = read_values(baseline_path, fieldnames, filters, key_columns, value_column)
        candidate = read_values(candidate_path, fieldnames, filters, key_columns, value_column)
        if baseline.keys() != candidate.keys():
            missing = sorted(baseline.keys() - candidate.keys())
            extra = sorted(candidate.keys() - baseline.keys())
            raise ValueError(f"run {run}: key mismatch; missing={missing}, extra={extra}")
        run_keys = set(baseline)
        if expected_keys is None:
            expected_keys = run_keys
        elif run_keys != expected_keys:
            missing = sorted(expected_keys - run_keys)
            extra = sorted(run_keys - expected_keys)
            raise ValueError(f"run {run}: key set changed; missing={missing}, extra={extra}")
        for key, baseline_value in baseline.items():
            samples.setdefault(key, []).append((baseline_value, candidate[key]))

    comparisons: list[Comparison] = []
    for key in sorted(samples):
        pairs = samples[key]
        ratios = [baseline / candidate for baseline, candidate in pairs]
        comparisons.append(
            Comparison(
                key=key,
                samples=len(pairs),
                paired_median=statistics.median(ratios),
                wins=sum(ratio > 1.0 for ratio in ratios),
                baseline_median=statistics.median(baseline for baseline, _ in pairs),
                candidate_median=statistics.median(candidate for _, candidate in pairs),
                minimum_ratio=min(ratios),
                maximum_ratio=max(ratios),
            )
        )
    return comparisons


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, help="Baseline file pattern containing {run}.")
    parser.add_argument("--candidate", required=True, help="Candidate file pattern containing {run}.")
    parser.add_argument("--runs", required=True, help="Run numbers, for example 1:7 or 1,3,5.")
    parser.add_argument("--header", help="Explicit comma-separated fields for headerless CSV input.")
    parser.add_argument("--where", action="append", default=[], help="Exact column=value filter; repeatable.")
    parser.add_argument("--key", required=True, help="Comma-separated columns identifying one benchmark case.")
    parser.add_argument("--value", required=True, help="Column containing the positive timing value.")
    return parser


def main(arguments: Sequence[str] | None = None) -> int:
    parser = build_parser()
    options = parser.parse_args(arguments)
    try:
        runs = parse_runs(options.runs)
        filters = parse_filters(options.where)
        fieldnames = tuple(options.header.split(",")) if options.header else None
        key_columns = tuple(column.strip() for column in options.key.split(",") if column.strip())
        if not key_columns:
            raise ValueError("at least one key column is required")
        comparisons = compare_pairs(
            options.baseline,
            options.candidate,
            runs,
            fieldnames,
            filters,
            key_columns,
            options.value,
        )
    except (OSError, ValueError) as error:
        parser.error(str(error))

    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(
        [
            *key_columns,
            "samples",
            "paired_median",
            "wins",
            "baseline_median",
            "candidate_median",
            "minimum_ratio",
            "maximum_ratio",
        ]
    )
    for comparison in comparisons:
        writer.writerow(
            [
                *comparison.key,
                comparison.samples,
                f"{comparison.paired_median:.9f}",
                comparison.wins,
                f"{comparison.baseline_median:.9f}",
                f"{comparison.candidate_median:.9f}",
                f"{comparison.minimum_ratio:.9f}",
                f"{comparison.maximum_ratio:.9f}",
            ]
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

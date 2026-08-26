"""Tests for paired benchmark process comparison."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from compare_benchmark_pairs import compare_pairs, parse_filters, parse_runs


class CompareBenchmarkPairsTest(unittest.TestCase):
    def test_headered_csv_uses_true_odd_median(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline_pattern = str(root / "baseline-{run}.csv")
            candidate_pattern = str(root / "candidate-{run}.csv")
            baseline_values = (10.0, 12.0, 8.0)
            candidate_values = (5.0, 6.0, 8.0)
            for run, (baseline, candidate) in enumerate(zip(baseline_values, candidate_values), start=1):
                (root / f"baseline-{run}.csv").write_text(
                    f"metric,case,stat,value\nlowering,chain,p50,{baseline}\n", encoding="utf-8"
                )
                (root / f"candidate-{run}.csv").write_text(
                    f"metric,case,stat,value\nlowering,chain,p50,{candidate}\n", encoding="utf-8"
                )

            result = compare_pairs(
                baseline_pattern,
                candidate_pattern,
                (1, 2, 3),
                None,
                {"metric": "lowering", "stat": "p50"},
                ("case",),
                "value",
            )

            self.assertEqual(len(result), 1)
            self.assertEqual(result[0].paired_median, 2.0)
            self.assertEqual(result[0].wins, 2)
            self.assertEqual(result[0].baseline_median, 10.0)
            self.assertEqual(result[0].candidate_median, 6.0)

    def test_headerless_csv_supports_short_summary_rows(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, value in (("baseline", 100.0), ("candidate", 80.0)):
                (root / f"{name}-1.csv").write_text(
                    f"heft_phase,chain,lowering,p50,50,ns/plan\nchain,{value},ns/plan\nchecksum,1,value\n",
                    encoding="utf-8",
                )

            result = compare_pairs(
                str(root / "baseline-{run}.csv"),
                str(root / "candidate-{run}.csv"),
                (1,),
                ("metric", "case", "phase", "stat", "value", "unit"),
                {"phase": "ns/plan"},
                ("metric",),
                "case",
            )

            self.assertEqual(result[0].key, ("chain",))
            self.assertEqual(result[0].paired_median, 1.25)

    def test_rejects_mismatched_keys(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "baseline-1.csv").write_text("case,value\na,1\n", encoding="utf-8")
            (root / "candidate-1.csv").write_text("case,value\nb,1\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "key mismatch"):
                compare_pairs(
                    str(root / "baseline-{run}.csv"),
                    str(root / "candidate-{run}.csv"),
                    (1,),
                    None,
                    {},
                    ("case",),
                    "value",
                )

    def test_rejects_key_set_changes_between_runs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for role in ("baseline", "candidate"):
                (root / f"{role}-1.csv").write_text("case,value\na,1\nb,2\n", encoding="utf-8")
                (root / f"{role}-2.csv").write_text("case,value\na,1\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "key set changed"):
                compare_pairs(
                    str(root / "baseline-{run}.csv"),
                    str(root / "candidate-{run}.csv"),
                    (1, 2),
                    None,
                    {},
                    ("case",),
                    "value",
                )

    def test_parses_runs_and_filters(self) -> None:
        self.assertEqual(parse_runs("1:3,5"), (1, 2, 3, 5))
        self.assertEqual(parse_filters(("metric=lowering", "stat=p50")), {"metric": "lowering", "stat": "p50"})


if __name__ == "__main__":
    unittest.main()

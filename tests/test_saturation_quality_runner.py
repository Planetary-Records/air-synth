"""Fixed-purpose quality policy/CLI checks; never execute the DSP workload here."""
import argparse
import json
from pathlib import Path
import subprocess
import unittest

PARSER = argparse.ArgumentParser()
PARSER.add_argument("--policy", type=Path, required=True)
PARSER.add_argument("--quality", type=Path)
PARSER.add_argument("--output", type=Path, required=True)
ARGS = None


class QualityPolicy(unittest.TestCase):
    def test_completed_cohort_is_incomplete_quality_and_indexed(self):
        if ARGS is None:
            self.skipTest("fixed-purpose check requires explicit executables and fresh output")
        directory = ARGS.output / "cohort"
        result = subprocess.run([str(ARGS.policy), "cohort", str(directory)], capture_output=True)
        (ARGS.output / "cohort.stdout").write_bytes(result.stdout)
        (ARGS.output / "cohort.stderr").write_bytes(result.stderr)
        self.assertEqual(result.returncode, 0)
        receipt = json.loads((directory / "cohort.json").read_text())
        self.assertEqual(receipt["status"], "INCOMPLETE_QUALITY")
        self.assertEqual(receipt["cohort"], "alias-44100")
        self.assertIsNone(receipt["selected_factor"])
        self.assertFalse(receipt["adoptable"])
        self.assertFalse((directory / "decision.json").exists())
        import csv
        import hashlib
        with (directory / "artifact-hashes.csv").open(newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual({row["file"] for row in rows},
            {p.name for p in directory.iterdir()} - {"artifact-hashes.csv"})
        for row in rows:
            raw = (directory / row["file"]).read_bytes()
            self.assertEqual(int(row["bytes"]), len(raw))
            self.assertEqual(row["sha256"], hashlib.sha256(raw).hexdigest())

    def test_completed_outcomes_and_incomplete_receipts(self):
        if ARGS is None:
            self.skipTest("fixed-purpose check requires explicit executables and fresh output")
        for case, code, status in (("pass", 0, "quality_pass"), ("reject", 1, "quality_rejected"),
                                   ("invalid", 2, "invalid"), ("missing", 2, "invalid"),
                                   ("duplicate", 2, "invalid"), ("unknown", 2, "invalid")):
            with self.subTest(case=case):
                directory = ARGS.output / case
                result = subprocess.run([str(ARGS.policy), case, str(directory)], capture_output=True)
                (ARGS.output / (case + ".stdout")).write_bytes(result.stdout)
                (ARGS.output / (case + ".stderr")).write_bytes(result.stderr)
                self.assertEqual(result.returncode, code)
                receipt = json.loads((directory / "decision.json").read_text())
                self.assertEqual(receipt["status"], status)
                self.assertEqual(receipt["scope"], "quality_only")
                self.assertEqual(receipt["diagnostic_factor"], 4)
                self.assertIsNone(receipt["selected_factor"])
                self.assertEqual(receipt["production"], "FORCED_BYPASS")
                self.assertEqual(receipt["cpu_validation"], "DEFERRED")
                self.assertFalse(receipt["adoptable"])
                self.assertNotIn("all_gates_pass", receipt)
                self.assertNotIn("passing_candidate", json.dumps(receipt))
                self.assertEqual(receipt["not_run"], {"core_cpu_mean_and_p99": "NOT_RUN",
                    "callback_cpp_heap": "NOT_RUN", "core_cpu_output_integrity": "NOT_RUN"})

    def test_cli_refuses_before_creating_output(self):
        if ARGS is None:
            self.skipTest("fixed-purpose check requires explicit executables and fresh output")
        if ARGS.quality is None:
            self.skipTest("baseline receipt RED only")
        existing = ARGS.output / "existing"
        existing.mkdir()
        marker = existing / "sentinel"
        marker.write_bytes(b"preserve me")
        fresh = ARGS.output / "must-not-exist"
        cases = [[], ["--factor", "1", "--output", str(fresh)],
                 ["--factor", "4", "--output", "relative"],
                 ["--factor", "4", "--output", str(existing)],
                 ["--factor", "4", "--output", str(fresh), "--timings"],
                 ["--factor", "4", "--factor", "4", "--output", str(fresh)],
                 ["--factor", "4", "--output", str(fresh), "--output", str(fresh)],
                 ["--factor", "4", "--output", str(fresh), "--cohort", "alias-32000"],
                 ["--factor", "4", "--output", str(fresh), "--cohort", "alias-44100", "--cohort", "alias-44100"],
                 ["--factor", "4", "--output", str(fresh), "--cohort", "all"]]
        for index, args in enumerate(cases):
            with self.subTest(args=args):
                result = subprocess.run([str(ARGS.quality), *args], capture_output=True)
                (ARGS.output / f"cli-{index}.stdout").write_bytes(result.stdout)
                (ARGS.output / f"cli-{index}.stderr").write_bytes(result.stderr)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(fresh.exists())
                self.assertEqual(marker.read_bytes(), b"preserve me")

    def test_artifact_index_io_error_propagates(self):
        if ARGS is None:
            self.skipTest("fixed-purpose check requires explicit executables and fresh output")
        directory = ARGS.output / "artifact-error"
        result = subprocess.run([str(ARGS.policy), "artifact_error", str(directory)], capture_output=True)
        (ARGS.output / "artifact-error.stdout").write_bytes(result.stdout)
        (ARGS.output / "artifact-error.stderr").write_bytes(result.stderr)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"artifact-index I/O failure propagated from real finish", result.stderr)
        self.assertTrue((directory / "artifact-hashes.csv").is_dir())
        self.assertTrue((directory / "decision.json").is_file())


if __name__ == "__main__":
    ARGS = PARSER.parse_args()
    ARGS.output.mkdir(parents=True, exist_ok=False)
    unittest.main(argv=[__file__], verbosity=2)

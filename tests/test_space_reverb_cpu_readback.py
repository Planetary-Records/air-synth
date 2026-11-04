import array
import importlib.util
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("space_cpu_readback", Path(__file__).resolve().parents[1] / "tools/space_reverb_cpu_readback.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class CpuReadbackTests(unittest.TestCase):
    def make_row(self, root, durations, *, claimed_p99=None):
        values = array.array("d", durations)
        if sys.byteorder != "little":
            values.byteswap()
        timing = root / "row-00-attempt-1.timings.bin"
        timing.write_bytes(values.tobytes())
        stdout = root / "row-00-attempt-1.stdout"
        period = 64 / 44100
        misses = sum(x > period for x in durations)
        mean = math.fsum(durations) / len(durations)
        p99 = sorted(durations)[math.ceil(.99 * len(durations)) - 1]
        passing = misses == 0 and mean <= .5 * period and p99 <= .5 * period
        fields = {"samples": str(len(durations)), "mean_seconds": str(mean), "p99_seconds": str(p99 if claimed_p99 is None else claimed_p99), "max_seconds": str(max(durations)), "period_seconds": str(period), "limit_seconds": str(.5 * period), "misses": str(misses), "warm_seconds": "10", "measure_seconds": "20", "domain": "poleContour", "tanks": "8", "rate": "44100", "block": "64", "allocations": "0", "frees": "0", "overflow": "0", "interrupted": "0", "checksum": "1", "prepared_bytes": "2000000", "pass": "1" if passing else "0"}
        fields.update(case="base-2.000000-brightness-1.000000", size="0.5", backend="sse2")
        fields = {"row": "0", **fields}
        stdout.write_text("CPU_RESULT " + " ".join(f"{k}={v}" for k, v in fields.items()) + "\nCPU_FOOTER row=0 complete=1 acceptance=0\n")
        record = {"row": 0, "attempt": 1, "valid_complete_native_record": True, "exit_code": 0 if passing else 1, "fields": fields, "stdout": stdout.name, "timings": timing.name}
        (root / "row-00.complete.json").write_text(json.dumps(record))
        provenance = {"timings_sha256": audit.sha256(timing), "stdout_sha256": audit.sha256(stdout)}
        (root / "row-00-attempt-1.json").write_text(json.dumps(provenance))

    def test_literal_statistics_and_incomplete_matrix(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_row(root, [1e-5] * 19 + [2e-5])
            result = audit.assess(root)
            self.assertEqual(result["status"], "NOT_PASS")
            self.assertEqual(result["rows"][0]["status"], "PASS")
            self.assertAlmostEqual(result["rows"][0]["mean_seconds"], 1.05e-5)
            self.assertEqual(result["rows"][0]["p99_seconds"], 2e-5)
            self.assertEqual(result["rows"][1]["status"], "INCOMPLETE")

    def test_false_native_percentile_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_row(root, [1e-5] * 19 + [2e-5], claimed_p99=1e-5)
            row = audit.assess(root)["rows"][0]
            self.assertEqual(row["status"], "FAIL")
            self.assertIn("p99_seconds readback", row["errors"])

    def test_one_deadline_miss_fails_even_with_low_mean(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_row(root, [1e-5] * 999 + [.002])
            row = audit.assess(root)["rows"][0]
            self.assertEqual(row["status"], "FAIL")
            self.assertEqual(row["misses"], 1)
            self.assertLess(row["mean_seconds"], .5 * 64 / 44100)

    def test_changed_raw_timing_is_invalid(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_row(root, [1e-5] * 20)
            (root / "row-00-attempt-1.timings.bin").write_bytes(array.array("d", [2e-5] * 20).tobytes())
            self.assertEqual(audit.assess(root)["rows"][0]["status"], "INVALID")

    def test_wrong_curve_is_rejected_despite_valid_timings(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_row(root, [1e-5] * 20)
            path = root / "row-00.complete.json"
            record = json.loads(path.read_text())
            record["fields"]["case"] = "six-alternating"
            path.write_text(json.dumps(record))
            self.assertEqual(audit.assess(root)["rows"][0]["status"], "FAIL")

    def test_controller_claim_cannot_override_original_native_workload(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.make_row(root, [1e-5] * 20)
            stdout = root / "row-00-attempt-1.stdout"
            stdout.write_text(stdout.read_text().replace("case=base-2.000000-brightness-1.000000", "case=six-alternating"))
            path = root / "row-00-attempt-1.json"
            provenance = json.loads(path.read_text())
            provenance["stdout_sha256"] = audit.sha256(stdout)
            path.write_text(json.dumps(provenance))
            self.assertEqual(audit.assess(root)["rows"][0]["status"], "FAIL")


if __name__ == "__main__":
    unittest.main()

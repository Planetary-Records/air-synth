import importlib.util
from pathlib import Path
import tempfile
import unittest


class CiReportTests(unittest.TestCase):
    def setUp(self):
        path = Path(__file__).resolve().parents[1] / "tools" / "ci_report.py"
        spec = importlib.util.spec_from_file_location("ci_report", path)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "junit.xml"

    def read(self, body):
        self.path.write_text(body, encoding="utf-8")
        return self.module.read_junit(self.path)

    def test_includes_waiting_in_successful_tests_and_keeps_failure_exit(self):
        report = self.read('''<testsuite><testcase name="pass" time="2.1"><system-out>
NATIVE_TIMING wait_ms=2000 run_ms=100 exit_code=0 scope=machine
</system-out></testcase><testcase name="fail" time=".3"><failure message="check failed"/><system-out>
NATIVE_TIMING wait_ms=0 run_ms=300 exit_code=7 scope=machine
</system-out></testcase></testsuite>''')
        self.assertEqual((report["passed"], report["failed"], report["skipped"]), (1, 1, 0))
        self.assertEqual(report["aggregate_wait_seconds"], 2)
        self.assertEqual(report["aggregate_run_seconds"], .4)
        self.assertEqual(report["tests"][1]["exit_code"], 7)
        self.assertIn("Aggregate resource waiting", self.module.markdown(report))

    def test_uses_outer_launcher_record_without_double_counting_nested_commands(self):
        report = self.read('''<testsuite><testcase name="nested" time="1.2"><system-out>
NATIVE_TIMING wait_ms=100 run_ms=200 exit_code=0 scope=machine
NATIVE_TIMING wait_ms=400 run_ms=800 exit_code=0 scope=machine
</system-out></testcase></testsuite>''')
        self.assertEqual(report["aggregate_wait_seconds"], .4)
        self.assertEqual(report["aggregate_run_seconds"], .8)

    def test_preserves_signed_windows_crash_status_and_timing(self):
        report = self.read('''<testsuite><testcase name="crash" time=".4"><failure/><system-out>
NATIVE_TIMING wait_ms=100 run_ms=300 exit_code=-1073741819 scope=machine
</system-out></testcase></testsuite>''')
        self.assertEqual(report["tests"][0]["exit_code"], -1073741819)
        self.assertEqual(report["aggregate_wait_seconds"], .1)
        self.assertEqual(report["aggregate_run_seconds"], .3)
        self.assertEqual(report["failed"], 1)

    def test_preserves_unknown_legacy_timing_and_skipped_tests(self):
        report = self.read('''<testsuite><testcase name="legacy" time="75"/>
<testcase name="not-run" time="0"><skipped/></testcase></testsuite>''')
        self.assertEqual(report["unknown_timing_tests"], ["legacy"])
        self.assertIsNone(report["tests"][0]["wait_seconds"])
        self.assertEqual(report["tests"][0]["elapsed_seconds"], 75)
        self.assertEqual(report["skipped"], 1)

    def test_rejects_malformed_and_overlarge_reports(self):
        with self.assertRaises(ValueError):
            self.read("<testsuite>")
        self.path.write_bytes(b" " * (16 * 1024 * 1024 + 1))
        with self.assertRaises(ValueError):
            self.module.read_junit(self.path)


if __name__ == "__main__":
    unittest.main()

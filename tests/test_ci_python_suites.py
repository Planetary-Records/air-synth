import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("ci_python_suites", ROOT / "tools" / "ci_python_suites.py")
suites = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suites)


class PythonSuitesTests(unittest.TestCase):
    def test_parse_result_reads_the_final_unittest_summary(self):
        ok = suites.parse_result("test_a ... ok\n\n----\nRan 3 tests in 0.1s\n\nOK (skipped=2)\n", 0)
        self.assertEqual((ok["ran"], ok["skipped"], ok["ok"]), (3, 2, True))
        bad = suites.parse_result("Ran 4 tests in 1.0s\n\nFAILED (failures=1, errors=2, skipped=1)\n", 1)
        self.assertEqual((bad["failures"], bad["errors"], bad["skipped"], bad["ok"]), (1, 2, 1, False))
        # A module without tests exits 5; discovery reports import crashes as errors with exit 1.
        self.assertTrue(suites.parse_result("\nRan 0 tests in 0.0s\n\nNO TESTS RAN\n", 5)["ok"])
        self.assertFalse(suites.parse_result("ImportError: boom\n", 1)["ok"])

    def test_exit_five_requires_a_genuine_empty_unittest_summary(self):
        for text in ("", "process crashed", "Ran 2 tests in 0.1s\nNO TESTS RAN\n",
                     "Ran 0 tests in 0.1s\nFAILED (errors=1)\n", "Ran 0 tests in 0.1s\n",
                     "Ran 0 tests in 0.1s\nNO TESTS RAN\nRan 3 tests in 0.1s\nFAILED\n"):
            with self.subTest(text=text):
                self.assertFalse(suites.parse_result(text, 5)["ok"])

    def test_success_exit_requires_a_completed_nonempty_unittest_summary(self):
        for text in ("", "Ran 0 tests in 0.0s\n\nOK\n", "Ran 3 tests in 0.1s\n",
                     "Ran 3 tests in 0.1s\n\nFAILED (failures=1)\n", "Ran 3 tests in 0.1s\n\nOK (errors=1)\n"):
            with self.subTest(text=text):
                self.assertFalse(suites.parse_result(text, 0)["ok"])
        self.assertTrue(suites.parse_result("Ran 3 tests in 0.1s\n\nOK (skipped=1)\nprinted stdout\n", 0)["ok"])

    def test_success_exit_without_discovery_does_not_pass_module(self):
        code, out, _ = self.run_fixture({"test_exit": "raise SystemExit(0)\n"})
        self.assertEqual(code, 1, out)
        self.assertIn("failed_modules=test_exit", out)

    def test_actual_empty_discovery_with_buffered_import_stdout(self):
        with tempfile.TemporaryDirectory() as root:
            Path(root, "test_print.py").write_text("print('import output')\n", encoding="utf-8")
            result = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", root],
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.assertEqual(result.returncode, 5)
        self.assertIn("NO TESTS RAN", result.stdout)
        self.assertTrue(suites.parse_result(result.stdout, result.returncode)["ok"])

    def test_running_interpreter_reports_actual_empty_discovery_as_exit_five(self):
        with tempfile.TemporaryDirectory() as root:
            result = subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", root,
                                     "-p", "test_empty.py"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 5)
        self.assertTrue(suites.parse_result(result.stdout + result.stderr, result.returncode)["ok"])

    def test_schedule_starts_longest_modules_first_for_every_interpreter(self):
        plan = suites.schedule({"3.12": "a", "3.14": "b"}, ["test_x", "test_y", "test_z"], {"test_y": 9, "test_z": 3})
        self.assertEqual(plan, [("3.12", "test_y"), ("3.14", "test_y"), ("3.12", "test_z"), ("3.14", "test_z"),
                                ("3.12", "test_x"), ("3.14", "test_x")])

    def run_fixture(self, modules):
        temp = tempfile.TemporaryDirectory(prefix="python suites ")
        self.addCleanup(temp.cleanup)
        root = Path(temp.name)
        (root / "tests").mkdir()
        for name, body in modules.items():
            (root / "tests" / f"{name}.py").write_text(body, encoding="utf-8")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = suites.main(["--source", str(root), "--suite", f"a={sys.executable}", "--suite", f"b={sys.executable}",
                                "--jobs", "2", "--output-root", str(root / "out")])
        return code, out.getvalue(), root

    def test_all_modules_run_for_every_interpreter_and_durations_are_kept(self):
        passing = "import unittest\nclass T(unittest.TestCase):\n    def test_a(self): pass\n    @unittest.skip('x')\n    def test_b(self): pass\n"
        code, out, root = self.run_fixture({"test_one": passing, "test_two": passing, "test_empty": "x = 1\n"})
        self.assertEqual(code, 0, out)
        self.assertIn("PYTHON_SUITE a modules=3 ran=4 skipped=2 failures=0 errors=0 failed_modules=-", out)
        self.assertIn("PYTHON_SUITE b modules=3 ran=4 skipped=2 failures=0 errors=0 failed_modules=-", out)
        durations = json.loads((root / "out" / "python-durations.json").read_text(encoding="utf-8"))
        self.assertEqual(sorted(durations), ["test_empty", "test_one", "test_two"])
        self.assertTrue((root / "out" / "a" / "test_one.log").is_file())

    def test_a_failing_or_broken_module_fails_the_run_and_prints_its_log(self):
        failing = "import unittest\nclass T(unittest.TestCase):\n    def test_a(self): self.fail('MARKER')\n"
        code, out, _ = self.run_fixture({"test_bad": failing, "test_broken": "import missing_module_xyz\n"})
        self.assertEqual(code, 1)
        self.assertIn("failed_modules=test_bad,test_broken", out)
        self.assertIn("MARKER", out)


if __name__ == "__main__":
    unittest.main()

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake")


@unittest.skipUnless(CMAKE, "CMake script mode")
class NativeTestLimitTests(unittest.TestCase):
    def limits(self, table, names):
        with tempfile.TemporaryDirectory(prefix="native limits ") as temp:
            path = Path(temp) / "durations.json"
            if table is not None:
                path.write_text(json.dumps(table), encoding="utf-8")
            script = Path(temp) / "probe.cmake"
            script.write_text(
                f'set(RACKSYNTH_TEST_DURATIONS_FILE "{path.as_posix()}")\n'
                f'include("{(ROOT / "tools" / "NativeTestLimits.cmake").as_posix()}")\n'
                + "".join(f'racksynth_test_run_seconds("{n}" v)\nmessage(STATUS "LIMIT {n}=${{v}}")\n' for n in names),
                encoding="utf-8")
            result = subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        output = result.stdout + result.stderr
        return {n: int(output.split(f"LIMIT {n}=", 1)[1].split()[0]) for n in names}

    def test_measured_tests_get_headroom_between_floor_and_default(self):
        table = {"headroom": 400, "floorSeconds": 120, "tests": {"Quick": 5, "Mid": 50, "Slow": 400, "Odd": 31}}
        self.assertEqual(self.limits(table, ["Quick", "Mid", "Slow", "Odd", "Unmeasured"]),
                         {"Quick": 120, "Mid": 200, "Slow": 600, "Odd": 124, "Unmeasured": 600})

    def test_missing_or_incomplete_table_keeps_the_historical_limit(self):
        self.assertEqual(self.limits(None, ["Any"]), {"Any": 600})
        self.assertEqual(self.limits({"tests": {"Any": 10}}, ["Any"]), {"Any": 600})

    def test_checked_in_table_is_valid_and_registration_uses_it(self):
        table = json.loads((ROOT / "tools" / "ci-test-durations.json").read_text(encoding="utf-8"))
        self.assertTrue(set(table) >= {"headroom", "floorSeconds", "tests"})
        self.assertTrue(all(isinstance(v, int) and v > 0 for v in table["tests"].values()))
        source = (ROOT / "tools" / "NativeTests.cmake").read_text(encoding="utf-8")
        self.assertIn('include("${CMAKE_CURRENT_LIST_DIR}/NativeTestLimits.cmake")', source)
        self.assertIn("--run-seconds ${run_seconds}", source)
        self.assertNotIn("--run-seconds 600", source)

    def test_heavy_cases_share_one_slot_reservation(self):
        limits = (ROOT / "tools" / "NativeTestLimits.cmake").read_text(encoding="utf-8")
        self.assertIn('set(RACKSYNTH_HEAVY_TEST_PROCESSORS 8 CACHE STRING', limits)
        files = [ROOT / "CMakeLists.txt", ROOT / "probes" / "space-reverb" / "CMakeLists.txt"]
        text = "".join(path.read_text(encoding="utf-8") for path in files)
        self.assertNotIn("PROCESSORS 8", text)
        self.assertEqual(text.count("PROCESSORS ${RACKSYNTH_HEAVY_TEST_PROCESSORS}"), 6)


if __name__ == "__main__":
    unittest.main()

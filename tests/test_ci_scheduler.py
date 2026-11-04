"""Exercise the real CTest scheduler: four GUI commands leave headless capacity."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == "win32" and shutil.which("cmake") and shutil.which("ctest"),
                     "Windows CMake/CTest scheduler")
class CiSchedulerTests(unittest.TestCase):
    def test_gui_limit_does_not_consume_the_headless_budget(self):
        repo = Path(__file__).resolve().parents[1]
        native = Path(os.environ.get("AIRACK_NATIVE_RUNNER", "build/native-run/Debug/RackSynthNativeRun.exe")).resolve()
        if not native.is_file():
            self.skipTest("Native coordinator bootstrap")
        with tempfile.TemporaryDirectory(prefix="synth scheduler ") as temp:
            root = Path(temp)
            child = root / "record.py"
            child.write_text('''import json, os, pathlib, sys, time
start = time.perf_counter()
path = pathlib.Path(sys.argv[1])
if path.name.startswith('timing'):
    time.sleep(.2)
else:
    if len(list(path.parent.glob('timing*.json'))) != 3:
        raise RuntimeError('Functional work started before timing checks completed')
    path.with_suffix('.ready').touch()
    release = path.parent / 'release'
    while not release.exists():
        if len(list(path.parent.glob('*.ready'))) >= 12:
            release.touch()
        if time.perf_counter() - start > 20:
            raise RuntimeError('Scheduler did not admit four GUI and eight headless commands')
        time.sleep(.01)
path.with_suffix('.json').write_text(json.dumps({
    'start': start, 'end': time.perf_counter(),
    'allocation': os.environ.get('CTEST_RESOURCE_GROUP_0_GUI')}))
''', encoding="utf-8")
            tests = []
            for group, count in (("timing", 3), ("gui", 5), ("headless", 12)):
                for n in range(count):
                    name = f"{group}{n}"
                    exclusive = "EXCLUSIVE " if group == "timing" else ""
                    tests.append(f'racksynth_add_test(NAME {name} {exclusive}COMMAND "{Path(sys.executable).as_posix()}" '
                                 f'"{child.as_posix()}" "{(root / name).as_posix()}")')
                    if group == "gui":
                        tests.append(f'set_tests_properties({name} PROPERTIES RESOURCE_LOCK desktop COST 100)')
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.22)\nproject(Scheduler NONE)\nenable_testing()\n'
                f'include("{repo.as_posix()}/tools/NativeTests.cmake")\n' + "\n".join(tests) +
                '\nget_directory_property(tests TESTS)\nracksynth_private_desktops(${tests})\n'
                'racksynth_order_timing_tests(${tests})\n', encoding="utf-8")
            configure = subprocess.run(["cmake", "-S", str(root), "-B", str(root / "build"),
                                        "-G", "Visual Studio 17 2022", "-A", "x64"],
                                       capture_output=True, text=True, timeout=60)
            self.assertEqual(configure.returncode, 0, configure.stdout + configure.stderr)
            coordinator = root / "build" / "tools" / "native-run" / "Debug" / "RackSynthNativeRun.exe"
            coordinator.parent.mkdir(parents=True)
            shutil.copyfile(native, coordinator)
            metadata = subprocess.run(["ctest", "--test-dir", str(root / "build"), "-C", "Debug", "--show-only=json-v1"],
                                      capture_output=True, text=True, timeout=15)
            self.assertEqual(metadata.returncode, 0, metadata.stderr)
            registered = json.loads(metadata.stdout)["tests"]
            for test in registered:
                if test["name"].startswith("timing"):
                    properties = {p["name"]: p["value"] for p in test["properties"]}
                    self.assertFalse(properties.get("RUN_SERIAL", False), "Exclusive commands must be queued together")
            # Preserve the production registration; use the existing coordinator and synthetic locks for this fixture.
            generated = root / "build" / "CTestTestfile.cmake"
            command = registered[0]["command"][0]
            content = generated.read_text()
            self.assertIn(f'"{command}"', content)
            generated.write_text(content.replace(f'"{command}"',
                f'"{command}" "--test-lock-file" "{(root / "resources.lock").as_posix()}"'))
            env = os.environ.copy()
            for key in ("AIRACK_NATIVE_LEASE", "AIRACK_NATIVE_DESKTOP", "AIRACK_NATIVE_DESKTOP_HELD",
                        "AIRACK_NATIVE_PRIVATE_DESKTOP", "AIRACK_NATIVE_PRIVATE_DESKTOP_ACTIVE",
                        "AIRACK_NATIVE_REQUIRE_MACHINE", "AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT"):
                env.pop(key, None)
            run = subprocess.run(["ctest", "--test-dir", str(root / "build"), "-C", "Debug", "--parallel", "16",
                                  "--resource-spec-file", str(repo / "tools" / "ci-resources.json"),
                                  "--output-on-failure"], capture_output=True, text=True, timeout=40, env=env)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            events = []
            results = []
            for group, count in (("timing", 3), ("gui", 5), ("headless", 12)):
                for n in range(count):
                    result = json.loads((root / f"{group}{n}.json").read_text())
                    results.append((group, result))
                    if group == "gui":
                        self.assertEqual(result["allocation"], "id:private,slots:1")
                    events += [(result["start"], group, 1), (result["end"], group, -1)]
            active = {"timing": 0, "gui": 0, "headless": 0}
            peak_gui = peak_total = 0
            for _, group, delta in sorted(events):
                active[group] += delta
                if active["timing"]:
                    self.assertEqual(sum(active.values()), 1, "Timing work overlapped another command")
                peak_gui = max(peak_gui, active["gui"])
                peak_total = max(peak_total, sum(active.values()))
            self.assertEqual(peak_gui, 4)
            self.assertGreater(peak_total, 8, "GUI work still exhausts the old eight-slot budget")
            self.assertLessEqual(max(r["end"] for g, r in results if g == "timing"),
                                 min(r["start"] for g, r in results if g != "timing"))
            print(f"CI_SCHEDULER_PEAK gui={peak_gui} total={peak_total}")


if __name__ == "__main__":
    unittest.main()

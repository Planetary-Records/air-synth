"""Process-level checks for worktree-independent native resource coordination."""
import json
import ctypes
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


# Query the actual Win32 desktop, rather than trusting the launcher's environment.
DESKTOP_PROBE = """
import ctypes
from ctypes import wintypes as w
user = ctypes.WinDLL('user32', use_last_error=True)
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
user.GetThreadDesktop.argtypes = [w.DWORD]
user.GetThreadDesktop.restype = w.HANDLE
user.OpenInputDesktop.argtypes = [w.DWORD, w.BOOL, w.DWORD]
user.OpenInputDesktop.restype = w.HANDLE
user.OpenDesktopW.argtypes = [w.LPCWSTR, w.DWORD, w.BOOL, w.DWORD]
user.OpenDesktopW.restype = w.HANDLE
user.CloseDesktop.argtypes = [w.HANDLE]
user.GetUserObjectInformationW.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD, w.LPVOID]
def desktop_name(handle):
    name = ctypes.create_unicode_buffer(256)
    if not user.GetUserObjectInformationW(handle, 2, name, ctypes.sizeof(name), None):
        raise ctypes.WinError(ctypes.get_last_error())
    return name.value
def current_desktop():
    return desktop_name(user.GetThreadDesktop(kernel.GetCurrentThreadId()))
def input_desktop():
    handle = user.OpenInputDesktop(0, False, 1)
    if not handle:
        # Windows can lock or show a secure prompt while unattended CI continues.
        if ctypes.get_last_error() == 5:
            return None
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return desktop_name(handle)
    finally:
        user.CloseDesktop(handle)
"""


@unittest.skipUnless(sys.platform == "win32", "Windows native runner")
class NativeRunTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exe = Path(os.environ.get("AIRACK_NATIVE_RUNNER", "build/native-run/Debug/RackSynthNativeRun.exe")).resolve()
        if not cls.exe.is_file():
            if "AIRACK_NATIVE_RUNNER" in os.environ:
                raise AssertionError(f"Configured native runner is missing: {cls.exe}")
            raise unittest.SkipTest("Native runner regression runs in CTest after the native build")

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rack runner ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.processes = []
        self.addCleanup(self.stop_processes)
        self.env = os.environ.copy()
        for key in ("AIRACK_NATIVE_LEASE", "AIRACK_NATIVE_DESKTOP", "AIRACK_NATIVE_DESKTOP_HELD", "AIRACK_NATIVE_PRIVATE_DESKTOP", "AIRACK_NATIVE_PRIVATE_DESKTOP_ACTIVE", "AIRACK_NATIVE_REQUIRE_MACHINE", "AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT"):
            self.env.pop(key, None)
        # Distinct executable locations and working directories reproduce separate worktrees.
        for name in ("a", "b"):
            folder = self.root / name
            folder.mkdir()
            shutil.copyfile(self.exe, folder / self.exe.name)

    def stop_processes(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)

    def start(self, name, *flags, code=None, args=(), lock_file=None):
        ready, release = self.root / (name + ".ready"), self.root / (name + ".release")
        if code is None:
            code = ("import os,pathlib,time,json; "
                    f"ready=pathlib.Path({str(ready)!r}); temp=ready.with_suffix('.tmp'); "
                    "temp.write_text(json.dumps(dict(os.environ, child_pid=os.getpid()))); temp.replace(ready); "
                    f"release=pathlib.Path({str(release)!r}); "
                    "exec('while not release.exists(): time.sleep(.02)')")
        folder = self.root / ("a" if len(self.processes) % 2 == 0 else "b")
        with (self.root / (name + ".stderr")).open("wb") as errors:
            process = subprocess.Popen([str(folder / self.exe.name), "--test-lock-file", str(lock_file or self.root / "resources.lock"), *flags, "--", sys.executable,
                                        "-c", code, *args], cwd=folder, env=self.env,
                                       stdout=subprocess.DEVNULL, stderr=errors)
        self.processes.append(process)
        return process, ready, release

    def wait_for(self, path):
        deadline = time.monotonic() + 10
        while not path.exists() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertTrue(path.exists(), str(path))

    def release(self, item, exit_code=0):
        process, _, release = item
        release.touch()
        self.assertEqual(process.wait(timeout=10), exit_code)

    def test_waiting_measurement_is_not_starved_by_new_activity(self):
        first = self.start("first")
        self.wait_for(first[1])
        measurement = self.start("waiting", "--exclusive")
        log = self.root / "waiting.stderr"
        deadline = time.monotonic() + 10
        while "activity slot" not in log.read_text() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertIn("activity slot", log.read_text())
        late = self.start("late", "--wait-seconds", "1")
        self.assertEqual(late[0].wait(timeout=10), 124)
        self.assertFalse(late[1].exists())
        self.assertFalse(measurement[1].exists())
        self.release(first)
        self.wait_for(measurement[1])
        self.release(measurement)

    def test_nested_shared_commands_do_not_deadlock_and_cannot_upgrade(self):
        runner = str(self.root / "b" / self.exe.name)
        prefix = [runner, "--test-lock-file", str(self.root / "resources.lock")]
        code = ("import subprocess,sys; "
                f"shared=subprocess.run({prefix!r}+['--desktop','--',sys.executable,'-c','pass']); "
                f"exclusive=subprocess.run({prefix!r}+['--exclusive','--',sys.executable,'-c','pass']); "
                "sys.exit(0 if shared.returncode==0 and exclusive.returncode==125 else 9)")
        item = self.start("nested", "--run-seconds", "5", code=code)
        self.assertEqual(item[0].wait(timeout=10), 0)

    def test_worktrees_share_activity_but_not_diagnostics(self):
        first, second = self.start("one"), self.start("two")
        self.wait_for(first[1])
        self.wait_for(second[1])
        roots = [json.loads(item[1].read_text())["AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT"] for item in (first, second)]
        self.assertNotEqual(*roots)
        for root in roots:
            self.assertTrue(Path(root).is_absolute())
            self.assertTrue(Path(root).is_relative_to(self.root))
        self.assertEqual(json.loads(first[1].read_text())["MSBUILDDISABLENODEREUSE"], "1")
        self.release(first)
        self.release(second)

    def test_desktop_serializes_only_desktop_work(self):
        first = self.start("desktop1", "--desktop")
        self.wait_for(first[1])
        second = self.start("desktop2", "--desktop", "--wait-seconds", "1")
        regular = self.start("regular")
        self.wait_for(regular[1])
        self.assertEqual(second[0].wait(timeout=10), 124)
        self.assertFalse(second[1].exists())
        self.release(first)
        self.release(regular)
        third = self.start("desktop3", "--desktop")
        self.wait_for(third[1])
        self.release(third)

    def private_desktop(self, name, *flags):
        ready, release = self.root / (name + ".ready"), self.root / (name + ".release")
        code = DESKTOP_PROBE + f"""
import os,json,pathlib,time
ready=pathlib.Path({str(ready)!r})
temp=ready.with_suffix('.tmp')
temp.write_text(json.dumps(dict(desktop=current_desktop(), input=input_desktop(), active=os.environ.get('AIRACK_NATIVE_PRIVATE_DESKTOP_ACTIVE'))))
temp.replace(ready)
while not pathlib.Path({str(release)!r}).exists(): time.sleep(.02)
"""
        return self.start(name, "--private-desktop", *flags, code=code)

    def assert_desktop_closed(self, name):
        # Desktop objects must disappear after both the owner and child tree exit.
        scope = {}
        exec(DESKTOP_PROBE, scope)
        deadline = time.monotonic() + 5
        while True:
            handle = scope['user'].OpenDesktopW(name, 0, False, 1)
            if not handle:
                self.assertEqual(ctypes.get_last_error(), 2, "Desktop lookup failed for a reason other than removal")
                break
            scope['user'].CloseDesktop(handle)
            self.assertLess(time.monotonic(), deadline, "Private desktop survived its command")
            time.sleep(.02)

    def test_private_desktops_overlap_physical_work_without_switching_input(self):
        scope = {}
        exec(DESKTOP_PROBE, scope)
        original = scope['current_desktop'](), scope['input_desktop']()
        physical = self.start("physical", "--desktop")
        self.wait_for(physical[1])
        first, second = self.private_desktop("private1"), self.private_desktop("private2")
        for item in (first, second):
            self.wait_for(item[1])
        results = [json.loads(item[1].read_text()) for item in (first, second)]
        self.assertNotEqual(results[0]['desktop'], results[1]['desktop'])
        for result in results:
            self.assertEqual(result['desktop'], result['active'])
            self.assertNotEqual(result['desktop'], original[0])
            if result['input'] is not None and original[1] is not None:
                self.assertEqual(result['input'], original[1])
        self.assertEqual(scope['current_desktop'](), original[0])
        current_input = scope['input_desktop']()
        if current_input is not None and original[1] is not None:
            self.assertEqual(current_input, original[1])
        for item, result in zip((first, second), results):
            self.release(item)
            self.assert_desktop_closed(result['desktop'])
        self.release(physical)

    def test_nested_private_desktop_reuses_actual_desktop(self):
        runner = str(self.root / "b" / self.exe.name)
        prefix = [runner, "--test-lock-file", str(self.root / "resources.lock")]
        child = DESKTOP_PROBE + "print(current_desktop())"
        code = DESKTOP_PROBE + f"""
import subprocess,sys
for flags in ([], ['--private-desktop']):
    result = subprocess.run({prefix!r}+flags+['--',sys.executable,'-c',{child!r}], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == current_desktop(), result.stdout
"""
        item = self.start("private-nested", "--private-desktop", code=code)
        self.assertEqual(item[0].wait(timeout=10), 0, (self.root / "private-nested.stderr").read_text())

    def test_private_desktop_timeout_and_killed_owner_cleanup(self):
        for name, flags in (("private-timeout", ("--run-seconds", "1")), ("private-killed", ())):
            item = self.private_desktop(name, *flags)
            self.wait_for(item[1])
            desktop = json.loads(item[1].read_text())['desktop']
            if not flags:
                item[0].kill()
            self.assertEqual(item[0].wait(timeout=10), 124 if flags else 1)
            self.assert_desktop_closed(desktop)

    def test_private_desktop_rejects_stale_inherited_marker(self):
        scope = {}
        exec(DESKTOP_PROBE, scope)
        for name in ('not-this-desktop', scope['current_desktop']()):
            self.env['AIRACK_NATIVE_PRIVATE_DESKTOP_ACTIVE'] = name
            item = self.start("stale-private")
            self.assertEqual(item[0].wait(timeout=10), 125)
            self.assertFalse(item[1].exists())
            self.assertIn("Inherited private desktop does not match", (self.root / "stale-private.stderr").read_text())

    def test_legacy_hard_link_and_machine_path_share_resource_locks(self):
        legacy = self.root / "legacy-account.lock"
        machine = self.root / "machine.lock"
        legacy.touch()
        first = self.start("legacy-desktop", "--desktop", lock_file=legacy)
        self.wait_for(first[1])
        os.link(legacy, machine)
        desktop = self.start("machine-desktop", "--desktop", "--wait-seconds", "1", lock_file=machine)
        measurement = self.start("machine-measurement", "--exclusive", "--wait-seconds", "1", lock_file=machine)
        for item in (desktop, measurement):
            self.assertEqual(item[0].wait(timeout=10), 124)
            self.assertFalse(item[1].exists())
        self.release(first)
        next_work = self.start("machine-after-release", "--exclusive", lock_file=machine)
        self.wait_for(next_work[1])
        self.release(next_work)

    def test_resource_inventory_reports_scope_and_canonical_default_path(self):
        result = subprocess.run([self.exe, "--resource-info"], env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = result.stdout.splitlines()
        self.assertIn(lines[0], ("NATIVE_RESOURCES scope=machine", "NATIVE_RESOURCES scope=account"))
        path = Path(lines[1])
        self.assertTrue(path.is_absolute())
        self.assertEqual(path.name, "native-resources-v1.lock")

    def test_measurement_waits_for_activity_and_blocks_new_work(self):
        first = self.start("activity")
        self.wait_for(first[1])
        refused = self.start("no-measurement", "--exclusive", "--wait-seconds", "1")
        self.assertEqual(refused[0].wait(timeout=10), 124)
        self.assertFalse(refused[1].exists())
        self.release(first)
        measurement = self.start("measurement", "--exclusive")
        self.wait_for(measurement[1])
        blocked = self.start("no-activity", "--wait-seconds", "1")
        self.assertEqual(blocked[0].wait(timeout=10), 124)
        self.assertFalse(blocked[1].exists())
        self.release(measurement)
        next_work = self.start("next")
        self.wait_for(next_work[1])
        self.release(next_work)

    def test_killed_owner_releases_locks_and_kills_its_child(self):
        descendant = self.root / "descendant.pid"
        child_code = (f"import os,pathlib,time; target=pathlib.Path({str(descendant)!r}); "
                      "temp=target.with_suffix('.tmp'); temp.write_text(str(os.getpid())); temp.replace(target); time.sleep(60)")
        ready = self.root / "owner.ready"
        code = ("import subprocess,sys,os,json,pathlib,time; "
                f"subprocess.Popen([sys.executable,'-c',{child_code!r}]); "
                f"target=pathlib.Path({str(ready)!r}); temp=target.with_suffix('.tmp'); "
                "temp.write_text(json.dumps(dict(child_pid=os.getpid()))); temp.replace(target); time.sleep(60)")
        item = self.start("owner", "--exclusive", code=code)
        self.wait_for(item[1])
        self.wait_for(descendant)
        pids = [json.loads(item[1].read_text())["child_pid"], int(descendant.read_text())]
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_ulong)
        kernel.CloseHandle.argtypes = (ctypes.c_void_p,)
        handles = [kernel.OpenProcess(0x100000, False, pid) for pid in pids]
        self.assertTrue(all(handles))
        for handle in handles:
            self.addCleanup(kernel.CloseHandle, handle)
        item[0].kill()
        item[0].wait(timeout=10)
        for handle in handles:
            self.assertEqual(kernel.WaitForSingleObject(handle, 10000), 0, "Owned child tree survived its launcher")
        next_work = self.start("after-kill", "--exclusive", "--wait-seconds", "3")
        self.wait_for(next_work[1])
        self.release(next_work)

    def test_arguments_exit_code_and_explicit_diagnostics_survive(self):
        self.env["AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT"] = str(self.root / "explicit")
        output = self.root / "arguments.json"
        code = ("import sys,os,pathlib,json; "
                f"pathlib.Path({str(output)!r}).write_text(json.dumps([sys.argv[1:],os.environ['AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT']])); "
                "sys.exit(7)")
        arguments = ('space here', 'embedded"quote', 'trailing space \\', '', '$literal;unchanged')
        item = self.start("args", code=code, args=arguments)
        self.assertEqual(item[0].wait(timeout=10), 7)
        self.assertEqual(json.loads(output.read_text()), [list(arguments), self.env["AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT"]])
        timing = (self.root / "args.stderr").read_text()
        self.assertRegex(timing, r"NATIVE_TIMING wait_ms=\d+ run_ms=\d+ exit_code=7")

    def test_timing_separates_resource_wait_from_execution(self):
        first = self.start("timing-owner", "--desktop")
        self.wait_for(first[1])
        second = self.start("timing-waiter", "--desktop")
        errors = self.root / "timing-waiter.stderr"
        deadline = time.monotonic() + 10
        while "desktop slot" not in errors.read_text() and time.monotonic() < deadline:
            time.sleep(.02)
        self.assertIn("desktop slot", errors.read_text())
        time.sleep(.15)
        self.assertFalse(second[1].exists())
        self.release(first)
        self.wait_for(second[1])
        time.sleep(.08)
        self.release(second)
        match = re.search(r"NATIVE_TIMING wait_ms=(\d+) run_ms=(\d+) exit_code=0", errors.read_text())
        self.assertIsNotNone(match, errors.read_text())
        self.assertGreaterEqual(int(match[1]), 150)
        self.assertGreaterEqual(int(match[2]), 80)

    def test_python_launcher_accepts_version_selector(self):
        launcher = shutil.which("py")
        if launcher is None:
            self.skipTest("Optional Windows Python launcher is not installed")
        runner = self.root / "a" / self.exe.name
        version = f"-{sys.version_info.major}.{sys.version_info.minor}"
        result = subprocess.run([runner, "--test-lock-file", self.root / "resources.lock", "--", launcher,
                                 version, "-c", "import sys; print(sys.version_info[:2])"],
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), str(sys.version_info[:2]))

    def test_runtime_deadline_releases_resources(self):
        item = self.start("timeout", "--exclusive", "--run-seconds", "1")
        self.assertEqual(item[0].wait(timeout=10), 124)
        next_work = self.start("after-timeout", "--exclusive", "--wait-seconds", "3")
        self.wait_for(next_work[1])
        self.release(next_work)


if __name__ == "__main__":
    unittest.main()

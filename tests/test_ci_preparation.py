"""Preparation keeps one shared admission and returns only a fresh state receipt."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


@unittest.skipUnless(sys.platform == "win32" and shutil.which("pwsh"), "Windows CI preparation")
class CiPreparationTests(unittest.TestCase):
    def setUp(self):
        self.native = Path(os.environ.get("AIRACK_NATIVE_RUNNER", "build/native-run/Debug/RackSynthNativeRun.exe")).resolve()
        if not self.native.is_file():
            self.skipTest("Native coordinator bootstrap")
        self.temp = tempfile.TemporaryDirectory(prefix="synth preparation ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.output = self.root / "reports"
        self.lock = self.root / "resources.lock"
        self.helper = Path(__file__).resolve().parents[1] / "tools" / "Invoke-CiPreparation.ps1"
        self.env = os.environ.copy()
        for key in ("AIRACK_NATIVE_LEASE", "AIRACK_NATIVE_DESKTOP", "AIRACK_NATIVE_DESKTOP_HELD",
                    "AIRACK_NATIVE_PRIVATE_DESKTOP", "AIRACK_NATIVE_PRIVATE_DESKTOP_ACTIVE",
                    "AIRACK_NATIVE_REQUIRE_MACHINE", "AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT"):
            self.env.pop(key, None)
        self.runner = self.root / "native.cmd"
        self.runner.write_text(f'@echo off\n"{self.native}" --test-lock-file "{self.lock}" %*\nexit /b %errorlevel%\n')
        self.child = self.root / "step.py"
        self.child.write_text('''import json, os, pathlib, sys, time
p = pathlib.Path(sys.argv[1]); n = sys.argv[2]
(p / (n + '.started')).write_text(os.environ.get('AIRACK_NATIVE_LEASE', 'missing'))
deadline = time.monotonic() + 10
while n == 'one' and not (p / 'continue').exists():
    if time.monotonic() > deadline:
        raise RuntimeError('Exclusive contender never queued')
    time.sleep(.01)
time.sleep(.2)
(p / (n + '.done')).write_text(str(time.perf_counter()))
''')
        self.payload = self.root / "prepare.ps1"
        self.payload.write_text('''param($PrepareNonce,$Python312,$Python314,$Pluginval,$OutputRoot,[switch]$RequireMachineResources)
if ($env:TEST_FAIL -eq '1') { exit 7 }
foreach ($n in @('one','two','three')) {
    & $env:TEST_NATIVE -- $Python312 $env:TEST_CHILD $OutputRoot $n
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
@{ nonce=$PrepareNonce;source=$env:TEST_SOURCE;python312=$Python312;python314=$Python314 } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputRoot 'prepare.json')
Write-Output 'PREPARATION fixture complete'
''')
        self.harness = self.root / "harness.ps1"
        self.harness.write_text('''$ErrorActionPreference='Stop'
$state = & $env:TEST_HELPER -NativeRunner $env:TEST_NATIVE -Script $env:TEST_PAYLOAD -Source $env:TEST_SOURCE `
    -Python312 $env:TEST_PYTHON -Python314 $env:TEST_PYTHON -Pluginval $env:TEST_CHILD -OutputRoot $env:TEST_OUTPUT `
    -RequireMachineResources:($env:TEST_REQUIRE_MACHINE -eq '1')
@{ type=$state.GetType().Name;lease=$env:AIRACK_NATIVE_LEASE;python312=$state.python312 } |
    ConvertTo-Json -Compress | Set-Content -LiteralPath (Join-Path $env:TEST_OUTPUT 'returned.json')
''')
        self.env.update(TEST_HELPER=str(self.helper), TEST_NATIVE=str(self.runner), TEST_PAYLOAD=str(self.payload),
                        TEST_SOURCE=str(self.root), TEST_PYTHON=sys.executable, TEST_CHILD=str(self.child),
                        TEST_OUTPUT=str(self.output))

    def command(self):
        return ["pwsh", "-NoProfile", "-NonInteractive", "-File", str(self.harness)]

    def test_keeps_nested_commands_together_and_releases_before_test_phase(self):
        harness = subprocess.Popen(self.command(), cwd=self.root, env=self.env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        contender = None
        try:
            deadline = time.monotonic() + 15
            while not (self.output / "one.started").exists() and harness.poll() is None and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue((self.output / "one.started").exists(), "Preparation never started")
            exclusive = self.root / "exclusive.txt"
            contender = subprocess.Popen([str(self.native), "--test-lock-file", str(self.lock), "--exclusive",
                "--run-seconds", "10", "--", sys.executable, "-c",
                "import pathlib,sys,time; pathlib.Path(sys.argv[1]).write_text(str(time.perf_counter()))", str(exclusive)],
                cwd=self.root, env=self.env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True)
            self.assertIn("Waiting for native activity slot", contender.stderr.readline())
            (self.output / "continue").touch()
            stdout, stderr = harness.communicate(timeout=20)
            self.assertEqual(harness.returncode, 0, stdout + stderr)
            _, error = contender.communicate(timeout=15)
            self.assertEqual(contender.returncode, 0, error)
            for n in ("one", "two", "three"):
                self.assertEqual((self.output / f"{n}.started").read_text(), "shared")
                self.assertLess(float((self.output / f"{n}.done").read_text()), float(exclusive.read_text()))
            result = json.loads((self.output / "returned.json").read_text(encoding="utf-8-sig"))
            self.assertEqual(result["type"], "PSCustomObject")
            self.assertFalse(result["lease"], "Preparation leaked its lease into the caller's test phase")
            self.assertEqual(Path(result["python312"]).resolve(), Path(sys.executable).resolve())
        finally:
            for process in (harness, contender):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.communicate(timeout=15)

    def test_zero_exit_without_preparation_refuses_stale_receipt(self):
        self.runner.write_text("@exit /b 0\n")
        self.output.mkdir()
        (self.output / "prepare.json").write_text(json.dumps(dict(nonce="0" * 32, source=str(self.root),
            python312=sys.executable, python314=sys.executable)))
        result = subprocess.run(self.command(), cwd=self.root, env=self.env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("preparation receipt", result.stderr.lower())
        self.assertFalse((self.output / "returned.json").exists())

    def test_preparation_failure_never_returns_success_state(self):
        self.env["TEST_FAIL"] = "1"
        result = subprocess.run(self.command(), cwd=self.root, env=self.env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CI preparation failed", result.stderr)
        self.assertFalse((self.output / "returned.json").exists())

    def test_machine_requirement_applies_to_the_outer_admission(self):
        self.env["TEST_REQUIRE_MACHINE"] = "1"
        result = subprocess.run(self.command(), cwd=self.root, env=self.env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Machine resources cannot use a test lock file", result.stderr)
        self.assertFalse((self.output / "one.started").exists())


if __name__ == "__main__":
    unittest.main()

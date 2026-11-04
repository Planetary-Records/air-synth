"""Read-only discovery contracts, using real temporary installation trees."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1] / "tools" / "native_preflight.py"


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SOURCE.is_file(), "The preflight implementation is missing")
        spec = importlib.util.spec_from_file_location("native_preflight", SOURCE)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.root = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        self.environment = {
            "ProgramFiles": str(self.root / "Program Files"),
            "ProgramFiles(x86)": str(self.root / "Program Files (x86)"),
            "ProgramData": str(self.root / "ProgramData"),
            "LOCALAPPDATA": str(self.root / "Local"),
            "PATH": str(self.root / "bin"),
        }
        self.stack.enter_context(patch.dict(os.environ, self.environment, clear=True))
        self.stack.enter_context(patch.object(self.module.sys, "platform", "win32"))
        self.paths = {}
        self.stack.enter_context(patch.object(self.module.shutil, "which", side_effect=lambda name: self.paths.get(name)))
        self.registry = self.stack.enter_context(patch.object(self.module, "_read_ableton_uninstall", return_value=[]))
        self.process = self.stack.enter_context(patch.object(self.module.subprocess, "run", side_effect=AssertionError("Unexpected execution")))

    def file(self, relative):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"fixture")
        return path

    def vswhere(self, stdout="[]", returncode=0):
        self.paths["vswhere"] = str(self.file("bin/vswhere.exe"))
        self.process.side_effect = None
        self.process.return_value = subprocess.CompletedProcess([], returncode, stdout, "")

    def toolchain(self):
        self.paths["cmake"] = str(self.file("bin/cmake.exe"))
        install = self.root / "Program Files/Microsoft Visual Studio/2022/Community"
        base = install.relative_to(self.root).as_posix() + "/VC/Tools/MSVC/14.44.35207/"
        for name in ("bin/Hostx64/x64/cl.exe", "bin/Hostx64/x64/link.exe", "include/vector", "lib/x64/libcmt.lib"):
            self.file(base + name)
        sdk = "Program Files (x86)/Windows Kits/10/"
        for name in ("Include/10.0.26100.0/um/Windows.h", "Include/10.0.26100.0/shared/sdkddkver.h", "Include/10.0.26100.0/ucrt/stdio.h", "Lib/10.0.26100.0/um/x64/kernel32.lib", "Lib/10.0.26100.0/ucrt/x64/ucrt.lib", "bin/10.0.26100.0/x64/rc.exe", "bin/10.0.26100.0/x64/mt.exe"):
            self.file(sdk + name)
        return install

    def test_missing_compiler_and_sdk_are_not_probe_ready(self):
        self.paths["cmake"] = str(self.file("bin/cmake.exe"))
        report = self.module.collect_environment()
        self.assertFalse(report["gates"]["native_probe_ready"])
        self.assertEqual(report["tools"]["msvc"]["status"], "missing")
        self.assertEqual(report["tools"]["windows_sdk"]["status"], "missing")

    def test_empty_partial_and_malformed_vs_results_cannot_create_readiness(self):
        for value in ("[]", "", "{oops", "{}", '[{}]', '[{"installationPath":"missing","isComplete":true}]'):
            with self.subTest(value=value):
                self.vswhere(value)
                report = self.module.collect_environment()
                self.assertFalse(report["gates"]["native_probe_ready"])
                self.assertTrue(report["diagnostics"])

    def test_metadata_without_real_toolchain_is_not_ready(self):
        self.vswhere(json.dumps([{"installationPath": str(self.root / "absent"), "installationVersion": "17.14.1", "isComplete": True, "isLaunchable": True}]))
        self.assertFalse(self.module.collect_environment()["gates"]["native_probe_ready"])

    def test_empty_or_partial_vswhere_does_not_certify_files_left_on_disk(self):
        install = self.toolchain()
        for instances in ([], [{}], [{"installationPath": str(install), "installationVersion": "17.14.1", "isComplete": False, "isLaunchable": True}]):
            with self.subTest(instances=instances):
                self.vswhere(json.dumps(instances))
                self.assertFalse(self.module.collect_environment()["gates"]["native_probe_ready"])

    def test_complete_path_toolchain_can_be_discovered_outside_known_locations(self):
        install = self.toolchain()
        custom = self.root / "custom-vs"
        install.rename(custom)
        self.paths["cl"] = str(custom / "VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe")
        self.assertTrue(self.module.collect_environment()["gates"]["native_probe_ready"])

    def test_sdk_without_resource_and_manifest_tools_is_not_probe_ready(self):
        self.toolchain()
        for name in ("rc.exe", "mt.exe"):
            path = self.root / "Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" / name
            path.unlink()
            self.assertFalse(self.module.collect_environment()["gates"]["native_probe_ready"])
            path.write_bytes(b"fixture")

    def test_known_complete_install_is_only_probe_ready(self):
        self.toolchain()
        report = self.module.collect_environment()
        self.assertTrue(report["gates"]["native_probe_ready"])
        self.assertFalse(report["gates"]["native_build_verified"])
        self.assertFalse(report["gates"]["ableton_host_verified"])

    def test_partial_sdk_or_msvc_fails_probe_readiness(self):
        install = self.toolchain()
        sdk_file = self.root / "Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/ucrt/x64/ucrt.lib"
        sdk_file.unlink()
        self.assertFalse(self.module.collect_environment()["gates"]["native_probe_ready"])
        sdk_file.write_bytes(b"fixture")
        (install / "VC/Tools/MSVC/14.44.35207/lib/x64/libcmt.lib").unlink()
        self.assertFalse(self.module.collect_environment()["gates"]["native_probe_ready"])

    def test_vswhere_custom_location_checks_actual_files(self):
        install = self.toolchain()
        custom = self.root / "custom-vs"
        install.rename(custom)
        self.vswhere(json.dumps([{"installationPath": str(custom), "installationVersion": "17.14.1", "isComplete": True, "isLaunchable": True}]))
        report = self.module.collect_environment()
        self.assertTrue(report["gates"]["native_probe_ready"])
        self.assertIn(str(custom), report["tools"]["msvc"]["paths"][0])
        args, kwargs = self.process.call_args
        self.assertIsInstance(args[0], list)
        self.assertGreater(kwargs["timeout"], 0)
        self.assertLessEqual(kwargs["timeout"], 15)
        self.assertFalse(kwargs.get("shell", False))

    def test_download_and_unrelated_live_executable_are_not_installations(self):
        self.file("Downloads/ableton_live_12_suite.zip")
        self.file("ProgramData/Ableton/Live 12 Suite/Program/Live.exe")
        self.registry.return_value = [{"DisplayName": "Unrelated Live", "Publisher": "Other", "InstallLocation": str(self.root / "ProgramData/Ableton/Live 12 Suite")}]
        self.assertEqual(self.module.collect_environment()["ableton"]["status"], "missing")

    def test_known_live_install_is_detected_but_not_host_verified(self):
        executable = self.file("ProgramData/Ableton/Live 12 Suite/Program/Ableton Live 12 Suite.exe")
        report = self.module.collect_environment()
        self.assertEqual(report["ableton"]["status"], "found")
        self.assertIn(str(executable), report["ableton"]["paths"])
        self.assertFalse(report["gates"]["ableton_host_verified"])

    def test_uninstall_metadata_requires_existing_executable(self):
        location = self.root / "custom-live"
        self.registry.return_value = [{"DisplayName": "Ableton Live 12 Suite", "Publisher": "Ableton", "DisplayVersion": "12.3.1", "InstallLocation": str(location)}]
        self.assertEqual(self.module.collect_environment()["ableton"]["status"], "missing")
        executable = self.file("custom-live/Program/Ableton Live 12 Suite.exe")
        report = self.module.collect_environment()
        self.assertIn(str(executable), report["ableton"]["paths"])
        self.assertEqual(report["ableton"]["installations"][0]["version"], "12.3.1")

    def test_cli_writes_json_without_environment_or_installation_changes(self):
        self.toolchain()
        before_environment = dict(os.environ)
        before_files = {str(p): p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        output = self.root / "report.json"
        self.assertEqual(self.module.main(["--output", str(output)]), 0)
        report = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(report["schema_version"], 1)
        self.assertEqual(set(report), {"schema_version", "platform", "tools", "ableton", "gates", "diagnostics"})
        self.assertEqual(dict(os.environ), before_environment)
        self.assertEqual({str(p): p.read_bytes() for p in self.root.rglob("*") if p.is_file() and p != output}, before_files)

    def test_missing_dependencies_are_successful_inventory(self):
        self.assertEqual(self.module.main(["--output", str(self.root / "report.json")]), 0)

    def test_discovery_failures_produce_diagnostics_and_nonzero_exit(self):
        for failure in (subprocess.CompletedProcess([], 5, "", "secret should not be copied"), subprocess.TimeoutExpired(["vswhere"], 10), OSError("execution denied")):
            with self.subTest(failure=failure):
                self.vswhere()
                if isinstance(failure, Exception):
                    self.process.side_effect = failure
                else:
                    self.process.return_value = failure
                output = self.root / "failed.json"
                self.assertNotEqual(self.module.main(["--output", str(output)]), 0)
                report = json.loads(output.read_text(encoding="utf-8"))
                self.assertTrue(any(d["severity"] == "error" for d in report["diagnostics"]))
                self.assertFalse(report["gates"]["native_probe_ready"])
                self.assertNotIn("secret should not be copied", output.read_text())

    def test_malformed_vswhere_json_has_operational_exit(self):
        self.vswhere("{oops")
        self.assertNotEqual(self.module.main(["--output", str(self.root / "bad.json")]), 0)

    def test_unsupported_platform_and_invalid_output_have_explicit_errors(self):
        with patch.object(self.module.sys, "platform", "linux"):
            output = self.root / "unsupported.json"
            self.assertNotEqual(self.module.main(["--output", str(output)]), 0)
            self.assertIn("unsupported_platform", [d["code"] for d in json.loads(output.read_text())["diagnostics"]])
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            self.assertNotEqual(self.module.main(["--output", str(self.root)]), 0)
        self.assertIn("output_error", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()

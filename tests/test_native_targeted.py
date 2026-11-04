"""Focused runner trust boundaries; fixtures are synthetic, not native acceptance."""
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools/native_targeted.py"
targeted = None
if SCRIPT.exists():
    sys.path.insert(0, str(ROOT / "tools"))
    spec = importlib.util.spec_from_file_location("native_targeted", SCRIPT)
    targeted = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(targeted)


class NativeTargetedTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(targeted, "The targeted native runner is not implemented")
        self.temp = tempfile.TemporaryDirectory(prefix="focused native ")
        self.addCleanup(self.temp.cleanup)
        self.source = Path(self.temp.name)
        self.build = self.source / "build/windows-debug"
        self.build.mkdir(parents=True)
        self.reply = self.build / ".cmake/api/v1/reply"
        self.reply.mkdir(parents=True)
        self.exe = self.build / "test/Debug/check.exe"
        self.launcher = self.build / "tools/native-run/Debug/RackSynthNativeRun.exe"
        self.cache = {
            "CMAKE_HOME_DIRECTORY": str(self.source), "CMAKE_CACHEFILE_DIR": str(self.build),
            "CMAKE_CONFIGURATION_TYPES": "Debug;Release", "CMAKE_COMMAND": sys.executable,
        }

    def test_cache_refuses_wrong_source_build_or_configuration(self):
        targeted.validate_cache(self.cache, self.source, self.build, "Debug")
        for key, value in [("CMAKE_HOME_DIRECTORY", str(self.source / "other")),
                           ("CMAKE_CACHEFILE_DIR", str(self.build / "other")),
                           ("CMAKE_CONFIGURATION_TYPES", "Release")]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                targeted.validate_cache(dict(self.cache, **{key: value}), self.source, self.build, "Debug")
        with self.assertRaises(ValueError):
            targeted.validate_cache(self.cache, self.source, self.source / "other", "Debug")
        with self.assertRaises(ValueError):
            targeted.validate_cache(self.cache, self.source, self.build, "debug")

    def test_selector_requires_nonempty_valid_exact_inventory(self):
        tests = [self.entry("check.a+1"), self.entry("other")]
        selected = targeted.select_tests(tests, [tests[0]], "Debug")
        self.assertEqual([t["name"] for t in selected], ["check.a+1"])
        pattern = targeted.exact_pattern(selected)
        self.assertIsNotNone(re.fullmatch(pattern, "check.a+1"))
        self.assertIsNone(re.fullmatch(pattern, "checkXa11"))
        for inventory, scope in [(tests, []), (tests, [self.entry("unknown")]),
                                  (tests, [dict(tests[0], config="Release")]),
                                  (tests + [tests[0]], [tests[0]])]:
            with self.subTest(scope=scope), self.assertRaises(ValueError):
                targeted.select_tests(inventory, scope, "Debug")
        with self.assertRaises(ValueError):
            targeted.validate_pattern("[")
        with self.assertRaises(ValueError):
            targeted.validate_pattern("")

    def test_ctest_compacted_backtrace_index_does_not_change_command_identity(self):
        inventory = dict(self.entry(), backtrace=566)
        filtered = dict(inventory, backtrace=2)
        selected = targeted.select_tests([inventory], [filtered], "Debug")
        self.assertEqual(selected[0]["command"], inventory["command"])
        self.assertEqual(selected[0]["properties"], inventory["properties"])
        self.assertEqual(selected, targeted.select_tests([inventory], [inventory], "Debug"))
        with self.assertRaises(ValueError):
            targeted.select_tests([inventory], [dict(filtered, command=["changed.exe"])], "Debug")

    def test_timing_and_physical_desktop_need_opt_in_private_desktop_does_not(self):
        timing = self.entry(properties=[{"name": "LABELS", "value": ["timing"]}])
        physical = self.entry(command=[str(self.launcher), "--desktop", "--", str(self.exe)])
        for entry in (timing, physical):
            with self.assertRaises(ValueError):
                targeted.select_tests([entry], [entry], "Debug")
        targeted.select_tests([timing], [timing], "Debug", allow_timing=True)
        targeted.select_tests([physical], [physical], "Debug", allow_physical_desktop=True)
        for separator in (["--"], []):
            for flag, opt_in in [("--exclusive", "allow_timing"), ("--desktop", "allow_physical_desktop")]:
                entry = self.entry(command=[str(self.launcher), flag, *separator, str(self.exe)])
                with self.subTest(separator=separator, flag=flag):
                    with self.assertRaises(ValueError):
                        targeted.select_tests([entry], [entry], "Debug")
                    self.assertEqual(targeted.select_tests([entry], [entry], "Debug", **{opt_in: True}), [entry])
        for property_name, value, opt_in in [("LABELS", "timing", "allow_timing"),
                                             ("RESOURCE_LOCK", "desktop", "allow_physical_desktop")]:
            entry = self.entry(command=[str(self.launcher), "--", str(self.exe), "--desktop", "--exclusive"],
                               properties=[{"name": "ENVIRONMENT", "value": ["AIRACK_NATIVE_PRIVATE_DESKTOP=1"]},
                                           {"name": property_name, "value": [value]}])
            with self.subTest(property_name=property_name):
                with self.assertRaises(ValueError):
                    targeted.select_tests([entry], [entry], "Debug")
                self.assertEqual(targeted.select_tests([entry], [entry], "Debug", **{opt_in: True}), [entry])
        private = self.entry(properties=[{"name": "ENVIRONMENT", "value": ["AIRACK_NATIVE_PRIVATE_DESKTOP=1"]}])
        targeted.select_tests([private], [private], "Debug")
        disabled = self.entry(properties=[{"name": "DISABLED", "value": True}])
        with self.assertRaises(ValueError):
            targeted.select_tests([disabled], [disabled], "Debug")

    def test_private_child_flags_after_first_launcher_separator_do_not_need_runner_opt_ins(self):
        for child_flags in (["--desktop"], ["--exclusive"], ["--", "--desktop", "--exclusive"]):
            entry = self.entry("RackSynthTileArrayTests_desktop",
                               command=[str(self.launcher), "--run-seconds", "600", "--", str(self.exe), *child_flags],
                               properties=[{"name": "ENVIRONMENT", "value": ["AIRACK_NATIVE_PRIVATE_DESKTOP=1"]}])
            with self.subTest(child_flags=child_flags):
                self.assertEqual(targeted.select_tests([entry], [entry], "Debug"), [entry])

    def test_file_api_artifacts_are_bound_to_current_source_build_config_and_generation(self):
        index = self.file_api()
        artifacts = targeted.load_artifacts(index, self.source, self.build, "Debug", 0)
        self.assertEqual(artifacts[str(self.exe.resolve()).casefold()], ["Check"])
        for source, build, config, started in [(self.source / "other", self.build, "Debug", 0),
                                               (self.source, self.build / "other", "Debug", 0),
                                               (self.source, self.build, "Release", 0),
                                               (self.source, self.build, "Debug", time.time_ns() + 10**10)]:
            with self.subTest(config=config, source=source), self.assertRaises(ValueError):
                targeted.load_artifacts(index, source, build, config, started)

    def test_python_wrapper_builds_both_native_arguments_and_environment_executable(self):
        actual = self.build / "actual/Debug/actual.exe"
        reference = self.build / "reference/Debug/reference.exe"
        helper = self.build / "helper/Debug/helper.exe"
        artifacts = {str(p.resolve()).casefold(): [name] for p, name in
                     [(self.launcher, "RackSynthNativeRun"), (actual, "Actual"),
                      (reference, "Reference"), (helper, "Helper")]}
        entry = self.entry(command=[str(self.launcher), "--run-seconds", "600", "--", sys.executable,
                                         str(self.source / "tools/compare.py"), "--actual", str(actual),
                                         "--reference=" + str(reference)],
                                properties=[{"name": "ENVIRONMENT", "value": ["HELPER=" + str(helper)]}])
        result = targeted.infer_targets([entry], artifacts, self.build, [Path(sys.executable)])
        self.assertEqual(result, ["Actual", "Helper", "RackSynthNativeRun", "Reference"])

    def test_unknown_or_ambiguous_build_executable_and_bundle_directory_refuse(self):
        artifacts = {str(self.launcher.resolve()).casefold(): ["RackSynthNativeRun"],
                     str(self.exe.resolve()).casefold(): ["Check"]}
        targeted.infer_targets([self.entry()], artifacts, self.build, [])
        for command in [[str(self.build / "unmapped.exe")], [str(self.exe.parent)],
                        [str(self.exe), str(self.build / "unresolved.vst3")],
                        [str(self.exe), str(self.build / "runtime-fixture.json")]]:
            with self.subTest(command=command), self.assertRaises(ValueError):
                targeted.infer_targets([self.entry(command=command)], artifacts, self.build, [])
        artifacts[str(self.exe.resolve()).casefold()] = ["Check", "Other"]
        with self.assertRaises(ValueError):
            targeted.infer_targets([self.entry()], artifacts, self.build, [])

    def test_environment_modification_preserves_windows_drive_path(self):
        artifacts = {str(self.exe.resolve()).casefold(): ["Check"]}
        entry = self.entry(command=[sys.executable], properties=[{"name": "ENVIRONMENT_MODIFICATION",
                            "value": ["HELPER=set:" + str(self.exe)]}])
        self.assertEqual(targeted.infer_targets([entry], artifacts, self.build, [Path(sys.executable)]), ["Check"])

    def test_source_identity_hashes_staged_only_changes(self):
        (self.source / "staged.cpp").write_text("new bytes", encoding="utf-8")
        def git(argv):
            if "diff" in argv:
                return b"staged.cpp\0"
            if "ls-files" in argv:
                return b""
            if "rev-parse" in argv:
                return b"abc123\n"
            return b"M  staged.cpp\0"
        with patch.object(targeted.subprocess, "check_output", side_effect=git):
            identity = targeted.source_identity(self.source)
        self.assertIn("staged.cpp", identity["dirtyFiles"])
        self.assertIsNotNone(identity["dirtyFiles"]["staged.cpp"])

    def test_same_build_lock_refuses_second_writer_and_releases(self):
        with targeted.writer(self.build):
            with self.assertRaises(OSError):
                with targeted.writer(self.build):
                    self.fail("Second writer admitted")
        with targeted.writer(self.build):
            pass

    def test_execute_builds_multi_selection_and_validates_exact_junit(self):
        self.file_api()
        self.launcher.parent.mkdir(parents=True)
        self.launcher.write_bytes(b"synthetic coordinator fixture")
        cache = self.build / "CMakeCache.txt"
        cache.write_text("\n".join(k + ":STRING=" + v for k, v in self.cache.items()), encoding="utf-8")
        calls = []
        # Tool processes are the external boundary; selection/build/result handling stays real.
        def command(argv, log, source, state):
            calls.append(list(map(str, argv)))
            text = ""
            if "--resource-info" in argv:
                text = "NATIVE_RESOURCES scope=machine\n"
            elif "--show-only=json-v1" in argv:
                text = json.dumps({"tests": [self.entry("z-last"), self.entry("a-first")]})
            elif "--output-junit" in argv:
                junit = Path(argv[argv.index("--output-junit") + 1])
                junit.write_text('<testsuite><testcase name="z-last" status="run"/>'
                                 '<testcase name="a-first" status="run"/></testsuite>', encoding="utf-8")
                # The synthetic tool has no process duration; stamp its output with the precise clock.
                stamp = time.time_ns()
                os.utime(junit, ns=(stamp, stamp))
            elif "-S" in argv:
                self.file_api()
            log.write_text(text, encoding="utf-8")
            return text
        actual_load = targeted.load_artifacts
        def artifacts(*args):
            result = actual_load(*args)
            result[str(self.launcher.resolve()).casefold()] = ["RackSynthNativeRun"]
            return result
        args = targeted.argparse.Namespace(source=self.source, build_root=Path("build/windows-debug"),
            output_root=self.source / "out", config="Debug", test_regex=".*", cmake=Path(sys.executable),
            native_runner=self.launcher, allow_timing=False, allow_physical_desktop=False, parallel=8)
        fake_ctest = Path(sys.executable).with_name("ctest.exe" if os.name == "nt" else "ctest")
        original_is_file = Path.is_file
        def is_file(path):
            return path == fake_ctest or original_is_file(path)
        with patch.object(targeted, "run_command", side_effect=command), \
             patch.object(targeted, "source_identity", return_value={"revision": "fixture", "dirtyFiles": {}}), \
             patch.object(targeted, "load_artifacts", side_effect=artifacts), \
             patch.object(Path, "is_file", is_file), patch.dict(os.environ, {}, clear=False):
            os.environ.pop("AIRACK_NATIVE_LEASE", None)
            result = targeted.execute(args)
        receipts = list((self.source / "out").glob("*/focused-receipt.json"))
        state = json.loads(receipts[0].read_text())
        self.assertEqual(result, 0, state.get("error"))
        self.assertEqual(state["outcome"], "FOCUSED_PASS")
        self.assertEqual(state["scope"], "FOCUSED_ONLY_NOT_RELEASE_ACCEPTANCE")
        self.assertEqual(state["targets"], ["Check", "RackSynthNativeRun"])
        builds = [a for a in calls if "--build" in a]
        self.assertEqual(len(builds), 1)
        self.assertEqual(builds[0][builds[0].index("--target") + 1:], ["Check", "RackSynthNativeRun"])
        self.assertFalse(list((self.source / "out").rglob("test-profiles.json")))

    def test_junit_requires_current_unique_all_passing_final_membership(self):
        junit = self.source / "result.xml"
        valid = '<testsuite><testcase name="check" status="run"/></testsuite>'
        junit.write_text(valid, encoding="utf-8")
        targeted.validate_junit(junit, ["check"], 0)
        for body in ["<testsuite/>", "bad", valid.replace('status="run"', 'status="notrun"'),
                     valid.replace("/>", "><skipped/></testcase>"),
                     valid.replace("/>", "><failure/></testcase>"),
                     valid.replace('name="check"', 'name="other"'),
                     valid.replace("</testsuite>", '<testcase name="check" status="run"/></testsuite>')]:
            with self.subTest(body=body):
                junit.write_text(body, encoding="utf-8")
                with self.assertRaises(ValueError):
                    targeted.validate_junit(junit, ["check"], 0)
        junit.write_text(valid, encoding="utf-8")
        with self.assertRaises(ValueError):
            targeted.validate_junit(junit, ["check"], time.time_ns() + 10**10)

    def test_command_records_failure_and_argv_without_shell_interpolation(self):
        state = {"commands": []}
        argv = [sys.executable, "-c", "import sys; print(sys.argv[1]); sys.exit(7)", "literal $(no); &no"]
        with self.assertRaises(targeted.CommandFailed):
            targeted.run_command(argv, self.source / "command.log", self.source, state)
        self.assertEqual(state["commands"][0]["exitCode"], 7)
        self.assertEqual(state["commands"][0]["argv"], argv)
        self.assertIn("literal $(no); &no", (self.source / "command.log").read_text())

    def test_test_argv_selects_exact_names_without_retries_or_umbrella_lease(self):
        argv = targeted.ctest_argv(Path("ctest.exe"), self.build, "Debug", [self.entry()], self.source, 8)
        self.assertEqual(argv[0], "ctest.exe")
        self.assertEqual(argv[argv.index("-R") + 1], "^(check)$")
        self.assertNotIn("--repeat", argv)
        self.assertNotIn("--exclusive", argv)
        self.assertIn("--resource-spec-file", argv)
        self.assertEqual(argv[argv.index("-C") + 1], "Debug")

    def test_workflow_inputs_are_data_and_full_remains_default(self):
        workflow = (ROOT / ".github/workflows/windows-native.yml").read_text()
        self.assertIn("default: full", workflow)
        self.assertIn("TARGETED_TEST_REGEX: ${{ inputs.test_regex }}", workflow)
        self.assertIn('"$env:TARGETED_TEST_REGEX"', workflow)
        self.assertIn("if: always() && inputs.mode == 'targeted'", workflow)
        self.assertIn("runs-on: [self-hosted, Windows, X64, ai-rack-synth]", workflow)
        for block in re.findall(r"(?ms)^        run:.*?(?=^      -|\Z)", workflow):
            self.assertNotRegex(block, r"\$\{\{\s*inputs\.")

    def test_workflow_cleanup_enables_long_paths_without_changing_preserved_cache(self):
        workflow = (ROOT / ".github/workflows/windows-native.yml").read_text()
        cleanup = re.search(r"(?m)^        run: (git .*clean.*)$", workflow)
        self.assertIsNotNone(cleanup)
        self.assertEqual(cleanup.group(1).split(),
                         ["git", "-c", "core.longpaths=true", "clean", "-ffdx", "-e", "/build/ci/"])

    def test_workflow_artifacts_require_focused_step_to_have_started(self):
        workflow = (ROOT / ".github/workflows/windows-native.yml").read_text()
        focused = workflow.split("- name: Focused native build and exact CTest selection\n", 1)[1].split("      - name:", 1)[0]
        self.assertIn("        id: focused\n", focused)
        upload = workflow.split("- name: Retain focused evidence including failures\n", 1)[1]
        self.assertIn("if: always() && inputs.mode == 'targeted' && steps.focused.outcome != 'skipped'", upload)

    def test_non_targeted_artifacts_are_bound_to_run_and_attempt(self):
        workflow = (ROOT / ".github/workflows/windows-native.yml").read_text()
        gate = workflow.split("- name: Native build, validation and workflow tests\n", 1)[1].split("      - name:", 1)[0]
        self.assertIn('$outputRoot = "$env:GITHUB_WORKSPACE/build/ci/reports/$env:GITHUB_RUN_ID-$env:GITHUB_RUN_ATTEMPT"', gate)
        for invocation in ('-Measure -OutputRoot $outputRoot', '-Extended -OutputRoot $outputRoot', '-OutputRoot $outputRoot }'):
            self.assertIn(invocation, gate)
        for title in ('Retain complete candidate gate receipts including failures', 'Retain measurement evidence including failures'):
            block = workflow.split('- name: ' + title + '\n', 1)[1].split('      - name:', 1)[0]
            self.assertIn('path: build/ci/reports/${{ github.run_id }}-${{ github.run_attempt }}/', block)
            self.assertNotIn('path: build/ci/reports/\n', block)

    def test_workflow_measure_mode_is_data_and_never_release_acceptance(self):
        workflow = (ROOT / ".github/workflows/windows-native.yml").read_text()
        self.assertIn("options: [full, targeted, measure, extended]", workflow)
        self.assertIn("CI_MODE: ${{ inputs.mode }}", workflow)
        self.assertIn("if ($env:CI_MODE -eq 'measure') { ./tools/windows-native-ci.ps1 -Measure -OutputRoot $outputRoot }", workflow)
        self.assertIn("timeout-minutes: ${{ inputs.mode == 'measure' && 360 || inputs.mode == 'extended' && 300 || 240 }}", workflow)
        self.assertIn("'Measurement run (not release acceptance)'", workflow)
        upload = workflow.split("- name: Retain measurement evidence including failures\n", 1)[1]
        self.assertIn("if: always() && inputs.mode == 'measure'", upload)

    def test_workflow_extended_mode_is_the_complete_release_gate(self):
        workflow = (ROOT / ".github/workflows/windows-native.yml").read_text()
        self.assertIn("elseif ($env:CI_MODE -eq 'extended') { ./tools/windows-native-ci.ps1 -Extended -OutputRoot $outputRoot }", workflow)
        self.assertIn("else { ./tools/windows-native-ci.ps1 -OutputRoot $outputRoot }", workflow)
        self.assertIn("inputs.mode == 'extended' && 'Windows native extended checks'", workflow)
        # Each tier has its own one-finished-run-per-commit guard.
        self.assertIn("if: inputs.mode == 'full' && !inputs.rerun", workflow)
        guard = workflow.split("- name: Refuse a repeated extended run for this commit\n", 1)[1].split("- name:", 1)[0]
        self.assertIn("if: inputs.mode == 'extended' && !inputs.rerun", guard)
        self.assertIn('--job "Windows native extended checks"', guard)

    def test_workflow_guards_full_gate_before_any_work(self):
        workflow = (ROOT / ".github/workflows/windows-native.yml").read_text()
        self.assertIn("  actions: read\n", workflow.split("concurrency:", 1)[0])
        self.assertNotIn("write", workflow.split("permissions:", 1)[1].split("concurrency:", 1)[0])
        steps = workflow.split("    steps:\n", 1)[1]
        guard = steps.split("- name: Refuse a repeated full gate for this commit\n", 1)
        self.assertEqual(len(guard), 2)
        # The guard runs straight after checkout, before cleaning or building anything.
        self.assertNotIn("- name:", guard[0].split("persist-credentials: false", 1)[1])
        self.assertIn("if: inputs.mode == 'full' && !inputs.rerun", guard[1].split("      - name:", 1)[0])
        self.assertIn('tools/ci_repeat_guard.py --repository "$env:GITHUB_REPOSITORY" --sha "$env:GITHUB_SHA"', guard[1])

    def entry(self, name="check", command=None, properties=None):
        return {"name": name, "config": "Debug", "command": command or [str(self.launcher), "--", str(self.exe)],
                "properties": properties or [{"name": "TIMEOUT", "value": 2410}]}

    def file_api(self):
        (self.reply / "target.json").write_text(json.dumps({"name": "Check", "type": "EXECUTABLE",
            "artifacts": [{"path": "test/Debug/check.exe"}]}), encoding="utf-8")
        (self.reply / "codemodel.json").write_text(json.dumps({
            "paths": {"source": str(self.source), "build": str(self.build)},
            "configurations": [{"name": "Debug", "targets": [{"name": "Check", "jsonFile": "target.json"}]}]
        }), encoding="utf-8")
        index = self.reply / "index-now.json"
        index.write_text(json.dumps({"reply": {"client-native-targeted": {"query.json": {
            "responses": [{"kind": "codemodel", "version": {"major": 2}, "jsonFile": "codemodel.json"}]
        }}}}), encoding="utf-8")
        stamp = time.time_ns()
        os.utime(index, ns=(stamp, stamp))
        return index


if __name__ == "__main__":
    unittest.main()

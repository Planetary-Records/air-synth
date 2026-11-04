"""The real PowerShell entrypoint must fail before allocating an invalid CI run."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SATURATION_NAMES = """
RackSynthSaturationHistory_64_44100_modes_0
RackSynthSaturationHistory_64_44100_modes_1
RackSynthSaturationHistory_64_44100_modes_2
RackSynthSaturationHistory_64_44100_cuts
RackSynthSaturationHistory_64_48000_modes_0
RackSynthSaturationHistory_64_48000_modes_1
RackSynthSaturationHistory_64_48000_modes_2
RackSynthSaturationHistory_64_48000_cuts
RackSynthSaturationHistory_64_96000_modes_0
RackSynthSaturationHistory_64_96000_modes_1
RackSynthSaturationHistory_64_96000_modes_2
RackSynthSaturationHistory_64_96000_cuts
RackSynthSaturationControlLaw_64_44100
RackSynthSaturationControlLaw_64_48000
RackSynthSaturationControlLaw_64_96000
RackSynthSaturationChargedFade_64_44100
RackSynthSaturationChargedFade_64_48000
RackSynthSaturationChargedFade_64_96000
""".split()
SPACE_NAMES = """
RackSynthSpaceReverbAlternatingTests
RackSynthSpaceReverbHighBoundaryTests
RackSynthSpaceReverbWarmTests
RackSynthSpaceReverbCoincidentTests
RackSynthSpaceReverbLongDecayTests
RackSynthSpaceReverbFieldTests
RackSynthSpaceReverbProductTankTests
""".split()
SPACE_PATTERN = "^RackSynthSpaceReverb(Alternating|HighBoundary|Warm|Coincident|LongDecay|Field|ProductTank)Tests$"
DEBUG_NAMES = ["RackSynthSaturationHistory_4_96000_modes", "RackSynthSaturationChargedFade_4_44100",
               "RackSynthStateTests"] + [f"UnrelatedDebugCheck{i}" for i in range(270)]


@unittest.skipUnless(sys.platform == "win32" and shutil.which("pwsh"), "PowerShell CI profile helpers")
class WindowsNativeCiProfileTests(unittest.TestCase):
    """Synthetic external outcomes exercise the real helpers, never native acceptance."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="synth CI profiles ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.script = Path(__file__).resolve().parents[1] / "tools" / "windows-native-ci.ps1"

    def powershell(self, body, data=None):
        payload = self.root / "input.json"
        payload.write_text(json.dumps(data or {}), encoding="utf-8")
        harness = self.root / "helpers.ps1"
        harness.write_text(r'''
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($env:TEST_CI_SCRIPT, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] }, $false) |
    ForEach-Object { . ([scriptblock]::Create($_.Extent.Text)) }
$data = Get-Content -LiteralPath $env:TEST_INPUT -Raw | ConvertFrom-Json -AsHashtable
''' + body, encoding="utf-8")
        return subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(harness)],
                              cwd=self.root, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30,
                              env=dict(os.environ, TEST_CI_SCRIPT=str(self.script), TEST_INPUT=str(payload),
                                       TEST_ROOT=str(self.root)))

    def json_result(self, body, data=None):
        result = self.powershell(body, data)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout.strip().splitlines()[-1])

    def plan(self, names=None, space=SPACE_PATTERN):
        return self.json_result("Get-CiReferenceProfilePlan $data.names $data.space | ConvertTo-Json -Depth 20 -Compress",
                                {"names": names or DEBUG_NAMES + SPACE_NAMES + SATURATION_NAMES, "space": space})

    def test_main_configure_refreshes_cache_without_changing_build_contract(self):
        argv = self.json_result(r'''
$commands = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.CommandAst] -and
    $node.Extent.Text -match '^& \$native -- cmake (?:--fresh )?-S \. -B build/ci -G '
}, $true))
if ($commands.Count -ne 1) { throw 'Expected one main CI configure command' }
function Invoke-ConfigureCapture {
    if ($args[0] -eq '--') { $script:configureArgv = @($args[1..($args.Count - 1)]) }
    else { $script:configureArgv = @($args) }
}
$native = 'Invoke-ConfigureCapture'
$resolved312 = 'C:/fixture/python312.exe'
. ([scriptblock]::Create($commands[0].Extent.Text))
ConvertTo-Json -InputObject @($script:configureArgv) -Compress
''')
        self.assertEqual(argv, [
            'cmake', '--fresh', '-S', '.', '-B', 'build/ci',
            '-G', 'Visual Studio 17 2022', '-A', 'x64',
            '-DPython3_EXECUTABLE=C:/fixture/python312.exe',
            '-DRACKSYNTH_PARALLEL_COMPILE=ON',
        ])

    def test_rack_inventory_refuses_missing_case(self):
        result = self.powershell("Assert-CiRackLibraryInventory $data", {"tests": [{"name": "RackSynthRackPackageTests"}]})
        self.assertNotEqual(result.returncode, 0, "Incomplete rack-library inventory was admitted")

    def test_rack_inventory_requires_command_and_gui_resources_without_narrowing_suite(self):
        names = ['RackSynthRackPackageTests', 'RackSynthRackPackageStoreTests', 'RackSynthRackInventoryTests',
                 'RackSynthRackInventoryBoundariesTests', 'RackSynthRackInventoryCanonicalOverflowTests',
                 'RackSynthRackPackageRecallTests', 'RackSynthRackLibraryClientTests', 'RackSynthRackLibraryHttpWindowsTests',
                 'RackSynthInstrumentAiTests', 'RackSynthAiWorkflowSelfTest', 'RackSynthRackLibraryUiTests',
                 'RackSynthRackLibraryProductTests_core', 'RackSynthRackLibraryProductTests_bundle']
        tests = [dict(name=name, command=['native-run', '--', name], properties=[
                    dict(name='LABELS', value=['desktop']), dict(name='RESOURCE_GROUPS', value=[dict(requirements=[dict(resource='.gui', slots=1)])])]) for name in names]
        tests.append(dict(name='UnrelatedMandatoryTest', command=['test'], properties=[]))
        inventory = dict(tests=tests)
        self.assertEqual(self.json_result('ConvertTo-Json -InputObject @(Assert-CiRackLibraryInventory $data) -Compress', inventory), names)
        for missing in ('command', 'desktop'):
            bad = json.loads(json.dumps(inventory))
            target = next(t for t in bad['tests'] if t['name'] == 'RackSynthRackLibraryUiTests')
            if missing == 'command':
                target['command'] = []
            else:
                target['properties'] = []
            self.assertNotEqual(self.powershell('Assert-CiRackLibraryInventory $data', bad).returncode, 0)

    def test_exact_partition_and_no_space_preserve_all_names(self):
        plan = self.plan()
        self.assertEqual(plan["Debug"], sorted(DEBUG_NAMES))
        self.assertEqual(plan["SpaceRelease"], sorted(SPACE_NAMES))
        self.assertEqual(plan["SaturationRelease"], sorted(SATURATION_NAMES))
        self.assertEqual([len(plan[k]) for k in ("Debug", "SpaceRelease", "SaturationRelease")], [273, 7, 18])
        self.assertEqual(len(set(plan["Debug"] + plan["SpaceRelease"] + plan["SaturationRelease"])), 298)
        no_space = self.plan(space="")
        self.assertEqual(no_space["Debug"], sorted(DEBUG_NAMES + SPACE_NAMES))
        self.assertEqual(no_space["SpaceRelease"], [])
        self.assertEqual(no_space["SupportTargets"], ["RackSynthNativeRun"])
        self.assertEqual(no_space["SaturationTargets"], ["RackSynthSaturationHistoryLegacy", "RackSynthSaturationHistoryActual",
                         "RackSynthSaturationControlReference", "RackSynthSaturationControlActual",
                         "RackSynthSaturationChargedReference", "RackSynthSaturationChargedActual"])
        self.assertIn("NewUnrelatedTest", self.plan(DEBUG_NAMES + SPACE_NAMES + SATURATION_NAMES + ["NewUnrelatedTest"])["Debug"])

    def test_partition_refuses_missing_duplicate_extra_matches_and_wrong_space(self):
        names = DEBUG_NAMES + SPACE_NAMES + SATURATION_NAMES
        cases = [(names[:-1], SPACE_PATTERN), (names + [SATURATION_NAMES[0]], SPACE_PATTERN),
                 (names + [DEBUG_NAMES[0]], SPACE_PATTERN), (names + [SATURATION_NAMES[0] + "\n"], SPACE_PATTERN),
                 (names, "SpaceReverb"), (names, ".*"), (names, "^Unrelated")]
        # A broad Space selector becomes invalid when it picks a newly registered witness.
        cases[4] = (names + ["RackSynthSpaceReverbOtherTests"], "SpaceReverb")
        for inventory, space in cases:
            with self.subTest(space=space, last=inventory[-1]):
                result = self.powershell("Get-CiReferenceProfilePlan $data.names $data.space", {"names": inventory, "space": space})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("profile", result.stderr.lower())

    def stage_body(self, extra=""):
        return r'''
$plan = Get-CiReferenceProfilePlan $data.names $data.space
$state = New-CiProfileReceipt ('a' * 32) 'fixture-source'
Write-CiProfileReceipt $state (Join-Path $env:TEST_ROOT 'test-profiles.json') -Initialize
$inventory = @{ tests = @($plan.Names | ForEach-Object { @{name=$_;config='Debug';command=@('fixture-debug', $_)} }) }
Set-CiProfileInventory $state $plan $inventory
$junit = Join-Path $env:TEST_ROOT 'result.xml'
$log = Join-Path $env:TEST_ROOT 'result.log'
$argv = @('--test-dir', $env:TEST_ROOT, '-C', 'Debug', '--output-junit', $junit, '-E', $plan.DebugExcludePattern)
Start-CiProfileStage $state 'Debug' $inventory $argv $env:TEST_ROOT $junit $log
''' + extra

    def fixture(self, **extra):
        return {"names": DEBUG_NAMES + SPACE_NAMES + SATURATION_NAMES, "space": "", **extra}

    def test_stage_removes_stale_artifacts_and_binds_real_configuration(self):
        for name in ("result.xml", "result.log"):
            (self.root / name).write_text("old success", encoding="utf-8")
        state = self.json_result(self.stage_body("$state | ConvertTo-Json -Depth 30 -Compress"), self.fixture())
        stage = state["profiles"]["Debug"]
        self.assertTrue(stage["invoked"])
        self.assertEqual(stage["runId"], state["runId"])
        self.assertEqual(stage["configuration"], "Debug")
        self.assertEqual(len(stage["inventory"]["tests"]), 280)
        self.assertFalse((self.root / "result.xml").exists())
        self.assertFalse((self.root / "result.log").exists())

    def result(self, xml, mutation=""):
        body = self.stage_body(r'''
if ($null -ne $data.xml) { Set-Content -LiteralPath $junit -Value $data.xml }
''' + mutation + r'''
Test-CiProfileResult $state 'Debug' | ConvertTo-Json -Depth 30 -Compress
''')
        return self.json_result(body, self.fixture(xml=xml))

    def xml(self, names=None):
        return "<testsuite>" + "".join(f'<testcase name="{name}" status="run"/>'
                                           for name in (names or DEBUG_NAMES + SPACE_NAMES)) + "</testsuite>"

    def test_result_requires_fresh_complete_unique_passing_final_cases(self):
        valid = self.result(self.xml())
        self.assertTrue(valid["valid"])
        self.assertEqual(len(valid["passingNames"]), 280)
        cases = [None, "broken", "<testsuite/>", self.xml(DEBUG_NAMES[:-1]),
                 self.xml(DEBUG_NAMES + SPACE_NAMES + [DEBUG_NAMES[0]]), self.xml([SATURATION_NAMES[0]]),
                 self.xml().replace('status="run"', 'status="notrun"', 1),
                 self.xml().replace('status="run"', '', 1)]
        cases += [self.xml().replace('/>', f'><{tag}/></testcase>', 1) for tag in ("failure", "error", "skipped")]
        for xml in cases:
            with self.subTest(xml=str(xml)[:60]):
                self.assertFalse(self.result(xml)["valid"])

    def test_result_refuses_uninvoked_wrong_run_or_configuration_evidence(self):
        mutations = ["$state.profiles.Debug.invoked = $false", "$state.profiles.Debug.runId = 'old-run'",
                     "$state.profiles.Debug.stageId = 'OtherStage'", "$state.profiles.Debug.argv += @('-R', 'WrongSelection')",
                     "$state.profiles.Debug.argv[3] = 'Release'",
                     "$state.profiles.Debug.inventory.tests[0].config = 'Release'",
                     "$state.profiles.Debug.inventory.tests[0].command = @()",
                     "$state.profiles.Debug.inventory.tests[0].name = 'OtherProfile'"]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.assertFalse(self.result(self.xml(), mutation)["valid"])
        self.assertFalse(self.result(self.xml().replace('<testsuite>', '<testsuite configuration="Debug">'),
                                     "$state.profiles.Debug.argv = @()" )["valid"])

    def test_final_retry_results_do_not_hide_raw_attempts_or_allow_duplicates(self):
        result = self.json_result(self.stage_body(r'''
Set-Content -LiteralPath $log -Value 'attempt 1 FAILED; attempt 2 passed'
Set-Content -LiteralPath $junit -Value $data.xml
$result = Test-CiProfileResult $state 'Debug'
@{ result=$result; raw=(Get-Content -LiteralPath $log -Raw) } | ConvertTo-Json -Depth 30 -Compress
'''), self.fixture(xml=self.xml()))
        self.assertTrue(result["result"]["valid"])
        self.assertIn("attempt 1 FAILED", result["raw"])
        self.assertFalse(self.result(self.xml(DEBUG_NAMES + SPACE_NAMES + [DEBUG_NAMES[0]]))["valid"])

    def test_initial_receipt_archives_previous_success_and_has_unknown_membership(self):
        path = self.root / "test-profiles.json"
        path.write_text('{"outcome":"COMPLETE","runId":"old"}', encoding="utf-8")
        state = self.json_result(r'''
$state = New-CiProfileReceipt ('a' * 32) 'fixture-source'
Write-CiProfileReceipt $state (Join-Path $env:TEST_ROOT 'test-profiles.json') -Initialize
$state | ConvertTo-Json -Depth 30 -Compress
''')
        self.assertEqual(state["outcome"], "IN_PROGRESS")
        self.assertFalse(state["inventory"]["validated"])
        self.assertIsNone(state["inventory"]["count"])
        self.assertTrue(all(p["state"] == "NOT_RUN" for p in state["profiles"].values()))
        retained = list(self.root.glob("test-profiles.previous-*.json"))
        self.assertEqual(len(retained), 1)
        self.assertEqual(json.loads(retained[0].read_text())["runId"], "old")

    def test_phase_times_record_start_and_finish_without_changing_states(self):
        state = self.json_result(r'''
$state = New-CiProfileReceipt ('a' * 32) 'fixture-source'
Set-CiPhase $state 'inventory' 'IN_PROGRESS'
Set-CiPhase $state 'inventory' 'PASS'
Set-CiPhase $state 'gui' 'IN_PROGRESS'
$state | ConvertTo-Json -Depth 30 -Compress
''')
        self.assertEqual(state["mode"], "full")
        self.assertEqual(state["phases"]["inventory"], "PASS")
        self.assertEqual(state["phases"]["gui"], "IN_PROGRESS")
        times = state["phaseTimes"]
        self.assertEqual(times["preparation"]["startedUtc"], state["startedUtc"])
        self.assertLessEqual(times["inventory"]["startedUtc"], times["inventory"]["finishedUtc"])
        self.assertIsNone(times["gui"]["finishedUtc"])
        self.assertNotIn("space-tests", times)
        unknown = self.powershell("$state = New-CiProfileReceipt ('a' * 32) 'fixture-source'; Set-CiPhase $state 'unknown' 'PASS'")
        self.assertNotEqual(unknown.returncode, 0)

    def test_ctest_policy_stops_on_failure_only_outside_measurement(self):
        policy = self.json_result("@{full=@(Get-CiCtestPolicy); measure=@(Get-CiCtestPolicy -Measure)} | ConvertTo-Json -Compress")
        self.assertEqual(policy["full"], ["--repeat", "until-pass:2", "--stop-on-failure"])
        self.assertEqual(policy["measure"], ["--repeat", "until-pass:2"])

    def test_measured_run_records_failures_and_is_never_complete(self):
        result = self.powershell(r'''
$state = New-CiProfileReceipt ('a' * 32) 'fixture-source' -Measure
$path = Join-Path $env:TEST_ROOT 'test-profiles.json'
Write-CiProfileReceipt $state $path -Initialize
$state.phase = 'debug-tests'
if (-not (Add-CiMeasuredFailure $state 'debug-tests' 8 'CTest failed')) { throw 'Measured failure was not absorbed.' }
$state.outcome = 'MEASURED'
Complete-CiProfileReceipt $state $path 1 $null
Read-CiDelegatedReceipt $path ('a' * 32) | ConvertTo-Json -Depth 30 -Compress
try { Read-CiDelegatedReceipt $path ('a' * 32) -RequireComplete | Out-Null; 'ACCEPTED' } catch { 'REFUSED' }
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        lines = result.stdout.strip().splitlines()
        self.assertEqual(lines[-1], "REFUSED")
        state = json.loads(lines[-2])
        self.assertEqual((state["mode"], state["outcome"], state["failure"]), ("measure", "MEASURED", None))
        self.assertEqual(state["measureFailures"], [{"phase": "debug-tests", "exitCode": 8, "error": "CTest failed"}])
        self.assertEqual(state["phases"]["debug-tests"], "FAILED")
        self.assertIsNotNone(state["finishedUtc"])
        full = self.json_result(r'''
$state = New-CiProfileReceipt ('a' * 32) 'fixture-source'
@{absorbed=(Add-CiMeasuredFailure $state 'debug-tests' 8 'CTest failed'); phase=$state.phases['debug-tests']
  failures=@($state.measureFailures).Count} | ConvertTo-Json -Compress
''')
        self.assertEqual(full, {"absorbed": False, "phase": "NOT_RUN", "failures": 0})

    def finalizer_harness(self, failure, *, phase="preparation", write_failure=False):
        return self.powershell(r'''
$state = New-CiProfileReceipt ('a' * 32) 'fixture-source'
$receiptPath = Join-Path $env:TEST_ROOT 'test-profiles.json'
Write-CiProfileReceipt $state $receiptPath -Initialize
$ciExit = 0; $ciError = $null
$profileState = $state; $ownsProfileReceipt = $true
$state.phase = $data.phase
if ($data.writeFailure) { $receiptPath = Join-Path $env:TEST_ROOT 'missing/receipt.json' }
$finalizer = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.TryStatementAst] -and $null -ne $node.Finally }, $true) |
    Where-Object { $_.Finally.Extent.Text -like '*Complete-CiProfileReceipt*' } | Select-Object -Last 1
if (-not $finalizer) { throw 'Missing real CI finalizer' }
try { . ([scriptblock]::Create($data.failure)) }
catch { $ciError = $_.Exception.Message; throw }
finally { . ([scriptblock]::Create(($finalizer.Finally.Statements.Extent.Text -join "`n"))) }
''', {"phase": phase, "failure": failure, "writeFailure": write_failure})

    def test_real_finalizer_preserves_exit_throw_and_stops_later_profiles(self):
        for phase, failure, code in [("preparation", "$ciExit=37; exit $ciExit", 37),
                                     ("inventory", "throw 'inventory fixture failure'", 1),
                                     ("saturation-build", "$ciExit=39; exit $ciExit", 39),
                                     ("native-report", "$ciExit=41; exit $ciExit", 41),
                                     ("gui", "throw 'GUI fixture failure'", 1)]:
            with self.subTest(phase=phase):
                result = self.finalizer_harness(failure, phase=phase)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                state = json.loads((self.root / "test-profiles.json").read_text(encoding="utf-8-sig"))
                self.assertEqual(state["outcome"], "FAILED")
                self.assertEqual(state["failure"]["phase"], phase)
                self.assertEqual(state["failure"]["exitCode"], code if code != 1 else None)
                self.assertTrue(all(not p["invoked"] for p in state["profiles"].values()))
        result = self.finalizer_harness("$ciExit=43; exit $ciExit", write_failure=True)
        self.assertEqual(result.returncode, 43, result.stderr)
        self.assertIn("receipt publication failed", result.stdout + result.stderr)

    def test_finalizer_reporting_failure_after_success_fails_acceptance(self):
        result = self.finalizer_harness("$state.outcome='COMPLETE'; exit 0")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        state = json.loads((self.root / "test-profiles.json").read_text(encoding="utf-8-sig"))
        self.assertEqual(state["outcome"], "INCOMPLETE")
        result = self.finalizer_harness("exit 0", write_failure=True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_preparation_child_cannot_initialize_or_replace_parent_receipt(self):
        state = self.json_result(r'''
$receiptPath=Join-Path $env:TEST_ROOT 'test-profiles.json'
$parent=New-CiProfileReceipt ('b' * 32) 'parent-source'
Write-CiProfileReceipt $parent $receiptPath -Initialize
$before=Get-Content -LiteralPath $receiptPath -Raw
$PrepareNonce='c' * 32; $ownsProfileReceipt=-not $PrepareNonce; $profileState=$null
$initialize=$ast.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and
    $node.Extent.Text.StartsWith('if ($ownsProfileReceipt)')}, $true) | Select-Object -First 1
if (-not $initialize) { throw 'Missing execution-owner initialization' }
. ([scriptblock]::Create($initialize.Extent.Text))
@{unchanged=($before -ceq (Get-Content -LiteralPath $receiptPath -Raw));childOwns=$ownsProfileReceipt;state=$profileState} |
    ConvertTo-Json -Depth 10 -Compress
''')
        self.assertTrue(state["unchanged"])
        self.assertFalse(state["childOwns"])
        self.assertIsNone(state["state"])

    def test_delegator_correlates_current_child_and_retains_post_child_failure(self):
        body = r'''
$validator=Join-Path $env:TEST_ROOT 'validator.exe'
Set-Content -LiteralPath $validator -Value 'not executed'
$child=Join-Path $env:TEST_ROOT 'fixture-child.ps1'
@'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($env:TEST_CI_SCRIPT,[ref]$tokens,[ref]$errors)
$ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$false) |
    ForEach-Object { . ([scriptblock]::Create($_.Extent.Text)) }
$state=New-CiProfileReceipt $env:AIRACK_CI_DELEGATED_RUN_ID 'explicit child fixture'
$state.outcome='COMPLETE'
if ($env:TEST_CHILD_FAILED -eq 'True') {
    $state.outcome='FAILED'; $state.phase='debug-tests'
    $state.failure=@{phase='debug-tests';exitCode=59;error='original child fixture failure'}
}
Write-CiProfileReceipt $state (Join-Path $env:TEST_ROOT 'output/test-profiles.json') -Initialize
exit 59
'@ | Set-Content -LiteralPath $child
$env:GITHUB_SHA='a' * 40
$env:TEST_CHILD_FAILED=[string]$data.childFailed
& $env:TEST_CI_SCRIPT -Pluginval $validator -Python312 $child -CacheRoot (Join-Path $env:TEST_ROOT 'cache') -OutputRoot (Join-Path $env:TEST_ROOT 'output')
exit $LASTEXITCODE
'''
        for child_failed in (False, True):
            with self.subTest(child_failed=child_failed):
                result = self.powershell(body, {"childFailed": child_failed})
                self.assertEqual(result.returncode, 59, result.stdout + result.stderr)
                state = json.loads((self.root / "output/test-profiles.json").read_text(encoding="utf-8-sig"))
                self.assertEqual(state["outcome"], "FAILED")
                self.assertEqual(state["failure"]["phase"], "debug-tests" if child_failed else "delegation")
                self.assertEqual(state["failure"]["exitCode"], 59)
                if child_failed:
                    self.assertEqual(state["failure"]["error"], "original child fixture failure")

    def test_failed_current_debug_counts_only_fresh_partial_results(self):
        previous = "@{outcome='COMPLETE';runId='old'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $env:TEST_ROOT 'test-profiles.json')\n"
        state = self.json_result(previous + self.stage_body(r'''
foreach ($name in @('release-numerical.xml', 'saturation-reference-release.xml')) {
    Set-Content -LiteralPath (Join-Path $env:TEST_ROOT $name) -Value $data.xml
}
Set-Content -LiteralPath $junit -Value $data.partial
$state.profiles.Debug.exitCode = 47
$state.profiles.Debug.result = Test-CiProfileResult $state 'Debug'
$state.profiles.Debug.state = 'FAILED'
$state.phase = 'debug-tests'
Complete-CiProfileReceipt $state (Join-Path $env:TEST_ROOT 'test-profiles.json') 47 $null
$state | ConvertTo-Json -Depth 30 -Compress
'''), self.fixture(space=SPACE_PATTERN, xml=self.xml(), partial=self.xml([DEBUG_NAMES[0]])))
        self.assertEqual(state["outcome"], "FAILED")
        self.assertEqual(state["coverage"]["passingNames"], [DEBUG_NAMES[0]])
        self.assertEqual(state["profiles"]["SpaceRelease"]["state"], "NOT_RUN")
        self.assertEqual(state["profiles"]["SaturationRelease"]["state"], "NOT_RUN")
        self.assertFalse(self.result(None)["valid"])

    def test_all_profiles_validate_fresh_results_and_exact_complete_union(self):
        for space in (SPACE_PATTERN, ""):
            with self.subTest(space=space):
                state = self.json_result(r'''
$plan=Get-CiReferenceProfilePlan $data.names $data.space
$state=New-CiProfileReceipt ('a' * 32) 'explicit external outcome fixture'
$inventory=@{tests=@($plan.Names | ForEach-Object {@{name=$_;config='Debug';command=@('fixture',$_)}})}
Set-CiProfileInventory $state $plan $inventory
foreach ($key in 'Debug','SpaceRelease','SaturationRelease') {
    $stage=$state.profiles[$key]
    if (-not $stage.required) { continue }
    $inventory=@{tests=@($stage.names | ForEach-Object {@{name=$_;config=$stage.configuration;command=@('fixture',$_)}})}
    $junit=Join-Path $env:TEST_ROOT "$key.xml"; $log=Join-Path $env:TEST_ROOT "$key.log"
    $argv=@('--test-dir',$env:TEST_ROOT,'-C',$stage.configuration,'--output-junit',$junit,
        $(if ($key -eq 'Debug') {'-E'} else {'-R'}),$stage.selector)
    Start-CiProfileStage $state $key $inventory $argv $env:TEST_ROOT $junit $log
    Set-Content -LiteralPath $junit -Value $data.cases[$key]
    New-Item -ItemType Directory -Path (Join-Path $env:TEST_ROOT 'Testing/Temporary') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $env:TEST_ROOT 'Testing/Temporary/LastTest.log') -Value 'explicit raw attempt fixture'
    Set-CiProfileResult $state $key 0
}
foreach ($phase in @($state.phases.Keys)) { if ($state.phases[$phase] -ne 'NOT_REQUIRED') {$state.phases[$phase]='PASS'} }
$state.outcome='COMPLETE'
Complete-CiProfileReceipt $state (Join-Path $env:TEST_ROOT 'test-profiles.json') 0 $null
$state | ConvertTo-Json -Depth 50 -Compress
''', self.fixture(space=space, cases={"Debug": self.xml(DEBUG_NAMES if space else DEBUG_NAMES + SPACE_NAMES),
                      "SpaceRelease": self.xml(SPACE_NAMES), "SaturationRelease": self.xml(SATURATION_NAMES)}))
                self.assertEqual(state["outcome"], "COMPLETE")
                self.assertEqual(state["coverage"]["count"], 298)
                self.assertEqual(state["coverage"]["gaps"], [])
                self.assertEqual(state["profiles"]["SpaceRelease"]["invoked"], bool(space))

    def test_missing_stage_artifacts_cannot_replace_original_nonzero_exit(self):
        state = self.json_result(self.stage_body(r'''
Set-CiProfileResult $state 'Debug' 53
$state.phase='debug-tests'
Complete-CiProfileReceipt $state (Join-Path $env:TEST_ROOT 'test-profiles.json') 53 $null
$state | ConvertTo-Json -Depth 30 -Compress
'''), self.fixture())
        self.assertEqual(state["failure"]["exitCode"], 53)
        self.assertFalse(state["profiles"]["Debug"]["result"]["valid"])
        self.assertEqual(state["coverage"]["count"], 0)

    def test_complete_requires_union_and_no_space_does_not_require_optional_xml(self):
        body = self.stage_body(r'''
foreach ($key in @('Debug', 'SaturationRelease')) {
    $stage = $state.profiles[$key]
    $stage.invoked=$true; $stage.runId=$state.runId; $stage.exitCode=0; $stage.state='PASS'
    $stage.result=@{valid=$true;passingNames=@($stage.names)}
}
$state.outcome='COMPLETE'
foreach ($phase in @($state.phases.Keys)) { $state.phases[$phase]='PASS' }
Complete-CiProfileReceipt $state (Join-Path $env:TEST_ROOT 'test-profiles.json') 0 $null
$state | ConvertTo-Json -Depth 30 -Compress
''')
        state = self.json_result(body, self.fixture())
        self.assertEqual(state["outcome"], "COMPLETE")
        self.assertEqual(state["coverage"]["count"], 298)
        extended = self.json_result(body, self.fixture(names=DEBUG_NAMES + SPACE_NAMES + SATURATION_NAMES + ["NewCheck", "newcheck"]))
        self.assertEqual(extended["coverage"]["count"], 300)
        bad = body.replace("$state.outcome='COMPLETE'", "$state.profiles.SaturationRelease.result.passingNames += $state.profiles.Debug.names[0]; $state.outcome='COMPLETE'")
        result = self.powershell(bad, self.fixture())
        self.assertNotEqual(result.returncode, 0)

    GATE_BODY = r'''
$plan=Get-CiReferenceProfilePlan $data.names $data.space
$state=New-CiProfileReceipt ('a' * 32) 'gate fixture'
$inventory=@{tests=@($plan.Names | ForEach-Object {
    $labels=if ($_ -ceq 'UnrelatedDebugCheck0') { @('timing') } elseif ($_ -ceq 'UnrelatedDebugCheck1') { @('desktop','extended') } else { @('desktop') }
    @{name=$_;config='Debug';command=@('fixture',$_);properties=@(@{name='LABELS';value=$labels})}})}
Set-CiProfileInventory $state $plan $inventory
Set-CiGateTier $state $plan $inventory
$stage=$state.profiles.Debug
$stage.invoked=$true; $stage.runId=$state.runId; $stage.exitCode=0; $stage.state='PASS'
$stage.result=@{valid=$true;passingNames=@($stage.names)}
foreach ($phase in @($state.phases.Keys)) { if ($state.phases[$phase] -notin 'NOT_REQUIRED','DEFERRED') { $state.phases[$phase]='PASS' } }
$state.outcome='GATE_COMPLETE'
Complete-CiProfileReceipt $state (Join-Path $env:TEST_ROOT 'test-profiles.json') 0 $null
@{state=$state;pattern=$plan.DebugExcludePattern} | ConvertTo-Json -Depth 30 -Compress
'''

    def test_gate_defers_labelled_checks_and_release_profiles_without_claiming_release(self):
        result = self.json_result(self.GATE_BODY, self.fixture(space=SPACE_PATTERN))
        state, pattern = result["state"], result["pattern"]
        self.assertEqual(state["outcome"], "GATE_COMPLETE")
        self.assertEqual(state["tier"], "gate")
        deferred = ["UnrelatedDebugCheck0", "UnrelatedDebugCheck1"]
        self.assertEqual(sorted(state["deferred"]["names"]), sorted(deferred + SPACE_NAMES + SATURATION_NAMES))
        self.assertEqual(state["deferred"]["count"], 2 + len(SPACE_NAMES) + len(SATURATION_NAMES))
        self.assertEqual(state["profiles"]["Debug"]["count"], len(DEBUG_NAMES) - 2)
        self.assertEqual(state["profiles"]["Debug"]["selector"], pattern)
        for name in deferred + SPACE_NAMES + SATURATION_NAMES:
            self.assertRegex(name, pattern)
        for name in ("UnrelatedDebugCheck2", "UnrelatedDebugCheck10", "RackSynthStateTests"):
            self.assertNotRegex(name, pattern)
        for key in ("SpaceRelease", "SaturationRelease"):
            self.assertFalse(state["profiles"][key]["required"])
            self.assertEqual(state["profiles"][key]["state"], "DEFERRED")
        self.assertEqual(state["phases"]["saturation-tests"], "DEFERRED")
        self.assertEqual(state["coverage"]["count"], len(DEBUG_NAMES) - 2)

    def test_gate_cannot_be_reported_as_release_or_cover_deferred_checks(self):
        passing = "$stage.result=@{valid=$true;passingNames=@($stage.names)}"
        for bad in (self.GATE_BODY.replace("$state.outcome='GATE_COMPLETE'", "$state.outcome='COMPLETE'"),
                    self.GATE_BODY.replace("Set-CiGateTier $state $plan $inventory", ""),
                    self.GATE_BODY.replace(passing, "$stage.result=@{valid=$true;passingNames=@($stage.names + 'UnrelatedDebugCheck0')}"),
                    self.GATE_BODY.replace(passing, "$stage.result=@{valid=$true;passingNames=@($stage.names | Select-Object -Skip 1)}")):
            with self.subTest(body=bad[-300:]):
                self.assertNotEqual(self.powershell(bad, self.fixture(space=SPACE_PATTERN)).returncode, 0)
        receipt = self.root / "gate.json"
        receipt.write_text(json.dumps({"runId": "a" * 32, "outcome": "GATE_COMPLETE"}), encoding="utf-8")
        refused = self.powershell("Read-CiDelegatedReceipt $data.path ('a' * 32) -RequireComplete", {"path": str(receipt)})
        self.assertNotEqual(refused.returncode, 0)

    def test_gate_lock_admits_one_holder_and_reports_its_wait(self):
        lock = self.root / "locks" / "gate.lock"
        body = "$env:AIRACK_CI_GATE_LOCK=$data.lock; Enter-CiGateLock -WaitMinutes 0"
        acquired = self.powershell(body, {"lock": str(lock)})
        self.assertEqual(acquired.returncode, 0, acquired.stdout + acquired.stderr)
        self.assertIn(f"CI_GATE_LOCK path={lock} wait_seconds=0", acquired.stdout)
        with open(lock, "r+b"):
            held = self.powershell(body, {"lock": str(lock)})
        self.assertNotEqual(held.returncode, 0)
        self.assertIn("Another CI gate held", held.stdout + held.stderr)

    def test_delegation_receipt_requires_current_invocation_and_success(self):
        for run_id, outcome, success in [("a" * 32, "COMPLETE", True), ("old", "COMPLETE", False),
                                          ("a" * 32, "FAILED", False)]:
            with self.subTest(run_id=run_id, outcome=outcome):
                result = self.powershell(r'''
$path = Join-Path $env:TEST_ROOT 'delegated.json'
$data.receipt | ConvertTo-Json | Set-Content -LiteralPath $path
Read-CiDelegatedReceipt $path ('a' * 32) -RequireComplete | ConvertTo-Json -Compress
''', {"receipt": {"runId": run_id, "outcome": outcome}})
                self.assertEqual(result.returncode == 0, success, result.stderr)

    def release_branches(self, *, space=SPACE_PATTERN, failure=None, key="SpaceRelease"):
        """Execute real branch statements with synthetic external command outcomes."""
        return self.json_result(r'''
$plan=Get-CiReferenceProfilePlan $data.names $data.space
$env:GITHUB_SHA='b' * 40
$profileState=New-CiProfileReceipt ('a' * 32) 'synthetic branch fixture'
$initial=@{tests=@($plan.Names | ForEach-Object {@{name=$_;config='Debug';command=@('synthetic-debug',$_)}})}
Set-CiProfileInventory $profileState $plan $initial
$OutputRoot=Join-Path $env:TEST_ROOT ([Guid]::NewGuid().ToString('N'))
$buildRoot=Join-Path $OutputRoot 'build'
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$script:events=[Collections.Generic.List[object]]::new()
$script:built=@{}; $script:payloads=@{}; $ciExit=0; $ciError=$null
if ($data.failure -in @('retention','retention_query_exit')) {
    $filename=if ($data.key -eq 'SpaceRelease') {'ctest-inventory-space-release.json'} else {'ctest-inventory-saturation-release.json'}
    New-Item -ItemType Directory -Path (Join-Path $OutputRoot $filename) | Out-Null
}
function Invoke-ReleaseBuildCapture {
    [string[]]$argv=@($args); if ($argv[0] -eq '--') {$argv=@($argv[1..($argv.Count-1)])}
    $key=if ($argv -contains $plan.SpaceTargets[0]) {'SpaceRelease'} else {'SaturationRelease'}
    $script:events.Add(@{operation='build';key=$key;argv=$argv;phase=$profileState.phase})
    $global:LASTEXITCODE=0
    if ($data.key -eq $key -and $data.failure -eq 'build') {$global:LASTEXITCODE=37;return}
    $script:built[$key]=$true
}
$native='Invoke-ReleaseBuildCapture'
function ctest {
    $argv=@($args);$key=if ($argv[-1] -ceq $plan.SpacePattern) {'SpaceRelease'} else {'SaturationRelease'}
    $global:LASTEXITCODE=0
    if ($argv -contains '--show-only=json-v1') {
        $script:events.Add(@{operation='inventory';key=$key;argv=$argv;phase=$profileState.phase})
        $tests=@($plan[$key] | ForEach-Object {@{name=$_;config='Release';command=if ($script:built[$key]) {@('synthetic-release',$_)} else {@()}}})
        if ($data.key -eq $key) {
            switch ($data.failure) {
                'missing' {$tests=@($tests[1..($tests.Count-1)])}
                'extra' {$tests+=@{name='UnexpectedReleaseWitness';config='Release';command=@('synthetic-release')}}
                'duplicate' {$tests[-1].name=$tests[0].name}
                'case' {$tests[0].name=$tests[0].name.ToLowerInvariant()}
                'configuration' {$tests[0].config='Debug'}
                'missing_command' {$tests[0].Remove('command')}
                'empty_command' {$tests[0].command=@()}
            }
        }
        $raw=if ($data.key -eq $key -and $data.failure -eq 'malformed') {'not JSON'} else {ConvertTo-Json -InputObject @{tests=$tests} -Depth 10 -Compress}
        $script:payloads[$key]=@($raw)
        if ($data.key -eq $key -and $data.failure -in @('query_exit','retention_query_exit')) {$global:LASTEXITCODE=43}
        return $raw
    }
    $script:events.Add(@{operation='test';key=$key;argv=$argv;phase=$profileState.phase})
    $junit=$argv[[Array]::IndexOf($argv,'--output-junit')+1]
    Set-Content -LiteralPath $junit -Value $data.xml[$key]
    New-Item -ItemType Directory -Path (Join-Path $buildRoot 'Testing/Temporary') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $buildRoot 'Testing/Temporary/LastTest.log') -Value 'synthetic attempt1 passed'
}
$pipeline=$ast.FindAll({param($node) $node -is [Management.Automation.Language.TryStatementAst] -and
    $node.Finally.Extent.Text -like '*Complete-CiProfileReceipt*'},$true) | Select-Object -Last 1
$statements=@($pipeline.Body.Statements)
$first=@(0..($statements.Count-1) | Where-Object {$statements[$_].Extent.Text.StartsWith('if ($plan.SpaceRelease.Count)')})
$last=@(0..($statements.Count-1) | Where-Object {$statements[$_].Extent.Text.StartsWith("`$profileState.phase='complete'")})
if ($first.Count -ne 1 -or $last.Count -ne 1) {throw 'Missing actual Release branch boundaries'}
try {. ([scriptblock]::Create(($statements[$first[0]..($last[0]-1)].Extent.Text -join "`n")))}
catch {$ciError=$_.Exception.Message}
finally {Complete-CiProfileReceipt $profileState (Join-Path $OutputRoot 'test-profiles.json') $ciExit $ciError}
$raw=@{}
foreach ($key in 'SpaceRelease','SaturationRelease') {
    $name=if ($key -eq 'SpaceRelease') {'ctest-inventory-space-release.json'} else {'ctest-inventory-saturation-release.json'}
    $path=Join-Path $OutputRoot $name
    $raw[$key]=if (Test-Path -LiteralPath $path -PathType Leaf) {Get-Content -LiteralPath $path -Raw} else {$null}
}
@{state=$profileState;events=$script:events.ToArray();raw=$raw;payloads=$script:payloads;error=$ciError;exit=$ciExit;buildRoot=$buildRoot} |
    ConvertTo-Json -Depth 80 -Compress
''', self.fixture(space=space, failure=failure, key=key,
                 xml={"SpaceRelease": self.xml(SPACE_NAMES), "SaturationRelease": self.xml(SATURATION_NAMES)}))

    def assert_release_query(self, result, key):
        stage = result["state"]["profiles"][key]
        query = stage["inventoryQuery"]
        event = next(e for e in result["events"] if e["key"] == key and e["operation"] == "inventory")
        self.assertEqual(query["argv"], event["argv"])
        self.assertEqual(query["configuration"], "Release")
        self.assertEqual(query["buildRoot"], result["buildRoot"])
        self.assertEqual(query["runId"], "a" * 32)
        self.assertEqual(query["revision"], "b" * 40)
        if result["raw"][key] is not None:
            self.assertEqual(result["raw"][key], os.linesep.join(result["payloads"][key]) + os.linesep)
            self.assertEqual(query["rawSha256"], hashlib.sha256(Path(query["rawPath"]).read_bytes()).hexdigest())
        return query

    def test_release_order_builds_before_inventory(self):
        for space in (SPACE_PATTERN, ""):
            with self.subTest(space=bool(space)):
                result = self.release_branches(space=space)
                self.assertIsNone(result["error"], result)
                keys = ["SpaceRelease", "SaturationRelease"] if space else ["SaturationRelease"]
                self.assertEqual([(e["key"], e["operation"]) for e in result["events"]],
                                 [(key, op) for key in keys for op in ("build", "inventory", "test")])
                for key in keys:
                    query = self.assert_release_query(result, key)
                    self.assertEqual(query["exitCode"], 0)
                    stage = result["state"]["profiles"][key]
                    self.assertEqual(stage["state"], "PASS")
                    self.assertTrue(stage["result"]["valid"])
                    build, inventory, test = [e for e in result["events"] if e["key"] == key]
                    targets = SPACE_NAMES if key == "SpaceRelease" else ["RackSynthNativeRun", "RackSynthSaturationHistoryLegacy",
                        "RackSynthSaturationHistoryActual", "RackSynthSaturationControlReference", "RackSynthSaturationControlActual",
                        "RackSynthSaturationChargedReference", "RackSynthSaturationChargedActual"]
                    self.assertEqual(build["argv"], ["cmake", "--build", "build/ci", "--config", "Release", "--parallel", "8", "--target", *targets])
                    self.assertEqual(inventory["argv"], ["--test-dir", result["buildRoot"], "-C", "Release", "--show-only=json-v1", "-R", stage["selector"]])
                    self.assertEqual(test["argv"], ["--test-dir", result["buildRoot"], "-C", "Release", "--parallel", "6",
                        "--repeat", "until-pass:2", "--stop-on-failure", "--output-on-failure", "--test-output-truncation", "middle",
                        "--output-junit", stage["junit"], "-R", stage["selector"]])
                    self.assertEqual([e["phase"] for e in (build, inventory, test)],
                                     ["space-" + p if key == "SpaceRelease" else "saturation-" + p for p in ("build", "inventory", "tests")])
                if not space:
                    self.assertEqual(result["state"]["profiles"]["SpaceRelease"]["state"], "NOT_REQUIRED")
                    self.assertIsNone(result["raw"]["SpaceRelease"])

    def test_release_inventory_refusals_retain_query(self):
        for key in ("SpaceRelease", "SaturationRelease"):
            for failure in ("missing", "extra", "duplicate", "case", "configuration", "missing_command", "empty_command", "malformed", "query_exit"):
                with self.subTest(key=key, failure=failure):
                    result = self.release_branches(key=key, failure=failure)
                    self.assertIsNotNone(result["error"])
                    query = self.assert_release_query(result, key)
                    self.assertEqual(query["exitCode"], 43 if failure == "query_exit" else 0)
                    self.assertEqual(result["exit"], query["exitCode"])
                    self.assertEqual(result["state"]["failure"]["phase"], "space-inventory" if key == "SpaceRelease" else "saturation-inventory")
                    self.assertFalse(result["state"]["profiles"][key]["invoked"])
                    self.assertEqual([e["operation"] for e in result["events"] if e["key"] == key], ["build", "inventory"])
                    if key == "SpaceRelease":
                        self.assertFalse(any(e["key"] == "SaturationRelease" for e in result["events"]))

    def test_release_build_and_retention_failures_stop(self):
        for key in ("SpaceRelease", "SaturationRelease"):
            for failure in ("build", "retention", "retention_query_exit"):
                with self.subTest(key=key, failure=failure):
                    result = self.release_branches(key=key, failure=failure)
                    self.assertIsNotNone(result["error"])
                    self.assertEqual(result["exit"], 37 if failure == "build" else 43 if failure == "retention_query_exit" else 0)
                    self.assertFalse(result["state"]["profiles"][key]["invoked"])
                    self.assertEqual([e["operation"] for e in result["events"] if e["key"] == key],
                                     ["build"] if failure == "build" else ["build", "inventory"])
                    phase = ("space-" if key == "SpaceRelease" else "saturation-") + ("build" if failure == "build" else "inventory")
                    self.assertEqual(result["state"]["failure"]["phase"], phase)
                    if failure != "build":
                        query = self.assert_release_query(result, key)
                        self.assertIsNone(query["rawSha256"])
                        self.assertIsNone(result["raw"][key])
                        self.assertTrue(query["retentionError"])
                    if key == "SpaceRelease":
                        self.assertFalse(any(e["key"] == "SaturationRelease" for e in result["events"]))


    def test_source_wires_fixed_split_support_target_and_preparation_ownership(self):
        source = self.script.read_text(encoding="utf-8-sig")
        self.assertIn("Get-CiReferenceProfilePlan", source)
        self.assertIn("@($plan.SupportTargets) + @($plan.SaturationTargets)", source)
        self.assertIn("saturation-reference-release.xml", source)
        self.assertIn("AIRACK_CI_RUN_ID", source)
        self.assertIn("$prepared.runId", source)
        self.assertIn("$ownsProfileReceipt = -not $PrepareNonce", source)

    def test_release_references_share_one_parallel_level(self):
        source = self.script.read_text(encoding="utf-8-sig")
        self.assertIn("function Get-CiReleaseParallel { '6' }", source)
        self.assertEqual(source.count("'-C', 'Release', '--parallel', (Get-CiReleaseParallel))"), 2)
        self.assertNotIn("'Release', '--parallel', '2'", source)

    def test_preparation_runs_both_python_suites_through_the_module_runner(self):
        source = self.script.read_text(encoding="utf-8-sig")
        self.assertNotIn("-m unittest discover -s tests -p 'test_*.py'", source)
        runner = source.split("& $native -- $resolved312 (Join-Path $PSScriptRoot 'ci_python_suites.py')", 1)
        self.assertEqual(len(runner), 2)
        self.assertIn('--suite "3.12=$resolved312" --suite "3.14=$resolved314"', runner[1].split("\n", 1)[0])
        self.assertIn("Assert-CiExit 'Python 3.12 and 3.14 development checks'", runner[1].split("Native resource setup", 1)[0])

    def test_source_wires_measurement_through_every_ctest_profile(self):
        source = self.script.read_text(encoding="utf-8-sig")
        self.assertEqual(source.count("(Get-CiCtestPolicy -Measure:$Measure)"), 3)
        body = source.split("function Get-CiCtestPolicy", 1)[1].split("\n}\n", 1)[1]
        self.assertNotIn("'--stop-on-failure'", body)
        self.assertIn("New-CiProfileReceipt $runId $source -Measure:$Measure", source)


@unittest.skipUnless(sys.platform == "win32" and shutil.which("pwsh"), "Windows PowerShell CI entrypoint")
class WindowsNativeCiTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="synth CI refusal ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.script = Path(__file__).resolve().parents[1] / "tools" / "windows-native-ci.ps1"

    def run_script(self, *args, extra_env=None):
        env = dict(os.environ, GITHUB_SHA="a" * 40, PYTHON_BASIC_REPL="1",
                   AIRACK_CI_GATE_LOCK=str(self.root / "gate.lock"))
        env.update(extra_env or {})
        return subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(self.script), *map(str, args)],
                              cwd=self.root, env=env, stdin=subprocess.DEVNULL,
                              capture_output=True, text=True, timeout=20)

    def test_measurement_refuses_delegated_workspace_before_outputs(self):
        result = self.run_script("-Measure", "-CacheRoot", self.root / "cache", "-OutputRoot", self.root / "output")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Measurement", result.stdout + result.stderr)
        self.assertFalse((self.root / "output").exists())

    def test_missing_validator_fails_before_creating_outputs(self):
        result = self.run_script("-Pluginval", self.root / "missing.exe", "-OutputRoot", self.root / "output")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("pluginval 1.0.4 is required", result.stderr)
        self.assertFalse((self.root / "output").exists())

    def test_full_ci_refuses_a_parent_lease_before_building(self):
        validator = self.root / "unused-validator.exe"
        validator.write_bytes(b"not executed")
        result = self.run_script("-Pluginval", validator, "-OutputRoot", self.root / "output",
                                 extra_env={"AIRACK_NATIVE_LEASE": "shared"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("outside a native lease", result.stderr)
        self.assertFalse((self.root / "output").exists())

    def test_preparation_refuses_invalid_nonce_lease_or_parent_id_before_outputs(self):
        validator = self.root / "unused-validator.exe"
        validator.write_bytes(b"not executed")
        for nonce, lease, run_id, message in [
                ("wrong", "shared", "a" * 32, "shared native lease"),
                ("a" * 32, "", "a" * 32, "shared native lease"),
                ("a" * 32, "shared", "", "parent invocation ID")]:
            with self.subTest(nonce=nonce, lease=lease, run_id=run_id):
                result = self.run_script("-Pluginval", validator, "-PrepareNonce", nonce,
                                         "-OutputRoot", self.root / "output",
                                         extra_env={"AIRACK_NATIVE_LEASE": lease, "AIRACK_CI_RUN_ID": run_id})
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertFalse((self.root / "output").exists())

    def test_ci_matches_interactive_priority_when_started_below_normal(self):
        harness = self.root / "priority.ps1"
        harness.write_text('''
[Diagnostics.Process]::GetCurrentProcess().PriorityClass = 'BelowNormal'
$before = [Diagnostics.Process]::GetCurrentProcess().PriorityClass.ToString()
try { & $env:TEST_CI_SCRIPT -Pluginval $env:TEST_MISSING_VALIDATOR -OutputRoot $env:TEST_OUTPUT }
catch { $refusal = $_.Exception.Message }
if ($refusal -notlike '*pluginval 1.0.4 is required*') { throw 'Expected early validator refusal' }
@{ before = $before; after = [Diagnostics.Process]::GetCurrentProcess().PriorityClass.ToString() } | ConvertTo-Json -Compress
''', encoding="utf-8")
        result = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File", str(harness)],
                                cwd=self.root, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=20,
                                env=dict(os.environ, TEST_CI_SCRIPT=str(self.script),
                                         TEST_MISSING_VALIDATOR=str(self.root / "missing.exe"),
                                         TEST_OUTPUT=str(self.root / "output")))
        self.assertEqual(result.returncode, 0, result.stderr)
        priority = json.loads(result.stdout.strip().splitlines()[-1])
        self.assertEqual(priority["before"], "BelowNormal")
        self.assertEqual(priority["after"], "Normal")
        self.assertFalse((self.root / "output").exists())

    def test_wrong_revision_refuses_persistent_workspace_before_build(self):
        validator = self.root / "unused-validator.exe"
        validator.write_bytes(b"not executed")
        programs = [("-Python312", sys.executable, "-Python314", sys.executable)]
        if shutil.which("py"):
            programs.append(())
        for program in programs:
            with self.subTest(program=program):
                result = self.run_script("-Pluginval", validator, "-CacheRoot", self.root / "cache",
                                         *program, "-OutputRoot", self.root / "output")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Incoming source does not match the exact CI revision", result.stderr)
                self.assertFalse((self.root / "cache").exists())
                self.assertFalse((self.root / "output").exists())

    def resolve_python(self, runner, version, program=None):
        return subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-File",
                               str(self.script.with_name("Resolve-CiPython.ps1")),
                               "-Program", program or sys.executable, "-Version", version, "-NativeRunner", str(runner)],
                              cwd=self.root, stdin=subprocess.DEVNULL,
                              env=dict(os.environ, PYTHON_BASIC_REPL="1"),
                              capture_output=True, text=True, timeout=20)

    @unittest.skipUnless(shutil.which("py"), "Optional Python launcher")
    def test_launcher_selects_both_requested_versions(self):
        runner = Path(os.environ.get("AIRACK_NATIVE_RUNNER", "build/native-run/Debug/RackSynthNativeRun.exe")).resolve()
        if not runner.is_file():
            self.skipTest("Native runner regression runs after bootstrap")
        for version in ("3.12", "3.14"):
            with self.subTest(version=version):
                result = self.resolve_python(runner, version, "py")
                self.assertEqual(result.returncode, 0, result.stderr)
                executable = Path(result.stdout.strip())
                self.assertTrue(executable.is_file(), result.stdout)
                identity = subprocess.run([str(executable), "-c", "import sys; print('.'.join(map(str, sys.version_info[:2])))"],
                                          capture_output=True, text=True, timeout=10)
                self.assertEqual(identity.returncode, 0, identity.stderr)
                self.assertEqual(identity.stdout.strip(), version)

    def test_interpreter_resolution_refuses_zero_exit_without_execution(self):
        noop = self.root / "noop.cmd"
        noop.write_text("@exit /b 0\n")
        result = self.resolve_python(noop, f"{sys.version_info.major}.{sys.version_info.minor}")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Python did not report one executable path", result.stderr)

    def test_resolves_real_interpreter_and_rejects_wrong_version(self):
        runner = Path(os.environ.get("AIRACK_NATIVE_RUNNER", "build/native-run/Debug/RackSynthNativeRun.exe")).resolve()
        if not runner.is_file():
            self.skipTest("Native runner regression runs after bootstrap")
        version = f"{sys.version_info.major}.{sys.version_info.minor}"
        result = self.resolve_python(runner, version)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(Path(result.stdout.strip()).resolve(), Path(sys.executable).resolve())
        wrong = self.resolve_python(runner, "3.12" if version == "3.14" else "3.14")
        self.assertNotEqual(wrong.returncode, 0)


if __name__ == "__main__":
    unittest.main()

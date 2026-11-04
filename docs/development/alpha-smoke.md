# Repeatable alpha VST3 smoke

Run this on a complete candidate bundle before accepting an alpha. It validates the actual VST3; it does not install the bundle, sign in, open an editor or play through speakers. DAW and listening checks remain separate.

Use the existing Release build tree and the native coordinator described in [building](building.md). Build `RackSynthRender` before starting. Provide a reviewed pluginval executable; the current alpha checks use 1.0.4, strictness 5 and seed 20261003.

The candidate bundle must contain:

- `Contents/x86_64-win/AI Rack Synth.vst3`
- `Contents/x86_64-win/AI Rack Synth Companion.exe`
- `Contents/Resources/moduleinfo.json`

Use a staged copy with its matching helper. If development will continue during the run, also copy the renderer into that staging directory and supply `--renderer` with its absolute path. Keep all tested files unchanged until the command finishes. Do not build over an executable being tested.

From the repository root, substitute your candidate and validator paths:

```powershell
$bundle = (Resolve-Path 'build/host-fixtures/candidate/AI Rack Synth.vst3').Path
$validator = (Resolve-Path 'C:/tools/pluginval/pluginval.exe').Path
py -3.12 tools/alpha_smoke.py --plugin $bundle --validator $validator --output "$PWD/build/host-fixtures/alpha-smoke-1"
```

The output directory must be fresh and inside this checkout's `build`. The script acquires a separate native lease for each phase; run Python directly rather than wrapping the script in another lease. Each phase uses a private desktop. The renderer runs below normal priority.

The default workload runs ten simultaneous instances for 3,600 seconds, after validator, scan, lifecycle/recall and render checks. It covers the original matrix of 30 fixtures and 90 fresh instances, and all 15 combinations of 44.1/48/96 kHz with buffers of 64/128/256/512/1024 samples. It compares partitioning and offline/realtime output, checks finite/unclipped/non-silent audio and recalled state, and requires both 3,600 seconds of wall time and at least 172,800,000 rendered frames per instance. Scan time, private memory and cadence lateness are observations; there is no added DAW deadline or memory growth budget.

For a short harness diagnostic, add `--soak-seconds 1`. A short run explicitly cannot qualify the hour, even when all its checks pass.

While running, read `render/alpha-smoke-progress.json` in the output directory. It updates once per minute after the ten-instance soak begins. At completion, require the process exit to be zero and inspect:

- `smoke-receipt.json`: complete, success and full_smoke_complete must all be true for full qualification; input hashes must remain unchanged.
- `render/alpha-smoke.json`: full_hour_soak must be true, with the requested duration, ten instances, required frames and all 15 rows.
- `render/report.json`: the original 30-fixture matrix must pass.
- `validator.log` and `renderer.log`: preserve the exact first attempt, including failures.

The script writes a failed receipt on errors after startup. An input/path failure before creating the output directory is a setup failure, not product acceptance. Keep failed output directories; use a new directory for a deliberate rerun and record its reason. This local smoke does not replace the final candidate's CI, source/package bindings or human host/listening acceptance.

Fast harness checks:

```powershell
py -3.12 -m unittest discover -s tools -p test_alpha_smoke.py -v
& ./build/native-run/Debug/RackSynthNativeRun.exe --run-seconds 60 -- "$PWD/build/windows-release/RackSynthRender_artefacts/Release/RackSynthRender.exe" --alpha-smoke-self-test
```

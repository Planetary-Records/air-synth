# Windows product builds and fast checks

Use Windows x64, Visual Studio 2022 Build Tools with MSVC C++20, Visual C++ CMake tools and Windows 11 SDK 10.0.26100.0. The included [.vsconfig](../../.vsconfig) lists these components. CMake 3.22 or newer and Git are required. Python is development tooling, not a shipped runtime dependency.

JUCE 9.0.3 is pinned to be29c81492b6151c8ea8d14c840e1311963b3a83. First configure fetches it from its public upstream. For an offline release-source build, extract the matching JUCE archive separately and pass FETCHCONTENT_SOURCE_DIR_JUCE plus FETCHCONTENT_FULLY_DISCONNECTED=ON. Do not share another worktree's mutable dependency checkout.

## Product and three functional checks

Run from this repository root in PowerShell:

```powershell
$vs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$env:PATH = "$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin;$env:PATH"
cmake -S tools/native-run -B build/native-run -G 'Visual Studio 17 2022' -A x64
cmake --build build/native-run --config Debug --target RackSynthNativeRun
$native = (Resolve-Path build/native-run/Debug/RackSynthNativeRun.exe).Path
& $native -- cmake -S . -B build/contributor -G 'Visual Studio 17 2022' -A x64 -DCMAKE_SYSTEM_VERSION=10.0.26100.0 -DRACKSYNTH_BUILD_PROBES=OFF
& $native -- cmake --build build/contributor --config Debug --parallel 4 --target RackSynthCompanionDeploy RackSynthCoreTests RackSynthStateTests RackSynthRender
ctest --test-dir build/contributor -C Debug --output-on-failure -R '^(RackSynthCoreTests|RackSynthStateTests|RackSynthRenderSelfTest)$'
```

Check each command's exit status before continuing. Bootstrap builds only the coordinator; subsequent native operations use it. CTest acquires its own leases. Reuse this worktree's build tree for later edits and keep one writer per tree. No system-wide installation, DAW, sign-in or speaker playback is needed.

The VST3 is build/contributor/RackSynthInstrument_artefacts/Debug/VST3/AI Rack Synth.vst3. Deploy also builds the standalone format and puts its matching helper beside both. To supply an extracted dependency offline, add `-DFETCHCONTENT_SOURCE_DIR_JUCE='C:/path/to/JUCE' -DFETCHCONTENT_FULLY_DISCONNECTED=ON` to configure. The selected folder must contain JUCE's CMakeLists.txt.

For a Release product, use a separate Release build tree/configuration with probes off. Shipping Release links the C++ runtime statically. Core allocation tests require Debug's CRT hook; do not treat a Release CoreTests failure as a product failure.

## Scope

This is the supported contributor smoke path, not the complete private numerical/performance campaign. Some historical probes and Python witnesses require deliberately excluded private packets or tools. Do not enable probes and claim those results from this source selection. The render self-test explicitly skips two absent private comparison-recipe assertions and still rejects a malformed present recipe.

For packaging changes, the portable package and installer tests run with `py -3.12 -m unittest discover -s tests -p test_release_preparation.py` and `test_release_installer.py`. For product qualification, use [alpha smoke](alpha-smoke.md); that one-hour workload is separate from a fast PR check. [Validation](validation.md) distinguishes automated, host and listening evidence. The public workflow remains an [operator-reviewed template](public-contributor-ci.md) until its separate activation gate.

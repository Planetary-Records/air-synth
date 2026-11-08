# Assembling the skin and integrating the rack

## Where the files go

| What | Where |
| --- | --- |
| Brief, wireframes, prompts, concept images, drafts | `docs/design/racks/2026-10-09-studio-rack/<slug>/` (`rack-brief.json`, `wireframe-closed.svg` / `.png`, `wireframe-open.svg` / `.png`, `prompt-closed.txt`, `prompt-open.txt`, `closed.png`, `open.png`, `drafts/`) |
| Generated production artwork and cell map | the bundle's `skin-sources/` (`plate-<view>.png`, `assets-01.png`, further sheets as needed, editable grid guides and `asset-map.json`) |
| Generation and review records | the bundle's `prompts.json`, `manifest.json` and `review.md`, one entry per image |
| Skin script and its working files | `docs/design/racks/2026-10-09-studio-rack/<slug>/skin/` (`<slug_underscored>_skin.py`, `measure.json`, `extract.json`, `elements.json`, `plates/`, `parts/`, `compare/`) |
| Installed skin | `resources/maker-skins/<slug>/` (`<slug>-elements.json`, `plates/`, `parts/`, `SOURCE.json`, `.gitattributes`) |
| Spec, plan | `docs/superpowers/specs/<date>-<slug>-faceplate.md`, `docs/superpowers/plans/<date>-<slug>-faceplate.md` |
| Technical record, snapshots | `docs/verification/<date>-<slug>-faceplate-technical.md`, `docs/verification/studio-rack-<slug>/` |

The design record must sit beside the three makers: the skin script imports `../../house/skin/house_skin.py` by a path relative to itself.

## The skin script

Start from the structure and native contract of `thread-press/skin/thread_press_skin.py`, the simplest one: knobs, levers, a lamp and an in-place recess. Borrow from `ember-reed/skin/ember_reed_skin.py` for knurled grip rings, faders, selectors, a drawer and rows planned to whole units, and from `glass-current/skin/glass_current_skin.py` for displays with live lettering, pages of soft knobs, detents, a hanging door and a maker strip. Replace their historical control extraction and missing-state synthesis with the generated plates and shared sheets from [faceplate-imagegen.md](faceplate-imagegen.md#production-plates-and-shared-asset-sheets). It is development tooling: Python 3.12 with OpenCV and NumPy, run as `py -3.12 -B <slug_underscored>_skin.py <stage>`, never shipped.

| Stage | Does | Writes |
| --- | --- | --- |
| `measure` | records control placement, pivots, ticks and drawn values from the concepts; validates sheet cell/state mappings and registers the generated clean plates to the approved layout | `measure.json`, `compare/measure-*.png` |
| `extract` | slices the generated sheets by recorded cell rectangles, preserving alpha and pivots, and resamples the generated clean plates and parts to the house pitch; never extracts control silhouettes from concepts | `plates/`, `parts/`, `extract.json` |
| `elements` | the element contract the product loads | `elements.json` |
| `compare` | checks the assembled production composites against the concepts for layout and appearance; `compare <dir>` scores the native images `RackSynthMakerFaceTests <dir>` writes against those recorded composites | `compare/diff-report.json`, `compare/fidelity.png`; exits non-zero out of bounds |
| `install` | copies the skin into `resources/maker-skins/<slug>/`, writes `SOURCE.json` (the script's, concepts', clean plates', sheets', cell map's and every installed file's SHA256) and `.gitattributes` (`* -text`, so the bytes stay exact) | the installed skin |

Record each sheet's dimensions, grid and padding in `asset-map.json`; each occupied cell has an asset ID, state/frame index, pixel rectangle, pivot within the cell and value/angle mapping. Map each control instance to those reusable assets separately from its parameter binding. Several asset families may share a sheet, and a family may span sheets when its frame count needs the space. Slicing cells into individual runtime files is fine; generating one sheet per control instance is unnecessary.

Assembly rules (the shared-sheet rules supersede the merged scripts' cutout techniques):
- **Pitch.** Everything is in face pixels at the house pitch: 2140 px across the 19-inch panel, 197.11 px per rack unit (EIA 1.75 in). Angles are degrees clockwise from 12 o'clock.
- **Heights from the drawing.** Measure the drawn face's aspect ratio against whole units (2U is 5.43:1, 3U is 3.62:1). Resample by at most a few per cent; reach whole units by repeating rows of plain panel from bands that cross only vertical edges. Never stretch a control, legend or screw.
- **Plates.** Use generated clean plates with every sheet-supplied part and its shadow, lit layers and live display text omitted. Do not remove controls from concepts and reconstruct their covered surfaces in code. Fixed engraving and scales stay on the plate; shared sprites carry only their own hardware detail.
- **Parts.** Use the generated grid cells with transparent padding and a recorded pivot. Use a consistent cell size and pivot throughout each asset's state sequence. A round knob may use separately generated body and rotor cells for native rotation; a fader cap may translate. Generate frame sequences where the artwork itself changes with motion. Reuse matching parts across this rack's control instances.
- **Required states.** Obtain button, lever, lamp, selector and animated knob states from the planned sheet cells. Missing states require a corrected or additional shared sheet, not mirroring, darkening or painting a concept cutout. State order and value mapping must reach every required position; do not silently quantize continuous parameters to sparse sprites. If the existing loader cannot represent the chosen sheet animation, extend it under tests first rather than reverting to concept extraction.
- **The open state keeps the closed plate.** Share the generated clean closed plate between views; register the generated clean reveal plate to it and compose the reveal over it. Cropping a plate region or slicing a grid cell is fine; cutting moving controls out of either concept is not.
- **Scales.** Each printed tick is a `[value, angle]` pair measured on the drawing. Between ticks the angle is linear in the response's proportion; outside them the pointer pins at the end stop.
- **Hit areas.** A control drawn small at the stack's scale takes its whole cell, legend included.
- **Names.** Every installed file's name starts with `<slug>-`. JUCE's binary data keeps file names only and all makers share it; `makerSkinFile` refuses a name without its skin's prefix.
- **Fidelity gates.** Check the production composites beside the approved concepts at 1:1 for layout, scale, material, lettering and control identity, recording differences for the user's visual review. Newly generated artwork is not pixel-identical to the concepts: the native render's pixel target is the recorded composite made from the clean plates and sheet cells, hashed before native comparison. Against that target, retain a mean absolute difference of at most 1.0 per channel on the face and 2.0 on the reveal; report the 99th percentile and each control's score too. Preview every discrete state and sweep animated controls to check for missing cells, clipped shadows, pivot jitter, duplicate baked controls and inconsistent lighting.

## `elements.json`

Top level: `format` (`ai-rack-maker-skin`), `version`, `design_id` (the example design's UUID), `maker`, `model` (the engraved model line), `units` (the closed height), `reveal`, `rack_pitch`, `views` (each view's plate, units and source), `assets` (each file with its size, anchor, source and SHA256), `groups` (members and Tab `focus_order` per view), `elements`. Glass Current adds `readout: "display"` (its VFD replaces the host's touch readout), `strip`, `pages` and `screens`.

Thread Press's GRIP, trimmed:

```json
{
  "id": "face.grip", "view": "face", "kind": "rotary", "label": "GRIP", "group": "face.face",
  "binding": { "rack_unit": 0, "key": "compressor.threshold_db", "type": "float",
               "range": [-36, 0], "unit": "dB", "default": -18, "response": "linear" },
  "geometry": { "pivot": [592.09, 213.63], "face_r": 65.87, "tick_r": 73.69, "hit_r": 73.69, "dot": [641.72, 164.0] },
  "scale": { "sweep": [-135.44, 136.23],
             "ticks": [[-36, -135.44], [-24, -78.79], [-18, -0.59], [-12, 78.61], [0, 136.23]] },
  "layers": { "pointer": "face:grip.head" }
}
```

`binding.rack_unit` is an index into the design's units and `key` a local key of that unit's module; the host resolves the registry parameter per placed rack. `dot` is where the lock dot goes. Keep `binding.range` equal to the focused control's range unless the spec records why.

Element kinds the loader (`src/studio/HouseSkin.cpp`) reads: `rotary` (with `knurl` frames and teeth, `detents` for a choice), `selector`, `fader`, `lever` (`positions`, each with a value and a sprite; `inverted` for a bypass shown as IN), `toggle`, `action` (openers, closers, page buttons), `lamp` (`lit_value`), `still`, `display` (a lattice the native dot-matrix renderer letters) and `tape`. Reveal kinds (`MakerFace`): `recess` (a view over the face, in place), `drawer` (a view between the face and its `front` view, adding units) and `door` (a panel over the face and a door hung below it). Each has `opener`, `closers` or `titles`, and `motion` (ms and the moving rect). A design whose skin needs a kind or behaviour the loader lacks extends `HouseSkin`, `HouseControls` and `MakerFace` under tests first, as each merged rack did.

## Registering the rack

1. **The example design.** Append it to `exampleRackDesigns()` in `src/racks/RackExamples.cpp` with a fixed new UUID, and widen `std::array<RackDesign,3>` there, in `src/racks/RackExamples.h` and in `src/plugin/RackPanel.cpp`. Append, so `examples[0]` to `[2]` keep meaning Ember Reed, Glass Current and Thread Press across the tests. `libraryRackDesigns()` then lists it among the LIBRARY's examples and the AI's catalog.
2. **The skin.** Add the slug to the list in `LoadedMakerSkins` (`src/studio/MakerFace.cpp`). The loader finds the example by the skin's `design_id`, and a skin that faces no example fails to load.
3. **The binary data.** Add every installed file to the `RackSynthMakerSkins` sources in `CMakeLists.txt`.
4. **The pinned checks.** Find them with `rg "==21|size\(\)==21|n<3" tests/native` and `rg "historicalHashes" tests/native`, and migrate each so it still checks what it checked:
   - the library's size and order (`library_designs` in `RackDesignTests.cpp`: examples first, then the factory designs);
   - `historicalHashes` in `factory_placement_and_recall`, which loops over every example: add the new design's hash from the test's `LEGACY_EXAMPLE` output;
   - the LIBRARY's row counts in `StudioWindowTests.cpp`, the catalog's entry counts in `AiProposalTests.cpp`, `AiPanelTests.cpp`, `CompanionWireTests.cpp` and `DynamicEqRackTests.cpp`;
   - loops over the examples in `RackPanelTests.cpp`.
5. **New checks, written first and seen failing.** In `tests/native/MakerFaceTests.cpp`, after the merged racks' groups: `<rack>_face_for` (the faceplate for the example and for a copy with other values, intent or revision; none after a change to its ID, name, role, units, sections or any focused control's field; its files found only under its own skin's name), `<rack>_skin_loads` (the maker, model, units and element count; each view's focus order; every bound element on an active key of its unit's module, with its dot, ticks and drawn value; the plate at 2140 px and whole units), `<rack>_angles` (each knob's mapping passes through its first and last ticks, pins beyond them), and groups for the reveal and any new kind. In `tests/native/StudioWindowTests.cpp`: the faceplate in the stack bound to its own banks, through a second placement's bindings too; one drag is one gesture; the lamp follows its parameter; the reveal opens without writing or undo, joins Tab order only while open, returns focus on closing and closes on fold; locks, marks, AI: KEEP and the folded strip. Its snapshots go to `docs/verification/studio-rack-<slug>/`.

## Commands

```powershell
cmake --build --preset windows-debug --parallel 4
# each test target is build/windows-debug/<target>_artefacts/Debug/<target>.exe; run the ones the change touches:
RackSynthRackDesignTests
RackSynthMakerFaceTests <dir>                # writes the native <slug>-drawn.png and <slug>-open-drawn.png
RackSynthStudioWindowTests --images <dir>    # writes the stack and window snapshots
RackSynthRackPanelTests; RackSynthAiPanelTests; RackSynthAiPanelRackDiffTests; RackSynthProgrammerTests
```

Then `py -3.12 -B <slug_underscored>_skin.py compare <dir>` on the native images (Thread Press's script reads the older names `face-drawn.png` and `face-open-drawn.png`; later racks read `<slug>-drawn.png`), and pluginval on `build/windows-debug/RackSynthInstrument_artefacts/Debug/VST3/AI Rack Synth.vst3`, at `--strictness-level 5 --random-seed 20261003`, once with `--skip-gui-tests` and once without. The pluginval path is the `PLUGINVAL` variable in `.github/workflows/windows-native.yml`; it is a GUI-subsystem program, so start it with `Start-Process -Wait` as the workflow does. CI runs every CTest target on the self-hosted runner. Builds, direct test runs, pluginval and the standalone app go through `RackSynthNativeRun`, the GUI ones under its desktop slot, as [parallel native work](../../../../docs/development/parallel-native-work.md) sets out; CTest coordinates its own tests, and other worktrees and CI may run meanwhile. Record each command's exit, check counts and fidelity scores in the technical record.

## The standalone app

The instrument also builds as JUCE's standalone app, `build/windows-debug/RackSynthInstrument_artefacts/Debug/Standalone/AI Rack Synth.exe`; `RackSynthCompanionDeploy` puts the companion beside it, as in the VST3 bundle. It is a development and review build, not installed. `tools/standalone-window.ps1` launches or finds it, sizes the editor, presses controls by their accessible names through UI Automation, sends keys to a focused control and captures the window with `PrintWindow`, without touching the user's mouse or keyboard. The app keeps its session between launches.

The window raises and takes the focus, so hold the desktop slot while it is open: put the calls in one script, end it with `-Close`, and run the script under `RackSynthNativeRun --desktop`, which ends whatever its command started when the command returns.

```powershell
# review.ps1
$w = "$PSScriptRoot/../../tools/standalone-window.ps1"
try {
    & $w -List                                                               # the names -Press and -Focus take
    & $w -Width 1080 -Height 680 -Press AI -Focus TIME -Out "$PSScriptRoot/ai.png"   # a control's focus brings its rack into view
    & $w -Press 'Add rack' -Out "$PSScriptRoot/library.png"
    & $w -Focus 'Library screen' -Keys Down,Down,Down,Down,Down,Down,Down -Out "$PSScriptRoot/row.png"
    & $w -Focus 'Library screen' -Keys Enter
    & $w -Press REVIEW,APPLY -Out "$PSScriptRoot/placed.png"
} finally { & $w -Close }
```

```powershell
& $native -- $cmake --build build/windows-debug --config Debug --target RackSynthCompanionDeploy   # VST3, standalone and companion
& $native --desktop --run-seconds 600 -- (Get-Command pwsh).Source -NoProfile -File "$PWD/build/shots/review.ps1"
```

Each call presses first, then focuses and sends keys, then captures. Controls outside the stack's view are not offered to UI Automation, so bring a rack into view by focusing one of its controls first. The LIBRARY's screen takes Up and Down to move its cursor through the tabs and rows and Enter to choose; read each capture to see where the cursor is. Mouse messages posted to the window do nothing, since JUCE checks the real cursor before a button reacts; for hover states, use the Computer Use tools when they are available, or ask the user. Close the app before rebuilding it.

## A new module

When no module in `NativeModule` does the job, the rack waits on a DSP feature with its own spec, plan and user approval. The rack's face still follows the merged makers: take only code touchpoints from the last module appended (`dynamicEq` on main today), never the graphs or the editing and AI workflows of Contour Dynamics, Character Saturation or Space Reverb. Find them with `rg "dynamicEq|dynamic_eq|DynamicEq" src`:
- the DSP unit under `src/dsp/` with its own tests, and its place in `src/dsp/RoutedSynthEngine.cpp` and `src/dsp/DspCompatibility.h`;
- appended enum values (`NativeModule` and `EffectKind`), never renumbering the old ones;
- every bound and list keyed to the last value: `rg "NativeModule::dynamicEq|EffectKind::dynamicEq" src` finds the range checks (`<= NativeModule::dynamicEq` in `RackLayout.cpp`, `RackDesign.cpp`, `ProposalDocument.cpp`, `CompanionWire.cpp`, `ProgrammerUnit.cpp`) and the `static_assert`s on the name lists, among them `moduleNames` and `effectNames` in `src/ai/AiCatalog.cpp`, which name `module_keys`;
- appended parameter IDs and state keys in `src/state/ParameterRegistry.cpp`, and `rackParameterActive` in `src/racks/RackDesign.cpp`;
- a new sound-state version that keeps every earlier decoder (`src/state/SoundState.cpp` maps each version to its last module);
- the all-controls panel and the PROGRAMMER (`src/studio/AllControlsPanel.cpp`, `ProgrammerUnit.cpp`).

DSP comes first under failing behaviour checks; the suite's capacity and first-attempt policies hold; listening acceptance is the user's.

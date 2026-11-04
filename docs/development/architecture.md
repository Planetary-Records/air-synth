# Source orientation

The shipped instrument and companion are C++20/JUCE. Python and PowerShell under `tools/` support development, measurements and packaging; they are not the shipped audio runtime.

| Area | Responsibility / start here |
|---|---|
| `src/dsp` | Synthesis, effects and routed audio |
| `src/racks`, `src/pattern` | Rack designs/catalog and editable musical patterns |
| `src/state/ParameterRegistry.cpp` | Stable host/control identities and domains; preserve old descriptors |
| `src/state` | Sound/project formats, validation, migrations, presets and undo |
| `src/plugin` | JUCE processor/editor, host audio/MIDI/transport, state publication |
| `src/studio` | Library, rack faces, programmers, pattern UI and visual analysis |
| `src/drums` | Immutable sample assets, bundles, preparation and one-shot playback |
| `src/ai` | Bounded proposals, catalog and request validation |
| `src/companion` | Native IPC, account-backed inference and optional local ComfyUI jobs |
| `probes/oauth` | OAuth code reused by the companion, plus development probes/tests; not all probe code is development-only |
| `resources` | Runtime skin images, fonts, mappings and source manifests |
| `tests/native`, `tests/fixtures` | Native behavioral tests and compatibility fixtures |
| `tools/native-run`, `tools/NativeTests.cmake` | Native process/resource coordination and CTest registration |
| `CMakeLists.txt` | Dependency pin, target links, embedded resources and product deployment |

Start with the caller and its owning tests rather than copying historical execution recipes. Audio callbacks consume prepared/validated state; preserve existing allocation, bounded-work and lifecycle contracts. Control/state changes require compatible recall and stable parameter identities. UI/AI controls reach those same parameters. AI proposals are inspected/auditioned before transactional Apply. Samples are owned by saved state rather than paths into temporary model output.

Read [AGENTS.md](../../AGENTS.md), [contribution guidance](../../CONTRIBUTING.md) and [native work rules](parallel-native-work.md). Private historical acceptance is not a new validation of this independent source tree.

See [research methods and limits](research-methods.md) for the treatment of private investigations.

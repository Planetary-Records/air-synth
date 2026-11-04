# M0 observable host probe

This diagnostic instrument retains the original 160 `probe.p000`–`probe.p159` parameters and MIDI-gated reference sine. `p000` is gain. It is not the production synth or a linked-instance registry.

The upper panel shows the current runtime UUID, stored member/group UUIDs, actual process ID/creation label, process sequence/drop counters and most recent process event. All 160 manual controls remain in the generic editor below. Opening/closing an editor does not own the trace or change identities.

## Test controls

- **Create test group** sets a fresh group UUID; **Copy group** copies it; paste it into another instance's UUID field and use **Join entered group**. **Clear group** removes that instance's test group. These explicit actions preserve the stored member ID. They only seed state for observation; they do not connect peers.
- **Save/Load native sound** uses a parameter-only `.probe-sound` file. Loading preserves the destination's identities. Invalid files are rejected before any parameter changes. Select a filename including the desired extension; the chooser does not silently alter it.
- **Export trace JSON** saves the current process-local ring, up to 256 records, with total sequence and dropped counts. Export after every test operation. Export before closing the last plugin/host process: this in-memory trace is lost when its module/process unloads. It is deliberately not an automatic filesystem logger.
- **Configure 129** should be used with transport stopped and Live Configure enabled. It gestures through p000–p128, temporarily changes and restores each value, and can write host automation. Verify the host's actual count/surface. Manually configure/automate p159 separately. This action does not prove Live accepted 129 controls.

Host full-state loading still restores the serialized member/group IDs exactly. Duplication is observed without automatic collision repair. Loading a Live device preset is separate from the native sound control case above; whether Live exposes origin metadata remains unverified.

## Trace semantics

`construct`, `destroy`, `group.explicit`, `state.load`, `stream`, `channel`, `native_sound.load`, and `configure.p000-p128` are observations. State loads include accepted/rejected status and before/after IDs. The `stream` record contains load/save direction, interface/list availability, numeric SDK query results, raw supplied StateType/path/filename and recognized exact StateType names. The `channel` record includes optional name, colour, UID, runtime ID, index, namespace/order and location.

No state origin or project identity is inferred. Missing/failed queries, null lists, empty supplied values and unknown names remain distinct. Strings are capped at 512 UTF-16 units with truncation flags; file names have the SDK's 128-unit buffer and a `capacity_reached` indicator. A successful full filename buffer cannot establish whether its original source was longer. PID/creation time labels a process, not a project. The latest event may belong to another probe in the same process; each record names its runtime UUID.

## Reproducible build seam

Configure invokes `tools/apply_juce_probe_patch.py` against pinned JUCE. Its checked `juce-patches/host-observation.json` descriptor has exact source preimage/postimage SHA-256, unique replacement contexts and commit identity. Matching postimage is an idempotent no-op; mismatches leave the source unchanged and fail configure. The patch adds an include and three observation calls under `PROBE_HOST_DIAGNOSTICS`, enabled only for `RackSynthProbe_VST3`.

The observer synchronously copies metadata and releases only its acquired `IStreamAttributes` reference. It never reads/writes/seeks the host stream or retains the borrowed attribute list. JUCE's ordinary channel forwarding and state serialization remain intact. Diagnostics use a control-side mutex/ring; `processBlock` and the parameter listener do not record or take this mutex. Native file selection/export is user-initiated on the message thread.

`vstpresetkeys.h` is omitted from the bundled SDK subset. The two literal keys `StateType` and `FilePathString`, and recognized values `Project`, `Default`, `TrackPreset`, come from the [official PresetAttributes reference](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/namespaceSteinberg_1_1Vst_1_1PresetAttributes.html) and [StateType reference](https://steinbergmedia.github.io/vst3_doc/vstinterfaces/namespaceSteinberg_1_1Vst_1_1StateType.html), checked 2026-10-03.

The unchanged full-state format is documented in `docs/verification/m0-native.md`. Native sound is 1,320 bytes: little-endian magic `0x31534e50`, version `1`, 160 pairs of 32-bit fixed index and normalized IEEE float, then SHA-256 of the preceding 1,288 bytes. Group, member, runtime, edit-count and host metadata are absent. SHA-256 is corruption detection, not authentication. As with the existing probe state, concurrent automation is not claimed to produce an atomic all-parameter snapshot.

Build/test and remaining actual-host boundaries are recorded in `docs/verification/m0-host-diagnostics.md`. Installation is parent-owned and is not performed by CMake.

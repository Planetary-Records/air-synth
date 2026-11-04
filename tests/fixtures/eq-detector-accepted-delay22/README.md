# Accepted delay stereo states for detector integration

Captured from main `8540d9858af052e6bd3e0f5c46fe81b0cfd9b633` before the detector codec integration. The independent writer uses that commit's2215 controls, Sound22 and Project21; its three sounds and two projects self-round-trip with main's own codec. These files are fixed migration inputs, never updated from the new decoder.

The rich sound changes all24 stereo controls and every EQ extension selector, with locks and continuous macros. The embedded sound places the published Oxide Return (14 explicit defaults, including NOTE). The projects contain two notes; the rich project also contains group metadata. `registry-v22.json` captures all descriptor fields and choices in index order.

`capture-accepted-delay22.cpp` is development tooling. Reproduce in a private `git archive` of the pinned main: add an executable target for this file, link only `RackSynthProject`, configure with the pinned JUCE cache and probes OFF, and run the writer with a fresh output directory under `RackSynthNativeRun`. `provenance.json` pins the source, executable, original archive and every emitted byte stream. No shipped target or source was changed.

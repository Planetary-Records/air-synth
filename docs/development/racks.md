# Contributing a rack

Propose a musical purpose and a small scope before implementation. Thread Press, Ember Reed and Glass
Current are the reference implementations for appearance, parameter movement and AI exposure.

1. Compose a RackDesign from existing native modules and explicit parameter bindings. Preserve existing
   host IDs, state migrations, undo/recall and offline/manual operation.
2. Sketch closed/open controls, their motion and interaction states. Supply original or appropriately
   licensed art and its authoring inputs. Editable runtime assets and mappings are under resources;
   keep new master art and scripts with the contribution under docs/design. Font licenses remain separate.
3. Integrate parameter motion, rack library/catalog and AI admission. Do not introduce an inference model
   or account requirement for manual playback. Coordinate new DSP/native modules separately.
4. Add focused behavior/state tests and verify the real rendered UI at useful sizes. Distinguish visual,
   listening, host and automated acceptance; no source hash alone proves those results.

For a design informed by another instrument, document measured behavior and your implementation's source.
Do not contribute proprietary binaries, extracted/decompiled code, copied UI or recordings without the
necessary permission. A reference product's availability or a measurement does not establish those rights.
Presets, original musical examples, documentation and testing are useful contributions without new code.

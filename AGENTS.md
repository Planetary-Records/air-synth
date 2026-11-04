# Contributor workspace instructions

Read CONTRIBUTING.md and docs/development/building.md before changing code. Keep one focused change per branch and preserve host parameter identities, state migrations, undo/recall and offline manual playback.

Use the documented probes-off product/fast-test path. Native configure, build and direct tests use RackSynthNativeRun; CTest owns its leases. Reuse a worktree's build directory and serialize writers. Automated windows use a private desktop and do not interrupt a musician's active project or play through speakers.

For a behavior fix, show the failure with a focused regression, implement the smallest complete change and run affected checks. Builds, automated audio, visual review, listening and human DAW checks are distinct evidence. Record the limits honestly.

The public source does not contain private research, runner configuration or historical acceptance packets. Do not invent those results or weaken executable guards to make them appear available. Unknown PR code runs only in an isolated disposable executor, never on a maintainer's development PC.

New racks need original/licensed material, explicit native bindings and recall/undo tests; see docs/development/racks.md. Preserve original third-party copyright/license notices. Do not publish account data, private projects or material without distribution permission.

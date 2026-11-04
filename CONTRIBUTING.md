# Contributing

Start with a small change: a clearer guide, one original preset/MIDI example, a reproducible host test or a focused bug fix. Search issues before opening one. For a new rack, DSP behavior, dependency or substantial UI change, describe the musical goal and scope before implementation. Suggestions and contributions do not create a maintenance commitment.

Read the [community conduct policy](CODE_OF_CONDUCT.md). New here? Follow [your first contribution](docs/development/first-contribution.md). You can also help without compiling anything: the [alpha tester guide](docs/user-guide/testing.md) gives a short first-session check and report instructions. Use the cleared public repository for forks; do not import private development history.

## Source changes

Read [AGENTS.md](AGENTS.md), [architecture](docs/development/architecture.md) and [build/test guide](docs/development/building.md). Preserve active features and stable parameter/state contracts. Use a separate branch/worktree with private build/output directories. The maintainer owns roadmap changes. Do not bundle unrelated refactoring or generated evidence into a fix.

For behavior changes, first demonstrate the failure with a focused test, implement the smallest fix and run affected checks. Record actual commands, exit codes and limitations in the PR. Builds, automated render checks, DAW compatibility, visual approval and listening are different evidence. Native operations use RackSynthNativeRun; CTest coordinates its own tests. Performance work requires unchanged workloads/budgets and a clean measurement, not repeated retries until a pass.

Public PRs do not run on the maintainer's PC. Never ask a maintainer to approve execution of an unreviewed workflow. Review changes statically first; run unknown code only in a disposable environment without secrets, private network access or shared host folders. Paid hosted runners remain disabled. See [CI safety](docs/development/public-ci-safety.md).

## Racks

Follow [the rack contribution guide](docs/development/racks.md). The reference implementations for appearance, parameter movement and AI exposure are Thread Press, Ember Reed and Glass Current. Contour Dynamics, Character Drive/Band Color and Lumen Field are reference-investigation evidence, not visual/binding templates.

A proposal needs a musical purpose, native-module composition, parameter bindings, AI catalog exposure, recall/undo behavior and original artwork provenance. The workflow orders reference investigation where applicable, a brief, hand-drawn closed/open wireframes, generated concepts and owner selection, shared asset/state sheets, native integration, tests and visual acceptance. Coordinate reference investigation and art provenance with the maintainer before a major rack contribution. Documentation or a preset contribution does not require those tools. Never submit proprietary plugin binaries, decompiled source, copied UI art or audio without redistribution permission.

## Presets, examples and testing

- For a preset/MIDI example, supply a descriptive name, musical intent, source version, playback instructions and any expected tempo/mode. Keep output levels usable.
- Include only original or appropriately licensed audio. State authorship, source and permission for every sample/image/font; links and hashes alone are not licenses. A DAW project can embed private recordings and samples.
- For host testing, report Windows/DAW versions, package checksum, sample rate/buffer, steps and observed playback/recall/MIDI behavior. A failed check is useful evidence.
- Documentation, accessibility feedback, issue reproduction and musical examples are welcome without code. Include a short explanation of what the change improves.

## Licensing and review

By intentionally submitting a contribution, you offer your original changes under the repository's AGPL-3.0-only license, unless an explicit existing third-party license applies. You retain your copyright. No copyright assignment, new CLA or support obligation is requested. Identify third-party material and preserve its notices; do not claim ownership you do not have. See [license scope and unresolved provenance](docs/release/licensing.md).

PRs should state the problem, resulting behavior, verification and limits. Maintainers may request a narrower change or decline work that would create unsupported commitments. Do not include credentials, local account folders, personal information, private research outputs or ignored build products.

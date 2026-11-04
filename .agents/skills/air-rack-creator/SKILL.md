---
name: air-rack-creator
description: Help a music producer author an original downloadable rack for the air synth, using its existing sound tools, then prepare an optional reviewed rack-library contribution.
---

# Create an original air rack

Work with the producer in musical terms. Ask what the rack should do, its few main controls and its visual character. Explain the next useful decision in plain language. The producer does not need to write code. This skill is an authoring guide, not an installed rack, an air engine update or permission to publish.

## Establish the actual authoring environment

Use the source archive matching the producer's air installer. Ask for its location when missing; do not silently substitute the private development repository or another version. Read its current rack-authoring instructions and examples before editing. In source distributions that include them, start with `docs/development/rack-library.md`, `racks/`, `src/state/ParameterRegistry.h`, `src/racks/RackPackage.h` and `tools/rack_packages.py`. Follow the source's more detailed creating-racks skill when present. If the needed authoring inputs are absent, identify them and stop rather than inventing a schema, compiled module or successful installation.

The original Thread Press, Ember Reed and Glass Current authoring inputs, when available, illustrate the intended maker-rack workflow. A downloadable rack combines existing sound tools with a recipe, controls, artwork and notices. New engines or native controls need a separately built air release. MCP edits existing sounds; it does not add new DSP code to the installed plugin.

## Design and build

1. Agree on a small musical brief, original name and controls. Check the installed version's available modules and each parameter's actual range and meaning. Bind every visible control to a working parameter. Keep the rest reachable. Do not invent cosmetic controls that change nothing.
2. Show the producer a wireframe before final artwork. Keep a rack's ears, proportions and control readability consistent with air. The host draws its own rails, selection, locks and proposal marks; do not bake those into the faceplate. Separate the background artwork from moving controls and their states.
3. Prepare a new authoring directory with the source version's supported `package.json`, canonical `rack.json`, `skin/` assets, native `screenshot.png` and original notices. Follow its actual manifest, file hashes and package bounds. Keep editable originals separately. Do not reuse another rack's immutable identity or version for different bytes.
4. Use the source's package validator and native admission checks. The initial aggregate rack-package CLI is bound to its frozen eleven-rack cohort: it is not a generic publish command for a new design. For a new individual rack, use the supported individual package-building path after reading its actual interface; if no suitable authoring tool is available, report the limitation instead of editing the frozen cohort or published catalog.
5. Test every control, layout at normal window size, install/placement, save/reopen and sound recall. Capture the actual rack in air. Keep numerical checks, a concept image and the producer's own visual/listening acceptance distinct. Do not claim that a test run or generated artwork proves musical quality.

Use original audio and artwork or material with explicit distribution rights. Keep source/attribution/licence notices with the submission. Do not redistribute reference-plugin internals, private projects, passwords, sign-in tokens or unlicensed samples. Reference-product investigation requires the user's authorization and the environment's appropriate tools; it is not a prerequisite for an original combination of existing air modules.

## Optional submission

When the producer asks to contribute, check the current instructions at `https://github.com/Planetary-Records/air-rack-library`. If it is not public or submissions are closed, explain that and stop. Do not change repository visibility or request access on the producer's behalf.

Prepare a fork, new branch and draft PR with the editable authoring files, package, actual screenshot, short musical description, compatible air version, notices and test results. Follow the repository's contribution convention; if none exists, propose a `submissions/<rack-name>/` directory in the draft rather than assuming automatic acceptance. Do not modify the trusted catalog, tags, existing racks or release assets. Maintainers validate and publish accepted racks.

Show the complete diff and proposed PR description to the producer. Creating or uploading a PR requires their explicit submission instruction. Making a local rack does not authorize messages, uploads or public publication.

Producer manual: `https://air.planetary.music/manual#make-racks`.

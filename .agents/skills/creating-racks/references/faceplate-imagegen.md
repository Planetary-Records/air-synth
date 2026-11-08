# Generating the faceplate concepts

The approved concepts establish the faceplate's appearance. Production artwork comes from clean plates and shared asset/state grid sheets generated to match them; animated controls are never cut out of the concept images. Before any generation, the agent hand-draws and shows closed and open wireframes as described in step 5 of `../SKILL.md`. Generate the closed concept from its wireframe, then the open concept from its wireframe and the approved closed image. Every prompt is written from `rack-brief.json`; every output is checked against the brief and the wireframe. The generator renders the specified design, with no freedom to invent its layout or interaction mechanisms.

## The tool

The merged racks used Codex CLI's `image_gen.imagegen`, driven by `codex exec` with a wrapper prompt on stdin:

```powershell
New-Item -ItemType Directory -Force "$env:TEMP\rack-gen" | Out-Null
$OutputEncoding = [Text.UTF8Encoding]::new($false)
Get-Content -Raw -Encoding utf8 $wrapperFile | codex exec --skip-git-repo-check -s read-only -C "$env:TEMP\rack-gen" -
```

- Run it outside the repository, so Codex does not load this project's `AGENTS.md` and act as a developer.
- The wrapper is the bundle's recorded `wrapper_template` in `docs/design/racks/2026-10-09-studio-rack/prompts.json`, with "image generator", or "image editor" for edits. It tells Codex to call the tool exactly once with the prompt verbatim, and to pass no references or exactly the listed absolute paths as `referenced_image_paths`.
- Read prompt files as UTF-8 and keep prompts ASCII. A wrapper that read ANSI once turned arrows into "???".
- The tool exposes `prompt`, `transparent_background`, `referenced_image_paths` and `num_last_images_to_include`, with no model, seed or size setting. The aspect ratio goes in the prompt's text, and the widest output is 2172 x 724.
- The output lands under `%USERPROFILE%\.codex\generated_images\<session>\`. Copy it into the bundle byte for byte, keeping its SHA256.

An agent with its own image tool can use it with the same prompts; record the tool. Without one, stop and ask the user. Every generation spends the user's credits, so generate what the step needs and no more.

## The closed prompt

Use `thread-press/prompt-closed.txt` for the prompt's structure, with this new reference requirement: pass this rack's `wireframe-closed.png` in `referenced_image_paths`. The merged racks' historical no-reference prompts do not apply. Never pass a merged rack's artwork as an edit target or style reference; that carries its maker into the new face. State that the wireframe controls the silhouette, proportions, control count, positions, sizes, grouping, labels and reveal affordance, while the brief supplies the materials and styling. Its sections, in order:
1. **Use case.** A product UI concept for a software synthesizer whose interface is a 19-inch studio equipment rack, each unit real hardware from a different manufacturer; one unit only.
2. **Asset type.** An isolated front panel as a flat orthographic front elevation: camera square to the faceplate, no perspective, tilt or depth of field, evenly lit like a catalogue photograph, clean enough to be traced into a native plugin UI.
3. **Canvas.** Wide landscape, 3:1. The unit is a standard 19-inch NU unit (state the true proportion: 2U is 5.4:1, 3U is 3.6:1) spanning the full width, centred on plain flat dark charcoal, nothing else.
4. **Maker and theme.** The fictional maker, its era and country, the brief's analog/digital/hybrid hardware theme and interaction vocabulary, and its design language: panel material and finish, legend method and typeface, knob family, switch and lamp style, screws, maker's plate, rack ears, and one adjective line ("Utilitarian, engineered, serious").
5. **Branding.** The maker's plate text, the engraved rack name and the model line, in quotes.
6. **Composition.** Follow the referenced wireframe exactly: the signature object and where everything sits around it. Do not rearrange, resize, add or remove elements, or substitute a generic row of knobs on a flat panel.
7. **Controls, exactly these and nothing more.** Numbered, in the order the composition places them: each control's label in quotes, its kind, its printed scale in real units, and where its pointer sits (the recipe's value). The lamp and what lights it.
8. **Hidden-controls affordance.** What opens the reveal, where it sits, its legend, and that it is closed.
9. **Rendering.** Physically plausible materials, named; crisp legible text spelled exactly as given; exact alignment; high detail.
10. **Avoid.** Perspective, room reflections, wood, meters, screens or LEDs that are not in the brief, extra knobs, switches, jacks or buttons, random or misspelled text, real brand names or logos, watermarks. Forbid controls and reveal mechanisms outside the declared theme, including web-style accordions on analog hardware.

## The open prompt

`thread-press/prompt-open.txt` is the model, with both `wireframe-open.png` and the approved `closed.png` in `referenced_image_paths`. Identify their separate roles in the prompt:
- **Reference images.** The open wireframe fixes the reveal's layout, motion, proportions and controls. The approved closed concept supplies the finished appearance. Keep the NU faceplate faithful, naming its finish, plate, text, sections and controls "in the same positions and settings"; only the specified reveal changes. Retain the same hardware theme in the hidden panel and its controls.
- **Canvas.** Wide enough for the open unit: 3:1 for Thread Press, whose recess opens in place; 3:2 for Ember Reed's drawer, which slides out below; 5:2 for Glass Current's door, which hangs below. Say whether the unit grows.
- **State.** What has moved and how (a cover lifted off, a drawer pulled out on runners, a door flipped down on its hinges), the revealed sub-panel's finish, and its controls exactly, by row or page, with labels, scales and set positions.
- The same rendering and avoid lines.

Name every entry the reveal prints. Glass Current's first open prompt asked for "a small printed parameter chart" without its rows, and the generator filled it with KEYTRACK, filter DRIVE and reverb WIDTH; the edit that fixed it named every row. Ember Reed's first open prompt named its deck's controls but itself asked for GLIDE and POLY / MONO, which are global parameters. Check each control in the brief against the registry before it goes into a prompt.

## Production plates and shared asset sheets

After both concepts are approved, use this rack's approved images as appearance references for production generation. Keep the wireframes as the plates' geometry constraints. Generate:

- **Clean plates** for each required view, with the fixed materials, engraving and scales intact, but every part supplied by the sheets (including its shadow), lit layers and live display text omitted. These provide the surfaces behind every control without hand-erasing controls or filling holes in code.
- **Shared grid sheets** of isolated controls and their required visual states, with `transparent_background: true`. Pack several asset types onto the same sheet. Repeated controls with the same appearance and size use the same asset cells. Use more sheets when the inventory would crowd cells or reduce detail below the native size; one sheet per asset is not the default.

Draw the sheet layout explicitly and pass it as a reference. Specify the canvas, rows, columns, cell dimensions, padding, and the exact asset/state assigned to each occupied cell. Rows may hold asset families and columns their states; longer state sequences can span rows. Keep cell IDs, captions and grid borders in the recorded map or reference guide only, never in the generated sheet.

Enumerate the states the rack actually uses: button rest/pressed (and hover if specified), switch positions, lamp off/on, selector detents and any rotary animation frames. Continuous controls need the planned frame sequence or separate sheet-generated body/rotor layers for native rotation, not merely minimum and maximum pictures. A fader cap may translate as one reusable sprite. Do not invent missing artwork by mirroring a concept's lever, darkening its lamp or cutting and rotating its knob. Generate the required visual states together so their material, lighting, size, alignment and pivot remain consistent.

Review the sheets at 1:1 before slicing: every planned cell is present, alpha is real, padding contains the complete sprite and shadow, and states share a stable pivot without scale or lighting jumps. Composite the cells on the clean plates at the concept's values and at the other states. Check those production composites against the concepts for layout, materials, labels and control identity; retain them as the native renderer's pixel targets. Correct incomplete or inconsistent sheets through image generation; never fall back to extracting controls from the concepts. Record each cell's asset ID, state/frame index, rectangle, pivot within the cell and value/angle mapping for the skin script. Rectangular slicing of these prepared cells is expected; silhouette extraction from concept artwork is not.

## Reviewing an output

Inspect every output at full resolution against the brief before the user sees it, and record the result in `review.md` (Text and values, Mapping, Issues):
- every legend spelled as given, and no extra words beyond a harmless one the review names (Glass Current gained "POWER");
- exactly the briefed controls, each at its drawn value, its scale in the brief's units;
- silhouette, positions, sizes, grouping and reveal mechanism matching the supplied wireframe; no invented layout or convenience widgets;
- theme consistency across materials, controls, readouts and reveal in both states; reject an analog face with a web-style accordion even if its parameters are correctly bound;
- every chart, deck and page entry a real key of its unit's module, with no `global.*` parameter (GLIDE, POLY / MONO), no invented one (KEYTRACK, filter DRIVE, reverb WIDTH) and no meter or waveform;
- the affordance legible, at 3:1 contrast or more against its panel;
- controls big enough to use at the stack's scale (Thread Press's trims are about 20 to 25 px at 1100 px across), or a recorded remedy such as a whole-cell hit area;
- a drawing that misstates the parameter recorded with its fix (RATIO drawn as a stepped switch over a continuous parameter);
- the unit's drawn proportion measured against the wireframe's whole-U heights; fix a mismatch rather than accepting a generator-invented height;
- distinct from the three makers and the house look on every axis in `merged-racks.md`, and recorded in the review's distinctiveness check.

## Fixing an output

- **Edit; do not regenerate an approved image.** Use the image-editor wrapper with the image as its reference: "Edit the referenced image. Keep everything identical: ...", naming what stays, then a numbered list of only what changes, ending "Spell every word exactly as given and add no other words."
- Keep the relevant wireframe in the edit's references as the layout constraint; include the approved closed concept for open-state edits. Update the wireframe and brief first if a layout change is intended.
- Every edit re-renders the whole image: 2 to 4 % of its pixels change, and three of round 3's six edits damaged a detail they were not asked to touch. Diff each output against its parent (pixels changed by more than 24 levels), check the untouched details at 1:1, and stop after three passes on one image. List what remains in the review.
- Keep each superseded draft in `<slug>/drafts/` as the edit's parent, with a note saying why it was superseded. Record rejected outputs in `prompts.json` even if they are not kept.
- If a piece is drawn smaller than its plate and comes out soft, make one registered edit at 2172 x 724 under F12.22's gates (`docs/superpowers/specs/2026-10-14-sharper-deck-and-strip.md`): near-identity affine registration, each control within 4 px, at least 1.5 times the Laplacian energy, and every word checked at 1:1.

## Weak points to plan around

- **Unit pitch.** In multi-unit scenes 1U came out anywhere from 63 to 170 px, and rail numbers skipped or disagreed. The host draws the rails and stacks units at a fixed pitch, so a scene image sets order, states and annotations, not heights. Generate each rack as one unit.
- **Short legends** get misspelled ("CREATE RAIGB"), so check every word.
- **Drawn values** drift from the prompt, so the skin reads the drawn values back from the pointers.
- **Re-rendering.** The open image redraws the closed face slightly differently. Keep one shared clean closed plate and register the clean reveal plate to it; do not harvest controls from either concept.

## Showing the user

Show both wireframes before generating concepts. Show the closed candidates with their paths, written as `/C:/...` with forward slashes, in the turn's final message, and ask which one to take. Options vary only the maker's identity and keep the control list, with each composition fixed by its own wireframe. After the user chooses, generate the open state from that image and the open wireframe and ask again. Record each decision, quoted with its date, in `review.md` and the brief's `status`.

## What to record

- `prompts.json`: per generation `id`, `rack`, `state`, `prompt_file`, the `prompt` as Codex received it, `referenced_image_paths`, `codex_session`, `generated_source` and its `generated_source_sha256`, `output`, `selected`, `parent` and `note`.
- `manifest.json`: every file in the bundle with its bytes, SHA256 and pixel size.
- `review.md`: the findings per image, the distinctiveness check and the user's decisions.
- The editable closed/open wireframes and their raster references, listed with dimensions and SHA256 in `manifest.json`; record the wireframe layout and theme checks in `review.md`.
- The clean plates, shared sheets, editable grid guides and cell/state map, with dimensions and SHA256 in `manifest.json`; record state coverage and assembled previews in `review.md`.
- The prompts themselves, as `<slug>/prompt-closed.txt`, `prompt-open.txt`, and one file per plate, sheet or edit.

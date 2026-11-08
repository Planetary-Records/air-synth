---
name: creating-racks
description: Builds a new AI Rack Synth rack end to end - a RackDesign composed from native modules, a fictional maker's faceplate with generated concepts and shared asset/state grid sheets assembled into a native skin, its parameter bindings, and its exposure to the built-in AI - following the merged Thread Press, Ember Reed and Glass Current racks. A rack referencing another tool, synth, plugin or VST3 requires REA MCP investigation, host parameters and measured renders from the installed copy, shown to the user first. Use when asked to create, design, add or prototype a rack, a rack faceplate or rack UI, or a rack modelled on an existing plugin or VST3.
---

# Creating racks

A rack is a piece of rack-mount hardware from a fictional maker: a `RackDesign` built from the synth's native modules, wearing the maker's own faceplate on the host's 19-inch rails. The built-in AI places it from the library and sets the same parameters its faceplate's controls set.

**A rack modelled on a plugin starts with the plugin.** When the user names a VST3 or product, the first thing done is to understand how that plugin works from the user's installed copy, show the user the evidence, and only then build a similar rack (step 2). No design, maker, brief, drawing or spec comes before that evidence. Never ask whether or how deeply to investigate, and never let the manual stand in for the plugin.

## Copy the merged racks

The maker racks merged before the REA work are the examples for how a rack looks, how its knobs, faders and levers move their parameters, and how the AI reaches it: **Thread Press** (MARLOW & FINCH), **Ember Reed** (HALLORAN) and **Glass Current** (KUROSHIO). Read [references/merged-racks.md](references/merged-racks.md) and open their `closed.png`, `open.png` and native snapshots before designing anything. When a choice is open, do what they did. Their historical extraction of controls from concept images is superseded by the shared asset-sheet workflow below.

These are not examples of a rack's look, its bindings or its exposure to the AI, even once merged:
- The REA-led racks, each modelled on a commercial plugin after the makers: **Contour Dynamics** and its EQ follow-ups (Pro-Q 4), **Character Saturation** with its designs **Character Drive** and **Band Color** (Decapitator), and **Space Reverb** with its design **Lumen Field** (Pro-R). Their REA investigations are the worked examples for step 2 ([references/reference-investigation.md](references/reference-investigation.md)); their faces, graphs, bindings and AI workflows are not examples for anything.
- The factory designs in `factoryRackDesigns()` that wear the house plate, which is the AI Rack Synth's own look. A new rack must not look like it.
- PR #3 (`codex/ui-design-draft`) drew every rack in one plugin vocabulary, differing mostly by colour; the user rejected that direction. Its creator API (`draft-v2`), which its `skills/ai-rack-ui-design` and the `host_api` blocks in the merged `rack-brief.json` files target, was never built: a maker faceplate is the host's own skin of an example design.

## What you need

- **Image generation**, in this agent or through one that has it. The merged racks used Codex CLI's image tool through `codex exec` ([references/faceplate-imagegen.md](references/faceplate-imagegen.md)). Without one, stop and tell the user: a faceplate drawn in code, SVG or JUCE vector shapes is not a substitute. Code or SVG is appropriate for the mandatory hand-drawn wireframes, which precede the generated artwork.
- Whenever investigating another tool, synth, plugin or VST3: **REA MCP is mandatory**, alongside the installed copy and a headless VST3 host for parameter lists and renders where applicable. Follow `reverse-engineer-anything` and [references/reference-investigation.md](references/reference-investigation.md). If the client's REA MCP is absent, broken or blocked, use the user-authorized Windows fallback `cmd.exe /d /c npx -y rea-agents@6.7.0 mcp` and connect over stdio. Successful target-relevant calls through that server satisfy MCP; no new installation question is needed for this pinned launch, including package acquisition. If both MCP routes fail, diagnose/repair and obtain any missing approval for further setup changes. Only if neither route can be made usable, stop, warn the user, describe the missing evidence and ask whether to continue without REA MCP (**not recommended**); wait for explicit approval. Manuals, measurements and ordinary REA CLI analysis do not silently replace MCP.
- The repo's Windows build (`README.md`) for the skin's compare stage, the native tests and pluginval.

## Rules

1. **Every drawn control is one real parameter.** It binds a rack unit and a local key that the unit's module has in `src/state/ParameterRegistry.cpp`. No meter, scope, waveform drawing, table name, chart entry or legend without a parameter behind it. A lamp or LED reads back `enabled` (synth unit) or `bypassed` (effect unit). `global.*` parameters belong to the house unit, never to a rack.
2. **The face's main controls are the design's focused controls**, in order: each is lettered with the control's label and binds its unit and key over its range and response, and stays bound to that one parameter and live in every mode. The faceplate draws its own form: GRIP is a chicken-head, though its `FocusWidget` is the vertical slider the house plate would draw. The face may add the unit's IN / OUT lever and readbacks. `makerFaceFor` (`src/studio/MakerFace.cpp`) shows the faceplate only while the design matches its example in ID, name, role, units and modules, section names and every focused control's fields; values, intent and revision may differ. At most 8 focused controls in 1 to 4 sections. The faceplate engraves the rack's name.
3. **The reveal holds the rest.** A recess, drawer or door shows the voicing the face leaves out. Whatever neither shows stays reachable in the drawer's all-controls panel, which the host adds for every rack. Nothing is AI-only.
4. **The rack is hardware with its own personality.** A fictional maker with an era and a country; whole rack units at real 19-inch proportions; rack ears; a flat front elevation. It differs from the three makers and from the house look (graphite, white legends, no accent colour) in material, colour temperature, knob family, typography, readout strategy, height and reveal mechanism, not in hue alone. Its personality is also its composition: one signature object from the maker's world leads the face, and the layout follows from it. Thread Press's layout with new materials (maker plate at the left, titled sections between rules, a lamp over the IN / OUT switch, a cover at the right) is a recolour; the user rejected Oxide Return's first concept as "too generic" for exactly that.
5. **The host draws its own parts** on every rack: rails, U numbers, the selection light, the fold triangle, the OPERATIONS key, AI: KEEP tape, lock dots and the AI's dashed marks. The faceplate never draws them.
6. **Design the layout before generating pixels; generate animation assets on shared grids.** The agent hand-draws closed and open wireframes from the brief and supplies them as image references (step 5). The image generator renders that design; it must not invent the composition, control placement or reveal. After concept approval, generate clean background plates and one or more grid sheets containing the distinct controls and their required states. Reuse a sheet across asset types and an asset across matching control instances; add sheets when asset count, size or state count needs more room. Never cut knobs, buttons, levers or other animated parts out of the concept images or fabricate missing visual states from those cutouts. Slicing the generated sheets by their recorded cell rectangles is the intended workflow.
7. **The reference plugin's evidence first, then the faceplate.** A rack modelled on a plugin begins with how the installed plugin works, shown to the user with its evidence (step 2); nothing is designed, briefed or drawn before it. After that, draw the wireframes and show the concept promptly after the brief, before DSP work runs long. A rack composed from existing modules needs no DSP change.
8. **Name the status honestly**: concept image, skin built, in the plugin, user accepted. A concept image is not a UI, and passing builds and tests are not visual or listening acceptance.
9. **The theme governs appearance and interaction.** Declare an analog, digital or deliberately specified hybrid hardware theme in the brief. Carry it through every maker control, readout, material and reveal, closed and open. An analog rack uses physical knobs, switches, scales, doors, drawers or covers; a web-style accordion or collapsible section header is not its reveal. A digital rack's displays, buttons and paging must fit its maker and era; digital does not automatically mean web UI. Do not add an element or hiding mechanism merely because a generic software widget is convenient. The host's own parts and all-controls panel retain their contract.

## Workflow

Copy this checklist and keep it current:

```
- [ ] 1. Orient
- [ ] 2. Understand the reference plugin and show the user the evidence (racks modelled on a plugin; before anything else)
- [ ] 3. Compose the design
- [ ] 4. Write the rack brief
- [ ] 5. Hand-draw and show the closed and open wireframes
- [ ] 6. Closed concept from its wireframe, then the user chooses
- [ ] 7. Open concept from its wireframe and the approved closed image, then the user approves
- [ ] 8. Generate shared asset/state sheets and assemble the skin
- [ ] 9. Integrate
- [ ] 10. Verify, then the user's visual acceptance, then the PR
```

### 1. Orient

Read `AGENTS.md`, the active feature in `docs/roadmap-discipline/features.md` (search it; it is large), the rack contract in `docs/design/racks/2026-10-09-studio-rack/design-brief.md`, and [references/merged-racks.md](references/merged-racks.md). Follow the flow the merged racks used: a spec, a plan, test-first tasks, a technical record and one PR per rack. The parent agent owns the roadmap.

### 2. Understand the reference plugin and show the evidence

Whenever the user names a plugin or product, and before anything of the rack itself. Follow [references/reference-investigation.md](references/reference-investigation.md): the design questions, each answered from the user's installed copy (its identity, the host's parameter list, measured renders and **mandatory REA MCP evidence**, with the manual only as a cross-check), mapped onto the native modules, recorded, and shown to the user with the evidence. Do not ask whether to investigate; try the authorized direct MCP fallback before applying any remaining setup or proceeding-without-MCP approval gate. If the plugin cannot be loaded or measured, stop and say what failed. Proceed to step 3 only after showing the evidence and recording any explicit MCP exception and its limitations.

### 3. Compose the design

- Choose the role and the modules from `NativeModule` in `src/racks/RackDesign.h`: `analog`, `wavetable`, `drive`, `delay`, `reverb`, `compressor`, `eq`, `filter`, `chorus`, `phaser`, `flanger`, `stereo`, `dimension`, `frequencyShift`, `convolution`, `frequencySplit`, `midSideSplit`, `dynamicEq`. A synth rack's first unit is its only engine; an FX rack holds effects only. New designs prefer one voice per synth rack and one effect per FX rack (decision 11 of `docs/superpowers/specs/2026-10-15-ai-uses-user-controls.md`); the merged racks predate that and keep their inserts.
- Write the recipe with `value(unit, key, physical)` and the focused controls with `focus(label, unit, section, key, widget, low, high, response)`, as `src/racks/RackExamples.cpp` does. Values are in the registry's physical units; a focused control's range is the useful part that the face prints.
- Write the `intent` (at most 512 characters; the name at most 64) for the AI: what the rack sounds like and what to shape with it. The AI chooses library racks by name, intent and recipe.
- If no module can do the job, the rack needs new DSP. That is a separate feature with its own spec and the user's approval ("A new module" in [references/skin-and-integration.md](references/skin-and-integration.md)). The planned keys' ranges, defaults and laws come from step 2's measurements, not from reasoning. The faceplate concept can go ahead against the planned keys meanwhile.

### 4. Write the rack brief

Fill [assets/rack-brief-template.json](assets/rack-brief-template.json) as `docs/design/racks/2026-10-09-studio-rack/<slug>/rack-brief.json`: the maker, the hardware theme and its interaction vocabulary, the look, every drawn control with its binding, printed scale and drawn value, the reveal and what it holds, the readout strategy, and what is left to the all-controls panel. Specify how the theme governs the reveal as well as the face. Inventory the distinct asset families, their reusable instances, required visual states and shared grid layout; plan additional sheets only where needed for usable resolution and spacing. Every prompt and every review is checked against this file.

### 5. Hand-draw the wireframes

Before calling image generation, explicitly draw the layout yourself with hand-placed lines, shapes and text (for example SVG or a small drawing script), not an image-generation prompt for a wireframe. Follow any layout the user supplied. Save editable sources and raster references as `wireframe-closed.svg` / `.png` and `wireframe-open.svg` / `.png` beside the brief; an equivalent editable format is fine. These are layout drawings, not the final faceplate artwork.

Draw the real 19-inch proportions and whole-U heights, maker/name plate, signature object, every bound control and label, readout, scale area and reveal opener. The open drawing fixes the reveal's motion, added height and hidden controls while keeping the main face in place. Use explicit positions and sizes at the intended rack scale so the generator has no composition to invent. Check parameter coverage, usable spacing and theme consistency, then show both wireframes to the user before generating the concepts. Record the files and review in the brief and `review.md`.

### 6 and 7. Concepts

Follow [references/faceplate-imagegen.md](references/faceplate-imagegen.md): the closed state uses its wireframe as the layout reference; the open state uses both its wireframe and the approved closed image. The brief supplies materials and styling. Review each output at full resolution against both brief and wireframe before the user sees it; reject invented controls, changed geometry or theme-breaking widgets. Show the user the candidates with their file paths and ask them to choose. If they want options, vary only the maker's identity, draw a wireframe for each distinct composition, and keep the control list identical. Do not build a skin before the user approves both concepts. A deliberate layout change starts with an updated wireframe and brief, never with an improvised generation prompt.

### 8. Generate shared asset sheets and assemble the skin

Generate the clean plates and shared asset/state grids using the approved concepts as appearance references, following [references/faceplate-imagegen.md](references/faceplate-imagegen.md). Keep matching states aligned at the same scale and pivot on transparent cells, with explicit state order and value mapping. Do not default to a separate sheet per knob or button. Follow [references/skin-and-integration.md](references/skin-and-integration.md) to slice the sheets and assemble a `<slug>_skin.py` with the stages `measure`, `extract`, `elements`, `compare` and `install`. Borrow the merged scripts' structure and native contract, not their concept-cutout techniques. It installs into `resources/maker-skins/<slug>/` with a `SOURCE.json` of hashes.

### 9. Integrate

Same reference: the example design, the skin's name in `LoadedMakerSkins`, the `RackSynthMakerSkins` binary-data target, new checks written first and seen failing, and the pinned library checks migrated without losing what they check.

### 10. Verify

Build without warnings. Run the touched test targets, the compare stage on the native snapshots, and pluginval with and without its GUI tests; CI must pass on the PR. Before showing any snapshot, check it at 1:1 beside its drawing for overlap, clipped or misspelled text, uneven padding, pointers off their scales, and host parts covering the maker's.

Then look at the rack as the user will, in the standalone app ("The standalone app" in [references/skin-and-integration.md](references/skin-and-integration.md)): place it from the LIBRARY and capture the window at the default 1080 x 680 and at a larger size, with nothing beside the stack, with the AI panel open and with the LIBRARY open, then hover and press the face's controls. Check each capture at 1:1: the rack shows whole with nothing to scroll sideways, its LIBRARY row names its maker and height, and no host highlight or outline sits on the faceplate that its drawing lacks.

Then put the snapshots and the captures to the user beside the drawings. The user's visual acceptance comes before the merge and is recorded apart from the automated results; listening acceptance is separate again, and not needed when the sound does not change.

## How the AI reaches the rack

- **The library.** `libraryRackDesigns()` is `exampleRackDesigns()` followed by `factoryRackDesigns()`. The AI's catalog sends each design's ID, revision, content hash, name, intent, role, capacity, and its units with their modules and recipe keys, units and ranges (`rackCatalogDescriptor` in `src/racks/RackLibrary.cpp`). The faceplate and the focused controls are not sent, so the name, the intent and the recipe carry the rack's meaning.
- **Placing and tweaking.** The AI places a design with `catalog_selections` (its exact ID, revision and hash, and a local ID per placement) and sets values with `catalog_parameter_edits`, addressed by that local ID, a unit and a key: the same unit and key each faceplate element binds. `module_keys` lists every key a unit of each module holds (`src/ai/AiCatalog.cpp`).
- **Reach.** Once placed, every parameter of the rack's units is in reach (`userReach`, `src/racks/UserReach.h`) and shows in the drawer's all-controls panel. An AI edit outside reach is rejected as `outOfReach` and goes back to the model for repair.
- **Review on the faceplate.** A proposal shows on the element bound to each changed key: a knob's dashed pointer at the proposed angle, a fader's dashed cap, a lever's dashed outline. The reveal's opener is outlined while a change hides behind it, and the whole faceplate is outlined when the proposal changes the panel, which would drop the faceplate for the house plate. A control lock is the yellow dot at the element's `dot` place; a rack lock is AI: KEEP tape across the left ear.
- **Nothing rack-specific.** The merged racks reach the AI only through this path: no code under `src/ai/`, prompt text or schema names them. A new rack adds nothing to it either; a new module adds only its name to the catalog's module lists.

So: name and intent describe the sound and its use; the recipe sets every parameter the starting sound depends on; the face carries the controls a player reaches for most, with evocative labels over scales in real units.

## Red flags

Stop and correct course when any of these happens:
- a rack modelled on a plugin designed, briefed, drawn or specified before the installed plugin's evidence was shown to the user;
- asking the user whether, how deeply or with which evidence to investigate the reference plugin, except for setup beyond the authorized direct MCP launch or proceeding-without-MCP approval after both routes and repair fail;
- investigating another tool without REA MCP and without the user's explicit informed exception, or requesting that exception before trying the authorized direct MCP fallback and required connection repair;
- a claim about the reference plugin's ranges, curves or behaviour backed only by its manual, or new DSP's ranges, defaults or laws chosen by reasoning when the plugin could be measured;
- controls drawn before the brief binds them, or concept variants with different control sets;
- concept generation without the agent's hand-drawn wireframe as an image reference, or a generator allowed to invent or rearrange the layout;
- animated controls cut out of concept artwork, missing visual states fabricated from cutouts, or one sheet per control when a shared grid would fit;
- a control, readout or reveal that breaks the declared theme, such as a web-style accordion on an analog rack;
- a concept drawn as a standalone plugin window instead of one rack unit;
- a printed scale that disagrees with its parameter's range or units;
- a legend, display line, chart row, meter or LED with no parameter behind it;
- a faceplate that looks like the AI Rack Synth unit, the house plate, or a recolour of a merged maker;
- a face with no signature object, laid out as a row of like knobs in titled sections, or on another rack's layout;
- a merged rack's image passed to the closed concept as an edit target or style reference, or another maker's knobs, levers or lamps reused as the new rack's parts;
- a face, graph, binding or AI workflow taken from Contour Dynamics, Character Saturation (Character Drive, Band Color) or Space Reverb (Lumen Field) instead of the merged makers;
- a face control that rebinds or goes inactive with a mode, or a reveal opener that opens the all-controls drawer instead of the maker's own reveal;
- hours of DSP or performance work with no concept image yet (the reference plugin's investigation comes first and is not cut short for it);
- regenerating a whole approved image to fix one detail, or more than three edit passes on one image;
- a concept image reported as the UI, or snapshots shown without a 1:1 check;
- a rack put to the user without standalone captures at the default window beside the AI panel and the LIBRARY: the test snapshots showed Oxide Return scrolling sideways there, by the rule of the day, and only the user's own session caught it.

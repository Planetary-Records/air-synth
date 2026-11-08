# The merged racks

Thread Press, Ember Reed and Glass Current are the example maker racks: [PR #20](https://github.com/KadoBOT/ai-rack-synth/pull/20), [PR #21](https://github.com/KadoBOT/ai-rack-synth/pull/21) and [PR #22](https://github.com/KadoBOT/ai-rack-synth/pull/22), with their motion in [PR #24](https://github.com/KadoBOT/ai-rack-synth/pull/24) and sharper art in [PR #25](https://github.com/KadoBOT/ai-rack-synth/pull/25). Each is an example design in `exampleRackDesigns()` (`src/racks/RackExamples.cpp`), a skin in `resources/maker-skins/<slug>/`, and a design record in `docs/design/racks/2026-10-09-studio-rack/<slug>/`. Any other skin in `resources/maker-skins/` is not an example.

Their control-cutout, plate-repair and synthesized-state techniques below are historical implementation notes. New racks follow the [shared asset/state sheet workflow](faceplate-imagegen.md#production-plates-and-shared-asset-sheets), using generated clean plates and grid cells instead of extracting animated controls from concept images. Their appearance, interactions, bindings and AI integration remain the examples.

Look at these first:
- `docs/design/racks/2026-10-09-studio-rack/rack/overview-r2.png`: all three on the rails as approved, with the house unit, AI: KEEP tape and a lock dot.
- `docs/verification/studio-rack-glass-current/stack-glass-marks-open.png`: all three in the plugin below the house unit, Glass Current's door open, AI: KEEP across its ear and an AI proposal's dashed pointer on FACET.
- Each rack's `closed.png` and `open.png`.

## What they share

The look: a catalogue photograph of real hardware, square on and evenly lit. Physical materials, legends engraved or printed on the panel, rack ears with screws, the maker's name and the model line on the face. Labels are evocative (GRIP, AIR, FACET) while the scales and readouts stay in the parameter's real units.

They differ on every axis at once:

| | Thread Press | Ember Reed | Glass Current |
| --- | --- | --- | --- |
| Maker | MARLOW & FINCH Ltd, Sheffield; 1960s British broadcast | HALLORAN Electronic Music Co.; 1970s American analog | KUROSHIO; mid-1980s Japanese digital |
| Panel | blue-gray hammertone enamel, brass maker plate | black brushed aluminium, chrome handles, printed signal-flow arrows | putty-gray steel, smoked display window, triple pinstripe |
| Controls | black bakelite chicken-heads, chrome bat-handle levers, a red jewel lamp | black knurled knobs on aluminium skirts, faders with burnt-orange caps | small rubber-topped knobs with a coloured line, page buttons with LEDs |
| Reading a value | engraved scales in real units | 0 to 10 maker scales, with the host's touch readout | a cyan VFD showing the last control touched |
| Height | 2U, closed and open | 3U closed, 7U open | 2U closed, 4U open |
| Reveal | SET-UP plate lifts off in place (`recess`) | PROGRAMMER drawer slides out below (`drawer`) | EDIT door flips down and hangs below (`door`) |
| Folded | the host's fallback strip | the host's fallback strip | KUROSHIO's own 1U strip |

The host's parts on every faceplate: the rails, U numbers and selection light; the fold triangle on the left rail; the OPERATIONS key on the right rail, which opens the drawer with the all-controls panel; AI: KEEP tape across the left ear; a yellow lock dot at each locked control's `dot`; the AI's dashed marks; and the touch readout beside a control while it is dragged, hovered or focused (Glass Current's VFD replaces it).

How every control behaves, from `HouseControls`: a vertical drag is one host gesture; the wheel, the arrow keys and a double-click to the default are each one complete gesture; a lever flips on a click, Space or Enter. A value outside the printed scale, from automation or the registry's wider range, pins the pointer at its end stop while the readout shows the true value. The processor refuses an edit while another gesture or an AI reservation holds the parameter.

How every reveal behaves: opening and closing write no parameter and make no undo entry; the reveal's controls join Tab order only while it is open; closing with focus inside returns focus to the opener; folding the unit closes it.

## Thread Press: FX, compressor then drive

Intent: "An ordered dynamics then saturation strip for gentle front shaping and parallel density." Unit 0 is a compressor, unit 1 a drive.

| Face control | Binds | Drawn as |
| --- | --- | --- |
| GRIP | 0 `compressor.threshold_db`, -36 to 0 dB | chicken-head |
| FRONT | 0 `compressor.attack_ms`, 1 to 60 ms, logarithmic | chicken-head |
| BLEND | 0 `compressor.mix`, 0 to 100 % | chicken-head |
| DENSITY | 1 `drive.gain_db`, 0 to 12 dB | small chicken-head |
| IN / OUT | 0 `bypassed`, inverted: IN while not bypassed | bat lever; the red lamp reads it back |

The SET-UP recess holds the calibration a broadcast engineer sets once: RATIO, RELEASE and MAKE-UP for the compressor; TANH / SOFT, TONE, MIX and IN / OUT for the drive. The face reads left to right in signal order: DYNAMICS, SATURATION, IN / OUT.

Lessons:
- A chicken-head is not round, so its whole head is the pointer layer; the plate keeps the round skirt, rebuilt where the head covered it.
- RATIO's drawing is a stepped switch, but the parameter is continuous: it turns smoothly through the printed steps and the readout shows 2.5:1 exactly.
- The trims are small at the stack's scale, so each one's hit area is its whole cell, legend included.
- `open.png` re-renders the whole face, so the open state keeps the closed plate and adds only the recess cut from `open.png`.
- The unlit lamp is the lit one darkened; each lever's other position is its drawn bat mirrored about the pivot and foreshortened by the drawing's own ratio.

## Ember Reed: synth, analog voice and drive

Intent: "A warm, articulate pulse and triangle voice with a brief filter bloom and restrained native drive." Unit 0 is an analog engine, unit 1 a drive.

| Face control | Binds | Drawn as |
| --- | --- | --- |
| AIR | 0 `analog.filter.cutoff`, 100 Hz to 10 kHz, logarithmic | large knurled knob |
| BITE | 0 `analog.filter.resonance`, printed 0 to 10 | knurled knob |
| BLOOM | 0 `analog.filter_env.decay`, 20 ms to 2 s, logarithmic | fader, printed 0 to 10 |
| ATTACK | 0 `analog.amp.attack`, 1 ms to 1 s, logarithmic | fader |
| RELEASE | 0 `analog.amp.release`, 20 ms to 2 s, logarithmic | fader |
| GRAIN | 1 `drive.gain_db`, 0 to 12 dB | small knurled knob |
| Pilot lamp | 0 `enabled` | readback only |

The PROGRAMMER drawer's deck holds 24 controls in OSC 1, OSC 2, FILTER ENV, AMP, MOTION and DRIVE, among them VEL TO AIR, which is route 1's amount (`modulation.1.amount`).

Lessons:
- A knob is a still body with its light, a rotor carrying the pointer, and a grip ring rebuilt with evenly spaced teeth and stored as 24 frames over one tooth period, chosen from the angle. Rotating the whole knob rotates its lighting, and a pointer turning over a still ring does not read as a turning knob (the user's first-look feedback on the spike).
- Heights come from measuring the drawings, not from the brief: the brief said a 3U face, the drawing is 2U, so the unit is a 2U face and a 1U drawer front. Whole units come from repeating rows of plain panel, never from stretching controls.
- The deck was cut from an `open.png` drawn smaller than the plate and came out soft; F12.22 sharpened it with one registered Codex edit (`docs/superpowers/specs/2026-10-14-sharper-deck-and-strip.md`).
- A printed route stays true: while route 1 is not velocity to cutoff, the host's console tape covers VEL TO AIR's legend with the route's real name.
- The first drawer prompt asked for GLIDE and POLY / MONO, which are global parameters; one edit replaced them with VEL TO AIR and LFO FREE / SYNC.
- The skin maps BITE's printed 0 to 10 over 0 to 0.95 while the design's focused range is 0 to 0.8. Keep a face control's printed travel equal to its focused range unless the spec records why.

## Glass Current: synth, wavetable, delay and reverb

Intent: "A gently moving factory wavetable voice with independent echo and a longer spatial tail." Unit 0 is a wavetable engine, unit 1 a delay, unit 2 a reverb.

| Face control | Binds | Drawn as |
| --- | --- | --- |
| FACET | 0 `wavetable.osc.1.position`, 0 to 1 | large rubber-topped knob |
| DRIFT | 0 `lfo.1.rate_hz`, 0.05 to 5 Hz, logarithmic | rubber-topped knob |
| ECHO | 1 `delay.mix`, 0 to 50 % | rubber-topped knob |
| HORIZON | 2 `reverb.decay_seconds`, 0.3 to 8 s, logarithmic | rubber-topped knob |
| POWER LED | 0 `enabled` | readback only |
| VFD | the last control touched: name and value, with a bar for a face knob | readback only |

The EDIT door holds six page buttons (OSC, FILTER, ENV, LFO, DELAY, REVERB) with LEDs, an LCD label strip and four soft knobs that rebind with each page's screens; the door's inside carries the printed parameter chart.

Lessons:
- Displays are cut without their drawn letters and lettered live by the native dot-matrix renderer, so the VFD and the LCD always show real values.
- Each screen's soft knob is its own element at the knob's place, shown only with its screen, so a gesture always ends on the parameter it began on. Choices turn by detents, one per choice.
- The first open prompt left the door's chart unnamed, and the draft invented KEYTRACK, filter DRIVE and reverb WIDTH; an edit naming every row fixed it. TABLE shows table numbers only, never invented names or waveform drawings.
- An affordance's legend needs at least 3:1 contrast; EDIT was drawn too faint and the native build had to reach it.

## What the AI sees of a merged rack

Thread Press's catalog entry carries its ID, revision, content hash, name, the intent above, the role `fx`, and its two units. Each unit lists the design's own `value()` keys with their units and registry ranges: the compressor's `threshold_db`, `ratio`, `attack_ms`, `release_ms`, `makeup_db` and `mix`, and the drive's `shape`, `gain_db` and `mix`. The panel's focused controls are not sent, so the intent and the keys are all the AI knows of the rack's character.

The AI places it with a `catalog_selections` entry and edits it with `catalog_parameter_edits` on a unit and key. Any key active for that unit's module is accepted, the ones the design leaves unset included (`drive.tone_hz`, which the SET-UP plate's TONE turns); `module_keys` lists them. When a proposal sets unit 0's `compressor.threshold_db`, GRIP shows a dashed head at the proposed angle until the proposal is applied. If it sets `drive.tone_hz` while SET-UP is closed, the cover is outlined. If the user locks GRIP's parameter, its yellow dot appears beside GRIP's scale and the AI must keep that value.

## Files per rack

| | Thread Press | Ember Reed | Glass Current |
| --- | --- | --- | --- |
| Spec | `docs/superpowers/specs/2026-10-13-maker-faceplates.md` | `docs/superpowers/specs/2026-10-13-ember-reed-faceplate.md` | `docs/superpowers/specs/2026-10-13-glass-current-faceplate.md` |
| Technical record | `docs/verification/2026-10-13-maker-faceplates-technical.md` | `docs/verification/2026-10-13-ember-reed-faceplate-technical.md` | `docs/verification/2026-10-13-glass-current-faceplate-technical.md` |
| Skin script | `thread-press/skin/thread_press_skin.py` | `ember-reed/skin/ember_reed_skin.py` | `glass-current/skin/glass_current_skin.py` |
| Snapshots | `docs/verification/studio-rack-faceplates/` | `docs/verification/studio-rack-ember-reed/` | `docs/verification/studio-rack-glass-current/` |

The skin scripts are under `docs/design/racks/2026-10-09-studio-rack/`. The bundle's `prompts.json`, `manifest.json` and `review.md` record every generation, its review and the user's decisions. Reveal motion is in `docs/superpowers/specs/2026-10-13-faceplate-motion.md`.

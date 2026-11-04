# Sound27 / Project26 witnesses before Era Reverb

Written on main's wavetable warmth tree at `2f762569` (3,539 parameters, 17 effect banks), in Debug and in Release, by `RackSynthEraReverbFreeze27`, a copy of this test's `--freeze`, `warmSound` and render built against that tree, where Era Reverb does not exist. Era Reverb took format 28 / 27 when its branch merged main, because the wavetable warmth append had taken 27 / 26 first. Both configurations wrote byte-identical states and identical render hashes, and a second Debug run wrote the same bytes.

- `default`, `copper-arc`, `lumen-field` (as in `era-reverb-sound26`) and `warm`, the wavetable warmth legacy "rich" sound: synth slot 2 at revision 2 with every `synth.2.wavetable.*` key, the nine warmth keys included, away from its default. Signed `*-v27.soundstate` and `*-v26.airackproject`.
- `registry-v27.json`: every registry row of format 27.
- `renders-<configuration>.json`: SHA-256 of both float channels of the same two-second 48 kHz engine render. `default`, `copper-arc` and `lumen-field` equal their format 26 hashes.

`--legacy` decodes these bytes, checks the frozen values, the upgrade to the current format and the unchanged renders. Preserve them when the writer advances.

# Sound26 / Project25 witnesses before Era Reverb

Written by `RackSynthEraReverbProductTests --freeze` on the tree at `aea90a63` (Space Reverb's main format, 3,503 parameters, 17 effect banks), in Debug and in Release, before any Era Reverb registry or state change. Both configurations wrote byte-identical states and identical render hashes.

- `default`, `copper-arc` and `lumen-field` (Copper Arc with the Lumen Field factory rack placed on the master chain): signed `*-v26.soundstate` and `*-v25.airackproject`.
- `registry-v26.json`: every registry row of format 26.
- `renders-<configuration>.json`: SHA-256 of both float channels of a two-second 48 kHz engine render (notes 48/55/60 held half a second, 64-sample blocks).

`--legacy` decodes these bytes, checks the frozen values, the upgrade to the current format and the unchanged renders. Preserve them when the writer advances.

# Optional local generation setup record

The plugin expects a separately operated ComfyUI service at http://127.0.0.1:8189. It does not install or
start that service, inspect which weights ran or provide an account/model entitlement. Manual import and
saved-kit playback do not require it. No model or Python runtime is bundled with the instrument.

The prior research setup used ComfyUI 0.42.0 at 1a82e6d01f395b43275c77acd289af1eb7090bb9,
Python 3.13.14 and Torch 2.14.1+cu130. This is a recorded existing-environment condition, not a clean
installation recipe, minimum GPU requirement or a new compatibility check.

Files below are relative to ComfyUI's models folder. Obtain them separately subject to their terms:

| File | Bytes | SHA-256 |
|---|---:|---|
| checkpoints/stable_audio_3_small_sfx.safetensors | 2270384940 | ed9cf1b6172f1a8c2921a9560c21109ff3239524563ced9dce6dcdef41e2f515 |
| text_encoders/t5gemma_b_b_ul2.safetensors | 1187264003 | 1e1eba25be8872edb0d3c6335c6658fd6388e7b14b60da6e454e404cfcd8150e |

The fixed plugin recipe uses ten-second audio, eight LCM steps, the simple schedule, CFG 1 and FLAC output.
See the [Tile Array guide](tile-array.md#optional-local-sample-generation) for source/terms links and account
caveats. Clean setup/resource qualification remains open. The private research bootstrap is not a public
installer; no isolation guard was weakened or copied into an unsafe generic launcher.

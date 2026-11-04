# Tile Array

Tile Array is a sixteen-pad instrument for individual drum hits. Choose **Tile Array** in the rack library and place it in a synth rack. A new kit starts empty.

## Play and edit a kit

Click a pad to select and play it; Shift-click selects without playing. Open its **FILE + VOICE** programmer to edit the selected pad. The sixteen pads initially use MIDI notes **36–51**. Change each pad's MIDI note to match your controller or DAW sequence. MIDI velocity controls hit level.

Use the existing pattern editor for drum sequences: add notes at the pad's assigned pitches and edit their timing and velocity. Double-click adds a note, dragging moves it, its right edge changes duration, and Alt-drag changes velocity. Choose **Internal Pattern** for playback with the DAW's running transport and tempo, or **External MIDI** for incoming MIDI. Each drum hit is a one-shot; trim and decay shape its length.

The selected pad has gain, pan, tune, trim start/end, attack, decay, MIDI note and choke group controls. Pads in the same nonzero choke group cut off earlier hits in that group. Group 0 leaves them independent. **KIT LEVEL** and **KIT PAN** affect the whole kit.

## Import and keep a hit

1. Select a pad, open its programmer and choose **IMPORT**. Pick a WAV, AIFF or FLAC file.
2. Drag across the source overview to select the hit, or enter exact **START** and **END** frame numbers. START is included; END is excluded. Choose no more than ten seconds.
3. Set a kept name and the source **GAIN multiplier**, then choose **Prepare**. The multiplier must be greater than 0 and no more than 16. If preparation reports a peak above 1, lower the multiplier and prepare again.
4. Choose **Audition** to hear the prepared source before pad trim, voice controls and effects. Choose **Keep PAD** to replace the captured pad, or **Cancel** to leave the kit as it was. Use **Undo** to reverse a kept replacement.

The source gain multiplier prepares the sample; the pad's gain control changes playback level afterward. Changing the source selection or gain requires another Prepare before Audition or Keep.

Supported sources are mono or stereo at 8–384 kHz. Manual source files can be up to **256 MiB** and **ten minutes** long. Each generated candidate is limited to **16 MiB**. The part you keep is always at most **ten seconds**; a generated file can extend slightly beyond that, so shorten its selection before preparing it.

## Optional local sample generation

In the selected pad's programmer, choose **GENERATE**, describe one isolated hit and press **Generate**. A request attempts three candidates. Choose an available candidate row or **TAKE 1–3**, then select the source range, name it, set gain, Prepare, Audition and Keep as above. Only Keep changes the captured pad. Finish or cancel the current source or AI request before starting another. Cancel discards this batch; the local service may finish work already submitted.

Sample generation needs the existing local ComfyUI setup running at **`http://127.0.0.1:8189`**. The address and recipe are fixed in this version. ComfyUI needs these files in its model folders:

| Folder under `models/` | File |
|---|---|
| `checkpoints/` | `stable_audio_3_small_sfx.safetensors` |
| `text_encoders/` | `t5gemma_b_b_ul2.safetensors` |

The fixed recipe requests Stable Audio 3 Small SFX, ten-second audio, eight LCM steps, the simple schedule, CFG 1 and FLAC output. The [sanitized local setup record](local-generation.md) describe the existing configuration. The plugin uses that running service; it does not install or start it, or confirm which model the service actually ran.

The verified research setup used ComfyUI 0.42.0 at `1a82e6d01f395b43275c77acd289af1eb7090bb9`, Python 3.13.14 and Torch 2.14.1+cu130 on the development NVIDIA GPU. This is an existing-environment result, not a tested general installer or minimum GPU requirement. A clean setup guide and resource requirements remain public-preview gates for this optional feature. Manual import/playback do not require any of these tools.

Obtain models separately from [the pinned Comfy-Org distribution](https://huggingface.co/Comfy-Org/stable-audio-3/tree/75aad8836271cb5a5123fb58304fae5d3bb4b0f9) and compare both sizes/hashes with the setup record. The checkpoint is about 2.27 GB and text encoder 1.19 GB, before runtime/cache/output space. Stable Audio 3 uses the [Stability AI Community License](https://huggingface.co/stabilityai/stable-audio-3-small-sfx); T5Gemma has separate Gemma terms. These are not the instrument's AGPL license. Review current model/access conditions; a download source may require a Hugging Face account and terms acceptance. No such account/acceptance is needed for saved-kit playback, and this product supplies neither model entitlement nor a token. The recorded download used public access without account registration. Models and ComfyUI are not included in the instrument ZIP.

The pad's Generate makes sample audio locally. The main **AI panel** instead proposes sound, rack, control and editable-pattern changes through the companion and connected account. Its Generate/Refine/Apply workflow is described in the [first-session guide](getting-started.md#optional-cloud-ai).

## Save and reopen

Save a **sound preset** to reuse the kit, kept samples and sound controls. Loading a sound preset preserves the destination's pattern and playback mode. Saving the **DAW project** also saves the pattern and mode. Kept sample audio travels with either saved state, so it does not depend on the original import file or a running generator. Candidate takes you have not kept are temporary. Saved kits, manual editing, MIDI and pattern playback work without ComfyUI or an AI account.

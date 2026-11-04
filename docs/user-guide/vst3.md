# Use air in your music program

air is a Windows x64 VST3 instrument. Add it to an instrument/MIDI track, then send it notes. [Install it first](install.md). These host instructions follow the music programs' manuals; air's human compatibility checks in the three hosts remain unfinished.

Choose your host: [Ableton Live](ableton-live.md), [Reaper](reaper.md), [FL Studio](fl-studio.md).

## First sound

Open air and press **Start with a sound** if the rack is empty. Choose **External MIDI**, then play your controller or a short MIDI clip on the track. Check the track's monitoring and output if it is silent. See [first session](getting-started.md) for saving and editing.

The installed folder may be called `AI Rack Synth.vst3` while the instrument appears as **air**. Keep the whole folder together and avoid duplicate scanned copies.

## Pattern playback

Choose **Internal Pattern**, add notes and start the host transport. Host tempo drives the pattern. Choose **External MIDI** when the host clip or controller should play the instrument instead. Export MIDI from air's pattern editor when you want to edit its phrase in the host.

Save the DAW project to keep the complete sound, notes and mode. A saved sound file restores the sound while preserving the destination's existing pattern and mode.

## Several instances

You can use different air instances for different parts. A group can share musical context within the same music program: create a group, then use **Refresh/Join** in the other instance. Explicitly choose which peers may supply context. Only the instance making a generation request applies its result.

After a project recall, group participation may need **Resume** before it shares context again. Groups work within one music program. Manual playback does not require a group or an account.

## Optional generation

Read [the generation steps](getting-started.md#optional-cloud-ai) and inspect/audition each proposal before Apply. Account permission and availability can vary. If generation is unavailable, continue with manual controls. [Privacy](../PRIVACY.md) explains what an explicit request sends.

## Help

For missing plug-ins, silence, recall, update or account problems, start with [FAQ](faq.md). Click the **ALPHA** badge to preview a problem report. The [help page](../SUPPORT.md) explains what to share.

# First session

air is an early **alpha, before beta**. Use a copy of a project while testing and save your work often. These pages cover the next alpha candidate; the guide included with an earlier download describes that version.

## Hear a sound first

1. [Install air](install.md) and add it to an instrument/MIDI track. Host steps: [Ableton Live](ableton-live.md), [Reaper](reaper.md), [FL Studio](fl-studio.md).
2. Open air. If the rack is empty, press **Start with a sound**. It places a playable built-in synth rack.
3. Choose **External MIDI**. Play a few keyboard notes, or draw a short MIDI clip in your music program and play it.
4. If you hear nothing, check the track's monitoring, MIDI input and audio output. Start at a comfortable listening level. [Troubleshooting](faq.md#why-is-it-silent) covers the common causes.
5. Move a rack control and hear the change. Use **Undo** and **Redo** to compare edits.

This works without signing in, downloading extra racks or setting up sound generation.

## Keep the sound

Save your **DAW project** to keep the complete sound, notes and playback mode.

To reuse the sound in another project, press **SAVE** in air. For a first save, choose a folder and filename in the save dialog. Later SAVE presses update the selected sound file. To load a saved sound, choose **BROWSE → Open a saved sound...**, choose its file, then press **LOAD**.

Loading a sound file changes the sound while preserving the destination's existing pattern and playback mode. Saving and reopening the DAW project restores all of them. Kept samples in a saved kit travel with the sound/project; unkept candidate takes do not.

## Write a pattern

Choose **Internal Pattern**. Double-click in the pattern editor to add notes. Drag a note to move it, drag its right edge to change its length, and Alt-drag to change its velocity. Start the DAW transport to hear it at the host tempo.

Choose **External MIDI** again when the DAW clip or controller should supply the notes. Keep one intended note source to avoid doubled notes. Export MIDI from the pattern editor when you want to edit the phrase in the DAW. [Tile Array](tile-array.md) uses the same editor for its drum-pad notes.

## Optional cloud AI

Sound generation is experimental. For the built-in GPT feature, this alpha supports a paid ChatGPT subscription; free-account access is not supported in this alpha. Account permissions and usage limits still apply, so a paid subscription does not guarantee access. Manual controls and saved sounds stay available without it. The [illustrated producer manual](https://air.planetary.music/manual#chatgpt) explains sign-in; its [external-assistant guide](https://air.planetary.music/manual#mcp) explains the separate MCP connection, which does not require signing in inside air.

1. Open the **AI** panel and choose **Sign in**. Complete the browser flow. Use **Account → Use another account…** to add a different account, or choose a saved account to return to it.
2. Choose the available generation option and set the role, key, scale and tempo for the part. Choose **Sound**, **Pattern** or **Both**. Enable **Create editable notes** when you want a new phrase.
3. Enter a description such as **a soft pluck**, then press **Generate**. You can start from an empty rack.
4. Before Apply, new racks appear dimmed with dashed outlines. Changed controls on existing racks show dashed targets. Read the proposed settings and press **AUDITION** to hear the candidate.
5. Choose **Apply** to keep it, or **Discard** to preserve your existing sound. Apply is one undoable change. **Refine** starts another explicit generation request; turn off note creation when you want to keep the phrase.

A confirmation email alone does not mean the connection finished. If a sign-in fails, an account entry marked **finish sign-in** lets you retry that registration. If the browser instead says a required permission is unavailable, stop repeating the same sign-in: continue manually or choose another saved account. You do not need to upgrade an account to use air's manual controls. See [privacy](../PRIVACY.md) for what an explicit generation sends.

## Next steps

Choose one of twelve starter sounds from **BROWSE**, such as **Soft Pluck**, **Round Bass** or **Velvet Pad**. Selecting its name loads it immediately; Undo returns to the previous sound. Your existing notes and playback mode stay in place. SAVE opens a dialog for your own copy.

Try a second synth rack, add an effect, or build a drum kit with your own samples in [Tile Array](tile-array.md). The extra rack library is still being prepared for new users. The starter sounds and first-sound button work with the built-in racks.

To try complementary parts across instances, read [groups and context](vst3.md#several-instances). For a problem or suggestion, click the **ALPHA** badge and follow [help](../SUPPORT.md). Reports are shown to you before you share anything.

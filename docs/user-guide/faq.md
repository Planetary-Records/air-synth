# Questions and troubleshooting

These pages cover the next alpha candidate. Use the guide bundled with an earlier download for that version.

## Do I need an account?

No account is needed for manual sounds, MIDI, saved sound files or your own imported samples. Optional generation is experimental and needs an available, permitted connection. A confirmation email alone does not prove that connection finished. If the browser refuses a required permission, use manual controls or another saved account; repeating the same sign-in does not change the workspace's permissions.

## Why is it silent?

- An empty rack is silent. Press **Start with a sound**.
- For a DAW clip or controller, choose **External MIDI** and check the track's MIDI input, monitoring and output.
- For air's notes, choose **Internal Pattern**, add notes and start the DAW transport.
- An empty Tile Array pad is silent; import a sample and play that pad's assigned note.
- Check the rack, track and master output levels at a comfortable monitoring volume.

## Why can't my DAW find air?

Use a Windows x64 host with VST3 instrument support. Close the host, install the complete bundle and rescan. The folder is called `AI Rack Synth.vst3`; the instrument appears as **air**. Check the host's disabled/failed plug-in list. There is no separate runtime download for this alpha. [Host setup](vst3.md) has the scanning steps.

## What do the dashed racks and controls mean?

They show a proposed change before Apply. A dimmed rack can be a proposed addition; dashed control targets show changes to an existing rack. Inspect its placement/settings and use **AUDITION**. **Apply** keeps it as one undoable change; **Discard** keeps your accepted sound. If the proposal is not useful, discard it and try a clearer description or edit manually.

## Why is a generated sound quiet?

Generated levels can vary. Check the proposed output setting and audition before Apply. After Apply, you can edit the output control normally. You can also discard the proposal. Do not assume every generated sound has the same loudness.

## Why does playback crackle or lag?

Try a larger audio buffer in your music program and close unused instruments. Some quality choices and effects add delay or need more processing time. Bypass the last effect you added and compare. If the problem remains, note the sound, sample rate and buffer setting in your report. Save before changing the project further.

## What if air or my music program crashes?

Reopen a copy of your last saved project. If the problem repeats when air is loaded, try an empty project and report the smallest set of steps that repeats it. Include the air version, music program and the exact error text. Do not overwrite your only saved project while investigating. You can [report a problem](../SUPPORT.md) even if air cannot open.

## How do I save a sound?

Save the DAW project for the complete sound, notes and mode. To reuse only the sound, press **SAVE** in air; a first save opens a file dialog. Later saves update that selected file. Choose **BROWSE → Open a saved sound...**, select the file, then press **LOAD**. Sound-file loading preserves the destination's pattern and playback mode.

## Where are the starter sounds?

Open **BROWSE** and choose a name from the twelve included sounds. That loads it immediately; Undo brings back your previous sound. Your current notes and playback mode stay in place. These sounds need no sign-in or extra download. SAVE opens a dialog for your own copy.

## Why is the rack library unavailable?

The extra library remains private during the current testing stage. Built-in racks still work offline. When public downloads are available, an unavailable connection should explain that the library cannot be reached; try **REFRESH** later. Installing a rack does not add it to the current sound until you choose **Add**.

## Where are my files?

Your DAW project stays wherever you save it. Sound files use the folder and filename you choose in air's save dialog. Exported MIDI goes to the location you choose when exporting. **Save to Desktop** puts a problem report on your Windows Desktop. Uninstalling air keeps these user files.

air also keeps preferences and account data in your Windows user folder. Those files are not sound backups and should not be attached to a report. See [privacy](../PRIVACY.md) if you deliberately need to remove account data.

## Can I use it on a Mac, Linux or an ARM PC?

Those versions are not available. The current alpha package is Windows x64 VST3.

## How do I report a problem?

Click **ALPHA**, describe what happened and review the report. You can copy or save it; air does not send it automatically. The report webpage is still being prepared. Keep the report for your testing conversation, and see [help](../SUPPORT.md). Avoid private projects, account files and audio you cannot share.

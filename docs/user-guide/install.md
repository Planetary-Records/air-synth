# Install and update air

air is an early **alpha, before beta**, for Windows x64. Keep copies of your projects and saved sounds. Current installation checks cover Windows 11; other Windows versions and individual music programs still need testing.

Use the Download button on the [air page](https://air.planetary.music/) for the versioned Windows installer. It downloads the executable attached to the matching GitHub release. The same release includes a manual-install ZIP, source and checksums. Use the guide included with that download; older alpha guides describe their own version.

## Recommended: Windows installer

1. Download the versioned `air-<version>-Windows-x64-Setup.exe` and its checksum file from the same release. [Check the download](checksums.md).
2. Save your music project and close every music program or standalone window using air.
3. Open the installer normally. You do not need to select “Run as administrator” yourself; Windows may ask for permission during installation. The alpha installer is unsigned, so Windows may show an unknown-publisher warning.
4. Follow the installer. It places air in the shared VST3 folder and includes the files it needs. No separate runtime installation, account or sound-generation setup is needed for manual sounds.
5. Reopen your music program and rescan its plug-ins if needed. Find **air**, add it to an instrument/MIDI track, then follow [first session](getting-started.md).

The installer does not close your music program or restart it for you. If an update says air is in use, save and close the host, then retry. It leaves a real active client alone rather than forcing it to close.

## Alternative: ZIP installation

Use the installer above unless you normally manage plug-ins yourself.

1. Download and [check](checksums.md) the matching Windows x64 ZIP.
2. Extract it, then close every program using air.
3. Copy the **whole `AI Rack Synth.vst3` folder** to `C:\Program Files\Common Files\VST3`. Windows may ask for permission. Keep its `Contents` folder and all files together.
4. Reopen your music program and rescan. Custom locations work only when your host supports and scans them.

The folder's older name is intentional; the plug-in appears as **air**. Do not install two renamed copies of the same plug-in. Keep `AI Rack Synth Companion.exe` beside the inner module; it supports optional generation. There is no separate runtime download for this alpha.

## Update or return to an older version

Keep a backup of projects, sound files and the previous installer or ZIP. Close all programs using air, install the new version in the same location, then rescan if necessary. For a manual copy, replace the complete bundle.

To return to an earlier version, restore its complete bundle and your backed-up projects. Older versions may not understand sounds or projects saved by newer ones. Renaming a bundle does not make it an independent instrument.

## Uninstall

Close programs using air. For an installer-managed copy, open **Windows Settings → Apps**, find **air (VST3)** and uninstall it. For a manual ZIP copy, remove the installed `AI Rack Synth.vst3` folder yourself.

Uninstall removes the program files while keeping your sounds, projects and other user data. Account data is also retained. The alpha has no in-app account-removal control; see [privacy](../PRIVACY.md) before deleting account files yourself.

## Rack library packages

The extra rack library remains private during this testing stage, so a new user cannot download its extras yet. The two built-in synth racks and fifteen effects work offline. **Start with a sound** does not need a library download.

When the library is available, installing a rack and adding it to a sound are separate actions: choose **Install**, then **Add**. Projects keep their saved rack settings and owned samples. Existing projects do not silently switch to a newer rack version. See [FAQ](faq.md) and [help](../SUPPORT.md) if a rack cannot be reached or removed.

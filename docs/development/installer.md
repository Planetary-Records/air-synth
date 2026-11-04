# Windows installer development

The installer wraps an already built, hash-bound complete VST3 package. It does not compile the instrument, download a runtime, sign, upload or install automatically. Use [release packaging](../release/README.md) to bind the exact source, version, module/helper and guides first.

Prepare with a fresh output directory:

```powershell
py -3.12 tools/release_installer.py --package 'C:/path/air-VERSION-Windows-x64.zip' --sha256 'REVIEWED_FULL_SHA256' --output build/installer-preparation
```

Preparation checks the entire ZIP manifest, hashes, allowed payloads, paths/links/collisions and prerelease version before writing output. Fixture tests require no real native binary or installer compiler: `py -3.12 -m unittest discover -s tests -p test_release_installer.py`.

To compile, explicitly supply an already installed reviewed Inno Setup compiler with --iscc. Its original license is separate from project AGPL terms and retained under licenses/third-party/inno. Compilation does not run the resulting installer. Use a separate fresh output for each candidate.

Normal setup places the legacy-named complete bundle in the shared x64 VST3 folder, requires Windows administrator permission and installs guides/notices under Program Files. It does not force-close/restart music programs, create a startup task or delete user projects/accounts. Shipping alpha uses probes off and a statically linked C++ runtime; no separate producer runtime installation is needed.

The isolated-test option has its own destination/uninstall identity and is not a distributable installer. Actual protected-machine install/upgrade/uninstall, warnings and user-file preservation need separate receipts on the final candidate. Never rehearse by overwriting a musician's working copy. The alpha is unsigned; describe observed warnings without disabling protection.

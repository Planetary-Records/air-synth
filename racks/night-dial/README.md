# Night Dial

A moving wavetable voice with analogue warmth for cinematic future garage: wide drifting pads, slow swells, evolving textures, reese and sub bass, vowel choirs and soft plucks.

Maker: ELMSTEAD SIGNAL WORKS. Package: `night-dial` version `1.0.0`. Requires AIR 0.1.0 or a compatible newer release.

## Editable files

- `rack.json`: native modules (wavetable), their defaults and the panel's parameter bindings.
- `package.json`: package identity, requirements, tags and the allowlist of exact runtime files.
- `screenshot.png`: native rack preview.
- `skin/` when present: declarative faceplate, control positions/motion and image parts.
- `notices/`: original licenses and font/artwork notices; keep these with every revision.

The audio algorithms are supplied by AIR. A recipe or skin contribution does not need a new DSP binary. New native algorithms or editor types need an AIR release.

## Try a change

Edit the recipe or skin, bump the package version, and bump the recipe revision when its design changes. From the contribution repository root:

```powershell
py -3.12 tools/rack_packages.py prepare --source racks/night-dial
py -3.12 tools/rack_packages.py build-rack --source racks/night-dial --output "$PWD/packages"
```

With the compatible `RackSynthRackPreview` developer executable, run `preview --source racks/night-dial --executable <absolute-executable> --workspace <new-absolute-folder>` through `tools/rack_packages.py`. Add `--check` for the bounded native editor capture, audio and recall check. Interactive preview uses the actual native rack controls and an audition keyboard. Application session/save/export/feedback/AI surfaces are excluded; no companion, MCP service or remote catalog is used. Choose a fresh workspace for every iteration. Installed racks and existing project files stay separate.

Describe the musical change, include a native screenshot and report the checks you actually ran. Follow the repository's CONTRIBUTING.md. Never change an immutable released source directory or submit private authoring maps/prompts.

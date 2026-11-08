# Contributing a rack

Fork this repository, make a branch, and choose one folder under `racks/`. Start with Thread Press, Ember Reed or Glass Current to understand recipes, bindings and moving controls. A documentation improvement or small binding/default correction is a useful first PR.

1. Explain the musical goal. Edit `rack.json` for modules/defaults/control bindings and `skin/` for appearance. Keep original notices. Check the rack's README for its module requirements.
2. For a changed release, increase `package.json`'s semantic version. Increase the recipe revision when its design changes; preserve the design UUID for a revision of the same rack. A new rack needs its own lowercase ID and design UUID. Never edit `releases/` or overwrite an existing published version.
3. Run these commands from the repository root, substituting the chosen folder:

```powershell
py -3.12 tools/rack_packages.py prepare --source racks/thread-press
py -3.12 tools/rack_packages.py build-rack --source racks/thread-press --output "$PWD/packages"
py -3.12 -m unittest discover -s tests -p test_rack_authoring.py -v
```

`prepare` explicitly refreshes hashes in `package.json` after validating the listed files. `build-rack` validates edited files and produces deterministic data; it leaves source files alone and refuses different bytes under an existing output ID/version. It does not install anything. Native AIR admission is authoritative for recipe and skin behavior.

4. Preview with the native developer tool as described in the rack README. Use a new workspace for each preview. Confirm control movement, recall and the musical result. Keep screenshots at useful sizes; numerical checks do not establish listening or visual acceptance.
5. Open a pull request with the musical intent, changed files, actual checks, screenshot and original asset authorship/license information. Do not commit build output. Maintainers review source before running contributions; there is no automatic execution on the maintainer's PC.

Do not include credentials, private prompts, development source maps, proprietary plugin code, copied interface artwork or unlicensed audio. Supply original artwork authoring inputs with a contribution when available, and agree their review with the maintainer. Each release retains the original notices. Contributing original work offers it under this repository's AGPL-3.0-only license without copyright assignment or a new CLA.

Publication is maintainer-operated: reviewed source and exact archived inputs are staged together, old immutable source trees are retained, then the source index is selected. A PR does not install or activate a rack for users.

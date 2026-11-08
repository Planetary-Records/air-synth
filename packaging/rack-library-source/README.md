# AIR rack sources

Browse and contribute AIR racks here. Each folder under `racks/` contains readable module recipes, defaults, control bindings, faceplate assets, screenshots and original notices. Downloads remain available in Releases and through AIR's Library.

An `.airrack` is a standard ZIP of data. AIR supplies the shared native audio engines; these folders do not contain independently compiled DSP plugins. New combinations, bindings and skins can be contributed here. A new native algorithm or editor requires a change to AIR itself.

Start with [CONTRIBUTING](CONTRIBUTING.md). Python 3.12 or later and the standard library are enough to prepare and build a rack. A compatible AIR developer preview executable is needed to see and play the result.

`racks/<id>/` is editable current source. `releases/<id>/<version>/<archive-sha256>/` retains the exact inputs for a download. `SOURCE-INDEX.json` maps downloads to those immutable directories. Source checksums cover the prepared source snapshot. Existing downloaded versions and saved projects retain their own identities; source changes do not update a sound automatically.

Original contributions use [AGPL-3.0-only](LICENSE), subject to each rack's original artwork/font/third-party notices. Keep all applicable notices. This repository has no automatic executor for untrusted contributions.

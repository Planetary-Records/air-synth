# Runtime artwork and fonts

Original project artwork uses AGPL-3.0-only. This does not replace font licenses or grant rights to external reference material. Preserve original copyright, attribution and license texts with source/download copies.

## Fonts

| File under `studio-skin/fonts/` | License | Original notice |
|---|---|---|
| `IBMPlexSansCondensed-Regular.ttf` | SIL OFL 1.1 | `IBMPlexSansCondensed-OFL.txt` |
| `Inconsolata-Regular.ttf` | SIL OFL 1.1 | `LICENSE.txt` |
| `PermanentMarker-Regular.ttf` | Apache 2.0 | `PermanentMarker-LICENSE.txt`; actual font attribution is also retained in `../licenses/third-party/fonts/PermanentMarker-attribution.txt` |

The native build embeds the supplied runtime fonts and their three original license texts. Do not rename their terms to AGPL or remove the separate Permanent Marker attribution. [OFL guidance](https://openfontlicense.org/how-to-use-ofl-fonts) and [Apache 2.0 section 4](https://www.apache.org/licenses/LICENSE-2.0.html) explain notice retention.

## Skins

Runtime PNG/SVG and control/layout JSON under resources are the inputs embedded by CMake. SOURCE.json indexes record file identities and may refer to historical design or acceptance stages; a hash alone does not prove rights, usability or a new acceptance result. Preserve that distinction when changing art.

See [editing notes](../docs/design/README.md), [source/artwork scope](../docs/release/provenance-review.md) and [licensing](../docs/release/licensing.md). Private research/comparison/approval records are not included. No reference-plugin screenshot, decompiled code or model weight becomes redistributable through this note.

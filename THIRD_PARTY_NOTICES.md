# Third-party notices

Original AI Rack Synth material is licensed under [AGPL-3.0-only](LICENSE). This does not relicense dependencies, fonts, model weights or third-party research material. [Scope and open provenance work](docs/release/licensing.md) must be reviewed before distribution.

| Component | Source / terms | Included notices |
|---|---|---|
| JUCE 9.0.3 | Commit `be29c81492b6151c8ea8d14c840e1311963b3a83`; AGPL-3.0-only selected for this preparation | Original JUCE license and 63-package SPDX inventory in `licenses/third-party/juce`; complete AGPL text in `LICENSE` |
| JUCE vendored dependencies | SDK, codec, graphics and platform libraries retain separate licenses; SPDX inventory includes components not linked into this Windows product | Available original license/COPYING notices and selected embedded notice blocks are preserved under `licenses/third-party/juce` |
| VST3 SDK | MIT in this JUCE pin, not an inferred license from another SDK version | SDK and subcomponent original notices |
| IJG JPEG (vendored jpeglib) | IJG terms in the pinned README; preserve its copyright, conditions and disclaimer | `licenses/third-party/juce/modules/juce_graphics/image_formats/jpglib/README.txt` |
| IBM Plex Sans Condensed | SIL OFL 1.1; IBM copyright / reserved name preserved | `resources/studio-skin/fonts/IBMPlexSansCondensed-OFL.txt` and package notice copy |
| Inconsolata | SIL OFL 1.1; project authors' notice preserved | `resources/studio-skin/fonts/LICENSE.txt` and package notice copy |
| Permanent Marker | Supplied Apache 2.0 text; font attribution recovered from its metadata | `resources/studio-skin/fonts/PermanentMarker-LICENSE.txt` and package attribution copy |
| Microsoft runtime | Release product links statically; no separate producer runtime installation | No arbitrary DLL copies; development/validation tools may have separate prerequisites |
| Inno Setup 6.7.3 | Private installer compiler/runtime; [versioned upstream license](https://github.com/jrsoftware/issrc/blob/is-6_7_3/license.txt) is separate from AGPL | Original license in `licenses/third-party/inno/LICENSE.txt`; generated installer retains upstream runtime notices/website addresses. Compiler is a development tool, not part of the instrument ZIP. No purchase or ongoing payment commitment. |

The inventory is conservative: it is not a claim that all 63 JUCE packages ship. It also is not certification that a notice filename search establishes complete license compliance; embedded source notices and exact linked components must be reconciled for the final candidate. Upstream notices are kept verbatim. No product affiliation with reference-plugin manufacturers is implied.

This software is based in part on the work of the Independent JPEG Group.

VST is a registered trademark of Steinberg Media Technologies GmbH. The SDK license does not grant trademark rights. See Steinberg's [VST usage guidelines](https://steinbergmedia.github.io/vst3_dev_portal/pages/VST%2B3%2BLicensing/Usage%2Bguidelines.html). No Steinberg endorsement is claimed and no VST logo is added by these notices.

The [dependency clearance review](docs/release/dependencies.md) maps the selected module declarations, conditional Windows components and exact known JUCE diagnostic patch. It does not certify the contents of an unbuilt release.

ComfyUI, Stable Audio 3 and T5Gemma are optional separately installed tools/models, not ZIP payloads. Their GPL/model/Gemma terms do not become the project's AGPL terms. See [Tile Array setup](docs/user-guide/tile-array.md#optional-local-sample-generation). No provider usage credits, model redistribution permission or copyright guarantee for generated output is included.

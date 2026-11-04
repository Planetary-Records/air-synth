# Interface source assets and editing

The native interface is built from the PNG/SVG artwork, fonts and control/layout JSON under resources. CMake embeds those files directly; building the plug-in does not regenerate the art or require an image service. Edit the supplied runtime assets and their mappings together, retaining font and copyright notices.

Historical design comparisons, screenshots of development sessions and approval transcripts remain private. They are not claimed as public acceptance. Where a contribution uses editable master artwork or drawing/cropping scripts, keep those original inputs alongside the new asset with rights and instructions. Never replace a reference with copied commercial UI or unlicensed audio.

After editing, verify file identities, control bindings, useful window sizes and actual native pixels. A hash change alone does not establish usability or visual approval. See [rack contribution](../development/racks.md).

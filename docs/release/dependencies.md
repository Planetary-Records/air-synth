# Dependency source and notices

JUCE 9.0.3 is pinned to be29c81492b6151c8ea8d14c840e1311963b3a83. Preserve its complete source and original notices with the matching release. licenses/third-party/juce includes the selected original licenses and conservative SPDX inventory; an inventory is not a claim that all listed packages are linked.

The Windows graphics adjustment is reproduced by tools/JuceGraphics.cmake with a preimage guard. Product builds use probes off and do not apply the historical host-observation development patch. Do not silently substitute a modified cache or a newer dependency revision.

Shipping Release statically links the C++ runtime; producers need no separate runtime install. Development tools and the validator may have their own prerequisites. Inno Setup's compiler/runtime has separate original terms in licenses/third-party/inno. The instrument ZIP does not contain the installer compiler, model weights or arbitrary runtime DLLs.

Keep the original font/SDK/codec notices, the IJG JPEG acknowledgement and trademark attribution in [third-party notices](../../THIRD_PARTY_NOTICES.md). Provide the exact source/build inputs for each distributed binary, not only a private URL or upstream pin. Source/notice review, actual rebuild and clean installation are separate release evidence.

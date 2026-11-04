#pragma once
#include "PatternDocument.h"
#include <juce_core/juce_core.h>

namespace airack
{
inline constexpr int patternMidiTicksPerQuarter = 960;
inline constexpr std::size_t maximumPatternAssetBytes = 4920;
// Control-thread operations. Rejection preserves the output/destination.
bool encodePatternAsset(const PatternDocument&, juce::MemoryBlock&);
bool decodePatternAsset(const void*, std::size_t, PatternDocument&);
bool savePatternAsset(const PatternDocument&, const juce::File&);
bool loadPatternAsset(const juce::File&, PatternDocument&);
bool encodePatternMidi(const PatternDocument&, juce::MemoryBlock&);
bool exportPatternMidi(const PatternDocument&, const juce::File&);
}

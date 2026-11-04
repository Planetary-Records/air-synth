#pragma once
#include "PatternDocument.h"
namespace airack {
struct PatternPlaybackNote {
    double startBeats=0, endBeats=.25;
    std::uint8_t pitch=60, velocity=100, channel=1;
    bool operator==(const PatternPlaybackNote&) const = default;
};
struct PatternPlaybackNotes {
    double lengthBeats=4;
    std::uint16_t noteCount=0;
    std::array<PatternPlaybackNote, maximumPatternNotes> notes {};
    bool operator==(const PatternPlaybackNotes&) const = default;
};
bool makePatternPlaybackNotes(const PatternDocument&, PatternPlaybackNotes&) noexcept;
}

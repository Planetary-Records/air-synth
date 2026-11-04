#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace airack
{
inline constexpr std::size_t maximumPatternNotes = 256;

struct PhraseMeter
{
    std::uint8_t numerator = 4, denominator = 4;
    bool operator==(const PhraseMeter&) const = default;
};
enum class PatternGrid : std::uint8_t { quarter, eighth, sixteenth, thirtySecond, sixtyFourth };
double patternGridBeats(PatternGrid) noexcept;

struct PatternNote
{
    double startBeats = 0, durationBeats = .25;
    std::uint8_t pitch = 60, velocity = 100, channel = 1;
    bool operator==(const PatternNote&) const = default;
};
struct PatternDocument
{
    PhraseMeter meter {};
    std::uint8_t bars = 1;
    PatternGrid grid = PatternGrid::sixteenth;
    double swing = 0;
    std::uint16_t noteCount = 0;
    std::array<PatternNote, maximumPatternNotes> notes {};
    bool operator==(const PatternDocument&) const = default;
};

bool validatePhraseMeter(PhraseMeter) noexcept;
double patternLengthBeats(const PatternDocument&) noexcept;
bool validatePatternDocument(const PatternDocument&) noexcept;
bool makeEmptyPattern(PhraseMeter, PatternDocument&) noexcept;

enum class PatternEditKind : std::uint8_t { addNote, replaceNote, removeNote, setSettings };
enum class PatternEditResult : std::uint8_t { applied, unchanged, invalid, capacity };
struct PatternEdit
{
    PatternEditKind kind = PatternEditKind::addNote;
    std::uint16_t index = 0;
    PatternNote note {};
    PhraseMeter meter {};
    std::uint8_t bars = 1;
    PatternGrid grid = PatternGrid::sixteenth;
    double swing = 0;
};

// Rejection preserves output, including aliasing; a valid no-op publishes the source.
PatternEditResult editPatternDocument(const PatternDocument&, const PatternEdit&, PatternDocument&) noexcept;
}

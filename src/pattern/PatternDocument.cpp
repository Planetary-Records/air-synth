#include "PatternDocument.h"
#include <cmath>

namespace airack
{
namespace
{
bool validNote(const PatternNote& note, double length) noexcept
{
    return std::isfinite(note.startBeats) && std::isfinite(note.durationBeats)
        && note.startBeats >= 0 && note.startBeats < length
        && note.durationBeats > 0 && note.durationBeats <= length - note.startBeats
        && note.pitch <= 127 && note.velocity >= 1 && note.velocity <= 127
        && note.channel >= 1 && note.channel <= 16;
}
}

double patternGridBeats(PatternGrid grid) noexcept
{
    switch (grid)
    {
        case PatternGrid::quarter: return 1;
        case PatternGrid::eighth: return .5;
        case PatternGrid::sixteenth: return .25;
        case PatternGrid::thirtySecond: return .125;
        case PatternGrid::sixtyFourth: return .0625;
    }
    return 0;
}

bool validatePhraseMeter(PhraseMeter meter) noexcept
{
    return meter.numerator >= 1 && meter.numerator <= 32
        && (meter.denominator == 1 || meter.denominator == 2 || meter.denominator == 4
            || meter.denominator == 8 || meter.denominator == 16 || meter.denominator == 32);
}

double patternLengthBeats(const PatternDocument& document) noexcept
{
    if (!validatePhraseMeter(document.meter) || document.bars < 1 || document.bars > 8)
        return 0;
    return document.bars * document.meter.numerator * 4.0 / document.meter.denominator;
}

bool validatePatternDocument(const PatternDocument& document) noexcept
{
    const auto length = patternLengthBeats(document);
    if (length == 0 || patternGridBeats(document.grid) == 0
        || !std::isfinite(document.swing) || document.swing < 0 || document.swing > 1
        || document.noteCount > maximumPatternNotes)
        return false;
    for (std::size_t i = 0; i < document.noteCount; ++i)
        if (!validNote(document.notes[i], length)) return false;
    for (std::size_t i = document.noteCount; i < maximumPatternNotes; ++i)
        if (document.notes[i] != PatternNote {}) return false;
    return true;
}

bool makeEmptyPattern(PhraseMeter meter, PatternDocument& output) noexcept
{
    if (!validatePhraseMeter(meter)) return false;
    PatternDocument candidate;
    candidate.meter = meter;
    output = candidate;
    return true;
}

PatternEditResult editPatternDocument(const PatternDocument& source, const PatternEdit& edit,
                                      PatternDocument& output) noexcept
{
    if (!validatePatternDocument(source)) return PatternEditResult::invalid;
    auto candidate = source;
    switch (edit.kind)
    {
        case PatternEditKind::addNote:
            if (!validNote(edit.note, patternLengthBeats(source))) return PatternEditResult::invalid;
            if (source.noteCount == maximumPatternNotes) return PatternEditResult::capacity;
            candidate.notes[candidate.noteCount++] = edit.note;
            break;
        case PatternEditKind::replaceNote:
            if (edit.index >= source.noteCount) return PatternEditResult::invalid;
            candidate.notes[edit.index] = edit.note;
            break;
        case PatternEditKind::removeNote:
            if (edit.index >= source.noteCount) return PatternEditResult::invalid;
            for (std::size_t i = edit.index; i + 1 < source.noteCount; ++i)
                candidate.notes[i] = source.notes[i + 1];
            candidate.notes[--candidate.noteCount] = {};
            break;
        case PatternEditKind::setSettings:
            candidate.meter = edit.meter;
            candidate.bars = edit.bars;
            candidate.grid = edit.grid;
            candidate.swing = edit.swing;
            break;
        default:
            return PatternEditResult::invalid;
    }
    if (!validatePatternDocument(candidate)) return PatternEditResult::invalid;
    const auto result = candidate == source ? PatternEditResult::unchanged : PatternEditResult::applied;
    output = candidate;
    return result;
}
}

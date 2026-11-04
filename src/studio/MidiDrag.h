#pragma once

#include "PatternDocument.h"
#include "StudioServices.h"

namespace airack::studio
{
/** The tooltip of an EXPORT that both drags and saves. */
inline constexpr const char* midiDragTip = "Drag MIDI into your DAW; click or press Return to save a MIDI file";

/** The start of a pattern's drag: today's sentence for it, and whether the drag began. */
struct MidiDragStart
{
    juce::String sentence;
    bool started = false;
};

/** Writes `pattern` to a new standard MIDI file in the temporary folder's AI Rack Synth MIDI and drags it out of the
    window from `source`, for a DAW to import; `ended` runs on the message thread when the drop or cancel completes.
    While the drag lasts the file stays open for reading only, so importers may read it and nothing may delete it. Our
    files older than a day expire before each drag; with 128 left, the drag is refused rather than removing a young file
    a DAW may still be reading. */
MidiDragStart dragPatternMidi (const PatternDocument& pattern, juce::Component& source, const StudioServices& services,
                               std::function<void()> ended);
} // namespace airack::studio

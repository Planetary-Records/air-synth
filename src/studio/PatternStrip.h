#pragma once

#include "InstrumentProcessor.h"
#include "SkinView.h"
#include "StudioServices.h"

namespace airack::studio
{
/** What the strip display is made from: the pattern's settings, today's operation and playback sentences, the status
    they make together, and when the operation was set. */
struct StripFacts
{
    int bars = 4, numerator = 4, denominator = 4;
    juce::String grid;
    double swing = 0;
    juce::String operation, playback, status;
    std::int64_t operationAt = 0, now = 0;
};

/** The strip display: an operation's message for its hold, else a playback alert while it stands, else the settings. */
StripShown stripShown (const StripFacts& f, const DisplaySpec& d);

/** The editing grids' names, by PatternGrid. */
inline constexpr const char* gridNames[] { "1/4", "1/8", "1/16", "1/32", "1/64" };

/** The pattern status's playback sentence, word for word as the displays' playback tables key it. */
String playbackSentence (PlaybackMode mode, PatternPlaybackStatus playback, PerformanceInputStatus input);

std::vector<Note> notesOf (const PatternDocument& p);

/** The folded PATTERN strip over the processor: the EXT MIDI / INTERNAL lever, EXPORT, the fold, the display and the
    overview of the project's notes, drawn whole or, beside a side unit, as strip.narrow. EXPORT saves the pattern as a
    MIDI file when clicked and drags it into a DAW when dragged. Today's pattern status sentence stays in the undrawn
    label strip.status, and the display shows it shortened. */
class PatternStrip final : public juce::Component
{
public:
    PatternStrip (InstrumentProcessor& p, const Model& model, StudioServices s);

    /** Called on the editor's timer. */
    void refresh();
    juce::String sentence() const { return status.getText(); }

    std::function<void (bool unfolded)> onFold;
    void setUnfolded (bool unfolded);
    /** Shows strip.narrow, or the whole strip, and takes that view's size. */
    void setNarrow (bool narrow);

private:
    void bind (SkinView& v);
    void update (bool force);
    bool snapshot (ProjectDocument& d, std::uint64_t& revision);
    void message (const juce::String& text);
    void setMode (bool internal);
    void exportMidi();
    void dragMidi (juce::Component& source);
    void showStatus();

    InstrumentProcessor& processor;
    StudioServices services;
    SkinView view, narrowView;
    juce::Label status;
    ProjectDocument shown;
    std::uint64_t shownRevision = 0, shownReset = 0;
    juce::String operation;
    std::int64_t operationAt = 0;
    bool first = true, choosing = false, dragging = false, unfolded = false, narrowed = false;
};
} // namespace airack::studio

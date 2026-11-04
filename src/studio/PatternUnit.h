#pragma once

#include "PatternStrip.h"

#include <optional>

namespace airack::studio
{
/** The unfolded PATTERN unit over the processor: the settings knobs and METER's menu, the selected note's knobs, ADD
    NOTE and DELETE, the roll, the membrane keyboard with OCT - and OCT +, the EXT MIDI / INTERNAL lever, PLAYING, NEW
    and the pattern files. Its status sentences stay word for word in the undrawn label pattern.status, and the
    displays show them shortened. A knob drag is one edit, previewed until the release; a change the unit did not make
    cancels its drags and closes its knob entries. */
class PatternUnit final : public juce::Component
{
public:
    PatternUnit (InstrumentProcessor& p, const Model& model, StudioServices s);
    ~PatternUnit() override;
    /** Release processor keyboard ownership before deferred editor deletion. */
    void retire();

    /** Called on the editor's timer. */
    void refresh();
    juce::String sentence() const { return status.getText(); }
    /** The project revision the unit shows: it admits a gesture only from the processor's current one. */
    std::uint64_t revision() const { return shownRevision; }

    std::function<void (bool unfolded)> onFold;
    void setUnfolded (bool unfolded);

private:
    struct Binding;
    enum class FileAction { browse, save, load, exportMidi };

    /** What the unit shows: a knob drag's preview, else the committed pattern. */
    const PatternDocument& current() const { return preview ? *preview : shown.pattern; }
    bool hasSelection() const { return selected >= 0 && selected < (int) current().noteCount; }
    void update (bool force, const PatternDocument* accepted = nullptr);
    void show();
    void showStatus();
    bool snapshot (ProjectDocument& d, std::uint64_t& revision);
    void message (const juce::String& text);
    void result (bool ok, const juce::String& action);
    void cancelDrag();
    void commit (const PatternDocument& candidate, std::uint64_t revision, const juce::String& action);
    /** A settings edit, or a value edit of the selected note, to `candidate`; `inRange` false when a typed value broke
        the entry's rules. */
    void edit (const PatternDocument& candidate, bool note, bool inRange = true);
    void addNote (const ProjectDocument& d, std::uint64_t revision, const PatternNote& note);
    void removeNote();
    void setMode (bool internal);
    void newPattern();
    bool beginDrag (const Binding& b);
    void dragTo (const PatternDocument& candidate, bool note);
    void endDrag (bool keep, bool note);
    bool key (bool on, int pitch, int velocity);
    void fileAction (FileAction action);
    void dragMidi (juce::Component& source);
    void performFile (FileAction action, const juce::File& file, const PatternDocument& captured);
    void commitPathEntry();
    void setPath (const juce::String& text);

    InstrumentProcessor& processor;
    StudioServices services;
    ProjectDocument shown, admitted;
    std::uint64_t shownRevision = 0, shownReset = 0, admittedRevision = 0, dragRevision = 0;
    std::optional<PatternDocument> preview;
    const Binding* dragging = nullptr;
    int selected = -1;
    juce::String operation, path;
    std::int64_t operationAt = 0;
    bool first = true, choosing = false, midiDragging = false, unfolded = false, retired = false;
    juce::Label status;
    // Last, so that it is destroyed first: its controls read the state above while they go.
    SkinView view;
};
} // namespace airack::studio

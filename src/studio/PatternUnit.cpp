#include "PatternUnit.h"
#include "MidiDrag.h"
#include "PatternIO.h"

#include <charconv>
#include <cmath>
#include <limits>

namespace airack::studio
{
namespace
{
const juce::String invalidNote = "Invalid note value: use a finite number within the phrase and MIDI range";
const juce::String invalidSettings = "Invalid settings: bars 1-8, meter 1-32, swing 0-1";
const juce::String invalidPhrase = "Invalid phrase size: edit notes to fit before shrinking bars or meter";
const juce::String projectChanged = "Edit cancelled: project changed; select the note again";

/** The entry's text for a value: every significant digit of the double. */
String number (double value)
{
    char buffer[64];
    const auto r = std::to_chars (buffer, buffer + sizeof (buffer), value, std::chars_format::general, std::numeric_limits<double>::max_digits10);
    return r.ec == std::errc {} ? String::fromUTF8 (buffer, (int) (r.ptr - buffer)) : String();
}

/** The entry's parse: the whole text one finite number, with an optional leading '+'. */
bool parse (const String& input, double& value)
{
    const auto text = input.trim().toStdString();
    if (text.empty())
        return false;
    auto* start = text.data();
    const auto* end = start + text.size();
    if (*start == '+')
        ++start;
    const auto r = std::from_chars (start, end, value, std::chars_format::general);
    return r.ec == std::errc {} && r.ptr == end && std::isfinite (value);
}

bool integer (const String& input, int low, int high, int& value)
{
    double parsed = 0;
    if (! parse (input, parsed) || parsed < low || parsed > high || std::floor (parsed) != parsed)
        return false;
    value = (int) parsed;
    return true;
}

PatternNote toPattern (const Note& n) { return { n.start, n.duration, (std::uint8_t) n.pitch, (std::uint8_t) n.velocity, (std::uint8_t) n.channel }; }
} // namespace

/** A knob of the unit: a pattern setting, or a field of the selected note. Notes start and last in grid steps, as the
    roll snaps them. Outside its own drag a knob edits the committed pattern. */
struct PatternUnit::Binding final : public KnobBinding
{
    Binding (PatternUnit& u, String f) : unit (u), field (std::move (f)) {}

    bool isNote() const { return field == "pitch" || field == "start" || field == "length" || field == "velocity" || field == "channel"; }
    const PatternDocument& pattern() const { return unit.current(); }
    const PatternNote& note() const { return pattern().notes[(std::size_t) unit.selected]; }
    double grid() const { return patternGridBeats (pattern().grid); }

    int steps() const override
    {
        if (field == "bars") return 7;
        if (field == "meter") return 31;
        if (field == "grid") return 4;
        if (field == "swing") return 100;
        if (field == "pitch") return 127;
        if (field == "velocity") return 126;
        if (field == "channel") return 15;
        if (! unit.hasSelection()) return 1;
        const auto length = patternLengthBeats (pattern());
        if (field == "start") return std::max (1, (int) std::floor ((length - note().durationBeats) / grid() + 1.0e-9));
        return std::max (1, (int) std::floor ((length - note().startBeats) / grid() + 1.0e-9) - 1);
    }

    double fraction() const override
    {
        const auto& p = pattern();
        if (field == "bars") return (p.bars - 1) / 7.0;
        if (field == "meter") return (p.meter.numerator - 1) / 31.0;
        if (field == "grid") return (int) p.grid / 4.0;
        if (field == "swing") return p.swing;
        if (! unit.hasSelection()) return 0.0;
        const auto& n = note();
        if (field == "pitch") return n.pitch / 127.0;
        if (field == "velocity") return (n.velocity - 1) / 126.0;
        if (field == "channel") return (n.channel - 1) / 15.0;
        if (field == "start") return juce::jlimit (0.0, 1.0, n.startBeats / grid() / steps());
        return juce::jlimit (0.0, 1.0, (n.durationBeats / grid() - 1.0) / steps());
    }

    void set (double f) override
    {
        const auto k = (int) std::lround (juce::jlimit (0.0, 1.0, f) * steps());
        const auto own = unit.dragging == this;
        auto candidate = own ? pattern() : unit.shown.pattern;
        if (field == "bars") candidate.bars = (std::uint8_t) (1 + k);
        else if (field == "meter") candidate.meter.numerator = (std::uint8_t) (1 + k);
        else if (field == "grid") candidate.grid = (PatternGrid) k;
        else if (field == "swing") candidate.swing = k / 100.0;
        else
        {
            if (! unit.hasSelection())
                return;
            auto& n = candidate.notes[(std::size_t) unit.selected];
            if (field == "pitch") n.pitch = (std::uint8_t) k;
            else if (field == "velocity") n.velocity = (std::uint8_t) (1 + k);
            else if (field == "channel") n.channel = (std::uint8_t) (1 + k);
            else if (field == "start") n.startBeats = k * grid();
            else n.durationBeats = (k + 1) * grid();
        }
        if (own)
            unit.dragTo (candidate, isNote());
        else
            unit.edit (candidate, isNote());
    }

    String valueText() const override
    {
        const auto& p = pattern();
        if (field == "bars") return String ((int) p.bars) + (p.bars == 1 ? " BAR" : " BARS");
        if (field == "meter") return String ((int) p.meter.numerator) + "/" + String ((int) p.meter.denominator);
        if (field == "grid") return gridNames[juce::jlimit (0, 4, (int) p.grid)];
        if (field == "swing") return "SWING " + String (juce::roundToInt (p.swing * 100.0)) + "%";
        if (! unit.hasSelection()) return "NO NOTE SELECTED";
        const auto& n = note();
        if (field == "pitch") return "NOTE " + noteName (n.pitch);
        if (field == "start") return "START " + startText (n.startBeats, p.meter.numerator, p.meter.denominator);
        if (field == "length") return "LENGTH " + lengthText (n.durationBeats);
        if (field == "velocity") return "VELOCITY " + String ((int) n.velocity);
        return "CHANNEL " + String ((int) n.channel);
    }

    /** The entry's text: bars, the meter (N/D), the grid's name and swing 0-1; the note's start and length in quarter
        beats, its pitch, velocity and channel. */
    String entryText() const override
    {
        const auto& p = pattern();
        if (field == "bars") return String ((int) p.bars);
        if (field == "meter") return String ((int) p.meter.numerator) + "/" + String ((int) p.meter.denominator);
        if (field == "grid") return gridNames[juce::jlimit (0, 4, (int) p.grid)];
        if (field == "swing") return number (p.swing);
        if (! unit.hasSelection()) return {};
        const auto& n = note();
        if (field == "pitch") return String ((int) n.pitch);
        if (field == "start") return number (n.startBeats);
        if (field == "length") return number (n.durationBeats);
        if (field == "velocity") return String ((int) n.velocity);
        return String ((int) n.channel);
    }

    /** The entry's rules: whole numbers in range, finite numbers, METER as N or N/D, GRID as 1/4 to 1/64. */
    bool enter (const String& raw) override
    {
        auto candidate = unit.shown.pattern;
        const auto text = raw.trim();
        int value = 0;
        double real = 0;
        bool ok = false;
        if (field == "bars")
        {
            ok = integer (text, 1, 8, value);
            candidate.bars = (std::uint8_t) value;
        }
        else if (field == "meter")
        {
            int den = candidate.meter.denominator;
            ok = integer (text.upToFirstOccurrenceOf ("/", false, false), 1, 32, value)
                 && (! text.containsChar ('/')
                     || (integer (text.fromFirstOccurrenceOf ("/", false, false), 1, 32, den) && validatePhraseMeter ({ 1, (std::uint8_t) den })));
            candidate.meter = { (std::uint8_t) value, (std::uint8_t) den };
        }
        else if (field == "grid")
        {
            for (int i = 0; i < 5; ++i)
                if (text == gridNames[i])
                {
                    ok = true;
                    candidate.grid = (PatternGrid) i;
                }
        }
        else if (field == "swing")
        {
            ok = parse (text, real) && real >= 0.0 && real <= 1.0;
            candidate.swing = real;
        }
        else
        {
            if (! unit.hasSelection())
                return false;
            auto& n = candidate.notes[(std::size_t) unit.selected];
            if (field == "start" || field == "length")
            {
                ok = parse (text, real);
                (field == "start" ? n.startBeats : n.durationBeats) = real;
            }
            else
            {
                const auto high = field == "pitch" ? 127 : field == "velocity" ? 127 : 16;
                ok = integer (text, field == "pitch" ? 0 : 1, high, value);
                (field == "pitch" ? n.pitch : field == "velocity" ? n.velocity : n.channel) = (std::uint8_t) value;
            }
        }
        unit.edit (candidate, isNote(), ok);
        return true;
    }

    bool enabled() const override { return ! isNote() || unit.hasSelection(); }

    void addMenu (Menu& m) override
    {
        if (field != "meter")
            return;
        for (const int den : { 1, 2, 4, 8, 16, 32 })
            m.push_back ({ "Denominator " + String (den), [this, den]
                           {
                               auto candidate = unit.shown.pattern;
                               candidate.meter.denominator = (std::uint8_t) den;
                               unit.edit (candidate, false);
                           } });
    }

    bool begin() override { return unit.beginDrag (*this); }
    void end (bool keep) override { unit.endDrag (keep, isNote()); }

    PatternUnit& unit;
    const String field;
};

PatternUnit::PatternUnit (InstrumentProcessor& p, const Model& model, StudioServices s)
    : processor (p), services (std::move (s)), view (model, "pattern")
{
    setComponentID ("pattern.panel");
    addAndMakeVisible (view);
    setSize (view.getWidth(), view.getHeight());
    status.setComponentID ("pattern.status");
    status.setName ("Pattern edit and playback status");
    addChildComponent (status);
    for (auto& k : view.knobs)
        k->setBinding (std::make_unique<Binding> (*this, k->element.id.fromFirstOccurrenceOf ("pattern.", false, false)));
    const auto button = [this] (const char* id) { return view.get<SkinButton> (id); };
    if (auto* b = button ("pattern.new"))
        b->onClick = [this] { newPattern(); };
    if (auto* b = button ("pattern.add"))
        b->onClick = [this]
        {
            cancelDrag();
            ProjectDocument d;
            std::uint64_t revision = 0;
            if (snapshot (d, revision))
                addNote (d, revision, { 0.0, std::min (patternGridBeats (d.pattern.grid), patternLengthBeats (d.pattern)), 60, 100, 1 });
        };
    if (auto* b = button ("pattern.delete"))
        b->onClick = [this] { removeNote(); };
    if (auto* b = button ("pattern.save"))
        b->onClick = [this] { fileAction (FileAction::save); };
    if (auto* b = button ("pattern.load"))
        b->onClick = [this] { fileAction (FileAction::load); };
    if (auto* b = button ("pattern.export"))
    {
        b->onClick = [this] { fileAction (FileAction::exportMidi); };
        b->onDragOut = [this, b] { dragMidi (*b); };
        b->setTooltip (midiDragTip);
    }
    if (auto* b = button ("pattern.browse"))
        b->onClick = [this] { fileAction (FileAction::browse); };
    if (auto* b = button ("pattern.down"))
    {
        b->onClick = [this] { view.keyboard->shift (-1); };
        b->litWhen = [this] { return view.keyboard->isPlaying(); };
    }
    if (auto* b = button ("pattern.up"))
        b->onClick = [this] { view.keyboard->shift (1); };
    if (auto* lever = view.get<Lever> ("pattern.mode"))
        lever->onClick = [this, lever] { setMode (lever->position() == 1); };
    if (view.fold != nullptr)
        view.fold->onPress = [this]
        {
            if (onFold)
                onFold (! unfolded);
        };
    auto& roll = *view.roll;
    roll.onAdmit = [this]
    {
        cancelDrag();
        return snapshot (admitted, admittedRevision);
    };
    roll.onSelect = [this] (int i)
    {
        selected = i;
        show();
    };
    roll.accepts = [this] (int i, const Note& n)
    {
        auto candidate = admitted.pattern;
        if (i < 0 || i >= (int) candidate.noteCount)
            return false;
        candidate.notes[(std::size_t) i] = toPattern (n);
        return validatePatternDocument (candidate);
    };
    roll.onEdit = [this] (int i, const Note& n)
    {
        ProjectDocument d;
        std::uint64_t revision = 0;
        if (! snapshot (d, revision) || i < 0 || i >= (int) d.pattern.noteCount)
            return;
        auto candidate = d.pattern;
        candidate.notes[(std::size_t) i] = toPattern (n);
        commit (candidate, admittedRevision, "Note edit");
    };
    roll.onAdd = [this] (const Note& n) { addNote (admitted, admittedRevision, toPattern (n)); };
    roll.onCancel = [this]
    {
        cancelDrag();
        message ("Edit cancelled; pattern preserved");
    };
    roll.onDelete = [this] { removeNote(); };
    view.keyboard->onKey = [this] (bool on, int pitch, int velocity) { return key (on, pitch, velocity); };
    if (auto* d = view.display.get())
    {
        d->onPath = [this] (const juce::String& text) { setPath (text); };
        d->entry.onTextChange = [this, d]
        {
            if (! d->entry.isVisible())
                setPath (d->entry.getText());
        };
    }
    refresh();
}

PatternUnit::~PatternUnit(){retire();}

void PatternUnit::retire()
{
    if(retired)return;retired=true;
    view.keyboard->onKey=nullptr;
    if(auto* display=view.display.get()){display->onPath=nullptr;display->entry.onTextChange=nullptr;}
    cancelDrag();
    for(auto& knob:view.knobs)knob->setBinding(nullptr);
    for(auto& button:view.buttons){button->onClick=nullptr;button->onDragOut=nullptr;}
    for(auto& lever:view.levers)lever->onClick=nullptr;
    view.fold->onPress=nullptr;
    auto& roll=*view.roll;roll.onAdmit=nullptr;roll.onSelect=nullptr;roll.onAdd=nullptr;roll.onEdit=nullptr;
    roll.onCancel=nullptr;roll.onDelete=nullptr;roll.accepts=nullptr;
    processor.releaseKeyboardNotes();
}

void PatternUnit::refresh() { update (false); }

void PatternUnit::update (bool force, const PatternDocument* accepted)
{
    if(retired)return;
    ProjectDocument d;
    std::uint64_t revision = 0, reset = 0;
    if (processor.tryProjectSnapshot (d, revision, reset) && (first || force || revision != shownRevision || reset != shownReset))
    {
        const bool resetChanged = ! first && reset != shownReset;
        const bool changed = ! first && (revision != shownRevision || resetChanged || d.pattern != shown.pattern);
        const bool own = accepted != nullptr && d.pattern == *accepted && ! resetChanged;
        if (changed && ! own)
        {
            cancelDrag();
            for (auto& k : view.knobs)
                k->closeEntry();
            // Mode or sound changes keep an otherwise identical selection.
            if (resetChanged || d.pattern != shown.pattern)
                selected = -1;
            operation = projectChanged;
            operationAt = services.nowMs();
        }
        if (resetChanged)
            view.keyboard->forget();
        shown = d;
        shownRevision = revision;
        shownReset = reset;
        first = false;
        if (selected >= (int) shown.pattern.noteCount)
            selected = -1;
    }
    show();
}

void PatternUnit::show()
{
    const auto& p = current();
    if (auto* lever = view.get<Lever> ("pattern.mode"))
        lever->setPosition (shown.mode == PlaybackMode::internalPattern ? 1 : 0, juce::dontSendNotification);
    for (auto& k : view.knobs)
        k->refresh();
    if (auto* b = view.get<SkinButton> ("pattern.delete"))
        b->setEnabled (hasSelection());
    RollShown r;
    r.notes = notesOf (p);
    r.bars = p.bars;
    r.numerator = p.meter.numerator;
    r.denominator = p.meter.denominator;
    r.grid = gridNames[juce::jlimit (0, 4, (int) p.grid)];
    r.gridBeats = patternGridBeats (p.grid);
    r.selected = hasSelection() ? selected : -1;
    r.playhead = shown.mode == PlaybackMode::internalPattern ? processor.patternPlaybackPosition() : std::nullopt;
    view.roll->show (r);
    NoteShown n;
    n.selected = hasSelection();
    if (n.selected)
    {
        const auto& note = p.notes[(std::size_t) selected];
        const auto name = noteName (note.pitch), start = startText (note.startBeats, p.meter.numerator, p.meter.denominator),
                   length = lengthText (note.durationBeats), velocity = String ((int) note.velocity), channel = String ((int) note.channel);
        n.text = "NOTE " + name + "  START " + start + "  LENGTH " + length + "  VELOCITY " + velocity + "  CHANNEL " + channel;
        n.accessible = "Selected note " + name + ", start " + start + ", length " + length + ", velocity " + velocity + ", channel " + channel;
    }
    else
    {
        n.text = "NO NOTE SELECTED";
        n.accessible = "No note selected";
    }
    view.noteDisplay->show (n);
    showStatus();
}

void PatternUnit::showStatus()
{
    if(retired)return;
    const auto playback = processor.patternPlaybackStatus();
    const auto input = processor.performanceInputStatus();
    const auto sentence = playbackSentence (shown.mode, playback, input);
    status.setText (operation.isEmpty() ? sentence : operation + " | " + sentence, juce::dontSendNotification);
    const auto& p = current();
    StripFacts f;
    f.bars = p.bars;
    f.numerator = p.meter.numerator;
    f.denominator = p.meter.denominator;
    f.grid = gridNames[juce::jlimit (0, 4, (int) p.grid)];
    f.swing = p.swing;
    f.operation = operation;
    f.playback = sentence;
    f.status = status.getText();
    f.operationAt = operationAt;
    f.now = services.nowMs();
    auto settings = stripShown (f, view.model.displays.at ("pattern.settings"));
    settings.path = path;
    view.display->show (settings);
    const auto internal = shown.mode == PlaybackMode::internalPattern;
    view.playing->setLit (internal && playback == PatternPlaybackStatus::playing);
    if (auto* b = view.get<SkinButton> ("pattern.up"))
        b->setToggleState (input == PerformanceInputStatus::ready && (! internal || playback == PatternPlaybackStatus::stopped), juce::dontSendNotification);
}

bool PatternUnit::snapshot (ProjectDocument& d, std::uint64_t& revision)
{
    if(retired)return false;
    std::uint64_t reset = 0;
    if (! processor.tryProjectSnapshot (d, revision, reset))
    {
        message ("Pattern is busy; retry the edit");
        return false;
    }
    if (revision == shownRevision && reset == shownReset && d.pattern == shown.pattern)
        return true;
    // Old selection or text cannot borrow a replacement's fresh revision.
    cancelDrag();
    selected = -1;
    update (true);
    message (projectChanged);
    return false;
}

void PatternUnit::message (const juce::String& text)
{
    operation = text;
    operationAt = services.nowMs();
    showStatus();
}

void PatternUnit::result (bool ok, const juce::String& action)
{
    message (ok ? action + " complete" : action + " rejected: pattern changed or values invalid; current pattern preserved");
}

void PatternUnit::cancelDrag()
{
    dragging = nullptr;
    for (auto& k : view.knobs)
        if (k->isDragging())
            k->cancelDrag();
    view.roll->cancelDrag();
    if (preview)
    {
        preview.reset();
        show();
    }
}

void PatternUnit::commit (const PatternDocument& candidate, std::uint64_t revision, const juce::String& action)
{
    const bool ok = processor.applyPatternEdit (candidate, revision);
    result (ok, action);
    update (true, ok ? &candidate : nullptr);
    if (hasSelection())
        view.roll->reveal (shown.pattern.notes[(std::size_t) selected].pitch);
}

void PatternUnit::edit (const PatternDocument& candidate, bool note, bool inRange)
{
    cancelDrag();
    ProjectDocument d;
    std::uint64_t revision = 0;
    if (! snapshot (d, revision))
        return;
    if (note)
    {
        if (! hasSelection())
            return;
        if (! inRange || ! validatePatternDocument (candidate))
            return message (invalidNote);
        return commit (candidate, revision, "Note value");
    }
    if (! inRange)
        return message (invalidSettings);
    if (! validatePatternDocument (candidate))
    {
        message (invalidPhrase);
        update (true);
        return;
    }
    // A reset field's later Return is a no-op and retains the cancellation explanation rather than reporting an edit of
    // the replacement.
    if (candidate == d.pattern)
        return;
    commit (candidate, revision, "Pattern settings");
}

void PatternUnit::addNote (const ProjectDocument& d, std::uint64_t revision, const PatternNote& note)
{
    if (d.pattern.noteCount == maximumPatternNotes)
        return message ("Pattern is full (256 notes); delete a note before adding");
    PatternEdit e;
    e.note = note;
    PatternDocument candidate;
    if (editPatternDocument (d.pattern, e, candidate) != PatternEditResult::applied)
        return message ("Invalid note: outside the phrase or supported range");
    selected = d.pattern.noteCount;
    commit (candidate, revision, "Add note");
}

void PatternUnit::removeNote()
{
    cancelDrag();
    ProjectDocument d;
    std::uint64_t revision = 0;
    if (! snapshot (d, revision) || selected < 0 || selected >= (int) d.pattern.noteCount)
        return;
    PatternEdit e;
    e.kind = PatternEditKind::removeNote;
    e.index = (std::uint16_t) selected;
    PatternDocument candidate;
    if (editPatternDocument (d.pattern, e, candidate) != PatternEditResult::applied)
        return;
    selected = std::min (selected, (int) candidate.noteCount - 1);
    commit (candidate, revision, "Delete note");
}

void PatternUnit::setMode (bool internal)
{
    cancelDrag();
    ProjectDocument d;
    std::uint64_t revision = 0;
    if (! snapshot (d, revision))
        return;
    const bool ok = processor.setPlaybackMode (internal ? PlaybackMode::internalPattern : PlaybackMode::externalMidi, revision);
    update (true);
    result (ok, "Mode edit");
}

void PatternUnit::newPattern()
{
    cancelDrag();
    ProjectDocument d;
    std::uint64_t revision = 0;
    if (! snapshot (d, revision))
        return;
    const bool host = processor.currentHostMeter().has_value();
    const bool ok = processor.createPattern (d.pattern.bars, revision);
    selected = -1;
    update (true);
    result (ok, host ? "New pattern uses current host meter" : "New pattern uses manual 4/4 (host meter unavailable)");
}

bool PatternUnit::beginDrag (const Binding& b)
{
    cancelDrag();
    ProjectDocument d;
    if (! snapshot (d, dragRevision))
        return false;
    dragging = &b;
    preview = d.pattern;
    return true;
}

void PatternUnit::dragTo (const PatternDocument& candidate, bool note)
{
    if (! validatePatternDocument (candidate))
        return message (note ? invalidNote : invalidPhrase);
    preview = candidate;
    // The preview is the newest feedback: the settings display shows it rather than a held message.
    operation = {};
    show();
}

void PatternUnit::endDrag (bool keep, bool note)
{
    if (dragging == nullptr)
        return;
    dragging = nullptr;
    const auto candidate = preview.value_or (shown.pattern);
    preview.reset();
    if (! keep)
    {
        show();
        return message ("Edit cancelled; pattern preserved");
    }
    ProjectDocument d;
    std::uint64_t revision = 0;
    if (! snapshot (d, revision))
        return;
    if (candidate == d.pattern)
        return show();
    commit (candidate, dragRevision, note ? "Note value" : "Pattern settings");
}

bool PatternUnit::key (bool on, int pitch, int velocity)
{
    const bool ok = processor.enqueueKeyboardNote (on, (std::uint8_t) pitch, (std::uint8_t) (on ? velocity : 0), 1);
    if (ok)
        showStatus();
    else
        message ("Keyboard queue full; notes released. Try again.");
    if (auto* b = view.get<SkinButton> ("pattern.down"))
        b->repaint();
    return ok;
}

void PatternUnit::fileAction (FileAction action)
{
    if(retired)return;
    commitPathEntry();
    if (choosing)
        return message ("Finish or cancel the open pattern chooser first");
    ProjectDocument committed;
    std::uint64_t revision = 0;
    // File capture is read-only and independent of stale selection or text.
    if (! processor.tryProjectSnapshot (committed, revision))
        return message ("Pattern is busy; retry the file operation");
    // The committed pattern when asked, even if a drag previews or a later edit or recall wins before the answer.
    const auto captured = committed.pattern;
    const auto entered = path.trim();
    if (action != FileAction::browse && entered.isNotEmpty())
    {
        if (! juce::File::isAbsolutePath (entered))
            return message ("Pattern operation failed: enter an absolute file path");
        return performFile (action, juce::File (entered), captured);
    }
    const juce::String title = action == FileAction::save         ? "Pattern Save"
                               : action == FileAction::exportMidi ? "Pattern Export"
                               : action == FileAction::load       ? "Pattern Load"
                                                                  : "Pattern Browse";
    const auto writing = action == FileAction::save || action == FileAction::exportMidi;
    choosing = true;
    const juce::Component::SafePointer<PatternUnit> safe (this);
    services.chooseFile ({ title, action == FileAction::exportMidi ? "*.mid" : "*.aipattern", writing, true,
                           juce::File::isAbsolutePath (entered) ? juce::File (entered) : juce::File(), this },
                         [safe, action, captured] (const juce::File& file)
                         {
                             if (safe == nullptr)
                                 return;
                             safe->choosing = false;
                             if (file == juce::File())
                                 safe->message ("Pattern selection cancelled; committed project preserved");
                             else
                                 safe->performFile (action, file, captured);
                         });
}

void PatternUnit::dragMidi (juce::Component& source)
{
    if (choosing)
        return message ("Finish or cancel the open pattern chooser first");
    if (midiDragging)
        return message ("Finish the current MIDI drag first");
    ProjectDocument committed;
    std::uint64_t revision = 0;
    if (! processor.tryProjectSnapshot (committed, revision))
        return message ("Pattern is busy; retry the MIDI drag");
    midiDragging = true;
    const juce::Component::SafePointer<PatternUnit> safe (this);
    const auto start = dragPatternMidi (committed.pattern, source, services, [safe]
                                        {
                                            if (safe == nullptr)
                                                return;
                                            safe->midiDragging = false;
                                            safe->message ("MIDI drag ended; file retained for DAW import");
                                        });
    if (! start.started)
        midiDragging = false;
    message (start.sentence);
}

void PatternUnit::performFile (FileAction action, const juce::File& file, const PatternDocument& captured)
{
    if(retired)return;
    if (action == FileAction::browse)
    {
        setPath (file.getFullPathName());
        return message ("Pattern path selected");
    }
    const bool ok = action == FileAction::save         ? savePatternAsset (captured, file)
                    : action == FileAction::exportMidi ? exportPatternMidi (captured, file)
                                                       : processor.loadPattern (file);
    setPath (file.getFullPathName());
    const juce::String caption = action == FileAction::save ? "Save pattern" : action == FileAction::exportMidi ? "MIDI export" : "Load pattern";
    if (ok && action == FileAction::load)
    {
        cancelDrag();
        selected = -1;
        update (true);
    }
    message (ok ? caption + " complete" : caption + " failed: check the path, permissions and valid pattern asset; committed project preserved");
}

void PatternUnit::commitPathEntry()
{
    if (view.display->entry.isVisible())
        view.display->commitPath();
}

void PatternUnit::setPath (const juce::String& text)
{
    path = text;
    view.display->entry.setText (text, false);
    showStatus();
}

void PatternUnit::setUnfolded (bool u)
{
    unfolded = u;
    if (view.fold != nullptr)
        view.fold->setExpanded (u);
}
} // namespace airack::studio

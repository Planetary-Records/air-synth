#include "PatternStrip.h"
#include "MidiDrag.h"
#include "PatternIO.h"

namespace airack::studio
{
namespace
{
String percent (double swing) { return String (juce::roundToInt (swing * 100.0)) + "%"; }

String settingsText (const StripFacts& f)
{
    return String (f.bars) + (f.bars == 1 ? " BAR" : " BARS") + "  " + String (f.numerator) + "/" + String (f.denominator) + "  " + f.grid
           + "  SWING " + percent (f.swing);
}
} // namespace

String playbackSentence (PlaybackMode mode, PatternPlaybackStatus playback, PerformanceInputStatus input)
{
    if (input != PerformanceInputStatus::ready)
        return "Input capacity reached; notes released. Try fewer simultaneous events.";
    if (mode == PlaybackMode::externalMidi)
        return "External MIDI / keyboard available";
    switch (playback)
    {
        case PatternPlaybackStatus::playing: return "Internal Pattern playing";
        case PatternPlaybackStatus::stopped: return "Internal Pattern stopped / keyboard available";
        case PatternPlaybackStatus::invalidTiming:
            return "Invalid host timing: beat coordinates must be within +/-16777216; valid tempo and loop required";
        case PatternPlaybackStatus::eventCapacity: return "Pattern paused: too many events in this audio block";
        case PatternPlaybackStatus::invalidBlock: return "Pattern paused: unsupported audio rate or block size";
        case PatternPlaybackStatus::unprepared: return "Internal Pattern waiting for audio and host timing";
        default: return "Internal Pattern waiting for host timing (tempo and beat position)";
    }
}

std::vector<Note> notesOf (const PatternDocument& p)
{
    std::vector<Note> out;
    for (std::size_t i = 0; i < p.noteCount; ++i)
    {
        const auto& n = p.notes[i];
        out.push_back ({ n.startBeats, n.durationBeats, n.pitch, n.velocity, n.channel });
    }
    return out;
}

StripShown stripShown (const StripFacts& f, const DisplaySpec& d)
{
    StripShown out;
    out.accessible = f.status + ". " + String (f.bars) + (f.bars == 1 ? " bar, " : " bars, ") + String (f.numerator) + "/"
                     + String (f.denominator) + ", grid " + f.grid + ", swing " + percent (f.swing);
    if (f.operation.isNotEmpty())
        for (auto& o : d.operations)
            if (o.today == f.operation && o.text.isNotEmpty() && f.now - f.operationAt < o.holdMs)
            {
                out.layout = o.amber ? "alert" : "settings";
                out.text = o.text;
                out.amber = o.amber;
                return out;
            }
    for (auto& p : d.playback)
        if (p.today == f.playback && p.alert.isNotEmpty())
        {
            out.layout = "alert";
            out.text = p.alert;
            out.amber = true;
            return out;
        }
    out.layout = "settings";
    out.text = settingsText (f);
    return out;
}

PatternStrip::PatternStrip (InstrumentProcessor& p, const Model& model, StudioServices s)
    : processor (p), services (std::move (s)), view (model, "strip"), narrowView (model, "strip.narrow")
{
    setComponentID ("strip");
    addAndMakeVisible (view);
    setSize (view.getWidth(), view.getHeight());
    status.setComponentID ("strip.status");
    status.setName ("Pattern edit and playback status");
    addChildComponent (status);
    bind (view);
    bind (narrowView);
    refresh();
}

void PatternStrip::bind (SkinView& v)
{
    if (auto* lever = v.get<Lever> ("strip.mode"))
        lever->onClick = [this, lever] { setMode (lever->position() == 1); };
    if (auto* b = v.get<SkinButton> ("strip.export"))
    {
        b->onClick = [this] { exportMidi(); };
        b->onDragOut = [this, b] { dragMidi (*b); };
        b->setTooltip (midiDragTip);
    }
    if (v.fold != nullptr)
        v.fold->onPress = [this]
        {
            if (onFold)
                onFold (! unfolded);
        };
}

void PatternStrip::setNarrow (bool narrow)
{
    if (narrow == narrowed)
        return;
    narrowed = narrow;
    auto& shownView = narrow ? narrowView : view;
    removeChildComponent (narrow ? &view : &narrowView);
    addAndMakeVisible (shownView);
    setSize (shownView.getWidth(), shownView.getHeight());
}

void PatternStrip::refresh() { update (false); }

void PatternStrip::update (bool force)
{
    ProjectDocument d;
    std::uint64_t revision = 0, reset = 0;
    if (processor.tryProjectSnapshot (d, revision, reset) && (first || force || revision != shownRevision || reset != shownReset))
    {
        shown = d;
        shownRevision = revision;
        shownReset = reset;
        first = false;
        for (auto* v : { &view, &narrowView })
        {
            if (auto* lever = v->get<Lever> ("strip.mode"))
                lever->setPosition (shown.mode == PlaybackMode::internalPattern ? 1 : 0, juce::dontSendNotification);
            if (v->overview != nullptr)
                v->overview->show (notesOf (shown.pattern), shown.pattern.bars, shown.pattern.meter.numerator, shown.pattern.meter.denominator);
        }
    }
    showStatus();
}

void PatternStrip::showStatus()
{
    const auto playback = playbackSentence (shown.mode, processor.patternPlaybackStatus(), processor.performanceInputStatus());
    status.setText (operation.isEmpty() ? playback : operation + " | " + playback, juce::dontSendNotification);
    StripFacts f;
    f.bars = shown.pattern.bars;
    f.numerator = shown.pattern.meter.numerator;
    f.denominator = shown.pattern.meter.denominator;
    f.grid = gridNames[juce::jlimit (0, 4, (int) shown.pattern.grid)];
    f.swing = shown.pattern.swing;
    f.operation = operation;
    f.playback = playback;
    f.status = status.getText();
    f.operationAt = operationAt;
    f.now = services.nowMs();
    for (auto* v : { &view, &narrowView })
        if (v->display != nullptr)
            v->display->show (stripShown (f, v->model.displays.at (v->name)));
}

void PatternStrip::message (const juce::String& text)
{
    operation = text;
    operationAt = services.nowMs();
    showStatus();
}

bool PatternStrip::snapshot (ProjectDocument& d, std::uint64_t& revision)
{
    std::uint64_t reset = 0;
    if (! processor.tryProjectSnapshot (d, revision, reset))
    {
        message ("Pattern is busy; retry the edit");
        return false;
    }
    if (revision == shownRevision && reset == shownReset && d.pattern == shown.pattern)
        return true;
    update (true);
    message ("Edit cancelled: project changed; select the note again");
    return false;
}

void PatternStrip::setMode (bool internal)
{
    ProjectDocument d;
    std::uint64_t revision = 0;
    if (! snapshot (d, revision))
        return;
    const bool ok = processor.setPlaybackMode (internal ? PlaybackMode::internalPattern : PlaybackMode::externalMidi, revision);
    update (true);
    message (ok ? "Mode edit complete" : "Mode edit rejected: pattern changed or values invalid; current pattern preserved");
}

void PatternStrip::exportMidi()
{
    if (choosing)
    {
        message ("Finish or cancel the open pattern chooser first");
        return;
    }
    ProjectDocument committed;
    std::uint64_t revision = 0;
    if (! processor.tryProjectSnapshot (committed, revision))
    {
        message ("Pattern is busy; retry the file operation");
        return;
    }
    choosing = true;
    const juce::Component::SafePointer<PatternStrip> safe (this);
    services.chooseFile ({ "Pattern Export", "*.mid", true, true, {}, this },
                         [safe, captured = committed.pattern] (const juce::File& file)
                         {
                             if (safe == nullptr)
                                 return;
                             safe->choosing = false;
                             if (file == juce::File())
                                 safe->message ("Pattern selection cancelled; committed project preserved");
                             else
                                 safe->message (exportPatternMidi (captured, file)
                                                    ? "MIDI export complete"
                                                    : "MIDI export failed: check the path, permissions and valid pattern asset; committed project preserved");
                         });
}

void PatternStrip::dragMidi (juce::Component& source)
{
    if (choosing)
    {
        message ("Finish or cancel the open pattern chooser first");
        return;
    }
    if (dragging)
    {
        message ("Finish the current MIDI drag first");
        return;
    }
    ProjectDocument committed;
    std::uint64_t revision = 0;
    if (! processor.tryProjectSnapshot (committed, revision))
    {
        message ("Pattern is busy; retry the MIDI drag");
        return;
    }
    dragging = true;
    const juce::Component::SafePointer<PatternStrip> safe (this);
    const auto start = dragPatternMidi (committed.pattern, source, services, [safe]
                                        {
                                            if (safe == nullptr)
                                                return;
                                            safe->dragging = false;
                                            safe->message ("MIDI drag ended; file retained for DAW import");
                                        });
    if (! start.started)
        dragging = false;
    message (start.sentence);
}

void PatternStrip::setUnfolded (bool u)
{
    unfolded = u;
    for (auto* v : { &view, &narrowView })
        if (v->fold != nullptr)
            v->fold->setExpanded (u);
}
} // namespace airack::studio

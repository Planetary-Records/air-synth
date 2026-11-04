#pragma once

#include "InstrumentProcessor.h"
#include "SkinView.h"
#include "StudioServices.h"

#include <optional>

namespace airack::studio
{
/** What the session display is made from: today's sentences, when the status was set, and the processor's state. */
struct SessionFacts
{
    juce::String presetPath, soundName, status, tempoText;
    std::int64_t statusAt = 0, now = 0;
    std::optional<double> hostBpm;
    bool waiting = false, loadError = false;
};

/** The session display: a result's message for its hold, else a standing load error, else the sound's name (the
    preset path's stem for a sound with none); the tempo block either way. ERROR lights while a load error stands or a
    failed result shows. */
SessionShown sessionShown (const SessionFacts& f, const DisplaySpec& d);

/** The SESSION unit over the processor: the processor's preset path with SAVE, LOAD and BROWSE, UNDO and REDO, the
    RACK / ADVANCED lever, the AI toggle and the ERROR LED; a right-click on the display copies the sound's name.
    Today's sentences stay in the undrawn labels status, tempo.status and state.load.error, and the display shows them
    shortened. */
class SessionUnit final : public juce::Component
{
public:
    SessionUnit (InstrumentProcessor& p, const Model& model, StudioServices s);
    void paintOverChildren (juce::Graphics& g) override;

    /** Called on the editor's timer. */
    void refresh();
    /** A status sentence from elsewhere in the editor. */
    void report (const juce::String& sentence);
    juce::String sentence() const { return status.getText(); }

    std::function<void (bool advanced)> onLever;
    std::function<void (bool open)> onAi;
    /** After UNDO, REDO, SAVE or LOAD has run, since they change what the rest of the editor shows. */
    std::function<void()> onEdit;
    void setLever (bool advanced);
    void setAiOpen (bool open);

private:
    juce::Button* button (const char* id) const;
    void result (bool ok, const juce::String& operation);
    void preset (bool writing);
    void browseSounds();
    void browse(bool saving = false);
    /** Takes what an open path entry holds as the path; a press can arrive without the entry losing the focus. */
    void commitEntry();
    void setPath (const juce::String& path);
    void copyName();

    InstrumentProcessor& processor;
    StudioServices services;
    SkinView view;
    juce::Image badge;
    std::unique_ptr<juce::Drawable> wordmark;
    juce::Label status, tempo, loadError;
    /** The processor's preset path the entry last took. Text set on the hidden entry stays until its change message
        arrives, unless the processor's path changes meanwhile. */
    juce::String shownPath;
    std::int64_t statusAt = 0;
    std::uint64_t pathGeneration = 0;
    bool choosing = false;
};
} // namespace airack::studio

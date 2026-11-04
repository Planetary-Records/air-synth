#pragma once
#include "FeedbackReport.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace airack
{
/** "Report a problem": what the musician types, an optional email, the technical details they may include, and the exact
    text that results, shown before anything leaves the panel. The panel never sends anything itself: the musician copies
    the text, saves it to the Desktop, or opens the feedback page in their browser (with only the report ID and version
    in the address). */
class FeedbackPanel final : public juce::Component
{
public:
    struct Host
    {
        std::function<feedback::Environment()> environment;
        std::function<juce::StringArray()> logLines;
        /** Opens the feedback page. Production launches the default browser. */
        std::function<void(const juce::URL&)> openUrl;
        /** Where "Save as file" puts the report. Production uses the Desktop. */
        std::function<juce::File()> saveFolder;
    };

    explicit FeedbackPanel(Host);
    /** Replaces the panel's connections; used by the window and by tests. Takes effect from the next open(). */
    void setHost(Host h) { host = std::move(h); }

    /** Starts a new report with a new ID. */
    void open(bool previousSessionEndedUnexpectedly = false);
    void close();
    bool isOpen() const { return isVisible(); }

    juce::String reportId() const { return id; }
    juce::String reportText() const;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void refresh();
    void copy();
    void save();
    void openPage();
    void say(const juce::String&);

    Host host;
    juce::Random random;
    juce::String id;
    bool endedUnexpectedly = false;
    juce::StringArray log;  // read once when the report starts, not on every keystroke
    juce::Label title, intro, messageLabel, emailLabel, footnote, status;
    juce::TextEditor message, email, preview;
    juce::ToggleButton details;
    juce::TextButton copyButton, saveButton, openButton, closeButton;
    juce::Rectangle<int> card;
};
}

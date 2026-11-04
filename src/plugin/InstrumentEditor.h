#pragma once
#include "InstrumentProcessor.h"
#include "FeedbackPanel.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace airack
{
namespace studio { struct Model; }
class InstrumentEditor final : public juce::AudioProcessorEditor
{
public:
    /** The smallest window, and the one it opens at; at this size with no side unit the rack shows at its readable size. */
    static constexpr int minimumWidth=1080,minimumHeight=680;
    explicit InstrumentEditor(InstrumentProcessor&);
    /** Without a skin it shows one line of text instead of the units. */
    InstrumentEditor(InstrumentProcessor&,const studio::Model* skin);
    ~InstrumentEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    /** Offers to prepare a report after a session that did not end normally (once per process). */
    void showUnexpectedEndNotice();
private:
    // Advanced bank controls and their attachments are retained by Content
    // while the native personality view is shown or hidden.
    struct Content;
    struct AlphaBadge;
    struct NoticeBar;
    std::unique_ptr<Content> content;
    std::unique_ptr<AlphaBadge> alphaBadge;
    std::unique_ptr<FeedbackPanel> feedbackPanel;
    std::unique_ptr<NoticeBar> notice;
    juce::Label skinError;
    juce::TooltipWindow tooltips{this,700};
};
}

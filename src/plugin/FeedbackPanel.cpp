#include "FeedbackPanel.h"
#include "ReleaseInfo.h"

namespace airack
{
namespace
{
const juce::Colour ink(0xfff2e6c9), dim(0xffb9b2a2), amber(0xffe8b04a), cardFill(0xff1b1f25), field(0xff12151a);

void name(juce::Component& c, const char* idText, const juce::String& accessible)
{
    c.setComponentID(idText);
    c.setName(accessible);
    c.setTitle(accessible);
}

void style(juce::TextEditor& e)
{
    e.setColour(juce::TextEditor::backgroundColourId, field);
    e.setColour(juce::TextEditor::textColourId, ink);
    e.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff3a4048));
    e.setColour(juce::TextEditor::focusedOutlineColourId, amber);
    e.setColour(juce::CaretComponent::caretColourId, amber);
    e.setFont(juce::FontOptions(14.0f));
}

void label(juce::Label& l, const juce::String& text, float size, juce::Colour colour, bool bold = false)
{
    l.setText(text, juce::dontSendNotification);
    l.setFont(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
    l.setColour(juce::Label::textColourId, colour);
    l.setInterceptsMouseClicks(false, false);
}

void button(juce::TextButton& b, const char* idText, const juce::String& text)
{
    name(b, idText, text);
    b.setButtonText(text);
    b.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff262b32));
    b.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff3a4048));
    b.setColour(juce::TextButton::textColourOffId, ink);
    b.setColour(juce::TextButton::textColourOnId, ink);
}
}

FeedbackPanel::FeedbackPanel(Host h) : host(std::move(h)), random(juce::Time::currentTimeMillis())
{
    name(*this, "feedback.panel", "Report a problem");
    setVisible(false);
    setInterceptsMouseClicks(true, true);
    setWantsKeyboardFocus(false);

    label(title, "Report a problem or share feedback", 22.0f, ink, true);
    label(intro, "air is an early test version. Tell us what happened, or what you wish it did.", 14.0f, dim);
    label(messageLabel, "What happened?", 14.0f, ink, true);
    label(emailLabel, "Your email (optional, only if you would like a reply)", 14.0f, ink, true);
    label(footnote, "Nothing is sent from air. Copy the report or save it, then send it with the button below.", 13.0f, dim);
    label(status, {}, 13.0f, amber);

    name(message, "feedback.message", "What happened");
    message.setMultiLine(true, true);
    message.setReturnKeyStartsNewLine(true);
    message.setTextToShowWhenEmpty("For example: the sound cuts out after I save my project.", dim);
    name(email, "feedback.email", "Your email, optional");
    email.setMultiLine(false);
    email.setTextToShowWhenEmpty("name@example.com", dim);
    name(preview, "feedback.preview", "The report that will be copied or saved");
    preview.setMultiLine(true, false);
    preview.setReadOnly(true);
    preview.setCaretVisible(false);
    for (auto* e : { &message, &email, &preview })
        style(*e);
    for (auto* e : { &message, &email })
    {
        e->onTextChange = [this] { refresh(); };
        e->onEscapeKey = [this] { close(); };
    }

    name(details, "feedback.details", "Include technical details");
    details.setButtonText("Include technical details (shown in the report below)");
    details.setToggleState(true, juce::dontSendNotification);
    details.setColour(juce::ToggleButton::textColourId, ink);
    details.setColour(juce::ToggleButton::tickColourId, amber);
    details.onClick = [this] { refresh(); };

    button(copyButton, "feedback.copy", "Copy report");
    button(saveButton, "feedback.save", "Save to Desktop");
    button(openButton, "feedback.open", "Open the feedback page");
    button(closeButton, "feedback.close", "Close");
    copyButton.onClick = [this] { copy(); };
    saveButton.onClick = [this] { save(); };
    openButton.onClick = [this] { openPage(); };
    closeButton.onClick = [this] { close(); };

    for (auto* c : std::initializer_list<juce::Component*>{ &title, &intro, &messageLabel, &message, &emailLabel, &email, &details,
                                                            &preview, &footnote, &status, &copyButton, &saveButton, &openButton, &closeButton })
        addAndMakeVisible(*c);
}

void FeedbackPanel::open(bool unexpected)
{
    endedUnexpectedly = unexpected;
    id = feedback::newReportId(random);
    message.clear();
    email.clear();
    details.setToggleState(true, juce::dontSendNotification);
    status.setText({}, juce::dontSendNotification);
    log = host.logLines ? host.logLines() : juce::StringArray();
    refresh();
    setVisible(true);
    toFront(true);
    message.grabKeyboardFocus();
}

void FeedbackPanel::close()
{
    if (! isVisible())
        return;
    setVisible(false);
}

juce::String FeedbackPanel::reportText() const
{
    feedback::Input input;
    input.message = message.getText();
    input.email = email.getText();
    input.includeDetails = details.getToggleState();
    auto environment = host.environment ? host.environment() : feedback::Environment{};
    environment.releaseVersion = airack::releaseVersion;
    if (endedUnexpectedly)
        environment.previousSessionEndedNormally = false;
    return feedback::buildReport(input, environment, id, log);
}

void FeedbackPanel::refresh()
{
    preview.setText(reportText(), false);
}

void FeedbackPanel::say(const juce::String& text)
{
    status.setText(text, juce::dontSendNotification);
}

void FeedbackPanel::copy()
{
    juce::SystemClipboard::copyTextToClipboard(reportText());
    say("Copied. You can paste it into the feedback page or an email.");
}

void FeedbackPanel::save()
{
    const auto folder = host.saveFolder ? host.saveFolder() : juce::File::getSpecialLocation(juce::File::userDesktopDirectory);
    const auto file = folder.getChildFile("air-feedback-" + id + ".txt");
    if (folder.createDirectory().wasOk() && file.replaceWithText(reportText()))
        say("Saved to your Desktop as " + file.getFileName() + ".");
    else
        say("Could not save the file. Use Copy report instead.");
}

void FeedbackPanel::openPage()
{
    const auto url = feedback::feedbackPageUrl(id, airack::releaseVersion);
    if (host.openUrl)
        host.openUrl(url);
    else
        url.launchInDefaultBrowser();
    say("The feedback page opened in your browser. Paste your report there. Report ID: " + id);
}

void FeedbackPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xcc000000));
    g.setColour(cardFill);
    g.fillRoundedRectangle(card.toFloat(), 10.0f);
    g.setColour(amber);
    g.drawRoundedRectangle(card.toFloat().reduced(0.5f), 10.0f, 1.2f);
    g.setColour(dim);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("Report ID " + id, card.withTrimmedRight(24).withHeight(40).translated(0, 14), juce::Justification::centredRight, false);
}

void FeedbackPanel::resized()
{
    const int w = juce::jmin(760, getWidth() - 40), h = juce::jmin(620, getHeight() - 30);
    card = juce::Rectangle<int>(w, h).withCentre(getLocalBounds().getCentre());
    auto area = card.reduced(28, 22);
    title.setBounds(area.removeFromTop(30));
    intro.setBounds(area.removeFromTop(24));
    area.removeFromTop(8);
    messageLabel.setBounds(area.removeFromTop(22));
    message.setBounds(area.removeFromTop(juce::jmax(70, h / 6)));
    area.removeFromTop(8);
    emailLabel.setBounds(area.removeFromTop(22));
    email.setBounds(area.removeFromTop(30));
    area.removeFromTop(6);
    details.setBounds(area.removeFromTop(26));
    area.removeFromTop(4);
    auto bottom = area.removeFromBottom(34);
    status.setBounds(area.removeFromBottom(24));
    footnote.setBounds(area.removeFromBottom(22));
    area.removeFromBottom(4);
    preview.setBounds(area);
    const int gap = 10, count = 4, each = (bottom.getWidth() - gap * (count - 1)) / count;
    for (auto* b : { &copyButton, &saveButton, &openButton, &closeButton })
    {
        b->setBounds(bottom.removeFromLeft(each));
        bottom.removeFromLeft(gap);
    }
}

bool FeedbackPanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey) { close(); return true; }
    return false;
}
}

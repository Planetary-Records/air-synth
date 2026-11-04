#include "ProbeEditor.h"
#include "ProbeHostDiagnostics.h"

ProbeEditor::ProbeEditor(ProbeProcessor& p) : AudioProcessorEditor(p), probe(p), controls(p)
{
    for (auto* text : { &identities, &observation })
    {
        text->setMultiLine(true); text->setReadOnly(true); text->setScrollbarsShown(true); addAndMakeVisible(text);
    }
    identities.setName("Probe identities"); observation.setName("Latest process observation");
    group.setName("Test group UUID"); group.setTextToShowWhenEmpty("Enter the copied 32-digit test group UUID", juce::Colours::grey);
    addAndMakeVisible(group); addAndMakeVisible(status); addAndMakeVisible(controls);
    for (auto* button : { &create, &copy, &join, &clear, &save, &load, &exportTrace, &configure }) addAndMakeVisible(button);
    create.onClick = [this] { probe.createTestGroup(); timerCallback(); };
    clear.onClick = [this] { probe.clearTestGroup(); timerCallback(); };
    copy.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(probe.identitySnapshot()["group"].toString()); };
    join.onClick = [this]
    {
        const auto input = group.getText().trim();
        if (input.length() != 32 || input.retainCharacters("0123456789abcdefABCDEF") != input || juce::Uuid(input).isNull())
        { status.setText("Enter a non-zero 32-digit hexadecimal group UUID.", juce::dontSendNotification); return; }
        probe.joinTestGroup(juce::Uuid(input)); timerCallback();
    };
    save.onClick = [this] { chooseFile(0); }; load.onClick = [this] { chooseFile(1); };
    exportTrace.onClick = [this] { chooseFile(2); };
    configure.setTooltip("With transport stopped and Live Configure enabled: gestures p000-p128, briefly changes each value, then restores it. This can write host automation.");
    configure.onClick = [this]
    {
        probe.configureTestControls();
        status.setText("Gestured p000-p128; values restored. Verify Live's actual configured count.", juce::dontSendNotification);
    };
    status.setText("M0 diagnostics only. Stop transport before Configure 129; host automation may be written.", juce::dontSendNotification);
    setSize(820, 760); timerCallback(); startTimer(500);
}
void ProbeEditor::resized()
{
    auto area = getLocalBounds().reduced(8);
    identities.setBounds(area.removeFromTop(72)); area.removeFromTop(4);
    observation.setBounds(area.removeFromTop(64)); area.removeFromTop(4);
    auto row = area.removeFromTop(28);
    for (auto* button : { &create, &copy, &join, &clear }) button->setBounds(row.removeFromLeft(199).reduced(2));
    group.setBounds(area.removeFromTop(26).reduced(2));
    row = area.removeFromTop(28);
    for (auto* button : { &save, &load, &exportTrace, &configure }) button->setBounds(row.removeFromLeft(199).reduced(2));
    status.setBounds(area.removeFromTop(32)); controls.setBounds(area);
}
void ProbeEditor::timerCallback()
{
    const auto ids = probe.identitySnapshot();
    const auto trace = probe::ProbeHostDiagnostics::snapshot();
    identities.setText("Runtime: " + ids["runtime"].toString() + "   Member: " + ids["member"].toString()
        + "\nGroup: " + ids["group"].toString() + "\nPID: " + trace["pid"].toString() + "   Process start: " + trace["process_start"].toString()
        + "   Sequence: " + trace["sequence"].toString() + "   Dropped: " + trace["dropped"].toString(), false);
    const auto& records = trace["records"];
    if (records.size() > 0) observation.setText(juce::JSON::toString(records[records.size() - 1], true), false);
}
void ProbeEditor::chooseFile(int action)
{
    if (chooser) return;
    chooser = std::make_unique<juce::FileChooser>(action == 0 ? "Save parameter-only native sound" : action == 1 ? "Load parameter-only native sound" : "Export bounded process trace",
        juce::File(), action == 2 ? "*.json" : "*.probe-sound");
    const auto flags = action == 1 ? juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  : juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting;
    chooser->launchAsync(flags, [safe = juce::Component::SafePointer<ProbeEditor>(this), action](const juce::FileChooser& dialog)
    {
        if (safe == nullptr) return;
        const auto file = dialog.getResult();
        if (file != juce::File())
        {
            bool ok = false;
            if (action == 0) { const auto data = safe->probe.saveNativeSound(); ok = file.replaceWithData(data.getData(), data.getSize()); }
            if (action == 1)
            {
                juce::MemoryBlock data;
                ok = file.getSize() == 1320 && file.loadFileAsData(data) && safe->probe.loadNativeSound(data.getData(), static_cast<int>(data.getSize()));
            }
            if (action == 2) ok = file.replaceWithText(juce::JSON::toString(probe::ProbeHostDiagnostics::snapshot()));
            safe->status.setText(ok ? "Completed: " + file.getFileName() : "Failed or invalid file; no sound loaded.", juce::dontSendNotification);
        }
        safe->chooser.reset();
        safe->timerCallback();
    });
}

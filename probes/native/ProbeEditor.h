#pragma once
#include "ProbeProcessor.h"

class ProbeEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ProbeEditor(ProbeProcessor&);
    void resized() override;
private:
    void timerCallback() override;
    void chooseFile(int action);
    ProbeProcessor& probe;
    juce::GenericAudioProcessorEditor controls;
    juce::TextEditor identities, observation, group;
    juce::Label status;
    juce::TextButton create { "Create test group" }, copy { "Copy group" }, join { "Join entered group" }, clear { "Clear group" };
    juce::TextButton save { "Save native sound" }, load { "Load native sound" }, exportTrace { "Export trace JSON" }, configure { "Configure 129" };
    std::unique_ptr<juce::FileChooser> chooser;
};

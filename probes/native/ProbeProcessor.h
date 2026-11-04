#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>

class ProbeProcessor final : public juce::AudioProcessor,
                             private juce::AudioProcessorParameter::Listener
{
public:
    ProbeProcessor();
    ~ProbeProcessor() override;
    void prepareToPlay(double, int) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    juce::var identitySnapshot();
    void createTestGroup();
    void joinTestGroup(const juce::Uuid&);
    void clearTestGroup();
    juce::MemoryBlock saveNativeSound();
    bool loadNativeSound(const void*, int);
    void configureTestControls();
    const juce::Uuid& runtimeIdentity() const { return runtimeId; }
    const juce::String getName() const override { return "Rack Synth Probe"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
private:
    void parameterValueChanged(int, float) override { editCount.fetch_add(1, std::memory_order_relaxed); }
    void parameterGestureChanged(int, bool) override {}
    std::array<juce::AudioParameterFloat*, 160> controls {};
    juce::CriticalSection stateLock;
    juce::Uuid groupId { juce::Uuid::null() }, memberId;
    const juce::Uuid runtimeId;
    std::atomic<juce::uint64> editCount { 0 };
    double rate = 44100.0, phase = 0.0, phaseStep = 0.0;
    float velocity = 0.0f;
    int activeNote = -1, activeChannel = 0;
    bool wasPlaying = false;
};

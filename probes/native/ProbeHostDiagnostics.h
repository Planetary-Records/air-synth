#pragma once
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>
namespace Steinberg { class IBStream; namespace Vst { class IAttributeList; } }

namespace probe
{
struct StateOriginObservation { juce::var fields; };
struct ChannelObservation { juce::var fields; };
StateOriginObservation observeState(Steinberg::IBStream*, bool saving);
ChannelObservation observeChannel(Steinberg::Vst::IAttributeList*);
void recordState(juce::AudioProcessor&, Steinberg::IBStream*, bool saving);
void recordChannel(juce::AudioProcessor&, Steinberg::Vst::IAttributeList*);
class ProbeHostDiagnostics
{
public:
    static void record(const juce::Uuid& runtime, const juce::String& kind, juce::var details);
    static juce::var snapshot();
};
}

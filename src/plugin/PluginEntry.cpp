#include "InstrumentProcessor.h"
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new airack::InstrumentProcessor; }

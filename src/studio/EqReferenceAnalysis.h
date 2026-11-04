#pragma once
#include "EqCurveFit.h"
#include <juce_core/juce_core.h>
#include <optional>

namespace airack::studio
{
struct EqReference {
    EqCurve spectrum;
    std::bitset<129> valid;
    double sampleRate=0;
    juce::String label,captureId;
};
// Control-worker work only; no decoded audio is retained in the reference.
std::optional<EqReference> analyzeEqReferenceWav(const juce::File&,const std::atomic_bool& cancel);
bool encodeEqReference(const EqReference&,juce::String&);
bool decodeEqReference(const juce::String&,EqReference&);
std::optional<EqCurve> eqMatchCorrection(const EqReference& source,const EqReference& reference,float amount);
}

#pragma once

#include "SynthEngine.h"
#include <cstdint>
#include <span>
#include <vector>

namespace airack
{
inline constexpr std::size_t eqParameterStart = 759, eqParametersPerSlot = 9;
inline constexpr std::size_t filterParameterStart = eqParameterStart + 8 * eqParametersPerSlot, filterParametersPerSlot = 5;
inline constexpr std::size_t chorusParameterStart = filterParameterStart + 8 * filterParametersPerSlot, chorusParametersPerSlot = 5;
inline constexpr std::size_t phaserParameterStart = chorusParameterStart + 8 * chorusParametersPerSlot, phaserParametersPerSlot = 5;
inline constexpr std::size_t flangerParameterStart = phaserParameterStart + 8 * phaserParametersPerSlot, flangerParametersPerSlot = 5;
inline constexpr std::size_t stereoParameterStart = flangerParameterStart + 8 * flangerParametersPerSlot, stereoParametersPerSlot = 4;
inline constexpr std::size_t dimensionParameterStart = stereoParameterStart + 8 * stereoParametersPerSlot, dimensionParametersPerSlot = 4;
inline constexpr std::size_t frequencyShiftParameterStart = dimensionParameterStart + 8 * dimensionParametersPerSlot, frequencyShiftParametersPerSlot = 3;
inline constexpr std::size_t convolutionParameterStart = frequencyShiftParameterStart + 8 * frequencyShiftParametersPerSlot, convolutionParametersPerSlot = 4;
inline constexpr std::size_t frequencySplitParameterStart = convolutionParameterStart + 8 * convolutionParametersPerSlot, frequencySplitParametersPerSlot = 8;
inline constexpr std::size_t midSideSplitParameterStart = frequencySplitParameterStart + 8 * frequencySplitParametersPerSlot, midSideSplitParametersPerSlot = 7;
inline constexpr std::size_t dynamicEqParameterStart = midSideSplitParameterStart + 8 * midSideSplitParametersPerSlot;
inline constexpr std::size_t dynamicEqParametersPerBand = 11, dynamicEqParametersPerSlot = 89;
inline constexpr std::size_t dynamicEqGainParameterStart = dynamicEqParameterStart + 8 * dynamicEqParametersPerSlot;
inline constexpr std::size_t dynamicEqGainParametersPerSlot = 2;
inline constexpr std::size_t dynamicEqPlacementParameterStart = dynamicEqGainParameterStart + 8 * dynamicEqGainParametersPerSlot;
inline constexpr std::size_t dynamicEqPlacementParametersPerSlot = 8;
inline constexpr std::size_t delayTapeParameterStart = dynamicEqPlacementParameterStart + 8 * dynamicEqPlacementParametersPerSlot;
inline constexpr std::size_t delayTapeParametersPerSlot = 5;
inline constexpr std::size_t dynamicEqExtensionParameterStart = delayTapeParameterStart + 8 * delayTapeParametersPerSlot;
inline constexpr std::size_t dynamicEqExtensionParametersPerBand = 2, dynamicEqExtensionParametersPerSlot = 16;
inline constexpr std::size_t delayStereoParameterStart = dynamicEqExtensionParameterStart + 8 * dynamicEqExtensionParametersPerSlot;
inline constexpr std::size_t delayStereoParametersPerSlot = 3;
inline constexpr std::size_t dynamicEqDetectorParameterStart = delayStereoParameterStart + 8 * delayStereoParametersPerSlot;
inline constexpr std::size_t dynamicEqDetectorParametersPerBand = 5, dynamicEqDetectorParametersPerSlot = 40;
inline constexpr std::size_t dynamicEqSpectralParameterStart = dynamicEqDetectorParameterStart + 8 * dynamicEqDetectorParametersPerSlot;
inline constexpr std::size_t dynamicEqSpectralParametersPerBand = 3, dynamicEqSpectralParametersPerSlot = 25;
inline constexpr std::size_t spaceReverbParameterStart = dynamicEqSpectralParameterStart + 8 * dynamicEqSpectralParametersPerSlot;
inline constexpr std::size_t spaceReverbParametersPerSlot = 96;
inline constexpr std::size_t wavetableWarmthParameterStart = spaceReverbParameterStart + 8 * spaceReverbParametersPerSlot;
inline constexpr std::size_t wavetableWarmthParametersPerSlot = 9;
inline constexpr std::size_t eraReverbParameterStart = wavetableWarmthParameterStart + 4 * wavetableWarmthParametersPerSlot;
inline constexpr std::size_t eraReverbParametersPerSlot = 15;
inline constexpr std::size_t drumParameterStart = eraReverbParameterStart + 8 * eraReverbParametersPerSlot;
inline constexpr std::size_t drumParametersPerPad = 9, drumParametersPerSlot = 16 * drumParametersPerPad;
inline constexpr std::size_t drumModeParameterStart = drumParameterStart + 4 * drumParametersPerSlot;
inline constexpr std::size_t saturationParameterStart = drumModeParameterStart + 4;
inline constexpr std::size_t saturationGlobalParameters = 11, saturationParametersPerLane = 6, saturationParametersPerSlot = 35;
inline constexpr std::size_t parameterCount = saturationParameterStart + 8 * saturationParametersPerSlot;
static_assert(wavetableWarmthParameterStart == 3503 && eraReverbParameterStart == 3539);
static_assert(drumParameterStart == 3659 && drumModeParameterStart == 4235 && saturationParameterStart == 4239 && parameterCount == 4519);
using ParameterValues = std::array<float, parameterCount>;
enum class ParameterKind { continuous, choice, toggle };
struct ParameterDescriptor
{
    juce::String id, name, unit;
    ParameterKind kind;
    float minimum, maximum, defaultValue, skew;
    std::vector<juce::String> choices;
    std::uint32_t stateKey;
    bool automatable = true;
};

// Construct/cache the catalog on the control thread before starting audio.
std::span<const ParameterDescriptor> parameterRegistry();
ParameterValues defaultParameterValues();
std::optional<std::size_t> parameterIndexForKey(std::uint32_t) noexcept;
bool validateParameterValues(const ParameterValues&) noexcept;
// Numeric metadata only; callers validate normalized values before physical conversion.
bool isContinuousParameter(std::size_t) noexcept;
float parameterPhysicalValue(const ParameterValues&, std::size_t) noexcept;
// Validates every bank; invalid input leaves the destination unchanged.
bool makeSynthPatch(const ParameterValues&, SynthPatch&) noexcept;
}

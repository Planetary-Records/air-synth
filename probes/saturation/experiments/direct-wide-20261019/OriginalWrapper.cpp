#define airack wide_direct_original
#include "baseline/SaturationWideNetwork.h"
#undef airack
#include "tests/WideDirectApi.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <type_traits>

namespace wide_direct_layout {
namespace {
namespace w = wide_direct_original::probe::wide_network;

static_assert(sizeof(w::State) == sizeof(double) * 72);
static_assert(alignof(w::State) >= 32);
static_assert(std::is_standard_layout_v<w::State>);
w::NetworkCoefficients native(const Network& values) noexcept {
    const auto one = [](const Coefficients& c) { return w::Coefficients{c[0], c[1], c[2], c[3], c[4]}; };
    return {one(values[0]), one(values[1]), one(values[2]), one(values[3]), one(values[4])};
}
template<class State, class Apply> void histories(State& state, Apply apply) noexcept {
    std::size_t offset = 0;
    const auto one = [&](auto& biquad) {
        for (auto& value : biquad.z1) apply(value, offset++);
        for (auto& value : biquad.z2) apply(value, offset++);
    };
    for (auto& biquad : state.lowLP) one(biquad);
    for (auto& biquad : state.lowHP) one(biquad);
    for (auto& biquad : state.highLP) one(biquad);
    for (auto& biquad : state.highHP) one(biquad);
    one(state.lowCompensation);
}
}
bool avxAdmitted() noexcept {
    return juce::SystemStats::hasAVX() && w::operatingSystemSupportsAvx();
}
void* createOriginal() { return new w::State{}; }
void destroyOriginal(void* handle) noexcept { delete static_cast<w::State*>(handle); }
void clearOriginal(void* handle) noexcept { *static_cast<w::State*>(handle) = {}; }
void seedOriginal(void* handle, const History& values) noexcept {
    histories(*static_cast<w::State*>(handle), [&](double& value, std::size_t offset) { value = values[offset]; });
}
void runOriginal(void* handle, const Network& coefficients, const InputBlock& wet,
                 const InputBlock& dry, LegacyResult& result) noexcept {
    auto& state = *static_cast<w::State*>(handle);
    w::process4(state, native(coefficients), wet, dry, result.bands);
    histories(state, [&](double value, std::size_t offset) { result.history[offset] = value; });
}
}

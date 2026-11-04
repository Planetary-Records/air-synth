#define airack wide_direct_proposed
#include "proposed/SaturationWideNetwork.h"
#undef airack
#include "tests/WideDirectApi.h"
#include <algorithm>
#include <type_traits>

namespace wide_direct_layout {
namespace {
namespace w = wide_direct_proposed::probe::wide_network;

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
void* createProposed() { return new w::State{}; }
void destroyProposed(void* handle) noexcept { delete static_cast<w::State*>(handle); }
void clearProposed(void* handle) noexcept { *static_cast<w::State*>(handle) = {}; }
void seedProposed(void* handle, const History& values) noexcept {
    histories(*static_cast<w::State*>(handle), [&](double& value, std::size_t offset) { value = values[offset]; });
}
void runProposed(void* handle, const Network& coefficients, const InputBlock& wet,
                 const InputBlock& dry, DirectResult& result) noexcept {
    auto& state = *static_cast<w::State*>(handle);
    w::process4(state, native(coefficients), wet, dry, result.wet, result.dry);
    histories(state, [&](double value, std::size_t offset) { result.history[offset] = value; });
}
}

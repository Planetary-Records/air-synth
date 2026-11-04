#pragma once
#include <array>

namespace airack::probe::wide_network {
using Frame = std::array<double, 2>;
using Quad = std::array<double, 4>;
using InputBlock = std::array<Frame, 4>;
using WetBandBlock = std::array<InputBlock, 3>;
using DryBandBlock = std::array<std::array<Frame, 3>, 4>;
struct Coefficients { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
struct NetworkCoefficients { Coefficients lowLP, lowHP, highLP, highHP, highAP; };
struct alignas(32) BiquadState { Quad z1 {}, z2 {}; };
struct State {
    std::array<BiquadState, 2> lowLP, lowHP, highLP, highHP;
    BiquadState lowCompensation;
};
// JUCE's pinned Windows hasAVX() checks CPUID only. Call this at prepare time
// as well, before ever entering the AVX kernel.
bool operatingSystemSupportsAvx() noexcept;
// All four frames use the same coefficients. Quad lanes are wet L/R, dry L/R.
void process4(State&, const NetworkCoefficients&, const InputBlock& wet,
              const InputBlock& dry, WetBandBlock&, DryBandBlock&) noexcept;
}

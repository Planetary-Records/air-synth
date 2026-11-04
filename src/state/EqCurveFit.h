#pragma once
#include "DynamicEq.h"
#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>

namespace airack
{
struct EqCurve { std::array<float,129> db{}; };
struct EqFitResult {
    bool ready=false;
    DynamicEqSettings settings{};
    float rmsDb=0;
    std::uint32_t evaluations=0;
};
// Bounded control-thread work. The available mask never permits replacing an
// active band. Output contains a copy; cancellation never publishes a fit.
EqFitResult fitEqCurve(const EqCurve&, const DynamicEqSettings& base,
    std::bitset<8> available, double sampleRate, const std::atomic_bool* cancel=nullptr);
}

#pragma once

#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
#include <array>
#include <cstdint>
namespace airack::probe {
enum class OptimizationStage : unsigned { candidate, controls, prefix, crossover, lane, returning, count };
struct OptimizationProfile {
    std::array<std::uint64_t, unsigned(OptimizationStage::count)> nanoseconds {}, calls {};
    std::array<std::uint64_t, 9> selectors {};
    std::array<std::array<std::uint64_t, 16>, 4> coefficientRows {};
    std::array<std::uint64_t, 3> temporalEntries {}, preparedFallbacks {}, legacyEntries {};
    std::uint64_t denseLowEntries = 0;
};
void optimizationProfileSample(bool enabled) noexcept;
void optimizationProfileReset() noexcept;
OptimizationProfile optimizationProfileRead() noexcept;
OptimizationProfile runSaturationOptimizationProfileOverhead() noexcept;
int runSaturationOptimizationPathChecks();
}
#endif

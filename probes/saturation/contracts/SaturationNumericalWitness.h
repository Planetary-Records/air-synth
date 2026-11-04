#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
namespace airack::probe {
struct SaturationNumericalRecord {
    std::uintptr_t lane;
    int site, character, channel, kind, previousKind;
    double u, p, q, currentPrimitive, priorPrimitive, cachedPrimitive;
};
int runSaturationNumericalPathChecks();
int runSaturationProductStateIntegrationChecks();
std::span<const SaturationNumericalRecord> saturationNumericalRecords() noexcept;
void saturationNumericalConstants(double& direct, double& prepared, double& normalizer, std::size_t& zeroHalfWidths) noexcept;
}
#endif

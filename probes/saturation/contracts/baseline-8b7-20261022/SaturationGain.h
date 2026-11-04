#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace airack::gain_math {
// Original scalar law retained for out-of-domain inputs and independent tests.
inline double analyticGain(double db) noexcept { return std::pow(10.,db/20); }
struct GainCurve {
    static constexpr std::size_t cells=768;
    static constexpr double lowerDb=-48,upperDb=48,cellsPerDb=8;
    static constexpr double decibelsToNaturalLog=std::numbers::ln10/20;
    std::array<double,cells> centers{};
    static constexpr double centerDb(int cell) noexcept {
        return lowerDb+(double(cell)+.5)/cellsPerDb;
    }
    // Lifecycle/preparation only. No dynamic allocation or captured data.
    GainCurve() noexcept {
        for(std::size_t cell=0;cell<cells;++cell)centers[cell]=analyticGain(centerDb(int(cell)));
    }
    double gain(double db) const noexcept {
        if(db==0)return 1; // Keep the exact neutral endpoint, including -0dB.
        if(!(db>=lowerDb&&db<=upperDb))return analyticGain(db);
        const int cell=std::min(int((db-lowerDb)*cellsPerDb),int(cells)-1);
        const double z=(db-centerDb(cell))*decibelsToNaturalLog;
        // |z|<.0072. Exp Taylor degree6: relative truncation<2.02e-19.
        double p=1.0/720;
        p=1.0/120+z*p;
        p=1.0/24+z*p;
        p=1.0/6+z*p;
        p=.5+z*p;
        p=1+z*p;
        p=1+z*p;
        return centers[cell]*p;
    }
};
static_assert(sizeof(GainCurve)==6144);
} // namespace airack::gain_math

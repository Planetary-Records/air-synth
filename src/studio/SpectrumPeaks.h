#pragma once
#include "FxSpectrum.h"
#include <array>
#include <cstddef>

namespace airack::studio
{
struct SpectrumPeak
{
    double frequencyHz=0;
    float levelDb=0,q=1,prominenceDb=0;
};
struct SpectrumPeaks
{
    std::array<SpectrumPeak,8> values{};
    std::size_t size=0;
};
// Immutable message-thread display data only. Fixed storage, no FFT/FIFO work.
SpectrumPeaks findSpectrumPeaks(const FxSpectrum::Result&,bool output=false) noexcept;
}

#pragma once
#include <cmath>

#if defined(_WIN32)
#if !defined(_MSC_VER) || !defined(_M_X64) || defined(__clang__)
#error Saturation private gain import requires the validated MSVC Windows x64 ABI
#endif
extern "C" __declspec(dllimport) double __cdecl airack_saturation_release_pow(double, double) noexcept;
#endif

namespace airack::gain_math {
inline double canonicalPower10(double exponent) noexcept {
#if defined(_WIN32)
    return airack_saturation_release_pow(10., exponent);
#else
    return std::pow(10., exponent);
#endif
}
} // namespace airack::gain_math

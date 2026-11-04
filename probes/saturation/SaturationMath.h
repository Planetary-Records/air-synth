#pragma once
#include <cmath>
#include <numbers>

// Isolated development prototype. No tracked source includes this header.
// Preserve ordinary binary64 evaluation, current compiler FP settings and the
// public analytic oracle. No FMA, lookup table, external dependency or FP-mode change.
namespace airack::coefficient_math {
struct SinCos { double sn, cs; };
inline constexpr double maximumDesignAngle = 2.83;
inline constexpr double reducedRadiusBound = .786;

// Original-design admitted arguments lie in [0, 0.9*pi] (rounded w can be one
// ulp above 0.9*pi). The conservative 2.83 fence covers that entire domain.
// High/low centre constants retain argument accuracy near pi/2 and pi.
inline SinCos designSinCos(double w) noexcept {
    if (w == 0) return { w, 1 }; // Preserve signed zero in the standalone helper.
    if (!(w >= 0 && w <= maximumDesignAngle)) return { std::sin(w), std::cos(w) };
    constexpr double piHi = std::numbers::pi;
    constexpr double piLo = 1.2246467991473532071737640294583966e-16;
    constexpr double halfPiHi = std::numbers::pi / 2;
    constexpr double halfPiLo = 6.123233995736766035868820147291983e-17;
    int quadrant = 0;
    double r = w;
    if (w > 3 * piHi / 4) { quadrant = 2; r = (w - piHi) - piLo; }
    else if (w > piHi / 4) { quadrant = 1; r = (w - halfPiHi) - halfPiLo; }
    const double z = r * r;
    // Taylor through degree17. First omitted term <= .786^19/19! <8.48e-20.
    double s = 1.0 / 355687428096000.0;
    s = -1.0 / 1307674368000.0 + z * s;
    s = 1.0 / 6227020800.0 + z * s;
    s = -1.0 / 39916800.0 + z * s;
    s = 1.0 / 362880.0 + z * s;
    s = -1.0 / 5040.0 + z * s;
    s = 1.0 / 120.0 + z * s;
    s = -1.0 / 6.0 + z * s;
    s = r + (r * z) * s;
    // Taylor through degree16. First omitted term <= .786^18/18! <2.05e-18.
    double c = 1.0 / 20922789888000.0;
    c = -1.0 / 87178291200.0 + z * c;
    c = 1.0 / 479001600.0 + z * c;
    c = -1.0 / 3628800.0 + z * c;
    c = 1.0 / 40320.0 + z * c;
    c = -1.0 / 720.0 + z * c;
    c = 1.0 / 24.0 + z * c;
    c = -1.0 / 2.0 + z * c;
    c = 1 + z * c;
    if (quadrant == 1) return { c, -s };
    if (quadrant == 2) return { -s, -c };
    return { s, c };
}
} // namespace airack::coefficient_math

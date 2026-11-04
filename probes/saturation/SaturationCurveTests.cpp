#include "SaturationCandidate.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string_view>

// Standalone numerical witness. The unchanged public scalar helpers are the
// oracle; this file neither replaces their tests nor changes a quality gate.
int main() {
    using namespace airack;
    using namespace airack::probe;
    constexpr double epsilon = std::numeric_limits<double>::epsilon();
    std::size_t checks = 0, failures = 0, fastQuotients = 0, exactQuotients = 0;
    double maxPrimitiveError = 0, maxAdaaError = 0;
    const auto check = [&](bool okay) { ++checks; if (!okay) ++failures; };
    const auto primitiveCheck = [&](SaturationCharacter c, double x) {
        const double oracle = primitive(c, x), candidate = preparedPrimitive(c, x);
        const double error = std::abs(candidate - oracle);
        maxPrimitiveError = std::max(maxPrimitiveError, error / (1 + std::abs(oracle)));
        check(std::isfinite(candidate));
        check(error <= 16 * epsilon * (1 + std::abs(oracle)));
    };
    const auto quotientCheck = [&](SaturationCharacter c, double x, double prior) {
        const double oracle = adaa(c, x, prior), candidate = preparedAdaa(c, x, prior);
        const double error = std::abs(candidate - oracle);
        maxAdaaError = std::max(maxAdaaError, error);
        check(std::isfinite(candidate));
        // The primitive quotient is scaled by R=1/16 in the integrated lane.
        check(error / 16 <= 1e-12);
        if (preparedAdaaUsesExactPrimitive(x, prior)) ++exactQuotients;
        else ++fastQuotients;
    };
    constexpr std::array characters { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense };
    for (const auto c : characters) {
        // Cell interior, exact seams, and the adjacent representable doubles.
        // Glow's translated argument and Dense's translated/scaled knee land
        // exactly on these cells too, rather than only checking Soft's grid.
        const auto inputForArgument = [c](double argument) {
            return c == SaturationCharacter::glow ? argument - .35
                : c == SaturationCharacter::dense ? .75 + argument / 4 : argument;
        };
        for (int cell = 0; cell <= 512; ++cell) {
            const double seam = double(cell) / 32;
            for (const double argument : { std::nextafter(seam, -1.), seam, std::nextafter(seam, 33.), seam + 1. / 64 }) {
                for (const double sign : { -1., 1. }) {
                    const double x = sign * inputForArgument(argument);
                    primitiveCheck(c, x);
                    for (const double cutoff : { .001, .0001, .00001 }) {
                        for (const double scale : { .999, 1.001 }) {
                            const double delta = cutoff * (1 + std::abs(x)) * scale;
                            quotientCheck(c, x, x - delta);
                        }
                    }
                    quotientCheck(c, x, std::nextafter(x, 100.));
                }
            }
        }
        for (const double x : { 0., -.35, -.75, .75, -32., 32., -1000., 1000.,
                -double(std::numeric_limits<float>::max()), double(std::numeric_limits<float>::max()) }) {
            primitiveCheck(c, x);
            quotientCheck(c, x, -x);
        }
    }
    // Exercise non-grid arguments around both the original midpoint switch and
    // the prepared primitive switch. Expected values remain the analytic law.
    std::uint64_t seed = 0x65718d93c204af6bULL;
    const auto uniform = [&]() {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        return double(seed >> 11) / 9007199254740992.;
    };
    for (int n = 0; n < 16384; ++n) {
        double x = 32 * (2 * uniform() - 1);
        if (n % 4 == 0) x *= 1e-4;
        for (const auto c : characters) {
            primitiveCheck(c, x);
            for (const double cutoff : { 1e-6, .0001, .001 }) {
                const double direction = n % 2 == 0 ? 1 : -1;
                const double delta = direction * cutoff * (1 + std::abs(x)) * (.999 + .002 * uniform());
                quotientCheck(c, x, x + delta);
            }
        }
    }
    check(preparedPrimitive(SaturationCharacter::soft, .4) == preparedPrimitive(SaturationCharacter::soft, -.4));
    check(preparedPrimitive(SaturationCharacter::dense, .9) == preparedPrimitive(SaturationCharacter::dense, -.9));
    check(std::abs(preparedPrimitive(SaturationCharacter::glow, .4) - preparedPrimitive(SaturationCharacter::glow, -.4)) > .001);
    check(fastQuotients > 0 && exactQuotients > 0);
    std::size_t curveArrays = 0;
    for (const auto& view : saturationCoefficientManifest()) {
        if (std::string_view(view.name).starts_with("curve-")) {
            ++curveArrays; check(view.values.size() == 512 * 8);
            check(std::all_of(view.values.begin(), view.values.end(), [](double x) { return std::isfinite(x); }));
        }
    }
    check(curveArrays == 1);
    std::cout << std::setprecision(17) << "curve_checks=" << checks << " failures=" << failures
        << " max_primitive_error_over_1_plus_abs=" << maxPrimitiveError
        << " max_adaa_absolute_error=" << maxAdaaError
        << " table_quotients=" << fastQuotients << " exact_quotients=" << exactQuotients << '\n';
    return failures == 0 ? 0 : 1;
}

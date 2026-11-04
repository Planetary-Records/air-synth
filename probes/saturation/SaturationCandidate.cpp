#include "SaturationCandidate.h"
#include "SaturationWideNetwork.h"
#include "SaturationMath.h"
#include "SaturationGain.h"
#include "SaturationCalibration.h"
#include "SaturationProductState.h"
#include "contracts/SaturationNumericalWitness.h"
#include "contracts/SaturationOptimizationWitness.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>
#if defined(_WIN32) && defined(_M_X64)
#include <immintrin.h>
#endif
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
#include <cfenv>
#include <chrono>
#endif
#if defined(AIRACK_SATURATION_NUMERICAL_TEST) || defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
#include <cstring>
#include <iostream>
#endif

namespace airack {
namespace {
constexpr double pi = std::numbers::pi, R = 1. / 16;
constexpr double invalid = std::numeric_limits<double>::quiet_NaN();
bool bounded(float x, float lo, float hi) noexcept { return std::isfinite(x) && x >= lo && x <= hi; }
bool validRate(double rate) noexcept { return std::isfinite(rate) && rate >= 8000 && rate <= 384000; }
bool validCharacter(SaturationCharacter c) noexcept { return c == SaturationCharacter::soft || c == SaturationCharacter::glow || c == SaturationCharacter::dense; }
double gain(double db) noexcept { return gain_math::analyticGain(db); }
static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559
    && std::numeric_limits<double>::digits == 53 && std::numeric_limits<double>::max_exponent == 1024);
constexpr bool finiteValue(double value) noexcept {
    constexpr std::uint64_t exponent = 0x7ff0000000000000ULL;
    return (std::bit_cast<std::uint64_t>(value) & exponent) != exponent;
}
bool finite(std::array<double, 2> x) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    const auto mask = _mm_set_epi32(0x7ff00000, 0, 0x7ff00000, 0);
    const auto fields = _mm_and_si128(_mm_castpd_si128(_mm_loadu_pd(x.data())), mask);
    return _mm_movemask_pd(_mm_castsi128_pd(_mm_cmpeq_epi32(fields, mask))) == 0;
#else
    return finiteValue(x[0]) && finiteValue(x[1]);
#endif
}
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
bool optimizationOriginalReturn = false;
#endif
std::array<double, 2> returnDryAverage(const std::array<double, 2>& dry, const std::array<double, 2>& previous) noexcept {
    std::array<double, 2> result;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    if (!optimizationOriginalReturn) {
#endif
    _mm_storeu_pd(result.data(), _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(_mm_loadu_pd(dry.data()), _mm_loadu_pd(previous.data()))));
    return result;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    }
#endif
#endif
    for (std::size_t ch = 0; ch < 2; ++ch) result[ch] = .5 * (dry[ch] + previous[ch]);
    return result;
}
void returnBand(std::array<double, 2>& wet, std::array<double, 2>& dry, const std::array<double, 2>& y,
    const std::array<double, 2>& dryBand, const std::array<double, 2>& previous) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    if (!optimizationOriginalReturn) {
#endif
    const auto average = _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(_mm_loadu_pd(dryBand.data()), _mm_loadu_pd(previous.data())));
    _mm_storeu_pd(wet.data(), _mm_add_pd(_mm_loadu_pd(wet.data()), _mm_loadu_pd(y.data())));
    _mm_storeu_pd(dry.data(), _mm_add_pd(_mm_loadu_pd(dry.data()), average));
    return;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    }
#endif
#endif
    for (std::size_t ch = 0; ch < 2; ++ch) { wet[ch] += y[ch]; dry[ch] += .5 * (dryBand[ch] + previous[ch]); }
}
void returnGain(std::array<double, 2>& result, double outputGain) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    if (!optimizationOriginalReturn) {
#endif
    _mm_storeu_pd(result.data(), _mm_mul_pd(_mm_loadu_pd(result.data()), _mm_set1_pd(outputGain)));
    return;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    }
#endif
#endif
    result[0] *= outputGain; result[1] *= outputGain;
}
void returnFade(std::array<double, 2>& result, const std::array<double, 2>& next, double weight) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    if (!optimizationOriginalReturn) {
#endif
    const auto current = _mm_loadu_pd(result.data());
    _mm_storeu_pd(result.data(), _mm_add_pd(current, _mm_mul_pd(_mm_set1_pd(weight), _mm_sub_pd(_mm_loadu_pd(next.data()), current))));
    return;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    }
#endif
#endif
    for (std::size_t ch = 0; ch < 2; ++ch) result[ch] += weight * (next[ch] - result[ch]);
}
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
// Keep exception observations outside the compiler's arithmetic scheduling boundary.
__declspec(noinline) std::array<std::array<double, 2>, 2> returnArithmetic(bool original, unsigned operation,
    const std::array<double, 2>& a, const std::array<double, 2>& b, const std::array<double, 2>& previous, double scalar) noexcept {
        std::array<std::array<double, 2>, 2> result { a, b };
        if (original) {
            for (std::size_t ch = 0; ch < 2; ++ch) {
                if (operation == 0) result[0][ch] = .5 * (b[ch] + previous[ch]);
                if (operation == 1) { result[0][ch] += b[ch]; result[1][ch] += .5 * (a[ch] + previous[ch]); }
                if (operation == 2) result[0][ch] *= scalar;
                if (operation == 3) result[0][ch] += scalar * (b[ch] - result[0][ch]);
            }
        } else {
            if (operation == 0) result[0] = returnDryAverage(b, previous);
            if (operation == 1) returnBand(result[0], result[1], b, a, previous);
            if (operation == 2) returnGain(result[0], scalar);
            if (operation == 3) returnFade(result[0], b, scalar);
        }
    return result;
}
#endif

}
bool validateSaturation(const SaturationSettings& s) noexcept {
    if (s.mode != SaturationMode::fullBand && s.mode != SaturationMode::threeBand) return false;
    if (!bounded(s.inputTrimDb, -24, 24) || !bounded(s.outputTrimDb, -24, 12) || !bounded(s.mix, 0, 1)
        || !bounded(s.lowCrossoverHz, 40, 2000) || !bounded(s.highCrossoverHz, 500, 16000)
        || !bounded(s.preLowCutHz, 20, 2000) || !bounded(s.postHighCutHz, 1000, 20000)) return false;
    for (const auto& l : s.lanes)
        if (!validCharacter(l.character) || !bounded(l.driveDb, 0, 36) || !bounded(l.preTiltDb, -6, 6)
            || !bounded(l.mix, 0, 1) || !bounded(l.trimDb, -18, 18)) return false;
    return true;
}
SaturationCrossovers effectiveSaturationCrossovers(const SaturationSettings& s, double rate) noexcept {
    if (!validateSaturation(s) || !validRate(rate)) return { invalid, invalid };
    const double high = std::min(double(s.highCrossoverHz), .45 * rate);
    return { std::min(double(s.lowCrossoverHz), .8 * high), high };
}
float saturationDriveCompensationDb(SaturationCharacter c, float drive) noexcept {
    if (!validCharacter(c) || !bounded(drive, 0, 36)) return std::numeric_limits<float>::quiet_NaN();
    const auto table = probe::saturationCompensationTable(c);
    const auto low = std::min(std::size_t(drive), std::size_t(35));
    return float(table[low] + (double(drive) - double(low)) * (table[low + 1] - table[low]));
}
double saturationTailSeconds(const SaturationSettings& s, double rate) noexcept {
    if (!validateSaturation(s) || !validRate(rate)) return invalid;
    return std::max({ 2., std::log(1e9) / (2 * pi * 5), .080 * std::log(1e9) });
}

namespace probe {
#if defined(AIRACK_SATURATION_REFERENCE_TRACE)
void saturationReferenceTrace(Candidate::Frame wet, Candidate::Frame matchedDry) noexcept;
#endif
namespace {
double logCosh(double x) noexcept {
    const double v = std::abs(x);
    return v + std::log1p(std::exp(-2 * v)) - std::log(2.);
}
}
double shape(SaturationCharacter c, double x) noexcept {
    if (c == SaturationCharacter::soft) return std::tanh(x);
    if (c == SaturationCharacter::glow) {
        const double b = std::tanh(.35);
        return (std::tanh(x + .35) - b) / (1 - b * b);
    }
    const double u = std::abs(x);
    return std::copysign(u <= .75 ? u : .75 + .25 * std::tanh(4 * (u - .75)), x);
}
double primitive(SaturationCharacter c, double x) noexcept {
    if (c == SaturationCharacter::soft) return logCosh(x);
    if (c == SaturationCharacter::glow) {
        const double b = std::tanh(.35);
        return (logCosh(x + .35) - b * x) / (1 - b * b);
    }
    const double u = std::abs(x);
    return u <= .75 ? .5 * u * u : .28125 + .75 * (u - .75) + .0625 * logCosh(4 * (u - .75));
}
namespace {
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
std::size_t numericalZeroHalfWidths = 0;
#endif
double expMinus(double scale, double v) noexcept {
    return v > std::numeric_limits<double>::max() / scale ? 0. : std::exp(-scale * v);
}
double expm1Minus(double scale, double v) noexcept {
    return v > std::numeric_limits<double>::max() / scale ? -1. : std::expm1(-scale * v);
}
double softQuotient(double x, double p, double shift = 0) noexcept {
    const double h = .5 * x - .5 * p;
    const double m = (.5 * x + .5 * p) + shift;
    if (std::abs(h) <= .5)
        return std::atanh(std::tanh(m) * std::tanh(h)) / h;
    const double ax = std::abs(x + shift), ap = std::abs(p + shift);
    const double a = std::max(ax, ap), b = std::min(ax, ap), d = a - b;
    if (d == 0) return 0;
    const double e = expMinus(2., b);
    const double change = d + std::log1p((e / (1 + e)) * expm1Minus(2., d));
    return .5 * ((ax >= ap ? change : -change) / h);
}
double densePositiveMean(double a, double b) noexcept {
    const double d = b - a, e = expMinus(8., a - .75);
    return 1 + .0625 * (std::log1p((e / (1 + e)) * expm1Minus(8., d)) / d);
}
double denseQuotient(double x, double p) noexcept {
    const double a = std::min(x, p), b = std::max(x, p);
    const double halfWidth = .5 * b - .5 * a;
    double result = 0;
    const auto add = [&](double lo, double hi, double mean) {
        result += ((.5 * hi - .5 * lo) / halfWidth) * mean;
    };
    if (a < -.75) {
        const double hi = std::min(b, -.75);
        add(a, hi, -densePositiveMean(-hi, -a));
    }
    const double lo = std::max(a, -.75), hi = std::min(b, .75);
    if (lo < hi) add(lo, hi, .5 * lo + .5 * hi);
    if (b > .75) {
        const double start = std::max(a, .75);
        add(start, b, densePositiveMean(start, b));
    }
    return result;
}
double exactQuotient(SaturationCharacter c, double x, double p, double bias, double normalizer) noexcept {
    if (!std::isfinite(x) || !std::isfinite(p)) return (primitive(c, x) - primitive(c, p)) / (x - p);
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
    if (.5 * x - .5 * p == 0) ++numericalZeroHalfWidths;
#endif
    if (c == SaturationCharacter::soft) return softQuotient(x, p);
    if (c == SaturationCharacter::glow) return (softQuotient(x, p, .35) - bias) / normalizer;
    return denseQuotient(x, p);
}
}
double adaa(SaturationCharacter c, double x, double previous) noexcept {
    const double delta = x - previous;
    const double b = std::tanh(.35);
    return std::abs(delta) < 1e-6 * (1 + std::max(std::abs(x), std::abs(previous)))
        ? shape(c, .5 * (x + previous)) : exactQuotient(c, x, previous, b, 1 - b * b);
}
double advanceSag(double peak, double rate, double& envelope) noexcept {
    const double seconds = peak > envelope ? .002 : .080;
    envelope = peak + std::exp(-1 / (rate * seconds)) * (envelope - peak);
    const double reduction = std::clamp(6 * std::max(envelope - .5, 0.) / (envelope + .5), 0., 6.);
    return std::pow(10., -reduction / 20);
}
std::span<const double> saturationCompensationTable(SaturationCharacter c) noexcept {
    if (c == SaturationCharacter::soft) return calibration::soft;
    if (c == SaturationCharacter::glow) return calibration::glow;
    if (c == SaturationCharacter::dense) return calibration::dense;
    return {};
}

namespace {
constexpr std::size_t maxIirDirectSections = 5, maxIirDelayedSections = 4;
struct PhaseCoefficients {
    std::array<double, maxIirDirectSections> direct {};
    std::array<double, maxIirDelayedSections> delayed {};
    std::size_t directCount = 0, delayedCount = 0;
};
struct StageCoefficients { PhaseCoefficients up, down; };
constexpr std::array<std::size_t, 6> firLengths { 257, 129, 65, 33, 17, 17 };
// MSVC /fp:precise leaves these constant libm calls in the sample path.
// Initialize their identical binary64 results when the module is loaded.
struct PreparedConstants {
    const double logTwo = std::log(2.);
    const double sqrtTwo = std::sqrt(2.);
    const double glowBias = std::tanh(.35);
};
const PreparedConstants preparedConstants;
double cachedLogCosh(double x) noexcept {
    const double v = std::abs(x);
    return v + std::log1p(std::exp(-2 * v)) - preparedConstants.logTwo;
}
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
std::size_t optimizationPrimitiveCalls = 0;
std::size_t optimizationScalarCurveCalls = 0, optimizationPairCurveCalls = 0;
std::size_t optimizationSelectorCalls = 0;
std::size_t optimizationControlMathSteps = 0, optimizationControlCopies = 0;
std::size_t optimizationPrefixExecutions = 0, optimizationPrefixCopies = 0;
std::size_t optimizationStereoFrameCalls = 0;
std::size_t optimizationTemporalBlocks = 0, optimizationTemporalHornerChains = 0, optimizationTemporalLowBlocks = 0;
std::size_t optimizationTemporalPreparedFallbacks = 0, optimizationTemporalLegacy = 0;
std::size_t optimizationTemporalMixedBlocks = 0;
unsigned optimizationTemporalLastMixedMask = 0;
std::array<std::size_t, 3> optimizationTemporalByCharacter {}, optimizationTemporalFallbackByCharacter {}, optimizationTemporalLegacyByCharacter {};
bool optimizationOriginalLane = false;
bool optimizationSampling = false;
OptimizationProfile optimizationProfile;
struct OptimizationScope {
    OptimizationStage stage;
    bool enabled;
    std::chrono::steady_clock::time_point start;
    explicit OptimizationScope(OptimizationStage s) noexcept
        : stage(s), enabled(optimizationSampling) {
        if (enabled) start = std::chrono::steady_clock::now();
    }
    ~OptimizationScope() noexcept {
        if (!enabled) return;
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        const auto i = unsigned(stage);
        optimizationProfile.nanoseconds[i] += elapsed > 0 ? std::uint64_t(elapsed) : 0;
        ++optimizationProfile.calls[i];
    }
};
#define AIRACK_OPTIMIZATION_SCOPE(variable, stage) OptimizationScope variable(OptimizationStage::stage)
#else
#define AIRACK_OPTIMIZATION_SCOPE(variable, stage)
#endif
double cachedPrimitive(SaturationCharacter c, double x) noexcept {
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    ++optimizationPrimitiveCalls;
#endif
    if (c == SaturationCharacter::soft) return cachedLogCosh(x);
    if (c == SaturationCharacter::glow) {
        const double b = preparedConstants.glowBias;
        return (cachedLogCosh(x + .35) - b * x) / (1 - b * b);
    }
    const double u = std::abs(x);
    return u <= .75 ? .5 * u * u : .28125 + .75 * (u - .75) + .0625 * cachedLogCosh(4 * (u - .75));
}
double cachedShape(SaturationCharacter c, double x) noexcept {
    if (c != SaturationCharacter::glow) return shape(c, x);
    const double b = preparedConstants.glowBias;
    return (std::tanh(x + .35) - b) / (1 - b * b);
}
float admittedCompensationDb(SaturationCharacter c, float drive) noexcept {
    const auto table = saturationCompensationTable(c);
    const auto low = std::min(std::size_t(drive), std::size_t(35));
    return float(table[low] + (double(drive) - double(low)) * (table[low + 1] - table[low]));
}
struct PrimitiveCurve {
    static constexpr std::size_t cells = 512, degree = 7, stride = degree + 1;
    static constexpr double cellsPerUnit = 32, limit = double(cells) / cellsPerUnit;
    // Original log(cosh) Taylor coefficients, in ascending powers about the
    // midpoint of each 1/32 cell. This is independent of the resampler and the
    // fixed drive-compensation calibration; no captured reference data is used.
    std::array<double, cells * stride> coefficients {};
    PrimitiveCurve() noexcept {
        for (std::size_t cell = 0; cell < cells; ++cell) {
            const double center = (double(cell) + .5) / cellsPerUnit;
            auto* a = coefficients.data() + cell * stride;
            a[0] = logCosh(center);
            // If tanh(center+t)=sum(y[n]*t^n), y'=1-y*y generates
            // its coefficients; integrating gives the log(cosh) coefficients.
            std::array<double, degree> y {};
            y[0] = std::tanh(center); a[1] = y[0];
            for (std::size_t n = 0; n + 1 < degree; ++n) {
                double convolution = 0;
                for (std::size_t k = 0; k <= n; ++k) convolution += y[k] * y[n - k];
                y[n + 1] = ((n == 0 ? 1. : 0.) - convolution) / double(n + 1);
                a[n + 2] = y[n + 1] / double(n + 2);
            }
        }
    }
    double evaluate(double x) const noexcept {
        const double v = std::abs(x);
        // Retain the analytic tail, and test the range before converting the
        // index so NaN/infinite/extreme finite arguments cannot index the table.
        if (!(v < limit)) return cachedLogCosh(x);
        const int cell = int(v * cellsPerUnit); // Range guard bounds this to [0, cells).
        const double t = v - (double(cell) + .5) / cellsPerUnit;
        const auto* a = coefficients.data() + cell * stride;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        ++optimizationScalarCurveCalls;
#endif
        double result = a[degree];
        for (std::size_t n = degree; n-- > 0;) result = result * t + a[n];
        return result;
    }
    // On every radius-1 disk about a real center, |tanh(z)|<=tan(1).
    // With |t|<=1/64, Cauchy's estimate bounds the degree-7 log(cosh)
    // truncation by tan(1)*(1/64)^8/(8*(1-1/64)) < 7.03e-16.
    // Binary64 coefficient/libm/evaluation error is measured separately against
    // the unchanged analytic oracle; the close-quotient guard below prevents
    // dividing a few ulps of primitive error by the original 1e-6 cutoff.
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    forcedinline Candidate::Frame evaluatePair(Candidate::Frame x) const noexcept {
        const double v0 = std::abs(x[0]), v1 = std::abs(x[1]);
        if (!(v0 < limit) || !(v1 < limit)) return { evaluate(x[0]), evaluate(x[1]) };
        const int c0 = int(v0 * cellsPerUnit), c1 = int(v1 * cellsPerUnit);
        const double t0 = v0 - (double(c0) + .5) / cellsPerUnit;
        const double t1 = v1 - (double(c1) + .5) / cellsPerUnit;
        const auto* a0 = coefficients.data() + c0 * stride;
        const auto* a1 = coefficients.data() + c1 * stride;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        ++optimizationPairCurveCalls;
#endif
        const auto t = _mm_set_pd(t1, t0);
        auto result = _mm_set_pd(a1[degree], a0[degree]);
        for (std::size_t n = degree; n-- > 0;)
            result = _mm_add_pd(_mm_mul_pd(result, t), _mm_set_pd(a1[n], a0[n]));
        Candidate::Frame y; _mm_storeu_pd(y.data(), result); return y;
    }
#endif
};
double curvePrimitive(const PrimitiveCurve& curve, SaturationCharacter c, double x) noexcept {
    if (c == SaturationCharacter::soft) return curve.evaluate(x);
    if (c == SaturationCharacter::glow) {
        const double b = preparedConstants.glowBias;
        return (curve.evaluate(x + .35) - b * x) / (1 - b * b);
    }
    const double u = std::abs(x);
    return u <= .75 ? .5 * u * u : .28125 + .75 * (u - .75) + .0625 * curve.evaluate(4 * (u - .75));
}
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
template<SaturationCharacter Character>
forcedinline Candidate::Frame curvePrimitivePair(const PrimitiveCurve& curve, Candidate::Frame x) noexcept {
    if constexpr (Character == SaturationCharacter::soft) return curve.evaluatePair(x);
    else if constexpr (Character == SaturationCharacter::glow) {
        const double b = preparedConstants.glowBias;
        const auto primitive = curve.evaluatePair({ x[0] + .35, x[1] + .35 });
        return { (primitive[0] - b * x[0]) / (1 - b * b), (primitive[1] - b * x[1]) / (1 - b * b) };
    } else {
        const double u0 = std::abs(x[0]), u1 = std::abs(x[1]);
        if (u0 > .75 && u1 > .75) {
            const auto primitive = curve.evaluatePair({ 4 * (u0 - .75), 4 * (u1 - .75) });
            return { .28125 + .75 * (u0 - .75) + .0625 * primitive[0],
                     .28125 + .75 * (u1 - .75) + .0625 * primitive[1] };
        }
        return { curvePrimitive(curve, Character, x[0]), curvePrimitive(curve, Character, x[1]) };
    }
}
#endif
enum class PrimitiveKind : std::uint8_t { none, exact, curve };
PrimitiveKind quotientPrimitiveKind(double x, double previous) noexcept {
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    ++optimizationSelectorCalls;
#endif
    return std::abs(x - previous) < .0001 * (1 + std::max(std::abs(x), std::abs(previous)))
        ? PrimitiveKind::exact : PrimitiveKind::curve;
}
bool fixedFourSectionLayout(const std::array<StageCoefficients, 3>& stages) noexcept {
    return stages[0].up.directCount == 5 && stages[0].up.delayedCount == 4
        && stages[0].down.directCount == 5 && stages[0].down.delayedCount == 4
        && stages[1].up.directCount == 3 && stages[1].up.delayedCount == 3
        && stages[1].down.directCount == 3 && stages[1].down.delayedCount == 3;
}
struct Resources {
    std::array<StageCoefficients, 3> iir;
    std::array<std::array<double, 257>, 6> fir {};
    PrimitiveCurve primitiveCurve;
    gain_math::GainCurve gainCurve;
    bool fixed4Layout = false;
    std::vector<CoefficientView> manifest;
    Resources() {
        const auto design = [](double width, double rejection) {
            const auto source = juce::dsp::FilterDesign<double>::designIIRLowpassHalfBandPolyphaseAllpassMethod(width, rejection);
            PhaseCoefficients c;
            c.directCount = std::size_t(source.directPath.size()); c.delayedCount = std::size_t(source.delayedPath.size() - 1);
            if (c.directCount > c.direct.size() || c.delayedCount > c.delayed.size()) throw std::bad_alloc();
            for (std::size_t n = 0; n < c.directCount; ++n) c.direct[n] = source.directPath.getObjectPointer(int(n))->coefficients[0];
            for (std::size_t n = 0; n < c.delayedCount; ++n) c.delayed[n] = source.delayedPath.getObjectPointer(int(n + 1))->coefficients[0];
            return c;
        };
        // Original public-design parameters; the manifest exposes the arrays actually used.
        iir[0] = { design(.045, -110), design(.045, -110) };
        iir[1] = { design(.12, -100), design(.12, -100) };
        iir[2] = { design(.20, -90), design(.20, -90) };
        fixed4Layout = fixedFourSectionLayout(iir);
        constexpr std::array<const char*, 12> names { "iir1-up-direct", "iir1-up-delayed", "iir1-down-direct", "iir1-down-delayed",
            "iir2-up-direct", "iir2-up-delayed", "iir2-down-direct", "iir2-down-delayed", "iir3-up-direct", "iir3-up-delayed", "iir3-down-direct", "iir3-down-delayed" };
        for (std::size_t stage = 0; stage < iir.size(); ++stage) {
            const auto& s = iir[stage];
            manifest.push_back({ names[stage * 4], { s.up.direct.data(), s.up.directCount } });
            manifest.push_back({ names[stage * 4 + 1], { s.up.delayed.data(), s.up.delayedCount } });
            manifest.push_back({ names[stage * 4 + 2], { s.down.direct.data(), s.down.directCount } });
            manifest.push_back({ names[stage * 4 + 3], { s.down.delayed.data(), s.down.delayedCount } });
        }
        constexpr std::array<const char*, 6> firNames { "reference-fir1", "reference-fir2", "reference-fir3", "reference-fir4", "reference-fir5", "reference-fir6" };
        for (std::size_t stage = 0; stage < fir.size(); ++stage) {
            const auto count = firLengths[stage], center = (count - 1) / 2;
            double sum = 0;
            for (std::size_t n = 0; n < count; ++n) {
                const auto offset = std::ptrdiff_t(n) - std::ptrdiff_t(center);
                const double phase = 2 * pi * double(n) / double(count - 1);
                const double window = .35875 - .48829 * std::cos(phase) + .14128 * std::cos(2 * phase) - .01168 * std::cos(3 * phase);
                const double sinc = offset == 0 ? .5 : (offset % 2 == 0 ? 0 : std::sin(.5 * pi * double(offset)) / (pi * double(offset)));
                fir[stage][n] = sinc * window; sum += fir[stage][n];
            }
            for (std::size_t n = 0; n < count; ++n) fir[stage][n] /= sum;
            manifest.push_back({ firNames[stage], { fir[stage].data(), count } });
        }
        manifest.push_back({ "curve-log-cosh-taylor-degree7-cell1over32-domain16", primitiveCurve.coefficients });
        manifest.push_back({ "gain10-db20-exp-taylor6-cell1over8-domain48-centers", gainCurve.centers });
    }
};
const Resources& resources() { static const Resources r; return r; }
struct PhaseState {
    std::array<double, maxIirDirectSections> direct {};
    std::array<double, maxIirDelayedSections> delayed {};
    double held = 0;
    template<std::size_t Capacity>
    static double cascade(double x, const std::array<double, Capacity>& c, std::array<double, Capacity>& z, std::size_t count) noexcept {
        for (std::size_t n = 0; n < count; ++n) { const double y = c[n] * x + z[n]; z[n] = x - c[n] * y; x = y; } return x;
    }
    std::array<double, 2> up(double x, const PhaseCoefficients& c) noexcept {
        return { cascade(x, c.direct, direct, c.directCount), cascade(x, c.delayed, delayed, c.delayedCount) };
    }
    double down(double even, double odd, const PhaseCoefficients& c) noexcept {
        const double y = .5 * (held + cascade(even, c.direct, direct, c.directCount));
        held = cascade(odd, c.delayed, delayed, c.delayedCount); return y;
    }
};
struct FirState {
    std::array<double, 257> history {}; std::size_t head = 0;
    std::array<double, 2> up(double x, const std::array<double, 257>& c, std::size_t count) noexcept {
        const auto length = (count + 1) / 2;
        head = head == 0 ? length - 1 : head - 1; history[head] = x;
        std::array<double, 2> result {};
        for (std::size_t phase = 0; phase < 2; ++phase) {
            auto index = head;
            for (std::size_t n = phase; n < count; n += 2) {
                if (c[n] != 0) result[phase] += 2 * c[n] * history[index];
                if (++index == length) index = 0;
            }
        }
        return result;
    }
    double down(double even, double odd, const std::array<double, 257>& c, std::size_t count) noexcept {
        head = head == 0 ? count - 1 : head - 1; history[head] = even;
        double result = 0; auto index = head;
        for (std::size_t n = 0; n < count; ++n) { if (c[n] != 0) result += c[n] * history[index]; if (++index == count) index = 0; }
        head = head == 0 ? count - 1 : head - 1; history[head] = odd; return result;
    }
};
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
// Fixed4 stereo histories stay adjacent, so each allpass section needs one
// aligned packed load/store. Generic factors retain their scalar histories.
struct alignas(16) StereoPhaseState {
    alignas(16) std::array<Candidate::Frame, maxIirDirectSections> direct {};
    alignas(16) std::array<Candidate::Frame, maxIirDelayedSections> delayed {};
    alignas(16) Candidate::Frame held {};
};
static_assert(sizeof(Candidate::Frame) == 2 * sizeof(double) && alignof(StereoPhaseState) >= 16);
#endif
struct Resampler {
    std::array<std::array<PhaseState, 2>, 3> iir {};
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    std::array<StereoPhaseState, 2> stereoIir {};
#endif
    using FirBank = std::array<std::array<FirState, 2>, 6>;
    std::unique_ptr<FirBank> fir;
#if defined(AIRACK_SATURATION_TRANSITIONS)
    Resampler& operator=(const Resampler& other) noexcept {
        iir = other.iir;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
        stereoIir = other.stereoIir;
#endif
        // Both endpoints were prepared for the same factor before admission.
        if (fir && other.fir) *fir = *other.fir;
        return *this;
    }
#endif
    void prepare(unsigned factor) {
        if (factor == 64) fir = std::make_unique<FirBank>();
    }
    void clear() noexcept {
        iir = {};
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
        stereoIir = {};
#endif
        if (fir) *fir = {};
    }
    void up(Candidate::Frame input, unsigned factor, const Resources& r, std::array<Candidate::Frame, 64>& output) noexcept {
        output[0] = input;
        for (std::size_t stage = 0, count = 1; count < factor; ++stage, count *= 2) {
            std::array<Candidate::Frame, 64> previous;
            std::copy_n(output.begin(), count, previous.begin());
            for (std::size_t n = 0; n < count; ++n) for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto pair = factor == 64 ? (*fir)[stage][channel].up(previous[n][channel], r.fir[stage], firLengths[stage])
                    : iir[stage][channel].up(previous[n][channel], r.iir[stage].up);
                output[n * 2][channel] = pair[0]; output[n * 2 + 1][channel] = pair[1];
            }
        }
    }
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    template<std::size_t Capacity>
    static forcedinline __m128d cascadeStereo(__m128d x, const std::array<double, Capacity>& c,
        std::array<Candidate::Frame, Capacity>& state, std::size_t count) noexcept {
        for (std::size_t n = 0; n < count; ++n) {
            const auto coefficient = _mm_set1_pd(c[n]);
            const auto z = _mm_load_pd(state[n].data());
            const auto y = _mm_add_pd(_mm_mul_pd(coefficient, x), z);
            const auto next = _mm_sub_pd(x, _mm_mul_pd(coefficient, y));
            _mm_store_pd(state[n].data(), next); x = y;
        }
        return x;
    }
    template<std::size_t DirectCount = 0, std::size_t DelayedCount = 0>
    static forcedinline void upStereo(Candidate::Frame input, StereoPhaseState& state,
        const PhaseCoefficients& c, Candidate::Frame& even, Candidate::Frame& odd) noexcept {
        const auto directCount = DirectCount ? DirectCount : c.directCount, delayedCount = DelayedCount ? DelayedCount : c.delayedCount;
        const auto x = _mm_loadu_pd(input.data());
        _mm_storeu_pd(even.data(), cascadeStereo(x, c.direct, state.direct, directCount));
        _mm_storeu_pd(odd.data(), cascadeStereo(x, c.delayed, state.delayed, delayedCount));
    }
    template<std::size_t DirectCount = 0, std::size_t DelayedCount = 0>
    static forcedinline Candidate::Frame downStereo(Candidate::Frame even, Candidate::Frame odd,
        StereoPhaseState& state, const PhaseCoefficients& c) noexcept {
        const auto directCount = DirectCount ? DirectCount : c.directCount, delayedCount = DelayedCount ? DelayedCount : c.delayedCount;
        const auto held = _mm_load_pd(state.held.data());
        const auto filtered = cascadeStereo(_mm_loadu_pd(even.data()), c.direct, state.direct, directCount);
        const auto y = _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(held, filtered));
        const auto nextHeld = cascadeStereo(_mm_loadu_pd(odd.data()), c.delayed, state.delayed, delayedCount);
        _mm_store_pd(state.held.data(), nextHeld);
        Candidate::Frame result; _mm_storeu_pd(result.data(), y); return result;
    }
#endif
    void up4(Candidate::Frame input, const Resources& r, std::array<Candidate::Frame, 4>& output) noexcept {
        std::array<Candidate::Frame, 2> first;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
        if (r.fixed4Layout) {
            upStereo<5, 4>(input, stereoIir[0], r.iir[0].up, first[0], first[1]);
            for (std::size_t n = 0; n < 2; ++n)
                upStereo<3, 3>(first[n], stereoIir[1], r.iir[1].up, output[n * 2], output[n * 2 + 1]);
        } else {
            upStereo(input, stereoIir[0], r.iir[0].up, first[0], first[1]);
            for (std::size_t n = 0; n < 2; ++n)
                upStereo(first[n], stereoIir[1], r.iir[1].up, output[n * 2], output[n * 2 + 1]);
        }
#else
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto pair = iir[0][channel].up(input[channel], r.iir[0].up);
            first[0][channel] = pair[0]; first[1][channel] = pair[1];
        }
        for (std::size_t n = 0; n < 2; ++n) for (std::size_t channel = 0; channel < 2; ++channel) {
            const auto pair = iir[1][channel].up(first[n][channel], r.iir[1].up);
            output[n * 2][channel] = pair[0]; output[n * 2 + 1][channel] = pair[1];
        }
#endif
    }
    Candidate::Frame down4(std::array<Candidate::Frame, 4>& input, const Resources& r) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
        if (r.fixed4Layout) {
            for (std::size_t n = 0; n < 2; ++n)
                input[n] = downStereo<3, 3>(input[n * 2], input[n * 2 + 1], stereoIir[1], r.iir[1].down);
            return downStereo<5, 4>(input[0], input[1], stereoIir[0], r.iir[0].down);
        }
        for (std::size_t n = 0; n < 2; ++n)
            input[n] = downStereo(input[n * 2], input[n * 2 + 1], stereoIir[1], r.iir[1].down);
        return downStereo(input[0], input[1], stereoIir[0], r.iir[0].down);
#else
        for (std::size_t n = 0; n < 2; ++n) for (std::size_t channel = 0; channel < 2; ++channel)
            input[n][channel] = iir[1][channel].down(input[n * 2][channel], input[n * 2 + 1][channel], r.iir[1].down);
        Candidate::Frame result;
        for (std::size_t channel = 0; channel < 2; ++channel)
            result[channel] = iir[0][channel].down(input[0][channel], input[1][channel], r.iir[0].down);
        return result;
#endif
    }
    Candidate::Frame down(std::array<Candidate::Frame, 64>& input, unsigned factor, const Resources& r) noexcept {
        std::size_t stages = 0; for (auto n = factor; n > 1; n /= 2) ++stages;
        for (std::size_t stage = stages, count = factor / 2; stage-- > 0; count /= 2)
            for (std::size_t n = 0; n < count; ++n) for (std::size_t channel = 0; channel < 2; ++channel)
                input[n][channel] = factor == 64 ? (*fir)[stage][channel].down(input[n * 2][channel], input[n * 2 + 1][channel], r.fir[stage], firLengths[stage])
                    : iir[stage][channel].down(input[n * 2][channel], input[n * 2 + 1][channel], r.iir[stage].down);
        return input[0];
    }
};
using Coefficients = wide_network::Coefficients;
enum class Filter { low, high, allpass, lowShelf, highShelf };
struct DesignBasis { double cs = 1, alpha = 0; };
#if JUCE_USE_SIMD
forcedinline juce::dsp::SIMDRegister<double> coefficientStereo(double value) noexcept {
#if JUCE_INTEL && !defined(__AVX2__)
    return juce::dsp::SIMDRegister<double>::fromNative(_mm_set1_pd(value));
#else
    return juce::dsp::SIMDRegister<double>(value);
#endif
}
#endif
DesignBasis designBasis(double rate, double hz, bool four = false) noexcept {
    const double w = 2 * pi * hz / rate;
    if (!four) return { std::cos(w), std::sin(w) / preparedConstants.sqrtTwo };
    const auto sc = coefficient_math::designSinCos(w);
    return { sc.cs, sc.sn / preparedConstants.sqrtTwo };
}
Coefficients design(Filter type, const DesignBasis& basis, double db = 0, const gain_math::GainCurve* gainCurve = nullptr) noexcept {
    const double cs = basis.cs, alpha = basis.alpha;
    double b0, b1, b2, a0, a1, a2;
    if (type == Filter::low || type == Filter::high || type == Filter::allpass) {
        a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        if (type == Filter::low) { b0 = (1 - cs) * .5; b1 = 1 - cs; b2 = b0; }
        else if (type == Filter::high) { b0 = (1 + cs) * .5; b1 = -(1 + cs); b2 = b0; }
        else { b0 = a2; b1 = a1; b2 = a0; }
    } else {
        if (db == 0) return {};
        const double a = gainCurve ? gainCurve->gain(db * .5) : std::pow(10., db / 40), root = 2 * std::sqrt(a) * alpha;
        if (type == Filter::lowShelf) {
            b0 = a * ((a + 1) - (a - 1) * cs + root); b1 = 2 * a * ((a - 1) - (a + 1) * cs); b2 = a * ((a + 1) - (a - 1) * cs - root);
            a0 = (a + 1) + (a - 1) * cs + root; a1 = -2 * ((a - 1) + (a + 1) * cs); a2 = (a + 1) + (a - 1) * cs - root;
        } else {
            b0 = a * ((a + 1) + (a - 1) * cs + root); b1 = -2 * a * ((a - 1) + (a + 1) * cs); b2 = a * ((a + 1) + (a - 1) * cs - root);
            a0 = (a + 1) - (a - 1) * cs + root; a1 = 2 * ((a - 1) - (a + 1) * cs); a2 = (a + 1) - (a - 1) * cs - root;
        }
    }
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    using Stereo = juce::dsp::SIMDRegister<double>;
    if constexpr (Stereo::size() == 2) {
        alignas(Stereo::SIMDRegisterSize) std::array<double, 2> first { b0, b1 }, second { b2, a1 };
        const auto denominator = coefficientStereo(a0).value;
        Stereo::fromNative(_mm_div_pd(Stereo::fromRawArray(first.data()).value, denominator)).copyToRawArray(first.data());
        Stereo::fromNative(_mm_div_pd(Stereo::fromRawArray(second.data()).value, denominator)).copyToRawArray(second.data());
        return { first[0], first[1], second[0], second[1], a2 / a0 };
    }
#endif
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
}
void designSplit(const DesignBasis& basis, Coefficients& low, Coefficients& high, Coefficients* allpass = nullptr) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    using Stereo = juce::dsp::SIMDRegister<double>;
    if constexpr (Stereo::size() == 2) {
        const double cs = basis.cs, alpha = basis.alpha;
        const double a0 = 1 + alpha, a1 = -2 * cs, a2 = 1 - alpha;
        alignas(Stereo::SIMDRegisterSize) std::array<double, 2> lp { (1 - cs) * .5, 1 - cs };
        alignas(Stereo::SIMDRegisterSize) std::array<double, 2> hp { (1 + cs) * .5, -(1 + cs) };
        alignas(Stereo::SIMDRegisterSize) std::array<double, 2> denominatorTerms { a1, a2 };
        const auto denominator = coefficientStereo(a0).value;
        Stereo::fromNative(_mm_div_pd(Stereo::fromRawArray(lp.data()).value, denominator)).copyToRawArray(lp.data());
        Stereo::fromNative(_mm_div_pd(Stereo::fromRawArray(hp.data()).value, denominator)).copyToRawArray(hp.data());
        Stereo::fromNative(_mm_div_pd(Stereo::fromRawArray(denominatorTerms.data()).value, denominator)).copyToRawArray(denominatorTerms.data());
        // Original b2 equals b0 before division; equal quotient bits can be reused.
        low = { lp[0], lp[1], lp[0], denominatorTerms[0], denominatorTerms[1] };
        high = { hp[0], hp[1], hp[0], denominatorTerms[0], denominatorTerms[1] };
        if (allpass) *allpass = { denominatorTerms[1], denominatorTerms[0], a0 / a0, denominatorTerms[0], denominatorTerms[1] };
        return;
    }
#endif
    low = design(Filter::low, basis); high = design(Filter::high, basis);
    if (allpass) *allpass = design(Filter::allpass, basis);
}

#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
std::size_t numericalProductStateCalls = 0;
#endif
struct Biquad {
#if JUCE_USE_SIMD
    using Stereo = juce::dsp::SIMDRegister<double>;
    alignas(Stereo::SIMDRegisterSize) std::array<double, 2> z1 {}, z2 {};
#else
    std::array<double, 2> z1 {}, z2 {};
#endif
    std::array<double, 2> z1Low {}, z2Low {};
    template<bool ProductState = false>
    Candidate::Frame process(Candidate::Frame x, const Coefficients& c) noexcept {
        if constexpr (ProductState) {
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
            ++numericalProductStateCalls;
#endif
            for (std::size_t ch = 0; ch < 2; ++ch) {
                product_state::State state {{z1[ch], z1Low[ch]}, {z2[ch], z2Low[ch]}, {}};
                x[ch] = state.step(x[ch], {c.b0, c.b1, c.b2, c.a1, c.a2});
                z1[ch] = state.z1.h; z1Low[ch] = state.z1.l;
                z2[ch] = state.z2.h; z2Low[ch] = state.z2.l;
            }
            return x;
        } else {
#if JUCE_USE_SIMD
        if constexpr (Stereo::size() == 2) {
            alignas(Stereo::SIMDRegisterSize) Candidate::Frame samples = x;
            const auto input = Stereo::fromRawArray(samples.data());
            const auto y = coefficientStereo(c.b0) * input + Stereo::fromRawArray(z1.data());
            const auto nextZ1 = (coefficientStereo(c.b1) * input - coefficientStereo(c.a1) * y) + Stereo::fromRawArray(z2.data());
            const auto nextZ2 = coefficientStereo(c.b2) * input - coefficientStereo(c.a2) * y;
            nextZ1.copyToRawArray(z1.data()); nextZ2.copyToRawArray(z2.data());
            y.copyToRawArray(samples.data()); return samples;
        }
#endif
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double y = c.b0 * x[ch] + z1[ch]; z1[ch] = c.b1 * x[ch] - c.a1 * y + z2[ch]; z2[ch] = c.b2 * x[ch] - c.a2 * y; x[ch] = y;
        }
        return x;
        }
    }
};
struct Split {
    std::array<Biquad, 2> low, high;
    template<bool ProductState = false>
    std::array<Candidate::Frame, 2> process(Candidate::Frame x, const Coefficients& lp, const Coefficients& hp) noexcept {
        auto l = x, h = x;
        for (std::size_t n = 0; n < 2; ++n) { l = low[n].process<ProductState>(l, lp); h = high[n].process<ProductState>(h, hp); } return { l, h };
    }
};
using NetworkCoefficients = wide_network::NetworkCoefficients;
struct Network {
    Split low, high; Biquad lowCompensation;
    template<bool ProductState = false>
    std::array<Candidate::Frame, 3> process(Candidate::Frame x, const NetworkCoefficients& c) noexcept {
        const auto lower = low.process<ProductState>(x, c.lowLP, c.lowHP), upper = high.process<ProductState>(lower[1], c.highLP, c.highHP);
        return { lowCompensation.process<ProductState>(lower[0], c.highAP), upper[0], upper[1] };
    }
};
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
std::vector<SaturationNumericalRecord> numericalRecords;
bool numericalCollectRecords = true;
void recordNumericalLane(const void* lane, int site, SaturationCharacter c, int channel, double u, double p,
    double q, double current, double prior, double cached, PrimitiveKind previous, PrimitiveKind kind) {
    if (numericalCollectRecords) numericalRecords.push_back({ reinterpret_cast<std::uintptr_t>(lane), site, int(c), channel, int(kind), int(previous),
                                u, p, q, current, prior, cached });
}
#endif
#if defined(AIRACK_SATURATION_TRANSITIONS) && !defined(AIRACK_SATURATION_NUMERICAL_TEST) && defined(_WIN32) && defined(_M_X64) && JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
struct TemporalLaneInput {
    std::array<Candidate::Frame, 4> h, u;
    std::array<const double*, 8> coefficients;
    std::array<double, 8> coordinate;
    std::array<bool, 4> paired;
    bool denseLow = false;
    std::uint8_t denseLowMask = 0;
};
static_assert(sizeof(TemporalLaneInput) == 264 && alignof(TemporalLaneInput) == 8);
static_assert(offsetof(TemporalLaneInput, h) == 0 && offsetof(TemporalLaneInput, u) == 64
    && offsetof(TemporalLaneInput, coefficients) == 128 && offsetof(TemporalLaneInput, coordinate) == 192
    && offsetof(TemporalLaneInput, paired) == 256 && offsetof(TemporalLaneInput, denseLow) == 260
    && offsetof(TemporalLaneInput, denseLowMask) == 261);
constexpr std::array<double, PrimitiveCurve::stride> temporalZeroCoefficients {};
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
// Keep integer observation work outside the original floating-point caller.
__declspec(noinline) void optimizationObserveLane(const TemporalLaneInput* input, unsigned character, unsigned route) noexcept {
    if (route == 5) { ++optimizationProfile.preparedFallbacks[character]; return; }
    if (route == 6) { ++optimizationProfile.legacyEntries[character]; return; }
    ++optimizationProfile.temporalEntries[character];
    if (route == 4) { ++optimizationProfile.denseLowEntries; return; }
    unsigned mask = 0;
    for (unsigned i = 0; i < 4; ++i)
        if (input->coefficients[i] == input->coefficients[i+4]) mask |= 1u << i;
    ++optimizationProfile.coefficientRows[route][mask];
}
#endif
#endif
struct Lane {
    Biquad lowShelf, highShelf;
    Candidate::Frame previousX {}, previousH {}, previousU {}, previousDifference {}, dc {};
    Candidate::Frame previousPrimitive {};
    std::array<PrimitiveKind, 2> primitiveKind {};
    SaturationCharacter primitiveCharacter = SaturationCharacter::soft;
    bool tiltCharged = false;
    double envelope = 0;
#if defined(AIRACK_SATURATION_TRANSITIONS)
    double bypassWet = 1;
    double mixFor(const SaturationLaneSettings& s) const noexcept { return double(s.mix) * bypassWet; }
#else
    double mixFor(const SaturationLaneSettings& s) const noexcept { return s.bypassed ? 0 : s.mix; }
#endif
#if defined(AIRACK_SATURATION_TRANSITIONS) && !defined(AIRACK_SATURATION_NUMERICAL_TEST) && defined(_WIN32) && defined(_M_X64) && JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    template<SaturationCharacter Character, bool Mixed = false>
    __declspec(noinline) void temporal4(std::array<Candidate::Frame, 4>& frames, const TemporalLaneInput& input,
        const Candidate::Frame& priorPrimitive, double mix, double amount, double wetGain, double dcAlpha) noexcept {
        static_assert(!Mixed || Character == SaturationCharacter::dense);
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        if constexpr (Mixed) { ++optimizationTemporalMixedBlocks; optimizationTemporalLastMixedMask = input.denseLowMask; }
#endif
        __m256d lowMask01, lowMask23;
        if constexpr (Mixed) {
            const auto mask = [&](unsigned offset) {
                return _mm256_set_pd(input.denseLowMask & (1u << (offset+3)) ? -0. : 0., input.denseLowMask & (1u << (offset+2)) ? -0. : 0.,
                    input.denseLowMask & (1u << (offset+1)) ? -0. : 0., input.denseLowMask & (1u << offset) ? -0. : 0.);
            };
            lowMask01 = mask(0); lowMask23 = mask(4);
        }
        const auto join = [](const Candidate::Frame& a, const Candidate::Frame& b) {
            return _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_loadu_pd(a.data())), _mm_loadu_pd(b.data()), 1);
        };
        const auto x01 = join(frames[0], frames[1]), x23 = join(frames[2], frames[3]);
        const auto h01 = join(input.h[0], input.h[1]), h23 = join(input.h[2], input.h[3]);
        const auto u01 = join(input.u[0], input.u[1]), u23 = join(input.u[2], input.u[3]);
        const auto half = _mm256_set1_pd(.5), one = _mm256_set1_pd(1.);
        __m256d p01, p23;
        if constexpr (Character == SaturationCharacter::dense && !Mixed) {
            if (input.denseLow) {
                const auto sign = _mm256_set1_pd(-0.);
                const auto a01 = _mm256_andnot_pd(sign, u01), a23 = _mm256_andnot_pd(sign, u23);
                p01 = _mm256_mul_pd(_mm256_mul_pd(half, a01), a01);
                p23 = _mm256_mul_pd(_mm256_mul_pd(half, a23), a23);
            }
        }
        if (Mixed || Character != SaturationCharacter::dense || !input.denseLow) {
            const auto coefficient = [&](std::size_t offset, std::size_t degree) {
                return _mm256_set_pd(input.coefficients[offset+3][degree], input.coefficients[offset+2][degree],
                    input.coefficients[offset+1][degree], input.coefficients[offset][degree]);
            };
            const auto t01 = _mm256_loadu_pd(input.coordinate.data()), t23 = _mm256_loadu_pd(input.coordinate.data()+4);
            p01 = coefficient(0, PrimitiveCurve::degree); p23 = coefficient(4, PrimitiveCurve::degree);
            for (std::size_t degree = PrimitiveCurve::degree; degree-- > 0;) {
                p01 = _mm256_add_pd(_mm256_mul_pd(p01, t01), coefficient(0, degree));
                p23 = _mm256_add_pd(_mm256_mul_pd(p23, t23), coefficient(4, degree));
            }
            if constexpr (Character == SaturationCharacter::glow) {
                const auto b = _mm256_set1_pd(preparedConstants.glowBias);
                const auto normalization = _mm256_sub_pd(one, _mm256_mul_pd(b, b));
                p01 = _mm256_div_pd(_mm256_sub_pd(p01, _mm256_mul_pd(b, u01)), normalization);
                p23 = _mm256_div_pd(_mm256_sub_pd(p23, _mm256_mul_pd(b, u23)), normalization);
            } else if constexpr (Character == SaturationCharacter::dense) {
                const auto sign = _mm256_set1_pd(-0.), knee = _mm256_set1_pd(.75);
                auto a01 = _mm256_andnot_pd(sign, u01), a23 = _mm256_andnot_pd(sign, u23);
                if constexpr (Mixed) { a01 = _mm256_blendv_pd(a01, knee, lowMask01); a23 = _mm256_blendv_pd(a23, knee, lowMask23); }
                p01 = _mm256_add_pd(_mm256_add_pd(_mm256_set1_pd(.28125), _mm256_mul_pd(knee, _mm256_sub_pd(a01, knee))), _mm256_mul_pd(_mm256_set1_pd(.0625), p01));
                p23 = _mm256_add_pd(_mm256_add_pd(_mm256_set1_pd(.28125), _mm256_mul_pd(knee, _mm256_sub_pd(a23, knee))), _mm256_mul_pd(_mm256_set1_pd(.0625), p23));
                if constexpr (Mixed) {
                    // Inactive primitives use exact neutral operands before arithmetic.
                    const auto low01 = _mm256_blendv_pd(_mm256_setzero_pd(), _mm256_andnot_pd(sign, u01), lowMask01);
                    const auto low23 = _mm256_blendv_pd(_mm256_setzero_pd(), _mm256_andnot_pd(sign, u23), lowMask23);
                    p01 = _mm256_blendv_pd(p01, _mm256_mul_pd(_mm256_mul_pd(half, low01), low01), lowMask01);
                    p23 = _mm256_blendv_pd(p23, _mm256_mul_pd(_mm256_mul_pd(half, low23), low23), lowMask23);
                }
            }
        }
        const auto prior = [&](const Candidate::Frame& initial, __m256d first) {
            return _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_loadu_pd(initial.data())), _mm256_castpd256_pd128(first), 1);
        };
        const auto previous = [](__m256d first, __m256d second) { return _mm256_permute2f128_pd(first, second, 0x21); };
        const auto d01 = _mm256_mul_pd(half, _mm256_add_pd(x01, prior(previousX, x01)));
        const auto d23 = _mm256_mul_pd(half, _mm256_add_pd(x23, previous(x01, x23)));
        const auto t01 = _mm256_mul_pd(half, _mm256_add_pd(h01, prior(previousH, h01)));
        const auto t23 = _mm256_mul_pd(half, _mm256_add_pd(h23, previous(h01, h23)));
        const auto delta01 = _mm256_sub_pd(u01, prior(previousU, u01));
        const auto delta23 = _mm256_sub_pd(u23, previous(u01, u23));
        const auto shaped01 = _mm256_div_pd(_mm256_sub_pd(p01, prior(priorPrimitive, p01)), delta01);
        const auto shaped23 = _mm256_div_pd(_mm256_sub_pd(p23, previous(p01, p23)), delta23);
        const auto difference01 = _mm256_sub_pd(_mm256_mul_pd(_mm256_set1_pd(R), shaped01), t01);
        const auto difference23 = _mm256_sub_pd(_mm256_mul_pd(_mm256_set1_pd(R), shaped23), t23);
        auto rollingDifference = join(previousDifference, previousDifference), rollingDc = join(dc, dc);
        const auto render = [&](__m256d d, __m256d t, __m256d difference) {
            rollingDc = _mm256_add_pd(_mm256_sub_pd(difference, rollingDifference), _mm256_mul_pd(_mm256_set1_pd(dcAlpha), rollingDc));
            rollingDifference = difference;
            const auto blendDifference = Character == SaturationCharacter::glow ? rollingDc : difference;
            return _mm256_add_pd(d, _mm256_mul_pd(_mm256_set1_pd(mix), _mm256_sub_pd(
                _mm256_mul_pd(_mm256_add_pd(t, _mm256_mul_pd(_mm256_set1_pd(amount), blendDifference)), _mm256_set1_pd(wetGain)), d)));
        };
        const auto low = [](__m256d value) { return _mm256_permute2f128_pd(value, value, 0x00); };
        const auto high = [](__m256d value) { return _mm256_permute2f128_pd(value, value, 0x11); };
        _mm_storeu_pd(frames[0].data(), _mm256_castpd256_pd128(render(low(d01), low(t01), low(difference01))));
        _mm_storeu_pd(frames[1].data(), _mm256_castpd256_pd128(render(high(d01), high(t01), high(difference01))));
        _mm_storeu_pd(frames[2].data(), _mm256_castpd256_pd128(render(low(d23), low(t23), low(difference23))));
        _mm_storeu_pd(frames[3].data(), _mm256_castpd256_pd128(render(high(d23), high(t23), high(difference23))));
        _mm_storeu_pd(previousX.data(), _mm256_extractf128_pd(x23, 1));
        _mm_storeu_pd(previousH.data(), _mm256_extractf128_pd(h23, 1));
        _mm_storeu_pd(previousU.data(), _mm256_extractf128_pd(u23, 1));
        _mm_storeu_pd(previousPrimitive.data(), _mm256_extractf128_pd(p23, 1));
        _mm_storeu_pd(previousDifference.data(), _mm256_castpd256_pd128(rollingDifference));
        _mm_storeu_pd(dc.data(), _mm256_castpd256_pd128(rollingDc));
        primitiveKind = { PrimitiveKind::curve, PrimitiveKind::curve };
        _mm256_zeroupper();
    }
#endif
    template<SaturationCharacter Character>
    void process4Character(std::array<Candidate::Frame, 4>& frames, const SaturationLaneSettings& s,
        const Coefficients& low, const Coefficients& high, double driveGain, double amount, double wetGain, double dcAlpha,
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve, bool allowTemporal = false) noexcept {
        const bool tilted = s.preTiltDb != 0;
        if (tilted) tiltCharged = true;
        else if (tiltCharged) { lowShelf = {}; highShelf = {}; tiltCharged = false; }
        const double mix = mixFor(s);
        if (primitiveCharacter != Character) { primitiveKind = {}; primitiveCharacter = Character; }
        const auto prepare = [&](const Candidate::Frame& x, Candidate::Frame& h, Candidate::Frame& u) {
            h = x;
            if (tilted) h = highShelf.process(lowShelf.process(h, low), high);
            u = { driveGain * h[0] / R, driveGain * h[1] / R };
            if constexpr (Character == SaturationCharacter::dense) {
                const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
                const double alpha = peak > envelope ? sagAttack : sagRelease;
                envelope = peak + alpha * (envelope - peak);
                const double reduction = std::clamp(6 * std::max(envelope - .5, 0.) / (envelope + .5), 0., 6.);
                const double sag = gainCurve.gain(-reduction); u[0] *= sag; u[1] *= sag;
            }
        };
#if defined(AIRACK_SATURATION_TRANSITIONS) && !defined(AIRACK_SATURATION_NUMERICAL_TEST) && defined(_WIN32) && defined(_M_X64) && JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        allowTemporal = allowTemporal && !optimizationOriginalLane;
#endif
        // Poisoned private histories retain the original interleaved arithmetic and NaN words.
        const bool prepared = allowTemporal && finite(previousX) && finite(previousH) && finite(previousU)
            && finite(previousPrimitive) && finite(previousDifference) && finite(dc)
            && finite(lowShelf.z1) && finite(lowShelf.z2) && finite(highShelf.z1) && finite(highShelf.z2)
            && (std::bit_cast<std::uint64_t>(envelope) & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
        TemporalLaneInput input;
        if (prepared) {
            bool eligible = true, denseLow = true, denseHigh = true;
            for (std::size_t n = 0; n < 4; ++n) {
                prepare(frames[n], input.h[n], input.u[n]);
                const auto& previous = n == 0 ? previousU : input.u[n-1];
                input.paired[n] = quotientPrimitiveKind(input.u[n][0], previous[0]) == PrimitiveKind::curve
                    && quotientPrimitiveKind(input.u[n][1], previous[1]) == PrimitiveKind::curve;
                eligible = input.paired[n] && eligible;
                if constexpr (Character == SaturationCharacter::dense) for (std::size_t ch = 0; ch < 2; ++ch) {
                    const double value = input.u[n][ch];
                    const bool low = std::abs(value) <= .75;
                    denseLow = low && denseLow;
                    denseHigh = (std::abs(value) > .75) && denseHigh;
                    if (low) input.denseLowMask |= std::uint8_t(1u << (n*2+ch));
                }
            }
            input.denseLow = Character == SaturationCharacter::dense && denseLow;
            if (eligible && !input.denseLow) for (std::size_t n = 0; n < 4; ++n) for (std::size_t ch = 0; ch < 2; ++ch) {
                if constexpr (Character == SaturationCharacter::dense) if (input.denseLowMask & (1u << (n*2+ch))) {
                    input.coordinate[n*2+ch] = 0.; input.coefficients[n*2+ch] = temporalZeroCoefficients.data(); continue;
                }
                const double u = input.u[n][ch];
                const double argument = Character == SaturationCharacter::soft ? u
                    : Character == SaturationCharacter::glow ? u + .35 : 4 * (std::abs(u) - .75);
                const double v = std::abs(argument);
                if (!(v < PrimitiveCurve::limit)) { eligible = false; continue; }
                const int cell = int(v * PrimitiveCurve::cellsPerUnit);
                input.coordinate[n*2+ch] = v - (double(cell) + .5) / PrimitiveCurve::cellsPerUnit;
                input.coefficients[n*2+ch] = curve.coefficients.data() + cell * PrimitiveCurve::stride;
            }
            if (eligible) {
                const Candidate::Frame priorPrimitive {
                    primitiveKind[0] == PrimitiveKind::curve ? previousPrimitive[0] : curvePrimitive(curve, Character, previousU[0]),
                    primitiveKind[1] == PrimitiveKind::curve ? previousPrimitive[1] : curvePrimitive(curve, Character, previousU[1]) };
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
                ++optimizationTemporalBlocks;
                ++optimizationTemporalByCharacter[unsigned(Character)];
                if (input.denseLow) ++optimizationTemporalLowBlocks; else optimizationTemporalHornerChains += 2;
                if (optimizationSampling) optimizationProfile.selectors[unsigned(Character) * 3 + unsigned(PrimitiveKind::curve)] += 8;
                if (optimizationSampling) optimizationObserveLane(&input, unsigned(Character),
                    input.denseLow ? 4u : Character == SaturationCharacter::dense ? (denseHigh ? 2u : 3u) : unsigned(Character));
#endif
                if constexpr (Character == SaturationCharacter::dense) if (!denseLow && !denseHigh) {
                    temporal4<Character, true>(frames, input, priorPrimitive, mix, amount, wetGain, dcAlpha); return;
                }
                temporal4<Character>(frames, input, priorPrimitive, mix, amount, wetGain, dcAlpha); return;
            }
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
            ++optimizationTemporalPreparedFallbacks;
            ++optimizationTemporalFallbackByCharacter[unsigned(Character)];
            if (optimizationSampling) optimizationObserveLane(nullptr, unsigned(Character), 5);
#endif
        }
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        else {
            ++optimizationTemporalLegacy; ++optimizationTemporalLegacyByCharacter[unsigned(Character)];
            if (optimizationSampling) optimizationObserveLane(nullptr, unsigned(Character), 6);
        }
#endif
#else
        (void) allowTemporal;
#endif
        for (std::size_t n = 0; n < 4; ++n) {
            auto& frame = frames[n];
            const auto x = frame;
            Candidate::Frame h, u;
#if defined(AIRACK_SATURATION_TRANSITIONS) && !defined(AIRACK_SATURATION_NUMERICAL_TEST) && defined(_WIN32) && defined(_M_X64) && JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
            if (prepared) { h = input.h[n]; u = input.u[n]; }
            else
#endif
            prepare(x, h, u);
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
            const bool pairedCurve =
#if defined(AIRACK_SATURATION_TRANSITIONS) && !defined(AIRACK_SATURATION_NUMERICAL_TEST) && defined(_WIN32) && defined(_M_X64) && !defined(__AVX2__)
                prepared ? input.paired[n] :
#endif
                quotientPrimitiveKind(u[0], previousU[0]) == PrimitiveKind::curve
                && quotientPrimitiveKind(u[1], previousU[1]) == PrimitiveKind::curve;
            if (pairedCurve) {
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
                ++optimizationStereoFrameCalls;
                if (optimizationSampling) optimizationProfile.selectors[unsigned(Character) * 3 + unsigned(PrimitiveKind::curve)] += 2;
#endif
                const auto currentPrimitive = curvePrimitivePair<Character>(curve, u);
                const Candidate::Frame priorPrimitive {
                    primitiveKind[0] == PrimitiveKind::curve ? previousPrimitive[0] : curvePrimitive(curve, Character, previousU[0]),
                    primitiveKind[1] == PrimitiveKind::curve ? previousPrimitive[1] : curvePrimitive(curve, Character, previousU[1]) };
                const auto d = _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(_mm_loadu_pd(x.data()), _mm_loadu_pd(previousX.data())));
                const auto t = _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(_mm_loadu_pd(h.data()), _mm_loadu_pd(previousH.data())));
                const auto delta = _mm_sub_pd(_mm_loadu_pd(u.data()), _mm_loadu_pd(previousU.data()));
                const auto shaped = _mm_div_pd(_mm_sub_pd(_mm_loadu_pd(currentPrimitive.data()), _mm_loadu_pd(priorPrimitive.data())), delta);
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                Candidate::Frame numericalShaped; _mm_storeu_pd(numericalShaped.data(), shaped);
                for (std::size_t ch = 0; ch < 2; ++ch)
                    recordNumericalLane(this, 0, Character, int(ch), u[ch], previousU[ch], numericalShaped[ch],
                        currentPrimitive[ch], priorPrimitive[ch], previousPrimitive[ch], primitiveKind[ch], PrimitiveKind::curve);
#endif
                const auto difference = _mm_sub_pd(_mm_mul_pd(_mm_set1_pd(R), shaped), t);
                const auto nextDc = _mm_add_pd(_mm_sub_pd(difference, _mm_loadu_pd(previousDifference.data())),
                    _mm_mul_pd(_mm_set1_pd(dcAlpha), _mm_loadu_pd(dc.data())));
                const auto blendDifference = Character == SaturationCharacter::glow ? nextDc : difference;
                const auto result = _mm_add_pd(d, _mm_mul_pd(_mm_set1_pd(mix),
                    _mm_sub_pd(_mm_mul_pd(_mm_add_pd(t, _mm_mul_pd(_mm_set1_pd(amount), blendDifference)), _mm_set1_pd(wetGain)), d)));
                _mm_storeu_pd(dc.data(), nextDc); _mm_storeu_pd(previousDifference.data(), difference);
                previousPrimitive = currentPrimitive; primitiveKind = { PrimitiveKind::curve, PrimitiveKind::curve };
                previousX = x; previousH = h; previousU = u; _mm_storeu_pd(frame.data(), result);
                continue;
            }
#endif
            Candidate::Frame result {};
            for (std::size_t ch = 0; ch < 2; ++ch) {
                const double d = .5 * (x[ch] + previousX[ch]), t = .5 * (h[ch] + previousH[ch]);
                const double delta = u[ch] - previousU[ch];
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                const auto numericalPriorKind = primitiveKind[ch];
#endif
                double shaped;
                if (std::abs(delta) < 1e-6 * (1 + std::max(std::abs(u[ch]), std::abs(previousU[ch])))) {
                    shaped = cachedShape(Character, .5 * (u[ch] + previousU[ch])); primitiveKind[ch] = PrimitiveKind::none;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
                    if (optimizationSampling) ++optimizationProfile.selectors[unsigned(Character) * 3];
#endif
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                recordNumericalLane(this, 0, Character, int(ch), u[ch], previousU[ch], shaped,
                    0, 0, previousPrimitive[ch], numericalPriorKind, PrimitiveKind::none);
#endif
                } else {
                    const auto kind = quotientPrimitiveKind(u[ch], previousU[ch]);
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
                    if (optimizationSampling) ++optimizationProfile.selectors[unsigned(Character) * 3 + unsigned(kind)];
#endif
                    const auto evaluate = [&](double value) {
                        return kind == PrimitiveKind::exact ? cachedPrimitive(Character, value) : curvePrimitive(curve, Character, value);
                    };
                    const bool needsPrimitive = kind == PrimitiveKind::curve
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                        || true // Retain the numerical observer's original primitive/cache values.
#endif
                        ;
                    const double currentPrimitive = needsPrimitive ? evaluate(u[ch]) : 0.;
                    const double priorPrimitive = needsPrimitive
                        ? (primitiveKind[ch] == kind ? previousPrimitive[ch] : evaluate(previousU[ch])) : 0.;
                    shaped = kind == PrimitiveKind::exact
                        ? exactQuotient(Character, u[ch], previousU[ch], preparedConstants.glowBias,
                                        1 - preparedConstants.glowBias * preparedConstants.glowBias)
                        : (currentPrimitive - priorPrimitive) / delta;
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                recordNumericalLane(this, 0, Character, int(ch), u[ch], previousU[ch], shaped,
                    currentPrimitive, priorPrimitive, previousPrimitive[ch], numericalPriorKind, kind);
#endif
                    previousPrimitive[ch] = currentPrimitive; primitiveKind[ch] = kind;
                }
                double difference = R * shaped - t;
                dc[ch] = difference - previousDifference[ch] + dcAlpha * dc[ch]; previousDifference[ch] = difference;
                if constexpr (Character == SaturationCharacter::glow) difference = dc[ch];
                result[ch] = d + mix * ((t + amount * difference) * wetGain - d);
            }
            previousX = x; previousH = h; previousU = u; frame = result;
        }
    }
    void process4(std::array<Candidate::Frame, 4>& frames, const SaturationLaneSettings& s,
        const Coefficients& low, const Coefficients& high, double driveGain, double amount, double wetGain, double dcAlpha,
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve, bool allowTemporal = false) noexcept {
        AIRACK_OPTIMIZATION_SCOPE(laneScope, lane);
        switch (s.character) {
            case SaturationCharacter::soft:
                process4Character<SaturationCharacter::soft>(frames, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve, allowTemporal); return;
            case SaturationCharacter::glow:
                process4Character<SaturationCharacter::glow>(frames, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve, allowTemporal); return;
            case SaturationCharacter::dense:
                process4Character<SaturationCharacter::dense>(frames, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve, allowTemporal); return;
        }
        for (auto& frame : frames)
            frame = process<true>(frame, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve);
    }
    template<SaturationCharacter Character>
    Candidate::Frame processFourCharacter(Candidate::Frame x, const SaturationLaneSettings& s,
        const Coefficients& low, const Coefficients& high, double driveGain, double amount, double wetGain, double dcAlpha,
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve) noexcept {
        auto h = x;
        if (s.preTiltDb != 0) {
            h = highShelf.process(lowShelf.process(h, low), high); tiltCharged = true;
        } else if (tiltCharged) { lowShelf = {}; highShelf = {}; tiltCharged = false; }
        Candidate::Frame u { driveGain * h[0] / R, driveGain * h[1] / R };
        if constexpr (Character == SaturationCharacter::dense) {
            const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
            const double alpha = peak > envelope ? sagAttack : sagRelease;
            envelope = peak + alpha * (envelope - peak);
            const double reduction = std::clamp(6 * std::max(envelope - .5, 0.) / (envelope + .5), 0., 6.);
            const double sag = gainCurve.gain(-reduction); u[0] *= sag; u[1] *= sag;
        }
        Candidate::Frame result {};
        const double mix = mixFor(s);
        if (primitiveCharacter != Character) { primitiveKind = {}; primitiveCharacter = Character; }
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double d = .5 * (x[ch] + previousX[ch]), t = .5 * (h[ch] + previousH[ch]);
            const double delta = u[ch] - previousU[ch];
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                const auto numericalPriorKind = primitiveKind[ch];
#endif
            double shaped;
            if (std::abs(delta) < 1e-6 * (1 + std::max(std::abs(u[ch]), std::abs(previousU[ch])))) {
                shaped = cachedShape(Character, .5 * (u[ch] + previousU[ch])); primitiveKind[ch] = PrimitiveKind::none;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
                if (optimizationSampling) ++optimizationProfile.selectors[unsigned(Character) * 3];
#endif
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                recordNumericalLane(this, 1, Character, int(ch), u[ch], previousU[ch], shaped,
                    0, 0, previousPrimitive[ch], numericalPriorKind, PrimitiveKind::none);
#endif
            } else {
                const auto kind = quotientPrimitiveKind(u[ch], previousU[ch]);
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
                if (optimizationSampling) ++optimizationProfile.selectors[unsigned(Character) * 3 + unsigned(kind)];
#endif
                const auto evaluate = [&](double value) {
                    return kind == PrimitiveKind::exact ? cachedPrimitive(Character, value) : curvePrimitive(curve, Character, value);
                };
                const bool needsPrimitive = kind == PrimitiveKind::curve
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                    || true // Retain the numerical observer's original primitive/cache values.
#endif
                    ;
                const double currentPrimitive = needsPrimitive ? evaluate(u[ch]) : 0.;
                const double priorPrimitive = needsPrimitive
                    ? (primitiveKind[ch] == kind ? previousPrimitive[ch] : evaluate(previousU[ch])) : 0.;
                shaped = kind == PrimitiveKind::exact
                    ? exactQuotient(Character, u[ch], previousU[ch], preparedConstants.glowBias,
                                    1 - preparedConstants.glowBias * preparedConstants.glowBias)
                    : (currentPrimitive - priorPrimitive) / delta;
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                recordNumericalLane(this, 1, Character, int(ch), u[ch], previousU[ch], shaped,
                    currentPrimitive, priorPrimitive, previousPrimitive[ch], numericalPriorKind, kind);
#endif
                previousPrimitive[ch] = currentPrimitive; primitiveKind[ch] = kind;
            }
            double difference = R * shaped - t;
            dc[ch] = difference - previousDifference[ch] + dcAlpha * dc[ch]; previousDifference[ch] = difference;
            if constexpr (Character == SaturationCharacter::glow) difference = dc[ch];
            result[ch] = d + mix * ((t + amount * difference) * wetGain - d);
        }
        previousX = x; previousH = h; previousU = u; return result;
    }
    template<bool Four, bool ProductState = false>
    Candidate::Frame process(Candidate::Frame x, const SaturationLaneSettings& s,
        const Coefficients& low, const Coefficients& high, double driveGain, double amount, double wetGain, double dcAlpha,
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve) noexcept {
        if constexpr (Four) {
            switch (s.character) {
                case SaturationCharacter::soft:
                    return processFourCharacter<SaturationCharacter::soft>(x, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve);
                case SaturationCharacter::glow:
                    return processFourCharacter<SaturationCharacter::glow>(x, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve);
                case SaturationCharacter::dense:
                    return processFourCharacter<SaturationCharacter::dense>(x, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve);
            }
        }
        auto h = x;
        if (s.preTiltDb != 0) {
            h = highShelf.process<ProductState>(lowShelf.process<ProductState>(h, low), high); tiltCharged = true;
        } else if (tiltCharged) { lowShelf = {}; highShelf = {}; tiltCharged = false; }
        Candidate::Frame u { driveGain * h[0] / R, driveGain * h[1] / R };
        if (s.character == SaturationCharacter::dense) {
            const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
            const double alpha = peak > envelope ? sagAttack : sagRelease;
            envelope = peak + alpha * (envelope - peak);
            const double reduction = std::clamp(6 * std::max(envelope - .5, 0.) / (envelope + .5), 0., 6.);
            const double sag = Four ? gainCurve.gain(-reduction) : std::pow(10., -reduction / 20); u[0] *= sag; u[1] *= sag;
        }
        Candidate::Frame result {};
        const double mix = mixFor(s);
        if (primitiveCharacter != s.character) { primitiveKind = {}; primitiveCharacter = s.character; }
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double d = .5 * (x[ch] + previousX[ch]), t = .5 * (h[ch] + previousH[ch]);
            const double delta = u[ch] - previousU[ch];
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                const auto numericalPriorKind = primitiveKind[ch];
#endif
            double shaped;
            if (std::abs(delta) < 1e-6 * (1 + std::max(std::abs(u[ch]), std::abs(previousU[ch])))) {
                shaped = cachedShape(s.character, .5 * (u[ch] + previousU[ch])); primitiveKind[ch] = PrimitiveKind::none;
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                recordNumericalLane(this, 2, s.character, int(ch), u[ch], previousU[ch], shaped,
                    0, 0, previousPrimitive[ch], numericalPriorKind, PrimitiveKind::none);
#endif
            } else {
                const auto kind = Four ? quotientPrimitiveKind(u[ch], previousU[ch]) : PrimitiveKind::exact;
                const auto evaluate = [&](double value) {
                    return kind == PrimitiveKind::exact ? cachedPrimitive(s.character, value) : curvePrimitive(curve, s.character, value);
                };
                const double currentPrimitive = evaluate(u[ch]);
                const double priorPrimitive = primitiveKind[ch] == kind ? previousPrimitive[ch] : evaluate(previousU[ch]);
                shaped = kind == PrimitiveKind::exact
                    ? exactQuotient(s.character, u[ch], previousU[ch], preparedConstants.glowBias,
                                    1 - preparedConstants.glowBias * preparedConstants.glowBias)
                    : (currentPrimitive - priorPrimitive) / delta;
#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
                recordNumericalLane(this, 2, s.character, int(ch), u[ch], previousU[ch], shaped,
                    currentPrimitive, priorPrimitive, previousPrimitive[ch], numericalPriorKind, kind);
#endif
                previousPrimitive[ch] = currentPrimitive; primitiveKind[ch] = kind;
            }
            double difference = R * shaped - t;
            dc[ch] = difference - previousDifference[ch] + dcAlpha * dc[ch]; previousDifference[ch] = difference;
            if (s.character == SaturationCharacter::glow) difference = dc[ch];
            result[ch] = d + mix * ((t + amount * difference) * wetGain - d);
        }
        previousX = x; previousH = h; previousU = u; return result;
    }
};
}
std::span<const CoefficientView> saturationCoefficientManifest() noexcept {
    try { return resources().manifest; } catch (...) { return {}; }
}

#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
void optimizationProfileSample(bool enabled) noexcept { optimizationSampling = enabled; }
void optimizationProfileReset() noexcept {
    for (unsigned character = 0; character < 3; ++character)
        std::cout << "TEMPORAL_WORKLOAD_COUNTS character=" << character << " blocks=" << optimizationTemporalByCharacter[character]
            << " prepared_fallbacks=" << optimizationTemporalFallbackByCharacter[character] << " legacy=" << optimizationTemporalLegacyByCharacter[character] << '\n';
    optimizationTemporalByCharacter = {}; optimizationTemporalFallbackByCharacter = {}; optimizationTemporalLegacyByCharacter = {};
    optimizationProfile = {};
}
OptimizationProfile optimizationProfileRead() noexcept { return optimizationProfile; }
OptimizationProfile runSaturationOptimizationProfileOverhead() noexcept {
    optimizationProfileReset(); optimizationProfileSample(true);
    for (unsigned i = 0; i < 100000; ++i) {
        AIRACK_OPTIMIZATION_SCOPE(emptyCandidateScope, candidate);
    }
    optimizationProfileSample(false); return optimizationProfileRead();
}
int runSaturationCommonControlChecks();
int runSaturationOptimizationPathChecks() {
    int failures = 0;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    using CoefficientStereo = juce::dsp::SIMDRegister<double>;
    if constexpr (CoefficientStereo::size() == 2) {
        std::size_t values = 0, words = 0;
        const auto compareCoefficientStereo = [&](std::uint64_t bits) {
            const double value = std::bit_cast<double>(bits);
            alignas(CoefficientStereo::SIMDRegisterSize) std::array<double, 2> expected {}, actual {};
            CoefficientStereo(value).copyToRawArray(expected.data());
            coefficientStereo(value).copyToRawArray(actual.data());
            ++values;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                ++words;
                if (std::bit_cast<std::uint64_t>(actual[channel]) != std::bit_cast<std::uint64_t>(expected[channel])) ++failures;
                if (std::bit_cast<std::uint64_t>(actual[channel]) != bits) ++failures;
            }
        };
        for (std::uint64_t exponent = 0; exponent < 2048; ++exponent)
            for (std::uint64_t sign : { 0ULL, 0x8000000000000000ULL })
                for (std::uint64_t mantissa : { 0ULL, 1ULL, 0x0007ffffffffffffULL, 0x0008000000000000ULL,
                        0x0008000000000001ULL, 0x000fffffffffffffULL })
                    compareCoefficientStereo(sign | (exponent << 52) | mantissa);
        std::uint64_t varied = 0x96b236e54f702ad1ULL;
        for (unsigned n = 0; n < 16384; ++n) {
            varied ^= varied << 13; varied ^= varied >> 7; varied ^= varied << 17;
            compareCoefficientStereo(varied);
        }
        compareCoefficientStereo(0); compareCoefficientStereo(0x8000000000000000ULL);
        if (values != 40962 || words != 81924) ++failures;
        std::cout << "COEFFICIENT_STEREO values=" << values << " words=" << words << " failures=" << failures << '\n';
    }
#endif
    const double infinity = std::numeric_limits<double>::infinity();
    const auto check = [&](bool okay) { if (!okay) ++failures; };
    const auto originalFinite = [](Candidate::Frame x) { return std::isfinite(x[0]) && std::isfinite(x[1]); };
    const auto representationFinite = [](Candidate::Frame x) { return finiteValue(x[0]) && finiteValue(x[1]); };
    std::size_t finiteFrames = 0, environmentFrames = 0;
    const auto compareFinite = [&](Candidate::Frame x) {
        ++finiteFrames; check(finite(x) == originalFinite(x)); check(finite(x) == representationFinite(x));
    };
    for (std::uint64_t exponent = 0; exponent < 2048; ++exponent)
        for (std::uint64_t sign : { 0ULL, 0x8000000000000000ULL })
            for (std::uint64_t mantissa : { 0ULL, 1ULL, 0x0007ffffffffffffULL, 0x0008000000000000ULL, 0x000fffffffffffffULL }) {
                const double value = std::bit_cast<double>(sign | (exponent << 52) | mantissa);
                compareFinite({ value, value });
                for (double peer : { 0., infinity }) { compareFinite({ value, peer }); compareFinite({ peer, value }); }
            }
    std::uint64_t bits = 0x96b236e54f702ad1ULL;
    const auto variedValue = [&] {
        bits ^= bits << 13; bits ^= bits >> 7; bits ^= bits << 17;
        return std::bit_cast<double>(bits);
    };
    for (unsigned n = 0; n < 16384; ++n) {
        const double left = variedValue(), right = variedValue();
        compareFinite({ left, right }); compareFinite({ right, left });
    }
    std::fenv_t savedEnvironment;
    const bool held = std::feholdexcept(&savedEnvironment) == 0; check(held);
    if (held) {
        for (int rounding : { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO }) {
            check(std::fesetround(rounding) == 0);
            for (int flags : { 0, FE_INVALID, FE_DIVBYZERO })
                for (std::uint64_t word : { 0ULL, 0x8000000000000000ULL, 1ULL, 0x000fffffffffffffULL,
                        0x0010000000000000ULL, 0x7fefffffffffffffULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
                        0x7ff8000000000000ULL, 0xfff8000000000001ULL, 0x7ff0000000000001ULL, 0xfff7ffffffffffffULL })
                    for (double peer : { 1., infinity }) for (bool reverse : { false, true }) {
                        const double value = std::bit_cast<double>(word);
                        const Candidate::Frame frame = reverse ? Candidate::Frame { peer, value } : Candidate::Frame { value, peer };
                        const auto observe = [&](int original) {
                            check(std::feclearexcept(FE_ALL_EXCEPT) == 0); check(std::feraiseexcept(flags) == 0);
                            const bool result = original == 2 ? representationFinite(frame) : original ? originalFinite(frame) : finite(frame);
                            return std::array { int(result), std::fetestexcept(FE_ALL_EXCEPT), std::fegetround() };
                        };
                        const auto expected = observe(true), actual = observe(false);
                        ++environmentFrames; check(actual == expected); check(actual[2] == rounding); check(observe(2) == expected);
                    }
        }
        check(std::fesetenv(&savedEnvironment) == 0);
    }
    check(finiteFrames == 135168 && environmentFrames == 576);
    std::cout << "FINITE_PREDICATE frames=" << finiteFrames << " environment_frames=" << environmentFrames << " failures=" << failures << '\n';
    {
        std::size_t arithmeticCases = 0; std::array<std::size_t, 4> wordFailures {}, environmentFailures {}, alternativePayloadWords {};
        std::fenv_t environment; const bool saved = std::feholdexcept(&environment) == 0; check(saved);
        if (saved) {
            constexpr std::array<std::uint64_t, 14> words { 0, 0x8000000000000000ULL, 1, 0x800fffffffffffffULL,
                0x0010000000000000ULL, 0x7fefffffffffffffULL, 0xffefffffffffffffULL, 0x3ff0000000000000ULL,
                0xbff0000000000000ULL, 0x7ff0000000000000ULL, 0xfff0000000000000ULL,
                0x7ff8000000000123ULL, 0xfff8000000000456ULL, 0x7ff0000000000001ULL };
            for (int rounding : { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO }) {
                check(std::fesetround(rounding) == 0);
                for (int flags : { 0, FE_INVALID, FE_DIVBYZERO }) for (std::size_t n = 0; n < words.size(); ++n)
                    for (std::size_t m = 0; m < words.size(); ++m) for (unsigned operation = 0; operation < 4; ++operation) {
                        const Candidate::Frame a { std::bit_cast<double>(words[n]), std::bit_cast<double>(words[m]) };
                        const Candidate::Frame b { std::bit_cast<double>(words[m]), std::bit_cast<double>(words[(n + 3) % words.size()]) };
                        const Candidate::Frame previous { -a[1], a[0] }; const double scalar = b[0];
                        const auto observe = [&](bool original) {
                            check(std::feclearexcept(FE_ALL_EXCEPT) == 0); check(std::feraiseexcept(flags) == 0);
                            const auto result = returnArithmetic(original, operation, a, b, previous, scalar);
                            return std::pair { result, std::array { std::fetestexcept(FE_ALL_EXCEPT), std::fegetround() } };
                        };
                        const auto expected = observe(true), actual = observe(false);
                        const auto word = [](double x) { return std::bit_cast<std::uint64_t>(x); };
                        const auto isNaN = [&](double x) { return (word(x) & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL && (word(x) & 0x000fffffffffffffULL) != 0; };
                        const auto invalidSum = [&](double x, double y) {
                            return (word(x) & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL
                                && (word(y) & 0x7fffffffffffffffULL) == 0x7ff0000000000000ULL && (word(x) ^ word(y)) >> 63;
                        };
                        bool sameWords = true, permittedWords = true; std::array<unsigned, 4> choices {};
                        for (std::size_t row = 0; row < 2; ++row) for (std::size_t ch = 0; ch < 2; ++ch) {
                            const unsigned averageInputs = unsigned(isNaN(a[ch])) + unsigned(isNaN(previous[ch]));
                            const bool averageNaN = averageInputs != 0 || invalidSum(a[ch], previous[ch]);
                            unsigned contributing = 0;
                            if (row == 0 && operation == 0) contributing = unsigned(isNaN(b[ch])) + unsigned(isNaN(previous[ch]));
                            if (operation == 1) contributing = row == 0 ? unsigned(isNaN(a[ch])) + unsigned(isNaN(b[ch]))
                                : std::max(averageInputs, unsigned(isNaN(b[ch])) + unsigned(averageNaN));
                            if (row == 0 && operation == 2) contributing = unsigned(isNaN(a[ch])) + unsigned(isNaN(scalar));
                            choices[row * 2 + ch] = contributing;
                            const auto e = word(expected.first[row][ch]), aWord = word(actual.first[row][ch]);
                            if (aWord != e) {
                                sameWords = false;
                                const bool alternative = contributing >= 2 && isNaN(expected.first[row][ch]) && isNaN(actual.first[row][ch])
                                    && (aWord & 0x0008000000000000ULL) == (e & 0x0008000000000000ULL);
                                permittedWords = permittedWords && alternative; alternativePayloadWords[operation] += alternative;
                            }
                        }
                        std::cout << "STEREO_RETURN_WORDS," << operation << ',' << n << ',' << m << ',' << rounding << ',' << flags;
                        for (const auto& frame : { a, b, previous }) for (double value : frame) std::cout << ',' << std::hex << word(value) << std::dec;
                        std::cout << ',' << std::hex << word(scalar) << std::dec;
                        for (std::size_t row = 0; row < 2; ++row) for (std::size_t ch = 0; ch < 2; ++ch)
                            std::cout << ',' << std::hex << word(expected.first[row][ch]) << ',' << word(actual.first[row][ch]) << std::dec << ',' << choices[row * 2 + ch];
                        std::cout << ',' << expected.second[0] << ',' << actual.second[0] << ',' << expected.second[1] << ',' << actual.second[1] << '\n';
                        wordFailures[operation] += !sameWords; environmentFailures[operation] += actual.second != expected.second;
                        check(sameWords || permittedWords); check(actual.second == expected.second); ++arithmeticCases;
                    }
            }
            check(std::fesetenv(&environment) == 0);
        }
        for (std::size_t operation = 0; operation < 4; ++operation)
            std::cout << "STEREO_RETURN_CLASS operation=" << operation << " word_failures=" << wordFailures[operation]
                << " environment_failures=" << environmentFailures[operation] << " alternative_nan_words=" << alternativePayloadWords[operation] << '\n';
        check(arithmeticCases == 9408);
        std::cout << "STEREO_RETURN_ARITHMETIC cases=" << arithmeticCases << " failures=" << failures << '\n';
    }
    const auto sameFrame = [](const auto& a, const auto& b) { return std::memcmp(a.data(), b.data(), sizeof(a)) == 0; };
    const auto sameLane = [&](const Lane& a, const Lane& b) {
        return sameFrame(a.previousX, b.previousX) && sameFrame(a.previousH, b.previousH)
            && sameFrame(a.previousU, b.previousU) && sameFrame(a.previousDifference, b.previousDifference)
            && sameFrame(a.dc, b.dc) && sameFrame(a.previousPrimitive, b.previousPrimitive)
            && sameFrame(a.lowShelf.z1, b.lowShelf.z1) && sameFrame(a.lowShelf.z2, b.lowShelf.z2)
            && sameFrame(a.highShelf.z1, b.highShelf.z1) && sameFrame(a.highShelf.z2, b.highShelf.z2)
            && sameFrame(a.lowShelf.z1Low, b.lowShelf.z1Low) && sameFrame(a.lowShelf.z2Low, b.lowShelf.z2Low)
            && sameFrame(a.highShelf.z1Low, b.highShelf.z1Low) && sameFrame(a.highShelf.z2Low, b.highShelf.z2Low)
            && a.primitiveKind == b.primitiveKind && a.primitiveCharacter == b.primitiveCharacter
            && std::bit_cast<std::uint64_t>(a.envelope) == std::bit_cast<std::uint64_t>(b.envelope)
            && a.tiltCharged == b.tiltCharged && a.bypassWet == b.bypassWet;
    };
    const auto& r = resources(); const Coefficients coefficients {};
    const auto scalar = [&](Lane& lane, Candidate::Frame input, const SaturationLaneSettings& s) {
        return lane.process<true>(input, s, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve);
    };
#if defined(_WIN32) && defined(_M_X64)
    if (juce::SystemStats::hasAVX() && wide_network::operatingSystemSupportsAvx()) {
        for (auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
            SaturationLaneSettings setting; setting.character = character; setting.preTiltDb = 2;
            Lane actual; actual.primitiveCharacter = character; actual.tiltCharged = true;
            actual.previousX = { -.03125, .0625 }; actual.previousH = { .125, -.25 }; actual.previousU = { -.5, -.625 };
            actual.previousDifference = { .0625, -.125 }; actual.dc = { -.25, .375 }; actual.envelope = 1.125;
            actual.lowShelf.z1 = { .0001, -.0002 }; actual.lowShelf.z2 = { -.0003, .0004 };
            actual.highShelf.z1 = { -.0005, .0006 }; actual.highShelf.z2 = { .0007, -.0008 };
            actual.primitiveKind = { PrimitiveKind::curve, PrimitiveKind::curve };
            for (std::size_t ch = 0; ch < 2; ++ch) actual.previousPrimitive[ch] = curvePrimitive(r.primitiveCurve, character, actual.previousU[ch]);
            Lane expected = actual;
            std::array<Candidate::Frame, 4> frames {{ { 1.5*R, 2.*R }, { 2.*R, 2.5*R }, { 2.5*R, 3.*R }, { 3.*R, 3.5*R } }};
            auto reference = frames;
            optimizationTemporalBlocks = optimizationTemporalHornerChains = optimizationTemporalLowBlocks = 0;
            optimizationScalarCurveCalls = optimizationPairCurveCalls = optimizationStereoFrameCalls = optimizationSelectorCalls = 0;
            actual.process4(frames, setting, coefficients, coefficients, 1, .5, .9, .999, .25, .75, r.primitiveCurve, r.gainCurve, true);
            const auto blocks = optimizationTemporalBlocks, chains = optimizationTemporalHornerChains;
            const auto pairs = optimizationPairCurveCalls, selectors = optimizationSelectorCalls;
            expected.process4(reference, setting, coefficients, coefficients, 1, .5, .9, .999, .25, .75, r.primitiveCurve, r.gainCurve);
            check(sameFrame(frames, reference) && sameLane(actual, expected));
            std::cout << "TEMPORAL_COST character=" << int(character) << " blocks=" << blocks << " ymm_horner_chains=" << chains
                << " pair_horner=" << pairs << " selectors=" << selectors << '\n';
            check(blocks == 1 && chains == 2 && pairs == 0 && selectors == 8);
        }
        const auto chargedLane = [&](SaturationCharacter character, unsigned kinds) {
            Lane lane; lane.primitiveCharacter = character; lane.tiltCharged = true;
            lane.previousX = { -.03125, .0625 }; lane.previousH = { .125, -.25 }; lane.previousU = { -.5, -.625 };
            lane.previousDifference = { .0625, -.125 }; lane.dc = { -.25, .375 };
            lane.lowShelf.z1 = { .0001, -.0002 }; lane.lowShelf.z2 = { -.0003, .0004 };
            lane.highShelf.z1 = { -.0005, .0006 }; lane.highShelf.z2 = { .0007, -.0008 };
            lane.lowShelf.z1Low = { -0., .0001 }; lane.lowShelf.z2Low = { -.0002, 0. };
            lane.highShelf.z1Low = { .0003, -0. }; lane.highShelf.z2Low = { 0., -.0004 };
            lane.primitiveKind = { PrimitiveKind(kinds / 3), PrimitiveKind(kinds % 3) };
            for (std::size_t ch = 0; ch < 2; ++ch) lane.previousPrimitive[ch] = curvePrimitive(r.primitiveCurve, character, lane.previousU[ch]);
            return lane;
        };
        {
            const auto savedProfile = optimizationProfile; const bool savedSampling = optimizationSampling;
            optimizationProfile = {}; optimizationSampling = false;
            OptimizationProfile expectedProfile;
            std::array<std::array<bool, 16>, 4> reachedRows {};
            std::size_t rowCases = 0, oracleFailures = 0, parityFailures = 0, observationFailures = 0;
            const auto rowCheck = [&](bool okay, const char* reason, std::size_t& category) {
                check(okay);
                if (!okay) { ++category; if (category < 4) std::cout << "ROW_COUNTER_FAIL reason=" << reason << '\n'; }
            };
            const auto initialLane = [&](SaturationCharacter character) {
                auto lane = chargedLane(character, 8);
                lane.envelope = 0; lane.previousU = { -10., 10. };
                for (std::size_t ch = 0; ch < 2; ++ch)
                    lane.previousPrimitive[ch] = curvePrimitive(r.primitiveCurve, character, lane.previousU[ch]);
                return lane;
            };
            const auto framesFor = [](SaturationCharacter character, unsigned mask) {
                std::array<Candidate::Frame, 4> frames;
                for (unsigned i = 0; i < 4; ++i) for (unsigned half = 0; half < 2; ++half) {
                    const double argument = (32 + 8*i + (half && !(mask & (1u << i)) ? 1 : 0) + (half ? .75 : .25))/32;
                    const double u = character == SaturationCharacter::soft ? argument
                        : character == SaturationCharacter::glow ? argument-.35 : .75+argument/4;
                    const unsigned slot = i+4*half; frames[slot/2][slot%2] = u*R;
                }
                return frames;
            };
            const auto compareRows = [&](SaturationCharacter character, std::array<Candidate::Frame, 4> frames,
                unsigned route, unsigned requestedMask, unsigned requiredLowMask, bool allowTemporal) {
                SaturationLaneSettings setting; setting.character = character;
                auto actual = initialLane(character), expected = actual; auto reference = frames;
                auto previous = actual.previousU; double envelope = actual.envelope;
                std::array<int, 8> cells {}; std::array<double, 8> coordinates {};
                unsigned lowMask = 0; bool eligible = true;
                for (unsigned n = 0; n < 4; ++n) {
                    Candidate::Frame u { 1*frames[n][0]/R, 1*frames[n][1]/R };
                    if (character == SaturationCharacter::dense) {
                        const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
                        const double alpha = peak > envelope ? 1. : 1.;
                        envelope = peak + alpha*(envelope-peak);
                        const double reduction = std::clamp(6*std::max(envelope-.5, 0.)/(envelope+.5), 0., 6.);
                        const double sag = r.gainCurve.gain(-reduction); u[0] *= sag; u[1] *= sag;
                    }
                    eligible = (quotientPrimitiveKind(u[0], previous[0]) == PrimitiveKind::curve
                        && quotientPrimitiveKind(u[1], previous[1]) == PrimitiveKind::curve) && eligible;
                    for (unsigned ch = 0; ch < 2; ++ch) {
                        const unsigned slot = n*2+ch;
                        if (character == SaturationCharacter::dense && std::abs(u[ch]) <= .75) {
                            lowMask |= 1u << slot; cells[slot] = -1; continue;
                        }
                        const double argument = character == SaturationCharacter::soft ? u[ch]
                            : character == SaturationCharacter::glow ? u[ch]+.35 : 4*(std::abs(u[ch])-.75);
                        const double v = std::abs(argument);
                        if (!(v < PrimitiveCurve::limit)) { eligible = false; continue; }
                        cells[slot] = int(v*PrimitiveCurve::cellsPerUnit);
                        coordinates[slot] = v-(double(cells[slot])+.5)/PrimitiveCurve::cellsPerUnit;
                    }
                    previous = u;
                }
                unsigned actualMask = 0;
                for (unsigned i = 0; i < 4; ++i) if (cells[i] == cells[i+4]) {
                    actualMask |= 1u << i;
                    if (cells[i] >= 0 && route == 0)
                        rowCheck(coordinates[i] != coordinates[i+4], "matched_rows_have_distinct_coordinates", oracleFailures);
                }
                rowCheck(lowMask == requiredLowMask, "actual_low_mask", oracleFailures);
                rowCheck(eligible == (route != 2), "actual_original_curve_eligibility", oracleFailures);
                if (route == 0 && lowMask != 255) {
                    rowCheck(actualMask == requestedMask, "actual_transformed_cells_reach_requested_mask", oracleFailures);
                    const unsigned group = character == SaturationCharacter::dense ? (lowMask ? 3u : 2u) : unsigned(character);
                    rowCheck(character != SaturationCharacter::dense || (group == 2 ? lowMask == 0 : lowMask == 0x11 || lowMask == 0x01), "actual_polynomial_route", oracleFailures);
                    ++expectedProfile.coefficientRows[group][actualMask]; reachedRows[group][actualMask] = true;
                }
                if (route == 0) { ++expectedProfile.temporalEntries[unsigned(character)]; if (lowMask == 255) ++expectedProfile.denseLowEntries; }
                else if (route == 1) ++expectedProfile.legacyEntries[unsigned(character)];
                else ++expectedProfile.preparedFallbacks[unsigned(character)];
                optimizationSampling = true;
                actual.process4(frames, setting, coefficients, coefficients, 1, .73, .89, .997, 1, 1, r.primitiveCurve, r.gainCurve, allowTemporal);
                optimizationSampling = false;
                expected.process4(reference, setting, coefficients, coefficients, 1, .73, .89, .997, 1, 1, r.primitiveCurve, r.gainCurve, false);
                rowCheck(sameFrame(frames, reference) && sameLane(actual, expected), "generic_output_and_full_lane_state", parityFailures);
                for (unsigned continuation = 0; continuation < 2; ++continuation) {
                    std::array<Candidate::Frame, 4> future {{ { -.0, .03125 }, { -.0625, .046875 }, { .015625, -.078125 }, { .09375, -.109375 } }};
                    auto expectedFuture = future;
                    actual.process4(future, setting, coefficients, coefficients, 1, .73, .89, .997, 1, 1, r.primitiveCurve, r.gainCurve, true);
                    expected.process4(expectedFuture, setting, coefficients, coefficients, 1, .73, .89, .997, 1, 1, r.primitiveCurve, r.gainCurve, false);
                    rowCheck(sameFrame(future, expectedFuture) && sameLane(actual, expected), "independent_future_output_and_full_lane_state", parityFailures);
                }
                ++rowCases;
            };
            for (auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense })
                for (unsigned mask = 0; mask < 16; ++mask) compareRows(character, framesFor(character, mask), 0, mask, 0, true);
            for (unsigned match0 = 0; match0 < 2; ++match0) for (unsigned highMask = 0; highMask < 8; ++highMask) {
                const unsigned mask = match0 | (highMask << 1);
                auto frames = framesFor(SaturationCharacter::dense, mask);
                frames[0][0] = .125*R; if (match0) frames[2][0] = .25*R;
                compareRows(SaturationCharacter::dense, frames, 0, mask, match0 ? 0x11 : 0x01, true);
            }
            std::array<Candidate::Frame, 4> lowFrames {{ { .125*R, .25*R }, { .25*R, .125*R }, { .125*R, .25*R }, { .25*R, .125*R } }};
            compareRows(SaturationCharacter::dense, lowFrames, 0, 0, 255, true);
            for (auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
                compareRows(character, framesFor(character, 15), 1, 0, 0, false);
                auto fallback = framesFor(character, 15); fallback[0] = { -10.*R, 10.*R };
                compareRows(character, fallback, 2, 0, 0, true);
            }
            const auto observed = optimizationProfile;
            for (unsigned group = 0; group < 4; ++group) for (unsigned mask = 0; mask < 16; ++mask) {
                rowCheck(reachedRows[group][mask], "all_actual_row_buckets_reached", oracleFailures);
                rowCheck(observed.coefficientRows[group][mask] == expectedProfile.coefficientRows[group][mask], "sampled_row_bucket_observation", observationFailures);
            }
            for (unsigned character = 0; character < 3; ++character) {
                rowCheck(observed.temporalEntries[character] == expectedProfile.temporalEntries[character], "sampled_temporal_observation", observationFailures);
                rowCheck(observed.preparedFallbacks[character] == expectedProfile.preparedFallbacks[character], "sampled_prepared_fallback_observation", observationFailures);
                rowCheck(observed.legacyEntries[character] == expectedProfile.legacyEntries[character], "sampled_legacy_observation", observationFailures);
            }
            rowCheck(observed.denseLowEntries == expectedProfile.denseLowEntries, "sampled_dense_low_observation", observationFailures);
            rowCheck(rowCases == 71, "bounded_case_count", oracleFailures);
            std::cout << "ROW_COUNTER_SUMMARY cases=" << rowCases << " polynomial_entries=64 dense_low_entries=1 prepared_fallbacks=3 legacy_entries=3"
                << " oracle_failures=" << oracleFailures << " parity_failures=" << parityFailures << " observation_failures=" << observationFailures
                << " status=" << (oracleFailures || parityFailures || observationFailures ? "FAIL" : "PASS")
                << " reason=" << (oracleFailures ? "ORACLE" : parityFailures ? "PARITY" : observationFailures ? "MISSING_SAMPLED_OBSERVATIONS" : "EXACT_SAMPLED_OBSERVATIONS") << '\n';
            optimizationProfile = savedProfile; optimizationSampling = savedSampling;
        }
        const Coefficients chargedLow { .875, .0625, -.03125, -.25, .125 }, chargedHigh { 1.125, -.125, .0625, -.375, .0625 };
        const Coefficients* caseLow = &coefficients; const Coefficients* caseHigh = &coefficients;
        double attack = 1, release = 1; bool allow = true; int rounding = FE_TONEAREST, prefilled = -1;
        std::size_t temporalCases = 0, temporalFutureFrames = 0, temporalEnvironmentCases = 0, temporalMismatches = 0;
        std::size_t lastMixedBlocks = 0, lastPreparedFallbacks = 0, lastLegacy = 0;
        unsigned lastMixedMask = 0;
        std::array<std::size_t, 3> temporalInitialBlocks {};
        const auto compareTemporal = [&](const Lane& initial, std::array<Candidate::Frame, 4> frames, const SaturationLaneSettings& setting) {
            Lane actual = initial, expected = initial; auto reference = frames;
            const auto observe = [&](Lane& lane, auto& block, bool wide) {
                if (prefilled >= 0) { check(std::fesetround(rounding) == 0); check(std::feclearexcept(FE_ALL_EXCEPT) == 0); if (prefilled) check(std::feraiseexcept(prefilled) == 0); }
                lane.process4(block, setting, *caseLow, *caseHigh, 1, .73, .89, .997, attack, release, r.primitiveCurve, r.gainCurve, wide);
                return std::array { std::fetestexcept(FE_ALL_EXCEPT), std::fegetround() };
            };
            const auto beforeBlocks = optimizationTemporalBlocks;
            const auto beforeMixed = optimizationTemporalMixedBlocks, beforeFallbacks = optimizationTemporalPreparedFallbacks, beforeLegacy = optimizationTemporalLegacy;
            const auto actualEnvironment = observe(actual, frames, allow);
            const auto initialBlocks = optimizationTemporalBlocks - beforeBlocks;
            lastMixedBlocks = optimizationTemporalMixedBlocks - beforeMixed;
            lastPreparedFallbacks = optimizationTemporalPreparedFallbacks - beforeFallbacks;
            lastLegacy = optimizationTemporalLegacy - beforeLegacy;
            lastMixedMask = optimizationTemporalLastMixedMask;
            temporalInitialBlocks[unsigned(setting.character)] += initialBlocks;
            if (prefilled >= 0) check(initialBlocks == 1);
            const auto expectedEnvironment = observe(expected, reference, false);
            const bool same = sameFrame(frames, reference) && sameLane(actual, expected)
                && std::bit_cast<std::uint64_t>(actual.bypassWet) == std::bit_cast<std::uint64_t>(expected.bypassWet);
            check(same); if (!same && temporalMismatches++ < 12) {
                std::cout << "TEMPORAL_MISMATCH case=" << temporalCases << " character=" << int(setting.character) << " rounding=" << rounding << " prefilled=" << prefilled << '\n';
                for (std::size_t n = 0; n < 4; ++n) for (std::size_t ch = 0; ch < 2; ++ch)
                    std::cout << "TEMPORAL_WORD " << n << ',' << ch << ',' << std::hex << std::bit_cast<std::uint64_t>(frames[n][ch]) << ',' << std::bit_cast<std::uint64_t>(reference[n][ch]) << std::dec << '\n';
            }
            if (prefilled >= 0) { check(actualEnvironment == expectedEnvironment); ++temporalEnvironmentCases; }
            for (unsigned continuation = 0; continuation < 2; ++continuation) {
                std::array<Candidate::Frame, 4> future {{ { -.0, .03125 }, { -.0625, .046875 }, { .015625, -.078125 }, { .09375, -.109375 } }};
                auto expectedFuture = future;
                const auto a = observe(actual, future, allow), e = observe(expected, expectedFuture, false);
                check(sameFrame(future, expectedFuture) && sameLane(actual, expected));
                if (prefilled >= 0) check(a == e);
                temporalFutureFrames += 4;
            }
            ++temporalCases;
            return initialBlocks;
        };
        std::fenv_t temporalSavedEnvironment; check(std::fegetenv(&temporalSavedEnvironment) == 0);
        optimizationTemporalBlocks = optimizationTemporalHornerChains = optimizationTemporalLowBlocks = 0;
        optimizationTemporalPreparedFallbacks = optimizationTemporalLegacy = 0;
        {
            SaturationLaneSettings setting; setting.character = SaturationCharacter::dense;
            std::size_t mixedCases = 0, mixedEntries = 0, mixedFlags = 0, oldLow = 0, oldHigh = 0;
            std::array<bool, 256> reachedMasks {};
            std::array<bool, PrimitiveCurve::cells> reachedHighCells {};
            std::size_t exactMappings = 0, collapsedMappings = 0;
            const auto classify = [&](Lane lane, const std::array<Candidate::Frame, 4>& frames) {
                const bool prepared = allow && finite(lane.previousX) && finite(lane.previousH) && finite(lane.previousU)
                    && finite(lane.previousPrimitive) && finite(lane.previousDifference) && finite(lane.dc)
                    && finite(lane.lowShelf.z1) && finite(lane.lowShelf.z2) && finite(lane.highShelf.z1) && finite(lane.highShelf.z2)
                    && (std::bit_cast<std::uint64_t>(lane.envelope) & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL;
                if (!prepared) return std::array<unsigned, 3> { 0, 0, 0 };
                if (setting.preTiltDb == 0 && lane.tiltCharged) { lane.lowShelf = {}; lane.highShelf = {}; }
                bool eligible = true; unsigned mask = 0;
                for (std::size_t n = 0; n < 4; ++n) {
                    auto h = frames[n];
                    if (setting.preTiltDb != 0) h = lane.highShelf.process(lane.lowShelf.process(h, *caseLow), *caseHigh);
                    Candidate::Frame u { 1*h[0]/R, 1*h[1]/R };
                    const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
                    const double alpha = peak > lane.envelope ? attack : release;
                    lane.envelope = peak + alpha*(lane.envelope-peak);
                    const double reduction = std::clamp(6*std::max(lane.envelope-.5, 0.)/(lane.envelope+.5), 0., 6.);
                    const double sag = r.gainCurve.gain(-reduction); u[0] *= sag; u[1] *= sag;
                    eligible = (quotientPrimitiveKind(u[0], lane.previousU[0]) == PrimitiveKind::curve
                        && quotientPrimitiveKind(u[1], lane.previousU[1]) == PrimitiveKind::curve) && eligible;
                    for (std::size_t ch = 0; ch < 2; ++ch) {
                        const double absolute = std::abs(u[ch]);
                        if (absolute <= .75) mask |= 1u << (n*2+ch);
                        else if (!(4*(absolute-.75) < PrimitiveCurve::limit)) eligible = false;
                    }
                    lane.previousU = u;
                }
                return std::array<unsigned, 3> { mask, unsigned(eligible), 1 };
            };
            const auto compareMixed = [&](const Lane& initial, const std::array<Candidate::Frame, 4>& frames) {
                if (prefilled >= 0) check(std::fesetround(rounding) == 0);
                const auto route = classify(initial, frames);
                const auto blocks = compareTemporal(initial, frames, setting);
                const bool mixed = route[0] != 0 && route[0] != 255;
                check((blocks == 1) == bool(route[1]));
                check(lastMixedBlocks == std::size_t(route[1] && mixed));
                check(lastPreparedFallbacks == std::size_t(route[2] && !route[1]));
                check(lastLegacy == std::size_t(!route[2]));
                if (route[1]) {
                    if (mixed) { check(lastMixedMask == route[0]); ++mixedEntries; }
                    else if (route[0] == 255) ++oldLow;
                    else ++oldHigh;
                }
                ++mixedCases;
                if (prefilled >= 0) ++mixedFlags;
                return route;
            };
            const auto maskFrames = [](unsigned mask, double leftSign, double rightSign) {
                std::array<Candidate::Frame, 4> frames;
                for (std::size_t n = 0; n < 4; ++n) for (std::size_t ch = 0; ch < 2; ++ch) {
                    const auto i = n*2+ch;
                    const double value = mask & (1u << i) ? .125+double(i)/32 : 1.+double(i)/8;
                    frames[n][ch] = value*(ch ? rightSign : leftSign)*R;
                }
                return frames;
            };
            auto witness = chargedLane(SaturationCharacter::dense, 8);
            const auto witnessBlocks = compareTemporal(witness, maskFrames(0x69, 1., -1.), setting);
            std::cout << "DENSE_MIXED_COST blocks=" << witnessBlocks << " mixed_blocks=" << lastMixedBlocks
                << " prepared_fallbacks=" << lastPreparedFallbacks << " state_mismatches=" << temporalMismatches << '\n';
            check(witnessBlocks == 1 && lastMixedBlocks == 1 && lastPreparedFallbacks == 0);
            for (unsigned mask = 0; mask < 256; ++mask)
                for (double leftSign : { -1., 1. }) for (double rightSign : { -1., 1. })
                for (unsigned kinds = 0; kinds < 9; ++kinds) {
                    auto initial = chargedLane(SaturationCharacter::dense, kinds);
                    initial.previousU = { -20., 20. };
                    if (kinds%3 == 0) initial.primitiveCharacter = SaturationCharacter::glow;
                    const auto route = compareMixed(initial, maskFrames(mask, leftSign, rightSign));
                    check(route[0] == mask && route[1] == 1);
                    reachedMasks[route[0]] = true;
                }
            check(std::all_of(reachedMasks.begin(), reachedMasks.end(), [](bool value) { return value; }));
            for (int mode : { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO })
                for (int flags : { 0, FE_INVALID, FE_DIVBYZERO | FE_UNDERFLOW }) {
                    rounding = mode; prefilled = flags;
                    for (unsigned mask = 1; mask < 255; ++mask) {
                        auto initial = chargedLane(SaturationCharacter::dense, mask%9);
                        compareMixed(initial, maskFrames(mask, mask&1 ? -1. : 1., mask&2 ? -1. : 1.));
                    }
                }
            prefilled = -1; check(std::fesetenv(&temporalSavedEnvironment) == 0);
            for (std::size_t cell = 0; cell < PrimitiveCurve::cells; ++cell)
                for (double edge : { double(cell)/32, (double(cell)+.5)/32, double(cell+1)/32 })
                for (double target : { std::nextafter(edge, -infinity), edge, std::nextafter(edge, infinity) })
                for (double leftSign : { -1., 1. }) {
                    auto frames = maskFrames(0xaa, leftSign, -leftSign);
                    frames[0][0] = leftSign*(.75+target/4)*R;
                    const double actualU = 1*frames[0][0]/R;
                    const double actualArgument = 4*(std::abs(actualU)-.75);
                    if (std::bit_cast<std::uint64_t>(actualArgument) == std::bit_cast<std::uint64_t>(target)) ++exactMappings;
                    else ++collapsedMappings;
                    const auto route = compareMixed(chargedLane(SaturationCharacter::dense, unsigned(cell%9)), frames);
                    if (route[1] && std::abs(actualU) > .75) {
                        check(actualArgument < PrimitiveCurve::limit);
                        const int actualCell = int(actualArgument*PrimitiveCurve::cellsPerUnit);
                        check(actualCell >= 0 && actualCell < int(PrimitiveCurve::cells));
                        reachedHighCells[std::size_t(actualCell)] = true;
                    }
                }
            check(std::all_of(reachedHighCells.begin(), reachedHighCells.end(), [](bool value) { return value; }));
            for (double value : { 0., -0., std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min(),
                    std::numeric_limits<double>::min(), .75, -.75, std::nextafter(.75, 0.), std::nextafter(.75, infinity),
                    std::nextafter(4.75, 0.), 4.75, std::nextafter(4.75, infinity), std::numeric_limits<double>::max(),
                    infinity, invalid, std::bit_cast<double>(0x7ff0000000000123ULL) }) {
                auto frames = maskFrames(0x69, 1., -1.); frames[0] = { value*R, -.25*R };
                compareMixed(chargedLane(SaturationCharacter::dense, 8), frames);
            }
            caseLow = &chargedLow; caseHigh = &chargedHigh; attack = .25; release = .75; setting.preTiltDb = 2;
            for (unsigned mask = 0; mask < 256; ++mask) for (unsigned kinds = 0; kinds < 9; ++kinds) {
                auto initial = chargedLane(SaturationCharacter::dense, kinds); initial.envelope = 1.125;
                initial.bypassWet = .625; setting.mix = .37f;
                compareMixed(initial, maskFrames(mask, mask&1 ? -1. : 1., mask&2 ? -1. : 1.));
            }
            std::size_t nonfiniteEnvironmentCases = 0;
            for (int mode : { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO })
                for (int flags : { 0, FE_INVALID, FE_DIVBYZERO | FE_UNDERFLOW }) {
                    rounding = mode; prefilled = flags;
                    auto initial = chargedLane(SaturationCharacter::dense, 5); initial.envelope = 1.125;
                    const auto route = compareMixed(initial, maskFrames(0x69, 1., -1.));
                    check(route[1] && route[0] != 0 && route[0] != 255 && lastMixedBlocks == 1);
                    for (double poison : { invalid, std::bit_cast<double>(0x7ff0000000000123ULL), infinity }) {
                        auto actual = initial, expected = initial;
                        auto frames = maskFrames(0x69, 1., -1.); frames[0][0] = poison;
                        auto reference = frames;
                        const auto observe = [&](Lane& lane, auto& block, bool wide) {
                            check(std::fesetround(mode) == 0); check(std::feclearexcept(FE_ALL_EXCEPT) == 0);
                            if (flags) check(std::feraiseexcept(flags) == 0);
                            lane.process4(block, setting, *caseLow, *caseHigh, 1, .73, .89, .997, attack, release, r.primitiveCurve, r.gainCurve, wide);
                            return std::array { std::fetestexcept(FE_ALL_EXCEPT), std::fegetround() };
                        };
                        const auto a = observe(actual, frames, true), e = observe(expected, reference, false);
                        check(a == e && sameFrame(frames, reference) && sameLane(actual, expected));
                        ++nonfiniteEnvironmentCases;
                    }
                }
            prefilled = -1; check(std::fesetenv(&temporalSavedEnvironment) == 0);
            allow = false; compareMixed(witness, maskFrames(0x69, 1., -1.)); allow = true;
            witness.previousPrimitive[0] = std::bit_cast<double>(0x7ff8000000000123ULL);
            compareMixed(witness, maskFrames(0x69, 1., -1.));
            caseLow = &coefficients; caseHigh = &coefficients; attack = release = 1;
            std::cout << "DENSE_MIXED_COMPLETE cases=" << mixedCases << " eligible_mixed=" << mixedEntries
                << " environment_cases=" << mixedFlags << " masks=" << std::count(reachedMasks.begin(), reachedMasks.end(), true)
                << " high_cells=" << std::count(reachedHighCells.begin(), reachedHighCells.end(), true)
                << " exact_mappings=" << exactMappings << " collapsed_mappings=" << collapsedMappings
                << " homogeneous_low=" << oldLow << " homogeneous_high=" << oldHigh
                << " nonfinite_environment_cases=" << nonfiniteEnvironmentCases << '\n';
        }
        for (auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
            SaturationLaneSettings setting; setting.character = character;
            std::array<bool, PrimitiveCurve::cells> reachedCells {};
            std::size_t exactArgumentMappings = 0, collapsedArgumentMappings = 0, eligibleCellBatches = 0;
            std::size_t oldDenseLowBatches = 0, oldDenseHighBatches = 0, newDenseMixedBatches = 0;
            for (std::size_t cell = 0; cell < PrimitiveCurve::cells; ++cell)
                for (double edge : { double(cell)/32, (double(cell)+.5)/32, double(cell+1)/32 })
                for (double value : { std::nextafter(edge, -infinity), edge, std::nextafter(edge, infinity) })
                for (double leftSign : { -1., 1. }) for (double rightSign : { -1., 1. }) {
                    const auto transformed = [&](double v, double sign) {
                        if (character == SaturationCharacter::glow) return sign*v - .35;
                        if (character == SaturationCharacter::dense) return sign*(.75 + v/4);
                        return sign*v;
                    };
                    const double left = transformed(value, leftSign), right = transformed((double(511-cell)+.5)/32, rightSign);
                    auto initial = chargedLane(character, unsigned(cell%9));
                    initial.previousU = { cell%2 ? -20. : .10002, cell%3 ? 20. : -.10002 };
                    for (std::size_t ch = 0; ch < 2; ++ch) initial.previousPrimitive[ch] = curvePrimitive(r.primitiveCurve, character, initial.previousU[ch]);
                    if (cell%5 == 0) initial.primitiveCharacter = SaturationCharacter((unsigned(character)+1)%3);
                    const bool dense = character == SaturationCharacter::dense;
                    const std::array<Candidate::Frame, 4> inputs {{{ left*R, right*R }, { (dense ? -1.5 : -.5)*R, 1.25*R }, { 1.75*R, -2.25*R }, { (dense ? -3. : -.25)*R, (dense ? .875 : .5)*R }}};
                    const auto blocks = compareTemporal(initial, inputs, setting);
                    auto priorU = initial.previousU; bool admitted = true, allLow = true, allHigh = true;
                    Candidate::Frame firstU;
                    for (std::size_t n = 0; n < 4; ++n) {
                        Candidate::Frame u { 1*inputs[n][0]/R, 1*inputs[n][1]/R };
                        if (dense) {
                            const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
                            const double envelope = peak + 1*(0.-peak);
                            check(envelope == 0 && r.gainCurve.gain(-std::clamp(6*std::max(envelope-.5,0.)/(envelope+.5),0.,6.)) == 1);
                        }
                        if (n == 0) firstU = u;
                        admitted = (quotientPrimitiveKind(u[0], priorU[0]) == PrimitiveKind::curve
                            && quotientPrimitiveKind(u[1], priorU[1]) == PrimitiveKind::curve) && admitted;
                        for (double current : u) {
                            allLow = (std::abs(current) <= .75) && allLow; allHigh = (std::abs(current) > .75) && allHigh;
                            if (!dense || std::abs(current) > .75) {
                                const double argument = character == SaturationCharacter::soft ? current
                                    : character == SaturationCharacter::glow ? current+.35 : 4*(std::abs(current)-.75);
                                admitted = (std::abs(argument) < PrimitiveCurve::limit) && admitted;
                            }
                        }
                        priorU = u;
                    }
                    if (dense) {
                        if (allLow) { ++oldDenseLowBatches; check(lastMixedBlocks == 0); }
                        else if (allHigh) { ++oldDenseHighBatches; check(lastMixedBlocks == 0); }
                        else { ++newDenseMixedBatches; check(lastMixedBlocks == std::size_t(admitted)); }
                    }
                    check((blocks == 1) == admitted);
                    const double actualArgument = character == SaturationCharacter::soft ? firstU[0]
                        : character == SaturationCharacter::glow ? firstU[0]+.35 : 4*(std::abs(firstU[0])-.75);
                    const double targetArgument = dense ? value : leftSign*value;
                    if (std::bit_cast<std::uint64_t>(actualArgument) == std::bit_cast<std::uint64_t>(targetArgument)) ++exactArgumentMappings;
                    else ++collapsedArgumentMappings;
                    if (blocks) {
                        ++eligibleCellBatches;
                        const double v = std::abs(actualArgument); check(v < PrimitiveCurve::limit);
                        const int actualCell = int(v*PrimitiveCurve::cellsPerUnit); check(actualCell >= 0 && actualCell < int(PrimitiveCurve::cells));
                        reachedCells[std::size_t(actualCell)] = true;
                    }
                }
            check(std::all_of(reachedCells.begin(), reachedCells.end(), [](bool reached) { return reached; }));
            std::cout << "TEMPORAL_CELL_ROUTE character=" << int(character) << " reached_cells=" << std::count(reachedCells.begin(), reachedCells.end(), true)
                << " eligible_batches=" << eligibleCellBatches << " exact_argument_mappings=" << exactArgumentMappings << " collapsed_argument_mappings=" << collapsedArgumentMappings << '\n';
            if (character == SaturationCharacter::dense) std::cout << "DENSE_ORIGINAL_ROUTE_CATEGORIES homogeneous_low=" << oldDenseLowBatches
                << " homogeneous_high=" << oldDenseHighBatches << " mixed=" << newDenseMixedBatches << '\n';
            const std::array values { 0., -0., std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min(),
                std::numeric_limits<double>::min(), -std::numeric_limits<double>::min(), .1, .10000001, .10002, .35, -.35,
                .75, -.75, std::nextafter(.75, 0.), std::nextafter(.75, infinity), 4.75, -4.75, 15.65, -16.35, 16., -16.,
                std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), infinity, -infinity, invalid, -invalid,
                std::bit_cast<double>(0x7ff0000000000123ULL), std::bit_cast<double>(0xfff8000000000456ULL) };
            for (unsigned kinds = 0; kinds < 9; ++kinds) for (double left : values) for (double right : values) {
                auto initial = chargedLane(character, kinds);
                compareTemporal(initial, {{{ left*R, right*R }, { .1*R, -.1*R }, { .10002*R, -.10000001*R }, { .5*R, -.75*R }}}, setting);
            }
            for (double value : { std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(), std::bit_cast<double>(0x7ff0000000000123ULL) })
                compareTemporal(chargedLane(character, 8), {{{ value, -value }, { .125, -.25 }, { -.0, 0. }, { .25, -.125 }}}, setting);
            for (double bad : values) for (unsigned field = 0; field < 11; ++field) {
                auto initial = chargedLane(character, field%9);
                const std::array histories { &initial.previousX, &initial.previousH, &initial.previousU, &initial.previousPrimitive,
                    &initial.previousDifference, &initial.dc, &initial.lowShelf.z1, &initial.lowShelf.z2, &initial.highShelf.z1, &initial.highShelf.z2 };
                if (field < histories.size()) (*histories[field])[field%2] = bad; else initial.envelope = bad;
                setting.preTiltDb = 2;
                compareTemporal(initial, {{{ 1.5*R, 2.*R }, { 2.*R, 2.5*R }, { 2.5*R, 3.*R }, { 3.*R, 3.5*R }}}, setting);
                setting.preTiltDb = 0;
            }
            caseLow = &chargedLow; caseHigh = &chargedHigh; attack = .25; release = .75;
            for (unsigned kinds = 0; kinds < 9; ++kinds) for (bool supported : { false, true }) for (bool bypass : { false, true }) {
                allow = supported; auto initial = chargedLane(character, kinds); initial.envelope = 1.125;
                initial.bypassWet = bypass ? 0. : .625; setting.preTiltDb = 2; setting.mix = bypass ? 0.f : .37f;
                compareTemporal(initial, {{{ 1.5*R, 2.*R }, { 2.*R, 2.5*R }, { 2.5*R, 3.*R }, { 3.*R, 3.5*R }}}, setting);
            }
            allow = true; caseLow = &coefficients; caseHigh = &coefficients; attack = release = 1;
            setting.preTiltDb = 0; setting.mix = 1;
            for (int mode : { FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO }) for (int flags : { 0, FE_INVALID, FE_DIVBYZERO | FE_UNDERFLOW }) {
                rounding = mode; prefilled = flags;
                for (unsigned kinds = 0; kinds < 9; ++kinds) {
                    auto initial = chargedLane(character, kinds);
                    compareTemporal(initial, {{{ 1.5*R, 2.*R }, { 2.*R, 2.5*R }, { 2.5*R, 3.*R }, { 3.*R, 3.5*R }}}, setting);
                    compareTemporal(initial, {{{ .1*R, -.2*R }, { .2*R, -.3*R }, { .3*R, -.4*R }, { .4*R, -.5*R }}}, setting);
                }
            }
            prefilled = -1; check(std::fesetenv(&temporalSavedEnvironment) == 0);
        }
        check(std::fesetenv(&temporalSavedEnvironment) == 0);
        check(optimizationTemporalBlocks > 0 && optimizationTemporalLowBlocks > 0 && optimizationTemporalPreparedFallbacks > 0 && optimizationTemporalLegacy > 0);
        std::cout << "TEMPORAL_COMPLETE cases=" << temporalCases << " future_frames=" << temporalFutureFrames << " environment_cases=" << temporalEnvironmentCases
            << " blocks=" << optimizationTemporalBlocks << " low_blocks=" << optimizationTemporalLowBlocks << " ymm_horner_chains=" << optimizationTemporalHornerChains
            << " prepared_fallbacks=" << optimizationTemporalPreparedFallbacks << " legacy=" << optimizationTemporalLegacy << " mismatches=" << temporalMismatches << " failures=" << failures << '\n';
        std::cout << "TEMPORAL_INITIAL_BLOCKS soft=" << temporalInitialBlocks[0] << " glow=" << temporalInitialBlocks[1] << " dense=" << temporalInitialBlocks[2] << '\n';
    }
#endif
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    for (const auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
        SaturationLaneSettings setting; setting.character = character;
        Lane lane; lane.primitiveCharacter = character; lane.previousU = { .5, 1.5 };
        lane.primitiveKind = { PrimitiveKind::curve, PrimitiveKind::curve };
        for (std::size_t ch = 0; ch < 2; ++ch)
            lane.previousPrimitive[ch] = curvePrimitive(r.primitiveCurve, character, lane.previousU[ch]);
        Lane reference = lane;
        std::array<std::array<Candidate::Frame, 4>, 16> outputs;
        std::array<Lane, 16> states;
        optimizationScalarCurveCalls = optimizationPairCurveCalls = 0;
        optimizationSelectorCalls = optimizationStereoFrameCalls = 0;
        for (unsigned repeat = 0; repeat < 16; ++repeat) {
            auto& frames = outputs[repeat];
            auto previous = lane.previousU;
            for (unsigned n = 0; n < 4; ++n) {
                const Candidate::Frame u { 1. + .5 * n, 2. + .5 * n };
                for (std::size_t ch = 0; ch < 2; ++ch) {
                    check(quotientPrimitiveKind(u[ch], previous[ch]) == PrimitiveKind::curve);
                    check(std::abs(u[ch] - previous[ch]) >= 1e-6 * (1 + std::max(std::abs(u[ch]), std::abs(previous[ch]))));
                }
                frames[n] = { u[0] * R, u[1] * R }; previous = u;
            }
            lane.process4(frames, setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve);
            check(lane.primitiveKind == std::array { PrimitiveKind::curve, PrimitiveKind::curve });
            states[repeat] = lane;
        }
        std::cout << "CURVE_WORKCOUNT character=" << int(character) << " batches=16 scalar_horner="
            << optimizationScalarCurveCalls << " pair_horner=" << optimizationPairCurveCalls
            << " selector_calls=" << optimizationSelectorCalls << " complete_stereo_frames=" << optimizationStereoFrameCalls << '\n';
        check(optimizationScalarCurveCalls == 0 && optimizationPairCurveCalls == 16 * 4);
        check(optimizationSelectorCalls == 16 * 4 * 4);
        check(optimizationStereoFrameCalls == 16 * 4);
        for (unsigned repeat = 0; repeat < 16; ++repeat) {
            for (unsigned n = 0; n < 4; ++n)
                check(sameFrame(outputs[repeat][n], scalar(reference, { (1. + .5 * n) * R, (2. + .5 * n) * R }, setting)));
            check(sameLane(states[repeat], reference));
        }
        for (const Candidate::Frame input : { Candidate::Frame { -0., 0. }, Candidate::Frame { -.75 * R, 16. * R } })
            check(sameFrame(scalar(lane, input, setting), scalar(reference, input, setting)) && sameLane(lane, reference));
        check(sameLane(lane, reference));
    }
    std::size_t pairWords = 0;
    const auto words = [&](Candidate::Frame input, Candidate::Frame actual, Candidate::Frame expected, int character) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const auto a = std::bit_cast<std::uint64_t>(actual[ch]), b = std::bit_cast<std::uint64_t>(expected[ch]);
            ++pairWords; check(a == b);
            if (a != b) std::cout << "PAIR_MISMATCH character=" << character << " channel=" << ch
                << " input_words=" << std::bit_cast<std::uint64_t>(input[0]) << ',' << std::bit_cast<std::uint64_t>(input[1])
                << " actual=" << a << " expected=" << b << '\n';
        }
    };
    const auto comparePair = [&](Candidate::Frame input) {
        words(input, r.primitiveCurve.evaluatePair(input),
            { r.primitiveCurve.evaluate(input[0]), r.primitiveCurve.evaluate(input[1]) }, -1);
        words(input, curvePrimitivePair<SaturationCharacter::soft>(r.primitiveCurve, input),
            { curvePrimitive(r.primitiveCurve, SaturationCharacter::soft, input[0]), curvePrimitive(r.primitiveCurve, SaturationCharacter::soft, input[1]) }, 0);
        words(input, curvePrimitivePair<SaturationCharacter::glow>(r.primitiveCurve, input),
            { curvePrimitive(r.primitiveCurve, SaturationCharacter::glow, input[0]), curvePrimitive(r.primitiveCurve, SaturationCharacter::glow, input[1]) }, 1);
        words(input, curvePrimitivePair<SaturationCharacter::dense>(r.primitiveCurve, input),
            { curvePrimitive(r.primitiveCurve, SaturationCharacter::dense, input[0]), curvePrimitive(r.primitiveCurve, SaturationCharacter::dense, input[1]) }, 2);
    };
    for (std::size_t cell = 0; cell < PrimitiveCurve::cells; ++cell) {
        const double other = (double(PrimitiveCurve::cells - 1 - cell) + .5) / PrimitiveCurve::cellsPerUnit;
        for (double point : { double(cell) / PrimitiveCurve::cellsPerUnit, (double(cell) + .5) / PrimitiveCurve::cellsPerUnit,
                             double(cell + 1) / PrimitiveCurve::cellsPerUnit })
            for (double value : { std::nextafter(point, -infinity), point, std::nextafter(point, infinity) })
                for (double sign0 : { -1., 1. }) for (double sign1 : { -1., 1. }) {
                    comparePair({ sign0 * value, sign1 * other }); comparePair({ sign1 * other, sign0 * value });
                }
    }
    const std::array special { 0., -0., std::numeric_limits<double>::denorm_min(), -std::numeric_limits<double>::denorm_min(),
        .35, -.35, std::nextafter(.35, 0.), std::nextafter(.35, infinity),
        .75, -.75, std::nextafter(.75, 0.), std::nextafter(.75, infinity),
        4.75, -4.75, std::nextafter(4.75, 0.), std::nextafter(4.75, infinity),
        15.65, -16.35, std::nextafter(15.65, 0.), std::nextafter(15.65, infinity),
        16., -16., std::nextafter(16., 0.), std::nextafter(16., infinity),
        infinity, -infinity, invalid, -invalid, std::numeric_limits<double>::max(), -std::numeric_limits<double>::max() };
    for (double left : special) for (double right : special) comparePair({ left, right });
    std::cout << "PAIR_WORDS checked=" << pairWords << " failures=" << failures << '\n';
#endif
    for (const auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
        SaturationLaneSettings setting; setting.character = character;
        for (bool batched : { false, true }) {
            Lane lane; lane.primitiveCharacter = character; lane.previousU = { .10002, -.10002 };
            optimizationPrimitiveCalls = 0;
            for (unsigned repeat = 0; repeat < 16; ++repeat) {
                std::array<Candidate::Frame, 4> frames;
                for (unsigned n = 0; n < 4; ++n) {
                    const double u = n % 2 ? .10002 : .1;
                    check(quotientPrimitiveKind(u, n % 2 ? .1 : .10002) == PrimitiveKind::exact);
                    frames[n] = { u * R, -u * R };
                }
                if (batched) lane.process4(frames, setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve);
                else for (auto input : frames) scalar(lane, input, setting);
            }
            std::cout << "WORKCOUNT character=" << int(character) << " batch=" << batched
                << " exact_primitive_evaluations=" << optimizationPrimitiveCalls << '\n';
            check(optimizationPrimitiveCalls == 0);
        }
    }
    Lane batch, single;
    constexpr std::array levels { 0., 0., 2e-6, 4e-6, .1, .10002, .10002, -.1,
        -.10002, -.10002, -.75, -.75002, .75, .75002, -.75001, .75001,
        1., -1., 8., 8.00002, 16., 16.00002, 32., -32., 0., -0., .35, -.35,
        .03125, .03127, -.03125, -.03127 };
    std::array<std::size_t, 9> transitions {};
    for (const auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense, SaturationCharacter::soft }) {
        SaturationLaneSettings setting; setting.character = character;
        for (std::size_t offset = 0; offset < levels.size(); offset += 4) {
            std::array<Candidate::Frame, 4> frames;
            for (std::size_t n = 0; n < 4; ++n) frames[n] = { levels[offset+n] * R, -.7 * levels[offset+n] * R };
            const auto input = frames;
            batch.process4(frames, setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve);
            for (std::size_t n = 0; n < 4; ++n) {
                const auto previous = single.primitiveKind;
                const auto output = scalar(single, input[n], setting);
                check(std::memcmp(output.data(), frames[n].data(), sizeof(output)) == 0);
                for (std::size_t ch = 0; ch < 2; ++ch) ++transitions[3 * int(previous[ch]) + int(single.primitiveKind[ch])];
            }
            check(sameLane(batch, single));
            const Candidate::Frame continuation { -.03125 * R, .75001 * R };
            Lane batchContinuation = batch, singleContinuation = single;
            check(sameFrame(scalar(batchContinuation, continuation, setting), scalar(singleContinuation, continuation, setting))
                && sameLane(batchContinuation, singleContinuation));
        }
    }
    for (auto count : transitions) check(count != 0);
    std::size_t chargedBatches = 0;
    const auto basis = designBasis(192000, 1000, true);
    const auto low = design(Filter::lowShelf, basis, -3., &r.gainCurve);
    const auto high = design(Filter::highShelf, basis, 3., &r.gainCurve);
    const std::array chargedLevels { 0., -0., .1, .10002, .10002, -.75, -.75002, .75, .75002,
        -.75001, .75001, 1., -1., 15.65, -16.35, 16., -16., 32., -32.,
        std::nextafter(.75, 0.), std::nextafter(.75, infinity), std::nextafter(16., 0.), std::nextafter(16., infinity),
        .03125, .03127, -.03125, -.03127, .35, -.35, 0., -0., 2e-6 };
    for (const auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
        for (unsigned kinds = 0; kinds < 9; ++kinds) {
            Lane actual; actual.primitiveCharacter = character; actual.tiltCharged = true; actual.envelope = 1.125;
            actual.previousX = { -.125, .25 }; actual.previousH = { .375, -.5 }; actual.previousU = { .10002, -.75002 };
            actual.previousDifference = { -.0625, .03125 }; actual.dc = { .25, -.375 };
            actual.lowShelf.z1 = { .0001, -.0002 }; actual.lowShelf.z2 = { -.0003, .0004 };
            actual.highShelf.z1 = { -.0005, .0006 }; actual.highShelf.z2 = { .0007, -.0008 };
            actual.primitiveKind = { PrimitiveKind(kinds / 3), PrimitiveKind(kinds % 3) };
            for (std::size_t ch = 0; ch < 2; ++ch)
                actual.previousPrimitive[ch] = actual.primitiveKind[ch] == PrimitiveKind::curve
                    ? curvePrimitive(r.primitiveCurve, character, actual.previousU[ch]) : (ch ? -.125 : .25);
            Lane expected = actual;
            for (std::size_t offset = 0; offset < chargedLevels.size(); offset += 4) {
                SaturationLaneSettings setting; setting.character = offset < 16 ? character : SaturationCharacter((unsigned(character) + 1) % 3);
                setting.preTiltDb = offset < 8 ? 6.f : offset < 20 ? 0.f : -6.f;
                setting.mix = offset % 8 ? .37f : 0.f; actual.bypassWet = expected.bypassWet = offset % 12 ? .625 : 1.;
                std::array<Candidate::Frame, 4> frames;
                for (std::size_t n = 0; n < 4; ++n)
                    frames[n] = { chargedLevels[offset+n] * R, chargedLevels[(offset+n+7) % chargedLevels.size()] * R };
                const auto inputs = frames;
                actual.process4(frames, setting, low, high, 1.25, .73, .89, .997, .25, .75, r.primitiveCurve, r.gainCurve);
                for (std::size_t n = 0; n < 4; ++n)
                    check(sameFrame(frames[n], expected.process<true>(inputs[n], setting, low, high, 1.25, .73, .89, .997, .25, .75, r.primitiveCurve, r.gainCurve)));
                check(sameLane(actual, expected)); ++chargedBatches;
                const Candidate::Frame continuation { -.0, .75001 * R };
                check(sameFrame(actual.process<true>(continuation, setting, low, high, 1.25, .73, .89, .997, .25, .75, r.primitiveCurve, r.gainCurve),
                    expected.process<true>(continuation, setting, low, high, 1.25, .73, .89, .997, .25, .75, r.primitiveCurve, r.gainCurve)));
                check(sameLane(actual, expected));
            }
        }
        for (const double bad : { infinity, -infinity, invalid, -invalid }) {
            Lane actual; actual.primitiveCharacter = character; actual.previousU = { bad, -bad };
            actual.primitiveKind = { PrimitiveKind::exact, PrimitiveKind::curve }; actual.previousPrimitive = { -.25, .5 };
            actual.dc = { .125, -.375 }; actual.previousDifference = { -.5, .25 };
            Lane expected = actual; SaturationLaneSettings setting; setting.character = character;
            std::array<Candidate::Frame, 4> frames {{ { .125, -.25 }, { -.0, .0 }, { -.75 * R, .75 * R }, { 16. * R, -16. * R } }};
            const auto inputs = frames;
            actual.process4(frames, setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve);
            for (std::size_t n = 0; n < 4; ++n) check(sameFrame(frames[n], scalar(expected, inputs[n], setting)));
            check(sameLane(actual, expected)); actual = {}; expected = {};
            check(sameFrame(scalar(actual, { -.0, .125 }, setting), scalar(expected, { -.0, .125 }, setting)) && sameLane(actual, expected));
        }
    }
    std::cout << "COMPLETE_LANE charged_batches=" << chargedBatches << " failures=" << failures << '\n';
    for (const double bad : { infinity, -infinity, invalid, -invalid }) {
        Candidate actual, expected; check(actual.prepare(48000, 4) && expected.prepare(48000, 4));
        SaturationSettings setting; setting.lanes[0].character = SaturationCharacter::glow; setting.lanes[0].preTiltDb = 6;
        check(actual.setSettings(setting) && expected.setSettings(setting));
        for (unsigned n = 0; n < 32; ++n) actual.process({ .01 * n, -.02 * n });
        check(sameFrame(actual.process({ bad, .125 }), Candidate::Frame {})); expected.reset();
        for (unsigned n = 0; n < 32; ++n) check(sameFrame(actual.process({ -.01 * n, .02 * n }), expected.process({ -.01 * n, .02 * n })));
    }
    single = {};
    check(single.primitiveKind == Lane {}.primitiveKind && single.previousU == Lane {}.previousU);
    std::cout << "ORDINARY_LANE transitions="; for (auto count : transitions) std::cout << count << ',';
    std::cout << " failures=" << failures << '\n';
    failures += runSaturationCommonControlChecks();
    return failures;
}
#endif
double preparedPrimitive(SaturationCharacter c, double x) noexcept {
    try { return curvePrimitive(resources().primitiveCurve, c, x); } catch (...) { return invalid; }
}
bool preparedAdaaUsesExactPrimitive(double x, double previous) noexcept {
    return quotientPrimitiveKind(x, previous) == PrimitiveKind::exact;
}
double preparedAdaa(SaturationCharacter c, double x, double previous) noexcept {
    const double delta = x - previous;
    if (std::abs(delta) < 1e-6 * (1 + std::max(std::abs(x), std::abs(previous)))) return shape(c, .5 * (x + previous));
    const double b = preparedConstants.glowBias;
    if (!std::isfinite(x) || !std::isfinite(previous) || preparedAdaaUsesExactPrimitive(x, previous))
        return exactQuotient(c, x, previous, b, 1 - b * b);
    return (preparedPrimitive(c, x) - preparedPrimitive(c, previous)) / delta;
}

namespace {
struct Prefix4 {
    Candidate::Frame wetInput;
    std::array<Candidate::Frame, 4> wet, dry;
    std::array<std::array<Candidate::Frame, 4>, 3> bandWet;
    std::array<std::array<Candidate::Frame, 3>, 4> bandDry;
};
}
#if defined(AIRACK_SATURATION_TRANSITIONS)
namespace {
bool sameControlBits(float a, float b) noexcept { return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b); }
bool sameControlBits(double a, double b) noexcept { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }
bool sameContinuousControls(const SaturationSettings& a, const SaturationSettings& b) noexcept {
    if (!sameControlBits(a.inputTrimDb, b.inputTrimDb) || !sameControlBits(a.outputTrimDb, b.outputTrimDb)
        || !sameControlBits(a.mix, b.mix) || !sameControlBits(a.lowCrossoverHz, b.lowCrossoverHz)
        || !sameControlBits(a.highCrossoverHz, b.highCrossoverHz) || !sameControlBits(a.preLowCutHz, b.preLowCutHz)
        || !sameControlBits(a.postHighCutHz, b.postHighCutHz) || a.driveCompensation != b.driveCompensation) return false;
    for (std::size_t n = 0; n < 4; ++n) {
        const auto& x = a.lanes[n]; const auto& y = b.lanes[n];
        if (!sameControlBits(x.driveDb, y.driveDb) || !sameControlBits(x.preTiltDb, y.preTiltDb)
            || !sameControlBits(x.mix, y.mix) || !sameControlBits(x.trimDb, y.trimDb) || x.bypassed != y.bypassed) return false;
    }
    return true;
}
bool sameControlCoefficients(const Coefficients& a, const Coefficients& b) noexcept {
    return sameControlBits(a.b0, b.b0) && sameControlBits(a.b1, b.b1) && sameControlBits(a.b2, b.b2)
        && sameControlBits(a.a1, b.a1) && sameControlBits(a.a2, b.a2);
}
}
struct Endpoint {
    using Frame = Candidate::Frame;
    using ControlPeer = Endpoint;
#else
struct Candidate::State {
    using ControlPeer = State;
#endif
    SaturationSettings target {}, current {}, coefficientSettings {};
    std::array<double, 4> targetLogHz {};
    double rate = 0, highRate = 0, dcAlpha = 0, sagAttack = 0, sagRelease = 0; unsigned factor = 1, ramp = 0;
    bool prepared = false, admitted = false, coefficientsValid = false; const Resources* coefficients = nullptr;
    Resampler wetUp, dryUp, down; Network wetNetwork, dryNetwork;
    wide_network::State wideNetwork; bool useWideNetwork = false;
    std::array<Lane, 4> lanes; std::array<Frame, 4> previousDry {};
    Biquad preCut, postCut; NetworkCoefficients network; Coefficients pre, post;
    std::array<Coefficients, 4> shelfLow, shelfHigh; std::array<double, 4> driveGain {}, driveAmount {}, wetGain {};
    SaturationCrossovers coefficientCorners {}; DesignBasis shelfBasis;
    double inputGain = 1, outputGain = 1;
#if defined(AIRACK_SATURATION_TRANSITIONS)
    double compensationWet = 1, coefficientCompensation = -1;
    bool failed = false;
#endif
    double gainForFactor(double db) const noexcept { return factor == 4 ? coefficients->gainCurve.gain(db) : gain(db); }
    void cacheTargetFrequencies(const SaturationSettings& next, bool first) noexcept {
        // Compute the identical destination logs once per admitted change.
        // An unadmitted default State never ramps; its first admission fills
        // all entries, including frequencies equal to the default settings.
        if (first || next.lowCrossoverHz != target.lowCrossoverHz)
            targetLogHz[0] = std::log(double(next.lowCrossoverHz));
        if (first || next.highCrossoverHz != target.highCrossoverHz)
            targetLogHz[1] = std::log(double(next.highCrossoverHz));
        if (first || next.preLowCutHz != target.preLowCutHz)
            targetLogHz[2] = std::log(double(next.preLowCutHz));
        if (first || next.postHighCutHz != target.postHighCutHz)
            targetLogHz[3] = std::log(double(next.postHighCutHz));
    }
    template<bool ReuseCommon = false>
    void updateCoefficients(const ControlPeer* common = nullptr) noexcept {
        // Prepared rate and current settings are admitted. Bounded ramps and
        // validated structural edits preserve those bounds, so repeat only the
        // public helper's exact corner arithmetic, not its admission traversal.
        const double highCorner = std::min(double(current.highCrossoverHz), .45 * rate);
        const SaturationCrossovers corners { std::min(double(current.lowCrossoverHz), .8 * highCorner), highCorner };
        if (!coefficientsValid || corners.lowHz != coefficientCorners.lowHz) {
            if constexpr (ReuseCommon) { network.lowLP = common->network.lowLP; network.lowHP = common->network.lowHP; }
            else {
            const auto basis = designBasis(highRate, corners.lowHz, factor == 4);
            if (factor == 4) designSplit(basis, network.lowLP, network.lowHP);
            else { network.lowLP = design(Filter::low, basis); network.lowHP = design(Filter::high, basis); }
            }
        }
        if (!coefficientsValid || corners.highHz != coefficientCorners.highHz) {
            if constexpr (ReuseCommon) { network.highLP = common->network.highLP; network.highHP = common->network.highHP; network.highAP = common->network.highAP; }
            else {
            const auto basis = designBasis(highRate, corners.highHz, factor == 4);
            if (factor == 4) designSplit(basis, network.highLP, network.highHP, &network.highAP);
            else { network.highLP = design(Filter::low, basis); network.highHP = design(Filter::high, basis); network.highAP = design(Filter::allpass, basis); }
            }
        }
        if (!coefficientsValid || current.preLowCutHz != coefficientSettings.preLowCutHz) {
            if constexpr (ReuseCommon) pre = common->pre;
            else pre = design(Filter::high, designBasis(rate, std::min(double(current.preLowCutHz), .45 * rate), factor == 4));
        }
        if (!coefficientsValid || current.postHighCutHz != coefficientSettings.postHighCutHz) {
            if constexpr (ReuseCommon) post = common->post;
            else post = design(Filter::low, designBasis(highRate, std::min(double(current.postHighCutHz), .45 * rate), factor == 4));
        }
        if (!coefficientsValid || current.inputTrimDb != coefficientSettings.inputTrimDb) {
            if constexpr (ReuseCommon) inputGain = common->inputGain;
            else inputGain = gainForFactor(current.inputTrimDb);
        }
        if (!coefficientsValid || current.outputTrimDb != coefficientSettings.outputTrimDb) {
            if constexpr (ReuseCommon) outputGain = common->outputGain;
            else outputGain = gainForFactor(current.outputTrimDb);
        }
        for (std::size_t n = 0; n < 4; ++n) {
            const auto& l = current.lanes[n]; const auto& previous = coefficientSettings.lanes[n];
            if (!coefficientsValid || l.preTiltDb != previous.preTiltDb) {
                std::size_t match = 0; while (match < n && current.lanes[match].preTiltDb != l.preTiltDb) ++match;
                if (match < n) { shelfLow[n] = shelfLow[match]; shelfHigh[n] = shelfHigh[match]; }
                else {
                    if constexpr (ReuseCommon) { shelfLow[n] = common->shelfLow[n]; shelfHigh[n] = common->shelfHigh[n]; }
                    else {
                    shelfLow[n] = design(Filter::lowShelf, shelfBasis, -double(l.preTiltDb) / 2, factor == 4 ? &coefficients->gainCurve : nullptr);
                    shelfHigh[n] = design(Filter::highShelf, shelfBasis, double(l.preTiltDb) / 2, factor == 4 ? &coefficients->gainCurve : nullptr);
                    }
                }
            }
            if (!coefficientsValid || l.driveDb != previous.driveDb) {
                std::size_t match = 0; while (match < n && current.lanes[match].driveDb != l.driveDb) ++match;
                if constexpr (ReuseCommon) {
                    driveGain[n] = match < n ? driveGain[match] : common->driveGain[n];
                    driveAmount[n] = match < n ? driveAmount[match] : common->driveAmount[n];
                } else {
                driveGain[n] = match < n ? driveGain[match] : gain(l.driveDb);
                driveAmount[n] = match < n ? driveAmount[match] : double(l.driveDb) / 36;
                }
            }
            if (l.driveDb == 0) driveAmount[n] = double(l.driveDb); // Preserve the sign of zero across equal-key edits.
            if (!coefficientsValid || l.trimDb != previous.trimDb || l.driveDb != previous.driveDb
#if defined(AIRACK_SATURATION_TRANSITIONS)
                || compensationWet != coefficientCompensation
#endif
                || l.character != previous.character || current.driveCompensation != coefficientSettings.driveCompensation) {
#if defined(AIRACK_SATURATION_TRANSITIONS)
                if (compensationWet > 0 && compensationWet < 1)
                    wetGain[n] = gainForFactor(double(l.trimDb) + compensationWet * admittedCompensationDb(l.character, l.driveDb));
                else
                    wetGain[n] = gainForFactor(l.trimDb + (compensationWet == 1 ? admittedCompensationDb(l.character, l.driveDb) : 0));
#else
                wetGain[n] = gainForFactor(l.trimDb + (current.driveCompensation ? admittedCompensationDb(l.character, l.driveDb) : 0));
#endif
            }
        }
#if defined(AIRACK_SATURATION_TRANSITIONS)
        coefficientCompensation = compensationWet;
#endif
        coefficientSettings = current; coefficientCorners = corners; coefficientsValid = true;
    }
    void advanceControls() noexcept {
        AIRACK_OPTIMIZATION_SCOPE(controlsScope, controls);
        if (ramp == 0) return;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        ++optimizationControlMathSteps;
#endif
        const auto step = [this](float& value, float destination) { value = ramp == 1 ? destination : float(value + (double(destination) - value) / ramp); };
        const auto stepHz = [this](float& value, float destination, double destinationLog) {
            if (value != destination) {
                if (ramp == 1) value = destination;
                else {
                    const double currentLog = std::log(double(value));
                    value = float(std::exp(currentLog + (destinationLog - currentLog) / ramp));
                }
            }
        };
        step(current.inputTrimDb, target.inputTrimDb); step(current.outputTrimDb, target.outputTrimDb); step(current.mix, target.mix);
        stepHz(current.lowCrossoverHz, target.lowCrossoverHz, targetLogHz[0]); stepHz(current.highCrossoverHz, target.highCrossoverHz, targetLogHz[1]);
        stepHz(current.preLowCutHz, target.preLowCutHz, targetLogHz[2]); stepHz(current.postHighCutHz, target.postHighCutHz, targetLogHz[3]);
        for (std::size_t n = 0; n < 4; ++n) {
            step(current.lanes[n].driveDb, target.lanes[n].driveDb); step(current.lanes[n].preTiltDb, target.lanes[n].preTiltDb);
            step(current.lanes[n].mix, target.lanes[n].mix); step(current.lanes[n].trimDb, target.lanes[n].trimDb);
#if defined(AIRACK_SATURATION_TRANSITIONS)
            const double destination = target.lanes[n].bypassed ? 0 : 1;
            lanes[n].bypassWet = ramp == 1 ? destination : lanes[n].bypassWet + (destination - lanes[n].bypassWet) / ramp;
#endif
        }
#if defined(AIRACK_SATURATION_TRANSITIONS)
        const double destination = target.driveCompensation ? 1 : 0;
        compensationWet = ramp == 1 ? destination : compensationWet + (destination - compensationWet) / ramp;
#endif
        --ramp; updateCoefficients();
    }
#if defined(AIRACK_SATURATION_TRANSITIONS)
    bool hasCommonControls(const Endpoint& other) const noexcept {
        if (!prepared || !other.prepared || failed || other.failed || factor != 4 || other.factor != 4
            || !coefficientsValid || !other.coefficientsValid || !coefficients || coefficients != other.coefficients
            || ramp != other.ramp || useWideNetwork != other.useWideNetwork) return false;
        if (!sameContinuousControls(current, other.current) || !sameContinuousControls(target, other.target)
            || !sameContinuousControls(coefficientSettings, other.coefficientSettings)
            || !sameControlBits(rate, other.rate) || !sameControlBits(highRate, other.highRate)
            || !sameControlBits(dcAlpha, other.dcAlpha) || !sameControlBits(sagAttack, other.sagAttack)
            || !sameControlBits(sagRelease, other.sagRelease) || !sameControlBits(shelfBasis.cs, other.shelfBasis.cs)
            || !sameControlBits(shelfBasis.alpha, other.shelfBasis.alpha)
            || !sameControlBits(coefficientCorners.lowHz, other.coefficientCorners.lowHz)
            || !sameControlBits(coefficientCorners.highHz, other.coefficientCorners.highHz)
            || !sameControlBits(compensationWet, other.compensationWet) || !sameControlBits(coefficientCompensation, other.coefficientCompensation)
            || !sameControlBits(inputGain, other.inputGain) || !sameControlBits(outputGain, other.outputGain)
            || !sameControlCoefficients(network.lowLP, other.network.lowLP) || !sameControlCoefficients(network.lowHP, other.network.lowHP)
            || !sameControlCoefficients(network.highLP, other.network.highLP) || !sameControlCoefficients(network.highHP, other.network.highHP)
            || !sameControlCoefficients(network.highAP, other.network.highAP) || !sameControlCoefficients(pre, other.pre)
            || !sameControlCoefficients(post, other.post)) return false;
        for (std::size_t n = 0; n < 4; ++n)
            if (!sameControlBits(targetLogHz[n], other.targetLogHz[n]) || !sameControlBits(lanes[n].bypassWet, other.lanes[n].bypassWet)
                || !sameControlBits(driveGain[n], other.driveGain[n]) || !sameControlBits(driveAmount[n], other.driveAmount[n])
                || !sameControlCoefficients(shelfLow[n], other.shelfLow[n]) || !sameControlCoefficients(shelfHigh[n], other.shelfHigh[n])) return false;
        return true;
    }
    void advanceControlsFrom(const Endpoint& common) noexcept {
        AIRACK_OPTIMIZATION_SCOPE(controlsScope, controls);
        if (ramp == 0) return;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        ++optimizationControlCopies;
#endif
        // The destination's old ramp owns the final step even after the source reaches zero.
        current.inputTrimDb = common.current.inputTrimDb; current.outputTrimDb = common.current.outputTrimDb; current.mix = common.current.mix;
        current.lowCrossoverHz = common.current.lowCrossoverHz; current.highCrossoverHz = common.current.highCrossoverHz;
        current.preLowCutHz = common.current.preLowCutHz; current.postHighCutHz = common.current.postHighCutHz;
        for (std::size_t n = 0; n < 4; ++n) {
            current.lanes[n].driveDb = common.current.lanes[n].driveDb; current.lanes[n].preTiltDb = common.current.lanes[n].preTiltDb;
            current.lanes[n].mix = common.current.lanes[n].mix; current.lanes[n].trimDb = common.current.lanes[n].trimDb;
            lanes[n].bypassWet = common.lanes[n].bypassWet;
        }
        ramp = common.ramp; compensationWet = common.compensationWet; updateCoefficients<true>(&common);
    }
#endif
    void clear() noexcept {
        wetUp.clear(); dryUp.clear(); down.clear(); wetNetwork = {}; dryNetwork = {}; wideNetwork = {}; lanes = {}; previousDry = {}; preCut = {}; postCut = {};
        current = target; ramp = 0;
#if defined(AIRACK_SATURATION_TRANSITIONS)
        compensationWet = target.driveCompensation ? 1 : 0;
        for (std::size_t n = 0; n < 4; ++n) lanes[n].bypassWet = target.lanes[n].bypassed ? 0 : 1;
        failed = false;
#endif
        if (prepared) updateCoefficients();
    }
    template<std::size_t Capacity, bool ProductState = false>
    Frame processPrepared(Frame input, const ControlPeer* common = nullptr, Prefix4* prefix = nullptr) noexcept {
        if constexpr (Capacity == 64 && !ProductState)
            if (factor == 64) return processPrepared<64, true>(input);
        auto& s = *this;
#if defined(AIRACK_SATURATION_TRANSITIONS)
        if constexpr (Capacity == 4) {
            if (common) s.advanceControlsFrom(*common);
            else s.advanceControls();
        } else s.advanceControls();
#else
        s.advanceControls();
#endif
        std::array<Frame, Capacity> wet, dry, output;
        Frame wetInput;
        {
        AIRACK_OPTIMIZATION_SCOPE(prefixScope, prefix);
        if constexpr (Capacity == 4) {
            if (prefix && common) {
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
                ++optimizationPrefixCopies;
#endif
                wetInput = prefix->wetInput; wet = prefix->wet; dry = prefix->dry;
                s.preCut.z1 = common->preCut.z1; s.preCut.z2 = common->preCut.z2;
                if (s.coefficients->fixed4Layout) {
                    const auto copyPhase = []<std::size_t DirectCount, std::size_t DelayedCount>(auto& to, const auto& from) {
                        for (std::size_t n = 0; n < DirectCount; ++n) {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
                            _mm_store_pd(to.direct[n].data(), _mm_load_pd(from.direct[n].data()));
#else
                            to.direct[n] = from.direct[n];
#endif
                        }
                        for (std::size_t n = 0; n < DelayedCount; ++n) {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
                            _mm_store_pd(to.delayed[n].data(), _mm_load_pd(from.delayed[n].data()));
#else
                            to.delayed[n] = from.delayed[n];
#endif
                        }
                    };
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
                    copyPhase.operator()<5, 4>(s.wetUp.stereoIir[0], common->wetUp.stereoIir[0]);
                    copyPhase.operator()<3, 3>(s.wetUp.stereoIir[1], common->wetUp.stereoIir[1]);
                    copyPhase.operator()<5, 4>(s.dryUp.stereoIir[0], common->dryUp.stereoIir[0]);
                    copyPhase.operator()<3, 3>(s.dryUp.stereoIir[1], common->dryUp.stereoIir[1]);
#else
                    for (std::size_t channel = 0; channel < 2; ++channel) {
                        copyPhase.operator()<5, 4>(s.wetUp.iir[0][channel], common->wetUp.iir[0][channel]);
                        copyPhase.operator()<3, 3>(s.wetUp.iir[1][channel], common->wetUp.iir[1][channel]);
                        copyPhase.operator()<5, 4>(s.dryUp.iir[0][channel], common->dryUp.iir[0][channel]);
                        copyPhase.operator()<3, 3>(s.dryUp.iir[1][channel], common->dryUp.iir[1][channel]);
                    }
#endif
                } else {
                for (std::size_t stage = 0; stage < 2; ++stage) {
                    const auto& c = s.coefficients->iir[stage].up;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
                    std::copy_n(common->wetUp.stereoIir[stage].direct.begin(), c.directCount, s.wetUp.stereoIir[stage].direct.begin());
                    std::copy_n(common->wetUp.stereoIir[stage].delayed.begin(), c.delayedCount, s.wetUp.stereoIir[stage].delayed.begin());
                    std::copy_n(common->dryUp.stereoIir[stage].direct.begin(), c.directCount, s.dryUp.stereoIir[stage].direct.begin());
                    std::copy_n(common->dryUp.stereoIir[stage].delayed.begin(), c.delayedCount, s.dryUp.stereoIir[stage].delayed.begin());
#else
                    for (std::size_t channel = 0; channel < 2; ++channel) {
                        std::copy_n(common->wetUp.iir[stage][channel].direct.begin(), c.directCount, s.wetUp.iir[stage][channel].direct.begin());
                        std::copy_n(common->wetUp.iir[stage][channel].delayed.begin(), c.delayedCount, s.wetUp.iir[stage][channel].delayed.begin());
                        std::copy_n(common->dryUp.iir[stage][channel].direct.begin(), c.directCount, s.dryUp.iir[stage][channel].direct.begin());
                        std::copy_n(common->dryUp.iir[stage][channel].delayed.begin(), c.delayedCount, s.dryUp.iir[stage][channel].delayed.begin());
                    }
#endif
                }
                }
            }
        }
        if (!prefix || !common || Capacity != 4) {
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        if constexpr (Capacity == 4) ++optimizationPrefixExecutions;
#endif
        input[0] *= s.inputGain; input[1] *= s.inputGain;
        wetInput = input;
#if defined(AIRACK_SATURATION_TRANSITIONS)
        const auto preFiltered = s.preCut.process<ProductState>(input, s.pre);
        if (s.current.preLowCutEnabled) wetInput = preFiltered;
#else
        if (s.current.preLowCutEnabled) wetInput = s.preCut.process<ProductState>(input, s.pre);
#endif
        if constexpr (Capacity == 4) {
            s.wetUp.up4(wetInput, *s.coefficients, wet); s.dryUp.up4(input, *s.coefficients, dry);
        } else {
            s.wetUp.up(wetInput, s.factor, *s.coefficients, wet); s.dryUp.up(input, s.factor, *s.coefficients, dry);
        }
        if constexpr (Capacity == 4) if (prefix) { prefix->wetInput = wetInput; prefix->wet = wet; prefix->dry = dry; }
        }
        }
        std::array<std::array<wide_network::Quad, 3>, Capacity == 4 ? 4 : 0> wideBands;
        if constexpr (Capacity == 4) {
            if (s.useWideNetwork && s.current.mode == SaturationMode::threeBand) {
                AIRACK_OPTIMIZATION_SCOPE(crossoverScope, crossover);
                if (prefix && common) s.wideNetwork = common->wideNetwork;
                else wide_network::process4(s.wideNetwork, s.network, wet, dry, wideBands);
            }
        }
        std::array<std::array<Frame, 4>, Capacity == 4 ? 3 : 0> batchWet;
        std::array<std::array<Frame, 3>, Capacity == 4 ? 4 : 0> batchDry;
        if constexpr (Capacity == 4) {
            if (s.current.mode == SaturationMode::fullBand) {
                s.lanes[0].process4(wet, s.current.lanes[0], s.shelfLow[0], s.shelfHigh[0], s.driveGain[0], s.driveAmount[0], s.wetGain[0], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve
#if defined(AIRACK_SATURATION_TRANSITIONS) && !defined(AIRACK_SATURATION_NUMERICAL_TEST)
                    , s.useWideNetwork
#endif
                );
            } else {
                if (prefix && common) {
                    batchWet = prefix->bandWet; batchDry = prefix->bandDry;
                    if (!s.useWideNetwork) {
                        const auto copyBiquad = [](Biquad& to, const Biquad& from) { to.z1 = from.z1; to.z2 = from.z2; };
                        const auto copyNetwork = [&](Network& to, const Network& from) {
                            for (std::size_t n = 0; n < 2; ++n) {
                                copyBiquad(to.low.low[n], from.low.low[n]); copyBiquad(to.low.high[n], from.low.high[n]);
                                copyBiquad(to.high.low[n], from.high.low[n]); copyBiquad(to.high.high[n], from.high.high[n]);
                            }
                            copyBiquad(to.lowCompensation, from.lowCompensation);
                        };
                        copyNetwork(s.wetNetwork, common->wetNetwork); copyNetwork(s.dryNetwork, common->dryNetwork);
                    }
                } else {
                for (std::size_t n = 0; n < 4; ++n) {
                    if (s.useWideNetwork) {
                        for (std::size_t band = 0; band < 3; ++band) {
                            const auto& q = wideBands[n][band];
                            batchWet[band][n] = { q[0], q[1] }; batchDry[n][band] = { q[2], q[3] };
                        }
                    } else {
                        const auto wb = s.wetNetwork.process(wet[n], s.network), db = s.dryNetwork.process(dry[n], s.network);
                        for (std::size_t band = 0; band < 3; ++band) { batchWet[band][n] = wb[band]; batchDry[n][band] = db[band]; }
                    }
                }
                if (prefix) { prefix->bandWet = batchWet; prefix->bandDry = batchDry; }
                }
                for (std::size_t band = 0; band < 3; ++band) {
                    const auto lane = band + 1;
                    s.lanes[lane].process4(batchWet[band], s.current.lanes[lane], s.shelfLow[lane], s.shelfHigh[lane], s.driveGain[lane], s.driveAmount[lane], s.wetGain[lane], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve
#if defined(AIRACK_SATURATION_TRANSITIONS) && !defined(AIRACK_SATURATION_NUMERICAL_TEST)
                        , s.useWideNetwork
#endif
                    );
                }
            }
        }
        AIRACK_OPTIMIZATION_SCOPE(returningScope, returning);
        bool valid = finite(wetInput);
        const auto processingFactor = Capacity == 4 ? 4 : s.factor;
        for (std::size_t n = 0; n < processingFactor; ++n) {
            Frame w {}, d {};
            if (s.current.mode == SaturationMode::fullBand) {
                if constexpr (Capacity == 4) w = wet[n];
                else w = s.lanes[0].process<false, ProductState>(wet[n], s.current.lanes[0], s.shelfLow[0], s.shelfHigh[0], s.driveGain[0], s.driveAmount[0], s.wetGain[0], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve);
                if constexpr (Capacity == 4) d = returnDryAverage(dry[n], s.previousDry[0]);
                else for (std::size_t ch = 0; ch < 2; ++ch) d[ch] = .5 * (dry[n][ch] + s.previousDry[0][ch]);
                s.previousDry[0] = dry[n];
            } else {
                if constexpr (Capacity == 4) {
                    for (std::size_t band = 0; band < 3; ++band) {
                        const auto lane = band + 1;
                        const auto& y = batchWet[band][n]; const auto& dryBand = batchDry[n][band];
                        // The compiler drops intrinsic +0; retain the original scalar first contribution.
                        if (band == 0) for (std::size_t ch = 0; ch < 2; ++ch) {
                            w[ch] += y[ch]; d[ch] += .5 * (dryBand[ch] + s.previousDry[lane][ch]);
                        } else returnBand(w, d, y, dryBand, s.previousDry[lane]);
                        s.previousDry[lane] = dryBand; valid = valid && finite(y);
                    }
                } else {
                    const auto wb = s.wetNetwork.process<ProductState>(wet[n], s.network), db = s.dryNetwork.process<ProductState>(dry[n], s.network);
                    for (std::size_t band = 0; band < 3; ++band) {
                        const auto lane = band + 1;
                        const auto y = s.lanes[lane].process<false, ProductState>(wb[band], s.current.lanes[lane], s.shelfLow[lane], s.shelfHigh[lane], s.driveGain[lane], s.driveAmount[lane], s.wetGain[lane], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve);
                        for (std::size_t ch = 0; ch < 2; ++ch) { w[ch] += y[ch]; d[ch] += .5 * (db[band][ch] + s.previousDry[lane][ch]); }
                        s.previousDry[lane] = db[band]; valid = valid && finite(y);
                    }
                }
            }
#if defined(AIRACK_SATURATION_TRANSITIONS)
            const auto postFiltered = s.postCut.process<ProductState>(w, s.post);
            if (s.current.postHighCutEnabled) w = postFiltered;
#else
            if (s.current.postHighCutEnabled) w = s.postCut.process<ProductState>(w, s.post);
#endif
#if defined(AIRACK_SATURATION_REFERENCE_TRACE)
            saturationReferenceTrace(w, d);
#endif
            for (std::size_t ch = 0; ch < 2; ++ch) output[n][ch] = d[ch] + s.current.mix * (w[ch] - d[ch]);
            valid = valid && finite(w) && finite(d) && finite(output[n]);
        }
        auto result = [&] {
            if constexpr (Capacity == 4) return s.down.down4(output, *s.coefficients);
            else return s.down.down(output, s.factor, *s.coefficients);
        }();
        if constexpr (Capacity == 4) returnGain(result, s.outputGain);
        else { result[0] *= s.outputGain; result[1] *= s.outputGain; }
        if (!valid || !finite(result)) {
            s.clear();
#if defined(AIRACK_SATURATION_TRANSITIONS)
            s.failed = true;
#endif
            return {};
        } return result;
    }
};
#if defined(AIRACK_SATURATION_TRANSITIONS)
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
struct ControlObservation {
    const Endpoint* endpoints = nullptr;
    const unsigned* active = nullptr;
    const unsigned* fadeFrame = nullptr;
    const bool* fading = nullptr;
    const bool* common = nullptr;
    const bool* prefix = nullptr;
} optimizationControlObservation;
#endif
namespace {
bool sameStructure(const SaturationSettings& a, const SaturationSettings& b) noexcept {
    if (a.mode != b.mode || a.preLowCutEnabled != b.preLowCutEnabled || a.postHighCutEnabled != b.postHighCutEnabled) return false;
    for (std::size_t n = 0; n < 4; ++n) if (a.lanes[n].character != b.lanes[n].character) return false;
    return true;
}
void preserveStructure(SaturationSettings& to, const SaturationSettings& from) noexcept {
    to.mode = from.mode; to.preLowCutEnabled = from.preLowCutEnabled; to.postHighCutEnabled = from.postHighCutEnabled;
    for (std::size_t n = 0; n < 4; ++n) to.lanes[n].character = from.lanes[n].character;
}
void retarget(Endpoint& endpoint, const SaturationSettings& next) noexcept {
    if (next == endpoint.target) return;
    auto controls = next; preserveStructure(controls, endpoint.target);
    const bool controlsChanged = controls != endpoint.target;
    endpoint.cacheTargetFrequencies(next, false); endpoint.target = next;
    preserveStructure(endpoint.current, next);
    endpoint.current.driveCompensation = next.driveCompensation;
    for (std::size_t n = 0; n < 4; ++n) endpoint.current.lanes[n].bypassed = next.lanes[n].bypassed;
    if (controlsChanged) endpoint.ramp = unsigned(std::ceil(.020 * endpoint.rate));
    endpoint.updateCoefficients();
}
}
struct Candidate::State {
    std::array<Endpoint, 2> endpoints;
    SaturationSettings target {};
    unsigned active = 0, fadeFrame = 0, fadeLength = 1;
    bool prepared = false, admitted = false, fading = false, commonControls = false, commonPrefix = false;
    void startFade() noexcept {
        auto& from = endpoints[active]; auto& to = endpoints[1 - active];
        to = from;
        if (from.current.mode != target.mode) {
            // An inactive mode must not revive its old networks or lane audio.
            to.wetNetwork = {}; to.dryNetwork = {}; to.wideNetwork = {}; to.previousDry = {};
            for (auto& lane : to.lanes) { const double bypassWet = lane.bypassWet; lane = {}; lane.bypassWet = bypassWet; }
        }
        retarget(to, target); commonControls = from.hasCommonControls(to); fadeFrame = 0; fading = true;
        commonPrefix = commonControls && from.current.mode == to.current.mode && from.current.preLowCutEnabled == to.current.preLowCutEnabled;
    }
    void clear() noexcept {
        for (auto& endpoint : endpoints) { endpoint.cacheTargetFrequencies(target, true); endpoint.target = target; endpoint.clear(); }
        active = fadeFrame = 0; fading = commonControls = commonPrefix = false;
    }
};
// Two fixed endpoint objects and, only at factor64, six preallocated FIR banks.
static_assert(sizeof(Candidate::Frame) == 16);
// State is private; this bound also includes the single latest-intent settings bank.
Candidate::Candidate() : state(std::make_unique<State>()) {
    static_assert(sizeof(State) + 6 * sizeof(Resampler::FirBank) < 200000);
}
Candidate::~Candidate() {
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    if (optimizationControlObservation.endpoints == state->endpoints.data()) optimizationControlObservation = {};
#endif
}
bool Candidate::prepare(double rate, unsigned factor) noexcept {
    if (!validRate(rate) || (factor != 1 && factor != 2 && factor != 4 && factor != 8 && factor != 64)) return false;
    try {
        auto replacement = std::make_unique<State>(); replacement->target = state->target; replacement->admitted = state->admitted;
        for (auto& endpoint : replacement->endpoints) {
            endpoint.target = endpoint.current = replacement->target; endpoint.admitted = replacement->admitted;
            endpoint.cacheTargetFrequencies(replacement->target, true);
            endpoint.rate = rate; endpoint.highRate = rate * factor; endpoint.factor = factor;
            endpoint.wetUp.prepare(factor); endpoint.dryUp.prepare(factor); endpoint.down.prepare(factor);
            endpoint.dcAlpha = std::exp(-2 * pi * 5 / endpoint.highRate);
            endpoint.sagAttack = std::exp(-1 / (endpoint.highRate * .002)); endpoint.sagRelease = std::exp(-1 / (endpoint.highRate * .080));
            endpoint.shelfBasis = designBasis(endpoint.highRate, std::min(1000., .45 * rate), factor == 4);
            endpoint.coefficients = &resources(); endpoint.prepared = true;
            endpoint.useWideNetwork = factor == 4 && juce::SystemStats::hasAVX() && wide_network::operatingSystemSupportsAvx();
            endpoint.clear();
        }
        replacement->fadeLength = unsigned(std::ceil(.020 * rate)); replacement->prepared = true;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
        if (optimizationControlObservation.endpoints == state->endpoints.data()) optimizationControlObservation = {};
#endif
        state.swap(replacement); return true;
    } catch (...) { return false; }
}
bool Candidate::setSettings(const SaturationSettings& next) noexcept {
    if (!validateSaturation(next)) return false;
    auto& s = *state;
    if (s.admitted && next == s.target) return true;
    const bool first = !s.admitted; s.target = next; s.admitted = true;
    if (first || !s.prepared) { s.clear(); return true; }
    for (unsigned n = 0; n < (s.fading ? 2u : 1u); ++n) {
        auto& endpoint = s.endpoints[n == 0 ? s.active : 1 - s.active];
        auto continuous = next; preserveStructure(continuous, endpoint.current); retarget(endpoint, continuous);
    }
    if (s.fading) s.commonControls = s.commonControls && s.endpoints[s.active].hasCommonControls(s.endpoints[1 - s.active]);
    if (s.fading) s.commonPrefix = s.commonPrefix && s.commonControls
        && s.endpoints[s.active].current.mode == s.endpoints[1 - s.active].current.mode
        && s.endpoints[s.active].current.preLowCutEnabled == s.endpoints[1 - s.active].current.preLowCutEnabled;
    if (!s.fading && !sameStructure(next, s.endpoints[s.active].current)) s.startFade();
    return true;
}
Candidate::Frame Candidate::process(Frame input) noexcept {
    AIRACK_OPTIMIZATION_SCOPE(candidateScope, candidate);
    auto& s = *state;
#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
    optimizationControlObservation = { s.endpoints.data(), &s.active, &s.fadeFrame, &s.fading, &s.commonControls, &s.commonPrefix };
#endif
    if (!s.prepared) return {};
    if (!finite(input) || std::max(std::abs(input[0]), std::abs(input[1])) > std::numeric_limits<double>::max() / 1e12) { reset(); return {}; }
    const auto render = [&](Endpoint& endpoint) { return endpoint.factor == 4 ? endpoint.processPrepared<4>(input) : endpoint.processPrepared<64>(input); };
    Prefix4 prefix;
    auto result = s.commonPrefix ? s.endpoints[s.active].processPrepared<4>(input, nullptr, &prefix) : render(s.endpoints[s.active]);
    if (s.endpoints[s.active].failed) { reset(); return {}; }
    if (s.fading) {
        auto& destination = s.endpoints[1 - s.active];
        const auto next = s.commonControls ? destination.processPrepared<4>(input, &s.endpoints[s.active], s.commonPrefix ? &prefix : nullptr) : render(destination);
        if (s.endpoints[1 - s.active].failed) { reset(); return {}; }
        const double weight = double(s.fadeFrame) / (s.fadeLength - 1);
        if (s.endpoints[s.active].factor == 4) returnFade(result, next, weight);
        else for (std::size_t ch = 0; ch < 2; ++ch) result[ch] += weight * (next[ch] - result[ch]);
        if (++s.fadeFrame == s.fadeLength) {
            s.active = 1 - s.active; s.fading = s.commonControls = s.commonPrefix = false;
            if (!sameStructure(s.target, s.endpoints[s.active].current)) s.startFade();
        }
    }
    if (!finite(result)) { reset(); return {}; }
    return result;
}
void Candidate::reset() noexcept { state->clear(); }
#else
Candidate::Candidate() : state(std::make_unique<State>()) {}
Candidate::~Candidate() = default;
bool Candidate::prepare(double rate, unsigned factor) noexcept {
    if (!validRate(rate) || (factor != 1 && factor != 2 && factor != 4 && factor != 8 && factor != 64)) return false;
    try {
        auto replacement = std::make_unique<State>();
        replacement->target = state->target; replacement->current = state->target; replacement->admitted = state->admitted;
        replacement->targetLogHz = state->targetLogHz;
        replacement->rate = rate; replacement->highRate = rate * factor; replacement->factor = factor;
        replacement->wetUp.prepare(factor); replacement->dryUp.prepare(factor); replacement->down.prepare(factor);
        replacement->dcAlpha = std::exp(-2 * pi * 5 / replacement->highRate);
        replacement->sagAttack = std::exp(-1 / (replacement->highRate * .002));
        replacement->sagRelease = std::exp(-1 / (replacement->highRate * .080));
        replacement->shelfBasis = designBasis(replacement->highRate, std::min(1000., .45 * rate), factor == 4);
        replacement->coefficients = &resources(); replacement->prepared = true; replacement->updateCoefficients();
        replacement->useWideNetwork = factor == 4 && juce::SystemStats::hasAVX() && wide_network::operatingSystemSupportsAvx();
        state.swap(replacement); return true;
    } catch (...) { return false; }
}
bool Candidate::setSettings(const SaturationSettings& next) noexcept {
    if (!validateSaturation(next)) return false;
    if (state->admitted && next == state->target) return true;
    const bool first = !state->admitted;
    state->cacheTargetFrequencies(next, first);
    state->target = next; state->admitted = true;
    if (first || !state->prepared) {
        state->current = next; state->ramp = 0; if (state->prepared) state->updateCoefficients();
    } else {
        state->current.mode = next.mode; state->current.driveCompensation = next.driveCompensation;
        state->current.preLowCutEnabled = next.preLowCutEnabled; state->current.postHighCutEnabled = next.postHighCutEnabled;
        for (std::size_t n = 0; n < 4; ++n) { state->current.lanes[n].character = next.lanes[n].character; state->current.lanes[n].bypassed = next.lanes[n].bypassed; }
        state->ramp = unsigned(std::max(1., std::round(.020 * state->rate)));
    }
    return true;
}
Candidate::Frame Candidate::process(Frame input) noexcept {
    AIRACK_OPTIMIZATION_SCOPE(candidateScope, candidate);
    auto& s = *state; if (!s.prepared) return {};
    if (!finite(input) || std::max(std::abs(input[0]), std::abs(input[1])) > std::numeric_limits<double>::max() / 1e12) { reset(); return {}; }
    return s.factor == 4 ? s.processPrepared<4>(input) : s.processPrepared<64>(input);
}
void Candidate::reset() noexcept { state->clear(); }
#endif

#if defined(AIRACK_SATURATION_OPTIMIZATION_TEST)
int runSaturationCommonControlChecks() {
    Candidate candidate;
    SaturationSettings setting;
    int failures = !(candidate.prepare(8000, 4) && candidate.setSettings(setting));
    setting.inputTrimDb = 6; setting.lowCrossoverHz = 120;
    setting.lanes[0].driveDb = 12; setting.lanes[0].character = SaturationCharacter::glow;
    failures += !candidate.setSettings(setting);
    optimizationControlMathSteps = optimizationControlCopies = 0;
    for (unsigned n = 0; n < 12; ++n) candidate.process({ .01, -.007 });
    std::cout << "COMMON_CONTROLS public_moving_frames=12 original_math_steps=" << optimizationControlMathSteps
        << " copies=" << optimizationControlCopies << " required_math_steps=12 required_copies=12\n";
    failures += optimizationControlMathSteps != 12 || optimizationControlCopies != 12;
    for (auto mode : { SaturationMode::fullBand, SaturationMode::threeBand }) for (bool moving : { false, true }) {
        Candidate prefixCandidate; SaturationSettings initial; initial.mode = mode;
        failures += !(prefixCandidate.prepare(8000, 4) && prefixCandidate.setSettings(initial));
        auto next = initial; for (auto& lane : next.lanes) lane.character = SaturationCharacter::glow;
        if (moving) { next.inputTrimDb = 6; next.lowCrossoverHz = 120; next.lanes[0].driveDb = 12; }
        failures += !prefixCandidate.setSettings(next);
        optimizationControlMathSteps = optimizationControlCopies = optimizationPrefixExecutions = optimizationPrefixCopies = 0;
        const auto savedProfile = optimizationProfile; const bool savedSampling = optimizationSampling;
        optimizationProfile = {}; optimizationSampling = true;
        for (unsigned n = 0; n < 12; ++n) prefixCandidate.process({ .01, -.007 });
        const auto profile = optimizationProfile; optimizationProfile = savedProfile; optimizationSampling = savedSampling;
        std::cout << "COMMON_PREFIX public_frames=12 mode=" << unsigned(mode) << " moving=" << moving
            << " original_prefixes=" << optimizationPrefixExecutions << " copies=" << optimizationPrefixCopies
            << " required_prefixes=12 required_copies=12\n";
        failures += optimizationPrefixExecutions != 12 || optimizationPrefixCopies != 12;
        failures += optimizationControlMathSteps != (moving ? 12u : 0u) || optimizationControlCopies != (moving ? 12u : 0u);
        failures += !*optimizationControlObservation.prefix || optimizationControlObservation.endpoints[*optimizationControlObservation.active].admitted;
        failures += profile.calls[unsigned(OptimizationStage::prefix)] != 24 || profile.calls[unsigned(OptimizationStage::returning)] != 24
            || profile.calls[unsigned(OptimizationStage::lane)] != (mode == SaturationMode::fullBand ? 24u : 72u)
            || profile.calls[unsigned(OptimizationStage::crossover)] != (mode == SaturationMode::threeBand
                && juce::SystemStats::hasAVX() && wide_network::operatingSystemSupportsAvx() ? 24u : 0u);
    }
    std::size_t checks = 0, mismatchCases = 0, directFrames = 0, publicFrames = 0;
    const char* section = "eligibility";
    unsigned contextFactor = 0, contextFrame = 0;
    const auto check = [&](bool okay) { ++checks; if (!okay) { ++failures; if (failures < 12) std::cout << "COMMON_CONTROL_FAIL " << section << " check=" << checks << " factor=" << contextFactor << " frame=" << contextFrame << '\n'; } };
    const auto sameArray = [](const auto& a, const auto& b) { return std::memcmp(a.data(), b.data(), sizeof(a)) == 0; };
    const auto sameBiquad = [&](const Biquad& a, const Biquad& b) {
        return sameArray(a.z1, b.z1) && sameArray(a.z2, b.z2) && sameArray(a.z1Low, b.z1Low) && sameArray(a.z2Low, b.z2Low);
    };
    const auto sameNetwork = [&](const Network& a, const Network& b) {
        bool same = sameBiquad(a.lowCompensation, b.lowCompensation);
        for (std::size_t n = 0; n < 2; ++n) same = same && sameBiquad(a.low.low[n], b.low.low[n])
            && sameBiquad(a.low.high[n], b.low.high[n]) && sameBiquad(a.high.low[n], b.high.low[n]) && sameBiquad(a.high.high[n], b.high.high[n]);
        return same;
    };
    const auto sameResampler = [&](const Resampler& a, const Resampler& b) {
        for (std::size_t n = 0; n < 3; ++n) for (std::size_t ch = 0; ch < 2; ++ch)
            if (!sameArray(a.iir[n][ch].direct, b.iir[n][ch].direct) || !sameArray(a.iir[n][ch].delayed, b.iir[n][ch].delayed)
                || !sameControlBits(a.iir[n][ch].held, b.iir[n][ch].held)) return false;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
        for (std::size_t n = 0; n < 2; ++n)
            if (!sameArray(a.stereoIir[n].direct, b.stereoIir[n].direct) || !sameArray(a.stereoIir[n].delayed, b.stereoIir[n].delayed)
                || !sameArray(a.stereoIir[n].held, b.stereoIir[n].held)) return false;
#endif
        if (bool(a.fir) != bool(b.fir)) return false;
        if (a.fir) for (std::size_t n = 0; n < 6; ++n) for (std::size_t ch = 0; ch < 2; ++ch)
            if (!sameArray((*a.fir)[n][ch].history, (*b.fir)[n][ch].history) || (*a.fir)[n][ch].head != (*b.fir)[n][ch].head) return false;
        return true;
    };
    const auto sameAudio = [&](const Endpoint& a, const Endpoint& b) {
        if (!sameResampler(a.wetUp, b.wetUp) || !sameResampler(a.dryUp, b.dryUp) || !sameResampler(a.down, b.down)
            || !sameNetwork(a.wetNetwork, b.wetNetwork) || !sameNetwork(a.dryNetwork, b.dryNetwork)
            || !sameArray(a.previousDry, b.previousDry) || !sameBiquad(a.preCut, b.preCut) || !sameBiquad(a.postCut, b.postCut)) return false;
        const auto sameWide = [&](const auto& x, const auto& y) { return sameArray(x.z1, y.z1) && sameArray(x.z2, y.z2); };
        if (!sameWide(a.wideNetwork.lowCompensation, b.wideNetwork.lowCompensation)) return false;
        for (std::size_t n = 0; n < 2; ++n)
            if (!sameWide(a.wideNetwork.lowLP[n], b.wideNetwork.lowLP[n]) || !sameWide(a.wideNetwork.lowHP[n], b.wideNetwork.lowHP[n])
                || !sameWide(a.wideNetwork.highLP[n], b.wideNetwork.highLP[n]) || !sameWide(a.wideNetwork.highHP[n], b.wideNetwork.highHP[n])) return false;
        for (std::size_t n = 0; n < 4; ++n) {
            const auto& x = a.lanes[n]; const auto& y = b.lanes[n];
            if (!sameBiquad(x.lowShelf, y.lowShelf) || !sameBiquad(x.highShelf, y.highShelf)
                || !sameArray(x.previousX, y.previousX) || !sameArray(x.previousH, y.previousH) || !sameArray(x.previousU, y.previousU)
                || !sameArray(x.previousDifference, y.previousDifference) || !sameArray(x.dc, y.dc) || !sameArray(x.previousPrimitive, y.previousPrimitive)
                || x.primitiveKind != y.primitiveKind || x.primitiveCharacter != y.primitiveCharacter || x.tiltCharged != y.tiltCharged
                || !sameControlBits(x.envelope, y.envelope)) return false;
        }
        return true;
    };
    const auto sameEndpoint = [&](const Endpoint& a, const Endpoint& b) {
        if (!sameContinuousControls(a.current, b.current) || !sameContinuousControls(a.target, b.target)
            || !sameContinuousControls(a.coefficientSettings, b.coefficientSettings) || !sameStructure(a.current, b.current)
            || !sameStructure(a.target, b.target) || !sameStructure(a.coefficientSettings, b.coefficientSettings)
            || a.ramp != b.ramp || a.factor != b.factor || a.prepared != b.prepared || a.admitted != b.admitted
            || a.coefficientsValid != b.coefficientsValid || a.coefficients != b.coefficients || a.failed != b.failed || a.useWideNetwork != b.useWideNetwork
            || !sameControlBits(a.rate, b.rate) || !sameControlBits(a.highRate, b.highRate) || !sameControlBits(a.dcAlpha, b.dcAlpha)
            || !sameControlBits(a.sagAttack, b.sagAttack) || !sameControlBits(a.sagRelease, b.sagRelease)
            || !sameControlBits(a.compensationWet, b.compensationWet) || !sameControlBits(a.coefficientCompensation, b.coefficientCompensation)
            || !sameControlBits(a.inputGain, b.inputGain) || !sameControlBits(a.outputGain, b.outputGain)
            || !sameControlBits(a.shelfBasis.cs, b.shelfBasis.cs) || !sameControlBits(a.shelfBasis.alpha, b.shelfBasis.alpha)
            || !sameControlBits(a.coefficientCorners.lowHz, b.coefficientCorners.lowHz) || !sameControlBits(a.coefficientCorners.highHz, b.coefficientCorners.highHz)
            || !sameArray(a.targetLogHz, b.targetLogHz) || !sameArray(a.driveGain, b.driveGain) || !sameArray(a.driveAmount, b.driveAmount)
            || !sameArray(a.wetGain, b.wetGain) || !sameControlCoefficients(a.pre, b.pre) || !sameControlCoefficients(a.post, b.post)
            || !sameControlCoefficients(a.network.lowLP, b.network.lowLP) || !sameControlCoefficients(a.network.lowHP, b.network.lowHP)
            || !sameControlCoefficients(a.network.highLP, b.network.highLP) || !sameControlCoefficients(a.network.highHP, b.network.highHP)
            || !sameControlCoefficients(a.network.highAP, b.network.highAP) || !sameAudio(a, b)) return false;
        for (std::size_t n = 0; n < 4; ++n)
            if (!sameControlCoefficients(a.shelfLow[n], b.shelfLow[n]) || !sameControlCoefficients(a.shelfHigh[n], b.shelfHigh[n])
                || !sameControlBits(a.lanes[n].bypassWet, b.lanes[n].bypassWet)) return false;
        return true;
    };
    const auto readyEndpoint = [](Endpoint& e, const SaturationSettings& s, double rate = 8000, unsigned factor = 4) {
        e.target = e.current = s; e.cacheTargetFrequencies(s, true); e.rate = rate; e.highRate = rate * factor; e.factor = factor;
        e.wetUp.prepare(factor); e.dryUp.prepare(factor); e.down.prepare(factor);
        e.dcAlpha = std::exp(-2 * pi * 5 / e.highRate); e.sagAttack = std::exp(-1 / (e.highRate * .002)); e.sagRelease = std::exp(-1 / (e.highRate * .080));
        e.shelfBasis = designBasis(e.highRate, std::min(1000., .45 * rate), factor == 4); e.coefficients = &resources(); e.prepared = true;
        e.useWideNetwork = factor == 4 && juce::SystemStats::hasAVX() && wide_network::operatingSystemSupportsAvx(); e.clear();
    };
    const auto signal = [](unsigned n) -> Candidate::Frame { return { .031 * std::sin(.17 * n) + .007, -.023 * std::cos(.31 * n) - .003 }; };
    Endpoint source, peer; readyEndpoint(source, {}); readyEndpoint(peer, {}); peer = source;
    check(source.hasCommonControls(peer) && !source.admitted && !peer.admitted);
    const auto mismatch = [&](const auto& change) { peer = source; change(peer); check(!source.hasCommonControls(peer)); ++mismatchCases; };
    for (auto member : { &Endpoint::current, &Endpoint::target, &Endpoint::coefficientSettings }) {
        for (auto field : { &SaturationSettings::inputTrimDb, &SaturationSettings::outputTrimDb, &SaturationSettings::mix,
                &SaturationSettings::lowCrossoverHz, &SaturationSettings::highCrossoverHz, &SaturationSettings::preLowCutHz, &SaturationSettings::postHighCutHz })
            mismatch([&](Endpoint& e) { auto& value = (e.*member).*field; value = std::nextafter(value, std::numeric_limits<float>::infinity()); });
        mismatch([&](Endpoint& e) { (e.*member).driveCompensation = !(e.*member).driveCompensation; });
        for (std::size_t n = 0; n < 4; ++n) {
            for (auto field : { &SaturationLaneSettings::driveDb, &SaturationLaneSettings::preTiltDb, &SaturationLaneSettings::mix, &SaturationLaneSettings::trimDb })
                mismatch([&](Endpoint& e) { auto& value = (e.*member).lanes[n].*field; value = std::nextafter(value, std::numeric_limits<float>::infinity()); });
            mismatch([&](Endpoint& e) { (e.*member).lanes[n].bypassed = !(e.*member).lanes[n].bypassed; });
        }
    }
    for (auto field : { &Endpoint::rate, &Endpoint::highRate, &Endpoint::dcAlpha, &Endpoint::sagAttack, &Endpoint::sagRelease,
            &Endpoint::compensationWet, &Endpoint::coefficientCompensation, &Endpoint::inputGain, &Endpoint::outputGain })
        mismatch([&](Endpoint& e) { e.*field = std::nextafter(e.*field, std::numeric_limits<double>::infinity()); });
    for (auto field : { &Endpoint::prepared, &Endpoint::coefficientsValid, &Endpoint::failed, &Endpoint::useWideNetwork })
        mismatch([&](Endpoint& e) { e.*field = !(e.*field); });
    mismatch([](Endpoint& e) { ++e.ramp; }); mismatch([](Endpoint& e) { e.factor = 2; }); mismatch([](Endpoint& e) { e.coefficients = nullptr; });
    for (auto field : { &DesignBasis::cs, &DesignBasis::alpha }) mismatch([&](Endpoint& e) { e.shelfBasis.*field += .125; });
    for (auto field : { &SaturationCrossovers::lowHz, &SaturationCrossovers::highHz }) mismatch([&](Endpoint& e) { e.coefficientCorners.*field += .125; });
    for (std::size_t n = 0; n < 4; ++n) {
        mismatch([&](Endpoint& e) { e.targetLogHz[n] += .125; }); mismatch([&](Endpoint& e) { e.lanes[n].bypassWet -= .125; });
        mismatch([&](Endpoint& e) { e.driveGain[n] += .125; }); mismatch([&](Endpoint& e) { e.driveAmount[n] += .125; });
    }
    for (auto field : { &Coefficients::b0, &Coefficients::b1, &Coefficients::b2, &Coefficients::a1, &Coefficients::a2 }) {
        for (auto group : { &NetworkCoefficients::lowLP, &NetworkCoefficients::lowHP, &NetworkCoefficients::highLP, &NetworkCoefficients::highHP, &NetworkCoefficients::highAP })
            mismatch([&](Endpoint& e) { (e.network.*group).*field += .125; });
        for (auto group : { &Endpoint::pre, &Endpoint::post }) mismatch([&](Endpoint& e) { (e.*group).*field += .125; });
        for (std::size_t n = 0; n < 4; ++n) {
            mismatch([&](Endpoint& e) { e.shelfLow[n].*field += .125; }); mismatch([&](Endpoint& e) { e.shelfHigh[n].*field += .125; });
        }
    }
    for (auto member : { &Endpoint::current, &Endpoint::target, &Endpoint::coefficientSettings })
        mismatch([&](Endpoint& e) { (e.*member).inputTrimDb = -0.f; });
    peer = source; peer.current.mode = SaturationMode::threeBand; peer.target.preLowCutEnabled = true; peer.coefficientSettings.postHighCutEnabled = true;
    peer.current.lanes[0].character = SaturationCharacter::dense; peer.wetGain[0] = .125; peer.lanes[0].previousU = { .1, -.2 };
    check(source.hasCommonControls(peer));
    section = "direct-copy";
    for (double rate : { 8000., 44100., 44101., 48000., 96000. }) for (auto mode : { SaturationMode::fullBand, SaturationMode::threeBand })
        for (auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) for (bool changeHz : { false, true })
            for (unsigned cuts = 0; cuts < 4; ++cuts) {
            SaturationSettings initial; initial.mode = mode; initial.preLowCutEnabled = cuts & 1; initial.postHighCutEnabled = cuts & 2;
            for (std::size_t n = 0; n < 4; ++n) { initial.lanes[n].character = character; initial.lanes[n].preTiltDb = float(n); }
            Endpoint from, actual, expected, history; readyEndpoint(from, initial, rate); readyEndpoint(actual, initial, rate); readyEndpoint(expected, initial, rate); readyEndpoint(history, initial, rate);
            for (unsigned n = 0; n < 9; ++n) from.processPrepared<4>(signal(n)); actual = from;
            auto next = initial; next.inputTrimDb = 12; next.outputTrimDb = -6; next.mix = .25; next.driveCompensation = false;
            if (changeHz) { next.lowCrossoverHz = 50; next.highCrossoverHz = 3000; next.preLowCutHz = 150; next.postHighCutHz = 1100; }
            for (std::size_t n = 0; n < 4; ++n) { next.lanes[n].driveDb = n < 2 ? 18.f : float(n); next.lanes[n].preTiltDb = n < 2 ? -3.f : -float(n);
                next.lanes[n].mix = .5f; next.lanes[n].trimDb = -float(n); next.lanes[n].bypassed = n % 2 != 0; }
            retarget(from, next); auto destination = next; destination.mode = mode == SaturationMode::fullBand ? SaturationMode::threeBand : SaturationMode::fullBand;
            destination.preLowCutEnabled = !initial.preLowCutEnabled; destination.postHighCutEnabled = !initial.postHighCutEnabled;
            for (auto& lane : destination.lanes) lane.character = SaturationCharacter((unsigned(character) + 1) % 3);
            retarget(actual, destination); expected = actual; check(from.hasCommonControls(actual));
            const unsigned frames = from.ramp + 2;
            for (unsigned n = 0; n < frames; ++n) {
                history = actual; const auto ownRamp = actual.ramp;
                from.advanceControls(); expected.advanceControls(); actual.advanceControlsFrom(from);
                check(sameEndpoint(actual, expected) && sameAudio(actual, history));
                check(actual.ramp == (ownRamp ? ownRamp - 1 : 0));
                if (ownRamp == 1) check(from.ramp == 0 && sameContinuousControls(actual.current, actual.target));
                ++directFrames;
            }
            for (unsigned n = 0; n < 12; ++n) check(sameArray(actual.processPrepared<4>(signal(n)), expected.processPrepared<4>(signal(n))) && sameEndpoint(actual, expected));
        }
    for (float zero : { 0.f, -0.f }) {
        SaturationSettings initial; for (auto& lane : initial.lanes) lane.driveDb = 6;
        Endpoint from, actual, expected; readyEndpoint(from, initial); readyEndpoint(actual, initial); readyEndpoint(expected, initial);
        auto next = initial; for (auto& lane : next.lanes) lane.driveDb = zero;
        retarget(from, next); actual = from; for (auto& lane : next.lanes) lane.character = SaturationCharacter::dense;
        retarget(actual, next); expected = actual; check(from.hasCommonControls(actual));
        for (unsigned n = 0; n < 162; ++n) {
            const auto ramp = actual.ramp; optimizationControlMathSteps = optimizationControlCopies = 0;
            from.advanceControls(); expected.advanceControls(); actual.advanceControlsFrom(from);
            check(sameEndpoint(actual, expected));
            check(optimizationControlMathSteps == (ramp ? 2u : 0u) && optimizationControlCopies == (ramp ? 1u : 0u));
            if (ramp == 1) for (std::size_t lane = 0; lane < 4; ++lane)
                check(sameControlBits(actual.current.lanes[lane].driveDb, zero) && sameControlBits(actual.driveAmount[lane], double(zero)));
            ++directFrames;
        }
    }
    const auto originalRender = [](Endpoint& e, Candidate::Frame input) {
        optimizationOriginalReturn = true;
        optimizationOriginalLane = true;
        const auto result = e.factor == 4 ? e.processPrepared<4>(input) : e.processPrepared<64>(input);
        optimizationOriginalReturn = false; optimizationOriginalLane = false; return result;
    };
    section = "prefix-independent";
    auto dynamicResources = std::make_unique<Resources>(resources()); dynamicResources->fixed4Layout = false;
    dynamicResources->iir[1].up.directCount = 2; dynamicResources->iir[1].up.delayedCount = 2;
    const bool supportsWide = juce::SystemStats::hasAVX() && wide_network::operatingSystemSupportsAvx();
    std::size_t prefixCases = 0, prefixFrames = 0, prefixFutureFrames = 0, wideCases = 0;
    for (double rate : { 8000., 44100., 44101., 48000., 96000. }) for (auto mode : { SaturationMode::fullBand, SaturationMode::threeBand })
        for (auto fromCharacter : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense })
        for (auto toCharacter : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense })
        for (unsigned cuts = 0; cuts < 4; ++cuts) for (bool wide : { false, true }) for (bool dynamic : { false, true })
        for (unsigned ramp : { 0u, 1u, 5u }) {
        if (wide && !supportsWide) continue;
        SaturationSettings initial; initial.mode = mode; initial.preLowCutEnabled = cuts & 1; initial.postHighCutEnabled = cuts & 2;
        initial.preLowCutHz = 70; initial.postHighCutHz = 4000;
        for (std::size_t lane = 0; lane < 4; ++lane) {
            initial.lanes[lane].character = fromCharacter; initial.lanes[lane].driveDb = float(6 + lane);
            initial.lanes[lane].preTiltDb = float(lane); initial.lanes[lane].bypassed = lane % 2 != 0;
        }
        Endpoint from, sourceExpected, actual, expected;
        for (auto* e : { &from, &sourceExpected, &actual, &expected }) readyEndpoint(*e, initial, rate);
        from.useWideNetwork = wide; if (dynamic) from.coefficients = dynamicResources.get();
        for (unsigned n = 0; n < (cuts ? 9u : 0u); ++n) from.processPrepared<4>(signal(n));
        auto next = initial; next.inputTrimDb = 6; next.outputTrimDb = -3; next.mix = .375f;
        next.lowCrossoverHz = 90; next.highCrossoverHz = 2500; next.preLowCutHz = 130; next.postHighCutHz = 3000;
        next.driveCompensation = false;
        for (auto& lane : next.lanes) { lane.driveDb = 18; lane.preTiltDb = -2; lane.bypassed = !lane.bypassed; }
        if (ramp) retarget(from, next);
        from.ramp = ramp; actual = from;
        auto destination = from.target; destination.postHighCutEnabled = !destination.postHighCutEnabled;
        for (auto& lane : destination.lanes) lane.character = toCharacter;
        retarget(actual, destination);
        // Different unused histories must survive the active-prefix synchronization.
        actual.preCut.z1Low = { .125, -.25 }; actual.preCut.z2Low = { -.375, .5 };
        for (auto* up : { &actual.wetUp, &actual.dryUp }) {
            for (auto& stage : up->iir) for (auto& channel : stage) channel.held = .75;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
            for (std::size_t stage = 0; stage < 2; ++stage) {
                up->stereoIir[stage].held = { .625, -.875 };
                const auto& c = from.coefficients->iir[stage].up;
                for (std::size_t n = c.directCount; n < maxIirDirectSections; ++n) up->stereoIir[stage].direct[n] = { .25, -.5 };
                for (std::size_t n = c.delayedCount; n < maxIirDelayedSections; ++n) up->stereoIir[stage].delayed[n] = { -.25, .5 };
            }
            up->iir[0][0].direct[0] = .3125;
#else
            for (std::size_t stage = 0; stage < 2; ++stage) for (auto& channel : up->iir[stage]) {
                const auto& c = from.coefficients->iir[stage].up;
                for (std::size_t n = c.directCount; n < maxIirDirectSections; ++n) channel.direct[n] = .25;
                for (std::size_t n = c.delayedCount; n < maxIirDelayedSections; ++n) channel.delayed[n] = -.25;
            }
#endif
        }
        actual.wetNetwork.low.low[0].z1Low = { .125, -.25 }; actual.dryNetwork.high.high[1].z2Low = { -.125, .25 };
        if (wide || mode == SaturationMode::fullBand) actual.wetNetwork.low.low[0].z1 = { .0625, -.125 };
        if (!wide || mode == SaturationMode::fullBand) actual.wideNetwork.lowLP[0].z1 = { .25, -.5, .75, -1 };
        for (auto* e : { &from, &actual }) {
            const double charge = e == &from ? .015625 : -.03125;
            for (std::size_t lane = 0; lane < 4; ++lane) e->previousDry[lane] = lane % 2 ? Candidate::Frame { -0., 0. } : Candidate::Frame { charge, -2 * charge };
            e->postCut.z1 = { charge, -2 * charge }; e->postCut.z2 = { -charge, 3 * charge };
            e->postCut.z1Low = { -0., 0x1p-60 }; e->postCut.z2Low = { -0x1p-61, 0. };
            for (auto& stage : e->down.iir) for (auto& channel : stage) {
                channel.direct[0] = charge; channel.delayed[0] = -charge; channel.held = -0.;
            }
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
            for (auto& stage : e->down.stereoIir) {
                stage.direct[0] = { charge, -2 * charge }; stage.delayed[0] = { -charge, 3 * charge }; stage.held = { -0., 0. };
            }
#endif
        }
        sourceExpected = from; expected = actual; check(from.hasCommonControls(actual));
        for (unsigned n = 0; n < 8; ++n) {
            Prefix4 published;
            const auto sourceOutput = from.processPrepared<4>(signal(n), nullptr, &published);
            check(sameArray(sourceOutput, originalRender(sourceExpected, signal(n))) && sameEndpoint(from, sourceExpected));
            const auto usedInput = published.wetInput; const auto usedWet = published.wet, usedDry = published.dry;
            std::array<std::array<Candidate::Frame, 4>, 3> usedBandWet;
            std::array<std::array<Candidate::Frame, 3>, 4> usedBandDry;
            if (mode == SaturationMode::threeBand) { usedBandWet = published.bandWet; usedBandDry = published.bandDry; }
            check(sameArray(actual.processPrepared<4>(signal(n), &from, &published), originalRender(expected, signal(n))) && sameEndpoint(actual, expected));
            check(sameArray(published.wetInput, usedInput) && sameArray(published.wet, usedWet) && sameArray(published.dry, usedDry));
            if (mode == SaturationMode::threeBand) check(sameArray(published.bandWet, usedBandWet) && sameArray(published.bandDry, usedBandDry));
            ++prefixFrames;
        }
        for (unsigned n = 8; n < 16; ++n) {
            check(sameArray(actual.processPrepared<4>(signal(n)), originalRender(expected, signal(n))) && sameEndpoint(actual, expected));
            ++prefixFutureFrames;
        }
        ++prefixCases; if (wide) ++wideCases;
    }
    std::cout << "COMMON_PREFIX_INDEPENDENT cases=" << prefixCases << " frames=" << prefixFrames << " future_frames=" << prefixFutureFrames
        << " avx_cases=" << wideCases << " avx_supported=" << supportsWide << " failures=" << failures << '\n';
    section = "prefix-public-latch";
    for (unsigned change = 0; change < 3; ++change) {
        Candidate actual; SaturationSettings initial;
        check(actual.prepare(8000, 4) && actual.setSettings(initial)); actual.process(signal(0));
        auto next = initial;
        if (change == 0) next.mode = SaturationMode::threeBand;
        if (change == 1) next.preLowCutEnabled = true;
        if (change == 2) next.postHighCutEnabled = true;
        check(actual.setSettings(next));
        auto rejected = next; rejected.mix = std::numeric_limits<float>::quiet_NaN();
        check(!actual.setSettings(rejected) && !actual.prepare(0, 4));
        optimizationPrefixExecutions = optimizationPrefixCopies = 0;
        for (unsigned n = 0; n < 12; ++n) actual.process(signal(n));
        check(*optimizationControlObservation.common && *optimizationControlObservation.prefix == (change == 2));
        check(optimizationPrefixExecutions == (change == 2 ? 12u : 24u) && optimizationPrefixCopies == (change == 2 ? 12u : 0u));
    }
    {
        Candidate actual; SaturationSettings initial;
        check(actual.prepare(8000, 4) && actual.setSettings(initial)); actual.process(signal(0));
        auto next = initial; next.lanes[0].character = SaturationCharacter::glow; check(actual.setSettings(next));
        actual.process(signal(1)); check(*optimizationControlObservation.prefix);
        auto* endpoints = const_cast<Endpoint*>(optimizationControlObservation.endpoints);
        const auto active = *optimizationControlObservation.active;
        endpoints[1 - active].targetLogHz[0] = std::nextafter(endpoints[active].targetLogHz[0], 1e9);
        next.lanes[0].character = SaturationCharacter::dense; check(actual.setSettings(next));
        optimizationPrefixExecutions = optimizationPrefixCopies = 0; actual.process(signal(2));
        check(!*optimizationControlObservation.common && !*optimizationControlObservation.prefix
            && optimizationPrefixExecutions == 2 && optimizationPrefixCopies == 0);
        endpoints[1 - active].targetLogHz = endpoints[active].targetLogHz;
        check(endpoints[active].hasCommonControls(endpoints[1 - active]));
        next.lanes[1].character = SaturationCharacter::dense; check(actual.setSettings(next));
        optimizationPrefixExecutions = optimizationPrefixCopies = 0; actual.process(signal(3));
        check(!*optimizationControlObservation.common && !*optimizationControlObservation.prefix
            && optimizationPrefixExecutions == 2 && optimizationPrefixCopies == 0);
        for (unsigned n = 4; n <= 160; ++n) actual.process(signal(n));
        check(*optimizationControlObservation.fading && *optimizationControlObservation.common && *optimizationControlObservation.prefix);
        optimizationPrefixExecutions = optimizationPrefixCopies = 0; actual.process(signal(161));
        check(optimizationPrefixExecutions == 1 && optimizationPrefixCopies == 1);
        check(actual.prepare(8000, 4) && !optimizationControlObservation.endpoints);
        actual.process(signal(162)); check(!*optimizationControlObservation.prefix && !*optimizationControlObservation.fading);
    }
    section = "public-independent";
    for (unsigned factor : { 1u, 2u, 4u, 8u, 64u }) {
        contextFactor = factor;
        Candidate actual; SaturationSettings latest; latest.inputTrimDb = 6;
        check(actual.prepare(8000, factor) && actual.setSettings(latest));
        std::array<Endpoint, 2> refs; for (auto& e : refs) readyEndpoint(e, latest, 8000, factor);
        unsigned active = 0, fade = 0; bool fading = false;
        const auto clear = [&] { for (auto& e : refs) { e.cacheTargetFrequencies(latest, true); e.target = latest; e.clear(); } active = fade = 0; fading = false; };
        const auto start = [&] {
            auto& from = refs[active]; auto& to = refs[1 - active]; to = from;
            if (from.current.mode != latest.mode) { to.wetNetwork = {}; to.dryNetwork = {}; to.wideNetwork = {}; to.previousDry = {};
                for (auto& lane : to.lanes) { const auto bypass = lane.bypassWet; lane = {}; lane.bypassWet = bypass; } }
            retarget(to, latest); fade = 0; fading = true;
        };
        const auto settings = [&](const SaturationSettings& next) {
            const bool accepted = actual.setSettings(next); check(accepted == validateSaturation(next)); if (!accepted || next == latest) return;
            latest = next;
            for (unsigned n = 0; n < (fading ? 2u : 1u); ++n) { auto& e = refs[n == 0 ? active : 1 - active]; auto continuous = next; preserveStructure(continuous, e.current); retarget(e, continuous); }
            if (!fading && !sameStructure(latest, refs[active].current)) start();
        };
        const auto render = [&](Endpoint& e, Candidate::Frame input) { return originalRender(e, input); };
        bool sawEligible = false, sawSignedFallback = false, sawNoRejoin = false;
        for (unsigned n = 0; n < 1000; ++n) {
            contextFrame = n;
            if (n == 0) { auto next = latest; next.lanes[0].character = SaturationCharacter::glow; settings(next); }
            if (n == 100) { auto next = latest; next.inputTrimDb = +0.f; next.lanes[0].driveDb = 9; settings(next); }
            if (n == 110) { auto next = latest; next.inputTrimDb = -0.f; next.lanes[0].character = SaturationCharacter::dense; settings(next); }
            if (n == 280) { auto next = latest; next.lanes[0].driveDb = 15; settings(next); }
            if (n == 330 || n == 360 || n == 410 || n == 450) {
                auto next = latest; next.mode = n % 90 ? SaturationMode::threeBand : SaturationMode::fullBand;
                next.preLowCutEnabled = n % 80 != 0; next.postHighCutEnabled = !next.preLowCutEnabled;
                next.driveCompensation = !latest.driveCompensation; next.inputTrimDb = 6; next.outputTrimDb = -3; next.lowCrossoverHz = float(n);
                for (std::size_t lane = 0; lane < 4; ++lane) { next.lanes[lane].character = SaturationCharacter((n + lane) % 3); next.lanes[lane].bypassed = lane % 2 == 0;
                    next.lanes[lane].driveDb = float(lane + n % 30); next.lanes[lane].preTiltDb = float(lane); next.lanes[lane].mix = .5f; next.lanes[lane].trimDb = -float(lane); }
                settings(next);
            }
            if (n == 500) { auto next = latest; next.mix = std::numeric_limits<float>::quiet_NaN(); settings(next); check(!actual.prepare(0, factor)); }
            if (n == 550) { actual.reset(); clear(); }
            if (n == 570) { check(actual.prepare(8000, factor)); for (auto& e : refs) { readyEndpoint(e, latest, 8000, factor); e.admitted = true; } active = fade = 0; fading = false; check(!optimizationControlObservation.endpoints); }
            const auto input = n == 530 ? Candidate::Frame { invalid, 0 } : signal(n);
            Candidate::Frame expected {};
            if (!finite(input)) clear();
            else {
                expected = render(refs[active], input);
                if (fading) {
                    const auto next = render(refs[1 - active], input); const double weight = double(fade) / 159;
                    for (std::size_t ch = 0; ch < 2; ++ch) expected[ch] += weight * (next[ch] - expected[ch]);
                    if (++fade == 160) { active = 1 - active; fading = false; if (!sameStructure(latest, refs[active].current)) start(); }
                }
            }
            optimizationControlMathSteps = optimizationControlCopies = optimizationPrefixExecutions = optimizationPrefixCopies = 0;
            check(sameArray(actual.process(input), expected));
            const auto& observed = optimizationControlObservation;
            check(observed.endpoints && *observed.active == active && *observed.fading == fading && *observed.fadeFrame == fade);
            const bool endpointsMatch = sameEndpoint(observed.endpoints[0], refs[0]) && sameEndpoint(observed.endpoints[1], refs[1]);
            check(endpointsMatch);
            if (factor == 4 && n > 100 && n < 159) { check(*observed.common && optimizationControlMathSteps == 1 && optimizationControlCopies == 1); sawEligible = true; }
            if (factor == 4 && n > 100 && n < 159) check(*observed.prefix && optimizationPrefixExecutions == 1 && optimizationPrefixCopies == 1);
            if (factor == 4 && n == 159) {
                check(!*observed.common && refs[active].ramp != 0
                    && !sameControlBits(refs[active].target.inputTrimDb, refs[1 - active].target.inputTrimDb)); sawSignedFallback = true;
            }
            if (factor == 4 && n >= 160 && n < 260) check(!*observed.common && optimizationControlMathSteps == 2 && optimizationControlCopies == 0);
            if (factor == 4 && n >= 160 && n < 260) check(!*observed.prefix && optimizationPrefixExecutions == 2 && optimizationPrefixCopies == 0);
            if (factor == 4 && n == 259) check(!sameControlBits(observed.endpoints[active].current.inputTrimDb,
                observed.endpoints[1 - active].current.inputTrimDb) && observed.endpoints[active].ramp == 0 && observed.endpoints[1 - active].ramp == 0);
            if (factor == 4 && n >= 280 && n < 319) { check(!*observed.common && optimizationControlMathSteps == 2 && optimizationControlCopies == 0); sawNoRejoin = true; }
            if (factor == 4 && n >= 280 && n < 319) check(!*observed.prefix && optimizationPrefixExecutions == 2 && optimizationPrefixCopies == 0);
            if (factor == 4 && n == 330) check(*observed.common && optimizationControlMathSteps == 1 && optimizationControlCopies == 1);
            if (factor != 4) check(!*observed.common && optimizationControlCopies == 0);
            if (factor != 4) check(!*observed.prefix && optimizationPrefixCopies == 0);
            if (n == 530 || n == 550 || n == 570) check(!*observed.common);
            if (n == 530 || n == 550 || n == 570) check(!*observed.prefix);
            ++publicFrames;
        }
        check(factor != 4 || (sawEligible && sawSignedFallback && sawNoRejoin));
    }
    section = "failed-output";
    {
        Candidate actual; check(actual.prepare(8000, 4) && actual.setSettings({})); actual.process(signal(0));
        auto next = SaturationSettings {}; next.lanes[0].character = SaturationCharacter::glow; next.inputTrimDb = 6; check(actual.setSettings(next));
        auto* endpoints = const_cast<Endpoint*>(optimizationControlObservation.endpoints);
        endpoints[0].current.preLowCutEnabled = true; endpoints[0].preCut.z1 = { invalid, invalid };
        optimizationControlMathSteps = optimizationControlCopies = optimizationPrefixExecutions = optimizationPrefixCopies = 0;
        check(sameArray(actual.process(signal(1)), Candidate::Frame {}) && !*optimizationControlObservation.common && optimizationControlCopies == 0);
        check(!*optimizationControlObservation.fading && endpoints[0].ramp == 0 && endpoints[1].ramp == 0);
        check(!*optimizationControlObservation.prefix && optimizationPrefixExecutions == 1 && optimizationPrefixCopies == 0);
    }
    {
        std::size_t zeroCases = 0, zeroFrames = 0, zeroFailures = 0;
        for (double rate : { 8000., 44100., 44101., 48000., 96000. }) for (auto mode : { SaturationMode::fullBand, SaturationMode::threeBand })
            for (auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense })
            for (unsigned cuts = 0; cuts < 4; ++cuts) for (bool wide : { false, true }) for (bool dynamic : { false, true })
            for (bool negative : { false, true }) {
                if (wide && !supportsWide) continue;
                SaturationSettings settings; settings.mode = mode; settings.preLowCutEnabled = cuts & 1; settings.postHighCutEnabled = cuts & 2;
                settings.driveCompensation = false; for (auto& lane : settings.lanes) lane.character = character;
                Endpoint actual; readyEndpoint(actual, settings, rate); actual.useWideNetwork = wide;
                if (dynamic) actual.coefficients = dynamicResources.get();
                const double signedZero = negative ? -0. : 0.;
                for (auto& previous : actual.previousDry) previous = { signedZero, -signedZero };
                actual.postCut.z1 = actual.postCut.z2 = { -signedZero, signedZero };
                for (auto& stage : actual.down.iir) for (auto& channel : stage) { channel.direct[0] = signedZero; channel.delayed[0] = -signedZero; }
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
                for (auto& stage : actual.down.stereoIir) { stage.direct[0] = { signedZero, -signedZero }; stage.delayed[0] = { -signedZero, signedZero }; }
#endif
                Endpoint expected; expected = actual;
                for (unsigned n = 0; n < 12; ++n) {
                    const auto input = n < 4 ? Candidate::Frame { signedZero, -signedZero } : signal(n);
                    const auto output = actual.processPrepared<4>(input), reference = originalRender(expected, input);
                    zeroFailures += !sameArray(output, reference) || !sameEndpoint(actual, expected); ++zeroFrames;
                }
                ++zeroCases;
            }
        std::size_t nonfiniteCases = 0, nonfiniteFailures = 0;
        for (bool originalReturn : { false, true }) for (std::uint64_t bits : { 0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff8000000000123ULL, 0x7ff0000000000001ULL })
            for (std::size_t channel = 0; channel < 2; ++channel) for (unsigned where = 0; where < 5; ++where) {
                Candidate actual; SaturationSettings latest;
                bool okay = actual.prepare(8000, 4) && actual.setSettings(latest); actual.process(signal(0));
                latest.lanes[0].character = SaturationCharacter::glow;
                if (where == 2) latest.postHighCutEnabled = true;
                okay = actual.setSettings(latest) && okay;
                auto* endpoints = const_cast<Endpoint*>(optimizationControlObservation.endpoints);
                const double value = std::bit_cast<double>(bits); auto input = signal(1);
                if (where == 0) input[channel] = value;
                if (where == 1) { endpoints[0].current.preLowCutEnabled = true; endpoints[0].preCut.z1[channel] = value; }
                if (where == 2) endpoints[1].postCut.z1[channel] = value;
                if (where == 3) endpoints[0].outputGain = value;
                if (where == 4) {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
                    endpoints[0].down.stereoIir[1].direct[0][channel] = value;
#else
                    endpoints[0].down.iir[1][channel].direct[0] = value;
#endif
                }
                std::array<Endpoint, 2> expected; for (std::size_t n = 0; n < 2; ++n) {
                    expected[n] = endpoints[n]; expected[n].cacheTargetFrequencies(latest, true); expected[n].target = latest; expected[n].clear();
                }
                optimizationPrefixExecutions = optimizationPrefixCopies = 0;
                optimizationOriginalReturn = originalReturn;
                const auto failedOutput = actual.process(input); optimizationOriginalReturn = false;
                const bool zeroOutput = sameArray(failedOutput, Candidate::Frame {});
                okay = zeroOutput && okay;
                const auto& observed = optimizationControlObservation;
                okay = okay && !*observed.fading && !*observed.common && !*observed.prefix && *observed.active == 0 && *observed.fadeFrame == 0
                    && sameEndpoint(endpoints[0], expected[0]) && sameEndpoint(endpoints[1], expected[1]);
                okay = okay && optimizationPrefixExecutions == (where == 0 ? 0u : 1u) && optimizationPrefixCopies == (where == 2 ? 1u : 0u);
                const auto reference = originalRender(expected[0], signal(2));
                if (expected[0].failed) for (auto& e : expected) { e.cacheTargetFrequencies(latest, true); e.target = latest; e.clear(); }
                okay = sameArray(actual.process(signal(2)), reference) && sameEndpoint(endpoints[0], expected[0]) && sameEndpoint(endpoints[1], expected[1]) && okay;
                if (!okay) std::cout << "STEREO_RETURN_NONFINITE_FAIL original=" << originalReturn << " where=" << where
                    << " bits=" << std::hex << bits << std::dec << " channel=" << channel << " zero_output=" << zeroOutput
                    << " fading=" << *observed.fading << " active=" << *observed.active << " prefix_executions=" << optimizationPrefixExecutions
                    << " prefix_copies=" << optimizationPrefixCopies << '\n';
                nonfiniteFailures += !okay; ++nonfiniteCases;
            }
        failures += int(nonfiniteFailures);
        std::cout << "STEREO_RETURN_NONFINITE cases=" << nonfiniteCases << " failures=" << nonfiniteFailures << '\n';
        failures += int(zeroFailures);
        std::cout << "STEREO_RETURN_ZERO cases=" << zeroCases << " frames=" << zeroFrames << " failures=" << zeroFailures << '\n';
    }
    std::cout << "COMMON_CONTROL_CHECKS checks=" << checks << " mismatch_cases=" << mismatchCases << " direct_frames=" << directFrames
        << " public_frames=" << publicFrames << " failures=" << failures << '\n';
    return failures;
}
#endif

#if defined(AIRACK_SATURATION_NUMERICAL_TEST)
std::span<const SaturationNumericalRecord> saturationNumericalRecords() noexcept { return numericalRecords; }
void saturationNumericalConstants(double& direct, double& prepared, double& normalizer, std::size_t& zeroHalfWidths) noexcept {
    direct = std::tanh(.35); prepared = preparedConstants.glowBias;
    normalizer = 1 - prepared * prepared; zeroHalfWidths = numericalZeroHalfWidths;
}
int runSaturationProductStateIntegrationChecks() {
    const bool collected = numericalCollectRecords; numericalCollectRecords = false;
    int failures = 0; std::size_t checks = 0;
    const char* section = "biquad";
    const auto check = [&](bool okay) {
        ++checks;
        if (!okay) { ++failures; std::cout << "product_state_FAIL section=" << section << " check=" << checks << '\n'; }
    };
    const auto same = [](double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); };
    const auto sameFrame = [&](const auto& a, const auto& b) { return same(a[0], b[0]) && same(a[1], b[1]); };
    const auto sameBiquad = [&](const Biquad& a, const Biquad& b) {
        return sameFrame(a.z1, b.z1) && sameFrame(a.z2, b.z2) && sameFrame(a.z1Low, b.z1Low) && sameFrame(a.z2Low, b.z2Low);
    };
    const auto charge = [](Biquad& b) {
        b.z1 = { .125, -.1875 }; b.z2 = { -.0625, .09375 };
        b.z1Low = { 0x1p-60, -0x1p-59 }; b.z2Low = { -0x1p-61, 0x1p-60 };
    };
    const auto expectedBiquad = [](Biquad& b, Candidate::Frame x, const Coefficients& c) {
        for (std::size_t ch = 0; ch < 2; ++ch) {
            product_state::State reference {{b.z1[ch], b.z1Low[ch]}, {b.z2[ch], b.z2Low[ch]}, {}};
            x[ch] = reference.step(x[ch], {c.b0, c.b1, c.b2, c.a1, c.a2});
            b.z1[ch] = reference.z1.h; b.z1Low[ch] = reference.z1.l;
            b.z2[ch] = reference.z2.h; b.z2Low[ch] = reference.z2.l;
        }
        return x;
    };
    const auto signal = [](unsigned n) -> Candidate::Frame {
        return { .031 * std::sin(.17 * n) + .007, -.023 * std::cos(.31 * n) - .003 };
    };
    const Coefficients distinct { .3125, -.1875, .09375, -.42, .15 };
    Biquad actual, expected, ordinary, ordinaryExpected;
    charge(actual); expected = actual; charge(ordinary); ordinaryExpected = ordinary;
    std::size_t chargedFrames = 0;
    for (unsigned n = 0; n < 4096; ++n) {
        const auto input = signal(n);
        check(sameFrame(actual.process<true>(input, distinct), expectedBiquad(expected, input, distinct)) && sameBiquad(actual, expected));
        auto control = input;
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double y = distinct.b0 * control[ch] + ordinaryExpected.z1[ch];
            ordinaryExpected.z1[ch] = distinct.b1 * control[ch] - distinct.a1 * y + ordinaryExpected.z2[ch];
            ordinaryExpected.z2[ch] = distinct.b2 * control[ch] - distinct.a2 * y; control[ch] = y;
        }
        check(sameFrame(ordinary.process(input, distinct), control) && sameBiquad(ordinary, ordinaryExpected));
        if (actual.z1Low[0] != 0 && actual.z1Low[1] != 0 && actual.z2Low[0] != 0 && actual.z2Low[1] != 0) ++chargedFrames;
    }
    check(chargedFrames > 4000);
    section = "networks";
    const auto visitNetwork = [](Network& network, const auto& visit) {
        for (auto* split : { &network.low, &network.high }) {
            for (auto& b : split->low) visit(b);
            for (auto& b : split->high) visit(b);
        }
        visit(network.lowCompensation);
    };
    const auto sameNetwork = [&](const Network& a, const Network& b) {
        bool okay = sameBiquad(a.lowCompensation, b.lowCompensation);
        for (std::size_t n = 0; n < 2; ++n)
            okay = okay && sameBiquad(a.low.low[n], b.low.low[n]) && sameBiquad(a.low.high[n], b.low.high[n])
                && sameBiquad(a.high.low[n], b.high.low[n]) && sameBiquad(a.high.high[n], b.high.high[n]);
        return okay;
    };
    const auto expectedSplit = [&](Split& split, Candidate::Frame input, const Coefficients& lp, const Coefficients& hp) {
        auto low = input, high = input;
        for (std::size_t n = 0; n < 2; ++n) { low = expectedBiquad(split.low[n], low, lp); high = expectedBiquad(split.high[n], high, hp); }
        return std::array { low, high };
    };
    const auto expectedNetwork = [&](Network& network, Candidate::Frame input, const NetworkCoefficients& c) {
        const auto lower = expectedSplit(network.low, input, c.lowLP, c.lowHP);
        const auto upper = expectedSplit(network.high, lower[1], c.highLP, c.highHP);
        return std::array { expectedBiquad(network.lowCompensation, lower[0], c.highAP), upper[0], upper[1] };
    };
    Network wet, dry, wetExpected, dryExpected;
    visitNetwork(wet, charge); visitNetwork(dry, charge); wetExpected = wet; dryExpected = dry;
    NetworkCoefficients networkCoefficients;
    const auto lower = designBasis(48000. * 64, 180., false), upper = designBasis(48000. * 64, 3500., false);
    networkCoefficients.lowLP = design(Filter::low, lower); networkCoefficients.lowHP = design(Filter::high, lower);
    networkCoefficients.highLP = design(Filter::low, upper); networkCoefficients.highHP = design(Filter::high, upper);
    networkCoefficients.highAP = design(Filter::allpass, upper);
    for (unsigned n = 0; n < 512; ++n) {
        const auto input = signal(n), other = Candidate::Frame { input[1], -input[0] };
        const auto w = wet.process<true>(input, networkCoefficients), we = expectedNetwork(wetExpected, input, networkCoefficients);
        const auto d = dry.process<true>(other, networkCoefficients), de = expectedNetwork(dryExpected, other, networkCoefficients);
        for (std::size_t band = 0; band < 3; ++band) check(sameFrame(w[band], we[band]) && sameFrame(d[band], de[band]));
        check(sameNetwork(wet, wetExpected) && sameNetwork(dry, dryExpected));
    }
    section = "shelves";
    const auto& r = resources(); Lane lane;
    charge(lane.lowShelf); charge(lane.highShelf);
    auto lowExpected = lane.lowShelf, highExpected = lane.highShelf;
    SaturationLaneSettings laneSettings; laneSettings.preTiltDb = 6; laneSettings.driveDb = 18;
    const auto basis = designBasis(48000. * 64, 1000., false);
    const auto low = design(Filter::lowShelf, basis, -3), high = design(Filter::highShelf, basis, 3);
    for (unsigned n = 0; n < 512; ++n) {
        const auto input = signal(n), h = expectedBiquad(highExpected, expectedBiquad(lowExpected, input, low), high);
        const auto y = lane.process<false, true>(input, laneSettings, low, high, gain(18), .5, 1, .999, .99, .9999, r.primitiveCurve, r.gainCurve);
        check(finite(y) && sameFrame(lane.previousH, h) && sameBiquad(lane.lowShelf, lowExpected) && sameBiquad(lane.highShelf, highExpected));
    }
    laneSettings.preTiltDb = 0;
    lane.process<false, true>(signal(512), laneSettings, low, high, gain(18), .5, 1, .999, .99, .9999, r.primitiveCurve, r.gainCurve);
    check(!lane.tiltCharged && sameBiquad(lane.lowShelf, Biquad {}) && sameBiquad(lane.highShelf, Biquad {}));
    section = "endpoint-copy-reset-warm-fade";
    const auto readyEndpoint = [&](Endpoint& endpoint, const SaturationSettings& setting, double rate, unsigned factor) {
        endpoint.target = endpoint.current = setting; endpoint.admitted = true;
        endpoint.cacheTargetFrequencies(setting, true); endpoint.rate = rate; endpoint.highRate = rate * factor; endpoint.factor = factor;
        endpoint.wetUp.prepare(factor); endpoint.dryUp.prepare(factor); endpoint.down.prepare(factor);
        endpoint.dcAlpha = std::exp(-2 * pi * 5 / endpoint.highRate);
        endpoint.sagAttack = std::exp(-1 / (endpoint.highRate * .002)); endpoint.sagRelease = std::exp(-1 / (endpoint.highRate * .080));
        endpoint.shelfBasis = designBasis(endpoint.highRate, std::min(1000., .45 * rate), factor == 4);
        endpoint.coefficients = &r; endpoint.prepared = true; endpoint.clear();
    };
    SaturationSettings setting; setting.mode = SaturationMode::threeBand;
    for (auto& l : setting.lanes) { l.driveDb = 18; l.preTiltDb = 6; }
    Endpoint endpoint; readyEndpoint(endpoint, setting, 8000, 64);
    for (unsigned n = 0; n < 32; ++n) endpoint.processPrepared<64>(signal(n));
    check(endpoint.preCut.z1Low[0] != 0 && endpoint.postCut.z1Low[1] != 0);
    Endpoint copied; readyEndpoint(copied, setting, 8000, 64); copied = endpoint;
    for (unsigned n = 32; n < 96; ++n) check(sameFrame(endpoint.processPrepared<64>(signal(n)), copied.processPrepared<64, true>(signal(n))));
    check(sameBiquad(endpoint.preCut, copied.preCut) && sameBiquad(endpoint.postCut, copied.postCut)
        && sameNetwork(endpoint.wetNetwork, copied.wetNetwork) && sameNetwork(endpoint.dryNetwork, copied.dryNetwork));
    auto ramped = setting; ramped.inputTrimDb = 24; ramped.lowCrossoverHz = 40; retarget(endpoint, ramped); copied = endpoint;
    const auto rampBefore = endpoint.ramp;
    check(sameFrame(endpoint.processPrepared<64>(signal(96)), copied.processPrepared<64, true>(signal(96)))
        && endpoint.ramp + 1 == rampBefore && endpoint.ramp == copied.ramp && endpoint.current == copied.current);
    copied.clear();
    check(sameBiquad(copied.preCut, Biquad {}) && sameBiquad(copied.postCut, Biquad {})
        && sameNetwork(copied.wetNetwork, Network {}) && sameNetwork(copied.dryNetwork, Network {}));
    for (const auto& l : copied.lanes) check(sameBiquad(l.lowShelf, Biquad {}) && sameBiquad(l.highShelf, Biquad {}));
    // Independent endpoint rendering checks the actual public fade copy/retirement, including warm disabled cuts.
    for (const bool modeChange : { false, true }) {
        Candidate candidate; Endpoint from, to;
        check(candidate.setSettings(setting) && candidate.prepare(8000, 64)); readyEndpoint(from, setting, 8000, 64); readyEndpoint(to, setting, 8000, 64);
        for (unsigned n = 0; n < 32; ++n) check(sameFrame(candidate.process(signal(n)), from.processPrepared<64, true>(signal(n))));
        auto next = setting;
        if (modeChange) next.mode = SaturationMode::fullBand;
        else { next.preLowCutEnabled = true; next.postHighCutEnabled = true; }
        check(candidate.setSettings(next));
        auto continuous = next; preserveStructure(continuous, from.current); retarget(from, continuous); to = from;
        if (modeChange) {
            to.wetNetwork = {}; to.dryNetwork = {}; to.wideNetwork = {}; to.previousDry = {};
            for (auto& l : to.lanes) { const auto bypassWet = l.bypassWet; l = {}; l.bypassWet = bypassWet; }
        }
        check(sameBiquad(to.preCut, from.preCut) && sameBiquad(to.postCut, from.postCut)); retarget(to, next);
        for (unsigned n = 0; n < 192; ++n) {
            const auto input = signal(n + 32); auto expectedOutput = to.processPrepared<64, true>(input);
            if (n < 160) {
                auto old = from.processPrepared<64, true>(input);
                const double weight = double(n) / 159;
                for (std::size_t ch = 0; ch < 2; ++ch) old[ch] += weight * (expectedOutput[ch] - old[ch]);
                expectedOutput = old;
            }
            check(sameFrame(candidate.process(input), expectedOutput));
        }
    }
    section = "dispatch";
    for (const unsigned factor : { 1u, 2u, 4u, 8u, 64u }) for (const auto mode : { SaturationMode::fullBand, SaturationMode::threeBand })
        for (const float tilt : { 0.f, 6.f }) {
            Candidate candidate; auto s = setting; s.mode = mode; for (auto& l : s.lanes) l.preTiltDb = tilt;
            check(candidate.setSettings(s) && candidate.prepare(48000, factor));
            const auto before = numericalProductStateCalls; check(finite(candidate.process(signal(0))));
            const auto calls = numericalProductStateCalls - before;
            const auto expectedCalls = factor == 64 ? 1u + 64u * (1u + (mode == SaturationMode::threeBand ? 18u : 0u)
                + (tilt != 0 ? (mode == SaturationMode::threeBand ? 6u : 2u) : 0u)) : 0u;
            check(calls == expectedCalls);
            std::cout << "product_state_dispatch factor=" << factor << " mode=" << int(mode) << " tilt=" << tilt << " calls=" << calls << '\n';
        }
    section = "IEEE-glue";
    const double tiny = std::numeric_limits<double>::denorm_min(), normal = std::numeric_limits<double>::min();
    const double maximum = std::numeric_limits<double>::max(), guard = maximum / 1e12;
    const std::array values { 0., -0., tiny, -tiny, std::nextafter(normal, 0.), normal, std::nextafter(normal, 1.),
        -normal, 1e-300, -1e-300, .03125, -.03125, R, -R, 1., -1., std::nextafter(guard, 0.), guard,
        std::nextafter(guard, maximum), maximum, -maximum, std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() };
    for (const double value : values) {
        Biquad a, e; charge(a); e = a;
        check(sameFrame(a.process<true>({value, -value}, distinct), expectedBiquad(e, {value, -value}, distinct)) && sameBiquad(a, e));
    }
    section = "admitted-domain-reset";
    for (const double rate : { 8000., 384000. }) for (const bool maximumControls : { false, true })
        for (const auto mode : { SaturationMode::fullBand, SaturationMode::threeBand })
            for (const auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
                auto s = setting; s.mode = mode; s.preLowCutEnabled = s.postHighCutEnabled = true;
                s.inputTrimDb = maximumControls ? 24.f : -24.f; s.outputTrimDb = maximumControls ? 12.f : -24.f;
                s.lowCrossoverHz = maximumControls ? 2000.f : 40.f; s.highCrossoverHz = maximumControls ? 16000.f : 500.f;
                s.preLowCutHz = maximumControls ? 2000.f : 20.f; s.postHighCutHz = maximumControls ? 20000.f : 1000.f;
                s.mix = maximumControls ? 1.f : 0.f;
                for (auto& l : s.lanes) { l.character = character; l.driveDb = maximumControls ? 36.f : 0.f;
                    l.preTiltDb = maximumControls ? 6.f : -6.f; l.mix = maximumControls ? 1.f : 0.f; l.trimDb = maximumControls ? 18.f : -18.f; }
                Candidate candidate, fresh; check(candidate.setSettings(s) && candidate.prepare(rate, 64));
                check(fresh.setSettings(s) && fresh.prepare(rate, 64));
                for (const double value : values) {
                    for (const std::size_t ch : { 0u, 1u }) {
                        candidate.process(signal(0)); const auto before = numericalProductStateCalls;
                        Candidate::Frame input { .01, -.007 }; input[ch] = value;
                        const auto output = candidate.process(input); check(finite(output));
                        const bool rejected = !std::isfinite(value) || std::abs(value) > guard;
                        if (rejected) { check(sameFrame(output, Candidate::Frame {}) && numericalProductStateCalls == before); }
                        else candidate.reset();
                        fresh.reset();
                        for (unsigned n = 0; n < 4; ++n) check(sameFrame(candidate.process(signal(n)), fresh.process(signal(n))));
                    }
                }
                check(candidate.prepare(rate, 64)); fresh.reset();
                check(sameFrame(candidate.process(signal(0)), fresh.process(signal(0))));
            }
    section = "failed-output";
    auto enabled = setting; enabled.preLowCutEnabled = enabled.postHighCutEnabled = true;
    for (const bool pre : { false, true }) {
        Endpoint failed, fresh; readyEndpoint(failed, enabled, 8000, 64); readyEndpoint(fresh, enabled, 8000, 64);
        const double infinity = std::numeric_limits<double>::infinity();
        (pre ? failed.preCut : failed.postCut).z1 = { infinity, -infinity };
        check(sameFrame(failed.processPrepared<64>(signal(0)), Candidate::Frame {}) && failed.failed);
        check(sameBiquad(failed.preCut, Biquad {}) && sameBiquad(failed.postCut, Biquad {})
            && sameNetwork(failed.wetNetwork, Network {}) && sameNetwork(failed.dryNetwork, Network {}));
        for (unsigned n = 0; n < 8; ++n) check(sameFrame(failed.processPrepared<64>(signal(n)), fresh.processPrepared<64>(signal(n))));
    }
    // A finite pre-cut output can overflow feedback; the existing output-only guard resets on the next callback.
    Endpoint delayed, recovered; readyEndpoint(delayed, enabled, 8000, 64); readyEndpoint(recovered, enabled, 8000, 64);
    delayed.preCut.z1 = { maximum, -maximum };
    const auto first = delayed.processPrepared<64>(signal(0));
    const bool currentFinite = finite(first), feedbackFinite = finite(delayed.preCut.z1), failedFirst = delayed.failed;
    check(currentFinite && !feedbackFinite && !failedFirst);
    const auto second = delayed.processPrepared<64>(signal(1));
    check(sameFrame(second, Candidate::Frame {}) && delayed.failed);
    check(sameBiquad(delayed.preCut, Biquad {}) && sameBiquad(delayed.postCut, Biquad {})
        && sameNetwork(delayed.wetNetwork, Network {}) && sameNetwork(delayed.dryNetwork, Network {}));
    for (unsigned n = 0; n < 8; ++n) check(sameFrame(delayed.processPrepared<64>(signal(n)), recovered.processPrepared<64>(signal(n))));
    std::cout << "product_state_extreme pre_current_finite=" << currentFinite << " feedback_finite=" << feedbackFinite
        << " failed_first=" << failedFirst << " reset_next=" << sameFrame(second, Candidate::Frame {}) << '\n';
    std::cout << "product_state_checks=" << checks << " charged_frames=" << chargedFrames << " product_state_failures=" << failures
        << " domain_theorem=NORMAL_CORPUS_ONLY extra_IEEE=FIXED_ALGORITHM_AND_EXISTING_FAIL_CLOSED\n";
    numericalCollectRecords = collected;
    return failures;
}

int runSaturationNumericalPathChecks() {
    numericalRecords.clear(); numericalRecords.reserve(4096);
    int failures = 0;
    const auto check = [&](bool okay) { if (!okay) ++failures; };
    const auto same = [](double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); };
    const auto sameFrame = [&](const auto& a, const auto& b) { return std::memcmp(a.data(), b.data(), sizeof(a)) == 0; };
    const auto sameLane = [&](const Lane& a, const Lane& b) {
        return sameFrame(a.previousX, b.previousX) && sameFrame(a.previousH, b.previousH)
            && sameFrame(a.previousU, b.previousU) && sameFrame(a.previousDifference, b.previousDifference)
            && sameFrame(a.dc, b.dc) && sameFrame(a.previousPrimitive, b.previousPrimitive)
            && sameFrame(a.lowShelf.z1, b.lowShelf.z1) && sameFrame(a.lowShelf.z2, b.lowShelf.z2)
            && sameFrame(a.highShelf.z1, b.highShelf.z1) && sameFrame(a.highShelf.z2, b.highShelf.z2)
            && sameFrame(a.lowShelf.z1Low, b.lowShelf.z1Low) && sameFrame(a.lowShelf.z2Low, b.lowShelf.z2Low)
            && sameFrame(a.highShelf.z1Low, b.highShelf.z1Low) && sameFrame(a.highShelf.z2Low, b.highShelf.z2Low)
            && a.primitiveKind == b.primitiveKind && a.primitiveCharacter == b.primitiveCharacter
            && same(a.envelope, b.envelope) && a.tiltCharged == b.tiltCharged && same(a.bypassWet, b.bypassWet);
    };
    const auto& r = resources(); const Coefficients coefficients {};
    Lane batch, scalar, dispatched; std::array<Lane, 3> generic;
    constexpr std::array levels { 0., 0., 2e-6, 4e-6, .1, .10002, .10002, -.1,
        -.10002, -.10002, -.75, -.75002, .75, .75002, -.75001, .75001,
        1., -1., 8., 8.00002, 16., 16.00002, 32., -32., 0., -0., .35, -.35,
        .03125, .03127, -.03125, -.03127 };
    std::array<std::size_t, 3> dispatches {};
    for (const auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense, SaturationCharacter::soft }) {
        SaturationLaneSettings setting; setting.character = character;
        for (std::size_t offset = 0; offset < levels.size(); offset += 4) {
            std::array<Candidate::Frame, 4> input;
            for (std::size_t n = 0; n < 4; ++n) input[n] = { levels[offset + n] * R, -.7 * levels[offset + n] * R };
            auto output = input;
            switch (character) {
                case SaturationCharacter::soft: batch.process4Character<SaturationCharacter::soft>(output, setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve); break;
                case SaturationCharacter::glow: batch.process4Character<SaturationCharacter::glow>(output, setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve); break;
                case SaturationCharacter::dense: batch.process4Character<SaturationCharacter::dense>(output, setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve); break;
            }
            for (std::size_t n = 0; n < 4; ++n) {
                Candidate::Frame expected;
                switch (character) {
                    case SaturationCharacter::soft: expected = scalar.processFourCharacter<SaturationCharacter::soft>(input[n], setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve); break;
                    case SaturationCharacter::glow: expected = scalar.processFourCharacter<SaturationCharacter::glow>(input[n], setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve); break;
                    case SaturationCharacter::dense: expected = scalar.processFourCharacter<SaturationCharacter::dense>(input[n], setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve); break;
                }
                const auto dispatch = dispatched.process<true>(input[n], setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve);
                ++dispatches[std::size_t(character)];
                check(sameFrame(output[n], expected) && sameFrame(dispatch, expected) && sameLane(dispatched, scalar));
                // Factors2/8/64 all instantiate this exact generic Lane evaluator.
                for (auto& lane : generic) lane.process<false>(input[n], setting, coefficients, coefficients, 1, .5, .9, .999, 1, 1, r.primitiveCurve, r.gainCurve);
            }
            check(sameLane(batch, scalar));
        }
    }
    std::array<std::array<std::size_t, 3>, 3> coverage {};
    std::array<std::array<std::size_t, 9>, 3> transitions {};
    for (const auto& row : numericalRecords) {
        ++coverage[row.site][row.kind]; ++transitions[row.site][3 * row.previousKind + row.kind];
        const auto c = static_cast<SaturationCharacter>(row.character);
        if (row.kind == 0) {
            check(same(row.q, cachedShape(c, .5 * (row.u + row.p))));
        } else {
            const auto evaluate = [&](double v) { return row.kind == 1 ? cachedPrimitive(c, v) : curvePrimitive(r.primitiveCurve, c, v); };
            check(same(row.currentPrimitive, evaluate(row.u)) && same(row.priorPrimitive, evaluate(row.p)));
            if (row.previousKind == row.kind) check(same(row.cachedPrimitive, evaluate(row.p)));
            if (row.kind == 2) check(same(row.q, (row.currentPrimitive - row.priorPrimitive) / (row.u - row.p)));
        }
    }
    for (std::size_t site = 0; site < 3; ++site) {
        check(coverage[site][0] && coverage[site][1]);
        if (site < 2) { check(coverage[site][2]); for (auto count : transitions[site]) check(count != 0); }
        check(transitions[site][1] && transitions[site][3]);
        std::cout << "numerical_site=" << site << " none=" << coverage[site][0] << " exact=" << coverage[site][1] << " curve=" << coverage[site][2] << " transitions=";
        for (auto count : transitions[site]) std::cout << count << ',';
        std::cout << '\n';
    }
    for (std::size_t c = 0; c < 3; ++c) { check(dispatches[c] != 0); std::cout << "valid_character_dispatch=" << c << " count=" << dispatches[c] << '\n'; }
    Endpoint endpoint; endpoint.lanes[0] = scalar; endpoint.clear(); check(sameLane(endpoint.lanes[0], Lane {}));
    Endpoint replacement; replacement = endpoint; replacement.lanes[1] = batch; replacement.clear(); check(sameLane(replacement.lanes[1], Lane {}));
    numericalCollectRecords = false;
    for (const unsigned factor : { 2u, 4u, 8u, 64u }) {
        Candidate candidate, fresh;
        SaturationSettings setting; for (auto& lane : setting.lanes) lane.driveDb = 18;
        check(candidate.setSettings(setting) && candidate.prepare(48000, factor));
        check(fresh.setSettings(setting) && fresh.prepare(48000, factor));
        for (int n = 0; n < 16; ++n) candidate.process({ .01, -.007 });
        candidate.reset();
        for (int n = 0; n < 16; ++n) check(sameFrame(candidate.process({ .01, -.007 }), fresh.process({ .01, -.007 })));
        check(candidate.prepare(48000, factor) && fresh.prepare(48000, factor));
        check(sameFrame(candidate.process({ .01, -.007 }), fresh.process({ .01, -.007 })));
        if (factor == 4) {
            setting.mode = SaturationMode::threeBand;
            setting.lanes[1].character = SaturationCharacter::soft;
            setting.lanes[2].character = SaturationCharacter::glow;
            setting.lanes[3].character = SaturationCharacter::dense;
            check(candidate.setSettings(setting));
            const auto start = numericalRecords.size(); numericalCollectRecords = true;
            candidate.process({ .01, -.007 }); numericalCollectRecords = false;
            check(numericalRecords.size() == start + 32);
            for (const auto offset : { 8u, 16u, 24u }) for (std::size_t ch = 0; ch < 2; ++ch) {
                const auto& record = numericalRecords[start + offset + ch];
                check(record.p == 0 && record.cachedPrimitive == 0 && record.previousKind == 0);
            }
        }
    }
    numericalCollectRecords = true;
    check(numericalZeroHalfWidths == 0);
    std::cout << "generic_Four_true_fallthrough=UNREACHABLE_VALID_SETTINGS path_records=" << numericalRecords.size() << " path_failures=" << failures << '\n';
    return failures;
}
#endif

}
}

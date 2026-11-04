#include "Saturation.h"
#include "SaturationWideNetwork.h"
#include "SaturationMath.h"
#include "SaturationGain.h"
#include "SaturationCompensation.h"
#include "SaturationProductState.h"
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

namespace airack {
namespace saturation_detail {
struct CoefficientView { const char* name; std::span<const double> values; };
std::span<const double> saturationCompensationTable(SaturationCharacter) noexcept;
}
namespace {
constexpr double pi = std::numbers::pi, R = 1. / 16;
constexpr double invalid = std::numeric_limits<double>::quiet_NaN();
bool bounded(float x, float lo, float hi) noexcept { return std::isfinite(x) && x >= lo && x <= hi; }
bool validRate(double rate) noexcept { return std::isfinite(rate) && rate >= 8000 && rate <= 768000; }
unsigned transitionFrames(double rate) noexcept {
    return static_cast<unsigned>(std::max(1., std::round(.020 * rate)));
}
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
std::array<double, 2> returnDryAverage(const std::array<double, 2>& dry, const std::array<double, 2>& previous) noexcept {
    std::array<double, 2> result;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    _mm_storeu_pd(result.data(), _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(_mm_loadu_pd(dry.data()), _mm_loadu_pd(previous.data()))));
    return result;
#endif
    for (std::size_t ch = 0; ch < 2; ++ch) result[ch] = .5 * (dry[ch] + previous[ch]);
    return result;
}
void returnBand(std::array<double, 2>& wet, std::array<double, 2>& dry, const std::array<double, 2>& y,
    const std::array<double, 2>& dryBand, const std::array<double, 2>& previous) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    const auto average = _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(_mm_loadu_pd(dryBand.data()), _mm_loadu_pd(previous.data())));
    _mm_storeu_pd(wet.data(), _mm_add_pd(_mm_loadu_pd(wet.data()), _mm_loadu_pd(y.data())));
    _mm_storeu_pd(dry.data(), _mm_add_pd(_mm_loadu_pd(dry.data()), average));
    return;
#endif
    for (std::size_t ch = 0; ch < 2; ++ch) { wet[ch] += y[ch]; dry[ch] += .5 * (dryBand[ch] + previous[ch]); }
}
void returnGain(std::array<double, 2>& result, double outputGain) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    _mm_storeu_pd(result.data(), _mm_mul_pd(_mm_loadu_pd(result.data()), _mm_set1_pd(outputGain)));
    return;
#endif
    result[0] *= outputGain; result[1] *= outputGain;
}
void returnFade(std::array<double, 2>& result, const std::array<double, 2>& next, double weight) noexcept {
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    const auto current = _mm_loadu_pd(result.data());
    _mm_storeu_pd(result.data(), _mm_add_pd(current, _mm_mul_pd(_mm_set1_pd(weight), _mm_sub_pd(_mm_loadu_pd(next.data()), current))));
    return;
#endif
    for (std::size_t ch = 0; ch < 2; ++ch) result[ch] += weight * (next[ch] - result[ch]);
}

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
    const auto table = saturation_detail::saturationCompensationTable(c);
    const auto low = std::min(std::size_t(drive), std::size_t(35));
    return float(table[low] + (double(drive) - double(low)) * (table[low + 1] - table[low]));
}
double saturationTailSeconds(const SaturationSettings& s, double rate) noexcept {
    if (!validateSaturation(s) || !validRate(rate)) return invalid;
    return std::max({ 2., std::log(1e9) / (2 * pi * 5), .080 * std::log(1e9) });
}

namespace saturation_detail {
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
double cachedPrimitive(SaturationCharacter c, double x) noexcept {
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
    forcedinline Saturation::Frame evaluatePair(Saturation::Frame x) const noexcept {
        const double v0 = std::abs(x[0]), v1 = std::abs(x[1]);
        if (!(v0 < limit) || !(v1 < limit)) return { evaluate(x[0]), evaluate(x[1]) };
        const int c0 = int(v0 * cellsPerUnit), c1 = int(v1 * cellsPerUnit);
        const double t0 = v0 - (double(c0) + .5) / cellsPerUnit;
        const double t1 = v1 - (double(c1) + .5) / cellsPerUnit;
        const auto* a0 = coefficients.data() + c0 * stride;
        const auto* a1 = coefficients.data() + c1 * stride;
        const auto t = _mm_set_pd(t1, t0);
        auto result = _mm_set_pd(a1[degree], a0[degree]);
        for (std::size_t n = degree; n-- > 0;)
            result = _mm_add_pd(_mm_mul_pd(result, t), _mm_set_pd(a1[n], a0[n]));
        Saturation::Frame y; _mm_storeu_pd(y.data(), result); return y;
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
forcedinline Saturation::Frame curvePrimitivePair(const PrimitiveCurve& curve, Saturation::Frame x) noexcept {
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
    alignas(16) std::array<Saturation::Frame, maxIirDirectSections> direct {};
    alignas(16) std::array<Saturation::Frame, maxIirDelayedSections> delayed {};
    alignas(16) Saturation::Frame held {};
};
static_assert(sizeof(Saturation::Frame) == 2 * sizeof(double) && alignof(StereoPhaseState) >= 16);
#endif
struct Resampler {
    std::array<std::array<PhaseState, 2>, 3> iir {};
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
    std::array<StereoPhaseState, 2> stereoIir {};
#endif
    using FirBank = std::array<std::array<FirState, 2>, 6>;
    std::unique_ptr<FirBank> fir;
    Resampler& operator=(const Resampler& other) noexcept {
        iir = other.iir;
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
        stereoIir = other.stereoIir;
#endif
        // Both endpoints were prepared for the same factor before admission.
        if (fir && other.fir) *fir = *other.fir;
        return *this;
    }
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
    void up(Saturation::Frame input, unsigned factor, const Resources& r, std::array<Saturation::Frame, 64>& output) noexcept {
        output[0] = input;
        for (std::size_t stage = 0, count = 1; count < factor; ++stage, count *= 2) {
            std::array<Saturation::Frame, 64> previous;
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
        std::array<Saturation::Frame, Capacity>& state, std::size_t count) noexcept {
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
    static forcedinline void upStereo(Saturation::Frame input, StereoPhaseState& state,
        const PhaseCoefficients& c, Saturation::Frame& even, Saturation::Frame& odd) noexcept {
        const auto directCount = DirectCount ? DirectCount : c.directCount, delayedCount = DelayedCount ? DelayedCount : c.delayedCount;
        const auto x = _mm_loadu_pd(input.data());
        _mm_storeu_pd(even.data(), cascadeStereo(x, c.direct, state.direct, directCount));
        _mm_storeu_pd(odd.data(), cascadeStereo(x, c.delayed, state.delayed, delayedCount));
    }
    template<std::size_t DirectCount = 0, std::size_t DelayedCount = 0>
    static forcedinline Saturation::Frame downStereo(Saturation::Frame even, Saturation::Frame odd,
        StereoPhaseState& state, const PhaseCoefficients& c) noexcept {
        const auto directCount = DirectCount ? DirectCount : c.directCount, delayedCount = DelayedCount ? DelayedCount : c.delayedCount;
        const auto held = _mm_load_pd(state.held.data());
        const auto filtered = cascadeStereo(_mm_loadu_pd(even.data()), c.direct, state.direct, directCount);
        const auto y = _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(held, filtered));
        const auto nextHeld = cascadeStereo(_mm_loadu_pd(odd.data()), c.delayed, state.delayed, delayedCount);
        _mm_store_pd(state.held.data(), nextHeld);
        Saturation::Frame result; _mm_storeu_pd(result.data(), y); return result;
    }
#endif
    void up4(Saturation::Frame input, const Resources& r, std::array<Saturation::Frame, 4>& output) noexcept {
        std::array<Saturation::Frame, 2> first;
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
    Saturation::Frame down4(std::array<Saturation::Frame, 4>& input, const Resources& r) noexcept {
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
        Saturation::Frame result;
        for (std::size_t channel = 0; channel < 2; ++channel)
            result[channel] = iir[0][channel].down(input[0][channel], input[1][channel], r.iir[0].down);
        return result;
#endif
    }
    Saturation::Frame down(std::array<Saturation::Frame, 64>& input, unsigned factor, const Resources& r) noexcept {
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

struct Biquad {
#if JUCE_USE_SIMD
    using Stereo = juce::dsp::SIMDRegister<double>;
    alignas(Stereo::SIMDRegisterSize) std::array<double, 2> z1 {}, z2 {};
#else
    std::array<double, 2> z1 {}, z2 {};
#endif
    std::array<double, 2> z1Low {}, z2Low {};
    template<bool ProductState = false>
    Saturation::Frame process(Saturation::Frame x, const Coefficients& c) noexcept {
        if constexpr (ProductState) {
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
            alignas(Stereo::SIMDRegisterSize) Saturation::Frame samples = x;
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
    std::array<Saturation::Frame, 2> process(Saturation::Frame x, const Coefficients& lp, const Coefficients& hp) noexcept {
        auto l = x, h = x;
        for (std::size_t n = 0; n < 2; ++n) { l = low[n].process<ProductState>(l, lp); h = high[n].process<ProductState>(h, hp); } return { l, h };
    }
};
using NetworkCoefficients = wide_network::NetworkCoefficients;
struct Network {
    Split low, high; Biquad lowCompensation;
    template<bool ProductState = false>
    std::array<Saturation::Frame, 3> process(Saturation::Frame x, const NetworkCoefficients& c) noexcept {
        const auto lower = low.process<ProductState>(x, c.lowLP, c.lowHP), upper = high.process<ProductState>(lower[1], c.highLP, c.highHP);
        return { lowCompensation.process<ProductState>(lower[0], c.highAP), upper[0], upper[1] };
    }
};
#if ((((defined(_WIN32) && defined(_M_X64)) && JUCE_USE_SIMD) && JUCE_INTEL) && !defined(__AVX2__))
struct TemporalLaneInput {
    std::array<Saturation::Frame, 4> h, u;
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
#endif
struct Lane {
    Biquad lowShelf, highShelf;
    Saturation::Frame previousX {}, previousH {}, previousU {}, previousDifference {}, dc {};
    Saturation::Frame previousPrimitive {};
    std::array<PrimitiveKind, 2> primitiveKind {};
    SaturationCharacter primitiveCharacter = SaturationCharacter::soft;
    bool tiltCharged = false;
    double envelope = 0;
    double bypassWet = 1;
    double mixFor(const SaturationLaneSettings& s) const noexcept { return double(s.mix) * bypassWet; }
#if ((((defined(_WIN32) && defined(_M_X64)) && JUCE_USE_SIMD) && JUCE_INTEL) && !defined(__AVX2__))
    template<SaturationCharacter Character, bool Mixed = false>
    __declspec(noinline) void temporal4(std::array<Saturation::Frame, 4>& frames, const TemporalLaneInput& input,
        const Saturation::Frame& priorPrimitive, double mix, double amount, double wetGain, double dcAlpha) noexcept {
        static_assert(!Mixed || Character == SaturationCharacter::dense);
        __m256d lowMask01, lowMask23;
        if constexpr (Mixed) {
            const auto mask = [&](unsigned offset) {
                return _mm256_set_pd(input.denseLowMask & (1u << (offset+3)) ? -0. : 0., input.denseLowMask & (1u << (offset+2)) ? -0. : 0.,
                    input.denseLowMask & (1u << (offset+1)) ? -0. : 0., input.denseLowMask & (1u << offset) ? -0. : 0.);
            };
            lowMask01 = mask(0); lowMask23 = mask(4);
        }
        const auto join = [](const Saturation::Frame& a, const Saturation::Frame& b) {
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
        const auto prior = [&](const Saturation::Frame& initial, __m256d first) {
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
    void process4Character(std::array<Saturation::Frame, 4>& frames, const SaturationLaneSettings& s,
        const Coefficients& low, const Coefficients& high, double driveGain, double amount, double wetGain, double dcAlpha,
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve, bool allowTemporal = false) noexcept {
        const bool tilted = s.preTiltDb != 0;
        if (tilted) tiltCharged = true;
        else if (tiltCharged) { lowShelf = {}; highShelf = {}; tiltCharged = false; }
        const double mix = mixFor(s);
        if (primitiveCharacter != Character) { primitiveKind = {}; primitiveCharacter = Character; }
        const auto prepare = [&](const Saturation::Frame& x, Saturation::Frame& h, Saturation::Frame& u) {
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
#if ((((defined(_WIN32) && defined(_M_X64)) && JUCE_USE_SIMD) && JUCE_INTEL) && !defined(__AVX2__))
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
                const Saturation::Frame priorPrimitive {
                    primitiveKind[0] == PrimitiveKind::curve ? previousPrimitive[0] : curvePrimitive(curve, Character, previousU[0]),
                    primitiveKind[1] == PrimitiveKind::curve ? previousPrimitive[1] : curvePrimitive(curve, Character, previousU[1]) };
                if constexpr (Character == SaturationCharacter::dense) if (!denseLow && !denseHigh) {
                    temporal4<Character, true>(frames, input, priorPrimitive, mix, amount, wetGain, dcAlpha); return;
                }
                temporal4<Character>(frames, input, priorPrimitive, mix, amount, wetGain, dcAlpha); return;
            }
        }
#else
        (void) allowTemporal;
#endif
        for (std::size_t n = 0; n < 4; ++n) {
            auto& frame = frames[n];
            const auto x = frame;
            Saturation::Frame h, u;
#if ((((defined(_WIN32) && defined(_M_X64)) && JUCE_USE_SIMD) && JUCE_INTEL) && !defined(__AVX2__))
            if (prepared) { h = input.h[n]; u = input.u[n]; }
            else
#endif
            prepare(x, h, u);
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
            const bool pairedCurve =
#if ((defined(_WIN32) && defined(_M_X64)) && !defined(__AVX2__))
                prepared ? input.paired[n] :
#endif
                quotientPrimitiveKind(u[0], previousU[0]) == PrimitiveKind::curve
                && quotientPrimitiveKind(u[1], previousU[1]) == PrimitiveKind::curve;
            if (pairedCurve) {
                const auto currentPrimitive = curvePrimitivePair<Character>(curve, u);
                const Saturation::Frame priorPrimitive {
                    primitiveKind[0] == PrimitiveKind::curve ? previousPrimitive[0] : curvePrimitive(curve, Character, previousU[0]),
                    primitiveKind[1] == PrimitiveKind::curve ? previousPrimitive[1] : curvePrimitive(curve, Character, previousU[1]) };
                const auto d = _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(_mm_loadu_pd(x.data()), _mm_loadu_pd(previousX.data())));
                const auto t = _mm_mul_pd(_mm_set1_pd(.5), _mm_add_pd(_mm_loadu_pd(h.data()), _mm_loadu_pd(previousH.data())));
                const auto delta = _mm_sub_pd(_mm_loadu_pd(u.data()), _mm_loadu_pd(previousU.data()));
                const auto shaped = _mm_div_pd(_mm_sub_pd(_mm_loadu_pd(currentPrimitive.data()), _mm_loadu_pd(priorPrimitive.data())), delta);
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
            Saturation::Frame result {};
            for (std::size_t ch = 0; ch < 2; ++ch) {
                const double d = .5 * (x[ch] + previousX[ch]), t = .5 * (h[ch] + previousH[ch]);
                const double delta = u[ch] - previousU[ch];
                double shaped;
                if (std::abs(delta) < 1e-6 * (1 + std::max(std::abs(u[ch]), std::abs(previousU[ch])))) {
                    shaped = cachedShape(Character, .5 * (u[ch] + previousU[ch])); primitiveKind[ch] = PrimitiveKind::none;
                } else {
                    const auto kind = quotientPrimitiveKind(u[ch], previousU[ch]);
                    const auto evaluate = [&](double value) {
                        return kind == PrimitiveKind::exact ? cachedPrimitive(Character, value) : curvePrimitive(curve, Character, value);
                    };
                    const bool needsPrimitive = kind == PrimitiveKind::curve
                        ;
                    const double currentPrimitive = needsPrimitive ? evaluate(u[ch]) : 0.;
                    const double priorPrimitive = needsPrimitive
                        ? (primitiveKind[ch] == kind ? previousPrimitive[ch] : evaluate(previousU[ch])) : 0.;
                    shaped = kind == PrimitiveKind::exact
                        ? exactQuotient(Character, u[ch], previousU[ch], preparedConstants.glowBias,
                                        1 - preparedConstants.glowBias * preparedConstants.glowBias)
                        : (currentPrimitive - priorPrimitive) / delta;
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
    void process4(std::array<Saturation::Frame, 4>& frames, const SaturationLaneSettings& s,
        const Coefficients& low, const Coefficients& high, double driveGain, double amount, double wetGain, double dcAlpha,
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve, bool allowTemporal = false) noexcept {
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
    Saturation::Frame processFourCharacter(Saturation::Frame x, const SaturationLaneSettings& s,
        const Coefficients& low, const Coefficients& high, double driveGain, double amount, double wetGain, double dcAlpha,
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve) noexcept {
        auto h = x;
        if (s.preTiltDb != 0) {
            h = highShelf.process(lowShelf.process(h, low), high); tiltCharged = true;
        } else if (tiltCharged) { lowShelf = {}; highShelf = {}; tiltCharged = false; }
        Saturation::Frame u { driveGain * h[0] / R, driveGain * h[1] / R };
        if constexpr (Character == SaturationCharacter::dense) {
            const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
            const double alpha = peak > envelope ? sagAttack : sagRelease;
            envelope = peak + alpha * (envelope - peak);
            const double reduction = std::clamp(6 * std::max(envelope - .5, 0.) / (envelope + .5), 0., 6.);
            const double sag = gainCurve.gain(-reduction); u[0] *= sag; u[1] *= sag;
        }
        Saturation::Frame result {};
        const double mix = mixFor(s);
        if (primitiveCharacter != Character) { primitiveKind = {}; primitiveCharacter = Character; }
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double d = .5 * (x[ch] + previousX[ch]), t = .5 * (h[ch] + previousH[ch]);
            const double delta = u[ch] - previousU[ch];
            double shaped;
            if (std::abs(delta) < 1e-6 * (1 + std::max(std::abs(u[ch]), std::abs(previousU[ch])))) {
                shaped = cachedShape(Character, .5 * (u[ch] + previousU[ch])); primitiveKind[ch] = PrimitiveKind::none;
            } else {
                const auto kind = quotientPrimitiveKind(u[ch], previousU[ch]);
                const auto evaluate = [&](double value) {
                    return kind == PrimitiveKind::exact ? cachedPrimitive(Character, value) : curvePrimitive(curve, Character, value);
                };
                const bool needsPrimitive = kind == PrimitiveKind::curve
                    ;
                const double currentPrimitive = needsPrimitive ? evaluate(u[ch]) : 0.;
                const double priorPrimitive = needsPrimitive
                    ? (primitiveKind[ch] == kind ? previousPrimitive[ch] : evaluate(previousU[ch])) : 0.;
                shaped = kind == PrimitiveKind::exact
                    ? exactQuotient(Character, u[ch], previousU[ch], preparedConstants.glowBias,
                                    1 - preparedConstants.glowBias * preparedConstants.glowBias)
                    : (currentPrimitive - priorPrimitive) / delta;
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
    Saturation::Frame process(Saturation::Frame x, const SaturationLaneSettings& s,
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
        Saturation::Frame u { driveGain * h[0] / R, driveGain * h[1] / R };
        if (s.character == SaturationCharacter::dense) {
            const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
            const double alpha = peak > envelope ? sagAttack : sagRelease;
            envelope = peak + alpha * (envelope - peak);
            const double reduction = std::clamp(6 * std::max(envelope - .5, 0.) / (envelope + .5), 0., 6.);
            const double sag = Four ? gainCurve.gain(-reduction) : std::pow(10., -reduction / 20); u[0] *= sag; u[1] *= sag;
        }
        Saturation::Frame result {};
        const double mix = mixFor(s);
        if (primitiveCharacter != s.character) { primitiveKind = {}; primitiveCharacter = s.character; }
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double d = .5 * (x[ch] + previousX[ch]), t = .5 * (h[ch] + previousH[ch]);
            const double delta = u[ch] - previousU[ch];
            double shaped;
            if (std::abs(delta) < 1e-6 * (1 + std::max(std::abs(u[ch]), std::abs(previousU[ch])))) {
                shaped = cachedShape(s.character, .5 * (u[ch] + previousU[ch])); primitiveKind[ch] = PrimitiveKind::none;
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
    Saturation::Frame wetInput;
    std::array<Saturation::Frame, 4> wet, dry;
    std::array<std::array<Saturation::Frame, 4>, 3> bandWet;
    std::array<std::array<Saturation::Frame, 3>, 4> bandDry;
};
}
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
struct BandMonitorReturn {
    Biquad postCut;
    Resampler down;
};
struct Endpoint {
    using Frame = Saturation::Frame;
    using ControlPeer = Endpoint;
    SaturationSettings target {}, current {}, coefficientSettings {};
    std::array<double, 4> targetLogHz {};
    double rate = 0, highRate = 0, dcAlpha = 0, sagAttack = 0, sagRelease = 0; unsigned factor = 1, ramp = 0;
    bool prepared = false, admitted = false, coefficientsValid = false; const Resources* coefficients = nullptr;
    Resampler wetUp, dryUp, down; Network wetNetwork, dryNetwork;
    std::array<BandMonitorReturn, 3> monitorReturns;
    wide_network::State wideNetwork; bool useWideNetwork = false;
    std::array<Lane, 4> lanes; std::array<Frame, 4> previousDry {};
    Biquad preCut, postCut; NetworkCoefficients network; Coefficients pre, post;
    std::array<Coefficients, 4> shelfLow, shelfHigh; std::array<double, 4> driveGain {}, driveAmount {}, wetGain {};
    SaturationCrossovers coefficientCorners {}; DesignBasis shelfBasis;
    double inputGain = 1, outputGain = 1;
    double compensationWet = 1, coefficientCompensation = -1;
    bool failed = false;
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
                || compensationWet != coefficientCompensation
                || l.character != previous.character || current.driveCompensation != coefficientSettings.driveCompensation) {
                if (compensationWet > 0 && compensationWet < 1)
                    wetGain[n] = gainForFactor(double(l.trimDb) + compensationWet * admittedCompensationDb(l.character, l.driveDb));
                else
                    wetGain[n] = gainForFactor(l.trimDb + (compensationWet == 1 ? admittedCompensationDb(l.character, l.driveDb) : 0));
            }
        }
        coefficientCompensation = compensationWet;
        coefficientSettings = current; coefficientCorners = corners; coefficientsValid = true;
    }
    void advanceControls() noexcept {
        if (ramp == 0) return;
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
            const double destination = target.lanes[n].bypassed ? 0 : 1;
            lanes[n].bypassWet = ramp == 1 ? destination : lanes[n].bypassWet + (destination - lanes[n].bypassWet) / ramp;
        }
        const double destination = target.driveCompensation ? 1 : 0;
        compensationWet = ramp == 1 ? destination : compensationWet + (destination - compensationWet) / ramp;
        --ramp; updateCoefficients();
    }
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
        if (ramp == 0) return;
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
    void clear() noexcept {
        wetUp.clear(); dryUp.clear(); down.clear(); wetNetwork = {}; dryNetwork = {}; wideNetwork = {}; lanes = {}; previousDry = {}; preCut = {}; postCut = {};
        for (auto& m : monitorReturns) { m.down.clear(); m.postCut = {}; }
        current = target; ramp = 0;
        compensationWet = target.driveCompensation ? 1 : 0;
        for (std::size_t n = 0; n < 4; ++n) lanes[n].bypassWet = target.lanes[n].bypassed ? 0 : 1;
        failed = false;
        if (prepared) updateCoefficients();
    }
    template<std::size_t Capacity, bool ProductState = false>
    Frame processPrepared(Frame input, const ControlPeer* common = nullptr, Prefix4* prefix = nullptr, std::array<Frame, 3>* monitored = nullptr) noexcept {
        if constexpr (Capacity == 64 && !ProductState)
            if (factor == 64) return processPrepared<64, true>(input);
        auto& s = *this;
        if constexpr (Capacity == 4) {
            if (common) s.advanceControlsFrom(*common);
            else s.advanceControls();
        } else s.advanceControls();
        std::array<Frame, Capacity> wet, dry, output;
        Frame wetInput;
        {
        if constexpr (Capacity == 4) {
            if (prefix && common) {
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
        input[0] *= s.inputGain; input[1] *= s.inputGain;
        wetInput = input;
        const auto preFiltered = s.preCut.process<ProductState>(input, s.pre);
        if (s.current.preLowCutEnabled) wetInput = preFiltered;
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
                if (prefix && common) s.wideNetwork = common->wideNetwork;
                else wide_network::process4(s.wideNetwork, s.network, wet, dry, wideBands);
            }
        }
        std::array<std::array<Frame, 4>, Capacity == 4 ? 3 : 0> batchWet;
        std::array<std::array<Frame, 3>, Capacity == 4 ? 4 : 0> batchDry;
        if constexpr (Capacity == 4) {
            if (s.current.mode == SaturationMode::fullBand) {
                s.lanes[0].process4(wet, s.current.lanes[0], s.shelfLow[0], s.shelfHigh[0], s.driveGain[0], s.driveAmount[0], s.wetGain[0], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve
                    , s.useWideNetwork
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
                        , s.useWideNetwork
                    );
                }
            }
        }
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
            const auto postFiltered = s.postCut.process<ProductState>(w, s.post);
            if (s.current.postHighCutEnabled) w = postFiltered;
            for (std::size_t ch = 0; ch < 2; ++ch) output[n][ch] = d[ch] + s.current.mix * (w[ch] - d[ch]);
            valid = valid && finite(w) && finite(d) && finite(output[n]);
        }
        if constexpr (Capacity == 4) if (monitored) {
            *monitored = {};
            if (current.mode == SaturationMode::threeBand) {
                for (std::size_t band = 0; band < 3; ++band) {
                    auto audio = batchWet[band];
                    auto& history = monitorReturns[band];
                    for (auto& frame : audio) {
                        const auto filtered = history.postCut.process(frame, post);
                        if (current.postHighCutEnabled) frame = filtered;
                    }
                    auto returned = history.down.down4(audio, *coefficients);
                    returnGain(returned, outputGain);
                    (*monitored)[band] = returned;
                    valid = valid && finite(returned);
                }
            }
        }
        auto result = [&] {
            if constexpr (Capacity == 4) return s.down.down4(output, *s.coefficients);
            else return s.down.down(output, s.factor, *s.coefficients);
        }();
        if constexpr (Capacity == 4) returnGain(result, s.outputGain);
        else { result[0] *= s.outputGain; result[1] *= s.outputGain; }
        if (!valid || !finite(result)) {
            s.clear();
            s.failed = true;
            return {};
        } return result;
    }
};
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
    if (controlsChanged) endpoint.ramp = transitionFrames(endpoint.rate);
    endpoint.updateCoefficients();
}
}
} // namespace saturation_detail
using namespace saturation_detail;

struct Saturation::State {
    std::array<Endpoint, 2> endpoints;
    std::array<double, 4> monitorWeights {1, 0, 0, 0};
    std::array<double, 4> monitorTargets {1, 0, 0, 0};
    unsigned monitorRemaining = 0;
    SaturationSettings target {};
    unsigned active = 0, fadeFrame = 0, fadeLength = 1;
    bool prepared = false, admitted = false, fading = false, commonControls = false, commonPrefix = false;
    void startFade() noexcept {
        auto& from = endpoints[active]; auto& to = endpoints[1 - active];
        to = from;
        if (from.current.mode != target.mode) {
            // An inactive mode must not revive its old networks or lane audio.
            to.wetNetwork = {}; to.dryNetwork = {}; to.wideNetwork = {}; to.previousDry = {};
            for (auto& m : to.monitorReturns) { m.down.clear(); m.postCut = {}; }
            for (auto& lane : to.lanes) { const double bypassWet = lane.bypassWet; lane = {}; lane.bypassWet = bypassWet; }
        }
        retarget(to, target); commonControls = from.hasCommonControls(to); fadeFrame = 0; fading = true;
        commonPrefix = commonControls && from.current.mode == to.current.mode && from.current.preLowCutEnabled == to.current.preLowCutEnabled;
    }
    void clear() noexcept {
        for (auto& endpoint : endpoints) { endpoint.cacheTargetFrequencies(target, true); endpoint.target = target; endpoint.clear(); }
        active = fadeFrame = 0; fading = commonControls = commonPrefix = false;
        monitorWeights = monitorTargets = {1, 0, 0, 0}; monitorRemaining = 0;
    }
};
// Two fixed endpoint objects and, only at factor64, six preallocated FIR banks.
static_assert(sizeof(Saturation::Frame) == 16);
// State is private; this bound also includes the single latest-intent settings bank.
Saturation::Saturation() : state(std::make_unique<State>()) {
    static_assert(sizeof(State) < 200000);
    static_assert(sizeof(State) + 6 * sizeof(Resampler::FirBank) < 200000);
}
Saturation::~Saturation() {
}
bool Saturation::prepare(double rate) noexcept {
    constexpr unsigned factor = 4;
    if (!validRate(rate)) return false;
    try {
        auto replacement = std::make_unique<State>(); replacement->target = state->target; replacement->admitted = state->admitted;
        for (auto& endpoint : replacement->endpoints) {
            endpoint.target = endpoint.current = replacement->target; endpoint.admitted = replacement->admitted;
            endpoint.cacheTargetFrequencies(replacement->target, true);
            endpoint.rate = rate; endpoint.highRate = rate * factor; endpoint.factor = factor;
            endpoint.wetUp.prepare(factor); endpoint.dryUp.prepare(factor); endpoint.down.prepare(factor);
            for (auto& m : endpoint.monitorReturns) m.down.prepare(4);
            endpoint.dcAlpha = std::exp(-2 * pi * 5 / endpoint.highRate);
            endpoint.sagAttack = std::exp(-1 / (endpoint.highRate * .002)); endpoint.sagRelease = std::exp(-1 / (endpoint.highRate * .080));
            endpoint.shelfBasis = designBasis(endpoint.highRate, std::min(1000., .45 * rate), factor == 4);
            endpoint.coefficients = &resources(); endpoint.prepared = true;
            endpoint.useWideNetwork = factor == 4 && juce::SystemStats::hasAVX() && wide_network::operatingSystemSupportsAvx();
            endpoint.clear();
        }
        replacement->fadeLength = transitionFrames(rate); replacement->prepared = true;
        state.swap(replacement); return true;
    } catch (...) { return false; }
}
bool Saturation::setSettings(const SaturationSettings& next) noexcept {
    if (!validateSaturation(next)) return false;
    auto& s = *state;
    if (next.mode == SaturationMode::fullBand) cancelAudition();
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
Saturation::Frame Saturation::process(Frame input) noexcept {
    auto& s = *state;
    if (!s.prepared) return {};
    if (!finite(input) || std::max(std::abs(input[0]), std::abs(input[1])) > std::numeric_limits<double>::max() / 1e12) { reset(); return {}; }
    std::array<Frame, 3> monitored {}, nextMonitored {};
    const auto render = [&](Endpoint& endpoint, auto& bands) { return endpoint.processPrepared<4>(input, nullptr, nullptr, &bands); };
    Prefix4 prefix;
    auto result = s.commonPrefix ? s.endpoints[s.active].processPrepared<4>(input, nullptr, &prefix, &monitored) : render(s.endpoints[s.active], monitored);
    if (s.endpoints[s.active].failed) { reset(); return {}; }
    if (s.fading) {
        auto& destination = s.endpoints[1 - s.active];
        const auto next = s.commonControls ? destination.processPrepared<4>(input, &s.endpoints[s.active], s.commonPrefix ? &prefix : nullptr, &nextMonitored) : render(destination, nextMonitored);
        if (s.endpoints[1 - s.active].failed) { reset(); return {}; }
        const double weight = double(s.fadeFrame) / (s.fadeLength - 1);
        if (s.endpoints[s.active].factor == 4) returnFade(result, next, weight);
        else for (std::size_t ch = 0; ch < 2; ++ch) result[ch] += weight * (next[ch] - result[ch]);
        for (std::size_t band = 0; band < 3; ++band) returnFade(monitored[band], nextMonitored[band], weight);
        if (++s.fadeFrame == s.fadeLength) {
            s.active = 1 - s.active; s.fading = s.commonControls = s.commonPrefix = false;
            if (!sameStructure(s.target, s.endpoints[s.active].current)) s.startFade();
        }
    }
    if (!finite(result)) { reset(); return {}; }
    if (s.monitorRemaining) {
        for (std::size_t n = 0; n < 4; ++n) s.monitorWeights[n] += (s.monitorTargets[n] - s.monitorWeights[n]) / s.monitorRemaining;
        if (--s.monitorRemaining == 0) s.monitorWeights = s.monitorTargets;
    }
    if (s.monitorWeights == std::array<double, 4>{1, 0, 0, 0}) return result;
    for (std::size_t ch = 0; ch < 2; ++ch) {
        result[ch] *= s.monitorWeights[0];
        for (std::size_t band = 0; band < 3; ++band) result[ch] += s.monitorWeights[band + 1] * monitored[band][ch];
    }
    if (!finite(result)) { reset(); return {}; }
    return result;
}
void Saturation::reset() noexcept { state->clear(); }
const SaturationSettings& Saturation::settings() const noexcept { return state->target; }
bool Saturation::setAudition(std::optional<SaturationLane> lane) noexcept {
    if (lane && (*lane == SaturationLane::full || static_cast<unsigned>(*lane) > static_cast<unsigned>(SaturationLane::high))) return false;
    if (lane && (!state->prepared || state->target.mode != SaturationMode::threeBand)) return false;
    std::array<double, 4> next {};
    next[lane ? static_cast<std::size_t>(*lane) : 0] = 1.;
    if (next != state->monitorTargets) {
        state->monitorTargets = next;
        state->monitorRemaining = transitionFrames(state->endpoints[state->active].rate);
    }
    return true;
}
void Saturation::cancelAudition() noexcept { (void)setAudition(std::nullopt); }
bool Saturation::auditioning() const noexcept { return state->monitorTargets[0] != 1. || state->monitorWeights[0] != 1.; }
void Saturation::processBlock(std::span<Frame> block) noexcept {
    for (auto& frame : block) frame = process(frame);
}



}

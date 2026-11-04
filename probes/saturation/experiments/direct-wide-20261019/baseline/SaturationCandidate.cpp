#include "SaturationCandidate.h"
#include "SaturationWideNetwork.h"
#include "SaturationMath.h"
#include "SaturationGain.h"
#include "SaturationCalibration.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

namespace airack {
namespace {
constexpr double pi = std::numbers::pi, R = 1. / 16;
constexpr double invalid = std::numeric_limits<double>::quiet_NaN();
bool bounded(float x, float lo, float hi) noexcept { return std::isfinite(x) && x >= lo && x <= hi; }
bool validRate(double rate) noexcept { return std::isfinite(rate) && rate >= 8000 && rate <= 384000; }
bool validCharacter(SaturationCharacter c) noexcept { return c == SaturationCharacter::soft || c == SaturationCharacter::glow || c == SaturationCharacter::dense; }
double gain(double db) noexcept { return std::pow(10., db / 20); }
bool finite(std::array<double, 2> x) noexcept { return std::isfinite(x[0]) && std::isfinite(x[1]); }
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
double adaa(SaturationCharacter c, double x, double previous) noexcept {
    const double delta = x - previous;
    return std::abs(delta) < 1e-6 * (1 + std::max(std::abs(x), std::abs(previous)))
        ? shape(c, .5 * (x + previous)) : (primitive(c, x) - primitive(c, previous)) / delta;
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
#if JUCE_USE_SIMD && JUCE_INTEL && !defined(__AVX2__)
        static_assert(degree == 7 && stride == 8);
        const auto z = _mm_set1_pd(t * t);
        auto paired = _mm_loadu_pd(a + 6);
        paired = _mm_add_pd(_mm_mul_pd(paired, z), _mm_loadu_pd(a + 4));
        paired = _mm_add_pd(_mm_mul_pd(paired, z), _mm_loadu_pd(a + 2));
        paired = _mm_add_pd(_mm_mul_pd(paired, z), _mm_loadu_pd(a));
        const auto odd = _mm_unpackhi_pd(paired, paired);
        return _mm_cvtsd_f64(paired) + t * _mm_cvtsd_f64(odd);
#else
        double result = a[degree];
        for (std::size_t n = degree; n-- > 0;) result = result * t + a[n];
        return result;
#endif
    }
    // On every radius-1 disk about a real center, |tanh(z)|<=tan(1).
    // With |t|<=1/64, Cauchy's estimate bounds the degree-7 log(cosh)
    // truncation by tan(1)*(1/64)^8/(8*(1-1/64)) < 7.03e-16.
    // Binary64 coefficient/libm/evaluation error is measured separately against
    // the unchanged analytic oracle; the close-quotient guard below prevents
    // dividing a few ulps of primitive error by the original 1e-6 cutoff.
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
        const auto denominator = Stereo(a0).value;
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
        const auto denominator = Stereo(a0).value;
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
    Candidate::Frame process(Candidate::Frame x, const Coefficients& c) noexcept {
#if JUCE_USE_SIMD
        if constexpr (Stereo::size() == 2) {
            alignas(Stereo::SIMDRegisterSize) Candidate::Frame samples = x;
            const auto input = Stereo::fromRawArray(samples.data());
            const auto y = Stereo(c.b0) * input + Stereo::fromRawArray(z1.data());
            const auto nextZ1 = (Stereo(c.b1) * input - Stereo(c.a1) * y) + Stereo::fromRawArray(z2.data());
            const auto nextZ2 = Stereo(c.b2) * input - Stereo(c.a2) * y;
            nextZ1.copyToRawArray(z1.data()); nextZ2.copyToRawArray(z2.data());
            y.copyToRawArray(samples.data()); return samples;
        }
#endif
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double y = c.b0 * x[ch] + z1[ch]; z1[ch] = c.b1 * x[ch] - c.a1 * y + z2[ch]; z2[ch] = c.b2 * x[ch] - c.a2 * y; x[ch] = y;
        }
        return x;
    }
};
struct Split {
    std::array<Biquad, 2> low, high;
    std::array<Candidate::Frame, 2> process(Candidate::Frame x, const Coefficients& lp, const Coefficients& hp) noexcept {
        auto l = x, h = x;
        for (std::size_t n = 0; n < 2; ++n) { l = low[n].process(l, lp); h = high[n].process(h, hp); } return { l, h };
    }
};
using NetworkCoefficients = wide_network::NetworkCoefficients;
struct Network {
    Split low, high; Biquad lowCompensation;
    std::array<Candidate::Frame, 3> process(Candidate::Frame x, const NetworkCoefficients& c) noexcept {
        const auto lower = low.process(x, c.lowLP, c.lowHP), upper = high.process(lower[1], c.highLP, c.highHP);
        return { lowCompensation.process(lower[0], c.highAP), upper[0], upper[1] };
    }
};
struct Lane {
    Biquad lowShelf, highShelf;
    Candidate::Frame previousX {}, previousH {}, previousU {}, previousDifference {}, dc {};
    Candidate::Frame previousPrimitive {};
    std::array<PrimitiveKind, 2> primitiveKind {};
    SaturationCharacter primitiveCharacter = SaturationCharacter::soft;
    bool tiltCharged = false;
    double envelope = 0;
    template<SaturationCharacter Character>
    void process4Character(std::array<Candidate::Frame, 4>& frames, const SaturationLaneSettings& s,
        const Coefficients& low, const Coefficients& high, double driveGain, double amount, double wetGain, double dcAlpha,
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve) noexcept {
        const bool tilted = s.preTiltDb != 0;
        if (tilted) tiltCharged = true;
        else if (tiltCharged) { lowShelf = {}; highShelf = {}; tiltCharged = false; }
        const double mix = s.bypassed ? 0 : s.mix;
        if (primitiveCharacter != Character) { primitiveKind = {}; primitiveCharacter = Character; }
        for (auto& frame : frames) {
            const auto x = frame;
            auto h = x;
            if (tilted) h = highShelf.process(lowShelf.process(h, low), high);
            Candidate::Frame u { driveGain * h[0] / R, driveGain * h[1] / R };
            if constexpr (Character == SaturationCharacter::dense) {
                const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
                const double alpha = peak > envelope ? sagAttack : sagRelease;
                envelope = peak + alpha * (envelope - peak);
                const double reduction = std::clamp(6 * std::max(envelope - .5, 0.) / (envelope + .5), 0., 6.);
                const double sag = gainCurve.gain(-reduction); u[0] *= sag; u[1] *= sag;
            }
            Candidate::Frame result {};
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
                    const double currentPrimitive = evaluate(u[ch]);
                    const double priorPrimitive = primitiveKind[ch] == kind ? previousPrimitive[ch] : evaluate(previousU[ch]);
                    shaped = (currentPrimitive - priorPrimitive) / delta;
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
        double sagAttack, double sagRelease, const PrimitiveCurve& curve, const gain_math::GainCurve& gainCurve) noexcept {
        switch (s.character) {
            case SaturationCharacter::soft:
                process4Character<SaturationCharacter::soft>(frames, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve); return;
            case SaturationCharacter::glow:
                process4Character<SaturationCharacter::glow>(frames, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve); return;
            case SaturationCharacter::dense:
                process4Character<SaturationCharacter::dense>(frames, s, low, high, driveGain, amount, wetGain, dcAlpha, sagAttack, sagRelease, curve, gainCurve); return;
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
        const double mix = s.bypassed ? 0 : s.mix;
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
                const double currentPrimitive = evaluate(u[ch]);
                const double priorPrimitive = primitiveKind[ch] == kind ? previousPrimitive[ch] : evaluate(previousU[ch]);
                shaped = (currentPrimitive - priorPrimitive) / delta;
                previousPrimitive[ch] = currentPrimitive; primitiveKind[ch] = kind;
            }
            double difference = R * shaped - t;
            dc[ch] = difference - previousDifference[ch] + dcAlpha * dc[ch]; previousDifference[ch] = difference;
            if constexpr (Character == SaturationCharacter::glow) difference = dc[ch];
            result[ch] = d + mix * ((t + amount * difference) * wetGain - d);
        }
        previousX = x; previousH = h; previousU = u; return result;
    }
    template<bool Four>
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
            h = highShelf.process(lowShelf.process(h, low), high); tiltCharged = true;
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
        const double mix = s.bypassed ? 0 : s.mix;
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
                shaped = (currentPrimitive - priorPrimitive) / delta;
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
    if (preparedAdaaUsesExactPrimitive(x, previous)) return (primitive(c, x) - primitive(c, previous)) / delta;
    return (preparedPrimitive(c, x) - preparedPrimitive(c, previous)) / delta;
}

struct Candidate::State {
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
    void updateCoefficients() noexcept {
        // Prepared rate and current settings are admitted. Bounded ramps and
        // validated structural edits preserve those bounds, so repeat only the
        // public helper's exact corner arithmetic, not its admission traversal.
        const double highCorner = std::min(double(current.highCrossoverHz), .45 * rate);
        const SaturationCrossovers corners { std::min(double(current.lowCrossoverHz), .8 * highCorner), highCorner };
        if (!coefficientsValid || corners.lowHz != coefficientCorners.lowHz) {
            const auto basis = designBasis(highRate, corners.lowHz, factor == 4);
            if (factor == 4) designSplit(basis, network.lowLP, network.lowHP);
            else { network.lowLP = design(Filter::low, basis); network.lowHP = design(Filter::high, basis); }
        }
        if (!coefficientsValid || corners.highHz != coefficientCorners.highHz) {
            const auto basis = designBasis(highRate, corners.highHz, factor == 4);
            if (factor == 4) designSplit(basis, network.highLP, network.highHP, &network.highAP);
            else { network.highLP = design(Filter::low, basis); network.highHP = design(Filter::high, basis); network.highAP = design(Filter::allpass, basis); }
        }
        if (!coefficientsValid || current.preLowCutHz != coefficientSettings.preLowCutHz)
            pre = design(Filter::high, designBasis(rate, std::min(double(current.preLowCutHz), .45 * rate), factor == 4));
        if (!coefficientsValid || current.postHighCutHz != coefficientSettings.postHighCutHz)
            post = design(Filter::low, designBasis(highRate, std::min(double(current.postHighCutHz), .45 * rate), factor == 4));
        if (!coefficientsValid || current.inputTrimDb != coefficientSettings.inputTrimDb) inputGain = gainForFactor(current.inputTrimDb);
        if (!coefficientsValid || current.outputTrimDb != coefficientSettings.outputTrimDb) outputGain = gainForFactor(current.outputTrimDb);
        for (std::size_t n = 0; n < 4; ++n) {
            const auto& l = current.lanes[n]; const auto& previous = coefficientSettings.lanes[n];
            if (!coefficientsValid || l.preTiltDb != previous.preTiltDb) {
                std::size_t match = 0; while (match < n && current.lanes[match].preTiltDb != l.preTiltDb) ++match;
                if (match < n) { shelfLow[n] = shelfLow[match]; shelfHigh[n] = shelfHigh[match]; }
                else {
                    shelfLow[n] = design(Filter::lowShelf, shelfBasis, -double(l.preTiltDb) / 2, factor == 4 ? &coefficients->gainCurve : nullptr);
                    shelfHigh[n] = design(Filter::highShelf, shelfBasis, double(l.preTiltDb) / 2, factor == 4 ? &coefficients->gainCurve : nullptr);
                }
            }
            if (!coefficientsValid || l.driveDb != previous.driveDb) {
                std::size_t match = 0; while (match < n && current.lanes[match].driveDb != l.driveDb) ++match;
                driveGain[n] = match < n ? driveGain[match] : gain(l.driveDb);
                driveAmount[n] = match < n ? driveAmount[match] : double(l.driveDb) / 36;
            }
            if (l.driveDb == 0) driveAmount[n] = double(l.driveDb); // Preserve the sign of zero across equal-key edits.
            if (!coefficientsValid || l.trimDb != previous.trimDb || l.driveDb != previous.driveDb
                || l.character != previous.character || current.driveCompensation != coefficientSettings.driveCompensation)
                wetGain[n] = gainForFactor(l.trimDb + (current.driveCompensation ? admittedCompensationDb(l.character, l.driveDb) : 0));
        }
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
        }
        --ramp; updateCoefficients();
    }
    void clear() noexcept {
        wetUp.clear(); dryUp.clear(); down.clear(); wetNetwork = {}; dryNetwork = {}; wideNetwork = {}; lanes = {}; previousDry = {}; preCut = {}; postCut = {};
        current = target; ramp = 0; if (prepared) updateCoefficients();
    }
    template<std::size_t Capacity>
    Frame processPrepared(Frame input) noexcept {
        auto& s = *this;
        s.advanceControls(); input[0] *= s.inputGain; input[1] *= s.inputGain;
        auto wetInput = input; if (s.current.preLowCutEnabled) wetInput = s.preCut.process(input, s.pre);
        std::array<Frame, Capacity> wet, dry, output;
        if constexpr (Capacity == 4) {
            s.wetUp.up4(wetInput, *s.coefficients, wet); s.dryUp.up4(input, *s.coefficients, dry);
        } else {
            s.wetUp.up(wetInput, s.factor, *s.coefficients, wet); s.dryUp.up(input, s.factor, *s.coefficients, dry);
        }
        std::array<std::array<wide_network::Quad, 3>, Capacity == 4 ? 4 : 0> wideBands;
        if constexpr (Capacity == 4) {
            if (s.useWideNetwork && s.current.mode == SaturationMode::threeBand)
                wide_network::process4(s.wideNetwork, s.network, wet, dry, wideBands);
        }
        std::array<std::array<Frame, 4>, Capacity == 4 ? 3 : 0> batchWet;
        std::array<std::array<Frame, 3>, Capacity == 4 ? 4 : 0> batchDry;
        if constexpr (Capacity == 4) {
            if (s.current.mode == SaturationMode::fullBand) {
                s.lanes[0].process4(wet, s.current.lanes[0], s.shelfLow[0], s.shelfHigh[0], s.driveGain[0], s.driveAmount[0], s.wetGain[0], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve);
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
                for (std::size_t band = 0; band < 3; ++band) {
                    const auto lane = band + 1;
                    s.lanes[lane].process4(batchWet[band], s.current.lanes[lane], s.shelfLow[lane], s.shelfHigh[lane], s.driveGain[lane], s.driveAmount[lane], s.wetGain[lane], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve);
                }
            }
        }
        bool valid = finite(wetInput);
        const auto processingFactor = Capacity == 4 ? 4 : s.factor;
        for (std::size_t n = 0; n < processingFactor; ++n) {
            Frame w {}, d {};
            if (s.current.mode == SaturationMode::fullBand) {
                if constexpr (Capacity == 4) w = wet[n];
                else w = s.lanes[0].process<false>(wet[n], s.current.lanes[0], s.shelfLow[0], s.shelfHigh[0], s.driveGain[0], s.driveAmount[0], s.wetGain[0], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve);
                for (std::size_t ch = 0; ch < 2; ++ch) d[ch] = .5 * (dry[n][ch] + s.previousDry[0][ch]);
                s.previousDry[0] = dry[n];
            } else {
                std::array<Frame, 3> wb, db;
                if constexpr (Capacity == 4) {
                    wb = { batchWet[0][n], batchWet[1][n], batchWet[2][n] }; db = batchDry[n];
                } else {
                    wb = s.wetNetwork.process(wet[n], s.network); db = s.dryNetwork.process(dry[n], s.network);
                }
                for (std::size_t band = 0; band < 3; ++band) {
                    const auto lane = band + 1;
                    const auto y = [&] {
                        if constexpr (Capacity == 4) return wb[band];
                        else return s.lanes[lane].process<false>(wb[band], s.current.lanes[lane], s.shelfLow[lane], s.shelfHigh[lane], s.driveGain[lane], s.driveAmount[lane], s.wetGain[lane], s.dcAlpha, s.sagAttack, s.sagRelease, s.coefficients->primitiveCurve, s.coefficients->gainCurve);
                    }();
                    for (std::size_t ch = 0; ch < 2; ++ch) { w[ch] += y[ch]; d[ch] += .5 * (db[band][ch] + s.previousDry[lane][ch]); }
                    s.previousDry[lane] = db[band]; valid = valid && finite(y);
                }
            }
            if (s.current.postHighCutEnabled) w = s.postCut.process(w, s.post);
            for (std::size_t ch = 0; ch < 2; ++ch) output[n][ch] = d[ch] + s.current.mix * (w[ch] - d[ch]);
            valid = valid && finite(w) && finite(d) && finite(output[n]);
        }
        auto result = [&] {
            if constexpr (Capacity == 4) return s.down.down4(output, *s.coefficients);
            else return s.down.down(output, s.factor, *s.coefficients);
        }();
        result[0] *= s.outputGain; result[1] *= s.outputGain;
        if (!valid || !finite(result)) { s.clear(); return {}; } return result;
    }
};
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
    auto& s = *state; if (!s.prepared) return {};
    if (!finite(input) || std::max(std::abs(input[0]), std::abs(input[1])) > std::numeric_limits<double>::max() / 1e12) { reset(); return {}; }
    return s.factor == 4 ? s.processPrepared<4>(input) : s.processPrepared<64>(input);
}
void Candidate::reset() noexcept { state->clear(); }
}
}

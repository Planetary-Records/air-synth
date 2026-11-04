#include "SynthEngine.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace airack
{
namespace
{
constexpr double pi = std::numbers::pi;
constexpr int tableSize = 2048, mipCount = 10;
constexpr double headroom = 1.0 / 32.0;

bool inRange(float value, float low, float high) noexcept
{ return std::isfinite(value) && value >= low && value <= high; }
bool valid(const EnvelopeSettings& e) noexcept
{
    return inRange(e.attack, 0, 20) && inRange(e.decay, 0, 20)
        && inRange(e.release, 0, 20) && inRange(e.sustain, 0, 1);
}
bool valid(const ModRoute& r) noexcept
{
    if (r.source < ModSource::none || r.source > ModSource::note
        || r.target < ModTarget::none || r.target > ModTarget::filterEnvelopeAmount
        || r.bank < ModBank::common || r.bank > ModBank::wavetable
        || !inRange(r.amount, -1, 1)
        || (r.enabled && (r.source == ModSource::none || r.target == ModTarget::none))) return false;
    switch (r.target)
    {
        case ModTarget::none: case ModTarget::level: case ModTarget::pan: return r.bank == ModBank::common;
        case ModTarget::osc1Width: case ModTarget::osc2Width: return r.bank == ModBank::analog;
        case ModTarget::osc1Position: case ModTarget::osc2Position: return r.bank == ModBank::wavetable;
        default: return r.bank != ModBank::common;
    }
}
bool valid(const SynthPatch& p) noexcept
{
    if (!inRange(p.masterLevel, 0, 1)
        || (p.performance.mode != VoiceMode::polyphonic && p.performance.mode != VoiceMode::monophonic)
        || !inRange(p.performance.glideSeconds, 0, 2)) return false;
    for (const auto& l : p.layers)
    {
        if (!supportsSynthAlgorithmRevision(l.algorithmRevision)) return false;
        for (const auto& clock : l.lfos)
            if (clock.shape < LfoShape::sine || clock.shape > LfoShape::square
                || !inRange(clock.rateHz, .05f, 40) || !inRange(clock.beats, .0625f, 16)) return false;
        for (const auto& route : l.modulation) if (!valid(route)) return false;
        if ((l.engine != EngineKind::analog && l.engine != EngineKind::wavetable)
            || (l.filterMode != FilterMode::lowpass && l.filterMode != FilterMode::bandpass && l.filterMode != FilterMode::highpass)
            || !inRange(l.noiseLevel, 0, 1) || !inRange(l.cutoffHz, 20, 20000)
            || !inRange(l.resonance, 0, 0.95f) || !inRange(l.filterEnvelopeOctaves, -8, 8)
            || !inRange(l.level, 0, 1) || !inRange(l.pan, -1, 1)
            || !valid(l.ampEnvelope) || !valid(l.filterEnvelope)
            || !inRange(l.unisonWidth, 0, 1) || !inRange(l.drift, 0, 1)
            || l.filterType < FilterType::clean || l.filterType > FilterType::vowel
            || !inRange(l.vowelSize, .5f, 2) || !inRange(l.attackCurve, -1, 1)) return false;
        for (const auto& o : l.oscillators)
            if ((o.waveform != Waveform::sine && o.waveform != Waveform::triangle && o.waveform != Waveform::saw && o.waveform != Waveform::pulse)
                || o.tableIndex < 0 || o.tableIndex > 1 || !inRange(o.position, 0, 1)
                || !inRange(o.tune, -24, 24) || !inRange(o.detune, -100, 100)
                || !inRange(o.level, 0, 1) || !inRange(o.pulseWidth, 0.05f, 0.95f)
                || o.unison < 1 || o.unison > 7 || !inRange(o.unisonCents, 0, 100)) return false;
    }
    return true;
}
juce::ADSR::Parameters parameters(const EnvelopeSettings& e) noexcept
{ return {e.attack, e.decay, e.sustain, e.release}; }
void updateEnvelope(juce::ADSR& envelope, const EnvelopeSettings& settings, bool force) noexcept
{
    const auto& current = envelope.getParameters();
    // JUCE noteOff derives its release slope from the instantaneous level.
    // Reapplying unchanged parameters would replace that slope with one based
    // on sustain (and immediately end a zero-sustain release).
    if (force || current.attack != settings.attack || current.decay != settings.decay
        || current.sustain != settings.sustain || current.release != settings.release)
        envelope.setParameters(parameters(settings));
}

struct Tables
{
    // Original additive synthesis, no external samples: saw -2/(pi*h),
    // triangle alternating odd 8/(pi*pi*h*h), square odd 4/(pi*h).
    // Each mip retains harmonics 1..2^m. All phases start at zero.
    std::array<std::array<std::array<float, tableSize + 1>, mipCount>, 3> samples {};
    Tables()
    {
        for (int wave = 0; wave < 3; ++wave)
            for (int mip = 0; mip < mipCount; ++mip)
                for (int i = 0; i < tableSize; ++i)
                {
                    double value = 0;
                    for (int h = 1; h <= (1 << mip); ++h)
                    {
                        double amplitude = -2.0 / (pi * h);
                        if (wave == 1) amplitude = h % 2 == 0 ? 0 : (h % 4 == 1 ? 8.0 : -8.0) / (pi * pi * h * h);
                        if (wave == 2) amplitude = h % 2 == 0 ? 0 : 4.0 / (pi * h);
                        value += amplitude * std::sin(2 * pi * h * i / tableSize);
                    }
                    samples[wave][mip][i] = static_cast<float>(value);
                }
        for (auto& wave : samples) for (auto& mip : wave) mip[tableSize] = mip[0];
    }
    double read(int wave, double phase, double increment) const noexcept
    {
        // The richer mip is also safe: no stored harmonic exceeds 0.45 Fs.
        // Fade from the previous mip as a band becomes available to avoid
        // discrete mip jumps under continuous tuning. Phase interpolation is linear.
        const double available = 0.45 / increment;
        int mip = 0;
        while (mip + 1 < mipCount && (1 << (mip + 1)) <= available) ++mip;
        const double index = phase * tableSize;
        const auto i = static_cast<int>(index);
        const double fraction = index - i;
        auto lookup = [&](int m) {
            const auto& t = samples[wave][m];
            return t[i] + fraction * (t[i + 1] - t[i]);
        };
        const auto rich = lookup(mip);
        if (mip == 0) return rich;
        const double blend = std::clamp(available / (1 << mip) - 1.0, 0.0, 1.0);
        return std::lerp(lookup(mip - 1), rich, blend);
    }
};
const Tables& preparedTables()
{
    // First construction (and the C++ initialization guard) occurs only in prepare.
    static const Tables tables;
    return tables;
}

// Revision 2 tables, built from exact Fourier series (no external samples).
// Basic waves keep the legacy amplitudes; quarter-octave levels keep every
// harmonic up to 0.48 Fs. Wavetable frames are peak-normalized and use
// half-octave levels under the same limit. Position keeps its legacy meaning:
// frames 0..16 open a sine progressively into the full saw (table 0) or
// square (table 1), so brightness rises with position and pitch never moves.
class ModernTables
{
public:
    static constexpr int size = 4096, basicLevels = 45, frameLevels = 23, frameCount = 17;
    ModernTables()
        : basicSamples(std::size_t(3 * basicLevels * stride)), frameSamples(std::size_t(2 * frameCount * frameLevels * stride)),
          sineSamples(std::size_t(stride))
    {
        for (int i = 0; i <= size; ++i) sineSamples[std::size_t(i)] = static_cast<float>(std::sin(2 * pi * i / size));
        juce::dsp::FFT fft(12);
        std::vector<juce::dsp::Complex<float>> spectrum(static_cast<std::size_t>(size)), signal(static_cast<std::size_t>(size));
        std::vector<std::complex<double>> series(static_cast<std::size_t>(size / 2));
        // Calibrate the inverse transform scale with a unit cosine.
        spectrum[1] = spectrum[std::size_t(size - 1)] = {.5f * size, 0};
        fft.perform(spectrum.data(), signal.data(), true);
        const double scale = 1.0 / signal[0].real();
        const auto synthesize = [&](int harmonics, float* out, double gain) {
            std::fill(spectrum.begin(), spectrum.end(), juce::dsp::Complex<float> {});
            for (int h = 1; h <= harmonics; ++h)
            {
                const auto c = series[std::size_t(h)] * double(size);
                spectrum[std::size_t(h)] = {float(c.real()), float(c.imag())};
                spectrum[std::size_t(size - h)] = {float(c.real()), float(-c.imag())};
            }
            fft.perform(spectrum.data(), signal.data(), true);
            for (int i = 0; i < size; ++i) out[i] = static_cast<float>(signal[std::size_t(i)].real() * scale * gain);
            out[size] = out[0];
        };
        for (int m = 0; m < basicLevels; ++m)
        {
            basicHarmonics[std::size_t(m)] = std::max(1, int(2047 * std::exp2(-m / 4.0)));
            basicLimits[std::size_t(m)] = .48 / basicHarmonics[std::size_t(m)];
        }
        for (int m = 0; m < frameLevels; ++m)
        {
            frameHarmonics[std::size_t(m)] = std::max(1, int(2047 * std::exp2(-m / 2.0)));
            frameLimits[std::size_t(m)] = .48 / frameHarmonics[std::size_t(m)];
        }
        // x(t) = sum 2 Re(c_h exp(j 2 pi h t)); a sine series a_h sin() has c_h = -j a_h / 2.
        for (int wave = 0; wave < 3; ++wave)
        {
            for (int h = 1; h < size / 2; ++h)
            {
                double amplitude = -2.0 / (pi * h);
                if (wave == 1) amplitude = h % 2 == 0 ? 0 : (h % 4 == 1 ? 8.0 : -8.0) / (pi * pi * h * h);
                if (wave == 2) amplitude = h % 2 == 0 ? 0 : 4.0 / (pi * h);
                series[std::size_t(h)] = {0, -amplitude / 2};
            }
            for (int m = 0; m < basicLevels; ++m) synthesize(basicHarmonics[std::size_t(m)], basic(wave, m), 1);
        }
        for (int table = 0; table < 2; ++table)
            for (int frame = 0; frame < frameCount; ++frame)
            {
                frameSeries(table, frame, series);
                std::vector<float> richest(static_cast<std::size_t>(stride));
                synthesize(frameHarmonics[0], richest.data(), 1);
                float peak = 0;
                for (int i = 0; i < size; ++i) peak = std::max(peak, std::abs(richest[std::size_t(i)]));
                for (int m = 0; m < frameLevels; ++m) synthesize(frameHarmonics[std::size_t(m)], frames(table, frame, m), 1.0 / peak);
            }
    }
    // hint is the caller's previous level for this oscillator (any value is
    // accepted); the result always equals the full search.
    int basicLevel(double increment, int& hint) const noexcept { return select(basicLimits, increment, hint); }
    int frameLevel(double increment, int& hint) const noexcept { return select(frameLimits, increment, hint); }
    double basic(int wave, int level, double phase) const noexcept
    { return lookup(&basicSamples[std::size_t((wave * basicLevels + level) * stride)], phase); }
    double sine(double phase) const noexcept { return lookup(sineSamples.data(), phase); }
    double frames(int table, int level, double position, double phase) const noexcept { return read(framePair(table, level, position), phase); }
    // The two frames either side of a position and the mix between them, for reading several phases at one position.
    struct FramePair { const float* a; const float* b; double mix; };
    FramePair framePair(int table, int level, double position) const noexcept
    {
        const double x = std::clamp(position, 0.0, 1.0) * (frameCount - 1);
        const int frame = std::min(int(x), frameCount - 2);
        const float* a = &frameSamples[std::size_t(((table * frameCount + frame) * frameLevels + level) * stride)];
        return {a, a + frameLevels * stride, x - frame};
    }
    static double read(const FramePair& p, double phase) noexcept
    {
        const double index = phase * size;
        const int i = std::min(int(index), size - 1);
        const double fraction = index - i;
        const double first = p.a[i] + fraction * (p.a[i + 1] - p.a[i]), second = p.b[i] + fraction * (p.b[i + 1] - p.b[i]);
        return first + p.mix * (second - first);
    }

private:
    static constexpr int stride = size + 1;
    // The search returns the first level whose limit admits the increment, or
    // the last level. Limits never decrease, so a level is that answer exactly
    // when it admits the increment (or is last) and its predecessor does not.
    template<std::size_t count>
    static int select(const std::array<double, count>& limits, double increment, int& hint) noexcept
    {
        constexpr int last = int(count) - 1;
        const int cached = std::clamp(hint, 0, last);
        if ((cached == 0 || !(increment <= limits[std::size_t(cached - 1)])) && (cached == last || increment <= limits[std::size_t(cached)]))
            return cached;
        int low = 0, high = last;
        while (low < high) { const int middle = (low + high) / 2; if (increment <= limits[std::size_t(middle)]) high = middle; else low = middle + 1; }
        hint = low;
        return low;
    }
    static double lookup(const float* t, double phase) noexcept
    {
        const double index = phase * size;
        const int i = std::min(int(index), size - 1);
        return t[i] + (index - i) * (t[i + 1] - t[i]);
    }
    float* basic(int wave, int level) noexcept { return &basicSamples[std::size_t((wave * basicLevels + level) * stride)]; }
    float* frames(int table, int frame, int level) noexcept { return &frameSamples[std::size_t(((table * frameCount + frame) * frameLevels + level) * stride)]; }
    static void frameSeries(int table, int frame, std::vector<std::complex<double>>& series)
    {
        using namespace std::complex_literals;
        std::fill(series.begin(), series.end(), std::complex<double> {});
        // Sine-phase saw 2/(pi h) or square 4/(pi h) with harmonics weighted
        // exp(-(h - 1) / opening): a sine at frame 0, the full wave at the last
        // frame. Every frame keeps a positive fundamental, so crossfades never cancel.
        const double opening = frame == frameCount - 1 ? std::numeric_limits<double>::infinity() : .5 * std::pow(1.72, frame - 1);
        for (int h = 1; h < int(series.size()) && (frame > 0 || h == 1); ++h)
            if (table == 0 || h % 2 == 1)
                series[std::size_t(h)] = -.5i * (table == 0 ? 2.0 : 4.0) / (pi * h) * std::exp(-(h - 1) / opening);
    }
    std::vector<float> basicSamples, frameSamples, sineSamples;
    std::array<int, basicLevels> basicHarmonics {};
    std::array<int, frameLevels> frameHarmonics {};
    std::array<double, basicLevels> basicLimits {};
    std::array<double, frameLevels> frameLimits {};
};
const ModernTables& preparedModernTables()
{
    static const ModernTables tables;
    return tables;
}
// Exponential ADSR (overshoot targets): convex attack, decay/release whose
// time is the full excursion. Shapes live per layer; voices hold stage/level.
// Retriggering attacks from the current level. A curve other than 0.25 makes
// the attack (e^bx - 1) / (e^b - 1) over its time x, rising late at -1 and
// fast at 1; 0.25 is the convex shape's own curve, which an attack at the
// 0.5 ms minimum also keeps.
struct ContourShape
{
    double attack = 0, attackBase = 1, decay = 0, decayBase = 0, release = 0, releaseBase = 0, sustain = 0, settle = 0;
    void configure(const EnvelopeSettings& e, double rate, float curve = .25f) noexcept
    {
        constexpr double attackRatio = .3, decayRatio = .01;
        const auto coefficient = [rate](double seconds, double ratio) {
            return std::exp(-std::log((1 + ratio) / ratio) / std::max(1.0, seconds * rate));
        };
        if (curve == .25f || e.attack <= .0005f)
        { attack = coefficient(std::max(e.attack, .0005f), attackRatio); attackBase = (1 + attackRatio) * (1 - attack); }
        else
        {
            const double samples = std::max(1.0, double(e.attack) * rate), b = -4 * std::log(13.0 / 3) * curve;
            if (std::abs(b) < 1e-9) { attack = 1; attackBase = 1 / samples; }
            else { attack = std::exp(b / samples); attackBase = std::expm1(b / samples) / std::expm1(b); }
        }
        decay = coefficient(std::max(e.decay, .001f), decayRatio); decayBase = (e.sustain - decayRatio) * (1 - decay);
        release = coefficient(std::max(e.release, .002f), decayRatio); releaseBase = -decayRatio * (1 - release);
        sustain = e.sustain; settle = std::exp(-1 / std::max(1.0, .005 * rate));
    }
};
struct Contour
{
    enum class Stage : std::uint8_t { idle, attack, decay, sustain, release };
    Stage stage = Stage::idle;
    double level = 0;
    bool sameState(const Contour& other) const noexcept
    { return stage==other.stage&&std::bit_cast<std::uint64_t>(level)==std::bit_cast<std::uint64_t>(other.level); }
    void noteOn() noexcept { stage = Stage::attack; }
    void noteOff() noexcept { if (stage != Stage::idle) stage = Stage::release; }
    void reset() noexcept { stage = Stage::idle; level = 0; }
    bool active() const noexcept { return stage != Stage::idle; }
    double next(const ContourShape& s) noexcept
    {
        switch (stage)
        {
            case Stage::idle: break;
            case Stage::attack: level = s.attackBase + level * s.attack; if (level >= 1) { level = 1; stage = Stage::decay; } break;
            case Stage::decay: level = s.decayBase + level * s.decay; if (level <= s.sustain) stage = Stage::sustain; break;
            case Stage::sustain: level = s.sustain + (level - s.sustain) * s.settle; break;
            case Stage::release: level = s.releaseBase + level * s.release; if (level <= 0) { level = 0; stage = Stage::idle; } break;
        }
        return level;
    }
};
// Slow per-oscillator pitch wander for the analog engine: random targets
// every 0.6-2 s through two smoothing poles, so it drifts rather than
// warbles (negligible content above 1 Hz). Deterministic from its seed.
struct Drift
{
    double value = 0, smooth = 0, target = 0;
    int countdown = 0;
    std::uint32_t random = 1;
    double next(double coefficient, double rate) noexcept
    {
        if (--countdown <= 0)
        {
            random = random * 1664525u + 1013904223u;
            target = double(random >> 8) / 8388608.0 - 1;
            random = random * 1664525u + 1013904223u;
            countdown = 1 + int(rate * (.6 + 1.4 * double(random >> 8) / 16777216.0));
        }
        smooth += (target - smooth) * coefficient;
        value += (smooth - value) * coefficient;
        return value;
    }
};
// [7/6] Pade tangent: relative error < 4e-9 on [0, 0.45 pi].
double padeTan(double x) noexcept
{
    const double x2 = x * x;
    return x * (135135 + x2 * (-17325 + x2 * (378 - x2))) / (135135 + x2 * (-62370 + x2 * (3150 - 28 * x2)));
}
// Smooth bounded sigmoid (tanh within 0.024), flat beyond |x| = 3.
double soft(double x) noexcept
{
    x = std::clamp(x, -3.0, 3.0);
    const double x2 = x * x;
    return x * (27 + x2) / (27 + 9 * x2);
}
struct Ramp
{
    double current = 0, target = 0, step = 0;
    int remaining = 0;
    void set(double value, int samples, bool snap) noexcept
    {
        if (snap) { current = target = value; remaining = 0; step = 0; }
        else if (value != target) { target = value; remaining = samples; step = (target - current) / samples; }
    }
    double next() noexcept
    {
        if (remaining > 0) { current += step; if (--remaining == 0) current = target; }
        return current;
    }
};
struct OscControls
{
    Ramp level, position, tune, detune, width, spread;
    void set(const OscillatorSettings& o, int samples, bool snap) noexcept
    {
        level.set(o.level, samples, snap); position.set(o.position, samples, snap); tune.set(o.tune, samples, snap); detune.set(o.detune, samples, snap); width.set(o.pulseWidth, samples, snap);
        spread.set(o.unisonCents, samples, snap);
    }
};
struct LayerMode
{
    EngineKind engine = EngineKind::analog;
    FilterMode filter = FilterMode::lowpass;
    std::array<Waveform, 2> waves {};
    std::array<int, 2> tables {};
    struct ClockMode
    {
        LfoShape shape = LfoShape::sine;
        bool synced = false;
        bool operator==(const ClockMode&) const = default;
    };
    std::array<ClockMode, 2> clocks {};
    // Amounts live in independent ramps. Only applicable enabled routes enter
    // this mode, so an inactive bank edit cannot fade the audible bank.
    std::array<ModRoute, 16> routes {};
    std::array<size_t, 16> indices {};
    size_t routeCount = 0;
    unsigned clockMask = 0;
    AlgorithmRevision revision = legacyAlgorithmRevision;
    // Voices per oscillator and the filter type; other than 1 and Clean only for a revision-2 wavetable layer.
    std::array<int, 2> unison {1, 1};
    FilterType filterType = FilterType::clean;
    bool operator==(const LayerMode&) const = default;
};
struct LayerControls
{
    Ramp enabled, level, pan, cutoff, resonance, noise, envelopeAmount, width, drift, vowelSize;
    Ramp transition;
    LayerMode mode, pending;
    bool switching = false;
    std::array<OscControls, 2> oscillators;
    std::array<Ramp, 16> amounts;
    void set(const LayerSettings& l, int samples, bool snap) noexcept
    {
        enabled.set(l.enabled ? 1 : 0, samples, snap); level.set(l.level, samples, snap); pan.set(l.pan, samples, snap);
        cutoff.set(l.cutoffHz, samples, snap); resonance.set(l.resonance, samples, snap); noise.set(l.noiseLevel, samples, snap);
        envelopeAmount.set(l.filterEnvelopeOctaves, samples, snap); width.set(l.unisonWidth, samples, snap); drift.set(l.drift, samples, snap);
        vowelSize.set(l.vowelSize, samples, snap);
        for (size_t i = 0; i < oscillators.size(); ++i) oscillators[i].set(l.oscillators[i], samples, snap);
        LayerMode requested {l.engine, l.filterMode,
            {l.oscillators[0].waveform, l.oscillators[1].waveform}, {l.oscillators[0].tableIndex, l.oscillators[1].tableIndex}};
        for (size_t i = 0; i < 2; ++i) requested.clocks[i] = {l.lfos[i].shape, l.lfos[i].synced};
        requested.revision = l.algorithmRevision;
        if (l.engine == EngineKind::wavetable && l.algorithmRevision != legacyAlgorithmRevision)
        { requested.unison = {l.oscillators[0].unison, l.oscillators[1].unison}; requested.filterType = l.filterType; }
        const auto bank = l.engine == EngineKind::analog ? ModBank::analog : ModBank::wavetable;
        for (size_t i = 0; i < amounts.size(); ++i)
        {
            amounts[i].set(l.modulation[i].amount, samples, snap);
            const auto& route = l.modulation[i];
            if (!route.enabled || (route.bank != ModBank::common && route.bank != bank)) continue;
            requested.routes[i] = route; requested.routes[i].amount = 0;
            requested.indices[requested.routeCount++] = i;
            if (route.source == ModSource::lfo1) requested.clockMask |= 1;
            if (route.source == ModSource::lfo2) requested.clockMask |= 2;
        }
        if (snap) { mode = pending = requested; switching = false; transition.set(1, samples, true); }
        else if (requested != pending)
        {
            pending = requested;
            switching = pending != mode;
            transition.set(switching ? 0 : 1, samples, false);
        }
    }
    double transitionGain(int samples) noexcept
    {
        const double gain = transition.next();
        if (switching && transition.remaining == 0)
        {
            mode = pending; switching = false;
            transition.set(1, samples, false);
        }
        return gain;
    }
};
struct LayerVoice
{
    juce::ADSR amp, filter;
    std::array<double, 2> phase {};
    double ic1 = 0, ic2 = 0;
    uint32_t random = 1;
    // Revision 2: contours, ladder stages, smoothed velocity. Drift and the
    // per-voice component tolerances persist across notes until engine reset.
    Contour modernAmp, modernFilter;
    std::array<double, 4> ladder {};
    double gain = 0;
    std::array<Drift, 2> drift {};
    std::array<double, 2> tolerance {};
    // A wavetable voice slot's fixed pitch offsets at full drift, up to 25 cents.
    std::array<double, 2> driftOffset {};
    double cutoffTolerance = 1;
    std::array<int, 2> levelHints {};
    // Wavetable unison: each oscillator's voices after its first (which uses phase), and the right side's filters.
    std::array<std::array<double, 6>, 2> unisonPhase {};
    double ic1R = 0, ic2R = 0;
    std::array<double, 4> ladderR {};
    // Vowel: each formant's two state-variable histories, the left (or mono) side's three formants first.
    std::array<double, 12> vowel {};
    struct AnalogFilterTerms
    {
        std::array<std::uint64_t, 3> keys {};
        double G = 0, G2 = 0, G3 = 0, G4 = 0, k = 0;
        double oneMinusG = 0, denominator = 0, passbandGain = 0;
        bool valid = false;
    } analogFilter;
    void reset() noexcept
    {
        amp.reset(); filter.reset(); phase = {}; ic1 = ic2 = 0; random = 1;
        modernAmp.reset(); modernFilter.reset(); ladder = {}; gain = 0;
        unisonPhase = {}; ic1R = ic2R = 0; ladderR = {}; vowel = {};
        analogFilter.valid = false;
    }
    double noise() noexcept
    {
        random = random * 1664525u + 1013904223u;
        return double(random >> 8) / 8388608.0 - 1;
    }
};
static_assert(sizeof(LayerVoice::AnalogFilterTerms) == 96);
struct Voice
{
    bool active = false, held = false;
    int note = 0, channel = 0;
    uint64_t age = 0;
    double frequency = 440, velocity = 0;
    Ramp pitch, velocityRamp;
    std::array<LayerVoice, 4> layers;
    void kill() noexcept { active = held = false; for (auto& l : layers) l.reset(); }
    void release() noexcept
    { held = false; for (auto& l : layers) { l.amp.noteOff(); l.filter.noteOff(); l.modernAmp.noteOff(); l.modernFilter.noteOff(); } }
};
struct FrameOsc { double level, position, ratio, width; };
// One layer's unison voices: pitch ratios and side gains with the 1/sqrt(n) level law, keyed by counts, spread and width.
struct UnisonMath
{
    struct Oscillator { int count = 1; std::array<double, 7> ratio {}, left {}, right {}; };
    std::array<Oscillator, 2> oscillators;
    std::array<int, 2> counts {};
    std::array<double, 2> spread {};
    double width = 0;
    bool valid = false;
};
struct FrameLayer
{
    double left, right, cutoff, damping, noise, envelopeAmount;
    std::array<FrameOsc, 2> oscillators;
    double gain, level, pan, resonance;
    std::array<double, 2> tune, detune, lfos;
    std::array<double, 16> amounts;
    // Set when either oscillator has more than one voice; the layer then renders each side separately.
    const UnisonMath* unison = nullptr;
    // Drift's wander depth (a fraction of the pitch per unit of the drift process) and offset scale; zero at drift 0.
    double wander = 0, offset = 0;
    // The Vowel filter's formant scale: 2 halves every formant.
    double vowelSize = 1;
};
// Revision-2 filter terms for one layer and internal frame. Each entry is
// keyed by the exact bits of its inputs, so a later voice with equal inputs
// reads the value it would compute itself.
struct ModernFilterTerms
{
    std::uint64_t exponentKey = 0, resonanceKey = 0;
    std::array<std::uint64_t, 4> coefficientKeys {};
    double exponential = 0, resonance = 0, damping = 0, a1 = 0, a2 = 0, a3 = 0;
    bool exponentValid = false, resonanceValid = false, coefficientsValid = false;
    // Ladder and Classic: the coefficient keys guard these instead of the state-variable terms.
    LayerVoice::AnalogFilterTerms ladder;
    // Vowel: each formant's band-pass damping and state-variable terms.
    std::array<double, 3> vowelK {}, vowelA1 {}, vowelA2 {}, vowelA3 {};
};
struct Clock
{
    double phase = 0;
    Ramp frequency;
    bool initialized = false;
    double next(const LfoSettings& settings, const LayerMode::ClockMode& mode,
                std::optional<double> tempo, double rate, int samples, bool used) noexcept
    {
        double value = 0;
        if (used)
        {
            switch (mode.shape)
            {
                case LfoShape::sine: value = std::sin(2 * pi * phase); break;
                case LfoShape::triangle: value = 1 - 4 * std::abs(phase - .5); break;
                case LfoShape::saw: value = 2 * phase - 1; break;
                case LfoShape::square: value = phase < .5 ? 1 : -1; break;
            }
        }
        if (!mode.synced || tempo)
        {
            const double hz = mode.synced ? *tempo / (60 * settings.beats) : settings.rateHz;
            frequency.set(hz, samples, !initialized); initialized = true;
            phase += frequency.next() / rate;
            phase -= std::floor(phase);
        }
        return value;
    }
};
}

struct SynthEngine::State
{
    SynthPatch patch;
    double rate = 0;
    int maximumBlock = 0, rampSamples = 240;
    int midiScale = 1, factor = 1, latency = 0;
    RenderQuality quality = RenderQuality::realtime;
    std::unique_ptr<juce::dsp::Oversampling<float>> resampler;
    juce::AudioBuffer<float> work;
    std::array<float,8> lastHigh {};
    bool delayHighSample = false;
    std::vector<float> highGains,hostGains,gainDelay;
    size_t gainPosition = 0;
    uint64_t age = 0;
    const Tables* tables = nullptr;
    const ModernTables* modernTables = nullptr;
    std::array<std::array<ContourShape, 2>, 4> contours {};
    std::uint32_t phaseSeed = 1, unisonSeed = 1;
    double driftCoefficient = 0, velocitySmoothing = 1;
    std::array<Voice, 16> voices;
    struct HeldKey { uint64_t age = 0; int velocity = 0; };
    std::array<HeldKey, 128 * 16> keys {};
    VoiceMode mode = VoiceMode::polyphonic, pendingMode = VoiceMode::polyphonic;
    Ramp performanceGain;
    bool switchingMode = false;
    Ramp master;
    std::array<LayerControls, 4> controls;
    struct BaseLayerMath
    {
        double panArgument=0, cosine=0, sine=0;
        std::array<double,2> ratioArgument {}, ratio {};
        bool panValid=false;
        std::array<bool,2> ratioValid {};
    };
    std::array<BaseLayerMath,4> baseMath;
    std::array<UnisonMath, 4> unisonMath;
    std::array<std::array<Clock, 2>, 4> clocks;
    std::array<FrameLayer, 4> parallelFrames;
    bool parallelModulated=false;
    std::array<int,16> parallelActiveFrames{};
    unsigned parallelRetireMask=0;
    unsigned excludedMelodicLayers=0,freshSourceOnsetRequired=0;
    std::array<float*, 8> parallelOutputs {};
    std::optional<double> tempo;
    bool missingTempo = false;

    void timing(std::optional<double> bpm) noexcept
    {
        if (bpm && (!std::isfinite(*bpm) || *bpm < 20 || *bpm > 400)) bpm.reset();
        tempo = bpm; missingTempo = false;
        for (const auto& l : patch.layers) if (l.enabled)
            for (const auto& clock : l.lfos) missingTempo |= clock.synced && !tempo;
    }

    void applyPatch(bool snap) noexcept
    {
        if(snap)for(auto& math:baseMath){math.panValid=false;math.ratioValid={};}
        const auto requested = patch.performance.mode;
        if (snap)
        {
            mode = pendingMode = requested; switchingMode = false;
            performanceGain.set(1, rampSamples, true);
        }
        else if (requested != pendingMode)
        {
            pendingMode = requested; switchingMode = pendingMode != mode;
            performanceGain.set(switchingMode ? 0 : 1, rampSamples, false);
        }
        master.set(patch.masterLevel, rampSamples, snap);
        driftCoefficient = rate > 0 ? 1 - std::exp(-1 / (.35 * rate)) : 0;
        velocitySmoothing = rate > 0 ? 1 - std::exp(-1 / (.003 * rate)) : 1;
        for (size_t i = 0; i < controls.size(); ++i)
        {
            const auto& l = patch.layers[i];
            controls[i].set(l, rampSamples, snap);
            const bool warm = l.engine == EngineKind::wavetable && l.algorithmRevision != legacyAlgorithmRevision;
            contours[i][0].configure(l.ampEnvelope, rate, warm ? l.attackCurve : .25f); contours[i][1].configure(l.filterEnvelope, rate);
            for (auto& v : voices)
            {
                // snap is used after reset/prepare: voices are idle and sample
                // rate changes require recalculation even for identical settings.
                updateEnvelope(v.layers[i].amp, l.ampEnvelope, snap);
                updateEnvelope(v.layers[i].filter, l.filterEnvelope, snap);
            }
        }
    }
    bool modern(size_t layer) const noexcept { return controls[layer].mode.revision != legacyAlgorithmRevision; }
    double randomPhase() noexcept
    {
        phaseSeed = phaseSeed * 1664525u + 1013904223u;
        return double(phaseSeed >> 8) / 16777216.0;
    }
    // A separate sequence, so oscillator phases match those drawn before unison existed.
    double randomUnisonPhase() noexcept
    {
        unisonSeed = unisonSeed * 1664525u + 1013904223u;
        return double(unisonSeed >> 8) / 16777216.0;
    }
    const UnisonMath& unisonFor(size_t i, std::array<double, 2> spread, double width) noexcept
    {
        auto& u = unisonMath[i]; const auto counts = controls[i].mode.unison;
        if (u.valid && u.counts == counts && u.spread == spread && u.width == width) return u;
        for (size_t j = 0; j < 2; ++j)
        {
            auto& o = u.oscillators[j]; const int n = o.count = counts[j];
            const double scale = 1 / std::sqrt(double(n));
            double side = -1;
            for (int k = 0; k < n; ++k)
            {
                // Voices spread evenly over +-spread cents; an odd count's middle voice stays centred and the
                // rest alternate sides from the lowest, on the left.
                o.ratio[size_t(k)] = n == 1 ? 1 : std::exp2(spread[j] * (2.0 * k / (n - 1) - 1) / 1200);
                const bool centre = n % 2 == 1 && k == n / 2;
                const double p = centre ? 0 : width * side;
                if (!centre) side = -side;
                o.left[size_t(k)] = p == 0 ? scale : scale * std::sqrt(2.0) * std::cos((p + 1) * pi / 4);
                o.right[size_t(k)] = p == 0 ? scale : scale * std::sqrt(2.0) * std::sin((p + 1) * pi / 4);
            }
        }
        u.counts = counts; u.spread = spread; u.width = width; u.valid = true;
        return u;
    }
    void start(Voice& voice, int key) noexcept
    {
        const int note = key % 128, channel = key / 128 + 1;
        // Revision-1 layers restart from silence. A sounding revision-2 layer
        // keeps phase, filter and envelope level, so retrigger and stealing are click-free.
        std::array<bool, 4> continuing {};
        for (size_t i = 0; i < voice.layers.size(); ++i)
            continuing[i] = voice.active && modern(i) && voice.layers[i].modernAmp.active();
        voice.active = voice.held = false;
        for (size_t i = 0; i < voice.layers.size(); ++i) if (!continuing[i]) voice.layers[i].reset();
        voice.active = voice.held = true; voice.note = note; voice.channel = channel;
        voice.velocity = double(keys[size_t(key)].velocity) / 127;
        voice.frequency = 440 * std::exp2((note - 69.0) / 12);
        voice.age = keys[size_t(key)].age;
        voice.pitch.set(note, 0, true); voice.velocityRamp.set(voice.velocity, 0, true);
        for (size_t i = 0; i < voice.layers.size(); ++i)
        {
            auto& l = voice.layers[i];
            l.random = static_cast<uint32_t>(1 + note * 131 + channel * 17 + i * 7919);
            l.amp.noteOn(); l.filter.noteOn();
            l.modernAmp.noteOn(); l.modernFilter.noteOn();
            if (modern(i) && !continuing[i])
            {
                for (auto& phase : l.phase) phase = randomPhase();
                for (auto& oscillator : l.unisonPhase) for (auto& phase : oscillator) phase = randomUnisonPhase();
                l.gain = voice.velocity;
            }
            if((excludedMelodicLayers|freshSourceOnsetRequired)&(1u<<i))l.reset();
        }
    }
    void seedVoices() noexcept
    {
        const auto hash = [](std::uint32_t x) {
            x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x;
        };
        const auto bipolar = [&](std::uint32_t x) { return double(hash(x) >> 8) / 8388608.0 - 1; };
        phaseSeed = 0x2545f491u; unisonSeed = 0x6c8e9cf5u;
        for (std::uint32_t v = 0; v < voices.size(); ++v)
            for (std::uint32_t i = 0; i < 4; ++i)
            {
                auto& l = voices[v].layers[i];
                const std::uint32_t base = (v * 4 + i) * 8 + 1;
                for (std::uint32_t j = 0; j < 2; ++j)
                {
                    l.drift[j] = {0, 0, 0, 0, hash(base + j) | 1u};
                    l.tolerance[j] = 2.5 * std::log(2.0) / 1200 * bipolar(base + 2 + j);
                    l.driftOffset[j] = 25 * std::log(2.0) / 1200 * bipolar(base + 5 + j);
                }
                l.cutoffTolerance = 1 + .03 * bipolar(base + 4);
            }
    }
    int latestKey() const noexcept
    {
        int selected = -1;
        for (size_t i = 0; i < keys.size(); ++i)
            if (keys[i].age != 0 && (selected < 0 || keys[i].age > keys[size_t(selected)].age)) selected = int(i);
        return selected;
    }
    void selectMono(int key,unsigned returnedFresh=0) noexcept
    {
        auto& v = voices[0];
        const bool continuing = v.active && v.held;
        const auto currentPitch = v.pitch.current;
        if (!continuing || !patch.performance.legato) start(v, key);
        else
        {
            v.note = key % 128; v.channel = key / 128 + 1; v.age = keys[size_t(key)].age;
            v.velocityRamp.set(double(keys[size_t(key)].velocity) / 127, rampSamples, false);
            for(std::size_t i=0;i<4;++i)if(returnedFresh&(1u<<i)){
                auto& layer=v.layers[i];layer.random=static_cast<std::uint32_t>(1+v.note*131+v.channel*17+i*7919);layer.amp.noteOn();layer.filter.noteOn();layer.modernAmp.noteOn();layer.modernFilter.noteOn();layer.gain=v.velocity;
            }
        }
        const int glide = static_cast<int>(std::ceil(rate * patch.performance.glideSeconds));
        if (continuing && !patch.performance.legato) v.pitch.set(currentPitch, 0, true);
        v.pitch.set(v.note, glide, !continuing || glide == 0);
    }
    void noteOn(int note, int channel, int velocity) noexcept
    {
        const int key = (channel - 1) * 128 + note;
        keys[size_t(key)] = {++age, velocity};
        const unsigned returnedFresh=freshSourceOnsetRequired&~excludedMelodicLayers;
        freshSourceOnsetRequired&=~returnedFresh;
        if (mode == VoiceMode::monophonic) { selectMono(key,returnedFresh); return; }
        Voice* chosen = nullptr;
        for (auto& v : voices) if (v.active && v.held && v.note == note && v.channel == channel) { chosen = &v; break; }
        if (chosen == nullptr) for (auto& v : voices) if (!v.active) { chosen = &v; break; }
        if (chosen == nullptr) for (auto& v : voices) if (!v.held && (chosen == nullptr || v.age < chosen->age)) chosen = &v;
        if (chosen == nullptr) chosen = &*std::min_element(voices.begin(), voices.end(), [](const Voice& a, const Voice& b) { return a.age < b.age; });
        start(*chosen, key);
    }
    void rebuild() noexcept
    {
        for (auto& v : voices) v.kill();
        // Event/transition-only bounded selection: no held-key scan in ordinary frames.
        if (mode == VoiceMode::monophonic)
        { const int key = latestKey(); if (key >= 0) start(voices[0], key); return; }
        std::array<int, 16> latest; latest.fill(-1);
        for (size_t key = 0; key < keys.size(); ++key)
        {
            if (keys[key].age == 0) continue;
            for (size_t i = 0; i < latest.size(); ++i)
                if (latest[i] < 0 || keys[key].age > keys[size_t(latest[i])].age)
                {
                    for (size_t j = latest.size() - 1; j > i; --j) latest[j] = latest[j - 1];
                    latest[i] = int(key); break;
                }
        }
        for (size_t i = 0; i < latest.size(); ++i) if (latest[i] >= 0) start(voices[i], latest[i]);
    }
    void event(const juce::MidiMessageMetadata& midi) noexcept
    {
        if (midi.numBytes < 1) return;
        const auto* data = midi.data;
        if (data[0] == 0xfc) { for (auto& v : voices) v.kill(); keys = {}; age = 0; clocks = {}; return; }
        // Inspect the borrowed MIDI bytes. In particular, never copy SysEx.
        if (midi.numBytes != 3 || data[1] > 127 || data[2] > 127) return;
        const auto status = data[0] & 0xf0;
        const auto channel = (data[0] & 0x0f) + 1;
        if (status == 0x90 && data[2] != 0) { noteOn(data[1], channel, data[2]); return; }
        const bool noteOff = status == 0x80 || (status == 0x90 && data[2] == 0);
        const bool allOff = status == 0xb0 && data[1] == 123;
        const bool kill = status == 0xb0 && data[1] == 120;
        if (noteOff || allOff || kill)
        {
            if (noteOff) keys[size_t((channel - 1) * 128 + data[1])] = {};
            else for (int note = 0; note < 128; ++note) keys[size_t((channel - 1) * 128 + note)] = {};
            if (mode == VoiceMode::monophonic)
            {
                auto& v = voices[0];
                if (v.active && v.channel == channel && (!noteOff || v.note == data[1]))
                {
                    if (kill) v.kill();
                    const int key = latestKey();
                    if (key >= 0) selectMono(key);
                    else if (!kill && v.held) v.release();
                }
                return;
            }
            for (auto& v : voices)
                if (v.active && v.channel == channel && (!noteOff || v.note == data[1]))
                { if (kill) v.kill(); else v.release(); }
        }
    }
    double oscillator(Waveform wave, int table, const FrameOsc& f, EngineKind engine, double& phase, double frequency) const noexcept
    {
        const double increment = std::min(0.45, frequency * f.ratio / rate);
        double value = std::sin(2 * pi * phase);
        if (engine == EngineKind::wavetable)
            value = std::lerp(value, tables->read(table == 0 ? 0 : 2, phase, increment), f.position);
        else if (wave == Waveform::saw) value = tables->read(0, phase, increment);
        else if (wave == Waveform::triangle) value = tables->read(1, phase, increment);
        else if (wave == Waveform::pulse)
        {
            auto shifted = phase - f.width;
            if (shifted < 0) shifted += 1;
            value = tables->read(0, shifted, increment) - tables->read(0, phase, increment);
        }
        phase += increment;
        if (phase >= 1) phase -= 1;
        return value * f.level;
    }
    double modernOscillatorAtIncrement(Waveform wave, int table, const FrameOsc& f, bool analog, double& phase, int& hint, double increment) const noexcept
    {
        double value;
        if (!analog) value = modernTables->frames(table == 0 ? 0 : 1, modernTables->frameLevel(increment, hint), f.position, phase);
        else if (wave == Waveform::sine) value = modernTables->sine(phase);
        else
        {
            const int level = modernTables->basicLevel(increment, hint);
            if (wave == Waveform::pulse)
            {
                auto shifted = phase - f.width;
                if (shifted < 0) shifted += 1;
                value = modernTables->basic(0, level, shifted) - modernTables->basic(0, level, phase);
            }
            else value = modernTables->basic(wave == Waveform::saw ? 0 : 1, level, phase);
        }
        phase += increment;
        if (phase >= 1) phase -= 1;
        return value * f.level;
    }
    double modernOscillator(Waveform wave, int table, const FrameOsc& f, bool analog, double& phase, int& hint, double frequency) const noexcept
    {
        const double increment = std::clamp(frequency / rate, 0.0, .48);
        return modernOscillatorAtIncrement(wave, table, f, analog, phase, hint, increment);
    }
    double modernWarp(double hz) const noexcept
    {
        const double top = std::min(hz, 20000.0) / 20000, top2 = top * top;
        hz *= 1 + top2 * top2 * top2;
        return padeTan(pi * std::clamp(hz / rate, 1e-5, .49));
    }
    // Analog: TPT four-pole ladder, linear for the signal path (no folded
    // distortion) with a saturated resonance feedback, so full resonance
    // self-oscillates at a bounded level; outputs 24 dB low-pass (passband
    // restored to unity at any resonance), four-pole band-pass or 12 dB
    // high-pass. Wavetable: clean TPT SVF from Butterworth to Q 10. The top of
    // the cutoff range opens either filter past the audible band (doubling at
    // 20 kHz, clamped at 0.49 Fs).
    const LayerVoice::AnalogFilterTerms& analogTerms(LayerVoice& l, double cutoff, double r) const noexcept
    {
        const std::array<std::uint64_t, 3> keys {std::bit_cast<std::uint64_t>(cutoff),
            std::bit_cast<std::uint64_t>(r), std::bit_cast<std::uint64_t>(rate)};
        auto& coefficients = l.analogFilter;
        if (!coefficients.valid || coefficients.keys != keys)
        {
            const double g = modernWarp(cutoff);
            const double k = 4.3 * r * (.7 + .3 * r);
            const double G = g / (1 + g), G2 = G * G, G4 = G2 * G2;
            coefficients = {keys, G, G2, G * G2, G4, k, 1 - G, 1 + k * G4, 1 + k, true};
        }
        return coefficients;
    }
    static double analogSample(double signal, LayerVoice& l, const LayerVoice::AnalogFilterTerms& coefficients, FilterMode filter) noexcept
    { return ladderSample(signal, l.ladder, coefficients, filter); }
    static double ladderSample(double signal, std::array<double, 4>& s, const LayerVoice::AnalogFilterTerms& coefficients, FilterMode filter) noexcept
    {
        const double G = coefficients.G, G2 = coefficients.G2, k = coefficients.k;
        constexpr double drive = 1.5;
        const double estimate = (coefficients.G4 * signal + coefficients.oneMinusG *
            (coefficients.G3 * s[0] + G2 * s[1] + G * s[2] + s[3])) / coefficients.denominator;
        const double input = signal - k * soft(drive * estimate) / drive;
        std::array<double, 4> y;
        double stage = input;
        for (size_t n = 0; n < 4; ++n) { const double step = (stage - s[n]) * G; y[n] = step + s[n]; s[n] = y[n] + step; stage = y[n]; }
        if (filter == FilterMode::lowpass) return y[3] * coefficients.passbandGain;
        if (filter == FilterMode::bandpass) return 4 * (y[1] - 2 * y[2] + y[3]);
        return input - 2 * y[0] + y[1];
    }
    // Both sides' ladders in the two lanes of one SSE2 register, each lane exactly ladderSample's arithmetic.
    static std::array<double, 2> ladderPair(double left, double right, std::array<double, 4>& l, std::array<double, 4>& r,
                                            const LayerVoice::AnalogFilterTerms& c, FilterMode filter) noexcept
    {
#if defined(_M_X64) || defined(__SSE2__)
        const auto one = [](double v) { return _mm_set1_pd(v); };
        const __m128d G = one(c.G), G2 = one(c.G2), drive = one(1.5), signal = _mm_set_pd(right, left);
        std::array<__m128d, 4> s;
        for (size_t n = 0; n < 4; ++n) s[n] = _mm_set_pd(r[n], l[n]);
        const __m128d sum = _mm_add_pd(_mm_add_pd(_mm_add_pd(_mm_mul_pd(one(c.G3), s[0]), _mm_mul_pd(G2, s[1])), _mm_mul_pd(G, s[2])), s[3]);
        const __m128d estimate = _mm_div_pd(_mm_add_pd(_mm_mul_pd(one(c.G4), signal), _mm_mul_pd(one(c.oneMinusG), sum)), one(c.denominator));
        const __m128d x = _mm_min_pd(_mm_max_pd(_mm_mul_pd(drive, estimate), one(-3)), one(3)), x2 = _mm_mul_pd(x, x);
        const __m128d shaped = _mm_div_pd(_mm_mul_pd(x, _mm_add_pd(one(27), x2)), _mm_add_pd(one(27), _mm_mul_pd(one(9), x2)));
        const __m128d input = _mm_sub_pd(signal, _mm_div_pd(_mm_mul_pd(one(c.k), shaped), drive));
        std::array<__m128d, 4> y;
        __m128d stage = input;
        for (size_t n = 0; n < 4; ++n)
        {
            const __m128d step = _mm_mul_pd(_mm_sub_pd(stage, s[n]), G);
            y[n] = _mm_add_pd(step, s[n]); s[n] = _mm_add_pd(y[n], step); stage = y[n];
            _mm_storel_pd(&l[n], s[n]); _mm_storeh_pd(&r[n], s[n]);
        }
        const __m128d out = filter == FilterMode::lowpass ? _mm_mul_pd(y[3], one(c.passbandGain))
            : filter == FilterMode::bandpass ? _mm_mul_pd(one(4), _mm_add_pd(_mm_sub_pd(y[1], _mm_mul_pd(one(2), y[2])), y[3]))
            : _mm_add_pd(_mm_sub_pd(input, _mm_mul_pd(one(2), y[0])), y[1]);
        std::array<double, 2> result;
        _mm_storel_pd(&result[0], out); _mm_storeh_pd(&result[1], out);
        return result;
#else
        return {ladderSample(left, l, c, filter), ladderSample(right, r, c, filter)};
#endif
    }
    static double wavetableSample(double signal, double& ic1, double& ic2, const ModernFilterTerms& terms, FilterMode filter) noexcept
    {
        const double damping = terms.damping, a1 = terms.a1, a2 = terms.a2, a3 = terms.a3;
        const double v3 = signal - ic2, band = a1 * ic1 + a2 * v3, low = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * band - ic1; ic2 = 2 * low - ic2;
        return filter == FilterMode::lowpass ? low : filter == FilterMode::bandpass ? band : signal - damping * band - low;
    }
    // Vowel: three parallel band-passes at the formants of U, O, A, E and I, the cutoff gliding between neighbours
    // in log frequency from 100 Hz to 12 kHz and the size scaling them down; the resonance narrows them.
    static constexpr double vowelMakeup = 4.69;
    void vowelTerms(ModernFilterTerms& t, double cutoff, double r, double size) const noexcept
    {
        static constexpr std::array<std::array<double, 3>, 5> formants {{{300, 870, 2240}, {570, 840, 2410},
            {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}}};
        static constexpr std::array<std::array<double, 5>, 3> widths {{{133, 103, 90, 82, 77},
            {201, 147, 121, 105, 95}, {262, 174, 137, 116, 104}}};
        const double v = 4 * std::clamp(std::log(cutoff / 100) / std::log(120.0), 0.0, 1.0), w = 4 * r;
        const int a = std::min(int(v), 3), b = std::min(int(w), 3);
        for (size_t n = 0; n < 3; ++n)
        {
            const double centre = std::clamp(formants[size_t(a)][n] * std::pow(formants[size_t(a + 1)][n] / formants[size_t(a)][n], v - a) / size,
                                             50.0, .45 * rate);
            const double k = std::lerp(widths[n][size_t(b)], widths[n][size_t(b + 1)], w - b) / centre, g = padeTan(pi * centre / rate);
            t.vowelK[n] = k; t.vowelA1[n] = 1 / (1 + g * (g + k)); t.vowelA2[n] = g * t.vowelA1[n]; t.vowelA3[n] = g * t.vowelA2[n];
        }
    }
    static double vowelSample(double x, double* s, const ModernFilterTerms& t) noexcept
    {
        double sum = 0;
        for (size_t n = 0; n < 3; ++n, s += 2)
        {
            const double v3 = x - s[1], band = t.vowelA1[n] * s[0] + t.vowelA2[n] * v3, low = s[1] + t.vowelA2[n] * s[0] + t.vowelA3[n] * v3;
            s[0] = 2 * band - s[0]; s[1] = 2 * low - s[1]; sum += t.vowelK[n] * band;
        }
        return sum * vowelMakeup;
    }
    // Both sides' formants in the two lanes of one SSE2 register, each lane exactly vowelSample's arithmetic.
    static std::array<double, 2> vowelPair(double left, double right, double* s, const ModernFilterTerms& t) noexcept
    {
#if defined(_M_X64) || defined(__SSE2__)
        const __m128d x = _mm_set_pd(right, left), two = _mm_set1_pd(2);
        __m128d sum = _mm_setzero_pd();
        for (size_t n = 0; n < 3; ++n)
        {
            double* l = s + 2 * n; double* r = l + 6;
            const __m128d s0 = _mm_set_pd(r[0], l[0]), s1 = _mm_set_pd(r[1], l[1]), a2 = _mm_set1_pd(t.vowelA2[n]), v3 = _mm_sub_pd(x, s1);
            const __m128d band = _mm_add_pd(_mm_mul_pd(_mm_set1_pd(t.vowelA1[n]), s0), _mm_mul_pd(a2, v3));
            const __m128d low = _mm_add_pd(_mm_add_pd(s1, _mm_mul_pd(a2, s0)), _mm_mul_pd(_mm_set1_pd(t.vowelA3[n]), v3));
            const __m128d n0 = _mm_sub_pd(_mm_mul_pd(two, band), s0), n1 = _mm_sub_pd(_mm_mul_pd(two, low), s1);
            _mm_storel_pd(&l[0], n0); _mm_storeh_pd(&r[0], n0); _mm_storel_pd(&l[1], n1); _mm_storeh_pd(&r[1], n1);
            sum = _mm_add_pd(sum, _mm_mul_pd(_mm_set1_pd(t.vowelK[n]), band));
        }
        const __m128d out = _mm_mul_pd(sum, _mm_set1_pd(vowelMakeup));
        std::array<double, 2> result;
        _mm_storel_pd(&result[0], out); _mm_storeh_pd(&result[1], out);
        return result;
#else
        return {vowelSample(left, s, t), vowelSample(right, s + 6, t)};
#endif
    }
    // The voice's left and right samples; they are equal unless the layer has unison.
    std::array<double, 2> modernVoice(const Voice& v, LayerVoice& l, const FrameLayer& f, const LayerMode& mode, double env, ModernFilterTerms& terms) noexcept
    {
        const bool analog = mode.engine == EngineKind::analog;
        double signal = f.noise * l.noise(), right = signal;
        constexpr double driftDepth = 3 * 0.69314718055994531 / 1200;
        if (f.unison != nullptr)
            for (size_t j = 0; j < l.phase.size(); ++j)
            {
                // Every voice reads the frame level of the oscillator's highest voice.
                const auto& u = f.unison->oscillators[j]; const auto& o = f.oscillators[j];
                double frequency = v.frequency * o.ratio;
                if (f.wander != 0) frequency *= 1 + f.wander * l.drift[j].next(driftCoefficient, rate) + f.offset * l.driftOffset[j];
                const double increment = frequency / rate;
                const int level = modernTables->frameLevel(std::clamp(increment * u.ratio[size_t(u.count - 1)], 0.0, .48), l.levelHints[j]);
                const auto frames = modernTables->framePair(mode.tables[j] == 0 ? 0 : 1, level, o.position);
                for (int k = 0; k < u.count; ++k)
                {
                    double& phase = k == 0 ? l.phase[j] : l.unisonPhase[j][size_t(k - 1)];
                    const double value = ModernTables::read(frames, phase) * o.level;
                    phase += std::clamp(increment * u.ratio[size_t(k)], 0.0, .48);
                    if (phase >= 1) phase -= 1;
                    signal += value * u.left[size_t(k)]; right += value * u.right[size_t(k)];
                }
            }
        else
            for (size_t j = 0; j < l.phase.size(); ++j)
            {
                double frequency = v.frequency * f.oscillators[j].ratio;
                if (analog) frequency *= 1 + driftDepth * l.drift[j].next(driftCoefficient, rate) + l.tolerance[j];
                else if (f.wander != 0) frequency *= 1 + f.wander * l.drift[j].next(driftCoefficient, rate) + f.offset * l.driftOffset[j];
                signal += modernOscillator(mode.waves[j], mode.tables[j], f.oscillators[j], analog, l.phase[j], l.levelHints[j], frequency);
            }
        const double product = env * f.envelopeAmount;
        const auto exponential = [&] {
            const auto key = std::bit_cast<std::uint64_t>(product);
            if (!terms.exponentValid || terms.exponentKey != key)
            { terms.exponential = std::exp2(product); terms.exponentKey = key; terms.exponentValid = true; }
            return terms.exponential;
        };
        const auto resonance = [&] {
            const auto key = std::bit_cast<std::uint64_t>(f.damping);
            if (!terms.resonanceValid || terms.resonanceKey != key)
            { terms.resonance = std::clamp((2 - f.damping) / (1.9 * .95), 0.0, 1.0); terms.resonanceKey = key; terms.resonanceValid = true; }
            return terms.resonance;
        };
        if (analog)
        {
            // Component tolerance belongs to this voice. Reuse only pure results
            // with exactly equal inputs; the four ladder histories always advance.
            const double cutoff = f.cutoff * l.cutoffTolerance * exponential(), r = resonance();
            const auto& coefficients = analogTerms(l, cutoff, r);
            const double out = analogSample(signal, l, coefficients, mode.filter);
            return {out, out};
        }
        const std::array<std::uint64_t, 4> keys {std::bit_cast<std::uint64_t>(f.cutoff), std::bit_cast<std::uint64_t>(product),
                                                 std::bit_cast<std::uint64_t>(f.damping), std::bit_cast<std::uint64_t>(f.vowelSize)};
        const bool ladder = mode.filterType == FilterType::ladder || mode.filterType == FilterType::classic;
        const bool vowel = mode.filterType == FilterType::vowel;
        if (!terms.coefficientsValid || terms.coefficientKeys != keys)
        {
            const double cutoff = f.cutoff * exponential(), g = vowel ? 0 : modernWarp(cutoff);
            if (vowel) vowelTerms(terms, cutoff, resonance(), f.vowelSize);
            else if (ladder)
            {
                // Ladder is the analog engine's, without its cutoff tolerance. Classic feeds back harder and restores
                // little of the passband, so its bass drops as the resonance rises.
                const bool classic = mode.filterType == FilterType::classic;
                const double r = resonance(), q = std::sqrt(r);
                const double k = classic ? 4.44 * q * std::sqrt(q) : 4.3 * r * (.7 + .3 * r);
                const double G = g / (1 + g), G2 = G * G, G4 = G2 * G2;
                terms.ladder = {{}, G, G2, G * G2, G4, k, 1 - G, 1 + k * G4, classic ? std::pow(1 + k, .061) : 1 + k, true};
            }
            else
            {
                terms.damping = 1.41421356 - 1.31421356 * resonance();
                terms.a1 = 1 / (1 + g * (g + terms.damping)); terms.a2 = g * terms.a1; terms.a3 = g * terms.a2;
            }
            terms.coefficientKeys = keys; terms.coefficientsValid = true;
        }
        if (ladder)
        {
            if (f.unison != nullptr) return ladderPair(signal, right, l.ladder, l.ladderR, terms.ladder, mode.filter);
            const double out = ladderSample(signal, l.ladder, terms.ladder, mode.filter);
            return {out, out};
        }
        if (vowel)
        {
            if (f.unison != nullptr) return vowelPair(signal, right, l.vowel.data(), terms);
            const double out = vowelSample(signal, l.vowel.data(), terms);
            return {out, out};
        }
        const double out = wavetableSample(signal, l.ic1, l.ic2, terms, mode.filter);
        if (f.unison == nullptr) return {out, out};
        return {out, wavetableSample(right, l.ic1R, l.ic2R, terms, mode.filter)};
    }
    static void modulate(FrameLayer& f, const LayerMode& mode, double amp, double filter,
                         double velocity, double pitch) noexcept
    {
        const std::array<double, 7> sources {0, amp, filter, f.lfos[0], f.lfos[1], velocity,
                                           std::clamp((pitch - 60) / 60, -1.0, 1.0)};
        std::array<double, 17> sums {};
        for (size_t n = 0; n < mode.routeCount; ++n)
        {
            const auto i = mode.indices[n]; const auto& route = mode.routes[i];
            sums[size_t(route.target)] += sources[size_t(route.source)] * f.amounts[i];
        }
        const auto sum = [&](ModTarget target) { return sums[size_t(target)]; };
        const auto add = [&](double base, ModTarget target, double scale, double low, double high) {
            const auto contribution = sum(target);
            return contribution == 0 ? base : std::clamp(base + scale * contribution, low, high);
        };
        if (sum(ModTarget::level) != 0 || sum(ModTarget::pan) != 0)
        {
            const auto gain = f.gain * add(f.level, ModTarget::level, 1, 0, 1);
            const auto pan = (add(f.pan, ModTarget::pan, 1, -1, 1) + 1) * pi / 4;
            f.left = gain * std::cos(pan); f.right = gain * std::sin(pan);
        }
        for (size_t i = 0; i < 2; ++i)
        {
            auto& o = f.oscillators[i];
            const auto target = [i](ModTarget first) { return static_cast<ModTarget>(int(first) + int(i)); };
            o.level = add(o.level, target(ModTarget::osc1Level), 1, 0, 1);
            o.position = add(o.position, target(ModTarget::osc1Position), 1, 0, 1);
            o.width = add(o.width, target(ModTarget::osc1Width), .9, .05, .95);
            if (sum(target(ModTarget::osc1Tune)) != 0 || sum(target(ModTarget::osc1Detune)) != 0)
            {
                const auto tune = add(f.tune[i], target(ModTarget::osc1Tune), 24, -24, 24);
                const auto detune = add(f.detune[i], target(ModTarget::osc1Detune), 100, -100, 100);
                o.ratio = std::exp2((tune + detune * .01) / 12);
            }
        }
        f.noise = add(f.noise, ModTarget::noise, 1, 0, 1);
        if (sum(ModTarget::cutoff) != 0) f.cutoff = std::clamp(f.cutoff * std::exp2(8 * sum(ModTarget::cutoff)), 20.0, 20000.0);
        if (sum(ModTarget::resonance) != 0) f.damping = 2 - 1.9 * add(f.resonance, ModTarget::resonance, .95, 0, .95);
        f.envelopeAmount = add(f.envelopeAmount, ModTarget::filterEnvelopeAmount, 8, -8, 8);
    }
    FrameLayer layerFrame(size_t i, bool& shareable) noexcept
    {
        auto& c = controls[i]; FrameLayer f;
        // Keep the default arithmetic order exactly as the accepted path.
        const double enabled = c.enabled.next(); f.level = c.level.next();
        const double transition = c.transitionGain(rampSamples);
        f.gain = enabled * transition;
        const double gain = enabled * f.level * transition;
        f.pan = c.pan.next(); const double pan = (f.pan + 1) * pi / 4;
        auto& math=baseMath[i];
        const bool panChanged=!math.panValid || pan!=math.panArgument;
        if(panChanged)math.cosine=std::cos(pan);
        f.left = gain * math.cosine;
        if(panChanged)math.sine=std::sin(pan);
        f.right = gain * math.sine;
        if(panChanged){math.panArgument=pan;math.panValid=true;}
        f.cutoff = c.cutoff.next(); f.resonance = c.resonance.next(); f.damping = 2 - 1.9 * f.resonance;
        f.noise = c.noise.next(); f.envelopeAmount = c.envelopeAmount.next();
        for (size_t j = 0; j < f.oscillators.size(); ++j)
        {
            auto& o = c.oscillators[j];
            f.tune[j] = o.tune.next(); f.detune[j] = o.detune.next();
            const double oscillatorLevel=o.level.next(), oscillatorPosition=o.position.next();
            const double ratioArgument=(f.tune[j] + f.detune[j] * 0.01) / 12;
            if(!math.ratioValid[j] || ratioArgument!=math.ratioArgument[j])
            {math.ratio[j]=std::exp2(ratioArgument);math.ratioArgument[j]=ratioArgument;math.ratioValid[j]=true;}
            f.oscillators[j] = {oscillatorLevel, oscillatorPosition, math.ratio[j], o.width.next()};
            f.lfos[j] = clocks[i][j].next(patch.layers[i].lfos[j], c.mode.clocks[j], tempo, rate, rampSamples,
                                         (c.mode.clockMask & (1u << j)) != 0);
        }
        for (size_t j = 0; j < f.amounts.size(); ++j) f.amounts[j] = c.amounts[j].next();
        const std::array<double, 2> spread {c.oscillators[0].spread.next(), c.oscillators[1].spread.next()};
        const double width = c.width.next();
        if (c.mode.unison[0] > 1 || c.mode.unison[1] > 1) f.unison = &unisonFor(i, spread, width);
        // At drift 1 the wander's 9.5 s standard deviation is 2.86 cents, Diva's at its maximum.
        constexpr double wanderDepth = 7.27 * 0.69314718055994531 / 1200;
        const double drift = c.drift.next();
        f.wander = wanderDepth * drift * drift; f.offset = drift * drift;
        f.vowelSize = c.vowelSize.next();
        // Current mode is settled by transitionGain above. LFO sources are
        // rack-global; all voice-dependent or unknown sources keep the
        // original path. These flags/results live only for this frame.
        shareable = c.mode.routeCount != 0;
        for (size_t n = 0; n < c.mode.routeCount; ++n)
        {
            const auto source = c.mode.routes[c.mode.indices[n]].source;
            if (source != ModSource::none && source != ModSource::lfo1 && source != ModSource::lfo2)
            { shareable = false; break; }
        }
        return f;
    }
    template<bool separated>
    void frame(float* const* outputs, int sample, float* masterGains) noexcept
    {
        const double performance = performanceGain.next();
        if (switchingMode && performanceGain.remaining == 0)
        {
            mode = pendingMode; switchingMode = false; rebuild();
            performanceGain.set(1, rampSamples, false);
        }
        std::array<FrameLayer, 4> frameLayers, sharedModulated;
        std::array<bool, 4> shareable {}, sharedValid {};
        struct FrameFilterCoefficients
        {
            double cutoff = 0, damping = 0, envelopeProduct = 0;
            double a1 = 0, a2 = 0, a3 = 0;
            bool valid = false;
        };
        // Pure results live only for this internal frame and actual layer.
        std::array<FrameFilterCoefficients, 4> frameCoefficients {};
        std::array<ModernFilterTerms, 4> modernTerms {};
        for (size_t i = 0; i < controls.size(); ++i)
            frameLayers[i] = layerFrame(i, shareable[i]);
        double lsum = 0, rsum = 0;
        std::array<std::array<double, 2>, 4> layerSums {};
        for (auto& v : voices)
        {
            if (!v.active) continue;
            if (mode == VoiceMode::monophonic)
            {
                v.frequency = 440 * std::exp2((v.pitch.next() - 69.0) / 12);
                v.velocity = v.velocityRamp.next();
            }
            bool sounding = v.held;
            for (size_t i = 0; i < v.layers.size(); ++i)
            {
                if((excludedMelodicLayers|freshSourceOnsetRequired)&(1u<<i))continue;
                auto& l = v.layers[i]; const auto& mode = controls[i].mode;
                const bool revised = mode.revision != legacyAlgorithmRevision;
                double amp, env;
                if (revised) { amp = l.modernAmp.next(contours[i][0]); env = l.modernFilter.next(contours[i][1]); }
                else { amp = l.amp.getNextSample(); env = l.filter.getNextSample(); }
                FrameLayer modulated;
                const FrameLayer* current = &frameLayers[i];
                if (mode.routeCount != 0)
                {
                    if (shareable[i])
                    {
                        if (!sharedValid[i])
                        {
                            sharedModulated[i] = *current;
                            // Keep the original first active voice evaluation.
                            modulate(sharedModulated[i], mode, amp, env, v.velocity, v.pitch.current);
                            sharedValid[i] = true;
                        }
                        current = &sharedModulated[i];
                    }
                    else
                    {
                        modulated = *current;
                        modulate(modulated, mode, amp, env, v.velocity, v.pitch.current);
                        current = &modulated;
                    }
                }
                const auto& f = *current;
                if (revised)
                {
                    sounding |= l.modernAmp.active() && controls[i].enabled.current != 0;
                    l.gain += (v.velocity - l.gain) * velocitySmoothing;
                    const auto voice = modernVoice(v, l, f, mode, env, modernTerms[i]);
                    const double left = voice[0] * amp * l.gain, right = voice[1] * amp * l.gain;
                    if constexpr (separated)
                    { layerSums[i][0] += left * f.left; layerSums[i][1] += right * f.right; }
                    else
                    { lsum += left * f.left; rsum += right * f.right; }
                    continue;
                }
                // Disabled layers keep their envelopes in sync for live enabling,
                // but an inaudible tail must not retain a released musical note.
                sounding |= l.amp.isActive() && controls[i].enabled.current != 0;
                double signal = f.noise * l.noise();
                for (size_t j = 0; j < l.phase.size(); ++j)
                    signal += oscillator(mode.waves[j], mode.tables[j], f.oscillators[j], mode.engine, l.phase[j], v.frequency);
                const double envelopeProduct = env * f.envelopeAmount;
                auto& coefficients = frameCoefficients[i];
                const bool finiteKeys = std::isfinite(f.cutoff) && std::isfinite(f.damping)
                                     && std::isfinite(envelopeProduct);
                const bool sameKeys = finiteKeys && coefficients.valid
                    && std::bit_cast<std::uint64_t>(f.cutoff) == std::bit_cast<std::uint64_t>(coefficients.cutoff)
                    && std::bit_cast<std::uint64_t>(f.damping) == std::bit_cast<std::uint64_t>(coefficients.damping)
                    && std::bit_cast<std::uint64_t>(envelopeProduct) == std::bit_cast<std::uint64_t>(coefficients.envelopeProduct);
                double a1, a2, a3;
                if (sameKeys)
                {
                    a1 = coefficients.a1; a2 = coefficients.a2; a3 = coefficients.a3;
                }
                else
                {
                    // Preserve the original first matching voice arithmetic.
                    const double cutoff = std::clamp(f.cutoff * std::exp2(env * f.envelopeAmount) / rate, 1e-6, 0.45);
                    const double g = std::tan(pi * cutoff);
                    a1 = 1 / (1 + g * (g + f.damping));
                    a2 = g * a1; a3 = g * a2;
                    if (finiteKeys)
                        coefficients = { f.cutoff, f.damping, envelopeProduct, a1, a2, a3, true };
                }
                const double v3 = signal - l.ic2, band = a1 * l.ic1 + a2 * v3;
                const double low = l.ic2 + a2 * l.ic1 + a3 * v3;
                l.ic1 = 2 * band - l.ic1; l.ic2 = 2 * low - l.ic2;
                const double filtered = mode.filter == FilterMode::lowpass ? low
                    : mode.filter == FilterMode::bandpass ? band : signal - f.damping * band - low;
                const double output = filtered * amp * v.velocity;
                if constexpr (separated)
                { layerSums[i][0] += output * f.left; layerSums[i][1] += output * f.right; }
                else
                { lsum += output * f.left; rsum += output * f.right; }
            }
            if (!sounding) v.active = v.held = false;
        }
        const double masterGain = master.next();
        if constexpr (separated)
        {
            masterGains[sample] = static_cast<float>(masterGain);
            for (size_t i = 0; i < layerSums.size(); ++i)
                for (size_t channel = 0; channel < 2; ++channel)
                    outputs[i * 2 + channel][sample] = static_cast<float>(layerSums[i][channel] * headroom * performance);
        }
        else
        {
            const double gain = masterGain * headroom * performance;
            outputs[0][sample] = static_cast<float>(lsum * gain);
            outputs[1][sample] = static_cast<float>(rsum * gain);
        }
    }
    template<bool analog>
    void settledVoiceBlock(Voice& v, size_t i, const FrameLayer& f,
                           std::span<std::array<double, 2>> sums) noexcept
    {
        auto& l = v.layers[i];
        const auto& layerMode = controls[i].mode;
        std::array<double, 2> increments {};
        LayerVoice::AnalogFilterTerms coefficients;
        ModernFilterTerms terms;
        const double exponential = std::exp2(0.0);
        const double resonance = std::clamp((2 - f.damping) / (1.9 * .95), 0.0, 1.0);
        if constexpr (analog)
            coefficients = analogTerms(l, f.cutoff * l.cutoffTolerance * exponential, resonance);
        else
        {
            for (size_t j = 0; j < 2; ++j)
                increments[j] = std::clamp((v.frequency * f.oscillators[j].ratio) / rate, 0.0, .48);
            const double g = modernWarp(f.cutoff * exponential);
            terms.damping = 1.41421356 - 1.31421356 * resonance;
            terms.a1 = 1 / (1 + g * (g + terms.damping)); terms.a2 = g * terms.a1; terms.a3 = g * terms.a2;
        }
        for (auto& sum : sums)
        {
            const double amp = l.modernAmp.next(contours[i][0]);
            l.modernFilter.next(contours[i][1]);
            l.gain += (v.velocity - l.gain) * velocitySmoothing;
            double signal = f.noise * l.noise();
            constexpr double driftDepth = 3 * 0.69314718055994531 / 1200;
            for (size_t j = 0; j < 2; ++j)
            {
                double increment;
                if constexpr (analog)
                {
                    double frequency = v.frequency * f.oscillators[j].ratio;
                    frequency *= 1 + driftDepth * l.drift[j].next(driftCoefficient, rate) + l.tolerance[j];
                    increment = std::clamp(frequency / rate, 0.0, .48);
                }
                else increment = increments[j];
                signal += modernOscillatorAtIncrement(layerMode.waves[j], layerMode.tables[j], f.oscillators[j],
                                                      analog, l.phase[j], l.levelHints[j], increment);
            }
            const double filtered = analog ? analogSample(signal, l, coefficients, layerMode.filter)
                                           : wavetableSample(signal, l.ic1, l.ic2, terms, layerMode.filter);
            const double output = filtered * amp * l.gain;
            sum[0] += output * f.left; sum[1] += output * f.right;
        }
    }
    bool layerOutputs(const std::array<juce::AudioBuffer<float>*, 4>& layers, std::span<float> gains,
                      std::array<float*, 9>& outputs) const noexcept
    {
        if (rate == 0 || layers[0] == nullptr) return false;
        const int count = layers[0]->getNumSamples();
        if (count < 1 || count > maximumBlock || gains.size() != static_cast<size_t>(count)) return false;
        for (const auto* layer : layers)
            if (layer == nullptr || layer->getNumChannels() != 2 || layer->getNumSamples() != count) return false;
        for (size_t i = 0; i < layers.size(); ++i)
            for (int channel = 0; channel < 2; ++channel) outputs[i * 2 + channel] = layers[i]->getWritePointer(channel);
        outputs.back() = gains.data();
        const auto bytes = static_cast<uintptr_t>(count) * sizeof(float);
        for (size_t i = 0; i < outputs.size(); ++i) for (size_t j = 0; j < i; ++j)
        {
            const auto first = reinterpret_cast<uintptr_t>(outputs[i]), second = reinterpret_cast<uintptr_t>(outputs[j]);
            if ((first > second ? first - second : second - first) < bytes) return false;
        }
        return true;
    }
    bool settledLayers(unsigned excluded=0) const noexcept
    {
        if (mode != VoiceMode::polyphonic || switchingMode
            || performanceGain.remaining != 0 || master.remaining != 0) return false;
        for (const auto& voice : voices) if (voice.active && !voice.held) return false;
        const auto settled = [](const auto&... ramps) { return ((ramps.remaining == 0) && ...); };
        for (const auto& c : controls)
        {
            if(excluded&(1u<<std::size_t(&c-controls.data())))continue;
            if (c.switching || c.mode.revision == legacyAlgorithmRevision || c.mode.routeCount != 0 || c.envelopeAmount.current != 0
                || c.mode.unison[0] > 1 || c.mode.unison[1] > 1 || c.mode.filterType != FilterType::clean
                || (c.mode.engine == EngineKind::wavetable && (c.drift.current != 0 || c.drift.remaining != 0))
                || !settled(c.enabled, c.level, c.pan, c.cutoff, c.resonance, c.noise, c.envelopeAmount, c.transition, c.width, c.vowelSize)) return false;
            for (const auto& o : c.oscillators)
                if (!settled(o.level, o.position, o.tune, o.detune, o.width, o.spread)) return false;
            for (const auto& amount : c.amounts) if (amount.remaining != 0) return false;
        }
        return true;
    }
    bool modernParallelLayers(unsigned excluded=0) const noexcept
    {
        if(mode!=VoiceMode::polyphonic||switchingMode||performanceGain.remaining||master.remaining)return false;
        for(std::size_t i=0;i<4;++i)if(!(excluded&(1u<<i))&&(controls[i].switching||controls[i].mode.revision==legacyAlgorithmRevision))return false;
        return true;
    }
    void prepareParallelVoices(int count) noexcept
    {
        parallelRetireMask=0;
        for(size_t voice=0;voice<voices.size();++voice){
            const auto& v=voices[voice];auto& frames=parallelActiveFrames[voice];frames=v.active?count:0;
            if(!v.active||v.held)continue;
            if(voice){
                const auto& previous=voices[voice-1];bool same=previous.active&&!previous.held;
                for(size_t layer=0;same&&layer<4;++layer)if(!(excludedMelodicLayers&(1u<<layer)))same=v.layers[layer].modernAmp.sameState(previous.layers[layer].modernAmp);
                if(same){
                    // Identical contour states and shared shapes/enable ramps
                    // have the same forecast; each live history still advances.
                    frames=parallelActiveFrames[voice-1];
                    if(parallelRetireMask&(1u<<(voice-1)))parallelRetireMask|=1u<<voice;
                    continue;
                }
            }
            std::array<Contour,4> amplitudes;std::array<Ramp,4> enabled;
            for(size_t layer=0;layer<4;++layer){amplitudes[layer]=v.layers[layer].modernAmp;enabled[layer]=controls[layer].enabled;}
            // Predict the serial retirement frame on copies; live histories still
            // advance once, on their owning streams, through that exact frame.
            for(int n=0;n<count;++n){
                bool sounding=false;
                for(size_t layer=0;layer<4;++layer)if(!(excludedMelodicLayers&(1u<<layer))){const auto gain=enabled[layer].next();amplitudes[layer].next(contours[layer][0]);sounding|=amplitudes[layer].active()&&gain!=0;}
                if(!sounding){frames=n+1;parallelRetireMask|=1u<<voice;break;}
            }
        }
    }
    void renderModulatedRange(float* const* outputs,size_t firstLayer,size_t lastLayer,int offset,int count) noexcept
    {
        for(size_t i=firstLayer;i<lastLayer;++i)for(int n=offset;n<offset+count;++n)
        {
            // The stream owns this layer's controls, clocks and voice histories.
            bool shareable=false;const auto base=layerFrame(i,shareable);
            if((excludedMelodicLayers|freshSourceOnsetRequired)&(1u<<i)){outputs[i*2][n]=outputs[i*2+1][n]=0;continue;}
            const auto& mode=controls[i].mode;
            FrameLayer shared;bool sharedValid=false;ModernFilterTerms terms{};double left=0,right=0;
            size_t voice=0;
            for(auto& v:voices)
            {
                if(n>=parallelActiveFrames[voice++])continue;
                auto& l=v.layers[i];const double amp=l.modernAmp.next(contours[i][0]),env=l.modernFilter.next(contours[i][1]);
                const FrameLayer* current=&base;FrameLayer modulated;
                if(mode.routeCount)
                {
                    if(shareable)
                    {
                        if(!sharedValid){shared=base;modulate(shared,mode,amp,env,v.velocity,v.pitch.current);sharedValid=true;}
                        current=&shared;
                    }
                    else{modulated=base;modulate(modulated,mode,amp,env,v.velocity,v.pitch.current);current=&modulated;}
                }
                const auto& f=*current;l.gain+=(v.velocity-l.gain)*velocitySmoothing;
                const auto out=modernVoice(v,l,f,mode,env,terms);
                left+=out[0]*amp*l.gain*f.left;right+=out[1]*amp*l.gain*f.right;
            }
            outputs[i*2][n]=static_cast<float>(left*headroom*performanceGain.current);
            outputs[i*2+1][n]=static_cast<float>(right*headroom*performanceGain.current);
        }
    }
    void prepareSettledLayers(int count, float* masterGains, std::array<FrameLayer, 4>& frameLayers) noexcept
    {
        for (size_t i = 0; i < controls.size(); ++i)
        {
            bool unused = false;
            frameLayers[i] = layerFrame(i, unused);
        }
        // No routes means no clock value is consumed, but their phase and
        // frequency ramps still advance with precisely the original recurrence.
        for (int sample = 1; sample < count; ++sample)
            for (size_t i = 0; i < controls.size(); ++i)
                if(excludedMelodicLayers&(1u<<i)){bool unused=false;(void)layerFrame(i,unused);}
                else
                for (size_t j = 0; j < 2; ++j)
                    clocks[i][j].next(patch.layers[i].lfos[j], controls[i].mode.clocks[j], tempo, rate, rampSamples, false);
        std::fill_n(masterGains, count, static_cast<float>(master.current));
    }
    void renderSettledRange(float* const* outputs, const std::array<FrameLayer, 4>& frameLayers,
                            size_t firstLayer, size_t lastLayer, int offset, int count) noexcept
    {
        for (int first = offset; first < offset + count; first += 64)
        {
            const int length = std::min(64, offset + count - first);
            for (size_t i = firstLayer; i < lastLayer; ++i)
            {
                if((excludedMelodicLayers|freshSourceOnsetRequired)&(1u<<i)){std::fill_n(outputs[i*2]+first,length,0.f);std::fill_n(outputs[i*2+1]+first,length,0.f);continue;}
                std::array<std::array<double, 2>, 64> sums {};
                const auto& f = frameLayers[i];
                for (auto& v : voices)
                {
                    if (!v.active) continue;
                    const auto samples = std::span(sums).first(static_cast<size_t>(length));
                    if (controls[i].mode.engine == EngineKind::analog) settledVoiceBlock<true>(v, i, f, samples);
                    else settledVoiceBlock<false>(v, i, f, samples);
                }
                for (int sample = 0; sample < length; ++sample)
                    for (size_t channel = 0; channel < 2; ++channel)
                        outputs[i * 2 + channel][first + sample] = static_cast<float>(
                            sums[static_cast<size_t>(sample)][channel] * headroom * performanceGain.current);
            }
        }
    }
    void renderSettledLayers(float* const* outputs, int count, float* masterGains) noexcept
    {
        std::array<FrameLayer, 4> frameLayers;
        prepareSettledLayers(count, masterGains, frameLayers);
        renderSettledRange(outputs, frameLayers, 0, 4, 0, count);
    }
    template<bool separated>
    void render(float* const* outputs, int count, const juce::MidiBuffer& midi, float* masterGains = nullptr) noexcept
    {
        if constexpr (separated)
            if (midi.isEmpty() && settledLayers())
            { renderSettledLayers(outputs, count, masterGains); return; }
        auto event = midi.begin(); const auto end = midi.end();
        for (int sample = 0; sample < count; ++sample)
        {
            while (event != end && std::int64_t((*event).samplePosition) * midiScale <= sample)
            {
                const auto metadata = *event;
                if (metadata.samplePosition >= 0) this->event(metadata);
                ++event;
            }
            frame<separated>(outputs, sample, masterGains);
        }
    }
    void renderHigher(int count,const juce::MidiBuffer& midi) noexcept
    {
        work.clear();
        auto host=juce::dsp::AudioBlock<float>(work).getSubBlock(0,static_cast<size_t>(count));
        auto high=resampler->processSamplesUp(host);std::array<float*,8> channels;
        for(size_t ch=0;ch<channels.size();++ch)channels[ch]=high.getChannelPointer(ch);
        render<true>(channels.data(),count*factor,midi,highGains.data());
        // Gains are a control signal, not FIR audio. Preserve the internally
        // clocked ramp and align its host samples with the delayed layer audio.
        for(int n=0;n<count;++n)
        {
            hostGains[static_cast<size_t>(n)]=gainDelay[gainPosition];
            gainDelay[gainPosition]=highGains[static_cast<size_t>(n*factor)];
            gainPosition=(gainPosition+1)%gainDelay.size();
        }
        if(delayHighSample)for(size_t ch=0;ch<channels.size();++ch)for(size_t n=0;n<high.getNumSamples();++n)
            std::swap(lastHigh[ch],channels[ch][n]);
        resampler->processSamplesDown(host);
    }
};
SynthEngine::SynthEngine() : state(std::make_unique<State>()) {}
SynthEngine::~SynthEngine() = default;
bool SynthEngine::prepare(double sampleRate, int maximumBlockSize)
{ return prepare(sampleRate,maximumBlockSize,RenderQuality::realtime); }
bool SynthEngine::prepareInternal(double sampleRate, int maximumBlockSize, int midiScale)
{
    const auto* tables = &preparedTables();
    state->modernTables = &preparedModernTables();
    state->rate = sampleRate; state->maximumBlock = maximumBlockSize; state->tables = tables;
    state->rampSamples = static_cast<int>(std::ceil(sampleRate * 0.005));
    state->midiScale = midiScale;
    for (auto& v : state->voices) for (auto& l : v.layers) { l.amp.setSampleRate(sampleRate); l.filter.setSampleRate(sampleRate); }
    reset();
    return true;
}
bool SynthEngine::prepare(double sampleRate, int maximumBlockSize, RenderQuality quality) try
{
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000
        || maximumBlockSize < 1 || maximumBlockSize > 65536
        || (quality != RenderQuality::realtime && quality != RenderQuality::higher)) return false;
    auto candidate = std::make_unique<State>(); candidate->patch=state->patch;
    const int factor = quality == RenderQuality::higher ? 2 : 1;
    if (factor == 2)
    {
        candidate->resampler=std::make_unique<juce::dsp::Oversampling<float>>(8);
        candidate->resampler->clearOversamplingStages();
        candidate->resampler->addOversamplingStage(juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,.08f,-100.f,.08f,-100.f);
        candidate->resampler->initProcessing(static_cast<size_t>(maximumBlockSize));
        // We generate directly into the high-rate buffer: no up-filter signal
        // delay. Identical symmetric FIRs make the down-only delay half the
        // combined JUCE latency. The measured impulse gate pins this contract.
        const auto delay = candidate->resampler->getLatencyInSamples() * .5f;
        if (delay != std::floor(delay) && delay-std::floor(delay)!=.5f) return false;
        candidate->delayHighSample=delay!=std::floor(delay);
        candidate->latency=static_cast<int>(std::ceil(delay));
        candidate->work.setSize(8,maximumBlockSize);
        candidate->highGains.resize(static_cast<size_t>(maximumBlockSize*factor));
        candidate->hostGains.resize(static_cast<size_t>(maximumBlockSize));
        candidate->gainDelay.resize(static_cast<size_t>(candidate->latency),candidate->patch.masterLevel);
    }
    // All fallible allocation is complete before replacing a working engine.
    const auto* tables = &preparedTables();
    candidate->modernTables = &preparedModernTables();
    candidate->rate=sampleRate*factor; candidate->maximumBlock=maximumBlockSize;
    candidate->tables=tables; candidate->factor=candidate->midiScale=factor; candidate->quality=quality;
    candidate->rampSamples=static_cast<int>(std::ceil(candidate->rate*.005));
    for(auto& v:candidate->voices)for(auto& l:v.layers){l.amp.setSampleRate(candidate->rate);l.filter.setSampleRate(candidate->rate);}
    state.swap(candidate); reset(); return true;
}
catch(const std::bad_alloc&) { return false; }
RenderQuality SynthEngine::effectiveQuality() const noexcept { return state->quality; }
int SynthEngine::latencySamples() const noexcept { return state->latency; }
bool validateSynthPatch(const SynthPatch& patch) noexcept { return valid(patch); }
bool SynthEngine::setPatch(const SynthPatch& patch) noexcept
{
    if (!validateSynthPatch(patch)) return false;
    state->patch = patch;
    state->applyPatch(state->rate == 0);
    return true;
}
const SynthPatch& SynthEngine::patch() const noexcept { return state->patch; }
void SynthEngine::render(juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, std::optional<double> tempoBpm) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    out.clear();
    if (state->rate == 0 || out.getNumChannels() != 2 || out.getNumSamples() > state->maximumBlock || out.getNumSamples() == 0) return;
    state->timing(tempoBpm);
    std::array<float*, 2> outputs {out.getWritePointer(0), out.getWritePointer(1)};
    if (!state->resampler) state->render<false>(outputs.data(), out.getNumSamples(), midi);
    else
    {
        const auto count=out.getNumSamples();state->renderHigher(count,midi);
        for(int ch=0;ch<2;++ch)for(int n=0;n<count;++n)
        {
            double sum=0;for(int layer=0;layer<4;++layer)sum+=state->work.getSample(layer*2+ch,n);
            out.setSample(ch,n,static_cast<float>(sum*state->hostGains[static_cast<size_t>(n)]));
        }
    }
}
void SynthEngine::renderLayers(const std::array<juce::AudioBuffer<float>*, 4>& layers,
                               std::span<float> masterGains, const juce::MidiBuffer& midi, std::optional<double> tempoBpm) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    const auto clear = [&] {
        for (auto* layer : layers) if (layer != nullptr) layer->clear();
        std::fill(masterGains.begin(), masterGains.end(), 0.0f);
    };
    std::array<float*, 9> outputs;
    if (!state->layerOutputs(layers, masterGains, outputs)) { clear(); return; }
    const int count = layers[0]->getNumSamples();
    state->timing(tempoBpm);
    if (!state->resampler) state->render<true>(outputs.data(), count, midi, masterGains.data());
    else
    {
        state->renderHigher(count,midi);
        for(size_t ch=0;ch<8;++ch)std::copy_n(state->work.getReadPointer(static_cast<int>(ch)),count,outputs[ch]);
        std::copy_n(state->hostGains.data(),count,masterGains.data());
    }
}
bool SynthEngine::beginParallelLayers(const std::array<juce::AudioBuffer<float>*, 4>& layers,
                                      std::span<float> gains, const juce::MidiBuffer& midi,
                                      std::optional<double> tempoBpm,bool allowModulated) noexcept
{
    for(const auto event:midi)if(event.samplePosition!=0)return false;
    return beginParallelLayerSegment(layers,gains,midi.cbegin(),midi.cend(),tempoBpm,allowModulated);
}
bool SynthEngine::beginParallelLayerSegment(const std::array<juce::AudioBuffer<float>*, 4>& layers,
                                      std::span<float> gains,juce::MidiBufferIterator first,juce::MidiBufferIterator last,
                                      std::optional<double> tempoBpm,bool allowModulated,unsigned excluded) noexcept
{
    // The caller's views define this segment; the borrowed group starts there.
    std::array<float*, 9> outputs;
    if (state->resampler) return false;
    for(auto it=first;it!=last;++it){const auto event=*it;
        if(event.samplePosition<0||!allowModulated||event.numBytes!=3||event.data[1]>127||event.data[2]>127
            ||((event.data[0]&0xf0)!=0x80&&(event.data[0]&0xf0)!=0x90))return false;
    }
    const bool settled=first==last&&state->settledLayers(excluded);
    if(!settled&&(!allowModulated||!state->modernParallelLayers(excluded)))return false;
    if(!state->layerOutputs(layers,gains,outputs))return false;
    state->excludedMelodicLayers=excluded&15u;
    state->timing(tempoBpm);
    for(auto it=first;it!=last;++it)state->event(*it);
    std::copy_n(outputs.begin(), 8, state->parallelOutputs.begin());
    state->parallelModulated=!settled;
    state->parallelRetireMask=0;
    if(settled)state->prepareSettledLayers(layers[0]->getNumSamples(), gains.data(), state->parallelFrames);
    else{state->prepareParallelVoices(layers[0]->getNumSamples());std::fill(gains.begin(),gains.end(),static_cast<float>(state->master.current));}
    return true;
}
void SynthEngine::renderLayerSegment(const std::array<juce::AudioBuffer<float>*,4>& layers,std::span<float> gains,
    juce::MidiBufferIterator first,juce::MidiBufferIterator last,int origin,std::optional<double> tempo,unsigned excluded) noexcept
{
    std::array<float*,9> outputs;if(state->resampler||!state->layerOutputs(layers,gains,outputs))return;
    state->excludedMelodicLayers=excluded&15u;state->timing(tempo);
    const int count=layers[0]->getNumSamples();
    if(first==last&&state->settledLayers(excluded)){state->renderSettledLayers(outputs.data(),count,gains.data());return;}
    for(int n=0;n<count;++n){while(first!=last&&std::int64_t((*first).samplePosition)*state->midiScale<=origin+n){const auto event=*first++;if(event.samplePosition>=0)state->event(event);}state->frame<true>(outputs.data(),n,gains.data());}
}
void SynthEngine::retireSourceLayer(std::size_t bank) noexcept
{
    if(bank>=4)return;for(auto& voice:state->voices)voice.layers[bank].reset();state->clocks[bank]={};state->freshSourceOnsetRequired|=1u<<bank;
}
#ifndef NDEBUG
SynthEngine::SourceTestSnapshot SynthEngine::sourceTestSnapshot() const noexcept {
    SourceTestSnapshot out;out.phase=state->phaseSeed;out.unison=state->unisonSeed;out.fresh=state->freshSourceOnsetRequired;out.excluded=state->excludedMelodicLayers;
    for(std::size_t bank=0;bank<4;++bank)if(state->modern(bank))out.modern|=1u<<bank;
    for(std::size_t n=0;n<16;++n){const auto& v=state->voices[n];auto& snapshot=out.voices[n];snapshot.active=v.active;snapshot.held=v.held;snapshot.note=v.note;snapshot.channel=v.channel;
        for(std::size_t bank=0;bank<4;++bank)if(v.active&&state->modern(bank)&&v.layers[bank].modernAmp.active())snapshot.continuing|=1u<<bank;
    }return out;
}
#endif
void SynthEngine::renderParallelLayers(size_t firstLayer, size_t lastLayer, int offset, int count) noexcept
{
    if(state->parallelModulated)state->renderModulatedRange(state->parallelOutputs.data(),firstLayer,lastLayer,offset,count);
    else state->renderSettledRange(state->parallelOutputs.data(), state->parallelFrames, firstLayer, lastLayer, offset, count);
}
void SynthEngine::endParallelLayers() noexcept {
    for(size_t voice=0;voice<state->voices.size();++voice)if(state->parallelRetireMask&(1u<<voice))state->voices[voice].active=state->voices[voice].held=false;
    state->parallelOutputs.fill(nullptr);
}
bool SynthEngine::tempoUnavailable() const noexcept { return state->missingTempo; }
void SynthEngine::reset() noexcept
{
    if(state->resampler)state->resampler->reset();
    state->lastHigh={};
    std::fill(state->gainDelay.begin(),state->gainDelay.end(),state->patch.masterLevel);state->gainPosition=0;
    for (auto& v : state->voices) v.kill();
    state->seedVoices();
    state->age = 0;
    state->keys = {};
    state->freshSourceOnsetRequired=0;
    state->clocks = {}; state->tempo.reset(); state->missingTempo = false;
    state->applyPatch(true);
}
std::size_t SynthEngine::activeNoteCount() const noexcept
{ return static_cast<size_t>(std::count_if(state->voices.begin(), state->voices.end(), [](const Voice& v) { return v.active; })); }
}

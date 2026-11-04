#include "ReverbAnalysis.h"
#include "EraReverbParts.h"
#include <complex>
#include "EraReverb.h"
#include "EraReverbVoicings.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <utility>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace { thread_local bool countAllocations = false; thread_local std::size_t allocations = 0; }
void* operator new(std::size_t size)
{
    if (countAllocations) ++allocations;
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
namespace ra = reverbAnalysis;
int checks = 0, failures = 0;
void check(bool good, const char* expression, int line)
{
    ++checks;
    if (!good) { ++failures; std::cout << "FAIL " << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
bool expect(bool good, const char* what, double value, int line)
{
    check(good, what, line);
    std::cout << "  " << what << ": " << value << (good ? "" : "  <- outside") << '\n';
    return good;
}
#define EXPECT(condition, what, value) expect(bool(condition), what, value, __LINE__)
constexpr double rate48 = 48000;

std::uint32_t next(std::uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
double gaussian(std::uint32_t& s)
{
    const double u1 = (double(next(s)) + 1) / 4294967297.0, u2 = double(next(s)) / 4294967296.0;
    return std::sqrt(-2 * std::log(u1)) * std::cos(2 * std::numbers::pi * u2);
}

void analysisMeasuresKnownSignals()
{
    std::uint32_t seed = 12345;
    const auto n = std::size_t(4 * rate48);
    ra::Stereo decaying { ra::Signal(n), ra::Signal(n) };
    for (std::size_t i = 0; i < n; ++i) {
        const double gain = std::pow(10.0, -3 * (double(i) / rate48) / 2);
        decaying.left[i] = gain * gaussian(seed); decaying.right[i] = gain * gaussian(seed);
    }
    const auto broad = ra::rt60(decaying, rate48), mid = ra::rt60(ra::octave(decaying, rate48, 1000), rate48);
    EXPECT(broad && std::abs(*broad / 2 - 1) < .03, "rt60 of a 2 s noise decay", broad.value_or(-1));
    EXPECT(mid && std::abs(*mid / 2 - 1) < .05, "1 kHz octave rt60 of a 2 s noise decay", mid.value_or(-1));

    ra::Stereo noise { ra::Signal(std::size_t(rate48)), ra::Signal(std::size_t(rate48)) };
    for (std::size_t i = 0; i < noise.left.size(); ++i) { noise.left[i] = gaussian(seed); noise.right[i] = gaussian(seed); }
    EXPECT(ra::iacc({ noise.left, noise.left }, rate48) > .99, "iacc of identical channels", ra::iacc({ noise.left, noise.left }, rate48));
    EXPECT(ra::iacc(noise, rate48) < .05, "iacc of independent noise", ra::iacc(noise, rate48));
    const auto dense = ra::nedDiffuseMs(noise, rate48);
    EXPECT(dense && *dense <= 15, "noise is diffuse at once (ms)", dense.value_or(-1));
    ra::Stereo sparse { ra::Signal(std::size_t(rate48)), ra::Signal(std::size_t(rate48)) };
    for (std::size_t i = 0; i < sparse.left.size(); i += 480) sparse.left[i] = sparse.right[i] = 1;
    CHECK(!ra::nedDiffuseMs(sparse, rate48));
    ra::Stereo residue { ra::Signal(std::size_t(rate48)), ra::Signal(std::size_t(rate48)) };
    const double pole = std::exp(-2 * std::numbers::pi * 10 / rate48);
    for (std::size_t i = 0; i < residue.left.size(); ++i) {
        const double x = i % 1920 == 0 ? 1 : 0, previous = i % 1920 == 1 ? 1 : 0;
        residue.left[i] = residue.right[i] = (i > 0 ? pole * residue.left[i - 1] : 0) + x - previous;
    }
    CHECK(!ra::nedDiffuseMs(residue, rate48));

    const auto tone = ra::sine(rate48, 1000, 4.3, .25);
    EXPECT(ra::modWidth(tone, rate48, 2.3, 4.3) < 1, "smear of a pure tone (Hz)", ra::modWidth(tone, rate48, 2.3, 4.3));
    ra::Stereo fm { ra::Signal(std::size_t(4.3 * rate48)), {} };
    for (std::size_t i = 0; i < fm.left.size(); ++i) {
        const double t = double(i) / rate48;
        fm.left[i] = std::sin(2 * std::numbers::pi * 1000 * t + 10 * std::sin(2 * std::numbers::pi * t));
    }
    fm.right = ra::Signal(fm.left.size());
    const double width = ra::modWidth(fm, rate48, 2.3, 4.3);
    EXPECT(std::abs(width - 10 / std::numbers::sqrt2) < .7, "smear of a 10 Hz FM tone (Hz)", width);

    ra::Stereo harmonic { ra::Signal(std::size_t(3 * rate48)), {} };
    for (std::size_t i = 0; i < harmonic.left.size(); ++i) {
        const double t = double(i) / rate48;
        harmonic.left[i] = .5 * std::sin(2 * std::numbers::pi * 220 * t) + .0005 * std::sin(2 * std::numbers::pi * 660 * t);
    }
    harmonic.right = harmonic.left;
    const auto h = ra::harmonics(harmonic, rate48);
    EXPECT(std::abs(h.h3 + 60) < 1, "third harmonic of a -60 dB test tone", h.h3);
    CHECK(h.h2 < -100);
    EXPECT(std::abs(ra::toneDb(ra::sine(rate48, 3000, 2, .25).left, rate48, 3000) + 12.04) < .1, "tone level of a 0.25 sine",
        ra::toneDb(ra::sine(rate48, 3000, 2, .25).left, rate48, 3000));
    CHECK(ra::bandwidth20(ra::welch(noise.left, rate48, 8192)) >= 23000);
}

double magnitudeDb(double b0, double b1, double b2, double a1, double a2, double hz, double rate)
{
    const auto z = std::polar(1.0, -2 * std::numbers::pi * hz / rate);
    return 20 * std::log10(std::abs((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z)));
}
double onePoleDb(const airack::era::OnePole& f, double hz, double rate) { return magnitudeDb(f.b0, f.b1, 0, f.a1, 0, hz, rate); }
double biquadDb(const airack::era::Biquad& f, double hz, double rate) { return magnitudeDb(f.b0, f.b1, f.b2, f.a1, f.a2, hz, rate); }

void partsBehave()
{
    using namespace airack::era;
    OnePole lp;
    lp.lowpass(1000, rate48);
    EXPECT(std::abs(onePoleDb(lp, 1000, rate48) + 3.01) < .1, "1 kHz low-pass at its corner (dB)", onePoleDb(lp, 1000, rate48));
    EXPECT(std::abs(onePoleDb(lp, 20000, rate48) + 26) < 3, "1 kHz low-pass at 20 kHz (dB)", onePoleDb(lp, 20000, rate48));
    for (const double rate : { 44100.0, 48000.0, 96000.0 }) {
        OnePole top, hp, floor;
        top.lowpass(20000, rate); hp.highpass(1500, rate); floor.highpass(10, rate);
        EXPECT(std::abs(onePoleDb(top, 20000, rate) + 3.01) < 1, "20 kHz low-pass at its corner (dB)", onePoleDb(top, 20000, rate));
        EXPECT(std::abs(onePoleDb(hp, 1500, rate) + 3.01) < .1, "1500 Hz high-pass at its corner (dB)", onePoleDb(hp, 1500, rate));
        EXPECT(std::abs(onePoleDb(floor, 10, rate) + 3.01) < .1, "10 Hz high-pass at its corner (dB)", onePoleDb(floor, 10, rate));
    }

    DelayLine d;
    d.allocate(64);
    for (int i = 0; i < 40; ++i) d.write(i);
    CHECK(d.read(1) == 39 && d.read(10) == 30);
    CHECK(std::abs(d.readLinear(10.5) - 29.5) < 1e-12);
    CHECK(d.readCubic(10) == d.read(10));
    CHECK(std::abs(d.readCubic(10.25) - 29.75) < 1e-9);

    for (const double rate : { 44100.0, 48000.0 }) {
        const auto through = [&](double hz) {
            HalfbandDown down;
            double in = 0, out = 0;
            Frame y {};
            for (int i = 0; i < 48000; ++i) {
                const double x = std::sin(2 * std::numbers::pi * hz * i / rate);
                if (down.push({ x, x }, y) && i > 2000) out += y[0] * y[0];
                if (i > 2000) in += x * x;
            }
            return 10 * std::log10(2 * out / in);
        };
        EXPECT(std::abs(through(3000)) < .1, "decimator at 3 kHz (dB)", through(3000));
        EXPECT(through(15000) < -90, "decimator at 15 kHz (dB)", through(15000));
        EXPECT(through(18000) < -90, "decimator at 18 kHz (dB)", through(18000));
    }
    HalfbandUp up;
    ra::Signal high;
    Frame first {}, second {};
    for (int i = 0; i < 24000; ++i) {
        const double x = std::sin(2 * std::numbers::pi * 3000 * i / 24000.0);
        up.push({ x, x }, first, second);
        high.push_back(first[0]); high.push_back(second[0]);
    }
    const ra::Signal settled(high.begin() + 4000, high.end());
    EXPECT(ra::toneDb(settled, 48000, 3000) > -.2, "interpolated 3 kHz (dB)", ra::toneDb(settled, 48000, 3000));
    EXPECT(ra::toneDb(settled, 48000, 21000) < -90, "interpolation image at 21 kHz (dB)", ra::toneDb(settled, 48000, 21000));
    for (const int factor : { 2, 4, 8 }) {
        RateChanger rates;
        rates.setFactor(factor);
        const double rate = 24000.0 * factor;
        ra::Signal out;
        std::array<Frame, airack::era::RateChanger::maximumFactor> expanded {};
        int read = 0;
        for (int i = 0; i < int(rate); ++i) {
            const double x = std::sin(2 * std::numbers::pi * 1000 * i / rate);
            Frame internal {};
            if (rates.push({ x, x }, internal)) { rates.expand(internal, expanded); read = 0; }
            out.push_back(expanded[std::size_t(read++)][0]);
        }
        const ra::Signal tail(out.begin() + std::ptrdiff_t(out.size() / 2), out.end());
        EXPECT(std::abs(ra::toneDb(tail, rate, 1000)) < .2, "1 kHz through the rate changer (dB)", ra::toneDb(tail, rate, 1000));
    }

    const auto level = [](const auto& curve, double amplitude, int harmonic) {
        std::complex<double> one, at;
        for (int i = 0; i < 12000; ++i) {
            const double phase = 2 * std::numbers::pi * 55 * i / 12000.0, y = curve(amplitude * std::sin(phase));
            one += y * std::polar(1.0, -phase);
            at += y * std::polar(1.0, -harmonic * phase);
        }
        return 20 * std::log10(std::max(std::abs(at), 1e-30) / std::abs(one));
    };
    const double drive = gritDrive(.5), quiet = std::pow(10.0, -36 / 20.0);
    const auto odd = [&](double x) { return gritOdd(x, drive); };
    const auto tape = [&](double x) { return gritTape(x, drive); };
    CHECK(gritOdd(.3, 0) == .3 && gritTape(-.7, 0) == -.7);
    const double oddLoud = level(odd, .5, 3), tapeLoud = level(tape, .5, 3);
    EXPECT(oddLoud > -70 && oddLoud < -40, "odd curve H3 at -6 dBFS (dB)", oddLoud);
    EXPECT(level(odd, quiet, 3) <= oddLoud - 20, "odd curve H3 at -36 dBFS (dB)", level(odd, quiet, 3));
    EXPECT(level(odd, .5, 2) <= oddLoud - 40, "odd curve H2 at -6 dBFS (dB)", level(odd, .5, 2));
    EXPECT(tapeLoud > -70 && tapeLoud < -40, "tape curve H3 at -6 dBFS (dB)", tapeLoud);
    EXPECT(level(tape, quiet, 3) <= tapeLoud - 20, "tape curve H3 at -36 dBFS (dB)", level(tape, quiet, 3));
    EXPECT(level(tape, .5, 2) >= tapeLoud - 35, "tape curve H2 at -6 dBFS (dB)", level(tape, .5, 2));

    Biquad low, top;
    low.lowShelf(300, 4, rate48);
    top.highShelf(3000, .25, rate48);
    EXPECT(std::abs(biquadDb(low, 30, rate48) - 12.04) < .5, "low shelf x4 at 30 Hz (dB)", biquadDb(low, 30, rate48));
    EXPECT(std::abs(biquadDb(low, 3000, rate48)) < .5, "low shelf x4 at 3 kHz (dB)", biquadDb(low, 3000, rate48));
    EXPECT(std::abs(biquadDb(top, 300, rate48)) < .5, "high shelf x0.25 at 300 Hz (dB)", biquadDb(top, 300, rate48));
    EXPECT(std::abs(biquadDb(top, 15000, rate48) + 12.04) < .5, "high shelf x0.25 at 15 kHz (dB)", biquadDb(top, 15000, rate48));

    Grain grain;
    grain.set(1.0 / 32768, 4800, 7);
    CHECK(grain.process(0) == 0);
    double squares = 0;
    for (int i = 0; i < 48000; ++i) { const double x = .01 * std::sin(i * .05), e = grain.process(x) - x; squares += e * e; }
    const double rms = std::sqrt(squares / 48000) * 32768;
    EXPECT(rms > .4 && rms < .6, "grain error in steps (rms)", rms);
    for (int i = 0; i < 4801; ++i) grain.process(0);
    CHECK(grain.process(0) == 0 && grain.process(1e-6) == 0);

    Ramp ramp;
    ramp.snap(0);
    ramp.to(1, 3);
    ramp.advance();
    ramp.advance();
    CHECK(ramp.value != 1);
    ramp.advance();
    CHECK(ramp.value == 1 && !ramp.advance());
}

using airack::EraAge;
using airack::EraReverb;
using airack::EraReverbSettings;
using airack::EraVoicing;
constexpr std::array tankVoicings { EraVoicing::hall, EraVoicing::space, EraVoicing::tape, EraVoicing::plate, EraVoicing::room };

// The spec's neutral setting for behaviour checks, in Clear.
EraReverbSettings neutral(EraVoicing voicing = EraVoicing::hall)
{
    EraReverbSettings s;
    s.voicing = voicing; s.age = EraAge::clear; s.predelayMs = 0; s.size = 1; s.attack = .5f; s.diffusion = 1;
    s.wobble = 0; s.grit = 0; s.bassMultiplier = 1; s.dampingHz = 20000; s.lowCutHz = 10; s.highCutHz = 20000; s.mix = 1;
    return s;
}
ra::Stereo render(const EraReverbSettings& s, const ra::Stereo& input, double seconds, double rate = rate48)
{
    EraReverb reverb;
    const bool ready = reverb.prepare(rate) && reverb.setSettings(s);
    CHECK(ready);
    reverb.reset();
    const auto n = std::size_t(seconds * rate);
    ra::Stereo out { ra::Signal(n), ra::Signal(n) };
    for (std::size_t i = 0; i < n; ++i) {
        const EraReverb::Frame x { i < input.left.size() ? input.left[i] : 0, i < input.right.size() ? input.right[i] : 0 };
        const auto y = reverb.process(x);
        out.left[i] = y[0]; out.right[i] = y[1];
    }
    return out;
}
double rtAt(const ra::Stereo& y, double hz) { return ra::rt60(ra::octave(y, rate48, hz), rate48).value_or(0); }
bool silent(const ra::Stereo& y)
{
    for (std::size_t i = 0; i < y.left.size(); ++i) if (y.left[i] != 0 || y.right[i] != 0) return false;
    return true;
}

void settingsAndTail()
{
    EraReverbSettings s;
    CHECK(airack::validateEraReverb(s));
    const auto invalid = [](auto edit) { EraReverbSettings v; edit(v); return !airack::validateEraReverb(v); };
    CHECK(invalid([](auto& v) { v.decaySeconds = .19f; }));
    CHECK(invalid([](auto& v) { v.decaySeconds = 70.1f; }));
    CHECK(invalid([](auto& v) { v.predelayMs = 501; }));
    CHECK(invalid([](auto& v) { v.size = 1.01f; }));
    CHECK(invalid([](auto& v) { v.wobbleRateHz = .09f; }));
    CHECK(invalid([](auto& v) { v.bassMultiplier = 4.1f; }));
    CHECK(invalid([](auto& v) { v.dampingHz = 99; }));
    CHECK(invalid([](auto& v) { v.lowCutHz = 1501; }));
    CHECK(invalid([](auto& v) { v.highCutHz = 20001; }));
    CHECK(invalid([](auto& v) { v.mix = std::nanf(""); }));
    CHECK(invalid([](auto& v) { v.voicing = EraVoicing(6); }));
    CHECK(invalid([](auto& v) { v.age = EraAge(3); }));
    const double threshold = 16.0127351353, cuts = threshold / (2 * std::numbers::pi) * (1 / 10.0 + 1 / 8000.0);
    const double hall = .02 + cuts + .01 + .05775 + .1 + threshold / std::log(1000.0) * 4 * 1.5;
    EXPECT(std::abs(airack::eraReverbTailSeconds(s) - hall) < 1e-9, "default tail (s)", airack::eraReverbTailSeconds(s));
    s.voicing = EraVoicing::gate;
    EXPECT(std::abs(airack::eraReverbTailSeconds(s) - (.02 + cuts + .01 + 1.21 + 1.62 * .4 + .03)) < 1e-9, "gate tail (s)", airack::eraReverbTailSeconds(s));
    s.decaySeconds = 0;
    CHECK(std::isnan(airack::eraReverbTailSeconds(s)));
}

void silenceStaysSilent()
{
    for (const auto voicing : tankVoicings) CHECK(silent(render(neutral(voicing), {}, 1)));
}

void mixIsEqualPower()
{
    auto s = neutral();
    s.predelayMs = 10;
    const auto full = render(s, ra::impulse(), 2);
    double fullWet = 0;
    for (const double v : full.left) fullWet += v * v;
    for (const float m : { 0.f, .25f, .5f, .75f }) {
        s.mix = m;
        const auto y = render(s, ra::impulse(), 2);
        double wet = 0;
        for (std::size_t i = 1; i < y.left.size(); ++i) wet += y.left[i] * y.left[i];
        EXPECT(std::abs(y.left[0] - std::cos(std::numbers::pi / 2 * m)) < .01, "dry gain", y.left[0]);
        EXPECT(std::abs(std::sqrt(wet / fullWet) - std::sin(std::numbers::pi / 2 * m)) < .01, "wet gain", std::sqrt(wet / fullWet));
    }
}

void predelayIsExact()
{
    auto s = neutral();
    const auto base = ra::onset(render(s, ra::impulse(), 1));
    for (const float ms : { 10.f, 50.f, 250.f, 500.f }) {
        s.predelayMs = ms;
        const auto shift = ra::onset(render(s, ra::impulse(), 1.6)) - base;
        EXPECT(shift == std::size_t(std::lround(ms * 48)), "pre-delay shift (samples)", double(shift));
    }
}

// Long enough for the decay to fall about 50 dB, which leaves the -5 to -25 dB fit unbiased.
double decayRender(double decaySeconds) { return 1 + .8 * decaySeconds; }
void decayFollowsTheSetting()
{
    for (const auto voicing : tankVoicings)
        for (const double d : { .5, 1.0, 2.0, 4.0, 8.0, 16.0 }) {
            auto s = neutral(voicing);
            s.decaySeconds = float(d);
            const double t = rtAt(render(s, ra::impulse(), decayRender(d)), 1000);
            EXPECT(std::abs(t / d - 1) <= (d < 2 ? .25 : .10), "1 kHz rt60 / decay", t / d);
        }
    auto s = neutral();
    s.decaySeconds = 32;
    const double t = rtAt(render(s, ra::impulse(), decayRender(32)), 1000);
    EXPECT(std::abs(t / 32 - 1) <= .15, "1 kHz rt60 / decay at 32 s", t / 32);
}

void sizeChangesBuildNotDecay()
{
    std::vector<double> times, diffuse;
    for (const float size : { 0.f, .5f, 1.f }) {
        auto s = neutral();
        s.size = size; s.decaySeconds = 2;
        const auto y = render(s, ra::impulse(), 6);
        times.push_back(rtAt(y, 1000));
        diffuse.push_back(ra::nedDiffuseMs(y, rate48).value_or(600));
    }
    const auto [low, high] = std::minmax_element(times.begin(), times.end());
    EXPECT(*low > 0 && *high / *low <= 1.10, "rt60 spread across Size", *high / *low);
    EXPECT(diffuse[2] >= 3 * diffuse[0], "diffuse time Size 1 / Size 0", diffuse[2] / diffuse[0]);
}

void diffusionDensifiesTheStart()
{
    for (const auto voicing : { EraVoicing::hall, EraVoicing::plate }) {
        auto s = neutral(voicing);
        s.decaySeconds = 2;
        const double dense = ra::nedDiffuseMs(render(s, ra::impulse(), 3), rate48).value_or(600);
        s.diffusion = 0;
        const double sparse = ra::nedDiffuseMs(render(s, ra::impulse(), 3), rate48).value_or(600);
        EXPECT(sparse >= 3 * dense, "diffuse time Diffusion 0 / Diffusion 1", sparse / dense);
    }
}

void dampingAndBassShapeTheDecay()
{
    auto s = neutral();
    s.decaySeconds = 4;
    const auto open = render(s, ra::impulse(), 9);
    EXPECT(std::abs(rtAt(open, 8000) / rtAt(open, 1000) - 1) <= .15, "8 kHz / 1 kHz rt60, Damping 20 kHz", rtAt(open, 8000) / rtAt(open, 1000));
    s.dampingHz = 3000;
    const auto dark = render(s, ra::impulse(), 9);
    const double ratio = rtAt(dark, 8000) / rtAt(dark, 500);
    EXPECT(ratio >= .3 && ratio <= .5, "8 kHz / 500 Hz rt60, Damping 3 kHz", ratio);
    constexpr std::array<std::array<double, 2>, 4> cases {{ { .25, .3 }, { .5, .2 }, { 2, .2 }, { 4, .3 } }};
    for (const auto& [m, tolerance] : cases) {
        auto b = neutral();
        b.decaySeconds = 4; b.bassMultiplier = float(m);
        const auto y = render(b, ra::impulse(), decayRender(4 * std::max(1.0, m)));
        const double low = rtAt(y, 63) / rtAt(y, 1000);
        EXPECT(std::abs(low / m - 1) <= tolerance, "63 Hz / 1 kHz rt60 over Bass", low / m);
    }
}

void outputFiltersAreStatic()
{
    auto s = neutral();
    s.decaySeconds = 4;
    const auto spectra = [](const ra::Stereo& y) {
        return std::pair { ra::welch(ra::slice(ra::sum(y), rate48, .03, .33), rate48, 4096), ra::welch(ra::slice(ra::sum(y), rate48, 1.5, 2.5), rate48, 4096) };
    };
    const auto reference = spectra(render(s, ra::impulse(), 3));
    const double binHz = reference.first.hz[1];
    airack::era::OnePole open, cut, floor, raised;
    open.lowpass(20000, rate48); cut.lowpass(4000, rate48); floor.highpass(10, rate48); raised.highpass(300, rate48);
    s.highCutHz = 4000;
    const auto high = spectra(render(s, ra::impulse(), 3));
    for (const double hz : { 4000.0, 8000.0, 16000.0 }) {
        const auto k = std::size_t(std::lround(hz / binHz));
        const double f = double(k) * binHz;
        const double early = high.first.db[k] - reference.first.db[k], late = high.second.db[k] - reference.second.db[k];
        EXPECT(std::abs(early - late) < 1, "High Cut early minus late (dB)", early - late);
        EXPECT(std::abs(late - (onePoleDb(cut, f, rate48) - onePoleDb(open, f, rate48))) < 1, "High Cut response (dB)", late);
    }
    s.highCutHz = 20000; s.lowCutHz = 300;
    const auto low = spectra(render(s, ra::impulse(), 3));
    for (const double hz : { 100.0, 300.0 }) {
        const auto k = std::size_t(std::lround(hz / binHz));
        const double f = double(k) * binHz;
        const double early = low.first.db[k] - reference.first.db[k], late = low.second.db[k] - reference.second.db[k];
        EXPECT(std::abs(early - late) < 1, "Low Cut early minus late (dB)", early - late);
        EXPECT(std::abs(late - (onePoleDb(raised, f, rate48) - onePoleDb(floor, f, rate48))) < 1, "Low Cut response (dB)", late);
    }
}

void checkWide(const EraReverbSettings& s)
{
    const double correlation = ra::iacc(render(s, ra::impulse(), 3), rate48);
    EXPECT(correlation <= .1, "tail iacc", correlation);
    const auto y = render(s, ra::leftImpulse(), 3);
    double l = 0, r = 0;
    for (std::size_t i = 0; i < y.left.size(); ++i) { l += y.left[i] * y.left[i]; r += y.right[i] * y.right[i]; }
    EXPECT(std::abs(10 * std::log10(l / r)) <= 10, "left-only input, left over right (dB)", 10 * std::log10(l / r));
}
void tailIsWide()
{
    for (const auto voicing : tankVoicings) {
        auto s = neutral(voicing);
        s.decaySeconds = 2;
        checkWide(s);
    }
}

double smear(EraVoicing voicing, float wobble, float rateHz)
{
    auto s = neutral(voicing);
    s.decaySeconds = 4; s.wobble = wobble; s.wobbleRateHz = rateHz;
    return ra::modWidth(render(s, ra::sine(rate48, 1000, 2, .25), 5), rate48, 2.3, 4.3);
}
// At Wobble 0 nothing moves, so the same input arriving later gives the same output later: the largest
// difference, over the output's peak.
double laterMinusEarlier(EraVoicing voicing)
{
    constexpr std::size_t shift = 4801;
    const auto tone = ra::sine(rate48, 1000, .5, .25);
    ra::Stereo later { ra::Signal(shift), ra::Signal(shift) };
    later.left.insert(later.left.end(), tone.left.begin(), tone.left.end());
    later.right.insert(later.right.end(), tone.right.begin(), tone.right.end());
    const auto first = render(neutral(voicing), tone, 2), second = render(neutral(voicing), later, 2.2);
    double peak = 0, error = 0;
    for (std::size_t i = 0; i < first.left.size(); ++i) {
        peak = std::max({ peak, std::abs(first.left[i]), std::abs(first.right[i]) });
        error = std::max({ error, std::abs(second.left[i + shift] - first.left[i]), std::abs(second.right[i + shift] - first.right[i]) });
    }
    return error / peak;
}
void wobbleZeroIsTimeInvariant()
{
    for (const auto voicing : tankVoicings) {
        const double drift = laterMinusEarlier(voicing);
        EXPECT(drift <= 1e-9, "Wobble 0, later output minus earlier, over the peak", drift);
    }
}
void wobbleWidensTheTone()
{
    double last = -1;
    for (const float depth : { .25f, .5f, .75f, 1.f }) {
        const double w = smear(EraVoicing::hall, depth, 1);
        EXPECT(w > last, "Hall smear rising with depth (Hz)", w);
        last = w;
    }
    last = -1;
    for (const float rateHz : { .3f, 1.f, 3.f, 10.f }) {
        const double w = smear(EraVoicing::hall, .5f, rateHz);
        EXPECT(w > last, "Hall smear rising with rate (Hz)", w);
        last = w;
    }
    const double full = smear(EraVoicing::hall, 1, 1), fast = smear(EraVoicing::hall, .5f, 10);
    EXPECT(full >= 6 && full <= 15, "Hall smear at Wobble 1, 1 Hz", full);
    EXPECT(fast >= 15, "Hall smear at Wobble 0.5, 10 Hz", fast);
    const double hall = smear(EraVoicing::hall, .38f, 2.53f), space = smear(EraVoicing::space, .38f, 2.53f), tape = smear(EraVoicing::tape, .38f, 2.53f);
    EXPECT(hall >= 6 && hall <= 14, "Hall smear at the defaults", hall);
    EXPECT(space <= 6, "Space smear at the defaults", space);
    EXPECT(tape >= hall, "Tape smear at the defaults", tape);
}

double peakSampleOf(const ra::Stereo& y, double rate)
{
    const auto env = ra::envelopeDb(y, rate, 10);
    const auto stop = std::min(env.size(), std::size_t(1.5 * rate));
    return double(std::max_element(env.begin(), env.begin() + std::ptrdiff_t(stop)) - env.begin());
}
double peakAfterOnsetMs(const ra::Stereo& y) { return (peakSampleOf(y, rate48) - double(ra::onset(y))) / rate48 * 1000; }
// The record's swells start at its impulse; a quiet swell's own onset can sit 100 ms late, so they count from the input.
double peakAfterInputMs(const ra::Stereo& y, double rate = rate48) { return peakSampleOf(y, rate) / rate * 1000; }
void attackSwellsAndBalances()
{
    for (const auto voicing : { EraVoicing::hall, EraVoicing::space, EraVoicing::tape, EraVoicing::plate }) {
        auto s = neutral(voicing);
        s.decaySeconds = 2; s.attack = 1;
        const double slow = peakAfterInputMs(render(s, ra::impulse(), 3));
        s.attack = 0;
        const double fast = peakAfterOnsetMs(render(s, ra::impulse(), 3));
        EXPECT(slow >= 200 && slow <= 300, "swell peak at Attack 1 (ms)", slow);
        for (int k = 1; k <= 100; ++k)
            CHECK(airack::era::swellSeconds(voicing, k / 100.0) > airack::era::swellSeconds(voicing, (k - 1) / 100.0));
        EXPECT(fast <= 20, "peak at Attack 0 (ms)", fast);
    }
    // Higher quality and 96 kHz hosts run Era at 96 kHz.
    for (const double rate : { rate48, 96000.0 }) {
        auto s = neutral(EraVoicing::room);
        s.decaySeconds = 2;
        const auto windows = [rate](const ra::Stereo& y) {
            const auto env = ra::envelopeDb(y, rate, 10);
            const auto peakIn = [&](double from, double to) {
                double best = -400;
                for (auto i = std::size_t(from * rate); i < std::min(env.size(), std::size_t(to * rate)); ++i) best = std::max(best, env[i]);
                return best;
            };
            return std::pair { peakIn(0, .07), peakIn(.1, 1) };
        };
        s.attack = 0;
        const auto [early0, late0] = windows(render(s, ra::impulse(), 3, rate));
        EXPECT(late0 <= early0 - 20, "Room Attack 0: tail below reflections (dB)", late0 - early0);
        s.attack = 1;
        const auto y = render(s, ra::impulse(), 3, rate);
        const auto [early1, late1] = windows(y);
        EXPECT(early1 <= late1 - 20, "Room Attack 1: reflections below tail (dB)", early1 - late1);
        const double peak = peakAfterInputMs(y, rate);
        EXPECT(peak >= 120 && peak <= 200, "Room tail peak at Attack 1 (ms)", peak);
    }
}

// The record's floor between harmonics for a 220 Hz sine at -36 dBFS, relative to the fundamental the voicing
// passes on average (the impulse response's power gain), so one resonance at 220 Hz cannot move it.
double floorRelDb(const EraReverbSettings& s)
{
    const auto in = ra::sine(rate48, 220, 3, std::pow(10.0, -36 / 20.0));
    const auto response = ra::sum(render(s, ra::impulse(), 3));
    double gain = 0;
    for (const double v : response) gain += v * v / 4;
    return ra::floorDb(ra::gritSpectrum(render(s, in, 3), rate48), 220) - ra::peakDb(ra::gritSpectrum(in, rate48), 220) - 10 * std::log10(gain);
}
// 1024-sample segments average a dense tail's bin-to-bin dips away; with the record's 8192 (about eight averages)
// a single bin dipping 20 dB below its neighbours set the reading.
double tailBandwidth(const EraReverbSettings& s, double rate)
{
    const auto y = render(s, ra::impulse(), 2, rate);
    const double start = double(ra::onset(y)) / rate + .2;
    return ra::bandwidth20(ra::welch(ra::slice(ra::sum(y), rate, start, start + .8), rate, 1024));
}
void agesColourTheTail()
{
    for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
        auto s = neutral();
        s.decaySeconds = 2; s.age = EraAge::dark;
        const double dark = tailBandwidth(s, rate);
        EXPECT(dark >= 10000 && dark <= 13000, "Dark -20 dB bandwidth (Hz)", dark);
        if (rate > 96000) continue;
        s.age = EraAge::warm;
        const double warm = tailBandwidth(s, rate);
        EXPECT(warm >= 15000 && warm <= 19000, "Warm -20 dB bandwidth (Hz)", warm);
    }
    auto s = neutral();
    s.decaySeconds = 2;
    const double clear = tailBandwidth(s, rate48);
    EXPECT(clear >= 19000, "Clear -20 dB bandwidth (Hz)", clear);
    s.age = EraAge::dark; s.decaySeconds = 4;
    const double darkDecay = rtAt(render(s, ra::impulse(), decayRender(4)), 1000);
    EXPECT(std::abs(darkDecay / 4 - 1) <= .10, "Dark 1 kHz rt60 / decay", darkDecay / 4);

    s.decaySeconds = 1;
    const auto toneOut = [&](double hz) { return ra::slice(ra::sum(render(s, ra::sine(rate48, hz, 3, .25), 3)), rate48, 1, 2.8); };
    const double reference = ra::toneDb(toneOut(3000), rate48, 3000);
    for (const double hz : { 15000.0, 18000.0 }) {
        const auto out = toneOut(hz);
        const double image = 24000 - hz, inBand = ra::toneDb(toneOut(image), rate48, image);
        EXPECT(ra::toneDb(out, rate48, hz) <= reference - 40, "Dark out-of-band tone below 3 kHz (dB)", ra::toneDb(out, rate48, hz) - reference);
        EXPECT(ra::toneDb(out, rate48, image) <= inBand - 90, "Dark alias image below the same tone in band (dB)", ra::toneDb(out, rate48, image) - inBand);
    }
    for (const auto voicing : tankVoicings) {
        auto dark = neutral(voicing);
        dark.age = EraAge::dark; dark.decaySeconds = 1;
        const double grain = floorRelDb(dark);
        EXPECT(grain >= -110 && grain <= -88, "Dark floor at -36 dBFS (dB)", grain);
    }
    s.age = EraAge::clear;
    const double clearFloor = floorRelDb(s);
    EXPECT(clearFloor <= -140, "Clear floor at -36 dBFS (dB)", clearFloor);
    for (const auto age : { EraAge::dark, EraAge::warm, EraAge::clear })
        for (const auto voicing : tankVoicings) {
            auto quiet = neutral(voicing);
            quiet.age = age;
            CHECK(silent(render(quiet, {}, 1)));
        }
}

// The reverb's own gain at hz (output over input Welch peak, Grit 0), which varies by several dB between
// neighbouring frequencies; harmonic levels are read against it.
double transferDb(EraReverbSettings s, double hz)
{
    s.grit = 0;
    const auto in = ra::sine(rate48, hz, 3, .5);
    return ra::peakDb(ra::gritSpectrum(render(s, in, 3), rate48), hz) - ra::peakDb(ra::gritSpectrum(in, rate48), hz);
}
void gritSaturatesLoudInput()
{
    for (const auto voicing : { EraVoicing::hall, EraVoicing::tape }) {
        auto s = neutral(voicing);
        s.decaySeconds = 1; s.grit = .5f;
        const double t1 = transferDb(s, 220), t2 = transferDb(s, 440) - t1, t3 = transferDb(s, 660) - t1;
        const auto loud = ra::harmonics(render(s, ra::sine(rate48, 220, 3, std::pow(10.0, -6 / 20.0)), 3), rate48);
        const auto quiet = ra::harmonics(render(s, ra::sine(rate48, 220, 3, std::pow(10.0, -36 / 20.0)), 3), rate48);
        const double h2 = loud.h2 - t2, h3 = loud.h3 - t3, quietH3 = quiet.h3 - t3;
        EXPECT(h3 >= -70 && h3 <= -40, "H3 at Grit 0.5, -6 dBFS (dB)", h3);
        EXPECT(quietH3 <= h3 - 20, "H3 at Grit 0.5, -36 dBFS (dB)", quietH3);
        if (voicing == EraVoicing::tape) EXPECT(h2 >= h3 - 35, "Tape H2 (dB)", h2);
        else EXPECT(h2 <= h3 - 40, "Hall H2 (dB)", h2);
    }
}

double bandEnergyDb(const ra::Stereo& y, double hz)
{
    const auto band = ra::octave(y, rate48, hz);
    double e = 0;
    for (std::size_t i = 0; i < band.left.size(); ++i) e += band.left[i] * band.left[i] + band.right[i] * band.right[i];
    return 10 * std::log10(e);
}
void gateShapesABurst()
{
    auto s = neutral(EraVoicing::gate);
    s.decaySeconds = .2f;
    double previous = 0;
    for (const float size : { 0.f, .25f, .5f, .75f, 1.f }) {
        s.size = size;
        const auto env = ra::envelopeDb(render(s, ra::impulse(), 2.5), rate48, 10);
        const double peak = *std::max_element(env.begin(), env.end());
        std::size_t first = env.size(), last = 0;
        for (std::size_t i = 0; i < env.size(); ++i) if (env[i] >= peak - 20) { first = std::min(first, i); last = i; }
        const double span = double(last - first) / rate48 * 1000;
        EXPECT(span > previous, "gate span within 20 dB (ms)", span);
        previous = span;
        const double expected = size == 0 ? 100 : size == .5f ? 380 : size == 1 ? 1210 : 0;
        if (expected > 0) EXPECT(std::abs(span / expected - 1) <= .15, "gate span over the record's", span / expected);
    }
    s.size = .5f; s.decaySeconds = 2;
    const double length = airack::era::gateLengthSeconds(.5) * rate48;
    const auto shape = [&](float attack) {
        s.attack = attack;
        const auto y = render(s, ra::impulse(), 2);
        return std::pair { ra::envelopeDb(y, rate48, 30), ra::onset(y) };
    };
    const auto at = [&](const ra::Signal& env, std::size_t start, double fraction) { return env[start + std::size_t(fraction * length)]; };
    const auto [decaying, start0] = shape(0);
    EXPECT(at(decaying, start0, .9) <= at(decaying, start0, .1) - 10, "Attack 0: end below start (dB)", at(decaying, start0, .9) - at(decaying, start0, .1));
    const auto [flat, start5] = shape(.5f);
    double low = 400, high = -400;
    for (double f = .1; f <= .9; f += .01) { low = std::min(low, at(flat, start5, f)); high = std::max(high, at(flat, start5, f)); }
    EXPECT(high - low <= 3, "Attack 0.5: flatness over the middle (dB)", high - low);
    const auto [rising, start1] = shape(1);
    double best = -400, where = 0;
    for (double f = 0; f <= 1.1; f += .01) if (at(rising, start1, f) > best) { best = at(rising, start1, f); where = f; }
    EXPECT(best >= at(rising, start1, .1) + 10, "Attack 1: rise to the peak (dB)", best - at(rising, start1, .1));
    EXPECT(where >= .75, "Attack 1: peak position in the burst", where);

    // The closing's taps can be 50 ms apart, so its -3 to -40 dB fall is read from its decay rate: the slope of
    // the Schroeder integral between -30 and -50 dB of the total, which lies wholly after the burst.
    s.attack = .5f;
    for (const double decay : { .5, 2.0, 10.0 }) {
        s.decaySeconds = float(decay);
        const double close = airack::era::gateCloseSeconds(decay);
        const auto perSixty = ra::rt60(render(s, ra::impulse(), 4.5), rate48, -30, -50);
        const double fall = perSixty.value_or(0) * 37 / 60;
        EXPECT(std::abs(fall / close - 1) <= .2, "gate close -3 to -40 dB / expected", fall / close);
    }

    auto t = neutral(EraVoicing::gate);
    t.size = .5f; t.bassMultiplier = 4;
    const double heavy = bandEnergyDb(render(t, ra::impulse(), 2), 63);
    t.bassMultiplier = .25f;
    const double light = bandEnergyDb(render(t, ra::impulse(), 2), 63);
    EXPECT(heavy - light >= 12, "Gate Bass 4 over 0.25 at 63 Hz (dB)", heavy - light);
    t.bassMultiplier = 1; t.dampingHz = 1000;
    const double dull = bandEnergyDb(render(t, ra::impulse(), 2), 8000);
    t.dampingHz = 20000;
    const double open = bandEnergyDb(render(t, ra::impulse(), 2), 8000);
    EXPECT(open - dull >= 6, "Gate Damping 20 kHz over 1 kHz at 8 kHz (dB)", open - dull);
    const double drift = laterMinusEarlier(EraVoicing::gate);
    EXPECT(drift <= 1e-9, "Gate Wobble 0, later output minus earlier, over the peak", drift);
    const auto g = neutral(EraVoicing::gate);
    checkWide(g);
    auto dark = g;
    dark.age = EraAge::dark; dark.decaySeconds = 1;
    const double grain = floorRelDb(dark);
    EXPECT(grain >= -110 && grain <= -88, "Gate Dark floor at -36 dBFS (dB)", grain);
    for (const auto age : { EraAge::dark, EraAge::warm, EraAge::clear }) {
        auto quiet = g;
        quiet.age = age;
        CHECK(silent(render(quiet, {}, 1)));
    }
}

std::vector<EraReverb::Frame> noiseFrames(std::size_t count, std::uint32_t seed)
{
    std::vector<EraReverb::Frame> frames(count);
    for (auto& f : frames) f = { airack::era::uniform(seed) - .5, airack::era::uniform(seed) - .5 };
    return frames;
}
double largestStep(const std::vector<EraReverb::Frame>& y, std::size_t from, std::size_t to)
{
    double step = 0;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i)
        step = std::max({ step, std::abs(y[i][0] - y[i - 1][0]), std::abs(y[i][1] - y[i - 1][1]) });
    return step;
}
void runtimeGuarantees()
{
    EraReverb refusing;
    CHECK(!refusing.prepare(7999) && !refusing.prepare(768001) && refusing.prepare(8000) && refusing.prepare(768000));

    EraReverbSettings changed;
    changed.voicing = EraVoicing::space; changed.age = EraAge::warm; changed.decaySeconds = 6; changed.size = .3f;
    changed.wobble = .8f; changed.grit = .4f; changed.predelayMs = 120;
    const auto run = [&](std::size_t block) {
        EraReverb reverb;
        reverb.prepare(rate48);
        reverb.setSettings(EraReverbSettings {});
        reverb.reset();
        auto frames = noiseFrames(std::size_t(3 * rate48), 9);
        for (std::size_t i = std::size_t(rate48 / 2); i < frames.size(); ++i) frames[i] = {};
        for (std::size_t i = 0; i < frames.size(); i += block) {
            if (i == std::size_t(rate48)) reverb.setSettings(changed);
            reverb.processBlock(std::span(frames).subspan(i, block));
        }
        return frames;
    };
    const auto single = run(1);
    for (const std::size_t block : { 64, 480, 4800 }) CHECK(run(block) == single);

    EraReverb reverb;
    reverb.prepare(96000);
    reverb.setSettings(EraReverbSettings {});
    reverb.reset();
    auto frames = noiseFrames(9600, 3);
    allocations = 0;
    countAllocations = true;
    for (std::size_t v = 0; v < airack::eraVoicingCount; ++v)
        for (std::size_t a = 0; a < airack::eraAgeCount; ++a) {
            EraReverbSettings s;
            s.voicing = EraVoicing(v); s.age = EraAge(a); s.wobble = .5f; s.grit = .3f;
            reverb.setSettings(s);
            for (int repeat = 0; repeat < 10; ++repeat) reverb.processBlock(frames);
        }
    countAllocations = false;
    EXPECT(allocations == 0, "allocations while processing and switching", double(allocations));

    const auto switched = [&](const EraReverbSettings& to) {
        EraReverb r;
        auto s = neutral();
        s.decaySeconds = 3;
        r.prepare(rate48);
        r.setSettings(s);
        r.reset();
        auto y = noiseFrames(std::size_t(3 * rate48), 11);
        for (auto& f : y) { f[0] *= .25; f[1] *= .25; }
        for (std::size_t i = 0; i < y.size(); ++i) {
            if (i == std::size_t(rate48)) r.setSettings(to);
            y[i] = r.process(y[i]);
        }
        return y;
    };
    auto plate = neutral(EraVoicing::plate);
    plate.age = EraAge::dark; plate.decaySeconds = 3;
    const auto y = switched(plate);
    const double before = largestStep(y, std::size_t(.5 * rate48), std::size_t(rate48));
    const double during = largestStep(y, std::size_t(rate48), std::size_t(1.2 * rate48));
    EXPECT(during <= before, "largest step during a voicing and age switch over before", during / before);
    EraReverb late;
    auto hall = neutral();
    hall.decaySeconds = 3;
    late.prepare(rate48);
    late.setSettings(hall);
    late.reset();
    ra::Stereo afterSwitch { ra::Signal(std::size_t(3 * rate48)), ra::Signal(std::size_t(3 * rate48)) };
    for (std::size_t i = 0; i < afterSwitch.left.size(); ++i) {
        if (i == std::size_t(rate48)) late.setSettings(plate);
        const double x = i == std::size_t(1.5 * rate48) ? 1.0 : 0.0;
        const auto out = late.process({ x, x });
        afterSwitch.left[i] = out[0]; afterSwitch.right[i] = out[1];
    }
    const double start = 1.5 + double(ra::onset({ ra::slice(afterSwitch.left, rate48, 1.5, 3), ra::slice(afterSwitch.right, rate48, 1.5, 3) })) / rate48 + .2;
    const double bandwidth = ra::bandwidth20(ra::welch(ra::slice(ra::sum(afterSwitch), rate48, start, start + .8), rate48, 1024));
    EXPECT(bandwidth <= 13000, "bandwidth after switching to Dark (Hz)", bandwidth);

    auto moving = neutral();
    moving.decaySeconds = 3;
    EraReverb glide;
    glide.prepare(rate48);
    glide.setSettings(moving);
    glide.reset();
    auto g = noiseFrames(std::size_t(2 * rate48), 13);
    for (auto& f : g) { f[0] *= .25; f[1] *= .25; }
    for (std::size_t i = 0; i < g.size(); ++i) {
        if (i == std::size_t(rate48)) { moving.size = .2f; moving.predelayMs = 300; glide.setSettings(moving); }
        g[i] = glide.process(g[i]);
    }
    EXPECT(largestStep(g, std::size_t(rate48), std::size_t(1.1 * rate48)) <= 1.5 * largestStep(g, std::size_t(.5 * rate48), std::size_t(rate48)),
        "largest step while Size and Pre-delay glide over before", largestStep(g, std::size_t(rate48), std::size_t(1.1 * rate48)));

    EraReverb guarded;
    guarded.prepare(rate48);
    guarded.setSettings(neutral());
    guarded.reset();
    auto n = noiseFrames(std::size_t(rate48), 17);
    n[1000] = { std::nan(""), std::numeric_limits<double>::infinity() };
    bool finite = true;
    for (std::size_t i = 0; i < n.size(); ++i) {
        const auto out = guarded.process(n[i]);
        if (i != 1000) finite = finite && std::isfinite(out[0]) && std::isfinite(out[1]);
    }
    CHECK(finite);

    for (const auto age : { EraAge::dark, EraAge::warm, EraAge::clear }) {
        auto quick = neutral();
        quick.age = age; quick.decaySeconds = .5f;
        const auto tail = render(quick, ra::impulse(), 31);
        CHECK(silent({ ra::slice(tail.left, rate48, 30, 31), ra::slice(tail.right, rate48, 30, 31) }));
    }

    for (const double rate : { 44100.0, 96000.0 }) {
        auto s = neutral();
        s.decaySeconds = 4;
        const auto yr = render(s, ra::impulse(), 5.8, rate);
        const auto t = ra::rt60(ra::octave(yr, rate, 1000), rate);
        EXPECT(t && std::abs(*t / 4 - 1) <= .10, "1 kHz rt60 / decay at another rate", t.value_or(-1) / 4);
    }

    for (const auto voicing : { EraVoicing::hall, EraVoicing::gate }) {
        auto s = neutral(voicing);
        s.decaySeconds = 1; s.predelayMs = 20;
        const double tail = airack::eraReverbTailSeconds(s);
        const auto yt = render(s, ra::impulse(), tail + 1);
        double peak = 0, after = 0;
        for (std::size_t i = 0; i < yt.left.size(); ++i) {
            const double v = std::max(std::abs(yt.left[i]), std::abs(yt.right[i]));
            peak = std::max(peak, v);
            if (double(i) >= tail * rate48) after = std::max(after, v);
        }
        EXPECT(after <= peak * 1e-6 / 9, "largest sample after the reported tail, relative", after / peak);
    }
}

// The engine prepares effects at every internal rate from 8 to 768 kHz; behaviour is measured at 44.1, 48 and 96 kHz,
// and elsewhere the unit must stay silent on silence, finite and inside its reported tail.
void ratesBeyondTheContract()
{
    for (const double rate : { 8000.0, 22050.0, 32000.0, 384000.0, 768000.0 })
        for (const auto age : { EraAge::dark, EraAge::warm, EraAge::clear })
            for (const auto voicing : { EraVoicing::hall, EraVoicing::tape, EraVoicing::gate }) {
                auto s = neutral(voicing);
                s.age = age; s.decaySeconds = .4f; s.wobble = .38f;
                EraReverb quiet;
                CHECK(quiet.prepare(rate) && quiet.setSettings(s));
                quiet.reset();
                std::vector<EraReverb::Frame> zeros(std::size_t(rate * .05));
                quiet.processBlock(zeros);
                bool exact = true;
                for (const auto& f : zeros) exact = exact && f[0] == 0 && f[1] == 0;
                CHECK(exact);
                const double tail = airack::eraReverbTailSeconds(s);
                const auto y = render(s, ra::impulse(), tail + .3, rate);
                double peak = 0, after = 0;
                bool finite = true;
                for (std::size_t i = 0; i < y.left.size(); ++i) {
                    finite = finite && std::isfinite(y.left[i]) && std::isfinite(y.right[i]);
                    const double v = std::max(std::abs(y.left[i]), std::abs(y.right[i]));
                    peak = std::max(peak, v);
                    if (double(i) >= tail * rate) after = std::max(after, v);
                }
                CHECK(finite && peak > 1e-4);
                EXPECT(after <= peak * 1e-6 / 9, "largest sample after the reported tail at another rate, relative", after / peak);
            }
    for (const double rate : { 384000.0, 768000.0 }) {
        auto s = neutral();
        s.decaySeconds = 2; s.age = EraAge::dark;
        const double dark = tailBandwidth(s, rate);
        EXPECT(dark >= 10000 && dark <= 13000, "Dark -20 dB bandwidth above 192 kHz (Hz)", dark);
    }
}

void profile()
{
    double sink = 0;
    for (const double rate : { 48000.0, 96000.0 })
        for (const auto voicing : { EraVoicing::hall, EraVoicing::space, EraVoicing::gate })
            for (const auto age : { EraAge::dark, EraAge::clear }) {
                EraReverb reverb;
                reverb.prepare(rate);
                EraReverbSettings s;
                s.voicing = voicing; s.age = age; s.grit = .15f;
                reverb.setSettings(s);
                reverb.reset();
                auto frames = noiseFrames(std::size_t(rate * 10), 5);
                const auto start = std::chrono::steady_clock::now();
                reverb.processBlock(frames);
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                for (const auto& f : frames) sink += f[0];
                const auto resetStart = std::chrono::steady_clock::now();
                reverb.reset();
                const double resetMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - resetStart).count();
                std::cout << "  " << rate << " Hz, voicing " << int(voicing) << ", age " << int(age) << ": " << seconds * 10
                          << "% of one core per slot, " << seconds * 80 << "% for eight slots; reset " << resetMs << " ms\n";
            }
    std::cout << "  (sink " << sink << ")\n";
}

void resetForgetsEverything()
{
    for (std::size_t v = 0; v < airack::eraVoicingCount; ++v) {
        auto s = neutral(EraVoicing(v));
        s.predelayMs = 40; s.wobble = .5f; s.grit = .3f; s.age = EraAge::dark;
        const auto respond = [](EraReverb& r) {
            std::vector<EraReverb::Frame> y(static_cast<std::size_t>(rate48));
            y[0] = { 1, 1 };
            r.processBlock(y);
            return y;
        };
        EraReverb fresh, used;
        for (auto* r : { &fresh, &used }) { r->prepare(rate48); r->setSettings(s); r->reset(); }
        for (const double seconds : { 3.0, .3 }) {
            auto noise = noiseFrames(std::size_t(seconds * rate48), 5);
            used.processBlock(noise);
            used.reset();
            CHECK(respond(used) == respond(fresh));
            fresh.reset();
        }
    }
}
}

int main(int argc, char** argv)
{
    const std::string_view section = argc > 1 ? argv[1] : "";
    const auto run = [&](std::string_view name) { return section.empty() || section == name; };
    constexpr std::string_view sections[] { "--analysis", "--parts", "--core", "--wobble", "--attack", "--ages", "--grit", "--gate", "--runtime", "--rates", "--profile" };
    if (!section.empty() && std::find(std::begin(sections), std::end(sections), section) == std::end(sections)) {
        std::cerr << "Usage: RackSynthEraReverbTests [--analysis|--parts|--core|--wobble|--attack|--ages|--grit|--gate|--runtime|--rates|--profile]\n";
        return 2;
    }
    if (run("--analysis")) analysisMeasuresKnownSignals();
    if (run("--parts")) partsBehave();
    if (run("--core")) {
        settingsAndTail(); silenceStaysSilent(); mixIsEqualPower(); predelayIsExact(); decayFollowsTheSetting();
        sizeChangesBuildNotDecay(); diffusionDensifiesTheStart(); dampingAndBassShapeTheDecay(); outputFiltersAreStatic(); tailIsWide();
    }
    if (run("--wobble")) { wobbleZeroIsTimeInvariant(); wobbleWidensTheTone(); }
    if (run("--attack")) attackSwellsAndBalances();
    if (run("--ages")) agesColourTheTail();
    if (run("--grit")) gritSaturatesLoudInput();
    if (run("--gate")) gateShapesABurst();
    if (run("--runtime")) { runtimeGuarantees(); resetForgetsEverything(); }
    if (run("--rates")) ratesBeyondTheContract();
    if (section == "--profile") profile();
    std::cout << "Era Reverb: " << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

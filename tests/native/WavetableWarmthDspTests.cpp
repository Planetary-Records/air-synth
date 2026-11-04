#include "SynthEngine.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
using namespace airack;
int checks = 0, failures = 0;
void check(bool good, const char* expression, int line)
{ ++checks; if (!good) { ++failures; if (failures < 85) std::cerr << "FAIL " << line << ' ' << expression << '\n'; } }
#define CHECK(x) check(bool(x), #x, __LINE__)
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking = false;
thread_local int allocations = 0, frees = 0;
int __cdecl allocationHook(int kind, void*, size_t, int, long, const unsigned char*, int)
{ if (tracking && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC)) ++allocations; if (tracking && kind == _HOOK_FREE) ++frees; return 1; }
#endif
using Events = std::vector<std::pair<double, juce::MidiMessage>>;
std::uint64_t fingerprint(const std::vector<float>& x)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (auto v : x) hash = (hash ^ std::bit_cast<std::uint32_t>(v)) * 1099511628211ull;
    return hash;
}
// Four revision-2 wavetable layers reaching every path the warmth keys will touch at their neutral values:
// each filter mode, noise, high resonance, filter envelope, and LFO and velocity routes in the wavetable bank.
SynthPatch neutralScene()
{
    SynthPatch p; p.masterLevel = .7f;
    for (std::size_t i = 0; i < 4; ++i) {
        auto& l = p.layers[i];
        l.enabled = true; l.engine = EngineKind::wavetable; l.algorithmRevision = modernSynthAlgorithmRevision;
        l.oscillators[0] = {Waveform::saw, 0, .3f, 0, 0, .6f, .5f};
        l.oscillators[1] = {Waveform::saw, 1, .5f, -12, 4, .25f, .5f};
        l.ampEnvelope = {.004f, .2f, .15f, .6f}; l.filterEnvelope = {.002f, .3f, .2f, .2f};
        l.cutoffHz = 1800; l.resonance = .2f; l.level = .35f; l.pan = -.6f + .4f * float(i);
    }
    auto& a = p.layers[0]; a.oscillators[0].position = .7f; a.oscillators[0].detune = 12;
    auto& b = p.layers[1]; b.filterMode = FilterMode::bandpass; b.oscillators[0].tableIndex = 1; b.cutoffHz = 900; b.filterEnvelopeOctaves = 3;
    b.lfos[0].rateHz = 3; b.modulation[0] = {true, ModSource::lfo1, ModTarget::cutoff, ModBank::wavetable, .1f};
    auto& c = p.layers[2]; c.filterMode = FilterMode::highpass; c.cutoffHz = 400; c.noiseLevel = .3f;
    c.modulation[0] = {true, ModSource::velocity, ModTarget::cutoff, ModBank::wavetable, .2f};
    auto& d = p.layers[3]; d.cutoffHz = 1200; d.resonance = .9f;
    d.lfos[1].rateHz = .7f; d.modulation[0] = {true, ModSource::lfo2, ModTarget::osc1Position, ModBank::wavetable, .3f};
    return p;
}
// One hash per rate, quality and output mode, in neutralFingerprint's order.
const std::vector<std::uint64_t> neutralHashes {
    0xf68171df797fc5d2ull, 0x455d73df904ec12bull, 0x8b83bf35a8252fa2ull, 0xd646d172e2f2b5a3ull,
    0x8403f81e363f64bdull, 0xf211ee0d7a5c8568ull, 0x926e173b952f4129ull, 0x6d226438c091686ull,
    0x4d5b7221315bc3edull, 0xf24c4cad2132c542ull, 0x343d74781d57b535ull, 0xac3c471dda8547cull};
// Sixteen overlapping notes, two retriggers of sounding notes, a seventeenth that steals, releases, then a reset
// and a fresh pair. Separated output interleaves each frame's master gain with the four layers' stereo samples.
std::vector<float> phrase(const SynthPatch& scene, double rate, RenderQuality quality, int partition, bool separated)
{
    SynthEngine engine; CHECK(engine.setPatch(scene) && engine.prepare(rate, 512, quality));
    std::array<juce::AudioBuffer<float>, 4> layers; for (auto& b : layers) b.setSize(2, 512);
    juce::AudioBuffer<float> mixed(2, 512); std::array<float, 512> gains {}; juce::MidiBuffer midi; std::vector<float> out;
    const auto frames = [&](double seconds) { return int(std::lround(seconds * rate)); };
    const auto append = [&](double seconds, const Events& events) {
        for (int offset = 0, total = frames(seconds); offset < total;) {
            const int count = std::min(partition, total - offset);
            midi.clear();
            for (const auto& [at, message] : events)
                if (const int frame = frames(at); frame >= offset && frame < offset + count) midi.addEvent(message, frame - offset);
            if (separated) {
                std::array<juce::AudioBuffer<float>, 4> views; std::array<juce::AudioBuffer<float>*, 4> pointers {};
                for (std::size_t l = 0; l < 4; ++l) { views[l].setDataToReferTo(layers[l].getArrayOfWritePointers(), 2, count); pointers[l] = &views[l]; }
                engine.renderLayers(pointers, std::span(gains).first(std::size_t(count)), midi);
                for (int n = 0; n < count; ++n) {
                    out.push_back(gains[std::size_t(n)]);
                    for (const auto& v : views) for (int ch = 0; ch < 2; ++ch) out.push_back(v.getSample(ch, n));
                }
            } else {
                juce::AudioBuffer<float> view(mixed.getArrayOfWritePointers(), 2, count);
                engine.render(view, midi);
                for (int n = 0; n < count; ++n) for (int ch = 0; ch < 2; ++ch) out.push_back(view.getSample(ch, n));
            }
            offset += count;
        }
    };
    Events notes;
    for (int n = 0; n < 16; ++n) notes.emplace_back(.004 * n, juce::MidiMessage::noteOn(1, 36 + 3 * n, .4f + .035f * float(n)));
    notes.emplace_back(.08, juce::MidiMessage::noteOn(1, 48, .7f)); notes.emplace_back(.1, juce::MidiMessage::noteOn(1, 60, .5f));
    notes.emplace_back(.11, juce::MidiMessage::noteOn(1, 86, .9f));
    for (int n = 0; n < 5; ++n) notes.emplace_back(.13 + .005 * n, juce::MidiMessage::noteOff(1, n < 4 ? 36 + 3 * n : 86));
    append(.18, notes);
    engine.reset();
    append(.06, {{0, juce::MidiMessage::noteOn(1, 50, .8f)}, {.01, juce::MidiMessage::noteOn(1, 57, .6f)}, {.04, juce::MidiMessage::noteOff(1, 50)}});
    return out;
}
void neutralFingerprint()
{
    std::cout << "SECTION wavetable_neutral_fingerprint\n";
    // Every partition must match the rate, quality and output mode's hash bit for bit.
    std::size_t index = 0;
    for (const double rate : {44100.0, 48000.0, 96000.0})
        for (const auto quality : {RenderQuality::realtime, RenderQuality::higher})
            for (const bool separated : {false, true}) {
                const auto samples = phrase(neutralScene(), rate, quality, 64, separated);
                const auto hash = fingerprint(samples);
                CHECK(std::any_of(samples.begin(), samples.end(), [](float v) { return std::abs(v) > 1e-3f; }));
                CHECK(std::all_of(samples.begin(), samples.end(), [](float v) { return std::isfinite(v); }));
                for (const int partition : {512, 73}) CHECK(fingerprint(phrase(neutralScene(), rate, quality, partition, separated)) == hash);
                std::cout << "WARMTH_NEUTRAL_FINGERPRINT rate=" << rate << " quality=" << int(quality) << " separated=" << separated
                          << " samples=" << samples.size() << " fnv=0x" << std::hex << hash << std::dec << "ull\n";
                CHECK(index < neutralHashes.size() && neutralHashes[index] == hash);
                ++index;
            }
}
constexpr double pi = 3.14159265358979323846, sampleRate = 48000;
struct Stereo { std::vector<float> left, right; };
using Timed = std::vector<std::pair<int, juce::MidiMessage>>;
Stereo render(SynthEngine& engine, int frames, const Timed& events = {})
{
    Stereo out; juce::AudioBuffer<float> buffer(2, 512); juce::MidiBuffer midi;
    for (int offset = 0; offset < frames; offset += 512) {
        const int count = std::min(512, frames - offset);
        midi.clear();
        for (const auto& [at, message] : events) if (at >= offset && at < offset + count) midi.addEvent(message, at - offset);
        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), 2, count);
        engine.render(view, midi);
        for (int n = 0; n < count; ++n) { out.left.push_back(view.getSample(0, n)); out.right.push_back(view.getSample(1, n)); }
    }
    return out;
}
juce::MidiMessage on(int note) { return juce::MidiMessage::noteOn(1, note, .8f); }
// One revision-2 wavetable layer: a sine (table 0, position 0) through Clean at 20 kHz with no resonance or drift.
SynthPatch unisonScene(int voices, float cents, float width = 1)
{
    SynthPatch p; p.masterLevel = .7f;
    auto& l = p.layers[0];
    l.engine = EngineKind::wavetable; l.algorithmRevision = modernSynthAlgorithmRevision;
    l.oscillators[0].level = .5f; l.oscillators[0].unison = voices; l.oscillators[0].unisonCents = cents;
    l.oscillators[1].level = 0;
    l.cutoffHz = 20000; l.resonance = 0; l.level = 1; l.unisonWidth = width; l.ampEnvelope = {.002f, .1f, .1f, 1};
    return p;
}
Stereo play(const SynthPatch& p, int note, int frames, const Timed& more = {})
{
    SynthEngine engine; CHECK(engine.setPatch(p) && engine.prepare(sampleRate, 512));
    Timed events {{0, on(note)}}; events.insert(events.end(), more.begin(), more.end());
    return render(engine, frames, events);
}
double voiceCents(int k, int n, double spread) { return n == 1 ? 0 : spread * (2.0 * k / (n - 1) - 1); }
// Width-1 position: an odd count's middle voice is centred; the rest alternate from the lowest on the left.
int voiceSide(int k, int n)
{
    if (n % 2 == 1 && k == n / 2) return 0;
    const int rank = n % 2 == 1 && k > n / 2 ? k - 1 : k;
    return rank % 2 == 0 ? -1 : 1;
}
// In-place radix-2 transform of a power-of-two length.
void fft(std::vector<double>& re, std::vector<double>& im)
{
    const std::size_t n = re.size();
    double* r = re.data(); double* i = im.data();
    for (std::size_t a = 1, b = 0; a < n; ++a) {
        std::size_t bit = n >> 1;
        for (; b & bit; bit >>= 1) b ^= bit;
        b ^= bit;
        if (a < b) { std::swap(r[a], r[b]); std::swap(i[a], i[b]); }
    }
    for (std::size_t size = 2; size <= n; size <<= 1)
        for (std::size_t k = 0, half = size / 2; k < half; ++k) {
            const double angle = -2 * pi * double(k) / double(size), wr = std::cos(angle), wi = std::sin(angle);
            for (std::size_t a = k; a < n; a += size) {
                const std::size_t b = a + half;
                const double xr = r[b] * wr - i[b] * wi, xi = r[b] * wi + i[b] * wr;
                r[b] = r[a] - xr; i[b] = i[a] - xi; r[a] += xr; i[a] += xi;
            }
        }
}
// Magnitude spectra of both sides through a Blackman-Harris window zero-padded to at least eight times its length.
struct Spectra { std::vector<double> left, right, sum; double binHz = 0; };
Spectra spectra(const Stereo& x, std::size_t first, std::size_t length)
{
    std::size_t n = 1;
    while (n < 8 * length) n <<= 1;
    std::vector<double> re(n), im(n);
    for (std::size_t k = 0; k < length; ++k) {
        const double t = 2 * pi * double(k) / double(length - 1);
        const double w = .35875 - .48829 * std::cos(t) + .14128 * std::cos(2 * t) - .01168 * std::cos(3 * t);
        re[k] = w * x.left[first + k]; im[k] = w * x.right[first + k];
    }
    fft(re, im);
    const double* r = re.data(); const double* i = im.data();
    // One complex transform carries both real sides: L = (Z(k) + conj Z(n-k)) / 2, R = (Z(k) - conj Z(n-k)) / 2j.
    Spectra s; s.binHz = sampleRate / double(n);
    s.left.resize(n / 2); s.right.resize(n / 2); s.sum.resize(n / 2);
    for (std::size_t k = 1; k < n / 2; ++k) {
        const double cr = r[n - k], ci = -i[n - k];
        const double lr = (r[k] + cr) / 2, li = (i[k] + ci) / 2, rr = (i[k] - ci) / 2, ri = -(r[k] - cr) / 2;
        s.left[k] = std::hypot(lr, li); s.right[k] = std::hypot(rr, ri); s.sum[k] = std::hypot(lr + rr, li + ri);
    }
    return s;
}
// The largest bin within reachHz of hz, refined by a parabola through its log magnitudes.
struct Peak { double hz = 0; std::size_t bin = 0; };
Peak peak(const std::vector<double>& m, double binHz, double hz, double reachHz = .5)
{
    const auto centre = std::size_t(std::lround(hz / binHz)), reach = std::size_t(std::ceil(reachHz / binHz));
    std::size_t best = centre - reach;
    for (auto k = centre - reach; k <= centre + reach; ++k) if (m[k] > m[best]) best = k;
    const double a = std::log(m[best - 1]), b = std::log(m[best]), c = std::log(m[best + 1]);
    return {(double(best) + .5 * (a - c) / (a - 2 * b + c)) * binHz, best};
}
// Every voice's partial sits at its law's frequency; returns the worst error in cents, or 1e9 when a voice is missing.
double partialError(const Spectra& s, int n, double spread, double hz, std::array<Peak, 7>& peaks)
{
    const double strongest = *std::max_element(s.sum.begin(), s.sum.end());
    double worst = 0;
    for (int k = 0; k < n; ++k) {
        const double expected = hz * std::exp2(voiceCents(k, n, spread) / 1200);
        peaks[std::size_t(k)] = peak(s.sum, s.binHz, expected);
        worst = std::max(worst, std::abs(1200 * std::log2(peaks[std::size_t(k)].hz / expected)));
    }
    for (int k = 0; k < n; ++k) if (!(s.sum[peaks[std::size_t(k)].bin] > .25 * strongest)) worst = 1e9;
    return worst;
}
double rms(const Stereo& x, std::size_t first, std::size_t count)
{
    double sum = 0;
    for (std::size_t n = first; n < first + count; ++n) sum += double(x.left[n]) * x.left[n] + double(x.right[n]) * x.right[n];
    return std::sqrt(sum / double(count));
}
double largestStep(const Stereo& x, std::size_t first, std::size_t last)
{
    double step = 0;
    for (std::size_t n = first + 1; n < last; ++n)
        step = std::max({step, std::abs(double(x.left[n]) - x.left[n - 1]), std::abs(double(x.right[n]) - x.right[n - 1])});
    return step;
}
// Each partial's phase at frame `at`, from a joint least-squares fit of known frequencies over [first, first + count).
std::vector<double> phases(const std::vector<float>& x, const std::vector<double>& increments, int at, int first, int count)
{
    const std::size_t m = 2 * increments.size();
    std::vector<double> g(m * m), b(m), basis(m);
    for (int n = first; n < first + count; ++n) {
        for (std::size_t k = 0; k < increments.size(); ++k) {
            const double angle = 2 * pi * increments[k] * double(n - at);
            basis[2 * k] = std::cos(angle); basis[2 * k + 1] = std::sin(angle);
        }
        for (std::size_t r = 0; r < m; ++r) {
            b[r] += basis[r] * x[std::size_t(n)];
            for (std::size_t c = 0; c < m; ++c) g[r * m + c] += basis[r] * basis[c];
        }
    }
    for (std::size_t c = 0; c < m; ++c) {
        std::size_t pivot = c;
        for (std::size_t r = c + 1; r < m; ++r) if (std::abs(g[r * m + c]) > std::abs(g[pivot * m + c])) pivot = r;
        for (std::size_t k = 0; k < m; ++k) std::swap(g[c * m + k], g[pivot * m + k]);
        std::swap(b[c], b[pivot]);
        for (std::size_t r = c + 1; r < m; ++r) {
            const double factor = g[r * m + c] / g[c * m + c];
            for (std::size_t k = c; k < m; ++k) g[r * m + k] -= factor * g[c * m + k];
            b[r] -= factor * b[c];
        }
    }
    for (std::size_t c = m; c-- > 0;) {
        for (std::size_t k = c + 1; k < m; ++k) b[c] -= g[c * m + k] * b[k];
        b[c] /= g[c * m + c];
    }
    std::vector<double> result;
    for (std::size_t k = 0; k < increments.size(); ++k) result.push_back(std::atan2(-b[2 * k + 1], b[2 * k]));
    return result;
}
void unison()
{
    std::cout << "SECTION wavetable_unison\n";
    constexpr int analysed = 192000, settle = 2400;
    for (const float spread : {15.f, 60.f})
        for (int n = 2; n <= 7; ++n) {
            const auto x = play(unisonScene(n, spread), 93, settle + analysed);
            const auto s = spectra(x, settle, analysed);
            std::array<Peak, 7> peaks;
            const double error = partialError(s, n, spread, 1760, peaks);
            CHECK(error < .1);
            double separation = 1e9, centre = 0;
            for (int k = 0; k < n; ++k) {
                const auto bin = peaks[std::size_t(k)].bin;
                const double l = s.left[bin], r = s.right[bin], side = voiceSide(k, n);
                if (side == 0) centre = std::abs(20 * std::log10(l / r));
                else separation = std::min(separation, 20 * std::log10(side < 0 ? l / r : r / l));
            }
            CHECK(separation >= 60); CHECK(centre < .01);
            std::cout << "UNISON_SPREAD voices=" << n << " cents=" << spread << " worst_cents=" << error
                      << " separation_db=" << separation << " centre_db=" << centre << '\n';
        }
    {
        const auto x = play(unisonScene(7, 60, 0), 93, 24000);
        CHECK(rms(x, 0, 24000) > 1e-3); CHECK(x.left == x.right);
    }
    auto saw = [](int n) { auto p = unisonScene(n, 25); p.layers[0].oscillators[0].position = 1; return p; };
    const double one = rms(play(saw(1), 69, settle + 96000), settle, 96000);
    for (int n = 2; n <= 7; ++n) {
        const double level = 20 * std::log10(rms(play(saw(n), 69, settle + 96000), settle, 96000) / one);
        CHECK(std::abs(level) <= 1.5);
        std::cout << "UNISON_LEVEL voices=" << n << " db=" << level << '\n';
    }
    {
        // The count is structural: the layer fades through silence before the new voices sound.
        auto p = saw(1); SynthEngine e; CHECK(e.setPatch(p) && e.prepare(sampleRate, 512));
        const double before = rms(render(e, 24000, {{0, on(69)}}), 12000, 12000);
        p.layers[0].oscillators[0].unison = 7; CHECK(e.setPatch(p));
        const auto fade = render(e, 512);
        CHECK(std::abs(fade.left[239]) < 1e-10 && std::abs(fade.right[239]) < 1e-10);
        CHECK(rms(fade, 480, 32) > 0);
        const double after = rms(render(e, 48000), 0, 48000);
        CHECK(after > .5 * before && after < 2 * before);
    }
    {
        // Spread and width ramp: sweeps across 20 blocks step no further than either end's steady signal.
        const auto steady = [&](float spread, float width) {
            return largestStep(play(unisonScene(7, spread, width), 69, settle + 144000), settle, settle + 144000);
        };
        const auto sweep = [&](float spread0, float spread1, float width0, float width1) {
            auto p = unisonScene(7, spread0, width0); SynthEngine e; CHECK(e.setPatch(p) && e.prepare(sampleRate, 512));
            auto x = render(e, 12000, {{0, on(69)}});
            for (int b = 1; b <= 20; ++b) {
                p.layers[0].oscillators[0].unisonCents = spread0 + (spread1 - spread0) * float(b) / 20;
                p.layers[0].unisonWidth = width0 + (width1 - width0) * float(b) / 20;
                CHECK(e.setPatch(p));
                const auto block = render(e, 512);
                x.left.insert(x.left.end(), block.left.begin(), block.left.end());
                x.right.insert(x.right.end(), block.right.begin(), block.right.end());
            }
            return largestStep(x, 11999, x.left.size());
        };
        const double spreadSweep = sweep(0, 100, 1, 1), spreadBound = std::max(steady(0, 1), steady(100, 1));
        const double widthSweep = sweep(60, 60, 0, 1), widthBound = std::max(steady(60, 0), steady(60, 1));
        CHECK(spreadSweep <= 1.05 * spreadBound); CHECK(widthSweep <= 1.05 * widthBound);
        std::cout << "UNISON_SWEEP spread=" << spreadSweep / spreadBound << " width=" << widthSweep / widthBound << '\n';
    }
    {
        // A retrigger keeps every voice's free-running phase.
        constexpr int at = 24000, fit = 2400;
        const auto x = play(unisonScene(7, 60), 93, at + 9600, {{at, on(93)}});
        CHECK(largestStep(x, at - fit, at + fit) <= 1.05 * largestStep(x, settle, at - fit));
        std::vector<float> sum(x.left.size());
        for (std::size_t n = 0; n < sum.size(); ++n) sum[n] = x.left[n] + x.right[n];
        std::vector<double> increments;
        for (int k = 0; k < 7; ++k) increments.push_back(1760 * std::exp2(voiceCents(k, 7, 60) / 1200) / sampleRate);
        const auto early = phases(sum, increments, at, at - fit, fit), late = phases(sum, increments, at, at, fit);
        double worst = 0;
        for (std::size_t k = 0; k < 7; ++k) worst = std::max(worst, std::abs(std::remainder(late[k] - early[k], 2 * pi)));
        CHECK(worst < .05);
        std::cout << "UNISON_RETRIGGER worst_rad=" << worst << '\n';
    }
    for (const auto [tune, detune] : {std::pair {.05f, .25f}, std::pair {-.1f, .5f}}) {
        // LFO 1 holds +1 (a square at 0.1 Hz) through the analysis, moving every voice by the same interval.
        auto p = unisonScene(7, 15); auto& l = p.layers[0];
        l.lfos[0] = {LfoShape::square, false, .1f, 1};
        l.modulation[0] = {true, ModSource::lfo1, ModTarget::osc1Tune, ModBank::wavetable, tune};
        l.modulation[1] = {true, ModSource::lfo1, ModTarget::osc1Detune, ModBank::wavetable, detune};
        const auto s = spectra(play(p, 93, settle + analysed), settle, analysed);
        std::array<Peak, 7> peaks;
        const double error = partialError(s, 7, 15, 1760 * std::exp2((24.0 * tune + 100.0 * detune * .01) / 12), peaks);
        CHECK(error < .1);
        std::cout << "UNISON_ROUTES tune=" << tune << " detune=" << detune << " worst_cents=" << error << '\n';
    }
    // One voice at any spread and width renders the neutral scenes bit for bit.
    auto scene = neutralScene();
    for (std::size_t i = 0; i < 4; ++i) {
        auto& l = scene.layers[i];
        l.oscillators[0].unisonCents = 37 + 11 * float(i); l.oscillators[1].unisonCents = 83; l.unisonWidth = .3f + .2f * float(i);
    }
    std::size_t index = 0;
    for (const double rate : {44100.0, 48000.0, 96000.0})
        for (const auto quality : {RenderQuality::realtime, RenderQuality::higher})
            for (const bool separated : {false, true}) CHECK(fingerprint(phrase(scene, rate, quality, 64, separated)) == neutralHashes[index++]);
}
// The first `layers` layers, each unisonScene's sine with one voice, at the given drift.
SynthPatch driftScene(float drift, int layers = 4)
{
    auto p = unisonScene(1, 0);
    for (std::size_t i = 0; i < std::size_t(layers); ++i) { p.layers[i] = p.layers[0]; p.layers[i].enabled = true; p.layers[i].drift = drift; }
    return p;
}
// Each layer's left side, rendered separated in 512-sample blocks.
std::array<std::vector<float>, 4> layerLefts(SynthEngine& engine, int frames, const Timed& events)
{
    std::array<juce::AudioBuffer<float>, 4> layers; for (auto& b : layers) b.setSize(2, 512);
    std::array<float, 512> gains {}; juce::MidiBuffer midi; std::array<std::vector<float>, 4> out;
    for (int offset = 0; offset < frames; offset += 512) {
        const int count = std::min(512, frames - offset);
        midi.clear();
        for (const auto& [at, message] : events) if (at >= offset && at < offset + count) midi.addEvent(message, at - offset);
        std::array<juce::AudioBuffer<float>, 4> views; std::array<juce::AudioBuffer<float>*, 4> pointers {};
        for (std::size_t l = 0; l < 4; ++l) { views[l].setDataToReferTo(layers[l].getArrayOfWritePointers(), 2, count); pointers[l] = &views[l]; }
        engine.renderLayers(pointers, std::span(gains).first(std::size_t(count)), midi);
        for (std::size_t l = 0; l < 4; ++l) for (int n = 0; n < count; ++n) out[l].push_back(views[l].getSample(0, n));
    }
    return out;
}
// Each 50 ms frame's pitch in cents against hz, from the span of its interpolated rising zero crossings.
std::vector<double> pitchTrack(const std::vector<float>& x, double hz)
{
    constexpr std::size_t frame = 2400;
    std::vector<double> cents;
    for (std::size_t first = 0; first + frame <= x.size(); first += frame) {
        double start = -1, end = 0; int periods = -1;
        for (std::size_t n = first + 1; n < first + frame; ++n)
            if (x[n - 1] < 0 && x[n] >= 0) {
                end = double(n - 1) + double(x[n - 1]) / (double(x[n - 1]) - double(x[n]));
                if (start < 0) start = end;
                ++periods;
            }
        cents.push_back(1200 * std::log2(periods * sampleRate / (end - start) / hz));
    }
    return cents;
}
// A held A3 in every layer for 57 s after a reset: the mean, over 4 layers by 6 consecutive 9.5 s windows, of each
// window's pitch standard deviation and peak to peak, in cents.
std::pair<double, double> wander(float drift)
{
    SynthEngine engine; CHECK(engine.setPatch(driftScene(drift)) && engine.prepare(sampleRate, 512));
    engine.reset();
    double deviation = 0, range = 0;
    for (const auto& x : layerLefts(engine, 57 * 48000, {{0, on(57)}})) {
        const auto cents = pitchTrack(x, 220);
        for (std::ptrdiff_t w = 0; w < 6; ++w) {
            const auto first = cents.begin() + 190 * w, last = first + 190;
            double mean = 0, square = 0;
            for (auto c = first; c != last; ++c) mean += *c / 190;
            for (auto c = first; c != last; ++c) square += (*c - mean) * (*c - mean) / 190;
            const auto [low, high] = std::minmax_element(first, last);
            deviation += std::sqrt(square) / 24; range += (*high - *low) / 24;
        }
    }
    return {deviation, range};
}
// The neutral scene with one edit on every layer renders the neutral fingerprints at every rate, quality and output.
void neutralWith(void (*edit)(LayerSettings&))
{
    auto scene = neutralScene();
    for (auto& l : scene.layers) edit(l);
    std::size_t index = 0;
    for (const double rate : {44100.0, 48000.0, 96000.0})
        for (const auto quality : {RenderQuality::realtime, RenderQuality::higher})
            for (const bool separated : {false, true}) CHECK(fingerprint(phrase(scene, rate, quality, 64, separated)) == neutralHashes[index++]);
}
void drift()
{
    std::cout << "SECTION wavetable_drift\n";
    neutralWith([](LayerSettings& l) { l.drift = 0; });
    const auto [deviation, range] = wander(1);
    CHECK(std::abs(deviation / 2.86 - 1) <= .2); CHECK(std::abs(range / 12.8 - 1) <= .3);
    const double half = wander(.5f).first / deviation;
    CHECK(std::abs(half / .25 - 1) <= .25);
    std::cout << "DRIFT_WANDER sd_cents=" << deviation << " peak_to_peak_cents=" << range << " half_ratio=" << half << '\n';
    {
        // Sixteen voice slots, one note each, hold their fixed offsets from the first sample after a reset.
        SynthEngine engine; CHECK(engine.setPatch(driftScene(1, 1)) && engine.prepare(sampleRate, 512));
        engine.reset();
        Timed notes;
        for (int k = 0; k < 16; ++k) notes.emplace_back(0, on(72 + 2 * k));
        const auto s = spectra(render(engine, 9600, notes), 0, 9600);
        double low = 1e9, high = -1e9;
        for (int k = 0; k < 16; ++k) {
            const double hz = 440 * std::exp2((72 + 2 * k - 69) / 12.0);
            const double cents = 1200 * std::log2(peak(s.sum, s.binHz, hz, hz * (std::exp2(27 / 1200.0) - 1)).hz / hz);
            CHECK(std::abs(cents) <= 26);
            low = std::min(low, cents); high = std::max(high, cents);
        }
        CHECK(high - low >= 30);
        std::cout << "DRIFT_OFFSETS low_cents=" << low << " high_cents=" << high << '\n';
    }
    {
        const Timed notes {{0, on(57)}, {4800, on(64)}, {9600, on(71)}};
        SynthEngine engine; CHECK(engine.setPatch(driftScene(1)) && engine.prepare(sampleRate, 512));
        const auto first = render(engine, 48000, notes);
        engine.reset();
        const auto second = render(engine, 48000, notes);
        SynthEngine fresh; CHECK(fresh.setPatch(driftScene(1)) && fresh.prepare(sampleRate, 512));
        const auto third = render(fresh, 48000, notes);
        CHECK(first.left == second.left && first.right == second.right && first.left == third.left && first.right == third.right);
    }
}
// A0's saw (table 0, position 1) through the given filter, at an osc level low enough for the ladders to stay linear.
SynthPatch filterScene(FilterType type, FilterMode mode, float cutoff, float resonance, float level = .01f)
{
    auto p = unisonScene(1, 0); auto& l = p.layers[0];
    l.oscillators[0].position = 1; l.oscillators[0].level = level;
    l.filterType = type; l.filterMode = mode; l.cutoffHz = cutoff; l.resonance = resonance;
    return p;
}
// Each harmonic's Blackman-Harris magnitude over 4 s of a held A0, up to 0.4 of the rate.
std::vector<double> harmonics(const SynthPatch& p)
{
    constexpr int settle = 12000, analysed = 192000;
    const auto s = spectra(play(p, 21, settle + analysed), settle, analysed);
    std::vector<double> m;
    for (int h = 1; 27.5 * h <= .4 * sampleRate; ++h) m.push_back(s.left[peak(s.left, s.binHz, 27.5 * h).bin]);
    return m;
}
// The engine's prewarped cutoff, opening past the audible band at the top of the range.
double warp(double cutoff)
{
    const double top = std::min(cutoff, 20000.0) / 20000, top2 = top * top;
    return std::tan(pi * std::clamp(cutoff * (1 + top2 * top2 * top2) / sampleRate, 1e-5, .49));
}
// Clean's low-pass response at hz: the bilinear state-variable filter 1 / (s^2 + damping s + 1).
double cleanResponse(double cutoff, double damping, double hz)
{
    const auto delay = std::polar(1.0, -2 * pi * hz / sampleRate), s = (1.0 - delay) / (warp(cutoff) * (1.0 + delay));
    return std::abs(1.0 / (s * s + damping * s + 1.0));
}
// The ladder's linear response at hz: four TPT one-poles h at the engine's warped cutoff with feedback k give
// passband h^4 / (1 + k h^4) low-pass, 4 h^2 (1 - h)^2 / (1 + k h^4) band-pass and (1 - h)^2 / (1 + k h^4) high-pass.
double ladderResponse(FilterMode mode, double cutoff, double k, double passband, double hz)
{
    const double g = warp(cutoff);
    const auto delay = std::polar(1.0, -2 * pi * hz / sampleRate);
    const auto h = g * (1.0 + delay) / ((1 + g) + (g - 1) * delay), h4 = h * h * h * h;
    if (mode == FilterMode::lowpass) return std::abs(passband * h4 / (1.0 + k * h4));
    if (mode == FilterMode::bandpass) return std::abs(4.0 * h * h * (1.0 - h) * (1.0 - h) / (1.0 + k * h4));
    return std::abs((1.0 - h) * (1.0 - h) / (1.0 + k * h4));
}
double db(double ratio) { return 20 * std::log10(ratio); }
void ladderClassic()
{
    std::cout << "SECTION wavetable_ladder_classic\n";
    neutralWith([](LayerSettings& l) { l.filterType = FilterType::clean; });
    // Responses are measured against Clean opened to 20 kHz, so the expected ladder responses are divided by Clean's.
    const auto open = harmonics(filterScene(FilterType::clean, FilterMode::lowpass, 20000, 0));
    CHECK(*std::min_element(open.begin(), open.end()) > 1e-4 * open[0]);
    const auto response = [&](const SynthPatch& p) {
        auto x = harmonics(p);
        for (std::size_t h = 0; h < x.size(); ++h) x[h] /= open[h];
        return x;
    };
    std::array<std::vector<double>, 3> flat;
    for (std::size_t c = 0; c < 3; ++c)
        for (const float resonance : {0.f, .475f, .855f})
            for (const auto mode : {FilterMode::lowpass, FilterMode::bandpass, FilterMode::highpass}) {
                const double cutoff = std::array {200.0, 1000.0, 5000.0}[c], r = resonance / .95, k = 4.3 * r * (.7 + .3 * r);
                const auto x = response(filterScene(FilterType::ladder, mode, float(cutoff), resonance));
                std::vector<double> expected;
                for (std::size_t h = 0; h < x.size(); ++h) {
                    const double hz = 27.5 * double(h + 1);
                    expected.push_back(ladderResponse(mode, cutoff, k, 1 + k, hz) / cleanResponse(20000, 1.41421356, hz));
                }
                const double passband = mode == FilterMode::lowpass ? expected.front()
                    : mode == FilterMode::highpass ? expected.back() : *std::max_element(expected.begin(), expected.end());
                double worst = 0; int compared = 0;
                for (std::size_t h = 0; h < x.size(); ++h)
                    if (expected[h] >= .01 * passband) { worst = std::max(worst, std::abs(db(x[h] / expected[h]))); ++compared; }
                CHECK(compared > 20 && worst <= .1);
                std::cout << "LADDER_RESPONSE cutoff=" << cutoff << " resonance=" << resonance << " mode=" << int(mode)
                          << " harmonics=" << compared << " worst_db=" << worst << '\n';
                if (resonance == 0 && mode == FilterMode::lowpass) flat[c] = x;
            }
    for (std::size_t c = 0; c < 3; ++c) {
        // At resonance 0 Classic is Ladder.
        const auto x = response(filterScene(FilterType::classic, FilterMode::lowpass, float(std::array {200, 1000, 5000}[c]), 0));
        double worst = 0;
        for (std::size_t h = 0; h < x.size(); ++h) if (flat[c][h] >= .01 * flat[c][0]) worst = std::max(worst, std::abs(db(x[h] / flat[c][h])));
        CHECK(worst <= .1);
    }
    const auto passband = [](const std::vector<double>& x) { return (x[0] + x[1] + x[2]) / 3; };
    const auto rise = [&](const std::vector<double>& x) { return db(*std::max_element(x.begin(), x.end()) / passband(x)); };
    const auto zero = response(filterScene(FilterType::classic, FilterMode::lowpass, 400, 0));
    const auto warm = response(filterScene(FilterType::classic, FilterMode::lowpass, 400, .665f));
    const double loss = db(passband(warm) / passband(zero)), resonant = rise(warm);
    CHECK(std::abs(loss + 11.3) <= 2); CHECK(std::abs(resonant - 20.3) <= 2);
    // Scaling every harmonic by one reference cancels in the rise, so the level-0.01 reference serves every level.
    const double quiet = rise(response(filterScene(FilterType::classic, FilterMode::lowpass, 400, .665f, .05f)));
    const double loud = rise(response(filterScene(FilterType::classic, FilterMode::lowpass, 400, .665f, .5f)));
    CHECK(std::abs(loud - quiet) < 1.5);
    std::cout << "CLASSIC_RESPONSE loss_db=" << loss << " peak_db=" << resonant << " peak_at_0.05=" << quiet << " peak_at_0.5=" << loud << '\n';
    {
        // Resonance 0.9 self-oscillates with no input, steady and bounded, and the envelope still ends the voice.
        auto p = filterScene(FilterType::classic, FilterMode::lowpass, 400, .9f); p.layers[0].ampEnvelope.release = .2f;
        SynthEngine e; CHECK(e.setPatch(p) && e.prepare(sampleRate, 512));
        render(e, 24000, {{0, on(21)}});
        p.layers[0].oscillators[0].level = 0; CHECK(e.setPatch(p));
        const auto ring = render(e, 52800);
        CHECK(std::all_of(ring.left.begin(), ring.left.end(), [](float v) { return std::isfinite(v); }));
        double worst = 0, previous = rms(ring, 4800, 4800);
        CHECK(previous > 1e-5);
        for (std::size_t w = 2; w < 11; ++w) {
            const double level = rms(ring, 4800 * w, 4800);
            worst = std::max(worst, std::abs(db(level / previous))); previous = level;
        }
        CHECK(worst <= 1);
        const auto tail = render(e, 16800, {{0, juce::MidiMessage::noteOff(1, 21)}});
        double after = 0;
        for (std::size_t n = 12000; n < tail.left.size(); ++n) after = std::max({after, std::abs(double(tail.left[n])), std::abs(double(tail.right[n]))});
        CHECK(after < 1e-6);
        std::cout << "CLASSIC_RING level_rms=" << previous << " worst_window_db=" << worst << " after_release=" << after << '\n';
    }
}
// The layer's noise alone through the given filter, held on middle C.
SynthPatch vowelScene(float cutoff, float size, float resonance, FilterType type = FilterType::vowel)
{
    auto p = unisonScene(1, 0); auto& l = p.layers[0];
    l.oscillators[0].level = 0; l.noiseLevel = 1;
    l.filterType = type; l.cutoffHz = cutoff; l.vowelSize = size; l.resonance = resonance;
    return p;
}
constexpr int vowelSettle = 2400, vowelFrames = 2400 + 384000;
// |H| per 16384-point bin from y against x, the same noise through another filter: the cross spectrum over x's own,
// summed over Hann segments at half overlap. Unlike either spectrum alone, the ratio carries no noise variance.
std::vector<double> transfer(const std::vector<float>& y, const std::vector<float>& x)
{
    constexpr std::size_t n = 16384;
    std::vector<double> crossRe(n / 2), crossIm(n / 2), power(n / 2), re(n), im(n);
    for (std::size_t start = vowelSettle; start + n <= y.size(); start += n / 2) {
        for (std::size_t k = 0; k < n; ++k) {
            const double w = .5 - .5 * std::cos(2 * pi * double(k) / double(n));
            re[k] = w * y[start + k]; im[k] = w * x[start + k];
        }
        fft(re, im);
        for (std::size_t k = 1; k < n / 2; ++k) {
            const double cr = re[n - k], ci = -im[n - k];
            const double yr = (re[k] + cr) / 2, yi = (im[k] + ci) / 2, xr = (im[k] - ci) / 2, xi = -(re[k] - cr) / 2;
            crossRe[k] += yr * xr + yi * xi; crossIm[k] += yi * xr - yr * xi; power[k] += xr * xr + xi * xi;
        }
    }
    std::vector<double> h(n / 2);
    for (std::size_t k = 1; k < n / 2; ++k) h[k] = std::hypot(crossRe[k], crossIm[k]) / power[k];
    return h;
}
// A formant peak: the largest bin within 20 % of hz, its frequency refined by a parabola, and its -3 dB width.
// The parabola is a least-squares fit to log|H| over the bins within 1 dB of the largest. These peaks are 30 to 90
// bins wide, so a fit through three bins follows the estimate's bin-to-bin ripple by most of a bin.
struct Formant { double hz = 0, width = 0, level = 0; };
Formant formant(const std::vector<double>& h, double hz)
{
    constexpr double bin = sampleRate / 16384;
    std::size_t best = std::size_t(.8 * hz / bin);
    for (auto k = best; k <= std::size_t(1.2 * hz / bin); ++k) if (h[k] > h[best]) best = k;
    const double top = h[best] * std::pow(10.0, -1.0 / 20);
    std::size_t first = best, last = best;
    while (first > 1 && h[first - 1] > top) --first;
    while (last + 2 < h.size() && h[last + 1] > top) ++last;
    std::array<double, 5> s {}; std::array<double, 3> t {};
    for (auto k = first; k <= last; ++k) {
        const double x = double(k) - double(best), y = std::log(h[k]);
        double p = 1;
        for (std::size_t i = 0; i < 5; ++i) { s[i] += p; if (i < 3) t[i] += p * y; p *= x; }
    }
    const auto det = [](double a, double b, double c, double d, double e, double f, double g, double i, double j) {
        return a * (e * j - f * i) - b * (d * j - f * g) + c * (d * i - e * g);
    };
    const double normal = det(s[0], s[1], s[2], s[1], s[2], s[3], s[2], s[3], s[4]);
    const double slope = det(s[0], t[0], s[2], s[1], t[1], s[3], s[2], t[2], s[4]) / normal;
    const double curvature = det(s[0], s[1], t[0], s[1], s[2], t[1], s[2], s[3], t[2]) / normal;
    const double half = h[best] * h[best] / 2;
    std::size_t low = best, high = best;
    while (low > 1 && h[low] * h[low] > half) --low;
    while (high + 1 < h.size() && h[high] * h[high] > half) ++high;
    const auto crossing = [&](std::size_t below, std::size_t above) {
        const double p = h[below] * h[below], q = h[above] * h[above];
        return double(below) + (half - p) / (q - p) * (double(above) - double(below));
    };
    return {(double(best) - slope / (2 * curvature)) * bin, (crossing(high, high - 1) - crossing(low, low + 1)) * bin, h[best]};
}
void vowel()
{
    std::cout << "SECTION wavetable_vowel\n";
    static constexpr std::array<std::array<double, 3>, 5> formants {{{300, 870, 2240}, {570, 840, 2410},
        {730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}}};
    static constexpr std::array<std::array<double, 5>, 3> widths {{{133, 103, 90, 82, 77},
        {201, 147, 121, 105, 95}, {262, 174, 137, 116, 104}}};
    constexpr std::array<float, 5> cutoffs {100, 331, 1100, 3600, 12000};
    const auto open = play(vowelScene(20000, 1, 0, FilterType::clean), 60, vowelFrames).left;
    const auto measure = [&](std::size_t v, float size, float resonance) {
        const auto h = transfer(play(vowelScene(cutoffs[v], size, resonance), 60, vowelFrames).left, open);
        std::array<Formant, 3> peaks;
        for (std::size_t n = 0; n < 3; ++n) peaks[n] = formant(h, formants[v][n] / size);
        return peaks;
    };
    double worstHz = 0, worstWidth = 0, worstLevel = 0, worstSize = 0, worstEnds = 0;
    for (std::size_t v = 0; v < 5; ++v) {
        const auto peaks = measure(v, 1, .2375f);
        double low = 1e9, high = 0;
        for (std::size_t n = 0; n < 3; ++n) {
            worstHz = std::max(worstHz, std::abs(peaks[n].hz / formants[v][n] - 1));
            worstWidth = std::max(worstWidth, std::abs(peaks[n].width / widths[n][1] - 1));
            low = std::min(low, peaks[n].level); high = std::max(high, peaks[n].level);
        }
        worstLevel = std::max(worstLevel, db(high / low));
        for (const float size : {2.f, .5f}) {
            const auto moved = measure(v, size, .2375f);
            for (std::size_t n = 0; n < 3; ++n) worstSize = std::max(worstSize, std::abs(moved[n].hz / peaks[n].hz * size - 1));
        }
        for (const auto [resonance, column] : {std::pair {0.f, 0}, std::pair {.95f, 4}}) {
            const auto ends = measure(v, 1, resonance);
            for (std::size_t n = 0; n < 3; ++n) worstEnds = std::max(worstEnds, std::abs(ends[n].width / widths[n][std::size_t(column)] - 1));
        }
    }
    CHECK(worstHz <= .03); CHECK(worstWidth <= .15); CHECK(worstLevel <= 2); CHECK(worstSize <= .03); CHECK(worstEnds <= .15);
    std::cout << "VOWEL_PEAKS worst_hz=" << worstHz << " worst_width=" << worstWidth << " worst_level_db=" << worstLevel
              << " worst_size=" << worstSize << " worst_end_width=" << worstEnds << '\n';
    // A saw at 110 Hz, cutoff 4 kHz, resonance 0.25: Vowel's makeup holds its level to Clean's.
    const auto saw = [](FilterType type, float cutoff, float size) {
        auto p = unisonScene(1, 0); auto& l = p.layers[0];
        l.oscillators[0].position = 1; l.filterType = type; l.cutoffHz = cutoff; l.vowelSize = size; l.resonance = .25f;
        return p;
    };
    const double level = db(rms(play(saw(FilterType::vowel, 4000, 1), 45, 12000 + 96000), 12000, 96000)
                            / rms(play(saw(FilterType::clean, 4000, 1), 45, 12000 + 96000), 12000, 96000));
    CHECK(std::abs(level) <= 1.5);
    std::cout << "VOWEL_LEVEL db=" << level << '\n';
    // Sweeps across 40 blocks step no further than the filter held at any setting they pass through.
    const auto sweep = [&](float (*cutoff)(int), float (*size)(int)) {
        auto p = saw(FilterType::vowel, cutoff(0), size(0)); SynthEngine e; CHECK(e.setPatch(p) && e.prepare(sampleRate, 512));
        auto x = render(e, 12000, {{0, on(45)}});
        double bound = 0;
        for (int b = 0; b <= 40; ++b) {
            bound = std::max(bound, largestStep(play(saw(FilterType::vowel, cutoff(b), size(b)), 45, 16800), 12000, 16800));
            if (b == 0) continue;
            p.layers[0].cutoffHz = cutoff(b); p.layers[0].vowelSize = size(b); CHECK(e.setPatch(p));
            const auto block = render(e, 512);
            x.left.insert(x.left.end(), block.left.begin(), block.left.end());
            x.right.insert(x.right.end(), block.right.begin(), block.right.end());
        }
        return largestStep(x, 11999, x.left.size()) / bound;
    };
    const double cutoffSweep = sweep([](int b) { return float(100 * std::pow(120.0, b / 40.0)); }, [](int) { return 1.f; });
    const double sizeSweep = sweep([](int) { return 1100.f; }, [](int b) { return float(.5 * std::pow(4.0, b / 40.0)); });
    CHECK(cutoffSweep <= 1.05); CHECK(sizeSweep <= 1.05);
    std::cout << "VOWEL_SWEEP cutoff=" << cutoffSweep << " size=" << sizeSweep << '\n';
    // The filter mode means nothing to Vowel, and the size nothing to Clean.
    const auto lowpass = play(vowelScene(1100, 1, .2375f), 60, 24000);
    for (const auto mode : {FilterMode::bandpass, FilterMode::highpass}) {
        auto p = vowelScene(1100, 1, .2375f); p.layers[0].filterMode = mode;
        const auto x = play(p, 60, 24000);
        CHECK(x.left == lowpass.left && x.right == lowpass.right);
    }
    const auto small = play(vowelScene(1100, .5f, .2375f, FilterType::clean), 60, 24000), large = play(vowelScene(1100, 2, .2375f, FilterType::clean), 60, 24000);
    CHECK(small.left == large.left && small.right == large.right);
}
// A sine detuned by cents, with sustain 1 and the given attack and curve.
SynthPatch curveScene(float cents, float attack, float curve)
{
    auto p = unisonScene(1, 0); auto& l = p.layers[0];
    l.oscillators[0].detune = cents; l.ampEnvelope = {attack, .1f, .1f, 1}; l.attackCurve = curve;
    return p;
}
// The amplitude of a sine at hz around frame `at`, demodulated in quadrature over the cycle centred there.
double amplitude(const std::vector<float>& x, double hz, int at)
{
    const int cycle = int(std::lround(sampleRate / hz));
    double i = 0, q = 0;
    for (int n = at - cycle / 2; n < at - cycle / 2 + cycle; ++n) {
        const double angle = 2 * pi * hz * n / sampleRate;
        i += x[std::size_t(n)] * std::cos(angle); q += x[std::size_t(n)] * std::sin(angle);
    }
    return 2 * std::hypot(i, q) / cycle;
}
void attackCurve()
{
    std::cout << "SECTION wavetable_attack_curve\n";
    neutralWith([](LayerSettings& l) { l.attackCurve = .25f; });
    // Note 114 detuned 23.264 cents is 6 kHz.
    constexpr float cents = 23.264f;
    const double hz = 440 * std::exp2((114 - 69 + cents * .01) / 12);
    const auto law = [](double x, double curve) {
        const double b = -4 * std::log(13.0 / 3) * curve;
        return std::abs(b) < 1e-9 ? x : std::expm1(b * x) / std::expm1(b);
    };
    double worstHalf = 0, worstEnd = 0;
    for (const float attack : {.01f, 1.5f, 8.f})
        for (const float curve : {-1.f, 0.f, .25f, 1.f}) {
            const int length = int(std::lround(attack * sampleRate)), ms = 48;
            const auto x = play(curveScene(cents, attack, curve), 114, length + 25 * ms).left;
            const double sustained = amplitude(x, hz, length + 20 * ms);
            const double half = amplitude(x, hz, length / 2) / sustained;
            const double before = amplitude(x, hz, length - ms) / sustained, after = amplitude(x, hz, length + ms) / sustained;
            worstHalf = std::max(worstHalf, std::abs(half - law(.5, curve)));
            worstEnd = std::max({worstEnd, std::abs(before - law(1 - double(ms) / length, curve)), std::abs(after - 1)});
            std::cout << "ATTACK_CURVE attack=" << attack << " curve=" << curve << " half=" << half << " before_end=" << before << " after_end=" << after << '\n';
        }
    // The level at half the attack is the law's, and the level reaches 1 at the attack time: 1 ms before it is still
    // the law's, and 1 ms after it is 1.
    CHECK(worstHalf <= .01); CHECK(worstEnd <= .01);
    for (const float curve : {-1.f, 0.f, .25f, 1.f}) {
        // A retrigger at 40 % of a 1.5 s attack on a 110 Hz sine continues from the current level.
        const auto x = play(curveScene(0, 1.5f, curve), 45, 36000, {{28800, on(45)}});
        CHECK(largestStep(x, 28800 - 2400, 28800 + 2400) <= 1.05 * largestStep(play(curveScene(0, 0, .25f), 45, 24000), 12000, 24000));
    }
    {
        // The curve shapes the amp contour only: a filter-envelope sweep under an instant amp attack is unchanged.
        auto p = curveScene(0, 0, -1); auto& l = p.layers[0];
        l.oscillators[0].position = 1; l.cutoffHz = 200; l.filterEnvelopeOctaves = 5; l.filterEnvelope = {.5f, .5f, .2f, .3f};
        const auto curved = play(p, 45, 48000);
        l.attackCurve = .25f;
        const auto plain = play(p, 45, 48000);
        CHECK(curved.left == plain.left && curved.right == plain.right);
    }
    std::cout << "ATTACK_CURVE worst_half=" << worstHalf << " worst_end=" << worstEnd << '\n';
}
// Four warm layers: both oscillators at 7 voices, drift, Classic in two and Vowel in two, a fast and a late
// attack curve, and LFO routes on the cutoff and position.
SynthPatch warmScene()
{
    SynthPatch p; p.masterLevel = .27f;
    for (std::size_t i = 0; i < 4; ++i) {
        auto& l = p.layers[i];
        l.enabled = true; l.engine = EngineKind::wavetable; l.algorithmRevision = modernSynthAlgorithmRevision;
        l.pan = float(i) * .2f - .3f; l.noiseLevel = .01f; l.filterEnvelopeOctaves = 1.3f; l.resonance = .5f;
        l.filterType = i < 2 ? FilterType::classic : FilterType::vowel; l.vowelSize = 1.3f; l.drift = 1; l.unisonWidth = .8f;
        l.attackCurve = i % 2 ? -1.f : 1.f; l.ampEnvelope.attack = .02f;
        for (auto& o : l.oscillators) { o.unison = 7; o.unisonCents = 30; o.position = .4f; }
        l.lfos[0] = {LfoShape::sine, false, 2.75f, 1};
        l.modulation[0] = {true, ModSource::lfo1, ModTarget::cutoff, ModBank::wavetable, .1f};
        l.modulation[1] = {true, ModSource::lfo1, ModTarget::osc1Position, ModBank::wavetable, .2f};
    }
    return p;
}
// The warm scene at notes 0 and 127, every warmth key moving across 48 blocks and the counts and filter types
// changing every 8: render and setPatch allocate and free nothing, and the output stays finite.
void warmCallbacks()
{
    std::cout << "SECTION wavetable_warm_callbacks\n";
#if defined(_MSC_VER) && defined(_DEBUG)
    for (const int note : {0, 127}) {
        auto p = warmScene(); SynthEngine e; CHECK(e.setPatch(p) && e.prepare(sampleRate, 512));
        juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi; midi.ensureSize(1024);
        bool finite = true;
        allocations = 0; frees = 0; const auto old = _CrtSetAllocHook(allocationHook); tracking = true;
        for (int b = 0; b < 48; ++b) {
            if (b == 0) midi.addEvent(on(note), 0);
            if (b == 40) midi.addEvent(juce::MidiMessage::noteOff(1, note), 7);
            const float t = float(b) / 47;
            for (auto& l : p.layers) {
                l.unisonWidth = 1 - t; l.drift = t; l.vowelSize = .5f + 1.5f * t; l.attackCurve = 2 * t - 1;
                l.cutoffHz = 100 + 12000 * t; l.resonance = .95f * t;
                for (auto& o : l.oscillators) o.unisonCents = 100 * t;
                if (b % 8 == 4) {
                    l.filterType = FilterType((int(l.filterType) + 1) % 4);
                    for (auto& o : l.oscillators) o.unison = 1 + (o.unison + 2) % 7;
                }
            }
            CHECK(e.setPatch(p));
            e.render(audio, midi); midi.clear();
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < 512; ++n) finite = finite && std::isfinite(audio.getSample(ch, n)) && std::abs(audio.getSample(ch, n)) < 100;
        }
        tracking = false; _CrtSetAllocHook(old);
        CHECK(allocations == 0 && frees == 0); CHECK(finite);
        std::cout << "WARM_CALLBACKS note=" << note << " allocations=" << allocations << " frees=" << frees << " finite=" << finite << '\n';
    }
#else
    CHECK(false); // This acceptance executable requires the MSVC Debug allocation guard.
#endif
}
}

int main()
{
    neutralFingerprint();
    unison();
    drift();
    ladderClassic();
    vowel();
    attackCurve();
    warmCallbacks();
    std::cout << "RESULT checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

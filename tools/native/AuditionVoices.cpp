// Development audition: renders fixed musical briefs straight through the
// routed engine once per synth algorithm revision, so voice kernels can be
// compared by ear (loudness-matched copies), by eye (spectrograms) and by the
// printed measurements. It never touches plugin state, libraries or hosts.
#include "RoutedSynthEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_graphics/juce_graphics.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace airack;
constexpr double rate = 48000;
constexpr int block = 256;
constexpr double pi = std::numbers::pi;

void require(bool good, const std::string& what) { if (!good) throw std::runtime_error(what); }

struct Note { double start, length; int key, velocity; };
struct Brief
{
    std::string name;
    EngineKind engine;
    double seconds;
    std::vector<Note> notes;
    std::function<void(LayerSettings&, SynthPatch&)> voice;
};

LayerSettings copperArc()
{
    LayerSettings l;
    l.enabled = true; l.engine = EngineKind::analog; l.level = .7f;
    l.oscillators[0] = {Waveform::saw, 0, 0, 0, -3, .46f, .46f};
    l.oscillators[1] = {Waveform::triangle, 0, 0, 0, 3, .24f, .54f};
    l.cutoffHz = 2200; l.resonance = .22f;
    l.ampEnvelope = {.012f, .25f, .45f, .72f};
    l.filterEnvelope = {.02f, .3f, .4f, .18f}; l.filterEnvelopeOctaves = .6f;
    l.lfos[0] = {LfoShape::sine, false, .25f, 1};
    l.modulation[0] = {true, ModSource::lfo1, ModTarget::cutoff, ModBank::analog, .035f};
    return l;
}
LayerSettings prismRelay()
{
    LayerSettings l;
    l.enabled = true; l.engine = EngineKind::wavetable; l.level = .7f;
    l.oscillators[0] = {Waveform::saw, 0, .62f, 0, -4, .48f, .5f};
    l.oscillators[1] = {Waveform::saw, 1, .36f, 0, 4, .22f, .5f};
    l.cutoffHz = 7200; l.resonance = .16f;
    l.ampEnvelope = {.008f, .18f, .32f, .68f};
    l.filterEnvelope = {.006f, .22f, .24f, .14f}; l.filterEnvelopeOctaves = .4f;
    l.lfos[0] = {LfoShape::sine, false, .35f, 1};
    l.modulation[0] = {true, ModSource::lfo1, ModTarget::osc1Position, ModBank::wavetable, .035f};
    return l;
}
std::vector<Note> bassLine()
{
    std::vector<Note> notes;
    const int keys[] {36, 36, 48, 36, 39, 36, 43, 41, 36, 36, 48, 36, 46, 43, 39, 41};
    for (int i = 0; i < 16; ++i) notes.push_back({.25 * i, .2, keys[i], i % 2 ? 92 : 112});
    return notes;
}
std::vector<Note> arpeggio(int root)
{
    std::vector<Note> notes;
    const int steps[] {0, 4, 7, 11, 12, 11, 7, 4};
    for (int i = 0; i < 24; ++i) notes.push_back({.125 * i, .1, root + steps[i % 8] + (i >= 16 ? -3 : 0), i % 4 ? 88 : 110});
    return notes;
}
std::vector<Note> melody(int root)
{
    const double lengths[] {.5, .25, .25, .75, .25, .5, .5, 1};
    const int steps[] {0, 2, 3, 7, 5, 3, 2, 0};
    std::vector<Note> notes; double t = 0;
    for (int i = 0; i < 8; ++i) { notes.push_back({t, lengths[i] * .95, root + steps[i], 100}); t += lengths[i]; }
    return notes;
}
std::vector<Note> chords(std::vector<std::vector<int>> voicings, double hold, int velocity = 96)
{
    std::vector<Note> notes;
    for (size_t c = 0; c < voicings.size(); ++c)
        for (int key : voicings[c]) notes.push_back({hold * c, hold * .97, key, velocity});
    return notes;
}
std::vector<Note> scale()
{
    std::vector<Note> notes;
    for (int i = 0; i < 25; ++i) notes.push_back({.16 * i, .14, 72 + 2 * i - (i > 18 ? 2 * (i - 18) : 0), 100});
    return notes;
}

std::vector<Brief> briefs()
{
    std::vector<Brief> b;
    b.push_back({"analog-bass", EngineKind::analog, 4.6, bassLine(), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[1].tune = -12; l.oscillators[1].level = .3f; l.cutoffHz = 320; l.resonance = .35f;
        l.filterEnvelope = {.001f, .28f, .1f, 0}; l.filterEnvelopeOctaves = 2.6f; l.ampEnvelope = {.002f, .4f, .12f, .75f}; }});
    b.push_back({"analog-pluck", EngineKind::analog, 4.2, arpeggio(60), [](LayerSettings& l, SynthPatch&) {
        l.cutoffHz = 520; l.resonance = .3f; l.filterEnvelope = {.001f, .22f, .2f, 0}; l.filterEnvelopeOctaves = 3.6f;
        l.ampEnvelope = {.001f, .55f, .45f, 0}; }});
    b.push_back({"analog-lead", EngineKind::analog, 4.8, melody(67), [](LayerSettings& l, SynthPatch& p) {
        l.oscillators[1] = {Waveform::pulse, 0, 0, 0, 7, .32f, .38f}; l.cutoffHz = 1500; l.resonance = .62f;
        l.filterEnvelope = {.004f, .45f, .3f, .3f}; l.filterEnvelopeOctaves = 1.6f; l.ampEnvelope = {.004f, .3f, .3f, .85f};
        p.performance = {VoiceMode::monophonic, .05f, true}; }});
    b.push_back({"analog-brass", EngineKind::analog, 5.2, chords({{48, 55, 60, 64}, {46, 53, 58, 62}}, 2), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[0].detune = -7; l.oscillators[1] = {Waveform::saw, 0, 0, 0, 7, .38f, .5f}; l.cutoffHz = 650; l.resonance = .18f;
        l.filterEnvelope = {.07f, .55f, .4f, .45f}; l.filterEnvelopeOctaves = 2.2f; l.ampEnvelope = {.035f, .3f, .35f, .85f}; }});
    b.push_back({"analog-pad", EngineKind::analog, 8, chords({{48, 55, 59, 64}}, 4.5, 84), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[0].detune = -9; l.oscillators[1] = {Waveform::saw, 0, 0, 0, 9, .4f, .5f}; l.cutoffHz = 1100; l.resonance = .25f;
        l.filterEnvelope = {1.4f, 1.5f, 2.5f, .55f}; l.filterEnvelopeOctaves = 1; l.ampEnvelope = {.9f, 1.5f, 2.6f, .85f};
        l.lfos[0].rateHz = .2f; l.modulation[0].amount = .06f; }});
    b.push_back({"analog-resonance-sweep", EngineKind::analog, 6.5, {{0, 6, 36, 100}}, [](LayerSettings& l, SynthPatch&) {
        l.oscillators[0].level = .55f; l.oscillators[1].level = 0; l.cutoffHz = 260; l.resonance = .86f; l.filterEnvelopeOctaves = 0;
        l.ampEnvelope = {.005f, .1f, .3f, 1}; l.lfos[0] = {LfoShape::triangle, false, .16f, 1}; l.modulation[0].amount = .55f; }});
    b.push_back({"analog-high-scale", EngineKind::analog, 4.6, scale(), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[1].level = 0; l.cutoffHz = 20000; l.resonance = 0; l.filterEnvelopeOctaves = 0;
        l.ampEnvelope = {.002f, .1f, .1f, 1}; l.modulation[0].enabled = false; }});
    b.push_back({"digital-bass", EngineKind::wavetable, 4.6, bassLine(), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[0].position = .95f; l.oscillators[1].tune = -12; l.oscillators[1].position = .5f; l.oscillators[1].level = .3f;
        l.cutoffHz = 1800; l.resonance = .25f; l.filterEnvelope = {.001f, .2f, .1f, 0}; l.filterEnvelopeOctaves = 2.2f; l.ampEnvelope = {.001f, .3f, .1f, .8f}; }});
    b.push_back({"digital-pluck", EngineKind::wavetable, 4.2, arpeggio(72), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[0].position = .3f; l.cutoffHz = 2400; l.resonance = .2f; l.filterEnvelope = {.001f, .25f, .2f, 0};
        l.filterEnvelopeOctaves = 2.4f; l.ampEnvelope = {.001f, .45f, .35f, 0}; }});
    b.push_back({"digital-lead", EngineKind::wavetable, 4.8, melody(72), [](LayerSettings& l, SynthPatch& p) {
        l.oscillators[0].position = 1; l.oscillators[1].position = .8f; l.oscillators[1].detune = 9; l.oscillators[1].level = .35f;
        l.cutoffHz = 6000; l.resonance = .45f; l.ampEnvelope = {.003f, .3f, .3f, .85f}; p.performance = {VoiceMode::monophonic, .04f, true}; }});
    b.push_back({"digital-chord", EngineKind::wavetable, 5.5, chords({{60, 63, 67, 70}, {58, 62, 65, 69}}, 2.2), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[0].position = .5f; l.cutoffHz = 5200; l.resonance = .2f; l.ampEnvelope = {.02f, .4f, .6f, .75f};
        l.lfos[0].rateHz = .3f; l.modulation[0].amount = .3f; }});
    b.push_back({"digital-pad", EngineKind::wavetable, 8, chords({{48, 55, 62, 67}}, 4.5, 84), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[0].position = .6f; l.oscillators[1].detune = 11; l.oscillators[1].level = .32f; l.cutoffHz = 4200;
        l.ampEnvelope = {.8f, 1.2f, 2.6f, .85f}; l.filterEnvelope = {1, 1.5f, 2, .6f}; l.filterEnvelopeOctaves = .8f; }});
    b.push_back({"digital-high-scale", EngineKind::wavetable, 4.6, scale(), [](LayerSettings& l, SynthPatch&) {
        l.oscillators[0].position = 1; l.oscillators[1].level = 0; l.cutoffHz = 20000; l.resonance = 0; l.filterEnvelopeOctaves = 0;
        l.ampEnvelope = {.002f, .1f, .1f, 1}; l.modulation[0].enabled = false; }});
    return b;
}

std::uint64_t hashOf(const juce::AudioBuffer<float>& x)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (int ch = 0; ch < x.getNumChannels(); ++ch)
        for (int i = 0; i < x.getNumSamples(); ++i) hash = (hash ^ std::bit_cast<std::uint32_t>(x.getSample(ch, i))) * 1099511628211ull;
    return hash;
}
void addEffect(RoutedPatch& p, EffectKind kind)
{
    const auto slot = p.layout.effectCount[masterChain];
    RackEdit edit; edit.kind = RackEditKind::addEffect; edit.slot = slot; edit.destination = masterChain; edit.position = slot; edit.effect = kind;
    require(editRackLayout(p.layout, edit, p.layout) == RackEditResult::applied, "effect placement failed");
}
RoutedPatch patchFor(const Brief& b, AlgorithmRevision revision, bool wet)
{
    RoutedPatch p;
    p.synth.masterLevel = 1;
    auto& l = p.synth.layers[0];
    l = b.engine == EngineKind::analog ? copperArc() : prismRelay();
    b.voice(l, p.synth);
    l.algorithmRevision = revision;
    p.compatibility.synth[0][static_cast<size_t>(l.engine)] = revision;
    p.outputGainDb = 24;
    if (wet)
    {
        addEffect(p, EffectKind::drive); addEffect(p, EffectKind::reverb);
        p.effects[0].drive = {3, DriveShape::tanh, 12000, .15f};
        p.effects[1].reverb = {.7f, 2.4f, .35f, 18, .16f};
        p.effects[1].algorithmRevision = 2; p.compatibility.effects[1][2] = 2;
    }
    return p;
}
juce::AudioBuffer<float> render(const RoutedPatch& p, const std::vector<Note>& notes, double seconds,
                                RenderQuality quality = RenderQuality::realtime)
{
    RoutedSynthEngine engine;
    require(engine.setPatch(p), "patch rejected");
    require(engine.prepare(rate, block, quality), "prepare failed");
    struct Event { int frame; bool on; juce::MidiMessage message; };
    std::vector<Event> events;
    for (const auto& n : notes)
    {
        events.push_back({int(std::lround(n.start * rate)), true, juce::MidiMessage::noteOn(1, n.key, juce::uint8(n.velocity))});
        events.push_back({int(std::lround((n.start + n.length) * rate)), false, juce::MidiMessage::noteOff(1, n.key)});
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.frame != b.frame ? a.frame < b.frame : !a.on && b.on; });
    const int total = int(std::ceil(seconds * rate));
    juce::AudioBuffer<float> out(2, total);
    for (int start = 0; start < total; start += block)
    {
        const int size = std::min(block, total - start);
        juce::AudioBuffer<float> audio(2, size);
        juce::MidiBuffer midi;
        for (const auto& e : events) if (e.frame >= start && e.frame < start + size) midi.addEvent(e.message, e.frame - start);
        engine.render(audio, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, start, audio, ch, 0, size);
    }
    return out;
}

// Bit-level fingerprints of paths the briefs do not cover, for revision-1 retention checks.
void fingerprints(AlgorithmRevision revision)
{
    const auto list = briefs();
    const auto& pad = list[4];
    const auto print = [&](const std::string& name, const juce::AudioBuffer<float>& audio) {
        std::cout << "HASH revision=" << int(revision) << ' ' << name << ' ' << std::hex << hashOf(audio) << std::dec << '\n';
    };
    for (auto mode : {FilterMode::bandpass, FilterMode::highpass})
    {
        auto p = patchFor(list[1], revision, false);
        p.synth.layers[0].filterMode = mode; p.synth.layers[0].noiseLevel = .2f; p.synth.layers[0].oscillators[1].waveform = Waveform::pulse;
        if (validateRoutedPatch(p)) print(mode == FilterMode::bandpass ? "bandpass-noise" : "highpass-noise", render(p, list[1].notes, 2));
    }
    auto higher = patchFor(list[10], revision, true);
    if (validateRoutedPatch(higher)) print("digital-chord-higher", render(higher, list[10].notes, 3, RenderQuality::higher));
    auto layered = patchFor(pad, revision, false);
    RackEdit edit; edit.kind = RackEditKind::addSynth; edit.slot = 1; edit.position = 1;
    if (editRackLayout(layered.layout, edit, layered.layout) == RackEditResult::applied)
    {
        layered.synth.layers[1] = prismRelay(); layered.synth.layers[1].pan = .4f; layered.synth.layers[1].algorithmRevision = revision;
        layered.compatibility.synth[1][1] = revision;
        if (validateRoutedPatch(layered)) print("two-layer-pad", render(layered, pad.notes, 3));
        else std::cout << "HASH two-layer-pad rejected\n";
    }
}

// ITU-R BS.1770 K-weighted, gated integrated loudness at 48 kHz.
double loudness(const juce::AudioBuffer<float>& x)
{
    const double b1[3] {1.53512485958697, -2.69169618940638, 1.19839281085285}, a1[3] {1, -1.69065929318241, .73248077421585};
    const double b2[3] {1, -2, 1}, a2[3] {1, -1.99004745483398, .99007225036621};
    const int n = x.getNumSamples();
    std::vector<double> power(size_t(n), 0.0);
    for (int ch = 0; ch < x.getNumChannels(); ++ch)
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0, u1 = 0, u2 = 0, z1 = 0, z2 = 0;
        for (int i = 0; i < n; ++i)
        {
            const double in = x.getSample(ch, i);
            const double y = b1[0] * in + b1[1] * x1 + b1[2] * x2 - a1[1] * y1 - a1[2] * y2; x2 = x1; x1 = in; y2 = y1; y1 = y;
            const double z = b2[0] * y + b2[1] * u1 + b2[2] * u2 - a2[1] * z1 - a2[2] * z2; u2 = u1; u1 = y; z2 = z1; z1 = z;
            power[size_t(i)] += z * z;
        }
    }
    const int size = int(.4 * rate), hop = size / 4;
    std::vector<double> blocks;
    for (int s = 0; s + size <= n; s += hop)
    {
        double sum = 0; for (int i = s; i < s + size; ++i) sum += power[size_t(i)];
        blocks.push_back(sum / size);
    }
    const auto gated = [&](double threshold) {
        double sum = 0; int count = 0;
        for (double z : blocks) if (-.691 + 10 * std::log10(z + 1e-30) > threshold) { sum += z; ++count; }
        return count ? sum / count : 0.0;
    };
    const double absolute = gated(-70);
    if (absolute <= 0) return -100;
    const double relative = gated(-.691 + 10 * std::log10(absolute) - 10);
    return -.691 + 10 * std::log10(std::max(relative, 1e-30));
}
struct Spectrum { std::vector<double> power; double binHz; };
Spectrum averageSpectrum(const juce::AudioBuffer<float>& x)
{
    constexpr int order = 12, size = 1 << order;
    juce::dsp::FFT fft(order);
    std::vector<float> data(2 * size);
    Spectrum s {std::vector<double>(size / 2 + 1, 0.0), rate / size};
    for (int start = 0; start + size <= x.getNumSamples(); start += size / 2)
    {
        for (int i = 0; i < size; ++i)
            data[size_t(i)] = float(.5 * (x.getSample(0, start + i) + x.getSample(1, start + i)) * (.5 - .5 * std::cos(2 * pi * i / size)));
        std::fill(data.begin() + size, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform(data.data(), true);
        for (int k = 0; k <= size / 2; ++k) s.power[size_t(k)] += double(data[size_t(k)]) * data[size_t(k)];
    }
    return s;
}
struct Measures { double peak, rms, lufs, centroid, above5k, above10k, above15k; std::uint64_t hash; };
Measures measure(const juce::AudioBuffer<float>& x)
{
    Measures m {};
    m.hash = 1469598103934665603ull;
    double sum = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < x.getNumSamples(); ++i)
        {
            const float sample = x.getSample(ch, i);
            m.hash = (m.hash ^ std::bit_cast<std::uint32_t>(sample)) * 1099511628211ull;
            const double v = sample; m.peak = std::max(m.peak, std::abs(v)); sum += v * v;
        }
    m.rms = std::sqrt(sum / (2.0 * x.getNumSamples()));
    m.lufs = loudness(x);
    const auto s = averageSpectrum(x);
    double total = 0, weighted = 0, a5 = 0, a10 = 0, a15 = 0;
    for (size_t k = 1; k < s.power.size(); ++k)
    {
        const double f = k * s.binHz, p = s.power[k];
        total += p; weighted += f * p;
        if (f > 5000) a5 += p;
        if (f > 10000) a10 += p;
        if (f > 15000) a15 += p;
    }
    if (total > 0) { m.centroid = weighted / total; m.above5k = a5 / total; m.above10k = a10 / total; m.above15k = a15 / total; }
    return m;
}

juce::Colour magma(double t)
{
    static const juce::Colour stops[] {juce::Colour(0xff000004), juce::Colour(0xff3b0f70), juce::Colour(0xff8c2981),
                                       juce::Colour(0xffde4968), juce::Colour(0xfffe9f6d), juce::Colour(0xfffcfdbf)};
    t = std::clamp(t, 0.0, 1.0) * 5;
    const int i = std::min(4, int(t));
    return stops[i].interpolatedWith(stops[i + 1], float(t - i));
}
constexpr double lowestHz = 30;
int rowFor(double hz, int height) { return int(std::lround((height - 1) * (1 - std::log(hz / lowestHz) / std::log(rate * .5 / lowestHz)))); }
juce::Image spectrogram(const juce::AudioBuffer<float>& x, int height)
{
    constexpr int order = 12, size = 1 << order;
    const int hop = int(rate / 100);
    const int columns = std::max(1, (x.getNumSamples() - size) / hop + 1);
    juce::dsp::FFT fft(order);
    std::vector<float> data(2 * size), window(size);
    for (int i = 0; i < size; ++i) window[size_t(i)] = float(.5 - .5 * std::cos(2 * pi * i / size));
    std::vector<std::pair<int, int>> rows(static_cast<size_t>(height));
    const double top = rate * .5;
    for (int y = 0; y < height; ++y)
    {
        const auto hz = [&](double r) { return lowestHz * std::pow(top / lowestHz, 1 - r / height); };
        const int low = int(std::floor(hz(y + 1) / rate * size)), high = int(std::ceil(hz(y) / rate * size));
        rows[size_t(y)] = {std::clamp(low, 0, size / 2), std::clamp(std::max(low, high), 0, size / 2)};
    }
    juce::Image image(juce::Image::RGB, columns, height, true);
    for (int c = 0; c < columns; ++c)
    {
        const int start = c * hop;
        for (int i = 0; i < size; ++i)
            data[size_t(i)] = float(.5 * (x.getSample(0, start + i) + x.getSample(1, start + i))) * window[size_t(i)];
        std::fill(data.begin() + size, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform(data.data(), true);
        for (int y = 0; y < height; ++y)
        {
            float peak = 0;
            for (int k = rows[size_t(y)].first; k <= rows[size_t(y)].second; ++k) peak = std::max(peak, data[size_t(k)]);
            const double db = 20 * std::log10(peak / (size / 4.0) + 1e-12);
            image.setPixelAt(c, y, magma((db + 120) / 110));
        }
    }
    return image;
}
void writePng(const juce::File& file, const juce::Image& image)
{
    auto stream = file.createOutputStream();
    require(stream != nullptr && juce::PNGImageFormat().writeImageToStream(image, *stream), "PNG write failed: " + file.getFullPathName().toStdString());
}
juce::Image comparison(const std::string& title, const std::string& subtitle, const std::vector<std::pair<std::string, juce::Image>>& panels)
{
    constexpr int label = 26, header = 44, axis = 46;
    int width = 0, height = header;
    for (const auto& p : panels) { width = std::max(width, p.second.getWidth()); height += p.second.getHeight() + label; }
    juce::Image image(juce::Image::RGB, width + axis, height, true);
    juce::Graphics g(image);
    g.fillAll(juce::Colour(0xff15151a));
    g.setFont(juce::Font(juce::FontOptions(16.0f)));
    g.setColour(juce::Colours::white);
    g.drawFittedText(title, axis, 2, width - 4, 22, juce::Justification::centredLeft, 1);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.setColour(juce::Colour(0xaaffffff));
    g.drawFittedText(subtitle, axis, 24, width - 4, 16, juce::Justification::centredLeft, 1);
    int y = header;
    for (const auto& [name, panel] : panels)
    {
        g.drawImageAt(panel, axis, y);
        g.setColour(juce::Colour(0x66ffffff));
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        for (double hz : {100.0, 1000.0, 10000.0})
        {
            const int row = y + rowFor(hz, panel.getHeight());
            g.drawHorizontalLine(row, float(axis), float(axis + panel.getWidth()));
            g.drawText(hz >= 1000 ? juce::String(int(hz / 1000)) + "k" : juce::String(int(hz)), 0, row - 8, axis - 6, 16, juce::Justification::centredRight);
        }
        y += panel.getHeight();
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions(15.0f)));
        g.drawText(name, axis, y, width, label, juce::Justification::centredLeft);
        y += label;
    }
    return image;
}
void writeWav(const juce::File& file, const juce::AudioBuffer<float>& audio, bool floatingPoint)
{
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    require(stream != nullptr, "cannot create " + file.getFullPathName().toStdString());
    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions().withSampleRate(rate).withNumChannels(2).withBitsPerSample(floatingPoint ? 32 : 24)
        .withSampleFormat(floatingPoint ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint : juce::AudioFormatWriterOptions::SampleFormat::integral);
    auto writer = format.createWriterFor(stream, options);
    require(writer != nullptr && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()) && writer->flush(), "WAV write failed");
}

// Steady single-oscillator probes through the bare engine.
SynthPatch probePatch(EngineKind engine, AlgorithmRevision revision)
{
    SynthPatch p;
    auto& l = p.layers[0];
    l.engine = engine; l.algorithmRevision = revision; l.level = 1;
    l.oscillators[0] = {Waveform::saw, 0, 1, 0, 0, 1, .5f}; l.oscillators[1].level = 0;
    l.cutoffHz = 20000; l.resonance = 0; l.ampEnvelope = {0, 0, 0, 1}; l.filterEnvelope = {0, 0, 0, 1};
    p.masterLevel = 1;
    return p;
}
std::vector<float> renderMono(SynthEngine& e, int count, const std::vector<std::pair<int, juce::MidiMessage>>& events)
{
    std::vector<float> result(static_cast<size_t>(count));
    for (int start = 0; start < count; start += block)
    {
        const int size = std::min(block, count - start);
        juce::AudioBuffer<float> audio(2, size); juce::MidiBuffer midi;
        for (const auto& [frame, message] : events) if (frame >= start && frame < start + size) midi.addEvent(message, frame - start);
        e.render(audio, midi);
        for (int i = 0; i < size; ++i) result[size_t(start + i)] = audio.getSample(0, i) + audio.getSample(1, i);
    }
    return result;
}
void bandwidthProbe(EngineKind engine, AlgorithmRevision revision)
{
    constexpr int order = 16, size = 1 << order;
    for (int key : {45, 57, 69, 76, 88, 96})
    {
        SynthEngine e; require(e.setPatch(probePatch(engine, revision)) && e.prepare(rate, block), "probe rejected");
        const auto x = renderMono(e, size + 12000, {{0, juce::MidiMessage::noteOn(1, key, juce::uint8(127))}});
        juce::dsp::FFT fft(order); std::vector<float> data(2 * size);
        for (int i = 0; i < size; ++i)
        {
            const double t = double(i) / (size - 1);
            const double w = .35875 - .48829 * std::cos(2 * pi * t) + .14128 * std::cos(4 * pi * t) - .01168 * std::cos(6 * pi * t);
            data[size_t(i)] = float(x[size_t(12000 + i)] * w);
        }
        fft.performFrequencyOnlyForwardTransform(data.data(), true);
        const double f0 = 440 * std::exp2((key - 69) / 12.0), bin = rate / size;
        std::vector<double> power(size / 2 + 1);
        double total = 0;
        for (int k = 8; k <= size / 2; ++k) { power[size_t(k)] = double(data[size_t(k)]) * data[size_t(k)]; total += power[size_t(k)]; }
        double harmonic = 0, fundamental = 0; int top = 0;
        std::vector<bool> used(power.size(), false);
        for (int h = 1; h * f0 < rate / 2; ++h)
        {
            const double centre = h * f0 / bin;
            const int spread = 6 + int(std::ceil(centre * .004));
            double energy = 0;
            for (int k = std::max(8, int(centre) - spread); k <= std::min(size / 2, int(centre) + spread); ++k)
                if (!used[size_t(k)]) { energy += power[size_t(k)]; used[size_t(k)] = true; }
            harmonic += energy;
            if (h == 1) fundamental = energy;
            // Ideal saw harmonic h is 1/h of the fundamental: count it as present within 6 dB.
            if (fundamental > 0 && energy >= fundamental / (h * double(h)) * .25) top = h;
        }
        const double alias = std::max(total - harmonic, 1e-30) / std::max(total, 1e-30);
        std::cout << "BANDWIDTH engine=" << (engine == EngineKind::analog ? "analog" : "wavetable") << " revision=" << int(revision)
                  << " key=" << key << " f0=" << std::fixed << std::setprecision(1) << f0 << " top_harmonic_hz=" << top * f0
                  << " alias_db=" << std::setprecision(1) << 10 * std::log10(alias) << '\n';
    }
}
void envelopeProbe(EngineKind engine, AlgorithmRevision revision)
{
    auto p = probePatch(engine, revision);
    p.layers[0].oscillators[0].waveform = Waveform::sine; p.layers[0].oscillators[0].position = 0;
    p.layers[0].ampEnvelope = {0, 1, .5f, 0};
    SynthEngine e; require(e.setPatch(p) && e.prepare(rate, block), "probe rejected");
    const int count = int(rate * 1.6);
    const auto x = renderMono(e, count, {{0, juce::MidiMessage::noteOn(1, 69, juce::uint8(127))}});
    const auto level = [&](double seconds) {
        const int centre = int(seconds * rate); float peak = 0;
        for (int i = std::max(0, centre - 120); i < std::min(count, centre + 120); ++i) peak = std::max(peak, std::abs(x[size_t(i)]));
        return peak;
    };
    const double reference = level(.004);
    std::cout << "ENVELOPE engine=" << (engine == EngineKind::analog ? "analog" : "wavetable") << " revision=" << int(revision) << " decay1s_levels";
    for (double t : {.25, .5, .75, 1.0}) std::cout << " t" << t << "=" << std::setprecision(3) << level(t) / std::max(reference, 1e-12);
    std::cout << '\n';
}
void onsetProbe(EngineKind engine, AlgorithmRevision revision)
{
    auto p = probePatch(engine, revision);
    p.layers[0].cutoffHz = 3000; p.layers[0].oscillators[1] = {Waveform::saw, 0, 1, 0, 7, .6f, .5f};
    SynthEngine e; require(e.setPatch(p) && e.prepare(rate, block), "probe rejected");
    const int second = int(rate * .5), window = int(rate * .03);
    const auto x = renderMono(e, int(rate), {{0, juce::MidiMessage::noteOn(1, 57, juce::uint8(110))}, {second - 2000, juce::MidiMessage::noteOff(1, 57)},
                                              {second, juce::MidiMessage::noteOn(1, 57, juce::uint8(110))}});
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < window; ++i) { const double a = x[size_t(i)], b = x[size_t(second + i)]; ab += a * b; aa += a * a; bb += b * b; }
    std::cout << "ONSET engine=" << (engine == EngineKind::analog ? "analog" : "wavetable") << " revision=" << int(revision)
              << " repeated_note_correlation=" << std::setprecision(4) << ab / std::sqrt(std::max(aa * bb, 1e-30)) << '\n';
}
void resonanceProbe(EngineKind engine, AlgorithmRevision revision)
{
    for (float resonance : {.5f, .8f, .95f})
    {
        auto p = probePatch(engine, revision);
        p.layers[0].oscillators[0].level = 0; p.layers[0].noiseLevel = .25f; p.layers[0].cutoffHz = 1000; p.layers[0].resonance = resonance;
        SynthEngine e; require(e.setPatch(p) && e.prepare(rate, block), "probe rejected");
        constexpr int order = 14, size = 1 << order;
        const auto x = renderMono(e, int(rate * 1.5), {{0, juce::MidiMessage::noteOn(1, 60, juce::uint8(127))}});
        juce::dsp::FFT fft(order); std::vector<double> power(size / 2 + 1, 0.0); std::vector<float> data(2 * size);
        for (int start = int(rate * .2); start + size <= int(x.size()); start += size / 2)
        {
            for (int i = 0; i < size; ++i) data[size_t(i)] = float(x[size_t(start + i)] * (.5 - .5 * std::cos(2 * pi * i / size)));
            std::fill(data.begin() + size, data.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform(data.data(), true);
            for (int k = 0; k <= size / 2; ++k) power[size_t(k)] += double(data[size_t(k)]) * data[size_t(k)];
        }
        const auto band = [&](double low, double high) {
            double sum = 0; int count = 0;
            for (int k = int(low / rate * size); k <= int(high / rate * size); ++k) { sum += power[size_t(k)]; ++count; }
            return sum / std::max(count, 1);
        };
        float peak = 0; for (auto v : x) peak = std::max(peak, std::abs(v));
        std::cout << "RESONANCE engine=" << (engine == EngineKind::analog ? "analog" : "wavetable") << " revision=" << int(revision)
                  << " resonance=" << std::setprecision(2) << resonance << " peak_over_passband_db=" << std::setprecision(1)
                  << 10 * std::log10(band(900, 1100) / std::max(band(150, 350), 1e-30)) << " stopband_4k_db="
                  << 10 * std::log10(band(3800, 4200) / std::max(band(150, 350), 1e-30)) << " output_peak=" << std::setprecision(3) << peak << '\n';
    }
}
// Staggered notes start one frame apart; unison notes start together, so every
// voice of a layer sees the same envelope values.
struct CpuLoad { double sampleRate = rate; int blockSize = block; bool unison = false; };
// Worst-case voice load: four layers (two per engine), both oscillators, noise,
// resonance, filter envelope and an LFO route, all 16 voices held for 10 s.
void cpuProbe(AlgorithmRevision revision, const CpuLoad& load)
{
    SynthPatch p;
    for (size_t i = 0; i < p.layers.size(); ++i)
    {
        auto& l = p.layers[i];
        l = i < 2 ? copperArc() : prismRelay();
        l.enabled = true; l.algorithmRevision = revision; l.level = .25f; l.noiseLevel = .05f; l.resonance = .5f;
        l.oscillators[0].level = .5f; l.oscillators[1].level = .5f; l.oscillators[1].detune = 7;
        l.filterEnvelopeOctaves = 2; l.ampEnvelope.sustain = .8f;
        l.modulation[0] = {true, ModSource::lfo1, ModTarget::cutoff, i < 2 ? ModBank::analog : ModBank::wavetable, .2f};
    }
    require(validateSynthPatch(p), "cpu patch rejected");
    for (int run = 0; run < 3; ++run)
    {
        SynthEngine e; require(e.setPatch(p) && e.prepare(load.sampleRate, load.blockSize), "cpu prepare failed");
        juce::AudioBuffer<float> audio(2, load.blockSize); juce::MidiBuffer notes, empty;
        for (int v = 0; v < 16; ++v) notes.addEvent(juce::MidiMessage::noteOn(1, 36 + 2 * v, juce::uint8(100)), load.unison ? 0 : v);
        const int blocks = int(10 * load.sampleRate / load.blockSize);
        std::vector<double> micros(static_cast<size_t>(blocks));
        for (int b = 0; b < blocks; ++b)
        {
            const auto start = std::chrono::steady_clock::now();
            e.render(audio, b == 0 ? notes : empty);
            micros[size_t(b)] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        }
        require(e.activeNoteCount() == 16, "cpu workload lost voices");
        std::vector<double> steady(micros.begin() + 20, micros.end());
        double sum = 0; for (double m : steady) sum += m;
        const double mean = sum / double(steady.size());
        std::sort(steady.begin(), steady.end());
        const double budget = 1e6 * load.blockSize / load.sampleRate;
        std::cout << "CPU revision=" << int(revision) << " rate=" << int(load.sampleRate) << " block=" << load.blockSize
                  << " notes=" << (load.unison ? "unison" : "staggered") << " run=" << run << std::fixed << std::setprecision(1) << " mean_us=" << mean
                  << " p99_us=" << steady[size_t(.99 * double(steady.size() - 1))] << " max_us=" << steady.back()
                  << " budget_us=" << budget << " load_percent=" << std::setprecision(2) << 100 * mean / budget << '\n';
    }
}

// Revision-1 effect audit (--effects). Stimuli come from a bare revision-2
// wavetable voice (a sine at position 0, or white noise) rendered once dry and
// once through master effects, so each measurement compares an effect's output
// with its exact input. "measured/predicted" pairs come from the kernel's own
// math; an EFFECT_CHECK line fails when the measurement does not reproduce it.
constexpr std::array<double, 2> auditRates {44100, 48000};
constexpr std::array<RenderQuality, 2> auditQualities {RenderQuality::realtime, RenderQuality::higher};
int failedChecks = 0;

const char* qualityName(RenderQuality quality) { return quality == RenderQuality::higher ? "higher" : "realtime"; }
double internalRate(double sampleRate, RenderQuality quality) { return quality == RenderQuality::higher ? 2 * sampleRate : sampleRate; }
std::string fixed(double value, int digits = 2)
{
    std::ostringstream text;
    text << std::fixed << std::setprecision(digits) << value;
    return text.str();
}
void check(bool good, const std::string& what)
{
    if (!good) ++failedChecks;
    std::cout << "EFFECT_CHECK " << (good ? "pass " : "FAIL ") << what << '\n';
}

struct Stimulus { double hz = 1000; float level = .5f; bool noise = false; };
RoutedPatch stimulusPatch(const Stimulus& s, int& key)
{
    RoutedPatch p;
    p.synth.masterLevel = 1;
    auto& l = p.synth.layers[0];
    l.enabled = true; l.engine = EngineKind::wavetable; l.algorithmRevision = 2; l.level = s.level;
    p.compatibility.synth[0][static_cast<size_t>(EngineKind::wavetable)] = 2;
    const double pitch = 69 + 12 * std::log2(s.hz / 440);
    const int octaves = pitch > 127.4 ? int(std::ceil((pitch - 127.4) / 12)) : 0;
    key = std::clamp(int(std::lround(pitch)) - 12 * octaves, 0, 127);
    l.oscillators[0] = {Waveform::sine, 0, 0, float(12 * octaves), float((pitch - 12 * octaves - key) * 100), s.noise ? 0.0f : 1.0f, .5f};
    l.oscillators[1].level = 0; l.noiseLevel = s.noise ? 1.0f : 0.0f;
    l.cutoffHz = 20000; l.resonance = 0; l.filterEnvelopeOctaves = 0; l.ampEnvelope = {0, 0, 0, 1};
    return p;
}
juce::AudioBuffer<float> renderStimulus(const RoutedPatch& p, int key, double seconds, double sampleRate,
                                        RenderQuality quality = RenderQuality::realtime)
{
    RoutedSynthEngine engine;
    require(engine.setPatch(p), "stimulus patch rejected");
    require(engine.prepare(sampleRate, block, quality), "stimulus prepare failed");
    const int total = int(std::ceil(seconds * sampleRate));
    juce::AudioBuffer<float> out(2, total);
    for (int start = 0; start < total; start += block)
    {
        const int size = std::min(block, total - start);
        juce::AudioBuffer<float> audio(2, size);
        juce::MidiBuffer midi;
        if (start == 0) midi.addEvent(juce::MidiMessage::noteOn(1, key, juce::uint8(127)), 0);
        engine.render(audio, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, start, audio, ch, 0, size);
    }
    return out;
}
// Appends the effect to the master chain, after an optional exact-gain stereo
// utility that raises the effect's input level.
template <typename Configure>
RoutedPatch through(RoutedPatch p, EffectKind kind, Configure&& configure, float preGainDb = 0, AlgorithmRevision revision = 1)
{
    if (preGainDb > 0)
    {
        addEffect(p, EffectKind::stereo);
        p.effects[0].stereo = {preGainDb, 0, 1, StereoChannelMode::stereo};
    }
    const auto slot = p.layout.effectCount[masterChain];
    addEffect(p, kind);
    configure(p.effects[slot]);
    p.effects[slot].algorithmRevision = revision;
    p.compatibility.effects[slot][static_cast<size_t>(kind)] = revision;
    require(validateRoutedPatch(p), "effect patch rejected");
    return p;
}

constexpr int sideChannel = 2;
std::vector<double> samples(const juce::AudioBuffer<float>& x, int from, int count, int channel = 0)
{
    require(from >= 0 && count > 0 && from + count <= x.getNumSamples(), "analysis window outside the render");
    std::vector<double> v(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        v[size_t(i)] = channel == sideChannel ? .5 * (double(x.getSample(0, from + i)) - x.getSample(1, from + i))
                                              : double(x.getSample(channel, from + i));
    return v;
}
double peakOf(const std::vector<double>& x)
{
    double peak = 0;
    for (double v : x) peak = std::max(peak, std::abs(v));
    return peak;
}
// Linear layer level that gives the stimulus sine the requested peak.
float levelFor(double hz, double peak, double sampleRate)
{
    int key = 0;
    const auto unitPatch = stimulusPatch({hz, 1}, key);
    const auto unit = samples(renderStimulus(unitPatch, key, .4, sampleRate), int(.1 * sampleRate), int(.3 * sampleRate));
    const double level = peak / peakOf(unit);
    require(level > 0 && level <= 1, "stimulus cannot reach the requested peak");
    return float(level);
}

// Blackman-Harris power spectrum of 2^16 samples.
constexpr int lineOrder = 16, lineSize = 1 << lineOrder;
std::vector<double> powerSpectrum(const std::vector<double>& x)
{
    require(x.size() >= static_cast<size_t>(lineSize), "short spectrum window");
    juce::dsp::FFT fft(lineOrder);
    std::vector<float> data(2 * static_cast<size_t>(lineSize));
    for (int i = 0; i < lineSize; ++i)
    {
        const double t = double(i) / (lineSize - 1);
        const double w = .35875 - .48829 * std::cos(2 * pi * t) + .14128 * std::cos(4 * pi * t) - .01168 * std::cos(6 * pi * t);
        data[size_t(i)] = float(x[size_t(i)] * w);
    }
    fft.performFrequencyOnlyForwardTransform(data.data(), true);
    std::vector<double> power(static_cast<size_t>(lineSize / 2 + 1));
    for (size_t k = 0; k < power.size(); ++k) power[k] = double(data[k]) * data[k];
    return power;
}
double bandEnergy(const std::vector<double>& power, double low, double high, double sampleRate)
{
    const double bin = sampleRate / lineSize;
    double sum = 0;
    for (int k = std::max(4, int(std::ceil(low / bin))); k <= std::min(int(power.size()) - 1, int(std::floor(high / bin))); ++k)
        sum += power[size_t(k)];
    return sum;
}
double totalEnergy(const std::vector<double>& power)
{
    double sum = 0;
    for (size_t k = 4; k < power.size(); ++k) sum += power[k];
    return sum;
}
// Energy within a few bins of each harmonic of f0 below Nyquist is harmonic;
// the rest (aliases, spurious products, noise) is inharmonic.
struct Lines { double fundamental = 0, harmonic = 0, total = 0; };
Lines lines(const std::vector<double>& x, double f0, double sampleRate)
{
    const auto power = powerSpectrum(x);
    const double bin = sampleRate / lineSize;
    Lines result;
    result.total = totalEnergy(power);
    std::vector<bool> used(power.size(), false);
    for (int h = 1; h * f0 < sampleRate / 2; ++h)
    {
        const double centre = h * f0 / bin;
        const int spread = 6 + int(std::ceil(centre * .0005));
        double energy = 0;
        for (int k = std::max(4, int(centre) - spread); k <= std::min(int(power.size()) - 1, int(centre) + spread); ++k)
            if (!used[size_t(k)]) { energy += power[size_t(k)]; used[size_t(k)] = true; }
        result.harmonic += energy;
        if (h == 1) result.fundamental = energy;
    }
    return result;
}
double inharmonicDb(const Lines& l) { return 10 * std::log10(std::max(l.total - l.harmonic, 1e-30) / std::max(l.total, 1e-30)); }
double distortionDb(const Lines& l) { return 10 * std::log10(std::max(l.harmonic - l.fundamental, 1e-30) / std::max(l.fundamental, 1e-30)); }
double outsideDb(const std::vector<double>& x, double f0, double halfWidth, double sampleRate)
{
    const auto power = powerSpectrum(x);
    const double total = totalEnergy(power), inside = bandEnergy(power, f0 - halfWidth, f0 + halfWidth, sampleRate);
    return 10 * std::log10(std::max(total - inside, 1e-30) / std::max(total, 1e-30));
}

// Welch cross-spectral response of y to x: sum(Y conj X) / sum(|X|^2).
using Response = std::vector<std::complex<double>>;
constexpr int responseOrder = 13, responseSize = 1 << responseOrder;
Response response(const std::vector<double>& x, const std::vector<double>& y)
{
    require(x.size() == y.size() && x.size() >= static_cast<size_t>(responseSize), "response windows differ");
    juce::dsp::FFT fft(responseOrder);
    Response cross(static_cast<size_t>(responseSize / 2 + 1));
    std::vector<double> input(cross.size());
    std::vector<float> a(2 * static_cast<size_t>(responseSize)), b(a.size());
    for (size_t start = 0; start + static_cast<size_t>(responseSize) <= x.size(); start += static_cast<size_t>(responseSize / 2))
    {
        std::fill(a.begin(), a.end(), 0.0f); std::fill(b.begin(), b.end(), 0.0f);
        for (int i = 0; i < responseSize; ++i)
        {
            const double w = .5 - .5 * std::cos(2 * pi * i / responseSize);
            a[size_t(i)] = float(x[start + size_t(i)] * w); b[size_t(i)] = float(y[start + size_t(i)] * w);
        }
        fft.performRealOnlyForwardTransform(a.data(), true); fft.performRealOnlyForwardTransform(b.data(), true);
        for (size_t k = 0; k < cross.size(); ++k)
        {
            const std::complex<double> X(a[2 * k], a[2 * k + 1]), Y(b[2 * k], b[2 * k + 1]);
            cross[k] += Y * std::conj(X); input[k] += std::norm(X);
        }
    }
    for (size_t k = 0; k < cross.size(); ++k) cross[k] = input[k] > 0 ? cross[k] / input[k] : std::complex<double> {};
    return cross;
}
// Centre of the bins gainDb averages; predictions are evaluated there, since
// on steep slopes the nearest bin differs measurably from the nominal frequency.
double binHz(double hz, double sampleRate)
{
    const double bin = sampleRate / responseSize;
    return double(std::lround(hz / bin)) * bin;
}
double gainDb(const Response& h, double hz, double sampleRate)
{
    const double bin = sampleRate / responseSize;
    const int centre = int(std::lround(hz / bin)), spread = std::max(2, int(hz * .0025 / bin));
    double sum = 0; int count = 0;
    for (int k = std::max(1, centre - spread); k <= std::min(int(h.size()) - 1, centre + spread); ++k) { sum += std::norm(h[size_t(k)]); ++count; }
    return 10 * std::log10(sum / count);
}

// Largest over smallest instantaneous amplitude of the band f0 +- 2 kHz across
// the central 80% of 2^17 samples (analytic signal of a Tukey-tapered window).
constexpr int envelopeOrder = 17, envelopeSize = 1 << envelopeOrder;
double modulationDb(const std::vector<double>& x, double f0, double sampleRate)
{
    require(x.size() >= static_cast<size_t>(envelopeSize), "short modulation window");
    juce::dsp::FFT fft(envelopeOrder);
    std::vector<juce::dsp::Complex<float>> waveform(static_cast<size_t>(envelopeSize)), spectrum(waveform.size());
    const int taper = envelopeSize / 20;
    for (int i = 0; i < envelopeSize; ++i)
    {
        const int edge = std::min(i, envelopeSize - 1 - i);
        const double w = edge < taper ? .5 - .5 * std::cos(pi * edge / taper) : 1.0;
        waveform[size_t(i)] = {float(x[size_t(i)] * w), 0.0f};
    }
    fft.perform(waveform.data(), spectrum.data(), false);
    const double bin = sampleRate / envelopeSize;
    const int low = std::max(1, int((f0 - 2000) / bin)), high = std::min(envelopeSize / 2 - 1, int((f0 + 2000) / bin));
    for (int k = 0; k < envelopeSize; ++k) if (k < low || k > high) spectrum[size_t(k)] = {};
    fft.perform(spectrum.data(), waveform.data(), true);
    double largest = 0, smallest = std::numeric_limits<double>::max();
    for (int i = envelopeSize / 10; i < envelopeSize - envelopeSize / 10; ++i)
    {
        const double magnitude = std::abs(waveform[size_t(i)]);
        largest = std::max(largest, magnitude); smallest = std::min(smallest, magnitude);
    }
    return 20 * std::log10(largest / std::max(smallest, 1e-30));
}

// Kernel predictions and analog prototypes.
std::complex<double> unitDelay(double hz, double fs) { return std::polar(1.0, -2 * pi * hz / fs); }
std::complex<double> onePole(double cutoff, double hz, double fs)
{
    const double a = 1 - std::exp(-2 * pi * std::min(cutoff, fs * .49) / fs);
    return a / (1.0 - (1 - a) * unitDelay(hz, fs));
}
double toDb(double magnitude) { return 20 * std::log10(magnitude); }
double halfSampleAverageDb(double hz, double fs) { return toDb(std::abs(std::cos(pi * hz / fs))); }
double biquadDb(const std::array<double, 6>& c, double hz, double fs)
{
    const auto z = unitDelay(hz, fs);
    return toDb(std::abs((c[0] + c[1] * z + c[2] * z * z) / (c[3] + c[4] * z + c[5] * z * z)));
}
double analogBellDb(double hz, double centre, double q, double gain)
{
    const double A = std::pow(10.0, gain / 40);
    const std::complex<double> s(0, hz / centre);
    return toDb(std::abs((s * s + s * (A / q) + 1.0) / (s * s + s / (A * q) + 1.0)));
}
double analogHighShelfDb(double hz, double corner, double q, double gain)
{
    const double A = std::pow(10.0, gain / 40), r = std::sqrt(A) / q;
    const std::complex<double> s(0, hz / corner);
    return toDb(std::abs(A * (A * s * s + r * s + 1.0) / (s * s + r * s + A)));
}
double analogLowpassDb(double hz, double cutoff, double q)
{
    const std::complex<double> s(0, hz / cutoff);
    return toDb(std::abs(1.0 / (s * s + s / q + 1.0)));
}
// Bilinear (TPT) frequency warping around a prewarped corner.
double warped(double hz, double corner, double fs) { return corner * std::tan(pi * hz / fs) / std::tan(pi * corner / fs); }

void stimulusChecks()
{
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
        {
            int key = 0, noiseKey = 0;
            const auto sine = stimulusPatch({10000, .5f}, key), noise = stimulusPatch({1000, .5f, true}, noiseKey);
            const double seconds = .4 + double(envelopeSize) / sampleRate;
            const auto a = renderStimulus(sine, key, seconds, sampleRate, quality), b = renderStimulus(sine, key, seconds, sampleRate, quality);
            const auto c = renderStimulus(noise, noiseKey, 1, sampleRate, quality), d = renderStimulus(noise, noiseKey, 1, sampleRate, quality);
            const auto x = samples(a, int(.3 * sampleRate), envelopeSize);
            bool mono = true;
            for (int i = 0; i < a.getNumSamples(); ++i) mono = mono && a.getSample(0, i) == a.getSample(1, i);
            const double inharmonic = inharmonicDb(lines(x, 10000, sampleRate)), modulation = modulationDb(x, 10000, sampleRate);
            const auto where = std::string("stimulus rate=") + std::to_string(int(sampleRate)) + " quality=" + qualityName(quality);
            std::cout << "EFFECT " << where << " sine_peak_level_0.5=" << fixed(peakOf(x), 4) << " sine_inharmonic_db=" << fixed(inharmonic, 1)
                      << " sine_modulation_db=" << fixed(modulation, 4) << " mono=" << mono << '\n';
            check(hashOf(a) == hashOf(b) && hashOf(c) == hashOf(d), where + " sine and noise renders repeat bit-identically");
            check(inharmonic < -80 && modulation < .01 && mono, where + " sine is clean, steady and mono");
        }
}

// Neutral settings must return the stimulus, which also confirms that the dry
// render is each effect's exact input in both qualities. The drive is the
// exception: its dry path is averaged even at mix 0, and revision 2's also
// passes through its oversampling filters (see drive_response).
void neutralProbes()
{
    struct Case { const char* name; EffectKind kind; void (*configure)(EffectSettings&); AlgorithmRevision revision = 1; };
    const Case cases[] {
        {"drive_mix0", EffectKind::drive, [](EffectSettings& e) { e.drive.mix = 0; }},
        {"drive2_mix0", EffectKind::drive, [](EffectSettings& e) { e.drive.mix = 0; }, 2},
        {"delay_mix0", EffectKind::delay, [](EffectSettings& e) { e.delay.mix = 0; }},
        {"compressor_mix0", EffectKind::compressor, [](EffectSettings& e) { e.compressor.mix = 0; }},
        {"eq_flat", EffectKind::eq, [](EffectSettings& e) { e.eq = {}; }},
        {"filter_mix0", EffectKind::filter, [](EffectSettings& e) { e.filter.mix = 0; }},
        {"chorus_mix0", EffectKind::chorus, [](EffectSettings& e) { e.chorus.mix = 0; }},
        {"phaser_mix0", EffectKind::phaser, [](EffectSettings& e) { e.phaser.mix = 0; }},
        {"flanger_mix0", EffectKind::flanger, [](EffectSettings& e) { e.flanger.mix = 0; }},
        {"dimension_mix0", EffectKind::dimension, [](EffectSettings& e) { e.dimension.mix = 0; }},
        {"frequency_shift_mix0", EffectKind::frequencyShift, [](EffectSettings& e) { e.frequencyShift.mix = 0; }},
        {"frequency_split_flat", EffectKind::frequencySplit, [](EffectSettings& e) { e.frequencySplit = {}; }},
        {"stereo_unity", EffectKind::stereo, [](EffectSettings& e) { e.stereo = {}; }}};
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
        {
            int key = 0;
            const auto noise = stimulusPatch({1000, .02f, true}, key);
            const auto dry = renderStimulus(noise, key, 1, sampleRate, quality);
            const int from = int(.25 * sampleRate);
            for (const auto& c : cases)
            {
                const auto wet = renderStimulus(through(noise, c.kind, c.configure, 0, c.revision), key, 1, sampleRate, quality);
                double peak = 0, worst = 0;
                for (int ch = 0; ch < 2; ++ch)
                    for (int i = from; i < dry.getNumSamples(); ++i)
                    {
                        peak = std::max(peak, std::abs(double(dry.getSample(ch, i))));
                        worst = std::max(worst, std::abs(double(wet.getSample(ch, i)) - dry.getSample(ch, i)));
                    }
                const auto where = std::string("neutral ") + c.name + " rate=" + std::to_string(int(sampleRate)) + " quality=" + qualityName(quality);
                std::cout << "EFFECT " << where << " deviation_db=" << (worst == 0 ? std::string("exact") : fixed(toDb(worst / peak), 1)) << '\n';
                if (c.kind != EffectKind::drive) check(worst <= 1e-6 * peak, where + " returns its input");
            }
        }
}

// Small-signal response of the drive. In revision 1 the dry path and
// first-order ADAA both average adjacent samples, |cos(pi f / fs)|; revision 2
// does that at four times the rate and should meet the analog prototype of its
// gain and tone one-pole within 0.5 dB to 15 kHz and 1 dB to 18 kHz.
void driveResponse()
{
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
        {
            int key = 0;
            const auto noise = stimulusPatch({1000, .02f, true}, key);
            const double fs = internalRate(sampleRate, quality);
            const int from = int(.25 * sampleRate), count = int(2.5 * sampleRate);
            const auto dry = samples(renderStimulus(noise, key, 3, sampleRate, quality), from, count);
            struct Case { const char* name; DriveSettings settings; };
            const Case cases[] {{"mix0", {0, DriveShape::tanh, 20000, 0}}, {"mix1_tone20k", {0, DriveShape::tanh, 20000, 1}},
                                {"default", {6, DriveShape::tanh, 12000, .5f}}, {"cinder_coil", {6, DriveShape::tanh, 6500, .25f}}};
            for (AlgorithmRevision revision : {AlgorithmRevision {1}, AlgorithmRevision {2}})
                for (const auto& c : cases)
                {
                    const auto wet = samples(renderStimulus(through(noise, EffectKind::drive, [&](EffectSettings& e) { e.drive = c.settings; }, 0, revision),
                                                            key, 3, sampleRate, quality), from, count);
                    const auto h = response(dry, wet);
                    const double gain = std::pow(10.0, c.settings.gainDb / 20);
                    // Untouched dry plus the small-signal tanh gain through the tone one-pole.
                    const auto intended = [&](double hz) {
                        return toDb(std::abs((1.0 - c.settings.mix) + c.settings.mix * gain * onePole(c.settings.toneHz, binHz(hz, sampleRate), fs)));
                    };
                    const auto analog = [&](double hz) {
                        return toDb(std::abs((1.0 - c.settings.mix) + c.settings.mix * gain / std::complex<double>(1, binHz(hz, sampleRate) / c.settings.toneHz)));
                    };
                    const auto predicted = [&](double hz) {
                        return revision == 1 ? intended(hz) + halfSampleAverageDb(binHz(hz, sampleRate), fs) : analog(hz);
                    };
                    const double reference = gainDb(h, 1000, sampleRate);
                    double worst = 0, worstTreble = 0;
                    std::cout << "EFFECT drive_response revision=" << int(revision) << ' ' << c.name << " rate=" << int(sampleRate) << " quality=" << qualityName(quality);
                    for (double hz : {2000.0, 5000.0, 10000.0, 15000.0, 18000.0})
                    {
                        const double measured = gainDb(h, hz, sampleRate) - reference, expected = predicted(hz) - predicted(1000);
                        double& band = hz <= 15000 ? worst : worstTreble;
                        band = std::max(band, std::abs(measured - expected));
                        std::cout << " f" << int(hz) << "=" << fixed(measured) << "/" << fixed(expected) << "/analog" << fixed(analog(hz) - analog(1000));
                    }
                    std::cout << '\n';
                    const auto where = std::string("drive_response revision=") + std::to_string(int(revision)) + ' ' + c.name + " rate="
                                       + std::to_string(int(sampleRate)) + " quality=" + qualityName(quality);
                    if (revision == 1)
                        check(std::max(worst, worstTreble) <= (quality == RenderQuality::higher ? .1 : .05),
                              where + " matches the half-sample-average prediction (worst " + fixed(std::max(worst, worstTreble), 3) + " dB)");
                    else
                        check(worst <= .5 && worstTreble <= (c.settings.mix == 0 ? .5 : 1.),
                              where + " meets the analog prototype (worst " + fixed(worst, 3) + " dB to 15 kHz, " + fixed(worstTreble, 3) + " dB at 18 kHz)");
                }
        }
}

// Saturated sines at engine levels. SynthEngine sums layers at 1/32 headroom
// and the output gain (default +30 dB) restores loudness after the master
// chain, so one full-level voice reaches the effects at about a 0.022 peak;
// the +12 dB stereo-utility lift stands for about four voices in phase.
// Harmonic distortion shows how hard each setting saturates; inharmonic
// energy is aliasing (drive revision 1: first-order ADAA; revision 2: the same
// at four times the rate with its knee at 1/16; filter FX: direct tanh).
void saturationProbes()
{
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
            for (double f0 : {1760.0, 3520.0, 7040.0})
                for (float lift : {0.0f, 12.0f})
                {
                    int key = 0;
                    const auto sine = stimulusPatch({f0, levelFor(f0, .02, sampleRate)}, key);
                    const int from = int(.25 * sampleRate);
                    const double seconds = .3 + double(lineSize) / sampleRate;
                    const auto renderWith = [&](const RoutedPatch& p) { return samples(renderStimulus(p, key, seconds, sampleRate, quality), from, lineSize); };
                    const auto input = renderWith(lift > 0 ? through(sine, EffectKind::stereo, [&](EffectSettings& e) { e.stereo = {lift, 0, 1, StereoChannelMode::stereo}; }) : sine);
                    std::cout << "EFFECT saturation rate=" << int(sampleRate) << " quality=" << qualityName(quality) << " f0=" << int(f0)
                              << " input_peak=" << fixed(peakOf(input), 4) << " input_inharmonic_db=" << fixed(inharmonicDb(lines(input, f0, sampleRate)), 1);
                    for (float drive : {6.0f, 12.0f, 24.0f})
                    {
                        const auto driveAt = [&](AlgorithmRevision revision) {
                            return lines(renderWith(through(sine, EffectKind::drive, [&](EffectSettings& e) { e.drive = {drive, DriveShape::tanh, 20000, 1}; }, lift, revision)), f0, sampleRate);
                        };
                        const auto driven = driveAt(1), modern = driveAt(2);
                        const auto filtered = lines(renderWith(through(sine, EffectKind::filter,
                            [&](EffectSettings& e) { e.filter = {FilterFxMode::lowpass, 20000, .70710678f, drive, 1}; }, lift)), f0, sampleRate);
                        std::cout << " drive" << int(drive) << "_thd_db=" << fixed(distortionDb(driven), 1) << " drive" << int(drive) << "_inharmonic_db=" << fixed(inharmonicDb(driven), 1)
                                  << " drive2_" << int(drive) << "_thd_db=" << fixed(distortionDb(modern), 1) << " drive2_" << int(drive) << "_inharmonic_db=" << fixed(inharmonicDb(modern), 1)
                                  << " filter" << int(drive) << "_thd_db=" << fixed(distortionDb(filtered), 1) << " filter" << int(drive) << "_inharmonic_db=" << fixed(inharmonicDb(filtered), 1);
                    }
                    std::cout << '\n';
                }
}

// Filter FX lowpass (TPT state variable): bilinear prediction and analog prototype.
void filterResponse()
{
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
        {
            int key = 0;
            const auto noise = stimulusPatch({1000, .02f, true}, key);
            const double fs = internalRate(sampleRate, quality);
            const int from = int(.25 * sampleRate), count = int(2.5 * sampleRate);
            const auto dry = samples(renderStimulus(noise, key, 3, sampleRate, quality), from, count);
            for (float cutoff : {4000.0f, 10000.0f})
            {
                const auto wet = samples(renderStimulus(through(noise, EffectKind::filter,
                    [&](EffectSettings& e) { e.filter = {FilterFxMode::lowpass, cutoff, .70710678f, 0, 1}; }), key, 3, sampleRate, quality), from, count);
                const auto h = response(dry, wet);
                const double corner = std::min(double(cutoff), .45 * fs);
                double worst = 0;
                std::cout << "EFFECT filter_response lowpass" << int(cutoff) << " rate=" << int(sampleRate) << " quality=" << qualityName(quality);
                for (double hz : {1000.0, 2000.0, 5000.0, 10000.0, 15000.0, 18000.0})
                {
                    const double at = binHz(hz, sampleRate), measured = gainDb(h, hz, sampleRate), digital = analogLowpassDb(warped(at, corner, fs), corner, .70710678);
                    worst = std::max(worst, std::abs(measured - digital));
                    std::cout << " f" << int(hz) << "=" << fixed(measured) << "/" << fixed(digital) << "/analog" << fixed(analogLowpassDb(hz, cutoff, .70710678));
                }
                std::cout << '\n';
                check(worst <= (quality == RenderQuality::higher ? .1 : .05), "filter_response lowpass" + std::to_string(int(cutoff)) + " rate="
                      + std::to_string(int(sampleRate)) + " quality=" + qualityName(quality) + " matches the bilinear prediction (worst " + fixed(worst, 3) + " dB)");
            }
        }
}

// Delay repeat at a half-sample length against the whole-sample length: the
// linear-interpolated read adds |cos(pi f / fs)| per pass.
void delayResponse()
{
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
        {
            int key = 0;
            const auto noise = stimulusPatch({1000, .02f, true}, key);
            const double fs = internalRate(sampleRate, quality), whole = std::round(.25 * fs);
            const int from = int(.25 * sampleRate), count = int(2.4 * sampleRate);
            const auto dry = samples(renderStimulus(noise, key, 3.2, sampleRate, quality), from, count);
            std::array<Response, 2> h;
            for (size_t i = 0; i < h.size(); ++i)
            {
                const float seconds = float((whole + .5 * double(i)) / fs);
                const auto wet = renderStimulus(through(noise, EffectKind::delay, [&](EffectSettings& e) { e.delay = {false, seconds, .5f, 0, 0, 20000, 1}; }),
                                                key, 3.2, sampleRate, quality);
                h[i] = response(dry, samples(wet, from + int(std::floor(seconds * sampleRate)), count));
            }
            double worst = 0;
            std::cout << "EFFECT delay_repeat half_sample_vs_whole rate=" << int(sampleRate) << " quality=" << qualityName(quality);
            for (double hz : {2000.0, 5000.0, 10000.0, 15000.0, 18000.0})
            {
                const double measured = gainDb(h[1], hz, sampleRate) - gainDb(h[0], hz, sampleRate), expected = halfSampleAverageDb(binHz(hz, sampleRate), fs);
                worst = std::max(worst, std::abs(measured - expected));
                std::cout << " f" << int(hz) << "=" << fixed(measured) << "/" << fixed(expected);
            }
            std::cout << '\n';
            check(worst <= .05, "delay_repeat rate=" + std::to_string(int(sampleRate)) + " quality=" + qualityName(quality)
                  + " matches the linear-interpolation prediction (worst " + fixed(worst, 3) + " dB)");
        }
}

// Compressor at realtime 48 kHz, where the output is exactly gain times input:
// the per-sample gain recovers the detector envelope through the hard-knee law,
// which identifies the ballistics coefficients. Thresholds sit 12 dB below the
// stimulus peak; the gain law is scale invariant, so only ratio and timing matter.
void compressorProbes()
{
    constexpr double sampleRate = 48000;
    const float twoPi = float(2 * pi);
    struct Case { const char* name; float ratio, attackMs, releaseMs; };
    const Case cases[] {{"anchor_press_timing", 3, 15, 150}, {"anchor_press_timing_x2pi", 3, 15 * twoPi, 150 * twoPi},
                        {"default_timing", 4, 10, 100}, {"default_timing_x2pi", 4, 10 * twoPi, 100 * twoPi}};
    const double peak = .02, thresholdDb = toDb(peak) - 12, threshold = std::pow(10.0, thresholdDb / 20);
    for (const auto& c : cases)
    {
        const CompressorSettings settings {float(thresholdDb), c.ratio, c.attackMs, c.releaseMs, 0, 1};
        const auto compress = [&](EffectSettings& e) { e.compressor = settings; };
        {
            int key = 0;
            const auto sine = stimulusPatch({1000, levelFor(1000, peak, sampleRate)}, key);
            const int count = int(1.4 * sampleRate);
            const auto x = samples(renderStimulus(sine, key, 1.5, sampleRate), 0, count);
            const auto y = samples(renderStimulus(through(sine, EffectKind::compressor, compress), key, 1.5, sampleRate), 0, count);
            const double exponent = c.ratio / (1 - c.ratio);
            const auto envelope = [&](size_t n) {
                const double g = y[n] / x[n];
                return g < 1 - 1e-6 ? threshold * std::pow(g, exponent) : -1.0;
            };
            std::vector<double> attack, release;
            for (size_t n = 1; n < x.size(); ++n)
            {
                const double input = std::abs(x[n]);
                if (std::abs(x[n - 1]) < .3 * peak || input < .3 * peak) continue;
                const double before = envelope(n - 1), after = envelope(n);
                if (before < 0 || after < 0 || std::abs(before - input) < .05 * before) continue;
                (input > before ? attack : release).push_back((after - input) / (before - input));
            }
            const auto tau = [&](std::vector<double>& estimates) {
                if (estimates.empty()) return -1.0;
                std::nth_element(estimates.begin(), estimates.begin() + std::ptrdiff_t(estimates.size() / 2), estimates.end());
                return -1000 / (sampleRate * std::log(estimates[estimates.size() / 2]));
            };
            const double attackTau = tau(attack), releaseTau = tau(release);
            const auto reduction = [&](double seconds) {
                const int centre = int(seconds * sampleRate);
                size_t best = size_t(centre);
                for (int n = std::max(0, centre - 24); n < std::min(count, centre + 24); ++n)
                    if (std::abs(x[size_t(n)]) > std::abs(x[best])) best = size_t(n);
                return -toDb(y[best] / x[best]);
            };
            std::cout << "EFFECT compressor_ballistics " << c.name << " label_attack_ms=" << fixed(c.attackMs) << " label_release_ms=" << fixed(c.releaseMs)
                      << " attack_tau_ms=" << fixed(attackTau, 3) << "/" << fixed(c.attackMs / (2 * pi), 3) << " (" << attack.size() << " samples)"
                      << " release_tau_ms=" << fixed(releaseTau, 3) << "/" << fixed(c.releaseMs / (2 * pi), 3) << " (" << release.size() << " samples)"
                      << " onset_gr_db";
            for (double ms : {1.0, 3.0, 10.0, 30.0, 100.0, 1000.0}) std::cout << " t" << fixed(ms, 0) << "ms=" << fixed(reduction(ms * .001));
            std::cout << '\n';
            check(attackTau > 0 && releaseTau > 0 && std::abs(attackTau / (c.attackMs / (2 * pi)) - 1) < .03
                      && std::abs(releaseTau / (c.releaseMs / (2 * pi)) - 1) < .03,
                  std::string("compressor_ballistics ") + c.name + " time constants are the labels divided by 2 pi");
        }
        for (double f0 : {55.0, 110.0})
        {
            int key = 0;
            const auto sine = stimulusPatch({f0, levelFor(f0, peak, sampleRate)}, key);
            const int from = int(1.0 * sampleRate);
            const double seconds = 1.1 + double(lineSize) / sampleRate;
            const auto x = samples(renderStimulus(sine, key, seconds, sampleRate), from, lineSize);
            const auto y = samples(renderStimulus(through(sine, EffectKind::compressor, compress), key, seconds, sampleRate), from, lineSize);
            const auto in = lines(x, f0, sampleRate), out = lines(y, f0, sampleRate);
            std::cout << "EFFECT compressor_low_frequency " << c.name << " f0=" << int(f0) << " input_thd_db=" << fixed(distortionDb(in), 1)
                      << " output_thd_db=" << fixed(distortionDb(out), 1) << " gain_reduction_db=" << fixed(-10 * std::log10(out.fundamental / in.fundamental)) << '\n';
        }
    }
}

// EQ bands (JUCE RBJ biquads at the internal rate): measured, digital and analog prototype.
void eqResponse()
{
    struct Case { const char* name; bool bell; EqBandSettings band; };
    const Case cases[] {{"bell_10k_+12_q1", true, {10000, 12, 1}}, {"bell_3k_+12_q2", true, {3000, 12, 2}},
                        {"high_shelf_8k_+12_q0.71", false, {8000, 12, .70710678f}}};
    using Factory = juce::dsp::IIR::ArrayCoefficients<double>;
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
        {
            int key = 0;
            const auto noise = stimulusPatch({1000, .02f, true}, key);
            const double fs = internalRate(sampleRate, quality);
            const int from = int(.25 * sampleRate), count = int(2.5 * sampleRate);
            const auto dry = samples(renderStimulus(noise, key, 3, sampleRate, quality), from, count);
            for (const auto& c : cases)
            {
                const auto wet = samples(renderStimulus(through(noise, EffectKind::eq, [&](EffectSettings& e) { (c.bell ? e.eq.bell : e.eq.highShelf) = c.band; }),
                                                        key, 3, sampleRate, quality), from, count);
                const auto h = response(dry, wet);
                const double corner = std::min(double(c.band.frequencyHz), .45 * fs), gain = std::pow(10.0, c.band.gainDb / 20.0);
                const auto coefficients = c.bell ? Factory::makePeakFilter(fs, corner, c.band.q, gain) : Factory::makeHighShelf(fs, corner, c.band.q, gain);
                double worst = 0;
                std::cout << "EFFECT eq_response " << c.name << " rate=" << int(sampleRate) << " quality=" << qualityName(quality);
                for (double hz : {1000.0, 2500.0, 5000.0, 7500.0, 10000.0, 12500.0, 15000.0, 18000.0})
                {
                    const double measured = gainDb(h, hz, sampleRate), digital = biquadDb(coefficients, binHz(hz, sampleRate), fs);
                    const double analog = c.bell ? analogBellDb(hz, c.band.frequencyHz, c.band.q, c.band.gainDb) : analogHighShelfDb(hz, c.band.frequencyHz, c.band.q, c.band.gainDb);
                    worst = std::max(worst, std::abs(measured - digital));
                    std::cout << " f" << int(hz) << "=" << fixed(measured) << "/" << fixed(digital) << "/analog" << fixed(analog);
                }
                std::cout << '\n';
                check(worst <= (quality == RenderQuality::higher ? .1 : .05), std::string("eq_response ") + c.name + " rate=" + std::to_string(int(sampleRate))
                      + " quality=" + qualityName(quality) + " matches its biquad (worst " + fixed(worst, 3) + " dB)");
            }
        }
}

// Modulated delays and allpass motion on steady sines, feedback 0 and wet
// only: one ideal moving tap keeps a constant amplitude, while linear
// interpolation dips by |cos(pi f / fs)| at half-sample positions. Dimension
// runs at its defaults; its side signal should stay within the slow sweep's
// sidebands, so energy outside f0 +- 150 Hz is interpolation product.
void modulationProbes()
{
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
            for (double f0 : {1000.0, 5000.0, 10000.0, 15000.0})
            {
                int key = 0;
                const auto sine = stimulusPatch({f0, .5f}, key);
                const int from = int(.3 * sampleRate);
                const double fs = internalRate(sampleRate, quality), seconds = .4 + double(envelopeSize) / sampleRate;
                const auto renderWith = [&](const RoutedPatch& p) { return renderStimulus(p, key, seconds, sampleRate, quality); };
                const auto dry = renderWith(sine);
                const auto chorus = renderWith(through(sine, EffectKind::chorus, [](EffectSettings& e) { e.chorus = {.35f, .25f, 18, 0, 1}; }));
                const auto flanger = renderWith(through(sine, EffectKind::flanger, [](EffectSettings& e) { e.flanger = {.25f, .6f, 2, 0, 1}; }));
                const auto phaser = renderWith(through(sine, EffectKind::phaser, [](EffectSettings& e) { e.phaser = {.35f, .6f, 1300, 0, 1}; }));
                const auto dimension = renderWith(through(sine, EffectKind::dimension, [](EffectSettings& e) { e.dimension = {.6f, .35f, 1, .5f}; }));
                const auto depth = [&](const juce::AudioBuffer<float>& b) { return modulationDb(samples(b, from, envelopeSize), f0, sampleRate); };
                const double expected = -halfSampleAverageDb(f0, fs), chorusDepth = depth(chorus), flangerDepth = depth(flanger);
                std::cout << "EFFECT modulation rate=" << int(sampleRate) << " quality=" << qualityName(quality) << " f0=" << int(f0)
                          << " input_am_db=" << fixed(depth(dry), 4) << " chorus_am_db=" << fixed(chorusDepth) << "/" << fixed(expected)
                          << " flanger_am_db=" << fixed(flangerDepth) << "/" << fixed(expected) << " phaser_am_db=" << fixed(depth(phaser), 3)
                          << " phaser_outside_100hz_db=" << fixed(outsideDb(samples(phaser, from, lineSize), f0, 100, sampleRate), 1)
                          << " dimension_side_outside_150hz_db=" << fixed(outsideDb(samples(dimension, from, lineSize, sideChannel), f0, 150, sampleRate), 1)
                          << " input_outside_100hz_db=" << fixed(outsideDb(samples(dry, from, lineSize), f0, 100, sampleRate), 1) << '\n';
                check(std::abs(chorusDepth - expected) <= .15 && std::abs(flangerDepth - expected) <= .15,
                      "modulation rate=" + std::to_string(int(sampleRate)) + " quality=" + qualityName(quality) + " f0=" + std::to_string(int(f0))
                          + " chorus and flanger dips match linear interpolation");
            }
}

// Single-sideband shift of +50 Hz: wanted level, mirror image and carrier.
void shifterProbe()
{
    for (double sampleRate : auditRates)
        for (auto quality : auditQualities)
            for (double f0 : {100.0, 1000.0, 5000.0, 10000.0, 15000.0})
            {
                int key = 0;
                const auto sine = stimulusPatch({f0, .5f}, key);
                const int from = int(.3 * sampleRate);
                const double seconds = .4 + double(lineSize) / sampleRate;
                const auto dry = powerSpectrum(samples(renderStimulus(sine, key, seconds, sampleRate, quality), from, lineSize));
                const auto wet = powerSpectrum(samples(renderStimulus(through(sine, EffectKind::frequencyShift,
                    [](EffectSettings& e) { e.frequencyShift = {50, 0, 1}; }), key, seconds, sampleRate, quality), from, lineSize));
                const auto at = [&](const std::vector<double>& power, double hz) { return bandEnergy(power, hz - 5, hz + 5, sampleRate); };
                const double wanted = at(wet, f0 + 50);
                const double wantedDb = 10 * std::log10(wanted / at(dry, f0));
                std::cout << "EFFECT frequency_shift rate=" << int(sampleRate) << " quality=" << qualityName(quality) << " f0=" << int(f0)
                          << " wanted_db=" << fixed(wantedDb) << " image_db=" << fixed(10 * std::log10(std::max(at(wet, f0 - 50), 1e-30) / wanted), 1)
                          << " carrier_db=" << fixed(10 * std::log10(std::max(at(wet, f0), 1e-30) / wanted), 1) << '\n';
                check(std::abs(wantedDb) < .5, "frequency_shift rate=" + std::to_string(int(sampleRate)) + " quality=" + qualityName(quality)
                      + " f0=" + std::to_string(int(f0)) + " keeps the shifted level");
            }
}

// Band isolation of the frequency splitter with the low band muted.
void splitterProbe()
{
    constexpr double sampleRate = 48000;
    int key = 0;
    const auto noise = stimulusPatch({1000, .02f, true}, key);
    const int from = int(.25 * sampleRate), count = int(2.5 * sampleRate);
    const auto dry = samples(renderStimulus(noise, key, 3, sampleRate), from, count);
    const auto wet = samples(renderStimulus(through(noise, EffectKind::frequencySplit,
        [](EffectSettings& e) { e.frequencySplit = {1000, 0, true, 0, 0, false, 0, 1}; }), key, 3, sampleRate), from, count);
    const auto h = response(dry, wet);
    double worst = 0;
    std::cout << "EFFECT frequency_split low_muted_1k rate=48000";
    for (double hz : {125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0})
    {
        const std::complex<double> s(0, warped(binHz(hz, sampleRate), 1000, sampleRate) / 1000);
        const double measured = gainDb(h, hz, sampleRate), onePoleHighpass = toDb(std::abs(s / (1.0 + s)));
        const double ratio = std::pow(binHz(hz, sampleRate) / 1000, 4), linkwitzRiley = toDb(ratio / (1 + ratio));
        worst = std::max(worst, std::abs(measured - onePoleHighpass));
        std::cout << " f" << int(hz) << "=" << fixed(measured) << "/" << fixed(onePoleHighpass) << "/lr4" << fixed(linkwitzRiley);
    }
    std::cout << '\n';
    check(worst <= .05, "frequency_split matches its complementary one-pole (worst " + fixed(worst, 3) + " dB)");
}

void effectAudit()
{
    stimulusChecks();
    neutralProbes();
    driveResponse();
    saturationProbes();
    filterResponse();
    delayResponse();
    compressorProbes();
    eqResponse();
    modulationProbes();
    shifterProbe();
    splitterProbe();
    std::cout << (failedChecks == 0 ? "SUCCESS" : "FAILED") << " effect audit with " << failedChecks << " failed checks\n";
}
}

int main(int argc, char* argv[])
{
    try
    {
        juce::StringArray args;
        for (int i = 1; i < argc; ++i) args.add(argv[i]);
        require(args.size() >= 2 && args[0] == "--output" && juce::File::isAbsolutePath(args[1]),
                "usage: RackSynthAudition --output <fresh absolute directory> [--revisions 1,2] [--probes-only | --cpu [--cpu-rate HZ] [--cpu-block FRAMES] [--cpu-unison] | --effects]");
        std::vector<AlgorithmRevision> revisions {1};
        bool probesOnly = false, cpuOnly = false, effectsOnly = false;
        CpuLoad load;
        for (int i = 2; i < args.size(); ++i)
        {
            if (args[i] == "--revisions" && i + 1 < args.size())
            {
                revisions.clear();
                for (const auto& token : juce::StringArray::fromTokens(args[++i], ",", ""))
                    revisions.push_back(static_cast<AlgorithmRevision>(token.getIntValue()));
            }
            else if (args[i] == "--probes-only") probesOnly = true;
            else if (args[i] == "--cpu") cpuOnly = true;
            else if (args[i] == "--cpu-rate" && i + 1 < args.size()) load.sampleRate = args[++i].getDoubleValue();
            else if (args[i] == "--cpu-block" && i + 1 < args.size()) load.blockSize = args[++i].getIntValue();
            else if (args[i] == "--cpu-unison") load.unison = true;
            else if (args[i] == "--effects") effectsOnly = true;
            else require(false, "unknown argument " + args[i].toStdString());
        }
        require(load.sampleRate >= 8000 && load.sampleRate <= 384000 && load.blockSize >= 16 && load.blockSize <= 8192, "cpu rate or block out of range");
        if (cpuOnly) { for (auto revision : revisions) cpuProbe(revision, load); return 0; }
        if (effectsOnly) { effectAudit(); return failedChecks == 0 ? 0 : 1; }
        for (auto revision : revisions) fingerprints(revision);
        for (auto revision : revisions)
            for (auto engine : {EngineKind::analog, EngineKind::wavetable})
            {
                auto p = probePatch(engine, revision);
                if (!validateSynthPatch(p)) { std::cout << "SKIP revision " << int(revision) << " is not supported by this engine build\n"; continue; }
                bandwidthProbe(engine, revision); envelopeProbe(engine, revision); onsetProbe(engine, revision); resonanceProbe(engine, revision);
            }
        if (probesOnly) return 0;
        const juce::File output(args[1]);
        require(!output.exists(), "output exists; choose a fresh directory");
        require(output.createDirectory().wasOk(), "cannot create output");
        const auto raw = output.getChildFile("raw"), listen = output.getChildFile("listen"), images = output.getChildFile("spectrograms");
        for (const auto& d : {raw, listen, images}) require(d.createDirectory().wasOk(), "cannot create subdirectory");
        constexpr double targetLufs = -18;
        for (const auto& brief : briefs())
            for (bool wet : {false, true})
            {
                std::vector<std::pair<std::string, juce::Image>> panels;
                for (auto revision : revisions)
                {
                    const auto patch = patchFor(brief, revision, wet);
                    if (!validateRoutedPatch(patch)) continue;
                    const auto audio = render(patch, brief.notes, brief.seconds);
                    const auto m = measure(audio);
                    const auto stem = brief.name + (wet ? "-wet" : "-dry") + "-rev" + std::to_string(int(revision));
                    writeWav(raw.getChildFile(stem + ".wav"), audio, true);
                    double gain = std::pow(10.0, (targetLufs - m.lufs) / 20);
                    if (m.peak * gain > .89) gain = .89 / m.peak;
                    auto copy = audio; copy.applyGain(float(gain));
                    writeWav(listen.getChildFile(stem + ".wav"), copy, false);
                    panels.push_back({"revision " + std::to_string(int(revision)) + (revision == 1 ? " (legacy kernel)" : " (modern kernel)"), spectrogram(copy, 300)});
                    std::cout << "RENDER " << stem << std::fixed << std::setprecision(4) << " peak=" << m.peak << " rms=" << m.rms
                              << std::setprecision(1) << " lufs=" << m.lufs << " centroid_hz=" << std::setprecision(0) << m.centroid
                              << std::setprecision(4) << " above5k=" << m.above5k << " above10k=" << m.above10k << " above15k=" << m.above15k
                              << std::setprecision(2) << " listen_gain_db=" << 20 * std::log10(gain)
                              << " hash=" << std::hex << m.hash << std::dec << '\n';
                }
                if (!panels.empty()) writePng(images.getChildFile(brief.name + (wet ? "-wet" : "-dry") + ".png"),
                                              comparison(brief.name + (wet ? " (restrained drive + reverb)" : " (dry)"),
                                                         "loudness matched, 30 Hz-24 kHz log, 110 dB range", panels));
            }
        std::cout << "SUCCESS audition " << output.getFullPathName() << '\n';
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAILED " << e.what() << '\n';
        return 1;
    }
}

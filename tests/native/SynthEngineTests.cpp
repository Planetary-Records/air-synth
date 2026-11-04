#include "SynthEngine.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace
{
using namespace airack;
constexpr double pi = std::numbers::pi;
int checks = 0, failures = 0, sections = 0;
void check(bool good, const char* what, int line)
{
    ++checks;
    if (!good) { ++failures; std::cout << "FAIL line " << line << ": " << what << '\n'; }
}
#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)
void section(const char* name) { ++sections; std::cout << "SECTION " << name << '\n'; }
using Event = std::pair<int, juce::MidiMessage>;
juce::MidiMessage on(int note = 69, int channel = 1, float velocity = 1.0f)
{ return juce::MidiMessage::noteOn(channel, note, velocity); }
juce::MidiMessage off(int note = 69, int channel = 1)
{ return juce::MidiMessage::noteOff(channel, note); }
SynthPatch flat(EngineKind kind = EngineKind::analog)
{
    SynthPatch p;
    auto& l = p.layers[0];
    l.engine = kind;
    l.oscillators[0].waveform = Waveform::sine;
    l.oscillators[0].level = 1;
    l.oscillators[1].level = 0;
    l.ampEnvelope = { 0, 0, 0, 1 };
    l.filterEnvelope = { 0, 0, 0, 1 };
    l.cutoffHz = 20000;
    l.resonance = 0;
    l.level = 1;
    p.masterLevel = 1;
    return p;
}
std::vector<float> render(SynthEngine& engine, int count, const std::vector<Event>& events = {}, int partition = 256, int channel = 0)
{
    std::vector<float> result(static_cast<size_t>(count));
    for (int start = 0; start < count; start += partition)
    {
        const int size = std::min(partition, count - start);
        juce::AudioBuffer<float> audio(2, size);
        juce::MidiBuffer midi;
        for (const auto& e : events)
            if (e.first >= start && e.first < start + size)
                midi.addEvent(e.second, e.first - start);
        engine.render(audio, midi);
        std::copy_n(audio.getReadPointer(channel), size, result.data() + start);
    }
    return result;
}
double energy(const std::vector<float>& x)
{
    double total = 0;
    for (auto v : x) total += static_cast<double>(v) * v;
    return total / std::max(size_t(1), x.size());
}
double difference(const std::vector<float>& a, const std::vector<float>& b)
{
    double largest = 0;
    for (size_t i = 0; i < a.size(); ++i) largest = std::max(largest, std::abs(double(a[i]) - b[i]));
    return largest;
}
double binEnergy(const std::vector<float>& x, double cycles)
{
    double re = 0, im = 0;
    for (size_t i = 0; i < x.size(); ++i)
    {
        const auto angle = 2 * pi * cycles * double(i) / double(x.size());
        re += x[i] * std::cos(angle); im += x[i] * std::sin(angle);
    }
    return 2 * (re * re + im * im) / double(x.size() * x.size());
}
double aliasFraction(const std::vector<float>& x)
{
    double mean = 0;
    for (auto v : x) mean += v;
    mean /= double(x.size());
    const auto total = energy(x) - mean * mean;
    // 7031.25 Hz at 48 kHz: exactly bin 600/4096; only 3 harmonics below Nyquist.
    return std::max(0.0, total - binEnergy(x, 600) - binEnergy(x, 1200) - binEnergy(x, 1800)) / std::max(total, 1e-30);
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking = false;
thread_local int allocations = 0;
thread_local int frees = 0;
int __cdecl allocationHook(int kind, void*, size_t, int, long, const unsigned char*, int)
{ if (tracking && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC)) ++allocations; if (tracking && kind == _HOOK_FREE) ++frees; return 1; }
#endif

void validation()
{
    section("Preparation and patch rejection preserve the working sound");
    SynthEngine e, reference;
    auto p = flat();
    CHECK(e.setPatch(p)); CHECK(reference.setPatch(p));
    CHECK(e.prepare(48000, 512)); CHECK(reference.prepare(48000, 512));
    CHECK(difference(render(e, 100, {{0, on()}}), render(reference, 100, {{0, on()}})) == 0);
    for (auto rate : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        CHECK(!e.prepare(rate, 512));
    CHECK(!e.prepare(48000, 0)); CHECK(!e.prepare(48000, -1));
    CHECK(!e.prepare(7999, 512)); CHECK(!e.prepare(384001, 512));
    CHECK(!e.prepare(std::numeric_limits<double>::max(), 512));
    CHECK(!e.prepare(48000, 65537)); CHECK(!e.prepare(48000, std::numeric_limits<int>::max()));
    CHECK(e.activeNoteCount() == 1);
    auto reject = [&](const SynthPatch& bad) { CHECK(!e.setPatch(bad)); CHECK(e.patch() == p); };
    for (float v : {-0.01f, 1.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        auto q = p; q.masterLevel = v; reject(q);
        q = p; q.layers[3].level = v; reject(q);
        q = p; q.layers[2].noiseLevel = v; reject(q);
        q = p; q.layers[0].oscillators[1].level = v; reject(q);
        q = p; q.layers[0].oscillators[1].position = v; reject(q);
        q = p; q.layers[0].ampEnvelope.sustain = v; reject(q);
        q = p; q.layers[0].filterEnvelope.sustain = v; reject(q);
    }
    auto badFloat = [&](float LayerSettings::* member, float value) { auto q = p; q.layers[3].*member = value; reject(q); };
    badFloat(&LayerSettings::cutoffHz, 19); badFloat(&LayerSettings::cutoffHz, 20001);
    badFloat(&LayerSettings::resonance, -0.1f); badFloat(&LayerSettings::resonance, 0.951f);
    badFloat(&LayerSettings::pan, -1.01f); badFloat(&LayerSettings::pan, 1.01f);
    badFloat(&LayerSettings::filterEnvelopeOctaves, -8.01f); badFloat(&LayerSettings::filterEnvelopeOctaves, 8.01f);
    for (auto member : {&EnvelopeSettings::attack, &EnvelopeSettings::decay, &EnvelopeSettings::release})
        for (float value : {-0.01f, 20.01f, std::numeric_limits<float>::quiet_NaN()})
            for (bool amp : {false, true}) { auto q = p; (amp ? q.layers[1].ampEnvelope : q.layers[1].filterEnvelope).*member = value; reject(q); }
    for (auto bounds : {std::pair{&OscillatorSettings::tune, 24.0f}, {&OscillatorSettings::detune, 100.0f}})
        for (float value : {-bounds.second - 1, bounds.second + 1, std::numeric_limits<float>::quiet_NaN()})
        { auto q = p; q.layers[1].oscillators[1].*bounds.first = value; reject(q); }
    for (float value : {0.049f, 0.951f, std::numeric_limits<float>::quiet_NaN()})
    { auto q = p; q.layers[0].oscillators[0].pulseWidth = value; reject(q); }
    for (int value : {-1, 2}) { auto q = p; q.layers[0].oscillators[0].tableIndex = value; reject(q); }
    auto q = p; q.layers[0].engine = static_cast<EngineKind>(99); reject(q);
    q = p; q.layers[0].filterMode = static_cast<FilterMode>(99); reject(q);
    q = p; q.layers[0].oscillators[0].waveform = static_cast<Waveform>(99); reject(q);
    CHECK(difference(render(e, 400), render(reference, 400)) == 0);
    CHECK(e.prepare(96000, 512)); CHECK(e.patch() == p); CHECK(e.activeNoteCount() == 0);
    SynthEngine unprepared;
    CHECK(energy(render(unprepared, 64, {{0, on()}})) == 0);
    juce::AudioBuffer<float> unsupported(1, 64); unsupported.applyGain(0);
    juce::MidiBuffer midi; midi.addEvent(on(), 0);
    e.render(unsupported, midi); CHECK(e.activeNoteCount() == 0); CHECK(unsupported.getMagnitude(0, 64) == 0);
    juce::AudioBuffer<float> oversized(2, 513);
    e.render(oversized, midi); CHECK(e.activeNoteCount() == 0); CHECK(oversized.getMagnitude(0, 513) == 0);
    CHECK(e.prepare(8000, 1)); CHECK(e.prepare(384000, 65536));
}
void soundAndControls()
{
    section("Both engines produce A4, velocity, two oscillators and editable timbre");
    for (auto kind : {EngineKind::analog, EngineKind::wavetable})
    {
        SynthEngine e; auto p = flat(kind); CHECK(e.setPatch(p)); CHECK(e.prepare(48000, 1024));
        auto x = render(e, 48000, {{0, on()}}, 1024);
        CHECK(energy(x) > 1e-6); CHECK(binEnergy(x, 440) > energy(x) * 0.95);
        e.reset(); auto quieter = render(e, 48000, {{0, on(69, 1, 0.25f)}}, 1024);
        CHECK(energy(quieter) < energy(x) * 0.08); CHECK(energy(quieter) > energy(x) * 0.04);
        p.layers[0].oscillators[0].level = 0; p.layers[0].oscillators[1].level = 1;
        p.layers[0].oscillators[1].waveform = Waveform::sine;
        p.layers[0].oscillators[1].tune = 12;
        CHECK(e.setPatch(p)); CHECK(e.prepare(48000, 1024));
        x = render(e, 48000, {{0, on()}}, 1024); CHECK(binEnergy(x, 880) > energy(x) * 0.95);
    }
    auto sample = [](SynthPatch p) { SynthEngine e; e.setPatch(p); e.prepare(48000, 512); return render(e, 4096, {{0, on()}}); };
    auto p = flat(EngineKind::wavetable);
    auto sine = sample(p); p.layers[0].oscillators[0].position = 1;
    auto bright = sample(p); CHECK(difference(sine, bright) > 0.005);
    p.layers[0].oscillators[0].tableIndex = 1;
    CHECK(difference(bright, sample(p)) > 0.005);
    p = flat(); p.layers[0].oscillators[0].waveform = Waveform::pulse;
    auto pulse = sample(p); p.layers[0].oscillators[0].pulseWidth = 0.2f;
    CHECK(difference(pulse, sample(p)) > 0.005);
    p = flat(); p.layers[0].oscillators[0].waveform = Waveform::triangle;
    CHECK(difference(sine, sample(p)) > 0.001);
    p = flat(); p.layers[0].noiseLevel = 1; p.layers[0].oscillators[0].level = 0;
    CHECK(energy(sample(p)) > 1e-6);
    p = flat(); p.layers[0].oscillators[0].waveform = Waveform::saw;
    auto open = sample(p); p.layers[0].cutoffHz = 100;
    CHECK(energy(sample(p)) < energy(open) * 0.1);
    p.layers[0].cutoffHz = 440; p.layers[0].resonance = 0.8f;
    auto resonant = sample(p); p.layers[0].resonance = 0;
    CHECK(energy(resonant) > energy(sample(p)) * 2);
    p.layers[0].filterMode = FilterMode::highpass;
    auto high = sample(p); p.layers[0].filterMode = FilterMode::bandpass;
    CHECK(difference(high, sample(p)) > 0.001);
    p.layers[0].filterMode = FilterMode::lowpass;
    const auto noEnvelope = sample(p);
    p.layers[0].filterEnvelopeOctaves = 3;
    CHECK(difference(sample(p), noEnvelope) > 0.001);
    p.layers[0].filterEnvelope = {0.04f, 0.01f, 0.02f, 0};
    const auto movingFilter = sample(p);
    CHECK(difference(sample(p), noEnvelope) > 0.001);
    CHECK(difference(movingFilter, sample(flat())) > 0.001);
    p = flat(); p.layers[0].ampEnvelope.attack = 0.05f;
    auto attack = sample(p); CHECK(energy(std::vector<float>(attack.begin(), attack.begin() + 240)) < energy(sine) * 0.02);
    p.layers[0].ampEnvelope = {0, 0.01f, 0.01f, 0.2f};
    auto decay = sample(p); CHECK(energy(std::vector<float>(decay.end() - 1024, decay.end())) < energy(sine) * 0.05);
}
void midiAndPool()
{
    section("Offsets, channel release, retrigger, oldest steal, controller and reset semantics");
    SynthEngine e; e.setPatch(flat()); e.prepare(48000, 1024);
    auto x = render(e, 512, {{101, on()}, {301, off()}});
    CHECK(energy(std::vector<float>(x.begin(), x.begin() + 101)) == 0);
    CHECK(energy(std::vector<float>(x.begin() + 102, x.begin() + 301)) > 1e-6);
    CHECK(energy(std::vector<float>(x.begin() + 301, x.end())) == 0); CHECK(e.activeNoteCount() == 0);
    render(e, 256, {{0, on(69, 1)}, {0, on(69, 2)}, {100, off(69, 1)}}); CHECK(e.activeNoteCount() == 1);
    CHECK(energy(render(e, 256)) > 1e-6);
    render(e, 256, {{0, juce::MidiMessage::allSoundOff(1)}}); CHECK(e.activeNoteCount() == 1);
    render(e, 256, {{0, juce::MidiMessage::allSoundOff(2)}}); CHECK(e.activeNoteCount() == 0);
    std::vector<Event> notes;
    for (int n = 60; n < 76; ++n) notes.emplace_back(n - 60, on(n));
    render(e, 256, notes); CHECK(e.activeNoteCount() == 16);
    render(e, 128, {{0, on(60)}}); CHECK(e.activeNoteCount() == 16);
    render(e, 128, {{0, on(90)}, {20, off(61)}}); CHECK(e.activeNoteCount() == 16); // 60 retrigger is newest: steal 61.
    render(e, 128, {{0, off(62)}}); CHECK(e.activeNoteCount() == 15);
    e.reset(); CHECK(e.activeNoteCount() == 0); CHECK(energy(render(e, 100)) == 0);
    auto p = flat(); p.layers[0].ampEnvelope.release = 0.01f; e.setPatch(p); e.prepare(48000, 1024);
    render(e, 128, notes);
    render(e, 1, {{0, off(70)}}); CHECK(e.activeNoteCount() == 16);
    render(e, 1, {{0, on(90)}}); CHECK(e.activeNoteCount() == 16);
    render(e, 600, {{0, off(60)}}); CHECK(e.activeNoteCount() == 15); // released 70 was reused, oldest held 60 survived.
    render(e, 1, {{0, juce::MidiMessage::allNotesOff(1)}}); CHECK(e.activeNoteCount() == 15);
    render(e, 600); CHECK(e.activeNoteCount() == 0);
    render(e, 64, {{0, on()}, {0, on(60, 2)}, {10, juce::MidiMessage::midiStop()}});
    CHECK(e.activeNoteCount() == 0); CHECK(energy(render(e, 64)) == 0);
    e.setPatch(flat()); e.prepare(48000, 1024);
    render(e, 64, {{0, on()}, {20, on(69, 1, 0)}}); CHECK(e.activeNoteCount() == 0);
    juce::AudioBuffer<float> audio(2, 64); juce::MidiBuffer invalid;
    invalid.addEvent(on(), -1); invalid.addEvent(on(60), 64); invalid.addEvent(on(70), 200);
    e.render(audio, invalid); CHECK(e.activeNoteCount() == 0); CHECK(audio.getMagnitude(0, 64) == 0);
    render(e, 100, {{0, on()}}); auto before = render(e, 128, {{0, on()}});
    e.reset(); auto after = render(e, 128, {{0, on()}}); CHECK(difference(before, after) == 0);
}
void layeringAndPartitions()
{
    section("Four layers retain sixteen musical notes and block partition equivalence");
    auto p = flat(); SynthEngine single, layered;
    single.setPatch(p); single.prepare(48000, 1024);
    for (auto& l : p.layers) { l = p.layers[0]; l.enabled = true; }
    layered.setPatch(p); layered.prepare(48000, 1024);
    auto a = render(single, 1024, {{0, on()}}); auto b = render(layered, 1024, {{0, on()}});
    for (auto& v : a) v *= 4;
    CHECK(difference(a, b) < 1e-6); CHECK(layered.activeNoteCount() == 1);
    std::vector<Event> events {{3, on(69, 1)}, {21, on(60, 2)}, {127, on(70)}, {700, off(69)}, {999, on(60, 2)}, {1789, juce::MidiMessage::allNotesOff(2)}, {2290, juce::MidiMessage::midiStop()}, {2401, on(127)}};
    for (auto& l : p.layers) { l.noiseLevel = 0.1f; l.ampEnvelope.release = 0.05f; l.filterEnvelopeOctaves = 2; }
    single.setPatch(p); layered.setPatch(p); single.prepare(48000, 1024); layered.prepare(48000, 1024);
    a = render(single, 4096, events, 1024); b = render(layered, 4096, events, 37);
    CHECK(difference(a, b) == 0); CHECK(single.activeNoteCount() == layered.activeNoteCount());
}
void smoothing()
{
    section("Live level, pan and filter edits ramp across blocks for at least five milliseconds");
    SynthEngine e, reference, coarseEngine; auto p = flat();
    e.setPatch(p); reference.setPatch(p); e.prepare(48000, 1024); reference.prepare(48000, 1024);
    render(e, 2048, {{0, on()}}); render(reference, 2048, {{0, on()}});
    p.masterLevel = 0; CHECK(e.setPatch(p));
    auto faded = render(e, 480, {}, 31); auto full = render(reference, 480, {}, 480);
    CHECK(energy(std::vector<float>(faded.begin(), faded.begin() + 100)) > 0);
    CHECK(std::abs(faded[0] - full[0]) < 0.01 * std::abs(full[0]));
    CHECK(std::abs(faded[119]) > 0.45 * std::abs(full[119]));
    CHECK(energy(std::vector<float>(faded.begin() + 240, faded.end())) < 1e-18);
    for (int edit = 0; edit < 5; ++edit)
    {
        p = flat();
        if (edit == 4) p.layers[0].cutoffHz = 1000;
        e.setPatch(p); reference.setPatch(p); coarseEngine.setPatch(p);
        e.prepare(48000, 1024); reference.prepare(48000, 1024); coarseEngine.prepare(48000, 1024);
        render(e, 2048, {{0, on()}}); render(reference, 2048, {{0, on()}}); render(coarseEngine, 2048, {{0, on()}});
        if (edit == 0) p.layers[0].level = 0;
        if (edit == 1) p.layers[0].pan = 1;
        if (edit == 2) p.layers[0].cutoffHz = 20;
        if (edit == 3) p.layers[0].oscillators[0].level = 0;
        if (edit == 4) p.layers[0].resonance = 0.95f;
        e.setPatch(p); coarseEngine.setPatch(p);
        auto fine = render(e, 512, {}, 17); auto coarse = render(coarseEngine, 512, {}, 512);
        auto unchanged = render(reference, 512);
        CHECK(difference(fine, coarse) == 0);
        CHECK(std::abs(fine[0] - unchanged[0]) < std::max(1e-7, 0.02 * std::abs(double(unchanged[0]))));
        CHECK(energy(std::vector<float>(fine.begin(), fine.begin() + 100)) > 1e-8);
        if (edit == 2)
            CHECK(energy(std::vector<float>(fine.begin() + 80, fine.begin() + 160)) > energy(std::vector<float>(unchanged.begin() + 80, unchanged.begin() + 160)) * 0.3);
        if (edit == 0 || edit == 1 || edit == 3)
        {
            // Oscillator gain is upstream of the resonant filter; allow its tail
            // to settle after the 240-sample control ramp has finished.
            const auto tail = energy(std::vector<float>(fine.begin() + (edit == 3 ? 320 : 241), fine.end()));
            CHECK(tail < 1e-16);
        }
    }
}
void structuralTransitions()
{
    section("Structural mode edits fade through silence before changing the signal path");
    for (int edit = 0; edit < 4; ++edit)
    {
        auto p = flat(edit == 2 ? EngineKind::wavetable : EngineKind::analog);
        p.layers[0].cutoffHz = 2000;
        p.layers[0].oscillators[0].position = 1;
        SynthEngine e, unchanged;
        e.setPatch(p); unchanged.setPatch(p); e.prepare(48000, 1024); unchanged.prepare(48000, 1024);
        render(e, 2048, {{0, on()}}); render(unchanged, 2048, {{0, on()}});
        if (edit == 0) p.layers[0].engine = EngineKind::wavetable;
        if (edit == 1) p.layers[0].oscillators[0].waveform = Waveform::pulse;
        if (edit == 2) p.layers[0].oscillators[0].tableIndex = 1;
        if (edit == 3) p.layers[0].filterMode = FilterMode::highpass;
        CHECK(e.setPatch(p)); CHECK(e.patch() == p);
        auto x = render(e, 512, {}, 31); auto baseline = render(unchanged, 512);
        CHECK(std::abs(x[0] - baseline[0]) < std::abs(baseline[0]) * 0.01);
        CHECK(std::abs(x[239]) < 1e-10);
        CHECK(energy(std::vector<float>(x.begin() + 480, x.end())) > 1e-10);
    }
    auto p = flat(); p.layers[0].ampEnvelope.release = 0.1f;
    SynthEngine e, fresh; e.setPatch(p); e.prepare(48000, 1024);
    render(e, 512, {{0, on()}});
    p.layers[0].oscillators[0].waveform = Waveform::saw; e.setPatch(p);
    render(e, 80); e.setPatch(p); // Republishing an identical patch does not restart a fade.
    p.layers[0].oscillators[0].waveform = Waveform::pulse; e.setPatch(p);
    auto coalesced = render(e, 160, {}, 17); CHECK(std::abs(coalesced.back()) < 1e-10);
    p.layers[0].filterMode = FilterMode::bandpass; e.setPatch(p);
    auto released = render(e, 512, {{0, off()}}, 37); CHECK(energy(released) > 1e-9);
    CHECK(e.activeNoteCount() == 1);
    e.reset(); CHECK(energy(render(e, 64)) == 0);
    fresh.setPatch(p); fresh.prepare(48000, 1024);
    CHECK(difference(render(e, 512, {{0, on()}}), render(fresh, 512, {{0, on()}})) == 0);
    p.layers[0].engine = EngineKind::wavetable; e.setPatch(p); render(e, 70);
    e.prepare(48000, 1024); fresh.setPatch(p); fresh.prepare(48000, 1024);
    CHECK(difference(render(e, 512, {{0, on()}}), render(fresh, 512, {{0, on()}})) == 0);
}
void spectral()
{
    section("High-note saw, pulse and bright tables suppress folded harmonics against naive oscillators");
    for (int shape = 0; shape < 4; ++shape)
    {
        auto p = flat(shape >= 2 ? EngineKind::wavetable : EngineKind::analog);
        auto& osc = p.layers[0].oscillators[0];
        osc.waveform = shape == 1 ? Waveform::pulse : Waveform::saw;
        osc.position = 1; osc.tableIndex = shape == 3 ? 1 : 0; osc.pulseWidth = 0.37f;
        osc.tune = float(12 * std::log2(7031.25 / (440 * std::exp2((96.0 - 69) / 12))));
        SynthEngine e; e.setPatch(p); e.prepare(48000, 1024);
        render(e, 8192, {{0, on(96)}}, 1024);
        auto x = render(e, 4096, {}, 1024);
        std::vector<float> naive(4096);
        for (size_t i = 0; i < naive.size(); ++i)
        {
            const double phase = std::fmod(double(i) * 600 / 4096, 1.0);
            naive[i] = float(shape == 1 || shape == 3 ? (phase < (shape == 3 ? 0.5 : 0.37) ? 1 : -1) : 2 * phase - 1);
        }
        auto clean = aliasFraction(x), dirty = aliasFraction(naive);
        std::cout << "SPECTRAL shape=" << shape << " alias_fraction=" << clean << " naive=" << dirty << '\n';
        CHECK(energy(x) > 1e-6); CHECK(clean < dirty * 0.1); CHECK(clean < 0.005);
    }
}
struct SeparatedAudio
{
    std::array<std::array<std::vector<float>, 2>, 4> layers;
    std::vector<float> gains;
    std::vector<float> mix(int channel) const
    {
        std::vector<float> result(gains.size());
        for (size_t frame = 0; frame < result.size(); ++frame)
        {
            double sum = 0;
            for (const auto& layer : layers) sum += layer[channel][frame];
            result[frame] = static_cast<float>(sum * gains[frame]);
        }
        return result;
    }
};
SeparatedAudio renderSeparated(SynthEngine& engine, int count, const std::vector<Event>& events = {},
                               int partition = 512, bool reapplyPatch = false)
{
    SeparatedAudio result;
    result.gains.resize(count);
    for (auto& layer : result.layers) for (auto& channel : layer) channel.resize(count);
    for (int start = 0; start < count; start += partition)
    {
        const int size = std::min(partition, count - start);
        std::array<juce::AudioBuffer<float>, 4> buffers;
        std::array<juce::AudioBuffer<float>*, 4> outputs;
        for (size_t layer = 0; layer < outputs.size(); ++layer)
        { buffers[layer].setSize(2, size); outputs[layer] = &buffers[layer]; }
        juce::MidiBuffer midi;
        for (const auto& e : events)
            if (e.first >= start && e.first < start + size) midi.addEvent(e.second, e.first - start);
        if (reapplyPatch) engine.setPatch(engine.patch());
        engine.renderLayers(outputs, std::span<float>(result.gains).subspan(start, size), midi);
        for (size_t layer = 0; layer < outputs.size(); ++layer)
            for (int channel = 0; channel < 2; ++channel)
                std::copy_n(buffers[layer].getReadPointer(channel), size, result.layers[layer][channel].data() + start);
    }
    return result;
}
SynthPatch distinctLayers()
{
    auto p = flat();
    for (size_t i = 0; i < p.layers.size(); ++i)
    {
        auto& l = p.layers[i]; l = p.layers[0]; l.enabled = true;
        l.oscillators[0].tune = static_cast<float>(i * 5);
        l.oscillators[0].waveform = static_cast<Waveform>(i);
        l.engine = i == 2 ? EngineKind::wavetable : EngineKind::analog;
        l.oscillators[0].position = 0.7f;
        l.pan = -0.75f + static_cast<float>(i) * 0.5f;
        l.noiseLevel = 0.03f;
    }
    p.masterLevel = 0.63f;
    return p;
}
void separatedLayersShareNotes()
{
    section("separatedLayersShareNotes: distinct stereo layers, shared notes, legacy sums, rates and partitions");
    const auto p = distinctLayers();
    std::vector<Event> events {{-1, on(99)}, {3, on(69)}, {63, on(60, 2)}, {127, off(69)},
        {511, on(72)}, {700, juce::MidiMessage::allNotesOff(2)}, {999, juce::MidiMessage::midiStop()},
        {1030, on(81)}, {2048, on(98)}};
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        SynthEngine reference; reference.setPatch(p); reference.prepare(rate, 512);
        const auto baseline = renderSeparated(reference, 2048, events);
        for (int partition : {1, 64, 512})
        {
            SynthEngine separated; separated.setPatch(p); separated.prepare(rate, 512);
            const auto audio = renderSeparated(separated, 2048, events, partition);
            CHECK(separated.activeNoteCount() == 1);
            for (int channel = 0; channel < 2; ++channel)
            {
                SynthEngine legacy; legacy.setPatch(p); legacy.prepare(rate, 512);
                CHECK(difference(audio.mix(channel), render(legacy, 2048, events, partition, channel)) < 1e-7);
                for (size_t layer = 0; layer < 4; ++layer)
                {
                    CHECK(energy(audio.layers[layer][channel]) > 1e-7);
                    CHECK(difference(audio.layers[layer][channel], baseline.layers[layer][channel]) == 0);
                    if (layer > 0) CHECK(difference(audio.layers[layer][channel], audio.layers[layer - 1][channel]) > 1e-3);
                    // A layer must equal its isolated legacy signal before the shared master.
                    auto solo = p; solo.masterLevel = 1;
                    for (size_t i = 0; i < 4; ++i) solo.layers[i].enabled = i == layer;
                    SynthEngine isolated; isolated.setPatch(solo); isolated.prepare(rate, 512);
                    CHECK(difference(audio.layers[layer][channel], render(isolated, 2048, events, partition, channel)) < 1e-7);
                }
            }
            CHECK(difference(audio.gains, baseline.gains) == 0);
        }
    }
    SynthEngine e; e.setPatch(p); e.prepare(48000, 512);
    std::vector<Event> notes; for (int n = 60; n < 76; ++n) notes.emplace_back(n - 60, on(n));
    renderSeparated(e, 128, notes); CHECK(e.activeNoteCount() == 16);
    renderSeparated(e, 128, {{0, on(90)}, {20, off(60)}}); CHECK(e.activeNoteCount() == 16);
    renderSeparated(e, 128, {{0, off(61)}}); CHECK(e.activeNoteCount() == 15);
    renderSeparated(e, 128, {{0, on(90)}}); CHECK(e.activeNoteCount() == 15);
    renderSeparated(e, 64, {{0, on(50, 2)}, {3, juce::MidiMessage::allSoundOff(1)}}); CHECK(e.activeNoteCount() == 1);
    renderSeparated(e, 64, {{0, juce::MidiMessage::allNotesOff(2)}}); CHECK(e.activeNoteCount() == 0);
}
void separatedMasterRamp()
{
    section("separatedMasterRamp: pre-master signal, smoothing, structural recall and early-release guard");
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        auto p = distinctLayers(); p.masterLevel = 1;
        SynthEngine e, legacy; e.setPatch(p); legacy.setPatch(p); e.prepare(rate, 512); legacy.prepare(rate, 512);
        renderSeparated(e, 512, {{0, on()}}); render(legacy, 512, {{0, on()}});
        p.masterLevel = 0; e.setPatch(p); legacy.setPatch(p);
        const int ramp = static_cast<int>(std::ceil(rate * 0.005));
        const auto faded = renderSeparated(e, 1024, {}, 1, true);
        CHECK(difference(faded.mix(0), render(legacy, 1024)) < 1e-7);
        CHECK(std::abs(faded.gains.front() - (1.0 - 1.0 / ramp)) < 1e-7);
        CHECK(faded.gains[ramp - 2] > 0); CHECK(faded.gains[ramp - 1] == 0);
        CHECK(energy(std::vector<float>(faded.gains.begin() + ramp, faded.gains.end())) == 0);
        for (const auto& layer : faded.layers)
            for (const auto& channel : layer) CHECK(energy(std::vector<float>(channel.begin() + ramp, channel.end())) > 1e-7);
        // Independent legacy comparison through a coalesced structural edit and full reset/recall.
        p.masterLevel = 0.75f; p.layers[0].filterMode = FilterMode::highpass;
        e.setPatch(p); legacy.setPatch(p);
        CHECK(difference(renderSeparated(e, 70).mix(0), render(legacy, 70)) < 1e-7);
        p.layers[0].oscillators[0].waveform = Waveform::pulse;
        e.setPatch(p); legacy.setPatch(p);
        CHECK(difference(renderSeparated(e, 1024, {}, 64, true).mix(0), render(legacy, 1024)) < 1e-7);
        e.reset(); legacy.reset(); CHECK(e.activeNoteCount() == 0);
        CHECK(energy(renderSeparated(e, 512).mix(0)) == 0); render(legacy, 512);
        CHECK(difference(renderSeparated(e, 1024, {{0, on()}}).mix(0), render(legacy, 1024, {{0, on()}})) < 1e-7);
        // Early note-off with zero sustain must keep its instantaneous release slope
        // even if an identical patch is republished every frame.
        for (auto& layer : p.layers)
        { layer.ampEnvelope = {0.04f, 0.1f, 0.03f, 0}; layer.filterEnvelope = {0.03f, 0.1f, 0.03f, 0}; layer.filterEnvelopeOctaves = 2; }
        e.setPatch(p); legacy.setPatch(p); e.reset(); legacy.reset();
        renderSeparated(e, 240, {{0, on()}}); render(legacy, 240, {{0, on()}});
        const auto release = renderSeparated(e, 512, {{0, off()}}, 1, true);
        CHECK(energy(release.mix(0)) > 1e-9); CHECK(e.activeNoteCount() == 1);
        CHECK(difference(release.mix(0), render(legacy, 512, {{0, off()}})) < 1e-7);
        CHECK(difference(renderSeparated(e, 4096, {}, 64, true).mix(0), render(legacy, 4096)) < 1e-7);
        CHECK(e.activeNoteCount() == 0);
        e.prepare(rate, 512); legacy.prepare(rate, 512);
        CHECK(difference(renderSeparated(e, 512, {{0, on()}}).mix(1), render(legacy, 512, {{0, on()}}, 512, 1)) < 1e-7);
    }
}
void invalidSeparatedBuffersPreserveState()
{
    section("invalidSeparatedBuffersPreserveState: reject shapes, aliases and bounds without advancing MIDI or ramps");
    for (int invalidCase = 0; invalidCase < 14; ++invalidCase)
    {
        SynthEngine e, reference; auto p = distinctLayers();
        e.setPatch(p); reference.setPatch(p); e.prepare(48000, 64); reference.prepare(48000, 64);
        render(e, 32, {{0, on()}}); render(reference, 32, {{0, on()}});
        p.masterLevel = 0.2f; p.layers[0].filterMode = FilterMode::highpass;
        e.setPatch(p); reference.setPatch(p);
        std::array<juce::AudioBuffer<float>, 4> buffers;
        std::array<juce::AudioBuffer<float>*, 4> outputs;
        for (size_t i = 0; i < 4; ++i) { buffers[i].setSize(2, 64); outputs[i] = &buffers[i]; }
        std::array<float, 128> gains;
        std::span<float> gainSpan(gains.data(), 64);
        std::array<float, 256> shared;
        std::array<float*, 2> pointers {shared.data(), shared.data() + 128};
        if (invalidCase == 0) outputs[2] = nullptr;
        if (invalidCase == 1) buffers[2].setSize(2, 63);
        if (invalidCase == 2) buffers[2].setSize(1, 64);
        if (invalidCase == 3)
        { for (auto& buffer : buffers) buffer.setSize(2, 65); gainSpan = std::span<float>(gains.data(), 65); }
        if (invalidCase == 4) gainSpan = std::span<float>(gains.data(), 63);
        if (invalidCase == 5) outputs[2] = outputs[0];
        if (invalidCase == 6 || invalidCase == 7)
        {
            buffers[0].setDataToReferTo(pointers.data(), 2, 64);
            pointers[0] += invalidCase == 6 ? 0 : 32;
            pointers[1] += invalidCase == 6 ? 0 : 32;
            buffers[1].setDataToReferTo(pointers.data(), 2, 64);
        }
        if (invalidCase == 8)
        { pointers[1] = pointers[0] + 32; buffers[0].setDataToReferTo(pointers.data(), 2, 64); }
        if (invalidCase == 9) gainSpan = std::span<float>(buffers[0].getWritePointer(0), 64);
        if (invalidCase == 10) gainSpan = std::span<float>(gains.data(), 65);
        if (invalidCase == 11) outputs[0] = nullptr;
        if (invalidCase == 12) buffers[2].setSize(3, 64);
        if (invalidCase == 13)
        { buffers[0].setDataToReferTo(pointers.data(), 2, 64); gainSpan = std::span<float>(shared.data() + 32, 64); }
        for (auto& buffer : buffers)
            for (int c = 0; c < buffer.getNumChannels(); ++c) std::fill_n(buffer.getWritePointer(c), buffer.getNumSamples(), 1.0f);
        std::fill(gainSpan.begin(), gainSpan.end(), 1.0f);
        juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::midiStop(), 0); midi.addEvent(on(91), 0);
        e.renderLayers(outputs, gainSpan, midi);
        CHECK(e.activeNoteCount() == 1);
        for (auto* buffer : outputs) if (buffer != nullptr) CHECK(buffer->getMagnitude(0, buffer->getNumSamples()) == 0);
        CHECK(std::all_of(gainSpan.begin(), gainSpan.end(), [](float v) { return v == 0; }));
        CHECK(difference(render(e, 512, {}, 64), render(reference, 512, {}, 64)) == 0);
    }
    SynthEngine e, reference; e.setPatch(distinctLayers()); reference.setPatch(distinctLayers());
    std::array<juce::AudioBuffer<float>, 4> buffers;
    std::array<juce::AudioBuffer<float>*, 4> outputs;
    for (size_t i = 0; i < 4; ++i) { buffers[i].setSize(2, 64); outputs[i] = &buffers[i]; }
    std::array<float, 64> gains; gains.fill(1);
    juce::MidiBuffer midi; midi.addEvent(on(), 0);
    e.renderLayers(outputs, gains, midi); CHECK(e.activeNoteCount() == 0);
    for (auto& buffer : buffers) CHECK(buffer.getMagnitude(0, 64) == 0);
    CHECK(std::all_of(gains.begin(), gains.end(), [](float v) { return v == 0; }));
    e.prepare(48000, 64); reference.prepare(48000, 64);
    for (auto& buffer : buffers) buffer.setSize(2, 0);
    e.renderLayers(outputs, {}, midi); CHECK(e.activeNoteCount() == 0);
    CHECK(difference(render(e, 64, {{0, on()}}), render(reference, 64, {{0, on()}})) == 0);
    // Adjacent channels in one allocation are valid; an overlap check must not reject them.
    std::array<float, 576> contiguous;
    for (size_t i = 0; i < 4; ++i)
    { std::array<float*, 2> channels {contiguous.data() + i * 128, contiguous.data() + i * 128 + 64}; buffers[i].setDataToReferTo(channels.data(), 2, 64); }
    e.renderLayers(outputs, std::span<float>(contiguous.data() + 512, 64), {});
    CHECK(buffers[0].getMagnitude(0, 64) > 1e-3);
    CHECK(contiguous.back() == distinctLayers().masterLevel);
    e.reset();
    juce::MidiBuffer outside; outside.addEvent(on(), -1); outside.addEvent(on(80), 64); outside.addEvent(on(90), 200);
    e.renderLayers(outputs, gains, outside);
    CHECK(e.activeNoteCount() == 0);
    for (auto& buffer : buffers) CHECK(buffer.getMagnitude(0, 64) == 0);
}
// A local rising-zero-crossing period measures actual filtered oscillator audio.
// Compare the geometric-time centre of that period against the expected pitch;
// a two-second glide keeps filter delay/chirp curvature well below one cent.
std::pair<double, double> measuredPitch(const std::vector<float>& audio, double rate, double centre)
{
    double previous = -1, bestDistance = std::numeric_limits<double>::max();
    std::pair<double, double> best {0, 0};
    for (size_t i = 1; i < audio.size(); ++i)
        if (audio[i - 1] <= 0 && audio[i] > 0)
        {
            const double crossing = double(i - 1) - audio[i - 1] / double(audio[i] - audio[i - 1]);
            if (previous >= 0)
            {
                const double middle = (previous + crossing) * 0.5;
                if (std::abs(middle - centre) < bestDistance)
                { bestDistance = std::abs(middle - centre); best = {rate / (crossing - previous), middle}; }
            }
            previous = crossing;
        }
    return best;
}
double cents(double frequency, double note)
{ return std::abs(1200 * std::log2(frequency / (440 * std::exp2((note - 69) / 12)))); }
void monoHeldKeysAndChannels()
{
    section("monoHeldKeysAndChannels: last held channel/key, bounded ownership and four-layer sound");
    auto p = flat(); p.performance.mode = VoiceMode::monophonic;
    SynthEngine e, reference; e.setPatch(p); e.prepare(48000, 512);
    reference.setPatch(flat()); reference.prepare(48000, 512);
    auto matchesFresh = [&](int note, const std::vector<Event>& events) {
        reference.reset();
        CHECK(difference(render(e, 512, events), render(reference, 512, {{0, on(note)}})) == 0);
        CHECK(e.activeNoteCount() == 1);
    };
    matchesFresh(60, {{0, on(60)}});
    matchesFresh(72, {{0, on(72, 2)}});
    matchesFresh(60, {{0, off(72, 2)}});
    SynthEngine untouched; untouched.setPatch(p); untouched.prepare(48000, 512);
    e.reset(); render(e, 256, {{0, on(60)}}); render(untouched, 256, {{0, on(60)}});
    CHECK(difference(render(e, 512, {{0, off(60, 2)}, {8, off(99)}, {19, juce::MidiMessage::allSoundOff(3)}}), render(untouched, 512)) == 0);
    matchesFresh(67, {{0, on(67, 2)}});
    matchesFresh(60, {{0, juce::MidiMessage::allNotesOff(2)}});
    matchesFresh(76, {{0, on(76, 3)}});
    matchesFresh(60, {{0, juce::MidiMessage::allSoundOff(3)}});
    CHECK(energy(render(e, 512, {{0, on(60, 1, 0)}})) == 0); CHECK(e.activeNoteCount() == 0);
    // Every key/channel record is available; >16 notes never create extra mono voices.
    std::vector<Event> keys;
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note < 128; ++note) keys.emplace_back(0, on(note, channel));
    render(e, 64, keys); CHECK(e.activeNoteCount() == 1);
    matchesFresh(126, {{0, off(127, 16)}});
    matchesFresh(127, {{0, juce::MidiMessage::allNotesOff(16)}});
    for (int channel = 15; channel >= 1; --channel)
        render(e, 1, {{0, juce::MidiMessage::allSoundOff(channel)}});
    CHECK(e.activeNoteCount() == 0); CHECK(energy(render(e, 256)) == 0);
    e.reset();
    for (auto& layer : p.layers) { layer = p.layers[0]; layer.enabled = true; }
    e.setPatch(p); e.prepare(48000, 512);
    auto four = renderSeparated(e, 1024, {{0, on(60)}, {100, on(72)}, {300, off(72)}}, 64);
    CHECK(e.activeNoteCount() == 1);
    reference.setPatch(flat()); reference.prepare(48000, 512);
    auto one = render(reference, 1024, {{0, on(60)}, {100, juce::MidiMessage::allSoundOff(1)}, {100, on(72)}, {300, juce::MidiMessage::allSoundOff(1)}, {300, on(60)}}, 64);
    for (auto& value : one) value *= 4;
    CHECK(difference(four.mix(0), one) < 1e-7);
    CHECK(energy(render(e, 64, {{0, juce::MidiMessage::midiStop()}})) == 0);
    render(e, 64, {{0, off(72)}, {0, off(60)}}); CHECK(e.activeNoteCount() == 0);
    // Ordinary and channel note-off allow a release, and identical patches retain its slope.
    p = flat(); p.performance = {VoiceMode::monophonic, 0, true}; p.layers[0].ampEnvelope = {0.1f, 0, 0.1f, 0};
    for (bool channelOff : {false, true})
    {
        e.setPatch(p); untouched.setPatch(p); e.prepare(48000, 512); untouched.prepare(48000, 512);
        render(e, 240, {{0, on()}}); render(untouched, 240, {{0, on()}});
        const std::vector<Event> release {{0, channelOff ? juce::MidiMessage::allNotesOff(1) : off()}};
        auto a = renderSeparated(e, 1024, release, 1, true).mix(0);
        auto b = render(untouched, 1024, release);
        CHECK(energy(a) > 1e-9); CHECK(difference(a, b) < 1e-7);
        render(e, 5000); CHECK(e.activeNoteCount() == 0);
    }
}
void glidePitchAndLegato()
{
    section("glidePitchAndLegato: measured logarithmic audio pitch, current retarget, envelope and velocity");
    double maxCents = 0;
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        const int duration = int(std::ceil(rate * 2.0));
        const int transition = int(rate / 10);
        auto p = flat(); p.performance = {VoiceMode::monophonic, 2, true};
        std::vector<float> baseline;
        for (int partition : {1, 64, 512})
        {
            SynthEngine e, reference; e.setPatch(p); e.prepare(rate, 512);
            reference.setPatch(flat()); reference.prepare(rate, 512);
            CHECK(difference(render(e, transition, {{0, on(57)}}, partition), render(reference, transition, {{0, on(57)}}, partition)) == 0);
            auto glide = render(e, duration + int(rate / 10), {{0, on(81)}}, partition);
            if (baseline.empty()) baseline = glide; else CHECK(difference(glide, baseline) == 0);
            for (double centre : {duration * 0.5, duration + rate * 0.05})
            {
                const auto [frequency, at] = measuredPitch(glide, rate, centre);
                const double expected = 57 + 24 * std::clamp(at / duration, 0.0, 1.0);
                const double error = cents(frequency, expected); maxCents = std::max(maxCents, error); CHECK(error <= 1);
            }
            CHECK(e.activeNoteCount() == 1);
            e.reset(); render(e, transition, {{0, on(57)}}, partition);
            render(e, duration / 2, {{0, on(81)}}, partition); // now A4, not original A3 or target A5
            auto retarget = render(e, duration + int(rate / 10), {{0, on(45)}}, partition);
            for (double centre : {rate * 0.01, duration * 0.5, duration + rate * 0.05})
            {
                const auto [frequency, at] = measuredPitch(retarget, rate, centre);
                const double expected = 69 - 24 * std::clamp(at / duration, 0.0, 1.0);
                const double error = cents(frequency, expected); maxCents = std::max(maxCents, error); CHECK(error <= 1);
            }
            // Zero glide is immediate; separated and mixed entries share the same state.
            p.performance.glideSeconds = 0; e.setPatch(p); e.reset(); reference.reset();
            render(e, transition, {{0, on(57)}}); render(reference, transition, {{0, on(57)}});
            auto instant = renderSeparated(e, int(rate / 10), {{0, on(81)}}, partition).mix(0);
            const auto [frequency, at] = measuredPitch(instant, rate, rate * 0.05); (void) at;
            CHECK(cents(frequency, 81) <= 1);
            p.performance.glideSeconds = 2;
        }
        // Retriggered articulation still glides from the current musical pitch.
        p.performance.legato = false;
        SynthEngine retrigger; retrigger.setPatch(p); retrigger.prepare(rate, 512);
        render(retrigger, transition, {{0, on(57)}});
        auto audio = render(retrigger, duration + transition, {{0, on(81)}}, 64);
        const auto [frequency, at] = measuredPitch(audio, rate, duration * 0.5);
        const auto error = cents(frequency, 57 + 24 * at / duration);
        maxCents = std::max(maxCents, error); CHECK(error <= 1);
        // Endpoint timing is sample-exact, including ceil for a nonintegral
        // duration: forcing the final pitch agrees at frame N, but not N-1.
        for (int early : {0, 1})
        {
            auto timed = flat(); timed.performance = {VoiceMode::monophonic, 0.013f, true};
            const int frames = int(std::ceil(rate * timed.performance.glideSeconds));
            SynthEngine natural, forced;
            natural.setPatch(timed); forced.setPatch(timed); natural.prepare(rate, 512); forced.prepare(rate, 512);
            render(natural, 200, {{0, on(57)}}); render(forced, 200, {{0, on(57)}});
            render(natural, frames - 1 - early, {{0, on(81)}}); render(forced, frames - 1 - early, {{0, on(81)}});
            timed.performance.glideSeconds = 0; forced.setPatch(timed);
            const double delta = difference(render(natural, 512), render(forced, 512, {{0, on(81, 2)}}));
            CHECK(early == 0 ? delta == 0 : delta > 1e-10);
        }
    }
    std::cout << "MONO_GLIDE max_audio_pitch_error_cents=" << maxCents << '\n';
    auto p = flat(); p.performance = {VoiceMode::monophonic, 1, true};
    SynthEngine e, unchanged; e.setPatch(p); unchanged.setPatch(p); e.prepare(48000, 512); unchanged.prepare(48000, 512);
    render(e, 512, {{0, on(60)}}); render(unchanged, 512, {{0, on(60)}});
    CHECK(difference(renderSeparated(e, 8192, {{0, on(72)}}, 1, true).mix(0), render(unchanged, 8192, {{0, on(72)}})) < 1e-7);
    // Editing the next glide duration cannot restart or alter the active glide.
    p.performance.glideSeconds = 2; e.setPatch(p);
    CHECK(difference(render(e, 8192), render(unchanged, 8192)) == 0);
    // A same-pitch key on another channel isolates phase/filter/envelope preservation.
    p.performance.glideSeconds = 0; p.layers[0].ampEnvelope.attack = 0.1f;
    e.setPatch(p); unchanged.setPatch(p); e.prepare(48000, 512); unchanged.prepare(48000, 512);
    render(e, 1200, {{0, on()}}); render(unchanged, 1200, {{0, on()}});
    CHECK(difference(render(e, 512, {{0, on(69, 2)}}), render(unchanged, 512)) == 0);
    p.performance.legato = false; e.setPatch(p);
    SynthEngine fresh; fresh.setPatch(p); fresh.prepare(48000, 512);
    CHECK(difference(render(e, 512, {{0, on(69, 3)}}), render(fresh, 512, {{0, on(69, 3)}})) == 0);
    // A new phrase retriggers even when a previous released voice still has a tail.
    p.performance.legato = true; p.performance.glideSeconds = 2; p.layers[0].ampEnvelope.release = 1;
    e.setPatch(p); fresh.setPatch(p); e.prepare(48000, 512); fresh.prepare(48000, 512);
    render(e, 3000, {{0, on(57)}, {2500, off(57)}});
    CHECK(difference(render(e, 512, {{0, on(81)}}), render(fresh, 512, {{0, on(81)}})) == 0);
    // Velocity ramp is measured as actual output gain, including the exact five-ms endpoint.
    p = flat(); p.performance = {VoiceMode::monophonic, 0, true};
    e.setPatch(p); unchanged.setPatch(p); e.prepare(48000, 512); unchanged.prepare(48000, 512);
    render(e, 1024, {{0, on()}}); render(unchanged, 1024, {{0, on()}});
    auto ramped = renderSeparated(e, 512, {{0, on(69, 2, 32.0f / 127)}}, 1, true).mix(0);
    auto constant = render(unchanged, 512);
    double maxRampError = 0;
    for (size_t i = 0; i < ramped.size(); ++i)
    {
        const double gain = 1 + (32.0 / 127 - 1) * std::min(1.0, double(i + 1) / 240);
        maxRampError = std::max(maxRampError, std::abs(ramped[i] - constant[i] * gain));
    }
    CHECK(maxRampError < 1e-7);
    std::cout << "MONO_VELOCITY max_ramp_error=" << maxRampError << '\n';
}
void performanceTransitionsAndValidation()
{
    section("performanceTransitionsAndValidation: transactional settings, mode rebuild/fade/coalesce and allocations");
    auto p = flat(); SynthEngine e, reference;
    CHECK(p.performance == PerformanceSettings{});
    e.setPatch(p); reference.setPatch(p); e.prepare(48000, 512); reference.prepare(48000, 512);
    render(e, 1024, {{0, on()}}); render(reference, 1024, {{0, on()}});
    for (float value : {-0.01f, 2.01f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    { auto bad = p; bad.performance.glideSeconds = value; CHECK(!e.setPatch(bad)); CHECK(e.patch() == p); }
    auto bad = p; bad.performance.mode = static_cast<VoiceMode>(99); CHECK(!e.setPatch(bad)); CHECK(e.patch() == p);
    CHECK(difference(render(e, 512), render(reference, 512)) == 0);
    auto polyPerformance = p; polyPerformance.performance = {VoiceMode::polyphonic, 2, true};
    CHECK(e.setPatch(polyPerformance));
    const std::vector<Event> polyNotes {{0, on(60)}, {30, on(72, 2)}, {100, off()}, {200, on(60)}};
    CHECK(difference(render(e, 512, polyNotes), render(reference, 512, polyNotes)) == 0);
    double maxFadeError = 0;
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (int partition : {1, 64, 512})
        {
            const int fade = int(std::ceil(rate * 0.005));
            p = flat(); e.setPatch(p); reference.setPatch(p); e.prepare(rate, 512); reference.prepare(rate, 512);
            render(e, 1024, {{0, on(60)}}); render(reference, 1024, {{0, on(60)}});
            p.performance.mode = VoiceMode::monophonic; e.setPatch(p);
            auto down = renderSeparated(e, fade, {}, partition, true).mix(0);
            auto full = render(reference, fade, {}, partition);
            for (int i = 0; i < fade; ++i)
                maxFadeError = std::max(maxFadeError, std::abs(down[size_t(i)] - full[size_t(i)] * (1 - double(i + 1) / fade)));
            CHECK(std::abs(down.back()) < 1e-12); CHECK(e.activeNoteCount() == 1);
            reference.reset(); auto rebuilt = render(reference, fade + 1, {{0, on(60)}}, partition);
            auto up = renderSeparated(e, fade, {}, partition, true).mix(0);
            for (int i = 0; i < fade; ++i)
                maxFadeError = std::max(maxFadeError, std::abs(up[size_t(i)] - rebuilt[size_t(i + 1)] * (double(i + 1) / fade)));
            CHECK(energy(up) > 1e-6);
            // Ownership updates during the fade; mono rebuild chooses latest remaining key.
            e.reset(); std::vector<Event> held;
            for (int note = 40; note < 70; ++note) held.emplace_back(0, on(note, note == 69 ? 2 : 1));
            render(e, 64, held); CHECK(e.activeNoteCount() == 1);
            p.performance.mode = VoiceMode::polyphonic; e.setPatch(p);
            render(e, fade, {{fade / 2, juce::MidiMessage::allSoundOff(2)}}, partition); CHECK(e.activeNoteCount() == 16);
            render(e, fade, {}, partition);
            // Only latest sixteen held keys were rebuilt (53..68); old held keys are not sounding.
            render(e, 32, {{0, off(40)}}); CHECK(e.activeNoteCount() == 16);
            render(e, 32, {{0, off(53)}}); CHECK(e.activeNoteCount() == 15);
            p.performance.mode = VoiceMode::monophonic; e.setPatch(p);
            render(e, fade + 1, {{fade / 2, off(68)}}, partition); CHECK(e.activeNoteCount() == 1);
            render(e, fade, {}, partition);
            const auto [hz, when] = measuredPitch(render(e, 4096, {}, partition), rate, 2048); (void) when;
            CHECK(cents(hz, 67) <= 1);
            // Returning to the original mode while fading cancels a stale rebuild.
            p.performance.mode = VoiceMode::polyphonic; e.setPatch(p); render(e, fade / 3);
            p.performance.mode = VoiceMode::monophonic; e.setPatch(p); render(e, 2 * fade, {}, partition); CHECK(e.activeNoteCount() == 1);
            render(e, 64, {{0, juce::MidiMessage::allNotesOff(1)}}); CHECK(e.activeNoteCount() == 0);
            p.performance.mode = VoiceMode::polyphonic; e.setPatch(p);
            render(e, 2 * fade, {}, partition); CHECK(e.activeNoteCount() == 0);
            // Stop clears held records even if a rebuild is pending.
            render(e, 64, {{0, on(60)}, {0, on(80, 2)}});
            p.performance.mode = VoiceMode::monophonic; e.setPatch(p);
            CHECK(energy(render(e, 2 * fade, {{0, juce::MidiMessage::midiStop()}}, partition)) == 0);
            CHECK(e.activeNoteCount() == 0);
            e.reset(); CHECK(energy(render(e, 512)) == 0);
            reference.setPatch(p); reference.prepare(rate, 512);
            CHECK(difference(render(e, 512, {{0, on(76)}}), render(reference, 512, {{0, on(76)}})) == 0);
            // Reset and reprepare clear pending transitions and every held record.
            p.performance.mode = VoiceMode::polyphonic; e.setPatch(p); render(e, fade / 2);
            e.reset(); reference.setPatch(p); reference.prepare(rate, 512);
            CHECK(difference(render(e, 512, {{0, on(72)}}), render(reference, 512, {{0, on(72)}})) == 0);
            p.performance.mode = VoiceMode::monophonic; e.setPatch(p); render(e, fade / 2);
            e.prepare(rate, 512); reference.setPatch(p); reference.prepare(rate, 512);
            CHECK(difference(render(e, 512, {{0, on(64)}}), render(reference, 512, {{0, on(64)}})) == 0);
        }
    CHECK(maxFadeError < 1e-7); std::cout << "PERFORMANCE_FADE max_audio_error=" << maxFadeError << '\n';
    // Prepared setting/event/transition/reset work, including the full key table, is allocation-free.
    juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer keys, released, stop;
    for (int channel = 1; channel <= 16; ++channel)
        for (int note = 0; note < 128; ++note) keys.addEvent(on(note, channel), 0);
    for (int channel = 1; channel <= 16; ++channel) released.addEvent(juce::MidiMessage::allNotesOff(channel), 1);
    stop.addEvent(juce::MidiMessage::midiStop(), 100);
    std::array<juce::AudioBuffer<float>, 4> buffers;
    std::array<juce::AudioBuffer<float>*, 4> outputs;
    for (size_t i = 0; i < 4; ++i) { buffers[i].setSize(2, 512); outputs[i] = &buffers[i]; }
    std::array<float, 512> gains;
    p = flat(); p.performance = {VoiceMode::monophonic, 2, true};
    for (auto& layer : p.layers) { layer = p.layers[0]; layer.enabled = true; }
    e.setPatch(p); e.prepare(48000, 512);
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations = 0; auto oldHook = _CrtSetAllocHook(allocationHook); tracking = true;
    for (int i = 0; i < 12; ++i)
    {
        e.render(audio, keys); e.setPatch(p); e.renderLayers(outputs, gains, released);
        p.performance.mode = VoiceMode::polyphonic; e.setPatch(p); e.renderLayers(outputs, gains, keys);
        p.performance.mode = VoiceMode::monophonic; e.setPatch(p); e.render(audio, {}); e.render(audio, stop); e.reset();
    }
    tracking = false; _CrtSetAllocHook(oldHook);
    std::cout << "PERFORMANCE_PREPARED allocations=" << allocations << '\n'; CHECK(allocations == 0);
#else
    CHECK(false);
#endif
}

// Modulation checks use audio from the accepted unmodulated engine as the
// carrier, plus analytic gain/phase expectations or explicit physical settings.
// Ignoring routes, advancing a paused clock, changing bank meaning, or resetting
// a clock on a note/patch event must fail these tests.
std::vector<float> modRender(SynthEngine& e, int count, const std::vector<Event>& events = {},
    int partition = 512, std::optional<double> tempo = std::nullopt, bool separated = false)
{
    std::vector<float> result(static_cast<size_t>(count));
    for (int start = 0; start < count; start += partition)
    {
        const int n = std::min(partition, count-start);
        juce::MidiBuffer midi;
        for (const auto& [at,message] : events) if (at >= start && at < start+n) midi.addEvent(message,at-start);
        juce::AudioBuffer<float> out(2,n);
        if (!separated) e.render(out,midi,tempo);
        else
        {
            std::array<juce::AudioBuffer<float>,4> buffers;
            std::array<juce::AudioBuffer<float>*,4> pointers;
            for (size_t i=0;i<4;++i) {buffers[i].setSize(2,n);pointers[i]=&buffers[i];}
            std::vector<float> gains(static_cast<size_t>(n));
            e.renderLayers(pointers,gains,midi,tempo);
            for(int s=0;s<n;++s)
            { double sum=0; for(auto& b:buffers)sum+=b.getSample(0,s);out.setSample(0,s,float(sum*gains[size_t(s)])); }
        }
        std::copy_n(out.getReadPointer(0),n,result.data()+start);
    }
    return result;
}
std::vector<float> modSound(const SynthPatch& p, int count=4096, double rate=48000,
    std::vector<Event> events={{0,on()}}, std::optional<double> tempo=std::nullopt)
{ SynthEngine e; CHECK(e.setPatch(p));CHECK(e.prepare(rate,512));return modRender(e,count,events,512,tempo); }
double gainError(const std::vector<float>& actual,const std::vector<float>& carrier,const std::vector<double>& gains)
{ double error=0;for(size_t i=0;i<actual.size();++i)error=std::max(error,std::abs(actual[i]-carrier[i]*gains[i]));return error; }
double lfoExpected(LfoShape shape,double phase)
{
    phase-=std::floor(phase);
    if(shape==LfoShape::sine)return std::sin(2*pi*phase);
    if(shape==LfoShape::triangle)return 1-4*std::abs(phase-.5);
    if(shape==LfoShape::saw)return 2*phase-1;
    return phase<.5?1:-1;
}
void lfoAudioPhaseAndTempo()
{
    section("lfoAudioPhaseAndTempo"); double worst=0;
    for(double rate:{44100.,48000.,96000.}) for(auto shape:{LfoShape::sine,LfoShape::triangle,LfoShape::saw,LfoShape::square})
    {
        const int count=int(rate*.63);auto p=flat();auto& l=p.layers[0];l.level=.5f;
        l.lfos[0]={shape,false,5,1};l.modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.25f};
        const auto carrier=modSound(flat(),count,rate);std::vector<double> gains(size_t(count),0);
        // Match iterative phase accumulation to avoid a square discontinuity's
        // mathematically ambiguous floating boundary, without using engine code.
        double phase=0;for(auto& gain:gains){gain=.5+.25*lfoExpected(shape,phase);phase=std::fmod(phase+5/rate,1.);}
        for(bool separated:{false,true})for(int partition:{1,64,512})
        { SynthEngine e;CHECK(e.setPatch(p));CHECK(e.prepare(rate,512));const auto x=modRender(e,count,{{0,on()}},partition,{},separated);
          const double error=gainError(x,carrier,gains);worst=std::max(worst,error);CHECK(error<4e-9); }
        auto synced=p;synced.layers[0].lfos[0].synced=true;synced.layers[0].lfos[0].beats=.5f;
        CHECK(difference(modSound(synced,count,rate,{{0,on()}},150),modSound(p,count,rate))==0);
    }
    // Independent clocks, signs and sums; unused clocks still run, note events
    // start oscillators but never restart either LFO.
    auto p=flat();auto& l=p.layers[0];l.level=.5f;l.lfos[0].rateHz=3;l.lfos[1]={LfoShape::triangle,false,7,1};
    l.modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.125f};
    l.modulation[1]={true,ModSource::lfo2,ModTarget::level,ModBank::common,-.125f};
    SynthEngine e,base;CHECK(e.setPatch(p));CHECK(base.setPatch(flat()));CHECK(e.prepare(48000,512));CHECK(base.prepare(48000,512));
    modRender(e,1237);modRender(base,1237);
    auto x=modRender(e,8000,{{0,on()},{2731,on(76)},{4513,off(76)}}),carrier=modRender(base,8000,{{0,on()},{2731,on(76)},{4513,off(76)}});
    std::vector<double> gains(8000);for(size_t i=0;i<gains.size();++i)gains[i]=.5+.125*lfoExpected(LfoShape::sine,(i+1237)*3/48000.)-.125*lfoExpected(LfoShape::triangle,(i+1237)*7/48000.);
    CHECK(gainError(x,carrier,gains)<4e-9);
    // Missing/invalid tempo holds the existing audible phase. Free LFO2 continues.
    l.lfos[0].synced=true;l.lfos[0].beats=.5f;
    e.setPatch(p);e.reset();base.reset();double phase1=0,phase2=0;
    for(auto tempo:{std::optional<double>{120},std::optional<double>{},std::optional<double>{NAN},std::optional<double>{19},std::optional<double>{401},std::optional<double>{120}})
    {
        const bool available=tempo&&std::isfinite(*tempo)&&*tempo>=20&&*tempo<=400;
        x=modRender(e,1200,{{0,on()}},64,tempo);carrier=modRender(base,1200,{{0,on()}},64);
        gains.resize(1200);for(auto& g:gains){g=.5+.125*std::sin(2*pi*phase1)-.125*lfoExpected(LfoShape::triangle,phase2);if(available)phase1=std::fmod(phase1+4/48000.,1.);phase2=std::fmod(phase2+7/48000.,1.);}
        CHECK(gainError(x,carrier,gains)<4e-9);CHECK(e.tempoUnavailable()==!available);
    }
    // A tempo change ramps frequency over 240 samples, then reaches exactly 8Hz.
    x=modRender(e,900,{},1,240);carrier=modRender(base,900,{},1);gains.resize(900);
    for(int i=0;i<900;++i){gains[size_t(i)]=.5+.125*std::sin(2*pi*phase1)-.125*lfoExpected(LfoShape::triangle,phase2);phase1=std::fmod(phase1+(4+4*std::min(i+1,240)/240.)/48000.,1.);phase2=std::fmod(phase2+7/48000.,1.);}
    CHECK(gainError(x,carrier,gains)<4e-9);
    // Unused synced clocks still report missing timing; disabled racks do not.
    p=flat();p.layers[0].lfos[1].synced=true;e.setPatch(p);e.reset();modRender(e,1);CHECK(e.tempoUnavailable());
    juce::AudioBuffer<float> empty(2,0),bad(1,10),large(2,513);juce::MidiBuffer midi;
    e.render(empty,midi,120);e.render(bad,midi,120);e.render(large,midi,120);CHECK(e.tempoUnavailable());
    modRender(e,1,{},1,120);CHECK(!e.tempoUnavailable());p.layers[0].enabled=false;e.setPatch(p);modRender(e,512);CHECK(!e.tempoUnavailable());
    std::cout<<"LFO_AUDIO maximum_gain_error="<<worst<<'\n';
}
void modulationSourcesAndDestinations()
{
    section("modulationSourcesAndDestinations");double worst=0;
    // Constant velocity1 routes must match literal physical settings. Each bank
    // is checked, with widths/positions restricted to their explicit meaning.
    for(auto bank:{ModBank::analog,ModBank::wavetable}) for(int t=1;t<=16;++t)
    {
        auto target=static_cast<ModTarget>(t);if((t==9||t==10)&&bank!=ModBank::analog)continue;if((t==11||t==12)&&bank!=ModBank::wavetable)continue;
        auto p=flat(bank==ModBank::analog?EngineKind::analog:EngineKind::wavetable);auto& l=p.layers[0];
        l.level=.4f;l.cutoffHz=1200;l.resonance=.2f;l.noiseLevel=.1f;
        for(auto& o:l.oscillators){o.level=.3f;o.position=.2f;o.waveform=Waveform::pulse;}
        l.oscillators[1].tune=7;auto expected=p;
        l.modulation[0]={true,ModSource::velocity,target,t<=2?ModBank::common:bank,.25f};
        auto& r=expected.layers[0];
        switch(target)
        {
            case ModTarget::level:r.level=.65f;break;case ModTarget::pan:r.pan=.25f;break;
            case ModTarget::osc1Level:r.oscillators[0].level=.55f;break;case ModTarget::osc2Level:r.oscillators[1].level=.55f;break;
            case ModTarget::osc1Tune:r.oscillators[0].tune=6;break;case ModTarget::osc2Tune:r.oscillators[1].tune=13;break;
            case ModTarget::osc1Detune:r.oscillators[0].detune=25;break;case ModTarget::osc2Detune:r.oscillators[1].detune=25;break;
            case ModTarget::osc1Width:r.oscillators[0].pulseWidth=.725f;break;case ModTarget::osc2Width:r.oscillators[1].pulseWidth=.725f;break;
            case ModTarget::osc1Position:r.oscillators[0].position=.45f;break;case ModTarget::osc2Position:r.oscillators[1].position=.45f;break;
            case ModTarget::noise:r.noiseLevel=.35f;break;case ModTarget::cutoff:r.cutoffHz=4800;break;
            case ModTarget::resonance:r.resonance=.4375f;break;case ModTarget::filterEnvelopeAmount:r.filterEnvelopeOctaves=2;break;
            default:break;
        }
        auto unmod=p;unmod.layers[0].modulation={};const auto actual=modSound(p),want=modSound(expected);
        const auto error=difference(actual,want);worst=std::max(worst,error);CHECK(error<3e-8);CHECK(difference(actual,modSound(unmod))>1e-6);
    }
    // Envelope sources have independently observable trajectories; note and
    // velocity sources use the sounding note's physical pitch/velocity.
    for(auto source:{ModSource::ampEnvelope,ModSource::filterEnvelope,ModSource::velocity,ModSource::note})
    {
        auto p=flat();p.layers[0].level=.5f;p.layers[0].ampEnvelope={.02f,0,0,1};p.layers[0].filterEnvelope={.04f,0,0,1};
        p.layers[0].modulation[0]={true,source,ModTarget::level,ModBank::common,.25f};auto base=p;base.layers[0].modulation={};base.layers[0].level=1;
        const auto actual=modSound(p,3000,48000,{{0,on(90,1,64/127.f)}}),carrier=modSound(base,3000,48000,{{0,on(90,1,64/127.f)}});
        // JUCE ADSR advances a float increment before sampling. Use a separate
        // public ADSR to preserve that rounding, with literal independent times.
        juce::ADSR envelope;envelope.setSampleRate(48000);envelope.setParameters({source==ModSource::ampEnvelope?.02f:.04f,0,1,0});envelope.noteOn();
        std::vector<double> gains(3000);for(auto& gain:gains){double value=source==ModSource::velocity?64/127.:source==ModSource::note?.5:envelope.getNextSample();gain=.5+.25*value;}
        CHECK(gainError(actual,carrier,gains)<4e-9);
    }
    // Depth edits ramp (including sign), repeated publishing cannot restart it.
    auto p=flat();p.layers[0].level=.5f;p.layers[0].modulation[0]={true,ModSource::velocity,ModTarget::level,ModBank::common,-.25f};
    SynthEngine e,base;e.setPatch(p);base.setPatch(flat());e.prepare(48000,512);base.prepare(48000,512);
    modRender(e,1000,{{0,on()}});modRender(base,1000,{{0,on()}});p.layers[0].modulation[0].amount=.25f;e.setPatch(p);
    std::vector<float> actual;for(int i=0;i<300;++i){e.setPatch(p);auto one=modRender(e,1,{},1);actual.push_back(one[0]);}
    auto carrier=modRender(base,300);std::vector<double> gains(300);for(int i=0;i<300;++i)gains[size_t(i)]=.25+.5*std::min(i+1,240)/240.;CHECK(gainError(actual,carrier,gains)<4e-9);
    // Sum before clamp: sixteen opposing contributions cancel without clipping
    // the intermediates. Equal-sign routes clamp the final supported value.
    for(float amount:{-1.f,1.f}){p=flat();p.layers[0].level=.5f;for(auto& route:p.layers[0].modulation)route={true,ModSource::velocity,ModTarget::level,ModBank::common,amount};auto reference=flat();reference.layers[0].level=amount<0?0.f:1.f;CHECK(difference(modSound(p),modSound(reference))==0);}
    for(size_t i=0;i<16;++i)p.layers[0].modulation[i].amount=i<8?1.f:-1.f;
    auto reference=flat();reference.layers[0].level=.5f;CHECK(difference(modSound(p),modSound(reference))==0);
    // Actual dynamic pitch and cutoff from both LFOs; square's first half-cycle
    // is a known constant so its resulting pitch/filter response is exact.
    for(auto target:{ModTarget::osc1Tune,ModTarget::cutoff})for(auto source:{ModSource::lfo1,ModSource::lfo2})
    {p=flat();p.layers[0].cutoffHz=1000;for(auto& clock:p.layers[0].lfos)clock={LfoShape::square,false,2,1};p.layers[0].modulation[0]={true,source,target,ModBank::analog,.25f};reference=p;reference.layers[0].modulation={};if(target==ModTarget::cutoff)reference.layers[0].cutoffHz=4000;else reference.layers[0].oscillators[0].tune=6;CHECK(difference(modSound(p),modSound(reference))==0);CHECK(difference(modSound(p,16000),modSound(reference,16000))>1e-5);}
    std::cout<<"MODULATION_PHYSICAL maximum_audio_error="<<worst<<'\n';
}
void qualifiedBanksAndRouteValidation()
{
    section("qualifiedBanksAndRouteValidation");auto p=flat();SynthEngine e,reference;e.setPatch(p);reference.setPatch(p);e.prepare(48000,512);reference.prepare(48000,512);
    modRender(e,100,{{0,on()}});modRender(reference,100,{{0,on()}});
    const auto reject=[&](const SynthPatch& bad){CHECK(!e.setPatch(bad));CHECK(e.patch()==p);};
    for(size_t layer=0;layer<4;++layer)
    {
        for(int value:{-1,99})
        { auto q=p;q.layers[layer].lfos[1].shape=static_cast<LfoShape>(value);reject(q);q=p;q.layers[layer].modulation[15].source=static_cast<ModSource>(value);reject(q);q=p;q.layers[layer].modulation[15].target=static_cast<ModTarget>(value);reject(q);q=p;q.layers[layer].modulation[15].bank=static_cast<ModBank>(value);reject(q); }
        for(float v:{-.01f,40.01f,NAN,INFINITY,-INFINITY}){auto q=p;q.layers[layer].lfos[0].rateHz=v;reject(q);}
        for(float v:{.049f}){auto q=p;q.layers[layer].lfos[0].rateHz=v;reject(q);}
        for(float v:{.0624f,16.01f,NAN,INFINITY,-INFINITY}){auto q=p;q.layers[layer].lfos[1].beats=v;reject(q);}
        for(float v:{-1.01f,1.01f,NAN,INFINITY,-INFINITY}){auto q=p;q.layers[layer].modulation[15].amount=v;reject(q);}
        auto q=p;q.layers[layer].modulation[0].enabled=true;reject(q);
        q.layers[layer].modulation[0].source=ModSource::velocity;reject(q);q=p;q.layers[layer].modulation[0]={true,ModSource::none,ModTarget::level,ModBank::common,1};reject(q);
    }
    CHECK(difference(modRender(e,2000),modRender(reference,2000))==0);
    for(bool enabled:{false,true})for(int t=0;t<=16;++t)for(auto bank:{ModBank::common,ModBank::analog,ModBank::wavetable})
    {
        auto q=p;q.layers[3].modulation[15]={enabled,ModSource::velocity,static_cast<ModTarget>(t),bank,.3f};
        const bool compatible=t==0?bank==ModBank::common:t<=2?bank==ModBank::common:t<=8?bank!=ModBank::common:t<=10?bank==ModBank::analog:t<=12?bank==ModBank::wavetable:bank!=ModBank::common;
        CHECK(e.setPatch(q)==(compatible&&(!enabled||t!=0)));
    }
    // Inactive edits must not even cause a rack fade; switching engine activates
    // the retained named position route, never pulse width or another control.
    p=flat();e.setPatch(p);e.reset();reference.reset();modRender(e,500,{{0,on()}});modRender(reference,500,{{0,on()}});
    p.layers[0].modulation[0]={true,ModSource::velocity,ModTarget::osc1Position,ModBank::wavetable,.5f};e.setPatch(p);
    CHECK(difference(modRender(e,1500),modRender(reference,1500))==0);CHECK(e.patch()==p);
    p.layers[0].engine=EngineKind::wavetable;e.setPatch(p);auto expected=p;expected.layers[0].modulation={};expected.layers[0].oscillators[0].position=.5f;
    e.reset();CHECK(difference(modRender(e,4096,{{0,on()}}),modSound(expected))==0);
    p.layers[0].engine=EngineKind::analog;e.setPatch(p);e.reset();CHECK(difference(modRender(e,4096,{{0,on()}}),modSound(flat()))==0);
}
void modulationClockEdges()
{
    // Ordinary edits, missing blocks, structural clock changes and their
    // coalescing all preserve the phase accumulated before the edit.
    for(double rate:{44100.,48000.,96000.})for(bool separated:{false,true})
    {
        auto p=flat();auto& l=p.layers[0];l.level=.5f;l.lfos[0]={LfoShape::sine,false,4,.5f};
        l.modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.25f};
        SynthEngine a,carrier;a.setPatch(p);carrier.setPatch(flat());a.prepare(rate,512);carrier.prepare(rate,512);
        modRender(a,1379,{{0,on()}},64,120,separated);modRender(carrier,1379,{{0,on()}},64);
        l.lfos[0].synced=true;a.setPatch(p);const int ramp=int(std::ceil(rate*.005));const int n=ramp*2+101;
        auto x=modRender(a,n,{},1,120,separated),y=modRender(carrier,n);
        std::vector<double> gains(size_t(n),0);double phase=1379*4/rate;
        for(int i=0;i<n;++i){const double fade=i<ramp?1-(i+1)/double(ramp):std::min(i-ramp+1,ramp)/double(ramp);gains[size_t(i)]=fade*(.5+.25*std::sin(2*pi*phase));phase+=4/rate;}
        CHECK(gainError(x,y,gains)<4e-9);
        // Changing the stored free rate while synced has no audio effect.
        SynthEngine twin;twin.setPatch(p);twin.prepare(rate,512);modRender(twin,1379+n,{{0,on()}},64,120,separated);
        l.lfos[0].rateHz=37;a.setPatch(p);CHECK(difference(modRender(a,900,{},64,120,separated),modRender(twin,900,{},64,120,separated))==0);
        modRender(carrier,900);phase+=900*4/rate;
        // Period .5 -> .25 resolves 4 -> 8 Hz and ramps for exactly five ms.
        l.lfos[0].beats=.25f;a.setPatch(p);x=modRender(a,700,{},64,120,separated);y=modRender(carrier,700);gains.resize(700);
        for(int i=0;i<700;++i){gains[size_t(i)]=.5+.25*std::sin(2*pi*phase);phase+=(4+4*std::min(i+1,ramp)/double(ramp))/rate;}CHECK(gainError(x,y,gains)<4e-9);
        // Invalid separated shapes/aliases and invalid mixed sizes consume no
        // clock or MIDI state and do not replace missing-tempo status.
        SynthEngine control;control.setPatch(p);control.prepare(rate,512);a.reset();
        modRender(a,333,{{0,on()}},64,{},separated);modRender(control,333,{{0,on()}},64,{},separated);CHECK(a.tempoUnavailable());
        juce::AudioBuffer<float> shortBuffer(2,1),zero(2,0);juce::MidiBuffer stop;stop.addEvent(juce::MidiMessage::midiStop(),0);
        std::array<juce::AudioBuffer<float>*,4> aliases{&shortBuffer,&shortBuffer,&shortBuffer,&shortBuffer};std::array<float,1> gain;
        a.renderLayers(aliases,gain,stop,120);aliases.fill(&zero);a.renderLayers(aliases,{},stop,120);CHECK(a.tempoUnavailable());
        CHECK(difference(modRender(a,2000,{},512,120,separated),modRender(control,2000,{},1,120,separated))==0);
        // New lfo2/shape/target selections switch at silence and fade back up
        // using its independently advanced phase, despite repeated publication.
        p=flat();p.layers[0].level=.5f;p.layers[0].lfos[0].rateHz=3;p.layers[0].lfos[1]={LfoShape::triangle,false,7,1};p.layers[0].modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.25f};
        a.setPatch(p);a.reset();carrier.reset();modRender(a,997,{{0,on()}});modRender(carrier,997,{{0,on()}});
        p.layers[0].modulation[0].source=ModSource::lfo2;a.setPatch(p);x.clear();for(int i=0;i<ramp*2+50;++i){a.setPatch(p);auto sample=modRender(a,1);x.push_back(sample[0]);}
        y=modRender(carrier,int(x.size()));gains.resize(x.size());
        for(size_t i=0;i<gains.size();++i){const double fade=int(i)<ramp?1-(i+1)/double(ramp):std::min(int(i)-ramp+1,ramp)/double(ramp);const double value=int(i)<ramp?lfoExpected(LfoShape::sine,(997+i)*3/rate):lfoExpected(LfoShape::triangle,(997+i)*7/rate);gains[i]=fade*(.5+.25*value);}CHECK(gainError(x,y,gains)<4e-9);
        // Cancel a pending route switch and ramp back from the current fade.
        a.reset();carrier.reset();modRender(a,997,{{0,on()}});modRender(carrier,997,{{0,on()}});
        auto changed=p;changed.layers[0].modulation[0].target=ModTarget::pan;a.setPatch(changed);modRender(a,37);modRender(carrier,37);a.setPatch(p);
        x=modRender(a,ramp+30);y=modRender(carrier,ramp+30);gains.resize(x.size());const double start=1-37/double(ramp);
        for(size_t i=0;i<gains.size();++i)gains[i]=(start+(1-start)*std::min(int(i)+1,ramp)/double(ramp))*(.5+.25*lfoExpected(LfoShape::triangle,(1034+i)*7/rate));CHECK(gainError(x,y,gains)<4e-9);
    }
    // Validate source frequencies physically using local audio periods for
    // every waveform and both independent clocks, rather than a status getter.
    double worstCents=0;
    for(double rate:{44100.,48000.,96000.})for(auto shape:{LfoShape::sine,LfoShape::triangle,LfoShape::saw,LfoShape::square})for(auto source:{ModSource::lfo1,ModSource::lfo2})
    {
        auto p=flat();for(auto& c:p.layers[0].lfos)c={shape,false,.05f,1};p.layers[0].modulation[0]={true,source,ModTarget::osc1Tune,ModBank::analog,.125f};
        const auto x=modSound(p,int(rate*.15),rate);const auto measured=measuredPitch(x,rate,rate*.1);const auto expected=69+3*lfoExpected(shape,measured.second*double(.05f)/rate);
        const auto error=cents(measured.first,expected);worstCents=std::max(worstCents,error);CHECK(error<.1);
    }
    std::cout<<"LFO_PITCH maximum_cents="<<worstCents<<'\n';
    // Endpoint frequency/period/tempo values are accepted and finite. The slow
    // free and synced endpoints agree with known physical periods in audio.
    for(float hz:{.05f,40.f}){auto p=flat();p.layers[0].level=.5f;p.layers[0].lfos[0].rateHz=hz;p.layers[0].modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.25f};auto x=modSound(p,5000);auto y=modSound(flat(),5000);std::vector<double> gains(5000);for(size_t i=0;i<gains.size();++i)gains[i]=.5+.25*std::sin(2*pi*i*double(hz)/48000);CHECK(gainError(x,y,gains)<4e-9);}
    for(float beats:{.0625f,16.f})for(double bpm:{20.,400.}){auto p=flat();p.layers[0].level=.5f;p.layers[0].lfos[0].synced=true;p.layers[0].lfos[0].beats=beats;p.layers[0].modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.25f};auto x=modSound(p,5000,48000,{{0,on()}},bpm);auto y=modSound(flat(),5000);std::vector<double> gains(5000);for(size_t i=0;i<gains.size();++i)gains[i]=.5+.25*std::sin(2*pi*i*bpm/(60*beats*48000));CHECK(gainError(x,y,gains)<4e-9);}
}
void modulationClampEdges()
{
    // All destinations clamp at the specified physical bounds, after summing
    // sixteen contributions. Extreme cutoff still obeys the sample-rate limit.
    for(auto bank:{ModBank::analog,ModBank::wavetable})for(int t=1;t<=16;++t)for(float sign:{-1.f,1.f})
    {
        if((t==9||t==10)&&bank!=ModBank::analog)continue;if((t==11||t==12)&&bank!=ModBank::wavetable)continue;
        auto p=flat(bank==ModBank::analog?EngineKind::analog:EngineKind::wavetable);auto& l=p.layers[0];l.level=.5f;l.cutoffHz=900;l.resonance=.5f;l.noiseLevel=.2f;l.filterEnvelopeOctaves=0;
        for(auto& o:l.oscillators){o.waveform=Waveform::pulse;o.position=.5f;o.level=.5f;}
        auto expected=p;auto& r=expected.layers[0];const auto target=static_cast<ModTarget>(t);for(auto& route:l.modulation)route={true,ModSource::velocity,target,t<=2?ModBank::common:bank,sign};
        const bool high=sign>0;switch(target)
        {
            case ModTarget::level:r.level=high?1.f:0.f;break;case ModTarget::pan:r.pan=sign;break;
            case ModTarget::osc1Level:r.oscillators[0].level=high?1.f:0.f;break;case ModTarget::osc2Level:r.oscillators[1].level=high?1.f:0.f;break;
            case ModTarget::osc1Tune:r.oscillators[0].tune=24*sign;break;case ModTarget::osc2Tune:r.oscillators[1].tune=24*sign;break;
            case ModTarget::osc1Detune:r.oscillators[0].detune=100*sign;break;case ModTarget::osc2Detune:r.oscillators[1].detune=100*sign;break;
            case ModTarget::osc1Width:r.oscillators[0].pulseWidth=high?.95f:.05f;break;case ModTarget::osc2Width:r.oscillators[1].pulseWidth=high?.95f:.05f;break;
            case ModTarget::osc1Position:r.oscillators[0].position=high?1.f:0.f;break;case ModTarget::osc2Position:r.oscillators[1].position=high?1.f:0.f;break;
            case ModTarget::noise:r.noiseLevel=high?1.f:0.f;break;case ModTarget::cutoff:r.cutoffHz=high?20000.f:20.f;break;
            case ModTarget::resonance:r.resonance=high?.95f:0.f;break;case ModTarget::filterEnvelopeAmount:r.filterEnvelopeOctaves=8*sign;break;
            default:break;
        }
        CHECK(difference(modSound(p,2000,8000),modSound(expected,2000,8000))<4e-8);
    }
    // Zero depths retain exact default DSP including default early release.
    auto p=flat();p.layers[0].ampEnvelope={.02f,.1f,.1f,0};auto q=p;
    for(auto& route:q.layers[0].modulation)route={true,ModSource::lfo1,ModTarget::cutoff,ModBank::analog,0};
    CHECK(difference(modSound(p,9000,48000,{{0,on()},{111,off()}}),modSound(q,9000,48000,{{0,on()},{111,off()}}))==0);
}

void modulationLifecycleAndCapacity()
{
    section("modulationLifecycleAndCapacity"); modulationClockEdges(); modulationClampEdges();
    for(double rate:{44100.,48000.,96000.})
    {
        auto p=flat();p.layers[0].level=.5f;p.layers[0].lfos[0].rateHz=11;p.layers[0].modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.25f};
        SynthEngine a,b;a.setPatch(p);b.setPatch(p);a.prepare(rate,512);b.prepare(rate,512);
        const std::vector<Event> events{{0,on()},{317,juce::MidiMessage::midiStop()},{317,on()}};
        auto x=modRender(a,2048,events,64),y=modRender(b,1731,{{0,on()}},512);CHECK(difference(std::vector<float>(x.begin()+317,x.end()),y)==0);
        a.reset();b.reset();CHECK(difference(modRender(a,2048,{{0,on()}},1),modRender(b,2048,{{0,on()}},512,{},true))<4e-9);
        a.prepare(rate,512);b.reset();CHECK(difference(modRender(a,1000,{{0,on()}}),modRender(b,1000,{{0,on()}}))==0);
        // Rate changes are five-ms frequency ramps with phase continuity.
        SynthEngine carrier;carrier.setPatch(flat());carrier.prepare(rate,512);a.reset();modRender(a,773,{{0,on()}});modRender(carrier,773,{{0,on()}});
        p.layers[0].lfos[0].rateHz=23;a.setPatch(p);const int ramp=int(std::ceil(rate*.005));x=modRender(a,ramp+400);y=modRender(carrier,ramp+400);double phase=773*11/rate;std::vector<double> gains(x.size());
        for(size_t i=0;i<gains.size();++i){gains[i]=.5+.25*std::sin(2*pi*phase);phase+=(11+12*std::min(int(i)+1,ramp)/double(ramp))/rate;}CHECK(gainError(x,y,gains)<4e-9);
        // Retargeting route structure fades the existing clock audio at zero.
        a.reset();carrier.reset();auto unchanged=p;SynthEngine old;old.setPatch(p);old.prepare(rate,512);modRender(a,1000,{{0,on()}});modRender(old,1000,{{0,on()}});
        p.layers[0].modulation[0].source=ModSource::lfo2;a.setPatch(p);x=modRender(a,ramp);y=modRender(old,ramp);gains.resize(size_t(ramp));for(int i=0;i<ramp;++i)gains[size_t(i)]=1-(i+1)/double(ramp);CHECK(gainError(x,y,gains)<4e-9);
        CHECK(std::abs(x.back())<1e-12);
        // Cancel/coalesce a pending route edit without restarting an identical one.
        a.setPatch(unchanged);a.reset();old.reset();modRender(a,300,{{0,on()}});modRender(old,300,{{0,on()}});a.setPatch(p);for(int i=0;i<ramp;++i){a.setPatch(p);modRender(a,1);}
        a.setPatch(unchanged);a.reset();old.reset();CHECK(difference(modRender(a,1000,{{0,on()}}),modRender(old,1000,{{0,on()}}))==0);
    }
    // Sustain-zero early release keeps its instantaneous slope even with active
    // modulation and identical patch updates. Mono note tracking follows glide.
    auto p=flat();p.layers[0].ampEnvelope={.2f,.2f,.1f,0};p.layers[0].level=.5f;p.layers[0].modulation[0]={true,ModSource::velocity,ModTarget::level,ModBank::common,.25f};
    SynthEngine a,b;a.setPatch(p);b.setPatch(p);a.prepare(48000,512);b.prepare(48000,512);modRender(a,1000,{{0,on()},{900,off()}});modRender(b,1000,{{0,on()},{900,off()}});
    std::vector<float> x;for(int i=0;i<1000;++i){a.setPatch(p);auto one=modRender(a,1);x.push_back(one[0]);}CHECK(energy(x)>1e-10);CHECK(difference(x,modRender(b,1000))==0);
    p=flat();p.performance={VoiceMode::monophonic,.1f,true};p.layers[0].level=.5f;p.layers[0].modulation[0]={true,ModSource::note,ModTarget::level,ModBank::common,.25f};auto base=p;base.layers[0].modulation={};base.layers[0].level=1;
    a.setPatch(p);a.reset();b.setPatch(base);b.reset();modRender(a,1000,{{0,on(60)}});modRender(b,1000,{{0,on(60)}});x=modRender(a,6000,{{0,on(120)}});auto y=modRender(b,6000,{{0,on(120)}});std::vector<double> gains(6000);const int glide=int(std::ceil(48000*double(p.performance.glideSeconds)));for(int i=0;i<6000;++i)gains[size_t(i)]=.5+.25*std::min(i+1,glide)/double(glide);CHECK(gainError(x,y,gains)<4e-9);
    // Full fixed capacity, all destinations, four racks, sixteen notes; both
    // render APIs, changes/reset/Stop are checked inside the CRT allocation guard.
    p=flat();for(auto& l:p.layers){l=p.layers[0];l.enabled=true;for(int r=0;r<16;++r){auto target=static_cast<ModTarget>(r+1);auto bank=r<2?ModBank::common:(r==10||r==11?ModBank::wavetable:ModBank::analog);l.modulation[size_t(r)]={true,static_cast<ModSource>(r%6+1),target,bank,r%2?1.f:-1.f};}l.lfos[1].synced=true;}
    a.setPatch(p);a.reset();juce::AudioBuffer<float> out(2,512);juce::MidiBuffer notes,empty,stop;for(int n=48;n<64;++n)notes.addEvent(on(n),0);stop.addEvent(juce::MidiMessage::midiStop(),113);
    std::array<juce::AudioBuffer<float>,4> buffers;std::array<juce::AudioBuffer<float>*,4> outputs;for(size_t i=0;i<4;++i){buffers[i].setSize(2,512);outputs[i]=&buffers[i];}std::array<float,512> master;
    bool accepted=true,finite=true;
#if defined(_MSC_VER) && defined(_DEBUG)
    auto oldHook=_CrtSetAllocHook(allocationHook);allocations=0;tracking=true;
#endif
    a.render(out,notes,120);
    for(int i=0;i<48;++i){p.layers[size_t(i%4)].modulation[size_t(i%16)].amount=i%2?.5f:-.5f;accepted&=a.setPatch(p);a.render(out,empty,i%2?std::optional<double>{90}:std::nullopt);a.renderLayers(outputs,master,empty,120);for(int s=0;s<512;++s)finite&=std::isfinite(out.getSample(0,s));}
    const auto count=a.activeNoteCount();a.render(out,stop);a.reset();a.renderLayers(outputs,master,notes,120);
#if defined(_MSC_VER) && defined(_DEBUG)
    tracking=false;_CrtSetAllocHook(oldHook);CHECK(allocations==0);std::cout<<"MODULATION allocations="<<allocations<<'\n';
#endif
    CHECK(accepted);CHECK(finite);CHECK(count==16);CHECK(a.activeNoteCount()==16);
}

void finiteAndAllocation()
{
    section("All high-note engines stay finite; default four-layer polyphony has headroom; render/reset allocate zero");
    for (double rate : {44100.0, 48000.0, 96000.0})
        for (auto kind : {EngineKind::analog, EngineKind::wavetable})
            for (auto wave : {Waveform::sine, Waveform::triangle, Waveform::saw, Waveform::pulse})
            {
                SynthEngine e; SynthPatch p;
                for (auto& l : p.layers) { l.enabled = true; l.engine = kind; l.resonance = 0.95f; l.filterEnvelopeOctaves = 8; for (auto& o : l.oscillators) { o.waveform = wave; o.tune = 24; o.detune = 100; o.position = 1; } }
                e.setPatch(p); e.prepare(rate, 256);
                std::vector<Event> notes; for (int n = 112; n < 128; ++n) notes.emplace_back(0, on(n));
                auto x = render(e, 1024, notes);
                CHECK(e.activeNoteCount() == 16);
                CHECK(std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 100; }));
            }
    SynthEngine e; SynthPatch p; for (auto& l : p.layers) l.enabled = true;
    e.setPatch(p); e.prepare(48000, 512);
    std::vector<Event> notes; for (int n = 48; n < 64; ++n) notes.emplace_back(0, on(n));
    auto x = render(e, 24000, notes, 512);
    auto peak = *std::max_element(x.begin(), x.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
    std::cout << "DEFAULT_POLYPHONIC peak=" << std::abs(peak) << " rms=" << std::sqrt(energy(x)) << '\n';
    CHECK(std::abs(peak) < 1); CHECK(energy(x) > 1e-6);
    juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi;
    std::vector<uint8_t> payload(32768, 42);
    midi.addEvent(juce::MidiMessage::createSysExMessage(payload.data(), int(payload.size())), 10);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 12000), 20);
    midi.addEvent(on(127), 30); midi.addEvent(off(127), 100);
    p.masterLevel = 0.2f; e.setPatch(p);
    SynthEngine cold; cold.setPatch(p); cold.prepare(48000, 512);
    std::array<juce::AudioBuffer<float>, 4> separatedBuffers;
    std::array<juce::AudioBuffer<float>*, 4> separatedOutputs;
    for (size_t i = 0; i < 4; ++i) { separatedBuffers[i].setSize(2, 512); separatedOutputs[i] = &separatedBuffers[i]; }
    std::array<float, 512> masterGains;
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations = 0; auto oldHook = _CrtSetAllocHook(allocationHook); tracking = true;
    cold.render(audio, midi); cold.reset();
    for (int i = 0; i < 20; ++i) e.render(audio, midi);
    e.reset(); e.render(audio, midi);
    cold.reset(); cold.renderLayers(separatedOutputs, masterGains, midi);
    for (int i = 0; i < 20; ++i) { e.setPatch(p); e.renderLayers(separatedOutputs, masterGains, midi); }
    e.reset(); e.renderLayers(separatedOutputs, masterGains, midi);
    tracking = false; _CrtSetAllocHook(oldHook);
    std::cout << "PREPARED_RENDER_RESET allocations=" << allocations << '\n'; CHECK(allocations == 0);
#else
    CHECK(false); // This acceptance executable requires the MSVC Debug allocation guard.
#endif
}
// Development-only oracle: a centred, normalized 513-tap Blackman-windowed
// sinc at 0.45 of the target sample rate filters an 8x periodic source before
// decimation. Analytic padding avoids startup transients and phase mismatch.
void qualityReferenceSensitivity()
{
    section("quality_reference_sensitivity");
    constexpr int count=4096,factor=8,half=256;
    std::array<double,2*half+1> kernel {}; double sum=0;
    for(int m=-half;m<=half;++m){const double cutoff=.45/factor;
        const double sinc=m==0?2*cutoff:std::sin(2*pi*cutoff*m)/(pi*m);
        const double window=.42+.5*std::cos(pi*m/half)+.08*std::cos(2*pi*m/half);
        kernel[std::size_t(m+half)]=sinc*window;sum+=sinc*window;}
    for(auto& k:kernel)k/=sum;
    auto source=[](int n,bool saw){const double phase=600.0*n/(4096*8);
        return saw?2*(phase-std::floor(phase))-1:std::sin(2*pi*phase);};
    auto relative=[](const std::vector<float>& observed,const std::vector<float>& reference){
        if(energy(reference)<1e-12)return std::numeric_limits<double>::infinity();
        double error=0;for(std::size_t i=0;i<observed.size();++i){const auto d=double(observed[i])-reference[i];error+=d*d;}
        return std::sqrt(error/(observed.size()*energy(reference)));};
    double negative=0,positive=0;
    for(bool saw:{false,true}){std::vector<float> naive(count),reference(count);
        for(int i=0;i<count;++i){naive[std::size_t(i)]=float(source(i*factor,saw));double filtered=0;
            for(int m=-half;m<=half;++m)filtered+=kernel[std::size_t(m+half)]*source(i*factor-m,saw);
            reference[std::size_t(i)]=float(filtered);}
        CHECK(energy(reference)>.01);const auto residual=relative(naive,reference);
        if(saw){positive=residual;CHECK(residual>.1);CHECK(relative(naive,naive)==0);}else{negative=residual;CHECK(residual<.001);}}
    CHECK(!std::isfinite(relative(std::vector<float>(count),std::vector<float>(count))));
    std::cout<<"QUALITY_REFERENCE alias_positive_relative_rms="<<positive<<" clean_sine_negative_relative_rms="<<negative<<" silence_rejected=1 factor=8 taps=513 cutoff_target_fraction=.45 centred_alignment=1\n";
}
void qualityVoiceArticulation()
{
    section("quality_voice_articulation");
    auto p=flat();for(auto& layer:p.layers){layer=p.layers[0];layer.enabled=true;layer.level=.15f;layer.ampEnvelope={.002f,.05f,.03f,.7f};}
    for(double rate:{44100.,48000.,96000.}){
        SynthEngine a,b;CHECK(a.setPatch(p)&&b.setPatch(p));CHECK(a.prepare(rate,512)&&b.prepare(rate,512));
        std::vector<Event> notes;for(int n=48;n<64;++n)notes.emplace_back(n-48,on(n));
        notes.emplace_back(400,on(90));notes.emplace_back(600,on(60));notes.emplace_back(900,juce::MidiMessage::allNotesOff(1));
        const auto coarse=render(a,int(rate*.15),notes,512),fine=render(b,int(rate*.15),notes,64);
        CHECK(energy(coarse)>1e-8);CHECK(difference(coarse,fine)<=1e-6);CHECK(a.activeNoteCount()==0&&b.activeNoteCount()==0);
        CHECK(energy(render(a,512))==0);a.reset();CHECK(difference(render(a,4096,{{17,on(60)},{600,off(60)}}),render(b,4096,{{17,on(60)},{600,off(60)}}))<=1e-6);
    }
}
}
void qualityClockAndFilterReference()
{
    section("quality_latency_and_partition: independent convolution, physical notes/envelopes/LFO");
    const auto coefficients=juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod(.08f,-100.f);
    const auto order=coefficients->getFilterOrder();const auto* taps=coefficients->getRawCoefficients();
    for(double rate:{44100.,48000.,96000.})for(bool impulse:{false,true})
    {
        auto p=flat();auto& l=p.layers[0];
        if(impulse){l.oscillators[0].level=0;l.noiseLevel=1;}
        else{l.ampEnvelope.attack=.013f;l.ampEnvelope.decay=.027f;l.ampEnvelope.release=.031f;l.ampEnvelope.sustain=.4f;l.level=.5f;l.lfos[0].rateHz=5;l.modulation[0]={true,ModSource::lfo1,ModTarget::level,ModBank::common,.25f};}
        SynthEngine high,reference;CHECK(high.setPatch(p)&&reference.setPatch(p));
        CHECK(high.prepare(rate,512,RenderQuality::higher)&&reference.prepare(rate*2,1024));
        const int release=impulse?18:1237;
        const int count=static_cast<int>(std::ceil(rate*.12));
        const auto actual=render(high,count,{{17,on()},{release,off()}},64);
        const auto raw=render(reference,count*2,{{34,on()},{release*2,off()}},1024);
        std::vector<float> expected(actual.size());
        for(size_t n=0;n<expected.size();++n)
        {double sum=0;for(size_t k=0;k<=order;++k){const auto source=std::int64_t(n*2)-1-std::int64_t(k);if(source>=0)sum+=raw[static_cast<size_t>(source)]*double(taps[k]);}expected[n]=static_cast<float>(sum);}
        CHECK(high.latencySamples()==static_cast<int>((order+2)/4));
        CHECK(energy(expected)>1e-12 && difference(actual,expected)<=1e-6);
        CHECK(high.activeNoteCount()==0 && reference.activeNoteCount()==0);
        std::cout<<"QUALITY_CLOCK rate="<<rate<<" impulse="<<impulse<<" actual_high_rate_reference="<<rate*2<<" fir_order="<<order<<" host_latency="<<high.latencySamples()<<" convolution_max_error="<<difference(actual,expected)<<'\n';
    }
    for(double rate:{8000.,384000.}){SynthEngine boundary;CHECK(boundary.setPatch(flat()));CHECK(boundary.prepare(rate,65536,RenderQuality::higher));CHECK(boundary.effectiveQuality()==RenderQuality::higher&&boundary.latencySamples()==21);}
    section("quality_higher_callback_allocations: four layers, sixteen notes, borrowed MIDI");
    auto p=distinctLayers();SynthEngine e;CHECK(e.setPatch(p)&&e.prepare(48000,512,RenderQuality::higher));
    juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer notes,empty;notes.ensureSize(32768);
    for(int n=48;n<64;++n)notes.addEvent(on(n),n-48);
    std::array<juce::AudioBuffer<float>,4> layers;std::array<juce::AudioBuffer<float>*,4> pointers;std::array<float,512> gains;
    for(size_t n=0;n<4;++n){layers[n].setSize(2,512);pointers[n]=&layers[n];}
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations=0;frees=0;auto old=_CrtSetAllocHook(allocationHook);tracking=true;
    e.render(audio,notes);for(int n=0;n<32;++n)e.renderLayers(pointers,gains,empty);e.reset();e.render(audio,notes);
    tracking=false;_CrtSetAllocHook(old);CHECK(allocations==0 && frees==0);CHECK(e.activeNoteCount()==16);
    std::cout<<"QUALITY_HIGHER_CORE allocations="<<allocations<<" frees="<<frees<<" layers=4 notes="<<e.activeNoteCount()<<'\n';
#else
    CHECK(false);
#endif
}
template<class Engine>
void preparedQualityContract()
{
    section("quality_prepared_mode_truth");
    if constexpr (requires(Engine& e) { e.effectiveQuality(); e.latencySamples(); })
    {
        using Quality = decltype(std::declval<Engine&>().effectiveQuality());
        const auto realtime = static_cast<Quality>(0), higher = static_cast<Quality>(1);
        Engine a, control;
        CHECK(a.setPatch(flat()) && control.setPatch(flat()));
        CHECK(a.prepare(48000,512) && control.prepare(48000,512));
        CHECK(a.effectiveQuality()==realtime && a.latencySamples()==0);
        CHECK(difference(render(a,1024,{{17,on()}}),render(control,1024,{{17,on()}}))==0);
        CHECK(a.activeNoteCount()==1);
        CHECK(!a.prepare(48000,512,static_cast<Quality>(255)));
        CHECK(!a.prepare(7999,512,higher) && !a.prepare(48000,65537,higher));
        CHECK(a.effectiveQuality()==realtime && a.activeNoteCount()==1);
        CHECK(difference(render(a,1024),render(control,1024))==0);
        CHECK(a.prepare(48000,512,higher));
        CHECK(a.effectiveQuality()==higher && a.latencySamples()>0 && a.activeNoteCount()==0);
        section("quality_latency_and_partition");
        Engine b; CHECK(b.setPatch(flat()) && b.prepare(48000,512,higher));
        const std::vector<Event> events{{17,on()},{2047,off()}};
        const auto coarse=render(a,4096,events,512), fine=render(b,4096,events,64);
        CHECK(energy(coarse)>1e-8 && difference(coarse,fine)<=1e-6);
        auto layered=distinctLayers();a.setPatch(layered);b.setPatch(layered);a.reset();b.reset();
        const auto separated=renderSeparated(a,1024,{{17,on()}},64);
        CHECK(difference(separated.mix(0),render(b,1024,{{17,on()}},512))==0);
        CHECK(std::all_of(separated.gains.begin(),separated.gains.end(),[&](float g){return g==layered.masterLevel;}));
        layered.masterLevel=.21f;CHECK(a.setPatch(layered)&&b.setPatch(layered));
        const auto ramp=renderSeparated(a,1024,{},64);
        CHECK(difference(ramp.mix(0),render(b,1024,{},512))==0);
        const auto latency=static_cast<size_t>(a.latencySamples());
        CHECK(ramp.gains[latency-1]==.63f);
        CHECK(std::abs(ramp.gains[latency]-(.63f+(.21f-.63f)/480.f))<1e-6);
        CHECK(ramp.gains[latency+239]>.21f && ramp.gains[latency+240]==.21f);
        CHECK(a.prepare(48000,512) && a.effectiveQuality()==realtime && a.latencySamples()==0);
    }
    else CHECK(false); // Missing prepared-quality behavior is a runnable RED.
}
SynthPatch modern(EngineKind kind = EngineKind::analog)
{ auto p = flat(kind); p.layers[0].algorithmRevision = 2; return p; }
std::uint64_t fingerprint(const std::vector<float>& x)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (auto v : x) hash = (hash ^ std::bit_cast<std::uint32_t>(v)) * 1099511628211ull;
    return hash;
}
double peakNear(const std::vector<float>& x, int centre, int half)
{
    float peak = 0;
    for (int i = std::max(0, centre - half); i < std::min(int(x.size()), centre + half); ++i) peak = std::max(peak, std::abs(x[size_t(i)]));
    return peak;
}
double maximumStep(const std::vector<float>& x, int from, int to)
{
    double step = 0;
    for (int i = std::max(1, from); i < std::min(int(x.size()), to); ++i) step = std::max(step, std::abs(double(x[size_t(i)]) - x[size_t(i - 1)]));
    return step;
}
// Saw harmonics present within 6 dB of 1/h, energy outside drift-tolerant harmonic windows,
// the fundamental's share of the power and the spectral centroid.
struct HarmonicReport { double topHz = 0, alias = 1, fundamental = 0, centroid = 0; };
HarmonicReport harmonics(const std::vector<float>& x, double rate, double f0)
{
    constexpr int order = 16, size = 1 << order;
    juce::dsp::FFT fft(order); std::vector<float> data(2 * size);
    const size_t start = x.size() - size;
    for (int i = 0; i < size; ++i)
    {
        const double t = double(i) / (size - 1);
        data[size_t(i)] = float(x[start + size_t(i)] * (.35875 - .48829 * std::cos(2 * pi * t) + .14128 * std::cos(4 * pi * t) - .01168 * std::cos(6 * pi * t)));
    }
    fft.performFrequencyOnlyForwardTransform(data.data(), true);
    const double bin = rate / size;
    std::vector<double> power(size / 2 + 1, 0.0); std::vector<bool> used(power.size(), false);
    double total = 0, harmonic = 0, fundamental = 0, moment = 0;
    for (int k = 8; k <= size / 2; ++k)
    { power[size_t(k)] = double(data[size_t(k)]) * data[size_t(k)]; total += power[size_t(k)]; moment += power[size_t(k)] * k * bin; }
    HarmonicReport report;
    for (int h = 1; h * f0 < rate / 2; ++h)
    {
        const double centre = h * f0 / bin;
        const int spread = 6 + int(std::ceil(centre * .004));
        double band = 0;
        for (int k = std::max(8, int(centre) - spread); k <= std::min(size / 2, int(centre) + spread); ++k)
            if (!used[size_t(k)]) { band += power[size_t(k)]; used[size_t(k)] = true; }
        harmonic += band;
        if (h == 1) fundamental = band;
        if (fundamental > 0 && band >= .25 * fundamental / (double(h) * h)) report.topHz = h * f0;
    }
    report.alias = std::max(total - harmonic, 1e-30) / std::max(total, 1e-30);
    report.fundamental = fundamental / std::max(total, 1e-30);
    report.centroid = moment / std::max(total, 1e-30);
    return report;
}
double bandPower(const std::vector<float>& x, double rate, double low, double high)
{
    constexpr int order = 13, size = 1 << order;
    juce::dsp::FFT fft(order); std::vector<float> data(2 * size); double sum = 0; int count = 0;
    for (size_t start = 0; start + size <= x.size(); start += size / 2)
    {
        for (int i = 0; i < size; ++i) data[size_t(i)] = float(x[start + size_t(i)] * (.5 - .5 * std::cos(2 * pi * i / size)));
        std::fill(data.begin() + size, data.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform(data.data(), true);
        for (int k = int(low / rate * size); k <= int(high / rate * size); ++k) { sum += double(data[size_t(k)]) * data[size_t(k)]; ++count; }
    }
    return sum / std::max(count, 1);
}
// The wavetable warmth fields far from neutral, where they must not act: every revision-1 layer and every analog one.
void warmUp(LayerSettings& l)
{
    for (auto& o : l.oscillators) { o.unison = 7; o.unisonCents = 60; }
    l.unisonWidth = .3f; l.drift = 1; l.filterType = FilterType::vowel; l.vowelSize = 1.7f; l.attackCurve = -.8f;
}
std::vector<std::uint64_t> kernelFingerprints(AlgorithmRevision revision, bool warm = false)
{
    std::vector<std::uint64_t> hashes;
    for (auto kind : {EngineKind::analog, EngineKind::wavetable})
    {
        auto p = flat(kind); auto& l = p.layers[0];
        l.oscillators[0] = {Waveform::saw, 0, .7f, 0, -5, .6f, .4f}; l.oscillators[1] = {Waveform::pulse, 1, .3f, -12, 6, .4f, .3f};
        l.noiseLevel = .05f; l.cutoffHz = 900; l.resonance = .6f; l.filterEnvelopeOctaves = 3;
        l.ampEnvelope = {.004f, .2f, .15f, .6f}; l.filterEnvelope = {.002f, .3f, .2f, .2f};
        l.lfos[0].rateHz = 3; l.modulation[0] = {true, ModSource::lfo1, ModTarget::cutoff, kind == EngineKind::analog ? ModBank::analog : ModBank::wavetable, .1f};
        p.layers[1] = l; p.layers[1].engine = kind == EngineKind::analog ? EngineKind::wavetable : EngineKind::analog; p.layers[1].filterMode = FilterMode::bandpass; p.layers[1].pan = -.5f;
        p.layers[1].modulation[0].bank = p.layers[1].engine == EngineKind::analog ? ModBank::analog : ModBank::wavetable;
        for (auto& layer : p.layers) layer.algorithmRevision = revision;
        if (warm) for (auto& layer : p.layers) if (revision == legacyAlgorithmRevision || layer.engine == EngineKind::analog) warmUp(layer);
        for (auto performance : {PerformanceSettings {}, PerformanceSettings {VoiceMode::monophonic, .03f, true}})
        {
            p.performance = performance;
            SynthEngine e; CHECK(e.setPatch(p) && e.prepare(48000, 256));
            hashes.push_back(fingerprint(render(e, 24000, {{0, on(48)}, {50, on(55)}, {3000, on(60)}, {9000, off(48)}, {12000, on(48, 1, .4f)}, {20000, off(55)}})));
        }
    }
    return hashes;
}
void legacyKernelFingerprint()
{
    section("legacy_kernel_fingerprint: revision 1 voices stay bit-identical");
    const auto hashes = kernelFingerprints(legacyAlgorithmRevision);
    // Captured from the accepted revision-1 engine before the revision-2 kernel existed.
    const std::vector<std::uint64_t> golden {0x83447aa9a21401a1ull, 0x6a1e0f4c8a4bda41ull, 0xa0657b86ca9e51ecull, 0x916b95ed22b97f87ull};
    for (size_t i = 0; i < hashes.size(); ++i) std::cout << "LEGACY_FINGERPRINT " << i << ' ' << std::hex << hashes[i] << std::dec << '\n';
    CHECK(hashes == golden);
    CHECK(kernelFingerprints(legacyAlgorithmRevision, true) == golden);
}
void modernKernelFingerprint()
{
    section("modern_kernel_fingerprint: revision 2 voices stay bit-identical");
    const auto hashes = kernelFingerprints(modernSynthAlgorithmRevision);
    // Fresh instances select revision 2, so saved projects depend on this output.
    const std::vector<std::uint64_t> golden {0xeb7be71d319b51ccull, 0x0bd6eb097b580af7ull, 0x3d74e0a200421e1aull, 0xe2411015bf4206e3ull};
    for (size_t i = 0; i < hashes.size(); ++i) std::cout << "MODERN_FINGERPRINT " << i << ' ' << std::hex << hashes[i] << std::dec << '\n';
    CHECK(hashes == golden);
    CHECK(kernelFingerprints(modernSynthAlgorithmRevision, true) == golden);
}
// Unison groups give voices identical filter inputs, on the shared and the
// per-voice modulation paths; a wide LFO pitch sweep moves every oscillator
// across many band-limited levels. Captured before any per-frame or per-voice
// reuse existed, so reuse must leave every sample unchanged.
void modernSharedFingerprint()
{
    section("modern_kernel_shared_fingerprint: unison groups and pitch sweeps stay bit-identical");
    std::vector<std::uint64_t> hashes;
    for (double rate : {44100.0, 96000.0})
        for (auto kind : {EngineKind::analog, EngineKind::wavetable})
        {
            SynthPatch p; p.masterLevel = 1;
            for (size_t i = 0; i < 2; ++i)
            {
                auto& l = p.layers[i];
                l.enabled = true; l.algorithmRevision = modernSynthAlgorithmRevision;
                l.engine = i == 0 ? kind : (kind == EngineKind::analog ? EngineKind::wavetable : EngineKind::analog);
                l.oscillators[0] = {Waveform::saw, 0, .6f, 0, -3, .5f, .4f}; l.oscillators[1] = {Waveform::pulse, 1, .4f, 7, 5, .4f, .3f};
                l.noiseLevel = .02f; l.cutoffHz = 1200; l.resonance = .5f; l.filterEnvelopeOctaves = 2; l.level = .5f; l.pan = i == 0 ? -.3f : .3f;
                l.filterMode = i == 0 ? FilterMode::lowpass : FilterMode::highpass;
                l.ampEnvelope = {.003f, .05f, .1f, .7f}; l.filterEnvelope = {.002f, .04f, .1f, .4f};
                l.lfos[0].rateHz = 2; l.lfos[1].rateHz = 7;
                const auto bank = l.engine == EngineKind::analog ? ModBank::analog : ModBank::wavetable;
                l.modulation[0] = {true, ModSource::lfo1, ModTarget::osc1Tune, bank, .8f};
                l.modulation[1] = {true, ModSource::lfo2, ModTarget::cutoff, bank, .1f};
                if (i == 1) l.modulation[2] = {true, ModSource::velocity, ModTarget::cutoff, bank, .2f};
            }
            CHECK(validateSynthPatch(p));
            SynthEngine e; CHECK(e.setPatch(p) && e.prepare(rate, 512));
            const int count = int(rate * .6), half = count / 2;
            std::vector<Event> events;
            for (int k = 0; k < 4; ++k) events.push_back({0, on(48 + 3 * k)});
            for (int k = 0; k < 3; ++k) events.push_back({0, on(62 + 4 * k, 1, .5f)});
            events.push_back({half, off(51)}); events.push_back({half + 100, on(80, 1, .8f)});
            const auto audio = render(e, count, events);
            CHECK(energy(audio) > 1e-6);
            hashes.push_back(fingerprint(audio));
        }
    const std::vector<std::uint64_t> golden {0xe863c954ed4a24e0ull, 0x180659a280fca24eull, 0x0b43e392597b3525ull, 0x62fc58190d40637cull};
    for (size_t i = 0; i < hashes.size(); ++i) std::cout << "MODERN_SHARED_FINGERPRINT " << i << ' ' << std::hex << hashes[i] << std::dec << '\n';
    CHECK(hashes == golden);
}
void modernVoiceKernel()
{
    section("modern_kernel_admission: synth revision 2 is admitted per layer and bank; unknown revisions stay rejected");
    {
        const auto p = flat();
        for (size_t layer = 0; layer < 4; ++layer) { auto q = p; q.layers[layer].algorithmRevision = 2; CHECK(validateSynthPatch(q)); }
        for (auto revision : {AlgorithmRevision(0), AlgorithmRevision(3), AlgorithmRevision(255)})
        { auto q = p; q.layers[1].algorithmRevision = revision; CHECK(!validateSynthPatch(q)); }
        for (size_t slot = 0; slot < 4; ++slot)
            for (size_t bank = 0; bank < 2; ++bank)
            {
                KernelCompatibility k; k.synth[slot][bank] = 2; CHECK(validateKernelCompatibility(k));
                k.synth[slot][bank] = 3; CHECK(!validateKernelCompatibility(k));
            }
        for (size_t bank = 0; bank < effectBankCount; ++bank) CHECK(supportsEffectAlgorithmRevision(bank, 2) == (bank <= 5 || bank == 6 || bank == 8 || bank == 10 || bank == 13));
        auto legacy = flat(), current = modern();
        legacy.layers[0].oscillators[0].waveform = current.layers[0].oscillators[0].waveform = Waveform::saw;
        SynthEngine a, b; CHECK(a.setPatch(legacy) && b.setPatch(current) && b.patch() == current);
        CHECK(a.prepare(48000, 512) && b.prepare(48000, 512));
        CHECK(difference(render(a, 4096, {{0, on()}}), render(b, 4096, {{0, on()}})) > 1e-3);
    }
    section("modern_kernel_bandwidth: oscillators reach at least 18 kHz at 48 kHz with folded energy below -70 dB");
    for (auto kind : {EngineKind::analog, EngineKind::wavetable})
        for (int key : {45, 69, 88})
        {
            auto p = modern(kind); auto& o = p.layers[0].oscillators[0]; o.waveform = Waveform::saw; o.position = 1;
            SynthEngine e; CHECK(e.setPatch(p) && e.prepare(48000, 1024));
            const auto report = harmonics(render(e, 65536 + 8192, {{0, on(key)}}, 1024), 48000, 440 * std::exp2((key - 69) / 12.0));
            std::cout << "MODERN_BANDWIDTH engine=" << int(kind) << " key=" << key << " top_hz=" << report.topHz << " alias_db=" << 10 * std::log10(report.alias) << '\n';
            CHECK(report.topHz >= 18000); CHECK(report.alias < 1e-7);
        }
    section("modern_kernel_wavetable_morph: position keeps the legacy sine-to-saw/square meaning; every frame keeps its pitch");
    for (int table : {0, 1})
    {
        double previous = 0;
        for (float position : {0.0f, .25f, .5f, .75f, 1.0f})
        {
            auto p = modern(EngineKind::wavetable); auto& o = p.layers[0].oscillators[0]; o.tableIndex = table; o.position = position;
            SynthEngine e; CHECK(e.setPatch(p) && e.prepare(48000, 1024));
            const auto report = harmonics(render(e, 65536 + 8192, {{0, on(57)}}, 1024), 48000, 220);
            std::cout << "MODERN_MORPH table=" << table << " position=" << position << " fundamental=" << report.fundamental
                      << " centroid_hz=" << report.centroid << " top_hz=" << report.topHz << '\n';
            CHECK(report.fundamental > .5); CHECK(report.centroid > previous); previous = report.centroid;
            if (position == 1) CHECK(report.topHz >= 18000);
        }
    }
    section("modern_kernel_envelopes: exponential decay and release; retrigger continues without a step");
    {
        auto p = modern(); p.layers[0].ampEnvelope = {0, 1, 1, 0};
        SynthEngine e; CHECK(e.setPatch(p) && e.prepare(48000, 512));
        auto x = render(e, 60000, {{0, on()}});
        const double start = peakNear(x, 240, 120), quarter = peakNear(x, 12000, 120) / start, half = peakNear(x, 24000, 120) / start;
        p.layers[0].ampEnvelope = {0, 0, 1, 1}; CHECK(e.setPatch(p)); e.reset();
        x = render(e, 60000, {{0, on()}, {4800, off()}});
        const double released = peakNear(x, 4800 + 24000, 120) / peakNear(x, 4700, 120);
        std::cout << "MODERN_ENVELOPE decay_quarter=" << quarter << " decay_half=" << half << " release_half=" << released << '\n';
        CHECK(quarter < .45); CHECK(half < .2 && half > .01); CHECK(released < .2 && released > .01);
        p.layers[0].ampEnvelope = {.005f, .05f, .2f, .8f};
        CHECK(e.setPatch(p)); e.reset();
        x = render(e, 24000, {{0, on()}, {12027, on()}});
        const double steady = maximumStep(x, 9000, 12000), retrigger = maximumStep(x, 12000, 12600);
        const double dip = peakNear(x, 12027 + 60, 60) / peakNear(x, 11000, 240);
        std::cout << "MODERN_RETRIGGER steady_step=" << steady << " retrigger_step=" << retrigger << " dip=" << dip << '\n';
        CHECK(retrigger < 2 * steady); CHECK(dip > .5);
    }
    section("modern_kernel_filter: analog ladder slope and bounded self-oscillation; wavetable resonance");
    for (auto kind : {EngineKind::analog, EngineKind::wavetable})
        for (float resonance : {0.0f, .95f})
        {
            auto p = modern(kind); auto& l = p.layers[0];
            l.oscillators[0].level = 0; l.noiseLevel = resonance == 0 ? 1 : .05f; l.cutoffHz = 1000; l.resonance = resonance;
            SynthEngine e; CHECK(e.setPatch(p) && e.prepare(48000, 512));
            const auto x = render(e, 72000, {{0, on(60)}});
            const double pass = bandPower(x, 48000, 150, 350), peak = bandPower(x, 48000, 900, 1100), stop = bandPower(x, 48000, 3800, 4200);
            const bool finite = std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 1; });
            std::cout << "MODERN_FILTER engine=" << int(kind) << " resonance=" << resonance << " peak_db=" << 10 * std::log10(peak / pass)
                      << " stop_db=" << 10 * std::log10(stop / pass) << " output_peak=" << peakNear(x, 36000, 36000) << '\n';
            CHECK(finite);
            if (kind == EngineKind::analog && resonance == 0) CHECK(stop < pass * 2.5e-4);
            if (kind == EngineKind::analog && resonance > 0) CHECK(peak > pass * 100);
            if (kind == EngineKind::wavetable && resonance > 0) CHECK(peak > pass * 30);
        }
    section("modern_kernel_phase_and_determinism: varied repeated onsets, identical reruns, partition invariance");
    {
        auto p = modern(); auto& l = p.layers[0];
        l.oscillators[0].waveform = Waveform::saw; l.oscillators[1] = {Waveform::saw, 0, 0, 0, 7, .6f, .5f}; l.cutoffHz = 3000;
        SynthEngine a, b; CHECK(a.setPatch(p) && b.setPatch(p) && a.prepare(48000, 512) && b.prepare(48000, 512));
        const std::vector<Event> events {{0, on(57)}, {22000, off(57)}, {24000, on(57)}};
        const auto x = render(a, 26000, events), y = render(b, 26000, events, 37);
        double ab = 0, aa = 0, bb = 0;
        for (int i = 0; i < 1440; ++i) { const double u = x[size_t(i)], v = x[size_t(24000 + i)]; ab += u * v; aa += u * u; bb += v * v; }
        const double correlation = ab / std::sqrt(std::max(aa * bb, 1e-30));
        a.reset(); const auto rerun = render(a, 26000, events, 1);
        std::cout << "MODERN_ONSETS correlation=" << correlation << '\n';
        CHECK(energy(x) > 1e-6); CHECK(correlation < .99); CHECK(difference(x, y) <= 1e-6); CHECK(difference(x, rerun) <= 1e-6);
        auto layered = distinctLayers();
        for (auto& layer : layered.layers) layer.algorithmRevision = 2;
        layered.layers[1].lfos[0].rateHz = 4; layered.layers[1].modulation[0] = {true, ModSource::lfo1, ModTarget::cutoff, ModBank::analog, .2f};
        SynthEngine c, d; CHECK(c.setPatch(layered) && d.setPatch(layered) && c.prepare(44100, 512) && d.prepare(44100, 512));
        std::vector<Event> chord; for (int n = 48; n < 60; ++n) chord.emplace_back(n * 7, on(n)); chord.emplace_back(9000, off(52));
        CHECK(difference(render(c, 16384, chord, 512), render(d, 16384, chord, 1)) <= 1e-6);
    }
    section("modern_kernel_finite_and_allocation: extreme notes stay finite; polyphony has headroom; callbacks allocate zero");
    for (double rate : {44100.0, 96000.0})
        for (auto kind : {EngineKind::analog, EngineKind::wavetable})
            for (auto wave : {Waveform::sine, Waveform::triangle, Waveform::saw, Waveform::pulse})
            {
                SynthEngine e; SynthPatch p;
                for (auto& l : p.layers) { l.enabled = true; l.engine = kind; l.algorithmRevision = 2; l.resonance = .95f; l.filterEnvelopeOctaves = 8; for (auto& o : l.oscillators) { o.waveform = wave; o.tune = 24; o.detune = 100; o.position = 1; } }
                CHECK(e.setPatch(p) && e.prepare(rate, 256));
                std::vector<Event> notes; for (int n = 112; n < 128; ++n) notes.emplace_back(0, on(n));
                const auto x = render(e, 2048, notes);
                CHECK(e.activeNoteCount() == 16);
                CHECK(std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v) && std::abs(v) < 100; }));
            }
    {
        SynthEngine e; SynthPatch p; for (auto& l : p.layers) { l.enabled = true; l.algorithmRevision = 2; }
        CHECK(e.setPatch(p) && e.prepare(48000, 512));
        std::vector<Event> notes; for (int n = 48; n < 64; ++n) notes.emplace_back(0, on(n));
        const auto x = render(e, 24000, notes, 512);
        const double peak = peakNear(x, 12000, 12000);
        std::cout << "MODERN_POLYPHONIC peak=" << peak << " rms=" << std::sqrt(energy(x)) << '\n';
        CHECK(peak < 1); CHECK(energy(x) > 1e-6);
        auto layered = distinctLayers(); for (auto& l : layered.layers) l.algorithmRevision = 2;
        SynthEngine high; CHECK(high.setPatch(layered) && high.prepare(48000, 512, RenderQuality::higher));
        juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi, empty; midi.ensureSize(32768);
        for (int n = 48; n < 64; ++n) midi.addEvent(on(n), n - 48);
        std::array<juce::AudioBuffer<float>, 4> buffers; std::array<juce::AudioBuffer<float>*, 4> pointers; std::array<float, 512> gains;
        for (size_t n = 0; n < 4; ++n) { buffers[n].setSize(2, 512); pointers[n] = &buffers[n]; }
#if defined(_MSC_VER) && defined(_DEBUG)
        allocations = 0; frees = 0; auto old = _CrtSetAllocHook(allocationHook); tracking = true;
        for (int i = 0; i < 8; ++i) { e.setPatch(p); e.render(audio, midi); }
        e.reset(); e.renderLayers(pointers, gains, midi);
        high.render(audio, midi); for (int i = 0; i < 8; ++i) high.renderLayers(pointers, gains, empty); high.reset(); high.render(audio, midi);
        tracking = false; _CrtSetAllocHook(old);
        std::cout << "MODERN_CALLBACK allocations=" << allocations << " frees=" << frees << '\n';
        CHECK(allocations == 0 && frees == 0);
#else
        CHECK(false);
#endif
        CHECK(std::all_of(audio.getReadPointer(0), audio.getReadPointer(0) + 512, [](float v) { return std::isfinite(v); }));
        CHECK(high.activeNoteCount() == 16);
    }
    section("modern_kernel_revision_switch: live revision edits fade structurally and keep the note sounding");
    for (auto from : {AlgorithmRevision(1), AlgorithmRevision(2)})
    {
        auto p = flat(); p.layers[0].algorithmRevision = from; p.layers[0].ampEnvelope = {.002f, .05f, .1f, .8f};
        SynthEngine e; CHECK(e.setPatch(p) && e.prepare(48000, 256));
        const auto before = render(e, 9600, {{0, on()}});
        p.layers[0].algorithmRevision = from == 1 ? 2 : 1; CHECK(e.setPatch(p));
        const auto after = render(e, 9600);
        std::vector<float> joined(before); joined.insert(joined.end(), after.begin(), after.end());
        const double steady = maximumStep(joined, 4800, 9600), edit = maximumStep(joined, 9590, 12000);
        std::cout << "MODERN_SWITCH from=" << int(from) << " steady_step=" << steady << " edit_step=" << edit << " tail=" << peakNear(after, 7200, 2400) << '\n';
        CHECK(edit < 1.5 * steady); CHECK(peakNear(after, 7200, 2400) > .3 * peakNear(before, 7200, 2400)); CHECK(e.activeNoteCount() == 1);
    }
}
void modernFilterCharacterization()
{
    section("modern_filter_coefficients_live_keys_reset_rate_characterization");
    // Captured from the unchanged voice implementation before coefficient reuse.
    constexpr std::array<std::uint64_t,4> expected {0xb7164be8d3e1beaeull,0xa2a768e0dfaf5945ull,
                                                   0x2ef889a2a55b668eull,0xc0f2890daae6f116ull};
    std::size_t scene=0;
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        SynthPatch initial;initial.masterLevel=.35f;
        for(std::size_t i=0;i<4;++i){auto& l=initial.layers[i];l.enabled=true;l.algorithmRevision=modernSynthAlgorithmRevision;
            l.engine=i%2?EngineKind::wavetable:EngineKind::analog;l.level=.12f;l.pan=float(i)*.4f-.6f;
            l.cutoffHz=500+1300*float(i);l.resonance=.2f+.15f*float(i);l.filterMode=static_cast<FilterMode>(i%3);
            l.ampEnvelope.attack=.001f;l.ampEnvelope.decay=.01f;l.ampEnvelope.sustain=1;l.ampEnvelope.release=.03f;
            l.filterEnvelope.attack=.002f;l.filterEnvelope.decay=.02f;l.filterEnvelope.sustain=.5f;l.filterEnvelope.release=.04f;
        }
        const auto phrase=[&](int partition){SynthEngine engine;CHECK(engine.setPatch(initial)&&engine.prepare(rate,512,quality));std::vector<float> result;
            const auto append=[&](int frames,const std::vector<Event>& events=std::vector<Event>{}){const auto audio=render(engine,frames,events,partition);result.insert(result.end(),audio.begin(),audio.end());};
            std::vector<Event> notes;for(int n=0;n<16;++n)notes.emplace_back(0,on(48+n,1,.4f+.025f*n));
            append(int(rate*.03),notes);auto patch=initial;
            for(std::size_t i=0;i<4;++i){auto& l=patch.layers[i];l.cutoffHz=3000+1700*float(i);l.resonance=.8f-.1f*float(i);l.filterEnvelopeOctaves=i%2?-3.f:4.f;}
            CHECK(engine.setPatch(patch));append(int(rate*.0017));
            for(std::size_t i=0;i<4;++i){auto& l=patch.layers[i];l.cutoffHz=i%2?20.f:20000.f;l.resonance=i%2?.95f:0;l.filterEnvelopeOctaves=0;}
            CHECK(engine.setPatch(patch));append(int(rate*.012));
            for(int k=0;k<7;++k){CHECK(engine.setPatch(patch));append(37);}
            CHECK(!engine.prepare(7999,512));append(91);append(int(rate*.02),{{17,juce::MidiMessage::midiStop()},{23,on(62)}});
            CHECK(engine.setPatch(initial));engine.reset();append(int(rate*.014),{{0,on(65)},{int(rate*.009),off(65)}});
            CHECK(engine.prepare(rate==48000?96000:48000,512,quality));append(1103,{{0,on(55)}});
            return result;
        };
        const auto fine=phrase(64),coarse=phrase(512),irregular=phrase(73);
        CHECK(energy(fine)>1e-9);CHECK(fingerprint(fine)==fingerprint(coarse)&&fingerprint(fine)==fingerprint(irregular));
        CHECK(std::all_of(fine.begin(),fine.end(),[](float v){return std::isfinite(v);}));
        const auto hash=fingerprint(fine);CHECK(hash==expected[scene++]);std::cout<<"MODERN_FILTER_CHARACTERIZATION rate="<<rate<<" quality="<<int(quality)<<" frames="<<fine.size()<<" fnv="<<std::hex<<hash<<std::dec<<'\n';
        if(const auto* directory=std::getenv("AIRACK_MODERN_FILTER_CHARACTERIZATION_DIR")){
            const juce::File folder{juce::String(directory)};CHECK(juce::File::isAbsolutePath(directory)&&folder.isAChildOf(juce::File::getCurrentWorkingDirectory()));CHECK(folder.createDirectory().wasOk());
            const auto file=folder.getChildFile("modern-filter-"+juce::String(int(rate))+"-"+juce::String(int(quality))+".f32");CHECK(!file.exists());if(!file.exists())CHECK(file.replaceWithData(fine.data(),fine.size()*sizeof(float)));
        }
    }
}
void modernOscillatorCharacterization()
{
    section("modern_wavetable_live_controls_reset_rate_characterization");
    // Captured before any oscillator call-boundary or lookup specialization.
    constexpr std::array<std::uint64_t,4> expected {0xaeee893f322a089full,0xda15586c51d493ccull,
                                                   0x72e4ceea20300180ull,0x34e1ad635e15df26ull};
    std::size_t scene=0;
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        SynthPatch initial;initial.masterLevel=.3f;
        for(std::size_t i=0;i<4;++i){auto& l=initial.layers[i];l.enabled=true;l.algorithmRevision=modernSynthAlgorithmRevision;
            l.engine=EngineKind::wavetable;l.level=.12f;l.pan=float(i)*.4f-.6f;l.cutoffHz=14000;l.resonance=.3f;
            l.ampEnvelope={.001f,.01f,.03f,1};
            l.oscillators[0]={Waveform::saw,int(i%2),float(i)/3,0,-3,.5f,.4f};
            l.oscillators[1]={Waveform::pulse,int(1-i%2),1-float(i)/3,7,5,.4f,.3f};
        }
        const auto phrase=[&](int partition){SynthEngine engine;CHECK(engine.setPatch(initial)&&engine.prepare(rate,512,quality));std::vector<float> result;
            const auto append=[&](int frames,const std::vector<Event>& events=std::vector<Event>{}){const auto audio=render(engine,frames,events,partition);result.insert(result.end(),audio.begin(),audio.end());};
            std::vector<Event> notes;for(int n=0;n<16;++n)notes.emplace_back(0,on(28+5*n,1,.5f+.02f*n));append(int(rate*.016),notes);
            auto patch=initial;
            for(int edit=0;edit<3;++edit){for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<2;++j){auto& o=patch.layers[i].oscillators[j];
                o.position=std::array<float,3>{1,.4375f,0}[std::size_t(edit)];o.tune=j?(edit%2?24.f:-24.f):(edit%2?-12.f:12.f);o.detune=j?100.f:-100.f;o.tableIndex=int((i+j+std::size_t(edit))%2);
            }CHECK(engine.setPatch(patch));append(int(rate*(edit==0?.0017:.008)));}
            for(auto& l:patch.layers){l.lfos[0].rateHz=7;l.lfos[1].rateHz=11;
                l.modulation[0]={true,ModSource::lfo1,ModTarget::osc1Tune,ModBank::wavetable,.6f};
                l.modulation[1]={true,ModSource::lfo2,ModTarget::osc2Position,ModBank::wavetable,.8f};}
            CHECK(engine.setPatch(patch));append(int(rate*.024));
            for(std::size_t i=0;i<4;i+=2)patch.layers[i].engine=EngineKind::analog;CHECK(engine.setPatch(patch));append(int(rate*.009));
            CHECK(engine.setPatch(initial));append(int(rate*.012));for(int k=0;k<3;++k){CHECK(engine.setPatch(initial));append(37);}
            CHECK(!engine.prepare(7999,512));append(91);append(301,{{17,juce::MidiMessage::midiStop()},{23,on(62)}});
            engine.reset();append(733,{{0,on(65)},{430,off(65)}});CHECK(engine.prepare(rate==48000?96000:48000,512,quality));append(1103,{{0,on(55)}});
            return result;
        };
        const auto fine=phrase(64),coarse=phrase(512),irregular=phrase(73);CHECK(energy(fine)>1e-9);
        CHECK(fingerprint(fine)==fingerprint(coarse)&&fingerprint(fine)==fingerprint(irregular));CHECK(std::all_of(fine.begin(),fine.end(),[](float v){return std::isfinite(v);}));
        CHECK(fingerprint(fine)==expected[scene++]);std::cout<<"MODERN_OSCILLATOR_CHARACTERIZATION rate="<<rate<<" quality="<<int(quality)<<" frames="<<fine.size()<<" fnv="<<std::hex<<fingerprint(fine)<<std::dec<<'\n';
        if(const auto* directory=std::getenv("AIRACK_WAVETABLE_CHARACTERIZATION_DIR")){
            const juce::File folder{juce::String(directory)};CHECK(juce::File::isAbsolutePath(directory)&&folder.isAChildOf(juce::File::getCurrentWorkingDirectory()));CHECK(folder.createDirectory().wasOk());
            const auto file=folder.getChildFile("modern-oscillator-"+juce::String(int(rate))+"-"+juce::String(int(quality))+".f32");CHECK(!file.exists());if(!file.exists())CHECK(file.replaceWithData(fine.data(),fine.size()*sizeof(float)));
        }
    }
}
void modernSeparatedCharacterization()
{
    section("original_full_separated_layers_global_and_voice_modulation_lifecycle");
    constexpr std::array<std::uint64_t,4> expected {0x420fb9f2a0a92000ull,0xc84c87a7978b0361ull,0x8c776202e29f9257ull,0x776b321bc654896aull};std::size_t scene=0;
    for(double rate:{48000.,96000.})for(auto quality:{RenderQuality::realtime,RenderQuality::higher}){
        SynthPatch initial;initial.masterLevel=.35f;
        for(std::size_t i=0;i<4;++i){auto& l=initial.layers[i];l.enabled=true;l.algorithmRevision=modernSynthAlgorithmRevision;l.engine=i%2?EngineKind::wavetable:EngineKind::analog;l.level=.17f;l.pan=float(i)*.4f-.6f;l.cutoffHz=800+1700*float(i);l.resonance=.15f+.2f*float(i);l.filterMode=FilterMode(i%3);l.noiseLevel=.01f;l.ampEnvelope={.001f,.01f,.012f,1};l.filterEnvelope={.002f,.02f,.015f,.5f};
            for(std::size_t j=0;j<2;++j){auto& o=l.oscillators[j];o.waveform=Waveform((i+j)%4);o.tableIndex=int((i+j)%2);o.position=float((i+j)%3)*.5f;o.pulseWidth=.2f+.15f*float(i);o.detune=j?7.f:-5.f;o.tune=j?7.f:0.f;}}
        const auto phrase=[&](int partition){SynthEngine engine;CHECK(engine.setPatch(initial)&&engine.prepare(rate,512,quality));auto patch=initial;std::vector<float> result;std::array<juce::AudioBuffer<float>,4> storage;for(auto& b:storage)b.setSize(2,512);std::array<float,512> gains;juce::MidiBuffer midi;midi.ensureSize(4096);
            const auto append=[&](int frames,const std::vector<Event>& events=std::vector<Event>{},std::optional<double> tempo=120.){for(int offset=0;offset<frames;){const int count=std::min(partition,frames-offset);std::array<juce::AudioBuffer<float>,4> views;std::array<juce::AudioBuffer<float>*,4> pointers;
                for(std::size_t layer=0;layer<4;++layer){views[layer].setDataToReferTo(storage[layer].getArrayOfWritePointers(),2,count);pointers[layer]=&views[layer];}midi.clear();for(const auto& [at,message]:events)if(at>=offset&&at<offset+count)midi.addEvent(message,at-offset);engine.renderLayers(pointers,std::span(gains).first(std::size_t(count)),midi,tempo);
                for(int n=0;n<count;++n){result.push_back(gains[std::size_t(n)]);for(auto& view:views)for(int channel=0;channel<2;++channel)result.push_back(view.getSample(channel,n));}offset+=count;}};
            std::vector<Event> notes;for(int n=0;n<16;++n)notes.emplace_back(0,on(36+3*n,n%2+1,.5f+.02f*n));append(int(rate*.016),notes);
            for(std::size_t i=0;i<4;++i)patch.layers[i].filterMode=FilterMode((i+1)%3);CHECK(engine.setPatch(patch));append(int(rate*.013));
            for(auto& l:patch.layers){l.lfos[0].rateHz=3;for(std::size_t n=0;n<16;++n)l.modulation[n]={true,ModSource::lfo1,n%2?ModTarget::cutoff:ModTarget::osc2Position,n%2?(l.engine==EngineKind::analog?ModBank::analog:ModBank::wavetable):ModBank::wavetable,.03f};}
            patch.masterLevel=.6f;CHECK(engine.setPatch(patch));append(int(rate*.013));
            for(auto& l:patch.layers)l.modulation[0]={true,ModSource::velocity,ModTarget::pan,ModBank::common,.3f};CHECK(engine.setPatch(patch));append(int(rate*.012));
            for(std::size_t i=0;i<4;++i)patch.layers[i].engine=i%2?EngineKind::analog:EngineKind::wavetable;CHECK(engine.setPatch(patch));append(int(rate*.0017));CHECK(engine.setPatch(initial));append(int(rate*.014));
            patch=initial;patch.performance={VoiceMode::monophonic,.05f,true};CHECK(engine.setPatch(patch));append(int(rate*.012),{{17,on(65,1,.8f)},{93,off(65)}});
            CHECK(engine.setPatch(initial));append(int(rate*.012),{{19,juce::MidiMessage::allSoundOff(2)},{23,on(83,1,.6f)}});
            append(419,{{17,juce::MidiMessage::midiStop()},{23,on(62,1,.7f)},{47,on(69,1,.8f)},{83,on(72,1,.9f)}});CHECK(!engine.prepare(7999,512));append(103);
            engine.reset();append(733,{{0,on(55)},{430,off(55)}});CHECK(engine.prepare(rate==48000?96000:48000,512,quality));append(1103,{{0,on(67)}});return result;};
        const auto fine=phrase(64),coarse=phrase(512),irregular=phrase(73);CHECK(energy(fine)>1e-9);CHECK(fingerprint(fine)==fingerprint(coarse)&&fingerprint(fine)==fingerprint(irregular));CHECK(std::all_of(fine.begin(),fine.end(),[](float v){return std::isfinite(v);}));const auto hash=fingerprint(fine);CHECK(hash==expected[scene++]);
        std::cout<<"MODERN_SEPARATED_CHARACTERIZATION rate="<<rate<<" quality="<<int(quality)<<" samples="<<fine.size()<<" fnv="<<std::hex<<hash<<std::dec<<'\n';
        if(const auto* directory=std::getenv("AIRACK_MODERN_SEPARATED_CHARACTERIZATION_DIR")){const juce::File folder{juce::String(directory)};CHECK(juce::File::isAbsolutePath(directory)&&folder.isAChildOf(juce::File::getCurrentWorkingDirectory()));CHECK(folder.createDirectory().wasOk());const auto file=folder.getChildFile("modern-separated-"+juce::String(int(rate))+"-"+juce::String(int(quality))+".f32");CHECK(!file.exists());if(!file.exists())CHECK(file.replaceWithData(fine.data(),fine.size()*sizeof(float)));}
    }
}
int main(int argc,char** argv)
{
    if(argc==2&&std::string_view(argv[1])=="--modern-separated-characterization"){modernSeparatedCharacterization();std::cout<<"Characterization checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--kernel-fingerprints"){legacyKernelFingerprint();modernKernelFingerprint();modernSharedFingerprint();std::cout<<"Fingerprint checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--modern-oscillator-characterization"){modernOscillatorCharacterization();std::cout<<"Characterization checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--modern-filter-characterization"){modernFilterCharacterization();std::cout<<"Characterization checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
    if(argc!=1)return 2;
    validation(); soundAndControls(); midiAndPool(); layeringAndPartitions(); smoothing(); structuralTransitions(); spectral(); finiteAndAllocation();
    separatedLayersShareNotes(); separatedMasterRamp(); invalidSeparatedBuffersPreserveState();
    monoHeldKeysAndChannels(); glidePitchAndLegato(); performanceTransitionsAndValidation();
    lfoAudioPhaseAndTempo(); modulationSourcesAndDestinations(); qualifiedBanksAndRouteValidation(); modulationLifecycleAndCapacity();
    qualityReferenceSensitivity(); qualityVoiceArticulation(); preparedQualityContract<SynthEngine>(); qualityClockAndFilterReference();
    legacyKernelFingerprint(); modernKernelFingerprint(); modernSharedFingerprint(); modernVoiceKernel();
    modernFilterCharacterization();
    modernOscillatorCharacterization();
    modernSeparatedCharacterization();
    std::cout << "RESULT sections=" << sections << " checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

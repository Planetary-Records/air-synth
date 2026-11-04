#include "PreviewRender.h"
#include "RoutedSynthEngine.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace { bool failNextAllocation = false; }
void* operator new(std::size_t n)
{
    // MSVC Debug default vector constructors allocate noexcept descriptor proxies.
    // Fault only substantive renderer allocations, whose failures are recoverable.
    if (failNextAllocation && n >= 4096) { failNextAllocation = false; throw std::bad_alloc(); }
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace airack;
using namespace airack::ai;
namespace
{
int checks = 0, failures = 0, sections = 0;
const char* section = "";
void check(bool pass, const char* expression, int line)
{
    ++checks;
    if (!pass) { ++failures; if (failures < 70) std::cerr << section << ':' << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void begin(const char* name) { section = name; ++sections; std::cout << name << '\n'; }
void cold_macro_binding()
{
    begin(__func__);
    // First renderer call, before any helper initializes the saved-key registry.
    ProjectDocument d;
    d.sound.macros[0] = {0x31924c5b, .25f}; // Established global.master_level key.
    const auto before = d;
    AuditionContext c;
    c.bpm = 400; // Empty phrase: no new musical fixture or timing interpretation.
    const auto rendered = renderPreview(d, c);
    CHECK(rendered.status == RenderStatus::ready);
    CHECK(rendered.audio.bodyFrames == 28800);
    CHECK(rendered.audio.left.size() == 220800 && rendered.audio.right.size() == 220800);
    CHECK(rendered.audio.context.phrase == c.phrase && rendered.audio.context.bpm == 400);
    CHECK(rendered.measurements.finite && rendered.measurements.peak == 0 && rendered.measurements.rms == 0);
    CHECK(d == before && d.sound.macros[0].targetKey == 0x31924c5b && d.sound.macros[0].amount == .25f);
}
bool near(double a, double b, double tolerance = 1e-11) { return std::abs(a - b) <= tolerance; }
bool exact(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}
bool same(const AudioMeasurements& a, const AudioMeasurements& b)
{
    return a.finite == b.finite && a.clippedSamples == b.clippedSamples && a.peak == b.peak
        && a.rms == b.rms && a.onsetSeconds == b.onsetSeconds && a.activityEndSeconds == b.activityEndSeconds
        && a.decaySeconds == b.decaySeconds && a.bandFractions == b.bandFractions && a.tailActiveAtEnd == b.tailActiveAtEnd;
}
bool same(const AuditionContext& a, const AuditionContext& b)
{
    return a.phrase == b.phrase && a.bpm == b.bpm && a.phraseSource == b.phraseSource && a.tempoSource == b.tempoSource;
}
void physical(ProjectDocument& d, const char* id, float value)
{
    const auto registry = parameterRegistry();
    const auto p = std::find_if(registry.begin(), registry.end(), [&](const auto& v) { return v.id == id; });
    CHECK(p != registry.end());
    if (p == registry.end()) return;
    juce::NormalisableRange<float> range(p->minimum, p->maximum, 0, p->skew);
    d.sound.values[std::size_t(p - registry.begin())] = range.convertTo0to1(value);
}
ProjectDocument sound(bool wavetable = false, bool effect = true)
{
    ProjectDocument d;
    physical(d, "output.gain_db", 18); physical(d, "output.protection", 0);
    physical(d, "global.master_level", .2f); physical(d, "synth.1.level", .3f);
    physical(d, "synth.1.engine", wavetable ? 1.f : 0.f);
    physical(d, "synth.1.analog.amp.release", .3f); physical(d, "synth.1.wavetable.amp.release", .3f);
    if (effect)
    {
        CHECK(editRackLayout(d.sound.layout, {RackEditKind::addEffect, 0, masterChain, 0, EffectKind::delay}, d.sound.layout) == RackEditResult::applied);
        physical(d, "effect.1.delay.synced", 1); physical(d, "effect.1.delay.beats", .25f);
        physical(d, "effect.1.delay.feedback", .5f); physical(d, "effect.1.delay.mix", .5f);
    }
    CHECK(validateProjectDocument(d)); return d;
}
AuditionContext musical()
{
    AuditionContext c;
    c.phrase.meter = {3, 8}; c.phrase.swing = .6;
    c.phrase.noteCount = 4;
    c.phrase.notes[0] = {0, 1.5, 60, 91, 1};
    c.phrase.notes[1] = {.25, .7, 60, 87, 1};
    c.phrase.notes[2] = {.25, .125, 64, 98, 2};
    c.phrase.notes[3] = {1.25, .25, 67, 80, 16};
    CHECK(validatePatternDocument(c.phrase)); return c;
}
struct Reference
{
    std::vector<float> left, right;
    std::vector<PatternEvent> edges;
    bool valid = false;
};
// This owns fresh accepted native objects. Its explicit body endpoint is supplied
// by hand-checked fixtures, never by the renderer or renderer measurements.
Reference nativeReference(const ProjectDocument& d, const AuditionContext& c, int blockSize, std::size_t body)
{
    Reference r;
    RoutedPatch patch;
    if (!makeRoutedPatch(d.sound, patch)) { CHECK(false); return r; }
    RoutedSynthEngine engine;
    if (!engine.prepare(48000, blockSize) || !engine.setPatch(patch)) { CHECK(false); return r; }
    engine.reset();
    PatternScheduler scheduler;
    if (!scheduler.prepare(48000, blockSize) || !scheduler.setPattern(c.phrase)) { CHECK(false); return r; }
    r.left.resize(body + 192000); r.right.resize(body + 192000);
    juce::AudioBuffer<float> buffer(2, blockSize);
    juce::MidiBuffer midi;
    PatternEventBuffer events;
    const double samplesPerBeat = 2880000. / c.bpm;
    for (std::size_t frame = 0; frame < r.left.size();)
    {
        const auto boundary = frame < body ? body : r.left.size();
        const auto count = int(std::min(std::size_t(blockSize), boundary - frame));
        HostPatternTiming h;
        h.playing = frame < body; h.bpm = c.bpm;
        h.ppqPosition = double(frame) / samplesPerBeat; h.samplePosition = std::int64_t(frame);
        const auto status = scheduler.processBlock(h, count, PlaybackMode::internalPattern, events);
        if (status != (h.playing ? PatternPlaybackStatus::playing : PatternPlaybackStatus::stopped)) { CHECK(false); return r; }
        midi.clear();
        for (std::size_t i = 0; i < events.count; ++i)
        {
            auto e = events.events[i];
            midi.addEvent(e.noteOn ? juce::MidiMessage::noteOn(e.channel, e.pitch, e.velocity)
                                  : juce::MidiMessage::noteOff(e.channel, e.pitch), e.sampleOffset);
            e.sampleOffset += int(frame); r.edges.push_back(e);
        }
        buffer.setSize(2, count, false, false, true);
        engine.render(buffer, midi, c.bpm);
        if (buffer.getNumChannels() != 2 || buffer.getNumSamples() != count) { CHECK(false); return r; }
        std::copy_n(buffer.getReadPointer(0), count, r.left.begin() + frame);
        std::copy_n(buffer.getReadPointer(1), count, r.right.begin() + frame);
        frame += std::size_t(count);
    }
    r.valid = true; return r;
}
bool audioReady(const RenderResult& r, std::size_t frames)
{
    CHECK(r.status == RenderStatus::ready);
    CHECK(r.audio.left.size() == frames && r.audio.right.size() == frames);
    return r.status == RenderStatus::ready && r.audio.left.size() == frames && r.audio.right.size() == frames
        && r.audio.left.data() != nullptr && r.audio.right.data() != nullptr;
}
void empty(const RenderResult& r, RenderStatus status)
{
    CHECK(r.status == status); CHECK(r.audio.left.empty() && r.audio.right.empty());
}
void same_core_partition_and_recall()
{
    begin(__func__); // Catches patch transition, altered clock/swing/channel handling and changed candidate mode.
    for (bool wt : {false, true})
    {
        auto d = sound(wt); auto c = musical(); const auto saved = d; const auto savedContext = c;
        juce::MemoryBlock encoded; ProjectDocument decoded;
        const bool encodedOk = encodeProjectState(d, encoded);
        CHECK(encodedOk);
        const bool decodedOk = encodedOk && encoded.getData() && decodeProjectState(encoded.getData(), encoded.getSize(), decoded);
        CHECK(decodedOk); if (!decodedOk) continue;
        CHECK(decoded == d);
        const auto a = renderPreview(d, c, {}, 64), b = renderPreview(d, c, {}, 512);
        const auto recall = renderPreview(decoded, c, {}, 512);
        const auto ref64 = nativeReference(d, c, 64, 36000), ref512 = nativeReference(d, c, 512, 36000);
        CHECK(ref64.valid && ref512.valid);
        const bool ar = audioReady(a, 228000), br = audioReady(b, 228000), rr = audioReady(recall, 228000);
        if (ar && br && rr && ref64.valid && ref512.valid)
        {
            CHECK(exact(a.audio.left, ref64.left) && exact(a.audio.right, ref64.right));
            CHECK(exact(b.audio.left, ref512.left) && exact(b.audio.right, ref512.right));
            CHECK(exact(a.audio.left, b.audio.left) && exact(a.audio.right, b.audio.right));
            CHECK(exact(b.audio.left, recall.audio.left) && exact(b.audio.right, recall.audio.right));
            CHECK(a.audio.bodyFrames == 36000 && !a.audio.phraseTruncated);
            CHECK(a.audio.sampleRate == 48000 && same(a.audio.context, c));
            CHECK(a.measurements.peak > 0 && a.measurements.decaySeconds > 0);
        }
        CHECK(d == saved && same(c, savedContext)); CHECK(d.mode == PlaybackMode::externalMidi);
        d.mode = PlaybackMode::internalPattern;
        const auto internal = renderPreview(d, c);
        if (br && audioReady(internal, 228000)) CHECK(exact(b.audio.left, internal.audio.left) && exact(b.audio.right, internal.audio.right));
        CHECK(d.mode == PlaybackMode::internalPattern);
    }
}
void bounded_phrase_release()
{
    begin(__func__); // Catches unbounded slow phrases, floor endpoint, phrase restart and cleared wet tails.
    auto d = sound(); AuditionContext c;
    c.phrase.meter = {1, 32}; c.bpm = 137; c.phrase.noteCount = 1;
    c.phrase.notes[0] = {0, .125, 60, 96, 1};
    c.phraseSource = PhraseSource::proposed; c.tempoSource = TempoSource::defaultTempo;
    const auto fractional = renderPreview(d, c, {}, 512);
    const auto ref = nativeReference(d, c, 512, 2628); // 360000/137 = 2627.737... -> 2628.
    CHECK(ref.valid);
    if (audioReady(fractional, 194628) && ref.valid)
    {
        CHECK(fractional.audio.bodyFrames == 2628 && !fractional.audio.phraseTruncated);
        CHECK(same(fractional.audio.context, c));
        CHECK(exact(fractional.audio.left, ref.left) && exact(fractional.audio.right, ref.right));
        CHECK(std::count_if(ref.edges.begin(), ref.edges.end(), [](auto e) { return e.noteOn; }) == 1);
        CHECK(std::all_of(ref.edges.begin(), ref.edges.end(), [](auto e) { return !e.noteOn || e.sampleOffset < 2628; }));
        CHECK(std::any_of(ref.edges.begin(), ref.edges.end(), [](auto e) { return !e.noteOn && e.sampleOffset == 2628; }));
        CHECK(std::any_of(fractional.audio.left.begin() + 2628, fractional.audio.left.begin() + 12000, [](float v) { return std::abs(v) > .00001f; }));
        CHECK(std::any_of(fractional.audio.left.begin() + 24000, fractional.audio.left.begin() + 48000, [](float v) { return std::abs(v) > .000001f; }));
    }
    c.phrase.meter = {32, 4}; c.phrase.bars = 8; c.bpm = 20; c.phrase.notes[0].durationBeats = 256;
    c.phraseSource = PhraseSource::captured; c.tempoSource = TempoSource::captured;
    const auto longPhrase = renderPreview(d, c);
    if (audioReady(longPhrase, 576000))
    {
        CHECK(longPhrase.audio.bodyFrames == 384000 && longPhrase.audio.phraseTruncated);
        CHECK(same(longPhrase.audio.context, c)); CHECK(2 * longPhrase.audio.left.size() * sizeof(float) == 4608000);
        CHECK(longPhrase.measurements.decaySeconds > 0);
    }
    c.phrase = {}; c.phrase.noteCount = 0; c.bpm = 400;
    c.phraseSource = PhraseSource::defaultNotes;
    const auto silent = renderPreview(d, c);
    if (audioReady(silent, 220800))
    {
        CHECK(silent.audio.bodyFrames == 28800 && silent.measurements.peak == 0);
        CHECK(std::all_of(silent.audio.left.begin(), silent.audio.left.end(), [](float v) { return v == 0; }));
        CHECK(std::all_of(silent.audio.right.begin(), silent.audio.right.end(), [](float v) { return v == 0; }));
        CHECK(silent.audio.context.phrase.noteCount == 0 && same(silent.audio.context, c));
    }
}
void raw_signal_measurements()
{
    begin(__func__); // Catches hidden normalization, wrong stereo RMS/counts, threshold timing and float-square overflow.
    std::vector<float> l(4800, .5f), r(4800, -.25f); AudioMeasurements m;
    CHECK(measurePreview(l, r, 2400, m) == RenderStatus::ready);
    CHECK(m.finite && m.clippedSamples == 0 && m.peak == .5);
    CHECK(near(m.rms, std::sqrt(.15625))); CHECK(m.onsetSeconds && *m.onsetSeconds == 0);
    CHECK(near(m.activityEndSeconds, .1) && near(m.decaySeconds, .05) && m.tailActiveAtEnd);
    CHECK(l.front() == .5f && r.back() == -.25f);
    std::fill(l.begin(), l.end(), 0.f); std::fill(r.begin(), r.end(), 0.f);
    l[123] = .002f; r[3000] = -.001f; r[4500] = .000999f;
    CHECK(measurePreview(l, r, 2400, m) == RenderStatus::ready);
    CHECK(m.onsetSeconds && near(*m.onsetSeconds, 123. / 48000));
    CHECK(near(m.activityEndSeconds, 3001. / 48000) && near(m.decaySeconds, 601. / 48000));
    const double expectedEnergy = double(l[123]) * l[123] + double(r[3000]) * r[3000] + double(r[4500]) * r[4500];
    CHECK(near(m.rms, std::sqrt(expectedEnergy / 9600), 1e-15) && m.tailActiveAtEnd);
    l.assign(16, .0001f); r.assign(16, -.0001f);
    CHECK(measurePreview(l, r, 8, m) == RenderStatus::ready);
    CHECK(m.peak == double(.0001f) && near(m.rms, double(.0001f), 1e-15));
    CHECK(!m.onsetSeconds && m.activityEndSeconds == 0 && m.decaySeconds == 0 && !m.tailActiveAtEnd);
    l.assign(16, 0); r.assign(16, 0);
    CHECK(measurePreview(l, r, 8, m) == RenderStatus::ready);
    CHECK(m.finite && m.peak == 0 && m.rms == 0 && !m.onsetSeconds);
    CHECK((m.bandFractions == std::array<double, 3>{0, 0, 0}));
    l[0] = -1; r[15] = 1;
    CHECK(measurePreview(l, r, 8, m) == RenderStatus::clipped);
    CHECK(m.clippedSamples == 2 && m.peak == 1 && near(m.rms, .25));
    const float huge = std::numeric_limits<float>::max(); l[7] = huge; r[8] = -huge;
    CHECK(measurePreview(l, r, 8, m) == RenderStatus::clipped);
    CHECK(m.finite && m.clippedSamples == 4 && m.peak == double(huge) && std::isfinite(m.rms));
    CHECK(near(m.rms / double(huge), .25, 1e-15));
    CHECK(std::all_of(m.bandFractions.begin(), m.bandFractions.end(), [](double v) { return std::isfinite(v) && v >= 0 && v <= 1; }));
    CHECK(near(m.bandFractions[0] + m.bandFractions[1] + m.bandFractions[2], 1));
    CHECK(l[7] == huge && r[8] == -huge);
    for (float nonFinite : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
    {
        for (bool right : {false, true})
        {
            l.assign(16, .2f); r.assign(16, -.2f); (right ? r : l)[7] = nonFinite;
            int cancelledChecks = 0;
            CHECK(measurePreview(l, r, 8, m, [&] { ++cancelledChecks; return false; }) == RenderStatus::nonFinite);
            CHECK(!m.finite && m.peak == double(.2f)); CHECK(cancelledChecks == 2);
            CHECK(std::isnan(nonFinite) ? std::isnan(m.rms) : std::isinf(m.rms));
            CHECK(m.clippedSamples == (std::isinf(nonFinite) ? 1u : 0u));
            CHECK((m.bandFractions == std::array<double, 3>{0, 0, 0}));
        }
    }
    l.assign(16, 0); r.assign(16, 0); r[7] = -std::numeric_limits<float>::infinity();
    CHECK(measurePreview(l, r, 8, m) == RenderStatus::nonFinite);
    CHECK(!m.finite && m.peak == 0 && std::isinf(m.rms) && m.clippedSamples == 1);
    CHECK(m.onsetSeconds && near(*m.onsetSeconds, 7. / 48000));
    CHECK(near(m.activityEndSeconds, 8. / 48000) && m.decaySeconds == 0 && m.tailActiveAtEnd);
    r[7] = std::numeric_limits<float>::quiet_NaN();
    CHECK(measurePreview(l, r, 8, m) == RenderStatus::nonFinite);
    CHECK(!m.finite && m.peak == 0 && std::isnan(m.rms) && m.clippedSamples == 0);
    CHECK(!m.onsetSeconds && m.activityEndSeconds == 0 && !m.tailActiveAtEnd);
    // Long release remains a truthful final-window threshold observation.
    auto d = sound(false, false); auto c = musical();
    physical(d, "synth.1.analog.amp.release", 20);
    const auto longRelease = renderPreview(d, c);
    if (audioReady(longRelease, 228000)) CHECK(longRelease.measurements.tailActiveAtEnd);
    // The renderer must withhold raw overloaded candidate audio, not turn protection on.
    physical(d, "output.gain_db", 36); physical(d, "global.master_level", 1); physical(d, "synth.1.level", 1);
    const auto overload = renderPreview(d, c);
    empty(overload, RenderStatus::clipped); CHECK(overload.measurements.clippedSamples > 0 && overload.measurements.peak > 1);
}
void stereo_spectral_bands()
{
    begin(__func__); // Catches wrong bin bands, incomplete-window loss and antiphase mono cancellation.
    std::vector<float> l(48001), r(l.size());
    for (int band = 0; band < 3; ++band)
    {
        const double frequency[] {100, 1000, 6000};
        for (std::size_t i = 0; i < l.size(); ++i) l[i] = r[i] = float(.2 * std::sin(2 * juce::MathConstants<double>::pi * frequency[band] * double(i) / 48000));
        AudioMeasurements aligned, inverted;
        CHECK(measurePreview(l, r, 24000, aligned) == RenderStatus::ready);
        CHECK(aligned.bandFractions[std::size_t(band)] > .95);
        CHECK(near(aligned.bandFractions[0] + aligned.bandFractions[1] + aligned.bandFractions[2], 1));
        for (auto& sample : r) sample = -sample;
        CHECK(measurePreview(l, r, 24000, inverted) == RenderStatus::ready);
        for (std::size_t b = 0; b < 3; ++b) CHECK(near(aligned.bandFractions[b], inverted.bandFractions[b]));
        CHECK(near(aligned.rms, inverted.rms) && inverted.peak > .19);
        for (auto& sample : l) sample *= 1e-20f;
        for (auto& sample : r) sample *= 1e-20f;
        AudioMeasurements tiny;
        CHECK(measurePreview(l, r, 24000, tiny) == RenderStatus::ready);
        CHECK(tiny.bandFractions[std::size_t(band)] > .95 && tiny.peak < 1e-20 && tiny.peak > 0);
    }
    std::fill(l.begin(), l.end(), 0.f); std::fill(r.begin(), r.end(), 0.f);
    AudioMeasurements zero;
    CHECK(measurePreview(l, r, 24000, zero) == RenderStatus::ready);
    CHECK((zero.bandFractions == std::array<double, 3>{0, 0, 0}));
}
void invalid_and_cancelled_publication()
{
    begin(__func__); // Catches unchecked bounds, mutating rejection and stale/partial publication.
    const auto d = sound(); const auto c = musical();
    for (int block : {0, -1, 1025}) empty(renderPreview(d, c, {}, block), RenderStatus::invalidInput);
    for (double bpm : {19.999, 400.001, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
    {
        auto invalid = c; invalid.bpm = bpm; empty(renderPreview(d, invalid), RenderStatus::invalidInput);
    }
    auto invalidContext = c; invalidContext.phrase.bars = 0;
    empty(renderPreview(d, invalidContext), RenderStatus::invalidInput);
    invalidContext = c; invalidContext.phrase.noteCount = 257;
    empty(renderPreview(d, invalidContext), RenderStatus::invalidInput);
    invalidContext = c; invalidContext.phraseSource = PhraseSource(255);
    empty(renderPreview(d, invalidContext), RenderStatus::invalidInput);
    invalidContext = c; invalidContext.tempoSource = TempoSource(255);
    empty(renderPreview(d, invalidContext), RenderStatus::invalidInput);
    auto invalidProject = d; invalidProject.mode = PlaybackMode(255);
    empty(renderPreview(invalidProject, c), RenderStatus::invalidInput);
    invalidProject = d; invalidProject.sound.values[0] = std::numeric_limits<float>::quiet_NaN();
    std::array<std::byte, sizeof(ProjectDocument)> originalBytes;
    std::memcpy(originalBytes.data(), &invalidProject, sizeof invalidProject);
    empty(renderPreview(invalidProject, c), RenderStatus::invalidInput);
    CHECK(std::memcmp(originalBytes.data(), &invalidProject, sizeof invalidProject) == 0);
    for (int trigger : {1, 3})
    {
        int calls = 0; empty(renderPreview(d, c, [&] { return ++calls == trigger; }), RenderStatus::cancelled);
        CHECK(calls == trigger);
    }
    int total = 0;
    const auto baseline = renderPreview(d, c, [&] { ++total; return false; });
    CHECK(baseline.status == RenderStatus::ready && total > 10);
    if (baseline.status == RenderStatus::ready && total > 10)
    {
        int calls = 0; empty(renderPreview(d, c, [&] { return ++calls == total; }), RenderStatus::cancelled);
        CHECK(calls == total);
    }
    AudioMeasurements sentinel;
    sentinel.finite = false; sentinel.peak = 13; sentinel.rms = 7; sentinel.clippedSamples = 99;
    sentinel.onsetSeconds = 1; sentinel.bandFractions = {.1, .2, .7}; sentinel.tailActiveAtEnd = true;
    const auto saved = sentinel;
    std::vector<float> l(8192, .1f), r(l.size(), -.1f);
    for (auto body : {std::size_t(0), std::size_t(8193), std::size_t(384001)})
    {
        CHECK(measurePreview(l, r, body, sentinel) == RenderStatus::invalidInput); CHECK(same(sentinel, saved));
    }
    CHECK(measurePreview({}, {}, 1, sentinel) == RenderStatus::invalidInput); CHECK(same(sentinel, saved));
    CHECK(measurePreview(l, std::span<const float>(r.data(), r.size() - 1), 1, sentinel) == RenderStatus::invalidInput); CHECK(same(sentinel, saved));
    std::vector<float> oversized(576001);
    CHECK(measurePreview(oversized, oversized, 1, sentinel) == RenderStatus::invalidInput); CHECK(same(sentinel, saved));
    for (int trigger : {1, 2, 4}) // Initial/second scan chunk; second FFT window.
    {
        int calls = 0;
        CHECK(measurePreview(l, r, 4096, sentinel, [&] { return ++calls == trigger; }) == RenderStatus::cancelled);
        CHECK(calls == trigger && same(sentinel, saved));
    }
    int measureTotal = 0;
    AudioMeasurements complete;
    CHECK(measurePreview(l, r, 4096, complete, [&] { ++measureTotal; return false; }) == RenderStatus::ready);
    if (measureTotal > 3)
    {
        int calls = 0;
        CHECK(measurePreview(l, r, 4096, sentinel, [&] { return ++calls == measureTotal; }) == RenderStatus::cancelled);
        CHECK(same(sentinel, saved));
    }
    failNextAllocation = true;
    try
    {
        const auto allocation = renderPreview(d, c);
        failNextAllocation = false;
        empty(allocation, RenderStatus::renderFailure);
    }
    catch (const std::bad_alloc&)
    {
        failNextAllocation = false;
        CHECK(false); // An allocation escaping the renderer is a counted failure, never a debug dialog.
    }
    failNextAllocation = false;
    CHECK(d == sound() && same(c, musical()));
    // Inclusive render block bounds and exact tempo endpoints remain valid.
    auto quiet = c; quiet.phrase = {}; quiet.phrase.meter = {1, 32}; quiet.bpm = 400;
    if (audioReady(renderPreview(d, quiet, {}, 1), 192900)) CHECK(quiet.bpm == 400);
    quiet.bpm = 20;
    if (audioReady(renderPreview(d, quiet, {}, 1024), 210000)) CHECK(quiet.bpm == 20);
}
}
template<class Quality>
void explicitQualityPreview()
{
    begin("quality_prepared_mode_truth: explicit preview quality and compensated boundaries");
    if constexpr(requires(Quality q){ renderPreview(sound(),musical(),{},512,q); })
    {
        const auto d=sound(); const auto c=musical();
        const auto rt=renderPreview(d,c,{},512,static_cast<Quality>(0));
        const auto high=renderPreview(d,c,{},512,static_cast<Quality>(1));
        const auto fine=renderPreview(d,c,{},64,static_cast<Quality>(1));
        CHECK(high.status==RenderStatus::ready && fine.status==RenderStatus::ready);
        CHECK(rt.quality==static_cast<Quality>(0) && rt.latencySamples==0);
        CHECK(high.quality==static_cast<Quality>(1) && high.latencySamples>0 && high.latencyCompensated);
        CHECK(high.audio.bodyFrames==rt.audio.bodyFrames && high.audio.left.size()==rt.audio.left.size());
        CHECK(exact(high.audio.left,fine.audio.left) && exact(high.audio.right,fine.audio.right));
        empty(renderPreview(d,c,{},512,static_cast<Quality>(255)),RenderStatus::invalidInput);
        int calls=0;empty(renderPreview(d,c,[&]{return ++calls==3;},512,static_cast<Quality>(1)),RenderStatus::cancelled);
        CHECK(calls==3);
    }
    else CHECK(false);
}
int main()
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    cold_macro_binding();
    same_core_partition_and_recall(); bounded_phrase_release(); raw_signal_measurements();
    stereo_spectral_bands(); invalid_and_cancelled_publication();
    explicitQualityPreview<RenderQuality>();
    std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

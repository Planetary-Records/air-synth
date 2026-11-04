#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <memory>
#include <span>
#include <optional>
#include "DspCompatibility.h"

namespace airack
{
enum class RenderQuality : std::uint8_t { realtime, higher };
enum class EngineKind { analog, wavetable };
enum class Waveform { sine, triangle, saw, pulse };
enum class FilterMode { lowpass, bandpass, highpass };
enum class FilterType { clean, ladder, classic, vowel };
enum class VoiceMode { polyphonic, monophonic };

struct PerformanceSettings
{
    VoiceMode mode = VoiceMode::polyphonic;
    float glideSeconds = 0;
    bool legato = false;
    bool operator==(const PerformanceSettings&) const = default;
};

struct EnvelopeSettings
{
    float attack = 0.005f, decay = 0.15f, release = 0.15f, sustain = 0.65f;
    bool operator==(const EnvelopeSettings&) const = default;
};
struct OscillatorSettings
{
    Waveform waveform = Waveform::saw;
    int tableIndex = 0;
    float position = 0.0f, tune = 0.0f, detune = 0.0f;
    float level = 0.5f, pulseWidth = 0.5f;
    int unison = 1;
    float unisonCents = 10;
    bool operator==(const OscillatorSettings&) const = default;
};
enum class LfoShape { sine, triangle, saw, square };
struct LfoSettings
{
    LfoShape shape = LfoShape::sine;
    bool synced = false;
    float rateHz = 1, beats = 1;
    bool operator==(const LfoSettings&) const = default;
};
enum class ModSource { none, ampEnvelope, filterEnvelope, lfo1, lfo2, velocity, note };
enum class ModTarget { none, level, pan, osc1Level, osc2Level, osc1Tune, osc2Tune,
    osc1Detune, osc2Detune, osc1Width, osc2Width, osc1Position, osc2Position,
    noise, cutoff, resonance, filterEnvelopeAmount };
enum class ModBank { common, analog, wavetable };
struct ModRoute
{
    bool enabled = false;
    ModSource source = ModSource::none;
    ModTarget target = ModTarget::none;
    ModBank bank = ModBank::common;
    float amount = 0;
    bool operator==(const ModRoute&) const = default;
};
struct LayerSettings
{
    bool enabled = false;
    EngineKind engine = EngineKind::analog;
    std::array<OscillatorSettings, 2> oscillators {};
    float noiseLevel = 0.0f, cutoffHz = 4000.0f, resonance = 0.25f;
    FilterMode filterMode = FilterMode::lowpass;
    EnvelopeSettings ampEnvelope {}, filterEnvelope {};
    float filterEnvelopeOctaves = 0.0f, level = 0.5f, pan = 0.0f;
    std::array<LfoSettings, 2> lfos {};
    std::array<ModRoute, 16> modulation {};
    AlgorithmRevision algorithmRevision = legacyAlgorithmRevision;
    // Revision-2 wavetable layers only; other kernels ignore them.
    float unisonWidth = 1, drift = 0;
    FilterType filterType = FilterType::clean;
    float vowelSize = 1, attackCurve = .25f;
    bool operator==(const LayerSettings&) const = default;
};
struct SynthPatch
{
    std::array<LayerSettings, 4> layers = [] {
        std::array<LayerSettings, 4> result {};
        result[0].enabled = true;
        return result;
    }();
    float masterLevel = 0.5f;
    PerformanceSettings performance {};
    bool operator==(const SynthPatch&) const = default;
};

bool validateSynthPatch(const SynthPatch&) noexcept;

// Single owning thread: setPatch is allowed between renders, never concurrently.
// The private storage is allocated at construction; render/reset never allocate.
// prepare supports 8000..384000 Hz and maximum blocks 1..65536 samples.
// Unsupported renders (unprepared, non-stereo, oversized) clear all output and
// do not advance state. MIDI offsets outside [0, blockSize) are ignored.
// CC123 releases its channel; CC120 kills its channel; MIDI Stop kills all.
// Levels, pan, oscillator controls and filter cutoff/resonance/amount ramp for
// >=5 ms. ADSR times/levels apply directly. Structural modes fade out for >=5 ms,
// switch at zero layer gain, then fade in for >=5 ms; newest edits coalesce.
// Performance mode is shared by all layers. Mono selects the latest held key
// across all MIDI channels; mode edits rebuild held voices at fade zero.
// Glide (0..2 s) applies on the next mono note transition in MIDI-pitch space;
// the first note of a phrase snaps. Legato preserves phase/filter/envelopes
// and ramps velocity for >=5 ms, otherwise selected-note changes retrigger.
// Two rack-global clocks preserve phase across notes/edits; reset/prepare/Stop
// restart them at zero. Optional finite 20..400 BPM drives synced clocks;
// missing timing pauses only those clocks and is reported after valid renders.
// Route bank qualification is retained; inactive-bank routes have no effect.
// Depth/frequency edits ramp >=5 ms. Shape/sync/source/target/bank/enable changes
// use the rack structural fade. Zero-depth routes preserve the default DSP path.
// Each layer's algorithmRevision selects its voice kernel (edits use the
// structural fade). Revision 1 is the retained legacy kernel, bit-identical.
// Revision 2 reads the same settings with harmonics to 0.48 Fs, exponential
// envelopes, free-running start phases and position as a sine-to-full-wave
// opening; analog layers add slow drift and a four-pole ladder that
// self-oscillates near full resonance. Sounding revision-2 layers retrigger and
// steal without resetting phase, filter or envelope level. Reset reseeds drift.
class SynthEngine
{
public:
    SynthEngine();
    ~SynthEngine();
    SynthEngine(const SynthEngine&) = delete;
    SynthEngine& operator=(const SynthEngine&) = delete;
    bool prepare(double sampleRate, int maximumBlockSize);
    // Preparation replaces voices/tails only on success; never call from render.
    bool prepare(double sampleRate, int maximumBlockSize, RenderQuality);
    RenderQuality effectiveQuality() const noexcept;
    int latencySamples() const noexcept; // Host frames; realtime is zero.
    bool setPatch(const SynthPatch&) noexcept;
    const SynthPatch& patch() const noexcept;
    void render(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
                std::optional<double> tempoBpm = std::nullopt) noexcept;
    // Four non-overlapping stereo buffers at existing headroom, before master
    // level, plus one smoothed master gain per frame. All lengths must match;
    // the gain storage must also be disjoint. Invalid input clears supplied
    // outputs without advancing state. Empty buffers advance nothing.
    // Higher aligns the host-sampled master ramp with the FIR-delayed audio;
    // it never filters the control as audio. Mixed render uses the same path.
    void renderLayers(const std::array<juce::AudioBuffer<float>*, 4>& layers,
                      std::span<float> masterGains, const juce::MidiBuffer&,
                      std::optional<double> tempoBpm = std::nullopt) noexcept;
    bool tempoUnavailable() const noexcept;
    void reset() noexcept;
    std::size_t activeNoteCount() const noexcept;

private:
    friend class RoutedSynthEngine;
    friend struct SynthParallelTestAccess;
#ifndef NDEBUG
    struct SourceTestSnapshot {
        struct Voice {bool active=false,held=false;int note=0,channel=0;unsigned continuing=0;};
        std::uint32_t phase=0,unison=0;
        unsigned fresh=0,excluded=0,modern=0;
        std::array<Voice,16> voices{};
    };
    SourceTestSnapshot sourceTestSnapshot() const noexcept;
#endif
    // Routed processing owns the resampler; borrowed MIDI offsets remain host frames.
    bool prepareInternal(double sampleRate, int maximumBlockSize, int midiScale);
    // Routed's prepared workers borrow disjoint layers only within one callback.
    // Failed admission advances nothing; end follows all worker completion fences.
    bool beginParallelLayers(const std::array<juce::AudioBuffer<float>*, 4>&, std::span<float>,
                             const juce::MidiBuffer&, std::optional<double>, bool allowModulated=false) noexcept;
    bool beginParallelLayerSegment(const std::array<juce::AudioBuffer<float>*, 4>&, std::span<float>,
                                   juce::MidiBufferIterator first,juce::MidiBufferIterator last,
                                   std::optional<double>,bool allowModulated,unsigned excludedMelodicLayers=0) noexcept;
    void renderLayerSegment(const std::array<juce::AudioBuffer<float>*,4>&,std::span<float>,
        juce::MidiBufferIterator,juce::MidiBufferIterator,int physicalOrigin,std::optional<double>,unsigned excludedMelodicLayers=0) noexcept;
    void retireSourceLayer(std::size_t) noexcept;
    void renderParallelLayers(std::size_t firstLayer, std::size_t lastLayer, int offset, int count) noexcept;
    void endParallelLayers() noexcept;
    struct State;
    std::unique_ptr<State> state;
};
}

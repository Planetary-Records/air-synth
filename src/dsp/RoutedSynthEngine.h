#pragma once

#include "SynthEngine.h"
#include "RackLayout.h"
#include "DynamicEq.h"
#include "Saturation.h"
#include "EraReverb.h"
#include "SpaceReverbControls.h"
#include "space/runtime/ModalConverter.h"
#include "DrumEngine.h"
#include "DrumOwnership.h"
#include "DspPipeline.h"
#include "space/SpaceActivation.h"
#include <optional>

namespace airack
{
namespace detail
{
// Fixed experimental Reverb2 geometry. The final supported geometry is selected
// from source-frozen measurements; no runtime line-count selector is exposed.
inline constexpr std::array<double,8> reverb2DelaySeconds {.0193,.0239,.0297,.0371,.0437,.0533,.0617,.0719};
inline constexpr std::array<std::array<double,4>,2> reverb2AllpassSeconds {{{.0037,.0053,.0071,.0109},{.0043,.0061,.0083,.0127}}};
inline constexpr double reverb2AllpassFeedback=.6, reverb2ModulationSeconds=.00050;
}
enum class DriveShape { tanh, softSign };
struct DriveSettings
{
    float gainDb = 6;
    DriveShape shape = DriveShape::tanh;
    float toneHz = 12000, mix = 0.5f;
    bool operator==(const DriveSettings&) const = default;
};
struct DelaySettings
{
    bool synced = false;
    float seconds = 0.25f, beats = 0.5f, feedback = 0.35f, stereo = 0;
    float filterHz = 12000, mix = 0.25f;
    float saturation = 0, lowCutHz = 20, wobble = 0, wobbleRateHz = 0.8f;
    bool glide = false;
    float rightRatio = 1;
    bool pingPong = false;
    int note = 0;
    bool operator==(const DelaySettings&) const = default;
};
struct ReverbSettings
{
    float size = 0.5f, decaySeconds = 2, damping = 0.5f, predelayMs = 0, mix = 0.25f;
    bool operator==(const ReverbSettings&) const = default;
};
struct CompressorSettings
{
    float thresholdDb = -18, ratio = 4, attackMs = 10, releaseMs = 100, makeupDb = 0, mix = 1;
    bool operator==(const CompressorSettings&) const = default;
};
struct EqBandSettings
{
    float frequencyHz = 1000, gainDb = 0, q = 1;
    bool operator==(const EqBandSettings&) const = default;
};
struct EqSettings
{
    EqBandSettings lowShelf {160, 0, .70710678f};
    EqBandSettings bell {1000, 0, 1};
    EqBandSettings highShelf {8000, 0, .70710678f};
    bool operator==(const EqSettings&) const = default;
};
enum class FilterFxMode { lowpass, bandpass, highpass };
struct FilterSettings
{
    FilterFxMode mode = FilterFxMode::lowpass;
    float cutoffHz = 4000, resonance = .70710678f, driveDb = 0, mix = 1;
    bool operator==(const FilterSettings&) const = default;
};
struct ChorusSettings
{
    float rateHz = .35f, depth = .25f, baseDelayMs = 18, feedback = .1f, mix = .25f;
    bool operator==(const ChorusSettings&) const = default;
};
struct PhaserSettings
{
    float rateHz = .35f, depth = .6f, centreHz = 1300, feedback = .2f, mix = .5f;
    bool operator==(const PhaserSettings&) const = default;
};
struct FlangerSettings
{
    float rateHz = .25f, depth = .6f, baseDelayMs = 2, feedback = .35f, mix = .5f;
    bool operator==(const FlangerSettings&) const = default;
};
enum class StereoChannelMode { stereo, swap, mono };
struct StereoSettings
{
    float gainDb = 0, pan = 0, width = 1;
    StereoChannelMode channelMode = StereoChannelMode::stereo;
    bool operator==(const StereoSettings&) const = default;
};
struct DimensionSettings
{
    float depth = .6f, rateHz = .35f, width = 1, mix = .5f;
    bool operator==(const DimensionSettings&) const = default;
};
struct FrequencyShiftSettings
{
    float hz = 7, stereoOffsetHz = 0, mix = .25f;
    bool operator==(const FrequencyShiftSettings&) const = default;
};
enum class ConvolutionIr { unitImpulse, shortRoom };
struct ConvolutionSettings
{
    ConvolutionIr ir = ConvolutionIr::shortRoom;
    float predelayMs = 0, wetTrimDb = 0, mix = .25f;
    bool operator==(const ConvolutionSettings&) const = default;
};
struct FrequencySplitSettings
{
    float crossoverHz = 1000, lowGainDb = 0;
    bool lowMuted = false;
    float lowTone = 0, highGainDb = 0;
    bool highMuted = false;
    float highTone = 0, mix = 1;
    bool operator==(const FrequencySplitSettings&) const = default;
};
struct MidSideSplitSettings
{
    float midGainDb = 0;
    bool midMuted = false;
    float midTone = 0, sideGainDb = 0;
    bool sideMuted = false;
    float sideTone = 0, mix = 1;
    bool operator==(const MidSideSplitSettings&) const = default;
};
namespace detail
{
// Complete admitted-control engineering bound, prepared/cached once per rate.
double midSideSplitTailSeconds(double effectiveSampleRate) noexcept;
double frequencySplitTailSeconds(double effectiveSampleRate, AlgorithmRevision = legacyAlgorithmRevision) noexcept;
double convolutionTailSeconds(const ConvolutionSettings&, double effectiveSampleRate) noexcept;
// Prepared on the control thread. Fixed capacity, independent channel histories.
struct FrequencyShiftPreparation
{
    std::array<double,24> coefficients {};
    std::size_t count = 0;
    double tailSeconds = 0;
};
bool prepareFrequencyShift(double effectiveSampleRate, FrequencyShiftPreparation&) noexcept;
double frequencyShiftTailSeconds(const FrequencyShiftSettings&, double effectiveSampleRate) noexcept;
double dimensionTailSeconds(const DimensionSettings&, double effectiveSampleRate, AlgorithmRevision = legacyAlgorithmRevision) noexcept;
double flangerTailSeconds(const FlangerSettings&, double effectiveSampleRate, AlgorithmRevision = legacyAlgorithmRevision) noexcept;
double phaserTailSeconds(const PhaserSettings&, double effectiveSampleRate) noexcept;
double chorusTailSeconds(const ChorusSettings&, double effectiveSampleRate, AlgorithmRevision = legacyAlgorithmRevision) noexcept;
double filterTailSeconds(const FilterSettings&, double effectiveSampleRate, AlgorithmRevision = legacyAlgorithmRevision) noexcept;
// Relative finite engineering estimate at the prepared effective internal rate.
double eqTailSeconds(const EqSettings&, double effectiveSampleRate, AlgorithmRevision = legacyAlgorithmRevision) noexcept;
// The low shelf, bell and high shelf as normalized {b0, b1, b2, a1, a2}.
bool eqBandCoefficients(const EqSettings&, double effectiveSampleRate, AlgorithmRevision, std::array<std::array<double,5>,3>&) noexcept;
// Drive revision 2's oversampling filters, which also carry the dry signal.
double drive2OversamplingTailSeconds(double effectiveSampleRate) noexcept;
// How far revision 2's delay read reaches past each repeat's length.
double delayReadSupportSeconds(double effectiveSampleRate, AlgorithmRevision) noexcept;
// The delay's low cut is two equal first-order high-passes at the key over this
// ratio, sqrt(1/(sqrt(2)-1)), so the pair is 3 dB down at the key.
inline constexpr double delayLowCutCornerRatio = 1.5537739740300374;
// `delay.note`'s lengths in beats; step 0, Beats, reads `delay.beats` instead.
inline constexpr std::array<double,22> delayNoteBeats {0,1./24,1./16,1./12,3./32,1./8,1./6,3./16,1./4,1./3,3./8,1./2,2./3,3./4,1,4./3,3./2,2,8./3,3,4,6};
}
struct EffectSettings
{
    bool bypassed = false;
    DriveSettings drive;
    DelaySettings delay;
    ReverbSettings reverb;
    CompressorSettings compressor;
    EqSettings eq;
    FilterSettings filter;
    ChorusSettings chorus;
    PhaserSettings phaser;
    FlangerSettings flanger;
    StereoSettings stereo;
    DimensionSettings dimension;
    FrequencyShiftSettings frequencyShift;
    ConvolutionSettings convolution;
    FrequencySplitSettings frequencySplit;
    MidSideSplitSettings midSideSplit;
    DynamicEqSettings dynamicEq;
    SaturationSettings saturation;
    space::Settings spaceReverb;
    EraReverbSettings eraReverb;
    AlgorithmRevision algorithmRevision = legacyAlgorithmRevision;
    bool operator==(const EffectSettings&) const = default;
};
struct RoutedDrumPadParameters
{
    float gainDb=0,pan=0,tuneSemitones=0,trimStart=0,trimEnd=1,attackSeconds=0,decaySeconds=0;
    int midiNote=36,chokeGroup=0;
    bool operator==(const RoutedDrumPadParameters&) const = default;
};
struct BorrowedDrumAssignments : std::array<std::array<const drums::SampleAsset*,16>,4>
{
    constexpr BorrowedDrumAssignments() noexcept : std::array<std::array<const drums::SampleAsset*,16>,4>{} {}
    bool operator==(const BorrowedDrumAssignments&) const = default;
};
struct RoutedPatch
{
    SynthPatch synth;
    RackLayout layout;
    std::array<EffectSettings, 8> effects {};
    std::array<bool, 4> mute {}, solo {};
    float outputGainDb = 0;
    bool outputProtection = false;
    KernelCompatibility compatibility {};
    std::array<std::array<RoutedDrumPadParameters,16>,4> drumPads=[] {
        std::array<std::array<RoutedDrumPadParameters,16>,4> result{};
        for(auto& bank:result)for(std::size_t pad=0;pad<16;++pad)bank[pad].midiNote=36+static_cast<int>(pad);return result;
    }();
    std::array<bool,4> drumSource{};
    bool operator==(const RoutedPatch&) const = default;
};

bool validateRoutedPatch(const RoutedPatch&) noexcept;

// One render owner, with three prepared internal DSP workers when available.
// Prepare may allocate; setPatch/render/reset do not. Each render drains its jobs.
// All retained banks are validated transactionally. Layout owns slot identity.
// Valid empty-MIDI renders advance tails. Invalid/empty output advances nothing.
// Continuous controls ramp >=5 ms. Structural edits fade down/switch/up,
// clearing affected chain histories at zero; newest requests coalesce.
// Optional post-master protection links both channels to a -1 dBFS sample
// ceiling, with instant reduction and 50 ms release (also tracked in bypass).
// No lookahead/true-peak guarantee; heavy overload may distort. It never boosts.
class FxMonitor;
struct EqPreparationModes
{
    std::array<DynamicEqProcessingMode,8> slots {};
};
EqPreparationModes eqPreparationModesFor(const RoutedPatch&) noexcept;
class RoutedSynthEngine
{
public:
    RoutedSynthEngine();
    ~RoutedSynthEngine();
    RoutedSynthEngine(const RoutedSynthEngine&) = delete;
    RoutedSynthEngine& operator=(const RoutedSynthEngine&) = delete;
    RoutedSynthEngine(RoutedSynthEngine&&) noexcept;
    RoutedSynthEngine& operator=(RoutedSynthEngine&&) noexcept;
    bool prepare(double sampleRate, int maximumBlockSize);
    bool prepare(double sampleRate, int maximumBlockSize, RenderQuality);
    bool prepare(double sampleRate, int maximumBlockSize, RenderQuality, const EqPreparationModes&);
    DynamicEqProcessingMode effectiveEqMode(std::size_t slot) const noexcept;
    RenderQuality effectiveQuality() const noexcept;
    // Attach only with rendering stopped. The monitor outlives this engine;
    // its per-slot producers are owned by this callback and its prepared jobs.
    void attachFxMonitor(FxMonitor*) noexcept;
    int latencySamples() const noexcept;
    bool setPatch(const RoutedPatch&,std::uint64_t recallToken=0) noexcept;
    // Caller retains every asset through configured/pending views and captured hits,
    // until this engine no longer references it after worker completion. Calls serialize
    // with rendering; true or a changed pad pointer never acknowledges safe reclamation.
    bool setPatch(const RoutedPatch&,const BorrowedDrumAssignments&,std::uint64_t recallToken=0) noexcept;
    bool setPatch(const RoutedPatch&,const BorrowedDrumAssignments&,const drums::PublicationIdentity&,
                  std::uint64_t recallToken=0) noexcept;
    // Owning lifecycle caller only: resets voices/history after synchronous preparation.
    bool setPatchQuiescent(const RoutedPatch&,std::uint64_t recallToken=0);
    bool setPatchQuiescent(const RoutedPatch&,const BorrowedDrumAssignments&,std::uint64_t recallToken=0);
    bool setPatchQuiescent(const RoutedPatch&,const BorrowedDrumAssignments&,const drums::PublicationIdentity&,
                           std::uint64_t recallToken=0);
    // Render owner after the final worker fence; incomplete reports never acknowledge adoption.
    drums::ReferenceReport drumReferenceReport(std::uint64_t lifecycle) const noexcept;
    // Actual complete installed authority; unfinished source/Space fences return zero target tokens.
    drums::PublicationIdentity installedDrumIdentity() const noexcept;
    // Host-serialized stopped lifecycle only; fences jobs before clearing every raw borrower.
    void clearDrumBorrowersQuiescent() noexcept;
    std::array<double,4> sourceTailSeconds() const noexcept;
    // Space-history composition only; a mixed-route legacy envelope is separate.
    double spaceTailSeconds() const noexcept;
    void deactivateSpace() noexcept;
    std::uint64_t spacePreparationFailures() const noexcept;
    struct SpaceRenderStats {
        std::array<std::uint64_t,8> renderedPhysicalFrames{};
        std::size_t modeCount=0;
        unsigned physicalFactor=1,activeMask=0;
        bool ready=false;
        std::optional<space::runtime::ModalConverterBackend> converterBackend;
    };
    // Render owner, or the lifecycle owner with rendering stopped.
    // Counters survive reset/recall and start again only at prepare.
    SpaceRenderStats spaceRenderStats() const noexcept;
    const RoutedPatch& patch() const noexcept;
    void render(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
                std::optional<double> tempoBpm = std::nullopt) noexcept;
    // External frames are host-rate, disjoint from output; empty means unavailable.
    // A nonempty length mismatch clears output without advancing any histories.
    void render(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
                std::optional<double>, std::span<const DynamicEq::Frame> external) noexcept;
    void render(juce::AudioBuffer<float>&,const juce::MidiBuffer&,std::optional<double>,
                std::span<const DynamicEq::Frame>,std::span<const drums::RenderEvent>) noexcept;
    bool drumDispatchUnavailable() const noexcept;
    void reset() noexcept;
    std::size_t activeNoteCount() const noexcept;
    struct DspWorkerStats
    {
        unsigned prepared = 0;
        std::uint64_t blocks = 0, internalFrames = 0;
    };
    // Diagnostic reads belong to the render owner, or lifecycle owner while stopped.
    DspWorkerStats dspWorkerStats() const noexcept;
    struct SpectralStats {
        std::size_t preparedBytes=0;
        std::uint64_t framesStarted=0,framesCompleted=0,deadlineMisses=0,maximumSliceWork=0,maximumSliceAllowance=0;
    };
    SpectralStats spectralStats() const noexcept;
    bool tempoUnavailable() const noexcept;
private:
    void renderImpl(juce::AudioBuffer<float>&,const juce::MidiBuffer&,std::optional<double>,
        std::span<const DynamicEq::Frame>,std::span<const drums::RenderEvent>,bool authoritativeDrums) noexcept;
#ifndef NDEBUG
    friend struct RoutedDrumTestAccess;
    struct DrumTestSnapshot {
        const RoutedPatch* committed=nullptr;
        const RoutedPatch* pending=nullptr;
        const RoutedPatch* previous=nullptr;
        BorrowedDrumAssignments committedAssignments{},pendingAssignments{},previousAssignments{};
        drums::PublicationIdentity committedIdentity{},pendingIdentity{},previousIdentity{},installedIdentity{};
        std::uint64_t passedGeneration=0;
        std::array<bool,4> installed{},target{},configuredEnabled{};
        std::array<int,4> sourceRemaining{},gateRemaining{};
        std::array<std::size_t,4> hits{};
        std::array<std::array<const drums::SampleAsset*,48>,4> references{};
        std::array<unsigned,6> ready{};
        unsigned streams=0;
        int transitionRemaining=0;
        bool pendingValid=false,previousValid=false,installPending=false,preparing=false;
    };
    DrumTestSnapshot drumTestSnapshot() const noexcept;
    // Set only with rendering stopped; the hook observes a real claimed worker's raw borrow.
    void setDrumSourceBorrowTestHook(void*,void (*)(void*,unsigned)) noexcept;
#endif
    bool prepareWithPipelineLaunch(double,int,RenderQuality,const EqPreparationModes&,
        detail::DspPipeline::Launch,const space::SpaceActivationOptions& = {});
    struct State;
    std::unique_ptr<State> state;
};
}

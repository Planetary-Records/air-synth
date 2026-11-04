#pragma once
#include "ProjectDocument.h"
#include "SynthEngine.h"
#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace airack { struct BorrowedDrumAssignments; class InstrumentProcessor; }
namespace airack::ai
{
using CancelCheck = std::function<bool()>;
inline constexpr double previewSampleRate = 48000;
inline constexpr std::size_t maximumPreviewBodyFrames = 384000;
inline constexpr std::size_t previewTailFrames = 192000;
inline constexpr std::size_t maximumPreviewFrames = 576000;
enum class PhraseSource : std::uint8_t { captured, proposed, defaultNotes };
enum class TempoSource : std::uint8_t { captured, defaultTempo };
struct AuditionContext
{
    PatternDocument phrase;
    double bpm = 120;
    PhraseSource phraseSource = PhraseSource::captured;
    TempoSource tempoSource = TempoSource::captured;
};
struct AudioMeasurements
{
    // On invalid signal, RMS can be NaN/infinity. Peak covers finite samples;
    // clipping/activity still use raw absolute comparisons. No FFT then runs.
    bool finite = true;
    std::size_t clippedSamples = 0;
    double peak = 0, rms = 0;
    std::optional<double> onsetSeconds;
    double activityEndSeconds = 0, decaySeconds = 0;
    std::array<double, 3> bandFractions {};
    bool tailActiveAtEnd = false;
};
struct PreviewAudio
{
    std::vector<float> left, right;
    double sampleRate = previewSampleRate;
    std::size_t bodyFrames = 0;
    bool phraseTruncated = false;
    AuditionContext context;
};
enum class RenderStatus : std::uint8_t { ready, invalidInput, cancelled, nonFinite, clipped, renderFailure };
struct PreviewRenderCall {std::int64_t startNs=0,endNs=0;};
inline constexpr std::size_t maximumPreviewRenderCalls=1152;
struct PreviewCallTrace final {
    std::array<PreviewRenderCall,maximumPreviewRenderCalls> calls;
    std::uint32_t count=0;bool overflow=false;
};
namespace detail {
// Private instrumentation seam, enabled only by the existing Processor friend.
// No renderer overload, source resolver, public control or admission is added.
class PreviewCallTraceScope final {
    friend class ::airack::InstrumentProcessor;
    PreviewCallTraceScope(PreviewCallTrace*,std::atomic_bool*) noexcept;
    ~PreviewCallTraceScope();
    PreviewCallTrace* previous=nullptr;std::atomic_bool* previousCompleted=nullptr;
};
}
struct PreviewWorkDiagnostics
{
    // One steady_clock epoch, also used by Profile callback timestamps.
    std::int64_t startNs=0,endNs=0,loopLifetimeStartNs=0,loopLifetimeEndNs=0;
    std::uint64_t renderedBlocks=0,renderedFrames=0,workerBlocks=0,workerFrames=0,spaceFrames=0;
    unsigned preparedWorkers=0,spaceModes=0,spaceMask=0,physicalFactor=0;
    std::array<unsigned,4> firstCaptured{};
    bool prepared=false,spaceReady=false,referencesComplete=false;
    RenderQuality effectiveQuality=RenderQuality::realtime;
    int latencySamples=0;
};
struct RenderResult
{
    RenderStatus status = RenderStatus::invalidInput;
    PreviewAudio audio;
    AudioMeasurements measurements;
    RenderQuality quality = RenderQuality::realtime;
    int latencySamples = 0;
    bool latencyCompensated = false;
    PreviewWorkDiagnostics work;
};
RenderResult renderPreview(const ProjectDocument&,const AuditionContext&,const BorrowedDrumAssignments&,
    const CancelCheck& cancelled = {},int blockSize=512,RenderQuality quality=RenderQuality::realtime);
RenderStatus measurePreview(std::span<const float> left, std::span<const float> right,
    std::size_t bodyFrames, AudioMeasurements& output, const CancelCheck& cancelled = {});
RenderResult renderPreview(const ProjectDocument&, const AuditionContext&,
    const CancelCheck& cancelled = {}, int blockSize = 512, RenderQuality quality = RenderQuality::realtime);
}

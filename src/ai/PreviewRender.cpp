#include "PreviewRender.h"
#include "RoutedSynthEngine.h"
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <cmath>
#include <new>
#include <chrono>

namespace airack::ai
{
namespace
{
bool isCancelled(const CancelCheck& check) { return check && check(); }
RenderResult failure(RenderStatus status)
{
    RenderResult result;
    result.status = status;
    return result;
}
}

RenderStatus measurePreview(std::span<const float> left, std::span<const float> right,
    std::size_t bodyFrames, AudioMeasurements& output, const CancelCheck& cancelled) try
{
    if (left.empty() || left.size() != right.size() || left.size() > maximumPreviewFrames
        || bodyFrames == 0 || bodyFrames > maximumPreviewBodyFrames || bodyFrames > left.size())
        return RenderStatus::invalidInput;

    AudioMeasurements measured;
    double squaredSum = 0;
    std::size_t activityEnd = 0;
    const auto finalWindow = left.size() > 2400 ? left.size() - 2400 : 0;
    for (std::size_t start = 0; start < left.size(); start += 4096)
    {
        if (isCancelled(cancelled)) return RenderStatus::cancelled;
        const auto end = std::min(start + 4096, left.size());
        for (auto i = start; i < end; ++i)
        {
            bool active = false;
            for (double value : {double(left[i]), double(right[i])})
            {
                const auto magnitude = std::abs(value);
                if (std::isfinite(value)) measured.peak = std::max(measured.peak, magnitude);
                else measured.finite = false;
                // An invalid signal has no finite RMS; retain the raw NaN/Inf
                // observation. This double sum stays safe for finite floats.
                squaredSum += value * value;
                measured.clippedSamples += magnitude >= 1 ? 1u : 0u;
                active |= magnitude >= .001;
            }
            if (active)
            {
                if (!measured.onsetSeconds) measured.onsetSeconds = double(i) / previewSampleRate;
                activityEnd = i + 1;
                measured.tailActiveAtEnd |= i >= finalWindow;
            }
        }
    }
    measured.rms = std::sqrt(squaredSum / (2 * double(left.size())));
    measured.activityEndSeconds = double(activityEnd) / previewSampleRate;
    measured.decaySeconds = std::max(0., measured.activityEndSeconds - double(bodyFrames) / previewSampleRate);

    if (measured.finite && measured.peak > 0)
    {
        constexpr std::size_t fftSize = 2048, hop = 1024;
        juce::dsp::FFT fft(11);
        juce::dsp::WindowingFunction<float> hann(fftSize, juce::dsp::WindowingFunction<float>::hann, false);
        std::array<float, fftSize * 2> scratch {};
        std::array<double, 3> power {};
        for (std::size_t start = 0; start < left.size(); start += hop)
        {
            if (isCancelled(cancelled)) return RenderStatus::cancelled;
            for (auto channel : {left, right})
            {
                scratch.fill(0);
                const auto count = std::min(fftSize, channel.size() - start);
                // Only analysis copies are scaled. Double division also preserves
                // the band ratios of very quiet or near-float-maximum signals.
                for (std::size_t i = 0; i < count; ++i)
                    scratch[i] = float(double(channel[start + i]) / measured.peak);
                hann.multiplyWithWindowingTable(scratch.data(), fftSize);
                fft.performRealOnlyForwardTransform(scratch.data(), true);
                for (std::size_t bin = 0; bin <= fftSize / 2; ++bin)
                {
                    const auto frequency = double(bin) * previewSampleRate / double(fftSize);
                    const auto band = frequency < 200 ? 0u : frequency < 2000 ? 1u : 2u;
                    const double real = scratch[2 * bin], imaginary = scratch[2 * bin + 1];
                    power[band] += real * real + imaginary * imaginary;
                }
            }
        }
        const auto total = power[0] + power[1] + power[2];
        if (total > 0)
            for (std::size_t band = 0; band < power.size(); ++band) measured.bandFractions[band] = power[band] / total;
    }
    if (isCancelled(cancelled)) return RenderStatus::cancelled;
    output = measured;
    return !measured.finite ? RenderStatus::nonFinite
         : measured.clippedSamples ? RenderStatus::clipped : RenderStatus::ready;
}
catch (const std::bad_alloc&)
{
    return RenderStatus::renderFailure;
}

namespace {
thread_local PreviewCallTrace* activeCallTrace=nullptr;
thread_local std::atomic_bool* firstCallCompleted=nullptr;
}
detail::PreviewCallTraceScope::PreviewCallTraceScope(PreviewCallTrace* trace,std::atomic_bool* completed) noexcept
    :previous(activeCallTrace),previousCompleted(firstCallCompleted){activeCallTrace=trace;firstCallCompleted=completed;}
detail::PreviewCallTraceScope::~PreviewCallTraceScope(){activeCallTrace=previous;firstCallCompleted=previousCompleted;}
namespace {
std::int64_t previewWorkClock() noexcept
{return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
RenderResult renderPreviewBody(const ProjectDocument& candidate,const AuditionContext& context,
    const BorrowedDrumAssignments* assignments,const CancelCheck& cancelled,int blockSize,RenderQuality quality,PreviewWorkDiagnostics& work) try
{
    (void) parameterRegistry(); // Control-thread bootstrap for saved macro target keys.
    if (!validateProjectDocument(candidate) || !validatePatternDocument(context.phrase)
        || !std::isfinite(context.bpm) || context.bpm < 20 || context.bpm > 400
        || context.phraseSource > PhraseSource::defaultNotes || context.tempoSource > TempoSource::defaultTempo
        || blockSize < 1 || blockSize > 1024
        || (quality!=RenderQuality::realtime && quality!=RenderQuality::higher))
        return failure(RenderStatus::invalidInput);
    if (isCancelled(cancelled)) return failure(RenderStatus::cancelled);

    RoutedPatch patch;
    if (!makeRoutedPatch(candidate.sound, patch)) return failure(RenderStatus::invalidInput);
    const auto requestedFrames = std::floor(patternLengthBeats(context.phrase) * 60 / context.bpm * previewSampleRate + .5);
    const auto bodyFrames = std::size_t(std::max(1., std::min(requestedFrames, double(maximumPreviewBodyFrames))));
    const auto totalFrames = bodyFrames + previewTailFrames;
    RoutedSynthEngine engine;
    if (!(assignments?engine.setPatch(patch,*assignments):engine.setPatch(patch)) || !engine.prepare(previewSampleRate, blockSize, quality,eqPreparationModesFor(patch)))
        return failure(RenderStatus::renderFailure);
    engine.reset();
    work.prepared=true;
    struct CaptureWork final {
        RoutedSynthEngine& engine;PreviewWorkDiagnostics& work;
        ~CaptureWork(){
            if(work.loopLifetimeStartNs&&!work.loopLifetimeEndNs)work.loopLifetimeEndNs=previewWorkClock();
            const auto workers=engine.dspWorkerStats();const auto space=engine.spaceRenderStats();
            work.preparedWorkers=workers.prepared;work.workerBlocks=workers.blocks;work.workerFrames=workers.internalFrames;
            work.spaceReady=space.ready;work.spaceModes=unsigned(space.modeCount);work.spaceMask=space.activeMask;work.physicalFactor=space.physicalFactor;
            work.spaceFrames=space.renderedPhysicalFrames[6];work.effectiveQuality=engine.effectiveQuality();work.latencySamples=engine.latencySamples();
        }
    } captureWork{engine,work};
    const auto latency=static_cast<std::size_t>(engine.latencySamples());
    const auto renderFrames=totalFrames+latency;
    PatternScheduler scheduler;
    if (!scheduler.prepare(previewSampleRate, blockSize) || !scheduler.setPattern(context.phrase))
        return failure(RenderStatus::renderFailure);

    PreviewAudio audio;
    audio.left.resize(totalFrames); audio.right.resize(totalFrames);
    audio.bodyFrames = bodyFrames; audio.phraseTruncated = requestedFrames > maximumPreviewBodyFrames;
    audio.context = context;
    juce::AudioBuffer<float> block(2, blockSize);
    juce::MidiBuffer midi;
    midi.ensureSize(maximumPatternEvents * 3);
    PatternEventBuffer events;
    work.loopLifetimeStartNs=previewWorkClock();
    for (std::size_t frame = 0; frame < renderFrames;)
    {
        if (isCancelled(cancelled)) return failure(RenderStatus::cancelled);
        const auto boundary = frame < bodyFrames ? bodyFrames : renderFrames;
        const auto count = int(std::min(std::size_t(blockSize), boundary - frame));
        HostPatternTiming clock;
        clock.playing = frame < bodyFrames; clock.bpm = context.bpm;
        clock.samplePosition = std::int64_t(frame);
        clock.ppqPosition = double(frame) * context.bpm / (60 * previewSampleRate);
        const auto status = scheduler.processBlock(clock, count, PlaybackMode::internalPattern, events);
        if (status != (clock.playing ? PatternPlaybackStatus::playing : PatternPlaybackStatus::stopped))
            return failure(RenderStatus::renderFailure);
        midi.clear();
        for (std::size_t i = 0; i < events.count; ++i)
        {
            const auto& event = events.events[i];
            midi.addEvent(event.noteOn ? juce::MidiMessage::noteOn(event.channel, event.pitch, event.velocity)
                                      : juce::MidiMessage::noteOff(event.channel, event.pitch), event.sampleOffset);
        }
        block.setSize(2, count, false, false, true);
        if(auto* trace=activeCallTrace){
            if(trace->count<trace->calls.size()){
                auto& call=trace->calls[trace->count];call.startNs=previewWorkClock();
                engine.render(block,midi,context.bpm); // includes the actual worker fence
                call.endNs=previewWorkClock();++trace->count;
                if(firstCallCompleted&&trace->count==1)firstCallCompleted->store(true,std::memory_order_release);
            }else{trace->overflow=true;engine.render(block,midi,context.bpm);}
        }else engine.render(block,midi,context.bpm);
        if(!work.renderedBlocks){const auto refs=engine.drumReferenceReport(0);work.referencesComplete=refs.complete;
            for(unsigned bank=0;bank<4;++bank)for(unsigned hit=0;hit<32;++hit)work.firstCaptured[bank]+=refs.references[bank*48+16+hit]!=nullptr;}
        ++work.renderedBlocks;work.renderedFrames+=std::uint64_t(count);
        // Flush the declared fixed filter delay and retain exactly the existing
        // phrase/tail geometry. Scheduled MIDI remains at physical host times.
        const auto start=std::max(frame,latency),end=std::min(frame+std::size_t(count),renderFrames);
        if(end>start)
        {
            const auto source=start-frame,destination=start-latency,length=end-start;
            std::copy_n(block.getReadPointer(0)+source,length,audio.left.begin()+destination);
            std::copy_n(block.getReadPointer(1)+source,length,audio.right.begin()+destination);
        }
        frame += std::size_t(count);
    }
    work.loopLifetimeEndNs=previewWorkClock();
    RenderResult result;
    result.quality=engine.effectiveQuality();result.latencySamples=engine.latencySamples();result.latencyCompensated=latency>0;
    result.status = measurePreview(audio.left, audio.right, bodyFrames, result.measurements, cancelled);
    if (isCancelled(cancelled)) return failure(RenderStatus::cancelled);
    if (result.status == RenderStatus::ready) result.audio = std::move(audio);
    return result;
}
catch (const std::bad_alloc&)
{
    return failure(RenderStatus::renderFailure);
}
RenderResult renderPreviewImpl(const ProjectDocument& candidate,const AuditionContext& context,
    const BorrowedDrumAssignments* assignments,const CancelCheck& cancelled,int blockSize,RenderQuality quality) {
    PreviewWorkDiagnostics work;work.startNs=previewWorkClock();
    auto result=renderPreviewBody(candidate,context,assignments,cancelled,blockSize,quality,work);
    work.endNs=previewWorkClock();result.work=work;return result;
}
}
RenderResult renderPreview(const ProjectDocument& candidate,const AuditionContext& context,const CancelCheck& cancelled,int blockSize,RenderQuality quality) {
    for(std::uint8_t bank=0;bank<4;++bank)if(candidate.sound.layout.synthUse[bank]==SlotUse::present&&sourceModule(candidate.sound,bank)==NativeModule::drum)return failure(RenderStatus::invalidInput);
    return renderPreviewImpl(candidate,context,nullptr,cancelled,blockSize,quality);
}
RenderResult renderPreview(const ProjectDocument& candidate,const AuditionContext& context,const BorrowedDrumAssignments& assignments,const CancelCheck& cancelled,int blockSize,RenderQuality quality) {
    return renderPreviewImpl(candidate,context,&assignments,cancelled,blockSize,quality);
}
}

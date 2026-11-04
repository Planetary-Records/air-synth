#pragma once
#include "PreviewRender.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <span>

namespace airack
{
class RenderedPreview
{
public:
    RenderedPreview();
    ~RenderedPreview();
    void prepare(double hostRate);
    void release();
    bool start(const ai::PreviewAudio&, std::uint64_t capturedManualRevision);
    bool startCanonical(std::span<const float>,std::uint32_t channels,std::uint64_t capturedManualRevision);
    std::uint64_t retainedPcmBytes() const;
    bool clearStopped();
    bool retireStoppedWithoutAudio();
    void stop() noexcept;
    bool stopped() const noexcept;
    bool playing() const noexcept;
    void mix(juce::AudioBuffer<float>& committed, std::uint64_t currentManualRevision) noexcept;
private:
    struct Buffer;
    void resetUnderLock(double hostRate);
    void acknowledge() noexcept;
    mutable std::mutex controlMutex;
    std::atomic_flag audioGate=ATOMIC_FLAG_INIT;
    std::unique_ptr<const Buffer> owner;
    std::atomic<const Buffer*> publishedBuffer {nullptr};
    std::atomic<std::uint64_t> published {0}, stopGeneration {0}, completed {0};
    double rate = 0;
    const Buffer* borrowed = nullptr;
    std::uint64_t audioGeneration = 0, frame = 0, targetFrames = 0, fadeFrames = 1;
    std::uint64_t stopFrames = 0;
    double lastGain = 0, stopGain = 0;
    bool fading = false;
};
}

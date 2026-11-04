#pragma once
#include "DrumDocument.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace juce
{
class File;
}
namespace airack::drums
{
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
namespace preparation_test
{
struct Access;
}
#endif
enum class PreparationError : std::uint8_t
{
    none,
    sourceBusy,
    ioError,
    unsupportedFormat,
    malformedSource,
    tooLarge,
    identityMismatch,
    invalidSelection,
    invalidGain,
    nonFinite,
    gainRequired,
    cancelled,
    allocationFailed
};
struct GeneratedOrigin
{
    AssetId expectedFileId{}, promptId{};
    std::string generator, condition;
    std::uint64_t seed = 0;
};
struct SourcePeakRange
{
    float minimum = 0, maximum = 0;
};
struct DrumSourceInfo
{
    AssetId fileId{}, decodedPcmId{};
    std::uint32_t rate = 0, channels = 0, bits = 0, buckets = 0;
    std::uint64_t frames = 0, encodedBytes = 0;
    float peak = 0;
    bool md5Verified = false;
    std::array<std::array<SourcePeakRange, 512>, 2> overview{};
};
struct PreparedDrumSample
{
    AssetPtr asset;
    SourceProvenance source;
    float peak = 0;
};
class GeneratedDrumBytes
{
  public:
    virtual ~GeneratedDrumBytes() = default;
    virtual std::uint64_t size() const noexcept = 0;
    virtual bool readAt(std::uint64_t, std::span<std::byte>) const noexcept = 0;
};
// All operations, moves and destruction are worker-only.
class DrumSource final
{
  public:
    ~DrumSource();
    DrumSource(DrumSource &&) noexcept;
    DrumSource &operator=(DrumSource &&) noexcept;
    DrumSource(const DrumSource &) = delete;
    DrumSource &operator=(const DrumSource &) = delete;
    const DrumSourceInfo &info() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit DrumSource(std::unique_ptr<Impl>) noexcept;
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
    friend struct preparation_test::Access;
#endif
    friend bool inspectDrumSource(const juce::File &, const std::optional<GeneratedOrigin> &,
                                  const std::atomic_bool &, std::unique_ptr<DrumSource> &,
                                  PreparationError &) noexcept;
    friend bool inspectDrumSource(std::unique_ptr<GeneratedDrumBytes> &, const GeneratedOrigin &,
                                  const std::atomic_bool &, std::unique_ptr<DrumSource> &,
                                  PreparationError &) noexcept;
    friend bool prepareDrumSample(DrumSource &, std::uint64_t, std::uint64_t, float,
                                  const std::atomic_bool &, PreparedDrumSample &,
                                  PreparationError &) noexcept;
};
bool inspectDrumSource(const juce::File &, const std::optional<GeneratedOrigin> &,
                       const std::atomic_bool &, std::unique_ptr<DrumSource> &,
                       PreparationError &) noexcept;
bool inspectDrumSource(std::unique_ptr<GeneratedDrumBytes> &, const GeneratedOrigin &,
                       const std::atomic_bool &, std::unique_ptr<DrumSource> &,
                       PreparationError &) noexcept;
bool prepareDrumSample(DrumSource &, std::uint64_t begin, std::uint64_t end, float gain,
                       const std::atomic_bool &, PreparedDrumSample &, PreparationError &) noexcept;
} // namespace airack::drums
#if defined(AIRACK_DRUM_PREPARATION_TESTS)
namespace airack::drums::preparation_test
{
enum class Phase
{
    preflight,
    hash,
    decode,
    resample,
    beforeCommit,
    readPending
};
enum class ReadMode
{
    system,
    readError,
    shortSuccess,
    pendingCancel,
    pendingDeadline,
    pendingCompletionRace,
    pendingWaitFailure
};
enum class IoEvent
{
    issue,
    pending,
    cancelRequest,
    completion,
    drained,
    releaseOverlapped,
    releaseEvent,
    releaseBuffer,
    closeSourceHandle
};
struct ReadTrace
{
    std::array<IoEvent, 32> events{};
    std::uint32_t used = 0;
    std::uint32_t issued = 0, cancels = 0, completions = 0, drains = 0;
    std::uint64_t offset = 0, requestedBytes = 0, returnedBytes = 0;
    std::uint32_t completionError = 0;
    bool pending = false, valid = true;
};
struct Control
{
    void (*progress)(Phase, std::uint64_t, void *) noexcept = nullptr;
    void *context = nullptr;
    Phase readPhase = Phase::hash;
    std::uint64_t readOffset = UINT64_MAX;
    ReadMode readMode = ReadMode::system;
    ReadTrace *trace = nullptr;
    std::uint32_t decodePartition = 4096;
    std::uint32_t decoderConstructions = 0, canonicalAllocations = 0;
};
void useControl(Control *) noexcept;
struct Access
{
    static bool readAt(DrumSource &, std::uint64_t, std::span<std::uint8_t>,
                       const std::atomic_bool &, PreparationError &) noexcept;
    static bool viewPosition(DrumSource &, std::uint64_t, PreparationError &) noexcept;
};
} // namespace airack::drums::preparation_test
#endif

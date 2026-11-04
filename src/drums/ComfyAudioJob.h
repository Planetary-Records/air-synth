#pragma once
#include "ComfyHttpWindows.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <juce_core/juce_core.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace airack::drums {
namespace detail { struct ComfyAudioJobTestAccess; }
using GenerationClock = std::chrono::steady_clock;
enum class DrumGenerationStatus : std::uint8_t {
  complete, partial, invalidRequest, busy, unavailable, recipeRejected,
  submissionAmbiguous, remoteFailed, invalidResponse, timeout, cancelled,
  storageFailure, invalidAudio, allocationFailed, stale
};
enum class ComfyAttemptStatus : std::uint8_t {
  unrun, rawReady, rejected, submissionAmbiguous, remoteFailed,
  invalidResponse, invalidAudio, unavailable, timeout, cancelled, storageFailure,
  allocationFailed, stale
};
struct ComfyAudioJobRequest {
  juce::Uuid runtime = juce::Uuid::null(), requestUuid = juce::Uuid::null();
  std::uint64_t request = 0;
  std::string prompt;
  std::array<std::uint64_t, 3> seeds{};
  std::array<juce::Uuid, 3> attempts{};
  GenerationClock::time_point deadline{};
};
struct ComfyAttemptResult {
  companion::ComfyOwnedAttempt owned;
  ComfyAttemptStatus status = ComfyAttemptStatus::unrun;
  unsigned httpStatus = 0;
  std::uint64_t encodedBytes = 0;
  std::array<std::byte, 32> fileSha256{};
  std::unique_ptr<companion::ComfyRawFile> raw;
};
struct ComfyAudioBatch {
  juce::Uuid runtime = juce::Uuid::null(), requestUuid = juce::Uuid::null();
  std::uint64_t request = 0;
  DrumGenerationStatus status = DrumGenerationStatus::invalidRequest;
  std::array<ComfyAttemptResult, 3> attempts{};
  std::uint8_t rawCount = 0;
};
enum class ComfyAuthorityBoundary : std::uint8_t { submit, history, download };
struct ComfyJobAdmission {
  bool (*current)(void *, ComfyAuthorityBoundary, std::size_t) noexcept = nullptr;
  void *context = nullptr;
};
class ComfyAudioJob final {
public:
  ComfyAudioJob(companion::ComfyLoopback, juce::File ownedRequestRoot);
  ~ComfyAudioJob();
  ComfyAudioJob(const ComfyAudioJob &) = delete;
  ComfyAudioJob &operator=(const ComfyAudioJob &) = delete;
  ComfyAudioBatch run(const ComfyAudioJobRequest &, ComfyJobAdmission);
  void cancel() noexcept;
private:
  explicit ComfyAudioJob(std::unique_ptr<companion::ComfyHttpWindows>);
  friend struct detail::ComfyAudioJobTestAccess;
  std::unique_ptr<companion::ComfyHttpWindows> http;
  std::atomic_bool cancelled{false};
  std::mutex pollMutex;
  std::condition_variable pollWake;
  bool ran = false;
};
bool buildComfyAudioSubmit(const ComfyAudioJobRequest &, std::size_t ordinal,
                          std::string &destination) noexcept;
enum class OwnedHistoryStatus : std::uint8_t { absent, success, remoteFailed, invalid };
struct ComfyOwnedOutput { std::string filename, subfolder; };
OwnedHistoryStatus parseOwnedComfyHistory(std::string_view,
    const companion::ComfyOwnedAttempt &, ComfyOwnedOutput &) noexcept;
} // namespace airack::drums

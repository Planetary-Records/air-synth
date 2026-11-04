// ComfyHttpWindows.h â€” complete initial declaration
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <juce_core/juce_core.h>
#include <memory>
#include <span>
#include <string>
namespace airack::companion {
namespace detail {
struct ComfyHttpTestAccess;
enum class ComfyTestPoint {
  rootTransition,
  beforeSend,
  sendPending,
  headers,
  chunk,
  beforeRename,
  afterRename,
  beforeDisposition,
  afterClose
};
enum class ComfyNetworkTestPoint {
  callbackLeaving, handlesClosed, beforeUnloadWait, afterUnloadWait,
  beforeOwnerRelease, resultSelected
};
enum class ComfyTestFault {
  none,
  createFile,
  write,
  flush,
  rename,
  disposition,
  read,
  shortRead,
  identity,
  createUnloadEvent, armUnloadEvent, closeReportOnce, waitReportOnce,
  setupAfterArm
};
struct ComfyTestControl {
  void (*point)(void *, ComfyTestPoint, const juce::File &) noexcept = nullptr;
  void *context = nullptr;
  ComfyTestFault fault = ComfyTestFault::none;
  unsigned renameCalls = 0, dispositionCalls = 0, osError = 0,
           firstBodyChunk = 0;
  const wchar_t *finalBasename = nullptr;
  void (*sessionObserved)(void *, void *, void *, const void *) noexcept = nullptr;
  void (*networkPoint)(void *, ComfyNetworkTestPoint, unsigned) noexcept = nullptr;
};
} // namespace detail
enum class ComfyLoopback { ipv4, ipv6 };
enum class ComfyOperation { submit, history, cancelOwned, download };
enum class ComfyHttpStatus {
  ok,
  invalidRequest,
  unavailable,
  httpError,
  timeout,
  cancelled,
  tooLarge,
  invalidResponse,
  storageFailure
};
struct ComfyOwnedAttempt {
  juce::Uuid runtime = juce::Uuid::null(), prompt = juce::Uuid::null();
  std::uint64_t request = 0, seed = 0;
};
struct ComfyHttpRequest {
  ComfyOperation operation = ComfyOperation::history;
  ComfyOwnedAttempt owned;
  std::string submitJson; // empty except submit; no credentials/headers
  std::string filename, subfolder; // empty except download
};
class ComfyRawFile {
public:
  ~ComfyRawFile(); // deletes only its own file; caller must be off audio
  ComfyRawFile(const ComfyRawFile &) = delete;
  ComfyRawFile &operator=(const ComfyRawFile &) = delete;
  const juce::File &file() const noexcept { return path; }
  bool readAt(std::uint64_t offset,
              std::span<std::byte> destination) const noexcept;
  std::uint64_t size() const noexcept { return byteCount; }
  const std::array<std::byte, 32> &sha256() const noexcept { return digest; }

private:
  ComfyRawFile() = default;
  juce::File path;
  std::uint64_t byteCount = 0;
  std::array<std::byte, 32> digest{};
  struct Identity;
  std::unique_ptr<Identity>
      identity; // held file/directory identity for cleanup
  friend class ComfyHttpWindows;
};
struct ComfyHttpReply {
  ComfyHttpStatus status = ComfyHttpStatus::unavailable;
  unsigned httpStatus = 0;
  bool requestMayHaveReachedServer = false;
  std::string json;
  std::unique_ptr<ComfyRawFile> raw;
  bool submitRejected = false;
};
class ComfyHttpWindows {
public:
  explicit ComfyHttpWindows(ComfyLoopback, juce::File ownedRequestRoot);
  ~ComfyHttpWindows();
  ComfyHttpWindows(const ComfyHttpWindows &) = delete;
  ComfyHttpWindows &operator=(const ComfyHttpWindows &) = delete;
  // Off audio: serial perform, concurrent abort only. Return retires session
  // callbacks. Join perform and every abort caller before destruction/unload.
  ComfyHttpReply perform(const ComfyHttpRequest &, const std::atomic_bool &,
                         std::chrono::steady_clock::time_point deadline);
  void abort() noexcept; // sticky for this instance; no remote request
private:
  ComfyHttpWindows(ComfyLoopback, juce::File, std::uint16_t fixturePort);
  struct Impl;
  std::unique_ptr<Impl> impl;
  void setTestControl(detail::ComfyTestControl *) noexcept;
  static std::array<unsigned, 2> liveNetworkState() noexcept;
  friend struct detail::ComfyHttpTestAccess;
};
} // namespace airack::companion

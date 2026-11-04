#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>

namespace airack::diagnostics
{
enum class Component { plugin, companion };
enum class Stage { startup, launch, connection, account, capture, backend, provider, parse, admission, render, terminal };
struct Event
{
    Component component = Component::plugin;
    Stage stage = Stage::terminal;
    juce::Uuid runtime = juce::Uuid::null();
    std::uint64_t requestId = 0;
    int generationStatus = -1, providerStatus = -1, proposalStatus = -1, renderStatus = -1;
    int runtimeStatus = -1;
    int httpStatus = 0;
    unsigned calls = 0, nativeError = 0;
    juce::String errorCode;
    juce::Uuid session = juce::Uuid::null();
    std::uint64_t connectionId = 0, jobId = 0, elapsedMs = 0;
    juce::String model, providerRequestId;
    bool interrupted = false;
    std::uint64_t outputBytes = 0;
};
inline constexpr std::int64_t maximumLogBytes = 512 * 1024;
inline constexpr int archivedLogCount = 3;
// Only call on a control/message or worker thread, never the audio callback.
// Best effort: disk, contention and allocation failures cannot escape.
juce::File directory() noexcept;
juce::String safeErrorCode(const juce::String&);
bool write(const Event&) noexcept;
// Explicit directory seam for file-level tests; production uses write(Event).
bool write(const juce::File& directory, const Event&) noexcept;
}

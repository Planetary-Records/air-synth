#pragma once
#include "CompanionRuntimeState.h"
#include <functional>

namespace airack::companion
{
inline constexpr std::uint64_t runtimeIoWaitMs = 20;
inline constexpr std::uint64_t runtimeReplyTimeoutMs = 30000;
inline constexpr std::uint64_t runtimeWorkerStartupMs = 5000;
inline constexpr std::uint64_t runtimeWorkerDrainMs = 65000;
using RuntimeRunJob = std::function<Frame(const RuntimeJob&)>;
using RuntimeCancelJob = std::function<void(const RuntimeJob&)>;
struct RuntimeServerResult
{
    RuntimeStatus status = RuntimeStatus::unavailable;
    bool workerRetained = false;
};
class RuntimeServer
{
public:
    RuntimeServer(RuntimeRunJob, RuntimeCancelJob);
    ~RuntimeServer();
    RuntimeServer(const RuntimeServer&) = delete;
    RuntimeServer& operator=(const RuntimeServer&) = delete;
    RuntimeServerResult run(const std::atomic_bool& stop, const juce::String& testSuffix = {});
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
namespace detail
{
struct RuntimeServerTestAccess
{
    static std::size_t retainedWorkers() noexcept;
    static bool admissionAvailable() noexcept;
    static void reapRetained();
    static void pauseBeforePickup(bool) noexcept;
    static bool pickupPaused() noexcept;
    static bool quitPublished() noexcept;
    static void pauseAfterReplyStart(bool) noexcept;
    static bool replyStartPaused() noexcept;
};
}
}

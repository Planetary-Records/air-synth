#pragma once
#include "CompanionPipe.h"

namespace airack::companion
{
inline constexpr const char* companionExecutableName = "AI Rack Synth Companion.exe";
struct CompanionDirectories { juce::File credentials, preferences; };
CompanionDirectories companionDirectories();
juce::File companionExecutable();
enum class LaunchRole { provider,mcpService };
struct LaunchOptions { juce::File executable; juce::String pipeSuffix; juce::File fixtureRoot; LaunchRole role=LaunchRole::provider; };
enum class LaunchStatus { started, reused, unavailable };
struct LaunchResult { LaunchStatus status = LaunchStatus::unavailable; PipePeer peer; };
namespace detail { struct CompanionLaunchTestAccess; }
// One caller-owned control/message thread per object, never the audio thread.
// Starting is not service readiness: callers must validate the wire welcome.
class CompanionLaunch
{
public:
    CompanionLaunch(); ~CompanionLaunch();
    CompanionLaunch(CompanionLaunch&&) noexcept;
    CompanionLaunch& operator=(CompanionLaunch&&) noexcept;
    CompanionLaunch(const CompanionLaunch&) = delete;
    CompanionLaunch& operator=(const CompanionLaunch&) = delete;
    LaunchResult start(const LaunchOptions&);
    bool running() const noexcept;
    PipePeer peer() const noexcept;
    void release() noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl;
    friend struct detail::CompanionLaunchTestAccess;
};
namespace detail
{
// Borrow until move/start/release/destruction. DuplicateHandle before releasing.
struct CompanionLaunchTestAccess
{
    static void* borrowProcessHandle(const CompanionLaunch&) noexcept;
};
}
}

#pragma once
#include "CompanionRuntimeState.h"
#include <functional>

namespace airack::companion
{
using BrowserOpen = std::function<bool(const juce::String&)>;
struct BackendOptions
{
    juce::File credentialDirectory, preferenceDirectory;
    rack::oauth::Transport transport;
    rack::oauth::Clock clock;
    BrowserOpen openBrowser;
};
// Ordinary methods are serial worker-only. Only cancel is cross-thread safe.
// The caller owns this object until every ordinary call/cancel has returned.
class CompanionBackend
{
public:
    explicit CompanionBackend(BackendOptions);
    ~CompanionBackend();
    CompanionBackend(const CompanionBackend&) = delete;
    CompanionBackend& operator=(const CompanionBackend&) = delete;
    Frame run(const RuntimeJob&);
    void cancel(const RuntimeJob&) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
namespace detail
{
struct CompanionBackendTestAccess
{
    static void pauseBeforeSignIn(bool) noexcept;
    static bool signInPaused() noexcept;
};
}
}

#pragma once
#include "CompanionWire.h"
#include <array>
#include <memory>

namespace airack::companion
{
inline constexpr std::size_t pipeChunkBytes = 65536;
inline constexpr std::uint64_t pipeFrameTimeoutMs = 5000;
inline constexpr std::uint32_t pipeRetirementWaitMs = 250;
inline constexpr std::size_t maximumPipeOwners = 34;
inline constexpr std::uint32_t maximumPipeInstances = 18;
enum class PipeDomain { companion,mcp };
enum class PipeStatus : std::uint8_t { ready, pending, complete, busy, closed, timeout, unauthorized, invalidMessage, unavailable };
struct PipePeer { std::uint32_t processId=0; std::uint64_t creationTime=0; };
juce::String currentUserPipeName(const juce::String& testSuffix = {}, PipeDomain domain=PipeDomain::companion);
namespace detail { struct PipeTestAccess; }
// Borrowed events are valid only until this owner is polled/moved/closed. Wait on
// nonnull active events only; never close them or mutate an owner during a wait.
// All operations belong to one control/background IO thread, never audio.
class PipeConnection
{
public:
    PipeConnection(); ~PipeConnection();
    PipeConnection(PipeConnection&&) noexcept;
    PipeConnection& operator=(PipeConnection&&) noexcept;
    PipeStatus connect(const juce::String& testSuffix = {}, PipeDomain domain=PipeDomain::companion);
    bool connected() const noexcept;
    PipePeer peer() const noexcept;
    PipeStatus beginRead(); PipeStatus pollRead(Frame&);
    PipeStatus beginWrite(const Frame&,std::uint64_t timeoutMs=pipeFrameTimeoutMs); PipeStatus pollWrite();
    std::array<void*,2> pendingEvents() const noexcept;
    void close() noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl;
    friend class PipeListener; friend struct detail::PipeTestAccess;
};
class PipeListener
{
public:
    PipeListener(); ~PipeListener();
    PipeListener(PipeListener&&) noexcept;
    PipeListener& operator=(PipeListener&&) noexcept;
    PipeStatus open(const juce::String& testSuffix = {}, PipeDomain domain=PipeDomain::companion);
    PipeStatus pollAccept(PipeConnection&);
    void* pendingEvent() const noexcept;
    void close() noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl;
    friend struct detail::PipeTestAccess;
};
namespace detail
{
// Narrow native-test seams: expected identity, early-connect race and completion
// observation. These never replace handles, token checks, IO or cancellation.
struct PipeTestAccess
{
    static void expectPeerSid(PipeConnection&,const juce::String&);
    static void expectPeerSid(PipeListener&,const juce::String&);
    static void pauseConnect(PipeListener&,bool);
    static void withholdCompletion(PipeConnection&,bool);
    static void withholdCompletion(PipeListener&,bool);
    static void releaseWithheldCompletions();
    static void reapRetired();
    static std::size_t retainedOwners() noexcept;
    static bool admissionAvailable() noexcept;
};
}
}

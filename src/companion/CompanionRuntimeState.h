#pragma once
#include "CompanionPipe.h"
#include "LocalPreviewExchange.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <variant>

namespace airack::companion
{
namespace detail { struct RuntimeStateTestAccess; }
inline constexpr std::size_t maximumRuntimeConnections = 16;
inline constexpr std::size_t maximumQueuedRuntimeJobs = 16;
inline constexpr std::uint64_t runtimeHelloTimeoutMs = 5000;
inline constexpr std::size_t maximumUnsentRuntimeReplyBytes =
    maximumControlPayloadBytes + maximumResultPayloadBytes;
using RuntimeConnectionId = std::uint64_t;
using RuntimeTime = std::chrono::steady_clock::time_point;

class JobCancellation
{
public:
    bool cancelled() const noexcept;
private:
    std::atomic_bool requested {false};
    friend class RuntimeState;
};
struct RuntimeJob
{
    std::uint64_t jobId = 0;
    RuntimeConnectionId connectionId = 0;
    juce::Uuid session = juce::Uuid::null();
    PipePeer peer;
    std::variant<ai::GenerationRequest, AccountCommand> input;
    std::shared_ptr<JobCancellation> cancellation;
    std::shared_ptr<const GroupPerformanceCapture> groupContext;
    std::shared_ptr<const ai::LocalPreviewContext> localPreview;
    std::shared_ptr<LocalPreviewExchange> previewExchange;
    RuntimeTime localDeadline{};
    std::shared_ptr<const struct RuntimeLocalGroupContext> localGroupContext;
};
struct RuntimeLocalPeer
{
    GroupUuid runtime;
    std::uint64_t loadRevision=0,editRevision=0;
    ai::PreviewSampleMap map;
    ai::PreviewDigest mapDigest{};
    bool metadataAvailable=false;
};
struct RuntimeLocalGroupContext
{
    std::array<RuntimeLocalPeer,maximumSelectedGroupPeers> peers{};
    std::uint8_t peerCount=0;
};
// One control/IO owner calls these methods. Captures are immutable; the bounded
// exchange and cancellation token coordinate the one waiting backend worker.
class RuntimeState
{
public:
    RuntimeState(); ~RuntimeState();
    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;
    RuntimeConnectionId open(PipePeer, RuntimeTime);
    bool alive(RuntimeConnectionId) const noexcept;
    std::size_t connectionCount() const noexcept;
    std::size_t registeredCount() const noexcept;
    std::size_t queuedJobCount() const noexcept;
    bool jobRunning() const noexcept;
    RuntimeStatus receive(RuntimeConnectionId, const Frame&, RuntimeTime);
    bool takeJob(RuntimeJob& output);
    bool finishJob(const RuntimeJob&, Frame reply);
    bool offerPreview(const RuntimeJob&,const ai::PreviewEvaluate&);
    bool acceptPreviewReceipt(RuntimeConnectionId,const Frame&,RuntimeTime);
    bool takeReply(RuntimeConnectionId, Frame& output);
    void finishReply(RuntimeConnectionId, bool delivered);
    void disconnect(RuntimeConnectionId) noexcept;
    void expire(RuntimeTime) noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl;
    friend struct detail::RuntimeStateTestAccess;
};
namespace detail
{
struct RuntimeStateTestAccess
{
    static void setNextPreviewTicket(RuntimeState&,std::uint64_t) noexcept;
    static void observePreviewExchangeAllocation(RuntimeState&,void*,void (*)(void*) noexcept) noexcept;
};
}
}

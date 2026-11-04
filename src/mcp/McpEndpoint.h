#pragma once
#include "McpService.h"
#include <memory>
#include <array>
#include <atomic>
#include <cstddef>
namespace airack::mcp {
namespace detail {struct EndpointTestAccess;}
struct EndpointMetadata { juce::Uuid instance,epoch;juce::String label,version;bool enabled=true;juce::var capabilities; };
struct EndpointObservation { bool available=false;unsigned clients=0;std::uint64_t requests=0; };
class Endpoint {
public:
    Endpoint();~Endpoint();
    Endpoint(const Endpoint&)=delete;Endpoint& operator=(const Endpoint&)=delete;
    bool start(const ServiceOptions&,const EndpointMetadata&);
    bool take(Command&);bool takeControl(Command&);bool reply(Reply&&);void update(const EndpointMetadata&);void close() noexcept;
    bool connected() const noexcept;EndpointMetadata metadata() const;
    EndpointObservation observation() const;
private:
    struct Impl;std::unique_ptr<Impl> impl;
    friend struct detail::EndpointTestAccess;
};
namespace detail {

enum class RequestStage : std::uint32_t { admitted=1,serviceReply,serviceLate,serviceExpired,enqueued,taken,takeExpired,endpointReply,executeBegin,executeEnd,executeExpired,previewEntryExpired,previewPollExpired,previewPreparedExpired };
// Private fixture data, never protocol data; slots publish once and never wrap.
struct RequestDiagnosticRow {std::uint64_t tick=0,deadline=0,route=0,client=0,sampledTick=0;std::uint32_t stage=0,tool=0;std::int32_t status=-1;std::atomic<std::uint32_t> ready{0};};
struct RequestDiagnosticData {std::uint64_t magic=0x4149524d43503138ULL,version=1,capacity=2048,rowBytes=sizeof(RequestDiagnosticRow);std::atomic<std::uint64_t> next{0},overflow{0};std::array<RequestDiagnosticRow,2048> rows;};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free&&std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(sizeof(RequestDiagnosticRow)==56&&offsetof(RequestDiagnosticData,rows)==48);
struct EndpointTestAccess {
    static void recordDiagnostic(Endpoint&,RequestStage,const Command&,int status=-1,std::uint64_t sampledTick=0) noexcept;
    static void armReplyContention(Endpoint&,std::atomic_bool* atReply,const std::atomic_bool* held);
    static void holdHandoff(Endpoint&,std::atomic_bool& entered,const std::atomic_bool& release);
    static bool controlPending(Endpoint&,CommandControl);
    static bool commandPending(Endpoint&,Tool);
    static std::size_t pendingResourceReads(Endpoint&);
    static std::array<std::size_t,4> queueUsage(Endpoint&);
};
}
}

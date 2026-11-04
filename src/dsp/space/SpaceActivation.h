#pragma once
#include "runtime/ModalService.h"
#include <atomic>
#include <memory>

namespace airack::space {
// One publisher and one consumer. Only the producer replaces owning payloads.
template<class Payload> class OwnedLatestMailbox {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
public:
    void publish(Payload&& value) noexcept {
        buffers_[producer_]=std::move(value);
        producer_=exchange_.exchange(producer_|dirty,std::memory_order_acq_rel)&indexMask;
    }
    const Payload* takeView() noexcept {
        if(!(exchange_.load(std::memory_order_acquire)&dirty))return nullptr;
        consumer_=exchange_.exchange(consumer_,std::memory_order_acq_rel)&indexMask;
        return &buffers_[consumer_];
    }
private:
    static constexpr unsigned dirty=4,indexMask=3;
    std::array<Payload,3> buffers_{};
    unsigned producer_=0,consumer_=1;
    std::atomic<unsigned> exchange_{2};
};
struct SpaceActivationRequest {
    runtime::ModalSlotSettings settings;
    runtime::ModalSlotIdentities identities{};
    std::uint64_t key=0;
    unsigned activeMask=0,installMask=255;
};
struct SpaceActivationOptions {
    // Off-audio test instrumentation; null in the product.
    void (*checkpoint)(void*,std::uint64_t,detail::PreparationControl) noexcept=nullptr;
    void* context=nullptr;
};
class SpaceActivation {
public:
    enum class Result {pending,ready,failed};
    SpaceActivation();~SpaceActivation();
    SpaceActivation(const SpaceActivation&)=delete;SpaceActivation& operator=(const SpaceActivation&)=delete;
    // Quiescent owning path; actual initial coefficients are installed synchronously.
    bool prepare(double logicalRate,unsigned factor,const SpaceActivationRequest&,SpaceActivationOptions={});
    void request(const SpaceActivationRequest&) noexcept;
    Result pollReady(const SpaceActivationRequest&) noexcept;
    bool installReady(const SpaceActivationRequest&) noexcept;
    // Callback, after its last Space segment is closed and histories are inactive.
    bool acknowledgeInactive() noexcept;
    runtime::ModalService& service() noexcept;
    const runtime::ModalService& service() const noexcept;
    std::uint64_t failureCount() const noexcept;
private:
    struct State;std::unique_ptr<State> state_;
};
}

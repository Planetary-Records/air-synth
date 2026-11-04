#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace airack::space::detail {
// One publisher, one taker; construction/destruction require quiescence.
template<class Payload> class LatestMailbox {
    static_assert(std::is_trivially_copyable_v<Payload>);
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
public:
    LatestMailbox()=default;
    LatestMailbox(const LatestMailbox&)=delete;
    LatestMailbox& operator=(const LatestMailbox&)=delete;
    void publish(const Payload& value) noexcept {
        buffers_[producer_]=value;
        producer_=exchange_.exchange(producer_|dirty,std::memory_order_acq_rel)&indexMask;
    }
    bool take(Payload& value) noexcept {
        if(const auto* view=takeView()){value=*view;return true;}
        return false;
    }
    // Stable consumer-owned view until the next successful take/takeView.
    // Empty takes do not release the current view. Destruction needs quiescence.
    const Payload* takeView() noexcept {
        if(!(exchange_.load(std::memory_order_acquire)&dirty))return nullptr;
        consumer_=exchange_.exchange(consumer_,std::memory_order_acq_rel)&indexMask;
        return &buffers_[consumer_];
    }
private:
    static constexpr std::uint32_t dirty=4,indexMask=3;
    std::array<Payload,3> buffers_{};
    std::uint32_t producer_=0,consumer_=1;
    std::atomic<std::uint32_t> exchange_{2};
};
}

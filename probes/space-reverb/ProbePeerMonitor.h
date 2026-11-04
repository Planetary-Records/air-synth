#pragma once
#include "ProbePeerGuard.h"
#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace airack::space::probe {
class ConcurrentPeerGuard {
    using Clock=std::chrono::steady_clock;
    static_assert(std::atomic<unsigned>::is_always_lock_free&&std::atomic<std::int64_t>::is_always_lock_free);
    bool controllers_=false;
public:
    using Scanner=PeerStatus(*)(void*,bool,PeerDetails&);
    using RefreshWait=void(*)(void*) noexcept;
    struct Statistics {std::size_t scans=0,waits=0;double maximumScanSeconds=0;};
private:
    Scanner scanner_=nullptr;void* context_=nullptr;
    RefreshWait refreshWait_=nullptr;void* refreshContext_=nullptr;
    std::atomic<unsigned> stopping_{0},ready_{0},latched_{0};
    std::atomic<std::int64_t> requested_{0},acknowledged_{-1},completed_{0};
    PeerDetails details_;Statistics statistics_;
    std::mutex waitMutex_;
    std::condition_variable wake_;
    std::thread worker_;
    static PeerStatus nativeScan(void*,bool controllers,PeerDetails& details){return nativePeerStatus(0,controllers,&details);}
    static void blockingRefresh(void*) noexcept {std::this_thread::sleep_for(std::chrono::milliseconds(1));}
    void run() noexcept {
        auto next=Clock::time_point::min();
        while(!stopping_.load(std::memory_order_acquire)){
            const auto now=Clock::now();const auto ticket=requested_.load(std::memory_order_acquire);
            if(now>=next||ticket>acknowledged_.load(std::memory_order_acquire)){
                PeerDetails observed;PeerStatus state=PeerStatus::unavailable;
                try{state=scanner_(context_,controllers_,observed);}catch(...){state=PeerStatus::unavailable;}
                const auto end=Clock::now();++statistics_.scans;
                const double duration=std::chrono::duration<double>(end-now).count();if(duration>statistics_.maximumScanSeconds)statistics_.maximumScanSeconds=duration;
                if(state!=PeerStatus::idle){details_=observed;latched_.store(state==PeerStatus::present?1:2,std::memory_order_release);break;}
                completed_.store(end.time_since_epoch().count(),std::memory_order_release);ready_.store(1,std::memory_order_release);
                acknowledged_.store(ticket,std::memory_order_release);next=now+std::chrono::milliseconds(100);
            }
            ++statistics_.waits;
            std::unique_lock lock(waitMutex_);
            wake_.wait_until(lock,next,[&]{return stopping_.load(std::memory_order_acquire)
                ||requested_.load(std::memory_order_acquire)>acknowledged_.load(std::memory_order_acquire);});
        }
    }
    void running() const {
        checkFailure();if(stopping_.load(std::memory_order_acquire))throw PeerObserved{"inventory_monitor_stopped",{}};
    }
public:
    explicit ConcurrentPeerGuard(bool controllers=false,Scanner scanner=nullptr,void* context=nullptr,RefreshWait refreshWait=nullptr,void* refreshContext=nullptr):controllers_(controllers),scanner_(scanner?scanner:nativeScan),context_(context),refreshWait_(refreshWait?refreshWait:blockingRefresh),refreshContext_(refreshContext),worker_([this]{run();}) {}
    ~ConcurrentPeerGuard(){stop();}
    ConcurrentPeerGuard(const ConcurrentPeerGuard&)=delete;
    ConcurrentPeerGuard& operator=(const ConcurrentPeerGuard&)=delete;
    void checkFailure() const {
        const auto value=latched_.load(std::memory_order_acquire);
        if(value)throw PeerObserved{value==1?"peer_native_work":"process_inventory_unavailable",details_};
    }
    void force(){
        running();std::int64_t ticket;
        {const std::lock_guard lock(waitMutex_);ticket=requested_.fetch_add(1,std::memory_order_acq_rel)+1;}
        wake_.notify_one();const auto deadline=Clock::now()+std::chrono::milliseconds(250);
        while(acknowledged_.load(std::memory_order_acquire)<ticket){running();if(Clock::now()>=deadline)throw PeerObserved{"inventory_monitor_stale",{}};refreshWait_(refreshContext_);}
        (*this)();
    }
    void operator()() const {
        running();if(!ready_.load(std::memory_order_acquire))throw PeerObserved{"inventory_monitor_unready",{}};
        const auto last=Clock::time_point(Clock::duration(completed_.load(std::memory_order_acquire)));
        if(Clock::now()-last>std::chrono::milliseconds(250))throw PeerObserved{"inventory_monitor_stale",{}};
    }
    void stop() noexcept {
        {const std::lock_guard lock(waitMutex_);stopping_.store(1,std::memory_order_release);}
        wake_.notify_one();if(worker_.joinable())worker_.join();
    }
    Statistics statistics() const noexcept {return worker_.joinable()?Statistics{}:statistics_;}
};
}

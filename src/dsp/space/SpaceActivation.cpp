#include "SpaceActivation.h"
#include "SpaceReverbMailbox.h"
#include <chrono>
#include <thread>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif
namespace airack::space {
namespace {
enum class Lifecycle:unsigned {stopped,starting,standby,active,retiring};
struct ReadyResult {
    std::unique_ptr<const runtime::ModalReadyBundle> bundle;
    std::uint64_t key=0;
    runtime::ModalSlotIdentities identities{};
    unsigned activeMask=0,installMask=0;
    bool success=false;
};
static_assert(std::is_trivially_copyable_v<SpaceActivationRequest>);
static_assert(std::atomic<Lifecycle>::is_always_lock_free);
}
struct SpaceActivation::State {
    runtime::ModalService rendering;
    detail::LatestMailbox<SpaceActivationRequest> requests;
    OwnedLatestMailbox<ReadyResult> results;
    std::atomic<std::uint64_t> newestKey{0};
    std::atomic<Lifecycle> lifecycle{Lifecycle::stopped};
    std::atomic<bool> stopping{false};
    std::atomic<int> startup{0};
    std::thread worker;
    SpaceActivationOptions options;
    double rate=0;unsigned factor=1;
    const ReadyResult* view=nullptr;
    std::uint64_t failedKey=0,failures=0;
    ~State(){stopping.store(true,std::memory_order_release);if(worker.joinable())worker.join();}
    struct Control {
        State* state;std::uint64_t key;
        static bool cancelled(void* raw) noexcept {const auto& c=*static_cast<Control*>(raw);return c.state->stopping.load(std::memory_order_acquire)||c.state->newestKey.load(std::memory_order_acquire)!=c.key;}
    };
    void run() noexcept {
        bool prioritized=true;
#if defined(_WIN32)
        prioritized=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL)!=0;
#endif
        startup.store(prioritized?1:-1,std::memory_order_release);if(!prioritized)return;
        SpaceActivationRequest desired;bool have=false;std::uint64_t completed=0;
        while(!stopping.load(std::memory_order_acquire)){
            SpaceActivationRequest next;if(requests.take(next)){desired=next;have=true;}
            if(have&&desired.activeMask==0){
                auto expected=Lifecycle::standby;
                if(lifecycle.compare_exchange_strong(expected,Lifecycle::retiring,std::memory_order_acq_rel)){
                    (void)rendering.stopWorkers();lifecycle.store(Lifecycle::stopped,std::memory_order_release);
                }
            }else if(have&&desired.key!=completed){
                bool workers=true;
                auto expected=Lifecycle::stopped;
                if(lifecycle.compare_exchange_strong(expected,Lifecycle::starting,std::memory_order_acq_rel)){
                    workers=rendering.startWorkers();
                    lifecycle.store(workers?Lifecycle::standby:Lifecycle::stopped,std::memory_order_release);
                }
                Control cancelled{this,desired.key};detail::PreparationControl control{&Control::cancelled,&cancelled};
                if(options.checkpoint)options.checkpoint(options.context,desired.key,control);
                auto bundle=workers?runtime::prepareModalBundle(rate,128,desired.key,desired.settings,desired.identities,control,factor):nullptr;
                if(!Control::cancelled(&cancelled)){
                    ReadyResult out;out.key=desired.key;out.identities=desired.identities;out.activeMask=desired.activeMask;out.installMask=desired.installMask;out.success=bundle!=nullptr;out.bundle=std::move(bundle);results.publish(std::move(out));completed=desired.key;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};
SpaceActivation::SpaceActivation()=default;SpaceActivation::~SpaceActivation()=default;
bool SpaceActivation::prepare(double rate,unsigned factor,const SpaceActivationRequest& initial,SpaceActivationOptions options) try {
    if(initial.key==0||initial.activeMask>255||initial.installMask!=255)return false;
    auto candidate=std::make_unique<State>();candidate->rate=rate;candidate->factor=factor;candidate->options=options;
    runtime::ModalServiceOptions serviceOptions;serviceOptions.threaded=false;serviceOptions.factor=factor;
    serviceOptions.converterBackend=runtime::modalConverterAvx2Available()?runtime::ModalConverterBackend::mirroredAvx2:runtime::ModalConverterBackend::mirroredSse2;
    if(!candidate->rendering.prepareStorage(rate,128,serviceOptions))return false;
    if(initial.activeMask){auto bundle=runtime::prepareModalBundle(rate,128,initial.key,initial.settings,initial.identities,{},factor);
        if(!bundle||!candidate->rendering.installBundle(*bundle,initial.key)||!candidate->rendering.startWorkers())return false;
        candidate->lifecycle.store(Lifecycle::active,std::memory_order_relaxed);
    }
    candidate->newestKey.store(initial.key,std::memory_order_relaxed);
    auto* borrowed=candidate.get();candidate->worker=std::thread([borrowed]{borrowed->run();});
    while(candidate->startup.load(std::memory_order_acquire)==0)std::this_thread::yield();
    if(candidate->startup.load(std::memory_order_acquire)<0)return false;
    state_=std::move(candidate);return true;
}catch(...){return false;}
void SpaceActivation::request(const SpaceActivationRequest& request) noexcept {
    if(!state_||request.key==0)return;
    state_->newestKey.store(request.key,std::memory_order_release);state_->requests.publish(request);
}
SpaceActivation::Result SpaceActivation::pollReady(const SpaceActivationRequest& request) noexcept {
    if(!state_||request.key==0)return Result::failed;auto& s=*state_;
    if(const auto* latest=s.results.takeView())s.view=latest;
    const auto* result=s.view;
    if(!result||result->key!=request.key||result->identities!=request.identities||result->activeMask!=request.activeMask||result->installMask!=request.installMask)return Result::pending;
    if(!result->success){if(s.failedKey!=request.key){s.failedKey=request.key;++s.failures;}return Result::failed;}
    auto life=s.lifecycle.load(std::memory_order_acquire);
    if(life==Lifecycle::standby){if(!s.lifecycle.compare_exchange_strong(life,Lifecycle::active,std::memory_order_acq_rel))return Result::pending;}
    else if(life!=Lifecycle::active)return Result::pending;
    return Result::ready;
}
bool SpaceActivation::installReady(const SpaceActivationRequest& request) noexcept {
    if(!state_)return false;const auto* result=state_->view;
    return result&&result->success&&result->key==request.key&&result->identities==request.identities&&result->activeMask==request.activeMask&&result->installMask==request.installMask
        &&state_->lifecycle.load(std::memory_order_acquire)==Lifecycle::active&&state_->rendering.installBundle(*result->bundle,request.key,request.installMask);
}
bool SpaceActivation::acknowledgeInactive() noexcept {
    if(!state_)return false;auto& s=*state_;const auto life=s.lifecycle.load(std::memory_order_acquire);
    if(life!=Lifecycle::active)return life==Lifecycle::standby||life==Lifecycle::stopped;
    if(!s.rendering.makeInactive())return false;s.lifecycle.store(Lifecycle::standby,std::memory_order_release);return true;
}
runtime::ModalService& SpaceActivation::service() noexcept {return state_->rendering;}
const runtime::ModalService& SpaceActivation::service() const noexcept {return state_->rendering;}
std::uint64_t SpaceActivation::failureCount() const noexcept {return state_?state_->failures:0;}
}

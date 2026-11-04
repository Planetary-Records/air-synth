#include "CompanionRuntimeServer.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace airack::companion
{
namespace
{
struct Handle
{
    HANDLE value=nullptr;
    ~Handle(){if(value)CloseHandle(value);}
};
Frame unavailableReply(const RuntimeJob& job)
{
    Frame frame;frame.session=job.session;
    if(const auto* request=std::get_if<ai::GenerationRequest>(&job.input)){
        frame.kind=MessageKind::failure;
        encodeControl({request->tag,RuntimeStatus::unavailable},frame.payload);
    }else{
        frame.kind=MessageKind::accountResult;AccountReply reply;reply.status=RuntimeStatus::unavailable;
        encodeAccountReply(reply,frame.payload);
    }
    return frame;
}
struct Completion {RuntimeJob job;Frame reply;};
struct PickupTiming
{
    std::atomic_bool pause{false},pickup{false},quit{false};
};
PickupTiming& pickupTiming(){static PickupTiming value;return value;}
struct ReplyTiming
{
    std::atomic_bool pause{false},held{false};
};
ReplyTiming& replyTiming(){static ReplyTiming value;return value;}
// A test holds the IO here with a reply's write started, so the request answering it arrives before the IO runs on.
void holdAfterReplyStart() noexcept
{
    auto& timing=replyTiming();
    if(!timing.pause.load(std::memory_order_acquire))return;
    timing.held.store(true,std::memory_order_release);
    while(timing.pause.load(std::memory_order_acquire))Sleep(1);
}
struct Worker
{
    RuntimeRunJob run;RuntimeCancelJob cancel;
    Handle wake,notification;std::mutex mutex;std::thread thread;
    std::optional<RuntimeJob> pending;
    std::optional<Completion> completion;
    bool startup=false,ready=false,quit=false,failed=false;
    Worker(RuntimeRunJob r,RuntimeCancelJob c):run(std::move(r)),cancel(std::move(c)){}
    void execute() noexcept
    {
        try{
            const bool priority=SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL)
                && GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_BELOW_NORMAL;
            {std::lock_guard lock(mutex);startup=true;ready=priority;SetEvent(notification.value);}
            if(!priority)return;
            for(;;){
                if(WaitForSingleObject(wake.value,INFINITE)!=WAIT_OBJECT_0)throw std::runtime_error("worker wait");
                auto& timing=pickupTiming();
                if(timing.pause.load(std::memory_order_acquire)){
                    timing.pickup.store(true,std::memory_order_release);
                    while(timing.pause.load(std::memory_order_acquire))Sleep(1);
                }
                std::optional<RuntimeJob> job;
                {std::lock_guard lock(mutex);ResetEvent(wake.value);if(quit)return;job=std::move(pending);pending.reset();}
                if(!job)continue;
                Frame reply;
                if(!job->cancellation->cancelled()){
                    try{reply=run(*job);}catch(...){reply=unavailableReply(*job);}
                }
                {std::lock_guard lock(mutex);completion.emplace(Completion{std::move(*job),std::move(reply)});SetEvent(notification.value);}
            }
        }catch(...){
            std::lock_guard lock(mutex);startup=true;ready=false;failed=true;SetEvent(notification.value);
        }
    }
};
struct Gate
{
    std::mutex mutex;bool reserved=false,poisoned=false,reaping=false;Worker* retained=nullptr;
};
// Non-owning process-lifetime slot: live std::thread storage has no static owner
// destructor. Only a signaled native THREAD handle permits join and deletion.
Gate& gate(){static Gate value;return value;}
RuntimeStatus reserve()
{
    auto& g=gate();std::lock_guard lock(g.mutex);
    if(g.poisoned)return RuntimeStatus::unavailable;
    if(g.reserved)return RuntimeStatus::busy;
    g.reserved=true;return RuntimeStatus::ready;
}
void releaseReservation(){auto& g=gate();std::lock_guard lock(g.mutex);g.reserved=false;}
RuntimeServerResult drain(Worker* worker,RuntimeStatus status,std::uint64_t deadline)
{
    if(!worker){releaseReservation();return {status,false};}
    if(!worker->thread.joinable()){delete worker;releaseReservation();return {status,false};}
    {std::lock_guard lock(worker->mutex);worker->quit=true;SetEvent(worker->wake.value);}
    auto& timing=pickupTiming();
    if(timing.pause.load(std::memory_order_acquire))timing.quit.store(true,std::memory_order_release);
    const auto now=GetTickCount64();const auto remaining=deadline>now?deadline-now:0;
    const auto observed=WaitForSingleObject(worker->thread.native_handle(),static_cast<DWORD>(remaining));
    if(observed==WAIT_OBJECT_0){worker->thread.join();delete worker;releaseReservation();return {status,false};}
    auto& g=gate();std::lock_guard lock(g.mutex);g.poisoned=true;g.retained=worker;
    return {RuntimeStatus::unavailable,true};
}
struct Slot
{
    PipeConnection pipe;RuntimeConnectionId id=0;bool reading=false,writing=false;
};
struct Io
{
    RuntimeState state;PipeListener listener;
    // ponytail: every scan is bounded by sixteen native client slots.
    std::array<std::optional<Slot>,maximumRuntimeConnections> slots;
    std::optional<RuntimeJob> active;bool cancelSent=false;
    bool hadConnection=false;
    std::optional<ai::PreviewEvaluate> previewOffer;
    Worker& worker;
    explicit Io(Worker& w):worker(w){}
    void close(Slot& slot)
    {
        if(slot.writing)state.finishReply(slot.id,false);
        state.disconnect(slot.id);slot.pipe.close();
    }
    bool cancelActive() noexcept
    {
        if(!active || cancelSent || !active->cancellation->cancelled())return true;
        cancelSent=true;
        try{worker.cancel(*active);return true;}catch(...){return false;}
    }
    void shutdown() noexcept
    {
        for(auto& slot:slots)if(slot){close(*slot);slot.reset();}
        listener.close();
    }
    bool progress()
    {
        detail::PipeTestAccess::reapRetired();
        PipeConnection candidate;const auto accepted=listener.pollAccept(candidate);
        if(accepted==PipeStatus::complete){
            hadConnection=true;
            const auto id=state.open(candidate.peer(),std::chrono::steady_clock::now());
            auto available=std::find_if(slots.begin(),slots.end(),[](const auto& slot){return !slot;});
            if(id!=0 && available!=slots.end()){
                available->emplace();(*available)->id=id;(*available)->pipe=std::move(candidate);
            }else{if(id)state.disconnect(id);candidate.close();}
        }else if(accepted==PipeStatus::closed || (accepted==PipeStatus::unavailable && listener.pendingEvent()==nullptr))return false;
        if(active&&active->previewExchange){
            if(active->cancellation->cancelled())previewOffer.reset();
            else {
                if(!previewOffer){ai::PreviewEvaluate value;if(active->previewExchange->takeEvaluation(value))previewOffer.emplace(std::move(value));}
                if(previewOffer&&state.offerPreview(*active,*previewOffer))previewOffer.reset();
            }
        }
        for(auto& slot:slots)if(slot){
            auto& s=*slot;
            if(state.alive(s.id) && s.pipe.connected()){
                // A request can answer the reply before it, so that reply's delivery is counted before the request is read.
                if(s.writing){
                    const auto write=s.pipe.pollWrite();
                    if(write==PipeStatus::complete){s.writing=false;state.finishReply(s.id,true);}
                    else if(write!=PipeStatus::pending){s.writing=false;state.finishReply(s.id,false);}
                }
                if(s.reading){
                    Frame frame;const auto read=s.pipe.pollRead(frame);
                    if(read==PipeStatus::complete){s.reading=false;state.receive(s.id,frame,std::chrono::steady_clock::now());}
                    else if(read!=PipeStatus::pending){s.reading=false;state.disconnect(s.id);}
                }
                if(state.alive(s.id) && s.pipe.connected() && !s.reading){
                    if(s.pipe.beginRead()==PipeStatus::ready)s.reading=true;else state.disconnect(s.id);
                }
                if(state.alive(s.id) && s.pipe.connected() && !s.writing){
                    Frame frame;
                    if(state.takeReply(s.id,frame)){
                        if(s.pipe.beginWrite(frame,runtimeReplyTimeoutMs)==PipeStatus::ready){s.writing=true;holdAfterReplyStart();}
                        else state.finishReply(s.id,false);
                    }
                }
            }
        }
        state.expire(std::chrono::steady_clock::now());
        for(auto& slot:slots)if(slot && (!state.alive(slot->id) || !slot->pipe.connected())){close(*slot);slot.reset();}
        if(!cancelActive())return false;
        std::optional<Completion> completed;bool failed=false;
        {std::lock_guard lock(worker.mutex);completed=std::move(worker.completion);worker.completion.reset();failed=worker.failed;ResetEvent(worker.notification.value);}
        if(completed){
            state.finishJob(completed->job,std::move(completed->reply));
            // Only a matching job completion drains State and the IO token.
            if(active && active->jobId==completed->job.jobId){active.reset();previewOffer.reset();cancelSent=false;}
        }
        if(failed)return false;
        if(!active){
            RuntimeJob job;
            if(state.takeJob(job)){
                active=job;cancelSent=false;
                {std::lock_guard lock(worker.mutex);worker.pending=std::move(job);SetEvent(worker.wake.value);}
            }
        }
        return cancelActive();
    }
    bool wait() const
    {
        std::array<HANDLE,64> handles{};DWORD count=0;
        auto add=[&](void* handle){if(handle && std::find(handles.begin(),handles.begin()+count,handle)==handles.begin()+count)handles[count++]=handle;};
        add(listener.pendingEvent());for(const auto& slot:slots)if(slot)for(auto handle:slot->pipe.pendingEvents())add(handle);add(worker.notification.value);
        // Borrowed owners remain unchanged for the entire native wait.
        if(count==0){Sleep(static_cast<DWORD>(runtimeIoWaitMs));return true;}
        return WaitForMultipleObjects(count,handles.data(),FALSE,static_cast<DWORD>(runtimeIoWaitMs))!=WAIT_FAILED;
    }
};
}
struct RuntimeServer::Impl
{
    RuntimeRunJob run;RuntimeCancelJob cancel;bool used=false;
    Impl(RuntimeRunJob r,RuntimeCancelJob c):run(std::move(r)),cancel(std::move(c)){}
};
RuntimeServer::RuntimeServer(RuntimeRunJob run,RuntimeCancelJob cancel):impl(std::make_unique<Impl>(std::move(run),std::move(cancel))){}
RuntimeServer::~RuntimeServer() = default;
RuntimeServerResult RuntimeServer::run(const std::atomic_bool& stop,const juce::String& suffix)
{
    if(impl->used)return {RuntimeStatus::busy,false};impl->used=true;
    if(!impl->run || !impl->cancel)return {};
    const auto admission=reserve();if(admission!=RuntimeStatus::ready)return {admission,false};
    Worker* worker=nullptr;const auto started=GetTickCount64();bool startupObserved=false;
    try{
        worker=new Worker(std::move(impl->run),std::move(impl->cancel));
        worker->wake.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);worker->notification.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!worker->wake.value || !worker->notification.value)return drain(worker,RuntimeStatus::unavailable,started+runtimeWorkerDrainMs);
        worker->thread=std::thread([worker]{worker->execute();});
        const auto signaled=WaitForSingleObject(worker->notification.value,static_cast<DWORD>(runtimeWorkerStartupMs));
        bool ready=false;
        {std::lock_guard lock(worker->mutex);ready=signaled==WAIT_OBJECT_0 && worker->startup && worker->ready;ResetEvent(worker->notification.value);}
        if(!ready)return drain(worker,RuntimeStatus::unavailable,started+runtimeWorkerDrainMs);
        startupObserved=true;
        Io io(*worker);RuntimeStatus result=RuntimeStatus::ready;
        try{
            if(io.listener.open(suffix)!=PipeStatus::ready)result=RuntimeStatus::unavailable;
            else {
                std::optional<std::uint64_t> idleSince=GetTickCount64();
                while(!stop.load(std::memory_order_acquire)){
                    if(!io.progress()){result=RuntimeStatus::unavailable;break;}
                    const bool connected=std::any_of(io.slots.begin(),io.slots.end(),[](const auto& slot){return slot.has_value();});
                    if(!connected&&!io.active){
                        const auto now=GetTickCount64();if(!idleSince)idleSince=now;
                        if(now-*idleSince>=(io.hadConnection?2000u:30000u))break;
                    }else idleSince.reset();
                    if(!io.wait()){result=RuntimeStatus::unavailable;break;}
                }
            }
        }catch(...){result=RuntimeStatus::unavailable;}
        io.shutdown();if(!io.cancelActive())result=RuntimeStatus::unavailable;
        return drain(worker,result,GetTickCount64()+runtimeWorkerDrainMs);
    }catch(...){return drain(worker,RuntimeStatus::unavailable,(startupObserved?GetTickCount64():started)+runtimeWorkerDrainMs);}
}
std::size_t detail::RuntimeServerTestAccess::retainedWorkers() noexcept
{auto& g=gate();std::lock_guard lock(g.mutex);return g.retained?1:0;}
bool detail::RuntimeServerTestAccess::admissionAvailable() noexcept
{auto& g=gate();std::lock_guard lock(g.mutex);return !g.poisoned && !g.reserved;}
void detail::RuntimeServerTestAccess::reapRetained()
{
    Worker* candidate=nullptr;Worker* completed=nullptr;auto& g=gate();
    {std::lock_guard lock(g.mutex);if(!g.retained || g.reaping)return;g.reaping=true;candidate=g.retained;}
    const auto observed=WaitForSingleObject(candidate->thread.native_handle(),0);
    {std::lock_guard lock(g.mutex);if(observed==WAIT_OBJECT_0){completed=g.retained;g.retained=nullptr;}g.reaping=false;}
    if(completed){completed->thread.join();delete completed;releaseReservation();}
}
void detail::RuntimeServerTestAccess::pauseBeforePickup(bool pause) noexcept
{
    auto& timing=pickupTiming();
    if(pause){timing.pickup.store(false,std::memory_order_release);timing.quit.store(false,std::memory_order_release);}
    timing.pause.store(pause,std::memory_order_release);
}
bool detail::RuntimeServerTestAccess::pickupPaused() noexcept
{return pickupTiming().pickup.load(std::memory_order_acquire);}
bool detail::RuntimeServerTestAccess::quitPublished() noexcept
{return pickupTiming().quit.load(std::memory_order_acquire);}
void detail::RuntimeServerTestAccess::pauseAfterReplyStart(bool pause) noexcept
{
    auto& timing=replyTiming();
    if(pause)timing.held.store(false,std::memory_order_release);
    timing.pause.store(pause,std::memory_order_release);
}
bool detail::RuntimeServerTestAccess::replyStartPaused() noexcept
{return replyTiming().held.load(std::memory_order_acquire);}
}

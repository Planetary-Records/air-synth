#include "PeriodicCaller.h"
#include <chrono>
#include <atomic>
#include <memory>
#include <thread>
#if defined(_WIN32)
#include "ScopedBenchmarkAffinity.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>
#include <emmintrin.h>
#endif
namespace airack::space::probe {
#if defined(_WIN32)
namespace {
using Clock=std::chrono::steady_clock;
// A local priority15 helper yields to the unchanged Pro Audio caller, without charging its spin to MMCSS.
struct NativeTimer {
    HANDLE timer=nullptr;
    HANDLE backup=nullptr;bool resolutionRequested=false;
    HANDLE request=nullptr;
    HANDLE completion=nullptr;
    std::thread thread;
    std::atomic<std::uint64_t> requested{0},completed{0};
    std::atomic<unsigned> startup{0};
    std::atomic<bool> stopping{false};
    std::atomic<std::int64_t> deadline{0};
    std::atomic<std::uint32_t> error{0};
    std::atomic<std::uint32_t> cleanupError{0};
    DWORD taskIndex=0;
    std::atomic<std::uint32_t> callerCpu{0};
    ~NativeTimer(){if(resolutionRequested)timeEndPeriod(1);if(backup)CloseHandle(backup);if(request)CloseHandle(request);if(completion)CloseHandle(completion);}
    void run() noexcept {
        ScopedBenchmarkAffinity placement;
        const auto self=GetCurrentThread();const auto priority=GetThreadPriority(self);
        THREAD_POWER_THROTTLING_STATE before{THREAD_POWER_THROTTLING_CURRENT_VERSION};
        const bool known=GetThreadInformation(self,ThreadPowerThrottling,&before,sizeof(before))!=FALSE;
        auto desired=before;desired.ControlMask|=THREAD_POWER_THROTTLING_EXECUTION_SPEED;desired.StateMask&=~THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        if(!known||!SetThreadInformation(self,ThreadPowerThrottling,&desired,sizeof(desired))||!SetThreadPriority(self,THREAD_PRIORITY_TIME_CRITICAL)){error=GetLastError();if(!error)error=ERROR_GEN_FAILURE;}
        if(!error){THREAD_POWER_THROTTLING_STATE actual{THREAD_POWER_THROTTLING_CURRENT_VERSION};
            if(!GetThreadInformation(self,ThreadPowerThrottling,&actual,sizeof(actual))||actual.ControlMask!=desired.ControlMask||actual.StateMask!=desired.StateMask)error=ERROR_GEN_FAILURE;}
        if(!error&&(GetPriorityClass(GetCurrentProcess())==REALTIME_PRIORITY_CLASS||GetThreadPriority(self)!=THREAD_PRIORITY_TIME_CRITICAL))error=ERROR_NOT_SUPPORTED;
        if(!error&&!placement.openNative(BenchmarkPlacement::movableTimer)){
            const auto failure=placement.setupError()?placement.setupError():placement.topologyError();
            if(!placement.close()||placement.cleanupError())error=placement.cleanupError();
            else error=failure?failure:ERROR_GEN_FAILURE;
        }
        startup.store(error?2u:1u,std::memory_order_release);WakeByAddressSingle(&startup);
        const auto affinityApi=nativeBenchmarkAffinityApi();auto activeAffinity=placement.during();
        std::uint64_t seen=0;
        while(!error){
            const auto waited=WaitForSingleObject(request,INFINITE);
            if(waited!=WAIT_OBJECT_0){error=waited==WAIT_FAILED?GetLastError():ERROR_GEN_FAILURE;break;}
            if(stopping.load(std::memory_order_acquire))break;
            const auto epoch=requested.load(std::memory_order_acquire);if(epoch==seen)continue;
            if(epoch&1u)continue;
            // Snapshot one complete request; missed timing hints may be superseded.
            const auto deadlineTicks=deadline.load(std::memory_order_relaxed);const auto caller=callerCpu.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(requested.load(std::memory_order_acquire)!=epoch)continue;
            seen=epoch;const auto end=Clock::time_point(Clock::duration(deadlineTicks));
            if(placement.opened()){
                const auto desired=timerAffinityOnCaller(placement.before(),std::uint16_t(caller>>16),std::uint8_t(caller));
                if(!desired.mask){error=ERROR_INVALID_DATA;break;}
                if(desired!=activeAffinity){
                    std::uint32_t failure=0;BenchmarkGroupAffinity actual{};
                    if(!affinityApi.set(affinityApi.context,desired,failure)||!affinityApi.get(affinityApi.context,actual,failure)||actual!=desired)
                        error=failure?failure:ERROR_INVALID_DATA;
                    else activeAffinity=desired;
                }
            }
            const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(end-Clock::now()).count();
            if(!error&&remaining>0)if(const auto sleep=periodicTimerSleep100ns(-remaining/100);sleep<0){
                LARGE_INTEGER due;due.QuadPart=sleep;
                if(!SetWaitableTimerEx(timer,&due,0,nullptr,nullptr,nullptr,0))error=GetLastError();
                else {const auto result=WaitForSingleObject(timer,250);
                    if(result!=WAIT_OBJECT_0)error=result==WAIT_FAILED?GetLastError():result==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_GEN_FAILURE;}
            }
            if(!error)while(Clock::now()<end)_mm_pause();
            completed.store(epoch,std::memory_order_release);
            if(!SetEvent(completion))error=GetLastError();
        }
        if(!placement.close())cleanupError=placement.cleanupError();
        SetThreadPriority(self,priority);if(known)SetThreadInformation(self,ThreadPowerThrottling,&before,sizeof(before));
    }
    void stop() noexcept {
        stopping.store(true,std::memory_order_release);requested.fetch_add(1,std::memory_order_release);(void)SetEvent(request);
        if(thread.joinable())thread.join();
    }
};
}
#endif
PeriodicApi nativePeriodicApi() noexcept {
#if defined(_WIN32)
    return {nullptr,
        [](void*,std::uint32_t& error) noexcept->PeriodicToken {
            DWORD index=0;const auto task=AvSetMmThreadCharacteristicsW(L"Pro Audio",&index);
            if(!task)error=GetLastError();return reinterpret_cast<PeriodicToken>(task);
        },
        [](void*,PeriodicToken token,std::uint32_t& error) noexcept {
            if(AvRevertMmThreadCharacteristics(reinterpret_cast<HANDLE>(token)))return true;
            error=GetLastError();return false;
        },
        [](void*,std::uint32_t& error) noexcept->PeriodicToken {
            const auto timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
            if(!timer){error=GetLastError();return 0;}
            try {
                auto state=std::make_unique<NativeTimer>();state->timer=timer;
                // Own this process's resolution request for exactly the timer lifetime.
                if(timeBeginPeriod(1)!=TIMERR_NOERROR){error=ERROR_NOT_SUPPORTED;CloseHandle(timer);return 0;}
                state->resolutionRequested=true;
                state->backup=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
                if(!state->backup){error=GetLastError();CloseHandle(timer);return 0;}
                state->completion=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                if(!state->completion){error=GetLastError();CloseHandle(timer);return 0;}
                state->request=CreateEventW(nullptr,FALSE,FALSE,nullptr);
                if(!state->request){error=GetLastError();CloseHandle(timer);return 0;}
                state->thread=std::thread([pointer=state.get()]{pointer->run();});
                unsigned seen=0;while((seen=state->startup.load(std::memory_order_acquire))==0)WaitOnAddress(&state->startup,&seen,sizeof(seen),INFINITE);
                if(seen!=1){error=state->error;state->stop();CloseHandle(timer);return 0;}
                return reinterpret_cast<PeriodicToken>(state.release());
            }catch(...){CloseHandle(timer);error=ERROR_NOT_ENOUGH_MEMORY;return 0;}
        },
        [](void*,PeriodicToken token,std::uint32_t& error) noexcept {
            auto* state=reinterpret_cast<NativeTimer*>(token);if(!state){error=ERROR_INVALID_HANDLE;return false;}
            state->stop();
            if(state->resolutionRequested){if(timeEndPeriod(1)!=TIMERR_NOERROR){error=ERROR_GEN_FAILURE;return false;}state->resolutionRequested=false;}
            if(state->backup){if(!CloseHandle(state->backup)){error=GetLastError();return false;}state->backup=nullptr;}
            if(const auto failure=state->cleanupError.exchange(0)){error=failure;return false;}
            if(state->request){if(!CloseHandle(state->request)){error=GetLastError();return false;}state->request=nullptr;}
            if(state->completion){if(!CloseHandle(state->completion)){error=GetLastError();return false;}state->completion=nullptr;}
            CancelWaitableTimer(state->timer);
            if(CloseHandle(state->timer)){delete state;return true;}error=GetLastError();return false;
        },
        [](void*,PeriodicToken token,std::int64_t relativeDue,std::int64_t sampledTick,std::uint32_t& error) noexcept {
            if(relativeDue>=0){error=ERROR_INVALID_PARAMETER;return false;}
            if(!token){error=ERROR_INVALID_HANDLE;return false;}
            if(relativeDue<-(std::numeric_limits<std::int64_t>::max()/100)){error=ERROR_ARITHMETIC_OVERFLOW;return false;}
            if(sampledTick<0){error=ERROR_INVALID_PARAMETER;return false;}
            const auto origin=Clock::time_point(Clock::duration(sampledTick));
            if(origin>Clock::now()){error=ERROR_INVALID_PARAMETER;return false;}
            const auto delay=std::chrono::nanoseconds(-relativeDue*100);
            if(delay>Clock::time_point::max()-origin){error=ERROR_ARITHMETIC_OVERFLOW;return false;}
            auto& state=*reinterpret_cast<NativeTimer*>(token);if(state.error){error=state.error;return false;}
            const auto end=origin+delay;const auto wakeDue=periodicCallerTimerDue100ns(relativeDue);
            if(wakeDue<0){const auto deadline=origin+std::chrono::nanoseconds(-wakeDue*100);
                const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-Clock::now()).count();
                if(remaining>0){LARGE_INTEGER due;due.QuadPart=-(remaining/100+(remaining%100!=0));
                    if(!SetWaitableTimerEx(state.backup,&due,0,nullptr,nullptr,nullptr,0)){error=GetLastError();return false;}
                    PROCESSOR_NUMBER caller{};GetCurrentProcessorNumberEx(&caller);
                    state.requested.fetch_add(1,std::memory_order_acq_rel);
                    state.deadline.store(std::chrono::duration_cast<Clock::duration>(deadline.time_since_epoch()).count(),std::memory_order_relaxed);
                    state.callerCpu.store((std::uint32_t(caller.Group)<<16)|caller.Number,std::memory_order_relaxed);
                    const auto epoch=state.requested.fetch_add(1,std::memory_order_release)+1;
                    if(!SetEvent(state.request)){error=GetLastError();return false;}
                    const HANDLE handles[]={state.completion,state.backup};const auto timeout=Clock::now()+std::chrono::milliseconds(250);
                    for(;;){
                        const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(timeout-Clock::now()).count();
                        if(left<=0){error=ERROR_TIMEOUT;return false;}
                        const auto result=WaitForMultipleObjects(2,handles,FALSE,DWORD(left));
                        if(result==WAIT_OBJECT_0+1)break;
                        if(result==WAIT_OBJECT_0){if(state.completed.load(std::memory_order_acquire)==epoch)break;continue;}
                        error=result==WAIT_FAILED?GetLastError():result==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_GEN_FAILURE;return false;
                    }
                }
            }
            if(!state.error)while(Clock::now()<end)_mm_pause();
            error=state.error;return error==0;
        },
        [](void*) noexcept {return std::int64_t(std::chrono::steady_clock::now().time_since_epoch().count());}};
#else
    return {};
#endif
}
NativePeriodicTimerInfo nativePeriodicTimerInfo(PeriodicToken token) noexcept {
#if defined(_WIN32)
    auto* state=reinterpret_cast<NativeTimer*>(token);
    return state&&state->thread.joinable()?NativePeriodicTimerInfo{GetThreadId(state->thread.native_handle()),state->taskIndex}:NativePeriodicTimerInfo{};
#else
    (void)token;
    return {};
#endif
}
std::uint64_t nativePeriodicTicksPerSecond() noexcept {
    using Clock=std::chrono::steady_clock;
    if constexpr(Clock::period::num==1&&Clock::period::den<=1000000000)return std::uint64_t(Clock::period::den);
    return 0;
}
}

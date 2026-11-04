#include "DspPipeline.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <system_error>
#include <algorithm>
#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#include <emmintrin.h>
#endif

namespace airack::detail
{
namespace
{
struct WorkerScheduling
{
    std::uint32_t assignedTask=0;
#if defined(_WIN32)
    THREAD_POWER_THROTTLING_STATE before{THREAD_POWER_THROTTLING_CURRENT_VERSION};
    PROCESSOR_NUMBER previousIdeal{};GROUP_AFFINITY previousAffinity{};bool pinned=false;
    HANDLE task=nullptr;bool touched=false,ready=false,hinted=false;
    void preferCore(unsigned stage,std::uintptr_t poolIdentity) noexcept
    {
        if(GetActiveProcessorGroupCount()!=1)return;
        GROUP_AFFINITY allowed{};if(!GetThreadGroupAffinity(GetCurrentThread(),&allowed))return;
        const auto kernel=GetModuleHandleW(L"kernel32.dll");
        const auto query=kernel?reinterpret_cast<decltype(&GetSystemCpuSetInformation)>(GetProcAddress(kernel,"GetSystemCpuSetInformation")):nullptr;
        if(!query)return;
        std::array<SYSTEM_CPU_SET_INFORMATION,128> information;ULONG bytes=0;
        if(!query(information.data(),DWORD(sizeof(information)),&bytes,nullptr,0)||bytes>sizeof(information))return;
        struct Core {PROCESSOR_NUMBER number;BYTE core;};
        std::array<Core,64> cores{};unsigned count=0;BYTE fastest=0;
        for(ULONG offset=0;offset<bytes;){
            if(bytes-offset<sizeof(SYSTEM_CPU_SET_INFORMATION))return;
            const auto& info=*reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(reinterpret_cast<const char*>(information.data())+offset);
            if(info.Size<sizeof(SYSTEM_CPU_SET_INFORMATION)||info.Size>bytes-offset)return;
            offset+=info.Size;if(info.Type!=CpuSetInformation)continue;
            const auto& cpu=info.CpuSet;
            if(cpu.Group!=allowed.Group||cpu.LogicalProcessorIndex>=64||!(allowed.Mask&(KAFFINITY(1)<<cpu.LogicalProcessorIndex))
                ||(cpu.Allocated&&!cpu.AllocatedToTargetProcess))continue;
            if(cpu.EfficiencyClass>fastest){fastest=cpu.EfficiencyClass;count=0;}
            if(cpu.EfficiencyClass!=fastest)continue;
            unsigned i=0;for(;i<count;++i)if(cores[i].core==cpu.CoreIndex)break;
            const Core candidate{{cpu.Group,cpu.LogicalProcessorIndex,0},cpu.CoreIndex};
            if(i<count){if(candidate.number.Number<cores[i].number.Number)cores[i]=candidate;}
            else if(count<cores.size())cores[count++]=candidate;
        }
        if(!count)return;
        std::sort(cores.begin(),cores.begin()+count,[](const auto& a,const auto& b){return a.number.Number<b.number.Number;});
        // Leave the first two performance cores available for host/driver work.
        const auto skip=count>=5?2u:0u;
        const auto offset=(poolIdentity>>6)%(count-skip);
        auto ideal=cores[skip+(offset+stage)%(count-skip)].number;
        hinted=SetThreadIdealProcessorEx(GetCurrentThread(),&ideal,&previousIdeal)!=FALSE;
        // Disjoint pairs prevent co-location while leaving a way around a busy host core.
        if(count-skip>=4){auto desired=allowed;desired.Mask=0;
            if(count-skip>=6){for(unsigned rank=stage;rank<count-skip;rank+=3)
                desired.Mask|=KAFFINITY(1)<<cores[skip+(offset+rank)%(count-skip)].number.Number;}
            else for(unsigned i=skip;i<count;++i)desired.Mask|=KAFFINITY(1)<<cores[i].number.Number;
            pinned=SetThreadGroupAffinity(GetCurrentThread(),&desired,&previousAffinity)!=FALSE;
            if(pinned&&!AvSetMmThreadPriority(task,AVRT_PRIORITY_HIGH)
                &&SetThreadGroupAffinity(GetCurrentThread(),&previousAffinity,nullptr))pinned=false;
        }
    }
    WorkerScheduling(unsigned stage,std::uintptr_t poolIdentity,std::uint32_t& shared) noexcept
    {
        if(!GetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&before,sizeof(before)))return;
        auto desired=before;desired.ControlMask|=THREAD_POWER_THROTTLING_EXECUTION_SPEED;desired.StateMask&=~THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        touched=true;if(!SetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&desired,sizeof(desired)))return;
        THREAD_POWER_THROTTLING_STATE actual{THREAD_POWER_THROTTLING_CURRENT_VERSION};
        if(!GetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&actual,sizeof(actual))||actual.ControlMask!=desired.ControlMask||actual.StateMask!=desired.StateMask)return;
        DWORD index=stage?shared:0;task=AvSetMmThreadCharacteristicsW(L"Pro Audio",&index);assignedTask=index;
        ready=task!=nullptr&&index!=0&&(stage==0||index==shared);
        if(stage==0&&ready)shared=index;
        if(ready)preferCore(stage,poolIdentity);
    }
    ~WorkerScheduling(){if(task)AvRevertMmThreadCharacteristics(task);if(pinned)SetThreadGroupAffinity(GetCurrentThread(),&previousAffinity,nullptr);if(hinted)SetThreadIdealProcessorEx(GetCurrentThread(),&previousIdeal,nullptr);if(touched)SetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&before,sizeof(before));}
#else
    bool ready=true;
    WorkerScheduling(unsigned,std::uintptr_t,std::uint32_t&) noexcept {assignedTask=1;}
#endif
};
void relax() noexcept
{
#if defined(_WIN32)
    _mm_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}
}
DspPipeline::~DspPipeline() { stop(); }
std::thread DspPipeline::launchThread(Entry function, DspPipeline* pool, unsigned stage)
{ return std::thread(function, pool, stage); }

bool DspPipeline::prepare(void* owner, Work function, Launch launch) noexcept
{
    if (started != 0 || owner == nullptr || function == nullptr || launch == nullptr) return false;
    context = owner; work = function; current = 0; priming = true;sharedTask=0;
    stopping.store(false, std::memory_order_relaxed);
    requested.store(0, std::memory_order_relaxed);
    for (auto& counter : ready) counter.value.store(0, std::memory_order_relaxed);
    for (auto& counter : completed) counter.value.store(0, std::memory_order_relaxed);
    for(auto& status:startup)status.store(0,std::memory_order_relaxed);
    try
    {
#if defined(_WIN32)
        for(auto& signal:wakeSignals){
            signal=reinterpret_cast<std::uintptr_t>(CreateEventW(nullptr,FALSE,FALSE,nullptr));
            if(!signal)throw std::system_error(int(GetLastError()),std::system_category());
        }
#endif
        for (unsigned i = 0; i < threads.size(); ++i)
        {
            threads[i] = launch(entry, this, i);
            if (!threads[i].joinable()) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            ++started;
            // Publish the first registration before other workers join its MMCSS task.
            auto& status=startup[i];while(status.load(std::memory_order_acquire)==0)relax();
            if(status.load(std::memory_order_acquire)<0)throw std::system_error(std::make_error_code(std::errc::operation_not_permitted));
        }
        // Complete a dry epoch before exposing the pool. No DSP or borrowed
        // output is touched, and thread/runtime setup stays outside rendering.
        begin(); finish(); priming = false;
        return true;
    }
    catch (...)
    {
        stop(); return false;
    }
}
void DspPipeline::wake() noexcept
{
#if defined(_WIN32)
    // Independent prepared signals avoid coupling job delivery through one address-wait bucket.
    for(auto signal:wakeSignals)if(signal){
        const auto signalled=SetEvent(reinterpret_cast<HANDLE>(signal));jassert(signalled!=FALSE);(void)signalled;
    }
#else
    requested.notify_all();
#endif
}
void DspPipeline::stop() noexcept
{
    stopping.store(true, std::memory_order_release);
    requested.fetch_add(1, std::memory_order_release); wake();
    for (auto& thread : threads) if (thread.joinable()) thread.join();
#if defined(_WIN32)
    for(auto& signal:wakeSignals)if(signal){CloseHandle(reinterpret_cast<HANDLE>(signal));signal=0;}
#endif
    started = 0;
}
void DspPipeline::begin() noexcept
{
    for (auto& counter : ready) counter.value.store(0, std::memory_order_relaxed);
#if defined(_WIN32)
    floatingPointMode = _mm_getcsr();
#endif
    current = requested.load(std::memory_order_relaxed) + 4;
    for (auto& counter : completed) counter.value.store(current, std::memory_order_relaxed);
    requested.store(current, std::memory_order_release);
    wake();
}
void DspPipeline::finish() const noexcept
{
    for (const auto& counter : completed)
        while (counter.value.load(std::memory_order_acquire) != (current|2u)) relax();
}
unsigned DspPipeline::finishUnstarted() noexcept
{
    unsigned cancelled=0;
    for(unsigned stage=0;stage<completed.size();++stage){
        auto pending=current;
        if(completed[stage].value.compare_exchange_strong(pending,current|2u,std::memory_order_acq_rel,std::memory_order_acquire))
            cancelled|=1u<<stage;
    }
    finish();return cancelled;
}
void DspPipeline::publish(unsigned stage, unsigned chunks) noexcept
{ ready[stage].value.store(chunks, std::memory_order_release); }
void DspPipeline::await(unsigned stage, unsigned chunks) const noexcept
{ while (ready[stage].value.load(std::memory_order_acquire) < chunks) relax(); }
void DspPipeline::entry(DspPipeline* pool, unsigned stage) noexcept
{
    WorkerScheduling scheduling(stage,reinterpret_cast<std::uintptr_t>(pool),pool->sharedTask);pool->startup[stage].store(scheduling.ready?1:-1,std::memory_order_release);
    if(!scheduling.ready)return;
    const juce::ScopedValueSetter taskTag(schedulingTask,scheduling.assignedTask);
    juce::ScopedNoDenormals noDenormals;
    std::uint64_t seen = 0;
    for (;;)
    {
        auto epoch = pool->requested.load(std::memory_order_acquire);
        while (epoch == seen && !pool->stopping.load(std::memory_order_relaxed))
        {
#if defined(_WIN32)
            const auto waited=WaitForSingleObject(reinterpret_cast<HANDLE>(pool->wakeSignals[stage]),INFINITE);
            jassert(waited==WAIT_OBJECT_0);(void)waited;
#else
            pool->requested.wait(seen, std::memory_order_acquire);
#endif
            epoch = pool->requested.load(std::memory_order_acquire);
        }
        if (pool->stopping.load(std::memory_order_acquire)) return;
        seen = epoch;
        // Claim the exact epoch before touching borrowed state; a cancelled late wake may skip generations.
        auto pending=epoch;
        if(!pool->completed[stage].value.compare_exchange_strong(pending,epoch|1u,std::memory_order_acq_rel,std::memory_order_acquire))continue;
        if (!pool->priming)
        {
#if defined(_WIN32)
            // Preserve the callback's rounding/denormal/exception-mask mode.
            // This is job data published by the same release/acquire epoch.
            _mm_setcsr(pool->floatingPointMode);
#endif
            rendering = true;
            pool->work(pool->context, stage);
        }
        // No payload access is permitted after this publication. The owner
        // may now reuse spans, update parameters, reset or retire its state.
        pool->completed[stage].value.store(seen|2u, std::memory_order_release);
        rendering = false;
    }
}
}

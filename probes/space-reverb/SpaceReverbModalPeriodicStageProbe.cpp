#include "ModalServiceProfile.h"
#include "ModalServiceStage.h"
#include "PeriodicCaller.h"
#include "ProbePeerMonitor.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#include <xmmintrin.h>
#endif
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;}
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t alignment){if(tracking)++allocations;void* p=nullptr;const auto a=std::size_t(alignment);
#if defined(_MSC_VER)
    p=_aligned_malloc(n?n:1,a);
#else
    if(n>std::numeric_limits<std::size_t>::max()-(a-1))throw std::bad_alloc();p=std::aligned_alloc(a,((n?n:1)+a-1)/a*a);
#endif
    if(p)return p;throw std::bad_alloc();}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept {if(tracking&&p)++frees;
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete(void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete[](void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
namespace {
using namespace airack::space::probe;using Clock=std::chrono::steady_clock;
constexpr std::array<const char*,10> names{"capture","down","excite","room","room_fade","mix","post","up","final","publish"};
struct Raw {std::uint64_t ordinal=0,release=0,start=0,begin=0,finish=0,cycles=0,maximumRooms=0,roomCopy=0,postAdmission=0,coldInstall=0,valid=0,allocated=0,freed=0;ModalServiceStageSample stages;};
std::uint64_t tick() noexcept {return std::uint64_t(Clock::now().time_since_epoch().count());}
struct Denormals {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned old=_mm_getcsr();Denormals(){_mm_setcsr(old|0x8040);}~Denormals(){_mm_setcsr(old);}
#endif
};
int run(const std::filesystem::path& output,bool paced){
    if(!exclusiveNativeLease()||std::filesystem::exists(output))return 2;
#if !defined(_WIN32)
    return 2;
#else
    constexpr unsigned row=10;constexpr std::uint64_t warm=15000,measured=4096;
    const auto frequency=nativePeriodicTicksPerSecond();std::vector<Raw> records;records.reserve(measured);ServiceCpuWorkload work;ConcurrentPeerGuard guard(true);PeriodicSession caller;Denormals denormals;
    bool valid=true,opened=false,closed=true,interrupted=false;const char* reason="none";const int beforePriority=GetThreadPriority(GetCurrentThread());int duringPriority=beforePriority,afterPriority=beforePriority;
    try{
        guard.force();if(!work.prepare(row,true,128,ModalConverterBackend::mirroredAvx2)||work.converterBackend()!=ModalConverterBackend::mirroredAvx2)throw "preparation";guard.force();
        tracking=true;auto* probe=::operator new(32,std::align_val_t(32));::operator delete(probe,std::align_val_t(32));tracking=false;if(allocations!=1||frees!=1)throw "heap_calibration";allocations=frees=0;
        if(paced){opened=caller.open(nativePeriodicApi(),frequency);if(!opened)throw "caller_open";duringPriority=GetThreadPriority(GetCurrentThread());}
        ReleaseSequence warmSequence;const auto anchor=tick();if(!warmSequence.initialize(std::int64_t(anchor),frequency,96000,64))throw "warm_sequence";
        for(std::uint64_t n=0;n<warm;++n){std::int64_t release=0;if(paced&&(!warmSequence.tickAt(n+1,release)||!caller.waitTo(release)))throw "warm_wait";tracking=true;const bool rendered=work.begin(n)&&work.renderAndEnd();tracking=false;const auto observed=work.observe();if(!rendered||!observed.valid)throw "warm_render";guard();}
        ReleaseSequence sequence;const auto measureAnchor=tick();if(!sequence.initialize(std::int64_t(measureAnchor),frequency,96000,64))throw "measure_sequence";
        for(std::uint64_t n=0;n<measured;++n){Raw raw;raw.ordinal=n;std::int64_t release=0;if(paced){if(!sequence.tickAt(n+1,release)||!caller.waitTo(release))throw "measure_wait";}else release=std::int64_t(tick());raw.release=std::uint64_t(release);
            ULONG64 firstCycles=0,lastCycles=0;if(!QueryThreadCycleTime(GetCurrentThread(),&firstCycles))throw "cycles_begin";const auto allocationBefore=allocations,freeBefore=frees;
            modalServiceStageSample=&raw.stages;tracking=true;raw.start=tick();const bool began=work.begin(warm+n);raw.begin=tick();const bool rendered=began&&work.renderAndEnd();raw.finish=tick();tracking=false;modalServiceStageSample=nullptr;
            if(!QueryThreadCycleTime(GetCurrentThread(),&lastCycles)||lastCycles<firstCycles)throw "cycles_end";raw.cycles=lastCycles-firstCycles;const auto observed=work.observe();raw.maximumRooms=observed.maximumRooms;raw.roomCopy=observed.roomCopy;raw.postAdmission=observed.postAdmission;raw.coldInstall=observed.coldInstall;raw.valid=rendered&&observed.valid;raw.allocated=allocations-allocationBefore;raw.freed=frees-freeBefore;records.push_back(raw);if(!raw.valid||raw.allocated||raw.freed)throw "measure_contract";guard();}
    }catch(const PeerObserved& p){tracking=false;modalServiceStageSample=nullptr;valid=false;interrupted=true;reason=p.reason;}
    catch(const char* failure){tracking=false;modalServiceStageSample=nullptr;valid=false;interrupted=true;reason=failure;}
    catch(...){tracking=false;modalServiceStageSample=nullptr;valid=false;interrupted=true;reason="exception";}
    if(paced){closed=caller.close()&&caller.close();afterPriority=GetThreadPriority(GetCurrentThread());closed=closed&&caller.cleanupError()==0&&afterPriority==beforePriority;}
    guard.stop();try{guard.checkFailure();}catch(const PeerObserved& p){valid=false;interrupted=true;reason=p.reason;}
    std::ofstream csv(output);if(!csv)return 2;csv<<"MODAL_PERIODIC_STAGE_V1,row=10,rate=96000,host=64,factor=2,modes=128,slots=8,warm="<<warm<<",expected="<<measured<<",frequency="<<frequency<<",paced="<<paced<<",opened="<<opened<<",closed="<<closed<<",priority_before="<<beforePriority<<",priority_during="<<duringPriority<<",priority_after="<<afterPriority<<",valid="<<valid<<",interrupted="<<interrupted<<",reason="<<reason<<",capacity_acceptance=0\n";
    csv<<"ordinal,release,start,begin,finish,cycles,maximum_rooms,room_copy,post_admission,cold_install,valid,allocations,frees";for(const auto* name:names)csv<<','<<name<<"_ns";for(const auto* name:names)csv<<','<<name<<"_calls";csv<<'\n';
    for(const auto& r:records){csv<<r.ordinal<<','<<r.release<<','<<r.start<<','<<r.begin<<','<<r.finish<<','<<r.cycles<<','<<r.maximumRooms<<','<<r.roomCopy<<','<<r.postAdmission<<','<<r.coldInstall<<','<<r.valid<<','<<r.allocated<<','<<r.freed;for(auto n:r.stages.ns)csv<<','<<n;for(auto n:r.stages.calls)csv<<','<<n;csv<<'\n';}csv.close();
    const bool complete=valid&&!interrupted&&closed&&records.size()==measured&&bool(csv);std::cout<<"MODAL_PERIODIC_STAGE_COMPLETE row=10 measured="<<records.size()<<" expected="<<measured<<" paced="<<paced<<" valid="<<valid<<" interrupted="<<interrupted<<" reason="<<reason<<" heap_allocations="<<allocations<<" heap_frees="<<frees<<" caller_closed="<<closed<<" capacity_acceptance=0\n";return complete?0:1;
#endif
}
}
int main(int argc,char** argv){if(argc<2||argc>3)return 2;const bool paced=argc==2;if(argc==3&&std::string_view(argv[2])!="--unpaced")return 2;return run(std::filesystem::path(argv[1]),paced);}

#include "ModalServiceProfile.h"
#include "ProbePeerGuard.h"
#include "ProbePeerMonitor.h"
#include "PeriodicCaller.h"
#if defined(SPACE_PROFILE_PINNED_CALLER)
#include "ScopedBenchmarkAffinity.h"
#endif
#if defined(SPACE_PROFILE_HIGHQOS)
#include "ProcessHighQos.h"
#endif
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
#include "PlacementDiagnostic.h"
#endif
#include <bit>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numeric>
#include <string_view>
#include <thread>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#include <xmmintrin.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;}
static_assert(sizeof(double)==8&&std::numeric_limits<double>::is_iec559);
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t alignment)
{
    if(tracking)++allocations;const auto a=std::size_t(alignment);void* p=nullptr;
#if defined(_MSC_VER)
    p=_aligned_malloc(n?n:1,a);
#else
    if(n>std::numeric_limits<std::size_t>::max()-(a-1))throw std::bad_alloc();
    p=std::aligned_alloc(a,((n?n:1)+a-1)/a*a);
#endif
    if(p)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept
{
    if(tracking&&p)++frees;
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
static_assert(std::endian::native==std::endian::little);
#if defined(SPACE_PROFILE_CONVERTER_AVX4)
constexpr auto profileRequestedConverter=ModalConverterBackend::mirroredAvx2;
constexpr auto profileExpectedConverter=ModalConverterBackend::mirroredAvx2;
constexpr const char* profileConverterName="mirrored_avx2";
#if defined(SPACE_PROFILE_PINNED_CALLER)
constexpr const char* profileSelectionLabel="SERVICE_CPU28_SELECTION",*profileTimingLabel="CPU28";
#elif defined(SPACE_PROFILE_OBSERVE_LIMITS)
constexpr const char* profileSelectionLabel="SERVICE_CPU27_SELECTION",*profileTimingLabel="CPU27";
#else
constexpr const char* profileSelectionLabel="SERVICE_CPU24_SELECTION",*profileTimingLabel="CPU24";
#endif
#else
constexpr auto profileRequestedConverter=ModalConverterBackend::automatic;
constexpr auto profileExpectedConverter=ModalConverterBackend::mirroredSse2;
constexpr const char* profileConverterName="mirrored_sse2",*profileSelectionLabel="SERVICE_CPU23_SELECTION",*profileTimingLabel="CPU23";
#endif

std::uint64_t tick() noexcept {return std::uint64_t(Clock::now().time_since_epoch().count());}
constexpr bool stopForPeriodOverrun(std::uint64_t duration,std::uint64_t period) noexcept {
#if defined(SPACE_PROFILE_OBSERVE_LIMITS)
    (void)duration;(void)period;return false;
#else
    return duration>period;
#endif
}
struct Denormals {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned old=_mm_getcsr();Denormals(){_mm_setcsr(old|0x8040);}~Denormals(){_mm_setcsr(old);}
#endif
};
struct CpuObserved {const char* reason;};
bool cycle(std::uint64_t& value) noexcept {
#if defined(_WIN32)
    ULONG64 cycles=0;if(!QueryThreadCycleTime(GetCurrentThread(),&cycles))return false;value=cycles;return true;
#else
    return false;
#endif
}
bool save(const std::filesystem::path& path,const ServiceCpuHeader& h,std::span<const ServiceCpuRecord> records){if(std::filesystem::exists(path))return false;std::ofstream out(path,std::ios::binary|std::ios::out);if(!out)return false;out.write(reinterpret_cast<const char*>(&h),sizeof(h));if(!records.empty())out.write(reinterpret_cast<const char*>(records.data()),std::streamsize(records.size_bytes()));out.close();return bool(out);}
ServiceCpuHeader header(const ServiceCpuCase& c){ServiceCpuHeader h;h.row=c.row;h.rate=c.rate;h.block=c.block;h.factor=c.factor;h.scenario=c.scenario;h.warmExpected=(10ULL*c.rate+c.block-1)/c.block;h.measureExpected=(20ULL*c.rate+c.block-1)/c.block;h.ticksPerSecond=nativePeriodicTicksPerSecond();return h;}
void metrics(std::ostream& out,const ServiceCpuAssessment& a){out<<" schema_valid="<<a.schemaValid<<" complete="<<a.complete<<" known_failure="<<a.knownFailure<<" processing_pass="<<a.processingPass<<" workload_complete="<<a.workloadComplete<<" mean_seconds="<<a.meanSeconds<<" p99_seconds="<<a.p99Seconds<<" max_seconds="<<a.maximumSeconds<<" copy_p99_seconds="<<a.copyP99Seconds<<" cold_p99_seconds="<<a.coldP99Seconds<<" kernel_overruns="<<a.overruns<<" dispatch_misses="<<a.dispatchMisses<<" room_copies="<<a.roomCopies<<" post_admissions="<<a.postAdmissions<<" cold_installs="<<a.coldInstalls<<" nine_room_callbacks="<<a.nineRoomCallbacks<<" start_lateness_p99_seconds="<<a.startLatenessP99Seconds<<" release_finish_p99_seconds="<<a.releaseFinishP99Seconds;}
int profile(unsigned row,const std::filesystem::path& path){const auto c=serviceCpuCase(row);if(!c||std::filesystem::exists(path))return 2;
#if !defined(_WIN32)
    return 2;
#else
#if defined(SPACE_PROFILE_CONVERTER_MIRROR)
    if(!exclusiveNativeLease()){std::cerr<<profileTimingLabel<<"_PROFILE_REQUIRES_EXCLUSIVE_COORDINATOR\n";return 2;}
#endif
    auto h=header(*c);std::vector<ServiceCpuRecord> records(std::size_t(h.measureExpected));records.clear();ServiceCpuWorkload work;ConcurrentPeerGuard guard(true);PeriodicSession caller;Denormals denormals;
#if defined(SPACE_PROFILE_HIGHQOS)
    ProcessHighQos power;
#endif
#if defined(SPACE_PROFILE_PINNED_CALLER)
    ScopedBenchmarkAffinity affinity;BenchmarkGroupAffinity inheritedAffinity{};bool inheritedCaptured=false;std::uint32_t inheritedError=0;
#endif
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
    const auto placementPath=std::filesystem::path(path.string()+".placement.json");if(std::filesystem::exists(placementPath))return 2;
    PlacementDiagnostic placement(h.warmExpected,h.measureExpected);
#endif
    bool interrupted=false,dataFailure=false,warmStarted=false,opened=false,closed=true,hooks=false;const char* reason="none";PeerDetails peer;int priorityBefore=GetThreadPriority(GetCurrentThread()),priorityDuring=priorityBefore,priorityAfter=priorityBefore;allocations=frees=0;
    try {
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
        if(!placement.captureTopology())throw CpuObserved{"placement_topology"};
#endif
#if defined(SPACE_PROFILE_PINNED_CALLER)
        inheritedCaptured=nativeBenchmarkAffinityApi().get(nullptr,inheritedAffinity,inheritedError);
        if(!inheritedCaptured)throw CpuObserved{"affinity_inherited_query"};
#endif
        guard.force();
#if defined(SPACE_PROFILE_HIGHQOS)
        if(!power.open(nativeProcessPowerApi()))throw CpuObserved{"power_setup"};
#endif
#if defined(SPACE_PROFILE_SMALL)
#if defined(SPACE_PROFILE_SPARSE_MODES)
        if(!work.prepare(row,true,SPACE_PROFILE_SPARSE_MODES,profileRequestedConverter))throw CpuObserved{"preparation_failed"};
#elif defined(SPACE_PROFILE_TINY)
        if(!work.prepare(row,true,256))throw CpuObserved{"preparation_failed"};
#else
        if(!work.prepare(row,true,512))throw CpuObserved{"preparation_failed"};
#endif
#else
        if(!work.prepare(row,true))throw CpuObserved{"preparation_failed"};
#endif
#if defined(SPACE_PROFILE_CONVERTER_MIRROR)
        if(work.converterBackend()!=profileExpectedConverter){dataFailure=true;throw CpuObserved{"converter_selection"};}
#endif
        h.productBytes=work.storageBytes();guard.force();tracking=true;auto* allocationProbe=::operator new(32,std::align_val_t(32));::operator delete(allocationProbe,std::align_val_t(32));tracking=false;hooks=allocations==1&&frees==1;allocations=frees=0;if(!hooks){dataFailure=true;throw CpuObserved{"allocation_hook_calibration"};}
        opened=caller.open(nativePeriodicApi(),h.ticksPerSecond);if(!opened)throw CpuObserved{"periodic_api_setup"};priorityDuring=GetThreadPriority(GetCurrentThread());
#if defined(SPACE_PROFILE_PINNED_CALLER)
        BenchmarkGroupAffinity afterPeriodic{};
        if(!nativeBenchmarkAffinityApi().get(nullptr,afterPeriodic,inheritedError)||afterPeriodic!=inheritedAffinity)throw CpuObserved{"affinity_inherited_changed"};
        if(!affinity.openNative(BenchmarkPlacement::stableCaller)||affinity.before()!=inheritedAffinity)throw CpuObserved{"affinity_setup_after_periodic"};
        if(!affinity.verifyCurrent())throw CpuObserved{"affinity_after_periodic_setup"};
#endif
        std::cout<<std::setprecision(17)<<"SERVICE_CPU10_BEGIN row="<<row<<" rate="<<c->rate<<" block="<<c->block<<" factor="<<c->factor<<" scenario="<<c->scenario<<" warm_callbacks="<<h.warmExpected<<" measured_callbacks="<<h.measureExpected<<" product_bytes="<<h.productBytes<<'\n'<<std::flush;
        ReleaseSequence warm;h.warmStart=tick();warmStarted=true;if(!warm.initialize(std::int64_t(h.warmStart),h.ticksPerSecond,h.rate,h.block))throw CpuObserved{"warm_sequence"};
        for(std::uint64_t n=0;n<h.warmExpected;++n){std::int64_t release=0;if(!warm.tickAt(n+1,release)||!caller.waitTo(release))throw CpuObserved{"warm_wait"};
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
            placement.begin(0,n);
#endif
            tracking=true;const bool valid=work.begin(n)&&work.renderAndEnd();tracking=false;
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
            placement.finish();
#endif
            const auto observed=work.observe();if(!valid||!observed.valid
#if defined(SPACE_PROFILE_OBSERVE_LIMITS)
                ||allocations||frees
#endif
            ){dataFailure=true;throw CpuObserved{"warm_callback_contract"};}++h.warmCompleted;guard();}
        std::int64_t warmFinish=0;if(!warm.tickAt(h.warmExpected+1,warmFinish)||!caller.waitTo(warmFinish))throw CpuObserved{"warm_finish"};h.warmEnd=tick();guard.force();
        ReleaseSequence sequence;h.measureAnchor=tick();if(!sequence.initialize(std::int64_t(h.measureAnchor),h.ticksPerSecond,h.rate,h.block))throw CpuObserved{"measure_sequence"};
        for(std::uint64_t n=0;n<h.measureExpected;++n){std::int64_t release=0;if(!sequence.tickAt(n+1,release)||!caller.waitTo(release))throw CpuObserved{"measure_wait"};
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
            placement.begin(1,n);
#endif
            std::uint64_t before=0,after=0;if(!cycle(before))throw CpuObserved{"cycle_before"};const auto allocationBefore=allocations,freeBefore=frees;
            tracking=true;const auto start=tick();const bool began=work.begin(h.warmExpected+n);const auto beginFinish=tick();const bool rendered=began&&work.renderAndEnd();const auto finish=tick();tracking=false;
            const bool validCycles=cycle(after)&&after>=before;
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
            placement.finish();
#endif
            const auto observed=work.observe();records.push_back({std::uint64_t(release),start,beginFinish,finish,validCycles?after-before:0,observed.roomCopy,observed.postAdmission,observed.coldInstall,observed.maximumRooms,allocations-allocationBefore,frees-freeBefore,std::bit_cast<std::uint64_t>(observed.peak),std::bit_cast<std::uint64_t>(observed.checksum),std::uint64_t(rendered&&observed.valid&&validCycles)});
            if(!records.back().valid
#if defined(SPACE_PROFILE_OBSERVE_LIMITS)
                ||allocations!=allocationBefore||frees!=freeBefore
#endif
            ){dataFailure=true;throw CpuObserved{"measured_callback_contract"};}if(stopForPeriodOverrun(finish-start,h.ticksPerSecond*h.block/h.rate))throw CpuObserved{"processing_period_overrun"};guard();}
        std::int64_t finish=0;if(!sequence.tickAt(h.measureExpected+1,finish)||!caller.waitTo(finish))throw CpuObserved{"measure_finish"};h.measureEnd=tick();guard.force();
    }catch(const PeerObserved& p){tracking=false;interrupted=true;reason=p.reason;peer=p.details;}
    catch(const CpuObserved& error){tracking=false;interrupted=true;reason=error.reason;}
    tracking=false;if(warmStarted&&!h.warmEnd)h.warmEnd=tick();if(h.measureAnchor&&!h.measureEnd)h.measureEnd=tick();h.recordCount=records.size();h.callbackAllocations=allocations;h.callbackFrees=frees;
#if defined(SPACE_PROFILE_PINNED_CALLER)
    if(affinity.opened()&&!affinity.verifyCurrent()){interrupted=true;if(std::string_view(reason)=="none")reason="affinity_before_periodic_cleanup";}
#endif
    const bool firstClose=caller.close(),secondClose=caller.close();priorityAfter=GetThreadPriority(GetCurrentThread());closed=firstClose&&secondClose&&caller.cleanupError()==0&&priorityBefore!=THREAD_PRIORITY_ERROR_RETURN&&priorityDuring!=THREAD_PRIORITY_ERROR_RETURN&&priorityAfter==priorityBefore;if(!closed){interrupted=true;if(std::string_view(reason)=="none")reason="periodic_cleanup";}
#if defined(SPACE_PROFILE_PINNED_CALLER)
    const bool affinityFirstClose=affinity.close(),affinitySecondClose=affinity.close();
    bool inheritedRestored=!inheritedCaptured,inheritedCleanupValid=true;
    if(inheritedCaptured){
        const auto api=nativeBenchmarkAffinityApi();BenchmarkGroupAffinity current{};std::uint32_t error=0;
        inheritedCleanupValid=api.get(nullptr,current,error)&&current==inheritedAffinity;
        if(!inheritedCleanupValid){error=0;(void)api.set(nullptr,inheritedAffinity,error);}
        error=0;inheritedRestored=api.get(nullptr,current,error)&&current==inheritedAffinity;
    }
    const bool affinityOk=affinityFirstClose&&affinitySecondClose&&affinity.qualified()&&inheritedRestored&&inheritedCleanupValid;
    if(affinity.attempted()&&!affinityOk){interrupted=true;if(std::string_view(reason)=="none")reason="affinity_cleanup";}
#endif
#if defined(SPACE_PROFILE_HIGHQOS)
    const bool powerFirstClose=power.close(),powerSecondClose=power.close();
    const bool powerOk=powerFirstClose&&powerSecondClose&&power.qualified();
    if(power.attempted()&&!powerOk){interrupted=true;if(std::string_view(reason)=="none")reason="power_cleanup";}
#endif
    guard.stop();if(!interrupted)try{guard.checkFailure();}catch(const PeerObserved& p){interrupted=true;reason=p.reason;peer=p.details;}
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
    if(!placement.save(placementPath,h.warmCompleted,records.size())){std::cerr<<"PLACEMENT_WRITE_FAILED\n";return 2;}
    std::cout<<"PLACEMENT_DIAGNOSTIC_ONLY capacity_acceptance=0\n";
#endif
    ServiceCpuAssessment a;if(warmStarted){if(!save(path,h,records)){std::cerr<<"SERVICE_CPU10_WRITE_FAILED\n";return 2;}a=assessServiceCpu(h,records);}if(dataFailure)a.knownFailure=true;
    ReleaseSequence sequence;if(h.measureAnchor)sequence.initialize(std::int64_t(h.measureAnchor),h.ticksPerSecond,h.rate,h.block);const auto period=h.ticksPerSecond*h.block/h.rate;
    for(std::size_t n=0;n<records.size();++n){const auto& r=records[n];if(r.finish-r.start>period)std::cout<<"SERVICE_CPU10_KERNEL_OVERRUN row="<<row<<" ordinal="<<n<<" ticks="<<r.finish-r.start<<" cycles="<<r.cycles<<" room_copy="<<r.roomCopy<<" cold="<<r.coldInstall<<'\n';if(classifySchedule(sequence,n,{std::int64_t(r.release),std::int64_t(r.start),std::int64_t(r.finish)})==ScheduleDisposition::late)std::cout<<"SERVICE_CPU10_DISPATCH_MISS row="<<row<<" ordinal="<<n<<" start_lateness_ticks="<<r.start-r.release<<" release_finish_ticks="<<r.finish-r.release<<" kernel_ticks="<<r.finish-r.start<<'\n';}
    const auto peerName=peerImageAscii(peer);const auto scans=guard.statistics();const bool pass=a.processingPass&&opened&&closed&&!interrupted&&!dataFailure;
#if defined(SPACE_PROFILE_PINNED_CALLER)
    const auto before=affinity.before(),during=affinity.during(),after=affinity.after();const auto selected=affinity.selected();
    for(const auto& cpu:affinity.topology())std::cout<<"SERVICE_CPU28_CPU id="<<cpu.id<<" group="<<cpu.group<<" index="<<unsigned(cpu.index)<<" core="<<unsigned(cpu.core)<<" efficiency="<<unsigned(cpu.efficiency)<<" flags="<<unsigned(cpu.flags)<<'\n';
    std::cout<<"SERVICE_CPU28_AFFINITY attempted="<<affinity.attempted()<<" opened="<<affinity.opened()<<" restored="<<affinity.restored()<<" close1="<<affinityFirstClose<<" close2="<<affinitySecondClose
      <<" setup_error="<<affinity.setupError()<<" cleanup_error="<<affinity.cleanupError()<<" topology_error="<<affinity.topologyError()<<" group_count="<<affinity.groupCount()
      <<" before_ok="<<affinity.beforeOk()<<" during_ok="<<affinity.duringOk()<<" after_ok="<<affinity.afterOk()
      <<" before_group="<<before.group<<" before_mask="<<before.mask<<" during_group="<<during.group<<" during_mask="<<during.mask<<" after_group="<<after.group<<" after_mask="<<after.mask
      <<" selected_id="<<selected.id<<" selected_group="<<selected.group<<" selected_index="<<unsigned(selected.index)<<" selected_core="<<unsigned(selected.core)<<" selected_efficiency="<<unsigned(selected.efficiency)<<" selected_flags="<<unsigned(selected.flags)
      <<" eligible_mask="<<affinity.eligibleMask()<<" performance_mask="<<affinity.performanceMask()<<'\n';
#endif
#if defined(SPACE_PROFILE_HIGHQOS)
    const auto pb=power.before(),pd=power.during(),pa=power.after();
    std::cout<<"SERVICE_CPU16_POWER attempted="<<power.attempted()<<" opened="<<power.opened()<<" restored="<<power.restored()
      <<" close1="<<powerFirstClose<<" close2="<<powerSecondClose<<" setup_error="<<power.setupError()<<" cleanup_error="<<power.cleanupError()
      <<" before_version="<<pb.version<<" before_control="<<pb.control<<" before_state="<<pb.state
      <<" during_version="<<pd.version<<" during_control="<<pd.control<<" during_state="<<pd.state
      <<" after_version="<<pa.version<<" after_control="<<pa.control<<" after_state="<<pa.state<<'\n';
#endif
    std::cout<<std::setprecision(17)<<"SERVICE_CPU10_CALLER allocation_hook_calibrated="<<hooks<<" opened="<<opened<<" closed="<<closed<<" task=Pro_Audio timer=high_resolution cadence=rational_no_skips helper_minimum_sleep_100ns="<<periodicHelperMinimumSleep100ns<<" helper_policy=local_preemptible_non_mmcss15 caller_precise_wait_ns=150000 priority_before="<<priorityBefore<<" priority_during="<<priorityDuring<<" priority_after="<<priorityAfter<<" failure="<<int(caller.failure())<<" platform_error="<<caller.platformError()<<" cleanup_error="<<caller.cleanupError()<<'\n';
    std::cout<<"SERVICE_CPU10_PEER pid="<<peer.pid<<" image="<<peerName.data()<<" site="<<inventorySiteName(peer.inventorySite)<<" win32_error="<<peer.win32Error<<" exit_known="<<peer.exitCodeKnown<<" exit_code="<<peer.exitCode<<" exit_query_error="<<peer.exitQueryError<<" scans="<<scans.scans<<" maximum_scan_seconds="<<scans.maximumScanSeconds<<'\n';
    if(peer.pid)std::wcerr<<L"SERVICE_CPU_PEER_SOURCE parent="<<peer.parentPid<<L" path="<<peer.imagePath.data()<<L" created_filetime="<<peer.creationFileTime<<L" observed_filetime="<<peer.observedFileTime<<L'\n';
    std::cout<<"SERVICE_CPU10_RESULT row="<<row;
#if defined(SPACE_PROFILE_OBSERVE_LIMITS)
    std::cout<<" limits_mode=observe_all_callbacks";
#endif
#if defined(SPACE_PROFILE_HIGHQOS)
    std::cout<<" power_ok="<<powerOk;
#endif
#if defined(SPACE_PROFILE_PINNED_CALLER)
    std::cout<<" affinity_ok="<<affinityOk<<" affinity_mode=single_fast_stable_reserved01_current_thread";
#endif
#if defined(SPACE_PROFILE_SMALL)
    std::cout<<" modes="<<(work.prepared(0)?work.prepared(0)->modes():0);
#endif
#if defined(SPACE_PROFILE_PREPARED_COPY)
    std::cout<<" copy_mode="<<(work.prepared(0)?(work.usesPreparedCopies()?"prepared":"validated"):"none");
#endif
#if defined(SPACE_PROFILE_BLOCKED_SCATTER)
    std::cout<<" scatter_mode="<<(work.prepared(0)?(work.usesBlockedScatter()?"blocked":"scalar_reference"):"none");
#endif
#if defined(SPACE_PROFILE_PAIRED_REDUCTION)
    std::cout<<" reduction_mode="<<(work.prepared(0)?(work.usesPairedReduction()?"paired_stereo":"reference"):"none");
#endif
#if defined(SPACE_PROFILE_CONVERTER_MIRROR)
    const auto converter=work.converterBackend();
    std::cout<<" converter_mode="<<(!converter?"none":*converter==ModalConverterBackend::mirroredAvx2?"mirrored_avx2":*converter==ModalConverterBackend::mirroredSse2?"mirrored_sse2":*converter==ModalConverterBackend::mirroredScalar?"mirrored_scalar":"reference")
        <<" converter_slots="<<(converter?8:0)<<" converter_factor="<<(converter?c->factor:0);
#endif
    std::cout<<" phase="<<(warmStarted?"workload":"admission")<<" rate="<<h.rate<<" block="<<h.block<<" factor="<<h.factor<<" scenario="<<h.scenario<<" samples="<<records.size()<<" warm_completed="<<h.warmCompleted<<" interrupted="<<interrupted<<" interrupt_reason="<<reason<<" allocations="<<allocations<<" frees="<<frees;metrics(std::cout,a);std::cout<<" pass="<<pass<<'\n';
    std::cout<<"SERVICE_CPU10_FOOTER row="<<row<<" complete="<<(a.complete&&!interrupted&&!dataFailure&&closed)<<" acceptance=0\n";
    return a.knownFailure?1:interrupted?3:pass?0:1;
#endif
}
#if defined(SPACE_PROFILE_PAIRED_REDUCTION)
int selectionSelfTest(){
#if defined(SPACE_PROFILE_CONVERTER_MIRROR)
    bool all=true;
    for(unsigned row:{4U,10U}){
        ServiceCpuWorkload work;const bool initiallyUnprepared=!work.converterBackend();
        if(!work.prepare(row,false,SPACE_PROFILE_SPARSE_MODES,profileRequestedConverter))return 1;
        const bool selected=work.usesPairedReduction()&&work.usesPreparedCopies()&&work.usesBlockedScatter()
            &&work.converterBackend()==profileExpectedConverter;
        allocations=frees=0;tracking=true;const bool rendered=work.begin(0)&&work.renderAndEnd();tracking=false;
        const bool valid=rendered&&work.observe().valid&&work.converterBackend()==profileExpectedConverter
            &&allocations==0&&frees==0&&initiallyUnprepared&&selected;all=all&&valid;
        std::cout<<profileSelectionLabel<<" row="<<row<<" modes="<<work.prepared(0)->modes()
            <<" copy_mode="<<(work.usesPreparedCopies()?"prepared":"validated")<<" scatter_mode="<<(work.usesBlockedScatter()?"blocked":"scalar_reference")
            <<" reduction_mode="<<(work.usesPairedReduction()?"paired_stereo":"reference")
            <<" converter_mode="<<(work.converterBackend()==profileExpectedConverter?profileConverterName:"other")
            <<" converter_slots=8 converter_factor="<<serviceCpuCase(row)->factor
#if defined(SPACE_PROFILE_OBSERVE_LIMITS)
            <<" limits_mode=observe_all_callbacks"
#endif
#if defined(SPACE_PROFILE_PINNED_CALLER)
            <<" affinity_mode=single_fast_stable_reserved01_current_thread"
#endif
            <<" rendered="<<rendered<<" allocations="<<allocations<<" frees="<<frees<<" valid="<<valid<<" actual_timing=0\n";
    }
    return all?0:1;
#else
    ServiceCpuWorkload work;const bool initiallyUnprepared=!work.usesPairedReduction();
#if defined(SPACE_PROFILE_SPARSE_MODES)
    const bool prepared=work.prepare(4,false,SPACE_PROFILE_SPARSE_MODES);if(!prepared)return 1;
#else
    const bool prepared=work.prepare(4,false,256);if(!prepared)return 1;
#endif
    const bool selected=work.usesPairedReduction()&&work.usesPreparedCopies()&&work.usesBlockedScatter();
    allocations=frees=0;tracking=true;const bool rendered=work.begin(0)&&work.renderAndEnd();tracking=false;
    const bool valid=rendered&&work.observe().valid&&work.usesPairedReduction()&&allocations==0&&frees==0&&initiallyUnprepared&&selected;
    std::cout<<"SERVICE_CPU20_SELECTION modes="<<work.prepared(0)->modes()<<" copy_mode="<<(work.usesPreparedCopies()?"prepared":"validated")<<" scatter_mode="<<(work.usesBlockedScatter()?"blocked":"scalar_reference")<<" reduction_mode="<<(work.usesPairedReduction()?"paired_stereo":"reference")<<" rendered="<<rendered<<" allocations="<<allocations<<" frees="<<frees<<" valid="<<valid<<" actual_timing=0\n";
    return valid?0:1;
#endif
}
#endif
int fixtures(const std::filesystem::path& directory){if(std::filesystem::exists(directory))return 2;std::filesystem::create_directories(directory);
    for(unsigned variant=0;variant<6;++variant){const unsigned row=variant==1?6:variant==2?12:0;auto h=header(*serviceCpuCase(row));h.ticksPerSecond=1000000000;h.warmStart=1000000000;h.warmEnd=h.warmStart+(h.warmExpected+1)*h.ticksPerSecond*h.block/h.rate;h.warmCompleted=h.warmExpected;h.measureAnchor=h.warmEnd+1000000;h.productBytes=32*1024*1024;std::vector<ServiceCpuRecord> records(std::size_t(h.measureExpected));const auto period=h.ticksPerSecond*h.block/h.rate;std::uint64_t previous=0;
        for(std::size_t n=0;n<records.size();++n){auto& r=records[n];r.release=h.measureAnchor+(n+1)*h.ticksPerSecond*h.block/h.rate;const bool late=(variant==3&&n==100)||(variant==5&&n==10);r.start=std::max(r.release+period/100+(variant==3&&late?2*period:0),previous);r.beginFinish=r.start+period/20;r.finish=r.start+(variant==5&&late?2*period:period/4);previous=r.finish;r.cycles=5678;r.valid=1;r.rawPeakBits=std::bit_cast<std::uint64_t>(.1);r.checksumBits=std::bit_cast<std::uint64_t>(double(n)/1024.);
            if(h.scenario){const auto every=(h.rate+4*h.block-1)/(4*h.block);r.coldInstall=(h.warmExpected+n)%every==0;r.maximumRooms=8;}else {r.maximumRooms=(n/8)%2?8:9;r.roomCopy=n%32==0;r.postAdmission=n%32==8;}}
        if(variant>=4)records.resize(20);h.recordCount=records.size();h.measureEnd=variant>=4?records.back().finish:h.measureAnchor+(h.measureExpected+1)*h.ticksPerSecond*h.block/h.rate;
        const auto a=assessServiceCpu(h,records);const bool expected=variant<4;if(a.processingPass!=expected||a.knownFailure!=(variant==5)||a.complete!=(variant<4)||(variant==3&&a.dispatchMisses==0))return 2;
        const auto stem="fixture-"+std::to_string(variant);if(!save(directory/(stem+".timings.bin"),h,records))return 2;std::ofstream out(directory/(stem+".expected.txt"));out<<std::setprecision(17)<<"SERVICE_CPU10_FIXTURE row="<<row<<" variant="<<variant;metrics(out,a);out<<'\n';out.close();if(!out)return 2;std::cout<<"SERVICE_CPU10_FIXTURE_CREATED variant="<<variant<<" row="<<row<<" records="<<records.size()<<" expected_processing_pass="<<expected<<'\n';}
    std::cout<<"SERVICE_CPU10_FIXTURES_ONLY no_actual_timing=1\n";return 0;
}
}
#if defined(SPACE_PROFILE_OBSERVE_LIMITS)
int observationSelfTest(const std::filesystem::path& output={}){
    auto h=header(*serviceCpuCase(4));h.ticksPerSecond=1000000000;h.warmStart=h.ticksPerSecond;
    h.warmEnd=h.warmStart+(h.warmExpected+1)*h.ticksPerSecond*h.block/h.rate;h.warmCompleted=h.warmExpected;
    h.measureAnchor=h.warmEnd+h.ticksPerSecond;h.productBytes=32*1024*1024;
    std::vector<ServiceCpuRecord> records(std::size_t(h.measureExpected));std::uint64_t previous=0;
    const auto period=h.ticksPerSecond*h.block/h.rate;
    for(std::size_t n=0;n<records.size();++n){auto& r=records[n];r.release=h.measureAnchor+(n+1)*h.ticksPerSecond*h.block/h.rate;
        r.start=std::max(r.release+period/100,previous);r.beginFinish=r.start+period/20;r.finish=r.start+(n==10?2*period:period/4);previous=r.finish;
        r.cycles=5678;r.valid=1;r.rawPeakBits=std::bit_cast<std::uint64_t>(.1);r.checksumBits=std::bit_cast<std::uint64_t>(double(n));
        r.maximumRooms=(n/8)%2?8:9;r.roomCopy=n%32==0;r.postAdmission=n%32==8;
    }
    h.recordCount=records.size();h.measureEnd=h.measureAnchor+(h.measureExpected+1)*h.ticksPerSecond*h.block/h.rate;
    const auto a=assessServiceCpu(h,records);std::size_t observed=0;for(const auto& r:records){++observed;if(stopForPeriodOverrun(r.finish-r.start,period))break;}
    const std::array<bool,4> checks{!stopForPeriodOverrun(period,period),!stopForPeriodOverrun(2*period,period),observed==records.size(),a.schemaValid&&a.complete&&a.workloadComplete&&a.knownFailure&&!a.processingPass&&a.overruns==1};
    const auto failures=std::count(checks.begin(),checks.end(),false);
    if(!output.empty()&&(std::filesystem::exists(output)||!save(output,h,records)))return 2;
    std::cout<<"CPU27_OBSERVATION_POLICY checks="<<checks.size()<<" failures="<<failures<<" observed="<<observed<<" expected="<<records.size()<<" original_processing_pass="<<a.processingPass<<" original_overruns="<<a.overruns<<" actual_timing=0\n";
    return failures?1:0;
}
#endif
int main(int argc,char** argv){try {if(argc==3&&std::string_view(argv[1])=="--write-fixtures")return fixtures(argv[2]);
#if defined(SPACE_PROFILE_OBSERVE_LIMITS)
    if((argc==2||argc==3)&&std::string_view(argv[1])=="--observation-self-test")return observationSelfTest(argc==3?std::filesystem::path(argv[2]):std::filesystem::path{});
#endif
#if defined(SPACE_PROFILE_PAIRED_REDUCTION)
    if(argc==2&&std::string_view(argv[1])=="--selection-self-test")return selectionSelfTest();
#endif
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
    if(argc!=4||std::string_view(argv[1])!="--diagnose-placement")return 2;
#else
    if(argc!=4||std::string_view(argv[1])!="--profile")return 2;
#endif
    unsigned row=24;const std::string_view text=argv[2];const auto parsed=std::from_chars(text.data(),text.data()+text.size(),row);if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||row>=24)return 2;
#if defined(SPACE_PLACEMENT_DIAGNOSTIC)
    if(row!=0)return 2;
#endif
    return profile(row,argv[3]);}catch(const std::exception& e){std::cerr<<"SERVICE_CPU10_EXCEPTION "<<e.what()<<'\n';return 2;}}

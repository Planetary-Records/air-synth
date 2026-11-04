#include "ModalTank.h"
#include "ProbeCases.h"
#include "ProbePeerGuard.h"
#include "ProbePeerMonitor.h"
#include "ModalHandoffRender.h"
#include "PeriodicCaller.h"
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
using namespace airack::space::probe;
using Clock=std::chrono::steady_clock;
struct Denormals {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned old=_mm_getcsr();Denormals(){_mm_setcsr(old|0x8040);}~Denormals(){_mm_setcsr(old);}
#endif
};
struct Stats {double mean=0,p99=0,maximum=0,minimum=0;};
Stats summarize(std::vector<double> values)
{
    if(values.empty())return {};
    Stats result;result.mean=std::accumulate(values.begin(),values.end(),0.)/values.size();std::sort(values.begin(),values.end());
    result.minimum=values.front();result.maximum=values.back();result.p99=values[std::min(values.size()-1,std::size_t(std::ceil(values.size()*.99))-1)];return result;
}

namespace owned=airack::space::probe;
using Record=ModalHandoffRecord;
static_assert(sizeof(Record)==24);
static_assert(std::endian::native==std::endian::little);
struct PeriodicObserved {const char* reason;};
template<class Guard=PeerGuard,bool periodic=false> int profile(int index,const std::filesystem::path& timingPath,bool diagnoseCycles,bool blockOrder) {
    const auto cyclePath=std::filesystem::path(timingPath.string()+".cycles.bin");
    const auto schedulePath=std::filesystem::path(timingPath.string()+".schedule.bin");
    if(index<0||index>=12||std::filesystem::exists(timingPath)||(diagnoseCycles&&std::filesystem::exists(cyclePath))||(periodic&&std::filesystem::exists(schedulePath))){std::cerr<<"MODAL_HANDOFF_CPU_INVALID_INPUT\n";return 2;}
#if !defined(_WIN32)
    if(diagnoseCycles)return 2;
#endif
    Guard guard(true);guard.force();Denormals denormals;
    constexpr std::array<double,3> rates{44100,48000,96000};constexpr std::array<int,2> blocks{64,512};
    const double rate=rates[std::size_t(index/4)];const int block=blocks[std::size_t(index/2%2)];const bool sameSize=index%2!=0;
    const double period=block/rate,limit=.5*period,copyLimit=.1*period;
    const auto cases=corpus();std::array<owned::PreparedModal,2> prepared;
    if(!owned::prepareModal(cases[35].curve,sameSize?1:0,rate,1024,prepared[0])||!owned::prepareModal(sameSize?airack::space::DecayCurve{}:cases[35].curve,1,rate,1024,prepared[1])){std::cerr<<"MODAL_HANDOFF_CPU_PREPARATION_FAILED\n";return 2;}
    guard.force();
    std::array<std::array<double,2>,4096> input;unsigned random=0x5eeda123;
    for(auto& sample:input)for(auto& x:sample){random^=random<<13;random^=random>>17;random^=random<<5;x=(double(random)/4294967296.-.5)*.001;}
    ModalHandoffHarness renderer;if(!renderer.prepare(rate,prepared,input,&guard))return 2;
    const auto& tanks=renderer.tanks;const auto bytes=renderer.bytes,duration=renderer.duration;
    const auto& adopted=renderer.adopted;const auto& checksum=renderer.checksum;const auto& peak=renderer.peak;
    allocations=frees=0;tracking=true;auto* probe=::operator new(32,std::align_val_t(32));::operator delete(probe,std::align_val_t(32));tracking=false;
    if(allocations!=1||frees!=1)return 2;
    std::vector<double> overhead;overhead.reserve(10000);for(int n=0;n<10000;++n){const auto a=Clock::now(),b=Clock::now();overhead.push_back(std::chrono::duration<double>(b-a).count());}const auto timer=summarize(std::move(overhead));
    const auto work=[&](bool measureCopy){return renderer.step<false>(block,blockOrder,measureCopy);};
    const auto warmCallbacks=(10*std::uint64_t(rate)+std::uint64_t(block)-1)/std::uint64_t(block),measuredCallbacks=(20*std::uint64_t(rate)+std::uint64_t(block)-1)/std::uint64_t(block);
    std::vector<Record> records;records.reserve(periodic?std::size_t(measuredCallbacks):1000000);std::vector<std::uint64_t> cycles;if(diagnoseCycles)cycles.reserve(records.capacity());
    std::vector<ScheduleRecord> schedules;
    if constexpr(periodic){records.resize(std::size_t(measuredCallbacks));records.clear();cycles.resize(std::size_t(measuredCallbacks));cycles.clear();schedules.resize(std::size_t(measuredCallbacks));schedules.clear();}
    const auto threadCycles=[](std::uint64_t& value) noexcept {
#if defined(_WIN32)
        ULONG64 result=0;const bool valid=QueryThreadCycleTime(GetCurrentThread(),&result)!=0;value=result;return valid;
#else
        return false;
#endif
    };
    bool interrupted=false,overflow=false,cyclesValid=true,scheduleValid=true,periodicOpened=false,periodicClosed=true;const char* reason="none";PeerDetails interruptedPeer;double warmSeconds=0,measureSeconds=0;
    PeriodicSession caller;ReleaseSequence measuredSequence;std::int64_t measureAnchor=0;int priorityBefore=0,priorityDuring=0,priorityAfter=0;
    if constexpr(periodic){
#if defined(_WIN32)
        priorityBefore=GetThreadPriority(GetCurrentThread());
#endif
        periodicOpened=caller.open(nativePeriodicApi(),nativePeriodicTicksPerSecond());
        if(!periodicOpened){const auto failure=caller.failure();const auto error=caller.platformError();caller.close();std::cout<<"MODAL_HANDOFF_CPU_ADMISSION_BLOCKED reason=periodic_api_setup failure="<<int(failure)<<" win32_error="<<error<<" cleanup_error="<<caller.cleanupError()<<'\n';return 3;}
#if defined(_WIN32)
        priorityDuring=GetThreadPriority(GetCurrentThread());
#endif
    }
    std::cout<<std::setprecision(17)<<"MODAL_HANDOFF_CPU_BEGIN row="<<index<<" rate="<<rate<<" block="<<block<<" case="<<(sameSize?"full_size_bank_change":"geometry_change")<<" banks=16 active=8 maximum_simultaneous=9 stages_old="<<prepared[0].count<<" stages_new="<<prepared[1].count<<" transition_frames="<<duration<<'\n'<<std::flush;
    allocations=frees=0;const auto warmStart=Clock::now();Clock::time_point measureStart{};
    try {
        if constexpr(periodic){
            ReleaseSequence warmSequence;std::int64_t finish=0;
            if(!warmSequence.initialize(std::int64_t(warmStart.time_since_epoch().count()),nativePeriodicTicksPerSecond(),std::uint32_t(rate),std::uint32_t(block))||!warmSequence.tickAt(warmCallbacks+1,finish))throw PeriodicObserved{"periodic_clock_setup"};
            for(std::uint64_t ordinal=1;ordinal<=warmCallbacks;++ordinal){std::int64_t release=0;
                if(!warmSequence.tickAt(ordinal,release)||!caller.waitTo(release))throw PeriodicObserved{"periodic_warm_wait"};
                tracking=true;work(false);tracking=false;guard();
            }
            if(!caller.waitTo(finish))throw PeriodicObserved{"periodic_warm_finish"};
            warmSeconds=std::chrono::duration<double>(Clock::now()-warmStart).count();guard.force();measureStart=Clock::now();measureAnchor=std::int64_t(measureStart.time_since_epoch().count());
            if(!measuredSequence.initialize(measureAnchor,nativePeriodicTicksPerSecond(),std::uint32_t(rate),std::uint32_t(block))||!measuredSequence.tickAt(measuredCallbacks+1,finish))throw PeriodicObserved{"periodic_measure_clock"};
            for(std::uint64_t ordinal=0;ordinal<measuredCallbacks;++ordinal){
                if(records.size()==records.capacity()||schedules.size()==schedules.capacity()||cycles.size()==cycles.capacity()){overflow=true;break;}
                std::int64_t release=0;if(!measuredSequence.tickAt(ordinal+1,release)||!caller.waitTo(release))throw PeriodicObserved{"periodic_measure_wait"};
                std::uint64_t before=0,after=0;if(!threadCycles(before)){cyclesValid=false;break;}
                tracking=true;const auto begin=Clock::now();auto record=work(true);const auto end=Clock::now();record.blockSeconds=std::chrono::duration<double>(end-begin).count();tracking=false;
                cyclesValid=threadCycles(after)&&after>=before;cycles.push_back(cyclesValid?after-before:0);
                const ScheduleRecord scheduled{release,std::int64_t(begin.time_since_epoch().count()),std::int64_t(end.time_since_epoch().count())};
                scheduleValid=classifySchedule(measuredSequence,ordinal,scheduled)!=ScheduleDisposition::invalid;schedules.push_back(scheduled);records.push_back(record);
                if(!cyclesValid||!scheduleValid)break;guard();
            }
            if(!caller.waitTo(finish))throw PeriodicObserved{"periodic_measure_finish"};
            measureSeconds=std::chrono::duration<double>(Clock::now()-measureStart).count();guard.force();
        }else{
        while(std::chrono::duration<double>(Clock::now()-warmStart).count()<10){tracking=true;work(false);tracking=false;guard();}
        warmSeconds=std::chrono::duration<double>(Clock::now()-warmStart).count();guard.force();measureStart=Clock::now();
        while(std::chrono::duration<double>(Clock::now()-measureStart).count()<20){if(records.size()==records.capacity()){overflow=true;break;}
            std::uint64_t before=0,after=0;if(diagnoseCycles&&!threadCycles(before)){cyclesValid=false;break;}
            tracking=true;const auto begin=Clock::now();auto record=work(true);record.blockSeconds=std::chrono::duration<double>(Clock::now()-begin).count();tracking=false;
            if(diagnoseCycles){cyclesValid=threadCycles(after)&&after>=before;cycles.push_back(cyclesValid?after-before:0);}
            records.push_back(record);if(!cyclesValid)break;guard();}
        measureSeconds=std::chrono::duration<double>(Clock::now()-measureStart).count();guard.force();
        }
    }catch(const PeerObserved& peer){tracking=false;interrupted=true;reason=peer.reason;interruptedPeer=peer.details;if(measureStart==Clock::time_point{})warmSeconds=std::chrono::duration<double>(Clock::now()-warmStart).count();else measureSeconds=std::chrono::duration<double>(Clock::now()-measureStart).count();}
    catch(const PeriodicObserved& error){tracking=false;interrupted=true;reason=error.reason;if(measureStart==Clock::time_point{})warmSeconds=std::chrono::duration<double>(Clock::now()-warmStart).count();else measureSeconds=std::chrono::duration<double>(Clock::now()-measureStart).count();}
    tracking=false;
    if constexpr(periodic){
        const bool firstClose=caller.close(),released=caller.close();periodicClosed=firstClose&&released&&caller.cleanupError()==0;
#if defined(_WIN32)
        priorityAfter=GetThreadPriority(GetCurrentThread());periodicClosed=periodicClosed&&priorityBefore!=THREAD_PRIORITY_ERROR_RETURN&&priorityAfter==priorityBefore;
#endif
        if(!released||priorityAfter!=priorityBefore){std::cerr<<"MODAL_HANDOFF_PERIODIC_CLEANUP_INVALID error="<<caller.cleanupError()<<" priority_before="<<priorityBefore<<" priority_after="<<priorityAfter<<'\n';return 2;}
        if(!periodicClosed){interrupted=true;if(std::string_view(reason)=="none")reason="periodic_cleanup_failure";}
    }
    if constexpr(requires{guard.stop();guard.checkFailure();}){
        guard.stop();if(!interrupted)try{guard.checkFailure();}catch(const PeerObserved& peer){interrupted=true;reason=peer.reason;interruptedPeer=peer.details;}
    }
    std::ofstream output(timingPath,std::ios::binary|std::ios::out);if(!output)return 2;if(!records.empty())output.write(reinterpret_cast<const char*>(records.data()),std::streamsize(records.size()*sizeof(Record)));output.close();if(!output)return 2;
    if(diagnoseCycles){std::ofstream cycleOutput(cyclePath,std::ios::binary|std::ios::out);if(!cycleOutput)return 2;
        if(!cycles.empty())cycleOutput.write(reinterpret_cast<const char*>(cycles.data()),std::streamsize(cycles.size()*sizeof(std::uint64_t)));cycleOutput.close();if(!cycleOutput)return 2;}
    if constexpr(periodic){std::ofstream scheduleOutput(schedulePath,std::ios::binary|std::ios::out);if(!scheduleOutput)return 2;
        if(!schedules.empty())scheduleOutput.write(reinterpret_cast<const char*>(schedules.data()),std::streamsize(schedules.size()*sizeof(ScheduleRecord)));scheduleOutput.close();if(!scheduleOutput)return 2;}
    std::vector<double> times,copies;times.reserve(records.size());copies.reserve(records.size());std::size_t misses=0;
    for(std::size_t i=0;i<records.size();++i){const auto& record=records[i];times.push_back(record.blockSeconds);if(record.copyUpperBytes)copies.push_back(record.copySeconds);
        bool late=record.blockSeconds>period;double deadlineElapsed=record.blockSeconds;
        if constexpr(periodic){late=classifySchedule(measuredSequence,i,schedules[i])==ScheduleDisposition::late;deadlineElapsed=double(schedules[i].finishTick-schedules[i].releaseTick)/nativePeriodicTicksPerSecond();}
        if(late){++misses;std::cout<<"MODAL_HANDOFF_DEADLINE_MISS row="<<index<<" ordinal="<<i<<" seconds="<<deadlineElapsed<<" kernel_seconds="<<record.blockSeconds<<'\n';}}
    const auto stats=summarize(times),copyStats=summarize(copies);
    const bool pass=!interrupted&&!overflow&&cyclesValid&&(!diagnoseCycles||cycles.size()==records.size())&&(!periodic||(periodicOpened&&periodicClosed&&scheduleValid&&schedules.size()==records.size()&&records.size()==measuredCallbacks))&&warmSeconds>=10&&measureSeconds>=20&&!times.empty()&&!copies.empty()&&adopted&&std::isfinite(checksum)&&peak<4&&allocations==0&&frees==0&&stats.mean<=limit&&stats.p99<=limit&&copyStats.p99<=copyLimit&&misses==0;
    const auto peerName=peerImageAscii(interruptedPeer);
    if constexpr(requires{guard.statistics();}){const auto monitor=guard.statistics();std::cout<<"MODAL_HANDOFF_INVENTORY_MONITOR mode=worker scans="<<monitor.scans<<" maximum_scan_seconds="<<monitor.maximumScanSeconds<<" cadence_ms=100 freshness_ms=250\n";}
    else std::cout<<"MODAL_HANDOFF_INVENTORY_MONITOR mode=inline\n";
    std::cout<<"MODAL_HANDOFF_ALLOCATION_DOMAIN render_thread=1\n";
    if constexpr(periodic){std::cout<<"MODAL_HANDOFF_PERIODIC_CALLER mode=periodic-proaudio6 opened="<<periodicOpened<<" closed="<<periodicClosed<<" task=Pro_Audio timer=high_resolution anchor_tick="<<measureAnchor<<" ticks_per_second="<<nativePeriodicTicksPerSecond()<<" expected_samples="<<measuredCallbacks<<" warm_callbacks="<<warmCallbacks<<" samples="<<schedules.size()<<" schedule_valid="<<scheduleValid<<" priority_before="<<priorityBefore<<" priority_during="<<priorityDuring<<" priority_after="<<priorityAfter<<" failure="<<int(caller.failure())<<" platform_error="<<caller.platformError()<<" cleanup_error="<<caller.cleanupError()<<" deadline=scheduled_release_plus_period no_skips=1\n";}
    std::cout<<"MODAL_HANDOFF_PEER_OBSERVATION pid="<<interruptedPeer.pid<<" image="<<peerName.data()<<" inventory_site="<<inventorySiteName(interruptedPeer.inventorySite)<<" win32_error="<<interruptedPeer.win32Error<<" exit_code_known="<<interruptedPeer.exitCodeKnown<<" exit_code="<<interruptedPeer.exitCode<<" exit_query_error="<<interruptedPeer.exitQueryError<<'\n';
    std::cout<<"MODAL_HANDOFF_WORKLOAD order="<<(blockOrder?"tank_major":"frame_major")<<" scratch_bytes="<<renderer.scratchBytes()<<'\n';
    std::cout<<"MODAL_HANDOFF_CYCLE_DIAGNOSTIC enabled="<<diagnoseCycles<<" valid="<<cyclesValid<<" samples="<<cycles.size()<<" format=uint64_native_little_endian outside_block_timer=1\n";
    std::cout<<"MODAL_HANDOFF_CPU_RESULT row="<<index<<" rate="<<rate<<" block="<<block<<" case="<<(sameSize?"full_size_bank_change":"geometry_change")<<" domain=directModal backend="<<tanks[0].backendName()<<" warm_seconds="<<warmSeconds<<" measure_seconds="<<measureSeconds<<" samples="<<records.size()<<" copy_events="<<copies.size()<<" mean_seconds="<<stats.mean<<" p99_seconds="<<stats.p99<<" max_seconds="<<stats.maximum<<" copy_mean_seconds="<<copyStats.mean<<" copy_p99_seconds="<<copyStats.p99<<" copy_max_seconds="<<copyStats.maximum<<" period_seconds="<<period<<" limit_seconds="<<limit<<" copy_limit_seconds="<<copyLimit<<" misses="<<misses<<" allocations="<<allocations<<" frees="<<frees<<" prepared_bytes="<<bytes<<" checksum="<<checksum<<" peak="<<peak<<" adopted="<<adopted<<" timer_min="<<timer.minimum<<" timer_p99="<<timer.p99<<" overflow="<<overflow<<" interrupted="<<interrupted<<" interrupt_reason="<<reason<<" pass="<<pass<<'\n';
    std::cout<<"MODAL_HANDOFF_CPU_FOOTER row="<<index<<" complete="<<!interrupted<<" acceptance=0\n";return interrupted?3:pass?0:1;
}
int compareBlockOrder(){
    int checks=0,failures=0;const auto check=[&](bool value,const char* label){++checks;if(!value){++failures;std::cout<<"FAIL "<<label<<'\n';}};
    Denormals denormals;const auto cases=corpus();
    for(double rate:{44100.,48000.,96000.})for(int block:{64,512})for(bool sameSize:{false,true}){
        ModalHandoffHarness::Banks banks;
        const bool prepared=owned::prepareModal(cases[35].curve,sameSize?1:0,rate,1024,banks[0])&&owned::prepareModal(sameSize?airack::space::DecayCurve{}:cases[35].curve,1,rate,1024,banks[1]);
        check(prepared,"comparison_banks");if(!prepared)continue;
        ModalHandoffHarness::Input input;unsigned random=0x5eeda123;
        for(auto& sample:input)for(auto& x:sample){random^=random<<13;random^=random>>17;random^=random<<5;x=(double(random)/4294967296.-.5)*.001;}
        ModalHandoffHarness reference,candidate;const bool storage=reference.prepare(rate,banks,input)&&candidate.prepare(rate,banks,input);
        check(storage,"comparison_storage");if(!storage)continue;
        for(std::size_t i=0;i<16;++i)for(std::size_t n=0;n<4096;++n){reference.tanks[i].tick(input[n%input.size()]);candidate.tanks[i].tick(input[n%input.size()]);}
        bool audioExact=true,copiesExact=true,finite=true;
        allocations=frees=0;tracking=true;
        const auto blocks=32*((reference.duration+1+std::size_t(block)-1)/std::size_t(block));
        for(std::size_t b=0;b<blocks;++b){
            const auto expected=reference.step<true>(block,false,false),actual=candidate.step<true>(block,true,false);
            copiesExact=copiesExact&&expected.copyUpperBytes==actual.copyUpperBytes;
            for(std::size_t slot=0;slot<8;++slot)for(int n=0;n<block;++n){const auto x=reference.captured[slot][std::size_t(n)],y=candidate.captured[slot][std::size_t(n)];
                audioExact=audioExact&&(std::bit_cast<std::array<std::uint64_t,2>>(x)==std::bit_cast<std::array<std::uint64_t,2>>(y));
                for(double value:y)finite=finite&&std::isfinite(value)&&std::abs(value)<4;}
        }
        bool continuation=true;
        for(std::size_t slot=0;slot<8;++slot)for(std::size_t n=0;n<64;++n){const auto x=reference.tanks[reference.active[slot]].tick(input[n]),y=candidate.tanks[candidate.active[slot]].tick(input[n]);
            continuation=continuation&&(std::bit_cast<std::array<std::uint64_t,2>>(x)==std::bit_cast<std::array<std::uint64_t,2>>(y));}
        tracking=false;
        check(reference.adopted&&candidate.adopted,"comparison_adoption");check(audioExact,"every_audible_sample_exact");check(copiesExact,"adoption_schedule_exact");check(finite,"comparison_finite");
        check(reference.active==candidate.active&&reference.spare==candidate.spare&&reference.selected==candidate.selected&&reference.phase==candidate.phase&&reference.transition==candidate.transition&&reference.nextSlot==candidate.nextSlot&&reference.offset==candidate.offset,"comparison_end_metadata");
        check(std::bit_cast<std::uint64_t>(reference.checksum)==std::bit_cast<std::uint64_t>(candidate.checksum)&&reference.peak==candidate.peak,"comparison_checksum_peak");
        check(continuation,"active_bank_continuation");check(allocations==0&&frees==0,"comparison_no_alloc_free");
        std::cout<<"MODAL_HANDOFF_BLOCK_CASE rate="<<rate<<" block="<<block<<" same_size="<<sameSize<<" compared_frames="<<blocks*std::size_t(block)<<" audio_exact="<<audioExact<<'\n';
    }
    std::cout<<"MODAL_HANDOFF_BLOCK_COMPARE checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
}
int main(int argc,char** argv){
    if(argc==2&&std::string_view(argv[1])=="--observe-inventory"){
        const auto deadline=Clock::now()+std::chrono::seconds(120);std::size_t scans=0;
        do{PeerDetails details;const auto status=nativePeerStatus(0,true,&details);++scans;
            if(status==PeerStatus::unavailable){const auto name=peerImageAscii(details);std::cout<<"INVENTORY_DIAGNOSTIC status=unavailable site="<<inventorySiteName(details.inventorySite)<<" win32_error="<<details.win32Error<<" pid="<<details.pid<<" image="<<name.data()<<" exit_code_known="<<details.exitCodeKnown<<" exit_code="<<details.exitCode<<" exit_query_error="<<details.exitQueryError<<" scans="<<scans<<" acceptance=0\n";return 0;}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }while(Clock::now()<deadline);
        std::cout<<"INVENTORY_DIAGNOSTIC status=not_observed scans="<<scans<<" acceptance=0\n";return 3;
    }
    if(argc==2&&std::string_view(argv[1])=="--compare-block-order")return compareBlockOrder();
    if(argc!=4||(std::string_view(argv[1])!="--profile"&&std::string_view(argv[1])!="--profile-cycles"&&std::string_view(argv[1])!="--profile-block-cycles"&&std::string_view(argv[1])!="--profile-monitored-cycles"&&std::string_view(argv[1])!="--profile-periodic-cycles"))return 2;
    int row=-1;const auto input=std::string_view(argv[2]);const auto parsed=std::from_chars(input.data(),input.data()+input.size(),row);if(parsed.ec!=std::errc{}||parsed.ptr!=input.data()+input.size())return 2;
    try{if(std::string_view(argv[1])=="--profile-periodic-cycles")return profile<ConcurrentPeerGuard,true>(row,argv[3],true,true);
        if(std::string_view(argv[1])=="--profile-monitored-cycles")return profile<ConcurrentPeerGuard>(row,argv[3],true,true);
        return profile(row,argv[3],std::string_view(argv[1])!="--profile",std::string_view(argv[1])=="--profile-block-cycles");}
    catch(const airack::space::probe::PeerObserved& peer){const auto name=peerImageAscii(peer.details);std::cout<<"MODAL_HANDOFF_CPU_ADMISSION_BLOCKED reason="<<peer.reason<<" pid="<<peer.details.pid<<" image="<<name.data()<<" inventory_site="<<inventorySiteName(peer.details.inventorySite)<<" win32_error="<<peer.details.win32Error<<" exit_code_known="<<peer.details.exitCodeKnown<<" exit_code="<<peer.details.exitCode<<" exit_query_error="<<peer.details.exitQueryError<<'\n';return 3;}
}

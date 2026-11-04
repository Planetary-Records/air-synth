#include "SpaceTank.h"
#include "ProbeCases.h"
#include "ProbePeerGuard.h"
#include "LossTail.h"
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
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#include <xmmintrin.h>
#endif
namespace {bool tracking=false;std::size_t allocations=0,frees=0;}
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
int profile(int index,const std::filesystem::path& timingPath,bool diagnostic,bool automatic)
{
    if(index<0||index>=24||std::filesystem::exists(timingPath)){std::cerr<<"CPU_INVALID_INPUT_OR_EXISTING_TIMINGS\n";return 2;}
    PeerGuard guard;guard.force();
    constexpr std::array<double,3> rates{44100,48000,96000};constexpr std::array<int,2> blocks{64,512};
    const double rate=rates[std::size_t(index/8)],size=index%2?1:.5;const int block=blocks[std::size_t(index/4%2)];
    const auto cases=corpus();const auto& named=cases[std::size_t(index/2%2?35:5)];
    const double period=block/rate,limit=.5*period;
    std::array<std::array<double,2>,4096> input;unsigned random=0x5eeda123;
    for(auto& sample:input)for(auto& channel:sample){random^=random<<13;random^=random>>17;random^=random<<5;channel=(double(random)/4294967296.-.5)*.001;}
    allocations=frees=0;tracking=true;void* counterProbe=::operator new(32,std::align_val_t(32));::operator delete(counterProbe,std::align_val_t(32));tracking=false;
    if(allocations!=1||frees!=1){std::cerr<<"CPU_ALLOCATION_COUNTER_INVALID\n";return 2;}
    std::vector<double> overhead;overhead.reserve(10000);
    for(int i=0;i<10000;++i){const auto begin=Clock::now(),end=Clock::now();overhead.push_back(std::chrono::duration<double>(end-begin).count());}
    const auto timer=summarize(std::move(overhead));
    std::array<SpaceTank,8> tanks;std::size_t bytes=0;bool prepared=true;
    const auto prepStart=Clock::now();
    for(auto& tank:tanks){
        const bool ready=tank.prepare(rate,block,named.curve,size,FitGeometry::compact,true,true,TankGeometry::eight,FitDomain::poleContour,automatic?TankBackend::automatic:TankBackend::sse2);
        prepared=prepared&&ready&&tank.preparedBytes()<=16*1024*1024;
        if(ready){auto network=inspectNetwork(tank);network.poll=[&]{guard();};prepared=certifyTail(network,46).passed&&prepared;}
        bytes+=tank.preparedBytes();guard.force();
    }
    const double prepSeconds=std::chrono::duration<double>(Clock::now()-prepStart).count();
    if(!prepared){std::cerr<<"CPU_PREPARATION_REJECTED row="<<index<<'\n';return 2;}
    std::vector<double> times;times.reserve(2000000);std::vector<std::uint64_t> cycles;if(diagnostic)cycles.reserve(times.capacity());
    std::size_t offset=0;double checksum=0;bool cycleQueryValid=true;
    allocations=frees=0;
    auto work=[&]{for(auto& tank:tanks)for(int n=0;n<block;++n){const auto wet=tank.tick(input[(offset+std::size_t(n))%input.size()]);checksum+=wet[0]+wet[1];}offset=(offset+std::size_t(block))%input.size();};
    bool interrupted=false,overflow=false;const char* interruptReason="none";double warmSeconds=0,measureSeconds=0;
    std::cout<<std::setprecision(14)<<"CPU_BEGIN row="<<index<<" rate="<<rate<<" block="<<block<<" case="<<named.name<<" size="<<size<<" tanks=8 prep_seconds="<<prepSeconds<<'\n';std::cout.flush();
    const auto warmStart=Clock::now();Clock::time_point measureStart{};
    try {
        while(std::chrono::duration<double>(Clock::now()-warmStart).count()<10){tracking=true;work();tracking=false;guard();}
        warmSeconds=std::chrono::duration<double>(Clock::now()-warmStart).count();guard.force();measureStart=Clock::now();
        while(std::chrono::duration<double>(Clock::now()-measureStart).count()<20){
            if(times.size()==times.capacity()){overflow=true;break;}
            tracking=true;const auto begin=Clock::now();std::uint64_t first=0,last=0;
#if defined(_WIN32)
            if(diagnostic){DWORD64 value=0;cycleQueryValid=QueryThreadCycleTime(GetCurrentThread(),&value)!=FALSE;first=value;}
#endif
            work();
#if defined(_WIN32)
            if(diagnostic){DWORD64 value=0;cycleQueryValid=(QueryThreadCycleTime(GetCurrentThread(),&value)!=FALSE)&&cycleQueryValid;last=value;}
#endif
            const auto end=Clock::now();tracking=false;
            times.push_back(std::chrono::duration<double>(end-begin).count());if(diagnostic)cycles.push_back(last-first);
            if(!cycleQueryValid)break;guard();
        }
        measureSeconds=std::chrono::duration<double>(Clock::now()-measureStart).count();guard.force();
    }catch(const PeerObserved& peer){tracking=false;interrupted=true;interruptReason=peer.reason;
        if(measureStart==Clock::time_point{})warmSeconds=std::chrono::duration<double>(Clock::now()-warmStart).count();
        else measureSeconds=std::chrono::duration<double>(Clock::now()-measureStart).count();}
    tracking=false;
    std::ofstream output(timingPath,std::ios::binary|std::ios::out);if(!output){std::cerr<<"CPU_TIMING_DUMP_OPEN_FAILED\n";return 2;}
    if(diagnostic){for(std::size_t i=0;i<times.size();++i){output.write(reinterpret_cast<const char*>(&times[i]),sizeof(double));output.write(reinterpret_cast<const char*>(&cycles[i]),sizeof(std::uint64_t));}}
    else if(!times.empty())output.write(reinterpret_cast<const char*>(times.data()),std::streamsize(times.size()*sizeof(double)));output.close();
    if(!output){std::cerr<<"CPU_TIMING_DUMP_WRITE_FAILED\n";return 2;}
    std::size_t misses=0;
    for(std::size_t i=0;i<times.size();++i)if(times[i]>period){++misses;std::cout<<"DEADLINE_MISS row="<<index<<" ordinal="<<i<<" seconds="<<times[i];if(diagnostic)std::cout<<" thread_cycles="<<cycles[i];std::cout<<'\n';}
    const auto stats=summarize(times);
    const bool pass=!interrupted&&!overflow&&!times.empty()&&warmSeconds>=10&&measureSeconds>=20&&std::isfinite(checksum)&&allocations==0&&frees==0
        &&stats.mean<=limit&&stats.p99<=limit&&misses==0;
    std::cout<<(diagnostic?"CPU_DIAGNOSTIC_RESULT row=":"CPU_RESULT row=")<<index<<" rate="<<rate<<" block="<<block<<" case="<<named.name<<" size="<<size<<" domain=poleContour tanks=8 backend="<<tanks[0].backendName()
        <<" warm_seconds="<<warmSeconds<<" measure_seconds="<<measureSeconds<<" samples="<<times.size()<<" mean_seconds="<<stats.mean<<" p99_seconds="<<stats.p99
        <<" max_seconds="<<stats.maximum<<" period_seconds="<<period<<" limit_seconds="<<limit<<" misses="<<misses<<" allocations="<<allocations<<" frees="<<frees
        <<" prepared_bytes="<<bytes<<" checksum="<<checksum<<" timer_min="<<timer.minimum<<" timer_mean="<<timer.mean<<" timer_p99="<<timer.p99
        <<" clock_tick_seconds="<<double(Clock::period::num)/Clock::period::den<<" overflow="<<overflow<<" interrupted="<<interrupted<<" interrupt_reason="<<interruptReason<<" pass="<<pass<<'\n';
    if(diagnostic){const auto cycleStats=summarize(std::vector<double>(cycles.begin(),cycles.end()));
        std::cout<<"CPU_CYCLES row="<<index<<" mean="<<cycleStats.mean<<" p99="<<cycleStats.p99<<" max="<<cycleStats.maximum<<" minimum="<<cycleStats.minimum
            <<" query_valid="<<cycleQueryValid<<" format=double_seconds_uint64_cycles_le records="<<cycles.size()<<" acceptance=0\n";
        std::cout<<"CPU_DIAGNOSTIC_FOOTER row="<<index<<" complete="<<(!interrupted&&cycleQueryValid&&warmSeconds>=10&&measureSeconds>=20)<<" acceptance=0\n";
        return interrupted?3:cycleQueryValid&&!overflow&&warmSeconds>=10&&measureSeconds>=20?0:2;}
    std::cout<<"CPU_FOOTER row="<<index<<" complete="<<!interrupted<<" acceptance=0\n";
    return interrupted?3:pass?0:1;
}
}
int main(int argc,char** argv)
{
    Denormals noDenormals;
    try {
        if(argc!=4||(std::string_view(argv[1])!="--row"&&std::string_view(argv[1])!="--row-automatic"&&std::string_view(argv[1])!="--diagnostic-row")){std::cerr<<"Usage: --row/--row-automatic/--diagnostic-row 0..23 NEW_TIMINGS_FILE\n";return 2;}
        const std::string_view value(argv[2]);int index=-1;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),index);
        if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())return 2;
        return profile(index,std::filesystem::path(argv[3]),std::string_view(argv[1])=="--diagnostic-row",std::string_view(argv[1])=="--row-automatic");
    }catch(const PeerObserved& peer){tracking=false;std::cout<<"CPU_INTERRUPTED "<<peer.reason<<'\n';return 3;}
}

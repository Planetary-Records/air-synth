#include "SaturationCandidate.h"
#define airack airack_baseline
#include RACKSYNTH_OPTIMIZATION_BASELINE_HEADER
#undef airack
#define airack airack_cost
#include RACKSYNTH_OPTIMIZATION_COST_HEADER
#define AIRACK_SATURATION_OPTIMIZATION_TEST 1
#include "SaturationOptimizationWitness.h"
#undef AIRACK_SATURATION_OPTIMIZATION_TEST
#undef airack
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include RACKSYNTH_OPTIMIZATION_ALLOCATION_HEADER

namespace {
using namespace airack;
using airack::probe::Candidate;
using Baseline = airack_baseline::probe::Candidate;
using Cost = airack_cost::probe::Candidate;
namespace cost = airack_cost::probe;
using Frame = Candidate::Frame;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
int checks = 0, failures = 0;
void check(bool okay, const char* message) {
    ++checks;
    if (!okay) { ++failures; if (failures < 20) std::cout << "FAIL " << message << '\n'; }
}
auto oldSettings(const SaturationSettings& s) {
    return std::bit_cast<airack_baseline::SaturationSettings>(s);
}
bool set(Candidate& c, const SaturationSettings& s) { return c.setSettings(s); }
bool set(Baseline& c, const SaturationSettings& s) { return c.setSettings(oldSettings(s)); }
bool set(Cost& c, const SaturationSettings& s) { return c.setSettings(std::bit_cast<airack_cost::SaturationSettings>(s)); }
template<class C> void ready(C& c, double rate, unsigned factor, const SaturationSettings& s) {
    check(c.prepare(rate, factor) && set(c, s), "prepare/settings"); c.reset();
}
bool same(Frame a, Frame b) {
    return std::bit_cast<std::uint64_t>(a[0]) == std::bit_cast<std::uint64_t>(b[0])
        && std::bit_cast<std::uint64_t>(a[1]) == std::bit_cast<std::uint64_t>(b[1]);
}
void hash(std::uint64_t& value, Frame frame) {
    for (double sample : frame) {
        auto bits = std::bit_cast<std::uint64_t>(sample);
        for (unsigned byte = 0; byte < 8; ++byte) { value ^= bits & 255; value *= 1099511628211ULL; bits >>= 8; }
    }
}
Frame signal(std::size_t n, double rate) {
    Frame x;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const double time = double(n)/rate, offset = channel ? std::numbers::pi/3 : 0;
        x[channel] = .01*std::sin(2*std::numbers::pi*220*time+offset)+.005*std::sin(2*std::numbers::pi*1760*time+offset);
    }
    return x;
}
// Exact functions extracted from the sealed baseline feasibility workload.
#include RACKSYNTH_OPTIMIZATION_WORKLOAD_HEADER
struct Pair {
    Candidate current;
    Baseline baseline;
    Cost profiled;
    Frame baselineOutput {};
    Frame costOutput {};
    std::size_t calls = 0;
    void settings(const SaturationSettings& s) { const auto accepted=set(current,s); check(accepted == set(baseline,s), "settings admission parity"); check(accepted == set(profiled,s), "cost settings admission parity"); }
    void prepare(double rate, unsigned factor, const SaturationSettings& s) { ready(current,rate,factor,s); ready(baseline,rate,factor,s); ready(profiled,rate,factor,s); }
    void reset() { current.reset(); baseline.reset(); profiled.reset(); }
    Frame process(Frame x) {
        const auto a = current.process(x), b = baseline.process(x);
        cost::optimizationProfileSample(calls++%127 == 0);
        const auto c = profiled.process(x);
        cost::optimizationProfileSample(false);
        baselineOutput = b;
        costOutput = c;
        check(same(a,b), "ordinary baseline/current output bits");
        check(same(a,c), "ordinary current/cost output bits (sampled and disabled)");
        check(std::isfinite(a[0]) && std::isfinite(a[1]), "finite output");
        return a;
    }
};
void cpuParity(const fs::path& output) {
    std::ofstream rows(output/"cpu-parity.csv"); rows << "suite,rate,block,moving,frames,fnv1a64,bit_differences\n";
    for (bool overlap : {false,true}) for (double rate : {44100.,48000.,96000.})
        for (std::size_t block : {64u,512u}) for (bool moving : {false,true}) {
            std::array<std::unique_ptr<Pair>,8> units;
            for (std::size_t u=0;u<8;++u) { units[u]=std::make_unique<Pair>(); units[u]->prepare(rate,4,cpuSettings(u,moving?0:-1)); }
            std::uint64_t checksum=14695981039346656037ULL;
            const auto before=failures;
            const auto name=std::string(overlap?"overlap":"base")+'-'+std::to_string(int(rate))+'-'+std::to_string(block)+'-'+std::to_string(moving);
            std::ofstream audio(output/(name+".f64"),std::ios::binary), baselineAudio(output/(name+".baseline.f64"),std::ios::binary), costAudio(output/(name+".cost.f64"),std::ios::binary);
            constexpr std::size_t frames=4096;
            for (std::size_t start=0;start<frames;start+=block) {
                if (overlap || moving) for (std::size_t u=0;u<8;++u) units[u]->settings(cpuTarget(u,moving,start,rate,overlap));
                for (std::size_t n=start;n<start+block;++n) {
                    auto x=signal(n,rate); for (auto& unit:units) x=unit->process(x);
                    hash(checksum,x); audio.write(reinterpret_cast<const char*>(x.data()),sizeof(x));
                    baselineAudio.write(reinterpret_cast<const char*>(units.back()->baselineOutput.data()),sizeof(x));
                    costAudio.write(reinterpret_cast<const char*>(units.back()->costOutput.data()),sizeof(x));
                }
            }
            rows << (overlap?"structural_overlap":"base") << ',' << int(rate) << ',' << block << ',' << moving << ',' << frames << ',' << checksum << ',' << failures-before << '\n';
        }
}
void lifecycleParity(const fs::path& output) {
    std::ofstream audio(output/"lifecycle.f64",std::ios::binary), baselineAudio(output/"lifecycle.baseline.f64",std::ios::binary), costAudio(output/"lifecycle.cost.f64",std::ios::binary);
    std::ofstream cases(output/"lifecycle-cases.csv");
    cases << "case,sample_rate,mode,character,factor,first_frame,frames\n";
    std::size_t firstFrame=0;
    for (double rate : {44100.,44101.,48000.,96000.}) for (unsigned mode : {0u,1u}) for (unsigned character : {0u,1u,2u}) {
        cases << "lifecycle-" << int(rate) << '-' << mode << '-' << character << ',' << int(rate) << ',' << mode << ',' << character << ",4," << firstFrame << ",8192\n";
        firstFrame+=8192;
        Pair p; auto s=cpuSettings(character,0); s.mode=static_cast<SaturationMode>(mode);
        for (auto& lane:s.lanes) lane.character=static_cast<SaturationCharacter>(character);
        p.prepare(rate,4,s);
        for (std::size_t n=0;n<8192;++n) {
            if (n%211==0 && n<4096) {
                s=cpuMoving(character,double(n)/rate); s.mode=static_cast<SaturationMode>((n/211+mode)%2);
                s.preLowCutEnabled=n%422==0; s.postHighCutEnabled=n%633==0; s.driveCompensation=n%844==0;
                for (auto& lane:s.lanes) { lane.character=static_cast<SaturationCharacter>((n/211+character)%3); lane.bypassed=n%422!=0; }
                p.settings(s);
            }
            if (n==4100) { auto bad=s; bad.lanes[3].driveDb=std::numeric_limits<float>::quiet_NaN(); check(!set(p.current,bad)&&!set(p.baseline,bad)&&!set(p.profiled,bad),"invalid settings refused"); }
            if (n==4200) { check(!p.current.prepare(0,4)&&!p.baseline.prepare(0,4)&&!p.profiled.prepare(0,4),"invalid rate refused"); }
            if (n==4300) p.process({std::numeric_limits<double>::infinity(),0});
            if (n==4400) p.reset();
            if (n==4500) p.prepare(rate,4,s);
            const auto x=p.process(n>6000?Frame{}:signal(n,rate));
            audio.write(reinterpret_cast<const char*>(x.data()),sizeof(x));
            baselineAudio.write(reinterpret_cast<const char*>(p.baselineOutput.data()),sizeof(x));
            costAudio.write(reinterpret_cast<const char*>(p.costOutput.data()),sizeof(x));
        }
    }
    for (unsigned factor : {1u,2u,8u,64u}) {
        cases << "fallback-" << factor << ",48000,1,2," << factor << ',' << firstFrame << ",2048\n";
        firstFrame+=2048;
        Pair p; p.prepare(48000,factor,cpuSettings(2,0));
        for (std::size_t n=0;n<2048;++n) {
            if (n%193==0) p.settings(cpuTarget(2,true,n,48000,true));
            if (n==1200) p.reset();
            const auto x=p.process(signal(n,48000)); audio.write(reinterpret_cast<const char*>(x.data()),sizeof(x));
            baselineAudio.write(reinterpret_cast<const char*>(p.baselineOutput.data()),sizeof(x));
            costAudio.write(reinterpret_cast<const char*>(p.costOutput.data()),sizeof(x));
        }
    }
}
bool sameProfile(const cost::OptimizationProfile& a, const cost::OptimizationProfile& b) {
    return a.nanoseconds==b.nanoseconds && a.calls==b.calls && a.selectors==b.selectors
        && a.coefficientRows==b.coefficientRows && a.temporalEntries==b.temporalEntries
        && a.preparedFallbacks==b.preparedFallbacks && a.legacyEntries==b.legacyEntries && a.denseLowEntries==b.denseLowEntries;
}
void profileCheck() {
    const auto exercise=[] {
        check(cost::runSaturationOptimizationPathChecks()==0,"existing work-count checks");
        Cost candidate; ready(candidate,96000,4,cpuSettings(0,0));
        for (std::size_t n=0;n<2048;++n) {
            if (n%64==0) check(set(candidate,cpuTarget(0,true,n,96000,true)),"profile controls admitted");
            const auto x=candidate.process(signal(n,96000));
            check(std::isfinite(x[0])&&std::isfinite(x[1]),"profile finite output");
        }
    };
    cost::optimizationProfileSample(false); cost::optimizationProfileReset();
    exercise(); check(sameProfile(cost::optimizationProfileRead(),{}),"disabled profile remains zero");
    cost::optimizationProfileSample(true); exercise();
    const auto observed=cost::optimizationProfileRead();
    for (unsigned i=0;i<unsigned(cost::OptimizationStage::count);++i) {
        std::cout << "PROFILE_STAGE " << i << " calls=" << observed.calls[i] << " ns=" << observed.nanoseconds[i] << '\n';
        check(observed.calls[i]>0&&observed.nanoseconds[i]>0,"every profile stage observed");
    }
    for (unsigned i=0;i<observed.selectors.size();++i) {
        std::cout << "PROFILE_SELECTOR " << i << " calls=" << observed.selectors[i] << '\n';
        check(observed.selectors[i]>0,"every character/selector observed");
    }
    cost::optimizationProfileSample(false); exercise();
    check(sameProfile(cost::optimizationProfileRead(),observed),"disabled profile does not change existing counts");
    cost::optimizationProfileReset(); check(sameProfile(cost::optimizationProfileRead(),{}),"reset clears counts");
    const auto overhead=cost::runSaturationOptimizationProfileOverhead();
    for (unsigned i=0;i<unsigned(cost::OptimizationStage::count);++i)
        check(overhead.calls[i]==(i==unsigned(cost::OptimizationStage::candidate)?100000:0),"empty scope overhead stage counts");
    check(overhead.selectors==cost::OptimizationProfile{}.selectors,"empty scope overhead has no selectors");
    check(overhead.coefficientRows==cost::OptimizationProfile{}.coefficientRows && overhead.temporalEntries==cost::OptimizationProfile{}.temporalEntries
        && overhead.preparedFallbacks==cost::OptimizationProfile{}.preparedFallbacks && overhead.legacyEntries==cost::OptimizationProfile{}.legacyEntries
        && overhead.denseLowEntries==0,"empty scope overhead has no sampled row or route counters");
    exercise(); check(sameProfile(cost::optimizationProfileRead(),overhead),"overhead returns sampling disabled");
    cost::optimizationProfileReset();
}
volatile double sink=0;
template<class C, bool Profiling = false, std::size_t Block = 64, std::size_t WarmCallbacks = 1500, std::size_t MeasuredCallbacks = 3000, bool RowColumns = false> void screenCase(std::ostream& rows, const fs::path& output, unsigned order, const char* name, bool moving, bool overlap) {
    constexpr double rate=96000; constexpr std::size_t block=Block, warmCallbacks=WarmCallbacks, measuredCallbacks=MeasuredCallbacks;
    std::array<std::unique_ptr<C>,8> units;
    for (std::size_t u=0;u<8;++u) { units[u]=std::make_unique<C>(); ready(*units[u],rate,4,cpuSettings(u,moving?0:-1)); }
    std::array<Frame,block> input,actual;
    std::array<double,measuredCallbacks> times;
    std::uint64_t checksum=14695981039346656037ULL;
    long double energy=0; bool finite=true,admitted=true;
    std::size_t allocations=0,frees=0;
    const auto id=std::to_string(order)+'-'+name+'-'+std::to_string(moving)+'-'+std::to_string(overlap);
    std::ofstream raw(output/(id+"-callbacks.csv")); raw << std::setprecision(17) << "phase,index,seconds,allocations,frees";
    if constexpr (Profiling) {
        for (const auto* stage : {"candidate","controls","prefix","crossover","lane","returning"}) raw << ',' << stage << "_ns," << stage << "_calls";
        for (unsigned i=0;i<9;++i) raw << ",selector_" << i;
        if constexpr (RowColumns) {
            for (unsigned group=0;group<4;++group) for (unsigned mask=0;mask<16;++mask) raw << ",row_" << group << '_' << mask;
            for (const auto* route : {"temporal","prepared_fallback","legacy"}) for (unsigned character=0;character<3;++character) raw << ',' << route << '_' << character;
            raw << ",dense_low_entries";
        }
    }
    raw << '\n';
    std::size_t rendered=0;
    for (unsigned phase=0;phase<2;++phase) for (std::size_t callback=0;callback<(phase?measuredCallbacks:warmCallbacks);++callback) {
        if constexpr (Profiling) { cost::optimizationProfileSample(false); cost::optimizationProfileReset(); }
        for (std::size_t n=0;n<block;++n) input[n]=signal(rendered+n,rate);
        allocationWitness::allocations=allocationWitness::frees=0;
        const auto before=Clock::now(); allocationWitness::enabled=true;
        if (overlap) for (std::size_t u=0;u<8;++u) admitted=set(*units[u],cpuTarget(u,moving,rendered,rate,true))&&admitted;
        else if (moving) for (std::size_t u=0;u<8;++u) admitted=set(*units[u],cpuMoving(u,double(rendered)/rate))&&admitted;
        std::size_t index=0;
        for (auto x:input) {
            if constexpr (Profiling) cost::optimizationProfileSample(phase && (rendered+index)%127==0);
            for (auto& unit:units) x=unit->process(x); actual[index++]=x;
        }
        allocationWitness::enabled=false; const auto after=Clock::now(); sink=actual.back()[0]+actual.back()[1];
        if constexpr (Profiling) cost::optimizationProfileSample(false);
        const auto seconds=std::chrono::duration<double>(after-before).count();
        if (phase) times[callback]=seconds;
        allocations+=allocationWitness::allocations; frees+=allocationWitness::frees;
        raw << phase << ',' << callback << ',' << seconds << ',' << allocationWitness::allocations << ',' << allocationWitness::frees;
        if constexpr (Profiling) {
            const auto observed=cost::optimizationProfileRead();
            for (unsigned i=0;i<unsigned(cost::OptimizationStage::count);++i) raw << ',' << observed.nanoseconds[i] << ',' << observed.calls[i];
            for (auto count:observed.selectors) raw << ',' << count;
            if constexpr (RowColumns) {
                std::array<std::uint64_t,4> groupEntries {}; std::uint64_t temporal=0,allEntries=0,polynomial=0;
                for (unsigned group=0;group<4;++group) for (auto count:observed.coefficientRows[group]) { raw << ',' << count; groupEntries[group]+=count; polynomial+=count; }
                for (const auto& route : {observed.temporalEntries,observed.preparedFallbacks,observed.legacyEntries})
                    for (auto count:route) { raw << ',' << count; allEntries+=count; }
                raw << ',' << observed.denseLowEntries;
                for (unsigned character=0;character<3;++character) {
                    temporal+=observed.temporalEntries[character];
                    const auto entries=observed.temporalEntries[character]+observed.preparedFallbacks[character]+observed.legacyEntries[character];
                    check(observed.selectors[character*3]+observed.selectors[character*3+1]+observed.selectors[character*3+2]==8*entries,"sampled per-character selector/route conservation");
                }
                check(allEntries==observed.calls[unsigned(cost::OptimizationStage::lane)],"sampled lane route conservation");
                check(polynomial+observed.denseLowEntries==temporal,"sampled polynomial/dense-low conservation");
                check(groupEntries[0]==observed.temporalEntries[0] && groupEntries[1]==observed.temporalEntries[1]
                    && groupEntries[2]+groupEntries[3]+observed.denseLowEntries==observed.temporalEntries[2],"sampled per-character admitted conservation");
                std::uint64_t sampledFrames=0;
                if (phase) for (std::size_t n=0;n<block;++n) sampledFrames+=(rendered+n)%127==0;
                const auto candidateCalls=observed.calls[unsigned(cost::OptimizationStage::candidate)];
                check(candidateCalls==8*sampledFrames,"sampled native-frame/candidate cadence");
                check(overlap ? allEntries>=3*candidateCalls && allEntries<=6*candidateCalls : allEntries==3*candidateCalls,"sampled actual endpoint lane bounds");
            }
        }
        raw << '\n';
        for (auto x:actual) { hash(checksum,x); for (double v:x) { finite=finite&&std::isfinite(v); energy+=static_cast<long double>(v)*v; } }
        rendered+=block;
    }
    double sum=0; for (double t:times) sum+=t;
    const double mean=sum/times.size(); std::sort(times.begin(),times.end()); const double p99=times[std::size_t(std::ceil(.99*times.size()))-1];
    rows << order << ',' << name << ',' << moving << ',' << overlap << ',' << warmCallbacks << ',' << measuredCallbacks << ',' << mean << ',' << p99 << ',' << times.back() << ',' << mean/(block/rate) << ',' << p99/(block/rate) << ',' << checksum << ',' << energy << ',' << finite << ',' << admitted << ',' << allocations << ',' << frees << '\n'; rows.flush();
    check(finite&&energy>0&&admitted&&allocations==0&&frees==0,"screen integrity/zero callback heap");
    std::cout << "SCREEN " << id << " mean_fraction=" << mean/(block/rate) << " p99_fraction=" << p99/(block/rate) << " FNV=" << checksum << std::endl;
}
void profile(const fs::path& output) {
    std::ofstream rows(output/"profile.csv"); rows << std::setprecision(17) << "order,kernel,moving,overlap,warm_callbacks,measured_callbacks,mean_seconds,p99_seconds,max_seconds,mean_fraction,p99_fraction,fnv1a64,energy,finite,settings_accepted,allocations,frees\n";
    for (bool moving:{false,true}) for (bool overlap:{false,true}) screenCase<Cost,true>(rows,output,0,"cost",moving,overlap);
    const auto overhead=cost::runSaturationOptimizationProfileOverhead();
    std::ofstream timer(output/"empty-scope-overhead.csv"); timer << "stage,nanoseconds,calls\n";
    for (unsigned i=0;i<unsigned(cost::OptimizationStage::count);++i) timer << i << ',' << overhead.nanoseconds[i] << ',' << overhead.calls[i] << '\n';
    check(overhead.calls[unsigned(cost::OptimizationStage::candidate)]==100000,"exact empty scope overhead count");
    const auto saved=cost::optimizationProfileRead(); Cost candidate; ready(candidate,96000,4,cpuSettings(0,-1));
    candidate.process(signal(0,96000));
    check(sameProfile(saved,cost::optimizationProfileRead()),"overhead returns with sampling disabled");
}
void rowProfile(const fs::path& output) {
    const auto dir64=output/"block-64",dir512=output/"block-512";
    fs::create_directory(dir64); fs::create_directory(dir512);
    std::ofstream rows64(dir64/"profile.csv"),rows512(dir512/"profile.csv");
    for (auto* rows : {&rows64,&rows512}) *rows << std::setprecision(17) << "order,kernel,moving,overlap,warm_callbacks,measured_callbacks,mean_seconds,p99_seconds,max_seconds,mean_fraction,p99_fraction,fnv1a64,energy,finite,settings_accepted,allocations,frees\n";
    for (bool moving:{false,true}) for (bool overlap:{false,true})
        screenCase<Cost,true,64,1500,3000,true>(rows64,dir64,0,"cost",moving,overlap);
    for (bool moving:{false,true}) for (bool overlap:{false,true})
        screenCase<Cost,true,512,188,375,true>(rows512,dir512,0,"cost",moving,overlap);
    const auto overhead=cost::runSaturationOptimizationProfileOverhead();
    std::ofstream timer(output/"empty-scope-overhead.csv"); timer << "stage,nanoseconds,calls\n";
    for (unsigned i=0;i<unsigned(cost::OptimizationStage::count);++i) timer << i << ',' << overhead.nanoseconds[i] << ',' << overhead.calls[i] << '\n';
    check(overhead.calls[unsigned(cost::OptimizationStage::candidate)]==100000,"exact empty scope overhead count");
    check(overhead.coefficientRows==cost::OptimizationProfile{}.coefficientRows && overhead.temporalEntries==cost::OptimizationProfile{}.temporalEntries
        && overhead.preparedFallbacks==cost::OptimizationProfile{}.preparedFallbacks && overhead.legacyEntries==cost::OptimizationProfile{}.legacyEntries
        && overhead.denseLowEntries==0,"row profile empty scopes have no sampled row or route counters");
    const auto saved=cost::optimizationProfileRead(); Cost candidate; ready(candidate,96000,4,cpuSettings(0,-1));
    candidate.process(signal(0,96000));
    check(sameProfile(saved,cost::optimizationProfileRead()),"overhead returns with sampling disabled");
}
void screen(const fs::path& output) {
    std::ofstream rows(output/"screen.csv"); rows << std::setprecision(17) << "order,kernel,moving,overlap,warm_callbacks,measured_callbacks,mean_seconds,p99_seconds,max_seconds,mean_fraction,p99_fraction,fnv1a64,energy,finite,settings_accepted,allocations,frees\n";
    for (unsigned order:{0u,1u}) for (bool overlap:{false,true}) for (bool moving:{false,true}) {
        if (order==0) { screenCase<Baseline>(rows,output,order,"baseline",moving,overlap); screenCase<Candidate>(rows,output,order,"candidate",moving,overlap); }
        else { screenCase<Candidate>(rows,output,order,"candidate",moving,overlap); screenCase<Baseline>(rows,output,order,"baseline",moving,overlap); }
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc==2 && std::string(argv[1])=="--work-count") return airack_cost::probe::runSaturationOptimizationPathChecks()?1:0;
        if (argc==2 && std::string(argv[1])=="--profile-check") { profileCheck(); std::cout << "Profile check: " << checks << " checks, " << failures << " failures\n"; return failures?1:0; }
        if (argc!=3 || (std::string(argv[1])!="--parity" && std::string(argv[1])!="--screen" && std::string(argv[1])!="--profile" && std::string(argv[1])!="--row-profile")) throw std::runtime_error("usage: --work-count | --profile-check | --parity|--screen|--profile|--row-profile FRESH_ABSOLUTE_DIRECTORY");
        const fs::path output=argv[2];
        if (!output.is_absolute() || fs::exists(output)) throw std::runtime_error("fresh absolute output required");
        fs::create_directories(output);
        if (std::string(argv[1])=="--parity") { cpuParity(output); lifecycleParity(output); }
        else if (std::string(argv[1])=="--profile") profile(output);
        else if (std::string(argv[1])=="--row-profile") rowProfile(output);
        else screen(output);
        std::cout << "Optimization witness: " << checks << " checks, " << failures << " failures; additive diagnostics, no acceptance credit\n";
        return failures?1:0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}

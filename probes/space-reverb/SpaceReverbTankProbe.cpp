#include "SpaceTank.h"
#include "ProbeCases.h"
#include "ProbePeerGuard.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <string_view>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif
#if defined(_MSC_VER) && defined(_M_X64)
#include <xmmintrin.h>
#endif
namespace { bool tracking=false;std::size_t allocations=0,frees=0; }
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}

namespace {
using namespace airack::space;
using namespace airack::space::probe;
int checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<line<<" "<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
struct Denormals
{
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned old=_mm_getcsr();
    Denormals(){_mm_setcsr(old|0x8040);}
    ~Denormals(){_mm_setcsr(old);}
#endif
};
struct Estimate {double time=0,r2=0;std::size_t points=0;bool valid=false;};
Estimate estimate(const std::vector<double>& energy,double dt,double endDb)
{
    std::vector<double> db(energy.size());double sum=0;
    for(size_t i=energy.size();i-->0;){sum+=energy[i];db[i]=sum;}
    if(!(sum>0)||!std::isfinite(sum))return {};
    double st=0,sy=0,stt=0,sty=0;size_t count=0;
    for(size_t i=0;i<db.size();++i){
        db[i]=db[i]>0?10*std::log10(db[i]/sum):-10000;
        if(db[i]<=-5&&db[i]>=endDb){const double t=(i+.5)*dt;st+=t;sy+=db[i];stt+=t*t;sty+=t*db[i];++count;}
    }
    if(count<4)return {};
    const double denominator=count*stt-st*st;
    if(!(denominator>0))return {};
    const double slope=(count*sty-st*sy)/denominator,intercept=(sy-slope*st)/count;
    double error=0,total=0;
    for(size_t i=0;i<db.size();++i)if(db[i]<=-5&&db[i]>=endDb){
        const double e=db[i]-intercept-slope*(i+.5)*dt;error+=e*e;const double y=db[i]-sy/count;total+=y*y;
    }
    if(!(slope<0)||!(total>0))return {};
    const double r2=1-error/total;
    return {-60/slope,r2,count,std::isfinite(slope)&&r2>=.95};
}
Biquad bandpass(double f,double q,double rate)
{
    const double w=2*std::numbers::pi*f/rate,a=std::sin(w)/(2*q),d=1+a;
    return {a/d,0,-a/d,-2*std::cos(w)/d,(1-a)/d};
}
struct Measurement {double target=0;Estimate t20,t30;};
struct Capture {std::array<Measurement,7> bands;double peak=0;bool finite=true;size_t bytes=0;double energy=0,tailFraction=std::numeric_limits<double>::infinity();std::array<double,7> fixedRatios{};};
constexpr std::array<double,7> frequencies{125,250,500,1000,2000,4000,8000};
template<class Source>
Capture captureSource(Source&& source,const DecayCurve& curve,double rate,size_t bytes,double maxQ=24,PeerGuard* guard=nullptr)
{
    double longest=.1;
    const double upper=std::min(20000.,.45*rate);
    for(int i=0;i<769;++i)longest=std::max(longest,evaluate(curve,20*std::pow(upper/20,i/768.),rate)->seconds);
    for(const auto& band:curve.bands)if(band.active&&!band.bypassed)longest=std::max(longest,evaluate(curve,band.frequencyHz,rate)->seconds);
    const size_t frames=size_t(std::ceil(std::max(2.,1.25*longest+.3)*rate));
    const size_t binFrames=size_t(std::round(rate*.001)),bins=(frames+binFrames-1)/binFrames;
    std::array<std::vector<double>,7> energies;
    std::array<std::array<std::array<Biquad,4>,2>,7> filters;
    std::array<std::array<std::array<Biquad,4>,2>,7> fixedFilters;
    std::array<double,7> early{},late{};
    Capture result;result.bytes=bytes;
    for(size_t b=0;b<7;++b){
        energies[b].resize(bins);
        result.bands[b].target=evaluate(curve,frequencies[b],rate)->seconds;
        const double q=std::clamp(frequencies[b]*result.bands[b].target/10.,.70710678,maxQ);
        for(auto& channel:filters[b])for(auto& filter:channel)filter=bandpass(frequencies[b],q,rate);
        for(auto& channel:fixedFilters[b])for(auto& filter:channel)filter=bandpass(frequencies[b],3,rate);
    }
    const size_t tailStart=frames-std::min(frames,size_t(std::ceil(.25*rate)));double tailEnergy=0;
    for(size_t n=0;n<frames;++n){
        if(guard&&(n&4095)==0)(*guard)();
        const auto wet=source(n);
        result.finite=result.finite&&std::isfinite(wet[0])&&std::isfinite(wet[1]);
        result.peak=std::max({result.peak,std::abs(wet[0]),std::abs(wet[1])});
        const double energy=wet[0]*wet[0]+wet[1]*wet[1];result.energy+=energy;if(n>=tailStart)tailEnergy+=energy;
        for(size_t b=0;b<7;++b)for(size_t ch=0;ch<2;++ch){double y=wet[ch];for(auto& filter:filters[b][ch])y=filter.tick(y);energies[b][n/binFrames]+=y*y;}
        for(size_t b=0;b<7;++b)for(size_t ch=0;ch<2;++ch){double y=wet[ch];for(auto& filter:fixedFilters[b][ch])y=filter.tick(y);
            if(double(n)>=.5*rate&&double(n)<.75*rate)early[b]+=y*y;
            if(double(n)>=1.5*rate&&double(n)<1.75*rate)late[b]+=y*y;}
    }
    result.tailFraction=result.energy>0?tailEnergy/result.energy:std::numeric_limits<double>::infinity();
    for(size_t b=0;b<7;++b)result.fixedRatios[b]=early[b]>0?late[b]/early[b]:std::numeric_limits<double>::infinity();
    for(size_t b=0;b<7;++b){result.bands[b].t20=estimate(energies[b],double(binFrames)/rate,-25);result.bands[b].t30=estimate(energies[b],double(binFrames)/rate,-35);}
    return result;
}
Capture capture(SpaceTank& tank,const DecayCurve& curve,double rate,double maxQ=24,PeerGuard* guard=nullptr)
{return captureSource([&](size_t n){return tank.tick(n==0?std::array<double,2>{.5,.25}:std::array<double,2>{0,0});},curve,rate,tank.preparedBytes(),maxQ,guard);}
std::vector<std::array<double,2>> impulse(SpaceTank& tank,size_t frames,int block)
{
    std::vector<std::array<double,2>> out(frames);
    for(size_t start=0;start<frames;start+=size_t(block))
        for(size_t n=start;n<std::min(frames,start+size_t(block));++n)out[n]=tank.tick(n==0?std::array<double,2>{.5,.25}:std::array<double,2>{0,0});
    return out;
}
void selfTest()
{
    SpaceTank tank;CHECK((tank.tick({1,0})==std::array<double,2>{}));
    auto c=flatForCorpus();c.decaySeconds=1.5;
    CHECK(tank.prepare(48000,512,c,.5,FitGeometry::compact));
    CHECK(tank.preparedBytes()<=16*1024*1024);
    const auto a=impulse(tank,12000,64);double left=0,right=0;
    for(const auto& x:a){left+=x[0]*x[0];right+=x[1]*x[1];}
    CHECK(left>1e-12&&right>1e-12);
    tank.reset();const auto b=impulse(tank,12000,512);CHECK(a==b);
    SpaceTank other;CHECK(other.prepare(48000,512,c,.5,FitGeometry::compact));
    CHECK(a==impulse(other,12000,512));
    allocations=frees=0;tracking=true;
    for(int i=0;i<20000;++i)tank.tick({0,0});
    tank.reset();tracking=false;
    CHECK(allocations==0&&frees==0);
    CHECK(!tank.prepare(0,512,c,.5,FitGeometry::compact));
    CHECK(!tank.prepare(48000,0,c,.5,FitGeometry::compact));
    tank.reset();const auto data=capture(tank,c,48000);const auto& m=data.bands[3];
    CHECK(data.finite&&data.peak>0&&data.peak<=4);
    CHECK(m.t20.valid&&std::abs(m.t20.time/1.5-1)<=.15);
    CHECK(m.t30.valid&&std::abs(m.t30.time/1.5-1)<=.15);
    auto shaped=flatForCorpus();for(auto& band:shaped.bands)band={true,false,DecayShape::bell,1000,4,4};
    const double known=evaluate(shaped,125,48000)->seconds;
    const auto calibration=captureSource([&](size_t n){const double t=double(n)/48000;
        const double y=.1*std::sin(2*std::numbers::pi*125*t)*std::exp(-std::log(1000.)*t/known)
            +std::sin(2*std::numbers::pi*1000*t)*std::exp(-std::log(1000.)*t/40);
        return std::array<double,2>{y,y};},shaped,48000,0);
    const auto& cal=calibration.bands[0];
    CHECK(cal.t20.valid&&std::abs(cal.t20.time/known-1)<=.15);
    CHECK(cal.t30.valid&&std::abs(cal.t30.time/known-1)<=.15);
    std::cout<<"CALIBRATION target="<<known<<" T20="<<cal.t20.time<<" T30="<<cal.t30.time<<" R20="<<cal.t20.r2<<" R30="<<cal.t30.r2<<'\n';
    std::cout<<"TANK_SELF T20="<<m.t20.time<<" T30="<<m.t30.time<<" R20="<<m.t20.r2<<" R30="<<m.t30.r2<<" peak="<<data.peak<<" bytes="<<data.bytes<<'\n';
    std::cout<<"SpaceTank checks="<<checks<<" failures="<<failures<<" allocations="<<allocations<<" frees="<<frees<<'\n';
}
void packedTest(TankGeometry geometry=TankGeometry::eight,FitDomain domain=FitDomain::unitCircle)
{
    const auto c=corpus()[35].curve;
    SpaceTank scalar,packed;
    CHECK(scalar.prepare(48000,512,c,.5,FitGeometry::compact,true,false,geometry,domain));
    CHECK(packed.prepare(48000,512,c,.5,FitGeometry::compact,true,true,geometry,domain));
    CHECK(packed.preparedBytes()<=16*1024*1024);
    bool same=true;unsigned state=0x5eedabcd;allocations=frees=0;tracking=true;
    for(int trial=0;trial<2;++trial){
        for(size_t n=0;n<100000;++n){state^=state<<13;state^=state>>17;state^=state<<5;
            const double x=n<10000?(double(state)/4294967296.-.5)*.001:0;
            const std::array<double,2> input{n==0?.5:x,n==0?.25:-.37*x};
            if(scalar.tick(input)!=packed.tick(input))same=false;
        }
        scalar.reset();packed.reset();
    }
    tracking=false;CHECK(same);CHECK(allocations==0&&frees==0);
    std::cout<<"PACKED_EQ checks="<<checks<<" failures="<<failures<<" exact="<<same<<" allocations="<<allocations<<" frees="<<frees<<'\n';
}
void automaticTest(bool exclusive=true)
{
    PeerGuard guard;if(exclusive)guard.force();const bool accelerated=availableAvx2Kernel()!=nullptr;
    const auto cases=corpus();
    for(double rate:{44100.,48000.,96000.})for(std::size_t curve:{std::size_t(5),std::size_t(35)})for(double size:{.5,1.}){
        SpaceTank scalar,sse,automatic;const auto& c=cases[curve].curve;
        CHECK(scalar.prepare(rate,512,c,size,FitGeometry::compact,true,false,TankGeometry::eight,FitDomain::poleContour));
        CHECK(sse.prepare(rate,512,c,size,FitGeometry::compact,true,true,TankGeometry::eight,FitDomain::poleContour,TankBackend::sse2));
        CHECK(automatic.prepare(rate,512,c,size,FitGeometry::compact,true,true,TankGeometry::eight,FitDomain::poleContour,TankBackend::automatic));
        CHECK(std::string_view(automatic.backendName())==(accelerated?"avx2":"sse2"));CHECK(std::string_view(sse.backendName())=="sse2");
        CHECK(automatic.preparedBytes()<=16*1024*1024);
        bool same=true;unsigned random=0x5eedabcd;allocations=frees=0;tracking=true;
        for(int trial=0;trial<2;++trial){
            for(std::size_t n=0;n<100000;++n){random^=random<<13;random^=random>>17;random^=random<<5;
                const double x=n<10000?(double(random)/4294967296.-.5)*.001:0;
                const std::array<double,2> input{n==0?.5:x,n==0?.25:-.37*x};
                const auto expected=scalar.tick(input);if(sse.tick(input)!=expected||automatic.tick(input)!=expected)same=false;
                if((n&4095)==0){tracking=false;if(exclusive)guard();tracking=true;}
            }scalar.reset();sse.reset();automatic.reset();
        }
        tracking=false;CHECK(same);CHECK(allocations==0&&frees==0);
        std::cout<<"AUTOMATIC_EQ rate="<<rate<<" curve="<<curve<<" size="<<size<<" backend="<<automatic.backendName()<<" exact="<<same<<" allocations="<<allocations<<" frees="<<frees<<'\n';if(exclusive)guard.force();
    }
    SpaceTank forced;CHECK(forced.prepare(48000,512,cases[35].curve,.5,FitGeometry::compact,true,true,TankGeometry::eight,FitDomain::poleContour,TankBackend::avx2)==accelerated);
    CHECK(!forced.prepare(48000,512,cases[35].curve,.5,FitGeometry::compact,true,true,TankGeometry::eight,FitDomain::poleContour,static_cast<TankBackend>(99)));
    std::cout<<"AUTOMATIC_SELF checks="<<checks<<" failures="<<failures<<" avx2_available="<<accelerated<<'\n';
}
void windowTest()
{
    auto c=flatForCorpus();SpaceTank first,second;CHECK(first.prepare(48000,512,c,.5,FitGeometry::compact));CHECK(second.prepare(48000,512,c,.5,FitGeometry::compact));
    const auto a=capture(first,c,48000);
    const auto b=captureSource([&](size_t n){auto wet=second.tick(n==0?std::array<double,2>{.5,.25}:std::array<double,2>{0,0});wet[0]*=2;wet[1]*=2;return wet;},c,48000,second.preparedBytes());
    CHECK(a.energy>0&&std::abs(b.energy/(4*a.energy)-1)<1e-12);
    for(size_t band=0;band<7;++band)CHECK(a.fixedRatios[band]>0&&std::isfinite(a.fixedRatios[band])&&std::abs(b.fixedRatios[band]/a.fixedRatios[band]-1)<1e-12);
    std::cout<<"WINDOW_SELF checks="<<checks<<" failures="<<failures<<'\n';
}
int decay(bool smoke,bool diffusion=true,double maxQ=24,TankGeometry geometry=TankGeometry::eight)
{
    auto rows=corpus();int count=0,bad=0;
    std::cout<<"case,rate,size,frequency,target,t20,t30,r20,r30,tolerance,peak,prepared_bytes,pass\n";
    for(size_t row=0;row<rows.size();++row){
        if(smoke&&row!=5&&row!=13&&row!=14&&row!=36)continue;
        for(double rate:{44100.,48000.,96000.})for(double size:{0.,.5,1.}){
            if(smoke&&(rate!=48000||size!=.5))continue;
            SpaceTank tank;const bool prepared=tank.prepare(rate,512,rows[row].curve,size,FitGeometry::compact,diffusion,true,geometry);
            Capture result;if(prepared)result=capture(tank,rows[row].curve,rate,maxQ);
            bool sharp=false;for(const auto& b:rows[row].curve.bands)if(b.active&&!b.bypassed&&b.q>=4)sharp=true;
            for(size_t band=0;band<7;++band){
                const auto& m=result.bands[band];const double tolerance=m.target<.3||sharp?.25:.15;
                const bool pass=prepared&&result.finite&&result.peak<=4&&result.bytes<=16*1024*1024&&m.t20.valid&&m.t30.valid
                    &&std::abs(m.t20.time/m.target-1)<=tolerance&&std::abs(m.t30.time/m.target-1)<=tolerance;
                ++count;if(!pass)++bad;
                std::cout<<std::setprecision(12)<<rows[row].name<<','<<rate<<','<<size<<','<<frequencies[band]<<','<<m.target<<','<<m.t20.time<<','<<m.t30.time<<','<<m.t20.r2<<','<<m.t30.r2<<','<<tolerance<<','<<result.peak<<','<<result.bytes<<','<<pass<<'\n';
            }
            std::cout.flush();
        }
    }
    std::cout<<"DECAY_SUMMARY rows="<<count<<" failures="<<bad<<'\n';return bad?1:0;
}
int impulseRow(int index)
{
    PeerGuard guard;guard.force();const auto cases=corpus();if(index<0||index>=int(cases.size()*9))return 2;
    const auto& named=cases[size_t(index/9)];
    constexpr std::array<double,3> rates{44100,48000,96000},sizes{0,.5,1};
    const double rate=rates[size_t(index%9/3)],size=sizes[size_t(index%3)];
    SpaceTank tank;const bool prepared=tank.prepare(rate,512,named.curve,size,FitGeometry::compact,true,true,TankGeometry::eight,FitDomain::poleContour);guard.force();
    Capture data;if(prepared)data=capture(tank,named.curve,rate,24,&guard);
    const bool pass=prepared&&data.finite&&std::isfinite(data.energy)&&data.energy>0&&data.peak>0&&data.peak<=4
        &&data.bytes<=16*1024*1024&&std::isfinite(data.tailFraction)&&data.tailFraction<=1e-6;
    bool sharp=false;for(const auto& band:named.curve.bands)if(band.active&&!band.bypassed&&band.q>=4)sharp=true;
    std::cout<<std::setprecision(14)<<"CONFIG index="<<index<<" domain=poleContour scope=raw_impulse_time_stability\n";
    std::cout<<"configuration,case,rate,size,frequency,target,t20,t30,r20,r30,t20_valid,t30_valid,original_slope_pass,fixed_late_early_ratio,raw_peak,raw_energy,raw_tail_fraction,prepared_bytes,prepared,finite,pass,domain\n";
    int slopeFailures=0;
    for(size_t b=0;b<7;++b){const auto& m=data.bands[b];const double tolerance=m.target<.3||sharp?.25:.15;
        const bool slope=prepared&&m.t20.valid&&m.t30.valid&&std::abs(m.t20.time/m.target-1)<=tolerance&&std::abs(m.t30.time/m.target-1)<=tolerance;
        if(!slope)++slopeFailures;
        std::cout<<index<<','<<named.name<<','<<rate<<','<<size<<','<<frequencies[b]<<','<<m.target<<','<<m.t20.time<<','<<m.t30.time<<','<<m.t20.r2<<','<<m.t30.r2<<','<<m.t20.valid<<','<<m.t30.valid<<','<<slope<<','<<data.fixedRatios[b]<<','<<data.peak<<','<<data.energy<<','<<data.tailFraction<<','<<data.bytes<<','<<prepared<<','<<data.finite<<','<<pass<<",poleContour\n";
    }
    guard.force();std::cout<<"EARLY_SLOPE_DIAGNOSTIC index="<<index<<" failures="<<slopeFailures<<" acceptance=0\n";
    std::cout<<"CONFIG_SUMMARY index="<<index<<" bands=7 failures="<<(pass?0:7)<<" acceptance=0\n";return prepared?(pass?0:1):2;
}
bool peerRunning()
{
#if defined(_WIN32)
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE)return true;
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool found=false;
    if(Process32FirstW(snapshot,&entry))do{
        const std::wstring_view name(entry.szExeFile);
        if(entry.th32ProcessID!=GetCurrentProcessId()&&
            (name.starts_with(L"RackSynth")||name==L"Runner.Worker.exe"||name==L"cl.exe"||name==L"link.exe")){found=true;break;}
    }while(Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);return found;
#else
    return true;
#endif
}
int cpuSmoke(TankGeometry geometry=TankGeometry::eight)
{
    using Clock=std::chrono::steady_clock;
    constexpr double rate=96000;constexpr int block=64;
    const double period=block/rate;int bad=0;
    std::array<std::array<double,2>,4096> input;
    unsigned state=0x5eeda123;
    for(auto& sample:input)for(auto& channel:sample){state^=state<<13;state^=state>>17;state^=state<<5;channel=(double(state)/4294967296.-.5)*.001;}
    double overhead=1;
    for(int i=0;i<10000;++i){const auto begin=Clock::now(),end=Clock::now();overhead=std::min(overhead,std::chrono::duration<double>(end-begin).count());}
    std::cout<<"CPU_TIMER_MIN_SECONDS "<<std::setprecision(12)<<overhead<<'\n';
    auto cases=corpus();
    for(size_t row:{size_t(5),size_t(35)}){
        if(peerRunning()){std::cout<<"CPU_ABORT peer_before_prepare\n";return 3;}
        std::array<SpaceTank,8> tanks;bool prepared=true;size_t bytes=0;
        const auto prepStart=Clock::now();
        for(auto& tank:tanks){prepared=tank.prepare(rate,block,cases[row].curve,1,FitGeometry::compact,true,true,geometry)&&prepared;bytes+=tank.preparedBytes();}
        const double prep=std::chrono::duration<double>(Clock::now()-prepStart).count();
        if(!prepared){std::cout<<"CPU_ABORT preparation_failed\n";return 1;}
        std::vector<double> times;times.reserve(2000000);size_t offset=0;double checksum=0;bool tainted=false,overflow=false;
        auto work=[&]{for(auto& tank:tanks)for(int n=0;n<block;++n){auto wet=tank.tick(input[(offset+size_t(n))%input.size()]);checksum+=wet[0]+wet[1];}offset=(offset+block)%input.size();};
        auto scan=Clock::now();const auto warm=scan;
        while(std::chrono::duration<double>(Clock::now()-warm).count()<1){work();if(Clock::now()-scan>=std::chrono::milliseconds(100)){tainted=peerRunning()||tainted;scan=Clock::now();}}
        const auto start=Clock::now();
        while(std::chrono::duration<double>(Clock::now()-start).count()<3){
            const auto begin=Clock::now();work();const auto end=Clock::now();
            if(times.size()==times.capacity()){overflow=true;break;}times.push_back(std::chrono::duration<double>(end-begin).count());
            if(end-scan>=std::chrono::milliseconds(100)){tainted=peerRunning()||tainted;scan=Clock::now();}
        }
        tainted=peerRunning()||tainted;
        double mean=0;size_t misses=0;for(double t:times){mean+=t;if(t>period)++misses;}mean/=times.size();
        std::sort(times.begin(),times.end());const double p99=times[std::min(times.size()-1,size_t(std::ceil(times.size()*.99))-1)],maximum=times.back();
        const bool pass=!tainted&&!overflow&&std::isfinite(checksum)&&mean<=.5*period&&p99<=.5*period&&misses==0;
        if(!pass)++bad;
        std::cout<<"CPU_SMOKE case="<<cases[row].name<<" rate="<<rate<<" block="<<block<<" size=1 tanks=8 prep_seconds="<<prep
            <<" samples="<<times.size()<<" mean_seconds="<<mean<<" p99_seconds="<<p99<<" max_seconds="<<maximum<<" period_seconds="<<period
            <<" deadline_misses="<<misses<<" prepared_bytes="<<bytes<<" checksum="<<checksum<<" tainted="<<tainted<<" overflow="<<overflow<<" pass="<<pass<<'\n';
        std::cout.flush();if(tainted)return 3;
    }
    return bad?1:0;
}
}
int main(int argc,char**argv)
{
  try {
    Denormals noDenormals;
    const std::string_view arg=argc>1?argv[1]:"--self-test";
    if(arg=="--self-test"){selfTest();return failures?1:0;}
    if(arg=="--packed-test"){packedTest();return failures?1:0;}
    if(arg=="--packed-pole"){packedTest(TankGeometry::eight,FitDomain::poleContour);return failures?1:0;}
    if(arg=="--automatic-test"){automaticTest();return failures?1:0;}
    if(arg=="--automatic-functional"){automaticTest(false);return failures?1:0;}
    if(arg=="--window-test"){windowTest();return failures?1:0;}
    if(arg=="--impulse-pole-row"&&argc==3){int index=-1;const std::string_view value(argv[2]);const auto parsed=std::from_chars(value.data(),value.data()+value.size(),index);
        if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())return 2;return impulseRow(index);}
    if(arg=="--packed-sixteen"){packedTest(TankGeometry::sixteen);return failures?1:0;}
    if(arg=="--decay-smoke")return decay(true);
    if(arg=="--decay-smoke-no-diffusion")return decay(true,false);
    if(arg=="--decay-bandwidth6")return decay(true,true,6);
    if(arg=="--decay-sixteen")return decay(true,true,24,TankGeometry::sixteen);
    if(arg=="--cpu-smoke")return cpuSmoke();
    if(arg=="--cpu-sixteen")return cpuSmoke(TankGeometry::sixteen);
    if(arg=="--calibrate-beating"){
        auto c=flatForCorpus();c.decaySeconds=.5;
        for(double delta:{1.,3.,10.})for(double sign:{-1.,1.}){
            const auto data=captureSource([&](size_t n){const double t=double(n)/48000;
                const double y=.5*(std::sin(2*std::numbers::pi*(1000-delta)*t)+sign*std::sin(2*std::numbers::pi*(1000+delta)*t))*std::exp(-std::log(1000.)*t/.5);
                return std::array<double,2>{y,y};},c,48000,0);
            const auto& m=data.bands[3];std::cout<<"BEAT_CAL modal_T60=.5 centre=1000 delta="<<delta<<" residue_sign="<<sign<<" T20="<<m.t20.time<<" T30="<<m.t30.time<<" r20="<<m.t20.r2<<" r30="<<m.t30.r2<<'\n';
        }return 0;
    }
    if(arg=="--calibrate-noise"||arg=="--calibrate-noise6"){
        for(double time:{.1,.5,1.3055117,2.}){
            auto c=flatForCorpus();c.decaySeconds=time;std::array<unsigned,2> state{0x5eed1234,0xabc123fe};
            const auto data=captureSource([&](size_t n){const double t=double(n)/48000;std::array<double,2> sample;
                for(size_t ch=0;ch<2;++ch){auto& s=state[ch];s^=s<<13;s^=s>>17;s^=s<<5;sample[ch]=(double(s)/4294967296.-.5)*std::exp(-std::log(1000.)*t/time);}return sample;},c,48000,0,arg=="--calibrate-noise6"?6:24);
            for(size_t b=0;b<7;++b){const auto& m=data.bands[b];std::cout<<"NOISE_CAL target="<<time<<" f="<<frequencies[b]<<" T20="<<m.t20.time<<" T30="<<m.t30.time<<" r20="<<m.t20.r2<<" r30="<<m.t30.r2<<'\n';}
        }return 0;
    }
    if(arg=="--calibrate-short"){
        auto c=flatForCorpus();c.bands[0]={true,false,DecayShape::bell,1000,.25,1};
        const std::array<double,3> fs{125,500,1000};
        const std::array<double,3> times{evaluate(c,125,48000)->seconds,evaluate(c,500,48000)->seconds,evaluate(c,1000,48000)->seconds};
        const auto data=captureSource([&](size_t n){const double t=double(n)/48000;double y=0;
            for(size_t k=0;k<3;++k)y+=.1*std::sin(2*std::numbers::pi*fs[k]*t)*std::exp(-std::log(1000.)*t/times[k]);
            return std::array<double,2>{y,y};},c,48000,0);
        for(size_t band:{size_t(2),size_t(3)}){const auto& m=data.bands[band];
            std::cout<<"SHORT_CAL f="<<frequencies[band]<<" target="<<m.target<<" T20="<<m.t20.time<<" T30="<<m.t30.time<<" r20="<<m.t20.r2<<" r30="<<m.t30.r2<<'\n';
            CHECK(m.t20.valid&&std::abs(m.t20.time/m.target-1)<=.15);CHECK(m.t30.valid&&std::abs(m.t30.time/m.target-1)<=.15);}
        return failures?1:0;
    }
    if(arg=="--decay")return decay(false);
    return 2;
  }catch(const PeerObserved& peer){std::cout<<"CONFIG_INTERRUPTED "<<peer.reason<<'\n';return 3;}
}


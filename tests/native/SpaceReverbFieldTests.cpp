#include "SpaceReverbField.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;int checks=0,failures=0;}
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t alignment){if(tracking)++allocations;const auto a=std::size_t(alignment);void* p=nullptr;
#if defined(_MSC_VER)
 p=_aligned_malloc(n?n:1,a);
#else
 p=std::aligned_alloc(a,((n?n:1)+a-1)/a*a);
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
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<"FAIL "<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;
using Stereo=std::array<double,2>;
void ready(const Settings& s,double rate,PreparedField& p,FieldRuntime& r) {
    CHECK(prepareField(s,rate,p));CHECK(r.prepareStorage(rate));allocations=frees=0;tracking=true;const bool configured=r.configure(p);tracking=false;CHECK(configured);CHECK(allocations==0&&frees==0);
}
Settings stationary() {Settings s;s.predelayMs=0;s.character=0;s.distance=1;s.width=1;s.mix=1;return s;}
void validation() {
    PreparedField p;Settings s;CHECK(prepareField(s,48000,p));const auto saved=p;
    for(double rate:{0.,7999.,768001.,std::numeric_limits<double>::quiet_NaN()}){CHECK(!prepareField(s,rate,p));CHECK(p==saved);}
    for(int bad=0;bad<3;++bad){auto c=s;if(bad==0)c.mix=2;if(bad==1)c.decay.bands[5].q=0;if(bad==2)c.post[5].shelfQ=5;CHECK(!prepareField(c,48000,p));CHECK(p==saved);}
    FieldRuntime empty;CHECK((empty.tick({1,1})==Stereo{}));CHECK(!empty.configure(p));empty.reset();
    allocations=frees=0;tracking=true;auto* known=::operator new(32,std::align_val_t(32));::operator delete(known,std::align_val_t(32));tracking=false;CHECK(allocations==1&&frees==1);
    for(double rate:{8000.,16000.,44100.,48000.,88200.,96000.,176400.,192000.,384000.,385000.,768000.}){PreparedPostEq eq;PreparedDuck duck;CHECK(preparePostEq(s,rate,eq));CHECK(prepareDuck(s,rate,duck));}
}
void exactTankAndReset() {
    for(double rate:{44100.,48000.,96000.}){
        auto s=stationary();PreparedField p;FieldRuntime field;ready(s,rate,p,field);detail::TankRuntime tank;
        CHECK(tank.prepareStorage(rate));CHECK(tank.configure(p.tank));
        bool same=true,nonzero=false,finite=true;allocations=frees=0;tracking=true;
        for(int trial=0;trial<2;++trial){for(int n=0;n<30000;++n){const Stereo input{n==0?.5:0,n==0?.25:0};const auto a=field.tick(input),b=tank.tick(input);if(a!=b)same=false;for(double x:a){nonzero|=x!=0;finite&=std::isfinite(x);}}
            field.reset();tank.reset();}
        tracking=false;CHECK(same);CHECK(nonzero);CHECK(finite);CHECK(allocations==0&&frees==0);CHECK(field.storageBytes()<=16*1024*1024);
    }
}
void earlyReflections() {
    auto s=stationary();s.distance=0;PreparedField p;FieldRuntime a,b;ready(s,48000,p,a);CHECK(b.prepareStorage(48000));CHECK(b.configure(p));
    bool exact=true;allocations=frees=0;tracking=true;
    for(int n=0;n<1000;++n){const auto left=a.tick({n==0?1.:0,0}),right=b.tick({0,n==0?1.:0});Stereo expectedL{},expectedR{};
        // All declared taps precede the first24.145ms late-tank pickup at this Size.
        if(n==350)expectedL[0]=.2016;if(n==351)expectedL[0]=.1344;
        if(n==859)expectedL[0]=.1152;if(n==860)expectedL[0]=.0288;
        if(n==657)expectedL[1]=.08;if(n==658)expectedL[1]=.12;
        if(n==427)expectedR[1]=.2688;if(n==428)expectedR[1]=.0672;
        if(n==926)expectedR[1]=.0864;if(n==927)expectedR[1]=.0576;
        if(n==532)expectedR[0]=.04;if(n==533)expectedR[0]=.16;
        for(std::size_t ch=0;ch<2;++ch)if(std::abs(left[ch]-expectedL[ch])>1e-11||std::abs(right[ch]-expectedR[ch])>1e-11)exact=false;
    }
    tracking=false;CHECK(exact);CHECK(allocations==0&&frees==0);
}
void distanceAndTransactions() {
    auto s=stationary();s.distance=0;PreparedField p0,p1,half;FieldRuntime a,b,c;
    ready(s,48000,p0,a);s.distance=1;ready(s,48000,p1,b);s.distance=.5;ready(s,48000,half,c);
    bool blend=true;allocations=frees=0;tracking=true;
    for(int n=0;n<30000;++n){const Stereo x{n==0?.5:0,n==0?.25:0};const auto early=a.tick(x),late=b.tick(x),mixed=c.tick(x);
        for(std::size_t ch=0;ch<2;++ch)if(std::abs(mixed[ch]-(.5*early[ch]+.5*late[ch]))>1e-12)blend=false;}
    tracking=false;CHECK(blend);CHECK(allocations==0&&frees==0);
    FieldRuntime control;CHECK(control.prepareStorage(48000));CHECK(control.configure(half));CHECK(c.configure(half));
    for(int n=0;n<2000;++n){const Stereo x{n==0?.5:0,n==0?.25:0};c.tick(x);control.tick(x);}
    auto bad=half;bad.post.stages[0].a2=2;bad.post.count=1;bad.post.counts[0]=1;for(std::size_t i=1;i<6;++i)bad.post.starts[i]=1;
    allocations=frees=0;tracking=true;const bool rejected=!c.configure(bad);bool preserved=true;
    for(int n=0;n<10000;++n)if(c.tick({0,0})!=control.tick({0,0}))preserved=false;tracking=false;
    CHECK(rejected);CHECK(preserved);CHECK(c.prepared()==half);CHECK(allocations==0&&frees==0);
    CHECK(!c.prepareStorage(0));CHECK(c.prepared()==half);
}
void predelayAndPartitions() {
    constexpr int frames=20000;const auto s=stationary();PreparedField p;FieldRuntime base;ready(s,48000,p,base);
    std::vector<Stereo> expected(frames);for(int n=0;n<frames;++n)expected[std::size_t(n)]=base.tick({n==0?.5:0,n==0?.25:0});
    for(double delay:{0.,.5,480.,12000.}){
        auto settings=s;settings.predelayMs=delay/48;PreparedField q;FieldRuntime a,b;ready(settings,48000,q,a);CHECK(b.prepareStorage(48000));CHECK(b.configure(q));
        std::vector<Stereo> first(frames),second(frames);bool correct=true;allocations=frees=0;tracking=true;
        for(int start=0;start<frames;start+=64)for(int n=start;n<std::min(start+64,frames);++n)first[std::size_t(n)]=a.tick({n==0?.5:0,n==0?.25:0});
        for(int start=0;start<frames;start+=512)for(int n=start;n<std::min(start+512,frames);++n)second[std::size_t(n)]=b.tick({n==0?.5:0,n==0?.25:0});
        for(int n=0;n<frames;++n){const int whole=int(std::floor(delay));const double fraction=delay-whole;
            for(std::size_t ch=0;ch<2;++ch){const double now=n>=whole?expected[std::size_t(n-whole)][ch]:0,prior=n>whole?expected[std::size_t(n-whole-1)][ch]:0;
                if(std::abs(first[std::size_t(n)][ch]-((1-fraction)*now+fraction*prior))>1e-11)correct=false;}}
        a.reset();b.reset();tracking=false;CHECK(correct);CHECK(first==second);CHECK(allocations==0&&frees==0);
    }
}
void widthMixAndPost() {
    auto s=stationary();PreparedField p;FieldRuntime base;ready(s,48000,p,base);std::vector<Stereo> wet(20000),input(20000);
    for(std::size_t n=0;n<input.size();++n){input[n]={n<1000?.001*std::sin(n*.13):0,n<1000?.0007*std::cos(n*.17):0};wet[n]=base.tick(input[n]);}
    for(double width:{0.,1.,2.})for(double mix:{0.,.25,1.}){
        auto settings=s;settings.width=width;settings.mix=mix;PreparedField q;FieldRuntime runtime;ready(settings,48000,q,runtime);bool same=true,mono=true;allocations=frees=0;tracking=true;
        for(std::size_t n=0;n<input.size();++n){auto expected=wet[n];if(width!=1){const double mid=(expected[0]+expected[1])*.5,side=(expected[0]-expected[1])*.5;expected={mid+width*side,mid-width*side};}
            const auto actual=runtime.tick(input[n]);if(width==0&&mix==1&&actual[0]!=actual[1])mono=false;
            for(std::size_t ch=0;ch<2;++ch){const double value=mix==0?input[n][ch]:mix==1?expected[ch]:(1-mix)*input[n][ch]+mix*expected[ch];if(std::abs(actual[ch]-value)>1e-12)same=false;}}
        tracking=false;CHECK(same);CHECK(mono);CHECK(allocations==0&&frees==0);
    }
    auto settings=s;settings.post[0]={true,false,PostShape::bell,1000,18,4,1,CutSlope::db12};settings.post[5]={true,false,PostShape::highCut,7000,0,1,1,CutSlope::db24};
    PreparedField q;FieldRuntime runtime;ready(settings,48000,q,runtime);CHECK(q.tank==p.tank);PostEq eq;CHECK(eq.prepare(q.post));bool same=true;
    allocations=frees=0;tracking=true;for(std::size_t n=0;n<input.size();++n){const auto expected=eq.tick(wet[n]),actual=runtime.tick(input[n]);for(std::size_t ch=0;ch<2;++ch)if(std::abs(actual[ch]-expected[ch])>1e-12)same=false;}tracking=false;
    CHECK(same);CHECK(allocations==0&&frees==0);
    // Dry bit patterns remain exact even through intentional wet gain changes.
    settings.mix=0;ready(settings,48000,q,runtime);bool dry=true;
    for(Stereo x:std::array<Stereo,3>{{{-0.,.5},{1e-40,-.75},{.001,-0.}}}){const auto y=runtime.tick(x);for(std::size_t ch=0;ch<2;++ch)if(std::bit_cast<std::uint64_t>(x[ch])!=std::bit_cast<std::uint64_t>(y[ch]))dry=false;}CHECK(dry);
}
void duckPlacement() {
    auto s=stationary();s.predelayMs=250;s.width=2;s.mix=.25;PreparedField p;FieldRuntime base;ready(s,48000,p,base);
    s.duckRangeDb=12;s.duckThresholdDb=-24;s.duckAttackMs=10;s.duckReleaseMs=250;PreparedField q;FieldRuntime ducked;ready(s,48000,q,ducked);
    double env=0;const double attack=std::exp(-1./480),release=std::exp(-1./12000);bool same=true,envelope=true;allocations=frees=0;tracking=true;
    for(int n=0;n<60000;++n){const Stereo x{n<480?1.:n>=20000&&n<20480?.5:0,n>=40000&&n<40480?1.:0};const auto a=base.tick(x),b=ducked.tick(x);
        const double peak=std::max(std::abs(x[0]),std::abs(x[1])),coefficient=peak>env?attack:release;env=coefficient*env+(1-coefficient)*peak;
        const double attenuation=std::min(12.,std::max(0.,20*std::log10(std::max(env,1e-15))+24)),gain=std::pow(10.,-attenuation/20);
        for(std::size_t ch=0;ch<2;++ch){const double expected=.75*x[ch]+gain*(a[ch]-.75*x[ch]);if(std::abs(b[ch]-expected)>1e-11)same=false;}
        if(std::abs(ducked.duckEnvelope()-env)>1e-12)envelope=false;
    }
    tracking=false;CHECK(same);CHECK(envelope);CHECK(allocations==0&&frees==0);
}
void extremeFields() {
    for(double rate:{44100.,48000.,96000.})for(bool cut:{false,true}){
        auto s=stationary();s.decay.decaySeconds=20;s.decay.brightness=1;s.character=1;s.distance=.5;s.width=2;s.predelayMs=250;
        for(std::size_t i=0;i<6;++i){s.decay.bands[i]={true,false,DecayShape::bell,125*std::pow(2.,double(i)),i%2?4.:.25,4};
            s.post[i]={true,false,cut?PostShape::lowCut:PostShape::bell,cut?20.:1000.,18,12,4,CutSlope::db24};}
        PreparedField p;FieldRuntime runtime;ready(s,rate,p,runtime);bool finite=true,nonzero=false;double peak=0;
        allocations=frees=0;tracking=true;for(int n=0;n<int(rate);++n){const auto value=runtime.tick({n==0?.5:0,n==0?.25:0});for(double x:value){finite&=std::isfinite(x);nonzero|=x!=0;peak=std::max(peak,std::abs(x));}}runtime.reset();tracking=false;
        CHECK(finite&&nonzero&&peak<1e12);CHECK(allocations==0&&frees==0);CHECK(p.tail.passed&&p.tail.seconds>0&&std::isfinite(p.tail.seconds));
    }
}
void characterAndTail() {
    for(double character:{0.,.25,1.})for(const auto phase:std::array<std::array<double,4>,3>{{{0,0,0,0},{1,-1,1,-1},{.707,-.3,.8,-.2}}}){
        std::array<double,8> value{1,2,3,4,-1,-2,-3,-4};const auto original=value;double before=0,after=0;for(double x:value)before+=x*x;
        detail::rotateSpaceFeedback(value,character,phase);for(double x:value)after+=x*x;CHECK(std::abs(before-after)<1e-11);if(character==0)CHECK(value==original);if(character>0&&phase[0]!=0)CHECK(value!=original);
    }
    const auto literal=detail::makeSpaceTailBound(0,48000,2);CHECK(literal.passed);CHECK(std::abs(literal.seconds-5.381020833333333)<1e-10);
    const auto doubled=detail::makeSpaceTailBound(std::log(2.),48000,2);CHECK(doubled.passed);CHECK(std::abs(doubled.seconds-5.581708333333333)<1e-10);
    const auto high=detail::makeSpaceTailBound(0,768000,46);CHECK(high.passed);CHECK(std::abs(high.seconds-143.43399869791668)<1e-8);
    PreparedPostEq eq;CHECK(preparePostEq(stationary(),48000,eq));auto bound=boundPostEq(eq,std::exp(-std::log(1000.)/(48000*46)));CHECK(bound.passed&&bound.logMagnitude==0);
    eq.count=1;eq.counts[0]=1;for(std::size_t i=1;i<6;++i)eq.starts[i]=1;eq.stages[0]={2,0,0,0,0};bound=boundPostEq(eq,.99);CHECK(bound.passed);CHECK(bound.logMagnitude>=std::log(2.)&&bound.logMagnitude<std::log(2.)+1e-6);
    // Exactly representable repeated pole: weighted DC norm is16*r*r/9.
    const double pole=1-std::ldexp(1.,-20),radius=1-std::ldexp(1.,-22);eq.stages[0]={std::ldexp(1.,-40),0,0,-2*pole,pole*pole};
    bound=boundPostEq(eq,radius);const double exact=std::log(16*radius*radius/9);CHECK(bound.passed);CHECK(bound.logMagnitude>=exact&&bound.logMagnitude<exact+1e-6);
    CHECK(!boundPostEq(eq,.9).passed);
    auto s=stationary();PreparedField p,q;FieldRuntime a,b;ready(s,48000,p,a);s.character=1;ready(s,48000,q,b);bool different=false,finite=true;double maximum=0;
    allocations=frees=0;tracking=true;for(int n=0;n<100000;++n){const Stereo in{n==0?.5:0,n==0?.25:0};const auto first=a.tick(in),second=b.tick(in);if(first!=second)different=true;for(double x:second){finite&=std::isfinite(x);maximum=std::max(maximum,std::abs(x));}}a.reset();b.reset();tracking=false;
    CHECK(different);CHECK(finite&&maximum<16);CHECK(allocations==0&&frees==0);CHECK(p.tail.passed&&q.tail.passed&&p.tail.seconds>0&&std::isfinite(q.tail.seconds));
}
}
int main(){validation();exactTankAndReset();earlyReflections();distanceAndTransactions();predelayAndPartitions();widthMixAndPost();duckPlacement();extremeFields();characterAndTail();std::cout<<"SpaceReverbField checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

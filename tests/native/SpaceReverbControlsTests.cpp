#include "SpaceReverbControls.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
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
constexpr double nan=std::numeric_limits<double>::quiet_NaN(),infinity=std::numeric_limits<double>::infinity();
bool near(double a,double b,double tolerance=1e-6){return std::isfinite(a)&&std::abs(a-b)<=tolerance;}
double db(const PreparedPostEq& p,double f,std::optional<std::size_t> band=std::nullopt){const auto h=postEqResponse(p,f,band);return h?20*std::log10(std::abs(*h)):nan;}
void validation()
{
    Settings s;CHECK(valid(s));CHECK(s.decay.decaySeconds==2&&s.decay.brightness==.5&&s.size==.5&&s.predelayMs==20);
    CHECK(s.character==.25&&s.distance==.5&&s.width==1&&s.mix==.25&&s.duckRangeDb==0);
    CHECK(s.duckThresholdDb==-24&&s.duckAttackMs==10&&s.duckReleaseMs==250);
    struct Field {double Settings::*member;double low,high;};
    for(const auto f:{Field{&Settings::size,0,1},{&Settings::predelayMs,0,250},{&Settings::character,0,1},
        {&Settings::distance,0,1},{&Settings::width,0,2},{&Settings::mix,0,1},{&Settings::duckRangeDb,0,24},
        {&Settings::duckThresholdDb,-60,0},{&Settings::duckAttackMs,.1,200},{&Settings::duckReleaseMs,10,2000}}){
        for(double value:{f.low,f.high}){auto x=s;x.*f.member=value;CHECK(valid(x));}
        for(double value:{std::nextafter(f.low,-infinity),std::nextafter(f.high,infinity),nan,infinity}){auto x=s;x.*f.member=value;CHECK(!valid(x));}
    }
    struct BandField {double PostBand::*member;double low,high;};
    for(std::size_t index=0;index<6;++index){
        for(const auto f:{BandField{&PostBand::frequencyHz,20,20000},{&PostBand::gainDb,-18,18},{&PostBand::bellQ,.25,12},{&PostBand::shelfQ,.25,4}}){
            for(double value:{f.low,f.high}){auto x=s;x.post[index].*f.member=value;CHECK(valid(x));}
            for(double value:{std::nextafter(f.low,-infinity),std::nextafter(f.high,infinity),nan,infinity}){auto x=s;x.post[index].*f.member=value;CHECK(!valid(x));}
        }
        auto x=s;x.post[index].shape=static_cast<PostShape>(99);CHECK(!valid(x));
        x=s;x.post[index].slope=static_cast<CutSlope>(99);CHECK(!valid(x));
        x=s;x.decay.bands[index].frequencyHz=nan;CHECK(!valid(x));
    }
    s.post[0].bellQ=12;s.post[0].shelfQ=4;
    for(auto shape:{PostShape::bell,PostShape::lowShelf,PostShape::highShelf,PostShape::lowCut,PostShape::highCut}){
        s.post[0].shape=shape;s.post[0].active=true;const auto before=s;PreparedPostEq p;CHECK(preparePostEq(s,48000,p));CHECK(s==before);
        CHECK(s.post[0].bellQ==12&&s.post[0].shelfQ==4);
    }
    PreparedPostEq p;CHECK(preparePostEq(s,48000,p));const auto saved=p;
    for(double rate:{0.,7999.,768001.,nan,infinity}){CHECK(!preparePostEq(s,rate,p));CHECK(p==saved);}
    auto bad=s;bad.post[5].gainDb=nan;CHECK(!preparePostEq(bad,48000,p));CHECK(p==saved);
    PreparedDuck d;CHECK(prepareDuck(s,48000,d));const auto savedDuck=d;
    bad=s;bad.duckReleaseMs=2001;CHECK(!prepareDuck(bad,48000,d));CHECK(d==savedDuck);
    CHECK(!prepareDuck(s,nan,d));CHECK(d==savedDuck);
}
void responses()
{
    for(double rate:{44100.,48000.,96000.}){
        for(double f:{20.,1000.,std::min(20000.,.45*rate)})for(double gain:{-18.,-6.,6.,18.}){
            Settings s;s.post[0].active=true;s.post[0].frequencyHz=f;s.post[0].gainDb=gain;
            PreparedPostEq p;CHECK(preparePostEq(s,rate,p));CHECK(p.count==1);CHECK(near(db(p,f),gain));CHECK(near(db(p,f,0),gain));
            CHECK(near(db(p,f,1),0));
        }
        for(auto shape:{PostShape::lowShelf,PostShape::highShelf})for(double q:{.25,1.,4.})for(double gain:{-18.,-6.,6.,18.}){
            Settings s;s.post[0].active=true;s.post[0].shape=shape;s.post[0].gainDb=gain;s.post[0].shelfQ=q;
            PreparedPostEq p;CHECK(preparePostEq(s,rate,p));CHECK(near(db(p,0),shape==PostShape::lowShelf?gain:0));
            CHECK(near(db(p,rate*.5),shape==PostShape::highShelf?gain:0));
        }
        for(auto shape:{PostShape::lowCut,PostShape::highCut})for(auto slope:{CutSlope::db12,CutSlope::db24}){
            Settings s;s.post[0]={true,false,shape,1000,18,12,4,slope};PreparedPostEq p;CHECK(preparePostEq(s,rate,p));
            CHECK(p.count==(slope==CutSlope::db12?1:2));CHECK(near(db(p,1000),-3.010299956639812));
            const int order=slope==CutSlope::db12?2:4;
            for(double f:{500.,2000.}){const double ratio=std::tan(std::numbers::pi*f/rate)/std::tan(std::numbers::pi*1000/rate);
                const double expected=-10*std::log10(1+std::pow(shape==PostShape::highCut?ratio:1/ratio,2*order));
                CHECK(near(db(p,f),expected,1e-5));}
            auto other=s;other.post[0].gainDb=-18;other.post[0].bellQ=.25;other.post[0].shelfQ=.25;PreparedPostEq b;
            CHECK(preparePostEq(other,rate,b));CHECK(near(db(p,500),db(b,500),1e-12));
        }
        Settings s;PreparedPostEq p;CHECK(preparePostEq(s,rate,p));CHECK(p.count==0);CHECK(!postEqResponse(p,-1));CHECK(!postEqResponse(p,nan));CHECK(!postEqResponse(p,rate));CHECK(!postEqResponse(p,1000,6));
        s.post[5].active=true;s.post[5].frequencyHz=20000;CHECK(preparePostEq(s,rate,p));CHECK(p.effectiveHz[5]==std::min(20000.,.45*rate));
    }
}
void identities()
{
    for(double rate:{44100.,48000.,96000.}){
        for(std::size_t index=0;index<6;++index){Settings s;s.post[index].active=true;s.post[index].gainDb=6;s.post[index].frequencyHz=250*std::pow(2.,double(index));
            PreparedPostEq p;CHECK(preparePostEq(s,rate,p));CHECK(p.count==1);CHECK(near(db(p,s.post[index].frequencyHz),6));
            for(std::size_t band=0;band<6;++band)CHECK(near(db(p,s.post[index].frequencyHz,band),band==index?6:0));
        }
        Settings s;for(auto& b:s.post){b.active=true;b.gainDb=6;}PreparedPostEq p;
        CHECK(preparePostEq(s,rate,p));CHECK(p.count==6);CHECK(near(db(p,1000),36));
        for(std::size_t band=0;band<6;++band)CHECK(near(db(p,1000,band),6));
        for(std::size_t band=1;band<6;band+=2)s.post[band].bypassed=true;
        CHECK(preparePostEq(s,rate,p));CHECK(p.count==3);CHECK(near(db(p,1000),18));
    }
}
void rendering()
{
    allocations=frees=0;tracking=true;auto* known=::operator new(32,std::align_val_t(32));::operator delete(known,std::align_val_t(32));tracking=false;CHECK(allocations==1&&frees==1);
    constexpr std::size_t frames=8192;std::vector<std::array<double,2>> input(frames),first(frames),second(frames);unsigned random=0x5eed1234;
    for(auto& frame:input)for(auto& channel:frame){random^=random<<13;random^=random>>17;random^=random<<5;channel=(double(random)/4294967296.-.5)*.001;}
    Settings s;s.post[0].active=true;s.post[0].gainDb=6;PreparedPostEq p;CHECK(preparePostEq(s,48000,p));PostEq a,b;CHECK(a.prepare(p));CHECK(b.prepare(p));
    struct Reference {double x1=0,x2=0,y1=0,y2=0;};std::array<std::array<Reference,12>,2> reference{};
    bool referenceSame=true;double maximumError=0;allocations=frees=0;tracking=true;
    for(std::size_t start=0;start<frames;start+=64)for(std::size_t i=start;i<std::min(frames,start+64);++i)first[i]=a.tick(input[i]);
    for(std::size_t start=0;start<frames;start+=512)for(std::size_t i=start;i<std::min(frames,start+512);++i)second[i]=b.tick(input[i]);
    for(std::size_t i=0;i<frames;++i)for(std::size_t channel=0;channel<2;++channel){double x=input[i][channel];
        for(std::size_t j=0;j<p.count;++j){auto& h=reference[channel][j];const auto& c=p.stages[j];const double y=c.b0*x+c.b1*h.x1+c.b2*h.x2-c.a1*h.y1-c.a2*h.y2;h.x2=h.x1;h.x1=x;h.y2=h.y1;h.y1=y;x=y;}
        maximumError=std::max(maximumError,std::abs(x-first[i][channel]));if(std::abs(x-first[i][channel])>1e-12)referenceSame=false;
    }
    tracking=false;CHECK(first==second);CHECK(a==b);CHECK(referenceSame);CHECK(allocations==0&&frees==0);
    a.reset();PostEq fresh;CHECK(fresh.prepare(p));CHECK(a==fresh);
    auto before=a;auto invalid=p;invalid.count=13;CHECK(!a.prepare(invalid));CHECK(a==before);
    CHECK(a.prepare(p));before=a;invalid=p;invalid.count=1;invalid.counts[0]=1;for(std::size_t j=1;j<6;++j)invalid.starts[j]=1;
    invalid.stages[0].a2=2;CHECK(!a.prepare(invalid));CHECK(a==before);
    for(auto shape:{PostShape::bell,PostShape::lowShelf,PostShape::highShelf}){
        Settings zero;for(auto& band:zero.post){band.active=true;band.shape=shape;band.gainDb=0;}
        PreparedPostEq unity;CHECK(preparePostEq(zero,48000,unity));CHECK(unity.count==0);PostEq eq;CHECK(eq.prepare(unity));bool exact=true;
        allocations=frees=0;tracking=true;for(const auto x:input)if(eq.tick(x)!=x)exact=false;const std::array<double,2> signedZero{-0.,0.};const auto out=eq.tick(signedZero);tracking=false;
        CHECK(exact);CHECK(std::bit_cast<std::uint64_t>(out[0])==std::bit_cast<std::uint64_t>(signedZero[0]));CHECK(allocations==0&&frees==0);
        zero.post[0].gainDb=18;zero.post[0].bypassed=true;CHECK(preparePostEq(zero,48000,unity));CHECK(unity.count==0);
    }
    PostEq left,swap;CHECK(left.prepare(p));CHECK(swap.prepare(p));bool independent=true;
    for(auto x:input){const auto l=left.tick({x[0],0}),r=swap.tick({0,x[0]});if(l[0]!=r[1]||l[1]!=0||r[0]!=0)independent=false;}CHECK(independent);
    for(double rate:{44100.,48000.,96000.})for(auto shape:{PostShape::bell,PostShape::lowShelf,PostShape::highShelf,PostShape::lowCut,PostShape::highCut}){
        Settings extreme;for(auto& band:extreme.post)band={true,false,shape,20,18,12,4,CutSlope::db24};PreparedPostEq ready;
        CHECK(preparePostEq(extreme,rate,ready));PostEq eq;CHECK(eq.prepare(ready));bool finite=true;allocations=frees=0;tracking=true;
        for(auto x:input){const auto y=eq.tick(x);if(!std::isfinite(y[0])||!std::isfinite(y[1]))finite=false;}eq.reset();tracking=false;
        CHECK(finite);CHECK(allocations==0&&frees==0);
    }
    std::cout<<"POST_REFERENCE max_abs_error="<<maximumError<<'\n';
}
void ducking()
{
    for(double rate:{44100.,48000.,96000.}){
        Settings s;s.duckRangeDb=12;PreparedDuck p;CHECK(prepareDuck(s,rate,p));DuckDetector d;CHECK(d.prepare(p));
        const int attackSamples=int(rate*.01),releaseSamples=int(rate*.25);double gain=1;
        for(int n=0;n<attackSamples;++n)gain=d.tick({1,.25});CHECK(near(d.envelope(),.6321205588285577,1e-10));
        CHECK(near(d.attenuationDb(),12,1e-12));CHECK(near(gain,.251188643150958,1e-12));
        for(int n=0;n<int(rate);++n)d.tick({1,0});const auto peak=d.envelope();for(int n=0;n<releaseSamples;++n)d.tick({0,0});
        CHECK(near(d.envelope()/peak,.36787944117144233,1e-10));
        for(int n=0;n<8*releaseSamples;++n)gain=d.tick({0,0});CHECK(gain==1);CHECK(d.attenuationDb()==0);
        d.reset();CHECK(d.envelope()==0&&d.attenuationDb()==0&&d.tick({0,0})==1);
        s.duckRangeDb=0;CHECK(prepareDuck(s,rate,p));CHECK(d.prepare(p));bool unity=true;for(int n=0;n<attackSamples;++n)if(d.tick({.25,1})!=1)unity=false;CHECK(unity);CHECK(d.envelope()>.6);
    }
    Settings s;s.duckRangeDb=24;s.duckThresholdDb=-6;PreparedDuck p;CHECK(prepareDuck(s,48000,p));DuckDetector first,second;CHECK(first.prepare(p));CHECK(second.prepare(p));
    bool linked=true;double gain=1;allocations=frees=0;tracking=true;
    for(int n=0;n<480;++n){gain=first.tick({1,.25});if(gain!=second.tick({.25,1})||first.envelope()!=second.envelope())linked=false;}
    tracking=false;CHECK(linked);CHECK(allocations==0&&frees==0);
    const double expectedAttenuation=20*std::log10(.6321205588285577)+6;CHECK(near(first.attenuationDb(),expectedAttenuation,1e-10));CHECK(near(gain,std::pow(10.,-expectedAttenuation/20),1e-10));
    const auto before=first;auto bad=p;bad.attack=1;CHECK(!first.prepare(bad));CHECK(first==before);
    bad=p;bad.release=nan;CHECK(!first.prepare(bad));CHECK(first==before);
}
}
int main(){validation();responses();identities();rendering();ducking();std::cout<<"SpaceReverbControls checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

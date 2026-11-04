#include "ModalTank.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <type_traits>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;unsigned checks=0,failures=0;}
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t a){if(tracking)++allocations;
#if defined(_MSC_VER)
    auto* p=_aligned_malloc(n?n:1,std::size_t(a));
#else
    auto* p=std::aligned_alloc(std::size_t(a),((n?n:1)+std::size_t(a)-1)/std::size_t(a)*std::size_t(a));
#endif
    if(p)return p;throw std::bad_alloc();
}
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<50)std::cerr<<"DIRECT_MODAL_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;
constexpr double rootHalf=.7071067811865475244;
bool near(double a,double b,double tolerance=1e-11){return std::abs(a-b)<=tolerance*std::max({1.,std::abs(a),std::abs(b)});}
DecayCurve shaped(){DecayCurve c;c.brightness=1;for(auto& b:c.bands)b={true,false,DecayShape::bell,20,.25,4};return c;}
void validation(){
    static_assert(std::is_trivially_copyable_v<PreparedModal>);
    auto p=std::make_unique<PreparedModal>();CHECK(prepareModal({},.5,48000,1024,*p));auto saved=std::make_unique<PreparedModal>(*p);
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    for(double rate:{0.,7999.,768001.,nan}){CHECK(!prepareModal({},.5,rate,1024,*p));CHECK(*p==*saved);}
    for(double size:{-1.,1.1,nan}){CHECK(!prepareModal({},size,48000,1024,*p));CHECK(*p==*saved);}
    for(auto count:{0U,4U,1023U,2049U}){CHECK(!prepareModal({},.5,48000,count,*p));CHECK(*p==*saved);}
    auto bad=DecayCurve{};bad.bands[5].q=0;CHECK(!prepareModal(bad,.5,48000,1024,*p));CHECK(*p==*saved);
    unsigned calls=0;const auto cancelled=[](void* context) noexcept {return ++*static_cast<unsigned*>(context)>2;};
    CHECK(!prepareModal({},.5,48000,1024,*p,{cancelled,&calls}));CHECK(*p==*saved);CHECK(calls==3);
    ModalTank empty;std::array<ModalTank::Frame,1> input{{{.5,.25}}},original=input;
    CHECK(!empty.processBlock(input));CHECK(input==original);CHECK((empty.tick({1,1})==ModalTank::Frame{}));
    CHECK(!empty.configure(*saved));CHECK(!empty.prepareStorage(0));empty.reset();
    tracking=true;auto* known=::operator new(32,std::align_val_t(64));::operator delete(known,std::align_val_t(64));tracking=false;CHECK(allocations==1&&frees==1);
}
void poles(){
    auto p=std::make_unique<PreparedModal>();
    auto shortest=DecayCurve{};shortest.decaySeconds=.1;shortest.brightness=1;
    auto longest=DecayCurve{};longest.decaySeconds=20;longest.brightness=1;for(auto& b:longest.bands)b={true,false,DecayShape::highShelf,20,4,.25};
    auto mixed=DecayCurve{};for(std::size_t k=0;k<6;++k)mixed.bands[k]={true,false,static_cast<DecayShape>(k%3),std::array<double,6>{53.,167.,713.,1807.,5001.,16000.}[k],k%2?4.:.25,.25+.75*k};
    for(const auto& curve:{shaped(),shortest,longest,mixed})for(auto count:{1024U,2048U})for(double rate:{8000.,44100.,48000.,96000.,768000.})for(double size:{0.,.5,1.}){
        const bool prepared=prepareModal(curve,size,rate,count,*p);CHECK(prepared);CHECK(p->count==count&&p->sampleRate==rate&&p->size==size&&p->curve==curve);
        bool exact=prepared,inside=prepared,injected=prepared,covered=prepared;double weighted=0;std::size_t factors=0;
        for(std::size_t k=0;k<p->count;++k){
            const double rc=p->rc[k],rs=p->rs[k],radius=std::hypot(rc,rs),hz=std::atan2(rs,rc)*rate/(2*std::numbers::pi);
            const auto expected=evaluate(curve,std::clamp(hz,20.,std::min(20000.,.45*rate)),std::min(rate,384000.));
            const double time=-std::log(1000.)/(rate*std::log(radius));
            exact=exact&&expected&&near(hz,p->frequency[k],1e-10)&&near(p->target[k],expected->seconds,1e-12)&&std::abs(time/expected->seconds-1)<=1e-6;
            const std::complex<double> pole{rc,rs};const auto residual=pole*pole-2*rc*pole+(rc*rc+rs*rs);
            inside=inside&&radius>0&&radius<1&&hz>=20-1e-8&&hz<=std::min(20000.,.45*rate)+1e-8&&std::abs(residual)<1e-12;
            for(const auto root:{pole,std::conj(pole)}){inside=inside&&std::abs(root*root-2*rc*root+(rc*rc+rs*rs))<1e-12;++factors;}
            injected=injected&&near(p->bReal[k]*p->bReal[k]+p->bImag[k]*p->bImag[k],1.,1e-12);
            weighted=std::max(weighted,radius/std::exp(-std::log(1000.)/(rate*46)));
        }
        for(unsigned branch=0;branch<2;++branch)for(double anchor:{20.,std::min(20000.,.45*rate),125.,250.,500.,1000.,2000.,4000.,8000.}){
            anchor=std::clamp(anchor,20.,std::min(20000.,.45*rate));bool found=false;
            for(std::size_t k=0;k<p->count;++k)if((k/4)%2==branch&&p->frequency[k]==anchor)found=true;
            covered=covered&&found;
        }
        for(const auto& band:curve.bands)if(band.active&&!band.bypassed)for(unsigned branch=0;branch<2;++branch){
            const double anchor=std::min(band.frequencyHz,std::min(20000.,.45*rate));bool found=false;
            for(std::size_t k=0;k<p->count;++k)if((k/4)%2==branch&&p->frequency[k]==anchor)found=true;
            covered=covered&&found;
        }
        CHECK(exact);CHECK(inside);CHECK(injected);CHECK(covered);CHECK(weighted<1&&near(weighted,p->weightedRadius,1e-12));
        CHECK(factors==count*2); // Both conjugate factors of every literal2x2 block.
    }
    auto flat=DecayCurve{};flat.brightness=1;CHECK(prepareModal(flat,.5,48000,1024,*p));
    auto other=std::make_unique<PreparedModal>();CHECK(prepareModal(shaped(),.5,48000,1024,*other));
    CHECK(p->bReal==other->bReal&&p->bImag==other->bImag&&p->pickup==other->pickup);
    CHECK(near(p->pickup,std::sqrt(4./1024)*std::sqrt(-std::expm1(-std::log(1000.)/48000)),1e-12));
    bool rejects=p->count>0;for(std::size_t k=0;k<p->count;++k){const double time=-std::log(1000.)/(48000*std::log(std::hypot(p->rc[k],p->rs[k])));rejects=rejects&&std::abs(time/8-1)>.15;}
    CHECK(rejects); // A2s recursive bank cannot implement8s by changing wet level.
    ModalSample state{.3,-.4};const double r=std::exp(-std::log(1000.)/8000),angle=.17;
    allocations=frees=0;tracking=true;for(int n=0;n<8000;++n)advanceModal(state,r*std::cos(angle),r*std::sin(angle),0,0,0);tracking=false;
    CHECK(near(std::hypot(state.real,state.imag),.0005,1e-12));CHECK(allocations==0&&frees==0);
}
ModalTank::Frame reference(const PreparedModal& p,std::vector<ModalSample>& states,ModalTank::Frame input){
    ModalTank::Frame accum{};
    for(std::size_t group=0;group<p.count/4;++group){
        const double excitation=input[0]*p.inputWeights[group][0]+input[1]*p.inputWeights[group][1];double out[4]{};
        for(std::size_t lane=0;lane<4;++lane){const auto k=group*4+lane;const double a=states[k].real,b=states[k].imag;
            states[k].real=(p.rc[k]*a-p.rs[k]*b)+p.bReal[k]*excitation;
            states[k].imag=(p.rs[k]*a+p.rc[k]*b)+p.bImag[k]*excitation;out[lane]=states[k].real;
        }
        accum[group%2]+=(out[0]+out[1])+(out[2]+out[3]);
    }
    const double mid=accum[0]*p.pickup,side=accum[1]*p.pickup;
    return {(mid+side)*rootHalf,(mid-side)*rootHalf};
}
void rendering(){
    auto p=std::make_unique<PreparedModal>();CHECK(prepareModal(shaped(),.5,48000,1024,*p));
    ModalTank one,blocks,destination,control;CHECK(one.prepareStorage(48000));CHECK(blocks.prepareStorage(48000));CHECK(destination.prepareStorage(48000));CHECK(control.prepareStorage(48000));
    CHECK(one.configure(*p));CHECK(blocks.configure(*p));CHECK(control.configure(*p));
    std::vector<ModalTank::Frame> input(4096),expected(4096),actual(4096);std::vector<ModalSample> states(maximumDirectModes);
    for(std::size_t n=0;n<input.size();++n)input[n]={n==0?.5:n<512?.001*std::sin(n*.13):0,n==0?.25:n<512?.0007*std::cos(n*.17):0};
    for(std::size_t n=0;n<input.size();++n)expected[n]=reference(*p,states,input[n]);
    allocations=frees=0;tracking=true;bool ok=true;
    for(std::size_t n=0;n<input.size();++n)actual[n]=one.tick(input[n]);
    tracking=false;CHECK(actual==expected);CHECK(allocations==0&&frees==0);
    bool stateExact=true;for(std::size_t k=0;k<p->count;++k)stateExact=stateExact&&one.modeState(k)==states[k];CHECK(stateExact);
    bool finite=true,stereo=false,nonzero=false;double peak=0;
    for(auto x:actual){stereo=stereo||x[0]!=x[1];for(double v:x){finite=finite&&std::isfinite(v);nonzero=nonzero||v!=0;peak=std::max(peak,std::abs(v));}}
    CHECK(finite&&nonzero&&stereo&&peak<=4);
    bool monoBecomesStereo=false;allocations=frees=0;tracking=true;
    for(unsigned n=0;n<128;++n){const auto frame=control.tick({n==0?.5:0,n==0?.5:0});monoBecomesStereo=monoBecomesStereo||frame[0]!=frame[1];}
    tracking=false;CHECK(monoBecomesStereo);CHECK(allocations==0&&frees==0);
    for(unsigned partition:{1U,17U,64U,512U}){
        actual=input;allocations=frees=0;tracking=true;blocks.reset();
        for(std::size_t offset=0;offset<actual.size();offset+=partition)ok=blocks.processBlock(std::span(actual).subspan(offset,std::min<std::size_t>(partition,actual.size()-offset)))&&ok;
        tracking=false;CHECK(ok);CHECK(actual==expected);CHECK(allocations==0&&frees==0);
    }
    auto bad=std::make_unique<PreparedModal>(*p);bad->rc[0]=2;
    allocations=frees=0;tracking=true;CHECK(!blocks.configure(*bad));CHECK(blocks.prepared()==*p);
    CHECK(!destination.adoptCopied(*p,destination));CHECK(destination.adoptCopied(*p,blocks));
    bool copied=true;for(std::size_t k=0;k<p->count;++k)copied=copied&&destination.modeState(k)==blocks.modeState(k);CHECK(copied);
    bool continuation=true;for(int n=0;n<128;++n)continuation=continuation&&destination.tick({0,0})==blocks.tick({0,0});
    tracking=false;CHECK(continuation);CHECK(allocations==0&&frees==0);
    auto changed=std::make_unique<PreparedModal>();auto curve=shaped();curve.bands[0].multiplier=4;CHECK(prepareModal(curve,1,48000,1024,*changed));
    allocations=frees=0;tracking=true;CHECK(destination.adoptCopied(*changed,blocks));copied=true;
    for(std::size_t k=0;k<p->count;++k)copied=copied&&destination.modeState(k)==blocks.modeState(k);CHECK(copied);tracking=false;CHECK(allocations==0&&frees==0);
    double beforeEnergy=0,afterEnergy=0,radius=0;
    for(std::size_t k=0;k<changed->count;++k){const auto z=destination.modeState(k);beforeEnergy+=z.real*z.real+z.imag*z.imag;radius=std::max(radius,std::hypot(changed->rc[k],changed->rs[k]));}
    std::array<ModalTank::Frame,512> silence{};allocations=frees=0;tracking=true;CHECK(destination.processBlock(silence));tracking=false;
    for(std::size_t k=0;k<changed->count;++k){const auto z=destination.modeState(k);afterEnergy+=z.real*z.real+z.imag*z.imag;}
    CHECK(beforeEnergy>0&&afterEnergy>0&&afterEnergy<=beforeEnergy*std::pow(radius,1024)*(1+1e-10));CHECK(allocations==0&&frees==0);
    std::array<ModalTank::Frame,513> oversized{};oversized[0]={.25,.5};const auto before=oversized;
    CHECK(!blocks.processBlock(oversized));CHECK(oversized==before);
    ModalTank clone;CHECK(clone.prepareStorage(48000));CHECK(clone.adoptCopied(*p,blocks));
    CHECK(blocks.processBlock({}));CHECK(blocks.tick({0,0})==clone.tick({0,0}));
    CHECK(!blocks.prepareStorage(0));CHECK(blocks.prepared()==*p);CHECK(blocks.storageBytes()<=16*1024*1024);
    blocks.reset();control.reset();CHECK(blocks.tick({.5,.25})==control.tick({.5,.25}));
}
}
int main(){validation();poles();rendering();std::cout<<"SpaceReverbDirectModal checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

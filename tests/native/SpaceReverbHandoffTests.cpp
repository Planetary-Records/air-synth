#include "SpaceReverbTank.h"
#include "SpaceReverbTankGolden.h"
#include "ProbeCases.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
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
using namespace airack::space::detail;
using Stereo=std::array<double,2>;
Stereo excitation(int n){return {n==0?.5:n<1000?.001*std::sin(n*.13):0,n==0?.25:n<1000?.0007*std::cos(n*.17):0};}
void identical() {
    const auto cases=probe::corpus();
    for(const auto& golden:test::tankGolden){PreparedTank p;CHECK(prepareTank(cases[std::size_t(golden.curve)].curve,golden.size,golden.rate,p));
        for(auto backend:{RenderBackend::scalar,RenderBackend::sse2,RenderBackend::automatic})for(double character:{0.,1.})for(int filled:{17,30000}){
            TankRuntime source,destination,control;CHECK(source.prepareStorage(golden.rate,backend));CHECK(destination.prepareStorage(golden.rate,backend));CHECK(control.prepareStorage(golden.rate,backend));
            CHECK(source.configure(p));CHECK(destination.configure(p));CHECK(control.configure(p));
            for(int n=0;n<filled;++n){source.tick(excitation(n),character);control.tick(excitation(n),character);}
            for(int n=0;n<37;++n)destination.tick({.001,-.002},character);
            allocations=frees=0;tracking=true;const bool adopted=destination.adoptCopied(p,source);bool same=true,nonzero=false;
            for(int n=0;n<5000;++n){const auto x=excitation(filled+n),a=source.tick(x,character),b=destination.tick(x,character),c=control.tick(x,character);same&=a==b&&a==c;for(double v:b)nonzero|=v!=0;}
            destination.reset();tracking=false;CHECK(adopted);CHECK(same);CHECK(nonzero);CHECK(allocations==0&&frees==0);CHECK(destination.prepared()==p);
        }
    }
}
// Independent chronological queues and direct matrix multiplication, not runtime ring indices.
struct Queue {
    std::vector<double> values;std::size_t written=0;
    void prepare(std::size_t length){values.assign(length,0);written=0;}
    double read() const {return values.front();}
    void push(double x){std::move(values.begin()+1,values.end(),values.begin());values.back()=x;written=std::min(written+1,values.size());}
    void change(std::size_t length){const auto k=std::min({written,values.size(),length});std::vector<double> next(length,0);std::copy(values.end()-k,values.end(),next.end()-k);values=std::move(next);written=k;}
};
struct Reference {
    PreparedTank prepared;std::array<Queue,8> delay;std::array<std::array<Queue,4>,2> diffusion;
    std::array<std::array<double,38>,8> z1{},z2{};
    void prepare(const PreparedTank& p){prepared=p;for(std::size_t i=0;i<8;++i)delay[i].prepare(p.lines[i].length);for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i)diffusion[ch][i].prepare(p.diffusion[ch][i].length);z1={};z2={};}
    void adopt(const PreparedTank& p){
        for(std::size_t i=0;i<8;++i){const auto& old=prepared.lines[i];const auto& next=p.lines[i];const bool equal=old.scalar==next.scalar&&old.count==next.count&&std::equal(old.coefficients.begin(),old.coefficients.begin()+old.count,next.coefficients.begin());
            if(!equal){z1[i].fill(0);z2[i].fill(0);}delay[i].change(next.length);}
        for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i)diffusion[ch][i].change(p.diffusion[ch][i].length);prepared=p;
    }
    Stereo tick(Stereo input){
        constexpr double scale=.3535533905932737622;
        for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i){auto& q=diffusion[ch][i];const double y=prepared.diffusion[ch][i].attenuation*q.read()-.6*input[ch];q.push(input[ch]+.6*y);input[ch]=y;}
        std::array<double,8> value{};for(std::size_t i=0;i<8;++i){double x=delay[i].read();const auto& line=prepared.lines[i];for(std::size_t j=0;j<line.count;++j){const auto& c=line.coefficients[j];const double y=c.b0*x+z1[i][j];z1[i][j]=c.b1*x-c.a1*y+z2[i][j];z2[i][j]=c.b2*x-c.a2*y;x=y;}value[i]=line.scalar*x;}
        Stereo out{};for(std::size_t i=0;i<8;++i){double mixed=0;for(std::size_t j=0;j<8;++j){auto bits=i&j;bool odd=false;while(bits){odd=!odd;bits&=bits-1;}mixed+=(odd?-1.:1.)*value[j];}
            const double sign=i%2?-1.:1.;delay[i].push(.25*(input[0]+sign*input[1])+scale*mixed);out[0]+=scale*value[i];out[1]+=scale*sign*value[i];}return out;
    }
};
PreparedTank synthetic(const PreparedTank& base,bool larger,bool changeBank){
    auto p=base;p.warmLines=0;p.stageCount=2;
    constexpr std::array<std::uint32_t,8> small{2,3,4,5,6,7,8,9},mixed{11,7,5,3,2,4,6,8};
    for(std::size_t i=0;i<8;++i){auto& line=p.lines[i];line.length=(larger?mixed:small)[i];line.scalar=.25;line.count=2;line.fitVariant=0;line.coefficients[0]={.5,.1,0,-.2,0};line.coefficients[1]={.75,.05,.02,-.1,.05};}
    if(changeBank)p.lines[0].coefficients[1].b0=.6;
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i){p.diffusion[ch][i].length=std::uint32_t(1+i+ch+(larger?1:0));p.diffusion[ch][i].attenuation=(larger?.7:.8)-.04*i;}
    return p;
}
void chronology() {
    PreparedTank base;CHECK(prepareTank(DecayCurve{},.5,48000,base));
    for(bool reverse:{false,true})for(bool changed:{false,true})for(int filled:{0,1,3,50}){
        const auto old=synthetic(base,reverse,false),next=synthetic(base,!reverse,changed);TankRuntime source,destination;Reference ref;ref.prepare(old);
        CHECK(source.prepareStorage(48000,RenderBackend::scalar));CHECK(destination.prepareStorage(48000,RenderBackend::scalar));CHECK(source.configure(old));CHECK(destination.configure(old));
        bool prior=true;for(int n=0;n<filled;++n){const auto a=source.tick(excitation(n)),b=ref.tick(excitation(n));for(std::size_t ch=0;ch<2;++ch)prior&=std::abs(a[ch]-b[ch])<1e-12;}CHECK(prior);
        ref.adopt(next);std::vector<Stereo> expected(2000);for(int n=0;n<2000;++n)expected[std::size_t(n)]=ref.tick(excitation(filled+n));
        allocations=frees=0;tracking=true;const bool adopted=destination.adoptCopied(next,source);bool same=true;double error=0;
        for(int n=0;n<2000;++n){const auto actual=destination.tick(excitation(filled+n));for(std::size_t ch=0;ch<2;++ch){error=std::max(error,std::abs(actual[ch]-expected[std::size_t(n)][ch]));same&=std::abs(actual[ch]-expected[std::size_t(n)][ch])<1e-12;}}
        tracking=false;CHECK(adopted);CHECK(same);CHECK(allocations==0&&frees==0);CHECK(destination.prepared()==next);
        std::cout<<"HANDOFF_QUEUE reverse="<<reverse<<" changed="<<changed<<" filled="<<filled<<" max_error="<<error<<'\n';
    }
}
void invalid() {
    PreparedTank p,q;CHECK(prepareTank(DecayCurve{},.5,48000,p));CHECK(prepareTank(DecayCurve{},.5,44100,q));TankRuntime source,destination,control,empty,wrongRate;
    CHECK(source.prepareStorage(48000));CHECK(destination.prepareStorage(48000));CHECK(control.prepareStorage(48000));CHECK(wrongRate.prepareStorage(44100));
    CHECK(source.configure(p));CHECK(destination.configure(p));CHECK(control.configure(p));CHECK(wrongRate.configure(q));
    for(int n=0;n<2000;++n){destination.tick(excitation(n));control.tick(excitation(n));source.tick(excitation(n));}
    auto bad=p;bad.lines[3].coefficients[0].a2=2;bad.lines[3].count=std::max(std::size_t(1),bad.lines[3].count);
    allocations=frees=0;tracking=true;CHECK(!destination.adoptCopied(p,destination));CHECK(!destination.adoptCopied(p,empty));CHECK(!empty.adoptCopied(p,source));CHECK(!destination.adoptCopied(p,wrongRate));CHECK(!destination.adoptCopied(bad,source));
    bool preserved=true;for(int n=0;n<10000;++n)preserved&=destination.tick({0,0})==control.tick({0,0});tracking=false;CHECK(preserved);CHECK(destination.prepared()==p);CHECK(allocations==0&&frees==0);
    allocations=frees=0;tracking=true;auto* known=::operator new(32,std::align_val_t(32));::operator delete(known,std::align_val_t(32));tracking=false;CHECK(allocations==1&&frees==1);
}
void switches() {
    PreparedTank base;CHECK(prepareTank(DecayCurve{},.5,48000,base));const auto a=synthetic(base,false,false),b=synthetic(base,true,true);Reference ref;ref.prepare(a);TankRuntime first,second;
    CHECK(first.prepareStorage(48000));CHECK(second.prepareStorage(48000));CHECK(first.configure(a));CHECK(second.configure(b));TankRuntime* active=&first;TankRuntime* spare=&second;bool finite=true,same=true,adopted=true;double peak=0,ending=0;std::size_t totalAlloc=0,totalFree=0;
    for(int n=0;n<10000;++n){if(n&&n%32==0){const auto& next=(n/32)%2?b:a;ref.adopt(next);allocations=frees=0;tracking=true;adopted&=spare->adoptCopied(next,*active);tracking=false;totalAlloc+=allocations;totalFree+=frees;std::swap(active,spare);}
        const auto expected=ref.tick(excitation(n));allocations=frees=0;tracking=true;const auto actual=active->tick(excitation(n));tracking=false;totalAlloc+=allocations;totalFree+=frees;
        for(std::size_t ch=0;ch<2;++ch){finite&=std::isfinite(actual[ch]);same&=std::abs(actual[ch]-expected[ch])<1e-12;peak=std::max(peak,std::abs(actual[ch]));if(n>9000)ending=std::max(ending,std::abs(actual[ch]));}}
    CHECK(adopted);CHECK(finite&&same&&peak<4);CHECK(ending<1e-12);CHECK(totalAlloc==0&&totalFree==0);
}
void highRate() {
    PreparedTank p;CHECK(prepareTank(DecayCurve{},1,768000,p));TankRuntime a,b;CHECK(a.prepareStorage(768000));CHECK(b.prepareStorage(768000));CHECK(a.configure(p));
    for(int n=0;n<130000;++n)a.tick(excitation(n));allocations=frees=0;tracking=true;const bool adopted=b.adoptCopied(p,a);bool same=true;for(int n=0;n<2000;++n)same&=a.tick({0,0})==b.tick({0,0});tracking=false;
    CHECK(adopted);CHECK(same);CHECK(allocations==0&&frees==0);CHECK(a.storageBytes()+b.storageBytes()<=16*1024*1024);
}
}
int main(){identical();chronology();invalid();switches();highRate();std::cout<<"SpaceReverbHandoff checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

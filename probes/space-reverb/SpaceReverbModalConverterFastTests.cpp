#include "ModalConverter.h"
#include "ModalConverterReference.h"
#include <cfenv>

#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"MODAL_FAST_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space::probe;
namespace original=airack::space::probe::converter_reference;
using Frame=ModalWetConverter::Frame;
std::uint64_t framePairs=0,downPairs=0,traces=0;
constexpr bool packedAvailable=
#if defined(_M_X64) || defined(__SSE2__)
true;
#else
false;
#endif
ModalConverterBackend resolved(ModalConverterBackend b){return b==ModalConverterBackend::automatic?(packedAvailable?ModalConverterBackend::mirroredSse2:ModalConverterBackend::mirroredScalar):b;}
bool same(std::span<const Frame> a,std::span<const Frame> b){return a.size()==b.size()&&(a.empty()||std::memcmp(a.data(),b.data(),a.size_bytes())==0);}
std::vector<Frame> input(unsigned kind,std::size_t count){
    std::vector<Frame> x(count);
    for(std::size_t i=0;i<count;++i){
        if(kind==0||kind==5)x[i]={double(int(i*17%97)-48)/16.,double(int(i*37%89)-44)/16.};
        if(kind==1){x[i]={0,0};if(i==0)x[i]={1,-.5};if(i==127)x[i]={-.25,.75};}
        if(kind==2)x[i]={i%2?-0.:0.,i%3?-0.:0.};
        if(kind==3){const auto tiny=std::numeric_limits<double>::denorm_min();x[i]={double(int(i%5)-2)*tiny,double(int(i%7)-3)*tiny};}
        if(kind==4)x[i]={std::ldexp(double(int(i%7)-3),200),std::ldexp(double(int(i%11)-5),400)};
    }return x;
}
void transform(std::span<Frame> x,unsigned kind){
    for(std::size_t i=0;i<x.size();++i){const auto old=x[i];
        if(kind==0)x[i]={.25*old[0]+.75*old[1],.5*old[0]-.5*old[1]};
        if(kind==1)x[i]={i%2?-0.:0.,i%3?-0.:0.};
        if(kind==2)x[i]={double(int(i%7)-3)/8.,double(int(i%11)-5)/16.};
    }
}
bool step(ModalWetConverter& a,original::ModalWetConverter& b,std::span<Frame> physicalA,std::span<Frame> physicalB,unsigned change){
    std::array<Frame,512> logicalA{},logicalB{};std::size_t countA=777,countB=777;
    const bool downA=a.down(physicalA,logicalA,countA),downB=b.down(physicalB,logicalB,countB);
    CHECK(downA==downB);CHECK(downA);if(!downA||!downB)return false;
    CHECK(countA==countB);CHECK(a.evenPhase()==b.evenPhase());CHECK(a.open()==b.open());
    CHECK(same(std::span(logicalA).first(countA),std::span(logicalB).first(countB)));downPairs+=countA;
    transform(std::span(logicalA).first(countA),change);transform(std::span(logicalB).first(countB),change);
    const bool upA=a.up(std::span(logicalA).first(countA),physicalA),upB=b.up(std::span(logicalB).first(countB),physicalB);
    CHECK(upA==upB);CHECK(upA);CHECK(same(physicalA,physicalB));CHECK(a.open()==b.open());
    framePairs+=physicalA.size();return upA&&upB;
}
void descriptors(const PreparedModalConverter& a,const original::PreparedModalConverter& b){
    CHECK(a.factor()==b.factor());CHECK(a.absoluteSum()==b.absoluteSum());CHECK(a.peakGain()==b.peakGain());CHECK(a.logGain()==b.logGain());
    CHECK(a.physicalSupport()==b.physicalSupport());CHECK(a.logicalSupport()==b.logicalSupport());CHECK(a.physicalGroupDelay()==b.physicalGroupDelay());
    for(unsigned k=0;k<127;++k)CHECK(std::bit_cast<std::uint64_t>(a.tap(k))==std::bit_cast<std::uint64_t>(b.tap(k)));
}
void trace(ModalConverterBackend backend,unsigned factor,unsigned kind,std::size_t partition,bool variable){
    PreparedModalConverter p;original::PreparedModalConverter q;CHECK(prepareModalConverter(factor,p));CHECK(original::prepareModalConverter(factor,q));descriptors(p,q);
    ModalWetConverter a;original::ModalWetConverter b;const bool admitted=a.configure(p,backend);
    if(backend==ModalConverterBackend::mirroredSse2&&!packedAvailable){CHECK(!admitted);return;}
    CHECK(admitted);if(!admitted)return;CHECK(a.selectedBackend()==resolved(backend));CHECK(b.configure(q));CHECK(a.storageBytes()<8192);
    const auto raw=input(kind,2053);auto actual=raw,expected=raw;
    const std::array<std::size_t,11> parts{1,3,17,64,127,128,255,511,512,2,31};
    allocations=frees=0;tracking=true;std::size_t ordinal=0;
    for(std::size_t at=0;at<raw.size();){
        const auto count=std::min(variable?parts[ordinal%parts.size()]:partition,raw.size()-at);
        CHECK(step(a,b,std::span(actual).subspan(at,count),std::span(expected).subspan(at,count),kind%4));
        if(kind==5&&ordinal%7==3){a.reset();b.reset();}
        ++ordinal;at+=count;
        CHECK(step(a,b,{},{},kind%4));
    }
    tracking=false;CHECK(allocations==0&&frees==0);CHECK(same(actual,expected));
    a.reset();b.reset();actual=raw;expected=raw;
    allocations=frees=0;tracking=true;
    for(std::size_t at=0;at<raw.size();){const auto n=std::min(partition,raw.size()-at);CHECK(step(a,b,std::span(actual).subspan(at,n),std::span(expected).subspan(at,n),kind%4));at+=n;}
    tracking=false;CHECK(allocations==0&&frees==0);CHECK(same(actual,expected));++traces;
}
void invalidAndCopies(ModalConverterBackend backend){
    PreparedModalConverter p;original::PreparedModalConverter q;CHECK(prepareModalConverter(2,p));CHECK(original::prepareModalConverter(2,q));
    ModalWetConverter a;original::ModalWetConverter b;const bool admitted=a.configure(p,backend);
    if(backend==ModalConverterBackend::mirroredSse2&&!packedAvailable){CHECK(!admitted);return;}
    CHECK(admitted);if(!admitted)return;CHECK(b.configure(q));
    std::array<Frame,3> x{{{1,-0.},{2,-2},{3,-3}}},y=x,wet{},wetB{};const auto before=x;std::size_t n=77,m=77;
    CHECK(!a.down(x,std::span(wet).first(1),n));CHECK(n==77&&!a.open());
    auto bad=x;bad[2][0]=std::numeric_limits<double>::quiet_NaN();CHECK(!a.down(bad,wet,n));CHECK(n==77&&!a.open());
    std::array<Frame,513> large{};CHECK(!a.down(large,wet,n));CHECK(!a.down(x,x,n));
    CHECK(a.down(x,wet,n));CHECK(b.down(y,wetB,m));CHECK(n==m&&n==2);CHECK(same(std::span(wet).first(n),std::span(wetB).first(m)));
    CHECK(!a.configure(p,backend));CHECK(!a.up(std::span(wet).first(n-1),x));CHECK(same(x,before)&&a.open());
    const auto held=wet;wet[1][0]=std::numeric_limits<double>::quiet_NaN();CHECK(!a.up(std::span(wet).first(n),x));CHECK(same(x,before)&&a.open());wet=held;
    CHECK(!a.up(std::span(wet).first(n),std::span(wet).first(3)));CHECK(a.up(std::span(wet).first(n),x));CHECK(b.up(std::span(wetB).first(m),y));CHECK(same(x,y));
    CHECK(!a.configure(p,static_cast<ModalConverterBackend>(999)));CHECK(a.selectedBackend()==resolved(backend));
    auto copied=a;auto copiedB=b;auto future=input(0,257),futureB=future;
    allocations=frees=0;tracking=true;
    for(std::size_t at=0;at<future.size();){const auto count=std::min(std::size_t(17),future.size()-at);CHECK(step(copied,copiedB,std::span(future).subspan(at,count),std::span(futureB).subspan(at,count),0));at+=count;}
    copied.reset();copiedB.reset();CHECK(step(copied,copiedB,x,y,1));tracking=false;CHECK(allocations==0&&frees==0);
}
}
int main(){
    std::cout.precision(17);std::fenv_t saved{};CHECK(std::fegetenv(&saved)==0);
    for(int rounding:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){
        CHECK(std::fesetround(rounding)==0);
        for(auto backend:{ModalConverterBackend::reference,ModalConverterBackend::mirroredScalar,ModalConverterBackend::mirroredSse2,ModalConverterBackend::automatic}){
            invalidAndCopies(backend);
            for(unsigned factor:{1U,2U})for(unsigned kind=0;kind<6;++kind)
                for(std::size_t partition:{1U,3U,17U,64U,127U,128U,255U,511U,512U})trace(backend,factor,kind,partition,partition==17);
        }
    }
    CHECK(std::fesetenv(&saved)==0);
    std::cout<<"MODAL_CONVERTER_FAST checks="<<checks<<" failures="<<failures<<" traces="<<traces<<" physical_frame_pairs="<<framePairs<<" logical_frame_pairs="<<downPairs<<" actual_timing=0\n";
    return failures?1:0;
}

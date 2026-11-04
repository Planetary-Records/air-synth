#include "ModalRoom.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {thread_local bool tracking=false;thread_local bool failAllocation=false;thread_local std::size_t allocations=0,frees=0;unsigned checks=0,failures=0;}
void* operator new(std::size_t n){if(failAllocation){failAllocation=false;throw std::bad_alloc();}if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t a){if(failAllocation){failAllocation=false;throw std::bad_alloc();}if(tracking)++allocations;
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


#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<30)std::cerr<<"BLOCKED_BOUNDARY_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;using Frame=ModalRoom::Frame;
}
int main(){
    unsigned cases=0,integerTaps=0;
    for(auto geometry:std::array<std::array<double,2>,4>{{{100000,1},{200000,0},{200000,1},{80000,.5}}})for(std::size_t packet:{1U,2U,17U,64U,511U,512U}){
        auto prepared=std::make_unique<PreparedModalRoom>();CHECK(prepareModalRoom({},geometry[1],geometry[0],256,*prepared));unsigned integer=0;for(auto d:prepared->diffusionSamples)integer+=d==std::floor(d);CHECK(integer>0);integerTaps+=integer;
        ModalRoom reference,candidate;CHECK(reference.prepareStorage(geometry[0],ModalBackend::automatic,false)&&candidate.prepareStorage(geometry[0],ModalBackend::automatic,true));CHECK(reference.configure(*prepared)&&candidate.configure(*prepared));CHECK(!reference.usesBlockedScatter()&&candidate.usesBlockedScatter());
        std::vector<Frame> a(8193),b;std::vector<double> character(a.size());for(std::size_t n=0;n<a.size();++n){a[n]={n%11==0?-0.:double(int(n%37)-18)/1024.,n%13==0?-0.:double(int(n%31)-15)/2048.};character[n]=double(n%4)/3.;}b=a;
        allocations=frees=0;tracking=true;bool ok=true;for(std::size_t at=0;at<a.size();at+=packet){const auto count=std::min(packet,a.size()-at);ok=reference.processBlock(std::span(a).subspan(at,count),std::span(character).subspan(at,count))&&ok;ok=candidate.processBlock(std::span(b).subspan(at,count),std::span(character).subspan(at,count))&&ok;}tracking=false;
        CHECK(ok&&allocations==0&&frees==0);CHECK(std::memcmp(a.data(),b.data(),a.size()*sizeof(Frame))==0);for(auto frame:a)for(double v:frame)CHECK(std::isfinite(v));for(std::size_t k=0;k<256;++k){const auto x=reference.modeState(k),y=candidate.modeState(k);CHECK(std::bit_cast<std::uint64_t>(x.real)==std::bit_cast<std::uint64_t>(y.real)&&std::bit_cast<std::uint64_t>(x.imag)==std::bit_cast<std::uint64_t>(y.imag));}++cases;
    }
    CHECK(cases==24&&integerTaps>0);std::cout<<"SpaceReverbBlockedBoundary checks="<<checks<<" failures="<<failures<<" cases="<<cases<<" integer_taps="<<integerTaps<<'\n';return failures?1:0;
}

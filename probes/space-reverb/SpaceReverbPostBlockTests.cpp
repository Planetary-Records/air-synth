#include "SpaceReverbControls.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<25)std::cerr<<"POST_BLOCK_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;
using Frame=std::array<double,2>;
std::uint64_t comparedFrames=0;
bool same(Frame a,Frame b){return std::bit_cast<std::array<std::uint64_t,2>>(a)==std::bit_cast<std::array<std::uint64_t,2>>(b);}
bool block(PostEq& eq,std::span<Frame> data){allocations=frees=0;tracking=true;const bool ok=eq.processBlock(data);tracking=false;CHECK(allocations==0&&frees==0);return ok;}
Settings fixture(unsigned index){
    Settings s;
    constexpr std::array<double,6> frequencies{20,110,780,3100,9100,20000},cuts{20,18000,40,16000,60,12000};
    for(unsigned j=0;j<6;++j){auto& b=s.post[j];b.active=index!=0;b.bypassed=index==1;
        b.frequencyHz=index%5==0?20:index%5==1?20000:frequencies[j];b.gainDb=j%2?18:-18;
        b.bellQ=index%2?12:.25;b.shelfQ=index%2?4:.25;b.slope=index%2?CutSlope::db24:CutSlope::db12;
        b.shape=index>=3&&index<=7?PostShape(index-3):PostShape((index+j)%5);
        if(index==2){b.shape=PostShape::bell;b.gainDb=0;}
        if(index==8||index==9){b.shape=j%2?PostShape::highCut:PostShape::lowCut;b.slope=CutSlope::db24;b.frequencyHz=cuts[j]*(index==9?1.05:1);}
        if(index>=10){if(index%3==0)b.active=j%2==0;if(index%4==0)b.bypassed=j%3==0;}
    }
    return s;
}
void invalidBlock(PostEq& eq){
    const auto before=eq;std::vector<Frame> data(513,{.125,-.25});const auto original=data;
    CHECK(!block(eq,data));CHECK(eq==before);CHECK(std::memcmp(data.data(),original.data(),data.size()*sizeof(Frame))==0);
}
bool run(double rate,unsigned index){
    PreparedPostEq coefficients;CHECK(preparePostEq(fixture(index),rate,coefficients));PostEq scalar,blocked;
    CHECK(scalar.prepare(coefficients));CHECK(blocked.prepare(coefficients));
    auto before=blocked;CHECK(block(blocked,{}));CHECK(blocked==before);invalidBlock(blocked);
    std::vector<Frame> input(2051);std::uint64_t random=0x9e3779b97f4a7c15ULL^(index+1);
    const auto noise=[&]{random^=random<<13;random^=random>>7;random^=random<<17;return double(int(random&2047)-1024)/8192;};
    for(std::size_t n=0;n<input.size();++n){
        if(n==0)input[n]={.5,-.375};
        else if(n>=1024||n%31==0)input[n]={n%2?-0.:0.,n%2?0.:-0.};
        else if(n%127==0)input[n]={0.,-.5};
        else input[n]={noise(),noise()};
    }
    auto output=input,expected=input;constexpr std::array<std::size_t,6> packets{1,3,17,64,511,512};std::size_t cursor=0,packet=0;
    while(cursor<input.size()){
        const auto count=std::min(packets[packet++%packets.size()],input.size()-cursor);
        for(std::size_t n=cursor;n<cursor+count;++n)expected[n]=scalar.tick(input[n]);
        const bool ok=block(blocked,std::span(output).subspan(cursor,count));CHECK(ok);if(!ok)return false;
        for(std::size_t n=cursor;n<cursor+count;++n)CHECK(same(output[n],expected[n]));
        CHECK(blocked==scalar);comparedFrames+=count;cursor+=count;
        if(cursor==1)invalidBlock(blocked);
    }
    invalidBlock(blocked);scalar.reset();blocked.reset();
    auto restart=std::span(output).first(64);std::copy_n(input.begin(),64,restart.begin());CHECK(block(blocked,restart));
    for(std::size_t n=0;n<64;++n)CHECK(same(restart[n],scalar.tick(input[n])));CHECK(blocked==scalar);
    CHECK(blocked.prepare(coefficients));CHECK(scalar.prepare(coefficients));std::copy_n(input.begin(),64,restart.begin());CHECK(block(blocked,restart));
    for(std::size_t n=0;n<64;++n)CHECK(same(restart[n],scalar.tick(input[n])));CHECK(blocked==scalar);
    before=blocked;CHECK(block(blocked,{}));CHECK(blocked==before);return true;
}
}
int main(){
    allocations=frees=0;tracking=true;auto* calibration=::operator new(16);::operator delete(calibration);tracking=false;CHECK(allocations==1&&frees==1);
    PostEq unprepared;std::array<Frame,1> raw{{{.25,-.5}}};const auto original=raw;
    CHECK(!block(unprepared,raw));CHECK(raw==original);CHECK(!block(unprepared,{}));
    unsigned completed=0;for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.,768000.})for(unsigned index=0;index<24;++index)completed+=unsigned(run(rate,index));
    std::cout<<"SpaceReverbPostBlock checks="<<checks<<" failures="<<failures<<" configurations="<<completed<<" compared_frames="<<comparedFrames<<"\n";
    return failures?1:0;
}

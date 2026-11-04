#include "ModalServiceProfile.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <limits>
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

#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<35)std::cerr<<"BLOCKED_SCATTER_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;
using Frame=ModalService::Frame;
struct Result {bool ok=true;std::size_t allocated=0,freed=0;};
Result run(ModalService& service,const ModalSlotSettings& settings,const ModalSlotIdentities& ids,unsigned mask,std::span<Frame> audio,std::size_t packet,std::span<std::array<Frame,1024>> seen){
    Result out;allocations=frees=0;tracking=true;
    out.ok=service.beginParent(settings,mask,ids,audio.size());
    for(unsigned slot=0;slot<8;++slot)if(mask&(1U<<slot)){for(std::size_t at=0;at<audio.size();at+=packet)
        out.ok=service.renderSlot(slot,audio.subspan(at,std::min(packet,audio.size()-at)))&&out.ok;std::copy(audio.begin(),audio.end(),seen[slot].begin());}
    out.ok=service.endParent()&&out.ok;tracking=false;out.allocated=allocations;out.freed=frees;return out;
}
void pump(ModalService& a,ModalService& b){
    for(unsigned n=0;n<32;++n){const bool x=a.pumpHeavy(),y=b.pumpHeavy();CHECK(x==y);if(!x&&!y)break;CHECK(n<31);}
    for(unsigned n=0;n<64;++n){const bool x=a.pumpLight(),y=b.pumpLight();CHECK(x==y);if(!x&&!y)break;CHECK(n<63);}
}
void paired(double rate,std::size_t modes,std::size_t parent,unsigned factor,unsigned mask){
    ModalSlotSettings settings{};ModalSlotIdentities ids{101,102,103,104,105,106,107,108};
    for(auto& s:settings){s.decay.decaySeconds=.4;s.mix=.6;s.width=1.2;s.character=.7;s.distance=.3;}
    auto first=prepareModalBundle(rate,modes,401,settings,ids,{},factor);
    auto cold=prepareModalBundle(rate,modes,402,settings,ids,{},factor);
    CHECK(bool(first)&&bool(cold));if(!first||!cold)return;
    ModalService a,b;ModalServiceOptions reference;reference.threaded=false;reference.factor=factor;reference.preparedCopies=true;reference.blockedScatter=false;
    auto candidate=reference;candidate.blockedScatter=true;
    CHECK(a.prepareStorage(rate,modes,reference)&&b.prepareStorage(rate,modes,candidate));
    CHECK(!a.usesBlockedScatter()&&b.usesBlockedScatter()&&a.usesPreparedCopies()&&b.usesPreparedCopies());
    CHECK(a.installBundle(*first,401)&&b.installBundle(*first,401));
    CHECK(a.storageBytes()==b.storageBytes()&&a.storageBytes()<=128*1024*1024);
    std::array<std::array<Frame,1024>,8> seenX{},seenY{};
    unsigned copies=0,post=0;std::uint64_t compared=0;bool exact=true,statusExact=true,tailExact=true,heapClean=true;
    const unsigned timeScale=std::max(1U,unsigned(std::ceil(rate/96000.)));
    for(unsigned iteration=0;iteration<96*timeScale;++iteration){const unsigned n=iteration/timeScale;const bool boundary=iteration%timeScale==0;
        if(boundary&&(n==4||n==24||n==48||n==72))for(unsigned i=0;i<8;++i){auto& s=settings[i];s.size=.05+double((n+i)%17)/20.;s.decay.decaySeconds=.15+double(n+i)/50.;s.decay.bands[0]={true,false,DecayShape::highShelf,4000,.65,1};}
        if(boundary&&(n==16||n==36))for(auto& s:settings){s.post[0]={true,false,PostShape::bell,1300,n==16?3.:-4.,.8,1,CutSlope::db12};s.duckRangeDb=12;s.duckThresholdDb=-30;s.duckAttackMs=2;s.predelayMs=n;s.character=n==16?1:0;}
        if(boundary&&n==44){CHECK(a.resetSlot(0)&&b.resetSlot(0));}
        if(boundary&&n==64){allocations=frees=0;tracking=true;const bool installed=a.installBundle(*cold,402)&&b.installBundle(*cold,402);tracking=false;CHECK(installed&&allocations==0&&frees==0);settings={};for(auto& s:settings){s.decay.decaySeconds=.4;s.mix=.6;s.width=1.2;s.character=.7;s.distance=.3;}}
        std::array<Frame,1024> x{},y{};const auto frames=parent*factor;
        for(std::size_t k=0;k<frames;++k)if(n<80)x[k]={double(int((k+31*n)%37)-18)/1024.,double(int((k+17*n)%31)-15)/2048.};y=x;
        const auto xr=run(a,settings,ids,mask,std::span(x).first(frames),512,seenX);
        const auto yr=run(b,settings,ids,mask,std::span(y).first(frames),n%2?17:1,seenY);
        CHECK(xr.ok&&yr.ok);heapClean=heapClean&&xr.allocated==0&&xr.freed==0&&yr.allocated==0&&yr.freed==0;
        for(unsigned slot=0;slot<8;++slot)if(mask&(1U<<slot))for(std::size_t k=0;k<frames;++k)for(unsigned c=0;c<2;++c){const auto u=seenX[slot][k][c],v=seenY[slot][k][c];exact=exact&&std::bit_cast<std::uint64_t>(u)==std::bit_cast<std::uint64_t>(v);CHECK(std::isfinite(u)&&std::isfinite(v));++compared;}
        ModalServiceStatus sa,sb;CHECK(a.snapshot(sa)&&b.snapshot(sb));statusExact=statusExact&&sa==sb;
        if(sb.admissionsThisParent){copies+=sb.maximumRoomsThisParent==std::popcount(mask)+1;post+=sb.maximumRoomsThisParent==std::popcount(mask);}
        CHECK(sa.admissionsThisParent<=1&&sb.admissionsThisParent<=1&&sa.maximumRoomsThisParent<=9&&sb.maximumRoomsThisParent<=9);
        for(auto s:sb.slots)CHECK(s.preparationFailures==0);
        std::array<std::size_t,8> indices{};std::size_t count=0;for(unsigned i=0;i<8;++i)if(mask&(1U<<i))indices[count++]=i;
        ModalTailReport ta,tb;CHECK(a.composeTail(std::span(indices).first(count),1,ta)&&b.composeTail(std::span(indices).first(count),1,tb));tailExact=tailExact&&ta==tb;
        pump(a,b);
    }
    CHECK(exact&&statusExact&&tailExact&&heapClean);CHECK(copies>=3&&post>=1);
    std::cout<<"BLOCKED_SCATTER_PAIR rate="<<rate<<" modes="<<modes<<" parent="<<parent<<" factor="<<factor<<" mask="<<mask<<" copies="<<copies<<" post="<<post<<" samples="<<compared<<" exact="<<exact<<" status="<<statusExact<<" tail="<<tailExact<<" heap="<<heapClean<<" bytes="<<a.storageBytes()<<'\n';
}

bool roomRun(ModalRoom& room,std::span<Frame> audio,std::span<const double> character,bool segmented){
    constexpr std::array<std::size_t,7> packets{1,2,3,17,64,511,512};bool ok=true;std::size_t step=0;
    for(std::size_t at=0;at<audio.size();){const auto count=std::min(segmented?packets[step++%packets.size()]:512U,audio.size()-at);ok=room.processBlock(audio.subspan(at,count),character.subspan(at,count))&&ok;at+=count;}return ok;
}
void roomHistories(){
    unsigned cases=0;std::uint64_t compared=0;
    for(double rate:{8000.,44100.,96000.,768000.})for(double size:{0.,.5,1.})for(double amplitude:{0.,std::numeric_limits<double>::denorm_min(),1e-150,1e150}){
        auto descriptor=std::make_unique<PreparedModalRoom>(),changed=std::make_unique<PreparedModalRoom>();DecayCurve curve;curve.decaySeconds=.3;
        CHECK(prepareModalRoom(curve,size,rate,256,*descriptor)&&prepareModalRoom(curve,1-size,rate,256,*changed));
        std::array<ModalRoom,2> sources,destinations;bool prepared=true;
        for(unsigned i=0;i<2;++i){prepared=sources[i].prepareStorage(rate,ModalBackend::automatic,i==1)&&destinations[i].prepareStorage(rate,ModalBackend::automatic,i==1)&&sources[i].configure(*descriptor)&&destinations[i].configure(*descriptor)&&prepared;CHECK(sources[i].usesBlockedScatter()==(i==1));}
        CHECK(prepared);if(!prepared)return;
        std::vector<Frame> a(8193),b;std::vector<double> character(8193);
        auto fill=[&](std::size_t count,unsigned salt){a.resize(count);character.resize(count);for(std::size_t n=0;n<count;++n){a[n]=amplitude==0?Frame{n%2?-0.:0.,n%3?-0.:0.}:Frame{double(int((n+salt)%37)-18)*amplitude/32.,double(int((n+salt)%31)-15)*amplitude/32.};character[n]=double((n+salt)%5)/4.;}b=a;};
        auto compare=[&](ModalRoom& reference,ModalRoom& candidate){allocations=frees=0;tracking=true;const bool ok=roomRun(reference,a,character,false)&&roomRun(candidate,b,character,true);tracking=false;CHECK(ok&&allocations==0&&frees==0);CHECK(std::memcmp(a.data(),b.data(),a.size()*sizeof(Frame))==0);for(auto frame:a)for(double v:frame)CHECK(std::isfinite(v));for(std::size_t k=0;k<256;++k){const auto x=reference.modeState(k),y=candidate.modeState(k);CHECK(std::bit_cast<std::uint64_t>(x.real)==std::bit_cast<std::uint64_t>(y.real)&&std::bit_cast<std::uint64_t>(x.imag)==std::bit_cast<std::uint64_t>(y.imag));}compared+=a.size()*2;};
        fill(8193,0);compare(sources[0],sources[1]);
        allocations=frees=0;tracking=true;const bool copied=destinations[0].adoptCopied(*changed,sources[0])&&destinations[1].adoptCopied(*changed,sources[1]);tracking=false;CHECK(copied&&allocations==0&&frees==0);
        CHECK(destinations[0].prepared()==*changed&&destinations[1].prepared()==*changed);CHECK(destinations[0].historyCopyBytes()==destinations[1].historyCopyBytes());
        fill(4097,7);compare(destinations[0],destinations[1]);destinations[0].reset();destinations[1].reset();fill(257,13);compare(destinations[0],destinations[1]);
        CHECK(destinations[1].storageBytes()<=16*1024*1024);++cases;
    }
    std::cout<<"BLOCKED_SCATTER_HISTORIES cases="<<cases<<" expected=48 samples="<<compared<<'\n';CHECK(cases==48);
}
}
int main(){
    CHECK(ModalServiceOptions{}.blockedScatter);
    ModalService unprepared;ServiceCpuWorkload workload;CHECK(!unprepared.usesBlockedScatter()&&!workload.usesBlockedScatter());
    {ModalService policy;ModalServiceOptions defaults;defaults.threaded=false;CHECK(policy.prepareStorage(8000,256,defaults));CHECK(policy.usesBlockedScatter());}
    for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.,768000.})for(std::size_t modes:{256U,512U,1024U,2048U})for(std::size_t parent:{64U,512U})for(unsigned factor:{1U,2U}){if(rate*factor>768000)continue;paired(rate,modes,parent,factor,1);if(failures){std::cout<<"SPACE_BLOCKED_SCATTER checks="<<checks<<" failures="<<failures<<" fail_fast=1\n";return 1;}}
    paired(48000,256,512,1,255);paired(48000,256,64,2,255);
    roomHistories();
    std::cout<<"SPACE_BLOCKED_SCATTER checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

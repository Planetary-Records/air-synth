#include "ModalServiceProfile.h"
#include "ModalKernel.h"
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

#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<35)std::cerr<<"PAIRED_REDUCTION_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
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
    ModalService a,b;ModalServiceOptions reference;reference.threaded=false;reference.factor=factor;reference.preparedCopies=true;reference.blockedScatter=true;reference.pairedReduction=false;
    auto candidate=reference;candidate.pairedReduction=true;
    CHECK(a.prepareStorage(rate,modes,reference)&&b.prepareStorage(rate,modes,candidate));
    CHECK(a.usesBlockedScatter()&&b.usesBlockedScatter()&&a.usesPreparedCopies()&&b.usesPreparedCopies());
    CHECK(a.installBundle(*first,401)&&b.installBundle(*first,401));
    CHECK(!a.usesPairedReduction()&&b.usesPairedReduction());if(!b.usesPairedReduction())return;
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
        ModalServiceStatus sa,sb;CHECK(a.snapshot(sa)&&b.snapshot(sb));statusExact=statusExact&&sa==sb;CHECK(!a.usesPairedReduction()&&b.usesPairedReduction());
        if(sb.admissionsThisParent){copies+=sb.maximumRoomsThisParent==std::popcount(mask)+1;post+=sb.maximumRoomsThisParent==std::popcount(mask);}
        CHECK(sa.admissionsThisParent<=1&&sb.admissionsThisParent<=1&&sa.maximumRoomsThisParent<=9&&sb.maximumRoomsThisParent<=9);
        for(auto s:sb.slots)CHECK(s.preparationFailures==0);
        std::array<std::size_t,8> indices{};std::size_t count=0;for(unsigned i=0;i<8;++i)if(mask&(1U<<i))indices[count++]=i;
        ModalTailReport ta,tb;CHECK(a.composeTail(std::span(indices).first(count),1,ta)&&b.composeTail(std::span(indices).first(count),1,tb));tailExact=tailExact&&ta==tb;
        pump(a,b);
    }
    CHECK(exact&&statusExact&&tailExact&&heapClean);CHECK(copies>=3&&post>=1);
    std::cout<<"PAIRED_REDUCTION_PAIR rate="<<rate<<" modes="<<modes<<" parent="<<parent<<" factor="<<factor<<" mask="<<mask<<" copies="<<copies<<" post="<<post<<" samples="<<compared<<" exact="<<exact<<" status="<<statusExact<<" tail="<<tailExact<<" heap="<<heapClean<<" bytes="<<a.storageBytes()<<'\n';
}

bool same(double a,double b) {return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
void fillKernel(std::vector<Frame>& frames,unsigned family,unsigned salt) {
    std::uint32_t random=0x7e15a39d+salt;
    constexpr std::array<double,12> edges{0.,-0.,std::numeric_limits<double>::denorm_min(),-std::numeric_limits<double>::denorm_min(),std::numeric_limits<double>::min(),-std::numeric_limits<double>::min(),1e-100,-1e-100,1e100,-1e100,.25,-.25};
    for(std::size_t n=0;n<frames.size();++n) {
        random^=random<<13;random^=random>>17;random^=random<<5;
        const double x=double(int(random&65535)-32768)/65536.;
        switch(family) {
            case 0: frames[n]={x,double(int((random>>16)&65535)-32768)/65536.};break;
            case 1: frames[n]={x,x};break;
            case 2: frames[n]={x,-x};break;
            default: frames[n]={edges[(n+salt)%edges.size()],edges[(3*n+salt+5)%edges.size()]};break;
        }
    }
}
bool renderKernel(ModalTank& tank,std::span<Frame> audio,std::size_t packet) {
    bool ok=true;
    for(std::size_t at=0;at<audio.size();at+=packet)
        ok=tank.processBlock(audio.subspan(at,std::min(packet,audio.size()-at)))&&ok;
    return ok;
}
void compareKernel(const ModalTank& a,const ModalTank& b,std::span<const Frame> x,std::span<const Frame> y,std::size_t modes) {
    CHECK(x.size()==y.size());
    for(std::size_t n=0;n<x.size();++n)for(unsigned c=0;c<2;++c)CHECK(same(x[n][c],y[n][c])&&std::isfinite(x[n][c])&&std::isfinite(y[n][c]));
    for(std::size_t k=0;k<modes;++k){const auto u=a.modeState(k),v=b.modeState(k);CHECK(same(u.real,v.real)&&same(u.imag,v.imag));}
    CHECK(a.prepared()==b.prepared());
}
bool dispatchWitness() {
    const auto avx=availableModalAvx2Kernel();
    CHECK(avx!=nullptr);if(!avx){std::cout<<"PAIRED_REDUCTION_UNAVAILABLE avx2=0\n";return false;}
    auto descriptor=std::make_unique<PreparedModal>();DecayCurve curve;
    CHECK(prepareModal(curve,.5,8000,256,*descriptor));
    const auto reference=specializeModalKernel(avx,*descriptor,false),candidate=specializeModalKernel(avx,*descriptor,true);
    CHECK(!modalKernelUsesPairedOutput(reference));CHECK(modalKernelUsesPairedOutput(candidate));CHECK(reference!=candidate);
    ModalTank a,b;CHECK(a.prepareStorage(8000,ModalBackend::automatic,false)&&b.prepareStorage(8000,ModalBackend::automatic,true));
    CHECK(!a.usesPairedReduction()&&!b.usesPairedReduction());
    CHECK(a.configure(*descriptor)&&b.configure(*descriptor));CHECK(!a.usesPairedReduction()&&b.usesPairedReduction());
    return failures==0;
}
void kernelPairs() {
    unsigned cases=0;
    for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.,768000.})
    for(std::size_t modes:{256U,512U,1024U,2048U})
    for(std::size_t packet:{1U,2U,3U,17U,64U,511U,512U})
    for(unsigned family=0;family<4;++family) {
        auto descriptor=std::make_unique<PreparedModal>(),changed=std::make_unique<PreparedModal>();
        DecayCurve curve;curve.decaySeconds=.4;curve.bands[0]={true,false,DecayShape::highShelf,3000,.65,.8};
        CHECK(prepareModal(curve,.2,rate,modes,*descriptor)&&prepareModal(curve,.8,rate,modes,*changed));
        std::array<ModalTank,2> sources,destinations;
        bool ready=true;
        for(unsigned i=0;i<2;++i)ready=sources[i].prepareStorage(rate,ModalBackend::automatic,i==1)&&destinations[i].prepareStorage(rate,ModalBackend::automatic,i==1)&&sources[i].configure(*descriptor)&&ready;
        CHECK(ready);if(!ready)return;
        CHECK(!sources[0].usesPairedReduction()&&sources[1].usesPairedReduction());
        std::vector<Frame> x(4099),y;fillKernel(x,family,0);y=x;
        allocations=frees=0;tracking=true;
        const bool first=renderKernel(sources[0],std::span(x).first(2051),packet)&&renderKernel(sources[1],std::span(y).first(2051),packet);
        const bool copied=destinations[0].adoptCopied(*changed,sources[1])&&destinations[1].adoptCopied(*changed,sources[0]);
        tracking=false;CHECK(first&&copied&&allocations==0&&frees==0);
        compareKernel(sources[0],sources[1],std::span(x).first(2051),std::span(y).first(2051),modes);
        CHECK(!destinations[0].usesPairedReduction()&&destinations[1].usesPairedReduction());
        allocations=frees=0;tracking=true;
        const bool second=renderKernel(destinations[0],std::span(x).subspan(2051),packet)&&renderKernel(destinations[1],std::span(y).subspan(2051),packet);
        tracking=false;CHECK(second&&allocations==0&&frees==0);
        compareKernel(destinations[0],destinations[1],std::span(x).subspan(2051),std::span(y).subspan(2051),modes);
        destinations[0].reset();destinations[1].reset();x.resize(257);fillKernel(x,family,9);y=x;
        allocations=frees=0;tracking=true;
        const bool reset=renderKernel(destinations[0],x,packet)&&renderKernel(destinations[1],y,packet);
        tracking=false;CHECK(reset&&allocations==0&&frees==0);compareKernel(destinations[0],destinations[1],x,y,modes);
        CHECK(destinations[0].storageBytes()==destinations[1].storageBytes());++cases;
    }
    CHECK(cases==784);std::cout<<"PAIRED_REDUCTION_KERNEL cases="<<cases<<" expected=784\n";
}
void fallbackAndRejection() {
    auto descriptor=std::make_unique<PreparedModal>();DecayCurve curve;CHECK(prepareModal(curve,.5,48000,256,*descriptor));
    // A valid custom excitation must keep the generic kernel in both selections.
    descriptor->bReal[0]=.5;descriptor->bImag[0]=.125;
    ModalTank a,b;CHECK(a.prepareStorage(48000,ModalBackend::avx2,false)&&b.prepareStorage(48000,ModalBackend::avx2,true));
    CHECK(a.configure(*descriptor)&&b.configure(*descriptor));CHECK(!a.usesPairedReduction()&&!b.usesPairedReduction());
    std::vector<Frame> x(1031),y;fillKernel(x,0,7);y=x;CHECK(renderKernel(a,x,17)&&renderKernel(b,y,511));compareKernel(a,b,x,y,256);
    const auto before=a.modeState(0);auto bad=std::make_unique<PreparedModal>(*descriptor);bad->pickup=std::numeric_limits<double>::quiet_NaN();
    CHECK(!a.configure(*bad)&&a.prepared()==*descriptor);CHECK(same(before.real,a.modeState(0).real)&&same(before.imag,a.modeState(0).imag));
    std::array<Frame,513> oversized{};CHECK(!a.processBlock(oversized));CHECK(same(before.real,a.modeState(0).real)&&same(before.imag,a.modeState(0).imag));
    for(auto backend:{ModalBackend::scalar,ModalBackend::sse2}) {
        ModalTank fallback;CHECK(fallback.prepareStorage(48000,backend,true)&&fallback.configure(*descriptor));CHECK(!fallback.usesPairedReduction());
    }
}
}
int main() {
    CHECK(ModalServiceOptions{}.pairedReduction);
    if(!dispatchWitness()){std::cout<<"SPACE_PAIRED_REDUCTION checks="<<checks<<" failures="<<failures<<" fail_fast=1\n";return 1;}
    kernelPairs();fallbackAndRejection();
    for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.,768000.})for(std::size_t modes:{256U,512U,1024U,2048U})for(std::size_t parent:{64U,512U})for(unsigned factor:{1U,2U}) {
        if(rate*factor>768000)continue;paired(rate,modes,parent,factor,1);
        if(failures){std::cout<<"SPACE_PAIRED_REDUCTION checks="<<checks<<" failures="<<failures<<" fail_fast=1\n";return 1;}
    }
    paired(48000,256,512,1,255);paired(48000,256,64,2,255);
    std::cout<<"SPACE_PAIRED_REDUCTION checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

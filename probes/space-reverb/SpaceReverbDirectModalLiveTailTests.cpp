#include "ModalLiveTail.h"
#include "ModalField.h"
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"DIRECT_MODAL_LIVE_TAIL_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;
#if defined(AIRACK_MODAL_128_WITNESS)
constexpr std::size_t testModes=128,testMaximumModes=128;
constexpr std::array<std::size_t,1> testCounts{128};
#elif defined(AIRACK_MODAL_192_WITNESS)
constexpr std::size_t testModes=192,testMaximumModes=192;
constexpr std::array<std::size_t,1> testCounts{192};
#elif defined(AIRACK_MODAL_256_WITNESS)
constexpr std::size_t testModes=256,testMaximumModes=256;
constexpr std::array<std::size_t,1> testCounts{256};
#elif defined(AIRACK_MODAL_512_WITNESS)
constexpr std::size_t testModes=512,testMaximumModes=512;
constexpr std::array<std::size_t,1> testCounts{512};
#else
constexpr std::size_t testModes=1024,testMaximumModes=2048;
constexpr std::array<std::size_t,2> testCounts{1024,2048};
#endif
using Frame=ModalRoom::Frame;
constexpr unsigned all=(1U<<12)-1;
bool close(double a,long double b){return std::abs(static_cast<long double>(a)-b)<=2e-11L*std::max(1.L,std::abs(b));}
void prepareAndBounds(){
    auto p=std::make_unique<PreparedModalRoom>();unsigned configurations=0;double shortest=1e9,longest=0;
    for(double rate:{8000.,44100.,48000.,96000.,768000.})for(std::size_t count:testCounts)for(unsigned kind=0;kind<4;++kind){
        Settings s;if(kind==1)s.decay.decaySeconds=.1;if(kind==2){s.decay.decaySeconds=20;s.decay.bands[0]={true,false,DecayShape::lowShelf,1000,4,1};}if(kind==3){s.decay.bands[0]={true,false,DecayShape::bell,250,.25,4};s.decay.bands[1]={true,false,DecayShape::highShelf,4000,2,.5};}
        ModalTailContext c;PreparedModalTail room,post;PreparedModalPost coefficients;CHECK(prepareModalTailContext(rate,count,c));CHECK(prepareModalRoom(s.decay,s.size,rate,count,*p));CHECK(prepareModalPost(s,rate,coefficients));CHECK(prepareModalRoomTail(c,*p,room));CHECK(prepareModalPostTail(c,coefficients,post));
        CHECK(c.rate()==rate&&c.modes()==count);CHECK(room.mask()&(1U<<11));CHECK(post.mask()==all);
        long double r=0,trace=0;for(std::size_t k=0;k<count;++k){r=std::max(r,std::hypot(static_cast<long double>(p->modal.rc[k]),static_cast<long double>(p->modal.rs[k])));const auto w=p->modal.inputWeights[k/4];trace+=(static_cast<long double>(p->modal.bReal[k])*p->modal.bReal[k]+static_cast<long double>(p->modal.bImag[k])*p->modal.bImag[k])*(static_cast<long double>(w[0])*w[0]+static_cast<long double>(w[1])*w[1]);}
        CHECK(room.maximumRadius()>=r);CHECK(room.maximumRadius()-r<1e-12);CHECK(room.injectionSquaredNorm()>=trace);CHECK(close(room.injectionSquaredNorm(),trace));
        ModalLiveTail history;CHECK(history.configure(c,room,post));ModalTailReport report;CHECK(history.report(report));CHECK(report.passed&&report.frames>c.support());CHECK(report.seconds==double(report.frames)/rate);CHECK(report.peakExponent==0&&report.sourceMultiplicity==1);
        std::uint64_t smallest=UINT64_MAX;for(unsigned b=0;b<12;++b){const bool usable=room.maximumRadius()<c.radius(b);CHECK(bool(room.mask()&(1U<<b))==usable);if(!(report.mask&(1U<<b)))continue;
            const long double gain=p->modal.pickup*std::sqrt(static_cast<long double>(count)*trace)*std::pow(1.4L,8)/(1-static_cast<long double>(room.maximumRadius())/c.radius(b))+.68L;
            CHECK(std::exp(static_cast<long double>(room.logGain(b)))>=gain);CHECK(room.logGain(b)-std::log(gain)<1e-8L);
            const long double exponent=report.logGains[b]-(static_cast<long double>(report.circleFrames[b]-report.support)-1)*(-std::log(static_cast<long double>(c.radius(b))));CHECK(exponent<=std::log(1e-6L));smallest=std::min(smallest,report.circleFrames[b]);
            std::cout<<"LIVE_TAIL_CIRCLE rate="<<rate<<" modes="<<count<<" kind="<<kind<<" bin="<<b<<" rho="<<c.radius(b)<<" room="<<room.logGain(b)<<" post="<<post.logGain(b)<<" log_gain="<<report.logGains[b]<<" frames="<<report.circleFrames[b]<<" support="<<report.support<<'\n';}
        CHECK(report.frames==smallest);if(kind==0){CHECK(report.seconds<25);shortest=std::min(shortest,report.seconds);}if(kind==2){CHECK(report.seconds>40);longest=std::max(longest,report.seconds);}
        std::cout<<"LIVE_TAIL_CASE rate="<<rate<<" modes="<<count<<" kind="<<kind<<" mask="<<report.mask<<" bin="<<report.selectedBin<<" frames="<<report.frames<<" support="<<report.support<<" peak="<<report.peakExponent<<'\n';++configurations;
    }
    std::cout<<"LIVE_TAIL_METADATA configurations="<<configurations<<" default_min_seconds="<<shortest<<" clipped_max_seconds="<<longest<<'\n';
}
void postBounds(){
    unsigned configurations=0;for(double rate:{8000.,48000.,768000.})for(auto shape:{PostShape::bell,PostShape::lowShelf,PostShape::highShelf,PostShape::lowCut,PostShape::highCut})for(double q:{.499999999999,.5,.500000000001,4.}){
        Settings s;for(auto& band:s.post)band={true,false,shape,20,18,q,q,CutSlope::db24};PreparedModalPost p;ModalTailContext c;PreparedModalTail t;CHECK(prepareModalTailContext(rate,testModes,c));CHECK(prepareModalPost(s,rate,p));CHECK(prepareModalPostTail(c,p,t));CHECK(t.mask()&(1U<<11));
        for(unsigned b=0;b<12;++b){double sum=0;bool usable=true;for(std::size_t j=0;j<p.coefficients.count;++j){const auto one=boundModalPostSection(p.coefficients.stages[j],c.radius(b));usable=usable&&one.passed;sum+=one.logL1;}CHECK(bool(t.mask()&(1U<<b))==usable);if(usable)CHECK(t.logGain(b)>=std::max(0.,sum)&&t.logGain(b)-std::max(0.,sum)<1e-8);}
        ++configurations;
    }std::cout<<"LIVE_TAIL_POST configurations="<<configurations<<'\n';
}
void lifecycle(){
    ModalTailContext c,other,empty;CHECK(prepareModalTailContext(48000,testModes,c));CHECK(prepareModalTailContext(96000,testModes,other));CHECK(!prepareModalTailContext(7999,testModes,c));CHECK(c.rate()==48000);CHECK(!prepareModalTailContext(48000,768,c));CHECK(!prepareModalTailContext(NAN,testModes,c));
    auto p=std::make_unique<PreparedModalRoom>();PreparedModalPost eq;PreparedModalTail room,post,missing;CHECK(prepareModalRoom({},.5,48000,testModes,*p));CHECK(prepareModalPost({},48000,eq));CHECK(prepareModalRoomTail(c,*p,room));CHECK(prepareModalPostTail(c,eq,post));const auto saved=room;
    p->modal.pickup*=2;CHECK(!prepareModalRoomTail(c,*p,room));CHECK(room==saved);p->modal.pickup*=.5;p->modal.rc[0]=1;CHECK(!prepareModalRoomTail(c,*p,room));CHECK(room==saved);CHECK(!prepareModalRoomTail(other,*p,room));CHECK(!prepareModalRoomTail(empty,*p,room));CHECK(!prepareModalPostTail(other,eq,room));CHECK(room==saved);eq.coefficients.stages[0].a1=NAN;eq.coefficients.count=1;CHECK(!prepareModalPostTail(c,eq,room));CHECK(room==saved);
    ModalLiveTail h;ModalTailReport r,unchanged;r.frames=123;unchanged=r;CHECK(!h.report(r));CHECK(r==unchanged);CHECK(!h.observePeak(1));CHECK(!h.configure(empty,room,post));CHECK(!h.configure(c,post,room));CHECK(h.configure(c,room,post));CHECK(h.report(r));const auto initial=r;
    for(auto pair:std::array<std::pair<double,int>,12>{{{0,0},{std::numeric_limits<double>::denorm_min(),0},{.5,0},{1,0},{std::nextafter(1.,2.),1},{2,1},{std::nextafter(2.,3.),2},{3,2},{4,2},{std::ldexp(1.,1000),1000},{std::numeric_limits<double>::max(),1024},{1,1024}}}){allocations=frees=0;tracking=true;const bool ok=h.observePeak(pair.first)&&h.report(r);tracking=false;CHECK(ok);CHECK(r.peakExponent==pair.second);CHECK(allocations==0&&frees==0);}
    unchanged=r;CHECK(!h.observePeak(-1));CHECK(!h.observePeak(INFINITY));CHECK(!h.observePeak(NAN));CHECK(!h.admitRoom(post));CHECK(!h.admitPost(room));CHECK(!h.admitRoom(missing));CHECK(h.report(r));CHECK(r==unchanged);
    CHECK(h.reset(room,post));CHECK(h.report(r));CHECK(r==initial);Settings slow;slow.decay.decaySeconds=20;CHECK(prepareModalRoom(slow.decay,1,48000,testModes,*p));PreparedModalTail longRoom;CHECK(prepareModalRoomTail(c,*p,longRoom));CHECK(h.admitRoom(longRoom));CHECK(h.report(r));CHECK(r.frames>initial.frames);const auto longReport=r;CHECK(h.admitRoom(room));CHECK(h.report(r));CHECK(r==longReport);CHECK(!h.reset(post,room));CHECK(h.report(r));CHECK(r==longReport);
    ModalLiveTail second;CHECK(second.configure(c,room,post));std::array<const ModalLiveTail*,2> chain{&h,&second};CHECK(composeModalLiveTail(chain,8,r));CHECK(r.support==2*c.support()&&r.sourceMultiplicity==8&&r.frames>longReport.frames);for(unsigned b=0;b<12;++b)if(r.mask&(1U<<b)){CHECK(r.logGains[b]>=longReport.logGains[b]+initial.logGains[b]);CHECK(r.circleFrames[b]>longReport.circleFrames[b]);std::cout<<"LIVE_TAIL_COMPOSITION bin="<<b<<" rho="<<c.radius(b)<<" node0="<<longReport.logGains[b]<<" node1="<<initial.logGains[b]<<" log_gain="<<r.logGains[b]<<" frames="<<r.circleFrames[b]<<" support="<<r.support<<'\n';}
    unchanged=r;CHECK(!composeModalLiveTail(chain,0,r));CHECK(!composeModalLiveTail(chain,9,r));CHECK(r==unchanged);std::array<const ModalLiveTail*,1> null{nullptr};CHECK(!composeModalLiveTail(null,1,r));CHECK(!composeModalLiveTail({},1,r));CHECK(r==unchanged);
    allocations=frees=0;tracking=true;bool ok=true;for(unsigned n=0;n<1000;++n){ok=h.admitRoom(room)&&h.admitPost(post)&&h.observePeak(2)&&h.report(r)&&composeModalLiveTail(chain,8,r)&&ok;}ok=h.reset(room,post)&&ok;tracking=false;CHECK(ok);CHECK(allocations==0&&frees==0);
}
struct Rig {
    std::array<ModalRoom,2> room;ModalWetOuter wet;ModalFinalOuter final;ModalLiveTail tail;ModalTailContext context;PreparedModalTail roomTail,postTail;unsigned active=0;
    bool prepare(const Settings& s,double rate){auto r=std::make_unique<PreparedModalRoom>();PreparedModalWet w;PreparedModalFinal f;return prepareModalRoom(s.decay,s.size,rate,testModes,*r)&&prepareModalWet(s,rate,w)&&prepareModalFinal(s,rate,f)&&prepareModalTailContext(rate,testModes,context)&&prepareModalRoomTail(context,*r,roomTail)&&prepareModalPostTail(context,w.post,postTail)&&tail.configure(context,roomTail,postTail)&&room[0].prepareStorage(rate)&&room[1].prepareStorage(rate)&&wet.prepareStorage(rate)&&final.prepareStorage(rate)&&room[0].configure(*r)&&wet.configure(w)&&final.configure(f);}
    bool change(const Settings& s){auto r=std::make_unique<PreparedModalRoom>();PreparedModalPost post;PreparedModalEarly early;PreparedModalTail rt,pt;if(!prepareModalRoom(s.decay,s.size,context.rate(),testModes,*r)||!prepareModalPost(s,context.rate(),post)||!prepareModalEarly(s.size,context.rate(),early)||!prepareModalRoomTail(context,*r,rt)||!prepareModalPostTail(context,post,pt))return false;
        if(!room[1-active].adoptCopied(*r,room[active])||!wet.setEarlyGeometries(early,early)||!wet.adoptPost(post)||!tail.admitRoom(rt)||!tail.admitPost(pt))return false;active=1-active;roomTail=rt;postTail=pt;return true;}
    bool run(std::span<Frame> frames,bool postEnd=false){double peak=0;for(auto f:frames)for(double x:f)peak=std::max(peak,std::abs(x));if(!tail.observePeak(peak)||!final.capture(frames)||!wet.excite(frames)||!room[active].processBlock(frames,wet.characters())||!wet.mixRooms(frames))return false;
        if(wet.postPending()){std::array<double,512> alpha{};if(postEnd)std::fill_n(alpha.begin(),frames.size(),1.);if(!wet.post(frames,std::span(alpha).first(frames.size())))return false;if(postEnd&&!wet.commitPost())return false;}else if(!wet.post(frames))return false;return final.finish(frames);}
};
void rendered(){
    for(double rate:{8000.,48000.})for(unsigned modules:{1U,2U}){Settings s;s.decay.decaySeconds=.1;s.predelayMs=0;s.character=1;s.distance=.5;s.width=2;s.mix=1;std::array<Rig,2> rig;bool ready=true;for(unsigned i=0;i<modules;++i)ready=rig[i].prepare(s,rate)&&ready;CHECK(ready);if(!ready)continue;
        std::array<Frame,64> audio{};audio.fill({.5,-1});bool ok=true;for(unsigned block=0;block<8;++block){audio.fill({.5,-1});for(unsigned i=0;i<modules;++i)ok=rig[i].run(audio)&&ok;}
        Settings changed=s;changed.size=1;changed.decay.decaySeconds=.2;changed.post[0]={true,false,PostShape::bell,1000,6,1,1,CutSlope::db12};for(unsigned i=0;i<modules;++i)ok=rig[i].change(changed)&&ok;for(unsigned block=0;block<2;++block){audio.fill({0,0});for(unsigned i=0;i<modules;++i)ok=rig[i].run(audio,block==1)&&ok;}
        for(unsigned change=0;change<3;++change){changed.size=double(change)/2;changed.decay.decaySeconds=change%2?.1:.2;changed.post[0].gainDb=change%2?-6:6;for(unsigned i=0;i<modules;++i)ok=rig[i].change(changed)&&ok;for(unsigned block=0;block<2;++block){audio.fill({0,0});for(unsigned i=0;i<modules;++i)ok=rig[i].run(audio,block==1)&&ok;}}
        std::array<const ModalLiveTail*,2> pointers{&rig[0].tail,&rig[1].tail};ModalTailReport report;CHECK(composeModalLiveTail(std::span(pointers).first(modules),1,report));CHECK(report.seconds<10);double peak=0;allocations=frees=0;tracking=true;
        for(std::uint64_t n=0;n<report.frames+4096;n+=audio.size()){audio.fill({0,0});for(unsigned i=0;i<modules;++i)ok=rig[i].run(audio)&&ok;if(n>=report.frames)for(auto f:audio)for(double v:f)peak=std::max(peak,std::abs(v));}tracking=false;
        CHECK(ok);CHECK(std::isfinite(peak)&&peak<1e-6);CHECK(allocations==0&&frees==0);std::cout<<"LIVE_TAIL_RENDER rate="<<rate<<" modules="<<modules<<" frames="<<report.frames<<" peak_after_stop="<<peak<<'\n';
    }
}
}
int main(){std::cout.precision(17);prepareAndBounds();postBounds();lifecycle();rendered();std::cout<<"SpaceReverbDirectModalLiveTail checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

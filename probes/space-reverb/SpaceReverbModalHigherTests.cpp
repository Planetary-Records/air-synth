#include "ModalService.h"
#include "ModalConverter.h"
#include <thread>
#include <atomic>
#include <chrono>
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"MODAL_HIGHER_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
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
using Frame=ModalService::Frame;
ModalSlotIdentities ids(){return {11,12,13,14,15,16,17,18};}
ModalServiceOptions options(unsigned factor){ModalServiceOptions o;o.threaded=false;o.factor=factor;return o;}
std::vector<Frame> input(std::size_t n,unsigned salt=0){std::vector<Frame> x(n);for(std::size_t k=0;k<n;++k)x[k]={k<5?.5:double(int((k+salt)*17%97)-48)/1024.,k<5?-.25:double(int((k+salt)*37%89)-44)/1024.};return x;}
bool bits(std::span<const Frame> a,std::span<const Frame> b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size_bytes())==0;}
bool silent(ModalService& service,const ModalSlotSettings& s,unsigned mask,std::size_t frames,std::size_t packet=17){if(!service.beginParent(s,mask,ids(),frames))return false;std::array<Frame,512> x{};bool ok=true;for(unsigned i=0;i<8;++i)if(mask&(1U<<i))for(std::size_t at=0;at<frames;at+=packet){auto span=std::span(x).first(std::min(packet,frames-at));std::fill(span.begin(),span.end(),Frame{0,0});ok=service.renderSlot(i,span)&&ok;}return service.endParent()&&ok;}
struct Reference {ModalRoom room;ModalWetOuter wet;ModalFinalOuter final;ModalWetConverter converter;unsigned factor=1;
    bool prepare(const ModalReadySlot& p,double rate,unsigned f){factor=f;PreparedModalConverter c;return prepareModalConverter(f,c)&&converter.configure(c)&&room.prepareStorage(rate)&&wet.prepareStorage(rate)&&final.prepareStorage(rate*f)&&room.configure(p.room)&&wet.configure(p.wet)&&final.configure(p.final);}
    bool run(std::span<Frame> physical){std::array<Frame,512> logical{};std::size_t count=0;if(!final.capture(physical))return false;if(factor==1)return wet.excite(physical)&&room.processBlock(physical,wet.characters())&&wet.mixRooms(physical)&&wet.post(physical)&&final.finish(physical);if(!converter.down(physical,logical,count))return false;auto host=std::span(logical).first(count);if(count&&(!wet.excite(host)||!room.processBlock(host,wet.characters())||!wet.mixRooms(host)||!wet.post(host)))return false;return converter.up(host,physical)&&final.finish(physical);}
};
void transport(){
    ModalTailContext plain,filtered;PreparedModalConverter converter;CHECK(prepareModalConverter(2,converter));CHECK(prepareModalTailContext(8000,testModes,plain));CHECK(prepareModalTailContext(8000,testModes,filtered,{converter.logicalSupport(),converter.logGain()}));CHECK(filtered.support()==plain.support()+127);const auto support=filtered.support();CHECK(!prepareModalTailContext(8000,testModes,filtered,{0,-1}));CHECK(!prepareModalTailContext(8000,testModes,filtered,{0,NAN}));CHECK(!prepareModalTailContext(8000,testModes,filtered,{UINT64_MAX,0}));CHECK(filtered.support()==support);
    auto p=std::make_unique<PreparedModalRoom>();Settings s;s.decay.decaySeconds=.1;CHECK(prepareModalRoom(s.decay,.5,8000,testModes,*p));PreparedModalPost eq;PreparedModalTail room,post;CHECK(prepareModalPost(s,8000,eq));CHECK(prepareModalRoomTail(plain,*p,room));CHECK(prepareModalPostTail(plain,eq,post));ModalLiveTail a,b;CHECK(a.configure(plain,room,post));CHECK(b.configure(filtered,room,post));ModalTailReport one,two,both;CHECK(a.report(one)&&b.report(two));const std::array<const ModalLiveTail*,2> chain{&a,&b};CHECK(composeModalLiveTail(chain,1,both));CHECK(both.support==one.support+two.support);CHECK(two.frames>one.frames);
    for(unsigned bin=0;bin<12;++bin)if(both.mask&(1U<<bin)){CHECK(std::abs(two.logGains[bin]-one.logGains[bin]-converter.logGain())<1e-12);CHECK(std::abs(both.logGains[bin]-one.logGains[bin]-two.logGains[bin]+1e-10)<1e-12);std::cout<<"MODAL_HIGHER_TRANSPORT bin="<<bin<<" rho="<<plain.radius(bin)<<" log_gain="<<both.logGains[bin]<<" frames="<<both.circleFrames[bin]<<" support="<<both.support<<'\n';}
}
void invalid(){ModalSlotSettings s;const auto identity=ids();CHECK(!prepareModalBundle(48000,testModes,1,s,identity,{},0));CHECK(!prepareModalBundle(48000,testModes,1,s,identity,{},3));CHECK(!prepareModalBundle(384001,testModes,1,s,identity,{},2));auto higher=prepareModalBundle(48000,testModes,1,s,identity,{},2),normal=prepareModalBundle(48000,testModes,1,s,identity);CHECK(bool(higher)&&bool(normal));if(!higher||!normal)return;CHECK(higher->factor()==2&&normal->factor()==1);CHECK(higher->slot(0).room.modal.sampleRate==48000&&higher->slot(0).wet.post.coefficients.sampleRate==48000&&higher->slot(0).final.duck.coefficients.sampleRate==96000);ModalService a,b;CHECK(a.prepareStorage(48000,testModes,options(1)));CHECK(b.prepareStorage(48000,testModes,options(2)));CHECK(!a.installBundle(*higher,1));CHECK(!b.installBundle(*normal,1));CHECK(a.installBundle(*normal,1)&&b.installBundle(*higher,1));CHECK(!a.beginParent(s,1,identity,1025));CHECK(!b.beginParent(s,1,identity,2049));ModalTailReport out;out.frames=99;const auto saved=out;const std::array<std::size_t,1> slot{0},bad{8};CHECK(!b.composeTail({},1,out));CHECK(!b.composeTail(bad,1,out));CHECK(out==saved);CHECK(b.beginParent(s,1,identity,1));CHECK(!b.composeTail(slot,1,out));std::array<Frame,1> raw{{{1,-1}}};CHECK(b.renderSlot(0,raw));CHECK(b.endParent());CHECK(b.composeTail(slot,1,out));ModalServiceStatus status;CHECK(b.snapshot(status));CHECK(status.slots[0].tailFrames==2*out.frames);CHECK(b.resetSlot(0));CHECK(silent(b,s,1,2048,512));}
void reference(){unsigned configurations=0;std::uint64_t frames=0;for(unsigned factor:{1U,2U})for(double rate:{8000.,44100.,48000.,96000.})for(std::size_t count:testCounts){
    ModalSlotSettings s;for(unsigned i=0;i<2;++i){s[i].decay.decaySeconds=.2;s[i].predelayMs=1.25;s[i].character=1;s[i].mix=i==1?0:1;s[i].width=0;s[i].duckRangeDb=i?0:24;s[i].duckThresholdDb=-36;s[i].duckAttackMs=.1;s[i].post[0]={true,false,PostShape::bell,4000,6,2,1,CutSlope::db12};}
    auto bundle=prepareModalBundle(rate,count,8,s,ids(),{},factor);CHECK(bool(bundle));if(!bundle)continue;for(std::size_t packet:{1U,3U,17U,64U,511U,512U}){ModalService service;CHECK(service.prepareStorage(rate,count,options(factor)));CHECK(service.installBundle(*bundle,8));std::array<Reference,2> rig;std::array<std::vector<Frame>,2> actual,expected,raw;for(unsigned i=0;i<2;++i){CHECK(rig[i].prepare(bundle->slot(i),rate,factor));CHECK(std::abs(bundle->slot(i).final.duck.coefficients.attack-std::exp(-1/(rate*factor*.001*s[i].duckAttackMs)))<2e-15);CHECK(bundle->slot(i).wet.post.coefficients.effectiveHz[0]==std::min(4000.,.45*rate));actual[i]=raw[i]=input(1025,i);expected[i]=actual[i];for(std::size_t at=0;at<1025;at+=37)CHECK(rig[i].run(std::span(expected[i]).subspan(at,std::min<std::size_t>(37,1025-at))));}
        bool ok=true;allocations=frees=0;tracking=true;for(std::size_t parent=0;parent<2;++parent){const std::size_t start=parent?513:0,n=parent?512:513;ok=service.beginParent(s,3,ids(),n)&&ok;for(unsigned i=0;i<2;++i)for(std::size_t at=0;at<n;at+=packet)ok=service.renderSlot(i,std::span(actual[i]).subspan(start+at,std::min(packet,n-at)))&&ok;ok=service.endParent()&&ok;}tracking=false;CHECK(ok);CHECK(allocations==0&&frees==0);for(unsigned i=0;i<2;++i)CHECK(bits(actual[i],expected[i]));CHECK(bits(actual[1],raw[1]));bool mono=true;for(auto f:actual[0])mono=mono&&f[0]==f[1];CHECK(mono);++configurations;frames+=2050;}
    }std::cout<<"MODAL_HIGHER_REFERENCE configurations="<<configurations<<" physical_slot_frames="<<frames<<'\n';}
void endpoint(){ModalSlotSettings s;auto bundle=prepareModalBundle(8000,testModes,4,s,ids(),{},2);CHECK(bool(bundle));if(!bundle)return;ModalService service;CHECK(service.prepareStorage(8000,testModes,options(2)));CHECK(service.installBundle(*bundle,4));s[0].size=1;CHECK(silent(service,s,1,1));CHECK(service.pumpHeavy());CHECK(silent(service,s,1,1));ModalServiceStatus status;CHECK(service.snapshot(status));CHECK(status.ticketSlot==0&&status.slots[0].appliedRoom<status.slots[0].requestedRoom);CHECK(silent(service,s,1,79));CHECK(service.snapshot(status));CHECK(status.ticketSlot==0);CHECK(silent(service,s,1,1));CHECK(service.snapshot(status));CHECK(status.ticketSlot==0);CHECK(silent(service,s,1,1));CHECK(service.snapshot(status));CHECK(status.ticketSlot==8&&status.slots[0].appliedRoom==status.slots[0].requestedRoom);}
void parallel(){ModalSlotSettings s;auto bundle=prepareModalBundle(48000,testModes,4,s,ids(),{},2);CHECK(bool(bundle));if(!bundle)return;ModalService a,b;CHECK(a.prepareStorage(48000,testModes,options(2))&&b.prepareStorage(48000,testModes,options(2)));CHECK(a.installBundle(*bundle,4)&&b.installBundle(*bundle,4));bool exact=true,clean=true;for(unsigned n=0;n<24;++n){s[0].size=.02*n;s[0].mix=n>12?0:1;s[0].duckAttackMs=1+n;s[0].post[0]={true,false,PostShape::bell,1000,double(n%7),1,1,CutSlope::db12};std::array<std::vector<Frame>,2> x{input(65,n),input(65,n+3)},y=x;CHECK(a.beginParent(s,3,ids(),65)&&b.beginParent(s,3,ids(),65));for(unsigned i=0;i<2;++i)CHECK(a.renderSlot(i,x[i]));std::array<bool,2> ok{},noAllocation{};std::array<std::thread,2> threads;for(unsigned i=0;i<2;++i)threads[i]=std::thread([&,i]{allocations=frees=0;tracking=true;ok[i]=true;for(std::size_t at=0;at<65;++at)ok[i]=b.renderSlot(i,std::span(y[i]).subspan(at,1))&&ok[i];tracking=false;noAllocation[i]=allocations==0&&frees==0;});for(auto& t:threads)t.join();for(unsigned i=0;i<2;++i){exact=exact&&ok[i]&&bits(x[i],y[i]);clean=clean&&noAllocation[i];}CHECK(a.endParent()&&b.endParent());CHECK(a.pumpHeavy()==b.pumpHeavy());CHECK(a.pumpLight()==b.pumpLight());ModalServiceStatus one,two;CHECK(a.snapshot(one)&&b.snapshot(two));exact=exact&&one==two;CHECK(one.admissionsThisParent<=1&&one.maximumRoomsThisParent<=9);}CHECK(exact&&clean);std::cout<<"MODAL_HIGHER_PARALLEL parents=24 physical_frames_per_parent=65 slots=2 exact=1\n";}
void tails(){for(unsigned modules:{1U,2U}){ModalSlotSettings s;for(auto& p:s){p.decay.decaySeconds=.1;p.predelayMs=0;p.mix=1;p.width=2;p.character=1;}auto bundle=prepareModalBundle(8000,testModes,4,s,ids(),{},2);CHECK(bool(bundle));if(!bundle)continue;ModalService service;CHECK(service.prepareStorage(8000,testModes,options(2)));CHECK(service.installBundle(*bundle,4));std::array<Frame,64> audio;audio.fill({.5,-1});const unsigned mask=(1U<<modules)-1;CHECK(service.beginParent(s,mask,ids(),64));for(unsigned i=0;i<modules;++i)CHECK(service.renderSlot(i,audio));CHECK(service.endParent());std::array<std::size_t,2> indices{0,1};ModalTailReport report;CHECK(service.composeTail(std::span(indices).first(modules),1,report));CHECK(report.frames>0&&report.seconds<10);const auto physicalStop=2*report.frames;double maximum=0;bool ok=true;allocations=frees=0;tracking=true;for(std::uint64_t at=0;at<physicalStop+2048;at+=64){audio.fill({0,0});ok=service.beginParent(s,mask,ids(),64)&&ok;for(unsigned i=0;i<modules;++i)ok=service.renderSlot(i,audio)&&ok;ok=service.endParent()&&ok;if(at>=physicalStop)for(auto f:audio)for(double v:f)maximum=std::max(maximum,std::abs(v));}tracking=false;CHECK(ok&&maximum<1e-6&&std::isfinite(maximum));CHECK(allocations==0&&frees==0);std::cout<<"MODAL_HIGHER_TAIL modules="<<modules<<" physical_stop="<<physicalStop<<" peak_after_stop="<<maximum<<'\n';}}

struct CancelWitness {std::atomic<bool> entered=false,cancelled=false,claimed=false;static void checkpoint(void* p,ModalPreparationTag tag,detail::PreparationControl control) noexcept {auto& self=*static_cast<CancelWitness*>(p);if(tag.domain!=ModalPreparationDomain::room||self.claimed.exchange(true))return;self.entered.store(true,std::memory_order_release);while(!control.isCancelled(control.context))std::this_thread::yield();self.cancelled.store(true,std::memory_order_release);}
    bool wait(std::atomic<bool>& value){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!value.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();return value.load(std::memory_order_acquire);}};
void realWorkers(){ModalSlotSettings s;auto bundle=prepareModalBundle(48000,testModes,5,s,ids(),{},2);CHECK(bool(bundle));if(!bundle)return;
    {CancelWitness witness;auto o=options(2);o.threaded=true;o.checkpoint=&CancelWitness::checkpoint;o.checkpointContext=&witness;ModalService service;CHECK(service.prepareStorage(48000,testModes,o));CHECK(service.installBundle(*bundle,5));s[0].size=1;s[0].duckAttackMs=1;CHECK(silent(service,s,1,65));CHECK(witness.wait(witness.entered));CHECK(service.resetSlot(0));CHECK(witness.wait(witness.cancelled));const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool ready=false;while(!ready&&std::chrono::steady_clock::now()<deadline){allocations=frees=0;tracking=true;const bool ok=silent(service,s,1,65);ModalServiceStatus status;const bool read=service.snapshot(status);tracking=false;CHECK(ok&&read);CHECK(allocations==0&&frees==0);ready=read&&status.slots[0].appliedRoom==status.slots[0].requestedRoom&&status.slots[0].appliedDuck==status.slots[0].requestedDuck;std::this_thread::yield();}CHECK(ready);}
    {s={};CancelWitness witness;auto o=options(2);o.threaded=true;o.checkpoint=&CancelWitness::checkpoint;o.checkpointContext=&witness;auto service=std::make_unique<ModalService>();CHECK(service->prepareStorage(48000,testModes,o));CHECK(service->installBundle(*bundle,5));s[0].size=.2;CHECK(silent(*service,s,1,65));CHECK(witness.wait(witness.entered));service.reset();CHECK(witness.cancelled.load(std::memory_order_acquire));}
    std::cout<<"MODAL_HIGHER_THREADED reset_cancellation=1 teardown_cancellation=1 physical_duck_worker=1\n";
}

void resources(){ModalSlotSettings s;auto bundle=prepareModalBundle(384000,testMaximumModes,4,s,ids(),{},2);CHECK(bool(bundle));if(!bundle)return;ModalService service;CHECK(service.prepareStorage(384000,testMaximumModes,options(2)));CHECK(service.installBundle(*bundle,4));CHECK(silent(service,s,255,2048,512));const auto bytes=service.storageBytes()+bundle->storageBytes();CHECK(bytes<=8*16*1024*1024);std::cout<<"MODAL_HIGHER_RESOURCE logical_rate=384000 physical_rate=768000 total_bytes="<<bytes<<'\n';}
}
int main(){std::cout.precision(17);transport();invalid();reference();endpoint();parallel();tails();realWorkers();resources();std::cout<<"SpaceReverbModalHigher checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

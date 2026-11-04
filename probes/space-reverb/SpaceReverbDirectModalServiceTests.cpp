#include "ModalService.h"
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <mutex>
#include <thread>
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"DIRECT_MODAL_SERVICE_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
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
ModalSlotIdentities identities(){return {101,102,103,104,105,106,107,108};}
ModalSlotSettings defaults(){return {};}
std::vector<Frame> input(std::size_t n,unsigned salt=0){std::vector<Frame> a(n);for(std::size_t k=0;k<n;++k)a[k]={k==0?.5:double(int((k+salt)%37)-18)/8192.,k==0?-.25:double(int((k+salt)%31)-15)/16384.};return a;}
bool bits(std::span<const Frame> a,std::span<const Frame> b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size_bytes())==0;}
bool callback(ModalService& service,const ModalSlotSettings& s,unsigned mask,const ModalSlotIdentities& ids,std::size_t n=512,std::size_t packet=64){
    if(!service.beginParent(s,mask,ids,n))return false;std::array<Frame,512> block{};bool ok=true;for(unsigned slot=0;slot<8;++slot)if(mask&(1U<<slot))for(std::size_t at=0;at<n;at+=packet){auto b=std::span(block).first(std::min(packet,n-at));std::fill(b.begin(),b.end(),Frame{0,0});ok=service.renderSlot(slot,b)&&ok;}return service.endParent()&&ok;
}
struct Reference {ModalRoom room;ModalWetOuter wet;ModalFinalOuter final;bool prepare(const ModalReadySlot& p,double rate){return room.prepareStorage(rate)&&wet.prepareStorage(rate)&&final.prepareStorage(rate)&&room.configure(p.room)&&wet.configure(p.wet)&&final.configure(p.final);}bool run(std::span<Frame> b){return final.capture(b)&&wet.excite(b)&&room.processBlock(b,wet.characters())&&wet.mixRooms(b)&&wet.post(b)&&final.finish(b);}};
void invalid(){
    auto s=defaults();auto ids=identities();CHECK(!prepareModalBundle(7999,testModes,1,s,ids));CHECK(!prepareModalBundle(48000,768,1,s,ids));CHECK(!prepareModalBundle(48000,testModes,0,s,ids));auto bad=ids;bad[2]=0;CHECK(!prepareModalBundle(48000,testModes,1,s,bad));s[7].post[5].shelfQ=5;CHECK(!prepareModalBundle(48000,testModes,1,s,ids));s=defaults();unsigned calls=0;detail::PreparationControl cancel{[](void* p) noexcept {return ++*static_cast<unsigned*>(p)>=3;},&calls};CHECK(!prepareModalBundle(48000,testModes,1,s,ids,cancel));CHECK(calls>=3);
    ModalService empty;ModalServiceStatus status;status.documentKey=999;auto retained=status;CHECK(!empty.snapshot(status));CHECK(status==retained);CHECK(!empty.beginParent(s,1,ids,64));CHECK(!empty.endParent());CHECK(!empty.resetSlot(0));CHECK(!empty.pumpHeavy()&&!empty.pumpLight());std::array<Frame,1> frame{{{1,-1}}},before=frame;CHECK(!empty.renderSlot(0,frame));CHECK(frame==before);CHECK(!empty.prepareStorage(0,testModes));
    auto bundle=prepareModalBundle(48000,testModes,9,s,ids);CHECK(bool(bundle));if(!bundle)return;CHECK(bundle->key()==9&&bundle->rate()==48000&&bundle->modes()==testModes);CHECK(bundle->slot(7).identity==ids[7]);ModalService service;CHECK(service.prepareStorage(48000,testModes,{false}));CHECK(!service.installBundle(*bundle,8));CHECK(!service.beginParent(s,1,ids,64));
    allocations=frees=0;tracking=true;const bool installed=service.installBundle(*bundle,9);tracking=false;CHECK(installed);CHECK(allocations==0&&frees==0);CHECK(service.snapshot(status));CHECK(status.documentKey==9&&status.completedParents==0);retained=status;
    s[1].mix=2;CHECK(!service.beginParent(s,1,ids,64));s=defaults();CHECK(!service.beginParent(s,256,ids,64));CHECK(!service.beginParent(s,1,ids,0));CHECK(!service.beginParent(s,1,ids,1025));CHECK(!service.beginParent(s,1,bad,64));CHECK(service.snapshot(status));CHECK(status==retained);
    CHECK(service.beginParent(s,1,ids,1));CHECK(!service.beginParent(s,1,ids,1));CHECK(!service.installBundle(*bundle,9));CHECK(!service.resetSlot(0));CHECK(!service.endParent());CHECK(!service.renderSlot(1,frame));CHECK(!service.renderSlot(8,frame));frame[0][0]=NAN;CHECK(!service.renderSlot(0,frame));CHECK(std::isnan(frame[0][0]));frame=before;CHECK(service.renderSlot(0,frame));CHECK(!service.renderSlot(0,frame));CHECK(service.endParent());CHECK(!service.endParent());CHECK(service.snapshot(status));CHECK(status.completedParents==1);
    auto other=prepareModalBundle(96000,testModes,10,s,ids);CHECK(bool(other));if(other)CHECK(!service.installBundle(*other,10));CHECK(service.snapshot(status));CHECK(status.documentKey==9);
}
void staticAudio(){
    std::uint64_t configurations=0,frames=0;for(double rate:{8000.,48000.,96000.})for(std::size_t count:testCounts){
        auto settings=defaults();auto ids=identities();for(unsigned i=0;i<8;++i){auto& s=settings[i];s.mix=i==0?0:i==1?1:.35;s.width=i%3;s.character=i%2?0:1;s.predelayMs=double(i)*1.25;s.duckRangeDb=i%2?12:0;s.decay.decaySeconds=.3+double(i)*.1;if(i>3)s.post[0]={true,false,PostShape::bell,1000,3,1,1,CutSlope::db12};}
        auto bundle=prepareModalBundle(rate,count,123,settings,ids);CHECK(bool(bundle));if(!bundle)continue;
        for(std::size_t packet:{1U,17U,64U,512U}){ModalService service;CHECK(service.prepareStorage(rate,count,{false}));CHECK(service.installBundle(*bundle,123));std::array<Reference,8> references;std::array<std::vector<Frame>,8> actual,expected;
            for(unsigned i=0;i<8;++i){CHECK(references[i].prepare(bundle->slot(i),rate));actual[i]=input(512,i);expected[i]=actual[i];for(std::size_t at=0;at<512;at+=128)CHECK(references[i].run(std::span(expected[i]).subspan(at,128)));}
            bool ok=true;allocations=frees=0;tracking=true;ok=service.beginParent(settings,255,ids,512)&&ok;for(unsigned i=0;i<8;++i)for(std::size_t at=0;at<512;at+=packet)ok=service.renderSlot(i,std::span(actual[i]).subspan(at,std::min(packet,512-at)))&&ok;ok=service.endParent()&&ok;tracking=false;
            CHECK(ok);CHECK(allocations==0&&frees==0);for(unsigned i=0;i<8;++i)CHECK(bits(actual[i],expected[i]));auto original=input(512,0);CHECK(bits(actual[0],original));ModalServiceStatus status;CHECK(service.snapshot(status));CHECK(status.maximumRoomsThisParent==8&&status.admissionsThisParent==0);for(auto slot:status.slots)CHECK(slot.tailFrames>0);++configurations;frames+=4096;
        }
    }std::cout<<"MODAL_SERVICE_REFERENCE configurations="<<configurations<<" frames="<<frames<<'\n';
}
void fairAndCoalesced(){
    auto s=defaults();auto ids=identities();auto bundle=prepareModalBundle(48000,testModes,77,s,ids);CHECK(bool(bundle));if(!bundle)return;ModalService service;CHECK(service.prepareStorage(48000,testModes,{false}));CHECK(service.installBundle(*bundle,77));
    for(unsigned i=0;i<8;++i){s[i].size=.1*double(i);s[i].decay.decaySeconds=.4+.1*i;s[i].post[0]={true,false,PostShape::bell,1000,3,1,1,CutSlope::db12};s[i].duckRangeDb=12;s[i].mix=0;}
    CHECK(callback(service,s,255,ids));unsigned heavy=0,light=0;while(service.pumpHeavy()&&heavy<32)++heavy;while(service.pumpLight()&&light<64)++light;CHECK(heavy==8&&light==16);
    unsigned admissions=0;std::array<bool,16> seen{};bool ok=true;for(unsigned n=0;n<16;++n){allocations=frees=0;tracking=true;ok=callback(service,s,255,ids,512,n%2?17:512)&&ok;tracking=false;CHECK(allocations==0&&frees==0);ModalServiceStatus status;CHECK(service.snapshot(status));CHECK(status.admissionsThisParent==1&&status.maximumRoomsThisParent<=9);admissions+=unsigned(status.admissionsThisParent);for(unsigned i=0;i<8;++i){seen[2*i]=seen[2*i]||status.slots[i].appliedRoom==status.slots[i].requestedRoom;seen[2*i+1]=seen[2*i+1]||status.slots[i].appliedPost==status.slots[i].requestedPost;CHECK(status.slots[i].appliedDuck==status.slots[i].requestedDuck);}}
    CHECK(ok&&admissions==16);for(bool value:seen)CHECK(value);ModalServiceStatus final;CHECK(service.snapshot(final));for(auto slot:final.slots){CHECK(slot.appliedRoom==slot.requestedRoom);CHECK(slot.appliedPost==slot.requestedPost);}
    s[0].size=.91;CHECK(callback(service,s,1,ids));CHECK(service.pumpHeavy());s[0].size=.92;s[0].mix=0;s[0].width=0;CHECK(callback(service,s,1,ids));CHECK(service.snapshot(final));CHECK(final.slots[0].appliedRoom+1==final.slots[0].requestedRoom);CHECK(service.pumpHeavy());CHECK(callback(service,s,1,ids));CHECK(service.snapshot(final));CHECK(final.slots[0].appliedRoom==final.slots[0].requestedRoom);
    for(unsigned n=0;n<20;++n){s[0].size=.2+.01*n;CHECK(callback(service,s,1,ids));}CHECK(service.pumpHeavy());CHECK(!service.pumpHeavy());CHECK(callback(service,s,1,ids));CHECK(service.snapshot(final));CHECK(final.slots[0].appliedRoom==final.slots[0].requestedRoom);
    const auto epoch=final.slots[0].epoch;CHECK(service.resetSlot(0));CHECK(callback(service,s,1,ids));while(service.pumpHeavy()){}while(service.pumpLight()){}CHECK(callback(service,s,1,ids));CHECK(service.snapshot(final));CHECK(final.slots[0].epoch>epoch);CHECK(final.slots[0].appliedRoom==final.slots[0].requestedRoom);CHECK(final.slots[0].appliedPost==final.slots[0].requestedPost);CHECK(final.slots[0].peakExponent==0);
    std::cout<<"MODAL_SERVICE_FAIR admissions="<<admissions<<" heavy_preparations="<<heavy<<" light_preparations="<<light<<'\n';
}
void transitions(){
    auto s=defaults();auto ids=identities();auto bundle=prepareModalBundle(48000,testModes,2,s,ids);CHECK(bool(bundle));if(!bundle)return;ModalService a,b;CHECK(a.prepareStorage(48000,testModes,{false})&&b.prepareStorage(48000,testModes,{false}));CHECK(a.installBundle(*bundle,2)&&b.installBundle(*bundle,2));
    bool same=true;for(unsigned n=0;n<40;++n){if(n%5==0){s[0].size=.02*n;s[0].decay.decaySeconds=.2+.01*n;s[0].character=double(n%2);s[0].predelayMs=double(n);s[0].post[0]={true,false,PostShape::bell,1000,double(n%7),1,1,CutSlope::db12};s[0].duckAttackMs=1+n;s[0].duckRangeDb=n%2?0:12;}
        if(n==21)s[0].mix=0;unsigned mask=n==17?2:3;CHECK(a.beginParent(s,mask,ids,64)&&b.beginParent(s,mask,ids,64));for(unsigned slot=0;slot<2;++slot)if(mask&(1U<<slot)){auto x=input(64,n),y=x;CHECK(a.renderSlot(slot,x));for(std::size_t at=0;at<64;at+=17)CHECK(b.renderSlot(slot,std::span(y).subspan(at,std::min<std::size_t>(17,64-at))));same=same&&bits(x,y);}
        CHECK(a.endParent()&&b.endParent());CHECK(a.pumpHeavy()==b.pumpHeavy());CHECK(a.pumpLight()==b.pumpLight());ModalServiceStatus sa,sb;CHECK(a.snapshot(sa)&&b.snapshot(sb));same=same&&sa==sb;CHECK(sa.admissionsThisParent<=1&&sa.maximumRoomsThisParent<=9);
    }CHECK(same);
    auto replacementIds=ids;replacementIds[0]+=100;auto next=prepareModalBundle(48000,testModes,3,s,replacementIds);CHECK(bool(next));if(next){CHECK(!a.beginParent(s,3,replacementIds,64));CHECK(a.installBundle(*next,3));CHECK(callback(a,s,3,replacementIds,64));ModalServiceStatus status;CHECK(a.snapshot(status));CHECK(status.documentKey==3&&status.slots[0].identity==replacementIds[0]);}
    CHECK(callback(a,s,0,next?replacementIds:ids,64));
}
struct Barrier {
    std::mutex mutex;std::condition_variable condition;bool entered=false,cancelled=false,release=false;std::atomic<bool> claimed{false};
    static void checkpoint(void* context,ModalPreparationTag tag,detail::PreparationControl control) noexcept {auto& self=*static_cast<Barrier*>(context);if(tag.domain!=ModalPreparationDomain::room||self.claimed.exchange(true))return;std::unique_lock lock(self.mutex);self.entered=true;self.condition.notify_all();while(!self.release&&!control.isCancelled(control.context))self.condition.wait_for(lock,std::chrono::milliseconds(1));self.cancelled=control.isCancelled(control.context);self.condition.notify_all();}
    bool wait(bool cancellation=false){std::unique_lock lock(mutex);return condition.wait_for(lock,std::chrono::seconds(5),[&]{return cancellation?cancelled:entered;});}
    void unblock(){std::lock_guard lock(mutex);release=true;condition.notify_all();}
};
void threaded(){
    auto s=defaults();auto ids=identities();auto bundle=prepareModalBundle(48000,testModes,8,s,ids);CHECK(bool(bundle));if(!bundle)return;
    {Barrier barrier;ModalService service;CHECK(service.prepareStorage(48000,testModes,{true,&Barrier::checkpoint,&barrier}));CHECK(service.installBundle(*bundle,8));CHECK(!service.pumpHeavy()&&!service.pumpLight());s[0].size=.2;CHECK(callback(service,s,1,ids,64));const bool entered=barrier.wait();CHECK(entered);CHECK(service.resetSlot(0));if(entered)CHECK(barrier.wait(true));else barrier.unblock();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);bool ready=false;while(std::chrono::steady_clock::now()<deadline&&!ready){allocations=frees=0;tracking=true;const bool ok=callback(service,s,1,ids,64);ModalServiceStatus status;const bool read=service.snapshot(status);tracking=false;CHECK(ok&&read);CHECK(allocations==0&&frees==0);ready=read&&status.slots[0].appliedRoom==status.slots[0].requestedRoom&&status.slots[0].appliedPost==status.slots[0].requestedPost&&status.slots[0].appliedDuck==status.slots[0].requestedDuck;std::this_thread::yield();}CHECK(ready);barrier.unblock();}
    {s=defaults();Barrier barrier;auto service=std::make_unique<ModalService>();CHECK(service->prepareStorage(48000,testModes,{true,&Barrier::checkpoint,&barrier}));CHECK(service->installBundle(*bundle,8));s[0].size=.3;CHECK(callback(*service,s,1,ids,64));CHECK(barrier.wait());service.reset();CHECK(barrier.cancelled);barrier.unblock();}
    {s=defaults();ModalService service;CHECK(service.prepareStorage(48000,testModes,{true}));CHECK(service.installBundle(*bundle,8));std::atomic<bool> done=false;bool consistent=true;std::thread reader([&]{std::uint64_t last=0;while(!done.load(std::memory_order_acquire)){ModalServiceStatus status;if(service.snapshot(status)){consistent=consistent&&status.documentKey==8&&status.completedParents>=last&&status.admissionsThisParent<=1&&status.maximumRoomsThisParent<=9;last=status.completedParents;}}});
        for(unsigned n=0;n<100;++n){s[n%8].size=.01*(n%100);s[n%8].mix=n%2?0:.5;CHECK(callback(service,s,255,ids,64));}done.store(true,std::memory_order_release);reader.join();CHECK(consistent);}
    std::cout<<"MODAL_SERVICE_THREADED reset_cancellation=1 teardown_cancellation=1 coherent_reader=1\n";
}

void failureAndFastIsolation(){
    auto s=defaults();auto ids=identities();auto bundle=prepareModalBundle(48000,testModes,33,s,ids);CHECK(bool(bundle));if(!bundle)return;ModalService service;CHECK(service.prepareStorage(48000,testModes,{false}));CHECK(service.installBundle(*bundle,33));s[0].size=.2;CHECK(callback(service,s,1,ids));failAllocation=true;CHECK(service.pumpHeavy());CHECK(!failAllocation);CHECK(callback(service,s,1,ids));ModalServiceStatus status;CHECK(service.snapshot(status));CHECK(status.slots[0].preparationFailures==1&&status.slots[0].appliedRoom<status.slots[0].requestedRoom);CHECK(!service.pumpHeavy());CHECK(callback(service,s,1,ids));CHECK(service.snapshot(status));CHECK(status.slots[0].preparationFailures==1);s[0].size=.3;s[0].mix=1;CHECK(callback(service,s,1,ids));CHECK(service.pumpHeavy());s[0].mix=0;s[0].width=0;CHECK(callback(service,s,1,ids));CHECK(service.snapshot(status));CHECK(status.slots[0].appliedRoom==status.slots[0].requestedRoom&&status.slots[0].preparationFailures==1);
    auto raw=input(64),audio=raw;CHECK(service.beginParent(s,1,ids,64));CHECK(service.renderSlot(0,audio));CHECK(service.endParent());CHECK(bits(audio,raw));s[0].mix=1;CHECK(callback(service,s,1,ids,1024,512));audio.assign(64,{0,0});CHECK(service.beginParent(s,1,ids,64));CHECK(service.renderSlot(0,audio));CHECK(service.endParent());bool mono=true,nonzero=false;for(auto frame:audio){mono=mono&&frame[0]==frame[1];nonzero=nonzero||frame[0]!=0;}CHECK(mono&&nonzero);
    std::cout<<"MODAL_SERVICE_FAILURE allocation_failure=1 recovered=1 stale_fast_isolated=1\n";
}
void parallelOwners(){
    auto s=defaults();auto ids=identities();auto bundle=prepareModalBundle(48000,testModes,40,s,ids);CHECK(bool(bundle));if(!bundle)return;ModalService sequential,parallel;CHECK(sequential.prepareStorage(48000,testModes,{false})&&parallel.prepareStorage(48000,testModes,{false}));CHECK(sequential.installBundle(*bundle,40)&&parallel.installBundle(*bundle,40));bool exact=true,rtClean=true;
    for(unsigned iteration=0;iteration<20;++iteration){for(unsigned i=0;i<8;++i){if(iteration%4==0){s[i].size=.02*(iteration+i);s[i].post[0]={true,false,PostShape::bell,1000,double(i+1),1,1,CutSlope::db12};}s[i].character=iteration%2;s[i].mix=.3;}
        std::array<std::vector<Frame>,8> a,b;for(unsigned i=0;i<8;++i){a[i]=input(512,i+iteration);b[i]=a[i];}CHECK(sequential.beginParent(s,255,ids,512)&&parallel.beginParent(s,255,ids,512));for(unsigned i=0;i<8;++i)CHECK(sequential.renderSlot(i,a[i]));
        std::barrier start(9);std::array<std::thread,8> jobs;std::array<bool,8> ok{},clean{};for(unsigned i=0;i<8;++i)jobs[i]=std::thread([&,i]{start.arrive_and_wait();allocations=frees=0;tracking=true;ok[i]=true;for(std::size_t at=0;at<512;at+=17)ok[i]=parallel.renderSlot(i,std::span(b[i]).subspan(at,std::min<std::size_t>(17,512-at)))&&ok[i];tracking=false;clean[i]=allocations==0&&frees==0;});start.arrive_and_wait();for(auto& job:jobs)job.join();for(unsigned i=0;i<8;++i){exact=exact&&ok[i]&&bits(a[i],b[i]);rtClean=rtClean&&clean[i];}CHECK(sequential.endParent()&&parallel.endParent());while(sequential.pumpHeavy()){CHECK(parallel.pumpHeavy());}CHECK(!parallel.pumpHeavy());while(sequential.pumpLight()){CHECK(parallel.pumpLight());}CHECK(!parallel.pumpLight());ModalServiceStatus one,two;CHECK(sequential.snapshot(one)&&parallel.snapshot(two));exact=exact&&one==two;
    }CHECK(exact);CHECK(rtClean);std::cout<<"MODAL_SERVICE_PARALLEL parents=20 slots=8 bit_exact=1 callback_allocation_free=0\n";
}

void resources(){for(double rate:{8000.,48000.,768000.}){auto s=defaults();auto ids=identities();auto bundle=prepareModalBundle(rate,testMaximumModes,5,s,ids);CHECK(bool(bundle));if(!bundle)continue;ModalService service;CHECK(service.prepareStorage(rate,testMaximumModes,{false}));CHECK(service.installBundle(*bundle,5));const auto bytes=service.storageBytes()+bundle->storageBytes();CHECK(bytes>8*1024*1024&&bytes<=8*16*1024*1024);CHECK(callback(service,s,255,ids,64));std::cout<<"MODAL_SERVICE_RESOURCE rate="<<rate<<" total_bytes_including_bundle="<<bytes<<" slot_upper_bytes="<<(bytes+7)/8<<'\n';}}
}
int main(){std::cout.precision(17);invalid();staticAudio();fairAndCoalesced();transitions();threaded();failureAndFastIsolation();parallelOwners();resources();std::cout<<"SpaceReverbDirectModalService checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

#include "space/SpaceActivation.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
namespace {
using namespace airack::space;
using namespace airack::space::runtime;
int checks=0,failures=0;
void check(bool value,const char* text,int line){++checks;if(!value){++failures;std::cout<<"FAIL "<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
ModalSlotIdentities identities(){return {1,2,3,4,5,6,7,8};}
void frames(ModalService& service,unsigned mask,std::size_t count){std::array<ModalService::Frame,512> audio{};for(unsigned i=0;i<8;++i)if(mask&(1U<<i))CHECK(service.renderSlot(i,std::span(audio).first(count)));CHECK(service.endParent());}
void lifecycle(){ModalSlotSettings settings;const auto ids=identities();ModalService service;ModalServiceOptions options;options.threaded=false;CHECK(service.prepareStorage(48000,128,options));auto bundle=prepareModalBundle(48000,128,1,settings,ids);CHECK(bundle);if(!bundle)return;CHECK(service.installBundle(*bundle,1));CHECK(!service.workersRunning());CHECK(service.startWorkers());CHECK(service.workersRunning());CHECK(service.beginParent(settings,1,ids,17));CHECK(!service.startWorkers());CHECK(!service.stopWorkers());CHECK(!service.makeInactive());CHECK(!service.resetSlot(0));frames(service,1,17);CHECK(!service.stopWorkers());CHECK(service.makeInactive());CHECK(service.stopWorkers());CHECK(!service.workersRunning());CHECK(service.startWorkers());CHECK(service.stopWorkers());}
void selectiveInstall(){ModalSlotSettings initial;auto ids=identities();ModalService service;ModalServiceOptions options;options.threaded=false;CHECK(service.prepareStorage(48000,128,options));auto first=prepareModalBundle(48000,128,1,initial,ids);CHECK(first&&service.installBundle(*first,1));auto edited=initial;edited[0].decay.decaySeconds=5;
    CHECK(service.beginParent(edited,3,ids,17));frames(service,3,17);CHECK(service.pumpHeavy());CHECK(service.beginParent(edited,3,ids,17));CHECK(service.admissionsThisParent()==1);frames(service,3,17);ModalServiceStatus before,after;CHECK(service.snapshot(before));CHECK(before.ticketSlot==0);
    ++ids[1];edited[1].mix=.6;auto second=prepareModalBundle(48000,128,2,edited,ids);CHECK(second&&service.installBundle(*second,2,2));CHECK(service.snapshot(after));CHECK(after.documentKey==2);CHECK(after.slots[0]==before.slots[0]);CHECK(after.completedParents==before.completedParents);CHECK(after.ticketSlot==before.ticketSlot);CHECK(after.slots[1].identity==ids[1]);
    auto wrong=ids;++wrong[0];auto invalid=prepareModalBundle(48000,128,3,edited,wrong);CHECK(invalid);CHECK(!service.installBundle(*invalid,3,2));ModalServiceStatus rejected;CHECK(service.snapshot(rejected)&&rejected==after);CHECK(service.beginParent(edited,3,ids,17,false));CHECK(service.admissionsThisParent()==0);frames(service,3,17);
}
void segmentedAdmission(){ModalSlotSettings initial;const auto ids=identities();ModalService service;ModalServiceOptions options;options.threaded=false;CHECK(service.prepareStorage(48000,128,options));auto bundle=prepareModalBundle(48000,128,1,initial,ids);CHECK(bundle&&service.installBundle(*bundle,1));auto edited=initial;for(auto& slot:edited)slot.decay.decaySeconds=5;
    CHECK(service.beginParent(edited,255,ids,1));frames(service,255,1);for(unsigned i=0;i<8;++i)CHECK(service.pumpHeavy());CHECK(service.beginParent(edited,255,ids,17));CHECK(service.admissionsThisParent()==1);frames(service,255,17);CHECK(service.beginParent(edited,255,ids,512,false));CHECK(service.admissionsThisParent()==0);frames(service,255,512);ModalServiceStatus status;CHECK(service.snapshot(status));CHECK(status.ticketSlot==8);CHECK(service.beginParent(edited,255,ids,3,false));CHECK(service.admissionsThisParent()==0);frames(service,255,3);CHECK(service.beginParent(edited,255,ids,1));CHECK(service.admissionsThisParent()==1);frames(service,255,1);
}
struct Gate {std::atomic<bool> entered{false},release{false};std::uint64_t key=2;
    static void checkpoint(void* raw,std::uint64_t key,airack::space::detail::PreparationControl control) noexcept {auto& gate=*static_cast<Gate*>(raw);if(key!=gate.key)return;gate.entered.store(true,std::memory_order_release);while(!gate.release.load(std::memory_order_acquire)&&!control.isCancelled(control.context))std::this_thread::yield();}
};
bool waitFlag(const std::atomic<bool>& value){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(!value.load(std::memory_order_acquire)&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));return value.load(std::memory_order_acquire);}
SpaceActivationRequest request(std::uint64_t key,unsigned mask){SpaceActivationRequest r;r.key=key;r.activeMask=mask;r.identities=identities();return r;}
void cancelledAndOwning(){Gate gate;auto activation=std::make_unique<SpaceActivation>();const auto empty=request(1,0);CHECK(activation->prepare(48000,1,empty,{&Gate::checkpoint,&gate}));auto stale=request(2,1);stale.settings[0].decay.decaySeconds=6;activation->request(stale);CHECK(waitFlag(gate.entered));auto newest=request(3,1);newest.settings[0].decay.decaySeconds=.5;activation->request(newest);
    auto state=SpaceActivation::Result::pending;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);while(state==SpaceActivation::Result::pending&&std::chrono::steady_clock::now()<deadline){state=activation->pollReady(newest);std::this_thread::sleep_for(std::chrono::milliseconds(1));}CHECK(state==SpaceActivation::Result::ready);CHECK(activation->installReady(newest));CHECK(activation->pollReady(stale)!=SpaceActivation::Result::ready);CHECK(activation->failureCount()==0);CHECK(activation->acknowledgeInactive());
    activation->request(request(4,0));activation.reset();
    // Destruction cancels a blocked preparation before its borrowed gate dies.
    gate.entered.store(false);gate.key=5;activation=std::make_unique<SpaceActivation>();CHECK(activation->prepare(48000,1,empty,{&Gate::checkpoint,&gate}));activation->request(request(5,1));CHECK(waitFlag(gate.entered));activation.reset();
}
}
int main(){lifecycle();selectiveInstall();segmentedAdmission();cancelledAndOwning();std::cout<<"SPACE_PRODUCT_LIFECYCLE checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

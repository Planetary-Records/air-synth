#include "ModalService.h"
#include "ModalColdCopyAudit.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#if !defined(AIRACK_MODAL_COLD_COPY_DIAGNOSTIC)
#error Cold install tests require operation accounting in ModalTank, ModalRoom and ModalService.
#endif
namespace {
using namespace airack::space;using namespace airack::space::probe;
unsigned checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<80)std::cerr<<__LINE__<<": " #x "\n";}}while(false)
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool tracking=false;thread_local unsigned allocations=0,frees=0;
int hook(int operation,void*,std::size_t,int type,long,const unsigned char*,int){if(tracking&&type!=_CRT_BLOCK){if(operation==_HOOK_FREE)++frees;else ++allocations;}return 1;}
struct HeapScope {decltype(_CrtSetAllocHook(nullptr)) previous;
    HeapScope():previous(_CrtSetAllocHook(hook)){allocations=frees=0;tracking=true;}
    ~HeapScope(){tracking=false;_CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);}
};
#else
struct HeapScope {};
#endif
using Frame=ModalService::Frame;
bool bits(std::span<const Frame> a,std::span<const Frame> b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size_bytes())==0;}
ModalSlotSettings settings(unsigned family){ModalSlotSettings result;
    for(unsigned slot=0;slot<8;++slot){auto& s=result[slot];s.size=double((slot+family)%9)/8;s.decay.decaySeconds=.2+.3*slot+family;s.character=.7;s.mix=.6;s.predelayMs=slot*3;
        s.decay.bands[0]={true,false,DecayShape::lowShelf,100,.5+.5*family,.8};s.post[0]={true,false,PostShape::lowCut,100+10.*slot,0,1,1,CutSlope::db24};}
    return result;
}
void compare(double rate,std::size_t modes,unsigned factor){
    const ModalSlotIdentities ids{1,2,3,4,5,6,7,8};const auto firstSettings=settings(0),secondSettings=settings(1);
    auto first=prepareModalBundle(rate,modes,1,firstSettings,ids,{},factor),second=prepareModalBundle(rate,modes,2,secondSettings,ids,{},factor);
    CHECK(first&&second);if(!first||!second)return;
    ModalService candidate,reference;ModalServiceOptions fast,full;fast.threaded=full.threaded=false;fast.factor=full.factor=factor;fast.preparedCopies=true;full.preparedCopies=false;
    CHECK(candidate.prepareStorage(rate,modes,fast)&&reference.prepareStorage(rate,modes,full));
    std::array<Frame,128> a{},b{};bool exact=true;std::size_t factoryWrites=0,stateClears=0;unsigned completedRoomChanges=0;
    for(unsigned recall=0;recall<6;++recall){const auto& bundle=recall%2?*second:*first;const auto& desired=recall%2?secondSettings:firstSettings;
        {HeapScope heap;clearModalColdCopyAccounting();CHECK(candidate.installBundle(bundle,bundle.key()));const auto work=modalColdCopyAccounting;
            CHECK(work.fullCoefficientValuesWritten==0);CHECK(work.factoryCoefficientValuesWritten==16*(6*modes+modes/2));CHECK(work.stateValuesCleared<=32*modes);
            factoryWrites=work.factoryCoefficientValuesWritten;stateClears=work.stateValuesCleared;
            clearModalColdCopyAccounting();CHECK(reference.installBundle(bundle,bundle.key()));CHECK(modalColdCopyAccounting.fullCoefficientValuesWritten==16*(6*maximumDirectModes+maximumDirectModes/2));
        }
        ModalServiceStatus left,right;CHECK(candidate.snapshot(left)&&reference.snapshot(right));CHECK(left==right);auto target=desired;
        for(unsigned parent=0;parent<24;++parent){const auto count=std::array<std::size_t,6>{1,3,17,64,127,128}[parent%6];
            if(parent==2)for(auto& slot:target){slot.size=1-slot.size;slot.decay.decaySeconds+=.35;}
            for(std::size_t n=0;n<count;++n)a[n]=b[n]={double(int((n*17+parent)%29)-14)/64.,double(int((n*13+parent)%31)-15)/64.};
            {HeapScope heap;CHECK(candidate.beginParent(target,255,ids,count)&&reference.beginParent(target,255,ids,count));
                for(unsigned slot=0;slot<8;++slot){CHECK(candidate.renderSlot(slot,std::span(a).first(count))&&reference.renderSlot(slot,std::span(b).first(count)));exact=bits(std::span(a).first(count),std::span(b).first(count))&&exact;}
                CHECK(candidate.endParent()&&reference.endParent());}
            CHECK(candidate.snapshot(left)&&reference.snapshot(right));CHECK(left==right);
            ModalTailReport ta,tb;const std::array<std::size_t,8> chain{0,1,2,3,4,5,6,7};CHECK(candidate.composeTail(chain,4,ta)&&reference.composeTail(chain,4,tb));CHECK(ta==tb);
            for(unsigned slot=0;slot<8;++slot)CHECK(candidate.pumpHeavy()==reference.pumpHeavy());
            for(unsigned slot=0;slot<16;++slot)CHECK(candidate.pumpLight()==reference.pumpLight());
        }
        CHECK(left.slots[0].appliedRoom>1);completedRoomChanges+=unsigned(left.slots[0].appliedRoom>1);
    }
    CHECK(exact&&completedRoomChanges==6);std::cout<<"COLD_FACTORY_INSTALL rate="<<rate<<" modes="<<modes<<" factor="<<factor<<" coefficient_values="<<factoryWrites<<" state_values_cleared="<<stateClears<<" exact="<<exact<<'\n';
}
void publicConfigurationRemainsFull(){
    ModalTank tank;CHECK(tank.prepareStorage(48000,ModalBackend::automatic));
    for(std::size_t modes:{2048U,128U,512U,192U,2048U,128U}){auto prepared=std::make_unique<PreparedModal>();CHECK(prepareModal({},.5,48000,modes,*prepared));
        for(std::size_t i=modes;i<maximumDirectModes;++i){prepared->rc[i]=17;prepared->rs[i]=-23;prepared->target[i]=42;}
        clearModalColdCopyAccounting();CHECK(tank.configure(*prepared));CHECK(tank.prepared()==*prepared);CHECK(modalColdCopyAccounting.fullCoefficientValuesWritten==6*maximumDirectModes+maximumDirectModes/2);CHECK(modalColdCopyAccounting.stateValuesCleared==2*maximumDirectModes);
        for(std::size_t i=0;i<modes;++i)CHECK(tank.modeState(i)==ModalSample{});
        auto bad=std::make_unique<PreparedModal>(*prepared);bad->rc[0]=2;CHECK(!tank.configure(*bad));CHECK(tank.prepared()==*prepared);
    }
}
}
int main(){publicConfigurationRemainsFull();for(double rate:{8000.,44100.,48000.,96000.})for(std::size_t modes:{128U,192U,256U,512U,1024U,2048U})for(unsigned factor:{1U,2U})compare(rate,modes,factor);std::cout<<"COLD_FACTORY_INSTALL checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

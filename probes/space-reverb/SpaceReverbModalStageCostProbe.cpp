#include "ModalService.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>
#if defined(_MSC_VER) && defined(_M_X64)
#include <xmmintrin.h>
#endif
namespace {
using namespace airack::space;using namespace airack::space::probe;
using Frame=ModalRoom::Frame;using Clock=std::chrono::steady_clock;
struct Slot {ModalRoom room;ModalWetOuter wet;ModalFinalOuter final;ModalWetConverter converter;ModalLiveTail tail;std::array<Frame,256> logical{};};
struct Raw {unsigned c,parent,stage;std::int64_t ns;};
constexpr std::array<const char*,10> names{"peak","final_capture","down","wet_excite","room","mix_rooms","post","up","final_finish","tail_report"};
Settings settings(unsigned family){Settings s;s.decay.decaySeconds=20;s.size=family?1:0;for(auto& b:s.decay.bands)b={true,false,DecayShape::lowShelf,20,family?4:.25,4};s.predelayMs=family?250:0;s.character=1;s.distance=.7;s.width=2;s.mix=.35;s.duckRangeDb=24;s.duckThresholdDb=-60;s.duckAttackMs=.1;s.duckReleaseMs=2000;
    constexpr std::array<double,6> centers{20,18000,40,16000,60,12000};for(unsigned j=0;j<6;++j)s.post[j]={true,false,j%2?PostShape::highCut:PostShape::lowCut,centers[j]*(family?1.05:1),0,1,1,CutSlope::db24};return s;}
bool run(unsigned c,std::vector<Raw>& raw){
    const unsigned factor=c/2+1,host=c%2?512:64,physical=host*factor;constexpr double rate=96000;
    PreparedModalConverter converter;ModalTailContext context;if(!prepareModalConverter(factor,converter)||!prepareModalTailContext(rate,128,context,{converter.logicalSupport(),converter.logGain()}))return false;
    auto slots=std::make_unique<std::array<Slot,8>>();
    for(unsigned i=0;i<8;++i){auto& x=(*slots)[i];const auto s=settings(i%2);PreparedModalRoom room;PreparedModalWet wet;PreparedModalFinal final;PreparedModalTail roomTail,postTail;
        if(!prepareModalRoom(s.decay,s.size,rate,128,room)||!prepareModalWet(s,rate,wet)||!prepareModalFinal(s,rate*factor,final)||!prepareModalRoomTail(context,room,roomTail)||!prepareModalPostTail(context,wet.post,postTail))return false;
        if(!x.room.prepareStorage(rate,ModalBackend::automatic,true,true)||!x.room.configure(room)||!x.wet.prepareStorage(rate)||!x.wet.configure(wet)||!x.final.prepareStorage(rate*factor)||!x.final.configure(final)||!x.converter.configure(converter,ModalConverterBackend::automatic)||!x.tail.configure(context,roomTail,postTail))return false;
    }
    std::array<Frame,1024> work{};std::uint32_t random=0x5eeda123;bool valid=true,finite=true;
    for(unsigned n=0;n<576;++n){
        for(auto& f:std::span(work).first(physical))for(auto& v:f){random^=random<<13;random^=random>>17;random^=random<<5;v=double(int(random&65535)-32768)/65536.;}
        std::array<std::int64_t,10> costs{};
        const auto timed=[&](unsigned stage,auto&& fn){const auto start=Clock::now();const bool ok=fn();const auto stop=Clock::now();valid=valid&&ok;costs[stage]+=std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start).count();};
        for(auto& x:*slots){for(unsigned at=0;at<physical;at+=512){
            auto packet=std::span(work).subspan(at,std::min(512U,physical-at));std::span<Frame> logical=packet;
            timed(0,[&]{double peak=0;for(auto f:packet)for(double v:f){if(!std::isfinite(v))return false;peak=std::max(peak,std::abs(v));}return x.tail.observePeak(peak);});
            timed(1,[&]{return x.final.capture(packet);});
            if(factor==2)timed(2,[&]{std::size_t made=0;const bool ok=x.converter.down(packet,x.logical,made);logical=std::span(x.logical).first(made);return ok;});
            timed(3,[&]{return x.wet.excite(logical);});
            timed(4,[&]{return x.room.processBlock(logical,x.wet.characters());});
            timed(5,[&]{return x.wet.mixRooms(logical);});
            timed(6,[&]{return x.wet.post(logical);});
            if(factor==2)timed(7,[&]{return x.converter.up(logical,packet);});
            timed(8,[&]{return x.final.finish(packet);});
        }timed(9,[&]{ModalTailReport tail;return x.tail.report(tail);});}
        for(auto f:std::span(work).first(physical))for(double v:f)finite=finite&&std::isfinite(v);
        if(n>=64)for(unsigned j=0;j<costs.size();++j)raw.push_back({c,n-64,j,costs[j]});
    }
    std::cout<<"MODAL_STAGE_CASE case="<<c<<" factor="<<factor<<" host="<<host<<" rate=96000 modes=128 slots=8 warm=64 measured=512 valid="<<valid<<" finite="<<finite<<" live_adoptions=0 capacity_acceptance=0\n";
    return valid&&finite;
}
}
int main(){
#if defined(_MSC_VER) && defined(_M_X64)
    const auto before=_mm_getcsr();_mm_setcsr(before|0x8040);
#endif
    std::vector<Raw> raw;raw.reserve(4*512*10);bool valid=true;for(unsigned c=0;c<4;++c)valid=run(c,raw)&&valid;
    for(const auto& r:raw)std::cout<<"MODAL_STAGE_SAMPLE case="<<r.c<<" parent="<<r.parent<<" stage="<<names[r.stage]<<" ns="<<r.ns<<'\n';
    std::cout<<"MODAL_STAGE_COMPLETE cases=4 samples="<<raw.size()<<" valid="<<valid<<" caller=ordinary_unpaced live_adoptions=0 capacity_acceptance=0\n";
#if defined(_MSC_VER) && defined(_M_X64)
    _mm_setcsr(before);
#endif
    return valid?0:1;
}

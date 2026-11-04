#include "ModalConverter.h"
#include "ModalConverterReference.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace {
using namespace airack::space::probe;
namespace original=airack::space::probe::converter_reference;
using Frame=ModalWetConverter::Frame;
using Clock=std::chrono::steady_clock;
constexpr unsigned warm=64, measured=512;
struct Case {unsigned factor,frames,packet;bool cold;};
constexpr std::array<Case,5> cases{{{2,128,128,false},{2,1024,512,false},
    {2,128,17,false},{2,128,128,true},{1,64,64,false}}};
struct Buffers {std::array<std::array<Frame,1024>,8> physical{};
    std::array<std::array<Frame,256>,8> logical{};};
struct Sample {std::int64_t ns=0;bool valid=false;};
template<class Converter>
Sample call(std::array<Converter,8>& converters,Buffers& buffers,const Case& c) {
    bool ok=true;const auto start=Clock::now();
    if(c.cold)for(auto& converter:converters)converter.reset();
    for(unsigned slot=0;slot<8;++slot)for(unsigned at=0;at<c.frames;at+=c.packet){
        const auto count=std::min(c.packet,c.frames-at);
        auto physical=std::span(buffers.physical[slot]).subspan(at,count);
        std::size_t made=0;
        ok=converters[slot].down(physical,buffers.logical[slot],made)&&ok;
        for(auto& frame:std::span(buffers.logical[slot]).first(made)){
            frame[0]=frame[0]*.75+.015625;frame[1]=frame[1]*.625-.0078125;
        }
        ok=converters[slot].up(std::span(buffers.logical[slot]).first(made),physical)&&ok;
    }
    const auto finish=Clock::now();
    return {std::chrono::duration_cast<std::chrono::nanoseconds>(finish-start).count(),ok};
}
struct Raw {unsigned c,backend,order,parent,position;bool candidate;std::int64_t ns;};
bool pair(unsigned id,ModalConverterBackend backend,unsigned order,std::vector<Raw>& raw){
    const auto c=cases[id];PreparedModalConverter descriptor;original::PreparedModalConverter reference;
    if(!prepareModalConverter(c.factor,descriptor)||!original::prepareModalConverter(c.factor,reference))return false;
    std::array<ModalWetConverter,8> candidate;std::array<original::ModalWetConverter,8> frozen;
    for(unsigned slot=0;slot<8;++slot)if(!candidate[slot].configure(descriptor,backend)||!frozen[slot].configure(reference))return false;
    const auto actual=candidate[0].selectedBackend();
    for(auto& converter:candidate)if(converter.selectedBackend()!=actual)return false;
    auto a=std::make_unique<Buffers>(),b=std::make_unique<Buffers>();
    bool exact=true,finite=true,valid=true;std::uint32_t random=0x638421a9;
    for(unsigned n=0;n<warm+measured;++n){
        for(unsigned slot=0;slot<8;++slot)for(unsigned i=0;i<c.frames;++i)for(unsigned channel=0;channel<2;++channel){
            random^=random<<13;random^=random>>17;random^=random<<5;
            a->physical[slot][i][channel]=double(int(random&65535)-32768)/65536.;
        }
        *b=*a;std::array<Sample,2> sample;
        // Each order has identical initial states and inputs. Retain both orders.
        for(unsigned position=0;position<2;++position){const auto which=position^order;
            sample[which]=which?call(candidate,*b,c):call(frozen,*a,c);
            if(n>=warm)raw.push_back({id,unsigned(backend),order,n-warm,position,which==1,sample[which].ns});
        }
        for(unsigned slot=0;slot<8;++slot){
            exact=exact&&std::memcmp(a->physical[slot].data(),b->physical[slot].data(),c.frames*sizeof(Frame))==0;
            for(auto frame:std::span(b->physical[slot]).first(c.frames))for(double v:frame)finite=finite&&std::isfinite(v);
        }
        valid=valid&&sample[0].valid&&sample[1].valid;
    }
    std::cout<<"CONVERTER_COST_PAIR case="<<id<<" requested="<<unsigned(backend)<<" actual="<<unsigned(actual)
        <<" order="<<order<<" factor="<<c.factor<<" physical="<<c.frames<<" packet="<<c.packet
        <<" cold="<<c.cold<<" slots=8 warm="<<warm<<" measured="<<measured
        <<" exact="<<exact<<" finite="<<finite<<" valid="<<valid<<" capacity_acceptance=0\n";
    return exact&&finite&&valid;
}
}
int main(){
    // All vectors/output are outside timed converter calls. No caller/worker/host policy is changed.
    std::vector<Raw> raw;raw.reserve(cases.size()*3*2*measured*2);bool ok=true;
    for(unsigned order=0;order<2;++order)for(unsigned c=0;c<cases.size();++c)
        for(auto backend:{ModalConverterBackend::reference,ModalConverterBackend::mirroredScalar,ModalConverterBackend::automatic})
            ok=pair(c,backend,order,raw)&&ok;
    for(const auto& r:raw)std::cout<<"CONVERTER_COST_SAMPLE case="<<r.c<<" requested="<<r.backend<<" order="<<r.order
        <<" parent="<<r.parent<<" position="<<r.position<<" candidate="<<r.candidate<<" ns="<<r.ns<<'\n';
    std::cout<<"CONVERTER_COST_COMPLETE pairs=30 samples="<<raw.size()<<" exact_and_contracts="<<ok
        <<" caller=ordinary_unpaced workers=none capacity_acceptance=0\n";return ok?0:1;
}

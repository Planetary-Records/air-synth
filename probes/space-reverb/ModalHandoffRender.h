#pragma once
#include "ModalTank.h"
#include "ProbePeerGuard.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>

namespace airack::space::probe {
struct ModalHandoffRecord {double blockSeconds=0,copySeconds=0;std::uint64_t copyUpperBytes=0;};
static_assert(sizeof(ModalHandoffRecord)==24);
struct ModalHandoffHarness {
    using Frame=ModalTank::Frame;
    using Input=std::array<Frame,4096>;
    using Banks=std::array<PreparedModal,2>;
    using Clock=std::chrono::steady_clock;
    std::array<ModalTank,16> tanks;
    std::array<std::size_t,8> active{0,1,2,3,4,5,6,7},spare{8,9,10,11,12,13,14,15};
    std::array<bool,8> selected{};
    int transition=-1;
    std::size_t nextSlot=0,phase=0,offset=0,duration=0,bytes=0;
    bool adopted=true;
    double checksum=0,peak=0;
    const Banks* banks=nullptr;
    const Input* input=nullptr;
    std::array<Frame,512> blockInput{},extra{};
    std::array<std::array<Frame,512>,8> primary{},captured{};
    std::size_t scratchBytes() const noexcept {return sizeof(blockInput)+sizeof(extra)+sizeof(primary)+sizeof(captured);}
    template<class Guard=PeerGuard> bool prepare(double rate,const Banks& descriptors,const Input& samples,Guard* guard=nullptr){
        banks=&descriptors;input=&samples;duration=std::size_t(std::ceil(.005*rate));
        for(std::size_t i=0;i<16;++i){if(!tanks[i].prepareStorage(rate,ModalBackend::automatic)||!tanks[i].configure(descriptors[i<8?0:1]))return false;
            bytes+=tanks[i].storageBytes();if(guard)guard->force();}
        return true;
    }
    static std::uint64_t upperCopy(const PreparedModal& a,const PreparedModal& b){
        return sizeof(PreparedModal)+2*std::min(a.count,b.count)*sizeof(double);
    }
    template<bool capture> ModalHandoffRecord step(int block,bool blockOrder,bool measureCopy){
        ModalHandoffRecord result;
        if(transition<0){const auto slot=nextSlot;const auto& next=(*banks)[selected[slot]?0:1];result.copyUpperBytes=upperCopy(tanks[active[slot]].prepared(),next);
            const auto start=measureCopy?Clock::now():Clock::time_point{};adopted=tanks[spare[slot]].adoptCopied(next,tanks[active[slot]])&&adopted;
            if(measureCopy)result.copySeconds=std::chrono::duration<double>(Clock::now()-start).count();transition=int(slot);phase=0;}
        if(blockOrder){
            const auto count=std::size_t(block);const int fading=transition;
            for(std::size_t n=0;n<count;++n)blockInput[n]=(*input)[(offset+n)%input->size()];
            for(std::size_t slot=0;slot<8;++slot){
                std::copy_n(blockInput.begin(),count,primary[slot].begin());
                adopted=tanks[active[slot]].processBlock(std::span(primary[slot]).first(count))&&adopted;
                if(fading==int(slot)){
                    std::copy_n(blockInput.begin(),count,extra.begin());
                    adopted=tanks[spare[slot]].processBlock(std::span(extra).first(count))&&adopted;
                }
            }
            for(std::size_t n=0;n<count;++n)for(std::size_t slot=0;slot<8;++slot){
                auto wet=primary[slot][n];
                if(fading==int(slot)){const auto next=extra[n];const double alpha=double(phase)/duration;
                    if(phase>=duration)wet=next;else if(phase!=0)for(std::size_t ch=0;ch<2;++ch)wet[ch]=(1-alpha)*wet[ch]+alpha*next[ch];
                    if(phase<=duration&&phase++==duration){std::swap(active[slot],spare[slot]);selected[slot]=!selected[slot];transition=-1;nextSlot=(slot+1)%8;}
                }
                if constexpr(capture)captured[slot][n]=wet;
                checksum+=wet[0]+wet[1];peak=std::max({peak,std::abs(wet[0]),std::abs(wet[1])});
            }
            offset=(offset+count)%input->size();
            return result;
        }
        // Retained per-frame reference, including exact fade/checksum order.
        for(int n=0;n<block;++n){const auto x=(*input)[(offset+std::size_t(n))%input->size()];
            for(std::size_t slot=0;slot<8;++slot){auto wet=tanks[active[slot]].tick(x);
                if(transition==int(slot)){const auto next=tanks[spare[slot]].tick(x);const double alpha=double(phase)/duration;
                    if(phase==duration)wet=next;else if(phase!=0)for(std::size_t ch=0;ch<2;++ch)wet[ch]=(1-alpha)*wet[ch]+alpha*next[ch];
                    if(phase++==duration){std::swap(active[slot],spare[slot]);selected[slot]=!selected[slot];transition=-1;nextSlot=(slot+1)%8;}}
                if constexpr(capture)captured[slot][std::size_t(n)]=wet;
                checksum+=wet[0]+wet[1];peak=std::max({peak,std::abs(wet[0]),std::abs(wet[1])});}}
        offset=(offset+std::size_t(block))%input->size();return result;
    }
};
}

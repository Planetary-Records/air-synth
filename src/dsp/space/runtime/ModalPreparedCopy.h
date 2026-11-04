#pragma once
#include "ModalTank.h"
#include <algorithm>
namespace airack::space::runtime {
// Only the private factory-result admission paths may use this operation.
// Public PreparedModal values may contain arbitrary unused entries.
class ModalFactoryCopy {
    static void assign(PreparedModal& to,const PreparedModal& from,bool& unusedZero) noexcept {
        const auto count=from.count;
        const auto clearEnd=unusedZero?std::max(to.count,count):maximumDirectModes;
        
        const auto copy=[&](auto& destination,const auto& source){
            std::copy_n(source.begin(),count,destination.begin());
            std::fill(destination.begin()+count,destination.begin()+clearEnd,0.);
        };
        copy(to.rc,from.rc);copy(to.rs,from.rs);copy(to.bReal,from.bReal);
        copy(to.bImag,from.bImag);copy(to.frequency,from.frequency);copy(to.target,from.target);
        std::copy_n(from.inputWeights.begin(),count/4,to.inputWeights.begin());
        std::fill(to.inputWeights.begin()+count/4,to.inputWeights.begin()+clearEnd/4,std::array<double,2>{});
        to.sampleRate=from.sampleRate;to.size=from.size;to.pickup=from.pickup;
        to.weightedRadius=from.weightedRadius;to.count=count;to.curve=from.curve;
        unusedZero=true;
    }
    friend class ModalTank;
    friend class ModalRoom;
};
}

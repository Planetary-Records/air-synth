#pragma once
#include "SpaceReverbLoss.h"
#include <limits>
namespace airack::space::detail {
struct TailCertificate {
    bool passed=false;
    double timeBound=0,radius=0,maxInternalPole=0,maxLoopMagnitude=std::numeric_limits<double>::infinity();
    std::size_t intervals=0;
    double rejectedHz=0;
    const char* reason="unprepared";
};
TailCertificate certifyTail(const LossNetwork&,double timeBound);
struct SectionMagnitudeBound {bool passed=false;double maximumLog=0,maximumPole=0;};
// Off-audio bound of the original coefficient response on the weighted circle.
SectionMagnitudeBound boundSectionMagnitude(const Biquad&,double physicalRate,double radius) noexcept;
}

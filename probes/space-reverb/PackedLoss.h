#pragma once
#include "SpaceReverbPacked.h"
namespace airack::space::probe {
using PackedLossStage=detail::PackedStage<16>;
using PackedLossKernel=detail::PackedKernel<16>;
inline PackedLossKernel availableAvx2Kernel() noexcept {return detail::availableAvx2Kernel16();}
}

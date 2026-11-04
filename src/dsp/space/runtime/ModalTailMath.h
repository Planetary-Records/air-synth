#pragma once
#include "SpaceReverbControls.h"
namespace airack::space::runtime {
struct ModalWeightedSection {bool passed=false;double logL1=0,maximumPole=0;};
ModalWeightedSection boundModalPostSection(const PostCoefficients&,double radius) noexcept;
namespace tail_detail {
std::array<std::complex<double>,2> roots(const PostCoefficients&) noexcept;
double rootMargin(const PostCoefficients&) noexcept;
}
}

#pragma once
#include <array>
#include <cstddef>
namespace airack::space::detail {
template<std::size_t N> struct alignas(64) PackedStage {
    static constexpr std::array<double,N> ones() {std::array<double,N> v{};v.fill(1);return v;}
    std::array<double,N> b0=ones(),b1{},b2{},a1{},a2{},z1{},z2{};
};
template<std::size_t N> using PackedKernel=void(*)(PackedStage<N>*,std::size_t,double*,std::size_t) noexcept;
PackedKernel<8> availableAvx2Kernel8() noexcept;
PackedKernel<16> availableAvx2Kernel16() noexcept;
void tickPackedSse2(PackedStage<8>*,std::size_t,double*,std::size_t) noexcept;
}

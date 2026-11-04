#pragma once
#include <array>
#include <cstdint>
namespace airack::space::test {
// Owned coefficients frozen before source movement; FNV1a is a regression fingerprint, not a security assertion.
struct TankGolden {int configuration,curve;double rate,size;std::uint64_t fingerprint;};
inline constexpr std::array<TankGolden,13> tankGolden {{
    {40,4,48000.,0.5,0xF4BA3D5C357C3C63ULL},
    {46,5,44100.,0.5,0x5DFB5A824D1BCD96ULL},
    {47,5,44100.,1,0x479DC3130AC8FBD9ULL},
    {49,5,48000.,0.5,0xB22EF2815A8E2388ULL},
    {50,5,48000.,1,0xC3795926304A57E9ULL},
    {52,5,96000.,0.5,0x860F691AC6017C0EULL},
    {53,5,96000.,1,0x6DEDC1D4614DEEFDULL},
    {316,35,44100.,0.5,0xDB10ED05B53CE8C1ULL},
    {317,35,44100.,1,0xA40B88BB6CAFF709ULL},
    {319,35,48000.,0.5,0xB4CB0970FB7876B3ULL},
    {320,35,48000.,1,0x6CAF67FDF048F425ULL},
    {322,35,96000.,0.5,0x028BD226E17495ACULL},
    {323,35,96000.,1,0x95023A6D6A547492ULL},
}};
}

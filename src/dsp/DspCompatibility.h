#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace airack
{
using AlgorithmRevision = std::uint8_t;
inline constexpr AlgorithmRevision legacyAlgorithmRevision = 1;
inline constexpr bool supportsAlgorithmRevision(AlgorithmRevision revision) noexcept
{ return revision == legacyAlgorithmRevision; }
// Synth banks: revision 1 is the retained legacy voice; revision 2 is the
// full-band voice kernel (both engines). Admission is independent of effects.
inline constexpr AlgorithmRevision modernSynthAlgorithmRevision = 2;
inline constexpr bool supportsSynthAlgorithmRevision(AlgorithmRevision revision) noexcept
{ return supportsAlgorithmRevision(revision) || revision == modernSynthAlgorithmRevision; }
// Effect-bank order is the retained numeric layout: drive, delay, reverb,
// compressor, eq, filter, chorus, phaser, flanger, stereo, dimension, frequency shift, convolution, frequency split, mid/side split,
// dynamic EQ, space reverb, era reverb.
inline constexpr std::size_t dynamicEqEffectBank = 15;
inline constexpr std::size_t spaceReverbEffectBank = 16;
inline constexpr std::size_t eraReverbEffectBank = 17;
inline constexpr std::size_t saturationEffectBank = 18;
inline constexpr AlgorithmRevision saturationAlgorithmRevision = legacyAlgorithmRevision;
inline constexpr std::size_t effectBankCount = 19;
// Ten effect banks have a second kernel: the 4x oversampled drive with its knee
// at full output level, the modulated hall/plate reverb, the compressor whose
// decibel-domain ballistics take the attack and release labels at face value,
// the EQ whose bands follow their analog prototypes to Nyquist, the filter whose
// pre-drive, state-variable core and mix run at four times the rate, the
// delay, chorus, flanger and dimension, whose delay lines read through a
// windowed sinc instead of linear interpolation, and the frequency split, whose
// fourth-order Linkwitz-Riley bands sum to an allpass.
inline constexpr std::size_t driveEffectBank = 0;
inline constexpr AlgorithmRevision modernDriveAlgorithmRevision = 2;
inline constexpr std::size_t delayEffectBank = 1;
inline constexpr AlgorithmRevision modernDelayAlgorithmRevision = 2;
inline constexpr std::size_t reverbEffectBank = 2;
inline constexpr AlgorithmRevision modernReverbAlgorithmRevision = 2;
inline constexpr std::size_t compressorEffectBank = 3;
inline constexpr AlgorithmRevision modernCompressorAlgorithmRevision = 2;
inline constexpr std::size_t eqEffectBank = 4;
inline constexpr AlgorithmRevision modernEqAlgorithmRevision = 2;
inline constexpr std::size_t filterEffectBank = 5;
inline constexpr AlgorithmRevision modernFilterAlgorithmRevision = 2;
inline constexpr std::size_t chorusEffectBank = 6;
inline constexpr std::size_t flangerEffectBank = 8;
inline constexpr std::size_t dimensionEffectBank = 10;
inline constexpr AlgorithmRevision modernModulatedTapAlgorithmRevision = 2;
inline constexpr std::size_t frequencySplitEffectBank = 13;
inline constexpr AlgorithmRevision modernFrequencySplitAlgorithmRevision = 2;
inline constexpr bool modulatedTapEffectBank(std::size_t bank) noexcept
{ return bank == chorusEffectBank || bank == flangerEffectBank || bank == dimensionEffectBank; }
inline constexpr bool supportsEffectAlgorithmRevision(std::size_t bank, AlgorithmRevision revision) noexcept
{
    return bank < effectBankCount && (supportsAlgorithmRevision(revision)
        || (bank == driveEffectBank && revision == modernDriveAlgorithmRevision)
        || (bank == delayEffectBank && revision == modernDelayAlgorithmRevision)
        || (bank == reverbEffectBank && revision == modernReverbAlgorithmRevision)
        || (bank == compressorEffectBank && revision == modernCompressorAlgorithmRevision)
        || (bank == eqEffectBank && revision == modernEqAlgorithmRevision)
        || (bank == filterEffectBank && revision == modernFilterAlgorithmRevision)
        || (modulatedTapEffectBank(bank) && revision == modernModulatedTapAlgorithmRevision)
        || (bank == frequencySplitEffectBank && revision == modernFrequencySplitAlgorithmRevision));
}
namespace detail
{
template<std::size_t Rows,std::size_t Columns>
constexpr auto legacyKernelBanks() noexcept
{
    std::array<std::array<AlgorithmRevision,Columns>,Rows> banks {};
    for (auto& bank : banks) bank.fill(legacyAlgorithmRevision);
    return banks;
}
}
struct KernelCompatibility
{
    std::array<std::array<AlgorithmRevision, 2>, 4> synth = detail::legacyKernelBanks<4,2>();
    std::array<std::array<AlgorithmRevision, effectBankCount>, 8> effects = detail::legacyKernelBanks<8,effectBankCount>();
    bool operator==(const KernelCompatibility&) const = default;
};
inline constexpr bool validateKernelCompatibility(const KernelCompatibility& value) noexcept
{
    for (const auto& banks : value.synth)
        for (auto revision : banks) if (!supportsSynthAlgorithmRevision(revision)) return false;
    for (const auto& banks : value.effects)
        for (std::size_t bank=0;bank<banks.size();++bank)
            if (!supportsEffectAlgorithmRevision(bank,banks[bank])) return false;
    return true;
}
// New instances select every modern kernel: synth, drive, delay, reverb, compressor, EQ, filter, the
// modulated taps and the frequency split. Decoded and recalled states keep their own revisions; formats without them keep the
// legacy default above.
inline constexpr KernelCompatibility newInstanceKernelCompatibility() noexcept
{
    KernelCompatibility value;
    for (auto& banks : value.synth) banks.fill(modernSynthAlgorithmRevision);
    for (auto& banks : value.effects)
    {
        banks[driveEffectBank] = modernDriveAlgorithmRevision; banks[delayEffectBank] = modernDelayAlgorithmRevision;
        banks[reverbEffectBank] = modernReverbAlgorithmRevision;
        banks[compressorEffectBank] = modernCompressorAlgorithmRevision;
        banks[eqEffectBank] = modernEqAlgorithmRevision;
        banks[filterEffectBank] = modernFilterAlgorithmRevision;
        banks[chorusEffectBank] = banks[flangerEffectBank] = banks[dimensionEffectBank] = modernModulatedTapAlgorithmRevision;
        banks[frequencySplitEffectBank] = modernFrequencySplitAlgorithmRevision;
    }
    return value;
}
}

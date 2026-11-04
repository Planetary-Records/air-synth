#pragma once

#include <array>
#include <cstdint>

namespace airack
{
inline constexpr std::uint8_t noSlot = 255, masterChain = 4;
enum class SlotUse : std::uint8_t { neverUsed = 0, present = 1, retired = 2 };
enum class EffectKind : std::uint8_t { drive = 0, delay = 1, reverb = 2, compressor = 3, eq = 4, filter = 5, chorus = 6, phaser = 7, flanger = 8, stereo = 9, dimension = 10, frequencyShift = 11, convolution = 12, frequencySplit = 13, midSideSplit = 14, dynamicEq = 15, spaceReverb = 16, eraReverb = 17, saturation = 18 };

struct EffectSlot
{
    SlotUse use = SlotUse::neverUsed;
    EffectKind kind = EffectKind::drive;
    std::uint8_t chain = masterChain;
    bool operator==(const EffectSlot&) const = default;
};

struct RackLayout
{
    std::array<SlotUse, 4> synthUse { SlotUse::present, SlotUse::neverUsed,
                                     SlotUse::neverUsed, SlotUse::neverUsed };
    std::array<std::uint8_t, 4> synthOrder { 0, noSlot, noSlot, noSlot };
    std::uint8_t synthCount = 1;
    std::array<EffectSlot, 8> effects {};
    std::array<std::array<std::uint8_t, 8>, 5> effectOrder = [] {
        std::array<std::array<std::uint8_t, 8>, 5> orders {};
        for (auto& order : orders) order.fill(noSlot);
        return orders;
    }();
    std::array<std::uint8_t, 5> effectCount {};
    bool operator==(const RackLayout&) const = default;
};

enum class RackEditKind : std::uint8_t
{
    addSynth, removeSynth, moveSynth, addEffect, removeEffect, moveEffect, replaceEffect
};
struct RackEdit
{
    RackEditKind kind = RackEditKind::addSynth;
    std::uint8_t slot = noSlot, destination = masterChain, position = 0;
    EffectKind effect = EffectKind::drive;
    bool allowReuse = false;
};
enum class RackEditResult : std::uint8_t { applied, invalid, capacityReached, replacementRequired };

bool validateRackLayout(const RackLayout&) noexcept;
// Transactional: every non-applied result preserves output, including input/output aliasing.
RackEditResult editRackLayout(const RackLayout&, const RackEdit&, RackLayout&) noexcept;
}

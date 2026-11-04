#include "RackLayout.h"
#include <algorithm>

namespace airack
{
namespace
{
bool validUse(SlotUse use) noexcept { return use <= SlotUse::retired; }
bool validEffect(EffectKind kind) noexcept { return kind <= EffectKind::saturation; }

// Only used after validation: slot is present exactly once, position/count are bounded.
template<std::size_t N>
void erase(std::array<std::uint8_t, N>& order, std::uint8_t& count, std::uint8_t slot) noexcept
{
    auto position = static_cast<std::size_t>(std::find(order.begin(), order.begin() + count, slot) - order.begin());
    for (; position + 1 < count; ++position) order[position] = order[position + 1];
    order[--count] = noSlot;
}
template<std::size_t N>
void insert(std::array<std::uint8_t, N>& order, std::uint8_t& count,
            std::uint8_t slot, std::uint8_t position) noexcept
{
    for (auto index = count; index > position; --index) order[index] = order[index - 1];
    order[position] = slot;
    ++count;
}

RackEditResult selectSlot(const RackLayout& layout, bool synth, const RackEdit& edit,
                          std::uint8_t& selected) noexcept
{
    const auto limit = synth ? layout.synthUse.size() : layout.effects.size();
    const auto use = [&](std::size_t slot) {
        return synth ? layout.synthUse[slot] : layout.effects[slot].use;
    };
    if (edit.slot != noSlot)
    {
        if (edit.slot >= limit || use(edit.slot) == SlotUse::present) return RackEditResult::invalid;
        if (use(edit.slot) == SlotUse::retired && !edit.allowReuse) return RackEditResult::replacementRequired;
        selected = edit.slot;
        return RackEditResult::applied;
    }
    bool hasRetired = false;
    for (std::uint8_t slot = 0; slot < limit; ++slot)
    {
        if (use(slot) == SlotUse::neverUsed)
        {
            selected = slot;
            return RackEditResult::applied;
        }
        hasRetired |= use(slot) == SlotUse::retired;
    }
    // Automatic selection never authorizes reuse, even when allowReuse is set.
    return hasRetired ? RackEditResult::replacementRequired : RackEditResult::capacityReached;
}
}

bool validateRackLayout(const RackLayout& layout) noexcept
{
    if (layout.synthCount > layout.synthUse.size()) return false;
    for (auto use : layout.synthUse) if (!validUse(use)) return false;
    for (const auto& effect : layout.effects)
        if (!validUse(effect.use) || !validEffect(effect.kind) || effect.chain > masterChain) return false;

    std::array<bool, 4> seenSynths {};
    for (std::size_t position = 0; position < layout.synthOrder.size(); ++position)
    {
        const auto slot = layout.synthOrder[position];
        if (position >= layout.synthCount)
        {
            if (slot != noSlot) return false;
        }
        else
        {
            if (slot >= seenSynths.size() || seenSynths[slot] || layout.synthUse[slot] != SlotUse::present) return false;
            seenSynths[slot] = true;
        }
    }
    for (std::size_t slot = 0; slot < seenSynths.size(); ++slot)
        if (seenSynths[slot] != (layout.synthUse[slot] == SlotUse::present)) return false;

    std::array<bool, 8> seenEffects {};
    std::size_t total = 0;
    for (std::size_t chain = 0; chain < layout.effectOrder.size(); ++chain)
    {
        const auto count = layout.effectCount[chain];
        total += count;
        if (count > layout.effects.size() || total > layout.effects.size()) return false;
        if (count != 0 && chain != masterChain && !seenSynths[chain]) return false;
        for (std::size_t position = 0; position < layout.effectOrder[chain].size(); ++position)
        {
            const auto slot = layout.effectOrder[chain][position];
            if (position >= count)
            {
                if (slot != noSlot) return false;
            }
            else
            {
                if (slot >= seenEffects.size() || seenEffects[slot]) return false;
                const auto& effect = layout.effects[slot];
                if (effect.use != SlotUse::present || effect.chain != chain) return false;
                seenEffects[slot] = true;
            }
        }
    }
    for (std::size_t slot = 0; slot < seenEffects.size(); ++slot)
        if (seenEffects[slot] != (layout.effects[slot].use == SlotUse::present)) return false;
    return true;
}

RackEditResult editRackLayout(const RackLayout& input, const RackEdit& edit, RackLayout& output) noexcept
{
    if (!validateRackLayout(input) || edit.kind > RackEditKind::replaceEffect || !validEffect(edit.effect))
        return RackEditResult::invalid;

    auto candidate = input;
    auto slot = edit.slot;
    const auto destinationValid = [&] {
        return edit.destination == masterChain
            || (edit.destination < masterChain && input.synthUse[edit.destination] == SlotUse::present);
    };
    switch (edit.kind)
    {
        case RackEditKind::addSynth:
        case RackEditKind::addEffect:
        {
            const bool synth = edit.kind == RackEditKind::addSynth;
            if (synth ? edit.position > input.synthCount
                      : (!destinationValid() || edit.position > input.effectCount[edit.destination]))
                return RackEditResult::invalid;
            const auto result = selectSlot(input, synth, edit, slot);
            if (result != RackEditResult::applied) return result;
            if (synth)
            {
                candidate.synthUse[slot] = SlotUse::present;
                insert(candidate.synthOrder, candidate.synthCount, slot, edit.position);
            }
            else
            {
                candidate.effects[slot] = { SlotUse::present, edit.effect, edit.destination };
                insert(candidate.effectOrder[edit.destination], candidate.effectCount[edit.destination], slot, edit.position);
            }
            break;
        }
        case RackEditKind::removeSynth:
        case RackEditKind::moveSynth:
        {
            if (slot >= input.synthUse.size() || input.synthUse[slot] != SlotUse::present)
                return RackEditResult::invalid;
            if (edit.kind == RackEditKind::moveSynth && edit.position >= input.synthCount)
                return RackEditResult::invalid;
            erase(candidate.synthOrder, candidate.synthCount, slot);
            if (edit.kind == RackEditKind::moveSynth)
                insert(candidate.synthOrder, candidate.synthCount, slot, edit.position);
            else
            {
                candidate.synthUse[slot] = SlotUse::retired;
                for (std::size_t position = 0; position < candidate.effectCount[slot]; ++position)
                    candidate.effects[candidate.effectOrder[slot][position]].use = SlotUse::retired;
                candidate.effectOrder[slot].fill(noSlot);
                candidate.effectCount[slot] = 0;
            }
            break;
        }
        case RackEditKind::removeEffect:
        case RackEditKind::moveEffect:
        case RackEditKind::replaceEffect:
        {
            if (slot >= input.effects.size() || input.effects[slot].use != SlotUse::present)
                return RackEditResult::invalid;
            auto& effect = candidate.effects[slot];
            if (edit.kind == RackEditKind::replaceEffect)
            {
                if (!edit.allowReuse) return RackEditResult::replacementRequired;
                effect.kind = edit.effect;
                break;
            }
            if (edit.kind == RackEditKind::moveEffect)
            {
                if (!destinationValid()) return RackEditResult::invalid;
                const auto finalCount = input.effectCount[edit.destination] - (effect.chain == edit.destination ? 1 : 0);
                if (edit.position > finalCount) return RackEditResult::invalid;
            }
            erase(candidate.effectOrder[effect.chain], candidate.effectCount[effect.chain], slot);
            if (edit.kind == RackEditKind::removeEffect) effect.use = SlotUse::retired;
            else
            {
                effect.chain = edit.destination;
                insert(candidate.effectOrder[effect.chain], candidate.effectCount[effect.chain], slot, edit.position);
            }
            break;
        }
    }
    if (!validateRackLayout(candidate)) return RackEditResult::invalid;
    output = candidate;
    return RackEditResult::applied;
}
}

#include "RackLayout.h"
#include <algorithm>
#include <iostream>
#include <type_traits>

namespace
{
using namespace airack;
using Kind = RackEditKind;
using Result = RackEditResult;
int checks = 0, failures = 0, sections = 0;
void check(bool good, const char* what, int line)
{
    ++checks;
    if (!good) { ++failures; std::cout << "FAIL line " << line << ": " << what << '\n'; }
}
#define CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
void section(const char* name) { ++sections; std::cout << "SECTION " << name << '\n'; }

// These fixtures are stated directly, independently of the editor/validator.
RackLayout fullSynths()
{
    RackLayout layout;
    layout.synthUse.fill(SlotUse::present);
    layout.synthOrder = { 0, 1, 2, 3 };
    layout.synthCount = 4;
    return layout;
}
RackLayout distributed()
{
    auto layout = fullSynths();
    for (std::uint8_t slot = 0; slot < 8; ++slot)
    {
        const auto chain = static_cast<std::uint8_t>(slot % 5);
        layout.effects[slot] = { SlotUse::present, static_cast<EffectKind>(slot % 4), chain };
        layout.effectOrder[chain][layout.effectCount[chain]++] = slot;
    }
    return layout;
}
void applied(RackLayout& layout, RackEdit edit)
{
    const auto before = layout;
    RackLayout separate;
    CHECK(editRackLayout(before, edit, separate) == Result::applied);
    CHECK(editRackLayout(layout, edit, layout) == Result::applied);
    CHECK(layout == separate);
    CHECK(validateRackLayout(layout));
}
void rejected(const RackLayout& layout, RackEdit edit, Result expected = Result::invalid)
{
    auto output = distributed();
    output.effects[7].kind = EffectKind::compressor;
    const auto originalOutput = output;
    CHECK(editRackLayout(layout, edit, output) == expected);
    CHECK(output == originalOutput);
    auto alias = layout;
    CHECK(editRackLayout(alias, edit, alias) == expected);
    CHECK(alias == layout);
}

void canonical_and_empty_layout()
{
    section("canonical_and_empty_layout");
    static_assert(std::is_trivially_copyable_v<RackLayout>);
    static_assert(std::is_nothrow_copy_assignable_v<RackLayout>);
    RackLayout layout;
    static_assert(noexcept(validateRackLayout(layout)));
    static_assert(noexcept(editRackLayout(layout, RackEdit {}, layout)));
    CHECK(noSlot == 255 && masterChain == 4);
    CHECK(layout.synthUse.size() == 4 && layout.effects.size() == 8);
    CHECK(layout.effectOrder.size() == 5 && layout.effectCount.size() == 5);
    CHECK(layout.synthCount == 1 && layout.synthOrder == std::array<std::uint8_t, 4> { 0, 255, 255, 255 });
    CHECK(layout.synthUse == std::array<SlotUse, 4> { SlotUse::present, SlotUse::neverUsed, SlotUse::neverUsed, SlotUse::neverUsed });
    for (const auto& effect : layout.effects)
        CHECK(effect == EffectSlot { SlotUse::neverUsed, EffectKind::drive, masterChain });
    for (std::size_t chain = 0; chain < 5; ++chain)
    {
        CHECK(layout.effectCount[chain] == 0 && layout.effectOrder[chain].size() == 8);
        for (auto slot : layout.effectOrder[chain]) CHECK(slot == noSlot);
    }
    CHECK(validateRackLayout(layout));
    applied(layout, { Kind::removeSynth, 0 });
    CHECK(layout.synthCount == 0 && layout.synthUse[0] == SlotUse::retired);
    CHECK(layout.synthOrder == std::array<std::uint8_t, 4> { 255, 255, 255, 255 });
    applied(layout, { Kind::addEffect });
    CHECK(layout.effects[0].chain == masterChain && layout.effectCount[4] == 1);
    applied(layout, { Kind::addSynth });
    CHECK(layout.synthOrder[0] == 1 && layout.synthUse[0] == SlotUse::retired);
}

void corrupt_layout_rejects()
{
    section("corrupt_layout_rejects");
    const auto corrupt = [](const RackLayout& layout) {
        CHECK(!validateRackLayout(layout));
        for (int kind = 0; kind <= 6; ++kind)
            rejected(layout, { static_cast<Kind>(kind), 0, 4, 0, EffectKind::delay, true });
    };
    for (int value = 3; value <= 255; ++value)
    {
        auto layout = RackLayout {}; layout.synthUse[3] = static_cast<SlotUse>(value); corrupt(layout);
        layout = {}; layout.effects[7].use = static_cast<SlotUse>(value); corrupt(layout);
    }
    for (auto use : { SlotUse::neverUsed, SlotUse::retired, SlotUse::present })
    {
        auto base = use == SlotUse::present ? distributed() : RackLayout {};
        base.effects[7].use = use;
        for (int value = 19; value <= 255; ++value)
        {
            auto layout = base; layout.effects[7].kind = static_cast<EffectKind>(value); corrupt(layout);
        }
        for (int value = 5; value <= 255; ++value)
        {
            auto layout = base; layout.effects[7].chain = static_cast<std::uint8_t>(value); corrupt(layout);
        }
    }
    auto layout = RackLayout {}; layout.synthCount = 5; corrupt(layout);
    layout = {}; layout.synthCount = 255; corrupt(layout);
    layout = {}; layout.synthCount = 0; corrupt(layout); // Missing present synth.
    layout = fullSynths(); layout.synthOrder[3] = 0; corrupt(layout);
    layout = {}; layout.synthOrder[0] = 4; corrupt(layout);
    layout = {}; layout.synthOrder[1] = 0; corrupt(layout);
    layout = {}; layout.synthUse[0] = SlotUse::retired; corrupt(layout);
    layout = {}; layout.synthUse[0] = SlotUse::neverUsed; corrupt(layout);
    for (std::size_t chain = 0; chain < 5; ++chain)
    {
        layout = {}; layout.effectCount[chain] = 9; corrupt(layout);
        layout = {}; layout.effectCount[chain] = 255; corrupt(layout);
        layout = {}; layout.effectOrder[chain][7] = 0; corrupt(layout);
    }
    layout = distributed(); layout.effectOrder[0][1] = 0; corrupt(layout);
    layout = distributed(); layout.effectOrder[1][0] = 0; corrupt(layout); // Duplicate across chains.
    layout = distributed(); layout.effectOrder[0][0] = 8; corrupt(layout);
    layout = distributed(); layout.effectOrder[0][0] = noSlot; corrupt(layout);
    layout = distributed(); layout.effectCount[4] = 0; layout.effectOrder[4].fill(noSlot); corrupt(layout);
    layout = distributed(); layout.effects[0].use = SlotUse::neverUsed; corrupt(layout);
    layout = distributed(); layout.effects[0].use = SlotUse::retired; corrupt(layout);
    layout = distributed(); layout.effects[0].chain = 4; corrupt(layout);
    layout = distributed(); layout.synthUse[3] = SlotUse::retired; layout.synthCount = 3; layout.synthOrder[3] = noSlot; corrupt(layout);
    layout = distributed(); layout.effectCount[4] = 2; layout.effectOrder[4][1] = 0; corrupt(layout); // Nine entries globally.
    layout = {}; layout.effects[7] = { SlotUse::retired, EffectKind::reverb, 3 };
    CHECK(validateRackLayout(layout)); // Retired owner need not remain present.
    layout.effects[7].use = SlotUse::neverUsed;
    CHECK(validateRackLayout(layout));
}

void physical_identity_survives_reorder()
{
    section("physical_identity_survives_reorder");
    auto layout = distributed();
    const auto original = layout;
    applied(layout, { Kind::moveSynth, 0, 4, 3 });
    CHECK(layout.synthOrder == std::array<std::uint8_t, 4> { 1, 2, 3, 0 });
    applied(layout, { Kind::moveSynth, 0, 4, 0 });
    CHECK(layout == original);
    applied(layout, { Kind::moveEffect, 0, 0, 1 });
    CHECK(layout.effectOrder[0][0] == 5 && layout.effectOrder[0][1] == 0);
    CHECK(layout.effects == original.effects);
    applied(layout, { Kind::moveEffect, 0, 0, 0 });
    CHECK(layout == original);
    applied(layout, { Kind::moveSynth, 2, 4, 2 });
    applied(layout, { Kind::moveEffect, 5, 0, 1 });
    CHECK(layout == original);

    if (!validateRackLayout(layout)) return; // Do not enumerate from a failed baseline.
    // All 24 visible synth permutations retain physical insert ownership.
    std::array<std::uint8_t, 4> desired { 0, 1, 2, 3 };
    int permutations = 0;
    do
    {
        for (std::uint8_t position = 0; position < 4; ++position)
            applied(layout, { Kind::moveSynth, desired[position], 4, position });
        CHECK(layout.synthOrder == desired);
        CHECK(layout.synthUse == original.synthUse && layout.effects == original.effects);
        CHECK(layout.effectCount == original.effectCount && layout.effectOrder == original.effectOrder);
        ++permutations;
    } while (std::next_permutation(desired.begin(), desired.end()));
    CHECK(permutations == 24);

    // All 40,320 full effect-chain permutations, each reached through accepted public edits.
    layout = original;
    for (std::uint8_t slot = 0; slot < 8; ++slot)
        applied(layout, { Kind::moveEffect, slot, 4, 0 });
    auto kinds = layout.effects;
    std::array<std::uint8_t, 8> effectDesired { 0, 1, 2, 3, 4, 5, 6, 7 };
    permutations = 0;
    do
    {
        for (std::uint8_t position = 0; position < 8; ++position)
            applied(layout, { Kind::moveEffect, effectDesired[position], 4, position });
        CHECK(layout.effectOrder[4] == effectDesired);
        CHECK(layout.effects == kinds && layout.effectCount[4] == 8);
        ++permutations;
    } while (std::next_permutation(effectDesired.begin(), effectDesired.end()));
    CHECK(permutations == 40320);
}

void global_four_eight_capacity()
{
    section("global_four_eight_capacity");
    RackLayout layout;
    for (std::uint8_t i = 1; i < 4; ++i) applied(layout, { Kind::addSynth, noSlot, 4, i });
    CHECK(layout == fullSynths());
    rejected(layout, {}, Result::capacityReached);
    rejected(layout, { Kind::addSynth, 0 }); // Occupied is invalid even at capacity.
    rejected(layout, { Kind::addSynth, noSlot, 4, 5 });
    for (std::uint8_t i = 0; i < 8; ++i)
        applied(layout, { Kind::addEffect, noSlot, static_cast<std::uint8_t>(i % 5), static_cast<std::uint8_t>(i / 5), static_cast<EffectKind>(i % 4) });
    CHECK(layout == distributed());
    for (std::uint8_t chain = 0; chain < 5; ++chain)
        rejected(layout, { Kind::addEffect, noSlot, chain }, Result::capacityReached);
    rejected(layout, { Kind::addEffect, 0 });
    rejected(layout, { Kind::addEffect, noSlot, 5 });
    rejected(layout, { Kind::addEffect, noSlot, 4, 2 });
    auto allMaster = fullSynths();
    for (std::uint8_t i = 0; i < 8; ++i) applied(allMaster, { Kind::addEffect, i, 4, i });
    CHECK(allMaster.effectCount[4] == 8);
    rejected(allMaster, { Kind::addEffect, noSlot, 4, 8 }, Result::capacityReached);
    rejected(allMaster, { Kind::addEffect, noSlot, 4, 9 });
}

void retirement_and_explicit_reuse()
{
    section("retirement_and_explicit_reuse");
    RackLayout layout;
    applied(layout, { Kind::addEffect, 0, 0, 0, EffectKind::delay });
    applied(layout, { Kind::removeEffect, 0 });
    CHECK(layout.effects[0] == EffectSlot { SlotUse::retired, EffectKind::delay, 0 });
    applied(layout, { Kind::addEffect });
    CHECK(layout.effects[1].use == SlotUse::present && layout.effects[0].use == SlotUse::retired);
    rejected(layout, { Kind::addEffect, 0 }, Result::replacementRequired);
    applied(layout, { Kind::addEffect, 0, 4, 1, EffectKind::reverb, true });
    CHECK(layout.effects[0] == EffectSlot { SlotUse::present, EffectKind::reverb, 4 });
    const auto before = layout;
    rejected(layout, { Kind::replaceEffect, 0, 255, 255, EffectKind::compressor }, Result::replacementRequired);
    applied(layout, { Kind::replaceEffect, 0, 255, 255, EffectKind::compressor, true });
    auto expected = before; expected.effects[0].kind = EffectKind::compressor;
    CHECK(layout == expected);
    rejected(layout, { Kind::addEffect, 0, 4, 0, EffectKind::drive, true });
    applied(layout, { Kind::removeSynth, 0 });
    applied(layout, { Kind::addSynth, noSlot, 255 });
    CHECK(layout.synthOrder[0] == 1);
    rejected(layout, { Kind::addSynth, 0 }, Result::replacementRequired);
    applied(layout, { Kind::addSynth, 0, 255, 1, EffectKind::drive, true });
    CHECK(layout.synthOrder[1] == 0);

    layout = distributed();
    for (std::uint8_t slot = 0; slot < 4; ++slot) applied(layout, { Kind::removeSynth, slot });
    applied(layout, { Kind::removeEffect, 4 });
    CHECK(layout.synthCount == 0);
    rejected(layout, {}, Result::replacementRequired);
    rejected(layout, { Kind::addSynth, noSlot, 4, 0, EffectKind::drive, true }, Result::replacementRequired);
    rejected(layout, { Kind::addEffect }, Result::replacementRequired);
    rejected(layout, { Kind::addEffect, noSlot, 4, 0, EffectKind::drive, true }, Result::replacementRequired);
    rejected(layout, { Kind::addEffect, 0, 0, 0, EffectKind::drive, true }); // Retired owner.
    rejected(layout, { Kind::addSynth, 0, 4, 1 }, Result::invalid); // Invalid position precedes permission.
    rejected(layout, { Kind::addEffect, 0, 4, 1 }, Result::invalid);
    applied(layout, { Kind::addSynth, 3, 4, 0, EffectKind::drive, true });
    applied(layout, { Kind::addEffect, 7, 3, 0, EffectKind::delay, true });
    CHECK(layout.effects[7] == EffectSlot { SlotUse::present, EffectKind::delay, 3 });
    CHECK(layout.synthOrder[0] == 3);
}

void insert_master_moves_and_owner_removal()
{
    section("insert_master_moves_and_owner_removal");
    auto layout = distributed();
    const auto original = layout;
    applied(layout, { Kind::moveEffect, 0, 4, 1 });
    CHECK(layout.effectOrder[0] == std::array<std::uint8_t, 8> { 5, 255, 255, 255, 255, 255, 255, 255 });
    CHECK(layout.effectOrder[4][0] == 4 && layout.effectOrder[4][1] == 0);
    CHECK(layout.effects[0] == EffectSlot { SlotUse::present, EffectKind::drive, 4 });
    applied(layout, { Kind::moveEffect, 0, 1, 1 });
    CHECK(layout.effectOrder[1][0] == 1 && layout.effectOrder[1][1] == 0 && layout.effectOrder[1][2] == 6);
    applied(layout, { Kind::moveEffect, 0, 0, 0 });
    CHECK(layout == original);
    applied(layout, { Kind::removeSynth, 1 });
    auto expected = original;
    expected.synthUse[1] = SlotUse::retired;
    expected.synthCount = 3; expected.synthOrder = { 0, 2, 3, 255 };
    expected.effects[1].use = SlotUse::retired; expected.effects[6].use = SlotUse::retired;
    expected.effectOrder[1].fill(noSlot); expected.effectCount[1] = 0;
    CHECK(layout == expected);
    rejected(layout, { Kind::moveEffect, 0, 1 });
    applied(layout, { Kind::removeEffect, 4 });
    CHECK(layout.effectCount[4] == 0 && layout.effects[4].use == SlotUse::retired);
    applied(layout, { Kind::moveEffect, 5, 4, 0 });
    applied(layout, { Kind::removeSynth, 0 });
    CHECK(layout.effects[5] == EffectSlot { SlotUse::present, EffectKind::delay, 4 });

    // Exercise every source/destination owner pair at full global capacity.
    for (std::uint8_t source = 0; source < 5; ++source)
        for (std::uint8_t destination = 0; destination < 5; ++destination)
        {
            layout = original;
            const auto slot = layout.effectOrder[source][0];
            const auto end = static_cast<std::uint8_t>(layout.effectCount[destination] - (source == destination ? 1 : 0));
            applied(layout, { Kind::moveEffect, slot, destination, end });
            CHECK(layout.effectOrder[destination][end] == slot);
            CHECK(layout.effects[slot].kind == original.effects[slot].kind);
            applied(layout, { Kind::moveEffect, slot, source, 0 });
            CHECK(layout == original);
        }
}

void invalid_edit_preserves_output_and_aliasing()
{
    section("invalid_edit_preserves_output_and_aliasing");
    const auto layout = distributed();
    for (int value = 7; value <= 255; ++value) rejected(layout, { static_cast<Kind>(value) });
    for (int kind = 0; kind <= 6; ++kind)
    {
        for (int value = 19; value <= 255; ++value)
            rejected(layout, { static_cast<Kind>(kind), 0, 4, 0, static_cast<EffectKind>(value), true });
        for (auto slot : { 8, 254, 255 })
        {
            if (slot == 255 && (kind == 0 || kind == 3)) continue;
            rejected(layout, { static_cast<Kind>(kind), static_cast<std::uint8_t>(slot), 4, 0, EffectKind::drive, true });
        }
    }
    for (auto kind : { Kind::addSynth, Kind::removeSynth, Kind::moveSynth })
        rejected(layout, { kind, 4 });
    for (auto kind : { Kind::removeSynth, Kind::moveSynth, Kind::removeEffect, Kind::moveEffect, Kind::replaceEffect })
    {
        rejected(RackLayout {}, { kind, 3, 4, 0, EffectKind::drive, true });
        auto retired = layout;
        applied(retired, { Kind::removeSynth, 3 });
        rejected(retired, { kind, 3, 4, 0, EffectKind::drive, true });
    }
    rejected(layout, { Kind::moveSynth, 0, 4, 4 });
    rejected(layout, { Kind::moveEffect, 0, 0, 2 });
    rejected(layout, { Kind::moveEffect, 0, 4, 2 });
    rejected(layout, { Kind::moveEffect, 0, 255 });
    rejected(RackLayout {}, { Kind::addEffect, noSlot, 3 });
    // Unused fields do not introduce extra policy; unknown EffectKind still always rejects.
    auto changed = layout;
    applied(changed, { Kind::removeEffect, 0, 255, 255, EffectKind::reverb, false });
    CHECK(changed.effects[0] == EffectSlot { SlotUse::retired, EffectKind::drive, 0 });
    applied(changed, { Kind::moveSynth, 0, 255, 1 });
    CHECK(changed.synthOrder[1] == 0);
}
}

void effect_eq_layout_authority()
{
    section("effect_eq_layout_authority");
    RackLayout layout,out;
    RackEdit e;e.kind=RackEditKind::addEffect;e.effect=static_cast<EffectKind>(4);
    CHECK(editRackLayout(layout,e,out)==RackEditResult::applied);
    if(out.effects[0].use!=SlotUse::present)return;
    CHECK(out.effects[0].kind==static_cast<EffectKind>(4)&&validateRackLayout(out));
    const auto before=out;e.effect=static_cast<EffectKind>(255);
    CHECK(editRackLayout(out,e,out)==RackEditResult::invalid&&out==before);
}
void filter_family_layout_availability()
{
    RackLayout original,result;RackEdit e;e.kind=RackEditKind::addEffect;e.effect=static_cast<EffectKind>(5);e.slot=0;e.destination=masterChain;e.position=0;
    CHECK(editRackLayout(original,e,result)==RackEditResult::applied);
}
void chorus_family_layout_availability()
{
    RackLayout original,result;RackEdit e;e.kind=RackEditKind::addEffect;e.effect=static_cast<EffectKind>(6);e.slot=0;e.destination=masterChain;e.position=0;
    CHECK(editRackLayout(original,e,result)==RackEditResult::applied);
}
void era_reverb_family_layout_availability()
{
    section("era_reverb_family_layout_availability");
    RackLayout original,result;RackEdit e;e.kind=RackEditKind::addEffect;e.effect=EffectKind::eraReverb;e.slot=0;e.destination=masterChain;e.position=0;
    CHECK(static_cast<int>(EffectKind::eraReverb)==17&&editRackLayout(original,e,result)==RackEditResult::applied&&validateRackLayout(result));
}
void saturation_family_layout_availability()
{
    section("saturation_family_layout_availability");
    CHECK(static_cast<int>(EffectKind::saturation) == 18);
    for (auto use : { SlotUse::neverUsed, SlotUse::retired, SlotUse::present })
    {
        auto layout = use == SlotUse::present ? distributed() : RackLayout {};
        layout.effects[7].use = use;
        layout.effects[7].kind = EffectKind::saturation;
        CHECK(validateRackLayout(layout));
    }
    RackLayout layout;
    applied(layout, { Kind::addEffect, 0, masterChain, 0, EffectKind::saturation });
    CHECK(layout.effects[0] == EffectSlot { SlotUse::present, EffectKind::saturation, masterChain });
    CHECK(layout.effectCount[masterChain] == 1 && layout.effectOrder[masterChain][0] == 0);
    applied(layout, { Kind::moveEffect, 0, 0, 0 });
    CHECK(layout.effects[0] == EffectSlot { SlotUse::present, EffectKind::saturation, 0 });
    CHECK(layout.effectCount[masterChain] == 0 && layout.effectCount[0] == 1 && layout.effectOrder[0][0] == 0);
}
int main()
{ auto midSideSplit=RackLayout{};midSideSplit.effects[0]={SlotUse::present,static_cast<EffectKind>(14),masterChain};midSideSplit.effectCount[masterChain]=1;midSideSplit.effectOrder[masterChain][0]=0;
    section("mid_side_split_family_layout_availability");CHECK(validateRackLayout(midSideSplit));
    auto frequencySplit=RackLayout{};frequencySplit.effects[0]={SlotUse::present,static_cast<EffectKind>(13),masterChain};frequencySplit.effectCount[masterChain]=1;frequencySplit.effectOrder[masterChain][0]=0;
    section("frequency_split_family_layout_availability");CHECK(validateRackLayout(frequencySplit));
    auto convolution=RackLayout{};convolution.effects[0]={SlotUse::present,static_cast<EffectKind>(12),masterChain};convolution.effectCount[masterChain]=1;convolution.effectOrder[masterChain][0]=0;
    section("convolution_family_layout_availability");CHECK(validateRackLayout(convolution));
    auto frequencyShift=RackLayout{};frequencyShift.effects[0]={SlotUse::present,static_cast<EffectKind>(11),masterChain};frequencyShift.effectCount[masterChain]=1;frequencyShift.effectOrder[masterChain][0]=0;
    section("frequency_shift_family_layout_availability");CHECK(validateRackLayout(frequencyShift));
    auto dimension=RackLayout{};dimension.effects[0]={SlotUse::present,static_cast<EffectKind>(10),masterChain};dimension.effectCount[masterChain]=1;dimension.effectOrder[masterChain][0]=0;
    section("dimension_family_layout_availability");CHECK(validateRackLayout(dimension));
    auto stereo=RackLayout{};stereo.effects[0]={SlotUse::present,static_cast<EffectKind>(9),masterChain};stereo.effectCount[masterChain]=1;stereo.effectOrder[masterChain][0]=0;
    section("stereo_family_layout_availability");CHECK(validateRackLayout(stereo));
    auto flanger=RackLayout{};flanger.effects[0]={SlotUse::present,static_cast<EffectKind>(8),masterChain};flanger.effectCount[masterChain]=1;flanger.effectOrder[masterChain][0]=0;
    section("flanger_family_layout_availability");CHECK(validateRackLayout(flanger));
    auto phaser=RackLayout{};phaser.effects[0]={SlotUse::present,static_cast<EffectKind>(7),masterChain};phaser.effectCount[masterChain]=1;phaser.effectOrder[masterChain][0]=0;
    section("phaser_family_layout_availability");CHECK(validateRackLayout(phaser));
    era_reverb_family_layout_availability(); saturation_family_layout_availability(); chorus_family_layout_availability();
    filter_family_layout_availability(); effect_eq_layout_authority();
    canonical_and_empty_layout();
    corrupt_layout_rejects();
    physical_identity_survives_reorder();
    global_four_eight_capacity();
    retirement_and_explicit_reuse();
    insert_master_moves_and_owner_removal();
    invalid_edit_preserves_output_and_aliasing();
    std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

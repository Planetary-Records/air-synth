#include "UserReach.h"

namespace airack
{
std::span<const std::size_t> houseParameters()
{
    static const auto indices = []
    {
        const juce::StringArray gates { "level", "pan", "mute", "solo" };
        const auto registry = parameterRegistry();
        std::vector<std::size_t> result;
        for (std::size_t i = 0; i < registry.size(); ++i)
        {
            const auto& id = registry[i].id;
            const auto key = id.fromFirstOccurrenceOf(".", false, false).fromFirstOccurrenceOf(".", false, false);
            if (i < 12 || id == "output.gain_db" || id == "output.protection" || (id.startsWith("synth.") && gates.contains(key)))
                result.push_back(i);
        }
        return result;
    }();
    return indices;
}

const RackUnit* bankUnit(const SoundDocument& d, RackRole role, std::uint8_t bank)
{
    const bool synth = role == RackRole::synth;
    if (d.personalities != nullptr)
        for (const auto& rack : d.personalities->instances)
            for (const auto& unit : rack.design.units)
                if (isSourceModule(unit.module) == synth)
                    if (const auto* binding = rackUnitBinding(rack, unit.id); binding != nullptr && binding->slot == bank)
                        return &unit;
    return nullptr;
}

NativeModule bankModule(const SoundDocument& d, RackRole role, std::uint8_t bank)
{
    const bool synth = role == RackRole::synth;
    if(bank>=(synth?4:8))return static_cast<NativeModule>(255);
    if (synth)
        return sourceModule(d,bank);
    return nativeModuleForEffect(d.layout.effects[bank].kind);
}

std::vector<std::size_t> unitReach(NativeModule module, std::uint8_t bank, AlgorithmRevision revision)
{
    const bool synth = isSourceModule(module);
    if((!synth&&!isEffectModule(module))||bank>=(synth?4:8))return {};
    const bool warmth = !(module == NativeModule::wavetable && revision == legacyAlgorithmRevision);
    const auto prefix = juce::String(synth ? "synth." : "effect.") + juce::String(bank + 1) + ".";
    const auto registry = parameterRegistry();
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < registry.size(); ++i)
    {
        if (!registry[i].id.startsWith(prefix) || (!warmth && i >= wavetableWarmthParameterStart)) continue;
        const auto key = registry[i].id.substring(prefix.length());
        if (rackParameterActive(module, key) && !(synth && (key == "engine"||key=="drum_mode"))) result.push_back(i);
    }
    return result;
}

std::bitset<parameterCount> userReach(const SoundDocument& d)
{
    std::bitset<parameterCount> reach;
    for (const auto i : houseParameters()) reach.set(i);
    const auto add = [&](RackRole role, std::uint8_t bank)
    {
        const auto module = bankModule(d, role, bank);
        const auto revision = rackAlgorithmRevision(d.compatibility,module,bank);
        for (const auto i : unitReach(module, bank, revision)) reach.set(i);
    };
    for (std::uint8_t b = 0; b < 4; ++b)
        if (d.layout.synthUse[b] == SlotUse::present) add(RackRole::synth, b);
    for (std::uint8_t b = 0; b < 8; ++b)
        if (d.layout.effects[b].use == SlotUse::present) add(RackRole::fx, b);
    return reach;
}
}

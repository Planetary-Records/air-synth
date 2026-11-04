#include "AiCatalog.h"
#include "ProposalDocument.h"
#include "RackExamples.h"
#include "UserReach.h"
#include "StrictJson.h"
#include <algorithm>

namespace airack::ai
{
namespace
{
constexpr const char* effectNames[] {"drive", "delay", "reverb", "compressor", "eq", "filter_fx", "chorus", "phaser", "flanger", "stereo", "dimension", "frequency_shift", "convolution", "frequency_split", "mid_side_split", "dynamic_eq", "space_reverb", "era_reverb", "saturation"};
static_assert(std::size(effectNames)==std::size_t(EffectKind::saturation)+1);
constexpr const char* useNames[] {"never_used", "present", "retired"};
constexpr const char* gridNames[] {"quarter", "eighth", "sixteenth", "thirty_second", "sixty_fourth"};
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject;
    for (const auto& f : fields) o->setProperty(f.first, f.second);
    return o;
}
template<std::size_t N>
juce::var order(const std::array<std::uint8_t, N>& slots, std::uint8_t count)
{
    juce::Array<juce::var> values;
    for (std::uint8_t i = 0; i < count; ++i) values.add(static_cast<int>(slots[i]));
    return values;
}
template<std::size_t N>
int position(const std::array<std::uint8_t, N>& order, std::uint8_t count, std::size_t slot)
{
    const auto end = order.begin() + count, found = std::find(order.begin(), end, slot);
    return found == end ? -1 : static_cast<int>(found - order.begin());
}
float physical(const ProjectDocument& d, const juce::String& id)
{
    const auto r = parameterRegistry();
    const auto found = std::find_if(r.begin(), r.end(), [&](const auto& field) { return field.id == id; });
    return parameterPhysicalValue(d.sound.values, static_cast<std::size_t>(found - r.begin()));
}
juce::var patternWire(const PatternDocument& p)
{
    juce::Array<juce::var> notes;
    for (std::size_t i = 0; i < p.noteCount; ++i) {
        const auto& n = p.notes[i];
        notes.add(object({{"start_beats", n.startBeats}, {"duration_beats", n.durationBeats},
            {"pitch", static_cast<int>(n.pitch)}, {"velocity", static_cast<int>(n.velocity)}, {"channel", static_cast<int>(n.channel)}}));
    }
    return object({{"bars", static_cast<int>(p.bars)}, {"meter_numerator", static_cast<int>(p.meter.numerator)},
        {"meter_denominator", static_cast<int>(p.meter.denominator)}, {"grid", gridNames[static_cast<std::size_t>(p.grid)]},
        {"swing", p.swing}, {"notes", notes}});
}
}

bool catalogSliceFor(const ProjectDocument& source,CatalogRendering rendering,std::size_t first,std::size_t count,juce::var& output)
{
    // Populate native key lookup before validating documents with saved bindings.
    const auto registry = parameterRegistry();
    if (rendering>CatalogRendering::ownedLocal||first>registry.size()||count>registry.size()-first||!validateProjectDocument(source)) return false;
    const auto& sound = source.sound; const auto& layout = sound.layout;
    for(std::uint8_t bank=0;bank<4;++bank)if(rendering==CatalogRendering::companionPcm&&layout.synthUse[bank]==SlotUse::present&&sourceModule(sound,bank)==NativeModule::drum)return false;
    std::array<bool, 4> enabled {}, wavetable {};
    for (std::size_t slot = 0; slot < enabled.size(); ++slot) {
        const auto prefix = "synth." + juce::String(static_cast<int>(slot + 1)) + ".";
        enabled[slot] = physical(source, prefix + "enabled") == 1;
        wavetable[slot] = physical(source, prefix + "engine") == 1;
    }
    juce::Array<juce::var> controls;
    for (std::size_t i = first; i < first+count; ++i) {
        const auto& field = registry[i];
        const auto tokens = juce::StringArray::fromTokens(field.id, ".", "");
        int slot = -1; juce::String module = tokens[0]; bool active = true, locked = sound.controlLocks[i];
        if (tokens[0] == "synth") {
            slot = tokens[1].getIntValue() - 1;
            const auto s = static_cast<std::size_t>(slot);
            locked = locked || sound.synthLocks[s]; active = layout.synthUse[s] == SlotUse::present && enabled[s];
            if (tokens[2] == "analog" || tokens[2] == "wavetable") {
                module = tokens[2]; active = active && (rendering==CatalogRendering::companionPcm||sourceModule(sound,std::uint8_t(s))!=NativeModule::drum) && ((module == "wavetable") == wavetable[s]);
            } else if (tokens[2] == "lfo" || tokens[2] == "modulation") module = tokens[2];
            else if(rendering==CatalogRendering::ownedLocal&&tokens[2]=="drum"){module="drum";active=active&&sourceModule(sound,std::uint8_t(s))==NativeModule::drum;}
        } else if (tokens[0] == "effect") {
            slot = tokens[1].getIntValue() - 1;
            const auto s = static_cast<std::size_t>(slot);
            locked = locked || sound.effectLocks[s]; active = layout.effects[s].use == SlotUse::present;
            for (auto name : effectNames) if (tokens[2] == name) {
                module = name; active = active && module == effectNames[static_cast<std::size_t>(layout.effects[s].kind)]; break;
            }
        }
        juce::Array<juce::var> choices;
        for (const auto& choice : field.choices) choices.add(choice);
        controls.add(object({{"id", field.id}, {"name", field.name},
            {"kind", field.kind == ParameterKind::continuous ? "continuous" : field.kind == ParameterKind::choice ? "choice" : "toggle"},
            {"unit", field.unit}, {"minimum", static_cast<double>(field.minimum)}, {"maximum", static_cast<double>(field.maximum)},
            {"default", static_cast<double>(field.defaultValue)}, {"skew", static_cast<double>(field.skew)}, {"choices", choices},
            {"value_normalized", static_cast<double>(sound.values[i])}, {"value_physical", static_cast<double>(parameterPhysicalValue(sound.values, i))},
            {"locked", locked}, {"slot", slot}, {"module", module}, {"active", active}}));
    }
    juce::Array<juce::var> synths, effects, effectOrders, bindings;
    for (std::size_t slot = 0; slot < layout.synthUse.size(); ++slot)
        synths.add(object({{"slot", static_cast<int>(slot)}, {"use", useNames[static_cast<std::size_t>(layout.synthUse[slot])]},
            {"position", position(layout.synthOrder, layout.synthCount, slot)}, {"locked", sound.synthLocks[slot]}}));
    for (std::size_t slot = 0; slot < layout.effects.size(); ++slot) {
        const auto& e = layout.effects[slot];
        effects.add(object({{"slot", static_cast<int>(slot)}, {"use", useNames[static_cast<std::size_t>(e.use)]},
            {"position", position(layout.effectOrder[e.chain], layout.effectCount[e.chain], slot)},
            {"chain", static_cast<int>(e.chain)}, {"kind", effectNames[static_cast<std::size_t>(e.kind)]}, {"locked", sound.effectLocks[slot]}}));
    }
    for (std::size_t chain = 0; chain < layout.effectOrder.size(); ++chain) effectOrders.add(order(layout.effectOrder[chain], layout.effectCount[chain]));
    for (std::size_t i = 0; i < sound.macros.size(); ++i) {
        const auto& b = sound.macros[i];
        const auto target = parameterIndexForKey(b.targetKey);
        bindings.add(object({{"index", static_cast<int>(i + 1)}, {"target_id", target ? juce::var(registry[*target].id) : juce::var()},
            {"amount", static_cast<double>(b.amount)}}));
    }
    juce::Array<juce::var> modules;
    for (auto name : {"global", "macro", "output", "synth", "analog", "wavetable", "lfo", "modulation", "effect", "drive", "delay", "reverb", "compressor", "eq", "filter_fx", "chorus", "phaser", "flanger", "stereo", "dimension", "frequency_shift", "convolution", "frequency_split", "mid_side_split", "dynamic_eq", "space_reverb", "era_reverb", "saturation"}) modules.add(name);
    if(rendering==CatalogRendering::ownedLocal)modules.add("drum");
    output = object({{"version", 1}, {"limits", object({
            {"synths", static_cast<int>(layout.synthUse.size())}, {"effects", static_cast<int>(layout.effects.size())}, {"musical_notes", 16},
            {"parameters", static_cast<int>(registry.size())}, {"macros", static_cast<int>(sound.macros.size())}, {"modulation_routes_per_synth", 16},
            {"pattern_notes", static_cast<int>(maximumPatternNotes)}, {"pattern_bars", 8}, {"json_depth", 24}, {"proposal_bytes", 262144},
            {"proposal_parameter_edits", static_cast<int>(parameterCount)}, {"proposal_rack_edits", 64}, {"proposal_macro_edits", 8}, {"explanation_bytes", 2048}})},
        {"controls", controls}, {"racks", object({{"synths", synths}, {"effects", effects}, {"synth_order", order(layout.synthOrder, layout.synthCount)}, {"effect_order", effectOrders}})},
        {"macro_bindings", bindings}, {"pattern", patternWire(source.pattern)},
        {"playback_mode", source.mode == PlaybackMode::externalMidi ? "external_midi" : "internal_pattern"}, {"modules", modules},
        {"wire_guidance", object({{"integer_fields", "Use plain JSON integer tokens. Decimal and exponent forms are rejected by native validation."}})}});
    return true;
}
bool catalogFor(const ProjectDocument& source,CatalogRendering rendering,juce::var& output)
{return catalogSliceFor(source,rendering,0,parameterCount,output);}
bool catalogInventoryFor(const RackCatalogSnapshot& snapshot,CatalogRendering rendering,juce::var& output)
{
    try {
        if(rendering>CatalogRendering::ownedLocal)return false;
        // An end-cursor page validates the private seal even for a sealed empty snapshot.
        // It is only an admission check; the output below always includes every entry.
        RackCatalogPage admitted;
        if (!catalogPage(snapshot, {}, snapshot.entries.size(), 65536, admitted)) return false;
        RackCatalogPage inventory;
        inventory.snapshot = snapshot.id; inventory.complete = true;
        inventory.entries.reserve(snapshot.entries.size());
        for (const auto& entry : snapshot.entries) {
            RackDesign design;
            if (!resolveCatalogDesign(snapshot, entry.definition.id, entry.definition.revision, entry.contentSha256, design)) return false;
            if(rendering==CatalogRendering::companionPcm&&std::any_of(design.units.begin(),design.units.end(),[](const auto& u){return u.module==NativeModule::drum;}))continue;
            inventory.entries.push_back({std::move(design), juce::String::fromUTF8(entry.contentSha256.toRawUTF8())});
        }
        inventory.next=inventory.entries.size();
        auto candidate = rackCatalogPageDescriptor(inventory);
        std::size_t bytes;
        if (!rackCatalogEscapedSize(candidate, maximumCatalogDescriptorBytes, bytes)) return false;
        output = std::move(candidate); return true;
    } catch (...) { return false; }
}
bool providerCatalogFor(const ProjectDocument& source,CatalogRendering rendering,juce::var& output)
{
    try {
        std::vector<RackDesign> embedded;
        if(source.sound.personalities)for(const auto& instance:source.sound.personalities->instances)embedded.push_back(instance.design);
        RackCatalogSnapshot snapshot;
        if(!captureRackCatalog(libraryRackDesigns(),embedded, {},snapshot))return false;
        return providerCatalogFor(source,snapshot,rendering,output);
    }catch(...){return false;}
}
static bool providerProjection(const ProjectDocument& source,juce::var inventory,CatalogRendering rendering,juce::var& output)
{
    try {
    juce::var legacy;
    if (!catalogFor(source,rendering,legacy)) return false;
    juce::Array<juce::var> rows, sets;
    const auto registry = parameterRegistry();
    const auto choiceSet = [&](std::size_t n) {
        juce::Array<juce::var> choices;
        for (const auto& choice : registry[n].choices) choices.add(choice);
        const juce::var set(choices); int index = 0;
        while (index < sets.size() && sets[index] != set) ++index;
        if (index == sets.size()) sets.add(set);
        return index;
    };
    const auto reach = userReach(source.sound);
    for (std::size_t n = 0; n < registry.size(); ++n) {
        if (!reach[n]) continue;
        const auto& p = registry[n]; const auto set = choiceSet(n);
        const auto& control = (*legacy["controls"].getArray())[int(n)];
        rows.add(juce::Array<juce::var>{p.id, p.unit, int(p.kind), double(p.minimum), double(p.maximum),
            double(p.defaultValue), double(p.skew), set, double(source.sound.values[n]), control["active"], control["locked"]});
    }
    juce::Array<juce::var> moduleNames{"analog","wavetable","drive","delay","reverb","compressor","eq","filter_fx","chorus", "phaser", "flanger", "stereo", "dimension", "frequency_shift", "convolution", "frequency_split", "mid_side_split", "dynamic_eq", "space_reverb", "era_reverb"};
    // Numeric module IDs stay stable when companion rendering hides the local drum source.
    moduleNames.add(rendering==CatalogRendering::ownedLocal?juce::var("drum"):juce::var());
    moduleNames.add("saturation");
    auto* moduleKeys = new juce::DynamicObject;
    for (int m = 0; m < moduleNames.size(); ++m) {
        if(moduleNames[m].isVoid())continue;
        const auto module = static_cast<NativeModule>(m);
        const auto prefix = juce::String(isSourceModule(module) ? "synth.1." : "effect.1.");
        juce::Array<juce::var> keys;
        for (const auto n : unitReach(module, 0)) {
            const auto& p = registry[n];
            keys.add(juce::Array<juce::var>{p.id.substring(prefix.length()), p.unit, int(p.kind), double(p.minimum), double(p.maximum),
                double(p.defaultValue), double(p.skew), choiceSet(n)});
        }
        moduleKeys->setProperty(juce::Identifier(moduleNames[m].toString()), keys);
    }
    auto compact = std::move(legacy); auto* o = compact.getDynamicObject();
    o->setProperty("controls", rows); o->setProperty("choice_sets", sets);
    o->setProperty("control_columns", juce::Array<juce::var>{"id", "unit", "kind", "min", "max", "default", "skew", "choice_set", "value_normalized", "active", "locked"});
    o->setProperty("module_keys", juce::var(moduleKeys));
    o->setProperty("module_key_columns", juce::Array<juce::var>{"key", "unit", "kind", "min", "max", "default", "skew", "choice_set"});
    const auto& sound = source.sound; const auto& layout = sound.layout;
    for(std::uint8_t bank=0;bank<4;++bank)if(rendering==CatalogRendering::companionPcm&&layout.synthUse[bank]==SlotUse::present&&sourceModule(sound,bank)==NativeModule::drum)return false;
    juce::Array<juce::var> synths, effects, bindings;
    for (std::size_t slot = 0; slot < layout.synthUse.size(); ++slot)
        synths.add(juce::Array<juce::var>{int(slot), int(layout.synthUse[slot]),
            position(layout.synthOrder, layout.synthCount, slot), sound.synthLocks[slot]});
    for (std::size_t slot = 0; slot < layout.effects.size(); ++slot) {
        const auto& e = layout.effects[slot];
        effects.add(juce::Array<juce::var>{int(slot), int(e.use),
            position(layout.effectOrder[e.chain], layout.effectCount[e.chain], slot), int(e.chain), int(e.kind), sound.effectLocks[slot]});
    }
    for (std::size_t n = 0; n < sound.macros.size(); ++n) {
        const auto& b = sound.macros[n]; const auto target = parameterIndexForKey(b.targetKey);
        bindings.add(juce::Array<juce::var>{int(n + 1), target ? juce::var(registry[*target].id) : juce::var(), double(b.amount)});
    }
    auto* racks = compact["racks"].getDynamicObject();
    racks->setProperty("synths", synths); racks->setProperty("effects", effects);
    racks->setProperty("synth_columns", juce::Array<juce::var>{"slot", "use", "position", "locked"});
    racks->setProperty("effect_columns", juce::Array<juce::var>{"slot", "use", "position", "chain", "kind", "locked"});
    o->setProperty("macro_bindings", bindings); o->setProperty("macro_columns", juce::Array<juce::var>{"index", "target_id", "amount"});
    compact["wire_guidance"].getDynamicObject()->setProperty("target_id", "Macro target parameter ID; null means unassigned. Macro index is one-based; rack slots/chains/positions are zero-based, position -1 means absent.");
    o->setProperty("projection_enums",object({
        {"parameter_kind",juce::Array<juce::var>{"continuous","choice","toggle"}},
        {"native_module",moduleNames},
        {"rack_role",juce::Array<juce::var>{"synth","fx"}},
        {"focus_widget",juce::Array<juce::var>{"rotary","verticalSlider","toggle","choice"}},
        {"focus_response",juce::Array<juce::var>{"linear","logarithmic"}},
        {"slot_use",juce::Array<juce::var>{"never_used","present","retired"}},
        {"effect_kind",juce::Array<juce::var>{"drive","delay","reverb","compressor","eq","filter_fx","chorus", "phaser", "flanger", "stereo", "dimension", "frequency_shift", "convolution", "frequency_split", "mid_side_split", "dynamic_eq", "space_reverb", "era_reverb", "saturation"}},
        {"pattern_grid",juce::Array<juce::var>{"quarter","eighth","sixteenth","thirty_second","sixty_fourth"}},
        {"playback_mode",juce::Array<juce::var>{"external_midi","internal_pattern"}}}));
    o->removeProperty("pattern"); // Requester musical context carries the saved pattern once.
    o->setProperty("rack_catalog", std::move(inventory));
    output = std::move(compact); return true;
    }catch(...){return false;}
}
bool providerCatalogFor(const ProjectDocument& source,const RackCatalogSnapshot& snapshot,CatalogRendering rendering,juce::var& output)
{
    juce::var inventory;return catalogInventoryFor(snapshot,rendering,inventory)&&providerProjection(source,std::move(inventory),rendering,output);
}
bool catalogInventoryFor(const RackInventorySnapshot& snapshot,CatalogRendering rendering,juce::var& output)
{
    auto descriptor=rackDiscoveryDescriptor(snapshot,rendering);if(descriptor.isVoid())return false;output=std::move(descriptor);return true;
}
bool providerCatalogFor(const ProjectDocument& source,const RackInventorySnapshot& snapshot,CatalogRendering rendering,juce::var& output)
{
    juce::var inventory;return catalogInventoryFor(snapshot,rendering,inventory)&&providerProjection(source,std::move(inventory),rendering,output);
}
bool providerCatalogFor(const ProjectDocument& source,const CatalogAuthority& authority,CatalogRendering rendering,juce::var& output)
{
    juce::var inventory;return authority.rendering==rendering&&detail::catalogAuthorityValid(authority)
        &&parseStrictJson(authority.descriptorJson,inventory,maximumCatalogDescriptorBytes)&&providerProjection(source,std::move(inventory),rendering,output);
}
bool catalogFor(const ProjectDocument& source,juce::var& out){return catalogFor(source,CatalogRendering::companionPcm,out);}
bool catalogInventoryFor(const RackCatalogSnapshot& snapshot,juce::var& out){return catalogInventoryFor(snapshot,CatalogRendering::companionPcm,out);}
bool providerCatalogFor(const ProjectDocument& source,juce::var& out){return providerCatalogFor(source,CatalogRendering::companionPcm,out);}
bool providerCatalogFor(const ProjectDocument& source,const RackCatalogSnapshot& snapshot,juce::var& out){return providerCatalogFor(source,snapshot,CatalogRendering::companionPcm,out);}
}

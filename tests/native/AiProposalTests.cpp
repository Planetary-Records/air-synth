#include "FrozenRackExamples.h"
#include "AiCatalog.h"
#include "OwnedFixture.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include <juce_cryptography/juce_cryptography.h>
#include "ProposalDocument.h"
#include "StrictJson.h"
#include "RackReconciliation.h"
#include "UserReach.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <clocale>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <string>

using namespace airack;
using namespace airack::ai;
namespace
{
int checks = 0, failures = 0, sections = 0;
const char* section = "";
void check(bool pass, const char* expression, int line)
{
    ++checks;
    if (!pass) { ++failures; if (failures < 80) std::cerr << section << ':' << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void begin(const char* name) { section = name; ++sections; std::cout << name << '\n'; }
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject;
    for (const auto& f : fields) o->setProperty(f.first, f.second);
    return o;
}
juce::var array(std::initializer_list<juce::var> values = {}) { return juce::Array<juce::var>(values); }
std::string json(const juce::var& v) { return juce::JSON::toString(v, true).toStdString(); }
std::size_t index(const juce::String& id)
{
    const auto r = parameterRegistry();
    const auto p = std::find_if(r.begin(), r.end(), [&](const auto& d) { return d.id == id; });
    CHECK(p != r.end()); return p == r.end() ? 0 : std::size_t(p - r.begin());
}
void set(ProjectDocument& d, const juce::String& id, float value) { d.sound.values[index(id)] = value; }
bool exact(const ProjectDocument& a, const ProjectDocument& b)
{
    if (!(a == b)) return false;
    for (std::size_t i = 0; i < parameterCount; ++i)
        if (std::bit_cast<std::uint32_t>(a.sound.values[i]) != std::bit_cast<std::uint32_t>(b.sound.values[i])) return false;
    for (std::size_t i = 0; i < 8; ++i)
        if (std::bit_cast<std::uint32_t>(a.sound.macros[i].amount) != std::bit_cast<std::uint32_t>(b.sound.macros[i].amount)) return false;
    if (std::bit_cast<std::uint64_t>(a.pattern.swing) != std::bit_cast<std::uint64_t>(b.pattern.swing)) return false;
    for (std::size_t i = 0; i < 256; ++i)
        if (std::bit_cast<std::uint64_t>(a.pattern.notes[i].startBeats) != std::bit_cast<std::uint64_t>(b.pattern.notes[i].startBeats)
            || std::bit_cast<std::uint64_t>(a.pattern.notes[i].durationBeats) != std::bit_cast<std::uint64_t>(b.pattern.notes[i].durationBeats)) return false;
    return true;
}
void layout(ProjectDocument& d, RackEdit e) { CHECK(editRackLayout(d.sound.layout, e, d.sound.layout) == RackEditResult::applied); }
ProjectDocument rich()
{
    ProjectDocument d;
    layout(d, {RackEditKind::addSynth, 1, masterChain, 1});
    layout(d, {RackEditKind::addSynth, 2, masterChain, 2});
    layout(d, {RackEditKind::removeSynth, 2});
    set(d, "synth.2.enabled", 1); set(d, "synth.2.engine", 1);
    layout(d, {RackEditKind::addEffect, 0, 0, 0, EffectKind::drive});
    layout(d, {RackEditKind::addEffect, 1, 1, 0, EffectKind::delay});
    layout(d, {RackEditKind::addEffect, 2, masterChain, 0, EffectKind::reverb});
    layout(d, {RackEditKind::addEffect, 3, masterChain, 1, EffectKind::compressor});
    layout(d, {RackEditKind::addEffect, 6, masterChain, 2});
    layout(d, {RackEditKind::removeEffect, 6});
    for (std::size_t i = 0; i < 8; ++i) d.sound.macros[i] = {parameterRegistry()[0].stateKey, float(i - 4.0) / 8};
    d.sound.synthLocks[2] = true; d.sound.effectLocks[7] = true;
    d.sound.controlLocks[index("synth.1.analog.filter.cutoff")] = true;
    d.pattern.bars = 2; d.pattern.meter = {7, 8}; d.pattern.noteCount = 2;
    d.pattern.notes[0] = {.013, .137, 0, 1, 1}; d.pattern.notes[1] = {6.5, .5, 127, 127, 16};
    d.mode = PlaybackMode::internalPattern;
    CHECK(validateProjectDocument(d)); return d;
}
juce::var wire()
{
    return object({{"version", 1}, {"parameter_edits", array()}, {"rack_edits", array()},
                   {"macro_edits", array()}, {"pattern", {}}, {"characteristics", "warm"}, {"explanation", "explicit edits"}});
}
juce::var param(const char* id, juce::var n) { return object({{"id", id}, {"normalized", n}}); }
juce::var rack(const char* kind, int slot, int chain = 4, int position = 0, const char* effect = "drive")
{ return object({{"kind", kind}, {"slot", slot}, {"chain", chain}, {"position", position}, {"effect", effect}}); }
juce::var macro(int i, juce::var target, juce::var amount) { return object({{"index", i}, {"target_id", target}, {"amount", amount}}); }
juce::var note(juce::var start = 0., juce::var duration = .25, juce::var pitch = 60, juce::var velocity = 100, juce::var channel = 1)
{ return object({{"start_beats", start}, {"duration_beats", duration}, {"pitch", pitch}, {"velocity", velocity}, {"channel", channel}}); }
juce::var pattern()
{ return object({{"bars", 1}, {"meter_numerator", 4}, {"meter_denominator", 4}, {"grid", "sixteenth"}, {"swing", 0.}, {"notes", array({note(.013, .137)})}}); }
bool same(const ProposalEdits& a, const ProposalEdits& b)
{
    if (a.parameters != b.parameters || a.macros != b.macros || a.pattern != b.pattern
        || a.characteristics != b.characteristics || a.explanation != b.explanation || a.name != b.name || a.racks.size() != b.racks.size()
        || a.reconciliation.detach != b.reconciliation.detach
        || a.reconciliation.remove != b.reconciliation.remove || a.reconciliation.replace != b.reconciliation.replace
        || a.reconciliation.controls.size() != b.reconciliation.controls.size()) return false;
    for (std::size_t i = 0; i < a.reconciliation.controls.size(); ++i) {
        const auto& x = a.reconciliation.controls[i]; const auto& y = b.reconciliation.controls[i];
        if (x.instance != y.instance || x.control != y.control || x.replacement != y.replacement) return false;
    }
    for (std::size_t i = 0; i < a.racks.size(); ++i) {
        const auto& x = a.racks[i]; const auto& y = b.racks[i];
        if (x.kind != y.kind || x.slot != y.slot || x.destination != y.destination || x.position != y.position || x.effect != y.effect || x.allowReuse != y.allowReuse) return false;
    }
    return true;
}
void badWire(const std::string& text)
{
    ProposalEdits output; output.parameters = {{"output.gain_db", .5f}}; output.characteristics = "retained";
    const auto before = output; CHECK(!decodeProposal(text, output)); CHECK(same(output, before));
}
void badWire(const juce::var& v) { badWire(json(v)); }
// A proposal's first parameter edit, rack edit and macro target, for a failure's report.
juce::String describe(const ProposalEdits& e)
{
    return (e.parameters.empty() ? juce::String() : e.parameters[0].id) + (e.racks.empty() ? "" : " rack edit " + juce::String(int(e.racks[0].kind)))
        + (e.macros.empty() ? juce::String() : " macro " + e.macros[0].targetId);
}
void reject(const ProjectDocument& source, ProposalEdits edits, ProposalPolicy policy = {},
            std::optional<ProposalStatus> wanted = {})
{
    auto output = rich(); const auto before = output;
    const auto status = admitProposal(source, policy, edits, output);
    CHECK(status != ProposalStatus::accepted); if (wanted) CHECK(status == *wanted); CHECK(exact(output, before));
    if (wanted && status != *wanted) std::cerr << section << ": status " << int(status) << ", wanted " << int(*wanted) << " for " << describe(edits) << '\n';
    auto alias = source;
    CHECK(admitProposal(alias, policy, edits, alias) == status); CHECK(exact(alias, source));
}
void closed(const juce::var& s, std::initializer_list<const char*> fields)
{
    CHECK(s["type"] == juce::var("object")); CHECK(s["additionalProperties"].isBool()); CHECK(!bool(s["additionalProperties"]));
    const auto* properties = s["properties"].getDynamicObject(); const auto* required = s["required"].getArray();
    CHECK(properties != nullptr); CHECK(required != nullptr);
    if (!properties || !required) return;
    CHECK(properties->getProperties().size() == int(fields.size())); CHECK(required->size() == int(fields.size()));
    for (auto f : fields) { CHECK(s["properties"].hasProperty(f)); CHECK(required->contains(juce::var(f))); }
}
void catalog_matches_all_758_controls()
{
    begin("catalog_matches_all_758_controls"); auto d = rich(); juce::var c;
    CHECK(catalogFor(d, c)); const auto* controls = c["controls"].getArray(); CHECK(controls != nullptr);
    if (!controls) return;
    CHECK(controls->size() == int(parameterCount)); const auto r = parameterRegistry();
    for (int i = 0; i < std::min(controls->size(), int(parameterCount)); ++i) {
        const auto& v = controls->getReference(i); const auto& p = r[std::size_t(i)];
        CHECK(v["id"].toString() == p.id); CHECK(v["name"].toString() == p.name); CHECK(v["unit"].toString() == p.unit);
        CHECK(v["kind"].toString() == (p.kind == ParameterKind::continuous ? "continuous" : p.kind == ParameterKind::choice ? "choice" : "toggle"));
        CHECK(double(v["minimum"]) == double(p.minimum)); CHECK(double(v["maximum"]) == double(p.maximum));
        CHECK(double(v["default"]) == double(p.defaultValue)); CHECK(double(v["skew"]) == double(p.skew));
        CHECK(double(v["value_normalized"]) == double(d.sound.values[std::size_t(i)]));
        CHECK(double(v["value_physical"]) == double(parameterPhysicalValue(d.sound.values, std::size_t(i))));
        const auto* choices = v["choices"].getArray(); CHECK(choices != nullptr);
        if (choices) { CHECK(choices->size() == int(p.choices.size())); for (int j = 0; j < choices->size(); ++j) CHECK((*choices)[j].toString() == p.choices[std::size_t(j)]); }
    }
    CHECK((*controls)[757]["id"].toString() == "output.protection"); CHECK(bool((*controls)[757]["active"]));
    CHECK(!bool((*controls)[int(index("synth.2.analog.filter.cutoff"))]["active"]));
    CHECK(bool((*controls)[int(index("synth.2.wavetable.filter.cutoff"))]["active"]));
    CHECK(!bool((*controls)[int(index("synth.3.enabled"))]["active"]));
    CHECK(!bool((*controls)[int(index("effect.1.delay.mix"))]["active"]));
    CHECK(bool((*controls)[int(index("effect.1.bypassed"))]["active"]));
}
void catalog_rack_locks_history_and_macros()
{
    begin("catalog_rack_locks_history_and_macros"); auto d = rich(); d.sound.synthLocks[1] = true;
    juce::var c; CHECK(catalogFor(d, c)); CHECK(int(c["version"]) == 1);
    CHECK(int(c["limits"]["parameters"]) == int(parameterCount)); CHECK(int(c["limits"]["synths"]) == 4); CHECK(int(c["limits"]["effects"]) == 8);
    CHECK(int(c["limits"]["musical_notes"]) == 16); CHECK(int(c["limits"]["proposal_bytes"]) == 262144);
    CHECK(c["playback_mode"].toString() == "internal_pattern");
    const auto* synths = c["racks"]["synths"].getArray(); const auto* effects = c["racks"]["effects"].getArray();
    CHECK(synths && synths->size() == 4); CHECK(effects && effects->size() == 8);
    if (synths) { CHECK((*synths)[2]["use"].toString() == "retired"); CHECK(bool((*synths)[2]["locked"])); CHECK((*synths)[3]["use"].toString() == "never_used"); }
    if (effects) { CHECK((*effects)[6]["use"].toString() == "retired"); CHECK(int((*effects)[1]["chain"]) == 1); CHECK((*effects)[3]["kind"].toString() == "compressor"); }
    const auto* controls = c["controls"].getArray();
    if (controls) { CHECK(bool((*controls)[int(index("synth.1.analog.filter.cutoff"))]["locked"])); CHECK(bool((*controls)[int(index("synth.2.lfo.1.rate_hz"))]["locked"])); CHECK(int((*controls)[757]["slot"]) == -1); }
    const auto* bindings = c["macro_bindings"].getArray(); CHECK(bindings && bindings->size() == 8);
    if (bindings) for (int i = 0; i < 8; ++i) { CHECK(int((*bindings)[i]["index"]) == i + 1); CHECK((*bindings)[i]["target_id"].toString() == "global.master_level"); CHECK(double((*bindings)[i]["amount"]) == double(d.sound.macros[std::size_t(i)].amount)); }
    auto w = wire(); w.getDynamicObject()->setProperty("pattern", c["pattern"]); ProposalEdits p;
    CHECK(decodeProposal(json(w), p)); CHECK(p.pattern && *p.pattern == d.pattern);
}
void schema_is_closed_required_nullable()
{
    begin("schema_is_closed_required_nullable"); const auto s = proposalSchema();
    closed(s, {"version", "parameter_edits", "rack_edits", "macro_edits", "pattern", "characteristics", "explanation", "reconciliation", "name"});
    const auto p = s["properties"];
    closed(p["parameter_edits"]["items"], {"id", "normalized"});
    closed(p["rack_edits"]["items"], {"kind", "slot", "chain", "position", "effect"});
    closed(p["macro_edits"]["items"], {"index", "target_id", "amount"});
    CHECK(!p["parameter_edits"]["items"]["properties"]["id"].hasProperty("enum"));
    const auto* branches = p["pattern"]["anyOf"].getArray(); CHECK(branches && branches->size() == 2);
    if (branches) { closed((*branches)[0], {"bars", "meter_numerator", "meter_denominator", "grid", "swing", "notes"}); closed((*branches)[0]["properties"]["notes"]["items"], {"start_beats", "duration_beats", "pitch", "velocity", "channel"}); CHECK((*branches)[1]["type"].toString() == "null"); }
    const auto* types = p["macro_edits"]["items"]["properties"]["target_id"]["type"].getArray(); CHECK(types && types->contains(juce::var("null")) && types->contains(juce::var("string")));
    ProposalEdits decoded; CHECK(decodeProposal(json(wire()), decoded)); CHECK(decoded.parameters.empty() && !decoded.pattern);
    auto w = wire(); w.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", .7)}));
    w.getDynamicObject()->setProperty("macro_edits", array({macro(8, {}, 0.)})); w.getDynamicObject()->setProperty("pattern", pattern());
    CHECK(decodeProposal(json(w), decoded)); CHECK(decoded.parameters.size() == 1); CHECK(decoded.macros.size() == 1); CHECK(decoded.pattern && decoded.pattern->notes[0].startBeats == .013);
}
void saturation_catalog_and_admission()
{
    begin(__func__);ProjectDocument source,output;juce::var compact;CHECK(providerCatalogFor(source,compact));
    const auto native=compact["projection_enums"]["native_module"],kinds=compact["projection_enums"]["effect_kind"];
    CHECK(native.isArray()&&native.size()==22&&native[20].isVoid()&&native[21].toString()=="saturation");
    CHECK(kinds.isArray()&&kinds.size()==19&&kinds[18].toString()=="saturation");
    CHECK(compact["module_keys"]["saturation"].isArray()&&compact["module_keys"]["saturation"].size()==36);
    for(std::uint8_t slot=0;slot<8;++slot){
        source=ProjectDocument{};output=source;
        auto w=wire();w.getDynamicObject()->setProperty("rack_edits",array({rack("add_effect",slot,masterChain,0,"saturation")}));
        ProposalEdits edits;const bool decoded=decodeProposal(json(w),edits);CHECK(decoded);if(!decoded)continue;
        // Direct bare-slot additions are outside the established AI placement authority.
        CHECK(admitProposal(source,{},edits,output)==ProposalStatus::outOfReach);CHECK(output==source);
        RackDesign design;design.name="Pending saturation contract";design.role=RackRole::fx;design.panel.sections={"Pending"};
        design.units={{0,NativeModule::saturation,{}}};RackOperation operation;operation.kind=RackOperationKind::place;operation.design=design;
        operation.destination={RackRole::fx,masterChain,0};
        for(std::uint8_t previous=0;previous<slot;++previous){layout(source,{RackEditKind::addEffect,previous,masterChain,0});layout(source,{RackEditKind::removeEffect,previous});}
        ProjectDocument placed=source;RackDifference difference;
        CHECK(makeRackCandidate(source.sound,operation,SoundEditOrigin::manual,placed.sound,difference)==RackCandidateStatus::ready);
        CHECK(placed.sound.layout.effects[slot].kind==EffectKind::saturation);
        CHECK(unitReach(NativeModule::saturation,slot).size()==36);
        juce::var full;CHECK(catalogFor(placed,full));CHECK(full["racks"]["effects"][slot]["kind"].toString()=="saturation");
        const auto prefix="effect."+juce::String(slot+1)+".";
        for(float raw:{0.f,1.f}){
            edits={};edits.parameters={{prefix+"bypassed",raw},{prefix+"saturation.full.drive_db",.5f},{"output.gain_db",.5f}};
            CHECK(admitProposal(placed,{},edits,output)==ProposalStatus::accepted);CHECK(output.sound.values[572+slot*23]==raw);
            RoutedPatch patch;CHECK(makeRoutedPatch(output.sound,patch));CHECK(patch.effects[slot].bypassed==(raw==1));
        }
    }
}
void every_control_is_addressable()
{
    begin("every_control_is_addressable"); ProjectDocument d, out;
    for (std::size_t i = 0; i < parameterCount; ++i) { ProposalEdits p; p.parameters = {{parameterRegistry()[i].id, d.sound.values[i]}}; CHECK(admitProposal(d, {}, p, out) == ProposalStatus::accepted); CHECK(exact(out, d)); }
    // effect.8 present, so its drive controls are in reach.
    layout(d, {RackEditKind::addEffect, 7, masterChain, 0, EffectKind::drive});
    ProposalEdits p; p.parameters = {{"output.gain_db", .8f}, {"output.protection", .2f}, {"effect.8.drive.shape", .75f}};
    CHECK(admitProposal(d, {}, p, out) == ProposalStatus::accepted);
    CHECK(out.sound.values[756] == .8f); CHECK(out.sound.values[757] == 0); CHECK(out.sound.values[index("effect.8.drive.shape")] == 1);
}
void required_pattern_admission()
{
    begin(__func__); const auto source = rich(); ProjectDocument output;
    CHECK(!ProposalPolicy{}.requirePattern);
    CHECK(static_cast<int>(ProposalStatus::invalidCandidate) == 8);
    CHECK(static_cast<int>(ProposalStatus::requestedPatternMissing) == 9);
    CHECK(static_cast<int>(ProposalStatus::outOfReach) == 10);
    for (auto scope : {RequestScope::pattern, RequestScope::both}) {
        ProposalPolicy policy{scope, false, false, true}; ProposalEdits edits;
        reject(source, edits, policy, ProposalStatus::requestedPatternMissing);
        edits.pattern = PatternDocument{};
        reject(source, edits, policy, ProposalStatus::requestedPatternMissing);
        auto w = wire(); w.getDynamicObject()->setProperty("pattern", pattern());
        CHECK(decodeProposal(json(w), edits));
        CHECK(admitProposal(source, policy, edits, output) == ProposalStatus::accepted);
        auto expected = source; expected.pattern = PatternDocument{};
        expected.pattern.noteCount = 1; expected.pattern.notes[0] = {.013, .137, 60, 100, 1};
        CHECK(exact(output, expected));
        policy.requirePattern = false;
        CHECK(admitProposal(source, policy, {}, output) == ProposalStatus::accepted); CHECK(exact(output, source));
        edits = {}; edits.pattern = PatternDocument{};
        CHECK(admitProposal(source, policy, edits, output) == ProposalStatus::accepted);
        expected = source; expected.pattern = *edits.pattern; CHECK(exact(output, expected));
    }
    reject(source, {}, {RequestScope::sound, false, false, true}, ProposalStatus::malformed);
    ProposalEdits edits; edits.pattern = PatternDocument{}; edits.pattern->noteCount = 1;
    edits.pattern->notes[0] = {0, .25, 45, 96, 1};
    reject(source, edits, {RequestScope::sound, false, false, true}, ProposalStatus::malformed);
}
void required_pattern_schema()
{
    begin(__func__); const auto optional = proposalSchema();
    for (auto scope : {RequestScope::sound, RequestScope::pattern, RequestScope::both})
        CHECK(json(proposalSchema({scope, true, true, false})) == json(optional));
    const auto* branches = optional["properties"]["pattern"]["anyOf"].getArray();
    CHECK(branches && branches->size() == 2); if (!branches || branches->size() != 2) return;
    CHECK(!(*branches)[0]["properties"]["notes"].hasProperty("minItems"));
    auto expected = optional.clone(); auto expectedPattern = (*branches)[0].clone();
    expectedPattern["properties"]["notes"].getDynamicObject()->setProperty("minItems", 1);
    expected["properties"].getDynamicObject()->setProperty("pattern", expectedPattern);
    for (auto scope : {RequestScope::pattern, RequestScope::both}) {
        const auto required = proposalSchema({scope, true, true, true});
        CHECK(json(required) == json(expected));
        const auto p = required["properties"]["pattern"];
        CHECK(!p.hasProperty("anyOf"));
        closed(p, {"bars", "meter_numerator", "meter_denominator", "grid", "swing", "notes"});
        closed(p["properties"]["notes"]["items"], {"start_beats", "duration_beats", "pitch", "velocity", "channel"});
        CHECK(p["properties"]["notes"]["type"].toString() == "array");
        CHECK(p["properties"]["notes"]["minItems"].isInt()); CHECK(int(p["properties"]["notes"]["minItems"]) == 1);
    }
    CHECK(json(proposalSchema()) == json(optional));
}
void sound_pattern_both_scope()
{
    begin("sound_pattern_both_scope"); auto d = rich(); ProjectDocument out;
    ProposalEdits sound; sound.parameters = {{"output.gain_db", .8f}};
    CHECK(admitProposal(d, {}, sound, out) == ProposalStatus::accepted); auto wanted = d; wanted.sound.values[756] = .8f; CHECK(exact(out, wanted));
    ProposalEdits notes; notes.pattern = PatternDocument{}; notes.pattern->noteCount = 1; notes.pattern->notes[0] = {.013, .137, 70, 99, 2};
    ProposalPolicy pp{RequestScope::pattern}; CHECK(admitProposal(d, pp, notes, out) == ProposalStatus::accepted); wanted = d; wanted.pattern = *notes.pattern; CHECK(exact(out, wanted));
    sound.pattern = notes.pattern; CHECK(admitProposal(d, {RequestScope::both}, sound, out) == ProposalStatus::accepted); wanted.sound.values[756] = .8f; CHECK(exact(out, wanted));
    reject(d, sound, {}, ProposalStatus::scopeViolation); reject(d, sound, pp, ProposalStatus::scopeViolation);
    for (int kind = 0; kind < 3; ++kind) { ProposalEdits p; if (kind == 0) p.parameters = {{"output.protection", 1}}; if (kind == 1) p.racks = {{RackEditKind::removeEffect, 0}}; if (kind == 2) p.macros = {{1, {}, 0}}; reject(d, p, pp, ProposalStatus::scopeViolation); }
    CHECK(admitProposal(d, {}, {}, out) == ProposalStatus::accepted); CHECK(exact(out, d)); reject(d, {}, {static_cast<RequestScope>(255)});
}
void macro_assignment_permission_and_locks()
{
    begin("macro_assignment_permission_and_locks"); auto d = rich(); ProjectDocument out; ProposalEdits p;
    p.parameters = {{"macro.8", .5f}}; CHECK(admitProposal(d, {}, p, out) == ProposalStatus::accepted); CHECK(out.sound.macros == d.sound.macros);
    p.parameters.clear(); for (int i = 1; i <= 8; ++i) p.macros.push_back({std::uint8_t(i), "global.master_level", float(i - 5) / 8});
    reject(d, p, {}, ProposalStatus::macroChangeNotRequested);
    CHECK(admitProposal(d, {RequestScope::sound, true}, p, out) == ProposalStatus::accepted); CHECK(out.sound.macros == d.sound.macros);
    p.macros = {{8, "output.gain_db", .25f}}; CHECK(admitProposal(d, {RequestScope::sound, true}, p, out) == ProposalStatus::accepted); CHECK(out.sound.macros[7].targetKey == parameterRegistry()[756].stateKey);
    auto locked = d; locked.sound.controlLocks[756] = true; reject(locked, p, {RequestScope::sound, true}, ProposalStatus::lockedDependency);
    locked = d; locked.sound.synthLocks[1] = true; p.macros = {{8, "synth.2.wavetable.filter.cutoff", .25f}}; reject(locked, p, {RequestScope::sound, true}, ProposalStatus::lockedDependency);
    locked.sound.macros[7] = {parameterRegistry()[index("synth.2.wavetable.filter.cutoff")].stateKey, .25f}; p.macros = {{8, {}, 0}}; reject(locked, p, {RequestScope::sound, true}, ProposalStatus::lockedDependency);
    locked = d; locked.sound.controlLocks[8] = true; reject(locked, p, {RequestScope::sound, true}, ProposalStatus::lockedDependency);
    p.macros = {{1, "macro.2", .2f}}; reject(d, p, {RequestScope::sound, true}); p.macros = {{1, "output.protection", .2f}}; reject(d, p, {RequestScope::sound, true});
}
// Decision 6: the AI no longer adds, removes, moves or swaps bare banks; RACK LAYOUT does, and RackLayoutTests check its
// capacity, history and replacement. Each sequence this test admitted is now outOfReach, whatever its capacity, reuse or
// lock status would be. The AI's placements meet those rules in the twelve-rack and catalog tests below.
void rack_edits_capacity_history_dependency()
{
    begin("rack_edits_capacity_history_dependency"); ProjectDocument d;
    const auto bare = [](const ProjectDocument& source, std::vector<RackEdit> racks, ProposalPolicy policy = {}) {
        ProposalEdits p; p.racks = std::move(racks); reject(source, p, policy, ProposalStatus::outOfReach);
    };
    const std::vector<RackEdit> synths{{RackEditKind::addSynth, noSlot, 4, 1}, {RackEditKind::addSynth, 2, 4, 2}, {RackEditKind::addSynth, 3, 4, 3}, {RackEditKind::moveSynth, 3, 4, 1}};
    bare(d, synths); for (const auto& e : synths) layout(d, e);
    bare(d, {{RackEditKind::addSynth, noSlot, 4, 4}});
    std::vector<RackEdit> effects; for (int i = 0; i < 8; ++i) effects.push_back({RackEditKind::addEffect, std::uint8_t(i), 4, std::uint8_t(i), static_cast<EffectKind>(i % 4)});
    bare(d, effects); for (const auto& e : effects) layout(d, e);
    bare(d, {{RackEditKind::addEffect, noSlot, 4, 8}});
    bare(d, {{RackEditKind::moveEffect, 7, 0, 0}, {RackEditKind::replaceEffect, 7, 4, 0, EffectKind::delay}}, {RequestScope::sound, false, true});
    bare(d, {{RackEditKind::removeSynth, 0}});
    auto locked = rich(); locked.sound.synthLocks[0] = true;
    bare(locked, {{RackEditKind::removeSynth, 0}, {RackEditKind::addSynth, 0, 4, 0}}, {RequestScope::sound, false, true});
    locked = rich(); locked.sound.effectLocks[0] = true;
    bare(locked, {{RackEditKind::removeSynth, 0}}); bare(locked, {{RackEditKind::moveEffect, 0, 4, 0}});
    bare(locked, {{RackEditKind::replaceEffect, 0, 4, 0, EffectKind::delay}}, {RequestScope::sound, false, true});
    ProjectDocument empty; layout(empty, {RackEditKind::removeSynth, 0}); bare(empty, {{RackEditKind::addSynth, noSlot, 4, 0}});
}
void explicit_parameters_create_analog_wavetable_and_effects()
{
    begin("explicit_parameters_create_analog_wavetable_and_effects"); ProjectDocument d, out; ProposalEdits p;
    p.racks = {{RackEditKind::addSynth, 1, 4, 1}, {RackEditKind::addEffect, 0, 0, 0, EffectKind::drive}, {RackEditKind::addEffect, 1, 1, 0, EffectKind::delay}, {RackEditKind::addEffect, 2, 4, 0, EffectKind::reverb}, {RackEditKind::addEffect, 3, 4, 1, EffectKind::compressor}};
    p.parameters = {{"synth.1.enabled", 1}, {"synth.1.engine", 0}, {"synth.2.enabled", 1}, {"synth.2.engine", 1}, {"effect.2.delay.mix", .3f}, {"effect.4.compressor.mix", .8f}, {"synth.2.wavetable.osc.1.position", .4f}};
    reject(d, p, {}, ProposalStatus::outOfReach);
    // RACK LAYOUT and the PROGRAMMER made the banks; the AI sets their values.
    for (const auto& e : p.racks) layout(d, e);
    d.sound.values[index("synth.2.engine")] = 1; p.racks.clear();
    CHECK(admitProposal(d, {}, p, out) == ProposalStatus::accepted); CHECK(validateProjectDocument(out)); CHECK(out.sound.layout.synthCount == 2);
    CHECK(out.sound.values[index("synth.2.enabled")] == 1); CHECK(out.sound.values[index("synth.2.engine")] == 1);
    CHECK(out.sound.values[index("synth.2.analog.filter.cutoff")] == d.sound.values[index("synth.2.analog.filter.cutoff")]);
    auto w = wire(); w.getDynamicObject()->setProperty("rack_edits", array({rack("add_synth", 1, 4, 1), rack("add_effect", 7, 4, 0, "compressor")})); ProposalEdits decoded;
    CHECK(decodeProposal(json(w), decoded)); CHECK(decoded.racks.size() == 2); if (decoded.racks.size() == 2) CHECK(decoded.racks[1].effect == EffectKind::compressor && !decoded.racks[1].allowReuse);
}
void strict_json_boundary()
{
    begin("strict_json_boundary"); juce::var output = "retained";
    CHECK(parseStrictJson("{\"a\":1,\"b\":[true,null,\"é\"]}", output)); CHECK(int(output["a"]) == 1);
    for (const std::string s : {"{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}", "{\"x\":{\"k\":1,\"k\":2}}", "{}[]", "{", "{\"a\":01}", "{\"a\":1.}", "{\"a\":1e}", "[1,]", "{\"a\":true,}", "{\"a\":\"\\u0000\"}", "{\"a\":\"\\ud800\"}"}) { output = "retained"; CHECK(!parseStrictJson(s, output)); CHECK(output.toString() == "retained"); }
    const std::string depth24 = std::string(24, '[') + "0" + std::string(24, ']'); CHECK(parseStrictJson(depth24, output));
    CHECK(!parseStrictJson(std::string(25, '[') + "0" + std::string(25, ']'), output));
    CHECK(parseStrictJson("{}", output, 2)); CHECK(!parseStrictJson("{} ", output, 2));
    const std::string nul("{}\0", 3); CHECK(!parseStrictJson(nul, output));
    auto utf8Wire = wire(); utf8Wire.getDynamicObject()->setProperty("explanation", "UTF8_MARKER");
    const auto utf8Template = json(utf8Wire); const auto marker = utf8Template.find("UTF8_MARKER"); CHECK(marker != std::string::npos);
    if (marker != std::string::npos)
    {
        for (std::string bytes : {std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2), std::string("\x80", 1)})
        {
            auto text = utf8Template; text.replace(marker, 11, bytes); output = "retained";
            CHECK(!parseStrictJson(text, output)); CHECK(output.toString() == "retained"); badWire(text);
        }
        const std::string validBytes("\xc3\xa9\xf0\x9f\x8e\xb5", 6); auto text = utf8Template; text.replace(marker, 11, validBytes);
        CHECK(parseStrictJson(text, output)); ProposalEdits valid; CHECK(decodeProposal(text, valid)); CHECK(valid.explanation.toStdString() == validBytes);
    }
    auto v = wire(); ProposalEdits p; CHECK(decodeProposal(json(v), p));
    for (const auto& property : v.getDynamicObject()->getProperties()) { auto x = v.clone(); x.getDynamicObject()->removeProperty(property.name); badWire(x); }
    auto x = v.clone(); x.getDynamicObject()->setProperty("request_id", "authority"); badWire(x);
    for (auto key : {"locks", "mode", "playback_mode", "allowReuse", "edit_revision", "context_revision", "allow_macro_changes"}) { x = v.clone(); x.getDynamicObject()->setProperty(key, true); badWire(x); }
    for (juce::var n : {juce::var(true), juce::var("0.5"), juce::var(), array(), object({})}) { x = v.clone(); x.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", n)})); badWire(x); }
    x = v.clone(); x.getDynamicObject()->setProperty("parameter_edits", array({object({{"id", "output.gain_db"}, {"normalized", .5}, {"locked", false}})})); badWire(x);
    x = v.clone(); x.getDynamicObject()->setProperty("rack_edits", array({object({{"kind", "add_synth"}, {"slot", 1}, {"chain", 4}, {"position", 1}, {"effect", "drive"}, {"allowReuse", true}})})); badWire(x);
    auto text = json(v); text.insert(text.find("\"version\""), "\"\\u0076ersion\":1,"); badWire(text);
    x = v.clone(); x.getDynamicObject()->setProperty("pattern", pattern()); text = json(x); text.insert(text.find("\"pitch\""), "\"\\u0070itch\":60,"); badWire(text);
    badWire(json(v) + " false"); badWire(json(v).substr(0, json(v).size() - 1));
}
void strict_json_quoted_equivalence()
{
    begin("strict_json_quoted_equivalence"); std::string fingerprints; int acceptedCount=0,rejectedCount=0;
    const auto reject = [&](std::string_view bytes,std::size_t cap=8*1024*1024) {
        juce::var output="retained"; CHECK(!parseStrictJson(bytes,output,cap)); CHECK(output.isString()&&output.toString()=="retained"); ++rejectedCount;
    };
    const auto accept = [&](const std::string& bytes,const std::string& expected) {
        juce::var output="retained"; const bool parsed=parseStrictJson(bytes,output,bytes.size()); CHECK(parsed); ++acceptedCount;
        if(parsed){const auto actual=output["k"].toString().toStdString(); CHECK(output["k"].isString()&&actual==expected); fingerprints+=juce::SHA256(actual.data(),actual.size()).toHexString().toStdString();}
    };
    const std::pair<std::string,std::string> strings[]{
        {R"("")",""},{R"("plain")","plain"},{R"("\"\\\/\b\f\n\r\t")","\"\\/\b\f\n\r\t"},
        {R"("\u0041\u00e9")","A\xc3\xa9"},{R"("\ud83c\udfb5")","\xf0\x9f\x8e\xb5"},
        {std::string("\"\xc3\xa9\xf0\x9f\x8e\xb5\""),"\xc3\xa9\xf0\x9f\x8e\xb5"}};
    for(const auto& [token,decoded]:strings){
        const auto bytes="{\"k\":"+token+"}";accept(bytes,decoded);
        for(std::size_t length=0;length<bytes.size();++length)reject(std::string_view(bytes.data(),length));
    }
    for(const std::string token:{"\"","\"abc","\"\\","\"\\u","\"\\u0","\"\\u00","\"\\u000","\"\\x\"","\"\\'\""})reject("{\"k\":"+token+"}");
    for(int digit=0;digit<4;++digit){auto token=std::string(R"("\u0000")");token[std::size_t(3+digit)]='g';reject("{\"k\":"+token+"}");}
    for(int byte=0;byte<32;++byte)reject(std::string("{\"k\":\"")+char(byte)+"\"}");
    for(const std::string bytes:{std::string("\xc0\xaf",2),std::string("\xed\xa0\x80",3),std::string("\xf4\x90\x80\x80",4),std::string("\xe2\x82",2),std::string("\x80",1)})reject("{\"k\":\""+bytes+"\"}");
    for(const auto* bytes:{R"({"k":1,"\u006b":2})",R"({"\u006b":1,"k":2})",R"({"outer":{"k":1,"\u006b":2}})",R"({"k":"\u0000"})",R"({"k":"\ud800"})"})reject(bytes);
    std::vector<std::vector<std::string>> paths;std::vector<std::string> tokens;juce::var observed;
    const auto* observerBytes=R"({"a\u002fb":[{"n":1.0}],"quoted\"key":-0.0})";
    CHECK(parseStrictJson(observerBytes,observed,4096,[&](std::span<const juce::String> path,std::string_view token){
        std::vector<std::string> decoded;for(const auto& step:path)decoded.push_back(step.toStdString());paths.push_back(std::move(decoded));tokens.emplace_back(token);
    }));
    CHECK((paths==std::vector<std::vector<std::string>>{{"a/b","","n"},{"quoted\"key"}}));CHECK((tokens==std::vector<std::string>{"1.0","-0.0"}));
    const auto* items=observed["a/b"].getArray();CHECK(items&&items->size()==1);if(items&&items->size()==1)CHECK((*items)[0]["n"].isDouble()&&double((*items)[0]["n"])==1.0);
    CHECK(observed["quoted\"key"].isDouble()&&std::signbit(double(observed["quoted\"key"])));
    std::string escaped,decoded;for(int i=0;i<32767;++i){escaped+=R"(\u0041\n)";decoded+="A\n";}
    for(const auto& [payload,expected]:{std::pair{std::string(6*1024*1024,'A'),std::string(6*1024*1024,'A')},std::pair{escaped,decoded}}){
        const auto bytes="{\"k\":\""+payload+"\"}";accept(bytes,expected);reject(bytes,bytes.size()-1);
        reject(std::string_view(bytes.data(),bytes.size()-2));reject(bytes.substr(0,bytes.size()-2)+"\\");
    }
    std::cout<<"QUOTED_EQUIVALENCE accepted="<<acceptedCount<<" rejected="<<rejectedCount<<" digest="<<juce::SHA256(fingerprints.data(),fingerprints.size()).toHexString()<<'\n';
}
void proposal_caps_and_note_boundaries()
{
    begin("proposal_caps_and_note_boundaries"); auto v = wire(); ProposalEdits p; juce::Array<juce::var> edits;
    ProjectDocument d, out;
    for (std::size_t i = 0; i < int(parameterCount); ++i) edits.add(param(parameterRegistry()[i].id.toRawUTF8(), double(d.sound.values[i])));
    v.getDynamicObject()->setProperty("parameter_edits", edits);
    const auto complete = json(v); std::cout << "COMPLETE_PARAMETER_PROPOSAL bytes=" << complete.size() << " cap=262144 controls=" << parameterCount << '\n';
    CHECK(complete.size() > 262144); badWire(complete);
    edits.clear(); for (std::size_t i = 0; i < 3503; ++i) edits.add(param(parameterRegistry()[i].id.toRawUTF8(), double(d.sound.values[i])));
    v.getDynamicObject()->setProperty("parameter_edits", edits); CHECK(decodeProposal(json(v), p)); CHECK(p.parameters.size() == 3503); CHECK(admitProposal(d, {}, p, out) == ProposalStatus::accepted);
    auto appended = wire(); appended.getDynamicObject()->setProperty("parameter_edits", array({param(parameterRegistry().back().id.toRawUTF8(), double(d.sound.values.back()))}));
    ProposalEdits last; CHECK(decodeProposal(json(appended), last)); CHECK(last.parameters.size() == 1 && last.parameters.front().id == parameterRegistry().back().id); CHECK(admitProposal(d, {}, last, out) == ProposalStatus::accepted);
    edits.add(edits[0]); v.getDynamicObject()->setProperty("parameter_edits", edits); badWire(v);
    p.parameters.push_back({"output.gain_db", .5f}); p.parameters.push_back({"output.gain_db", .5f}); reject(d, p);
    v = wire(); v.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", .5), param("output.gain_db", .5)})); badWire(v);
    edits.clear(); for (int i = 1; i <= 8; ++i) edits.add(macro(i, {}, 0.)); v = wire(); v.getDynamicObject()->setProperty("macro_edits", edits); CHECK(decodeProposal(json(v), p)); CHECK(admitProposal(d, {RequestScope::sound, true}, p, out) == ProposalStatus::accepted);
    edits.add(macro(1, {}, 0.)); v.getDynamicObject()->setProperty("macro_edits", edits); badWire(v); p.macros.push_back({1, {}, 0}); p.macros.push_back({1, {}, 0}); reject(d, p, {RequestScope::sound, true});
    v = wire(); v.getDynamicObject()->setProperty("macro_edits", array({macro(1, {}, 0.), macro(1, {}, 0.)})); badWire(v);
    // Copper Arc owns synth 1, so its moves are in reach.
    ProjectDocument copper; copper.sound = copperArcSound();
    edits.clear(); for (int i = 0; i < 64; ++i) edits.add(rack("move_synth", 0)); v = wire(); v.getDynamicObject()->setProperty("rack_edits", edits); CHECK(decodeProposal(json(v), p)); CHECK(admitProposal(copper, {}, p, out) == ProposalStatus::accepted); edits.add(rack("move_synth", 0)); v.getDynamicObject()->setProperty("rack_edits", edits); badWire(v); p.racks.resize(65); reject(d, p);
    for (double n : {-1., 1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) { v = wire(); v.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", n)})); badWire(v); p = {}; p.parameters = {{"output.gain_db", float(n)}}; reject(d, p); }
    auto text = json(wire()); auto at = text.find("\"version\": 1"); CHECK(at != std::string::npos); if (at != std::string::npos) { auto huge = text; huge.replace(at, 12, "\"version\": 1e9999"); badWire(huge); }
    for (juce::var n : {juce::var(1.5), juce::var(true), juce::var("1"), juce::var(2)}) { v = wire(); v.getDynamicObject()->setProperty("version", n); badWire(v); }
    for (auto r : {rack("add_synth", 255, 4, 0), rack("remove_synth", 0), rack("move_synth", 0), rack("add_effect", 255), rack("remove_effect", 0), rack("move_effect", 0), rack("replace_effect", 0, 4, 0, "delay")}) { v = wire(); v.getDynamicObject()->setProperty("rack_edits", array({r})); CHECK(decodeProposal(json(v), p)); }
    for (auto r : {rack("add_synth", 4), rack("remove_synth", 255), rack("move_synth", 0, 0), rack("remove_effect", 0, 0), rack("add_effect", 8), rack("replace_effect", 0, 4, 1), rack("add_effect", 0, 5), rack("unknown", 0)}) { v = wire(); v.getDynamicObject()->setProperty("rack_edits", array({r})); badWire(v); }
    v = wire(); auto r = rack("add_effect", 1); r.getDynamicObject()->setProperty("slot", 1.5); v.getDynamicObject()->setProperty("rack_edits", array({r})); badWire(v);
    for (auto m : {macro(0, {}, 0.), macro(9, {}, 0.), macro(1, {}, .1), macro(1, "unknown", 0.), macro(1, "output.gain_db", 1.01)}) { v = wire(); v.getDynamicObject()->setProperty("macro_edits", array({m})); badWire(v); }
    for (int count : {0, 256, 257}) { auto pat = pattern(); juce::Array<juce::var> notes; for (int i = 0; i < count; ++i) notes.add(note(.013, .137, i % 128, 1 + i % 127, 1 + i % 16)); pat.getDynamicObject()->setProperty("notes", notes); v = wire(); v.getDynamicObject()->setProperty("pattern", pat); if (count <= 256) { CHECK(decodeProposal(json(v), p)); CHECK(p.pattern && p.pattern->noteCount == count); CHECK(admitProposal(d, {RequestScope::pattern}, p, out) == ProposalStatus::accepted); } else badWire(v); }
    for (auto n : {note(-.01), note(4.), note(0., 0.), note(0., 4.01), note(0., .25, 128), note(0., .25, 60, 0), note(0., .25, 60, 128), note(0., .25, 60, 100, 0), note(0., .25, 60, 100, 17), note(0., .25, 60.5)}) { auto pat = pattern(); pat.getDynamicObject()->setProperty("notes", array({n})); v = wire(); v.getDynamicObject()->setProperty("pattern", pat); badWire(v); }
    for (const auto key : {"bars", "meter_numerator", "meter_denominator", "grid", "swing"}) { auto pat = pattern(); pat.getDynamicObject()->setProperty(key, juce::var("invalid")); v = wire(); v.getDynamicObject()->setProperty("pattern", pat); badWire(v); }
    for (int denominator : {1, 2, 4, 8, 16, 32}) for (const auto grid : {"quarter", "eighth", "sixteenth", "thirty_second", "sixty_fourth"}) { auto pat = pattern(); pat.getDynamicObject()->setProperty("bars", 8); pat.getDynamicObject()->setProperty("meter_numerator", 32); pat.getDynamicObject()->setProperty("meter_denominator", denominator); pat.getDynamicObject()->setProperty("grid", grid); pat.getDynamicObject()->setProperty("swing", 1.); v = wire(); v.getDynamicObject()->setProperty("pattern", pat); CHECK(decodeProposal(json(v), p)); }
    v = wire(); v.getDynamicObject()->setProperty("explanation", juce::String::repeatedString(juce::String::fromUTF8("é"), 1024)); CHECK(decodeProposal(json(v), p)); v.getDynamicObject()->setProperty("explanation", juce::String::repeatedString(juce::String::fromUTF8("é"), 1025)); badWire(v);
    text = json(wire()); CHECK(decodeProposal(text + std::string(262144 - text.size(), ' '), p)); badWire(text + std::string(262145 - text.size(), ' '));
    p = {}; p.parameters = {{"unknown", 0}}; reject(d, p); p = {}; p.macros = {{255, {}, 0}}; reject(d, p, {RequestScope::sound, true}); p = {}; p.macros = {{1, {}, .1f}}; reject(d, p, {RequestScope::sound, true});
    p = {}; p.racks = {{static_cast<RackEditKind>(255), 0}}; reject(d, p); p.racks = {{RackEditKind::removeSynth, 0, 0}}; reject(d, p); p.racks = {{RackEditKind::addEffect, 0, 4, 0, static_cast<EffectKind>(255)}}; reject(d, p);
    p = {}; p.pattern = PatternDocument{}; p.pattern->noteCount = 257; reject(d, p, {RequestScope::pattern}); p.pattern->noteCount = 0; p.pattern->notes[255].pitch = 0; reject(d, p, {RequestScope::pattern});
    p = {}; p.explanation = juce::String::repeatedString("a", 2049); reject(d, p);
}
void rejection_preserves_output_and_alias()
{
    begin("rejection_preserves_output_and_alias"); auto d = rich(); auto broken = d; broken.sound.values[0] = 2;
    reject(broken, {}); juce::var sentinel = "retained"; CHECK(!catalogFor(broken, sentinel)); CHECK(sentinel.toString() == "retained");
    broken = d; broken.pattern.noteCount = 257; reject(broken, {}); broken = d; broken.mode = static_cast<PlaybackMode>(255); reject(broken, {});
    ProposalEdits p; p.racks = {{RackEditKind::addSynth, 3, 4, 2}, {RackEditKind::removeEffect, 5}}; reject(d, p);
    p = {}; p.parameters = {{"output.gain_db", .8f}, {"synth.1.analog.filter.cutoff", .7f}}; reject(d, p, {}, ProposalStatus::lockedDependency);
    p = {}; p.parameters = {{"synth.1.modulation.1.enabled", 1}, {"synth.1.modulation.1.source", 0}}; reject(d, p);
    auto alias = d; CHECK(admitProposal(alias, {}, {}, alias) == ProposalStatus::accepted); CHECK(exact(alias, d));
}
void numeric_wrap_representability()
{
    begin("numeric_wrap_representability");
    for (const auto* token : {"18446744073709551617", "-18446744073709551617", "9223372036854775808",
                              "-9223372036854775808", "999999999999999999999.5", "1e9999", "1e-9999"}) {
        juce::var output = "retained";
        CHECK(!parseStrictJson(std::string("{\"n\":") + token + "}", output)); CHECK(output.toString() == "retained");
    }
    for (const auto* token : {"9223372036854775807", "-9223372036854775807", "0", "1", "0.5", "1e10", "1e-300"}) {
        juce::var output; CHECK(parseStrictJson(std::string("{\"n\":") + token + "}", output));
        CHECK(output["n"].isInt() || output["n"].isInt64() || output["n"].isDouble());
    }
    auto text = json(wire()); const auto at = text.find("\"version\": 1"); CHECK(at != std::string::npos);
    if (at != std::string::npos) { text.replace(at, 12, "\"version\": 18446744073709551617"); badWire(text); }
    auto v = wire(); v.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", 1e-100)})); badWire(v);
    // JUCE var equality treats tiny doubles as equal; construct a fresh fixture.
    v = wire(); v.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", double(std::numeric_limits<float>::denorm_min()))}));
    ProposalEdits p; CHECK(decodeProposal(json(v), p)); CHECK(p.parameters.size() == 1);
    if (!p.parameters.empty()) CHECK(p.parameters[0].normalized == std::numeric_limits<float>::denorm_min());
}
void integer_tokens_preserve_exact_fields()
{
    begin("integer_tokens_preserve_exact_fields");
    auto v = wire();
    v.getDynamicObject()->setProperty("rack_edits", array({rack("add_synth", 1, 4, 1)}));
    v.getDynamicObject()->setProperty("macro_edits", array({macro(1, {}, 0.)}));
    v.getDynamicObject()->setProperty("pattern", pattern());
    const auto plain = json(v); ProposalEdits p;
    CHECK(decodeProposal(plain, p));
    // The fixture's add_synth makes a bare bank, which the AI may not.
    ProjectDocument d, output; CHECK(admitProposal(d, {RequestScope::both, true}, p, output) == ProposalStatus::outOfReach);
    for (const auto& [field, token] : std::initializer_list<std::pair<const char*, const char*>>{
        {"version", "1"}, {"slot", "1"}, {"chain", "4"}, {"position", "1"}, {"index", "1"},
        {"bars", "1"}, {"meter_numerator", "4"}, {"meter_denominator", "4"},
        {"pitch", "60"}, {"velocity", "100"}, {"channel", "1"}}) {
        const auto prefix = std::string("\"") + field + "\": " + token;
        const auto at = plain.find(prefix); CHECK(at != std::string::npos);
        if (at == std::string::npos) continue;
        for (const auto* suffix : {".0", "e0", ".00000000000000000001"}) {
            auto text = plain; text.insert(at + prefix.size(), suffix); badWire(text);
        }
    }
    // General number fields retain decimal/exponent support and sub-grid doubles.
    auto n = wire(); n.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", .5)}));
    CHECK(decodeProposal(json(n), p)); auto text = json(n); const auto at = text.find("0.5"); CHECK(at != std::string::npos);
    if (at != std::string::npos) { text.replace(at, 3, "5e-1"); CHECK(decodeProposal(text, p)); CHECK(p.parameters.size() == 1 && p.parameters[0].normalized == .5f); }
}
void strict_numeric_precision_and_atomicity()
{
    begin("strict_numeric_precision_and_atomicity");
    juce::var parsed;
    const std::string numeric = "{\"text\":\"0.0010000000474974513054\",\"\\u0061\":[1,1.0,1e0,-0,-0.0,2147483647,2147483648,-2147483648,9223372036854775807,-9223372036854775807],\"nested\":{\"delay\":0.0010000000474974513054,\"pulse\":0.0500000007450580596923828125,\"attack\":0.100000001490116119384765625,\"tiny\":0.0000000000000000000012345678901234567}}";
    CHECK(parseStrictJson(numeric, parsed));
    CHECK(parsed["text"].isString()); CHECK(parsed["text"].toString() == "0.0010000000474974513054");
    const auto& a = parsed["a"];
    CHECK(a[0].isInt()); CHECK(a[1].isDouble()); CHECK(a[2].isDouble()); CHECK(a[3].isInt());
    CHECK(a[4].isDouble()); CHECK(std::signbit(double(a[4]))); CHECK(!std::signbit(double(a[3])));
    CHECK(a[5].isInt()); CHECK(a[6].isInt64()); CHECK(a[7].isInt64());
    CHECK(a[8].isInt64()); CHECK(static_cast<juce::int64>(a[8]) == INT64_MAX);
    CHECK(a[9].isInt64()); CHECK(static_cast<juce::int64>(a[9]) == -INT64_MAX);
    CHECK(double(parsed["nested"]["delay"]) == double(.001f));
    CHECK(double(parsed["nested"]["pulse"]) == double(.05f));
    CHECK(double(parsed["nested"]["attack"]) == double(.1f));
    CHECK(double(parsed["nested"]["tiny"]) == 1.2345678901234567e-21);
    for (const auto* token : {"-0.0", "-0e0", "-0.00000000000000000000000000000000000000000000"}) {
        CHECK(parseStrictJson(std::string("{\"n\":") + token + "}", parsed)); CHECK(parsed["n"].isDouble()); CHECK(std::signbit(double(parsed["n"])));
    }
    const std::string oldLocale = std::setlocale(LC_NUMERIC, nullptr);
    CHECK(std::setlocale(LC_NUMERIC, "German_Germany.1252") != nullptr);
    CHECK(parseStrictJson(numeric, parsed)); CHECK(double(parsed["nested"]["delay"]) == double(.001f));
    CHECK(parseStrictJson("{\"n\":1.25}", parsed)); CHECK(double(parsed["n"]) == 1.25);
    CHECK(std::setlocale(LC_NUMERIC, oldLocale.c_str()) != nullptr);
    for (const auto* token : {"+1", "01", "1.", ".1", "1e", "1e+", "NaN", "Infinity", "1e309", "1e-9999",
                             "9223372036854775808.0", "-9223372036854775808", "18446744073709551616e-20"}) {
        juce::var output = "retained";
        CHECK(!parseStrictJson(std::string("{\"first\":0.0010000000474974513054,\"bad\":") + token + "}", output));
        CHECK(output.isString() && output.toString() == "retained");
    }
    for (const auto& text : {std::string("{\"a\":0.0010000000474974513054,\"\\u0061\":2}"),
                            std::string("{\"a\":0.0010000000474974513054} trailing"),
                            std::string(25, '[') + "0.0010000000474974513054" + std::string(25, ']'),
                            std::string("{\"a\":0.0010000000474974513054,\"b\":\"\xc0\xaf\"}")}) {
        juce::var output = "retained"; CHECK(!parseStrictJson(text, output)); CHECK(output.isString() && output.toString() == "retained");
    }
    juce::var output = "retained"; CHECK(!parseStrictJson(numeric, output, numeric.size() - 1)); CHECK(output.toString() == "retained");
    CHECK(parseStrictJson(numeric, output, numeric.size()));
}
// Task4 staged tests: public APIs are approved; JSON reconciliation framing is
// a provisional test contract recorded in task-4-staged-preparation-report.md.
RackDesign nativeRecipe(bool inserts = false)
{
    RackDesign d; d.name = "Original brass"; d.intent = "Owned native analog with focused tone";
    d.units = {{3, NativeModule::analog, {{3, "engine", 0}, {3, "analog.filter.cutoff", .35f}}}};
    d.panel.sections = {"Tone"};
    d.panel.controls = {{"Tone", 3, 0, "analog.filter.cutoff", FocusWidget::rotary, FocusResponse::logarithmic, 20, 20000}};
    if (inserts) d.units.push_back({7, NativeModule::drive, {{7, "drive.gain_db", .25f}}});
    CHECK(validateRackDesign(d, RackValidationMode::portableDesign)); return d;
}
juce::var wire2()
{
    auto v = wire(); auto* o = v.getDynamicObject(); o->setProperty("version", 2);
    o->setProperty("reconciliation", object({{"detach", array()}, {"remove", array()}, {"replace", array()}, {"controls", array()}}));
    o->setProperty("name", "Warm Test Pad");
    return v;
}
void version2_has_no_rack_design()
{
    begin("version2_has_no_rack_design");
    auto without = wire2(); without.getDynamicObject()->removeProperty("rack_design");
    ProposalEdits edits; CHECK(decodeProposal(json(without), ProposalPolicy{}, edits));
    auto with = without.clone(); with.getDynamicObject()->setProperty("rack_design", juce::var());
    ProposalDecodeReason reason{}; CHECK(!decodeProposal(json(with), ProposalPolicy{}, edits, &reason) && reason == ProposalDecodeReason::rootFields);
    CHECK(!proposalSchema()["properties"].hasProperty("rack_design"));
}
void version2_requires_a_valid_name()
{
    begin("version2_requires_a_valid_name");
    const auto named = [](juce::var name) { auto v = wire2(); v.getDynamicObject()->setProperty("name", name); return v; };
    ProposalEdits edits; ProposalDecodeReason reason{};
    auto without = wire2(); without.getDynamicObject()->removeProperty("name");
    CHECK(!decodeProposal(json(without), ProposalPolicy{}, edits, &reason)); CHECK(reason == ProposalDecodeReason::rootFields);
    reason = {}; CHECK(!decodeProposal(json(named(7)), ProposalPolicy{}, edits, &reason)); CHECK(reason == ProposalDecodeReason::rootFields);
    const auto curly = juce::String(juce::CharPointer_UTF8("Rock\xe2\x80\x99n Pad"));
    for (const auto& good : {juce::String("Ethereal Rolling Bass"), juce::String("Lo-Fi Keys 2"), juce::String("Rock'n Pad"), curly,
                             juce::String::repeatedString("a", 40)}) {
        edits = {}; CHECK(decodeProposal(json(named(good)), ProposalPolicy{}, edits)); CHECK(edits.name == good);
    }
    edits = {}; CHECK(decodeProposal(json(named("  Glass Bell Lead  ")), ProposalPolicy{}, edits)); CHECK(edits.name == "Glass Bell Lead");
    for (const auto& bad : {juce::String("   "), juce::String::repeatedString("a", 41), juce::String("123 456"), juce::String("Bass/Lead"),
                            juce::String(juce::CharPointer_UTF8("Bass \xf0\x9f\x8e\xb8"))}) {
        reason = {}; edits = {}; CHECK(!decodeProposal(json(named(bad)), ProposalPolicy{}, edits, &reason));
        CHECK(reason == ProposalDecodeReason::invalidName); CHECK(edits.name.isEmpty());
    }
    CHECK(juce::String(proposalDecodeReasonName(ProposalDecodeReason::invalidName)) == "proposal_name_invalid");
    // A version 1 answer, which the schema no longer offers, has no name and keeps the sound's.
    edits = {}; CHECK(decodeProposal(json(wire()), ProposalPolicy{}, edits)); CHECK(edits.name.isEmpty());
    ProjectDocument source, out; source.sound.name = "Kept Name";
    CHECK(admitProposal(source, ProposalPolicy{}, edits, out) == ProposalStatus::accepted); CHECK(out.sound.name == "Kept Name");
    edits = {}; CHECK(decodeProposal(json(named("Ethereal Rolling Bass")), ProposalPolicy{}, edits));
    CHECK(admitProposal(source, ProposalPolicy{}, edits, out) == ProposalStatus::accepted); CHECK(out.sound.name == "Ethereal Rolling Bass");
}
ProjectDocument personalityProject()
{
    ProjectDocument d; RackDifference difference; SoundDocument next;
    RackOperation adopt; adopt.kind = RackOperationKind::adopt; adopt.newInstance = juce::Uuid();
    adopt.design = nativeRecipe(); adopt.allocation = {{3, 0}};
    const auto status = makeRackCandidate(d.sound, adopt, SoundEditOrigin::manual, next, difference);
    CHECK(status == RackCandidateStatus::ready); if (status != RackCandidateStatus::ready) return d;
    d.sound = next;
    RackOperation peer; peer.kind = RackOperationKind::place; peer.newInstance = juce::Uuid();
    peer.design = nativeRecipe(); peer.design->name = "Independent peer";
    peer.destination = {RackRole::synth, masterChain, 1}; peer.allocation = {{3, 1}};
    CHECK(makeRackCandidate(d.sound, peer, SoundEditOrigin::manual, next, difference) == RackCandidateStatus::ready);
    d.sound = next; CHECK(validateProjectDocument(d)); return d;
}
bool decode2(const juce::var& v, const ProposalPolicy& p, ProposalEdits& e)
{
    const auto success = decodeProposal(json(v), p, e); CHECK(success); return success;
}
void rejectWire2(const juce::var& v, const ProposalPolicy& p)
{
    ProposalEdits out; out.parameters = {{"output.gain_db", .3f}};
    out.reconciliation.detach = {juce::Uuid()}; const auto before = out;
    CHECK(!decodeProposal(json(v), p, out)); CHECK(same(out, before));
}
void create_sound_preserves_personality()
{
    begin(__func__); // Generic v1 must retain metadata and cannot silently detach deleted ownership.
    auto source = personalityProject(); if (!source.sound.personalities) return;
    const auto before = source; ProposalEdits edits; auto legacy = wire();
    legacy.getDynamicObject()->setProperty("parameter_edits", array({param("synth.1.analog.filter.cutoff", .8)}));
    CHECK(decodeProposal(json(legacy), edits)); ProjectDocument out;
    CHECK(admitProposal(source, {}, edits, out) == ProposalStatus::accepted);
    CHECK(out.sound.personalities && *out.sound.personalities == *source.sound.personalities);
    auto alias = source; CHECK(admitProposal(alias, {}, edits, alias) == ProposalStatus::accepted); CHECK(exact(alias, out)); CHECK(exact(source, before));
    legacy = wire(); legacy.getDynamicObject()->setProperty("rack_edits", array({rack("remove_synth", 0)}));
    CHECK(decodeProposal(json(legacy), edits)); reject(source, edits);
    for (auto scope : {RequestScope::sound, RequestScope::pattern, RequestScope::both}) {
        ProposalPolicy p; p.scope = scope; edits = {}; CHECK(decodeProposal(json(wire()), p, edits));
        CHECK(admitProposal(source, p, edits, out) == ProposalStatus::accepted); CHECK(exact(source, out));
    }
    // A bare bank beside the personalities is outside the AI's reach.
    edits = {}; edits.racks = {{RackEditKind::addSynth, 3, masterChain, 2}};
    reject(source, edits, {}, ProposalStatus::outOfReach);
}
void create_sound_rejects_uncaptured_owned_structure()
{
    begin(__func__); // Automatic manual reconciliation must not confer model authority.
    auto source = personalityProject(); RackOperation place; place.newInstance = juce::Uuid();
    place.design = nativeRecipe(true); place.design->units.push_back({8, NativeModule::reverb, {{8, "reverb.mix", .2f}}});
    place.allocation = {{3, 2}, {7, 5}, {8, 6}}; place.destination = {RackRole::synth, masterChain, 2};
    SoundDocument placed; RackDifference difference;
    CHECK(makeRackCandidate(source.sound, place, SoundEditOrigin::manual, placed, difference) == RackCandidateStatus::ready);
    source.sound = placed; CHECK(validateProjectDocument(source)); const auto before = source;
    if (!source.sound.personalities || source.sound.personalities->instances.size() != 3) return;
    const auto owner = source.sound.personalities->instances[2].id;
    for (int version : {1, 2}) {
        ProposalPolicy policy; policy.allowSlotReuse = true;
        for (int change = 0; change < 4; ++change) {
            auto v = version == 1 ? wire() : wire2();
            if (change == 0) v.getDynamicObject()->setProperty("parameter_edits", array({param("synth.3.engine", 1)}));
            if (change == 1) v.getDynamicObject()->setProperty("rack_edits", array({rack("replace_effect", 5, masterChain, 0, "delay")}));
            if (change == 2) v.getDynamicObject()->setProperty("rack_edits", array({rack("move_effect", 6, 2, 0, "reverb")}));
            if (change == 3) v.getDynamicObject()->setProperty("rack_edits", array({rack("move_synth", 2, masterChain, 0)}));
            ProposalEdits edits; CHECK(decodeProposal(json(v), policy, edits));
            // The rack's module fixes its engine and its inserts' kinds, so those edits are out of reach whatever the
            // capture. Reordering the rack's own inserts changes its snapshot.
            if (change == 3) {
                // A move forks the moved rack and each rack it shifts, as the user's move does.
                RackOperation move; move.kind = RackOperationKind::move; move.selected = owner; move.destination = {RackRole::synth, masterChain, 0};
                SoundDocument expected; CHECK(makeRackCandidate(source.sound, move, SoundEditOrigin::manual, expected, difference) == RackCandidateStatus::ready);
                expected.name = edits.name;
                ProjectDocument moved; CHECK(admitProposal(source, policy, edits, moved) == ProposalStatus::accepted); CHECK(moved.sound == expected);
            } else reject(source, edits, policy, change == 2 ? ProposalStatus::scopeViolation : ProposalStatus::outOfReach);
            CHECK(exact(source, before));
            if (change == 1) { auto noReuse = policy; noReuse.allowSlotReuse = false;
                reject(source, edits, noReuse, ProposalStatus::outOfReach); }
            // Replacing a rack is the user's.
            edits.reconciliation.replace = {owner}; reject(source, edits, policy, ProposalStatus::scopeViolation);
        }
        // The owner's cutoff is in reach; a bare compressor beside it is not.
        auto v = version == 1 ? wire() : wire2();
        v.getDynamicObject()->setProperty("parameter_edits", array({param("synth.3.analog.filter.cutoff", .8)}));
        v.getDynamicObject()->setProperty("rack_edits", array({rack("add_effect", 0, masterChain, 0, "compressor")}));
        ProposalEdits edits; CHECK(decodeProposal(json(v), policy, edits)); ProjectDocument out;
        reject(source, edits, policy, ProposalStatus::outOfReach); edits.racks.clear();
        CHECK(admitProposal(source, policy, edits, out) == ProposalStatus::accepted);
        CHECK(out.sound.personalities && *out.sound.personalities == *source.sound.personalities); CHECK(exact(source, before));
    }
    // Resolving a mapping is the user's too.
    ProposalEdits edits; edits.reconciliation.controls = {{owner, 0, {}}}; ProjectDocument out;
    reject(source, edits, {}, ProposalStatus::scopeViolation);
    for (bool remove : {false, true}) {
        ProposalPolicy captured; auto v = wire2();
        v["reconciliation"].getDynamicObject()->setProperty(remove ? "remove" : "detach", array({owner.toDashedString()}));
        ProposalEdits requested; CHECK(decodeProposal(json(v), captured, requested));
        // CREATE SOUND removes a placed rack, as the user can; detaching is the user's.
        if (!remove) { reject(source, requested, captured, ProposalStatus::scopeViolation); continue; }
        CHECK(admitProposal(source, captured, requested, out) == ProposalStatus::accepted);
        CHECK(out.sound.personalities && out.sound.personalities->instances.size() == 2);
        if (out.sound.personalities && out.sound.personalities->instances.size() == 2) {
            CHECK(out.sound.personalities->instances[0] == source.sound.personalities->instances[0]);
            CHECK(out.sound.personalities->instances[1] == source.sound.personalities->instances[1]);
        }
        CHECK(out.sound.layout.synthUse[2] == SlotUse::retired);
    }
    // Producer/manual behavior remains automatic; only AI admission is constrained.
    auto proposed = source.sound; proposed.values[index("synth.3.engine")] = 1;
    CHECK(reconcileRackEdit(source.sound, proposed, {}, SoundEditOrigin::manual, placed, difference) == RackCandidateStatus::ready);
    CHECK(placed.personalities && placed.personalities->instances[2].design.units[0].module == NativeModule::wavetable);
    CHECK(exact(source, before));
}
void create_sound_moves_but_never_maps()
{
    begin(__func__); // CREATE SOUND moves a placed rack as the user does, but leaves its mapping to the user.
    for (int movement = 0; movement < 3; ++movement) {
        const bool synth = movement == 0; ProjectDocument source; RackOperation own;
        own.kind = synth ? RackOperationKind::adopt : RackOperationKind::place; own.newInstance = juce::Uuid();
        own.design = nativeRecipe(); own.allocation = {{3, 0}};
        if (!synth) {
            own.design->role = RackRole::fx; own.design->units = {{3, NativeModule::drive, {{3, "drive.gain_db", .25f}}}};
            own.design->panel.controls = {{"Gain", 3, 0, "drive.gain_db", FocusWidget::rotary, FocusResponse::linear, 0, 24}};
            own.destination = {RackRole::fx, masterChain, 0};
        }
        SoundDocument sound; RackDifference difference;
        const auto status = makeRackCandidate(source.sound, own, SoundEditOrigin::manual, sound, difference);
        CHECK(status == RackCandidateStatus::ready); if (status != RackCandidateStatus::ready) continue;
        source.sound = sound;
        if (synth) layout(source, {RackEditKind::addSynth, 1, masterChain, 1});
        else layout(source, {RackEditKind::addEffect, 1, masterChain, 1, EffectKind::delay});
        CHECK(validateProjectDocument(source)); CHECK(source.sound.personalities && source.sound.personalities->instances.size() == 1);
        if (!source.sound.personalities || source.sound.personalities->instances.size() != 1) continue;
        const auto before = source; const auto original = source.sound.personalities->instances[0];
        CHECK(original.bindings == std::vector<UnitBinding>({{3, 0}})); CHECK(original.design.panel.controls.size() == 1);
        CHECK(synth ? source.sound.layout.synthOrder[0] == 0 && source.sound.layout.synthOrder[1] == 1
                    : source.sound.layout.effectOrder[masterChain][0] == 0 && source.sound.layout.effectOrder[masterChain][1] == 1);
        ProposalPolicy policy;
        auto mapping = wire2(); mapping["reconciliation"].getDynamicObject()->setProperty("controls", array({object({
            {"instance", original.id.toDashedString()}, {"control", 0}, {"replacement", {}}})}));
        ProposalEdits edits; CHECK(decodeProposal(json(mapping), policy, edits));
        reject(source, edits, policy, ProposalStatus::scopeViolation);
        auto move = wire2();
        move.getDynamicObject()->setProperty("rack_edits", synth ? array({rack("move_synth", 0, masterChain, 1)})
            : array({rack("move_effect", 0, movement == 1 ? masterChain : 0, movement == 1 ? 1 : 0)}));
        CHECK(decodeProposal(json(move), policy, edits));
        ProjectDocument out; CHECK(admitProposal(source, policy, edits, out) == ProposalStatus::accepted); CHECK(validateProjectDocument(out));
        if (out.sound.personalities) { const auto& moved = out.sound.personalities->instances[0];
            CHECK(moved.design.units == original.design.units && moved.design.panel.controls == original.design.panel.controls && moved.bindings == original.bindings); }
        CHECK(synth ? out.sound.layout.synthOrder[1] == 0
                    : out.sound.layout.effects[0].chain == (movement == 1 ? masterChain : 0));
        if (!synth && movement == 1) CHECK(out.sound.layout.effectOrder[masterChain][1] == 0);
        CHECK(exact(source, before));
    }
}
void v2_reconciliation_bounds()
{
    begin(__func__); // Missing byte/depth/count/closed-object checks must never partially replace outputs.
    ProposalPolicy policy; ProposalEdits decoded;
    const auto valid = json(wire2()); CHECK(decodeProposal(valid, policy, decoded));
    for (auto bad : {valid.substr(0, valid.size() - 1), valid + "x", std::string("\xc0\xaf") + valid,
                    std::string("\xf0\x9f") + valid, valid + std::string(262145, ' ')}) {
        auto out = decoded; const auto before = out; CHECK(!decodeProposal(bad, policy, out)); CHECK(same(out, before));
    }
    // CREATE SOUND carries a literal mapping request but never resolves it; that is the user's.
    auto source = personalityProject(); if (!source.sound.personalities || source.sound.personalities->instances.size() != 2) return;
    const auto selected = source.sound.personalities->instances[0].id;
    auto v = wire2(); v["reconciliation"].getDynamicObject()->setProperty("controls", array({object({
        {"instance", selected.toDashedString()}, {"control", 0}, {"replacement", {}}})}));
    if (decode2(v, policy, decoded)) { CHECK(decoded.reconciliation.controls.size() == 1 && decoded.reconciliation.controls[0].instance == selected);
        reject(source, decoded, policy, ProposalStatus::scopeViolation); }
    auto extra = v.clone(); extra["reconciliation"].getDynamicObject()->setProperty("permission", true); rejectWire2(extra, policy);
    extra = v.clone(); extra["reconciliation"]["controls"].getArray()->getReference(0).getDynamicObject()->setProperty("authority", true); rejectWire2(extra, policy);
    extra = v.clone(); extra["reconciliation"]["controls"].getArray()->getReference(0).getDynamicObject()->setProperty("control", 1.0); rejectWire2(extra, policy);
    extra = v.clone(); extra["reconciliation"]["controls"].getArray()->add(extra["reconciliation"]["controls"].getArray()->getReference(0).clone()); rejectWire2(extra, policy);
    auto overflow = wire2(); juce::Array<juce::var> tooMany;
    for (int n = 0; n < 97; ++n) tooMany.add(object({{"instance", selected.toDashedString()}, {"control", n}, {"replacement", {}}}));
    overflow["reconciliation"].getDynamicObject()->setProperty("controls", tooMany); rejectWire2(overflow, policy);
    for (auto key : {"detach", "remove", "replace"}) {
        overflow = wire2(); juce::Array<juce::var> ids; for (int n = 0; n < 13; ++n) ids.add(juce::Uuid().toDashedString());
        overflow["reconciliation"].getDynamicObject()->setProperty(key, ids); rejectWire2(overflow, policy);
    }
}

void drum_catalog_gate_and_retained_defaults() {
    begin("drum_catalog_gate_and_retained_defaults");
    auto source=std::make_unique<ProjectDocument>();source->sound.values[drumModeParameterStart]=1;CHECK(validateProjectDocument(*source));juce::var kept="retained";CHECK(!catalogFor(*source,kept)&&kept.toString()=="retained");CHECK(!providerCatalogFor(*source,kept)&&kept.toString()=="retained");
    RackDesign drum;drum.name="Drum retained";drum.panel.sections={"Drum"};drum.units={{0,NativeModule::drum,{},1}};RackCatalogSnapshot snapshot;CHECK(captureRackCatalog({},std::span<const RackDesign>(&drum,1),{},snapshot));
    CHECK(catalogInventoryFor(snapshot,kept)&&kept["entries"].isArray()&&kept["entries"].getArray()->isEmpty()&&bool(kept["complete"])&&kept["next"].toString()=="0");
    CHECK(catalogInventoryFor(snapshot,CatalogRendering::ownedLocal,kept));const auto* local=kept["entries"].getArray();
    CHECK(local&&local->size()==1&&juce::Uuid(local->getReference(0)["id"].toString())==drum.id&&local->getReference(0)["content_sha256"].toString()==snapshot.entries[0].contentSha256);
    auto broken=snapshot;broken.entries[0].definition.name="forged";kept="retained";CHECK(!catalogInventoryFor(broken,kept)&&kept.toString()=="retained");
    kept="retained";CHECK(!catalogInventoryFor(broken,CatalogRendering::ownedLocal,kept)&&kept.toString()=="retained");
    source->sound.values[drumModeParameterStart]=0;source->sound.values[13]=1;source->sound.compatibility.synth[0][1]=2;RackDesign retained;retained.name="All retained defaults";retained.panel.sections={"Tone"};retained.units={{0,NativeModule::wavetable,{},2}};
    for(std::size_t i=0;i<parameterCount;++i){const auto& p=parameterRegistry()[i];if(p.id.startsWith("synth.1.")&&p.id!="synth.1.drum_mode")retained.units[0].defaults.push_back({0,p.id.substring(8),source->sound.values[i]});}
    CHECK(retained.units[0].defaults.size()==293);CHECK(validateRackDesign(retained,RackValidationMode::instanceRecall));CHECK(captureRackCatalog({},std::span<const RackDesign>(&retained,1),{},snapshot));CHECK(catalogInventoryFor(snapshot,kept));CHECK(catalogFor(*source,kept)&&providerCatalogFor(*source,kept));
    auto state=std::make_shared<RackPersonalityState>();RackInstance instance;instance.id=juce::Uuid();instance.design=retained;instance.bindings={{0,0}};state->instances.push_back(instance);source->sound.personalities=state;CHECK(validateProjectDocument(*source));juce::MemoryBlock wire;CHECK(encodeProjectState(*source,wire));auto round=std::make_unique<ProjectDocument>();CHECK(decodeProjectState(wire.getData(),wire.getSize(),*round)&&*round==*source);CHECK(providerCatalogFor(*source,snapshot,kept));
    auto invalid=retained;invalid.units[0].defaults.push_back({0,"drum_mode",0});CHECK(!validateRackDesign(invalid,RackValidationMode::instanceRecall));
}
void native_catalog_complete_provider_inventory()
{
    begin(__func__);
    auto source=personalityProject();
    CHECK(source.sound.personalities&&source.sound.personalities->instances.size()==2);
    if(!source.sound.personalities||source.sound.personalities->instances.size()!=2)return;
    auto embedded=std::make_shared<RackPersonalityState>(*source.sound.personalities);
    embedded->instances.front().design.revision=4294967297ULL;source.sound.personalities=std::move(embedded);
    CHECK(validateProjectDocument(source));const auto before=source;juce::var projected;
    CHECK(providerCatalogFor(source,projected));CHECK(exact(source,before));
    const auto inventory=projected["rack_catalog"];
    CHECK(inventory.getDynamicObject()!=nullptr);
    if(!inventory.getDynamicObject())return; // Required availability RED has one owning cause.
    CHECK(inventory["snapshot"].isString()&&inventory["snapshot"].toString().isNotEmpty());
    CHECK(inventory["complete"].isBool()&&bool(inventory["complete"]));
    const auto* entries=inventory["entries"].getArray();CHECK(entries!=nullptr);
    if(!entries)return;
    const auto factories=airack::test::frozenFactoryRackDesigns();CHECK(factories.size()==21);
    auto contains=[&](const RackDesign& design){
        juce::MemoryBlock canonical;CHECK(encodeRackDesign(design,canonical,RackValidationMode::instanceRecall));
        const auto hash=juce::SHA256(canonical).toHexString();
        return std::any_of(entries->begin(),entries->end(),[&](const auto& row){
            return row["id"].toString()==design.id.toString()&&row["revision"].isString()
                &&row["revision"].toString()==juce::String(std::to_string(design.revision))&&row["name"].toString()==design.name
                &&row["content_sha256"].isString()&&row["content_sha256"].toString()==hash;
        });
    };
    const auto builtins=factoryRackDesigns();CHECK(builtins.size()==17&&entries->size()==19);
    for(const auto& factory:builtins)CHECK(contains(factory));
    if(source.sound.personalities)for(const auto& instance:source.sound.personalities->instances)CHECK(contains(instance.design));
    auto custom=nativeRecipe();custom.revision=UINT64_MAX;custom.name="Explicit custom";
    std::vector<RackDesign> native;for(const auto& instance:source.sound.personalities->instances)native.push_back(instance.design);
    native.push_back(custom);RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(factories,native,{},snapshot));
    CHECK(snapshot.entries.size()==24);juce::var customProjection;CHECK(providerCatalogFor(source,snapshot,customProjection));
    const auto* customRows=customProjection["rack_catalog"]["entries"].getArray();CHECK(customRows&&customRows->size()==24);
    juce::MemoryBlock bytes;CHECK(encodeRackDesign(custom,bytes));const auto expected=juce::SHA256(bytes).toHexString();
    if(customRows)CHECK(std::any_of(customRows->begin(),customRows->end(),[&](const auto& row){return row["id"].toString()==custom.id.toString()
        &&row["revision"].toString()=="18446744073709551615"&&row["content_sha256"].toString()==expected;}));
    CHECK(exact(source,before));juce::var sentinel("unchanged");auto forged=snapshot;forged.entries.front().contentSha256="altered";
    CHECK(!providerCatalogFor(source,forged,sentinel));CHECK(sentinel.toString()=="unchanged");
}
void provider_catalog_projection()
{
    begin(__func__); // Missing rows, wrong activity/locks/choice deduplication or over-budget serialization fail here.
    auto source = rich(); source.sound.macros[0] = {}; source.sound.macros[7].targetKey = parameterRegistry()[index("output.gain_db")].stateKey;
    const auto before = source; juce::var legacy, compact; CHECK(catalogFor(source, legacy)); CHECK(providerCatalogFor(source, compact));
    const auto enums = compact["projection_enums"];
    CHECK(json(enums["slot_use"]) == json(array({"never_used", "present", "retired"})));
    CHECK(json(enums["effect_kind"]) == json(array({"drive", "delay", "reverb", "compressor", "eq", "filter_fx", "chorus", "phaser", "flanger", "stereo", "dimension", "frequency_shift", "convolution", "frequency_split", "mid_side_split", "dynamic_eq", "space_reverb", "era_reverb", "saturation"})));
    const auto racks = compact["racks"];
    CHECK(json(racks["synth_columns"]) == json(array({"slot", "use", "position", "locked"})));
    CHECK(json(racks["effect_columns"]) == json(array({"slot", "use", "position", "chain", "kind", "locked"})));
    CHECK(json(compact["macro_columns"]) == json(array({"index", "target_id", "amount"})));
    CHECK(compact["wire_guidance"]["target_id"].isString() && !compact["wire_guidance"].hasProperty("target_control"));
    const auto* synths = racks["synths"].getArray(); const auto* effects = racks["effects"].getArray();
    const auto* macros = compact["macro_bindings"].getArray();
    CHECK(synths && synths->size() == 4 && effects && effects->size() == 8 && macros && macros->size() == 8);
    for (const auto name : {"synths", "effects"}) {
        const auto* rows = racks[name].getArray(); const auto* old = legacy["racks"][name].getArray();
        if (!rows || !old || rows->size() != old->size()) continue;
        for (int n = 0; n < rows->size(); ++n) {
            const auto* row = (*rows)[n].getArray(); const bool synth = std::string_view(name) == "synths";
            CHECK(row && row->size() == (synth ? 4 : 6)); CHECK((*old)[n].getDynamicObject());
            if (!row || row->size() != (synth ? 4 : 6)) continue;
            for (const auto& value : *row) CHECK(value.isInt() || value.isBool());
            CHECK(int((*row)[0]) == n); CHECK((*enums["slot_use"].getArray())[int((*row)[1])] == (*old)[n]["use"]);
            CHECK(int((*row)[2]) == int((*old)[n]["position"]));
            CHECK(bool((*row)[synth ? 3 : 5]) == bool((*old)[n]["locked"]));
            if (!synth) { CHECK(int((*row)[3]) == int((*old)[n]["chain"]));
                CHECK((*enums["effect_kind"].getArray())[int((*row)[4])] == (*old)[n]["kind"]); }
        }
    }
    CHECK(json(racks["synth_order"]) == json(legacy["racks"]["synth_order"]));
    CHECK(json(racks["effect_order"]) == json(legacy["racks"]["effect_order"]));
    if (macros && macros->size() == 8) for (int n = 0; n < 8; ++n) {
        const auto* row = (*macros)[n].getArray(); CHECK(row && row->size() == 3);
        const auto& old = (*legacy["macro_bindings"].getArray())[n]; CHECK(old.getDynamicObject());
        if (!row || row->size() != 3) continue;
        CHECK(int((*row)[0]) == n + 1); CHECK(double((*row)[2]) == double(old["amount"]));
        if (old["target_id"].isVoid()) CHECK((*row)[1].isVoid());
        else CHECK((*row)[1].isString() && (*row)[1].toString() == old["target_id"].toString());
    }
    juce::var legacyAfter; CHECK(catalogFor(source, legacyAfter)); CHECK(json(legacyAfter) == json(legacy)); CHECK(exact(source, before));
    const auto* rows = compact["controls"].getArray(); const auto* columns = compact["control_columns"].getArray();
    // Decision 7: the rows are the source's reach, ascending.
    const auto reach = userReach(source.sound); std::vector<std::size_t> reached;
    for (std::size_t i = 0; i < parameterCount; ++i) if (reach[i]) reached.push_back(i);
    const auto* choices = compact["choice_sets"].getArray(); CHECK(rows && rows->size() == int(reached.size())); CHECK(columns && columns->size() == 11);
    // Placement, Space shapes, EQ extensions and delay notes have distinct shared choice sets; byte budgets stay fixed.
    CHECK(choices && !choices->isEmpty());
    if (choices) for (int n = 0; n < choices->size(); ++n) for (int previous = 0; previous < n; ++previous)
        CHECK(json((*choices)[n]) != json((*choices)[previous]));
    CHECK(juce::JSON::toString(compact, true, 17).getNumBytesAsUTF8() <= 196608);
    if(choices)CHECK(std::count_if(choices->begin(),choices->end(),[](const auto& set){return json(set)==json(array({"Stereo","Left","Right","Mid","Side"}));})==1);
    if(choices){
        CHECK(std::count_if(choices->begin(),choices->end(),[](const auto& set){return json(set)==json(array({"Internal band","Internal free","External band","External free"}));})==1);
        CHECK(std::count_if(choices->begin(),choices->end(),[](const auto& set){return json(set)==json(array({"Legacy","Band pass","Tilt shelf","Flat tilt"}));})==1);
        CHECK(std::count_if(choices->begin(),choices->end(),[](const auto& set){return json(set)==json(array({"Legacy","6 dB/oct","36 dB/oct","48 dB/oct","96 dB/oct"}));})==1);
        CHECK(std::count_if(choices->begin(),choices->end(),[](const auto& set){return json(set)==json(array({"Full band","Three band"}));})==1);
        CHECK(std::count_if(choices->begin(),choices->end(),[](const auto& set){return json(set)==json(array({"Soft","Glow","Dense"}));})==1);
    }
    const char* expected[]{"id", "unit", "kind", "min", "max", "default", "skew", "choice_set", "value_normalized", "active", "locked"};
    if (columns && columns->size() == 11) for (int c = 0; c < 11; ++c) CHECK((*columns)[c].toString() == expected[c]);
    std::set<std::string> uniqueSets;
    if (choices) { CHECK(juce::JSON::toString(compact["choice_sets"], true, 17).getNumBytesAsUTF8() <= 10176);
        for (const auto& set : *choices) { const auto* a = set.getArray(); CHECK(a && a->size() <= 22); CHECK(uniqueSets.insert(json(set)).second);
            if (a) for (const auto& text : *a) { CHECK(text.isString() && text.toString().getNumBytesAsUTF8() <= 32);
                for (auto byte : text.toString().toStdString()) CHECK(static_cast<unsigned char>(byte) < 128 && byte != '"' && byte != '\\'); } } }
    if (!rows || rows->size() != int(reached.size()) || !choices) return;
    for (int r = 0; r < rows->size(); ++r) {
        const auto n = int(reached[std::size_t(r)]);
        const auto& p = parameterRegistry()[std::size_t(n)]; const auto& v = (*rows)[r]; const auto* row = v.getArray(); CHECK(row && row->size() == 11);
        CHECK(juce::JSON::toString(v, true, 17).getNumBytesAsUTF8() <= 240); if (!row || row->size() != 11) continue;
        CHECK((*row)[0].toString() == p.id && (*row)[1].toString() == p.unit);
        CHECK(p.id.getNumBytesAsUTF8() <= 64 && p.unit.getNumBytesAsUTF8() <= 16);
        for (const auto& text : {p.id, p.unit}) for (auto byte : text.toStdString()) CHECK(static_cast<unsigned char>(byte) < 128 && byte != '"' && byte != '\\');
        CHECK(int((*row)[2]) == static_cast<int>(p.kind)); CHECK(double((*row)[3]) == p.minimum && double((*row)[4]) == p.maximum);
        CHECK(double((*row)[5]) == p.defaultValue && double((*row)[6]) == p.skew && double((*row)[8]) == source.sound.values[std::size_t(n)]);
        CHECK(bool((*row)[9]) == bool((*legacy["controls"].getArray())[n]["active"])); CHECK(bool((*row)[10]) == bool((*legacy["controls"].getArray())[n]["locked"]));
        for (int c : {3, 4, 5, 6, 8}) CHECK(juce::JSON::toString((*row)[c], true, 17).getNumBytesAsUTF8() <= 25);
        const auto set = int((*row)[7]); CHECK(set >= 0 && set < choices->size());
        if (set >= 0 && set < choices->size()) { const auto* actual = (*choices)[set].getArray(); CHECK(actual && actual->size() == int(p.choices.size()));
            if (actual && actual->size() == int(p.choices.size())) for (int c = 0; c < actual->size(); ++c) CHECK((*actual)[c].toString() == p.choices[std::size_t(c)]); }
    }
    auto invalid = source; invalid.sound.values[0] = 2; juce::var sentinel = "unchanged";
    CHECK(!providerCatalogFor(invalid, sentinel)); CHECK(sentinel.toString() == "unchanged");
    for (auto scope : {RequestScope::sound, RequestScope::pattern, RequestScope::both}) {
        ProposalPolicy policy; policy.scope = scope;
        CHECK(juce::JSON::toString(proposalSchema(policy), true, 17).getNumBytesAsUTF8() <= 65536);
    }
}
}

void captured_compound_removal_permutations() {
    ProjectDocument source;RackDesign synth;synth.name="Source";synth.units={{0,NativeModule::analog,{}}};synth.panel.sections={"Tone"};
    RackOperation op;op.kind=RackOperationKind::adopt;op.design=synth;op.newInstance=juce::Uuid();op.allocation={{0,0}};SoundDocument out;RackDifference diff;
    CHECK(makeRackCandidate(source.sound,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);source.sound=out;const auto synthId=op.newInstance;
    RackDesign fx;fx.name="Independent";fx.role=RackRole::fx;fx.units={{0,NativeModule::drive,{}}};fx.panel.sections={"Tone"};op={};op.design=fx;op.newInstance=juce::Uuid();op.destination={RackRole::fx,0,0};
    CHECK(makeRackCandidate(source.sound,op,SoundEditOrigin::manual,out,diff)==RackCandidateStatus::ready);source.sound=out;const auto fxId=op.newInstance;
    ProposalPolicy policy;policy.scope=RequestScope::sound;
    ProposalEdits e;e.reconciliation.remove={synthId,fxId};ProjectDocument a,b;
    CHECK(admitProposal(source,policy,e,a)==ProposalStatus::accepted);e.reconciliation.remove={fxId,synthId};CHECK(admitProposal(source,policy,e,b)==ProposalStatus::accepted);CHECK(a==b);CHECK(a.sound.values==source.sound.values);CHECK(!a.sound.personalities);
    e.reconciliation.remove={synthId};auto sentinel=source;CHECK(admitProposal(source,policy,e,sentinel)!=ProposalStatus::accepted);CHECK(sentinel==source);
    auto locked=source;locked.sound.effectLocks[0]=true;e.reconciliation.remove={synthId,fxId};CHECK(admitProposal(locked,policy,e,sentinel)==ProposalStatus::lockedDependency);CHECK(sentinel==source);
}
void decode_diagnostics()
{
    begin(__func__);
    auto rejected=[&](const std::string& text,ProposalDecodeReason expected,const char* expectedName=nullptr){
        ProposalEdits output;output.explanation="preserved";ProposalDecodeReason reason=ProposalDecodeReason::accepted;
        CHECK(!decodeProposal(text,ProposalPolicy{},output,&reason));if(reason!=expected)std::cout<<"decode actual="<<int(reason)<<" expected="<<int(expected)<<" name="<<proposalDecodeReasonName(reason)<<'\n';CHECK(reason==expected);CHECK(output.explanation=="preserved");if(expectedName)CHECK(std::string_view(proposalDecodeReasonName(reason))==expectedName);};
    rejected("{",ProposalDecodeReason::strictJson);rejected("{}",ProposalDecodeReason::rootFields);
    auto value=wire();value.getDynamicObject()->setProperty("parameter_edits",array({object({{"id","output.gain_db"},{"normalized","bad"}})}));rejected(json(value),ProposalDecodeReason::parameterEdit);
    value=wire();value.getDynamicObject()->setProperty("rack_edits",array({object({})}));rejected(json(value),ProposalDecodeReason::rackEdit);
    value=wire();value.getDynamicObject()->setProperty("macro_edits",array({object({})}));rejected(json(value),ProposalDecodeReason::macroEdit);
    value=wire();value.getDynamicObject()->setProperty("pattern",object({}));rejected(json(value),ProposalDecodeReason::pattern);
    value=wire();value.getDynamicObject()->setProperty("parameter_edits",array({object({{"id","unsupported"},{"normalized",.5}})}));rejected(json(value),ProposalDecodeReason::unknownParameter,"proposal_parameter_unknown");
    value=wire();value.getDynamicObject()->setProperty("parameter_edits",array({object({{"id","output.gain_db"},{"normalized",.5}}),object({{"id","output.gain_db"},{"normalized",.4}})}));rejected(json(value),ProposalDecodeReason::duplicateParameter,"proposal_parameter_duplicate");
    value=wire();value.getDynamicObject()->setProperty("rack_edits",array({rack("remove_synth",0,0,0)}));rejected(json(value),ProposalDecodeReason::invalidRack,"proposal_rack_invalid");
    value=wire();value.getDynamicObject()->setProperty("explanation",juce::String::repeatedString("x",2049));rejected(json(value),ProposalDecodeReason::invalidText,"proposal_text_invalid");
    value=wire();value.getDynamicObject()->setProperty("macro_edits",array({macro(1,{},.5)}));rejected(json(value),ProposalDecodeReason::invalidMacro,"proposal_macro_invalid");
    value=wire();value.getDynamicObject()->setProperty("macro_edits",array({macro(1,"output.gain_db",.5),macro(1,"output.gain_db",.5)}));rejected(json(value),ProposalDecodeReason::invalidMacro,"proposal_macro_invalid");
    value=wire();value.getDynamicObject()->setProperty("macro_edits",array({macro(1,"unsupported",.5)}));rejected(json(value),ProposalDecodeReason::unknownMacroTarget,"proposal_macro_target_unknown");
    value=wire();value.getDynamicObject()->setProperty("version",2);value.getDynamicObject()->setProperty("name","Warm Test Pad");value.getDynamicObject()->setProperty("reconciliation",object({}));rejected(json(value),ProposalDecodeReason::reconciliation);
    ProposalEdits output;ProposalDecodeReason reason=ProposalDecodeReason::pattern;CHECK(decodeProposal(json(wire()),output,&reason));CHECK(reason==ProposalDecodeReason::accepted);
}
// UNCOMPILED / SOURCE-ONLY EXTERNAL DRAFT. No intended RED/PASS or runtime claim.
// Contract B: AC8ABDA728A1083AEF51F3608C372E645CDCBFBAE4FB057EEDE9F923928729E1
// Member bindings C: 0E56E3B943CF294F1577C73253DD3AD138B00375CA0E238B84997E698E4C6D39
// Explicit legacy/catalog context D:
// 6C434A8958EC91F5675F447CA62743DFAC5753683952018A06E4993B10082F76
// Root authority qualification E:
// 95712ABCD0D51DB5E1175859AA99CAD14F9107E98DAA94A81A60A634EF1686BA
// Frozen shared-interface source A:
// CCE7A9F31040BB521769E90CF0174A622CEC44854F7C47E8DF07BEE63D3FAF45
// Routing: inherited parent model/effort; no overrides.
//
// Owning insertion after the interface declaration boundary:
// 1. Insert this fragment after AiProposalTests.cpp's closing anonymous
//    namespace and before main. Its helpers join that same unnamed namespace;
//    all existing helpers listed below have already been declared there.
// 2. Add task12_catalog_authority_proposal(); to its ordinary main path after
//    the diagnostic-only argument exits, before native_catalog_complete_provider_inventory().
//    Keep every existing main invocation and every legacy section unchanged.
// 3. Reuse CHECK/begin/object/array/json/index/exact/same/closed/wire/wire2/param/
//    macro/nativeRecipe/rich. Existing includes already expose all
//    used public production APIs. This fragment makes its <array> use explicit.
// This is an insertion fragment, not an independent target or fixture framework.
//
// E moves the identical existing RequestTag declaration into ProposalDocument.h
// and adds CatalogAuthority::root without changing the approved overload order.
// This draft supplies a concrete full captured root and tests repeat stability
// plus independence of all four root fields. It invents no root helper/member
// or ID bit-pattern; request/context equality is owned by the wire/generation
// slices, not an additional public seam in this proposal-admission fixture.
//
// Factory Lumen Drift's actual defaults are compact (six). Native82 names the
// trusted recall mode, not the number of defaults in that factory. The positive
// full82 case below is an original-source embedded native bank. Registered FX
// remains portableDesign (at most23 unique active defaults); this fixture does
// not fabricate 23 supported keys for a module whose active bank has fewer.

#include <array>

namespace
{
bool task12_same_catalog_intent(const ProposalEdits& a, const ProposalEdits& b)
{
    if (!same(a, b) || a.catalogSelections.size() != b.catalogSelections.size()
        || a.catalogParameters.size() != b.catalogParameters.size()) return false;
    for (std::size_t n = 0; n < a.catalogSelections.size(); ++n) {
        const auto& x = a.catalogSelections[n]; const auto& y = b.catalogSelections[n];
        if (x.localId != y.localId || x.design != y.design || x.revision != y.revision
            || x.contentSha256 != y.contentSha256 || x.destination.role != y.destination.role
            || x.destination.chain != y.destination.chain || x.destination.position != y.destination.position) return false;
    }
    for (std::size_t n = 0; n < a.catalogParameters.size(); ++n) {
        const auto& x = a.catalogParameters[n]; const auto& y = b.catalogParameters[n];
        if (x.selection != y.selection || x.unit != y.unit || x.key != y.key
            || std::bit_cast<std::uint32_t>(x.normalized) != std::bit_cast<std::uint32_t>(y.normalized)) return false;
    }
    return true;
}

juce::var task12_selection_json(int local, const RackCatalogEntry& entry, int position)
{
    return object({{"local_id", local}, {"design", entry.definition.id.toDashedString()},
        {"revision", juce::String(std::to_string(entry.definition.revision))},
        {"content_sha256", entry.contentSha256},
        {"destination", object({{"role", entry.definition.role == RackRole::synth ? "synth" : "fx"},
            {"chain", int(masterChain)}, {"position", position}})}});
}

juce::var task12_parameter_json(int selection, int unit, const char* key, double normalized)
{
    return object({{"selection", selection}, {"unit", unit}, {"key", key}, {"normalized", normalized}});
}

juce::var task12_catalog_wire(juce::var selections, juce::var parameters)
{
    // D preserves the existing nine-field legacy version2 shape. This explicit
    // catalog context adds exactly the two required arrays; it never infers a
    // new context from JSON, and does not use the legacy seven-field v1 shape.
    auto v = wire2();
    v.getDynamicObject()->setProperty("catalog_selections", selections);
    v.getDynamicObject()->setProperty("catalog_parameter_edits", parameters);
    return v;
}

void task12_reject_catalog_wire(const juce::var& v, const ProposalPolicy& policy,
                               const CatalogAuthority& authority, const ProposalEdits& sentinel,
                               std::optional<ProposalDecodeReason> wanted = {})
{
    auto out = sentinel; const auto before = out;
    ProposalDecodeReason reason = ProposalDecodeReason::accepted;
    CHECK(!decodeProposal(json(v), policy, authority, out, &reason));
    CHECK(reason != ProposalDecodeReason::accepted);
    if (wanted && reason != *wanted) std::cout << "catalog decode actual=" << proposalDecodeReasonName(reason) << " expected=" << proposalDecodeReasonName(*wanted) << '\n';
    CHECK(!wanted || reason == *wanted);
    CHECK(task12_same_catalog_intent(out, before));
}

void task12_reject_catalog_admission(const ProjectDocument& source, const ProposalPolicy& policy,
                                   const ProposalEdits& edits, const CatalogAuthority& authority,
                                   std::optional<ProposalStatus> wanted = {})
{
    auto output = rich(); const auto before = output;
    const auto status = admitProposal(source, policy, edits, authority, output);
    CHECK(status != ProposalStatus::accepted);
    if (wanted) CHECK(status == *wanted);
    if (wanted && status != *wanted) std::cerr << section << ": status " << int(status) << ", wanted " << int(*wanted) << " for " << describe(edits) << '\n';
    CHECK(exact(output, before));
    auto alias = source;
    CHECK(admitProposal(alias, policy, edits, authority, alias) == status);
    CHECK(exact(alias, source));
}

void task12_catalog_authority_proposal()
{
    begin(__func__);
    const auto factories = airack::test::frozenFactoryRackDesigns(); CHECK(factories.size() == 21);
    const auto factory = std::find_if(factories.begin(), factories.end(), [](const auto& d) {
        return d.id == juce::Uuid("91387715-13d3-4c57-a479-12251842c401");
    });
    CHECK(factory != factories.end()); if (factory == factories.end()) return;
    CHECK(factory->role == RackRole::fx && factory->units.size() == 1
        && factory->units[0].module == NativeModule::reverb && factory->units[0].defaults.size() == 6);

    // Existing Task11 native fixture setup: seed the ORIGINAL source with a
    // full retained bank. This is not imported as portable authority during
    // the admission under test. Actual source bytes must establish provenance.
    RackDesign retained; retained.id = juce::Uuid("123f46e8-87e7-43da-a752-12a012120082");
    retained.revision = UINT64_MAX; retained.name = "Original retained native bank";
    retained.intent = "Full current bank with active and inactive recall values";
    retained.role = RackRole::fx; retained.units = {{0, NativeModule::drive, {}}};
    retained.panel.sections = {"Drive"};
    retained.panel.controls = {{"Gain", 0, 0, "drive.gain_db", FocusWidget::rotary, FocusResponse::linear, 0, 24}};
    const auto defaults = defaultParameterValues(); const auto registry = parameterRegistry();
    for (std::size_t n = 0; n < dynamicEqParameterStart; ++n) if (registry[n].id.startsWith("effect.1."))
        retained.units[0].defaults.push_back({0, registry[n].id.substring(9), defaults[n]});
    for (auto& p : retained.units[0].defaults) {
        if (p.key == "drive.gain_db") p.normalized = .37f;
        if (p.key == "reverb.mix") p.normalized = .63f;
        if (p.key == "frequency_shift.hz") p.normalized = .72f;
    }
    CHECK(retained.units[0].defaults.size() == 82);
    CHECK(validateRackDesign(retained, RackValidationMode::instanceRecall));
    CHECK(!validateRackDesign(retained, RackValidationMode::portableDesign));
    const std::array<RackDesign, 1> retainedDefinitions{retained};
    RackCatalogSnapshot seed; CHECK(captureRackCatalog({}, retainedDefinitions, {}, seed));
    CHECK(seed.entries.size() == 1); if (seed.entries.size() != 1) return;
    ProjectDocument source; SoundDocument seeded; RackDifference difference;
    RackOperation seedOperation; seedOperation.newInstance = juce::Uuid();
    seedOperation.destination = {RackRole::fx, masterChain, 0}; seedOperation.allocation = {{0, 0}};
    CHECK(makeCatalogRackCandidate(source.sound, seed, retained.id, retained.revision,
        seed.entries[0].contentSha256, seedOperation, SoundEditOrigin::manual, seeded, difference) == RackCandidateStatus::ready);
    source.sound = seeded; CHECK(validateProjectDocument(source));
    CHECK(source.sound.personalities && source.sound.personalities->instances.size() == 1);
    if (!source.sound.personalities || source.sound.personalities->instances.size() != 1) return;
    CHECK(source.sound.personalities->instances[0].design == retained);
    const auto original = source;

    auto portable = nativeRecipe(); portable.id = juce::Uuid("123f46e8-87e7-43da-a752-12a012120023");
    portable.revision = UINT64_MAX; portable.role = RackRole::fx;
    portable.name = "Custom portable drive"; portable.intent = "Explicit portable source";
    portable.units = {{0, NativeModule::drive, {{0, "drive.gain_db", .3f}, {0, "drive.mix", .8f}}}};
    portable.panel.controls = {{"Gain", 0, 0, "drive.gain_db", FocusWidget::rotary, FocusResponse::linear, 0, 24}};
    CHECK(validateRackDesign(portable, RackValidationMode::portableDesign));
    CHECK(portable.units[0].defaults.size() <= 23);
    const std::array<RackDesign, 2> embeddedDefinitions{retained, portable};
    RackCatalogSnapshot root; CHECK(captureRackCatalog(factories, embeddedDefinitions, {}, root));
    CHECK(root.entries.size() == 23);
    RackCatalogPage complete; CHECK(catalogPage(root, {}, 0, 65536, complete));
    CHECK(complete.complete && complete.next == root.entries.size() && complete.entries.size() == root.entries.size());
    if (!complete.complete || complete.entries.size() != root.entries.size()) return;
    auto entry = [&](juce::Uuid id) {
        return std::find_if(root.entries.begin(), root.entries.end(), [&](const auto& e) { return e.definition.id == id; });
    };
    const auto native = entry(factory->id), custom = entry(portable.id), embedded = entry(retained.id);
    CHECK(native != root.entries.end() && custom != root.entries.end() && embedded != root.entries.end());
    if (native == root.entries.end() || custom == root.entries.end() || embedded == root.entries.end()) return;
    CHECK(rackCatalogNativeEntry(root, std::size_t(native - root.entries.begin())));
    CHECK(rackCatalogNativeEntry(root, std::size_t(embedded - root.entries.begin())));
    CHECK(rackCatalogNativeEntry(root, std::size_t(custom - root.entries.begin())));

    CatalogAuthority authority;
    authority.root = {juce::Uuid("123f46e8-87e7-43da-a752-12a012120040"), 12, 107, 31};
    authority.snapshot = root.id;
    authority.descriptorJson = juce::JSON::toString(rackCatalogPageDescriptor(complete), true, 22).toStdString();
    for (const auto* e : {&*native, &*custom, &*embedded}) {
        CatalogDefinition definition; definition.design = e->definition.id; definition.revision = e->definition.revision;
        definition.contentSha256 = e->contentSha256;
        RackDesign resolved; CHECK(resolveCatalogDesign(root, definition.design, definition.revision, definition.contentSha256, resolved));
        CHECK(encodeRackDesign(resolved, definition.canonical, RackValidationMode::instanceRecall));
        CHECK(definition.canonical.getSize() <= 65536 && juce::SHA256(definition.canonical).toHexString() == definition.contentSha256);
        authority.definitions.push_back(std::move(definition));
    }
    CHECK(authority.definitions.size() == 3); // Repeated native selection shares canonical bytes.

    ProposalPolicy policy; policy.allowMacroChanges = true;
    auto v = task12_catalog_wire(array({task12_selection_json(7, *native, 1),
        task12_selection_json(99, *native, 2), task12_selection_json(201, *custom, 3),
        task12_selection_json(42, *embedded, 4)}), array({
        task12_parameter_json(7, 0, "reverb.mix", .25), task12_parameter_json(99, 0, "reverb.mix", .75),
        task12_parameter_json(201, 0, "drive.gain_db", .7), task12_parameter_json(42, 0, "drive.gain_db", .41)}));
    v.getDynamicObject()->setProperty("macro_edits", array({macro(1, "catalog:99:0:reverb.mix", .5)}));
    ProposalEdits edits; CHECK(decodeProposal(json(v), policy, authority, edits)); CHECK(edits.name == "Warm Test Pad");
    CHECK(edits.catalogSelections.size() == 4 && edits.catalogParameters.size() == 4 && edits.macros.size() == 1);
    if (edits.catalogSelections.size() != 4 || edits.catalogParameters.size() != 4 || edits.macros.size() != 1) return;
    CHECK(edits.catalogSelections[0].localId == 7 && edits.catalogSelections[1].localId == 99
        && edits.catalogSelections[0].design == edits.catalogSelections[1].design);
    CHECK(edits.catalogSelections[2].revision == UINT64_MAX && edits.catalogSelections[3].revision == UINT64_MAX);
    CHECK(edits.macros[0].targetId == "catalog:99:0:reverb.mix"); // Keep unlowered intent.
    // A provider copies a design's identity as the catalog shows it, which selects the same rack.
    {
        const auto shown = juce::JSON::parse(juce::String::fromUTF8(authority.descriptorJson.data(), int(authority.descriptorJson.size())));
        juce::String nativeId;
        if (const auto* rows = shown["entries"].getArray()) for (const auto& row : *rows) if (juce::Uuid(row["id"].toString()) == native->definition.id) nativeId = row["id"].toString();
        CHECK(nativeId.isNotEmpty() && nativeId != native->definition.id.toDashedString());
        auto copied = v.clone(); copied["catalog_selections"].getArray()->getReference(0).getDynamicObject()->setProperty("design", nativeId);
        ProposalEdits copiedEdits; CHECK(decodeProposal(json(copied), policy, authority, copiedEdits));
        CHECK(task12_same_catalog_intent(copiedEdits, edits));
    }

    const auto legacySchema = json(proposalSchema(policy));
    const auto schema = proposalSchema(policy, authority);
    closed(schema, {"version", "parameter_edits", "rack_edits", "macro_edits", "pattern", "characteristics", "explanation",
        "reconciliation", "name", "catalog_selections", "catalog_parameter_edits"});
    CHECK(json(schema).size() <= 65536 && json(proposalSchema(policy)) == legacySchema);
    const auto properties = schema["properties"];
    closed(properties["catalog_selections"]["items"], {"local_id", "design", "revision", "content_sha256", "destination"});
    closed(properties["catalog_selections"]["items"]["properties"]["destination"], {"role", "chain", "position"});
    closed(properties["catalog_parameter_edits"]["items"], {"selection", "unit", "key", "normalized"});
    CHECK(int(properties["catalog_selections"]["maxItems"]) == 12);
    CHECK(properties["catalog_selections"]["items"]["properties"]["revision"]["type"] == juce::var("string"));

    // Both legacy versions retain their exact successful helper shapes, with
    // no required catalog fields and no authority inferred from provider JSON.
    auto legacy1 = wire(); legacy1.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", .2)}));
    const auto legacy1Bytes = json(legacy1); ProposalEdits plain, policyAware;
    CHECK(decodeProposal(legacy1Bytes, plain) && decodeProposal(legacy1Bytes, policy, policyAware));
    CHECK(same(plain, policyAware) && plain.catalogSelections.empty() && plain.catalogParameters.empty());
    auto legacy2 = wire2(); const auto legacy2Bytes = json(legacy2);
    ProposalEdits v2Edits; CHECK(decodeProposal(legacy2Bytes, policy, v2Edits));
    CHECK(v2Edits.catalogSelections.empty() && v2Edits.catalogParameters.empty());
    ProjectDocument legacyOutput; CHECK(admitProposal(ProjectDocument{}, policy, v2Edits, legacyOutput) == ProposalStatus::accepted);
    CHECK(json(legacy1) == legacy1Bytes && json(legacy2) == legacy2Bytes);
    auto legacySentinel = edits; const auto legacyBefore = legacySentinel;
    CHECK(!decodeProposal(json(v), legacySentinel)); CHECK(task12_same_catalog_intent(legacySentinel, legacyBefore));
    CHECK(!decodeProposal(json(v), policy, legacySentinel)); CHECK(task12_same_catalog_intent(legacySentinel, legacyBefore));
    auto withoutContext = rich(); const auto withoutContextBefore = withoutContext;
    CHECK(admitProposal(source, policy, edits, withoutContext) != ProposalStatus::accepted);
    CHECK(exact(withoutContext, withoutContextBefore));
    auto aliasOnly = wire2(); aliasOnly.getDynamicObject()->setProperty("macro_edits", array({macro(1, "catalog:99:0:reverb.mix", .5)}));
    CHECK(!decodeProposal(json(aliasOnly), policy, legacySentinel)); CHECK(task12_same_catalog_intent(legacySentinel, legacyBefore));

    ProjectDocument candidate; CHECK(admitProposal(source, policy, edits, authority, candidate) == ProposalStatus::accepted);
    CHECK(exact(source, original) && validateProjectDocument(candidate));
    CHECK(candidate.sound.personalities && candidate.sound.personalities->instances.size() == 5);
    if (!candidate.sound.personalities || candidate.sound.personalities->instances.size() != 5) return;
    std::vector<const RackInstance*> repeated;
    const RackInstance* customInstance = nullptr; const RackInstance* retainedCopy = nullptr;
    for (const auto& i : candidate.sound.personalities->instances) {
        if (i.design.id == native->definition.id) repeated.push_back(&i);
        if (i.design.id == custom->definition.id) customInstance = &i;
        if (i.design.id == embedded->definition.id && i.id != source.sound.personalities->instances[0].id) retainedCopy = &i;
    }
    CHECK(repeated.size() == 2 && customInstance && retainedCopy);
    if (repeated.size() != 2 || !customInstance || !retainedCopy) return;
    auto first = repeated[0], second = repeated[1];
    auto mixIndex = [](const RackInstance& i) {
        const auto* binding = rackUnitBinding(i, 0); CHECK(binding);
        const auto resolved = binding ? rackParameterIndex(NativeModule::reverb, binding->slot, "reverb.mix") : std::nullopt;
        CHECK(resolved); return resolved.value_or(0);
    };
    if (candidate.sound.values[mixIndex(*first)] == .75f) std::swap(first, second);
    const auto firstMix = mixIndex(*first), secondMix = mixIndex(*second);
    CHECK(first->id != second->id && first->bindings != second->bindings && firstMix != secondMix);
    CHECK(first->design == *factory);
    CHECK(second->design == *factory);
    CHECK(candidate.sound.values[firstMix] == .25f && candidate.sound.values[secondMix] == .75f);
    CHECK(candidate.sound.macros[0].targetKey == registry[secondMix].stateKey && candidate.sound.macros[0].amount == .5f);
    CHECK(edits.macros[0].targetId == "catalog:99:0:reverb.mix" && task12_same_catalog_intent(edits, legacyBefore));
    const auto* portableBinding = rackUnitBinding(*customInstance, 0);
    const auto* retainedBinding = rackUnitBinding(*retainedCopy, 0); CHECK(portableBinding && retainedBinding);
    if (!portableBinding || !retainedBinding) return;
    const auto portableGain = rackParameterIndex(NativeModule::drive, portableBinding->slot, "drive.gain_db"); CHECK(portableGain);
    CHECK(customInstance->design == portable && portableGain && candidate.sound.values[*portableGain] == .7f);
    CHECK(retainedCopy->design == retained && retainedCopy->design.units[0].defaults.size() == 82);
    for (const auto& p : retained.units[0].defaults) {
        const auto physical = rackParameterIndex(NativeModule::drive, retainedBinding->slot, p.key); CHECK(physical);
        if (physical) CHECK(candidate.sound.values[*physical] == (p.key == "drive.gain_db" ? .41f : p.normalized));
    }
    ProjectDocument again; CHECK(admitProposal(source, policy, edits, authority, again) == ProposalStatus::accepted);
    CHECK(exact(again, candidate)); // Stable repeat admission of the same captured root/local IDs.
    for (int field = 0; field < 4; ++field) {
        auto anotherRoot = authority;
        if (field == 0) anotherRoot.root.runtime = juce::Uuid("123f46e8-87e7-43da-a752-12a012120041");
        if (field == 1) ++anotherRoot.root.requestId;
        if (field == 2) ++anotherRoot.root.editRevision;
        if (field == 3) ++anotherRoot.root.contextRevision;
        ProjectDocument independent;
        CHECK(admitProposal(source, policy, edits, anotherRoot, independent) == ProposalStatus::accepted);
        CHECK(independent.sound.personalities && independent.sound.personalities->instances.size() == 5);
        if (independent.sound.personalities && independent.sound.personalities->instances.size() == 5) {
            CHECK(independent.sound.personalities->instances[0] == source.sound.personalities->instances[0]);
            for (std::size_t n = 1; n < 5; ++n) {
                const auto& other = independent.sound.personalities->instances[n];
                CHECK(other.id != candidate.sound.personalities->instances[n].id);
                CHECK(!other.id.isNull() && other.bindings == candidate.sound.personalities->instances[n].bindings);
            }
        }
    }
    auto permission = policy; permission.allowMacroChanges = false;
    task12_reject_catalog_admission(source, permission, edits, authority, ProposalStatus::macroChangeNotRequested);
    auto locked = source;
    for (std::uint8_t slot = 1; slot < 8; ++slot) {
        const auto physical = rackParameterIndex(NativeModule::reverb, slot, "reverb.mix"); CHECK(physical);
        if (physical) locked.sound.controlLocks[*physical] = true;
    }
    task12_reject_catalog_admission(locked, policy, edits, authority, ProposalStatus::lockedDependency);
    auto patternOnly = policy; patternOnly.scope = RequestScope::pattern;
    task12_reject_catalog_admission(source, patternOnly, edits, authority, ProposalStatus::scopeViolation);
    auto overCapacity = edits;
    for (std::uint8_t n = 0; n < 4; ++n) overCapacity.catalogSelections.push_back({std::uint8_t(150 + n),
        native->definition.id, native->definition.revision, native->contentSha256,
        {RackRole::fx, masterChain, std::uint8_t(5 + n)}});
    task12_reject_catalog_admission(source, policy, overCapacity, authority, ProposalStatus::capacityReached);

    // Strict identities, tuple uniqueness, references, local grammar and
    // closed objects are behavior assertions, not schema mirroring alone.
    // Each names the part of the catalog intent it breaks.
    using Reason = ProposalDecodeReason;
    constexpr Reason mutationReasons[]{Reason::catalogIntent, Reason::catalogIntent, Reason::catalogIntent, Reason::catalogIntent,
        Reason::catalogIntent, Reason::catalogParameter, Reason::catalogSelection, Reason::catalogSelection, Reason::catalogIntent,
        Reason::catalogSelection, Reason::catalogSelection, Reason::rootFields};
    for (int mutation = 0; mutation < 12; ++mutation) {
        auto bad = v.clone(); auto* selections = bad["catalog_selections"].getArray();
        auto* parameters = bad["catalog_parameter_edits"].getArray();
        auto* selected = selections->getReference(0).getDynamicObject();
        auto* parameter = parameters->getReference(0).getDynamicObject();
        switch (mutation) {
            case 0: selections->getReference(1).getDynamicObject()->setProperty("local_id", 7); break;
            case 1: parameters->add(parameters->getReference(0).clone()); break;
            case 2: parameter->setProperty("selection", 200); break;
            case 3: parameter->setProperty("unit", 1); break;
            case 4: parameter->setProperty("key", "drive.gain_db"); break;
            case 5: parameter->setProperty("normalized", 1.001); break;
            case 6: selected->setProperty("revision", "01"); break;
            case 7: selected->setProperty("revision", "18446744073709551616"); break;
            case 8: selected->setProperty("content_sha256", juce::String::repeatedString("0", 64)); break;
            case 9: selected->setProperty("design", "00000000-0000-0000-0000-000000000000"); break;
            case 10: selected->setProperty("native", true); break;
            case 11: bad.getDynamicObject()->removeProperty("catalog_parameter_edits"); break;
        }
        task12_reject_catalog_wire(bad, policy, authority, edits, mutationReasons[mutation]);
    }
    for (const auto* target : {"catalog:200:0:reverb.mix", "catalog:99:1:reverb.mix", "catalog:99:0:drive.gain_db",
                               "catalog:99:0:", "catalog:99:0:reverb.mix:extra"}) {
        auto bad = v.clone(); bad.getDynamicObject()->setProperty("macro_edits", array({macro(1, target, .5)}));
        task12_reject_catalog_wire(bad, policy, authority, edits, Reason::catalogIntent);
    }
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto bad = edits;
        if (mutation == 0) bad.catalogSelections[1].localId = 7;
        if (mutation == 1) bad.catalogParameters.push_back(bad.catalogParameters.front());
        if (mutation == 2) bad.catalogParameters.front().normalized = std::numeric_limits<float>::quiet_NaN();
        if (mutation == 3) bad.macros[0].targetId = "catalog:99:0:drive.gain_db";
        task12_reject_catalog_admission(source, policy, bad, authority);
    }
    // An ordinary physical edit cannot conflict with the lowered selected tuple.
    auto conflict = edits; conflict.parameters.push_back({registry[firstMix].id, .9f});
    task12_reject_catalog_admission(source, policy, conflict, authority);
    auto corrupt = authority; auto* bytes = static_cast<char*>(corrupt.definitions[0].canonical.getData());
    CHECK(corrupt.definitions[0].canonical.getSize() > 1); bytes[0] = bytes[0] == '{' ? ' ' : '{';
    task12_reject_catalog_admission(source, policy, edits, corrupt);
    auto mismatch = authority; mismatch.definitions[0].revision += 1;
    task12_reject_catalog_admission(source, policy, edits, mismatch);
    auto nullRoot = authority; nullRoot.root.runtime = juce::Uuid(juce::String("00000000-0000-0000-0000-000000000000"));
    task12_reject_catalog_admission(source, policy, edits, nullRoot);
    auto zeroRequest = authority; zeroRequest.root.requestId = 0;
    task12_reject_catalog_admission(source, policy, edits, zeroRequest);
    auto wrongSnapshot = authority; wrongSnapshot.snapshot = juce::Uuid();
    task12_reject_catalog_admission(source, policy, edits, wrongSnapshot);
    auto incomplete = authority; juce::var descriptor;
    CHECK(parseStrictJson(incomplete.descriptorJson, descriptor, 65536));
    descriptor.getDynamicObject()->setProperty("complete", false); incomplete.descriptorJson = json(descriptor);
    task12_reject_catalog_admission(source, policy, edits, incomplete);

    // Self-consistent canonical bytes/hash/descriptor and a current factory ID
    // still cannot confer native recall. This altered 82-default record equals
    // neither the current factory nor an ORIGINAL source embedded definition.
    auto forgedFactory = *factory; forgedFactory.units[0].defaults.clear();
    for (const auto& p : retained.units[0].defaults) forgedFactory.units[0].defaults.push_back(p);
    CHECK(validateRackDesign(forgedFactory, RackValidationMode::instanceRecall));
    CHECK(!validateRackDesign(forgedFactory, RackValidationMode::portableDesign));
    const std::array<RackDesign, 1> forgedDefinitions{forgedFactory};
    RackCatalogSnapshot forgedRoot; CHECK(captureRackCatalog(forgedDefinitions, {}, {}, forgedRoot));
    RackCatalogPage forgedPage; CHECK(catalogPage(forgedRoot, {}, 0, 65536, forgedPage));
    CHECK(forgedPage.complete && forgedRoot.entries.size() == 1);
    if (!forgedPage.complete || forgedRoot.entries.size() != 1) return;
    CatalogAuthority forgedAuthority; forgedAuthority.root = authority.root; forgedAuthority.snapshot = forgedRoot.id;
    forgedAuthority.descriptorJson = juce::JSON::toString(rackCatalogPageDescriptor(forgedPage), true, 22).toStdString();
    CatalogDefinition forged; forged.design = forgedFactory.id; forged.revision = forgedFactory.revision;
    forged.contentSha256 = forgedRoot.entries[0].contentSha256;
    CHECK(encodeRackDesign(forgedFactory, forged.canonical, RackValidationMode::instanceRecall));
    CHECK(juce::SHA256(forged.canonical).toHexString() == forged.contentSha256);
    forgedAuthority.definitions.push_back(std::move(forged));
    ProposalEdits forgedIntent; forgedIntent.catalogSelections = {{7, forgedFactory.id, forgedFactory.revision,
        forgedRoot.entries[0].contentSha256, {RackRole::fx, masterChain, 1}}};
    task12_reject_catalog_admission(source, policy, forgedIntent, forgedAuthority);
    CHECK(exact(source, original));
}

void extension_local_keys_ai_catalog_and_controls()
{
    begin(__func__);const char* longest="dynamic_eq.band.8.cut_slope_extension";
    const auto factories=airack::test::frozenFactoryRackDesigns();RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(factories,{},{},snapshot));
    const auto entry=std::find_if(snapshot.entries.begin(),snapshot.entries.end(),[](const auto& row){return row.definition.name=="Contour Dynamics";});CHECK(entry!=snapshot.entries.end());if(entry==snapshot.entries.end())return;
    RackCatalogPage page;CHECK(catalogPage(snapshot,{},0,65536,page));CHECK(page.complete);
    CatalogAuthority authority;authority.root={juce::Uuid(),1,0,0};authority.snapshot=snapshot.id;authority.descriptorJson=juce::JSON::toString(rackCatalogPageDescriptor(page),true,22).toStdString();
    CatalogDefinition definition;definition.design=entry->definition.id;definition.revision=entry->definition.revision;definition.contentSha256=entry->contentSha256;CHECK(encodeRackDesign(entry->definition,definition.canonical,RackValidationMode::instanceRecall));authority.definitions.push_back(std::move(definition));
    ProposalPolicy policy;auto value=task12_catalog_wire(array({task12_selection_json(7,*entry,0)}),array({task12_parameter_json(7,0,"dynamic_eq.band.8.shape_extension",1),task12_parameter_json(7,0,longest,1)}));
    ProposalEdits edits;const bool read=decodeProposal(json(value),policy,authority,edits);CHECK(read);if(read){
        CHECK(edits.catalogParameters.size()==2&&edits.catalogParameters[1].key==longest);ProjectDocument source,out;CHECK(admitProposal(source,policy,edits,authority,out)==ProposalStatus::accepted);
        RoutedPatch patch;CHECK(makeRoutedPatch(out.sound,patch));CHECK(patch.effects[0].dynamicEq.bands[7].shape==DynamicEqShape::flatTilt&&patch.effects[0].dynamicEq.bands[7].slope==DynamicEqSlope::db96);
    }
    auto malformed=value.clone();malformed["catalog_parameter_edits"].getArray()->getReference(1).getDynamicObject()->setProperty("key",juce::String(longest)+"x");task12_reject_catalog_wire(malformed,policy,authority,edits);
    auto fractional=value.clone();fractional["catalog_parameter_edits"].getArray()->getReference(0).getDynamicObject()->setProperty("normalized",.5);
    ProposalEdits rounded;CHECK(decodeProposal(json(fractional),policy,authority,rounded));ProjectDocument source,output;CHECK(admitProposal(source,policy,rounded,authority,output)==ProposalStatus::accepted);
    CHECK(output.sound.values[dynamicEqExtensionParameterStart+14]==2.f/3);
    malformed=value.clone();malformed["catalog_parameter_edits"].getArray()->getReference(0).getDynamicObject()->setProperty("normalized",1.01);task12_reject_catalog_wire(malformed,policy,authority,edits);
    const auto instance=juce::Uuid();
    const auto replacement=object({{"label","Slope"},{"unit",0},{"section",0},{"key",longest},{"widget","choice"},{"response","linear"},{"minimum",0},{"maximum",4}});
    value=wire2();value["reconciliation"].getDynamicObject()->setProperty("controls",array({object({{"instance",instance.toDashedString()},{"control",0},{"replacement",replacement}})}));
    CHECK(decodeProposal(json(value),policy,edits));CHECK(edits.reconciliation.controls.size()==1&&edits.reconciliation.controls[0].replacement&&edits.reconciliation.controls[0].replacement->key==longest);
    malformed=value.clone();malformed["reconciliation"]["controls"].getArray()->getReference(0)["replacement"].getDynamicObject()->setProperty("key",juce::String(longest)+"xx");auto before=edits;CHECK(!decodeProposal(json(malformed),policy,edits)&&same(edits,before));
}

// The rack instance whose unit holds the bank in d, else null.
juce::Uuid bankOwner(const SoundDocument& d, bool synth, std::uint8_t bank)
{
    if (d.personalities) for (const auto& i : d.personalities->instances) for (const auto& u : i.design.units)
        if ((u.module < NativeModule::drive) == synth) if (const auto* b = rackUnitBinding(i, u.id); b && b->slot == bank) return i.id;
    return juce::Uuid::null();
}
// Verification 2: every value an admitted proposal changes is in the candidate's reach, a synth's ENGINE matching
// its rack, or in a bank whose rack the proposal placed, swapped or removed (the placement's own reset).
bool changedOnlyInReach(const ProjectDocument& source, const ProjectDocument& output)
{
    const auto reach = userReach(output.sound);
    for (std::size_t i = 0; i < parameterCount; ++i) {
        if (source.sound.values[i] == output.sound.values[i] || reach[i]) continue;
        const juce::String id(parameterRegistry()[i].id);
        const bool synth = id.startsWith("synth.");
        if (synth || id.startsWith("effect.")) {
            const auto rest = id.fromFirstOccurrenceOf(".", false, false);
            const auto bank = std::uint8_t(rest.getIntValue() - 1);
            const auto owner = bankOwner(output.sound, synth, bank);
            if (synth && rest.fromFirstOccurrenceOf(".", false, false) == "engine" && !owner.isNull()
                && parameterPhysicalValue(output.sound.values, i) == (bankModule(output.sound, RackRole::synth, bank) == NativeModule::wavetable ? 1.f : 0.f)) continue;
            if (owner != bankOwner(source.sound, synth, bank)) continue;
        }
        std::cerr << section << ": changed outside reach " << id << '\n'; return false;
    }
    return true;
}
ProjectDocument admitted(const ProjectDocument& source, const ProposalPolicy& policy, const ProposalEdits& edits,
                         const CatalogAuthority* authority = nullptr)
{
    ProjectDocument out;
    const auto status = authority ? admitProposal(source, policy, edits, *authority, out) : admitProposal(source, policy, edits, out);
    CHECK(status == ProposalStatus::accepted);
    if (status != ProposalStatus::accepted) { std::cerr << section << ": status " << int(status) << " for " << describe(edits) << '\n'; return source; }
    CHECK(changedOnlyInReach(source, out)); return out;
}
// A value for the parameter that differs from the source's, after a choice's quantization.
float other(const ProjectDocument& source, const char* id) { return source.sound.values[index(id)] < .5f ? 1.f : 0.f; }
ProposalEdits parameter(const char* id, float normalized) { ProposalEdits e; e.parameters = {{id, normalized}}; return e; }
ProposalEdits rackEdit(RackEdit edit) { ProposalEdits e; e.racks = {edit}; return e; }
juce::Uuid place(ProjectDocument& d, const RackDesign& design, std::uint8_t position)
{
    RackOperation op; op.design = design; op.newInstance = juce::Uuid(); op.destination = {design.role, masterChain, position};
    SoundDocument next; RackDifference difference;
    CHECK(makeRackCandidate(d.sound, op, SoundEditOrigin::manual, next, difference) == RackCandidateStatus::ready);
    d.sound = next; return op.newInstance;
}
const RackDesign& factory(const char* name)
{
    static const auto designs = airack::test::frozenFactoryRackDesigns();
    const auto d = std::find_if(designs.begin(), designs.end(), [&](const auto& x) { return x.name == name; });
    CHECK(d != designs.end()); return *d;
}
// These original numeric/companion-PCM unit fixtures seal the real historic
// melodic library. The public owned inventory is covered independently.
std::vector<RackDesign> historicalMelodicLibrary()
{
    auto library=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign().id;
    CHECK(library.size()==28&&std::count_if(library.begin(),library.end(),[&](const auto& d){return d.id==tile;})==1);
    std::erase_if(library,[&](const auto& d){return d.id==tile||std::any_of(d.units.begin(),d.units.end(),[](const auto& u){return u.module==NativeModule::saturation;});});CHECK(library.size()==25);return library;
}
// Every library design's definition, captured as the plugin captures the AI's catalog.
CatalogAuthority libraryAuthority(const ProjectDocument& source, RackCatalogSnapshot& root, std::initializer_list<const char*> names,bool includeSaturation=false)
{
    std::vector<RackDesign> embedded;
    if (source.sound.personalities) for (const auto& i : source.sound.personalities->instances) embedded.push_back(i.design);
    auto library=historicalMelodicLibrary();
    if(includeSaturation){for(const auto& design:airack::test::frozenFactoryRackDesigns())if(std::any_of(design.units.begin(),design.units.end(),[](const auto& u){return u.module==NativeModule::saturation;}))library.push_back(design);CHECK(library.size()==27);}
    CHECK(captureRackCatalog(library, embedded, {}, root));
    RackCatalogPage page; CHECK(catalogPage(root, {}, 0, 65536, page));
    CatalogAuthority a; a.root = {juce::Uuid("123f46e8-87e7-43da-a752-12a012120140"), 3, 0, 0}; a.snapshot = root.id;
    a.descriptorJson = juce::JSON::toString(rackCatalogPageDescriptor(page), true, 22).toStdString();
    for (const auto* name : names) {
        const auto e = std::find_if(root.entries.begin(), root.entries.end(), [&](const auto& e) { return e.definition.name == name; });
        CHECK(e != root.entries.end()); if (e == root.entries.end()) continue;
        CatalogDefinition d; d.design = e->definition.id; d.revision = e->definition.revision; d.contentSha256 = e->contentSha256;
        RackDesign resolved; CHECK(resolveCatalogDesign(root, d.design, d.revision, d.contentSha256, resolved));
        CHECK(encodeRackDesign(resolved, d.canonical, RackValidationMode::instanceRecall)); a.definitions.push_back(std::move(d));
    }
    return a;
}
CatalogDesignSelection selection(const CatalogAuthority& a, const RackDesign& design, std::uint8_t local, std::uint8_t position)
{
    const auto d = std::find_if(a.definitions.begin(), a.definitions.end(), [&](const auto& d) { return d.design == design.id; });
    CHECK(d != a.definitions.end()); if (d == a.definitions.end()) return {};
    return {local, d->design, d->revision, d->contentSha256, {design.role, masterChain, position}};
}

// Decisions 4 and 6: the AI's parameter edits, macro targets and rack edits stay inside the user's reach.
void reach_admission()
{
    begin(__func__);
    // These independent snapshots outlive nested admission calls on a 1 MiB test stack.
    const auto copperOwner = std::make_unique<ProjectDocument>(); auto& copper = *copperOwner; copper.sound = copperArcSound(); CHECK(validateProjectDocument(copper));
    const auto copperId = copper.sound.personalities->instances[0].id;
    const auto out = ProposalStatus::outOfReach;
    // Rack edits that make or change bare banks; detaching is the user's.
    reject(copper, rackEdit({RackEditKind::addSynth, 1, masterChain, 1}), {}, out);
    reject(copper, rackEdit({RackEditKind::addEffect, 0, masterChain, 0, EffectKind::reverb}), {}, out);
    const auto bareSynthOwner = std::make_unique<ProjectDocument>(copper); auto& bareSynth = *bareSynthOwner; layout(bareSynth, {RackEditKind::addSynth, 1, masterChain, 1});
    reject(bareSynth, rackEdit({RackEditKind::removeSynth, 1}), {}, out);
    const auto bareEffectOwner = std::make_unique<ProjectDocument>(copper); auto& bareEffect = *bareEffectOwner; layout(bareEffect, {RackEditKind::addEffect, 0, masterChain, 0, EffectKind::drive});
    reject(bareEffect, rackEdit({RackEditKind::moveEffect, 0, 0, 0}), {}, out);
    reject(bareEffect, rackEdit({RackEditKind::replaceEffect, 0, masterChain, 0, EffectKind::reverb}), {}, out);
    { ProposalEdits e; e.reconciliation.detach = {copperId}; reject(copper, e, {}, ProposalStatus::scopeViolation); }
    // Raw edits outside reach: a bank that isn't present, the other engine, ENGINE, another effect kind, render quality.
    for (const auto* id : {"synth.2.analog.filter.cutoff", "synth.1.wavetable.filter.cutoff", "synth.1.engine", "output.render_quality"})
        reject(copper, parameter(id, other(copper, id)), {}, out);
    reject(bareEffect, parameter("effect.1.reverb.mix", other(bareEffect, "effect.1.reverb.mix")), {}, out);
    reject(bareSynth, parameter("synth.2.engine", other(bareSynth, "synth.2.engine")), {}, out);
    ProposalPolicy macros; macros.allowMacroChanges = true;
    for (const auto* target : {"synth.1.analog.filter.mode", "synth.2.analog.filter.cutoff", "output.render_quality"}) {
        ProposalEdits e; e.macros = {{1, target, .5f}}; reject(copper, e, macros, out);
    }
    // Inside reach: the rack's and the house's controls, an unchanged value, a matching ENGINE and a macro on a route.
    { ProposalEdits e; e.parameters = {{"synth.1.analog.filter.cutoff", .7f}, {"output.gain_db", .4f}, {"synth.1.engine", 0},
        {"output.render_quality", copper.sound.values[index("output.render_quality")]}};
      const auto d = admitted(copper, {}, e); CHECK(d.sound.values[index("synth.1.analog.filter.cutoff")] == .7f); }
    { ProposalEdits e; e.macros = {{1, "synth.1.modulation.1.amount", .5f}}; admitted(copper, macros, e); }
    // CREATE SOUND moves and removes placed racks as the user does, with no capture granting it.
    { const auto twoOwner = std::make_unique<ProjectDocument>(copper); auto& two = *twoOwner; const auto prism = place(two, factory("Prism Relay"), 1);
      // A move forks the moved rack and the rack it shifts, as the user's move does.
      RackOperation op; op.kind = RackOperationKind::move; op.selected = prism; op.destination = {RackRole::synth, masterChain, 0};
      SoundDocument moved; RackDifference difference;
      CHECK(makeRackCandidate(two.sound, op, SoundEditOrigin::manual, moved, difference) == RackCandidateStatus::ready);
      const auto m = admitted(two, {}, rackEdit({RackEditKind::moveSynth, 1, masterChain, 0}));
      CHECK(m.sound == moved && m.sound.layout.synthOrder[0] == 1);
      if (m.sound.personalities) for (const auto& i : m.sound.personalities->instances) CHECK(i.edited);
      ProposalEdits named; named.reconciliation.remove = {prism};
      CHECK(admitted(two, {}, named).sound.layout.synthUse[1] == SlotUse::retired);
      auto both = rackEdit({RackEditKind::removeSynth, 1}); both.reconciliation.remove = {prism};
      const auto d = admitted(two, {}, both);
      CHECK(d.sound.layout.synthUse[1] == SlotUse::retired && d.sound.personalities->instances.size() == 1);
      // A remove edit alone takes a bank from its rack, which the user can't do.
      reject(two, rackEdit({RackEditKind::removeSynth, 1}), {}, out);
      // Detach, replace and remapping stay captured permissions.
      ProposalEdits detach; detach.reconciliation.detach = {prism}; reject(two, detach, {}, ProposalStatus::scopeViolation);
      ProposalEdits replace; replace.reconciliation.replace = {prism}; reject(two, replace, {}, ProposalStatus::scopeViolation);
      ProposalEdits remap; remap.reconciliation.controls = {{prism, 0, {}}}; reject(two, remap, {}, ProposalStatus::scopeViolation); }
    // The user changes a placed rack's effect by swapping the rack, not its bank's kind.
    { const auto driftOwner = std::make_unique<ProjectDocument>(copper); auto& drift = *driftOwner; const auto lumen = place(drift, factory("Lumen Drift"), 0);
      const auto& instances = drift.sound.personalities->instances;
      const auto placed = std::find_if(instances.begin(), instances.end(), [&](const auto& i) { return i.id == lumen; });
      CHECK(placed != instances.end());
      if (placed != instances.end()) reject(drift, rackEdit({RackEditKind::replaceEffect, placed->bindings[0].slot, masterChain, 0, EffectKind::delay}), {}, out); }
    // The catalog path: place library racks and set their values and macros through their local keys.
    RackCatalogSnapshot root; const auto authority = libraryAuthority(copper, root, {"Copper Arc", "Lumen Drift"});
    const auto lumen = factory("Lumen Drift"), arc = factory("Copper Arc");
    { ProposalEdits e; e.catalogSelections = {selection(authority, lumen, 1, 0)}; e.catalogParameters = {{1, 0, "reverb.mix", .4f}};
      const auto d = admitted(copper, {}, e, &authority);
      CHECK(d.sound.layout.effects[0].use == SlotUse::present && d.sound.values[index("effect.1.reverb.mix")] == .4f); }
    { ProposalEdits e; e.catalogSelections = {selection(authority, arc, 2, 1)}; e.catalogParameters = {{2, 0, "modulation.1.amount", .6f}};
      e.macros = {{2, "catalog:2:0:modulation.1.amount", .5f}};
      const auto d = admitted(copper, macros, e, &authority);
      CHECK(d.sound.layout.synthUse[1] == SlotUse::present && d.sound.values[index("synth.2.modulation.1.amount")] == .6f
          && d.sound.macros[1].targetKey == parameterRegistry()[index("synth.2.modulation.1.amount")].stateKey); }
    // A swap: remove Copper Arc and select Prism Relay at its place, uncaptured.
    { RackCatalogSnapshot prismRoot; const auto prismAuthority = libraryAuthority(copper, prismRoot, {"Prism Relay"});
      ProposalEdits e; e.reconciliation.remove = {copperId}; e.catalogSelections = {selection(prismAuthority, factory("Prism Relay"), 1, 0)};
      const auto d = admitted(copper, {}, e, &prismAuthority);
      CHECK(d.sound.personalities && d.sound.personalities->instances.size() == 1 && d.sound.personalities->instances[0].design.name == "Prism Relay"
          && d.sound.layout.synthCount == 1 && d.sound.layout.synthUse[0] == SlotUse::retired); }
    { ProposalEdits e; e.catalogSelections = {selection(authority, lumen, 1, 0)}; e.racks = {{RackEditKind::addEffect, 1, masterChain, 1, EffectKind::drive}};
      task12_reject_catalog_admission(copper, {}, e, authority, out); }
    { ProposalEdits e; e.catalogSelections = {selection(authority, arc, 2, 1)}; e.catalogParameters = {{2, 0, "engine", 1}};
      task12_reject_catalog_admission(copper, {}, e, authority, out); }
    { ProposalEdits e; e.catalogSelections = {selection(authority, arc, 2, 1)}; e.macros = {{1, "catalog:2:0:analog.filter.mode", .5f}};
      task12_reject_catalog_admission(copper, macros, e, authority, out); }
}

// Night Dial reaches the AI only through the library's catalog: placed by a selection, its warmth keys edited through
// the selection's unit, and nothing outside that unit's keys.
void night_dial_catalog_reach()
{
    begin(__func__);
    const auto copperOwner = std::make_unique<ProjectDocument>(); auto& copper = *copperOwner; copper.sound = copperArcSound();
    const auto night = airack::test::frozenExampleRackDesigns()[5]; CHECK(night.name == "Night Dial");
    RackCatalogSnapshot root; const auto authority = libraryAuthority(copper, root, {"Night Dial"});
    CHECK(authority.descriptorJson.find("\"Night Dial\"") != std::string::npos);
    ProposalEdits e; e.catalogSelections = {selection(authority, night, 1, 1)};
    e.catalogParameters = {{1, 0, "wavetable.drift", .6f}, {1, 0, "wavetable.filter.type", 1.f}};
    const auto d = admitted(copper, {}, e, &authority);
    CHECK(d.sound.personalities && d.sound.personalities->instances.size() == 2 && d.sound.personalities->instances.back().design.name == "Night Dial");
    CHECK(d.sound.layout.synthUse[1] == SlotUse::present && d.sound.compatibility.synth[1][1] == modernSynthAlgorithmRevision);
    CHECK(d.sound.values[index("synth.2.wavetable.drift")] == .6f && d.sound.values[index("synth.2.wavetable.filter.type")] == 1.f);
    ProposalEdits global; global.catalogSelections = e.catalogSelections; global.catalogParameters = {{1, 0, "global.master_level", .3f}};
    task12_reject_catalog_admission(copper, {}, global, authority);
}

// Small Hours reaches the AI only through the library's catalog: placed by a selection, its Era Reverb keys edited
// through the selection's unit, and nothing outside that unit's keys.
void small_hours_catalog_reach()
{
    begin(__func__);
    const auto copperOwner = std::make_unique<ProjectDocument>(); auto& copper = *copperOwner; copper.sound = copperArcSound();
    const auto small = airack::test::frozenExampleRackDesigns()[4]; CHECK(small.name == "Small Hours");
    RackCatalogSnapshot root; const auto authority = libraryAuthority(copper, root, {"Small Hours"});
    CHECK(authority.descriptorJson.find("\"Small Hours\"") != std::string::npos);
    ProposalEdits e; e.catalogSelections = {selection(authority, small, 1, 0)};
    e.catalogParameters = {{1, 0, "era_reverb.decay_seconds", .6f}, {1, 0, "era_reverb.age", .5f}};
    const auto d = admitted(copper, {}, e, &authority);
    CHECK(d.sound.personalities && d.sound.personalities->instances.size() == 2 && d.sound.personalities->instances.back().design.name == "Small Hours");
    CHECK(d.sound.layout.effects[0].use == SlotUse::present && d.sound.layout.effects[0].kind == EffectKind::eraReverb);
    CHECK(d.sound.values[index("effect.1.era_reverb.decay_seconds")] == .6f && d.sound.values[index("effect.1.era_reverb.age")] == .5f);
    ProposalEdits global; global.catalogSelections = e.catalogSelections; global.catalogParameters = {{1, 0, "global.master_level", .3f}};
    task12_reject_catalog_admission(copper, {}, global, authority);
}

// Decision 7: the AI is told the source's reach, each module's keys and the user's library.
void provider_catalog_reach()
{
    begin(__func__);
    ProjectDocument copper; copper.sound = copperArcSound();
    copper.sound.macros[2] = {parameterRegistry()[index("synth.2.analog.filter.cutoff")].stateKey, .25f};
    auto space = copper; place(space, factory("Lumen Drift"), 0);
    ProjectDocument bare;
    for (int n = 0; n < 3; ++n) { RackEdit e; e.kind = RackEditKind::addSynth; layout(bare, e); }
    for (int k = 0; k < 8; ++k) { RackEdit e; e.kind = RackEditKind::addEffect; e.effect = EffectKind(k); layout(bare, e); }
    const auto library = historicalMelodicLibrary();
    for (const auto* d : {&copper, &space, &bare}) {
        std::vector<RackDesign> embedded;if(d->sound.personalities)for(const auto& rack:d->sound.personalities->instances)embedded.push_back(rack.design);
        RackCatalogSnapshot captured;juce::var c; CHECK(validateProjectDocument(*d) && captureRackCatalog(library,embedded,{},captured) && providerCatalogFor(*d,captured,c)); if (!c.isObject()) continue;
        const auto reach = userReach(d->sound); juce::StringArray ids;
        for (std::size_t i = 0; i < parameterCount; ++i) if (reach[i]) ids.add(parameterRegistry()[i].id);
        const auto* rows = c["controls"].getArray(); CHECK(rows && rows->size() == ids.size());
        if (rows && rows->size() == ids.size()) for (int r = 0; r < rows->size(); ++r) CHECK((*rows)[r][0].toString() == ids[r]);
        CHECK(int(c["limits"]["parameters"]) == int(parameterCount));
        CHECK(json(c["module_key_columns"]) == json(array({"key", "unit", "kind", "min", "max", "default", "skew", "choice_set"})));
        const auto* names = c["projection_enums"]["native_module"].getArray(); const auto* sets = c["choice_sets"].getArray();
        CHECK(names && names->size() == 22 && sets && c["module_keys"].getDynamicObject() && c["module_keys"].getDynamicObject()->getProperties().size() == 21);
        if (names && sets) for (int m = 0; m < names->size(); ++m) {
            if((*names)[m].isVoid()){CHECK(m==int(NativeModule::drum));CHECK(!c["module_keys"].hasProperty("drum"));continue;}
            const auto module = NativeModule(m); const juce::String prefix(module < NativeModule::drive ? "synth.1." : "effect.1.");
            const auto expected = unitReach(module, 0); const auto* keys = c["module_keys"][juce::Identifier((*names)[m].toString())].getArray();
            CHECK(keys && keys->size() == int(expected.size())); if (!keys || keys->size() != int(expected.size())) continue;
            for (int k = 0; k < keys->size(); ++k) {
                const auto& p = parameterRegistry()[expected[std::size_t(k)]]; const auto& row = (*keys)[k];
                CHECK(row.size() == 8 && prefix + row[0].toString() == p.id && row[1].toString() == p.unit && int(row[2]) == int(p.kind));
                CHECK(double(row[3]) == p.minimum && double(row[4]) == p.maximum && double(row[5]) == p.defaultValue && double(row[6]) == p.skew);
                const auto set = int(row[7]); CHECK(set >= 0 && set < sets->size());
                if (set >= 0 && set < sets->size()) CHECK((*sets)[set].size() == int(p.choices.size()));
            }
        }
        const auto* entries = c["rack_catalog"]["entries"].getArray(); CHECK(entries);
        if (entries) for (const auto& design : library) CHECK(std::any_of(entries->begin(), entries->end(), [&](const auto& e) { return e["id"].toString() == design.id.toString(); }));
        CHECK(juce::JSON::toString(c, true, 17).getNumBytesAsUTF8() <= 196608);
    }
    juce::var c; CHECK(providerCatalogFor(copper, c));
    const auto* macro = c["macro_bindings"].getArray();
    CHECK(macro && macro->size() == 8 && (*macro)[2][1].toString() == "synth.2.analog.filter.cutoff" && (*macro)[0][1].isVoid());
}

std::string catalogBytes(const juce::var& value) { return juce::JSON::toString(value, true, 22).toStdString(); }
juce::String catalogHash(const std::string& bytes) { return juce::SHA256(bytes.data(), bytes.size()).toHexString(); }
juce::var catalogSentinel() { return object({{"nested", object({{"kept", array({7, "sentinel"})}})}}); }
std::string catalogProjectBytes(const ProjectDocument& source)
{
    juce::MemoryBlock bytes; const bool encoded = encodeProjectState(source, bytes); CHECK(encoded); if (!encoded) return {};
    return {static_cast<const char*>(bytes.getData()), bytes.getSize()};
}
std::string catalogSnapshotBytes(const RackCatalogSnapshot& snapshot)
{
    juce::Array<juce::var> entries;
    for (const auto& entry : snapshot.entries) {
        juce::MemoryBlock canonical; CHECK(encodeRackDesign(entry.definition, canonical, RackValidationMode::instanceRecall));
        entries.add(object({{"hash", entry.contentSha256}, {"canonical_base64", canonical.toBase64Encoding()}}));
    }
    RackCatalogPage admission;
    return catalogBytes(object({{"id", snapshot.id.toString()}, {"entries", entries},
        {"seal_admitted", catalogPage(snapshot, {}, snapshot.entries.size(), 65536, admission)}}));
}
RackCatalogSnapshot catalogSnapshot(const ProjectDocument& source, bool empty = false, bool custom = false)
{
    std::vector<RackDesign> embedded;
    if (!empty && source.sound.personalities)
        for (const auto& instance : source.sound.personalities->instances) embedded.push_back(instance.design);
    if (custom) {
        auto design = nativeRecipe(); design.id = juce::Uuid("70100000-0000-4000-8000-000000000099");
        design.revision = UINT64_MAX; design.name = "Fixed custom inventory"; embedded.push_back(design);
    }
    const auto library = empty ? std::vector<RackDesign>{} : libraryRackDesigns();
    RackCatalogSnapshot result; CHECK(captureRackCatalog(library, embedded, {}, result)); return result;
}
void catalogMutateOutput(juce::var& output)
{
    // Only mutate returned JSON. A copied var intentionally aliases it; another call must not.
    CHECK(output["limits"].isObject() && output["wire_guidance"].isObject());
    if (!output["limits"].isObject() || !output["wire_guidance"].isObject()) return;
    output["limits"].getDynamicObject()->setProperty("parameters", -1);
    output["wire_guidance"].getDynamicObject()->setProperty("target_id", "mutated");
    auto* orders = output["racks"]["effect_order"].getArray(); CHECK(orders && !orders->isEmpty());
    if (orders && !orders->isEmpty()) { auto* order = orders->getReference(0).getArray(); CHECK(order); if (order) order->add(-1); }
    auto* synthOrder = output["racks"]["synth_order"].getArray(); CHECK(synthOrder); if (synthOrder) synthOrder->add(-1);
    for (const auto* name : {"controls", "choice_sets"}) {
        auto* rows = output[name].getArray(); CHECK(rows && !rows->isEmpty());
        if (rows && !rows->isEmpty()) { auto* row = rows->getReference(0).getArray(); CHECK(row); if (row) row->add("mutated"); }
    }
    auto* keys = output["module_keys"].getDynamicObject(); CHECK(keys && keys->getProperties().size());
    if (keys && keys->getProperties().size()) {
        auto* rows = keys->getProperties().getValueAt(0).getArray(); CHECK(rows && !rows->isEmpty());
        if (rows && !rows->isEmpty()) { auto* row = rows->getReference(0).getArray(); CHECK(row); if (row) row->add("mutated"); }
    }
    auto* inventory = output["rack_catalog"]["entries"].getArray(); CHECK(inventory && !inventory->isEmpty());
    if (inventory && !inventory->isEmpty()) {
        auto* entry = inventory->getReference(0).getDynamicObject(); CHECK(entry); if (entry) entry->setProperty("name", "mutated");
    }
}

void provider_catalog_ownership_corpus(const juce::File& outputRoot = {}, const juce::File& baselineRoot = {})
{
    begin(__func__);
    struct Input { const char* id; std::unique_ptr<ProjectDocument> document; std::unique_ptr<RackCatalogSnapshot> snapshot; bool explicitSnapshot; };
    std::vector<Input> inputs;
    auto add = [&](const char* id, std::unique_ptr<ProjectDocument> document, bool explicitSnapshot = false, bool empty = false, bool custom = false) {
        CHECK(validateProjectDocument(*document)); auto snapshot = airack::testing::fixtureValue([&] { return catalogSnapshot(*document, empty, custom); });
        inputs.push_back({id, std::move(document), std::move(snapshot), explicitSnapshot});
    };
    add("default", std::make_unique<ProjectDocument>());
    auto detailed = airack::testing::fixtureValue([] { return rich(); }); detailed->sound.macros[0] = {}; detailed->sound.synthLocks[1] = true;
    add("rich", std::move(detailed));
    auto empty = std::make_unique<ProjectDocument>();
    empty->sound = std::move(*airack::testing::fixtureValue([] { return newInstanceSound(); })); add("empty", std::move(empty));
    constexpr EffectKind effects[]{EffectKind::drive, EffectKind::delay, EffectKind::reverb, EffectKind::compressor,
        EffectKind::eq, EffectKind::filter, EffectKind::chorus, EffectKind::phaser, EffectKind::flanger,
        EffectKind::stereo, EffectKind::dimension, EffectKind::frequencyShift, EffectKind::convolution,
        EffectKind::frequencySplit, EffectKind::midSideSplit, EffectKind::dynamicEq,
        EffectKind::spaceReverb, EffectKind::eraReverb, EffectKind::saturation};
    static_assert(std::size(effects) == std::size_t(EffectKind::saturation) + 1);
    for (int group = 0; group < 3; ++group) {
        auto mixed = std::make_unique<ProjectDocument>(); layout(*mixed, {RackEditKind::addSynth, 1, masterChain, 0});
        set(*mixed, "synth.2.engine", 1); set(*mixed, "synth.2.enabled", 0);
        mixed->sound.controlLocks[index("synth.1.analog.filter.cutoff")] = true; mixed->sound.effectLocks[1] = true;
        for (int slot = 0; slot < 8 && group * 8 + slot < int(std::size(effects)); ++slot)
            layout(*mixed, {RackEditKind::addEffect, std::uint8_t(slot), masterChain, 0, effects[group * 8 + slot]});
        add(group == 0 ? "effects-0-7" : group == 1 ? "effects-8-15" : "effects-16-18", std::move(mixed));
    }
    auto drum = std::make_unique<ProjectDocument>(); layout(*drum, {RackEditKind::addSynth, 1, masterChain, 1});
    layout(*drum, {RackEditKind::addSynth, 2, masterChain, 2}); set(*drum, "synth.2.engine", 1);
    set(*drum, "synth.2.enabled", 0); drum->sound.values[drumModeParameterStart + 2] = 1;
    CHECK(sourceModule(drum->sound, 0) == NativeModule::analog && sourceModule(drum->sound, 1) == NativeModule::wavetable
        && sourceModule(drum->sound, 2) == NativeModule::drum); add("drum", std::move(drum));
    auto embedded = airack::testing::fixtureValue([] { return personalityProject(); });
    CHECK(embedded->sound.personalities); if (!embedded->sound.personalities) return;
    auto personalities = std::make_shared<RackPersonalityState>(*embedded->sound.personalities);
    CHECK(personalities->instances.size() == 2);
    for (std::size_t n = 0; n < personalities->instances.size(); ++n) {
        auto& instance = personalities->instances[n];
        instance.id = juce::Uuid(n == 0 ? "70100000-0000-4000-8000-000000000001" : "70100000-0000-4000-8000-000000000002");
        instance.design.id = juce::Uuid(n == 0 ? "70100000-0000-4000-8000-000000000011" : "70100000-0000-4000-8000-000000000012");
        instance.design.revision = 4294967297ULL;
    }
    embedded->sound.personalities = std::move(personalities); add("embedded", std::make_unique<ProjectDocument>(*embedded));
    add("custom", std::make_unique<ProjectDocument>(*embedded), true, false, true); add("sealed-empty", std::make_unique<ProjectDocument>(), true, true);
    CHECK(inputs.size() == 10);

    juce::Array<juce::var> inputRecords, outcomes;
    std::vector<std::pair<juce::String, std::string>> files;
    for (const auto& input : inputs) {
        const auto& source = *input.document; const auto before = std::make_unique<ProjectDocument>(source);
        const auto saved = catalogProjectBytes(source), snapshotSaved = catalogSnapshotBytes(*input.snapshot);
        juce::MemoryBlock state(saved.data(), saved.size());
        inputRecords.add(object({{"case", input.id}, {"project_base64", state.toBase64Encoding()},
            {"snapshot", juce::String::fromUTF8(snapshotSaved.data(), int(snapshotSaved.size()))}, {"explicit_snapshot", input.explicitSnapshot}}));
        for (const auto rendering : {CatalogRendering::companionPcm, CatalogRendering::ownedLocal}) {
            const auto id = juce::String(input.id) + (rendering == CatalogRendering::companionPcm ? "-pcm" : "-owned");
            const bool expected = std::string_view(input.id) != "drum" || rendering == CatalogRendering::ownedLocal;
            const auto call = [&](juce::var& out) { return input.explicitSnapshot ? providerCatalogFor(source, *input.snapshot, rendering, out)
                : providerCatalogFor(source, rendering, out); };
            auto output = catalogSentinel(); const auto alias = output; const auto sentinel = catalogBytes(output);
            const bool success = call(output); CHECK(success == expected);
            const auto bytes = catalogBytes(output); const auto hash = catalogHash(bytes);
            CHECK(catalogBytes(alias) == sentinel);
            if (!success) CHECK(bytes == sentinel);
            CHECK(exact(source, *before) && catalogProjectBytes(source) == saved && catalogSnapshotBytes(*input.snapshot) == snapshotSaved);
            outcomes.add(object({{"case", id}, {"rendering", int(rendering)}, {"overload", input.explicitSnapshot ? "explicit_snapshot_rendering" : "implicit_snapshot_rendering"},
                {"expected", expected}, {"success", success}, {"bytes", int(bytes.size())}, {"sha256", hash}}));
            files.emplace_back(id + ".json", bytes);
            std::cout << "PROVIDER_CATALOG " << id << " success=" << success << " bytes=" << bytes.size() << " sha256=" << hash << '\n';
            const bool ownership = (std::string_view(input.id) == "default" && rendering == CatalogRendering::companionPcm)
                || (std::string_view(input.id) == "rich" && rendering == CatalogRendering::ownedLocal)
                || (std::string_view(input.id) == "drum" && rendering == CatalogRendering::ownedLocal)
                || (std::string_view(input.id) == "custom" && rendering == CatalogRendering::companionPcm);
            if (success && ownership) {
                juce::var independent; CHECK(call(independent)); CHECK(catalogBytes(independent) == bytes);
                const auto sharedCopy = output; catalogMutateOutput(output);
                CHECK(catalogBytes(output) != bytes && catalogBytes(sharedCopy) == catalogBytes(output));
                CHECK(catalogBytes(independent) == bytes); juce::var regenerated; CHECK(call(regenerated)); CHECK(catalogBytes(regenerated) == bytes);
                CHECK(exact(source, *before) && catalogProjectBytes(source) == saved && catalogSnapshotBytes(*input.snapshot) == snapshotSaved);
            }
        }
    }
    CHECK(outcomes.size() == 20);
    const auto& defaults = inputs.front();
    const auto defaultSaved = catalogProjectBytes(*defaults.document), defaultSnapshotSaved = catalogSnapshotBytes(*defaults.snapshot);
    juce::var implicit, rendering, snapshot, both;
    CHECK(providerCatalogFor(*defaults.document, implicit));
    CHECK(providerCatalogFor(*defaults.document, CatalogRendering::companionPcm, rendering));
    CHECK(providerCatalogFor(*defaults.document, *defaults.snapshot, snapshot));
    CHECK(providerCatalogFor(*defaults.document, *defaults.snapshot, CatalogRendering::companionPcm, both));
    CHECK(catalogBytes(implicit) == catalogBytes(rendering) && catalogBytes(implicit) == catalogBytes(snapshot) && catalogBytes(implicit) == catalogBytes(both));
    CHECK(catalogProjectBytes(*defaults.document) == defaultSaved && catalogSnapshotBytes(*defaults.snapshot) == defaultSnapshotSaved);

    auto reject = [&](const ProjectDocument& source, const RackCatalogSnapshot& captured, CatalogRendering mode) {
        // Invalid projects cannot be encoded; deep typed copies still check every input field and value bit.
        auto before = std::make_unique<ProjectDocument>(source);
        if (source.sound.personalities) before->sound.personalities = std::make_shared<RackPersonalityState>(*source.sound.personalities);
        const auto snapshotSaved = catalogSnapshotBytes(captured);
        auto out = catalogSentinel(); const auto alias = out; const auto saved = catalogBytes(out);
        CHECK(!providerCatalogFor(source, captured, mode, out)); CHECK(catalogBytes(out) == saved && catalogBytes(alias) == saved);
        CHECK(exact(source, *before) && catalogSnapshotBytes(captured) == snapshotSaved);
    };
    reject(*defaults.document, *defaults.snapshot, CatalogRendering(255));
    auto invalid = std::make_unique<ProjectDocument>(*defaults.document); invalid->sound.values[0] = 2; reject(*invalid, *defaults.snapshot, CatalogRendering::ownedLocal);
    *invalid = *defaults.document; invalid->sound.layout.synthCount = 5; reject(*invalid, *defaults.snapshot, CatalogRendering::ownedLocal);
    reject(*inputs[6].document, *inputs[6].snapshot, CatalogRendering::companionPcm);
    auto forged = std::make_unique<RackCatalogSnapshot>(*defaults.snapshot); forged->id = juce::Uuid("70100000-0000-4000-8000-000000000088");
    reject(*defaults.document, *forged, CatalogRendering::ownedLocal);
    *forged = *defaults.snapshot; CHECK(!forged->entries.empty());
    if (!forged->entries.empty()) {
        forged->entries.front().contentSha256 = juce::String::repeatedString("0", 64); reject(*defaults.document, *forged, CatalogRendering::ownedLocal);
        *forged = *defaults.snapshot; forged->entries.front().definition.name = "Altered canonical definition";
        // This is rejected by inventory validation after the private compact graph was constructed and mutated.
        reject(*defaults.document, *forged, CatalogRendering::ownedLocal);
    }

    if (outputRoot == juce::File()) return;
    const auto sourceFile = juce::File(juce::String(__FILE__)); juce::MemoryBlock fixture;
    CHECK(sourceFile.loadFileAsData(fixture));
    const auto inputManifest = catalogBytes(object({{"fixture_sha256", juce::SHA256(fixture).toHexString()}, {"inputs", inputRecords}}));
    const auto outcomeManifest = catalogBytes(object({{"outcomes", outcomes}}));
    // Compare full inputs before any output comparison; changed identities/seals/order refuse equivalence.
    if (baselineRoot != juce::File()) {
        CHECK(baselineRoot.getChildFile("inputs.json").existsAsFile());
        const bool identicalInputs = baselineRoot.getChildFile("inputs.json").loadFileAsString().toStdString() == inputManifest;
        CHECK(identicalInputs); if (!identicalInputs) return;
        CHECK(baselineRoot.getChildFile("outcomes.json").loadFileAsString().toStdString() == outcomeManifest);
        for (const auto& [name, bytes] : files) {
            const auto file = baselineRoot.getChildFile(name); CHECK(file.existsAsFile());
            CHECK(file.loadFileAsString().toStdString() == bytes);
        }
    }
    auto write = [&](const juce::String& name, const std::string& bytes) {
        const auto file = outputRoot.getChildFile(name); CHECK(bytes.size() <= 4 * 1024 * 1024 && !file.exists());
        if (bytes.size() > 4 * 1024 * 1024 || file.exists()) return;
        std::ofstream stream(std::filesystem::path(file.getFullPathName().toWideCharPointer()), std::ios::binary);
        stream.write(bytes.data(), std::streamsize(bytes.size())); stream.close(); CHECK(!stream.fail());
    };
    write("inputs.json", inputManifest); write("outcomes.json", outcomeManifest);
    for (const auto& [name, bytes] : files) write(name, bytes);
}

void band_color_catalog_admission()
{
    begin(__func__);ProjectDocument source;const auto before=source;RackCatalogSnapshot snapshot;
    for(auto rendering:{CatalogRendering::companionPcm,CatalogRendering::ownedLocal}){
        juce::var catalog;CHECK(providerCatalogFor(source,rendering,catalog));
        CHECK(catalog["module_keys"]["saturation"].size()==36);
        CHECK(catalog["projection_enums"]["native_module"][21].toString()=="saturation");
        CHECK((rendering==CatalogRendering::ownedLocal)==catalog["module_keys"].hasProperty("drum"));
    }
    const auto library=airack::test::frozenLibraryRackDesigns();const auto found=std::find_if(library.begin(),library.end(),[](const auto& d){return d.id==juce::Uuid("91387715-13d3-4c57-a479-12251842bf02");});
    CHECK(found!=library.end());if(found==library.end())return;
    const auto authority=libraryAuthority(source,snapshot,{"Band Color"},true);CHECK(ai::detail::catalogAuthorityValid(authority));
    ProposalEdits edits;edits.catalogSelections={selection(authority,*found,1,0)};
    const auto placed=admitted(source,{},edits,&authority);CHECK(source==before);
    CHECK(placed.sound.personalities&&placed.sound.personalities->instances.size()==1);if(!placed.sound.personalities||placed.sound.personalities->instances.empty())return;
    CHECK(placed.sound.personalities->instances[0].design==*found);
    CHECK(found->units[0].defaults.size()==36);
    for(const auto& value:found->units[0].defaults)edits.catalogParameters.push_back({1,0,value.key,value.normalized==0?1.f:0.f});
    const auto edited=admitted(source,{},edits,&authority);
    for(const auto& value:edits.catalogParameters){const auto i=rackParameterIndex(NativeModule::saturation,0,value.key);CHECK(i&&edited.sound.values[*i]==value.normalized);}
    RoutedPatch patch;CHECK(makeRoutedPatch(edited.sound,patch)&&!patch.effects[0].bypassed);
    CHECK(source==before);
}

// The assignment journal is a native admission result, never a provider/schema field.
RackDesign journalSource(NativeModule module=NativeModule::analog)
{
    RackDesign design; design.name="Journal Source"; design.panel.sections={"Tone"};
    design.units={{0,module,{},legacyAlgorithmRevision}}; return design;
}
RackOperation journalPlace(std::uint8_t bank, std::uint8_t position=0)
{
    RackOperation op; op.design=journalSource(); op.newInstance=juce::Uuid();
    op.allocation={{0,bank}}; op.allowSlotReuse=true; op.destination={RackRole::synth,masterChain,position}; return op;
}
void journalFullBank(const SoundDocument& a, unsigned from, const SoundDocument& b, unsigned to)
{
    const auto prefix="synth."+juce::String(from+1)+".", target="synth."+juce::String(to+1)+".";
    std::size_t count=0;
    for(std::size_t n=0;n<parameterCount;++n) if(parameterRegistry()[n].id.startsWith(prefix)) {
        const auto destination=index(target+parameterRegistry()[n].id.substring(prefix.length()));
        CHECK(a.values[n]==b.values[destination]); ++count;
    }
    CHECK(count==294); CHECK(a.compatibility.synth[from]==b.compatibility.synth[to]);
}
void source_assignment_journal_typed_primitives()
{
    begin(__func__);
    auto source=std::make_unique<ProjectDocument>(); auto out=std::make_unique<SoundDocument>();
    auto trace=std::make_unique<SourceAssignmentTrace>(); RackDifference diff;
    CHECK(trace->effects.size()==608);
    auto placeNew=journalPlace(1,1);
    CHECK(makeRackCandidate(source->sound,placeNew,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==1); const auto placed=trace->effects[0];
    CHECK(placed.kind==SourceAssignmentEffectKind::clear && placed.bank==1 && placed.operation==0);
    CHECK(placed.beforeUse==SlotUse::neverUsed && placed.afterUse==SlotUse::present);
    CHECK(placed.beforeOwner.isNull() && placed.afterOwner==placeNew.newInstance);
    CHECK(placed.invalidateTarget && placed.fullNumericBank && placed.freshLogicalOwner && placed.copyFrom==noSlot);
    auto fresh=std::make_unique<SoundDocument>(*out);
    layout(*source,{RackEditKind::addSynth,1,masterChain,1});
    layout(*source,{RackEditKind::removeSynth,1});
    source->sound.values=defaultParameterValues(); source->sound.values[index("synth.2.drum.pad.1.trim_start")]=.75f;
    trace=std::make_unique<SourceAssignmentTrace>(); auto reuse=journalPlace(1,1);
    CHECK(makeRackCandidate(source->sound,reuse,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==1 && trace->effects[0].beforeUse==SlotUse::retired && trace->effects[0].freshLogicalOwner);
    CHECK(trace->effects[0].kind==SourceAssignmentEffectKind::clear && trace->effects[0].fullNumericBank);
    journalFullBank(*fresh,1,*out,1);
    // Duplicate resolves the entry source and copies the entire physical bank, including inactive controls.
    source->sound=*out; source->sound.values[index("synth.2.analog.filter.cutoff")]=.31f;
    source->sound.values[index("synth.2.drum.pad.1.trim_start")]=.63f;
    RackOperation duplicate; duplicate.kind=RackOperationKind::duplicate; duplicate.selected=reuse.newInstance;
    duplicate.newInstance=juce::Uuid(); duplicate.allocation={{0,3}}; duplicate.destination={RackRole::synth,masterChain,2};
    trace=std::make_unique<SourceAssignmentTrace>();
    CHECK(makeRackCandidate(source->sound,duplicate,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==1); const auto copied=trace->effects[0];
    CHECK(copied.kind==SourceAssignmentEffectKind::copy && copied.copyFrom==1 && copied.bank==3);
    CHECK(copied.beforeOwner.isNull() && copied.afterOwner==duplicate.newInstance && copied.fullNumericBank && copied.freshLogicalOwner);
    journalFullBank(source->sound,1,*out,3);
    // Adopt binds an existing generic bank without resetting its assignment or numeric state.
    auto generic=std::make_unique<ProjectDocument>(); RackOperation adopt=journalPlace(0); adopt.kind=RackOperationKind::adopt;
    trace=std::make_unique<SourceAssignmentTrace>(); CHECK(makeRackCandidate(generic->sound,adopt,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==1 && trace->effects[0].kind==SourceAssignmentEffectKind::preserve);
    CHECK(!trace->effects[0].fullNumericBank && trace->effects[0].freshLogicalOwner && trace->effects[0].invalidateTarget);
    CHECK(trace->effects[0].beforeOwner.isNull() && trace->effects[0].afterOwner==adopt.newInstance);
    generic->sound=*out; RackOperation swap; swap.kind=RackOperationKind::swap; swap.selected=adopt.newInstance;
    swap.design=journalSource(NativeModule::wavetable); swap.allowSlotReuse=true;
    trace=std::make_unique<SourceAssignmentTrace>(); CHECK(makeRackCandidate(generic->sound,swap,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==2);
    CHECK(trace->effects[0].operation==0 && trace->effects[1].operation==0);
    CHECK(trace->effects[0].beforeOwner==adopt.newInstance && trace->effects[0].afterOwner.isNull());
    CHECK(trace->effects[1].beforeOwner.isNull() && trace->effects[1].afterOwner==adopt.newInstance);
    for(unsigned n=0;n<trace->count;++n) CHECK(trace->effects[n].kind==SourceAssignmentEffectKind::preserve
        && trace->effects[n].invalidateTarget && !trace->effects[n].fullNumericBank && !trace->effects[n].freshLogicalOwner);
    for(bool detach:{false,true}) {
        RackOperation remove; remove.kind=detach?RackOperationKind::detach:RackOperationKind::remove; remove.selected=adopt.newInstance;
        trace=std::make_unique<SourceAssignmentTrace>(); CHECK(makeRackCandidate(generic->sound,remove,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
        CHECK(trace->count==1); const auto& effect=trace->effects[0];
        CHECK(effect.kind==SourceAssignmentEffectKind::preserve && effect.beforeOwner==adopt.newInstance && effect.afterOwner.isNull());
        CHECK(effect.afterUse==(detach?SlotUse::present:SlotUse::retired));
        CHECK(effect.invalidateTarget && !effect.fullNumericBank && !effect.freshLogicalOwner);
    }
    RackOperation replace=journalPlace(0); replace.choices.remove={adopt.newInstance};
    trace=std::make_unique<SourceAssignmentTrace>(); CHECK(makeRackCandidate(generic->sound,replace,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==2 && trace->effects[0].operation==0 && trace->effects[1].operation==0);
    CHECK(trace->effects[0].afterUse==SlotUse::retired && trace->effects[1].beforeUse==SlotUse::retired);
    CHECK(trace->effects[0].kind==SourceAssignmentEffectKind::preserve && trace->effects[1].kind==SourceAssignmentEffectKind::clear);
    CHECK(trace->effects[1].afterOwner==replace.newInstance && trace->effects[1].freshLogicalOwner);
    RackOperation detachReuse=journalPlace(0); detachReuse.choices.detach={adopt.newInstance};
    trace=std::make_unique<SourceAssignmentTrace>();
    CHECK(makeRackCandidate(generic->sound,detachReuse,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==1 && trace->effects[0].beforeOwner==adopt.newInstance && trace->effects[0].afterOwner==detachReuse.newInstance);
    CHECK(trace->effects[0].kind==SourceAssignmentEffectKind::clear && trace->effects[0].freshLogicalOwner);
    // A later choice must not append an obsolete Uâ†’null effect over the freshly installed V.
    CHECK(out->personalities && out->personalities->instances.size()==1 && out->personalities->instances.front().id==detachReuse.newInstance);
    layout(*generic,{RackEditKind::addSynth,1,masterChain,1});
    RackOperation move; move.kind=RackOperationKind::move; move.selected=adopt.newInstance; move.destination={RackRole::synth,masterChain,1};
    trace=std::make_unique<SourceAssignmentTrace>();
    CHECK(makeRackCandidate(generic->sound,move,SoundEditOrigin::manual,*out,diff,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==1 && trace->effects[0].kind==SourceAssignmentEffectKind::preserve && !trace->effects[0].invalidateTarget);
    CHECK(trace->effects[0].beforeOwner==adopt.newInstance && trace->effects[0].afterOwner==adopt.newInstance
        && !trace->effects[0].fullNumericBank && !trace->effects[0].freshLogicalOwner);
    auto untouched=std::make_unique<SoundDocument>(generic->sound); auto prior=std::make_unique<SourceAssignmentTrace>(*trace);
    generic->sound.synthLocks[0]=true; *untouched=generic->sound; diff.sonic="locked sentinel";
    CHECK(makeRackCandidate(generic->sound,move,SoundEditOrigin::aiCandidate,*untouched,diff,trace.get())==RackCandidateStatus::lockedDependency);
    CHECK(*untouched==generic->sound && diff.sonic=="locked sentinel" && *trace==*prior);
}
void source_assignment_journal_admission_and_capacity()
{
    begin(__func__);
    auto source=std::make_unique<ProjectDocument>(personalityProject()); auto output=std::make_unique<ProjectDocument>();
    auto sentinel=std::make_unique<ProjectDocument>(*output); auto trace=std::make_unique<SourceAssignmentTrace>();
    trace->count=1; trace->effects[0].operation=8; trace->effects[0].bank=3;
    const auto seed=std::make_unique<SourceAssignmentTrace>(*trace);
    ProposalPolicy policy; policy.allowSlotReuse=true;
    ProposalEdits raw; raw.racks={{RackEditKind::moveSynth,0,masterChain,1},{RackEditKind::moveSynth,0,masterChain,0}};
    source->sound.values[index("synth.1.drum.pad.1.trim_start")]=.62f;
    CHECK(admitProposal(*source,policy,raw,*output,trace.get())==ProposalStatus::accepted);
    CHECK(trace->count==3 && trace->effects[0]==seed->effects[0]);
    CHECK(trace->effects[1].operation==9 && trace->effects[2].operation==10);
    CHECK(trace->effects[1].beforeUse==SlotUse::present && trace->effects[1].afterUse==SlotUse::present);
    CHECK(trace->effects[2].beforeUse==SlotUse::present && trace->effects[2].afterUse==SlotUse::present);
    for(unsigned n=1;n<3;++n) CHECK(trace->effects[n].kind==SourceAssignmentEffectKind::preserve
        && !trace->effects[n].invalidateTarget && !trace->effects[n].fullNumericBank && !trace->effects[n].freshLogicalOwner);
    CHECK(output->sound.values==source->sound.values);
    // Optional output cannot change the accepted numeric document or any compact schema byte.
    auto without=std::make_unique<ProjectDocument>(); CHECK(admitProposal(*source,policy,raw,*without)==ProposalStatus::accepted);
    CHECK(exact(*output,*without));
    juce::String warnings="previous warnings";bool allocationFailed=true;
    CHECK(admitProposal(*source,policy,raw,*output,trace.get(),&allocationFailed,&warnings)==ProposalStatus::accepted);
    CHECK(!allocationFailed&&warnings.isEmpty()&&exact(*output,*without));
    juce::MemoryBlock compactWith,compactWithout;
    CHECK(encodeProjectState(*output,compactWith) && encodeProjectState(*without,compactWithout) && compactWith==compactWithout);
    const auto schema=json(proposalSchema(policy)); CHECK(schema.find("source_assignment")==std::string::npos && schema.find("freshLogicalOwner")==std::string::npos);
    auto malformed=raw; malformed.parameters={{"missing.control",.3f}}; *trace=*seed; *output=*sentinel;
    CHECK(admitProposal(*source,policy,malformed,*output,trace.get())!=ProposalStatus::accepted);
    CHECK(exact(*output,*sentinel) && *trace==*seed);
    // Exact physical bound: 607+one append succeeds; the next source append rejects transactionally.
    trace=std::make_unique<SourceAssignmentTrace>(); trace->count=607;
    for(unsigned n=0;n<607;++n) trace->effects[n].operation=std::uint16_t(n);
    ProposalEdits remove; remove.racks={{RackEditKind::removeSynth,0}};remove.reconciliation.remove={source->sound.personalities->instances[0].id};
    CHECK(admitProposal(*source,policy,remove,*output,trace.get())==ProposalStatus::accepted);
    CHECK(trace->count==608 && trace->effects[607].operation==607);
    auto full=std::make_unique<SourceAssignmentTrace>(*trace); *output=*sentinel;
    CHECK(admitProposal(*source,policy,remove,*output,trace.get())!=ProposalStatus::accepted);
    CHECK(exact(*output,*sentinel) && *trace==*full);
    ProposalEdits numeric; numeric.parameters={{"global.master_level",.27f}};
    CHECK(admitProposal(*source,policy,numeric,*output,trace.get())==ProposalStatus::accepted && *trace==*full);
    trace->count=609; auto invalidCount=std::make_unique<SourceAssignmentTrace>(*trace); *output=*sentinel;
    warnings="warning sentinel";
    CHECK(admitProposal(*source,policy,numeric,*output,trace.get(),&allocationFailed,&warnings)!=ProposalStatus::accepted);
    CHECK(exact(*output,*sentinel) && *trace==*invalidCount&&warnings=="warning sentinel"&&!allocationFailed);
    *trace=*seed; trace->effects[0].operation=UINT16_MAX; auto exhausted=std::make_unique<SourceAssignmentTrace>(*trace);
    CHECK(admitProposal(*source,policy,remove,*output,trace.get())!=ProposalStatus::accepted && *trace==*exhausted && exact(*output,*sentinel));
    // The rack wrapper has the same output/difference/trace guarantee at overflow and ordinary failure.
    auto sound=std::make_unique<SoundDocument>(source->sound); RackDifference difference; difference.sonic="sentinel";
    auto placement=journalPlace(1,1); *trace=*full;
    CHECK(makeRackCandidate(source->sound,placement,SoundEditOrigin::manual,*sound,difference,trace.get())!=RackCandidateStatus::ready);
    CHECK(*sound==source->sound && difference.sonic=="sentinel" && *trace==*full);
}
void source_assignment_journal_catalog_prefix_once()
{
    begin(__func__);
    auto source=std::make_unique<ProjectDocument>();source->sound=copperArcSound();auto output=std::make_unique<ProjectDocument>();
    source->sound.macros[0]={parameterRegistry()[index("synth.1.analog.filter.cutoff")].stateKey,.3f};
    auto trace=std::make_unique<SourceAssignmentTrace>(); RackCatalogSnapshot root;
    const auto authority=libraryAuthority(*source,root,{"Copper Arc"});
    ProposalEdits edits; edits.racks={{RackEditKind::removeSynth,0}};edits.reconciliation.remove={source->sound.personalities->instances[0].id};
    edits.catalogSelections={selection(authority,factory("Copper Arc"),7,0)};
    edits.catalogParameters={{7,0,"analog.filter.cutoff",.37f}};
    juce::String warnings="warning sentinel";
    const auto admittedStatus=admitProposal(*source,{},edits,authority,*output,trace.get(),nullptr,&warnings);
    CHECK(admittedStatus==ProposalStatus::accepted); if(admittedStatus!=ProposalStatus::accepted)return;
    CHECK(warnings.contains("Global macro 1 target affected"));
    auto compatible=std::make_unique<ProjectDocument>();CHECK(admitProposal(*source,{},edits,authority,*compatible)==ProposalStatus::accepted&&exact(*output,*compatible));
    CHECK(output->sound.personalities && output->sound.personalities->instances.size()==1);
    if(!output->sound.personalities || output->sound.personalities->instances.empty())return;
    CHECK(trace->count==2);
    CHECK(trace->effects[0].kind==SourceAssignmentEffectKind::preserve && trace->effects[0].bank==0 && trace->effects[0].operation==0);
    CHECK(trace->effects[1].kind==SourceAssignmentEffectKind::clear && trace->effects[1].bank==1 && trace->effects[1].operation==1);
    CHECK(trace->effects[1].freshLogicalOwner && trace->effects[1].fullNumericBank && trace->effects[1].afterOwner==output->sound.personalities->instances.front().id);
    CHECK(output->sound.values[index("synth.2.analog.filter.cutoff")]==.37f);
    auto sentinel=std::make_unique<ProjectDocument>(*output); auto prior=std::make_unique<SourceAssignmentTrace>(*trace);
    edits.catalogParameters={{7,0,"not.a.local.key",.4f}};
    warnings="warning sentinel";
    CHECK(admitProposal(*source,{},edits,authority,*output,trace.get(),nullptr,&warnings)!=ProposalStatus::accepted);
    CHECK(exact(*output,*sentinel) && *trace==*prior&&warnings=="warning sentinel");
    // The direct catalog wrapper forwards one primitive and rolls both outputs back on a bad captured identity.
    CHECK(!authority.definitions.empty()); if(authority.definitions.empty())return;
    const auto& captured=authority.definitions.front(); auto op=journalPlace(1,1); op.design.reset();
    auto sound=std::make_unique<SoundDocument>(); RackDifference difference;
    trace=std::make_unique<SourceAssignmentTrace>(); CHECK(makeCatalogRackCandidate(source->sound,root,captured.design,captured.revision,captured.contentSha256,
        op,SoundEditOrigin::manual,*sound,difference,trace.get())==RackCandidateStatus::ready);
    CHECK(trace->count==1 && trace->effects[0].operation==0 && trace->effects[0].kind==SourceAssignmentEffectKind::clear);
    auto saved=std::make_unique<SoundDocument>(*sound); prior=std::make_unique<SourceAssignmentTrace>(*trace); difference.sonic="unchanged";
    CHECK(makeCatalogRackCandidate(source->sound,root,captured.design,captured.revision,"bad",op,SoundEditOrigin::manual,*sound,difference,trace.get())!=RackCandidateStatus::ready);
    CHECK(*sound==*saved && difference.sonic=="unchanged" && *trace==*prior);
}

// The face editor plan's decision 5: a REFINE that changes the rack's design forks it, so the library's design and the
// sound's never share an ID and revision with different content, which the catalog refuses as an identity conflict.
}

static void compact_inventory_authority()
{
    begin("compact_inventory_authority");
    CatalogAuthority authority;authority.root={juce::Uuid(),1,0,0};authority.snapshot=juce::Uuid();authority.rendering=CatalogRendering::ownedLocal;
    juce::Array<juce::var> rows;
    auto design=airack::test::frozenFactoryRackDesigns().front();
    for(int n=0;n<96;++n){
        design.id=juce::Uuid();juce::MemoryBlock canonical;CHECK(encodeRackDesign(design,canonical,RackValidationMode::instanceRecall));
        juce::Array<juce::var> modules;int synths=0,effects=0;
        for(const auto& unit:design.units){modules.add(int(unit.module));isSourceModule(unit.module)?++synths:++effects;}
        const auto hash=juce::SHA256(canonical).toHexString();
        rows.add(array({design.id.toString(),juce::String(std::to_string(design.revision)),hash,design.name,"synth",array({0}),synths,effects,modules}));
        if(n==95)authority.definitions.push_back({design.id,design.revision,hash,canonical});
    }
    auto descriptor=object({{"format",2},{"complete",true},{"columns",array({"id","revision","sha256","name","role","tags","synthUnits","fxUnits","modules"})},
        {"tags",array({object({{"id","warm"},{"label","Warm"}})})},{"entries",rows}});
    authority.descriptorJson=json(descriptor);CHECK(ai::detail::catalogAuthorityValid(authority));
    ProposalEdits edits;const auto& selected=authority.definitions.front();edits.catalogSelections={{7,selected.design,selected.revision,selected.contentSha256,{}}};
    CHECK(ai::detail::catalogIntentValid(edits,{},authority));CHECK(!proposalSchema({},authority).isVoid());
    const auto reject=[&](juce::var broken){auto bad=authority;bad.descriptorJson=json(broken);CHECK(!ai::detail::catalogAuthorityValid(bad));};
    auto bad=descriptor.clone();bad["columns"].getArray()->set(2,"archive");reject(bad);
    bad=descriptor.clone();bad["entries"].getArray()->add(rows[0]);reject(bad);
    bad=descriptor.clone();bad["entries"][0].getArray()->set(5,array({1}));reject(bad);
    bad=descriptor.clone();bad["entries"][0].getArray()->set(6,1.0);reject(bad);
    bad=descriptor.clone();bad["entries"][0].getArray()->set(8,array({999}));reject(bad);
    bad=descriptor.clone();bad["entries"][0].getArray()->set(3," ");reject(bad);
    bad=descriptor.clone();bad["tags"][0].getDynamicObject()->setProperty("label",juce::String::charToString(1));reject(bad);
    edits.catalogParameters={{7,0,"invented.key",.5f}};CHECK(!ai::detail::catalogIntentValid(edits,{},authority));
    auto corrupt=authority;corrupt.definitions.front().canonical.append("x",1);CHECK(!ai::detail::catalogAuthorityValid(corrupt));
}

void direct_control_edits()
{
    begin("direct_control_edits");ProposalEdits edits;ProposalPolicy policy;
    const std::string body=R"({"parameter_edits":[{"id":"global.master_level","normalized":0.23}]})";
    CHECK(decodeControlEdits(body,policy,nullptr,edits));CHECK(edits.name.isEmpty());CHECK(edits.parameters.size()==1);
    CHECK(!decodeProposal(body,policy,edits));
    auto source=std::make_unique<ProjectDocument>();source->sound=copperArcSound();auto candidate=std::make_unique<ProjectDocument>();
    CHECK(admitProposal(*source,policy,edits,*candidate)==ProposalStatus::accepted);
    CHECK(candidate->sound.name==source->sound.name);
    for(const auto* bad:{R"({"parameter_edits":[{"id":"global.master_level","normalized":0.2},{"id":"global.master_level","normalized":0.3}]})",
        R"({"parameter_edits":[{"id":"global.master_level","normalized":"0.2"}]})",R"({"parameter_edits":[{"id":"global.master_level","normalized":1.2}]})",
        R"({"parameter_edits":[{"id":"unknown.id","normalized":0.2}]})",R"({"unknown":[]})",R"({"catalog_selections":[{"local_id":1}]})",R"({"name":""})"})
    {
        CHECK(!decodeControlEdits(bad,policy,nullptr,edits));
        auto provider=juce::JSON::parse(bad);auto* fields=provider.getDynamicObject();fields->setProperty("version",1);
        for(auto key:{"parameter_edits","rack_edits","macro_edits"})if(!provider.hasProperty(key))fields->setProperty(key,juce::Array<juce::var>());
        if(!provider.hasProperty("pattern"))fields->setProperty("pattern",juce::var());fields->setProperty("characteristics","");fields->setProperty("explanation","");
        CHECK(!decodeProposal(juce::JSON::toString(provider,true).toStdString(),policy,edits));
    }
    auto schema=controlEditsSchema(policy,nullptr);CHECK(schema.getDynamicObject());CHECK(!schema["properties"].hasProperty("version"));
    CHECK(schema["required"].getArray()&&schema["required"].getArray()->isEmpty());
}
int main(int argc, char** argv)
{
    if(argc==2&&std::string_view(argv[1])=="--compact-inventory"){compact_inventory_authority();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;}
    if (argc >= 2 && std::string_view(argv[1]) == "--provider-catalog") {
        juce::File output, baseline;
        if (argc > 4) return 2;
        if (argc >= 3) {
            if (!juce::File::isAbsolutePath(argv[2])) return 2;
            output = juce::File(argv[2]); const auto sourceRoot = juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory();
            if (output == sourceRoot || output.isAChildOf(sourceRoot)) return 2;
            if (argc == 4) { if (!juce::File::isAbsolutePath(argv[3])) return 2; baseline = juce::File(argv[3]); if (!baseline.isDirectory()) return 2; }
            std::error_code error;
            if (!std::filesystem::create_directory(std::filesystem::path(output.getFullPathName().toWideCharPointer()), error) || error) return 2;
        }
        drum_catalog_gate_and_retained_defaults(); native_catalog_complete_provider_inventory();
        provider_catalog_projection(); provider_catalog_reach(); provider_catalog_ownership_corpus(output, baseline);
        std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n"; return failures ? 1 : 0;
    }
    if(argc==2&&std::string_view(argv[1])=="--strict-json") {
        strict_json_boundary();strict_json_quoted_equivalence();numeric_wrap_representability();integer_tokens_preserve_exact_fields();strict_numeric_precision_and_atomicity();
        std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
    }
    direct_control_edits();
    if(argc==2&&std::string_view(argv[1])=="--control-edits"){std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    compact_inventory_authority();
    decode_diagnostics();
    if(argc==2&&std::string_view(argv[1])=="--source-journal") {
        source_assignment_journal_typed_primitives(); source_assignment_journal_admission_and_capacity(); source_assignment_journal_catalog_prefix_once();
        std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n"; return failures?1:0;
    }
    if(argc==2&&std::string_view(argv[1])=="--extension-keys"){extension_local_keys_ai_catalog_and_controls();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--saturation"){saturation_catalog_and_admission();band_color_catalog_admission();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;}
    saturation_catalog_and_admission();band_color_catalog_admission();
    if(argc==2&&std::string_view(argv[1])=="--decode-diagnostics"){std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;}
    if (argc == 2 && std::string_view(argv[1]) == "--numeric-wrap") {
        numeric_wrap_representability();
        std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
        return failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--integer-tokens") {
        integer_tokens_preserve_exact_fields();
        std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
        return failures == 0 ? 0 : 1;
    }
    source_assignment_journal_typed_primitives(); source_assignment_journal_admission_and_capacity(); source_assignment_journal_catalog_prefix_once();
    extension_local_keys_ai_catalog_and_controls();task12_catalog_authority_proposal();
    drum_catalog_gate_and_retained_defaults();native_catalog_complete_provider_inventory();provider_catalog_ownership_corpus();captured_compound_removal_permutations();catalog_matches_all_758_controls(); catalog_rack_locks_history_and_macros(); schema_is_closed_required_nullable(); version2_has_no_rack_design(); version2_requires_a_valid_name();
    every_control_is_addressable(); sound_pattern_both_scope(); macro_assignment_permission_and_locks();
    required_pattern_admission(); required_pattern_schema();
    rack_edits_capacity_history_dependency(); explicit_parameters_create_analog_wavetable_and_effects();
    strict_json_boundary(); strict_json_quoted_equivalence(); proposal_caps_and_note_boundaries(); rejection_preserves_output_and_alias(); numeric_wrap_representability(); integer_tokens_preserve_exact_fields(); strict_numeric_precision_and_atomicity();
    create_sound_preserves_personality(); create_sound_rejects_uncaptured_owned_structure(); create_sound_moves_but_never_maps(); v2_reconciliation_bounds(); provider_catalog_projection(); reach_admission(); small_hours_catalog_reach(); night_dial_catalog_reach(); provider_catalog_reach();
    std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

#include "ProposalDocument.h"
#include "StrictJson.h"
#include "RackLibrary.h"
#include "RackInventory.h"
#include "UserReach.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace airack::ai
{
namespace
{
constexpr std::size_t maximumProposalBytes = 262144, maximumRackEdits = 64, maximumTextBytes = 2048;
constexpr const char* rackNames[] {"add_synth", "remove_synth", "move_synth", "add_effect", "remove_effect", "move_effect", "replace_effect"};
constexpr const char* effectNames[] {"drive", "delay", "reverb", "compressor", "eq", "filter_fx", "chorus", "phaser", "flanger", "stereo", "dimension", "frequency_shift", "convolution", "frequency_split", "mid_side_split", "dynamic_eq", "space_reverb", "era_reverb", "saturation"};
static_assert(std::size(effectNames)==std::size_t(EffectKind::saturation)+1);
constexpr const char* gridNames[] {"quarter", "eighth", "sixteenth", "thirty_second", "sixty_fourth"};

bool closed(const juce::var& value, std::initializer_list<const char*> fields)
{
    const auto* object = value.getDynamicObject();
    if (!object || object->getProperties().size() != static_cast<int>(fields.size())) return false;
    return std::all_of(fields.begin(), fields.end(), [&](auto field) { return value.hasProperty(field); });
}
bool number(const juce::var& value, double& output)
{
    if (!value.isInt() && !value.isInt64() && !value.isDouble()) return false;
    const auto n = static_cast<double>(value);
    if (!std::isfinite(n)) return false;
    output = n; return true;
}
bool integer(const juce::var& value, int low, int high, int& output)
{
    // Integral-looking doubles can conceal a fractional token after rounding.
    // Wire integer fields therefore require a plain integer lexical form.
    if (!value.isInt() && !value.isInt64()) return false;
    const auto n = static_cast<juce::int64>(value);
    if (n < low || n > high) return false;
    output = static_cast<int>(n); return true;
}
bool real(const juce::var& value, float low, float high, float& output)
{
    double n;
    if (!number(value, n) || n < low || n > high) return false;
    const auto f = static_cast<float>(n);
    if (!std::isfinite(f) || (n != 0 && f == 0)) return false;
    output = f; return true;
}
template<std::size_t N>
bool named(const juce::var& value, const char* const (&names)[N], std::uint8_t& index)
{
    if (!value.isString()) return false;
    for (std::size_t i = 0; i < N; ++i) if (value.toString() == names[i]) { index = static_cast<std::uint8_t>(i); return true; }
    return false;
}
std::optional<std::size_t> parameterIndex(const juce::String& id)
{
    const auto registry = parameterRegistry();
    const auto found = std::find_if(registry.begin(), registry.end(), [&](const auto& d) { return d.id == id; });
    if (found == registry.end()) return {};
    return static_cast<std::size_t>(found - registry.begin());
}
bool textValid(const juce::String& text)
{
    return text.getNumBytesAsUTF8() <= maximumTextBytes
        && juce::CharPointer_UTF8::isValidString(text.toRawUTF8(), static_cast<int>(text.getNumBytesAsUTF8()));
}
bool rackValid(const RackEdit& edit)
{
    if (edit.kind > RackEditKind::replaceEffect || edit.effect > EffectKind::saturation
        || edit.destination > masterChain || edit.position > 8) return false;
    const bool synth = edit.kind <= RackEditKind::moveSynth;
    const bool add = edit.kind == RackEditKind::addSynth || edit.kind == RackEditKind::addEffect;
    if (edit.slot >= (synth ? 4 : 8) && !(add && edit.slot == noSlot)) return false;
    if (synth && (edit.destination != masterChain || edit.effect != EffectKind::drive)) return false;
    if ((edit.kind == RackEditKind::removeSynth || edit.kind == RackEditKind::removeEffect)
        && (edit.destination != masterChain || edit.position != 0 || edit.effect != EffectKind::drive)) return false;
    if (edit.kind == RackEditKind::replaceEffect && (edit.destination != masterChain || edit.position != 0)) return false;
    return true;
}
ProposalStatus editsValid(const ProposalEdits& edits,ProposalDecodeReason* reason=nullptr,bool catalogAliases=false)
{
    const auto reject=[&](ProposalStatus status,ProposalDecodeReason why){if(reason)*reason=why;return status;};
    if (edits.parameters.size() > parameterCount || edits.racks.size() > maximumRackEdits || edits.macros.size() > 8)
        return reject(ProposalStatus::malformed,ProposalDecodeReason::validity);
    if(!textValid(edits.characteristics)||!textValid(edits.explanation))return reject(ProposalStatus::malformed,ProposalDecodeReason::invalidText);
    if(edits.name.isNotEmpty()&&(edits.name!=edits.name.trim()||!soundNameValid(edits.name)))return reject(ProposalStatus::malformed,ProposalDecodeReason::invalidName);
    std::set<juce::String> ids;
    for (const auto& edit : edits.parameters) {
        if (!std::isfinite(edit.normalized) || edit.normalized < 0 || edit.normalized > 1)
            return reject(ProposalStatus::malformed,ProposalDecodeReason::validity);
        if(!ids.insert(edit.id).second)return reject(ProposalStatus::malformed,ProposalDecodeReason::duplicateParameter);
        if (!parameterIndex(edit.id)) return reject(ProposalStatus::unsupported,ProposalDecodeReason::unknownParameter);
    }
    for (const auto& edit : edits.racks) if (!rackValid(edit)) return reject(ProposalStatus::malformed,ProposalDecodeReason::invalidRack);
    std::array<bool, 8> indices {};
    for (const auto& edit : edits.macros) {
        if (edit.index < 1 || edit.index > 8 || indices[edit.index - 1] || !std::isfinite(edit.amount)
            || edit.amount < -1 || edit.amount > 1 || (edit.targetId.isEmpty() && edit.amount != 0)) return reject(ProposalStatus::malformed,ProposalDecodeReason::invalidMacro);
        indices[edit.index - 1] = true;
        if (edit.targetId.isNotEmpty() && !parameterIndex(edit.targetId) && !(catalogAliases&&edit.targetId.startsWith("catalog:"))) return reject(ProposalStatus::unsupported,ProposalDecodeReason::unknownMacroTarget);
    }
    if (edits.pattern && !validatePatternDocument(*edits.pattern)) return reject(ProposalStatus::malformed,ProposalDecodeReason::pattern);
    return ProposalStatus::accepted;
}
bool decodePattern(const juce::var& value, PatternDocument& output)
{
    if (!closed(value, {"bars", "meter_numerator", "meter_denominator", "grid", "swing", "notes"})) return false;
    int bars, numerator, denominator; std::uint8_t grid; double swing;
    if (!integer(value["bars"], 1, 8, bars) || !integer(value["meter_numerator"], 1, 32, numerator)
        || !integer(value["meter_denominator"], 1, 32, denominator) || !named(value["grid"], gridNames, grid)
        || !number(value["swing"], swing) || swing < 0 || swing > 1) return false;
    const auto* notes = value["notes"].getArray();
    if (!notes || notes->size() > static_cast<int>(maximumPatternNotes)) return false;
    PatternDocument candidate;
    candidate.bars = static_cast<std::uint8_t>(bars);
    candidate.meter = {static_cast<std::uint8_t>(numerator), static_cast<std::uint8_t>(denominator)};
    candidate.grid = static_cast<PatternGrid>(grid); candidate.swing = swing;
    for (const auto& note : *notes) {
        if (!closed(note, {"start_beats", "duration_beats", "pitch", "velocity", "channel"})) return false;
        double start, duration; int pitch, velocity, channel;
        if (!number(note["start_beats"], start) || !number(note["duration_beats"], duration)
            || !integer(note["pitch"], 0, 127, pitch) || !integer(note["velocity"], 1, 127, velocity)
            || !integer(note["channel"], 1, 16, channel)) return false;
        candidate.notes[candidate.noteCount++] = {start, duration, static_cast<std::uint8_t>(pitch),
                                                 static_cast<std::uint8_t>(velocity), static_cast<std::uint8_t>(channel)};
    }
    if (!validatePatternDocument(candidate)) return false;
    output = candidate; return true;
}
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject;
    for (const auto& f : fields) o->setProperty(f.first, f.second);
    return o;
}
juce::var schemaObject(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    juce::Array<juce::var> required;
    for (const auto& field : fields) required.add(field.first);
    return object({{"type", "object"}, {"properties", object(fields)}, {"required", required}, {"additionalProperties", false}});
}
juce::var schemaArray(const juce::var& item) { return object({{"type", "array"}, {"items", item}}); }
juce::var schemaType(const char* type)
{
    if (std::string_view(type) == "integer")
        return object({{"type", type}, {"description", "Use a plain JSON integer token. Decimal and exponent forms are rejected by native validation."}});
    return object({{"type", type}});
}
template<std::size_t N>
juce::var schemaEnum(const char* const (&names)[N])
{
    juce::Array<juce::var> values;
    for (auto name : names) values.add(name);
    return object({{"type", "string"}, {"enum", values}});
}

bool bankLocked(const SoundDocument& d,NativeModule module,std::uint8_t slot)
{
    const bool isSynth=isSourceModule(module);
    if(isSynth?d.synthLocks[slot]:d.effectLocks[slot])return true;
    const auto prefix=juce::String(isSynth?"synth.":"effect.")+juce::String(slot+1)+".";
    const auto registry=parameterRegistry();
    for(std::size_t n=0;n<registry.size();++n)if(d.controlLocks[n] && registry[n].id.startsWith(prefix))return true;
    return false;
}
const RackInstance* selectedInstance(const SoundDocument& d, const juce::Uuid& id)
{
    if (d.personalities) for (const auto& i : d.personalities->instances) if (i.id == id) return &i;
    return nullptr;
}
bool reconciliationAllowed(const RackReconcileChoices& c, const RackReconcileChoices& allowed)
{
    std::set<juce::String> seen;
    for (const auto pair : {std::pair{&c.detach, &allowed.detach}, std::pair{&c.remove, &allowed.remove}, std::pair{&c.replace, &allowed.replace}}) {
        if (pair.first->size() > 12) return false;
        for (const auto& id : *pair.first)
            if (id.isNull() || !seen.insert(id.toString()).second
                || std::find(pair.second->begin(), pair.second->end(), id) == pair.second->end()) return false;
    }
    if (c.controls.size() > 96) return false;
    std::set<std::pair<juce::String, unsigned>> controls;
    for (const auto& r : c.controls) {
        if (r.instance.isNull() || !controls.emplace(r.instance.toString(), r.control).second) return false;
        if (std::none_of(allowed.controls.begin(), allowed.controls.end(), [&](const auto& a) {
            return a.instance == r.instance && a.control == r.control && a.replacement == r.replacement;
        })) return false;
    }
    return true;
}
bool noReconciliation(const RackReconcileChoices& c)
{ return c.detach.empty() && c.remove.empty() && c.replace.empty() && c.controls.empty(); }
ProposalStatus reconcileStatus(RackCandidateStatus s)
{
    switch (s) {
        case RackCandidateStatus::ready: return ProposalStatus::accepted;
        case RackCandidateStatus::lockedDependency: return ProposalStatus::lockedDependency;
        case RackCandidateStatus::capacityReached: return ProposalStatus::capacityReached;
        case RackCandidateStatus::replacementRequired: return ProposalStatus::replacementRequired;
        default: return ProposalStatus::invalidCandidate;
    }
}
bool uuid(const juce::var& v, juce::Uuid& id)
{
    if (!v.isString()) return false;
    juce::Uuid parsed(v.toString());
    if (parsed.isNull() || parsed.toDashedString() != v.toString()) return false;
    id = parsed; return true;
}
// Use the producer's closed focused-control codec rather than a second validator.
bool decodeControl(const juce::var& v, FocusedControl& out)
{
    if (!closed(v, {"label", "unit", "section", "key", "widget", "response", "minimum", "maximum"})) return false;
    int unit, section; std::uint8_t widget, response;
    constexpr const char* widgets[]{"rotary", "verticalSlider", "toggle", "choice"};
    constexpr const char* responses[]{"linear", "logarithmic"};
    if (!v["label"].isString() || !v["key"].isString() || !integer(v["unit"], 0, 8, unit)
        || !integer(v["section"], 0, 3, section) || !named(v["widget"], widgets, widget)
        || !named(v["response"], responses, response) || !real(v["minimum"], -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), out.minimum)
        || !real(v["maximum"], -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), out.maximum)) return false;
    out.label = v["label"].toString(); out.key = v["key"].toString(); out.unit = std::uint8_t(unit);
    out.section = std::uint8_t(section); out.widget = FocusWidget(widget); out.response = FocusResponse(response);
    return out.label.getNumBytesAsUTF8()<=64 && out.key.getNumBytesAsUTF8()<=rackParameterKeyBytes
        && out.key.isNotEmpty() && out.key.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.")
        && out.minimum<out.maximum;
}
}

namespace {
bool readBaseEdits(const juce::var& root,ProposalEdits& candidate,ProposalDecodeReason* reason,bool catalogAliases=false)
{
    const juce::Array<juce::var> empty;
    const auto* parameters = root.hasProperty("parameter_edits")?root["parameter_edits"].getArray():&empty;
    const auto* racks = root.hasProperty("rack_edits")?root["rack_edits"].getArray():&empty; const auto* macros = root.hasProperty("macro_edits")?root["macro_edits"].getArray():&empty;
    if (!parameters || !racks || !macros || parameters->size() > static_cast<int>(parameterCount)
        || racks->size() > static_cast<int>(maximumRackEdits) || macros->size() > 8) return false;
    for(auto field:{"characteristics","explanation"})if(root.hasProperty(field)&&!root[field].isString())return false;
    candidate.characteristics = root["characteristics"].toString(); candidate.explanation = root["explanation"].toString();
    if(reason)*reason=ProposalDecodeReason::parameterEdit;
    for (const auto& value : *parameters) {
        ParameterEdit edit;
        if (!closed(value, {"id", "normalized"}) || !value["id"].isString() || !real(value["normalized"], 0, 1, edit.normalized)) return false;
        edit.id = value["id"].toString(); candidate.parameters.push_back(edit);
    }
    if(reason)*reason=ProposalDecodeReason::rackEdit;
    for (const auto& value : *racks) {
        int slot, chain, position; std::uint8_t kind, effect;
        if (!closed(value, {"kind", "slot", "chain", "position", "effect"}) || !named(value["kind"], rackNames, kind)
            || !named(value["effect"], effectNames, effect) || !integer(value["slot"], 0, 255, slot)
            || !integer(value["chain"], 0, 4, chain) || !integer(value["position"], 0, 8, position)) return false;
        candidate.racks.push_back({static_cast<RackEditKind>(kind), static_cast<std::uint8_t>(slot),
            static_cast<std::uint8_t>(chain), static_cast<std::uint8_t>(position), static_cast<EffectKind>(effect), false});
    }
    if(reason)*reason=ProposalDecodeReason::macroEdit;
    for (const auto& value : *macros) {
        int index; MacroEdit edit;
        if (!closed(value, {"index", "target_id", "amount"}) || !integer(value["index"], 1, 8, index)
            || (!value["target_id"].isVoid() && !value["target_id"].isString()) || !real(value["amount"], -1, 1, edit.amount)) return false;
        // A string target must be a real ID; only JSON null denotes unbound.
        if (value["target_id"].isString() && value["target_id"].toString().isEmpty()) return false;
        edit.index = static_cast<std::uint8_t>(index); edit.targetId = value["target_id"].toString();
        candidate.macros.push_back(edit);
    }
    if(reason)*reason=ProposalDecodeReason::pattern;
    if (!root["pattern"].isVoid()) {
        PatternDocument pattern;
        if (!decodePattern(root["pattern"], pattern)) return false;
        candidate.pattern = pattern;
    }
    if(reason)*reason=ProposalDecodeReason::validity;
    if (editsValid(candidate,reason,catalogAliases) != ProposalStatus::accepted) return false;
    return true;
}
}
namespace {
bool readReconciliation(const juce::var& root,ProposalEdits& candidate,ProposalDecodeReason* reason) {
    const auto c = root["reconciliation"];
    if(reason)*reason=ProposalDecodeReason::reconciliation;
    if (!closed(c, {"detach", "remove", "replace", "controls"})) return false;
    std::set<juce::String> ids;
    for (const auto& p : {std::pair{"detach", &candidate.reconciliation.detach}, std::pair{"remove", &candidate.reconciliation.remove}, std::pair{"replace", &candidate.reconciliation.replace}}) {
        const auto* list = c[p.first].getArray(); if (!list || list->size() > 12) return false;
        for (const auto& v : *list) { juce::Uuid id; if (!uuid(v, id) || !ids.insert(id.toString()).second) return false; p.second->push_back(id); }
    }
    const auto* controls = c["controls"].getArray(); if (!controls || controls->size() > 96) return false;
    std::set<std::pair<juce::String, unsigned>> seen;
    for (const auto& v : *controls) {
        RackControlResolution r; int n;
        if (!closed(v, {"instance", "control", "replacement"}) || !uuid(v["instance"], r.instance)
            || !integer(v["control"], 0, 7, n) || !seen.emplace(r.instance.toString(), n).second) return false;
        r.control = std::uint8_t(n);
        if (!v["replacement"].isVoid()) { FocusedControl f; if (!decodeControl(v["replacement"], f)) return false; r.replacement = f; }
        candidate.reconciliation.controls.push_back(r);
    }
    return true;
}
}
const char* proposalDecodeReasonName(ProposalDecodeReason reason) noexcept
{
    switch(reason){case ProposalDecodeReason::accepted:return "proposal_decoded";case ProposalDecodeReason::strictJson:return "proposal_strict_json_invalid";
        case ProposalDecodeReason::rootFields:return "proposal_root_fields_invalid";case ProposalDecodeReason::parameterEdit:return "proposal_parameter_edit_invalid";
        case ProposalDecodeReason::rackEdit:return "proposal_rack_edit_invalid";case ProposalDecodeReason::macroEdit:return "proposal_macro_edit_invalid";
        case ProposalDecodeReason::pattern:return "proposal_pattern_invalid";
        case ProposalDecodeReason::reconciliation:return "proposal_reconciliation_invalid";case ProposalDecodeReason::validity:return "proposal_validity_invalid";
        case ProposalDecodeReason::unknownParameter:return "proposal_parameter_unknown";case ProposalDecodeReason::duplicateParameter:return "proposal_parameter_duplicate";
        case ProposalDecodeReason::invalidRack:return "proposal_rack_invalid";case ProposalDecodeReason::invalidText:return "proposal_text_invalid";
        case ProposalDecodeReason::invalidMacro:return "proposal_macro_invalid";case ProposalDecodeReason::unknownMacroTarget:return "proposal_macro_target_unknown";
        case ProposalDecodeReason::catalogSelection:return "proposal_catalog_selection_invalid";case ProposalDecodeReason::catalogParameter:return "proposal_catalog_parameter_invalid";
        case ProposalDecodeReason::catalogIntent:return "proposal_catalog_intent_invalid";case ProposalDecodeReason::invalidName:return "proposal_name_invalid";}
    return "proposal_validity_invalid";
}
bool soundNameValid(const juce::String& trimmed)
{
    const auto length=trimmed.length();bool letter=false;
    if(length<1||length>40)return false;
    for(auto p=trimmed.getCharPointer();!p.isEmpty();++p){
        const auto c=*p;letter=letter||juce::CharacterFunctions::isLetter(c);
        if(!juce::CharacterFunctions::isLetterOrDigit(c)&&c!=' '&&c!='-'&&c!='\''&&c!=0x2019)return false;
    }
    return letter;
}
bool decodeProposal(std::string_view utf8, const ProposalPolicy&, ProposalEdits& output, ProposalDecodeReason* reason)
{
    if(reason)*reason=ProposalDecodeReason::strictJson;
    juce::var root;
    if (!parseStrictJson(utf8, root, maximumProposalBytes)) return false;
    if(reason)*reason=ProposalDecodeReason::rootFields;
    int version;
    if (!integer(root["version"], 1, 2, version)) return false;
    if (version == 1) return decodeProposal(utf8, output, reason);
    if (!closed(root, {"version", "parameter_edits", "rack_edits", "macro_edits", "pattern", "characteristics", "explanation", "name", "reconciliation"})
        || !root["name"].isString()) return false;
    const auto name = root["name"].toString().trim();
    if (!soundNameValid(name)) { if(reason)*reason=ProposalDecodeReason::invalidName; return false; }
    auto legacy = root.clone(); legacy.getDynamicObject()->removeProperty("reconciliation"); legacy.getDynamicObject()->removeProperty("name");
    legacy.getDynamicObject()->setProperty("version", 1);
    ProposalEdits candidate;
    if (!decodeProposal(juce::JSON::toString(legacy, true, 22).toStdString(), candidate, reason)) return false;
    candidate.name = name;
    if(!readReconciliation(root,candidate,reason))return false;
    output = std::move(candidate);if(reason)*reason=ProposalDecodeReason::accepted; return true;
}

bool decodeProposal(std::string_view utf8, ProposalEdits& output, ProposalDecodeReason* reason)
{
    if(reason)*reason=ProposalDecodeReason::strictJson;
    juce::var root;
    if (!parseStrictJson(utf8, root, maximumProposalBytes)) return false;
    if(reason)*reason=ProposalDecodeReason::rootFields;
    if (!closed(root, {"version", "parameter_edits", "rack_edits", "macro_edits", "pattern", "characteristics", "explanation"})) return false;
    int version;
    if (!integer(root["version"], 1, 1, version) || !root["characteristics"].isString() || !root["explanation"].isString()) return false;
    ProposalEdits candidate;
    if(!readBaseEdits(root,candidate,reason))return false;
    output = std::move(candidate);if(reason)*reason=ProposalDecodeReason::accepted; return true;
}

bool detail::rackEditsInReach(const SoundDocument& source, const ProposalEdits& edits)
{
    if (!edits.reconciliation.detach.empty()) return false;
    const auto& removed = edits.reconciliation.remove;
    for (const auto& e : edits.racks) {
        if (e.kind == RackEditKind::addSynth || e.kind == RackEditKind::addEffect || e.kind == RackEditKind::replaceEffect) return false;
        const bool synth = e.kind <= RackEditKind::moveSynth;
        if (e.slot >= (synth ? 4 : 8) || !bankUnit(source, synth ? RackRole::synth : RackRole::fx, e.slot)) return false;
        if (e.kind == RackEditKind::moveSynth || e.kind == RackEditKind::moveEffect) continue;
        // A bank leaves only with its whole rack.
        const auto owner = std::find_if(source.personalities->instances.begin(), source.personalities->instances.end(), [&](const auto& i) {
            return std::any_of(i.design.units.begin(), i.design.units.end(), [&](const auto& u) {
                const auto* b = rackUnitBinding(i, u.id); return isSourceModule(u.module) == synth && b && b->slot == e.slot; });
        });
        if (owner == source.personalities->instances.end() || std::find(removed.begin(), removed.end(), owner->id) == removed.end()) return false;
    }
    return true;
}

namespace
{
// What the proposal sets is in the candidate's reach. Placing a rack resets its bank, keys outside reach included; that
// reset is not the proposal's edit. A synth's ENGINE follows the rack that owns its bank.
bool proposalInReach(const SoundDocument& source, const SoundDocument& candidate, const ProposalEdits& edits)
{
    const auto registry = parameterRegistry();
    const auto reach = userReach(candidate);
    const auto engineUnit = [&](std::size_t i) -> const RackUnit* {
        for (std::uint8_t bank = 0; bank < 4; ++bank)
            if (rackParameterIndex(NativeModule::analog, bank, "engine") == i) return bankUnit(candidate, RackRole::synth, bank);
        return nullptr;
    };
    for (const auto& edit : edits.parameters) {
        const auto i = *parameterIndex(edit.id);
        if (reach[i]) continue;
        if (const auto* unit = engineUnit(i)) {
            if (candidate.values[i] != (unit->module == NativeModule::wavetable ? 1.f : 0.f)) return false;
        } else if (candidate.values[i] != source.values[i]) return false;
    }
    for (const auto& edit : edits.macros) {
        if (edit.targetId.isEmpty()) continue;
        const auto i = *parameterIndex(edit.targetId);
        if (registry[i].kind != ParameterKind::continuous || !reach[i]) return false;
    }
    for (std::uint8_t bank = 0; bank < 4; ++bank)
        if (candidate.layout.synthUse[bank] == SlotUse::present && source.layout.synthUse[bank] != SlotUse::present
            && !bankUnit(candidate, RackRole::synth, bank)) return false;
    for (std::uint8_t bank = 0; bank < 8; ++bank)
        if (candidate.layout.effects[bank].use == SlotUse::present && source.layout.effects[bank].use != SlotUse::present
            && !bankUnit(candidate, RackRole::fx, bank)) return false;
    return true;
}
}

namespace {
struct ProposalSourceOwner final {
    SlotUse use=SlotUse::neverUsed;
    juce::Uuid rack=juce::Uuid::null();
    std::uint8_t unit=0;
    bool operator==(const ProposalSourceOwner&) const=default;
};
ProposalSourceOwner proposalSourceOwner(const SoundDocument& d,std::uint8_t bank,const RackLayout* layout=nullptr) noexcept
{
    ProposalSourceOwner out;out.use=(layout?*layout:d.layout).synthUse[bank];
    if(out.use!=SlotUse::present||!d.personalities)return out;
    for(const auto& i:d.personalities->instances)for(const auto& u:i.design.units)if(isSourceModule(u.module))
        if(const auto* b=rackUnitBinding(i,u.id);b&&b->slot==bank){out.rack=i.id;out.unit=u.id;return out;}
    return out;
}
bool proposalSourceOrdinal(const SourceAssignmentTrace& trace,std::uint16_t& ordinal) noexcept
{
    if(trace.count>trace.effects.size())return false;
    if(!trace.count){ordinal=0;return true;}
    if(trace.effects[trace.count-1].operation==UINT16_MAX)return false;
    ordinal=std::uint16_t(trace.effects[trace.count-1].operation+1);return true;
}
bool appendProposalSourceEffect(SourceAssignmentTrace& trace,std::uint8_t bank,
    const ProposalSourceOwner& before,const ProposalSourceOwner& after,std::uint16_t operation,bool invalidate) noexcept
{
    if(trace.count>=trace.effects.size())return false;
    SourceAssignmentEffect effect;effect.bank=bank;effect.operation=operation;effect.beforeUse=before.use;effect.afterUse=after.use;
    effect.beforeOwner=before.rack;effect.afterOwner=after.rack;effect.beforeUnit=before.unit;effect.afterUnit=after.unit;
    effect.invalidateTarget=invalidate;trace.effects[trace.count++]=effect;return true;
}
bool appendProposalReconciliationEffects(const SoundDocument& before,const SoundDocument& after,
    const RackReconcileChoices& choices,SourceAssignmentTrace& trace) noexcept
{
    std::uint16_t operation=0;bool ordinalChosen=false;
    const auto append=[&](const RackInstance& i){
        for(const auto& u:i.design.units)if(isSourceModule(u.module))if(const auto* b=rackUnitBinding(i,u.id)){
            const auto was=proposalSourceOwner(before,b->slot),now=proposalSourceOwner(after,b->slot);
            if(was==now)continue;
            if(!ordinalChosen){if(!proposalSourceOrdinal(trace,operation))return false;ordinalChosen=true;}
            if(!appendProposalSourceEffect(trace,b->slot,was,now,operation,true))return false;
        }return true;
    };
    // Reconciliation removes explicit dependencies before detaching retained personalities.
    for(const auto& id:choices.remove)if(const auto* i=selectedInstance(before,id);i&&i->design.role==RackRole::synth)
        if(!append(*i))return false;
    if(before.personalities)for(const auto& i:before.personalities->instances)
        if(std::find(choices.detach.begin(),choices.detach.end(),i.id)!=choices.detach.end()&&!append(i))return false;
    return true;
}
}
static void appendAutomationWarning(juce::String* warnings,const RackDifference& difference)
{
    if(warnings&&difference.automationWarning.isNotEmpty()){
        if(warnings->isNotEmpty())*warnings+="; ";
        *warnings+=difference.automationWarning;
    }
}
static ProposalStatus admitProposalImpl(const ProjectDocument& source,const ProposalPolicy& policy,
                            const ProposalEdits& edits,ProjectDocument& output,SourceAssignmentTrace* trace,juce::String* automationWarning)
{
    if (!edits.catalogSelections.empty() || !edits.catalogParameters.empty()) return ProposalStatus::unsupported;
    // Registry initialization is control-thread work and primes native state-key lookup.
    const auto registry = parameterRegistry();
    if (!validateProjectDocument(source)) return ProposalStatus::invalidCandidate;
    if (policy.scope > RequestScope::both || (policy.requirePattern && policy.scope == RequestScope::sound))
        return ProposalStatus::malformed;
    const auto valid = editsValid(edits); if (valid != ProposalStatus::accepted) return valid;
    if ((policy.scope == RequestScope::sound && edits.pattern)
        || (policy.scope == RequestScope::pattern && (!edits.parameters.empty() || !edits.racks.empty() || !edits.macros.empty())))
        return ProposalStatus::scopeViolation;
    if (!edits.macros.empty() && !policy.allowMacroChanges) return ProposalStatus::macroChangeNotRequested;
    if (policy.requirePattern && (!edits.pattern || edits.pattern->noteCount == 0))
        return ProposalStatus::requestedPatternMissing;
    auto candidateStorage = std::make_unique<ProjectDocument>(source);
    auto& candidate = *candidateStorage;
    auto choices = edits.reconciliation;
    // CREATE SOUND removes any placed rack, as the user can; it detaches, replaces and remaps nothing.
    RackReconcileChoices allowed;
    if (source.sound.personalities)
        for (const auto& i : source.sound.personalities->instances) allowed.remove.push_back(i.id);
    if (!reconciliationAllowed(choices, allowed)) return ProposalStatus::scopeViolation;
    if (!detail::rackEditsInReach(source.sound, edits))
        return ProposalStatus::outOfReach;
    for (auto edit : edits.racks) {
        const auto beforeLayout=candidate.sound.layout;
        edit.allowReuse = policy.allowSlotReuse;
        if (edit.kind == RackEditKind::removeSynth && candidate.sound.personalities)
            for (const auto& i : candidate.sound.personalities->instances) if (i.design.role == RackRole::fx)
                for (const auto& b : i.bindings) if (candidate.sound.layout.effects[b.slot].chain == edit.slot
                    && candidate.sound.layout.effects[b.slot].use == SlotUse::present) return ProposalStatus::invalidCandidate;
        switch (editRackLayout(candidate.sound.layout, edit, candidate.sound.layout)) {
            case RackEditResult::invalid: return ProposalStatus::invalidCandidate;
            case RackEditResult::capacityReached: return ProposalStatus::capacityReached;
            case RackEditResult::replacementRequired: return ProposalStatus::replacementRequired;
            case RackEditResult::applied: break;
        }
        if(trace&&(edit.kind==RackEditKind::addSynth||edit.kind==RackEditKind::removeSynth||edit.kind==RackEditKind::moveSynth)){
            const auto bank=edit.kind==RackEditKind::addSynth?candidate.sound.layout.synthOrder[edit.position]:edit.slot;
            std::uint16_t operation=0;
            if(!proposalSourceOrdinal(*trace,operation)||!appendProposalSourceEffect(*trace,bank,
                proposalSourceOwner(candidate.sound,bank,&beforeLayout),proposalSourceOwner(candidate.sound,bank),operation,
                edit.kind!=RackEditKind::moveSynth))return ProposalStatus::invalidCandidate;
        }
        // Preserve the legacy per-operation lock boundary without asking the
        // personality validator to approve unpublished partial ownership.
        auto numericBeforeStorage=std::make_unique<SoundDocument>(source.sound);
        auto numericAfterStorage=std::make_unique<SoundDocument>(candidate.sound);
        auto& numericBefore=*numericBeforeStorage;auto& numericAfter=*numericAfterStorage;
        numericBefore.personalities.reset(); numericAfter.personalities.reset();
        if (!validateAiSoundEdit(numericBefore,numericAfter)) return ProposalStatus::lockedDependency;
    }
    for (const auto& edit : edits.parameters) {
        const auto i = *parameterIndex(edit.id);
        auto value = edit.normalized;
        if (registry[i].kind != ParameterKind::continuous) {
            const auto steps = static_cast<float>(registry[i].choices.size() - 1);
            value = std::round(value * steps) / steps;
        }
        candidate.sound.values[i] = value;
    }
    for (const auto& edit : edits.macros) {
        candidate.sound.macros[edit.index - 1] = {edit.targetId.isEmpty() ? 0 : registry[*parameterIndex(edit.targetId)].stateKey, edit.amount};
    }
    if (edits.pattern) candidate.pattern = *edits.pattern;
    if (!proposalInReach(source.sound, candidate.sound, edits)) return ProposalStatus::outOfReach;
    RackDifference difference; auto reconciledStorage=std::make_unique<SoundDocument>();auto& reconciled=*reconciledStorage;
    const auto status = reconcileRackEdit(source.sound,candidate.sound,choices,SoundEditOrigin::aiCandidate,reconciled,difference);
    if (status != RackCandidateStatus::ready) return reconcileStatus(status);
    if(trace&&!appendProposalReconciliationEffects(candidate.sound,reconciled,choices,*trace))return ProposalStatus::invalidCandidate;
    candidate.sound=std::move(reconciled);
    candidate.sound.name = edits.name.isNotEmpty() ? edits.name : source.sound.name;
    if (source.sound.personalities) {
        // Manual/host reconciliation may infer recipe changes. AI must instead
        // preserve each snapshot except the requested choices and a move's fork.
        for (const auto& old : source.sound.personalities->instances) {
            const auto requested = [&](const auto& ids) { return std::find(ids.begin(), ids.end(), old.id) != ids.end(); };
            if (requested(choices.detach) || requested(choices.remove) || requested(choices.replace)) continue;
            auto expected = old; expected.design.panel.controls.clear(); bool resolved = false;
            for (std::size_t n = 0; n < old.design.panel.controls.size(); ++n) {
                const auto r = std::find_if(choices.controls.begin(), choices.controls.end(), [&](const auto& c) {
                    return c.instance == old.id && c.control == n;
                });
                if (r == choices.controls.end()) expected.design.panel.controls.push_back(old.design.panel.controls[n]);
                else { resolved = true; if (r->replacement) expected.design.panel.controls.push_back(*r->replacement); }
            }
            // A move forks the rack it moves and each rack it shifts, as the user's move does.
            const auto& a = source.sound.layout; const auto& b = candidate.sound.layout;
            const auto position = [](const auto& order, std::uint8_t slot) {
                return std::find(order.begin(), order.end(), slot) - order.begin();
            };
            bool relocated = false;
            for (const auto& binding : old.bindings) {
                const auto unit = std::find_if(old.design.units.begin(), old.design.units.end(),
                    [&](const auto& u) { return u.id == binding.unit; });
                if (unit == old.design.units.end()) return ProposalStatus::invalidCandidate;
                const auto slot = binding.slot;
                const auto from = a.effects[slot].chain, to = b.effects[slot].chain;
                relocated |= isSourceModule(unit->module) ? position(a.synthOrder, slot) != position(b.synthOrder, slot)
                                                       : from != to || position(a.effectOrder[from], slot) != position(b.effectOrder[to], slot);
            }
            if (resolved || relocated) {
                if (expected.design.revision == UINT64_MAX) return ProposalStatus::invalidCandidate;
                ++expected.design.revision; expected.edited = true;
            }
            const auto* current = selectedInstance(candidate.sound, old.id);
            if (!current || *current != expected) return ProposalStatus::scopeViolation;
        }
    }
    if (!validateProjectDocument(candidate)) return ProposalStatus::invalidCandidate;
    appendAutomationWarning(automationWarning,difference);
    output = candidate; return ProposalStatus::accepted;
}

ProposalStatus admitProposal(const ProjectDocument& source,const ProposalPolicy& policy,
    const ProposalEdits& edits,ProjectDocument& output,SourceAssignmentTrace* trace,bool* allocationFailed,juce::String* automationWarning)
{
    if(allocationFailed)*allocationFailed=false;
    try {
        if(trace&&trace->count>trace->effects.size())return ProposalStatus::invalidCandidate;
        auto staged=trace?std::make_unique<SourceAssignmentTrace>(*trace):nullptr;
        juce::String warnings;
        const auto status=admitProposalImpl(source,policy,edits,output,staged.get(),automationWarning?&warnings:nullptr);
        if(status==ProposalStatus::accepted){if(trace)*trace=*staged;if(automationWarning)*automationWarning=std::move(warnings);}return status;
    }catch(const std::bad_alloc&){if(allocationFailed)*allocationFailed=true;return ProposalStatus::invalidCandidate;}
    catch(...){return ProposalStatus::invalidCandidate;}
}

juce::var proposalSchema(const ProposalPolicy& policy)
{
    const auto integer = schemaType("integer"), number = schemaType("number"), text = schemaType("string");
    const auto version = schemaType("integer");
    version.getDynamicObject()->setProperty("enum", juce::Array<juce::var>{2});
    const auto note = schemaObject({{"start_beats", number}, {"duration_beats", number}, {"pitch", integer}, {"velocity", integer}, {"channel", integer}});
    const auto notes = schemaArray(note);
    if (policy.requirePattern) notes.getDynamicObject()->setProperty("minItems", 1);
    const auto pattern = schemaObject({{"bars", integer}, {"meter_numerator", integer}, {"meter_denominator", integer},
        {"grid", schemaEnum(gridNames)}, {"swing", number}, {"notes", notes}});
    auto schema = schemaObject({
        {"version", version},
        {"parameter_edits", schemaArray(schemaObject({{"id", text}, {"normalized", number}}))},
        {"rack_edits", schemaArray(schemaObject({{"kind", schemaEnum(rackNames)}, {"slot", integer}, {"chain", integer}, {"position", integer}, {"effect", schemaEnum(effectNames)}}))},
        {"macro_edits", schemaArray(schemaObject({{"index", integer}, {"target_id", object({{"type", juce::Array<juce::var>{"string", "null"}}})}, {"amount", number}}))},
        {"pattern", policy.requirePattern ? pattern : object({{"anyOf", juce::Array<juce::var>{pattern, schemaType("null")}}})},
        {"characteristics", text}, {"explanation", text}, {"name", text}});
    {
        const auto control = rackDesignSchema(false)["properties"]["panel"]["properties"]["controls"]["items"];
        const auto bounded = [&](juce::var item,int max) {auto a=schemaArray(item);a.getDynamicObject()->setProperty("maxItems",max);return a;};
        const auto reconciliation = schemaObject({{"detach",bounded(text,12)}, {"remove",bounded(text,12)}, {"replace",bounded(text,12)},
            {"controls",bounded(schemaObject({{"instance",text},{"control",integer},{"replacement",object({{"anyOf",juce::Array<juce::var>{control,schemaType("null")}}})}}),96)}});
        schema["properties"].getDynamicObject()->setProperty("reconciliation",reconciliation);
        schema["required"].getArray()->add("reconciliation");
    }
    return schema;
}

namespace
{
bool decimal(const juce::var& value, std::uint64_t& out)
{
    if (!value.isString()) return false;
    const auto s = value.toString().toStdString();
    if (s.empty() || (s.size() > 1 && s.front() == '0')) return false;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), out);
    return error == std::errc{} && end == s.data() + s.size();
}
bool hashValid(const juce::String& hash)
{ return hash.length() == 64 && hash.containsOnly("0123456789abcdef"); }
bool catalogText(const juce::var& value,std::size_t maximum)
{
    if(!value.isString())return false;const auto text=value.toString();
    if(text.trim().isEmpty()||!textValid(text)||text.getNumBytesAsUTF8()>maximum)return false;
    for(auto c:text)if(c==0||(c<32&&c!='\n'&&c!='\t'))return false;return true;
}
bool catalogUuid(const juce::var& value, juce::Uuid& out)
{
    if (!value.isString()) return false;
    out = juce::Uuid(value.toString());
    return !out.isNull() && out.toString() == value.toString();
}
bool descriptor(const CatalogAuthority& a, juce::var& root)
{
    if(!parseStrictJson(a.descriptorJson, root, maximumCatalogDescriptorBytes))return false;
    if(root.hasProperty("format"))return closed(root,{"format","complete","columns","tags","entries"})
        && root["format"].isInt()&&int(root["format"])==2&&root["complete"].isBool()&&bool(root["complete"])
        && root["columns"].isArray()&&*root["columns"].getArray()==juce::Array<juce::var>{"id","revision","sha256","name","role","tags","synthUnits","fxUnits","modules"}
        && root["tags"].isArray()&&root["tags"].size()<=128&&root["entries"].isArray()&&root["entries"].size()<=541;
    return closed(root, {"snapshot", "entries", "next", "complete"})
        && root["snapshot"].isString() && root["snapshot"].toString() == a.snapshot.toString()
        && root["complete"].isBool() && bool(root["complete"])
        && root["entries"].isArray() && root["entries"].getArray()->size() <= maximumRackCatalogEntries
        && root["next"].isString() && root["next"].toString() == juce::String(root["entries"].getArray()->size());
}
juce::var entryFor(const juce::var& root, const juce::Uuid& id, std::uint64_t revision, const juce::String& hash)
{
    if (const auto* entries = root["entries"].getArray()) for (const auto& e : *entries) {
        std::uint64_t r = 0;
        if(root.hasProperty("format")){
            if(e.isArray()&&e.size()==9&&e[0].toString()==id.toString()&&decimal(e[1],r)&&r==revision&&e[2].toString()==hash)
                return object({{"id",e[0]},{"revision",e[1]},{"content_sha256",e[2]},{"name",e[3]},{"role",e[4]},
                    {"capacity",object({{"synths",e[6]},{"effects",e[7]}})},{"modules",e[8]}});
            continue;
        }
        if (e["id"].toString() == id.toString() && decimal(e["revision"], r) && r == revision && e["content_sha256"].toString() == hash) return e;
    }
    return {};
}
bool localKey(const juce::var& row,std::uint8_t unit,const juce::String& key,CatalogRendering rendering)
{
    if (key.isEmpty() || key.getNumBytesAsUTF8() > rackParameterKeyBytes || !key.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.")) return false;
    if (const auto* units = row["units"].getArray()) for (const auto& u : *units) {
        int id, module;
        if (integer(u["id"], 0, 8, id) && id == unit && integer(u["module"],0,int(NativeModule::saturation),module)
            &&(rendering==CatalogRendering::ownedLocal||module!=int(NativeModule::drum)))
            return rackParameterActive(NativeModule(module), key) && rackParameterIndex(NativeModule(module), 0, key).has_value();
    }
    return false;
}
bool alias(const juce::String& text, std::uint8_t& selection, std::uint8_t& unit, juce::String& key)
{
    const auto parts = juce::StringArray::fromTokens(text, ":", "");
    std::uint64_t s, u;
    if (parts.size() != 4 || parts[0] != "catalog" || !decimal(juce::var(parts[1]), s)
        || !decimal(juce::var(parts[2]), u) || s > 255 || u > 255 || parts[3].isEmpty()) return false;
    selection = std::uint8_t(s); unit = std::uint8_t(u); key = parts[3]; return true;
}
const CatalogDesignSelection* selectionFor(const ProposalEdits& edits, std::uint8_t local)
{
    const auto i = std::find_if(edits.catalogSelections.begin(), edits.catalogSelections.end(), [&](const auto& s) { return s.localId == local; });
    return i == edits.catalogSelections.end() ? nullptr : &*i;
}
}
juce::Uuid catalogInstanceId(const RequestTag& root, std::uint8_t local)
{
    juce::MemoryOutputStream bytes;
    bytes.write("airack.catalog.instance.v1", 25); bytes.write(root.runtime.getRawData(), 16);
    bytes.writeInt64(juce::int64(root.requestId)); bytes.writeInt64(juce::int64(root.editRevision)); bytes.writeInt64(juce::int64(root.contextRevision));
    bytes.writeByte(char(local));
    const auto digest = juce::SHA256(bytes.getMemoryBlock()).getRawData();
    return juce::Uuid(static_cast<const std::uint8_t*>(digest.getData()));
}
bool detail::catalogAuthorityValid(const CatalogAuthority& a)
{
    try {
        if (a.rendering>CatalogRendering::ownedLocal||a.root.runtime.isNull() || a.root.requestId == 0 || a.snapshot.isNull() || a.definitions.size() > 12) return false;
        juce::var root; if (!descriptor(a, root)) return false;
        const bool compact=root.hasProperty("format");
        if(compact){
            std::set<juce::String> tags;
            for(const auto& tag:*root["tags"].getArray())
                if(!closed(tag,{"id","label"})||!tag["id"].isString()||!validRackTagId(tag["id"].toString())
                    ||!tags.insert(tag["id"].toString()).second||!catalogText(tag["label"],64))return false;
        }
        std::set<juce::String> identities;
        for (const auto& row : *root["entries"].getArray()) {
            juce::Uuid id; std::uint64_t revision;
            if(compact){
                if(!row.isArray()||row.size()!=9||!catalogUuid(row[0],id)||!decimal(row[1],revision)||!revision
                    ||!row[2].isString()||!hashValid(row[2].toString())||!identities.insert(id.toString()+":"+row[1].toString()).second
                    ||!catalogText(row[3],128)
                    ||!row[4].isString()||(row[4].toString()!="synth"&&row[4].toString()!="fx")||!row[5].isArray()||row[5].size()>8
                    ||!row[8].isArray()||row[8].size()==0||row[8].size()>9)return false;
                std::set<int> tags;for(const auto& tag:*row[5].getArray()){int n;if(!integer(tag,0,root["tags"].size()-1,n)||!tags.insert(n).second)return false;}
                int synths=0,effects=0;for(const auto& module:*row[8].getArray()){int n;if(!integer(module,0,int(NativeModule::saturation),n)
                    ||(a.rendering!=CatalogRendering::ownedLocal&&n==int(NativeModule::drum)))return false;isSourceModule(NativeModule(n))?++synths:++effects;}
                int s,e;if(!integer(row[6],0,1,s)||!integer(row[7],0,8,e)||s!=synths||e!=effects||(row[4].toString()=="synth"?s!=1:s!=0))return false;
                continue;
            }
            if (!closed(row, {"id", "revision", "content_sha256", "name", "intent", "role", "capacity", "parameter_columns", "units"})
                || !catalogUuid(row["id"], id) || !decimal(row["revision"], revision) || revision == 0
                || !row["content_sha256"].isString() || !hashValid(row["content_sha256"].toString())
                || !identities.insert(id.toString() + ":" + row["revision"].toString()).second
                || !row["name"].isString() || !row["intent"].isString() || !textValid(row["name"].toString()) || !textValid(row["intent"].toString())
                || !row["role"].isString() || (row["role"].toString() != "synth" && row["role"].toString() != "fx")
                || !closed(row["capacity"], {"synths", "effects"})) return false;
            const auto* columns = row["parameter_columns"].getArray();
            if (!columns || *columns != juce::Array<juce::var>{"key", "unit", "minimum", "maximum"}) return false;
            const auto* units = row["units"].getArray(); if (!units || units->isEmpty() || units->size() > 9) return false;
            std::set<int> ids; int synths = 0, effects = 0;
            for (const auto& u : *units) {
                int id, module, algorithm;
                if (!closed(u, {"id", "module", "algorithm_revision", "parameters"}) || !integer(u["id"], 0, 8, id)
                    || !ids.insert(id).second || !integer(u["module"],0,int(NativeModule::saturation),module)
                    ||(a.rendering!=CatalogRendering::ownedLocal&&module==int(NativeModule::drum))
                    || !integer(u["algorithm_revision"], 0, 255, algorithm) || !supportsAlgorithm(NativeModule(module), AlgorithmRevision(algorithm))) return false;
                isSourceModule(NativeModule(module)) ? ++synths : ++effects;
                const auto* params = u["parameters"].getArray();
                if (!params || params->size() > int(rackUnitParameterCapacity(NativeModule(module)))) return false;
                std::set<juce::String> keys;
                for (const auto& p : *params) {
                    const auto* tuple = p.getArray();
                    if (!tuple || tuple->size() != 4 || !(*tuple)[0].isString() || !keys.insert((*tuple)[0].toString()).second) return false;
                    const auto index = rackParameterIndex(NativeModule(module), 0, (*tuple)[0].toString()); if (!index) return false;
                    const auto& field = parameterRegistry()[*index]; double minimum, maximum;
                    if (!(*tuple)[1].isString() || (*tuple)[1].toString() != field.unit || !number((*tuple)[2], minimum)
                        || !number((*tuple)[3], maximum) || minimum != double(field.minimum) || maximum != double(field.maximum)) return false;
                }
            }
            int s, e;
            if (!integer(row["capacity"]["synths"], 0, 1, s) || !integer(row["capacity"]["effects"], 0, 8, e)
                || s != synths || e != effects || (row["role"].toString() == "synth" ? s != 1 : s != 0)) return false;
        }
        std::set<juce::String> definitions;
        for (const auto& d : a.definitions) {
            if (d.design.isNull() || d.revision == 0 || !hashValid(d.contentSha256) || d.canonical.getSize() == 0 || d.canonical.getSize() > 65536
                || !definitions.insert(d.design.toString() + ":" + juce::String(std::to_string(d.revision))).second
                || juce::SHA256(d.canonical).toHexString() != d.contentSha256) return false;
            RackDesign design; juce::MemoryBlock canonical;
            if (!decodeRackDesign(d.canonical.getData(), d.canonical.getSize(), design, RackValidationMode::instanceRecall)
                || design.id != d.design || design.revision != d.revision || !encodeRackDesign(design, canonical, RackValidationMode::instanceRecall)
                || canonical != d.canonical) return false;
            const auto row = entryFor(root, d.design, d.revision, d.contentSha256);
            if(row.isVoid())return false;
            if(compact){
                juce::Array<juce::var> modules;int synths=0,effects=0;for(const auto& unit:design.units){modules.add(int(unit.module));isSourceModule(unit.module)?++synths:++effects;}
                if(row["name"].toString()!=design.name||row["role"].toString()!=(design.role==RackRole::synth?"synth":"fx")
                    ||int(row["capacity"]["synths"])!=synths||int(row["capacity"]["effects"])!=effects||!row["modules"].isArray()||*row["modules"].getArray()!=modules)return false;
            }else if(juce::JSON::toString(row,true,22)!=juce::JSON::toString(rackCatalogDescriptor({design,d.contentSha256}),true,22))return false;
        }
        return true;
    } catch (...) { return false; }
}

bool detail::catalogIntentValid(const ProposalEdits& edits, const ProposalPolicy& policy, const CatalogAuthority& authority)
{
    if (!catalogAuthorityValid(authority) || edits.catalogSelections.size() > 12 || edits.catalogParameters.size() > maximumCatalogParameterEdits) return false;
    auto plain = edits; plain.catalogSelections.clear(); plain.catalogParameters.clear();
    for (auto& m : plain.macros) if (m.targetId.startsWith("catalog:")) m.targetId = "output.gain_db";
    if (editsValid(plain) != ProposalStatus::accepted) return false;
    if ((!edits.catalogSelections.empty() || !edits.catalogParameters.empty())
        && policy.scope == RequestScope::pattern) return false;
    juce::var root; if (!descriptor(authority, root)) return false;
    std::set<int> selections;
    for (const auto& s : edits.catalogSelections) {
        const auto row = entryFor(root, s.design, s.revision, s.contentSha256);
        if (s.design.isNull() || !selections.insert(s.localId).second || row.isVoid()
            || s.destination.role > RackRole::fx || s.destination.chain > masterChain || s.destination.position > 8
            || (s.destination.role == RackRole::synth && s.destination.chain != masterChain)
            || row["role"].toString() != (s.destination.role == RackRole::synth ? "synth" : "fx")) return false;
    }
    auto keyValid = [&](std::uint8_t local, std::uint8_t unit, const juce::String& key) {
        const auto* s = selectionFor(edits, local);
        if(!s)return false;
        if(!root.hasProperty("format"))return localKey(entryFor(root,s->design,s->revision,s->contentSha256),unit,key,authority.rendering);
        const auto definition=std::find_if(authority.definitions.begin(),authority.definitions.end(),[&](const auto& d){return d.design==s->design&&d.revision==s->revision&&d.contentSha256==s->contentSha256;});
        if(definition!=authority.definitions.end()){
            RackDesign design;if(!decodeRackDesign(definition->canonical.getData(),definition->canonical.getSize(),design,RackValidationMode::instanceRecall))return false;
            return localKey(rackCatalogDescriptor({design,s->contentSha256}),unit,key,authority.rendering);
        }
        if(unit>8)return false;
        const auto row=entryFor(root,s->design,s->revision,s->contentSha256);
        if(const auto* modules=row["modules"].getArray())for(const auto& m:*modules)
            if(rackParameterActive(NativeModule(int(m)),key)&&rackParameterIndex(NativeModule(int(m)),0,key))return true;
        return false;
    };
    std::set<std::tuple<unsigned, unsigned, juce::String>> tuples;
    for (const auto& p : edits.catalogParameters)
        if (!std::isfinite(p.normalized) || p.normalized < 0 || p.normalized > 1 || !keyValid(p.selection, p.unit, p.key)
            || !tuples.emplace(p.selection, p.unit, p.key).second) return false;
    for (const auto& m : edits.macros) if (m.targetId.startsWith("catalog:")) {
        std::uint8_t local, unit; juce::String key;
        if (!alias(m.targetId, local, unit, key) || !keyValid(local, unit, key)) return false;
    }
    return true;
}

namespace {
bool readCatalogEdits(const juce::var& root,const ProposalPolicy& policy,const CatalogAuthority& authority,ProposalEdits& candidate,ProposalDecodeReason* reason) {
    const juce::Array<juce::var> empty;
    const auto* selections=root.hasProperty("catalog_selections")?root["catalog_selections"].getArray():&empty;
    const auto* parameters=root.hasProperty("catalog_parameter_edits")?root["catalog_parameter_edits"].getArray():&empty;
    if(!selections||selections->size()>12||!parameters||parameters->size()>maximumCatalogParameterEdits)return false;
    if (reason) *reason = ProposalDecodeReason::catalogSelection;
    for (const auto& value : *selections) {
        CatalogDesignSelection s; int local, chain, position;
        // The design is named as the catalog lists it or in the dashed form the base intent uses.
        if (!closed(value, {"local_id", "design", "revision", "content_sha256", "destination"}) || !integer(value["local_id"], 0, 255, local)
            || !(uuid(value["design"], s.design) || catalogUuid(value["design"], s.design)) || !decimal(value["revision"], s.revision) || s.revision == 0 || !value["content_sha256"].isString()
            || !hashValid(value["content_sha256"].toString()) || !closed(value["destination"], {"role", "chain", "position"})
            || !value["destination"]["role"].isString() || !integer(value["destination"]["chain"], 0, 4, chain)
            || !integer(value["destination"]["position"], 0, 8, position)) return false;
        const auto role = value["destination"]["role"].toString(); if (role != "synth" && role != "fx") return false;
        s.localId = std::uint8_t(local); s.contentSha256 = value["content_sha256"].toString();
        s.destination = {role == "synth" ? RackRole::synth : RackRole::fx, std::uint8_t(chain), std::uint8_t(position)}; candidate.catalogSelections.push_back(std::move(s));
    }
    if (reason) *reason = ProposalDecodeReason::catalogParameter;
    for (const auto& value : *parameters) {
        CatalogParameterEdit p; int local, unit;
        if (!closed(value, {"selection", "unit", "key", "normalized"}) || !integer(value["selection"], 0, 255, local)
            || !integer(value["unit"], 0, 255, unit) || !value["key"].isString() || !real(value["normalized"], 0, 1, p.normalized)) return false;
        p.selection = std::uint8_t(local); p.unit = std::uint8_t(unit); p.key = value["key"].toString(); candidate.catalogParameters.push_back(std::move(p));
    }
    if (reason) *reason = ProposalDecodeReason::catalogIntent;
    if (!detail::catalogIntentValid(candidate, policy, authority)) return false;
    return true;
}
}
bool decodeProposal(std::string_view bytes, const ProposalPolicy& policy, const CatalogAuthority& authority, ProposalEdits& output, ProposalDecodeReason* reason)
{
    if (reason) *reason = ProposalDecodeReason::rootFields;
    juce::var root; int version;
    if (!detail::catalogAuthorityValid(authority) || !parseStrictJson(bytes, root, maximumProposalBytes)
        || !closed(root, {"version", "parameter_edits", "rack_edits", "macro_edits", "pattern", "characteristics", "explanation", "name", "reconciliation", "catalog_selections", "catalog_parameter_edits"})
        || !integer(root["version"], 2, 2, version)) return false;
    const auto* selections = root["catalog_selections"].getArray(); const auto* parameters = root["catalog_parameter_edits"].getArray();
    if (!selections || selections->size() > 12 || !parameters || parameters->size() > maximumCatalogParameterEdits) return false;
    auto legacy = root.clone(); auto* object = legacy.getDynamicObject(); object->removeProperty("catalog_selections"); object->removeProperty("catalog_parameter_edits");
    std::vector<std::pair<int, juce::String>> aliases;
    if (auto* macros = legacy["macro_edits"].getArray()) for (int i = 0; i < macros->size(); ++i) {
        auto& m = macros->getReference(i);
        if (m["target_id"].isString() && m["target_id"].toString().startsWith("catalog:")) {
            aliases.emplace_back(i, m["target_id"].toString()); m.getDynamicObject()->setProperty("target_id", "output.gain_db");
        }
    }
    ProposalEdits candidate;
    if (!decodeProposal(juce::JSON::toString(legacy, true, 22).toStdString(), policy, candidate, reason)) return false;
    for (const auto& [i, name] : aliases) candidate.macros[std::size_t(i)].targetId = name;
    if(!readCatalogEdits(root,policy,authority,candidate,reason))return false;
    output = std::move(candidate); if (reason) *reason = ProposalDecodeReason::accepted; return true;
}

static ProposalStatus admitCatalogProposal(const ProjectDocument& source, const ProposalPolicy& policy, const ProposalEdits& edits, const CatalogAuthority& authority,const RackInventorySnapshot* inventory, ProjectDocument& output,SourceAssignmentTrace* trace,bool* allocationFailed,juce::String* automationWarning)
{
    if(allocationFailed)*allocationFailed=false;
    try {
        if(trace&&trace->count>trace->effects.size())return ProposalStatus::invalidCandidate;
        auto journal=trace?std::make_unique<SourceAssignmentTrace>(*trace):nullptr;
        juce::String warnings;auto* collected=automationWarning?&warnings:nullptr;
        if (!validateProjectDocument(source)) return ProposalStatus::invalidCandidate;
        if(inventory&&authority.snapshot!=inventory->id())return ProposalStatus::invalidCandidate;
        if ((!edits.catalogSelections.empty() || !edits.catalogParameters.empty()) && policy.scope == RequestScope::pattern) return ProposalStatus::scopeViolation;
        if (!detail::catalogIntentValid(edits, policy, authority)) return ProposalStatus::malformed;
        if (!edits.macros.empty() && !policy.allowMacroChanges) return ProposalStatus::macroChangeNotRequested;
        if (edits.catalogSelections.empty()){
            const auto status=admitProposalImpl(source,policy,edits,output,journal.get(),collected);
            if(status==ProposalStatus::accepted){if(trace)*trace=*journal;if(automationWarning)*automationWarning=std::move(warnings);}return status;
        }
        auto structural = edits; structural.parameters.clear(); structural.macros.clear(); structural.pattern.reset();
        structural.catalogSelections.clear(); structural.catalogParameters.clear();
        auto structuralPolicy = policy; structuralPolicy.requirePattern = false;
        auto stagedStorage=std::make_unique<ProjectDocument>();auto& staged=*stagedStorage;
        auto status = admitProposalImpl(source, structuralPolicy, structural, staged,journal.get(),collected); if (status != ProposalStatus::accepted) return status;
        std::vector<RackDesign> embedded;
        if (source.sound.personalities) for (const auto& i : source.sound.personalities->instances) embedded.push_back(i.design);
        RackCatalogSnapshot native;
        if (!inventory&&!captureRackCatalog({}, embedded, {}, native)) return ProposalStatus::invalidCandidate;
        for (const auto& s : edits.catalogSelections) {
            const auto d = std::find_if(authority.definitions.begin(), authority.definitions.end(), [&](const auto& d) { return d.design == s.design && d.revision == s.revision && d.contentSha256 == s.contentSha256; });
            if (d == authority.definitions.end()) return ProposalStatus::invalidCandidate;
            RackDesign design; if (!decodeRackDesign(d->canonical.getData(), d->canonical.getSize(), design, RackValidationMode::instanceRecall)) return ProposalStatus::invalidCandidate;
            const auto id = catalogInstanceId(authority.root, s.localId);
            if (id.isNull() || selectedInstance(staged.sound, id)) return ProposalStatus::invalidCandidate;
            RackOperation op; op.newInstance = id; op.destination = s.destination; op.allowSlotReuse = policy.allowSlotReuse;
            auto placedStorage=std::make_unique<SoundDocument>();auto& placed=*placedStorage;RackDifference difference;
            const auto trusted = std::find_if(native.entries.begin(), native.entries.end(), [&](const auto& e) { return e.definition == design && e.contentSha256 == d->contentSha256; });
            RackCandidateStatus placement;
            if(inventory)
                placement=makeCatalogRackCandidate(staged.sound,*inventory,design.id,design.revision,d->contentSha256,op,SoundEditOrigin::aiCandidate,placed,difference,journal.get());
            else if (trusted != native.entries.end() && rackCatalogNativeEntry(native, std::size_t(trusted - native.entries.begin())))
                placement = makeCatalogRackCandidate(staged.sound, native, design.id, design.revision, d->contentSha256, op, SoundEditOrigin::aiCandidate, placed, difference,journal.get());
            else {
                if (!validateRackDesign(design, RackValidationMode::portableDesign)) return ProposalStatus::invalidCandidate;
                op.design = design; placement = makeRackCandidate(staged.sound, op, SoundEditOrigin::aiCandidate, placed, difference,journal.get());
            }
            if (placement != RackCandidateStatus::ready) return reconcileStatus(placement);
            appendAutomationWarning(collected,difference);
            staged.sound = std::move(placed);
        }
        auto lowered = edits; lowered.catalogSelections.clear(); lowered.catalogParameters.clear(); lowered.racks.clear(); lowered.reconciliation = {};
        auto target = [&](std::uint8_t local, std::uint8_t unit, const juce::String& key) -> juce::String {
            const auto* i = selectedInstance(staged.sound, catalogInstanceId(authority.root, local)); if (!i) return {};
            const auto u = std::find_if(i->design.units.begin(), i->design.units.end(), [&](const auto& u) { return u.id == unit; });
            const auto* b = rackUnitBinding(*i, unit); if (u == i->design.units.end() || !b) return {};
            const auto index = rackParameterIndex(u->module, b->slot, key); return index ? parameterRegistry()[*index].id : juce::String();
        };
        std::set<juce::String> keys; for (const auto& p : lowered.parameters) keys.insert(p.id);
        for (const auto& p : edits.catalogParameters) {
            const auto id = target(p.selection, p.unit, p.key); if (id.isEmpty() || !keys.insert(id).second) return ProposalStatus::malformed;
            lowered.parameters.push_back({id, p.normalized});
        }
        for (auto& m : lowered.macros) if (m.targetId.startsWith("catalog:")) {
            std::uint8_t local, unit; juce::String key;
            if (!alias(m.targetId, local, unit, key) || (m.targetId = target(local, unit, key)).isEmpty()) return ProposalStatus::malformed;
        }
        auto candidateStorage=std::make_unique<ProjectDocument>();auto& candidate=*candidateStorage;
        status = admitProposalImpl(staged, policy, lowered, candidate,journal.get(),collected); if (status != ProposalStatus::accepted) return status;
        if (!validateAiSoundEdit(source.sound, candidate.sound)) return ProposalStatus::lockedDependency;
        output = std::move(candidate);if(trace)*trace=*journal;if(automationWarning)*automationWarning=std::move(warnings);return ProposalStatus::accepted;
    } catch(const std::bad_alloc&){if(allocationFailed)*allocationFailed=true;return ProposalStatus::invalidCandidate;}
    catch (...) { return ProposalStatus::invalidCandidate; }
}

ProposalStatus admitProposal(const ProjectDocument& source,const ProposalPolicy& policy,const ProposalEdits& edits,const CatalogAuthority& authority,ProjectDocument& output,SourceAssignmentTrace* trace,bool* allocationFailed,juce::String* automationWarning)
{return admitCatalogProposal(source,policy,edits,authority,nullptr,output,trace,allocationFailed,automationWarning);}
ProposalStatus admitProposal(const ProjectDocument& source,const ProposalPolicy& policy,const ProposalEdits& edits,const CatalogAuthority& authority,const RackInventorySnapshot& inventory,ProjectDocument& output,SourceAssignmentTrace* trace,bool* allocationFailed,juce::String* automationWarning)
{return admitCatalogProposal(source,policy,edits,authority,&inventory,output,trace,allocationFailed,automationWarning);}

juce::var proposalSchema(const ProposalPolicy& policy, const CatalogAuthority& authority)
{
    if (!detail::catalogAuthorityValid(authority)) return {};
    auto schema = proposalSchema(policy);
    const auto integer = schemaType("integer"), text = schemaType("string"), number = schemaType("number");
    const auto selections = schemaArray(schemaObject({{"local_id", integer}, {"design", text}, {"revision", text}, {"content_sha256", text},
        {"destination", schemaObject({{"role", object({{"type", "string"}, {"enum", juce::Array<juce::var>{"synth", "fx"}}})}, {"chain", integer}, {"position", integer}})}}));
    selections.getDynamicObject()->setProperty("maxItems", 12);
    const auto parameters = schemaArray(schemaObject({{"selection", integer}, {"unit", integer}, {"key", text}, {"normalized", number}}));
    parameters.getDynamicObject()->setProperty("maxItems", int(maximumCatalogParameterEdits));
    schema["properties"].getDynamicObject()->setProperty("catalog_selections", selections);
    schema["properties"].getDynamicObject()->setProperty("catalog_parameter_edits", parameters);
    schema["required"].getArray()->add("catalog_selections"); schema["required"].getArray()->add("catalog_parameter_edits"); return schema;
}
bool decodeControlEdits(std::string_view bytes,const ProposalPolicy& policy,const CatalogAuthority* authority,ProposalEdits& output,ProposalDecodeReason* reason)
{
    if(reason)*reason=ProposalDecodeReason::strictJson;
    juce::var root;if(!parseStrictJson(bytes,root,maximumProposalBytes))return false;
    if(reason)*reason=ProposalDecodeReason::rootFields;
    const auto* object=root.getDynamicObject();if(!object)return false;
    for(const auto& field:object->getProperties())if(field.name.toString()!="parameter_edits"&&field.name.toString()!="rack_edits"&&field.name.toString()!="macro_edits"&&field.name.toString()!="pattern"
        &&field.name.toString()!="characteristics"&&field.name.toString()!="explanation"&&field.name.toString()!="name"&&field.name.toString()!="reconciliation"
        &&field.name.toString()!="catalog_selections"&&field.name.toString()!="catalog_parameter_edits")return false;
    ProposalEdits candidate;
    if(!readBaseEdits(root,candidate,reason,authority!=nullptr))return false;
    if(root.hasProperty("name")){
        if(reason)*reason=ProposalDecodeReason::invalidName;
        if(!root["name"].isString()||!soundNameValid(root["name"].toString().trim()))return false;
        candidate.name=root["name"].toString().trim();
    }
    if(root.hasProperty("reconciliation")&&!readReconciliation(root,candidate,reason))return false;
    if(authority){if(!detail::catalogAuthorityValid(*authority)||!readCatalogEdits(root,policy,*authority,candidate,reason))return false;}
    else if(root.hasProperty("catalog_selections")||root.hasProperty("catalog_parameter_edits"))return false;
    output=std::move(candidate);if(reason)*reason=ProposalDecodeReason::accepted;return true;
}
juce::var controlEditsSchema(const ProposalPolicy& policy,const CatalogAuthority* authority)
{
    auto schema=authority?proposalSchema(policy,*authority):proposalSchema(policy);
    if(!schema.getDynamicObject())return {};
    schema["properties"].getDynamicObject()->removeProperty("version");
    schema.getDynamicObject()->setProperty("required",juce::Array<juce::var>());
    return schema;
}
}

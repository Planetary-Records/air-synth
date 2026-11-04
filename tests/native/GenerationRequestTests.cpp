#include "FrozenRackExamples.h"
#include "GenerationRequest.h"
#include "AiCatalog.h"
#include "StrictJson.h"
#include "RackReconciliation.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include "GroupPerformance.h"
#include "UserReach.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

using namespace airack;
using namespace airack::ai;
namespace
{
int checks = 0, failures = 0, sections = 0;
const char* section = "";
void check(bool pass, const char* expression, int line)
{
    ++checks;
    if (!pass) { ++failures; if (failures < 90) std::cerr << section << ':' << line << ' ' << expression << '\n'; }
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
juce::var parse(std::string_view text)
{
    juce::var result; CHECK(parseStrictJson(text, result, 1024 * 1024)); return result;
}
juce::var wire()
{
    return object({{"version", 1}, {"parameter_edits", array()}, {"rack_edits", array()},
        {"macro_edits", array()}, {"pattern", {}}, {"characteristics", "warm"}, {"explanation", "explicit edits"}});
}
juce::var param(const char* id, double value) { return object({{"id", id}, {"normalized", value}}); }
juce::var note(double start, double duration, int pitch = 60, int velocity = 96, int channel = 1)
{ return object({{"start_beats", start}, {"duration_beats", duration}, {"pitch", pitch}, {"velocity", velocity}, {"channel", channel}}); }
juce::var phraseWire(bool empty = false)
{
    return object({{"bars", 1}, {"meter_numerator", 3}, {"meter_denominator", 8}, {"grid", "eighth"},
        {"swing", .35}, {"notes", empty ? array() : array({note(.25, .125, 65, 87, 2)})}});
}
void providerPattern(const juce::var& actual, const juce::var& literal)
{
    // Saved/audition semantics survive compact row encoding; object-per-note expansion does not.
    for (auto key : {"bars", "meter_numerator", "meter_denominator", "swing"}) CHECK(actual[key] == literal[key]);
    constexpr const char* grids[]{"quarter", "eighth", "sixteenth", "thirty_second", "sixty_fourth"};
    int grid = -1; for (int i = 0; i < 5; ++i) if (literal["grid"].toString() == grids[i]) grid = i;
    CHECK(actual["grid"].isInt() && int(actual["grid"]) == grid);
    const auto* rows = actual["notes"].getArray(); const auto* notes = literal["notes"].getArray();
    CHECK(rows && notes && rows->size() == notes->size()); if (!rows || !notes || rows->size() != notes->size()) return;
    for (int n = 0; n < rows->size(); ++n) {
        const auto* row = (*rows)[n].getArray(); CHECK(row && row->size() == 5); if (!row || row->size() != 5) continue;
        const char* keys[]{"start_beats", "duration_beats", "pitch", "velocity", "channel"};
        for (int c = 0; c < 5; ++c) CHECK(double((*row)[c]) == double((*notes)[n][keys[c]]));
        CHECK(juce::JSON::toString((*rows)[n], true, 17).getNumBytesAsUTF8() + 1 <= 64);
    }
}
rack::oauth::StructuredResult completed(std::string_view text)
{
    rack::oauth::StructuredResult r;
    r.result.status = rack::oauth::Status::connected; r.result.httpStatus = 200;
    r.text = juce::String::fromUTF8(text.data(), static_cast<int>(text.size())); return r;
}
StructuredCall reply(const juce::var& v)
{
    const auto text = json(v);
    return [text](const juce::String&, std::string_view, const juce::var&) { return completed(text); };
}
std::size_t index(const char* id)
{
    const auto r = parameterRegistry();
    const auto p = std::find_if(r.begin(), r.end(), [&](const auto& field) { return field.id == id; });
    CHECK(p != r.end()); return p == r.end() ? 0 : std::size_t(p - r.begin());
}
void physical(ProjectDocument& d, const char* id, float value)
{
    const auto i = index(id); const auto& f = parameterRegistry()[i];
    d.sound.values[i] = juce::NormalisableRange<float>(f.minimum, f.maximum, 0, f.skew).convertTo0to1(value);
}
GenerationRequest request()
{
    GenerationRequest r;
    r.tag = {juce::Uuid("11223344556677889900aabbccddeeff"), 917, 813, 719};
    r.instruction = "Warm rounded sound"; r.model = "account-selected_model:1.0";
    r.capturedPhrase = PatternDocument{}; r.capturedBpm = 400;
    return r;
}
bool ready(const GenerationResult& r, unsigned calls = 1)
{
    CHECK(r.status == GenerationStatus::ready); CHECK(r.inferenceCalls == calls);
    CHECK(r.candidate.has_value()); CHECK(!r.audio.left.empty() && r.audio.left.size() == r.audio.right.size());
    return r.status == GenerationStatus::ready && r.candidate && !r.audio.left.empty();
}
void withheld(const GenerationResult& r, GenerationStatus status, unsigned calls)
{
    CHECK(r.status == status); CHECK(r.inferenceCalls == calls); CHECK(!r.candidate);
    CHECK(r.edits.parameters.empty() && r.edits.racks.empty() && r.edits.macros.empty() && !r.edits.pattern);
    CHECK(r.edits.characteristics.isEmpty() && r.edits.explanation.isEmpty());
    CHECK(r.audio.left.empty() && r.audio.right.empty());
    CHECK(r.edits.reconciliation.detach.empty() && r.edits.reconciliation.remove.empty()
        && r.edits.reconciliation.replace.empty() && r.edits.reconciliation.controls.empty());
}
void full_captured_request()
{
    begin(__func__); // Missing musical authority/catalog or leaked local identities breaks these checks.
    auto r = request(); r.policy = {RequestScope::both, true, true};
    // Established saved key, before any registry call: validation must bootstrap lookup.
    r.source.sound.macros[0] = {0x31924c5b, .25f};
    r.instruction = "Cr\xc3\xa8me \xf0\x9f\x8e\xb9"; r.role = "lead"; r.key = "D"; r.scale = "minor";
    r.capturedPhrase->meter = {3, 8}; r.capturedPhrase->grid = PatternGrid::eighth; r.capturedPhrase->swing = .35;
    r.capturedPhrase->noteCount = 1; r.capturedPhrase->notes[0] = {.25, .125, 65, 87, 2}; r.capturedBpm = 143;
    const auto original = r.source; unsigned seen = 0;
    auto w = wire(); w.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", .5)}));
    const auto result = generateProposal(r, [&](const juce::String& model, std::string_view prompt, const juce::var& schema)
    {
        ++seen; CHECK(model == r.model); CHECK(prompt.size() <= 1048576); CHECK(json(schema) == json(proposalSchema()));
        const auto p = parse(prompt); auto* root = p.getDynamicObject(); CHECK(root != nullptr);
        CHECK(p["catalog"]["rack_catalog"]["entries"].isArray()
            && p["catalog"]["rack_catalog"]["entries"].size() == 0);
        CHECK(int(p["version"]) == 1); CHECK(p["instruction"].toString().toStdString() == r.instruction);
        const auto* synths = p["catalog"]["racks"]["synths"].getArray();
        const auto* effects = p["catalog"]["racks"]["effects"].getArray();
        const auto* macros = p["catalog"]["macro_bindings"].getArray();
        CHECK(synths && synths->size() == 4 && effects && effects->size() == 8 && macros && macros->size() == 8);
        if (synths) for (const auto& row : *synths) CHECK(row.getArray() && row.getArray()->size() == 4);
        if (effects) for (const auto& row : *effects) CHECK(row.getArray() && row.getArray()->size() == 6);
        if (macros) for (const auto& row : *macros) CHECK(row.getArray() && row.getArray()->size() == 3);
        if (macros && macros->size() == 8 && (*macros)[0].getArray() && (*macros)[0].getArray()->size() == 3) {
            const auto targetId = (*macros)[0].getArray()->getReference(1).toString();
            const auto target = std::find_if(parameterRegistry().begin(), parameterRegistry().end(), [&](const auto& d) { return d.id == targetId; });
            CHECK(target != parameterRegistry().end());
            if (target != parameterRegistry().end()) CHECK(target->stateKey == original.sound.macros[0].targetKey);
            CHECK(double((*macros)[0].getArray()->getReference(2)) == .25);
        }
        CHECK(p["repair"].isVoid()); CHECK(p["output_rules"].toString().containsIgnoreCase("integer"));
        CHECK(p["policy"]["scope"].toString() == "both"); CHECK(bool(p["policy"]["allow_macro_changes"])); CHECK(bool(p["policy"]["allow_slot_reuse"]));
        CHECK(p["context"]["role"].toString() == "lead" && p["context"]["key"].toString() == "D" && p["context"]["scale"].toString() == "minor");
        CHECK(double(p["context"]["bpm"]) == 143); CHECK(p["context"]["phrase_source"].toString() == "captured"); CHECK(p["context"]["tempo_source"].toString() == "captured");
        providerPattern(p["context"]["phrase"], phraseWire());
        // The rows are the source's reach, ascending.
        const auto reach = userReach(original.sound); std::vector<std::size_t> reached;
        for (std::size_t i = 0; i < parameterCount; ++i) if (reach[i]) reached.push_back(i);
        const auto* controls = p["catalog"]["controls"].getArray(); CHECK(controls && controls->size() == int(reached.size()));
        if (controls && controls->size() == int(reached.size())) for (int n = 0; n < int(reached.size()); ++n)
        {
            const auto i = reached[std::size_t(n)]; const auto& field = parameterRegistry()[i]; const auto& c = (*controls)[n];
            const auto* row = c.getArray(); CHECK(row && row->size() == 11); if (!row || row->size() != 11) continue;
            CHECK((*row)[0].toString() == field.id); CHECK(float(double((*row)[8])) == original.sound.values[i]);
            CHECK(float(double((*row)[3])) == field.minimum && float(double((*row)[4])) == field.maximum);
        }
        for (auto name : {"runtime", "requestId", "editRevision", "contextRevision", "process_id", "user_id", "credentials", "apply", "peer_registration"}) CHECK(!p.hasProperty(name));
        CHECK(prompt.find("11223344556677889900aabbccddeeff") == std::string_view::npos);
        return completed(json(w));
    });
    CHECK(seen == 1); CHECK(result.tag == r.tag); CHECK(r.source == original);
    if (ready(result)) { auto expected = original; expected.sound.values[index("output.gain_db")] = .5f; CHECK(*result.candidate == expected); CHECK(result.edits.parameters == std::vector<ParameterEdit>({{"output.gain_db", .5f}})); CHECK(result.audio.context.phrase == *r.capturedPhrase); CHECK(result.audio.context.bpm == 143); }
    for (auto scope : {RequestScope::sound, RequestScope::pattern, RequestScope::both})
    {
        r.policy = {scope, false, false};
        const auto result2 = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var&) {
            const auto p = parse(prompt); CHECK(p["policy"]["scope"].toString() == (scope == RequestScope::sound ? "sound" : scope == RequestScope::pattern ? "pattern" : "both")); CHECK(!bool(p["policy"]["allow_macro_changes"]) && !bool(p["policy"]["allow_slot_reuse"])); return completed(json(wire())); });
        ready(result2);
    }
}
void model_visible_pattern_contract()
{
    begin(__func__); // Missing model-visible schema/field semantics or substituting audition notes breaks this contract.
    auto r = request(); r.policy.scope = RequestScope::both;
    r.instruction = "a rolling bassline in the style of John Summit";
    r.role = "bass"; r.key = "A"; r.scale = "minor"; r.capturedBpm = 123;
    r.capturedPhrase.reset(); r.source.mode = PlaybackMode::externalMidi;
    const auto original = r.source;
    PatternDocument expected; expected.bars = 1; expected.meter = {4, 4}; expected.grid = PatternGrid::sixteenth;
    expected.swing = .1; expected.noteCount = 4;
    expected.notes[0] = {0, .25, 45, 96, 1}; expected.notes[1] = {.5, .25, 45, 88, 1};
    expected.notes[2] = {1, .25, 48, 92, 1}; expected.notes[3] = {1.5, .25, 43, 84, 1};
    auto proposal = wire(); proposal.getDynamicObject()->setProperty("pattern", object({
        {"bars", 1}, {"meter_numerator", 4}, {"meter_denominator", 4}, {"grid", "sixteenth"}, {"swing", .1},
        {"notes", array({note(0, .25, 45, 96), note(.5, .25, 45, 88), note(1, .25, 48, 92), note(1.5, .25, 43, 84)})}}));
    unsigned seen = 0;
    const auto result = generateProposal(r, [&](const juce::String& model, std::string_view prompt, const juce::var& schema)
    {
        ++seen; CHECK(model == r.model);
        const auto p = parse(prompt);
        CHECK(json(p["proposal_schema"]) == json(schema)); CHECK(json(p["proposal_schema"]) == json(proposalSchema()));
        CHECK(p["instruction"].toString() == "a rolling bassline in the style of John Summit");
        CHECK(p["policy"]["scope"].toString() == "both");
        CHECK(!bool(p["policy"]["allow_macro_changes"]) && !bool(p["policy"]["allow_slot_reuse"]));
        const auto context = p["context"];
        CHECK(context["role"].toString() == "bass" && context["key"].toString() == "A" && context["scale"].toString() == "minor");
        CHECK(double(context["bpm"]) == 123 && context["tempo_source"].toString() == "captured");
        CHECK(context["phrase_source"].toString() == "default_notes");
        providerPattern(context["phrase"], object({{"bars", 1}, {"meter_numerator", 4}, {"meter_denominator", 4},
            {"grid", "sixteenth"}, {"swing", 0.}, {"notes", array({note(0, .25), note(1, .25), note(2, .25), note(3, .25)})}}));
        const auto rules = p["output_rules"].toString();
        CHECK(rules.contains("conforming to proposal_schema"));
        CHECK(rules.containsIgnoreCase("pattern null preserves saved notes"));
        CHECK(rules.contains("a pattern object replaces editable notes"));
        CHECK(rules.contains("empty notes explicitly clear saved notes"));
        CHECK(rules.contains("quarter-note beats")); CHECK(rules.contains("MIDI pitch, velocity and channel"));
        CHECK(rules.contains("bounded plain JSON integer tokens")); CHECK(rules.contains("default_notes are audition-only"));
        CHECK(rules.contains("instruction requests a phrase and policy permits pattern edits"));
        CHECK(rules.contains("When the instruction describes a different sound rather than a change to the current one, choose the library racks that fit it best"));
        CHECK(rules.contains("provide the phrase in pattern instead of directions in explanation"));
        return completed(json(proposal));
    });
    CHECK(seen == 1 && result.tag == r.tag); CHECK(r.source == original && original.pattern.noteCount == 0);
    if (ready(result))
    {
        auto candidate = original; candidate.pattern = expected;
        CHECK(*result.candidate == candidate); CHECK(result.edits.pattern && *result.edits.pattern == expected);
        CHECK(result.audio.context.phrase == expected && result.audio.context.phraseSource == PhraseSource::proposed);
        CHECK(result.audio.context.bpm == 123 && result.audio.context.tempoSource == TempoSource::captured);
        CHECK(result.candidate->mode == PlaybackMode::externalMidi);
    }
    // Both permits a sound-only refinement; null must retain the parent's saved editable phrase.
    r.source.pattern = expected; r.instruction = "darker and tighter";
    const auto refinementSource = r.source;
    auto refinement = wire(); refinement.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", .3)}));
    seen = 0;
    const auto child = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var& schema)
    {
        ++seen; const auto p = parse(prompt);
        CHECK(json(p["proposal_schema"]) == json(schema)); CHECK(p["policy"]["scope"].toString() == "both");
        CHECK(p["context"]["phrase_source"].toString() == "captured");
        CHECK(p["instruction"].toString() == "darker and tighter");
        return completed(json(refinement));
    });
    CHECK(seen == 1 && r.source == refinementSource);
    if (ready(child))
    {
        auto candidate = refinementSource; candidate.sound.values[index("output.gain_db")] = .3f;
        CHECK(*child.candidate == candidate && child.candidate->pattern == expected); CHECK(!child.edits.pattern);
        CHECK(child.edits.parameters == std::vector<ParameterEdit>({{"output.gain_db", .3f}}));
        CHECK(child.audio.context.phrase == expected && child.audio.context.phraseSource == PhraseSource::captured);
        CHECK(child.candidate->mode == PlaybackMode::externalMidi);
    }
}
void scope_lock_reuse_and_macros()
{
    begin(__func__); // Failed admission must not leak candidate edits or widen local permissions.
    auto r = request(); auto w = wire();
    auto rejects = [&](ProposalStatus status) { const auto original = r.source; const auto out = generateProposal(r, reply(w)); withheld(out, GenerationStatus::budgetExhausted, 3); CHECK(out.proposalStatus == status); CHECK(r.source == original); };
    r.source.sound.controlLocks[index("synth.1.analog.filter.cutoff")] = true;
    w.getDynamicObject()->setProperty("parameter_edits", array({param("synth.1.analog.filter.cutoff", .9)})); rejects(ProposalStatus::lockedDependency);
    r = request(); w = wire(); w.getDynamicObject()->setProperty("pattern", phraseWire()); rejects(ProposalStatus::scopeViolation);
    r.policy.scope = RequestScope::pattern; w = wire(); w.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", .6)})); rejects(ProposalStatus::scopeViolation);
    r = request(); w = wire(); w.getDynamicObject()->setProperty("macro_edits", array({object({{"index", 8}, {"target_id", "output.gain_db"}, {"amount", .25}})})); rejects(ProposalStatus::macroChangeNotRequested);
    r.policy.allowMacroChanges = true; auto accepted = generateProposal(r, reply(w)); if (ready(accepted)) CHECK(accepted.candidate->sound.macros[7].targetKey == parameterRegistry()[index("output.gain_db")].stateKey);
    r = request(); CHECK(editRackLayout(r.source.sound.layout, {RackEditKind::addSynth, 1, masterChain, 1}, r.source.sound.layout) == RackEditResult::applied); CHECK(editRackLayout(r.source.sound.layout, {RackEditKind::removeSynth, 1}, r.source.sound.layout) == RackEditResult::applied);
    // A bare bank is outside the AI's reach, reused slot or not.
    w = wire(); w.getDynamicObject()->setProperty("rack_edits", array({object({{"kind", "add_synth"}, {"slot", 1}, {"chain", 4}, {"position", 1}, {"effect", "drive"}})})); rejects(ProposalStatus::outOfReach);
    r.policy.allowSlotReuse = true; rejects(ProposalStatus::outOfReach);
    // The accepted bool decoder rejects unsupported IDs before admission publishes edits.
    r = request(); w = wire(); w.getDynamicObject()->setProperty("parameter_edits", array({param("synth.99.enabled", 1.)})); rejects(ProposalStatus::malformed);
    r = request(); for (std::uint8_t i = 1; i < 4; ++i) CHECK(editRackLayout(r.source.sound.layout, {RackEditKind::addSynth, i, masterChain, i}, r.source.sound.layout) == RackEditResult::applied);
    w = wire(); w.getDynamicObject()->setProperty("rack_edits", array({object({{"kind", "add_synth"}, {"slot", 255}, {"chain", 4}, {"position", 4}, {"effect", "drive"}})})); rejects(ProposalStatus::outOfReach);
}
void mixed_shared_budget()
{
    begin(__func__); // A split repair budget or failed-attempt authority carryover breaks this sequence.
    auto r = request(); r.source.sound.controlLocks[index("synth.1.analog.filter.cutoff")] = true;
    r.capturedPhrase->meter = {3, 8}; r.capturedPhrase->noteCount = 1; r.capturedPhrase->notes[0] = {0, 1.5, 60, 127, 1};
    physical(r.source, "output.protection", 0); physical(r.source, "global.master_level", 1); physical(r.source, "synth.1.level", 1);
    auto locked = wire(); locked.getDynamicObject()->setProperty("parameter_edits", array({param("synth.1.analog.filter.cutoff", .9)}));
    auto clipped = wire(); clipped.getDynamicObject()->setProperty("parameter_edits", array({param("output.gain_db", 1.)}));
    unsigned seen = 0; std::vector<juce::var> prompts;
    const auto original = r.source;
    const auto failed = generateProposal(r, [&](const juce::String&, std::string_view p, const juce::var&) { prompts.push_back(parse(p)); ++seen; return completed(seen == 1 ? "{" : seen == 2 ? json(locked) : json(clipped)); });
    withheld(failed, GenerationStatus::budgetExhausted, 3); CHECK(seen == 3); CHECK(failed.proposalStatus == ProposalStatus::accepted); CHECK(failed.renderStatus == RenderStatus::clipped); CHECK(failed.measurements.peak > 1 && failed.measurements.clippedSamples > 0); CHECK(r.source == original);
    if (prompts.size() == 3) { CHECK(prompts[0]["repair"].isVoid()); CHECK(prompts[1]["repair"]["previous_response"].toString() == "{"); CHECK(prompts[1]["repair"]["proposal_status"].toString() == "malformed"); CHECK(prompts[2]["repair"]["previous_response"].toString().toStdString() == json(locked)); CHECK(prompts[2]["repair"]["proposal_status"].toString() == "lockedDependency"); CHECK(json(prompts[0]["catalog"]) == json(prompts[2]["catalog"])); }
    seen = 0;
    const auto repairedAudio = generateProposal(r, [&](const juce::String&, std::string_view p, const juce::var&) {
        ++seen;
        if (seen == 2) { const auto repair = parse(p)["repair"]; CHECK(repair["render_status"].toString() == "clipped"); CHECK(repair["proposal_status"].toString() == "accepted"); CHECK(bool(repair["measurements"]["finite"])); CHECK(double(repair["measurements"]["peak"]) > 1 && int(repair["measurements"]["clipped_samples"]) > 0); CHECK(repair["previous_response"].toString().toStdString() == json(clipped)); }
        return completed(seen == 1 ? json(clipped) : json(wire())); });
    if (ready(repairedAudio, 2)) CHECK(*repairedAudio.candidate == original);
    r = request(); seen = 0;
    const auto success = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { return completed(++seen == 1 ? "{" : json(wire())); }); ready(success, 2); CHECK(seen == 2);
    seen = 0; const auto fixed = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { ++seen; return completed(json(wire())); }); ready(fixed); CHECK(seen == 1);
    for (auto status : {rack::oauth::Status::networkFailure, rack::oauth::Status::invalidResponse, rack::oauth::Status::signedOut, rack::oauth::Status::inferenceNotGranted})
    {
        seen = 0; const auto out = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { ++seen; auto response = completed("SENSITIVE_PROVIDER_BODY"); response.result.status = status; response.result.httpStatus = 429; response.result.errorCode = status == rack::oauth::Status::invalidResponse ? "sse_refusal" : "rate_limit_exceeded"; return response; });
        withheld(out, GenerationStatus::providerFailure, 1); CHECK(seen == 1); CHECK(out.providerStatus == status && out.httpStatus == 429); CHECK(out.errorCode == (status == rack::oauth::Status::invalidResponse ? "sse_refusal" : "rate_limit_exceeded"));
    }
    withheld(generateProposal(r, reply(object({}))), GenerationStatus::budgetExhausted, 3);
    withheld(generateProposal(r, [](const juce::String&, std::string_view, const juce::var&) { return completed(""); }), GenerationStatus::providerFailure, 1);
    withheld(generateProposal(r, [](const juce::String&, std::string_view, const juce::var&) { return completed(std::string(262145, 'x')); }), GenerationStatus::providerFailure, 1);
    // Callback mutation cannot replace the original captured project/policy/tag for repairs.
    r = request(); const auto captured = r;
    seen = 0; const auto immutable = generateProposal(r, [&](const juce::String&, std::string_view p, const juce::var&) {
        ++seen; if (seen == 1) { r.source.sound.controlLocks[index("output.gain_db")] = true; r.policy.scope = RequestScope::pattern; r.tag.requestId = 999; r.instruction = "MUTATED"; return completed("{"); }
        CHECK(parse(p)["instruction"].toString().toStdString() == captured.instruction); return completed(json(clipped)); });
    if (ready(immutable, 2)) { CHECK(immutable.tag == captured.tag); CHECK(!immutable.candidate->sound.controlLocks[index("output.gain_db")]); }
}
void phrase_context_and_mode()
{
    begin(__func__); // Incorrect optional-empty precedence, fallback labels or mode edits break this group.
    auto r = request(); r.policy.scope = RequestScope::both; r.source.mode = PlaybackMode::externalMidi;
    r.source.pattern.noteCount = 1; r.source.pattern.notes[0] = {0, .25, 70, 99, 3};
    auto proposed = wire(); proposed.getDynamicObject()->setProperty("pattern", phraseWire(true));
    auto out = generateProposal(r, reply(proposed));
    if (ready(out)) { CHECK(out.audio.context.phrase.noteCount == 0 && out.audio.context.phrase.meter.numerator == 3); CHECK(out.audio.context.phraseSource == PhraseSource::proposed); CHECK(out.candidate->pattern.noteCount == 0); CHECK(out.candidate->mode == PlaybackMode::externalMidi); CHECK(out.edits.pattern.has_value()); }
    out = generateProposal(r, reply(wire()));
    if (ready(out)) { CHECK(out.audio.context.phrase.noteCount == 0 && out.audio.context.phraseSource == PhraseSource::captured); CHECK(out.candidate->pattern == r.source.pattern); CHECK(!out.edits.pattern); CHECK(out.measurements.peak == 0 && out.measurements.rms == 0); }
    r.capturedPhrase.reset(); out = generateProposal(r, reply(wire()));
    if (ready(out)) { CHECK(out.audio.context.phrase == r.source.pattern); CHECK(out.audio.context.phraseSource == PhraseSource::captured); CHECK(out.candidate->mode == r.source.mode); }
    r.source.pattern = {}; r.capturedBpm.reset(); r.source.mode = PlaybackMode::internalPattern;
    out = generateProposal(r, [&](const juce::String&, std::string_view p, const juce::var&) {
        const auto c = parse(p)["context"]; CHECK(c["phrase_source"].toString() == "default_notes" && c["tempo_source"].toString() == "default_tempo"); CHECK(double(c["bpm"]) == 120); const auto* notes = c["phrase"]["notes"].getArray(); CHECK(notes && notes->size() == 4); return completed(json(wire())); });
    if (ready(out)) { const auto& c = out.audio.context; CHECK(c.phraseSource == PhraseSource::defaultNotes && c.tempoSource == TempoSource::defaultTempo && c.bpm == 120); CHECK(c.phrase.meter == PhraseMeter({4, 4}) && c.phrase.bars == 1 && c.phrase.swing == 0 && c.phrase.noteCount == 4); for (int i = 0; i < 4; ++i) CHECK(c.phrase.notes[std::size_t(i)] == PatternNote({double(i), .25, 60, 96, 1})); CHECK(out.candidate->pattern == r.source.pattern && out.candidate->mode == PlaybackMode::internalPattern); CHECK(!out.edits.pattern); }
    // Quiet admitted audio is valid and must not consume repairs or boost the patch.
    r = request(); r.capturedPhrase->noteCount = 1; r.capturedPhrase->notes[0] = {0, .25, 60, 1, 1}; physical(r.source, "output.gain_db", -60);
    out = generateProposal(r, reply(wire())); if (ready(out)) { CHECK(out.measurements.peak > 0 && out.measurements.peak < .001); CHECK(*out.candidate == r.source); }
}
void cancellation_and_new_refinement()
{
    begin(__func__); // Every slow-stage boundary must withhold result; a refinement gets a fresh budget.
    auto r = request(); unsigned seen = 0;
    const auto callback = [&](const juce::String&, std::string_view, const juce::var&) { ++seen; return completed(json(wire())); };
    auto out = generateProposal(r, callback, [] { return true; }); withheld(out, GenerationStatus::cancelled, 0); CHECK(seen == 0 && out.tag == r.tag);
    out = generateProposal(r, callback, [&] { return seen > 0; }); withheld(out, GenerationStatus::cancelled, 1); CHECK(out.tag == r.tag);
    int totalChecks = 0; const auto baseline = generateProposal(r, callback, [&] { ++totalChecks; return false; }); ready(baseline); CHECK(totalChecks > 20);
    // Stops after callback/decode, early rendering, scan/FFT and the final publication check.
    for (int stop : {2, 3, 4, 12, totalChecks - 2, totalChecks - 1, totalChecks})
    {
        int at = 0; out = generateProposal(r, callback, [&] { return ++at >= stop; }); withheld(out, GenerationStatus::cancelled, stop == 2 ? 0u : 1u); CHECK(out.tag == r.tag);
    }
    out = generateProposal(r, [](const juce::String&, std::string_view, const juce::var&) { auto x = completed("SENSITIVE"); x.result.status = rack::oauth::Status::cancelled; return x; }); withheld(out, GenerationStatus::cancelled, 1);
    seen = 0; const auto first = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { ++seen; return completed("{"); }); withheld(first, GenerationStatus::budgetExhausted, 3);
    auto refined = r; ++refined.tag.requestId; refined.instruction = "More air";
    const auto second = generateProposal(refined, [&](const juce::String&, std::string_view p, const juce::var&) { CHECK(parse(p)["instruction"].toString() == "More air"); ++seen; return completed(json(wire())); }); ready(second); CHECK(seen == 4 && second.tag == refined.tag && first.tag == r.tag);
}
void raw_input_and_prompt_bounds()
{
    begin(__func__); // Raw byte validation must precede inference, and diagnostics must not copy private text.
    auto base = request(); const auto callback = reply(wire());
    auto invalid = [&](const GenerationRequest& r) { unsigned calls = 0; const auto before = r.source; const auto out = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { ++calls; return completed(json(wire())); }); withheld(out, GenerationStatus::invalidRequest, 0); CHECK(calls == 0 && r.source == before && out.tag == r.tag); };
    auto r = base; r.tag.runtime = juce::Uuid::null(); invalid(r); r = base; r.tag.requestId = 0; invalid(r);
    r = base; r.source.mode = static_cast<PlaybackMode>(255); invalid(r); r = base; r.policy.scope = static_cast<RequestScope>(255); invalid(r);
    r = base; r.source.sound.values[0] = std::numeric_limits<float>::infinity(); invalid(r);
    for (auto text : {std::string(), std::string(8193, 'a'), std::string("A\0B", 3), std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xf0\x9f", 2)}) { r = base; r.instruction = text; invalid(r); }
    for (int field = 0; field < 3; ++field) for (const auto& text : {std::string(129, 'x'), std::string("A\0B", 3), std::string("\x80", 1)}) { r = base; (field == 0 ? r.role : field == 1 ? r.key : r.scale) = text; invalid(r); }
    for (double bpm : {19.999, 400.001, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) { r = base; r.capturedBpm = bpm; invalid(r); }
    r = base; r.capturedPhrase->noteCount = 257; invalid(r); r = base; r.capturedPhrase->grid = static_cast<PatternGrid>(255); invalid(r);
    for (auto model : {juce::String(), juce::String::repeatedString("m", 257), juce::String("bad/model"), juce::String("model name"), juce::String::fromUTF8("\xc3\xa9")}) { r = base; r.model = model; invalid(r); }
    withheld(generateProposal(base, {}), GenerationStatus::invalidRequest, 0);
    r = base; r.instruction = std::string(8192, 'x'); r.role = std::string(128, 'r'); r.key = std::string(128, 'k'); r.scale = std::string(128, 's'); r.model = juce::String::repeatedString("m", 256); r.capturedBpm = 20;
    ready(generateProposal(r, callback));
    r = base; r.instruction.clear(); for (int i = 0; i < 2048; ++i) r.instruction += "\xf0\x9f\x8e\xb9"; ready(generateProposal(r, callback)); r.instruction += "a"; invalid(r);
    auto out = generateProposal(base, [](const juce::String&, std::string_view, const juce::var&) -> rack::oauth::StructuredResult { throw std::runtime_error("SENSITIVE_EXCEPTION_BODY"); }); withheld(out, GenerationStatus::providerFailure, 1); CHECK(!out.errorCode.contains("SENSITIVE"));
    out = generateProposal(base, [](const juce::String&, std::string_view, const juce::var&) { auto x = completed("SENSITIVE_PROVIDER_BODY"); x.result.status = rack::oauth::Status::networkFailure; x.result.errorCode = "SENSITIVE_PROVIDER_BODY"; x.result.requestId = "SENSITIVE_REQUEST_ID"; return x; }); withheld(out, GenerationStatus::providerFailure, 1); CHECK(!out.errorCode.contains("SENSITIVE"));
    unsigned call = 0; std::string last;
    const auto bounded = generateProposal(base, [&](const juce::String&, std::string_view p, const juce::var&) { ++call; CHECK(p.size() <= 573952); const auto payload = parse(p); if (call > 1) { const auto repair = payload["repair"]; CHECK(repair.getDynamicObject() && repair.getDynamicObject()->getProperties().size() == 4); CHECK(repair["previous_response"].toString().toStdString() == last); CHECK(!repair.hasProperty("transcript")); CHECK(repair["render_status"].toString() == "invalidInput"); CHECK(repair["measurements"].isVoid()); CHECK(juce::JSON::toString(repair, true, 17).getNumBytesAsUTF8() <= 65536); } last = std::string(60000, call == 1 ? 'a' : 'b'); return completed(last); }); withheld(bounded, GenerationStatus::budgetExhausted, 3); CHECK(call == 3);
    // The maximum legal output remains legal, but an oversized optional repair stops without another call.
    call = 0; const auto maximumReply = generateProposal(base, [&](const juce::String&, std::string_view, const juce::var&) { ++call; return completed(std::string(262144, 'x')); });
    withheld(maximumReply, GenerationStatus::invalidRequest, 1); CHECK(call == 1 && maximumReply.errorCode == "generation_repair_limit");
    call = 0; const auto expanded = generateProposal(base, [&](const juce::String&, std::string_view p, const juce::var&) { ++call; CHECK(p.size() <= 1048576); return completed(std::string(262144, '\x01')); });
    withheld(expanded, GenerationStatus::invalidRequest, 1); CHECK(call == 1 && expanded.errorCode == "generation_repair_limit" && expanded.tag == base.tag); CHECK(expanded.proposalStatus == ProposalStatus::malformed && expanded.renderStatus == RenderStatus::invalidInput);
    // Pin actual raw nonfinite observations -> valid compact JSON, not a second renderer.
    for (float poison : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
    {
        std::vector<float> left(16, .2f), right(16, -.2f); right[7] = poison; AudioMeasurements measured;
        CHECK(measurePreview(left, right, 8, measured) == RenderStatus::nonFinite); CHECK(!measured.finite);
        const auto feedback = parse(json(ai::detail::measurementsForFeedback(measured)));
        CHECK(feedback.getDynamicObject() && feedback.getDynamicObject()->getProperties().size() == 9); CHECK(!bool(feedback["finite"])); CHECK(feedback["rms"].isVoid()); CHECK(float(double(feedback["peak"])) == .2f); CHECK(int(feedback["clipped_samples"]) == (std::isinf(poison) ? 1 : 0));
        const auto rejected = parse(json(object({{"render_status", "nonFinite"}, {"measurements", ai::detail::measurementsForFeedback(measured)}})));
        CHECK(rejected["render_status"].toString() == "nonFinite" && !bool(rejected["measurements"]["finite"]) && rejected["measurements"]["rms"].isVoid());
    }
    AudioMeasurements nonfinite; nonfinite.finite = false; nonfinite.peak = std::numeric_limits<double>::infinity(); nonfinite.rms = std::numeric_limits<double>::quiet_NaN(); nonfinite.onsetSeconds = -std::numeric_limits<double>::infinity(); nonfinite.activityEndSeconds = nonfinite.peak; nonfinite.decaySeconds = nonfinite.rms; nonfinite.bandFractions = {nonfinite.peak, nonfinite.rms, -nonfinite.peak}; nonfinite.clippedSamples = 13; nonfinite.tailActiveAtEnd = true;
    const auto feedback = parse(json(ai::detail::measurementsForFeedback(nonfinite)));
    for (auto key : {"peak", "rms", "onset_seconds", "activity_end_seconds", "decay_seconds"}) CHECK(feedback[key].isVoid());
    const auto* bands = feedback["band_fractions"].getArray(); CHECK(bands && bands->size() == 3); if (bands) for (const auto& band : *bands) CHECK(band.isVoid()); CHECK(int(feedback["clipped_samples"]) == 13 && bool(feedback["tail_active_at_end"]) && !bool(feedback["finite"]));
    AudioMeasurements silent; const auto zero = parse(json(ai::detail::measurementsForFeedback(silent))); CHECK(zero["onset_seconds"].isVoid() && bool(zero["finite"]) && double(zero["rms"]) == 0);
}
GenerationRequest requiredRequest()
{
    auto r = request(); r.policy = {RequestScope::both, false, false, true};
    r.instruction = "a rolling bassline in the style of John Summit";
    r.role = "bass"; r.key = "A"; r.scale = "minor"; r.capturedBpm = 123; r.capturedPhrase.reset();
    return r;
}
void required_pattern_shared_repair()
{
    begin(__func__); auto r = requiredRequest(); const auto source = r.source;
    auto empty = wire(); empty.getDynamicObject()->setProperty("pattern", phraseWire(true));
    auto valid = wire(); valid.getDynamicObject()->setProperty("pattern", phraseWire());
    const std::array<std::string, 3> responses{json(wire()), json(empty), json(valid)};
    unsigned seen = 0;
    const auto result = generateProposal(r, [&](const juce::String& model, std::string_view prompt, const juce::var& schema) {
        const auto p = parse(prompt); CHECK(model == r.model);
        CHECK(p["policy"]["require_pattern"].isBool()); CHECK(bool(p["policy"]["require_pattern"]));
        CHECK(p["policy"]["scope"].toString() == "both");
        CHECK(json(p["proposal_schema"]) == json(schema)); CHECK(json(schema) == json(proposalSchema(r.policy)));
        CHECK(schema["properties"]["pattern"]["type"].toString() == "object");
        CHECK(int(schema["properties"]["pattern"]["properties"]["notes"]["minItems"]) == 1);
        CHECK(p["instruction"].toString() == "a rolling bassline in the style of John Summit");
        CHECK(p["context"]["role"].toString() == "bass" && p["context"]["key"].toString() == "A" && p["context"]["scale"].toString() == "minor");
        CHECK(double(p["context"]["bpm"]) == 123); CHECK(p["context"]["phrase_source"].toString() == "default_notes");
        CHECK(p["context"]["saved_pattern"]["notes"].getArray() && p["context"]["saved_pattern"]["notes"].getArray()->isEmpty());
        const auto rules = p["output_rules"].toString(); CHECK(rules.contains("require_pattern"));
        CHECK(rules.containsIgnoreCase("at least one note")); CHECK(rules.contains("default_notes are audition-only"));
        if (seen == 0) CHECK(p["repair"].isVoid());
        else {
            CHECK(p["repair"]["proposal_status"].toString() == "requestedPatternMissing");
            CHECK(p["repair"]["previous_response"].toString().toStdString() == responses[seen - 1]);
            CHECK(p["repair"]["render_status"].toString() == "invalidInput"); CHECK(p["repair"]["measurements"].isVoid());
        }
        CHECK(seen < responses.size()); return completed(responses[std::min<unsigned>(seen++, 2)]);
    });
    CHECK(seen == 3); CHECK(r.source == source && r.policy.requirePattern); CHECK(result.tag == r.tag);
    if (ready(result, 3)) {
        PatternDocument expected; expected.meter = {3, 8}; expected.grid = PatternGrid::eighth;
        expected.swing = .35; expected.noteCount = 1; expected.notes[0] = {.25, .125, 65, 87, 2};
        CHECK(result.edits.pattern && *result.edits.pattern == expected);
        auto candidate = source; candidate.pattern = expected; CHECK(*result.candidate == candidate);
        CHECK(result.audio.context.phrase == expected && result.audio.context.phraseSource == PhraseSource::proposed);
        CHECK(result.audio.context.bpm == 123); CHECK(result.errorCode.isEmpty());
    }
}
void required_pattern_withheld_results()
{
    begin(__func__); auto r = requiredRequest();
    r.source.pattern.noteCount = 1; r.source.pattern.notes[0] = {0, .25, 45, 96, 1};
    const auto source = r.source;
    for (bool empty : {false, true}) {
        auto w = wire(); w.getDynamicObject()->setProperty("explanation", "Play MIDI note A2 to hear a bassline");
        if (empty) w.getDynamicObject()->setProperty("pattern", phraseWire(true));
        unsigned seen = 0;
        const auto out = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var&) {
            const auto p = parse(prompt); if (seen > 0) CHECK(p["repair"]["proposal_status"].toString() == "requestedPatternMissing");
            ++seen; return completed(json(w));
        });
        withheld(out, GenerationStatus::budgetExhausted, 3); CHECK(seen == 3);
        CHECK(out.proposalStatus == ProposalStatus::requestedPatternMissing && out.errorCode == "requested_pattern_missing");
        CHECK(out.renderStatus == RenderStatus::invalidInput && out.measurements.rms == 0); CHECK(r.source == source);
    }
    r.policy.scope = RequestScope::sound; unsigned seen = 0;
    auto out = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { ++seen; return completed(json(wire())); });
    withheld(out, GenerationStatus::invalidRequest, 0); CHECK(seen == 0 && r.source == source);
    r.policy.scope = RequestScope::both; bool cancel = false;
    out = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { ++seen; cancel = true; return completed(json(wire())); }, [&] { return cancel; });
    withheld(out, GenerationStatus::cancelled, 1); CHECK(seen == 1 && out.tag == r.tag && r.source == source);
}
void required_pattern_captured_policy()
{
    begin(__func__); auto r = requiredRequest(); const auto captured = r;
    auto valid = wire(); valid.getDynamicObject()->setProperty("pattern", phraseWire()); unsigned seen = 0;
    const auto out = generateProposal(r, [&](const juce::String& model, std::string_view prompt, const juce::var& schema) {
        const auto p = parse(prompt); CHECK(model == captured.model);
        CHECK(p["policy"]["require_pattern"].isBool() && bool(p["policy"]["require_pattern"]));
        CHECK(p["policy"]["scope"].toString() == "both"); CHECK(p["instruction"].toString().toStdString() == captured.instruction);
        CHECK(json(p["proposal_schema"]) == json(schema) && json(schema) == json(proposalSchema(captured.policy)));
        ++seen; if (seen == 1) {
            r.policy = {}; r.instruction = "changed after capture"; r.source.pattern.noteCount = 1;
            r.source.pattern.notes[0] = {0, .25, 90, 50, 1}; ++r.tag.requestId;
            return completed(json(wire()));
        }
        CHECK(p["repair"]["proposal_status"].toString() == "requestedPatternMissing"); return completed(json(valid));
    });
    CHECK(seen == 2); CHECK(out.tag == captured.tag && out.tag != r.tag);
    if (ready(out, 2)) {
        CHECK(out.edits.pattern && out.candidate->pattern == *out.edits.pattern);
        CHECK(out.candidate->pattern.noteCount == 1 && out.candidate->pattern.notes[0].pitch == 65);
        CHECK(out.candidate->sound == captured.source.sound && out.candidate->mode == captured.source.mode);
    }
}
// Decision 8: a proposal outside the user's reach goes back to the model as outOfReach, and its repair is admitted.
juce::var generationWire2();
void reach_repair()
{
    begin(__func__);
    auto r = request(); r.source.sound = copperArcSound(); const auto source = r.source;
    auto outside = wire(); outside.getDynamicObject()->setProperty("parameter_edits", array({param("synth.2.analog.filter.cutoff", .9)}));
    auto inside = wire(); inside.getDynamicObject()->setProperty("parameter_edits", array({param("synth.1.analog.filter.cutoff", .9)}));
    unsigned seen = 0;
    auto out = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var&) {
        const auto p = parse(prompt);
        if (seen == 0) CHECK(p["repair"].isVoid()); else CHECK(p["repair"]["proposal_status"].toString() == "outOfReach");
        return completed(json(seen++ == 0 ? outside : inside));
    });
    CHECK(seen == 2);
    if (ready(out, 2)) CHECK(out.candidate->sound.values[index("synth.1.analog.filter.cutoff")] == .9f);
    seen = 0;
    out = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { ++seen; return completed(json(outside)); });
    withheld(out, GenerationStatus::budgetExhausted, 3);
    CHECK(seen == 3 && out.proposalStatus == ProposalStatus::outOfReach && r.source == source);
    // The selection phase: a bare-bank rack edit riding with a library rack is repaired before the selection returns.
    RackCatalogSnapshot root; const std::vector<RackDesign> embedded{source.sound.personalities->instances[0].design};
    auto historical=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
    CHECK(historical.size()==28&&std::count(historical.begin(),historical.end(),tile)==1);
    std::erase(historical,tile);CHECK(historical.size()==27);
    CHECK(captureRackCatalog(historical, embedded, {}, root));
    juce::var inventory; CHECK(catalogInventoryFor(root, inventory));
    auto authority = std::make_shared<CatalogAuthority>(); authority->root = r.tag; authority->snapshot = root.id;
    authority->descriptorJson = juce::JSON::toString(inventory, true, 22).toStdString();
    CatalogRequestContext context; context.root = r.tag; context.authority = authority; r.catalog = context;
    const auto lumen = std::find_if(root.entries.begin(), root.entries.end(), [](const auto& e) { return e.definition.name == "Lumen Drift"; });
    CHECK(lumen != root.entries.end()); if (lumen == root.entries.end()) return;
    auto picked = generationWire2(); auto* fields = picked.getDynamicObject();
    fields->setProperty("catalog_selections", array({object({{"local_id", 1}, {"design", lumen->definition.id.toDashedString()},
        {"revision", juce::String(std::to_string(lumen->definition.revision))}, {"content_sha256", lumen->contentSha256},
        {"destination", object({{"role", "fx"}, {"chain", int(masterChain)}, {"position", 0}})}})}));
    fields->setProperty("catalog_parameter_edits", array());
    auto bare = picked.clone(); bare.getDynamicObject()->setProperty("rack_edits", array({object({{"kind", "add_effect"}, {"slot", 1},
        {"chain", int(masterChain)}, {"position", 1}, {"effect", "drive"}})}));
    seen = 0;
    out = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var&) {
        const auto p = parse(prompt);
        if (seen > 0) CHECK(p["repair"]["proposal_status"].toString() == "outOfReach");
        return completed(json(seen++ == 0 ? bare : picked));
    });
    CHECK(seen == 2 && out.inferenceCalls == 2 && out.status == GenerationStatus::catalogSelectionRequired);
    CHECK(out.edits.catalogSelections.size() == 1 && out.edits.racks.empty() && r.source == source);
    // So is a detach riding with it.
    auto detaching = picked.clone(); detaching.getDynamicObject()->setProperty("reconciliation", object({{"detach",
        array({source.sound.personalities->instances[0].id.toDashedString()})}, {"remove", array()}, {"replace", array()}, {"controls", array()}}));
    seen = 0;
    out = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var&) {
        const auto p = parse(prompt);
        if (seen > 0) CHECK(p["repair"]["proposal_status"].toString() == "outOfReach");
        return completed(json(seen++ == 0 ? detaching : picked));
    });
    CHECK(seen == 2 && out.inferenceCalls == 2 && out.status == GenerationStatus::catalogSelectionRequired);
    CHECK(out.edits.catalogSelections.size() == 1 && out.edits.reconciliation.detach.empty() && r.source == source);
}
// Task4 staged tests. Unspecified provider field names are provisional and
// listed in the staged report; no production APIs/bodies are introduced here.
RackDesign generationRecipe(NativeModule module = NativeModule::analog)
{
    RackDesign d; d.name = "Native brass"; d.intent = "Original rack from owned modules";
    d.role = module < NativeModule::drive ? RackRole::synth : RackRole::fx;
    d.units = {{0, module, {}}}; d.panel.sections = {"Tone"};
    if (module == NativeModule::analog) {
        d.units[0].defaults = {{0, "analog.filter.cutoff", .35f}, {0, "engine", 0}};
        d.panel.controls = {{"Tone", 0, 0, "analog.filter.cutoff", FocusWidget::rotary, FocusResponse::logarithmic, 20, 20000}};
    } else if (module == NativeModule::drive) {
        d.units[0].defaults = {{0, "drive.gain_db", .1f}};
        d.panel.controls = {{"Gain", 0, 0, "drive.gain_db", FocusWidget::rotary, FocusResponse::linear, 0, 24}};
    }
    CHECK(validateRackDesign(d, RackValidationMode::portableDesign)); return d;
}
juce::var generationWire2()
{
    auto v = wire(); auto* o = v.getDynamicObject(); o->setProperty("version", 2);
    o->setProperty("name", "Catalog test sound");
    o->setProperty("reconciliation", object({{"detach", array()}, {"remove", array()}, {"replace", array()}, {"controls", array()}})); return v;
}
void previewBounds(const GenerationResult& r)
{
    CHECK(r.audio.sampleRate == 48000); CHECK(r.audio.bodyFrames <= 384000);
    CHECK(r.audio.left.size() == r.audio.right.size() && r.audio.left.size() <= 576000);
    CHECK(r.audio.left.size() == r.audio.bodyFrames + 192000); CHECK(r.measurements.finite);
    CHECK(std::all_of(r.audio.left.begin(), r.audio.left.end(), [](float v) { return std::isfinite(v); }));
    CHECK(std::all_of(r.audio.right.begin(), r.audio.right.end(), [](float v) { return std::isfinite(v); }));
}
std::string escapeText(std::size_t bytes)
{
    std::string text; for (std::size_t n = 0; n < bytes; ++n) text += n % 3 == 0 ? '\x01' : n % 3 == 1 ? '"' : '\\'; return text;
}
RackDesign maximumTextRecipe(NativeModule module,bool beforeTape=false,bool beforeStereo=false)
{
    auto d = generationRecipe(module); const auto text = escapeText(64);
    d.name = juce::String::fromUTF8(text.data(), int(text.size())); const auto intent = escapeText(512);
    d.intent = juce::String::fromUTF8(intent.data(), int(intent.size())); d.revision = UINT64_MAX;
    d.panel.sections = {d.name, d.name, d.name, d.name}; d.panel.controls.clear(); d.units[0].defaults.clear();
    const bool synth = d.role == RackRole::synth; const auto prefix = juce::String(synth ? "synth.1." : "effect.1.");
    for (const auto& p : parameterRegistry()) if (p.id.startsWith(prefix)) {
        auto key = p.id.substring(prefix.length());
        const bool compatible = rackParameterActive(module, key);
        const auto parameter=rackParameterIndex(module,0,key);
        if (!compatible || !parameter || (module==NativeModule::delay&&((beforeTape&&*parameter>=delayTapeParameterStart)||(beforeStereo&&*parameter>=delayStereoParameterStart)))) continue;
        const auto normalized = defaultParameterValues()[*rackParameterIndex(module, 0, key)]; d.units[0].defaults.push_back({0, key, normalized});
        if (d.panel.controls.size() < 8 && p.minimum < p.maximum)
            d.panel.controls.push_back({d.name, 0, std::uint8_t(d.panel.controls.size() % 4), key,
                p.kind == ParameterKind::continuous ? FocusWidget::rotary : p.kind == ParameterKind::toggle ? FocusWidget::toggle : FocusWidget::choice,
                FocusResponse::linear, p.minimum, p.maximum});
    }
    if(module==NativeModule::delay)CHECK(d.units[0].defaults.size()==(beforeTape?8u:beforeStereo?13u:16u));
    CHECK(d.panel.controls.size() == (module == NativeModule::drive ? 5u : 8u)); CHECK(validateRackDesign(d, RackValidationMode::portableDesign));
    // Maximal padded legal file size must not become a provider string/full definition duplication.
    juce::MemoryBlock encoded; CHECK(encodeRackDesign(d, encoded)); std::string padded(static_cast<const char*>(encoded.getData()), encoded.getSize());
    CHECK(padded.size() <= 65536); padded.resize(65536, ' '); RackDesign decoded;
    CHECK(decodeRackDesign(padded.data(), padded.size(), decoded)); CHECK(decoded == d); return d;
}
ProjectDocument fullPersonalityProject(bool beforeTape=false,bool beforeStereo=false)
{
    ProjectDocument d; SoundDocument next; RackDifference difference;
    for (std::uint8_t s = 0; s < 4; ++s) {
        RackOperation op; op.kind = s == 0 ? RackOperationKind::adopt : RackOperationKind::place;
        op.newInstance = juce::Uuid(); op.design = maximumTextRecipe(NativeModule::analog); op.allocation = {{0, s}};
        op.destination = {RackRole::synth, masterChain, s};
        const auto status = makeRackCandidate(d.sound, op, SoundEditOrigin::manual, next, difference); CHECK(status == RackCandidateStatus::ready);
        if (status != RackCandidateStatus::ready) return d; d.sound = next;
    }
    for (std::uint8_t s = 0; s < 8; ++s) {
        RackOperation op; op.newInstance = juce::Uuid(); op.design = maximumTextRecipe(NativeModule::delay,beforeTape,beforeStereo); op.allocation = {{0, s}};
        op.destination = {RackRole::fx, masterChain, s};
        const auto status = makeRackCandidate(d.sound, op, SoundEditOrigin::manual, next, difference); CHECK(status == RackCandidateStatus::ready);
        if (status != RackCandidateStatus::ready) return d; d.sound = next;
    }
    // Rich legal committed values/locks and saved notes, independent of recipes' preserved defaults.
    for (std::size_t i = 0; i < parameterCount; ++i) {
        if (parameterRegistry()[i].kind == ParameterKind::continuous) d.sound.values[i] = float(i % 101) / 100.f;
        d.sound.controlLocks[i] = i % 7 == 0;
    }
    d.sound.values[index("output.gain_db")] = .05f; d.pattern.bars = 8; d.pattern.noteCount = 256;
    for (std::size_t n = 0; n < 8; ++n) d.sound.macros[n] = {parameterRegistry()[0].stateKey, float(int(n) - 4) / 8};
    for (std::size_t n = 0; n < 256; ++n) d.pattern.notes[n] = {double(n) / 8, .03125, std::uint8_t(n % 128), std::uint8_t(1 + n % 31), std::uint8_t(1 + n % 16)};
    CHECK(d.sound.personalities && d.sound.personalities->instances.size() == 12); CHECK(validateProjectDocument(d)); return d;
}
std::size_t definitionCount(const juce::var& v)
{
    std::size_t n = v["intent"].isString() && v["units"].getArray() && v["panel"].getDynamicObject() ? 1 : 0;
    if (const auto* a = v.getArray()) for (const auto& item : *a) n += definitionCount(item);
    if (const auto* o = v.getDynamicObject()) for (const auto& p : o->getProperties()) n += definitionCount(p.value);
    return n;
}
void summaries(const juce::var& value, const ProjectDocument& d)
{
    const auto* a = value.getArray(); const auto count = d.sound.personalities ? d.sound.personalities->instances.size() : 0;
    CHECK(a && a->size() == int(count)); CHECK(juce::JSON::toString(value, true, 17).getNumBytesAsUTF8() <= 24576);
    if (!a || a->size() != int(count)) return;
    for (int n = 0; n < a->size(); ++n) {
        const auto& v = (*a)[n]; const auto* row = v.getArray(); CHECK(row && row->size() == 8); CHECK(juce::JSON::toString(v, true, 17).getNumBytesAsUTF8() + 1 <= 2048);
        if (!row || row->size() != 8) continue; const auto& i = d.sound.personalities->instances[std::size_t(n)];
        CHECK((*row)[0].toString() == i.id.toDashedString() && (*row)[1].toString() == i.design.id.toDashedString());
        CHECK((*row)[2].isString() && (*row)[2].toString().toStdString() == std::to_string(i.design.revision));
        CHECK((*row)[3].toString() == i.design.name); CHECK(int((*row)[4]) == static_cast<int>(i.design.role)); CHECK(bool((*row)[5]) == i.edited);
        const auto* units = (*row)[6].getArray(); const auto* controls = (*row)[7].getArray();
        CHECK(units && units->size() == int(i.design.units.size())); CHECK(controls && controls->size() == int(i.design.panel.controls.size()));
        if (units && units->size() == int(i.design.units.size())) for (int u = 0; u < units->size(); ++u) {
            const auto* unit = (*units)[u].getArray(); CHECK(unit && unit->size() == 3); CHECK(juce::JSON::toString((*units)[u], true, 17).getNumBytesAsUTF8() + 1 <= 64);
            if (unit && unit->size() == 3) { CHECK(int((*unit)[0]) == i.design.units[std::size_t(u)].id); CHECK(int((*unit)[1]) == static_cast<int>(i.design.units[std::size_t(u)].module));
                const auto binding = std::find_if(i.bindings.begin(), i.bindings.end(), [&](const auto& b) { return b.unit == i.design.units[std::size_t(u)].id; });
                CHECK(binding != i.bindings.end()); if (binding != i.bindings.end()) CHECK(int((*unit)[2]) == binding->slot); }
        }
        if (controls && controls->size() == int(i.design.panel.controls.size())) for (int c = 0; c < controls->size(); ++c) {
            const auto* control = (*controls)[c].getArray(); CHECK(control && control->size() == 2); CHECK(juce::JSON::toString((*controls)[c], true, 17).getNumBytesAsUTF8() + 1 <= 96);
            if (control && control->size() == 2) CHECK(int((*control)[0]) == i.design.panel.controls[std::size_t(c)].unit && (*control)[1].toString() == i.design.panel.controls[std::size_t(c)].key);
        }
        CHECK(!v.hasProperty("defaults") && !v.hasProperty("intent") && !v.hasProperty("panel"));
    }
}
void oauthEnvelopeBudget(std::string_view prompt, const juce::var& schema, bool grouped)
{
    // Exercise the product serializer, including its final guard, without native
    // HTTP or an actual account. Only this unique synthetic test directory is used.
    const auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("Task4-synthetic-body", "", false);
    CHECK(directory.createDirectory().wasOk());
    {
        rack::oauth::detail::Store store(directory); std::atomic_bool cancelled=false;
        CHECK(store.lock(cancelled));
        CHECK(store.save(object({{"access_token","synthetic-local-test"},
            {"scope","chatgpt.tokens.use.direct resource.invoke"},{"expires_at",juce::var(juce::int64(2000))}})));
    }
    unsigned calls=0;
    {
        rack::oauth::OAuthProbe probe(directory,[&](const rack::oauth::HttpRequest& request,const std::atomic_bool&) {
            ++calls; CHECK(request.method=="POST" && request.url.endsWith("/responses"));
            const auto bytes=request.body.getNumBytesAsUTF8();
            CHECK(bytes <= (grouped ? 2069504u : 1217536u)); CHECK(bytes <= rack::oauth::maximumStructuredRequestBytes);
            const auto body=juce::JSON::parse(request.body);
            const auto* input=body["input"].getArray(); CHECK(input && input->size()==1);
            if (input && input->size()==1) {
                CHECK((*input)[0]["content"].toString().toStdString()==prompt);
                CHECK(juce::JSON::toString((*input)[0]["content"],true).getNumBytesAsUTF8()<=2*prompt.size()+2);
            }
            CHECK(json(body["text"]["format"]["schema"])==json(schema));
            CHECK(bool(body["text"]["format"]["strict"]) && !bool(body["store"]) && bool(body["stream"]));
            return rack::oauth::HttpResponse{429,"{\"error\":{\"code\":\"rate_limit_exceeded\"}}"};
        },[]{return int64_t(1000);});
        const auto result=probe.requestStructured(juce::String::repeatedString("m",256),prompt,schema);
        CHECK(calls==1 && result.result.httpStatus==429);
        // Raw prompt is legal; its escaped final body exceeds 2 MiB. No second
        // transport call is allowed, so credentials/HTTP cannot precede the guard.
        const auto tooLarge=probe.requestStructured("m",std::string(350000,'\x01'),schema);
        CHECK(tooLarge.result.errorCode=="structured_request_limit" && calls==1);
    }
    CHECK(directory.deleteRecursively());
}
GroupUuid fixtureGroupId(unsigned n)
{
    GroupUuid id; id.bytes[0] = std::uint8_t(n); id.bytes[15] = 0x91; return id;
}
void groupIntentFor(ProjectDocument& d, unsigned member)
{
    GroupIntent g; g.group = fixtureGroupId(90); g.member = fixtureGroupId(member); const auto text = escapeText(128);
    for (auto* field : {&g.name, &g.role, &g.key, &g.scale}) CHECK(setGroupText(text, *field));
    g.bpm = 400; d.group = g; CHECK(validateProjectDocument(d));
}
// These spans measure the actual UTF-8 prompt after strict JSON validation.
// They never parse/reformat numeric tokens, which would expand binary32 values.
bool rawJsonSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
std::optional<std::size_t> rawJsonStringEnd(std::string_view text, std::size_t at)
{
    if (at >= text.size() || text[at++] != '"') return {};
    while (at < text.size()) {
        const auto c = text[at++];
        if (c == '"') return at;
        if (c == '\\' && at++ >= text.size()) return {};
    }
    return {};
}
std::optional<std::size_t> rawJsonValueEnd(std::string_view text, std::size_t at)
{
    if (at >= text.size()) return {};
    if (text[at] == '"') return rawJsonStringEnd(text, at);
    if (text[at] == '{' || text[at] == '[') {
        std::array<char,64> nesting{}; std::size_t depth = 0;
        while (at < text.size()) {
            const auto c = text[at++];
            if (c == '"') { const auto end = rawJsonStringEnd(text, at - 1); if (!end) return {}; at = *end; }
            else if (c == '{' || c == '[') { if (depth == nesting.size()) return {}; nesting[depth++] = c; }
            else if (c == '}' || c == ']') {
                if (depth == 0 || nesting[--depth] != (c == '}' ? '{' : '[')) return {};
                if (depth == 0) return at;
            }
        }
        return {};
    }
    const auto start = at;
    while (at < text.size() && !rawJsonSpace(text[at]) && text[at] != ',' && text[at] != ']' && text[at] != '}') ++at;
    return at > start ? std::optional<std::size_t>(at) : std::nullopt;
}
struct RawJsonMember { std::string_view value; std::size_t eraseFrom, eraseTo; };
std::optional<RawJsonMember> rawJsonMember(std::string_view text, std::string_view quotedKey)
{
    if (text.empty() || text.front() != '{') return {};
    std::size_t at = 1, precedingComma = std::string_view::npos;
    while (at < text.size()) {
        const auto prefix = at;
        while (at < text.size() && rawJsonSpace(text[at])) ++at;
        if (at == text.size() || text[at] == '}') return {};
        const auto keyStart = at; const auto keyEnd = rawJsonStringEnd(text, at); if (!keyEnd) return {};
        at = *keyEnd; while (at < text.size() && rawJsonSpace(text[at])) ++at;
        if (at == text.size() || text[at++] != ':') return {};
        while (at < text.size() && rawJsonSpace(text[at])) ++at;
        const auto valueStart = at; const auto valueEnd = rawJsonValueEnd(text, at); if (!valueEnd) return {};
        at = *valueEnd; while (at < text.size() && rawJsonSpace(text[at])) ++at;
        if (at == text.size() || (text[at] != ',' && text[at] != '}')) return {};
        if (text.substr(keyStart, *keyEnd - keyStart) == quotedKey) {
            const auto eraseFrom = text[at] == ',' || precedingComma == std::string_view::npos ? prefix : precedingComma;
            const auto eraseTo = text[at] == ',' ? at + 1 : *valueEnd;
            return RawJsonMember{text.substr(valueStart, *valueEnd - valueStart), eraseFrom, eraseTo};
        }
        if (text[at] == '}') return {};
        precedingComma = at++;
    }
    return {};
}
std::optional<std::string_view> rawJsonElement(std::string_view text, std::size_t index)
{
    if (text.empty() || text.front() != '[') return {};
    std::size_t at = 1;
    for (std::size_t element = 0; at < text.size(); ++element) {
        while (at < text.size() && rawJsonSpace(text[at])) ++at;
        if (at == text.size() || text[at] == ']') return {};
        const auto start = at; const auto end = rawJsonValueEnd(text, at); if (!end) return {};
        at = *end; while (at < text.size() && rawJsonSpace(text[at])) ++at;
        if (at == text.size() || (text[at] != ',' && text[at] != ']')) return {};
        if (element == index) return text.substr(start, *end - start);
        if (text[at++] != ',') return {};
    }
    return {};
}
void raw_peer_context_span_calibration()
{
    begin(__func__);
    const std::array<std::pair<std::string_view,std::string_view>,4> cases{{
        {R"({ "personalities": ["escaped\\\"[]{}"], "keep": {"x":[1,2]} })", R"({ "keep": {"x":[1,2]} })"},
        {R"({ "keep":1, "personalities":[{"x":"\\\""}] })", R"({ "keep":1 })"},
        {R"({"personalities":[]})", "{}"},
        {R"({"text":"\"personalities\": [,]} \\ \u00e9","nested":{"personalities":[1]},"personalities":[{"name":"[]{}"}],"tail":[0.1,{"s":"[,}]"}]})",
         R"({"text":"\"personalities\": [,]} \\ \u00e9","nested":{"personalities":[1]},"tail":[0.1,{"s":"[,}]"}]})"}
    }};
    for (const auto& [wire, expected] : cases) {
        juce::var parsed; CHECK(parseStrictJson(wire, parsed, 4096));
        const auto member = rawJsonMember(wire, "\"personalities\""); CHECK(member.has_value());
        if (member) {
            CHECK(wire.size() - (member->eraseTo - member->eraseFrom) == expected.size());
            CHECK(std::string(wire.substr(0, member->eraseFrom)) + std::string(wire.substr(member->eraseTo)) == expected);
        }
    }
    const auto nested = R"({"linked_context":{"peers":[{"text":"\\\"[,{}]"},{"personalities":[],"keep":[{"x":"]"}]}]}})";
    const auto linked = rawJsonMember(nested, "\"linked_context\""); CHECK(linked.has_value());
    const auto peers = linked ? rawJsonMember(linked->value, "\"peers\"") : std::nullopt; CHECK(peers.has_value());
    const auto second = peers ? rawJsonElement(peers->value, 1) : std::nullopt; CHECK(second == R"({"personalities":[],"keep":[{"x":"]"}]})");
    CHECK(peers && !rawJsonElement(peers->value, 2));
    CHECK(!rawJsonValueEnd(R"({"bad":[1})", 0)); CHECK(!rawJsonStringEnd("\"unfinished\\", 0));
}

void lossless_peer_control_numbers()
{
    begin(__func__);
    const std::array<float,16> probes{0.f,-0.f,std::numeric_limits<float>::denorm_min(),
        std::nextafter(std::numeric_limits<float>::min(),0.f),std::numeric_limits<float>::min(),
        std::nextafter(.5f,0.f),.5f,std::nextafter(.5f,1.f),std::nextafter(1.f,0.f),1.f,
        .01f,.07f,.1f,.29f,1.f/3.f,.123456789f};
    const auto bits=[](float v){return std::bit_cast<std::uint32_t>(v);};
    const auto registry=parameterRegistry();
    for(std::size_t rotation=0;rotation<probes.size();++rotation){
        auto r=std::make_unique<GenerationRequest>(request());groupIntentFor(r->source,1);
        r->tag.runtime=juce::Uuid(fixtureGroupId(40).getRawData());
        r->role=std::string(groupTextView(r->source.group->role));r->key=std::string(groupTextView(r->source.group->key));r->scale=std::string(groupTextView(r->source.group->scale));r->capturedBpm=400;
        auto capture=std::make_unique<GroupPerformanceCapture>();capture->ownership.registry=fixtureGroupId(99);
        capture->ownership.requesterRuntime=GroupUuid(r->tag.runtime);capture->ownership.loadRevision=1;capture->ownership.revision=432;
        capture->ownership.requester=*r->source.group;capture->requesterEditRevision=r->tag.editRevision;capture->peerCount=1;
        auto peer=std::make_unique<ProjectDocument>();groupIntentFor(*peer,2);
        peer->pattern.noteCount=1;peer->pattern.notes[0]={1.2345678901234567,.3456789012345678,60,96,1};
        auto& controls=capture->playback[0].controls;controls.availability=ObservedMidiAvailability::fresh;controls.reason=LiveControlReason::none;controls.values=peer->sound.values;
        std::size_t continuous=0,inactiveSpace=0;
        for(std::size_t i=0;i<registry.size();++i)if(registry[i].kind==ParameterKind::continuous){
            const auto original=peer->sound.values[i];auto selected=(rotation+i)%probes.size();
            while(bits(probes[selected])==bits(original))selected=(selected+1)%probes.size();
            peer->sound.values[i]=probes[selected];selected=(selected+1)%probes.size();
            while(bits(probes[selected])==bits(original)||bits(probes[selected])==bits(peer->sound.values[i]))selected=(selected+1)%probes.size();
            controls.values[i]=probes[selected];++continuous;
            if(registry[i].id.contains(".space_reverb.")){++inactiveSpace;CHECK(bits(peer->sound.values[i])!=bits(original));}
        }
        CHECK(continuous>0&&inactiveSpace==432);CHECK(controls.values!=peer->sound.values);
        capture->ownership.peers.push_back({fixtureGroupId(50),1,20,*peer});CHECK(validateGroupPerformanceCapture(*capture));
        const auto before=*capture;unsigned calls=0;
        const auto result=generateProposal(*r,[&](const juce::String&,std::string_view prompt,const juce::var&){
            ++calls;const auto parsed=parse(prompt);const auto linked=parsed["linked_context"];const auto value=linked["peers"][0];
            const auto* ids=linked["parameter_ids"].getArray();const auto* committed=value["committed"]["values_normalized"].getArray();const auto* observed=value["observations"]["controls"]["values_normalized"].getArray();
            CHECK(ids&&committed&&observed&&ids->size()==int(parameterCount)&&committed->size()==int(parameterCount)&&observed->size()==int(parameterCount));
            if(ids&&committed&&observed&&ids->size()==int(parameterCount)&&committed->size()==int(parameterCount)&&observed->size()==int(parameterCount))for(std::size_t i=0;i<parameterCount;++i){
                CHECK((*ids)[int(i)].toString()==registry[i].id);
                CHECK(bits(float(double((*committed)[int(i)])))==bits(peer->sound.values[i]));CHECK(bits(float(double((*observed)[int(i)])))==bits(controls.values[i]));
            }
            const auto note=value["committed"]["pattern"]["notes"][0];CHECK(double(note[0])==peer->pattern.notes[0].startBeats&&double(note[1])==peer->pattern.notes[0].durationBeats);
            rack::oauth::StructuredResult failed;failed.result.status=rack::oauth::Status::networkFailure;return failed;
        },{},capture.get());
        CHECK(calls==1&&result.status==GenerationStatus::providerFailure&&*capture==before);
    }
}
void bounded_group_prompt()
{
    begin(__func__); // Full metadata expansion, truncating peer semantics or duplicate selected definitions fail here.
    auto r = request(); r.source = fullPersonalityProject(); if (!r.source.sound.personalities || r.source.sound.personalities->instances.size() != 12) return;
    r.instruction = escapeText(8192); r.role = escapeText(128); r.key = r.role; r.scale = r.role;
    r.capturedPhrase = r.source.pattern; r.capturedBpm = 400; const auto before = r.source;
    unsigned calls = 0;
    auto ordinary = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var& schema) {
        ++calls; const auto p = parse(prompt); CHECK(prompt.size() <= 573952); CHECK(definitionCount(p) == 0);
        CHECK(p["selected_design"].isVoid()); summaries(p["personalities"], before); CHECK(juce::JSON::toString(p["catalog"], true, 17).getNumBytesAsUTF8() <= 196608);
        CHECK(juce::JSON::toString(p["context"], true, 17).getNumBytesAsUTF8() <= 40960); CHECK(juce::JSON::toString(schema, true, 17).getNumBytesAsUTF8() <= 65536);
        CHECK(juce::JSON::toString(p["policy"], true, 17).getNumBytesAsUTF8() + p["output_rules"].toString().getNumBytesAsUTF8() <= 65536);
        CHECK(p["instruction"].toString().toStdString() == r.instruction); oauthEnvelopeBudget(prompt, schema, false);
        return completed("{");
    });
    withheld(ordinary, GenerationStatus::budgetExhausted, 3); CHECK(calls == 3 && r.source == before);
    groupIntentFor(r.source, 1); r.tag.runtime = juce::Uuid(fixtureGroupId(40).getRawData());
    r.role = std::string(groupTextView(r.source.group->role)); r.key = std::string(groupTextView(r.source.group->key)); r.scale = std::string(groupTextView(r.source.group->scale));
    auto capture = std::make_unique<GroupPerformanceCapture>(); capture->ownership.registry = fixtureGroupId(99);
    capture->ownership.requesterRuntime = GroupUuid(r.tag.runtime); capture->ownership.loadRevision = 1; capture->ownership.revision = 432;
    capture->ownership.requester = *r.source.group; capture->requesterEditRevision = r.tag.editRevision; capture->peerCount = 4;
    for (unsigned n = 0; n < 4; ++n) {
        auto peer = fullPersonalityProject(); groupIntentFor(peer, n + 2); capture->ownership.peers.push_back({fixtureGroupId(n + 50), 1, n + 20, peer});
        auto& playback = capture->playback[n]; playback.controls.values = peer.sound.values; playback.controls.availability = ObservedMidiAvailability::stale;
        playback.controls.reason = LiveControlReason::clockRollback; playback.controls.ageMs = UINT64_MAX;
        playback.notes.meter = PhraseMeter{4, 4}; playback.notes.bpm = 400; playback.notes.windowBeats = 16; playback.notes.ageMs = UINT64_MAX;
        playback.notes.availability = ObservedMidiAvailability::stale; playback.notes.reason = ObservedMidiReason::clockRollback; playback.notes.truncated = true; playback.notes.noteCount = 256;
        for (std::size_t note = 0; note < 256; ++note) playback.notes.notes[note] = {double(note) / 32, note % 2 ? .015625 : 0.,
            std::uint8_t(note % 128), std::uint8_t(1 + note % 127), std::uint8_t(1 + note % 16), note % 2 == 0};
    }
    CHECK(validateGroupPerformanceCapture(*capture)); const auto captured = *capture; const auto requester = r.source; calls = 0;
    const auto grouped = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var& schema) {
        ++calls; const auto p = parse(prompt); CHECK(prompt.size() <= 999936); CHECK(definitionCount(p) == 0); oauthEnvelopeBudget(prompt, schema, true);
        summaries(p["personalities"], requester); const auto* peers = p["linked_context"]["peers"].getArray(); CHECK(peers && peers->size() == 4);
        std::size_t summaryBytes = juce::JSON::toString(p["personalities"], true, 17).getNumBytesAsUTF8();
        if (peers && peers->size() == 4) for (int n = 0; n < 4; ++n) {
            const auto& peer = (*peers)[n]; const auto& d = captured.ownership.peers[std::size_t(n)].project;
            summaries(peer["personalities"], d); summaryBytes += juce::JSON::toString(peer["personalities"], true, 17).getNumBytesAsUTF8();
            // Bound transmitted peer bytes, excluding the separately budgeted summaries.
            // Parsing then reprinting doubles would change these float numeric tokens.
            const auto rawLinked = rawJsonMember(prompt, "\"linked_context\"");
            const auto rawPeers = rawLinked ? rawJsonMember(rawLinked->value, "\"peers\"") : std::nullopt;
            const auto rawPeer = rawPeers ? rawJsonElement(rawPeers->value, std::size_t(n)) : std::nullopt;
            const auto rawSummary = rawPeer ? rawJsonMember(*rawPeer, "\"personalities\"") : std::nullopt;
            CHECK(rawPeer && rawSummary);
            if (rawPeer && rawSummary) {
                const auto contextBytes = rawPeer->size() - (rawSummary->eraseTo - rawSummary->eraseFrom);
                std::cout << "PEER_RAW_UTF8 peer=" << n << " context_bytes=" << contextBytes << " limit=81920\n";
                CHECK(contextBytes <= 81920);
            }
            const auto* values = peer["committed"]["values_normalized"].getArray(); const auto* observed = peer["observations"]["controls"]["values_normalized"].getArray();
            CHECK(values && values->size() == int(parameterCount) && observed && observed->size() == int(parameterCount));
            if (values && observed && values->size() == int(parameterCount) && observed->size() == int(parameterCount)) for (int v = 0; v < int(parameterCount); ++v) {
                CHECK(float(double((*values)[v])) == d.sound.values[std::size_t(v)]); CHECK(float(double((*observed)[v])) == captured.playback[std::size_t(n)].controls.values[std::size_t(v)]);
                CHECK(juce::JSON::toString((*values)[v], true, 17).getNumBytesAsUTF8() <= 25);
            }
            const auto* saved = peer["committed"]["pattern"]["notes"].getArray(); const auto* notes = peer["observations"]["notes"]["notes"].getArray();
            const auto committed = peer["committed"]; const auto* uses = committed["synth_use"].getArray(); const auto* order = committed["synth_order"].getArray();
            CHECK(uses && uses->size() == 4 && order && order->size() == 4);
            if (uses && order && uses->size() == 4 && order->size() == 4) for (int s = 0; s < 4; ++s) {
                CHECK(int((*uses)[s]) == static_cast<int>(d.sound.layout.synthUse[std::size_t(s)])); CHECK(int((*order)[s]) == d.sound.layout.synthOrder[std::size_t(s)]); }
            const auto* effects = committed["effect_slots"].getArray(); const auto* macros = committed["macro_bindings"].getArray(); const auto* effectOrders = committed["effect_order"].getArray();
            CHECK(effects && effects->size() == 8 && macros && macros->size() == 8 && effectOrders && effectOrders->size() == 5);
            if (effects && macros && effects->size() == 8 && macros->size() == 8) for (int e = 0; e < 8; ++e) {
                const auto* effect = (*effects)[e].getArray(); const auto* macro = (*macros)[e].getArray(); CHECK(effect && effect->size() == 3 && macro && macro->size() == 2);
                if (effect && effect->size() == 3) { const auto& slot = d.sound.layout.effects[std::size_t(e)]; CHECK(int((*effect)[0]) == static_cast<int>(slot.use));
                    CHECK(int((*effect)[1]) == static_cast<int>(slot.kind) && int((*effect)[2]) == slot.chain); }
                if (macro && macro->size() == 2) CHECK(int((*macro)[0]) == 0 && float(double((*macro)[1])) == d.sound.macros[std::size_t(e)].amount);
            }
            if (effectOrders && effectOrders->size() == 5) for (int chain = 0; chain < 5; ++chain) {
                const auto* rows = (*effectOrders)[chain].getArray(); CHECK(rows && rows->size() == d.sound.layout.effectCount[std::size_t(chain)]);
                if (rows && rows->size() == d.sound.layout.effectCount[std::size_t(chain)]) for (int position = 0; position < rows->size(); ++position)
                    CHECK(int((*rows)[position]) == d.sound.layout.effectOrder[std::size_t(chain)][std::size_t(position)]); }
            CHECK(int(committed["playback_mode"]) == static_cast<int>(d.mode));
            CHECK(int(committed["pattern"]["bars"]) == 8 && int(committed["pattern"]["meter_numerator"]) == 4 && int(committed["pattern"]["meter_denominator"]) == 4);
            CHECK(int(committed["pattern"]["grid"]) == static_cast<int>(d.pattern.grid) && double(committed["pattern"]["swing"]) == d.pattern.swing);
            const auto observedControls = peer["observations"]["controls"]; const auto observedNotes = peer["observations"]["notes"];
            CHECK(int(observedControls["availability"]) == static_cast<int>(ObservedMidiAvailability::stale) && int(observedControls["reason"]) == static_cast<int>(LiveControlReason::clockRollback));
            CHECK(int(observedNotes["availability"]) == static_cast<int>(ObservedMidiAvailability::stale) && int(observedNotes["reason"]) == static_cast<int>(ObservedMidiReason::clockRollback));
            CHECK(observedControls["age_ms"].toString() == "18446744073709551615" && bool(observedNotes["truncated"]));
            CHECK(double(observedNotes["bpm"]) == 400 && double(observedNotes["window_beats"]) == 16);
            CHECK(int(observedNotes["meter_numerator"]) == 4 && int(observedNotes["meter_denominator"]) == 4);
            CHECK(saved && saved->size() == 256 && notes && notes->size() == 256);
            if (saved && notes && saved->size() == 256 && notes->size() == 256) for (int note = 0; note < 256; ++note) {
                const auto* row = (*notes)[note].getArray(); const auto* savedRow = (*saved)[note].getArray(); CHECK(row && row->size() == 6 && savedRow && savedRow->size() == 5);
                CHECK(juce::JSON::toString((*notes)[note], true, 17).getNumBytesAsUTF8() + 1 <= 72); CHECK(juce::JSON::toString((*saved)[note], true, 17).getNumBytesAsUTF8() + 1 <= 64);
                if (row && row->size() == 6) { const auto& expected = captured.playback[std::size_t(n)].notes.notes[std::size_t(note)];
                    CHECK(double((*row)[0]) == expected.startBeats && double((*row)[1]) == expected.durationBeats);
                    CHECK(int((*row)[2]) == expected.pitch && int((*row)[3]) == expected.velocity && int((*row)[4]) == expected.channel && bool((*row)[5]) == expected.held); }
                if (savedRow && savedRow->size() == 5) { const auto& expected = d.pattern.notes[std::size_t(note)];
                    CHECK(double((*savedRow)[0]) == expected.startBeats && double((*savedRow)[1]) == expected.durationBeats);
                    CHECK(int((*savedRow)[2]) == expected.pitch && int((*savedRow)[3]) == expected.velocity && int((*savedRow)[4]) == expected.channel); }
            }
            const auto age = peer["observations"]["notes"]["age_ms"]; CHECK(age.isString() && age.toString() == "18446744073709551615");
            CHECK(peer["role"].toString().toStdString() == groupTextView(d.group->role)); CHECK(peer["key"].toString().toStdString() == groupTextView(d.group->key)); CHECK(peer["scale"].toString().toStdString() == groupTextView(d.group->scale));
        }
        CHECK(summaryBytes <= 122880); if (calls > 1) CHECK(juce::JSON::toString(p["repair"], true, 17).getNumBytesAsUTF8() <= 65536);
        return completed("{");
    }, {}, capture.get());
    withheld(grouped, GenerationStatus::budgetExhausted, 3); CHECK(calls == 3 && r.source == requester && *capture == captured);
}
void serialized_repair_boundaries()
{
    begin(__func__); // Raw-size checks alone miss JSON escaping and repair-field framing.
    auto r = request(); const auto source = r.source;
    for (const auto& text : {std::string(65536, 'x'), std::string(11000, '\x01'), std::string(33000, '"'), std::string(33000, '\\')}) {
        unsigned calls = 0; const auto result = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) { ++calls; return completed(text); });
        withheld(result, GenerationStatus::invalidRequest, 1); CHECK(calls == 1 && result.errorCode == "generation_repair_limit" && r.source == source);
    }
    // Literal field size=65536 is legal; one extra serialized byte forbids call2.
    auto repair = object({{"previous_response", ""}, {"proposal_status", "malformed"}, {"render_status", "invalidInput"}, {"measurements", {}}});
    const auto framing = juce::JSON::toString(repair, true, 17).getNumBytesAsUTF8(); CHECK(framing < 65536);
    for (int extra : {0, 1}) {
        const std::string previous(65536 - framing + std::size_t(extra), 'x'); unsigned calls = 0;
        const auto result = generateProposal(r, [&](const juce::String&, std::string_view prompt, const juce::var&) {
            ++calls; const auto p = parse(prompt); if (calls == 2) {
                CHECK(p["repair"]["previous_response"].toString().toStdString() == previous);
                CHECK(juce::JSON::toString(p["repair"], true, 17).getNumBytesAsUTF8() == 65536);
            }
            return completed(calls == 1 ? previous : json(wire()));
        });
        if (extra == 0) { ready(result, 2); CHECK(calls == 2); }
        else { withheld(result, GenerationStatus::invalidRequest, 1); CHECK(calls == 1 && result.errorCode == "generation_repair_limit"); }
        CHECK(r.source == source);
    }
}
}
// UNCOMPILED / SOURCE-ONLY EXTERNAL DRAFT. No intended RED/PASS or runtime claim.
// Basis: concrete contract B / AC8ABDA728A1083AEF51F3608C372E645CDCBFBAE4FB057EEDE9F923928729E1
// and member clarification C / 0E56E3B943CF294F1577C73253DD3AD138B00375CA0E238B84997E698E4C6D39.
// Explicit catalog decode/schema qualification D /
// 6C434A8958EC91F5675F447CA62743DFAC5753683952018A06E4993B10082F76.
// Routing: inherited parent model and reasoning effort; no overrides.
// After release and the frozen interface-declaration boundary, integrate this
// section into tests/native/GenerationRequestTests.cpp. Reuse its existing
// request/generationRecipe/object/array/json/parse/completed/physical/ready/
// previewBounds helpers, CHECK/begin counters and ordinary main. Add only the
// RackExamples include; no fixture framework, environment suffix or new target.
// The main below is the existing main plus one normally invoked owning section.
// This fragment depends on those existing helpers and the proposed declarations;
// compiling it as a separate translation unit is not the proposed owning check.

#include "RackExamples.h"

namespace
{
void catalog_complete_base_aggregate(const GenerationRequest& selectionRequest,
    const ProposalEdits& selectedIntent, const CatalogAuthority& authority)
{
    begin(__func__);
    auto r = selectionRequest;
    r.tag.requestId += 2;
    r.policy = {RequestScope::both, true, false};
    r.instruction = "Refine only the registered drive gain; retain the accepted full intent.";
    r.source.pattern.noteCount = 1;
    r.source.pattern.notes[0] = {0, .25, 48, 64, 1};
    r.capturedPhrase = r.source.pattern;
    const juce::Uuid synthId("12a01212-0001-4000-8000-000000000001");
    const juce::Uuid detachedId("12a01212-0001-4000-8000-000000000002");
    SoundDocument placed; RackDifference difference;
    RackOperation adopt;
    adopt.kind = RackOperationKind::adopt; adopt.newInstance = synthId;
    adopt.design = generationRecipe(); adopt.allocation = {{0, 0}};
    adopt.destination = {RackRole::synth, masterChain, 0};
    const auto adopted = makeRackCandidate(r.source.sound, adopt, SoundEditOrigin::manual, placed, difference);
    CHECK(adopted == RackCandidateStatus::ready);
    if (adopted != RackCandidateStatus::ready) return;
    r.source.sound = placed;
    RackOperation fx;
    fx.newInstance = detachedId; fx.design = generationRecipe(NativeModule::drive);
    fx.allocation = {{0, 0}}; fx.destination = {RackRole::fx, masterChain, 0};
    const auto fxPlaced = makeRackCandidate(r.source.sound, fx, SoundEditOrigin::manual, placed, difference);
    CHECK(fxPlaced == RackCandidateStatus::ready);
    if (fxPlaced != RackCandidateStatus::ready) return;
    r.source.sound = placed;
    r.catalog->authority = std::make_shared<const CatalogAuthority>(authority);
    r.catalog->baseIntent = selectedIntent;
    auto& base = r.catalog->baseIntent;
    base.parameters = {{"output.gain_db", .05f}};
    // The AI moves and removes placed racks; a bare bank or a detach is outside its reach.
    base.racks = {{RackEditKind::moveSynth, 0, masterChain, 0, EffectKind::drive, false}};
    base.macros = {{2, "catalog:7:0:analog.filter.cutoff", .25f}, {8, {}, 0}};
    base.reconciliation.remove = {detachedId};
    base.characteristics = "Accepted rounded tone with independent rack brightness";
    base.explanation = "Retain the accepted move, removal, macro and editable phrase while changing drive gain.";
    CHECK(base.catalogSelections.size() == 3 && base.catalogParameters.size() == 2);
    CHECK(base.catalogSelections.back().localId == 201 && base.catalogSelections.back().destination.role == RackRole::fx);
    CHECK(ai::detail::catalogAuthorityValid(authority));
    const auto original = r.source;
    auto refinement = generationWire2();
    juce::Array<juce::var> retainedSelections;
    for(const auto& selection:base.catalogSelections)retainedSelections.add(object({{"local_id",int(selection.localId)},
        {"design",selection.design.toDashedString()},{"revision",juce::String(std::to_string(selection.revision))},
        {"content_sha256",selection.contentSha256},{"destination",object({{"role",selection.destination.role==RackRole::synth?"synth":"fx"},
            {"chain",int(selection.destination.chain)},{"position",int(selection.destination.position)}})}}));
    refinement.getDynamicObject()->setProperty("catalog_selections", retainedSelections);
    refinement.getDynamicObject()->setProperty("catalog_parameter_edits", array({
        object({{"selection", 201}, {"unit", 0}, {"key", "drive.gain_db"}, {"normalized", .125}})}));

    ProposalEdits replyIntent;ProposalDecodeReason replyReason=ProposalDecodeReason::rootFields;
    CHECK(decodeProposal(json(refinement),r.policy,authority,replyIntent,&replyReason));
    CHECK(replyReason==ProposalDecodeReason::accepted&&replyIntent.catalogSelections==base.catalogSelections);

    // Both a prior phrase and an explicit empty replacement must reach the
    // callback as intent; the latter may not collapse to pattern=null.
    for (bool empty : {false, true}) {
        PatternDocument phrase;
        phrase.bars = 1; phrase.meter = {3, 8}; phrase.grid = PatternGrid::eighth; phrase.swing = .35;
        if (!empty) { phrase.noteCount = 1; phrase.notes[0] = {.25, .125, 65, 87, 2}; }
        base.pattern = phrase;
        CHECK(ai::detail::catalogRequestValid(r));
        ProjectDocument admitted;
        const auto admission = admitProposal(r.source, r.policy, base, authority, admitted);
        CHECK(admission == ProposalStatus::accepted);
        if (admission != ProposalStatus::accepted) return;
        CHECK(admitted.pattern == phrase && admitted.pattern != original.pattern);
        unsigned calls = 0;
        const auto result = generateProposal(r, [&](const juce::String& model, std::string_view prompt, const juce::var& schema) {
            ++calls;
            CHECK(model == r.model && prompt.size() <= 1048576);
            CHECK(juce::JSON::toString(schema, true, 17).getNumBytesAsUTF8() <= 65536);
            const auto payload = parse(prompt); const auto intent = payload["base_catalog_intent"];
            CHECK(intent.isObject());
            CHECK(json(intent["parameter_edits"]) == json(array({param("output.gain_db", double(.05f))})));
            CHECK(json(intent["rack_edits"]) == json(array({object({{"kind", "move_synth"}, {"slot", 0},
                {"chain", int(masterChain)}, {"position", 0}, {"effect", "drive"}})})));
            CHECK(json(intent["macro_edits"]) == json(array({
                object({{"index", 2}, {"target_id", "catalog:7:0:analog.filter.cutoff"}, {"amount", .25}}),
                object({{"index", 8}, {"target_id", {}}, {"amount", 0.}})})));
            CHECK(intent.hasProperty("pattern") && intent["pattern"].isObject());
            providerPattern(intent["pattern"], phraseWire(empty));
            CHECK(!intent.hasProperty("rack_design"));
            CHECK(intent["characteristics"].toString() == base.characteristics
                && intent["explanation"].toString() == base.explanation);
            const auto reconciliation = intent["reconciliation"];
            CHECK(json(reconciliation["remove"]) == json(array({detachedId.toDashedString()})));
            CHECK(json(reconciliation["detach"]) == json(array()) && json(reconciliation["replace"]) == json(array()));
            CHECK(json(reconciliation["controls"]) == json(array()));
            const auto* selections = intent["catalog_selections"].getArray();
            CHECK(selections && selections->size() == int(base.catalogSelections.size()));
            if (selections && selections->size() == int(base.catalogSelections.size())) for (int n = 0; n < selections->size(); ++n) {
                const auto& expected = base.catalogSelections[std::size_t(n)]; const auto& actual = (*selections)[n];
                CHECK(int(actual["local_id"]) == expected.localId && actual["design"].toString() == expected.design.toDashedString()
                    && actual["revision"].isString() && actual["revision"].toString().toStdString() == std::to_string(expected.revision)
                    && actual["content_sha256"].toString() == expected.contentSha256);
                CHECK(actual["destination"]["role"].toString() == (expected.destination.role == RackRole::synth ? "synth" : "fx")
                    && int(actual["destination"]["chain"]) == expected.destination.chain
                    && int(actual["destination"]["position"]) == expected.destination.position);
            }
            const auto* semantic = intent["catalog_parameter_edits"].getArray();
            CHECK(semantic && semantic->size() == int(base.catalogParameters.size()));
            if (semantic && semantic->size() == int(base.catalogParameters.size())) for (int n = 0; n < semantic->size(); ++n) {
                const auto& expected = base.catalogParameters[std::size_t(n)]; const auto& actual = (*semantic)[n];
                CHECK(int(actual["selection"]) == expected.selection && int(actual["unit"]) == expected.unit
                    && actual["key"].toString() == expected.key && double(actual["normalized"]) == double(expected.normalized));
            }
            const auto* definitions = payload["selected_definitions"].getArray();
            CHECK(definitions && definitions->size() == int(authority.definitions.size()) && authority.definitions.size() == 2);
            if (definitions && definitions->size() == int(authority.definitions.size())) for (int n = 0; n < definitions->size(); ++n) {
                const auto& expected = authority.definitions[std::size_t(n)];
                CHECK(json((*definitions)[n]) == json(parse(std::string_view(static_cast<const char*>(expected.canonical.getData()), expected.canonical.getSize()))));
            }
            CHECK(json(payload["proposal_schema"]) == json(schema) && json(schema) == json(proposalSchema(r.policy, authority)));
            CHECK(payload["context"]["saved_pattern"]["notes"].getArray()
                && payload["context"]["saved_pattern"]["notes"].getArray()->size() == 1);
            CHECK(payload["output_rules"].toString().contains("Complete base_catalog_intent")
                && payload["output_rules"].toString().contains("null pattern preserves its prior pattern intent"));
            // A new instance has no rack to copy a destination from, so the rules must say where a rack goes: the model
            // otherwise guessed chain 0 for a synth and every attempt was rejected.
            CHECK(payload["output_rules"].toString().contains("synth selection's destination chain is always 4")
                && payload["output_rules"].toString().contains("position 0 to 3")
                && payload["output_rules"].toString().contains("effect selection"));
            return completed(json(refinement));
        });
        std::cout<<"BASE_INTENT_RESULT empty="<<empty<<" calls="<<calls<<" status="<<int(result.status)<<" proposal="<<int(result.proposalStatus)<<" render="<<int(result.renderStatus)<<" error="<<result.errorCode<<" peak="<<result.measurements.peak<<" clipped="<<result.measurements.clippedSamples<<std::endl;
        CHECK(calls == 1 && calls <= 3 && result.inferenceCalls == 1);
        CHECK(r.source == original);
        if (!ready(result, 1)) return;
        CHECK(result.catalog && result.catalog->root == authority.root && result.catalog->snapshot == authority.snapshot);
        CHECK(result.edits.catalogSelections == selectedIntent.catalogSelections);
        CHECK(result.edits.pattern && *result.edits.pattern == phrase && result.candidate->pattern == phrase);
        CHECK(result.edits.parameters == base.parameters && result.edits.macros == base.macros);
        CHECK(result.edits.catalogParameters.size() == 3 && result.edits.catalogParameters.back().selection == 201
            && result.edits.catalogParameters.back().key == "drive.gain_db" && result.edits.catalogParameters.back().normalized == .125f);
        CHECK(result.candidate->sound.layout.effects[0].use == SlotUse::retired);
        CHECK(result.candidate->sound.macros == admitted.sound.macros);
        CHECK(result.candidate->sound.personalities && admitted.sound.personalities);
        if (result.candidate->sound.personalities && admitted.sound.personalities) {
            CHECK(result.candidate->sound.personalities->instances == admitted.sound.personalities->instances);
            const auto registered = std::find_if(result.candidate->sound.personalities->instances.begin(),
                result.candidate->sound.personalities->instances.end(), [&](const auto& instance) {
                    return instance.design.id == selectedIntent.catalogSelections.back().design;
                });
            CHECK(registered != result.candidate->sound.personalities->instances.end());
            if (registered != result.candidate->sound.personalities->instances.end()) {
                const auto* binding = rackUnitBinding(*registered, 0); CHECK(binding);
                if (binding) { const auto gain = rackParameterIndex(NativeModule::drive, binding->slot, "drive.gain_db");
                    CHECK(gain && result.candidate->sound.values[*gain] == .125f);
                }
            }
        }
    }
}

void catalog_selection_materialization_first()
{
    begin(__func__);
    auto r = request();
    r.policy.scope = RequestScope::sound;
    r.instruction = "Choose Copper Arc twice with independent brightness, plus the registered drive.";
    r.capturedPhrase->noteCount = 1;
    r.capturedPhrase->notes[0] = {0, .125, 60, 64, 1};
    r.capturedBpm = 120;
    physical(r.source, "output.gain_db", -24);
    const auto original = r.source;

    const auto factories = airack::test::frozenFactoryRackDesigns();
    CHECK(factories.size() == 21);
    const auto factory = std::find_if(factories.begin(), factories.end(), [](const auto& d) {
        return d.id == juce::Uuid("91387715-13d3-4c57-a479-12251842a402");
    });
    CHECK(factory != factories.end());
    if (factory == factories.end()) return;
    CHECK(factory->role == RackRole::synth && factory->units.size() == 1);

    auto portable = generationRecipe(NativeModule::drive);
    portable.id = juce::Uuid("124bc0e1-2be4-4df7-9251-12a012120001");
    portable.name = "Custom Task12 drive";
    portable.intent = "Portable native drive selected from an explicit saved source.";
    CHECK(validateRackDesign(portable, RackValidationMode::portableDesign));
    RackCatalogStatus catalogStatus = RackCatalogStatus::unavailable;
    const std::array<RackDesign, 1> customDefinitions{portable};

    RackCatalogSnapshot root;
    CHECK(captureRackCatalog(factories, customDefinitions, {}, root, &catalogStatus));
    CHECK(catalogStatus == RackCatalogStatus::ready && root.entries.size() == factories.size() + 1);
    RackCatalogPage complete;
    CHECK(catalogPage(root, {}, 0, 65536, complete, &catalogStatus));
    CHECK(complete.complete && complete.next == root.entries.size()
        && complete.entries.size() == root.entries.size() && complete.snapshot == root.id);
    if (!complete.complete || complete.entries.size() != root.entries.size()) return;
    auto findEntry = [&](juce::Uuid id) {
        return std::find_if(root.entries.begin(), root.entries.end(), [&](const auto& e) { return e.definition.id == id; });
    };
    const auto nativeEntry = findEntry(factory->id), portableEntry = findEntry(portable.id);
    CHECK(nativeEntry != root.entries.end() && portableEntry != root.entries.end());
    if (nativeEntry == root.entries.end() || portableEntry == root.entries.end()) return;
    CHECK(rackCatalogNativeEntry(root, std::size_t(nativeEntry - root.entries.begin())));
    CHECK(rackCatalogNativeEntry(root, std::size_t(portableEntry - root.entries.begin())));

    CatalogAuthority inventory;
    inventory.root = r.tag;
    inventory.snapshot = root.id;
    inventory.descriptorJson = juce::JSON::toString(rackCatalogPageDescriptor(complete), true, 22).toStdString();
    CatalogRequestContext select;
    select.phase = CatalogPhase::select;
    select.root = r.tag;
    select.authority = std::make_shared<const CatalogAuthority>(inventory);
    r.catalog = select;
    CHECK(r.catalog->authority->definitions.empty());

    auto selectionJson = [&](int local, const RackCatalogEntry& entry, RackRole role, int position) {
        return object({{"local_id", local}, {"design", entry.definition.id.toDashedString()},
            {"revision", juce::String(std::to_string(entry.definition.revision))},
            {"content_sha256", entry.contentSha256},
            {"destination", object({{"role", role == RackRole::synth ? "synth" : "fx"},
                {"chain", int(masterChain)}, {"position", position}})}});
    };
    auto proposal = generationWire2();
    auto* fields = proposal.getDynamicObject();
    fields->setProperty("version", 2);
    fields->setProperty("catalog_selections", array({
        selectionJson(7, *nativeEntry, RackRole::synth, 1),
        selectionJson(99, *nativeEntry, RackRole::synth, 2),
        selectionJson(201, *portableEntry, RackRole::fx, 0)}));
    fields->setProperty("catalog_parameter_edits", array({
        object({{"selection", 7}, {"unit", 0}, {"key", "analog.filter.cutoff"}, {"normalized", .25}}),
        object({{"selection", 99}, {"unit", 0}, {"key", "analog.filter.cutoff"}, {"normalized", .70}})}));
    ProposalEdits parsedIntent;
    CHECK(decodeProposal(json(proposal), r.policy, inventory, parsedIntent));
    CHECK(parsedIntent.catalogSelections.size() == 3 && parsedIntent.catalogParameters.size() == 2);
    ProposalEdits legacySentinel;
    legacySentinel.parameters.push_back({"output.gain_db", .17f});
    CHECK(!decodeProposal(json(proposal), r.policy, legacySentinel));
    CHECK(legacySentinel.parameters == std::vector<ParameterEdit>({{"output.gain_db", .17f}})
        && legacySentinel.catalogSelections.empty() && legacySentinel.catalogParameters.empty());

    // Captured authority chooses the closed catalog shape. Omitting both arrays
    // must repair/reject rather than downgrade to the legacy direct decoder.
    unsigned missingArrayCalls = 0;
    const auto missingArrays = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) {
        ++missingArrayCalls; return completed(json(generationWire2()));
    });
    CHECK(missingArrayCalls == 3 && missingArrays.inferenceCalls == 3);
    CHECK(missingArrays.status == GenerationStatus::budgetExhausted);
    CHECK(!missingArrays.candidate && !missingArrays.catalog && missingArrays.audio.left.empty());

    // Cancellation becomes observable after the post-provider check and before
    // the selection intermediate is published; no captured intent may escape.
    unsigned cancellationChecks = 0;
    const auto cancelledSelection = generateProposal(r, [&](const juce::String&, std::string_view, const juce::var&) {
        return completed(json(proposal));
    }, [&] { return ++cancellationChecks >= 4; });
    CHECK(cancelledSelection.status == GenerationStatus::cancelled);
    CHECK(!cancelledSelection.catalog && cancelledSelection.edits.catalogSelections.empty()
        && !cancelledSelection.candidate && cancelledSelection.audio.left.empty());

    unsigned providerCalls = 0;
    const auto selected = generateProposal(r, [&](const juce::String& model, std::string_view prompt, const juce::var& schema) {
        ++providerCalls;
        CHECK(model == r.model && prompt.size() <= rack::oauth::maximumStructuredPromptBytes);
        const auto payload = parse(prompt);
        const auto& delivered = payload["catalog"]["rack_catalog"];
        CHECK(delivered["snapshot"].toString() == root.id.toString() && bool(delivered["complete"]));
        const auto* entries = delivered["entries"].getArray();
        CHECK(entries && entries->size() == int(root.entries.size()));
        if (entries) for (const auto& captured : root.entries)
            CHECK(std::any_of(entries->begin(), entries->end(), [&](const auto& entry) {
                return entry["id"].toString() == captured.definition.id.toString()
                    && entry["revision"].toString().toStdString() == std::to_string(captured.definition.revision)
                    && entry["content_sha256"].toString() == captured.contentSha256;
            }));
        CHECK(json(payload["proposal_schema"]) == json(schema)
            && json(schema) == json(proposalSchema(r.policy, inventory)));
        return completed(json(proposal));
    });
    CHECK(providerCalls == 1 && selected.inferenceCalls == 1 && selected.tag == r.tag);
    CHECK(selected.status == GenerationStatus::catalogSelectionRequired);
    CHECK(selected.catalog && selected.catalog->root == r.tag && selected.catalog->snapshot == root.id);
    CHECK(!selected.candidate);
    CHECK(selected.audio.left.empty() && selected.audio.right.empty() && selected.audio.bodyFrames == 0);
    CHECK(selected.renderStatus == RenderStatus::invalidInput && selected.measurements.peak == 0
        && selected.measurements.rms == 0 && !selected.measurements.onsetSeconds);
    CHECK(selected.edits.catalogSelections.size() == 3 && selected.edits.catalogParameters.size() == 2);
    CHECK(selected.providerStatus == rack::oauth::Status::connected && selected.httpStatus == 200);
    CHECK(r.source == original);
    if (selected.status != GenerationStatus::catalogSelectionRequired || !selected.catalog) return;

    // Native hydration uses the sealed root once; identical selections share
    // canonical authority bytes while retaining independent local identities.
    auto materialized = inventory;
    for (const auto& selection : selected.edits.catalogSelections) {
        if (std::any_of(materialized.definitions.begin(), materialized.definitions.end(), [&](const auto& d) {
            return d.design == selection.design && d.revision == selection.revision
                && d.contentSha256 == selection.contentSha256;
        })) continue;
        RackDesign resolved;
        CHECK(resolveCatalogDesign(root, selection.design, selection.revision,
            selection.contentSha256, resolved, &catalogStatus));
        CatalogDefinition definition;
        definition.design = selection.design;
        definition.revision = selection.revision;
        definition.contentSha256 = selection.contentSha256;
        CHECK(encodeRackDesign(resolved, definition.canonical, RackValidationMode::instanceRecall));
        CHECK(definition.canonical.getSize() <= 65536
            && juce::SHA256(definition.canonical).toHexString() == definition.contentSha256);
        materialized.definitions.push_back(std::move(definition));
    }
    CHECK(materialized.definitions.size() == 2);
    CHECK(materialized.snapshot == root.id && materialized.descriptorJson == inventory.descriptorJson);

    auto followup = r;
    ++followup.tag.requestId; // Fresh outer transport ID; musical authority remains root.
    followup.catalog->phase = CatalogPhase::materialize;
    followup.catalog->selectionParent = selected.tag;
    followup.catalog->authority = std::make_shared<const CatalogAuthority>(materialized);
    followup.catalog->baseIntent = selected.edits;
    followup.catalog->inferenceCallsUsed = std::uint8_t(selected.inferenceCalls);
    followup.catalog->providerStatus = selected.providerStatus;
    followup.catalog->httpStatus = selected.httpStatus;
    unsigned materializationCalls = 0;
    const auto result = generateProposal(followup, [&](const juce::String&, std::string_view, const juce::var&) {
        ++materializationCalls;
        CHECK(false); // Materialization is native admit/render; a second inference fails.
        return completed(json(proposal));
    });
    CHECK(providerCalls == 1 && materializationCalls == 0);
    CHECK(result.tag == followup.tag && result.inferenceCalls == selected.inferenceCalls);
    CHECK(result.catalog && result.catalog->root == r.tag && result.catalog->snapshot == root.id);
    CHECK(result.providerStatus == selected.providerStatus && result.httpStatus == selected.httpStatus);
    CHECK(followup.source == original && r.source == original);
    if (!ready(result, 1)) return;
    previewBounds(result);
    CHECK(result.candidate->pattern == original.pattern);
    CHECK(result.candidate->sound.personalities && result.candidate->sound.personalities->instances.size() == 3);
    if (!result.candidate->sound.personalities) return;
    std::vector<const RackInstance*> duplicates;
    const RackInstance* custom = nullptr;
    for (const auto& instance : result.candidate->sound.personalities->instances) {
        if (instance.design.id == factory->id) duplicates.push_back(&instance);
        if (instance.design.id == portable.id) custom = &instance;
    }
    CHECK(duplicates.size() == 2 && custom);
    if (duplicates.size() == 2) {
        CHECK(duplicates[0]->id != duplicates[1]->id);
        const auto* a = rackUnitBinding(*duplicates[0], 0);
        const auto* b = rackUnitBinding(*duplicates[1], 0);
        CHECK(a && b && a->slot != b->slot);
        if (a && b) {
            const auto ai = rackParameterIndex(NativeModule::analog, a->slot, "analog.filter.cutoff");
            const auto bi = rackParameterIndex(NativeModule::analog, b->slot, "analog.filter.cutoff");
            CHECK(ai && bi);
            if (ai && bi) {
                const auto av = result.candidate->sound.values[*ai], bv = result.candidate->sound.values[*bi];
                CHECK((av == .25f && bv == .70f) || (av == .70f && bv == .25f));
            }
        }
    }
    if (custom) CHECK(validateRackDesign(custom->design, RackValidationMode::portableDesign));

    ProjectDocument repeated;
    CHECK(admitProposal(original, r.policy, selected.edits, materialized, repeated) == ProposalStatus::accepted);
    CHECK(repeated == *result.candidate); // Same root produces stable native IDs/banks.
    auto sentinel = original;
    sentinel.sound.values[index("output.gain_db")] = .17f;
    const auto beforeFailure = sentinel;
    CHECK(admitProposal(original, r.policy, selected.edits, sentinel) != ProposalStatus::accepted);
    CHECK(sentinel == beforeFailure); // Legacy overload grants no catalog authority.
    auto forged = selected.edits;
    forged.catalogSelections.back().contentSha256 =
        "0000000000000000000000000000000000000000000000000000000000000000";
    CHECK(admitProposal(original, r.policy, forged, materialized, sentinel) != ProposalStatus::accepted);
    CHECK(sentinel == beforeFailure);
    catalog_complete_base_aggregate(r, selected.edits, materialized);
}
}


// Task12 complete legal inventory envelope: real native capture and real
// page/provider serializers.
// Task12 worst-unique legal61 successor. Existing AA output is never reused.
void catalog_inventory_helper_admission_and_small_bytes()
{
    begin(__func__);CHECK(maximumCatalogDescriptorBytes==917504);
    const auto text=[](const juce::var& value){return juce::JSON::toString(value,true,22);};
    auto output=object({{"sentinel",true}});const auto unchanged=text(output);
    RackCatalogSnapshot unsealed;CHECK(!catalogInventoryFor(unsealed,output)&&text(output)==unchanged);
    RackCatalogSnapshot empty;CHECK(captureRackCatalog({}, {}, {},empty));
    RackCatalogPage emptyPage;CHECK(catalogPage(empty,{},0,65536,emptyPage)&&emptyPage.complete&&emptyPage.entries.empty()&&emptyPage.next==0);
    CHECK(catalogInventoryFor(empty,output)&&text(output)==text(rackCatalogPageDescriptor(emptyPage)));
    CHECK(output["entries"].isArray()&&output["entries"].getArray()->isEmpty()&&output["next"].toString()=="0"&&bool(output["complete"]));
    const auto factories=airack::test::frozenFactoryRackDesigns();
    RackCatalogSnapshot small;CHECK(captureRackCatalog(factories,{},{},small));
    RackCatalogPage page;CHECK(catalogPage(small,{},0,65536,page)&&page.complete&&page.entries.size()==21);
    const bool admitted=catalogInventoryFor(small,output);CHECK(admitted&&text(output)==text(rackCatalogPageDescriptor(page)));
    CHECK(small.entries.size()==21);if(!admitted||small.entries.size()!=21)return;
    for(unsigned which=0;which<2;++which){auto tampered=small;
        if(which==0)tampered.entries[0].definition.name+=" changed after seal";else tampered.entries[0].contentSha256="invalid hash";
        output=object({{"sentinel",true}});CHECK(!catalogInventoryFor(tampered,output)&&text(output)==unchanged);
    }
    auto stored=std::make_unique<GenerationRequest>(request());auto& r=*stored;
    auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=small.id;
    juce::var inventory;const bool inventoryOk=catalogInventoryFor(small,inventory);CHECK(inventoryOk);if(!inventoryOk)return;authority->descriptorJson=text(inventory).toStdString();
    CatalogRequestContext context;context.root=r.tag;context.authority=authority;r.catalog=context;
    CHECK(airack::ai::detail::catalogAuthorityValid(*authority)&&airack::ai::detail::catalogRequestValid(r));
    auto invalid=std::make_unique<GenerationRequest>(r);auto oversized=std::make_shared<CatalogAuthority>(*authority);
    oversized->descriptorJson.resize(maximumCatalogDescriptorBytes+1,' ');invalid->catalog->authority=oversized;
    CHECK(!airack::ai::detail::catalogAuthorityValid(*oversized)&&!airack::ai::detail::catalogRequestValid(*invalid));
    juce::MemoryBlock before,after;CHECK(encodeProjectState(invalid->source,before));unsigned calls=0;
    auto rejected=std::make_unique<GenerationResult>(generateProposal(*invalid,[&](const juce::String&,std::string_view,const juce::var&){++calls;return completed("{}");}));
    withheld(*rejected,GenerationStatus::invalidRequest,0);CHECK(calls==0);
    CHECK(encodeProjectState(invalid->source,after)&&after==before&&invalid->catalog->authority==oversized);
    CHECK(invalid->catalog->root==r.catalog->root&&oversized->descriptorJson.size()==maximumCatalogDescriptorBytes+1);
    CHECK(r.catalog->authority==authority&&authority->descriptorJson==text(inventory).toStdString());
}

void catalog_maximum_selected_materialization_refinement(const RackCatalogSnapshot& root,
    const std::vector<RackDesign>& embedded, const juce::File& output, std::size_t expectedSources)
{
    begin(__func__);std::cout<<"TASK13_NEXT 28 maximum_selection.entry"<<std::endl;CHECK(root.entries.size()==expectedSources&&embedded.size()==12);
    CHECK(!output.exists());if(output.exists()||root.entries.size()!=expectedSources||embedded.size()!=12)return;
    CHECK(output.createDirectory().wasOk());
    const auto unobserved=juce::JSON::toString(object({{"observed",false},{"reason","stage_not_reached"}}),true,22);
    for(const auto* file:{"refinement-prompt.json","refinement-schema.json","refinement-http-body.json"})CHECK(output.getChildFile(file).replaceWithText(unobserved));
    auto stored=std::make_unique<GenerationRequest>(request());auto& r=*stored;
    r.policy={RequestScope::both,true,false,false};r.instruction="Select all four synth and eight effect recipes, retaining independent local identities.";
    // A valid fresh empty rack source leaves all twelve banks available without reuse.
    r.source.sound.layout.synthUse.fill(SlotUse::neverUsed);r.source.sound.layout.synthOrder.fill(noSlot);r.source.sound.layout.synthCount=0;
    CHECK(validateProjectDocument(r.source));const auto original=std::make_unique<ProjectDocument>(r.source);
    PatternDocument phrase;phrase.bars=1;phrase.meter={3,8};phrase.grid=PatternGrid::eighth;phrase.swing=.35;
    phrase.noteCount=1;phrase.notes[0]={.25,.125,65,87,2};r.capturedPhrase=phrase;r.capturedBpm=120;
    juce::var inventory;const bool inventoryOk=catalogInventoryFor(root,inventory);CHECK(inventoryOk);if(!inventoryOk)return;
    const auto inventoryText=juce::JSON::toString(inventory,true,22);
    CHECK(bool(inventory["complete"])&&inventory["entries"].getArray()&&inventory["entries"].getArray()->size()==int(expectedSources)&&inventoryText.getNumBytesAsUTF8()<=maximumCatalogDescriptorBytes);
    auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=root.id;authority->descriptorJson=inventoryText.toStdString();
    CatalogRequestContext context;context.root=r.tag;context.authority=authority;r.catalog=context;
    auto proposal=generationWire2();juce::Array<juce::var> selections,semantic;
    for(std::size_t n=0;n<12;++n){const auto& design=embedded[n];const bool synth=n<4;
        const auto entry=std::find_if(root.entries.begin(),root.entries.end(),[&](const auto& e){return e.definition.id==design.id&&e.definition.revision==design.revision;});
        CHECK(entry!=root.entries.end());if(entry==root.entries.end())return;
        CHECK(rackCatalogNativeEntry(root,std::size_t(entry-root.entries.begin()))&&design.units.size()==1&&design.role==(synth?RackRole::synth:RackRole::fx));
        CHECK(design.name.getNumBytesAsUTF8()==64&&design.intent.getNumBytesAsUTF8()==512);
        selections.add(object({{"local_id",int(n)},{"design",design.id.toDashedString()},{"revision",juce::String(std::to_string(design.revision))},
            {"content_sha256",entry->contentSha256},{"destination",object({{"role",synth?"synth":"fx"},{"chain",int(masterChain)},{"position",int(synth?n:n-4)}})}}));
        semantic.add(object({{"selection",int(n)},{"unit",0},{"key",synth?"analog.filter.cutoff":"delay.mix"},{"normalized",synth?.5:.01}}));
    }
    auto* fields=proposal.getDynamicObject();fields->setProperty("catalog_selections",selections);fields->setProperty("catalog_parameter_edits",semantic);
    fields->setProperty("parameter_edits",array({param("output.gain_db",.15)}));fields->setProperty("pattern",phraseWire());
    fields->setProperty("macro_edits",array({object({{"index",1},{"target_id","catalog:0:0:analog.filter.cutoff"},{"amount",.25}}),
        object({{"index",8},{"target_id","catalog:11:0:delay.mix"},{"amount",.125}})}));
    ProposalEdits selectionPreflight;ProposalDecodeReason selectionReason;
    std::cout<<"TASK13_NEXT 28 selection_decode.before"<<std::endl;const bool selectionValid=decodeProposal(json(proposal),r.policy,*authority,selectionPreflight,&selectionReason);std::cout<<"TASK13_NEXT 28 selection_decode.after"<<std::endl;
    CHECK(selectionValid&&selectionReason==ProposalDecodeReason::accepted&&selectionPreflight.catalogSelections.size()==12&&selectionPreflight.catalogParameters.size()==12);
    if(!selectionValid)return;
    unsigned selectionCalls=0;
    auto selected=std::make_unique<GenerationResult>(generateProposal(r,[&](const juce::String&,std::string_view prompt,const juce::var&){
        ++selectionCalls;const auto payload=parse(prompt);CHECK(juce::JSON::toString(payload["catalog"]["rack_catalog"],true,22)==inventoryText);
        CHECK(payload["selected_definitions"].getArray()&&payload["selected_definitions"].getArray()->isEmpty());
        return completed(json(proposal));
    }));
    CHECK(selectionCalls==1&&selected->inferenceCalls==1&&selected->status==GenerationStatus::catalogSelectionRequired);
    CHECK(selected->catalog&&selected->catalog->root==r.tag&&selected->catalog->snapshot==root.id&&!selected->candidate&&selected->audio.left.empty());
    CHECK(selected->edits.catalogSelections.size()==12&&selected->edits.catalogParameters.size()==12&&selected->edits.parameters.size()==1&&selected->edits.macros.size()==2&&selected->edits.pattern==std::optional<PatternDocument>(phrase)&&r.source==*original);
    if(selected->status!=GenerationStatus::catalogSelectionRequired||selected->edits.catalogSelections.size()!=12||selected->edits.catalogParameters.size()!=12||selected->edits.parameters.size()!=1)return;
    auto hydrated=std::make_shared<CatalogAuthority>(*authority);std::size_t canonicalTotal=0;
    for(const auto& selection:selected->edits.catalogSelections){RackDesign resolved;
        CHECK(resolveCatalogDesign(root,selection.design,selection.revision,selection.contentSha256,resolved));
        CatalogDefinition definition;definition.design=selection.design;definition.revision=selection.revision;definition.contentSha256=selection.contentSha256;
        CHECK(encodeRackDesign(resolved,definition.canonical,RackValidationMode::instanceRecall)&&definition.canonical.getSize()<=65536);
        CHECK(juce::SHA256(definition.canonical).toHexString()==definition.contentSha256);canonicalTotal+=definition.canonical.getSize();hydrated->definitions.push_back(std::move(definition));
    }
    CHECK(hydrated->definitions.size()==12&&canonicalTotal<=12*65536&&ai::detail::catalogAuthorityValid(*hydrated));
    CHECK(hydrated->root==r.tag&&hydrated->snapshot==root.id&&hydrated->descriptorJson==authority->descriptorJson);
    auto followup=std::make_unique<GenerationRequest>(r);++followup->tag.requestId;auto& materialize=*followup->catalog;
    materialize.phase=CatalogPhase::materialize;materialize.selectionParent=selected->tag;materialize.authority=hydrated;materialize.baseIntent=selected->edits;
    materialize.inferenceCallsUsed=std::uint8_t(selected->inferenceCalls);materialize.providerStatus=selected->providerStatus;materialize.httpStatus=selected->httpStatus;
    CHECK(ai::detail::catalogRequestValid(*followup));unsigned materializationCalls=0;
    auto placed=std::make_unique<GenerationResult>(generateProposal(*followup,[&](const juce::String&,std::string_view,const juce::var&){++materializationCalls;return completed(json(proposal));}));
    CHECK(materializationCalls==0&&placed->inferenceCalls==1&&followup->source==*original);
    CHECK(output.getChildFile("materialization-result.json").replaceWithText(juce::JSON::toString(object({{"status",int(placed->status)},
        {"provider_calls",int(materializationCalls)},{"candidate_observed",bool(placed->candidate)},{"canonical_bytes",juce::var(juce::int64(canonicalTotal))}}),true,22)));
    if(!ready(*placed,1))return;
    CHECK(placed->edits.catalogSelections.size()==12&&placed->edits.catalogParameters.size()==12&&placed->edits.parameters.size()==1);
    if(placed->edits.catalogParameters.size()!=12||placed->edits.parameters.size()!=1)return;
    CHECK(placed->catalog&&placed->catalog->root==r.tag&&placed->catalog->snapshot==root.id&&placed->edits.catalogSelections==selected->edits.catalogSelections);
    CHECK(placed->candidate->sound.personalities&&placed->candidate->sound.personalities->instances.size()==12&&placed->candidate->sound.layout.synthCount==4&&placed->candidate->sound.layout.effectCount[masterChain]==8);
    CHECK(placed->candidate->pattern==phrase);previewBounds(*placed);
    auto refined=std::make_unique<GenerationRequest>(r);refined->tag.requestId+=2;refined->catalog->authority=hydrated;refined->catalog->baseIntent=placed->edits;
    refined->instruction="Change only local synth0 cutoff; retain all twelve selected recipes and prior accepted intent.";
    CHECK(ai::detail::catalogRequestValid(*refined));auto refinement=generationWire2();refinement.getDynamicObject()->setProperty("catalog_selections",selections);
    refinement.getDynamicObject()->setProperty("catalog_parameter_edits",array({object({{"selection",0},{"unit",0},{"key","analog.filter.cutoff"},{"normalized",.625}})}));
    refinement.getDynamicObject()->setProperty("characteristics",placed->edits.characteristics);refinement.getDynamicObject()->setProperty("explanation",placed->edits.explanation);
    ProposalEdits refinementPreflight;ProposalDecodeReason refinementReason;
    const bool refinementValid=decodeProposal(json(refinement),refined->policy,*hydrated,refinementPreflight,&refinementReason);
    CHECK(refinementValid&&refinementReason==ProposalDecodeReason::accepted&&refinementPreflight.catalogSelections.size()==12&&refinementPreflight.catalogParameters.size()==1);
    if(!refinementValid)return;
    const auto accountDirectory=output.getChildFile("synthetic-oauth-store");CHECK(accountDirectory.createDirectory().wasOk());
    {rack::oauth::detail::Store store(accountDirectory);std::atomic_bool cancelled=false;CHECK(store.lock(cancelled));
     CHECK(store.save(object({{"access_token","synthetic-local-test"},{"scope","chatgpt.tokens.use.direct resource.invoke"},{"expires_at",juce::var(juce::int64(2000))}})));}
    unsigned refinementCalls=0,httpCalls=0;bool promptObserved=false,bodyObserved=false;juce::var promptBytes,schemaBytes,bodyBytes;
    rack::oauth::OAuthProbe probe(accountDirectory,[&](const rack::oauth::HttpRequest& http,const std::atomic_bool&){
        ++httpCalls;bodyObserved=true;bodyBytes=juce::var(juce::int64(http.body.getNumBytesAsUTF8()));
        CHECK(http.method=="POST"&&http.url.endsWith("/responses")&&http.body.getNumBytesAsUTF8()<=rack::oauth::maximumStructuredRequestBytes);
        CHECK(output.getChildFile("refinement-http-body.json").replaceWithText(http.body));const auto body=juce::JSON::parse(http.body);
        CHECK(body["input"].getArray()&&body["input"].getArray()->size()==1&&(*body["input"].getArray())[0]["content"].toString()==output.getChildFile("refinement-prompt.json").loadFileAsString());
        CHECK(json(body["text"]["format"]["schema"])==json(juce::JSON::parse(output.getChildFile("refinement-schema.json").loadFileAsString())));
        const auto event=object({{"type","response.completed"},{"response",object({{"status","completed"},{"output",array({object({{"type","message"},
            {"content",array({object({{"type","output_text"},{"text",juce::String(json(refinement))}})})}})})}})}});
        return rack::oauth::HttpResponse{200,"event: response.completed\ndata: "+juce::JSON::toString(event,true,22)+"\n\n"};
    },[]{return int64_t(1000);});
    auto result=std::make_unique<GenerationResult>(generateProposal(*refined,[&](const juce::String& model,std::string_view prompt,const juce::var& schema){
        ++refinementCalls;promptObserved=true;promptBytes=juce::var(juce::int64(prompt.size()));schemaBytes=juce::var(juce::int64(juce::JSON::toString(schema,true,17).getNumBytesAsUTF8()));
        CHECK(prompt.size()<=rack::oauth::maximumStructuredPromptBytes&&juce::JSON::toString(schema,true,17).getNumBytesAsUTF8()<=rack::oauth::maximumStructuredSchemaBytes);
        CHECK(output.getChildFile("refinement-prompt.json").replaceWithText(juce::String::fromUTF8(prompt.data(),int(prompt.size()))));
        CHECK(output.getChildFile("refinement-schema.json").replaceWithText(juce::JSON::toString(schema,true,17)));const auto payload=parse(prompt);
        CHECK(juce::JSON::toString(payload["catalog"]["rack_catalog"],true,22)==inventoryText);
        const auto* definitions=payload["selected_definitions"].getArray();CHECK(definitions&&definitions->size()==12&&definitionCount(payload)==12);
        if(definitions&&definitions->size()==12)for(int n=0;n<12;++n){const auto& d=hydrated->definitions[std::size_t(n)];
            CHECK(juce::JSON::toString((*definitions)[n],true,22)==juce::JSON::toString(parse(std::string_view(static_cast<const char*>(d.canonical.getData()),d.canonical.getSize())),true,22));}
        const auto base=payload["base_catalog_intent"];CHECK(json(base["catalog_selections"])==json(juce::var(selections)));CHECK(!base.hasProperty("rack_design"));
        const auto* edits=base["catalog_parameter_edits"].getArray();CHECK(edits&&edits->size()==12);
        if(edits&&edits->size()==12)for(int n=0;n<12;++n){const auto& expected=placed->edits.catalogParameters[std::size_t(n)];const auto& actual=(*edits)[n];
            CHECK(int(actual["selection"])==expected.selection&&int(actual["unit"])==expected.unit&&actual["key"].toString()==expected.key&&double(actual["normalized"])==double(expected.normalized));}
        CHECK(json(base["parameter_edits"])==json(array({param("output.gain_db",double(placed->edits.parameters[0].normalized))})));
        CHECK(json(base["macro_edits"])==json(proposal["macro_edits"]));providerPattern(base["pattern"],phraseWire());
        CHECK(json(payload["proposal_schema"])==json(schema));return probe.requestStructured(model,prompt,schema);
    }));
    if(!bodyObserved)CHECK(bodyBytes.isVoid()&&httpCalls==0&&output.getChildFile("refinement-http-body.json").loadFileAsString()==unobserved);
    CHECK(output.getChildFile("measurement.json").replaceWithText(juce::JSON::toString(object({{"definitions",12},{"selections",12},{"synths",4},{"effects",8},
        {"selection_calls",int(selectionCalls)},{"materialization_calls",int(materializationCalls)},{"refinement_calls",int(refinementCalls)},{"http_calls",int(httpCalls)},
        {"prompt_observed",promptObserved},{"prompt_bytes",promptBytes},{"schema_bytes",schemaBytes},{"body_observed",bodyObserved},{"body_bytes",bodyBytes},{"status",int(result->status)}}),true,22)));
    CHECK(selectionCalls==1&&materializationCalls==0&&refinementCalls==1&&selectionCalls+refinementCalls<=3&&httpCalls==1&&promptObserved&&bodyObserved);
    CHECK(refined->source==*original&&r.source==*original);if(!ready(*result,1))return;
    CHECK(result->catalog&&result->catalog->root==r.tag&&result->catalog->snapshot==root.id&&result->edits.catalogSelections==placed->edits.catalogSelections);
    auto expected=placed->edits;expected.catalogParameters[0].normalized=.625f;
    CHECK(result->edits.catalogParameters==expected.catalogParameters&&result->edits.parameters==expected.parameters&&result->edits.macros==expected.macros&&result->edits.pattern==expected.pattern);
    CHECK(result->candidate->sound.macros==placed->candidate->sound.macros&&result->candidate->pattern==phrase);
    auto repeated=std::make_unique<ProjectDocument>();CHECK(admitProposal(*original,r.policy,result->edits,*hydrated,*repeated)==ProposalStatus::accepted&&*repeated==*result->candidate);
    CHECK(result->candidate->sound.personalities&&result->candidate->sound.personalities->instances.size()==12);
    if(result->candidate->sound.personalities&&result->candidate->sound.personalities->instances.size()==12&&placed->candidate->sound.personalities&&placed->candidate->sound.personalities->instances.size()==12)for(std::size_t n=0;n<12;++n){const auto& before=placed->candidate->sound.personalities->instances[n];const auto& after=result->candidate->sound.personalities->instances[n];
        CHECK(before.id==after.id&&before.design==after.design&&before.bindings==after.bindings);}
}


std::vector<RackDesign> branchSaturationLibrary()
{
    const auto examples=airack::test::frozenExampleRackDesigns();const auto factory=airack::test::frozenFactoryRackDesigns();
    CHECK(examples.size()==6&&factory.size()==21);
    std::vector<RackDesign> result(examples.begin(),examples.begin()+4);
    for(const auto& design:factory)if(design.units[0].module!=NativeModule::spaceReverb)result.push_back(design);
    CHECK(result.size()==24);return result;
}
void catalog_complete_61_inventory_envelope(int sourceEntries,bool mainHistory=false,bool acceptedMain=false,bool releasedMain=false,bool currentCatalog=false)
{
    begin(__func__);CHECK((sourceEntries>=65&&sourceEntries<=68)||(currentCatalog&&sourceEntries==71));
    const int externalEntries=releasedMain?42:44;acceptedMain=acceptedMain||releasedMain||currentCatalog;
    const bool beforeTape=sourceEntries<68&&!mainHistory&&!acceptedMain;
    const auto output=juce::File::getCurrentWorkingDirectory().getChildFile("build/task12-inventory-aj-output-"+juce::Uuid().toString());
    CHECK(!output.exists()); if(output.exists())return;
    CHECK(output.createDirectory().wasOk());
    const auto sources=output.getChildFile("canonical-sources");CHECK(sources.createDirectory().wasOk());
    auto requestStorage=std::make_unique<GenerationRequest>(request());auto& r=*requestStorage;
    r.source=fullPersonalityProject(beforeTape,!acceptedMain);
    CHECK(r.source.sound.personalities&&r.source.sound.personalities->instances.size()==12);
    if(!r.source.sound.personalities||r.source.sound.personalities->instances.size()!=12)return;
    std::vector<juce::String> names,intents;
    auto uniqueMaximumText=[&](RackDesign& d,unsigned ordinal){
        CHECK(ordinal<44);const auto originalId=d.id;
        std::string name(64,'\x01'),intent(512,'\x01');
        // U+0001/U+0002 each needs six JSON bytes. Six binary positions identify
        // all44 legal sources without inserting shorter unescaped ASCII suffixes.
        for(unsigned bit=0;bit<6;++bit)if(ordinal&(1u<<bit)){name[bit]='\x02';intent[bit]='\x02';}
        d.name=juce::String::fromUTF8(name.data(),int(name.size()));d.intent=juce::String::fromUTF8(intent.data(),int(intent.size()));
        for(auto& section:d.panel.sections)section=d.name;for(auto& control:d.panel.controls)control.label=d.name;
        CHECK(d.id==originalId&&d.name.getNumBytesAsUTF8()==64&&d.intent.getNumBytesAsUTF8()==512);
        CHECK(juce::JSON::toString(juce::var(d.name),true).getNumBytesAsUTF8()==6*64+2);
        CHECK(juce::JSON::toString(juce::var(d.intent),true).getNumBytesAsUTF8()==6*512+2);
        CHECK(std::find(names.begin(),names.end(),d.name)==names.end()&&std::find(intents.begin(),intents.end(),d.intent)==intents.end());
        names.push_back(d.name);intents.push_back(d.intent);
        CHECK(validateRackDesign(d,RackValidationMode::portableDesign));
        juce::MemoryBlock canonical;CHECK(encodeRackDesign(d,canonical)&&canonical.getSize()<=65536);
    };
    auto personalities=std::make_shared<RackPersonalityState>(*r.source.sound.personalities);
    for(unsigned n=0;n<12;++n)uniqueMaximumText(personalities->instances[n].design,n);
    r.source.sound.personalities=std::move(personalities);CHECK(validateProjectDocument(r.source));
    juce::MemoryBlock projectBytes;CHECK(encodeProjectState(r.source,projectBytes));
    CHECK(output.getChildFile("requester.airackproject").replaceWithData(projectBytes.getData(),projectBytes.getSize()));
    std::vector<RackDesign> embedded;for(const auto& i:r.source.sound.personalities->instances)embedded.push_back(i.design);
    auto factories=branchSaturationLibrary();
    if(releasedMain||currentCatalog){factories=airack::test::frozenLibraryRackDesigns();CHECK(factories.size()==28);
        std::erase(factories,airack::test::frozenTileArrayRackDesign());
        if(releasedMain)std::erase_if(factories,[](const auto& d){return d.units[0].module==NativeModule::saturation;});
        CHECK(factories.size()==std::size_t(currentCatalog?27:25));}
    if(beforeTape)std::erase_if(factories,[](const auto& d){return d.id==juce::Uuid("330cb53b-cd9b-443c-94d8-2700bc8b88f7");});
    // Keep the predecessor's exact recipes alongside the accepted main and integrated inventories.
    if(!acceptedMain)for(auto& d:factories)for(auto& u:d.units)if(u.module==NativeModule::delay)
        std::erase_if(u.defaults,[](const auto& p){return p.key=="delay.note"||p.key=="delay.right_ratio"||p.key=="delay.ping_pong";});
    if(factories.size()<std::size_t(sourceEntries-externalEntries))return;
    factories.resize(std::size_t(sourceEntries-externalEntries)); // Frozen65/66 prefixes and complete current68. Exact65 byte oracles remain below.
    auto rich=maximumTextRecipe(NativeModule::analog);const auto fx=maximumTextRecipe(NativeModule::delay,beforeTape,!acceptedMain);
    for(std::uint8_t unit=1;unit<=8;++unit){auto u=fx.units[0];u.id=unit;for(auto& d:u.defaults)d.unit=unit;rich.units.push_back(std::move(u));}
    CHECK(rich.units.size()==9&&validateRackDesign(rich,RackValidationMode::portableDesign));
    RackCatalogStatus status=RackCatalogStatus::unavailable;
    for(unsigned n=0;n<unsigned(externalEntries-12);++n){auto d=rich;d.id=juce::Uuid();uniqueMaximumText(d,12+n);embedded.push_back(std::move(d));}
    CHECK(names.size()==std::size_t(externalEntries)&&intents.size()==std::size_t(externalEntries)&&embedded.size()==std::size_t(externalEntries));
    RackCatalogSnapshot captured;
    const bool captureOk=captureRackCatalog(factories,embedded,{},captured,&status);
    CHECK(captureOk&&status==RackCatalogStatus::ready&&captured.entries.size()==std::size_t(sourceEntries));
    if(!captureOk||captured.entries.size()!=std::size_t(sourceEntries))return;
    std::size_t canonicalTotal=0,canonicalMaximum=0;unsigned native=0;
    for(std::size_t n=0;n<captured.entries.size();++n){const auto& e=captured.entries[n];
        const bool trusted=rackCatalogNativeEntry(captured,n);native+=trusted;
        for(const auto& unit:e.definition.units)CHECK(unit.defaults.size()<=(unit.module==NativeModule::spaceReverb?97:unit.module==NativeModule::dynamicEq?(releasedMain||currentCatalog?116:90):unit.module==NativeModule::analog||unit.module==NativeModule::wavetable?140:trusted?82:23));
        juce::MemoryBlock canonical;CHECK(encodeRackDesign(e.definition,canonical,trusted?RackValidationMode::instanceRecall:RackValidationMode::portableDesign));
        CHECK(canonical.getSize()<=65536&&juce::SHA256(canonical).toHexString()==e.contentSha256);
        CHECK(sources.getChildFile(juce::String(int(n))+"-"+e.definition.id.toDashedString()+".airackrack").replaceWithData(canonical.getData(),canonical.getSize()));
        canonicalTotal+=canonical.getSize();canonicalMaximum=std::max(canonicalMaximum,canonical.getSize());}
    CHECK(native==unsigned(sourceEntries)&&canonicalTotal<=64*65536);
    // Full evidence never becomes a substitute for an incomplete captured page.
    RackCatalogPage full;full.snapshot=captured.id;full.entries=captured.entries;full.next=std::size_t(sourceEntries);full.complete=true;
    const auto completeText=juce::JSON::toString(rackCatalogPageDescriptor(full),true,22);
    CHECK(output.getChildFile("complete-inventory.json").replaceWithText(completeText));
    RackCatalogPage bounded;status=RackCatalogStatus::unavailable;
    const bool pageOk=catalogPage(captured,{},0,65536,bounded,&status);
    const auto boundedText=juce::JSON::toString(rackCatalogPageDescriptor(bounded),true,22);
    CHECK(output.getChildFile("bounded-page.json").replaceWithText(boundedText));
    CHECK(pageOk&&status==RackCatalogStatus::ready&&!bounded.complete&&boundedText.getNumBytesAsUTF8()<=65536);
    if(releasedMain){CHECK(sourceEntries==67);CHECK(bounded.entries.size()==28&&bounded.next==28&&boundedText.getNumBytesAsUTF8()==60827);CHECK(completeText.getNumBytesAsUTF8()==526785);}
    if(mainHistory){CHECK(sourceEntries==66);CHECK(bounded.entries.size()==27&&bounded.next==27&&boundedText.getNumBytesAsUTF8()==64783);CHECK(completeText.getNumBytesAsUTF8()==518218);}
    if(acceptedMain&&sourceEntries==66){CHECK(bounded.entries.size()==27&&bounded.next==27&&boundedText.getNumBytesAsUTF8()==64919);CHECK(completeText.getNumBytesAsUTF8()==545969);}
    if(sourceEntries==65){CHECK(bounded.entries.size()==26&&bounded.next==26&&boundedText.getNumBytesAsUTF8()==63257);CHECK(completeText.getNumBytesAsUTF8()==462777);}
    CHECK(maximumCatalogDescriptorBytes==917504&&completeText.getNumBytesAsUTF8()<=maximumCatalogDescriptorBytes);
    std::size_t cursor=0,seen=0,pages=0;
    while(cursor<captured.entries.size()){RackCatalogPage page;CHECK(catalogPage(captured,{},cursor,65536,page,&status));
        CHECK(page.next>cursor&&!page.entries.empty());if(page.next<=cursor)break;
        for(const auto& entry:page.entries){CHECK(entry==captured.entries[seen]);++seen;}cursor=page.next;++pages;
        CHECK(page.complete==(cursor==captured.entries.size()));}
    CHECK(seen==std::size_t(sourceEntries)&&pages>1);
    auto inventory=object({{"sentinel",true}});const auto inventorySentinel=juce::JSON::toString(inventory,true,22);
    const bool inventoryOk=catalogInventoryFor(captured,inventory);const auto inventoryText=juce::JSON::toString(inventory,true,22);
    if(!inventoryOk)CHECK(inventoryText==inventorySentinel);
    const auto* inventoryEntries=inventory["entries"].getArray();
    const bool inventoryComplete=inventoryOk&&inventoryEntries&&inventoryEntries->size()==sourceEntries&&bool(inventory["complete"])
        &&inventory["next"].toString()==juce::String(sourceEntries)&&inventory["snapshot"].toString()==captured.id.toString();
    CHECK(inventoryComplete&&inventoryText==completeText);
    if(inventoryComplete)for(int n=0;n<sourceEntries;++n)CHECK(juce::JSON::toString((*inventoryEntries)[n],true,22)==juce::JSON::toString(rackCatalogDescriptor(captured.entries[std::size_t(n)]),true,22));
    CHECK(output.getChildFile("complete-helper.json").replaceWithText(inventoryText));
    auto catalog=object({{"sentinel",true}});const auto sentinel=json(catalog);
    const bool providerOk=providerCatalogFor(r.source,captured,catalog);
    if(!providerOk)CHECK(json(catalog)==sentinel);
    CHECK(output.getChildFile("provider-catalog.json").replaceWithText(juce::JSON::toString(catalog,true,22)));
    CHECK(providerOk&&juce::JSON::toString(catalog["rack_catalog"],true,22)==inventoryText);
    const auto unobserved=juce::JSON::toString(object({{"observed",false},{"reason","stage_not_reached"}}),true,22);
    CHECK(output.getChildFile("prompt.json").replaceWithText(unobserved));
    CHECK(output.getChildFile("schema.json").replaceWithText(unobserved));
    CHECK(output.getChildFile("http-body.json").replaceWithText(unobserved));
    const auto wireUnobserved=juce::JSON::toString(object({{"observed",false},{"reason","existing_Generation_target_does_not_link_CompanionWire"}}),true,22);
    CHECK(output.getChildFile("wire-unobserved.json").replaceWithText(wireUnobserved));
    bool promptObserved=false,fullBodyObserved=false;unsigned providerCalls=0,httpCalls=0;
    juce::var wireBytes,promptBytes,schemaBytes,bodyBytes;
    juce::String generationError="not_attempted_incomplete_inventory",oauthError="not_attempted_before_provider_callback";
    int generationStatus=-1,oauthStatus=-1;
    if(inventoryComplete&&providerOk){
        auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=captured.id;
        authority->descriptorJson=inventoryText.toStdString();CatalogRequestContext context;context.root=r.tag;context.authority=authority;r.catalog=context;
            const auto accountDirectory=output.getChildFile("synthetic-oauth-store");CHECK(accountDirectory.createDirectory().wasOk());
            {rack::oauth::detail::Store store(accountDirectory);std::atomic_bool cancelled=false;CHECK(store.lock(cancelled));
             CHECK(store.save(object({{"access_token","synthetic-local-test"},{"scope","chatgpt.tokens.use.direct resource.invoke"},{"expires_at",juce::var(juce::int64(2000))}})));}
            rack::oauth::OAuthProbe probe(accountDirectory,[&](const rack::oauth::HttpRequest& http,const std::atomic_bool&){
                ++httpCalls;fullBodyObserved=true;bodyBytes=juce::var(juce::int64(http.body.getNumBytesAsUTF8()));
                CHECK(http.method=="POST"&&http.url.endsWith("/responses")&&http.body.getNumBytesAsUTF8()<=rack::oauth::maximumStructuredRequestBytes);
                CHECK(output.getChildFile("http-body.json").replaceWithText(http.body));
                const auto body=juce::JSON::parse(http.body);const auto* input=body["input"].getArray();CHECK(input&&input->size()==1);
                CHECK(input&&input->size()==1&&(*input)[0]["content"].toString()==output.getChildFile("prompt.json").loadFileAsString());
                CHECK(json(body["text"]["format"]["schema"])==json(juce::JSON::parse(output.getChildFile("schema.json").loadFileAsString())));
                return rack::oauth::HttpResponse{429,"{\"error\":{\"code\":\"rate_limit_exceeded\"}}"};
            },[]{return int64_t(1000);});
            auto result=std::make_unique<GenerationResult>(generateProposal(r,[&](const juce::String& model,std::string_view prompt,const juce::var& schema){
                ++providerCalls;promptObserved=true;promptBytes=juce::var(juce::int64(prompt.size()));schemaBytes=juce::var(juce::int64(juce::JSON::toString(schema,true,17).getNumBytesAsUTF8()));
                CHECK(prompt.size()<=rack::oauth::maximumStructuredPromptBytes&&juce::JSON::toString(schema,true,17).getNumBytesAsUTF8()<=rack::oauth::maximumStructuredSchemaBytes);
                CHECK(output.getChildFile("prompt.json").replaceWithText(juce::String::fromUTF8(prompt.data(),int(prompt.size()))));
                CHECK(output.getChildFile("schema.json").replaceWithText(juce::JSON::toString(schema,true,17)));
                const auto payload=parse(prompt);const auto inventory=payload["catalog"]["rack_catalog"];
                CHECK(juce::JSON::toString(inventory,true,22)==inventoryText);
                auto reply=probe.requestStructured(model,prompt,schema);oauthStatus=int(reply.result.status);oauthError=reply.result.errorCode;
                if(!fullBodyObserved)CHECK(httpCalls==0&&reply.result.errorCode=="structured_request_limit");
                else CHECK(httpCalls==1&&reply.result.httpStatus==429);
                return reply;
            }));
            generationStatus=int(result->status);generationError=result->errorCode;
            CHECK(providerCalls<=1&&httpCalls<=1&&!result->candidate&&result->audio.left.empty()&&result->audio.right.empty());
    }
    CHECK(output.getChildFile("wire-unobserved.json").loadFileAsString()==wireUnobserved&&wireBytes.isVoid());
    if(!providerOk)CHECK(json(catalog)==sentinel&&!promptObserved&&!fullBodyObserved&&providerCalls==0&&httpCalls==0);
    if(!promptObserved){CHECK(output.getChildFile("prompt.json").loadFileAsString()==unobserved);CHECK(output.getChildFile("schema.json").loadFileAsString()==unobserved);}
    if(!fullBodyObserved)CHECK(output.getChildFile("http-body.json").loadFileAsString()==unobserved&&bodyBytes.isVoid());
    const auto measured=object({{"capture_ok",captureOk},{"source_entries",sourceEntries},{"factory_entries",sourceEntries-externalEntries},{"embedded_entries",externalEntries},{"unique_maximum_text_entries",externalEntries},
        {"name_bytes",64},{"intent_bytes",512},{"name_json_bytes",6*64+2},{"intent_json_bytes",6*512+2},
        {"native_entries",int(native)},{"canonical_total",juce::var(juce::int64(canonicalTotal))},{"canonical_maximum",juce::var(juce::int64(canonicalMaximum))},
        {"complete_descriptor_bytes",juce::var(juce::int64(completeText.getNumBytesAsUTF8()))},{"page_ok",pageOk},{"page_status",int(status)},
        {"page_complete",bounded.complete},{"page_entries",int(bounded.entries.size())},{"page_next",int(bounded.next)},
        {"bounded_descriptor_bytes",juce::var(juce::int64(boundedText.getNumBytesAsUTF8()))},{"helper_ok",inventoryOk},{"helper_complete",inventoryComplete},
        {"helper_descriptor_bytes",inventoryOk?juce::var(juce::int64(inventoryText.getNumBytesAsUTF8())):juce::var()},{"helper_matches_full",inventoryOk&&inventoryText==completeText},{"provider_complete",providerOk},
        {"wire_observed",false},{"wire_bytes",wireBytes},{"wire_reason","existing_Generation_target_does_not_link_CompanionWire"},
        {"prompt_observed",promptObserved},{"prompt_bytes",promptBytes},{"schema_bytes",schemaBytes},
        {"provider_calls",int(providerCalls)},{"http_calls",int(httpCalls)},{"full_body_observed",fullBodyObserved},{"full_body_bytes",bodyBytes},
        {"generation_status",generationStatus},{"generation_error",generationError},{"oauth_status",oauthStatus},{"oauth_error",oauthError}});
    CHECK(output.getChildFile("measurement.json").replaceWithText(juce::JSON::toString(measured,true,22)));
    std::cout<<"Task12 worst-unique AH envelope "<<juce::JSON::toString(measured,true,22)<<std::endl;
    // One complete legal inventory must pass every actual unchanged boundary.
    CHECK(inventoryComplete&&inventoryText==completeText&&providerOk);
    CHECK(promptObserved&&fullBodyObserved&&providerCalls==1&&httpCalls==1);
    std::cout<<"TASK13_NEXT 28 maximum_selection_call.before"<<std::endl;catalog_maximum_selected_materialization_refinement(captured,{embedded.begin(),embedded.begin()+12},output.getChildFile("selected-maximum"),std::size_t(sourceEntries));std::cout<<"TASK13_NEXT 28 maximum_selection_call.after"<<std::endl;
}
// Current library admission is separate from the unchanged historical melodic envelope fixtures.
void current_tile_catalog_modes()
{
    begin(__func__);const auto library=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
    CHECK(library.size()==28&&std::count(library.begin(),library.end(),tile)==1
        &&std::count_if(library.begin(),library.end(),[&](const auto& d){return d.id==tile.id;})==1);
    RackCatalogSnapshot captured;CHECK(captureRackCatalog(library,{},{},captured));
    CHECK(captured.entries.size()==28);if(captured.entries.size()!=28)return;
    const auto originalEntries=captured.entries;const auto originalId=captured.id;
    juce::var owned,legacy;CHECK(catalogInventoryFor(captured,CatalogRendering::ownedLocal,owned));
    CHECK(catalogInventoryFor(captured,legacy));
    CHECK(owned["entries"].size()==28&&owned["next"].toString()=="28"&&bool(owned["complete"]));
    CHECK(legacy["entries"].size()==27&&legacy["next"].toString()=="27"&&bool(legacy["complete"]));
    CHECK(owned["snapshot"].toString()==captured.id.toString()&&legacy["snapshot"].toString()==captured.id.toString());
    if(owned["entries"].size()!=28||legacy["entries"].size()!=27)return;
    std::size_t legacyIndex=0,tileIndex=captured.entries.size();juce::MemoryBlock tileBytes;
    CHECK(encodeRackDesign(tile,tileBytes,RackValidationMode::instanceRecall));const auto tileHash=juce::SHA256(tileBytes).toHexString();
    for(std::size_t n=0;n<captured.entries.size();++n){const auto& entry=captured.entries[n];
        CHECK(juce::JSON::toString(owned["entries"][int(n)],true,22)==juce::JSON::toString(rackCatalogDescriptor(entry),true,22));
        if(entry.definition.id==tile.id){tileIndex=n;CHECK(entry.definition==tile&&entry.contentSha256==tileHash&&rackCatalogNativeEntry(captured,n));}
        else{CHECK(legacyIndex<27);if(legacyIndex>=27)return;
            CHECK(juce::JSON::toString(legacy["entries"][int(legacyIndex++)],true,22)==juce::JSON::toString(rackCatalogDescriptor(entry),true,22));}}
    CHECK(tileIndex<captured.entries.size()&&legacyIndex==27);
    auto source=std::make_unique<ProjectDocument>();juce::var ownedProvider,legacyProvider,defaultProvider;
    CHECK(providerCatalogFor(*source,captured,CatalogRendering::ownedLocal,ownedProvider));
    CHECK(providerCatalogFor(*source,captured,legacyProvider));CHECK(providerCatalogFor(*source,defaultProvider));
    CHECK(ownedProvider["rack_catalog"]["entries"].size()==28&&ownedProvider["module_keys"]["drum"].isArray());
    CHECK(legacyProvider["rack_catalog"]["entries"].size()==27&&legacyProvider["module_keys"]["drum"].isVoid());
    CHECK(defaultProvider["rack_catalog"]["entries"].size()==17&&defaultProvider["module_keys"]["drum"].isVoid());
    RackCatalogSnapshot builtins;CHECK(captureRackCatalog(libraryRackDesigns(),{},{},builtins));CHECK(builtins.entries.size()==17);
    for(std::size_t n=0;n<builtins.entries.size();++n)CHECK(juce::JSON::toString(defaultProvider["rack_catalog"]["entries"][int(n)],true,22)==juce::JSON::toString(rackCatalogDescriptor(builtins.entries[n]),true,22));
    for(const auto* provider:{&ownedProvider,&legacyProvider,&defaultProvider}){
        const auto keys=(*provider)["module_keys"]["saturation"];CHECK(keys.isArray()&&keys.size()==36);
        if(keys.isArray()){
            CHECK(std::count_if(keys.getArray()->begin(),keys.getArray()->end(),[](const auto& key){return key[0].toString().startsWith("saturation.");})==35);
            CHECK(std::count_if(keys.getArray()->begin(),keys.getArray()->end(),[](const auto& key){return key[0].toString()=="bypassed";})==1);
            for(const auto& key:*keys.getArray())CHECK(key[0].toString()=="bypassed"||key[0].toString().startsWith("saturation."));}}
    CHECK(unsigned(NativeModule::drum)==20&&unsigned(NativeModule::saturation)==21);
    CHECK(captured.id==originalId&&captured.entries==originalEntries);
    if(tileIndex<captured.entries.size()){auto tampered=captured;tampered.entries[tileIndex].definition.name+=" altered after capture";
        for(const auto mode:{CatalogRendering::companionPcm,CatalogRendering::ownedLocal}){
            auto untouched=object({{"sentinel",true}});const auto sentinel=json(untouched);
            CHECK(!catalogInventoryFor(tampered,mode,untouched)&&json(untouched)==sentinel);}}
}
static void companion_native_authority_refusal()
{
    begin(__func__);auto r=std::make_unique<GenerationRequest>(request());r->source.sound=newInstanceSound();r->policy.scope=RequestScope::sound;
    RackDesign design;design.id=juce::Uuid();design.role=RackRole::fx;design.name="Native only full bank";design.panel.sections={"Drive"};design.units={{0,NativeModule::drive,{}}};
    const auto values=defaultParameterValues();const auto registry=parameterRegistry();for(std::size_t n=0;n<dynamicEqParameterStart;++n)if(registry[n].id.startsWith("effect.1."))design.units[0].defaults.push_back({0,registry[n].id.substring(9),values[n]});
    RackCatalogSnapshot selected;CHECK(captureRackCatalog(std::span(&design,1),{},{},selected));juce::var descriptor;CHECK(catalogInventoryFor(selected,CatalogRendering::companionPcm,descriptor));
    auto authority=std::make_shared<CatalogAuthority>();authority->root=r->tag;authority->snapshot=selected.id;authority->descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();
    CatalogDefinition definition{design.id,design.revision,selected.entries[0].contentSha256,{}};CHECK(encodeRackDesign(design,definition.canonical,RackValidationMode::instanceRecall));authority->definitions.push_back(definition);
    r->catalog.emplace();r->catalog->root=r->tag;r->catalog->selectionParent=r->tag;r->tag.requestId++;r->catalog->phase=CatalogPhase::materialize;r->catalog->authority=authority;r->catalog->inferenceCallsUsed=1;r->catalog->providerStatus=rack::oauth::Status::connected;
    r->catalog->baseIntent.catalogSelections={{7,design.id,design.revision,definition.contentSha256,{RackRole::fx,masterChain,0}}};
    unsigned calls=0;auto result=generateProposal(*r,[&](const auto&,auto,const auto&){++calls;return rack::oauth::StructuredResult{};});CHECK(result.status==GenerationStatus::invalidRequest&&!result.candidate&&calls==0);CHECK(result.errorCode=="generation_catalog_requires_local_native_authority");
    authority->descriptorJson.resize(maximumCatalogDescriptorBytes+1,' ');result=generateProposal(*r,[&](const auto&,auto,const auto&){++calls;return rack::oauth::StructuredResult{};});CHECK(result.errorCode=="generation_catalog_descriptor_limit"&&calls==0);
}
static void initial_companion_pcm_materialization()
{
    begin(__func__);unsigned cases=0;
    for(const auto& design:airack::test::frozenLibraryRackDesigns()){
        if(std::any_of(design.units.begin(),design.units.end(),[](const auto& u){return u.module==NativeModule::drum;}))continue;
        auto r=std::make_unique<GenerationRequest>(request());r->source.sound=newInstanceSound();r->policy.scope=RequestScope::sound;
        RackCatalogSnapshot selected;CHECK(captureRackCatalog(std::span(&design,1),{},{},selected));juce::var descriptor;CHECK(catalogInventoryFor(selected,CatalogRendering::companionPcm,descriptor));
        auto authority=std::make_shared<CatalogAuthority>();authority->root=r->tag;authority->snapshot=selected.id;authority->descriptorJson=juce::JSON::toString(descriptor,true,22).toStdString();
        CatalogDefinition definition{design.id,design.revision,selected.entries[0].contentSha256,{}};CHECK(encodeRackDesign(design,definition.canonical,RackValidationMode::instanceRecall));authority->definitions.push_back(definition);
        r->catalog.emplace();r->catalog->root=r->tag;r->catalog->selectionParent=r->tag;r->tag.requestId++;r->catalog->phase=CatalogPhase::materialize;r->catalog->authority=authority;
        r->catalog->inferenceCallsUsed=1;r->catalog->providerStatus=rack::oauth::Status::connected;r->catalog->baseIntent.catalogSelections={{7,design.id,design.revision,definition.contentSha256,{design.role,masterChain,0}}};
        CHECK(ai::detail::catalogAuthorityValid(*authority)&&ai::detail::catalogRequestValid(*r));
        unsigned calls=0;auto result=generateProposal(*r,[&](const auto&,auto,const auto&){++calls;return rack::oauth::StructuredResult{};});
        CHECK(ready(result,1)&&calls==0);if(result.candidate)CHECK(result.candidate->sound.personalities&&result.candidate->sound.personalities->instances.front().design==design);
        std::cout<<"INITIAL_PCM_MATERIALIZE "<<design.name<<" status="<<int(result.status)<<" calls="<<calls<<" error="<<result.errorCode<<'\n';++cases;
    }
    CHECK(cases==27);
}
void output_gain_policy()
{
    begin(__func__);
    for(const auto gain:{30.f,-24.f}) {
        auto r=request();physical(r.source,"output.gain_db",gain);
        auto result=generateProposal(r,[&](const juce::String&,std::string_view prompt,const juce::var&){
            const auto rules=parse(prompt)["output_rules"].toString();
            CHECK(rules.contains("Preserve captured output.gain_db"));
            CHECK(rules.contains("+30 dB")&&rules.contains("0 dB"));
            CHECK(rules.contains("user requests a level change")&&rules.contains("clipping repair"));
            return completed(json(wire()));
        });
        if(ready(result))CHECK(*result.candidate==r.source);
    }
    auto r=request();r.instruction="Make the output quieter";
    auto w=wire();w.getDynamicObject()->setProperty("parameter_edits",array({param("output.gain_db",.4)}));
    auto result=generateProposal(r,reply(w));
    if(ready(result))CHECK(result.candidate->sound.values[index("output.gain_db")]==.4f);
}
int main(int argc,char** argv)
{
    if(argc==2&&std::string_view(argv[1])=="--output-gain-policy"){output_gain_policy();phrase_context_and_mode();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--current-catalog-mode"){current_tile_catalog_modes();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&std::string_view(argv[1])=="--initial-materialization"){initial_companion_pcm_materialization();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    output_gain_policy();companion_native_authority_refusal();
    initial_companion_pcm_materialization();
    current_tile_catalog_modes(); catalog_selection_materialization_first(); catalog_inventory_helper_admission_and_small_bytes(); catalog_complete_61_inventory_envelope(65); catalog_complete_61_inventory_envelope(66); catalog_complete_61_inventory_envelope(67); catalog_complete_61_inventory_envelope(68); catalog_complete_61_inventory_envelope(66,true); catalog_complete_61_inventory_envelope(66,false,true); catalog_complete_61_inventory_envelope(68,false,true); catalog_complete_61_inventory_envelope(67,false,true,true); catalog_complete_61_inventory_envelope(71,false,true,false,true);
    full_captured_request(); model_visible_pattern_contract(); scope_lock_reuse_and_macros(); mixed_shared_budget(); phrase_context_and_mode(); cancellation_and_new_refinement(); raw_input_and_prompt_bounds();
    required_pattern_shared_repair(); required_pattern_withheld_results(); required_pattern_captured_policy(); reach_repair();
    bounded_group_prompt(); serialized_repair_boundaries();
    std::cout << "Generation request checks " << checks << " sections " << sections << " failures " << failures << '\n';
    return failures == 0 ? 0 : 1;
}

#include "FrozenRackExamples.h"
#include "OwnedFixture.h"
#include "InstrumentProcessor.h"
#include "DrumBundle.h"
#include "DrumImport.h"
#include "BlankInstance.h"
#include "OwnedFixture.h"
#include "RackExamples.h"
#include "RackPackageFixtures.h"
#include "IsolatedRackLibrary.h"
#include "CompanionWire.h"
#include "GenerationLog.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <typeinfo>
#include <windows.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace airack
{
// Test-only sealed historical library capture; original counter seams remain.
struct InstrumentAiTestAccess
{
    static auto placement(InstrumentProcessor& p){return p.libraryRuntime->client->placement(p.placementPreparation);}
    // Observation only; the public constructor and real C image own this request.
    static std::unique_ptr<ai::LocalGenerationRequest> currentLocal(InstrumentProcessor& p) {
        const std::lock_guard lock(p.restoreMutex);
        if(!p.aiLocal||!p.aiLocal->current||!p.aiPayloads->request||!p.aiLocalCurrentOwned())return {};
        auto result=std::make_unique<ai::LocalGenerationRequest>();result->request=*p.aiPayloads->request;result->local=*p.aiLocal->current;return result;
    }
    static void requestCounter(InstrumentProcessor& p, std::uint64_t v) { p.aiNextRequestId = v; }
    static void contextCounter(InstrumentProcessor& p, std::uint64_t v) { p.aiContextRevision = v; }
    static std::optional<ai::GenerationRequest> beginHistorical25(InstrumentProcessor& p, const AiRequestOptions& options,
        std::optional<double> bpm = {}, std::shared_ptr<const RackCatalogSnapshot>* captured = nullptr)
    try {
        if(options.policy.scope==ai::RequestScope::pattern)return airack::testing::beginAiRequest(p,options,bpm);
        p.refreshAiProposal();auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign().id;
        std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
        std::erase_if(factory,[&](const auto& design){return design.id==tile;});
        if(factory.size()!=25)throw std::runtime_error("historical25 library fixture");
        auto inventory=std::make_shared<RackCatalogSnapshot>();
        {
            const std::lock_guard lock(p.restoreMutex);auto source=std::make_unique<ProjectDocument>();std::uint64_t edit;
            if(!p.tryProjectSnapshotOwned(*source,edit))return {};
            std::vector<RackDesign> embedded;
            if(source->sound.personalities)for(const auto& instance:source->sound.personalities->instances)embedded.push_back(instance.design);
            if(!captureRackCatalog(factory,embedded,{},*inventory))return {};
        }
        if(captured)*captured=inventory;
        return p.beginAiRequestWithCatalog(options,bpm,std::move(inventory));
    }catch(const std::bad_alloc&){return {};}
};
// Read-only heap inspection; construction and every edit still go through the real public entry points.
struct InstrumentDrumTestAccess
{
    static bool manualPlaying(InstrumentProcessor& p) {return p.manualDrumPreviewPlaying();}
    static std::array<std::uint64_t,2> previewBytes(InstrumentProcessor& p) {
        const std::lock_guard lock(p.restoreMutex);return {p.manualPreviewBytes,p.renderedPreview.retainedPcmBytes()};
    }
#ifndef NDEBUG
    struct State {
        ProjectDocument project,group;
        drums::DrumSlots slots;
        InstrumentProcessor::DrumHistoryDebugView history;
        std::array<const drums::SampleAsset*,64> owners {};
        drums::TargetTokens targets {};
        std::array<std::uint64_t,3> tickets {};
        std::array<drums::AssetId,3> candidates {};
        std::uint64_t edit=0,generation=0,source=0,reset=0,context=0,groupEdit=0;
        bool groupDirty=false,valid=false;
        std::string lastActionType;
    };
    static std::unique_ptr<State> snapshot(InstrumentProcessor& p) {
        auto out=std::make_unique<State>(); const std::lock_guard lock(p.restoreMutex);
        out->valid=p.tryProjectSnapshotOwned(out->project,out->edit);
        const auto& state=*p.drumOwnership; out->slots=state.current_->document->slots();
        out->generation=state.current_->borrowed.generation;
        for(std::size_t n=0;n<64;++n) { out->owners[n]=state.current_->owners[n].get(); out->targets[n]=p.drumTargetTokens[n].load(); }
        for(std::size_t n=0;n<3;++n) { out->tickets[n]=state.preparations_[n].ticket;
            if(state.preparations_[n].asset) out->candidates[n]=state.preparations_[n].asset->id(); }
        p.drumHistoryDebugSnapshot(out->history);
        if(out->history.cursor) out->lastActionType=typeid(*p.editHistory->actions[out->history.cursor-1]).name();
        out->source=p.structuralGeneration.load(); out->reset=p.resetGeneration.load(); out->context=p.aiContextRevision;
        out->group=p.groupControl->committed; out->groupEdit=p.groupControl->committedEdit; out->groupDirty=p.groupControl->dirty;
        return out;
    }
#endif
};
}
namespace
{
using namespace airack;
using namespace airack::ai;
int checks = 0, failures = 0, groups = 0;
const char* group = "";
void check(bool pass, const char* expression, int line)
{
    ++checks;
    if (!pass) { ++failures; std::cout << "FAIL " << group << ':' << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void begin(const char* name) { group = name; ++groups; std::cout << "GROUP " << name << '\n'; }
std::size_t index(const char* id)
{
    const auto registry = parameterRegistry();
    const auto found = std::find_if(registry.begin(), registry.end(), [&](const auto& p) { return p.id == id; });
    CHECK(found != registry.end());
    return found == registry.end() ? 0 : std::size_t(found - registry.begin());
}
ProjectDocument project(InstrumentProcessor& p)
{
    ProjectDocument d; std::uint64_t edit = 0, reset = 0;
    CHECK(p.tryProjectSnapshot(d, edit, reset)); return d;
}
std::uint64_t revision(InstrumentProcessor& p)
{
    ProjectDocument d; std::uint64_t edit = 0, reset = 0;
    CHECK(p.tryProjectSnapshot(d, edit, reset)); return edit;
}
bool recall(InstrumentProcessor& p, const ProjectDocument& d)
{
    juce::MemoryBlock b; CHECK(encodeProjectState(d, b));
    return p.restoreProjectState(b.getData(), int(b.getSize()));
}
PatternDocument phrase(std::uint8_t pitch = 60)
{
    PatternDocument p; p.meter = {1, 4}; p.noteCount = 1;
    p.notes[0] = {0, .05, pitch, 70, 1}; return p;
}
ProjectDocument setup(InstrumentProcessor& p, PlaybackMode mode = PlaybackMode::externalMidi)
{
    ProjectDocument d; d.pattern = phrase(); d.mode = mode;
    d.sound.values[index("output.gain_db")] = .4f;
    CHECK(recall(p, d)); return d;
}
AiRequestOptions options(RequestScope scope = RequestScope::both)
{
    return {"A warm editable proposal", "synthetic-selected_model:1.0", {scope, true, true}};
}
juce::var object(std::initializer_list<std::pair<const char*, juce::var>> fields)
{
    auto* o = new juce::DynamicObject;
    for (const auto& f : fields) o->setProperty(f.first, f.second);
    return o;
}
juce::var patternJson(const PatternDocument& p)
{
    juce::Array<juce::var> notes;
    for (std::size_t n = 0; n < p.noteCount; ++n)
    {
        const auto& v = p.notes[n];
        notes.add(object({{"start_beats", v.startBeats}, {"duration_beats", v.durationBeats},
            {"pitch", int(v.pitch)}, {"velocity", int(v.velocity)}, {"channel", int(v.channel)}}));
    }
    constexpr const char* grids[] {"quarter", "eighth", "sixteenth", "thirty_second", "sixty_fourth"};
    return object({{"bars", int(p.bars)}, {"meter_numerator", int(p.meter.numerator)},
        {"meter_denominator", int(p.meter.denominator)}, {"grid", grids[std::size_t(p.grid)]},
        {"swing", p.swing}, {"notes", notes}});
}
std::string proposalJson(const ProposalEdits& edits, bool catalogAware = false)
{
    juce::Array<juce::var> params, racks, macros;
    for (const auto& e : edits.parameters) params.add(object({{"id", e.id}, {"normalized", double(e.normalized)}}));
    constexpr const char* kinds[] {"add_synth", "remove_synth", "move_synth", "add_effect", "remove_effect", "move_effect", "replace_effect"};
    constexpr const char* effects[] {"drive", "delay", "reverb", "compressor"};
    for (const auto& e : edits.racks) racks.add(object({{"kind", kinds[std::size_t(e.kind)]}, {"slot", int(e.slot)},
        {"chain", int(e.destination)}, {"position", int(e.position)}, {"effect", effects[std::size_t(e.effect)]}}));
    for (const auto& e : edits.macros) macros.add(object({{"index", int(e.index)},
        {"target_id", e.targetId.isEmpty() ? juce::var() : juce::var(e.targetId)}, {"amount", double(e.amount)}}));
    const bool named = catalogAware || edits.name.isNotEmpty();
    auto value = object({{"version", named ? 2 : 1}, {"parameter_edits", params}, {"rack_edits", racks},
        {"macro_edits", macros}, {"pattern", edits.pattern ? patternJson(*edits.pattern) : juce::var()},
        {"characteristics", "warm"}, {"explanation", "native fixture with explicit edits"}});
    if (named) {
        value.getDynamicObject()->setProperty("name", edits.name.isNotEmpty() ? edits.name : juce::String("Warm Test Pad"));
        value.getDynamicObject()->setProperty("reconciliation", object({{"detach", juce::Array<juce::var>()}, {"remove", juce::Array<juce::var>()}, {"replace", juce::Array<juce::var>()}, {"controls", juce::Array<juce::var>()}}));
    }
    if (catalogAware) {
        value.getDynamicObject()->setProperty("catalog_selections", juce::Array<juce::var>());
        value.getDynamicObject()->setProperty("catalog_parameter_edits", juce::Array<juce::var>());
    }
    return juce::JSON::toString(value, true).toStdString();
}
GenerationResult validResult(const GenerationRequest& request, const ProposalEdits& edits = {})
{
    const auto text = proposalJson(edits, request.catalog.has_value());
    auto result = generateProposal(request, [&](const juce::String& model, std::string_view, const juce::var&)
    {
        CHECK(model == request.model);
        rack::oauth::StructuredResult reply;
        reply.result.status = rack::oauth::Status::connected; reply.result.httpStatus = 200;
        reply.text = juce::String::fromUTF8(text.data(), int(text.size())); return reply;
    });
    CHECK(result.status == GenerationStatus::ready && result.inferenceCalls == 1 && result.candidate);
    juce::MemoryBlock wire; CHECK(companion::encodeResult(request, result, wire));
    return result;
}
bool propose(InstrumentProcessor& p, const ProposalEdits& edits = {}, AiRequestOptions o = options())
{
    auto request = InstrumentAiTestAccess::beginHistorical25(p,o, 400); CHECK(request.has_value());
    if (!request) return false; // Missing seed behavior fails assertions, never dereferences.
    CHECK(p.aiProposalPhase() == AiProposalPhase::pending);
    const auto before = project(p); auto result = validResult(*request, edits);
    CHECK(p.receiveAiResult(std::move(result))); CHECK(project(p) == before);
    CHECK(p.aiProposalPhase() == AiProposalPhase::proposed && p.aiProposal());
    return p.aiProposalPhase() == AiProposalPhase::proposed && p.aiProposal();
}
struct Head final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p; p.setIsPlaying(true); p.setBpm(133); p.setPpqPosition(.1); p.setTimeInSamples(2400); return p;
    }
};
auto captureRequest(InstrumentProcessor& processor, const AiRequestOptions& settings,
                    std::optional<double> bpm = {})
{
    // Keep request return buffers off the authority fixture's already large stack.
    return airack::testing::fixtureValue([&] { return InstrumentAiTestAccess::beginHistorical25(processor,settings, bpm); });
}
void capture_context_authority()
{
    begin(__func__);
    auto pStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()), otherStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());
    auto& p = *pStorage; auto& other = *otherStorage;
    auto beforeFixture = airack::testing::fixtureValue([&]{return project(p);}); const auto& before = *beforeFixture; const auto initialRevision = revision(p);
    CHECK(!p.aiRuntime().isNull() && p.aiRuntime() != other.aiRuntime());
    CHECK(p.aiMusicalContext() == AiMusicalContext{});
    CHECK(p.aiProposalPhase() == AiProposalPhase::idle && !p.aiProposal());
    auto o = options();
    for (const auto& bad : {std::string(), std::string("bad\0text", 8), std::string("\xc0\xaf", 2), std::string(8193, 'a')})
    {
        auto b = o; b.instruction = bad; CHECK(!*captureRequest(p, b)); CHECK(project(p) == before);
    }
    for (const auto& bad : {juce::String(), juce::String("model bad"), juce::String::repeatedString("a", 257)})
    { auto b = o; b.model = bad; CHECK(!*captureRequest(p, b)); CHECK(project(p) == before); }
    auto badPolicy = o; badPolicy.policy.scope = static_cast<RequestScope>(255); CHECK(!*captureRequest(p, badPolicy));
    auto requestOwner = captureRequest(p, o); auto& request = *requestOwner; CHECK(request.has_value());
    if (request)
    {
        CHECK(request->tag.runtime == p.aiRuntime() && request->tag.requestId == 1);
        CHECK(request->source == before && request->tag.editRevision == initialRevision && request->tag.contextRevision == 0);
        CHECK(request->instruction == o.instruction && request->model == o.model);
        CHECK(request->policy.scope == RequestScope::both && request->policy.allowMacroChanges && request->policy.allowSlotReuse);
        CHECK(request->role.empty() && request->key.empty() && request->scale.empty());
        CHECK(!request->capturedPhrase && !request->capturedBpm);
        CHECK(p.setAiMusicalContext({})); p.refreshAiProposal(); CHECK(p.aiProposalPhase() == AiProposalPhase::pending);
        auto invalid = AiMusicalContext{"bad", "C", "minor", 401};
        CHECK(!p.setAiMusicalContext(invalid)); CHECK(p.aiMusicalContext() == AiMusicalContext{});
        auto bad = o; bad.instruction.clear(); CHECK(!*captureRequest(p, bad)); CHECK(p.aiProposalPhase() == AiProposalPhase::pending);
    }
    AiMusicalContext context{"lead \xf0\x9f\x8e\xb9", "D", "minor", 143};
    CHECK(p.setAiMusicalContext(context)); CHECK(p.aiMusicalContext() == context);
    p.refreshAiProposal(); if (request) CHECK(p.aiProposalPhase() == AiProposalPhase::stale);
    auto capturedOwner = captureRequest(p, o, 97); auto& captured = *capturedOwner; CHECK(captured.has_value());
    if (captured)
    {
        CHECK(captured->tag.requestId == 2 && captured->tag.contextRevision == 1);
        CHECK(captured->role == context.role && captured->key == "D" && captured->scale == "minor" && captured->capturedBpm == 143);
    }
    for (const auto& bad : {std::string(129, 'x'), std::string("bad\0key", 7), std::string("\xed\xa0\x80", 3)})
    {
        auto c = context; c.role = bad; CHECK(!p.setAiMusicalContext(c)); CHECK(p.aiMusicalContext() == context);
    }
    auto badTempo = context; badTempo.auditionBpm = std::numeric_limits<double>::quiet_NaN(); CHECK(!p.setAiMusicalContext(badTempo));
    badTempo.auditionBpm = 19; CHECK(!p.setAiMusicalContext(badTempo));
    auto boundary = context; boundary.role = std::string(126, 'a') + "\xc3\xa9"; boundary.auditionBpm = 20;
    CHECK(p.setAiMusicalContext(boundary)); boundary.auditionBpm = 400; CHECK(p.setAiMusicalContext(boundary));
    boundary.auditionBpm.reset(); CHECK(p.setAiMusicalContext(boundary));
    auto maximum = o; maximum.instruction = std::string(8190, 'a') + "\xc3\xa9"; CHECK(captureRequest(p, maximum, 20)->has_value());
    auto ignoredTempoOwner = captureRequest(p, o, std::numeric_limits<double>::infinity()); auto& ignoredTempo = *ignoredTempoOwner; CHECK(ignoredTempo.has_value());
    if (ignoredTempo) CHECK(!ignoredTempo->capturedBpm);
    auto shortProject = setup(p); auto withPhraseOwner = captureRequest(p, o, 400); auto& withPhrase = *withPhraseOwner; CHECK(withPhrase.has_value());
    if (withPhrase) CHECK(withPhrase->capturedPhrase == shortProject.pattern && withPhrase->capturedBpm == 400);
    const auto unchanged = project(p);
    CHECK(p.beginParameterGesture(index("global.master_level"))); CHECK(!*captureRequest(p, o)); CHECK(project(p) == unchanged);
    CHECK(p.endParameterGesture(index("global.master_level")));
    p.control(index("synth.1.modulation.1.target")).setValue(1);
    CHECK(!validateProjectDocument(project(p))); CHECK(!*captureRequest(p, o));
    p.control(index("synth.1.modulation.1.target")).setValue(0);
    InstrumentAiTestAccess::requestCounter(p, std::numeric_limits<std::uint64_t>::max());
    CHECK(!*captureRequest(p, o)); CHECK(project(p) == unchanged);
    InstrumentAiTestAccess::contextCounter(p, std::numeric_limits<std::uint64_t>::max());
    const auto priorContext = p.aiMusicalContext(); CHECK(p.setAiMusicalContext(priorContext));
    auto overflow = priorContext; overflow.key = "E"; CHECK(!p.setAiMusicalContext(overflow)); CHECK(p.aiMusicalContext() == priorContext);
    auto preservedStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& preserved = *preservedStorage;
    setup(preserved); auto liveRequestOwner = captureRequest(preserved, o, 400); auto& liveRequest = *liveRequestOwner; CHECK(liveRequest.has_value());
    if (liveRequest)
    {
        const auto edit = revision(preserved); preserved.prepareToPlay(48000, 64); Head head; preserved.setPlayHead(&head);
        CHECK(preserved.enqueueKeyboardNote(true, 64, 90, 1));
        juce::AudioBuffer<float> audio(2, 64); juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 67, juce::uint8(70)), 0); preserved.processBlock(audio, midi);
        preserved.control(index("output.gain_db")).setValue(.61f); preserved.refreshAiProposal();
        CHECK(revision(preserved) == edit && preserved.aiProposalPhase() == AiProposalPhase::pending);
        CHECK(preserved.receiveAiResult(validResult(*liveRequest))); CHECK(preserved.aiProposal());
        preserved.setPlayHead(nullptr); preserved.releaseResources();
    }
}
void result_failure_outcomes();
void result_invalidation_by_edits();
void result_admission_restrictions();
void result_failure_and_invalidation()
{
    begin(__func__);
    auto pStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()), foreignStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());
    auto& p = *pStorage; auto& foreign = *foreignStorage; std::cout<<"TASK13_BOUNDARY 31 setup.before"<<std::endl;auto beforeFixture = airack::testing::fixtureValue([&]{return setup(p);});const auto& before = *beforeFixture; setup(foreign);std::cout<<"TASK13_BOUNDARY 31 setup.after"<<std::endl;
    std::cout<<"TASK13_BOUNDARY 31 capture_request.before"<<std::endl;auto requestFixture = airack::testing::fixtureValue([&]{return InstrumentAiTestAccess::beginHistorical25(p,options(),400);});auto& request = *requestFixture;std::cout<<"TASK13_BOUNDARY 31 capture_request.after"<<std::endl; CHECK(request.has_value());
    if (request)
    {
        std::cout<<"TASK13_BOUNDARY 31 valid_result.before"<<std::endl;auto task13Next1136_goodStorage = std::unique_ptr<GenerationResult>(new GenerationResult(validResult(*request))); auto& good = *task13Next1136_goodStorage;std::cout<<"TASK13_BOUNDARY 31 valid_result.after"<<std::endl;
        std::cout<<"TASK13_BOUNDARY 31 foreign_receive.before"<<std::endl;CHECK(!foreign.receiveAiResult(GenerationResult(good)));std::cout<<"TASK13_BOUNDARY 31 foreign_receive.after"<<std::endl; CHECK(foreign.aiProposalPhase() == AiProposalPhase::idle);
        for (int n = 0; n < 4; ++n)
        {
            auto task13Next1136_wrongStorage = std::make_unique<GenerationResult>(good); auto& wrong = *task13Next1136_wrongStorage;
            if (n == 0) wrong.tag.runtime = foreign.aiRuntime();
            if (n == 1) ++wrong.tag.requestId;
            if (n == 2) ++wrong.tag.editRevision;
            if (n == 3) ++wrong.tag.contextRevision;
            CHECK(!p.receiveAiResult(std::move(wrong))); CHECK(p.aiProposalPhase() == AiProposalPhase::pending);
        }
        auto newerFixture = airack::testing::fixtureValue([&]{return InstrumentAiTestAccess::beginHistorical25(p,options(),400);});auto& newer = *newerFixture; CHECK(newer.has_value());
        CHECK(!p.receiveAiResult(GenerationResult(good))); CHECK(p.aiProposalPhase() == AiProposalPhase::pending);
        if (newer)
        {
            auto task13Next1136_currentStorage = std::unique_ptr<GenerationResult>(new GenerationResult(validResult(*newer))); auto& current = *task13Next1136_currentStorage; CHECK(p.receiveAiResult(GenerationResult(current)));
            CHECK(!p.receiveAiResult(std::move(current))); CHECK(p.aiProposalPhase() == AiProposalPhase::proposed);
            auto bad = options(); bad.model = "bad model"; CHECK(!InstrumentAiTestAccess::beginHistorical25(p,bad)); CHECK(p.aiProposal());
            juce::MemoryBlock state; p.getStateInformation(state); auto storedFixture=std::make_unique<ProjectDocument>();auto& stored=*storedFixture;
            CHECK(decodeProjectState(state.getData(), state.getSize(), stored) && stored == before);
            p.discardAiProposal(); CHECK(p.aiProposalPhase() == AiProposalPhase::idle && !p.aiProposal());
            CHECK(!p.applyAiProposal()); CHECK(project(p) == before);
        }
    }
    // Each independent case below has its own frame: a Debug frame holds every scope's locals at once, and with
    // them the valid receive above overflows the default Windows stack.
    result_failure_outcomes(); result_invalidation_by_edits(); result_admission_restrictions();
}
void result_failure_outcomes()
{
    for (auto outcome : {GenerationStatus::providerFailure, GenerationStatus::cancelled})
    {
        auto qStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& q = *qStorage;
        setup(q); auto pending = InstrumentAiTestAccess::beginHistorical25(q,options(), 400); CHECK(pending.has_value());
        if (!pending) continue;
        auto task13Next1136_failedStorage = std::make_unique<GenerationResult>(); auto& failed = *task13Next1136_failedStorage; failed.tag = pending->tag; failed.status = outcome;
        failed.providerStatus = rack::oauth::Status::networkFailure;
        juce::MemoryBlock wire; CHECK(companion::encodeResult(*pending, failed, wire));
        CHECK(!q.receiveAiResult(std::move(failed))); CHECK(q.aiProposalPhase() == AiProposalPhase::failed && !q.aiProposal());
        CHECK(!q.applyAiProposal());
    }
    for (int bad = 0; bad < 5; ++bad)
    {
        auto qStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& q = *qStorage;
        setup(q); auto pending = InstrumentAiTestAccess::beginHistorical25(q,options(), 400); CHECK(pending.has_value());
        if (!pending) continue;
        auto task13Next1136_invalidStorage = std::unique_ptr<GenerationResult>(new GenerationResult(validResult(*pending))); auto& invalid = *task13Next1136_invalidStorage;
        if (bad == 0) invalid.candidate->sound.values[index("global.master_level")] = .13f;
        if (bad == 1) invalid.audio.left[0] = std::numeric_limits<float>::quiet_NaN();
        if (bad == 2) invalid.edits.parameters.push_back({"unsupported.control", .5f});
        if (bad == 3) invalid.inferenceCalls = 4;
        if (bad == 4) invalid.measurements.rms = std::numeric_limits<double>::infinity();
        CHECK(!q.receiveAiResult(std::move(invalid))); CHECK(q.aiProposalPhase() == AiProposalPhase::failed && !q.aiProposal());
    }
}
void result_invalidation_by_edits()
{
    // Pending and proposed authority both expire for real manual/context changes.
    for (int editKind = 0; editKind < 8; ++editKind)
    {
        auto qStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& q = *qStorage; const auto initial = setup(q);
        if (editKind == 5) { auto sound = initial.sound; sound.values[0] = .37f; CHECK(q.applySoundEdit(sound, SoundEditOrigin::manual, revision(q))); }
        auto pending = InstrumentAiTestAccess::beginHistorical25(q,options(), 400); CHECK(pending.has_value()); if (!pending) continue;
        auto result = validResult(*pending);
        if (editKind % 2 == 0) CHECK(q.receiveAiResult(GenerationResult(result)));
        if (editKind == 0) { auto sound = project(q).sound; sound.values[0] = .29f; CHECK(q.applySoundEdit(sound, SoundEditOrigin::manual, revision(q))); }
        if (editKind == 1) { auto sound = project(q).sound; sound.controlLocks[0] = true; CHECK(q.applySoundEdit(sound, SoundEditOrigin::manual, revision(q))); }
        if (editKind == 2) CHECK(q.applyPatternEdit(phrase(72), revision(q)));
        if (editKind == 3)
        {
            const auto directory = juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/host-fixtures")
                .getChildFile("airack-ai-preset-" + juce::Uuid().toString());
            if (directory.createDirectory().failed()) throw std::runtime_error("preset_fixture_setup");
            const auto file = directory.getChildFile("identical-preset.airack");
            CHECK(q.saveSoundPreset(file)); CHECK(q.loadSoundPreset(file));
            if (!directory.deleteRecursively()) throw std::runtime_error("preset_fixture_cleanup");
        }
        if (editKind == 4) CHECK(recall(q, project(q))); // Identical project recall still invalidates.
        if (editKind == 5) CHECK(q.undoSoundEdit());
        if (editKind == 6) CHECK(q.setAiMusicalContext({"bass", "C", "minor", {}}));
        if (editKind == 7) CHECK(q.applyRackEdit({RackEditKind::addEffect, noSlot, masterChain, 0, EffectKind::drive, false}, revision(q)) == RackEditResult::applied);
        const auto current = project(q); q.refreshAiProposal();
        CHECK(q.aiProposalPhase() == AiProposalPhase::stale && !q.aiProposal());
        CHECK(!q.receiveAiResult(std::move(result))); CHECK(!q.applyAiProposal()); CHECK(project(q) == current);
    }
}
void result_admission_restrictions()
{
    // Original-source admission rejects policy/lock/reuse violations in a purported ready reply.
    for (int restriction = 0; restriction < 4; ++restriction)
    {
        auto qStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& q = *qStorage; auto d = setup(q); auto o = options();
        if (restriction == 0) { d.sound.controlLocks[0] = true; CHECK(recall(q, d)); }
        if (restriction == 1) { d.sound.layout.effects[0].use = SlotUse::retired; CHECK(recall(q, d)); o.policy.allowSlotReuse = false; }
        if (restriction == 2) o.policy.scope = RequestScope::pattern;
        if (restriction == 3) o.policy.allowMacroChanges = false;
        auto pending = InstrumentAiTestAccess::beginHistorical25(q,o, 400); CHECK(pending.has_value()); if (!pending) continue;
        auto task13Next1136_invalidStorage = std::unique_ptr<GenerationResult>(new GenerationResult(validResult(*pending))); auto& invalid = *task13Next1136_invalidStorage;
        if (restriction == 0 || restriction == 2) { invalid.edits.parameters = {{"global.master_level", .2f}}; invalid.candidate->sound.values[0] = .2f; }
        if (restriction == 1) { invalid.edits.racks = {{RackEditKind::addEffect, 0, masterChain, 0, EffectKind::drive, true}}; }
        if (restriction == 3) invalid.edits.macros = {{1, "synth.1.pan", .25f}};
        CHECK(!q.receiveAiResult(std::move(invalid))); CHECK(q.aiProposalPhase() == AiProposalPhase::failed && !q.aiProposal());
        CHECK(project(q) == d);
    }
}
void sparse_apply_undo_automation()
{
    begin(__func__);
    auto pStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pStorage; const auto before = setup(p, PlaybackMode::internalPattern);
    // A bare bank is outside the AI's reach; catalog_refined_aggregate_transaction applies a placement as one action.
    ProposalEdits edits; edits.parameters = {{"global.master_level", .23f}};
    edits.macros = {{1, "synth.1.pan", .25f}}; edits.pattern = phrase(67);
    if (propose(p, edits))
    {
        const auto unrelated = index("output.gain_db"); p.control(unrelated).setValue(.61f);
        CHECK(p.applyAiProposal()); const auto applied = project(p);
        CHECK(applied.sound.values[unrelated] == .61f && applied.pattern == *edits.pattern);
        CHECK(applied.sound.values[0] == .23f);
        CHECK(applied.sound.macros[0] == (MacroBinding{parameterRegistry()[index("synth.1.pan")].stateKey, .25f}));
        CHECK(applied.mode == before.mode && applied.sound.controlLocks == before.sound.controlLocks);
        CHECK(!p.aiProposal() && p.aiProposalPhase() == AiProposalPhase::idle);
        p.control(unrelated).setValue(.72f); CHECK(p.undoSoundEdit()); auto undone = project(p);
        CHECK(undone.sound.values[unrelated] == .72f && undone.pattern == before.pattern);
        undone.sound.values[unrelated] = before.sound.values[unrelated]; CHECK(undone == before);
        CHECK(p.redoSoundEdit()); auto redone = project(p);
        CHECK(redone.sound.values[unrelated] == .72f); redone.sound.values[unrelated] = .61f; CHECK(redone == applied);
        CHECK(p.undoSoundEdit()); CHECK(!p.undoSoundEdit()); // Whole combined edit was one action.
    }
    auto notesStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& notes = *notesStorage; const auto notesBefore = setup(notes, PlaybackMode::internalPattern);
    ProposalEdits pattern; pattern.pattern = phrase(72);
    if (propose(notes, pattern, options(RequestScope::pattern)))
    {
        notes.control(index("output.gain_db")).setValue(.61f); CHECK(notes.applyAiProposal());
        auto after = project(notes); CHECK(after.mode == notesBefore.mode && after.pattern == *pattern.pattern);
        after.pattern = notesBefore.pattern; after.sound.values[index("output.gain_db")] = notesBefore.sound.values[index("output.gain_db")]; CHECK(after == notesBefore);
        CHECK(notes.undoSoundEdit()); CHECK(project(notes).mode == notesBefore.mode);
    }
    auto invalidStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& invalid = *invalidStorage; setup(invalid);
    ProposalEdits sound; sound.parameters = {{"global.master_level", .23f}};
    if (propose(invalid, sound))
    {
        CHECK(invalid.applyAiProposal()); invalid.control(index("synth.1.modulation.1.target")).setValue(1);
        const auto invalidLive = project(invalid); CHECK(!validateProjectDocument(invalidLive));
        CHECK(!invalid.undoSoundEdit()); CHECK(project(invalid) == invalidLive);
    }
    auto activeStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& active = *activeStorage; const auto activeBefore = setup(active);
    if (propose(active, sound))
    {
        CHECK(active.beginParameterGesture(index("output.gain_db"))); CHECK(!active.applyAiProposal());
        CHECK(project(active) == activeBefore); CHECK(active.endParameterGesture(index("output.gain_db")));
        CHECK(active.applyAiProposal());
    }
}
struct Events
{
    struct Listener final : juce::AudioProcessorParameter::Listener
    {
        Events* owner = nullptr; std::size_t index = 0;
        void parameterValueChanged(int, float v) override {
            ++owner->values[index]; owner->last[index] = v;
            if (!owner->valueFired && owner->onValue) { owner->valueFired=true; owner->onValue(); }
        }
        void parameterGestureChanged(int, bool started) override
        {
            if (started)
            {
                ++owner->starts[index];
                if (!owner->fired && owner->onBegin) { owner->fired = true; owner->onBegin(); }
            }
            else {
                ++owner->ends[index];
                if (!owner->endFired && owner->onEnd) { owner->endFired=true; owner->onEnd(); }
            }
        }
    };
    InstrumentProcessor& processor;
    std::array<Listener, parameterCount> listeners;
    std::array<int, parameterCount> starts {}, ends {}, values {};
    std::array<float, parameterCount> last {};
    std::function<void()> onBegin, onValue, onEnd;
    bool fired = false, valueFired = false, endFired = false;
    explicit Events(InstrumentProcessor& p) : processor(p)
    {
        for (std::size_t i = 0; i < parameterCount; ++i) { listeners[i].owner = this; listeners[i].index = i; p.control(i).addListener(&listeners[i]); }
    }
    ~Events() { for (std::size_t i = 0; i < parameterCount; ++i) processor.control(i).removeListener(&listeners[i]); }
    void changedOnly(std::size_t changed, bool published)
    {
        for (std::size_t i = 0; i < parameterCount; ++i)
        {
            CHECK(starts[i] == ends[i]); CHECK(starts[i] == (i == changed ? 1 : 0));
            CHECK(values[i] == (published && i == changed ? 1 : 0));
        }
        if (published) CHECK(last[changed] == .23f);
    }
};
void reentrant_notifications_and_noop()
{
    begin(__func__);
    const auto changed = index("global.master_level"), unrelated = index("output.gain_db");
    for (int callback = 0; callback < 7; ++callback)
    {
        auto pStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pStorage; auto beforeFixture = airack::testing::fixtureValue([&]{return setup(p);}); const auto& before = *beforeFixture;
        ProposalEdits edits; edits.parameters = {{"global.master_level", .23f}, {"synth.1.pan", .5f}};
        if (!propose(p, edits)) continue;
        std::unique_ptr<ProjectDocument> external;
        auto eventsFixture = std::make_unique<Events>(p); auto& events = *eventsFixture;
        events.onBegin = [&]
        {
            if (callback == 0) CHECK(recall(p, before));
            if (callback == 1) { auto sFixture = airack::testing::fixtureValue([&]{return project(p).sound;}); auto& s = *sFixture; s.values[unrelated] = .47f; CHECK(p.applySoundEdit(s, SoundEditOrigin::manual, revision(p))); }
            if (callback == 2) CHECK(p.setAiMusicalContext({"bass", "E", "minor", {}}));
            if (callback == 3) p.control(unrelated).setValue(.61f);
            if (callback == 4) CHECK(p.beginParameterGesture(unrelated));
            if (callback == 5) {
                auto live=std::make_unique<SoundDocument>();std::uint64_t revision;CHECK(p.trySoundSnapshot(*live,revision));
                live->controlLocks[changed]=true;CHECK(p.applySoundEdit(*live,SoundEditOrigin::manual,revision));
                external=airack::testing::fixtureValue([&]{return project(p);});
            }
            if (callback == 6) {
                GroupIntent intent;intent.group=juce::Uuid();intent.member=juce::Uuid();
                CHECK(setGroupText("Reentrant group",intent.name));CHECK(setGroupText("bass",intent.role));CHECK(setGroupText("C",intent.key));CHECK(setGroupText("minor",intent.scale));intent.bpm=120;
                CHECK(p.setGroupIntent(intent));external=airack::testing::fixtureValue([&]{return project(p);});
            }
        };
        const bool applied = p.applyAiProposal();
        CHECK(events.fired); CHECK(applied == (callback == 3));
        if(callback>=5){CHECK(external&&project(p)==*external);events.changedOnly(changed,false);}
        if (callback < 3)
        {
            auto expectedFixture = airack::testing::fixtureValue([&]{return before;}); auto& expected = *expectedFixture; if (callback == 1) expected.sound.values[unrelated] = .47f;
            CHECK(project(p) == expected); events.changedOnly(changed, false);
        }
        if (callback == 3)
        {
            auto expectedFixture = airack::testing::fixtureValue([&]{return before;}); auto& expected = *expectedFixture; expected.sound.values[changed] = .23f; expected.sound.values[unrelated] = .61f; expected.sound.name = "Warm Test Pad";
            CHECK(project(p) == expected); events.changedOnly(changed, true);
            p.control(unrelated).setValue(.72f); CHECK(p.undoSoundEdit()); CHECK(project(p).sound.values[unrelated] == .72f);
            CHECK(p.redoSoundEdit()); CHECK(project(p).sound.values[unrelated] == .72f);
        }
        if (callback == 4)
        {
            CHECK(project(p) == before); CHECK(p.endParameterGesture(unrelated));
            CHECK(events.starts[changed] == 1 && events.ends[changed] == 1 && events.values[changed] == 0);
            CHECK(events.starts[unrelated] == 1 && events.ends[unrelated] == 1);
        }
    }
    auto noopStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& noop = *noopStorage; auto beforeFixture = airack::testing::fixtureValue([&]{return setup(noop);}); const auto& before = *beforeFixture;
    auto manualFixture = airack::testing::fixtureValue([&]{return before.sound;}); auto& manual = *manualFixture; manual.values[changed] = .37f; manual.name = "Equal Pad";
    CHECK(noop.applySoundEdit(manual, SoundEditOrigin::manual, revision(noop)));
    ProposalEdits equal; equal.parameters = {{"global.master_level", .37f}}; equal.name = "Equal Pad";
    if (propose(noop, equal))
    {
        auto eventsFixture = std::make_unique<Events>(noop); auto& events = *eventsFixture; const auto edit = revision(noop);
        CHECK(noop.applyAiProposal()); CHECK(!noop.aiProposal() && noop.aiProposalPhase() == AiProposalPhase::idle);
        CHECK(revision(noop) == edit); CHECK(project(noop).sound == manual);
        for (std::size_t i = 0; i < parameterCount; ++i) CHECK(events.starts[i] == 0 && events.ends[i] == 0 && events.values[i] == 0);
        CHECK(noop.undoSoundEdit()); CHECK(project(noop) == before); CHECK(!noop.undoSoundEdit());
        CHECK(noop.redoSoundEdit()); CHECK(project(noop).sound == manual);
    }
}
void processIdentity(int argc, char** argv)
{
    FILETIME created {}, exited {}, kernel {}, user {}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULARGE_INTEGER time {}; time.LowPart = created.dwLowDateTime; time.HighPart = created.dwHighDateTime;
    wchar_t module[32768] {}; GetModuleFileNameW(nullptr, module, 32768);
    std::cout << "NATIVE PID=" << GetCurrentProcessId() << " FILETIME=" << time.QuadPart
        << " IMAGE=" << juce::String(module) << " CWD=" << juce::File::getCurrentWorkingDirectory().getFullPathName() << '\n';
    for (int n = 0; n < argc; ++n) std::cout << "ARGV[" << n << "]=" << argv[n] << '\n';
}
void required_pattern_result_injection()
{
    begin(__func__);
    for(bool empty : {false, true}) {
        auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;const auto before=setup(p);const auto edit=revision(p);
        auto o=options();o.policy.requirePattern=true;const auto pending=InstrumentAiTestAccess::beginHistorical25(p,o,400);
        CHECK(pending.has_value());if(!pending)continue;CHECK(pending->policy.requirePattern);
        auto optional=*pending;optional.policy.requirePattern=false;ProposalEdits edits;edits.parameters={{"output.gain_db",.6f}};
        if(empty)edits.pattern=PatternDocument{};auto injected=validResult(optional,edits);
        CHECK(injected.status==GenerationStatus::ready&&injected.candidate);
        CHECK(!p.receiveAiResult(std::move(injected)));CHECK(!p.aiProposal()&&!p.applyAiProposal());
        CHECK(project(p)==before&&revision(p)==edit);CHECK(!p.undoSoundEdit());
    }
}
void required_parent_optional_refinement()
{
    begin(__func__);
    for(bool clear : {false, true}) {
        auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;const auto before=setup(p);
        auto o=options();o.policy.requirePattern=true;ProposalEdits edits;edits.pattern=phrase(45);edits.parameters={{"output.gain_db",.31f}};
        CHECK(propose(p,edits,o));if(!p.aiProposal())continue;const auto parent=*p.aiCurrentRequest();const auto candidate=*p.aiProposal()->candidate;
        CHECK(parent.policy.requirePattern&&p.aiIntendedEdits()->pattern==candidate.pattern);
        const auto child=p.beginAiRefinement("Refine sound or explicitly clear","synthetic-selected_model:1.0");
        CHECK(child.has_value());if(!child)continue;CHECK(!child->policy.requirePattern);
        CHECK(child->policy.scope==parent.policy.scope&&child->policy.allowMacroChanges==parent.policy.allowMacroChanges&&child->policy.allowSlotReuse==parent.policy.allowSlotReuse);
        if(child->catalog) {
            CHECK(parent.catalog && child->source==before && child->catalog->root==parent.catalog->root);
            CHECK(child->catalog->baseIntent.pattern==candidate.pattern);
        } else CHECK(child->source==candidate);
        CHECK(child->capturedPhrase==candidate.pattern&&child->capturedBpm==parent.capturedBpm);
        CHECK(child->role==parent.role&&child->key==parent.key&&child->scale==parent.scale);
        CHECK(p.aiOriginalSource()&&*p.aiOriginalSource()==before&&project(p)==before);
        ProposalEdits changed;changed.parameters={{"output.gain_db",.55f}};if(clear)changed.pattern=PatternDocument{};
        auto result=validResult(*child,changed);CHECK(p.receiveAiResult(std::move(result)));
        if(p.aiProposalPhase()!=AiProposalPhase::proposed)continue;
        auto expected=candidate;expected.sound.values[index("output.gain_db")]=.55f;if(clear)expected.pattern=PatternDocument{};
        CHECK(p.aiProposal()->candidate&&*p.aiProposal()->candidate==expected);
        CHECK(p.aiIntendedEdits()&&p.aiIntendedEdits()->pattern==expected.pattern);CHECK(project(p)==before);
        if(clear&&child->catalog){
            const auto afterClear=p.beginAiRefinement("Refine the cleared phrase","synthetic-selected_model:1.0");
            CHECK(afterClear&&afterClear->catalog);if(afterClear&&afterClear->catalog){
                CHECK(afterClear->source==before&&afterClear->catalog->root==parent.catalog->root);
                CHECK(afterClear->catalog->baseIntent.pattern==expected.pattern);
                CHECK(afterClear->capturedPhrase&&afterClear->capturedPhrase->noteCount==0);
                unsigned calls=0;const auto response=proposalJson({},true);
                auto refined=generateProposal(*afterClear,[&](const juce::String&,std::string_view prompt,const juce::var&){
                    ++calls;const auto payload=juce::JSON::parse(juce::String::fromUTF8(prompt.data(),int(prompt.size())));
                    const auto* notes=payload["context"]["phrase"]["notes"].getArray();CHECK(notes&&notes->isEmpty());
                    rack::oauth::StructuredResult reply;reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;
                    reply.text=juce::String::fromUTF8(response.c_str());return reply;
                });
                CHECK(calls==1&&refined.status==GenerationStatus::ready);CHECK(p.receiveAiResult(std::move(refined)));
                CHECK(p.aiProposal()&&p.aiProposal()->candidate&&*p.aiProposal()->candidate==expected);
            }
        }
        CHECK(p.applyAiProposal());CHECK(project(p)==expected);CHECK(p.undoSoundEdit());CHECK(project(p)==before);CHECK(!p.undoSoundEdit());
    }
}
void generation_failure_diagnostics()
{
    begin(__func__);
#if defined(_DEBUG)
    const auto previousRoot=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});
    const auto testRoot=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/host-fixtures")
        .getChildFile("task12-diagnostics-"+juce::Uuid().toString());
    CHECK(testRoot.createDirectory().wasOk());
    CHECK(SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",testRoot.getFullPathName().toWideCharPointer()));
    struct RestoreRoot { juce::String value; ~RestoreRoot(){SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",value.isEmpty()?nullptr:value.toWideCharPointer());} } restoreRoot{previousRoot};
    std::cout<<"DEBUG_DIAGNOSTIC_ROOT "<<testRoot.getFullPathName()<<'\n';
#endif
    auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;const auto before=project(p);
    auto pending=InstrumentAiTestAccess::beginHistorical25(p,options());CHECK(pending);if(!pending)return;
    auto result=generateProposal(*pending,[](const juce::String&,std::string_view,const juce::var&){
        rack::oauth::StructuredResult response;response.result.status=rack::oauth::Status::networkFailure;
        response.result.httpStatus=400;return response;});
    CHECK(result.errorCode=="generation_provider_failure");CHECK(result.status==GenerationStatus::providerFailure);
    CHECK(!p.receiveAiResult(std::move(result)));CHECK(p.aiProposalPhase()==AiProposalPhase::failed);CHECK(project(p)==before);
    const auto message=p.aiWorkflowStatus().message;
    CHECK(message.contains("network_failure"));CHECK(message.contains("HTTP 400"));CHECK(message.contains("request "+juce::String(pending->tag.requestId)));
    const auto logged=diagnostics::directory().getChildFile("plugin.log").loadFileAsString();
    CHECK(logged.contains(pending->tag.runtime.toString()));CHECK(logged.contains("generation_provider_failure"));CHECK(!logged.contains(options().instruction));
    pending=InstrumentAiTestAccess::beginHistorical25(p,options());CHECK(pending);if(!pending)return;
    result=generateProposal(*pending,[](const juce::String&,std::string_view,const juce::var&){rack::oauth::StructuredResult response;response.result.status=rack::oauth::Status::connected;
        response.result.httpStatus=200;response.text="{SENSITIVE_PROVIDER_BODY";return response;});
    CHECK(result.status==GenerationStatus::budgetExhausted&&result.inferenceCalls==3);CHECK(!p.receiveAiResult(std::move(result)));
    CHECK(p.aiWorkflowStatus().message.contains("budget_exhausted"));CHECK(p.aiWorkflowStatus().message.contains("proposal malformed after 3 attempts"));CHECK(project(p)==before);
    // Decision 8: a proposal still outside the user's reach after the repairs says so.
    pending=InstrumentAiTestAccess::beginHistorical25(p,options());CHECK(pending);if(!pending)return;
    {const auto quality=before.sound.values[index("output.render_quality")]<.5f?"1":"0";
     const auto outside=std::string(R"({"version":2,"parameter_edits":[{"id":"output.render_quality","normalized":)")+quality
        +R"(}],"rack_edits":[],"macro_edits":[],"pattern":null,"characteristics":"warm","explanation":"outside","name":"Outside Test Sound",)"
        +R"("reconciliation":{"detach":[],"remove":[],"replace":[],"controls":[]},"catalog_selections":[],"catalog_parameter_edits":[]})";
     result=generateProposal(*pending,[&](const juce::String&,std::string_view,const juce::var&){rack::oauth::StructuredResult response;response.result.status=rack::oauth::Status::connected;
        response.result.httpStatus=200;response.text=juce::String::fromUTF8(outside.c_str());return response;});}
    CHECK(result.status==GenerationStatus::budgetExhausted&&result.proposalStatus==ProposalStatus::outOfReach&&result.inferenceCalls==3);
    CHECK(!p.receiveAiResult(std::move(result)));CHECK(p.aiWorkflowStatus().message.contains("proposal outside the rack controls after 3 attempts"));CHECK(project(p)==before);
    CHECK(!diagnostics::directory().getChildFile("companion.log").loadFileAsString().contains("SENSITIVE"));
    pending=InstrumentAiTestAccess::beginHistorical25(p,options());CHECK(pending);if(!pending)return;
    GenerationResult hostile;hostile.tag=pending->tag;hostile.status=GenerationStatus::providerFailure;hostile.errorCode="Bearer SENSITIVE_TOKEN";
    CHECK(!p.receiveAiResult(std::move(hostile)));CHECK(!p.aiWorkflowStatus().message.contains("SENSITIVE"));
    CHECK(!diagnostics::directory().getChildFile("plugin.log").loadFileAsString().contains("SENSITIVE"));
#if defined(_DEBUG)
    // The product exposes its test-root injection only in Debug. Retain every
    // blocked-root assertion in the configuration that implements this seam.
    const auto normal=diagnostics::directory().getFullPathName();const auto blocked=diagnostics::directory().getChildFile("blocked-file");CHECK(blocked.replaceWithText("not a directory"));
    CHECK(SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",blocked.getFullPathName().toWideCharPointer()));
    pending=InstrumentAiTestAccess::beginHistorical25(p,options());CHECK(pending);if(pending){GenerationResult failed;failed.tag=pending->tag;failed.status=GenerationStatus::providerFailure;
        failed.providerStatus=rack::oauth::Status::networkFailure;CHECK(!p.receiveAiResult(std::move(failed)));CHECK(p.aiWorkflowStatus().message.contains("network_failure"));CHECK(project(p)==before);}
    CHECK(SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",normal.toWideCharPointer()));
#endif
}
void catalog_request_inventory()
{
    begin("catalog_request_inventory");
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()};
    const auto before = setup(p);
    CHECK(!before.sound.personalities || before.sound.personalities->instances.empty());
    std::shared_ptr<const RackCatalogSnapshot> captured;
    RackCatalogStatus captureStatus = RackCatalogStatus::unavailable;
    CHECK(p.captureAvailableRackCatalog(captured, &captureStatus));
    CHECK(captureStatus == RackCatalogStatus::ready && captured);
    if (!captured) return;
    CHECK(captured->entries.size() == libraryRackDesigns().size());
    // Public capture always uses the actual current library, including a future Tile append.
    const auto publicRequestStorage = airack::testing::fixtureValue([&]{return airack::testing::beginAiRequest(p,options(RequestScope::sound), 120);});
    const auto& publicRequest = *publicRequestStorage;
    CHECK(publicRequest && publicRequest->catalog);
    if(!publicRequest || !publicRequest->catalog)return;
    RackInventoryPtr publicCaptured;CHECK(testing::readyRackInventory(p,publicCaptured));if(!publicCaptured)return;
    CHECK(publicRequest->catalog->authority->snapshot==publicCaptured->id());
    const auto publicInventory=juce::JSON::parse(juce::String(publicRequest->catalog->authority->descriptorJson));
    CHECK(int(publicInventory["format"])==2&&publicInventory["entries"].size()==int(publicCaptured->rows().size())&&bool(publicInventory["complete"]));
    for(const auto& expected:publicCaptured->rows()){bool found=false;const auto* entries=publicInventory["entries"].getArray();
        if(entries)for(const auto& row:*entries)if(row[0].toString()==expected.metadata.design.toString()
            &&row[1].toString()==juce::String(expected.metadata.revision)&&row[2].toString()==expected.metadata.canonicalSha256)found=true;
        CHECK(found);}
    const bool ownsDrum=false; // The retained seventeen AIR recipes contain no detached drum maker.
    CHECK(publicRequest->catalog->authority->rendering==(ownsDrum?CatalogRendering::ownedLocal:CatalogRendering::companionPcm));
    p.discardAiProposal();CHECK(project(p)==before);
    std::shared_ptr<const RackCatalogSnapshot> historical;
    const auto request = InstrumentAiTestAccess::beginHistorical25(p, options(RequestScope::sound), 120, &historical);
    CHECK(historical && historical->entries.size()==25);if(!historical)return;
    CHECK(request.has_value());
    if (!request) return;
    CHECK(request->source == before);
    bool called = false, promptObject = false, fullCapturedInventory = false;
    std::size_t entriesObserved = 0;
    bool snapshotMatches = false, completeObserved = false;
    const auto result = generateProposal(*request,
        [&](const juce::String& model, std::string_view utf8, const juce::var&)
        {
            called = true; CHECK(model == request->model);
            const auto payload = juce::JSON::parse(juce::String::fromUTF8(utf8.data(), int(utf8.size())));
            promptObject = payload.isObject();
            const auto inventory = payload["catalog"]["rack_catalog"];
            const auto* entries = inventory["entries"].getArray();
            entriesObserved = entries ? std::size_t(entries->size()) : 0;
            snapshotMatches = inventory["snapshot"].toString() == historical->id.toString();
            completeObserved = inventory["complete"].isBool() && bool(inventory["complete"]);
            fullCapturedInventory = entries && snapshotMatches && completeObserved
                && entriesObserved == historical->entries.size();
            for (const auto& expected : historical->entries)
            {
                bool found = false;
                if (entries) for (const auto& row : *entries)
                    if (row["id"].toString() == expected.definition.id.toString()
                        && row["revision"].toString() == juce::String(expected.definition.revision)
                        && row["content_sha256"].toString() == expected.contentSha256)
                        found = true;
                fullCapturedInventory = fullCapturedInventory && found;
            }
            rack::oauth::StructuredResult reply;
            reply.result.status = rack::oauth::Status::networkFailure;
            reply.result.errorCode = "synthetic_discovery_stop";
            return reply; // No real inference, repair/render, audio or candidate.
        });
    CHECK(called && promptObject);
    CHECK(result.status == GenerationStatus::providerFailure && result.inferenceCalls == 1);
    CHECK(!result.candidate && result.audio.left.empty() && result.audio.right.empty());
    CHECK(project(p) == before);
    std::cout << "CATALOG_DISCOVERY public=" << publicCaptured->rows().size() << " historical=" << historical->entries.size()
        << " observed=" << entriesObserved << " snapshot=" << snapshotMatches
        << " complete=" << completeObserved << " root=" << historical->id.toString() << '\n';
    CHECK(fullCapturedInventory);
    p.discardAiProposal();
    CHECK(project(p) == before);
}
void catalog_native_materialize_apply_undo()
{
    begin("catalog_native_materialize_apply_undo");
    const auto design = airack::test::frozenExampleRackDesigns().front();
    InstrumentProcessor p{airack::testing::isolatedRackRuntime()}; const auto original = setup(p);
    const auto request = InstrumentAiTestAccess::beginHistorical25(p,options(RequestScope::sound), 120); CHECK(request && request->catalog);
    if (!request || !request->catalog) return;
    juce::MemoryBlock canonical; CHECK(encodeRackDesign(design, canonical, RackValidationMode::instanceRecall));
    auto response = juce::JSON::parse("{\"version\":2,\"parameter_edits\":[],\"rack_edits\":[],\"macro_edits\":[],\"pattern\":null,\"characteristics\":\"library rack\",\"explanation\":\"native capture\",\"name\":\"Library Test Rack\",\"reconciliation\":{\"detach\":[],\"remove\":[],\"replace\":[],\"controls\":[]},\"catalog_selections\":[],\"catalog_parameter_edits\":[]}");
    auto* selection = new juce::DynamicObject; selection->setProperty("local_id", 7); selection->setProperty("design", design.id.toDashedString());
    selection->setProperty("revision", juce::String(std::to_string(design.revision))); selection->setProperty("content_sha256", juce::SHA256(canonical).toHexString());
    auto* destination = new juce::DynamicObject; destination->setProperty("role", "synth"); destination->setProperty("chain", int(masterChain)); destination->setProperty("position", 1);
    selection->setProperty("destination", juce::var(destination)); response["catalog_selections"].getArray()->add(juce::var(selection));
    unsigned calls = 0;
    auto result = generateProposal(*request, [&](const juce::String&, std::string_view, const juce::var&) {
        ++calls; rack::oauth::StructuredResult reply; reply.result.status = rack::oauth::Status::connected; reply.result.httpStatus = 200;
        reply.text = juce::JSON::toString(response, true, 22); return reply;
    });
    CHECK(result.status == GenerationStatus::catalogSelectionRequired && calls == 1 && !result.candidate);
    CHECK(result.proposalStatus == ProposalStatus::accepted && result.audio.left.empty() && result.audio.right.empty());
    juce::MemoryBlock intermediate; CHECK(companion::encodeResult(*request, result, intermediate));
    CHECK(p.receiveAiResult(std::move(result)));
    CHECK(p.aiProposalPhase() == AiProposalPhase::pending && !p.aiProposal() && project(p) == original);
    const auto* pending = p.aiCurrentRequest(); CHECK(pending && pending->catalog && pending->catalog->phase == CatalogPhase::materialize);
    if (pending) {
        auto ready = generateProposal(*pending, [&](const juce::String&, std::string_view, const juce::var&) { ++calls; return rack::oauth::StructuredResult{}; });
        CHECK(ready.status == GenerationStatus::ready && calls == 1 && ready.inferenceCalls == 1 && ready.candidate);
        CHECK(p.receiveAiResult(std::move(ready))); CHECK(p.aiProposal() && project(p) == original);
        CHECK(p.applyAiProposal()); const auto placed = project(p); CHECK(placed.sound.personalities && placed.sound.personalities->instances.size() == 1);
        CHECK(p.undoSoundEdit()); CHECK(project(p) == original); CHECK(!p.undoSoundEdit());
    }
}
// External source-first fixture block; inserted before the frozen namespace end.
juce::var catalogWitnessJson(const ProposalEdits& edits)
{
    auto response = juce::JSON::parse(proposalJson(edits, true));
    juce::Array<juce::var> selections, parameters;
    for (const auto& s : edits.catalogSelections)
        selections.add(object({{"local_id", int(s.localId)}, {"design", s.design.toDashedString()},
            {"revision", juce::String(std::to_string(s.revision))}, {"content_sha256", s.contentSha256},
            {"destination", object({{"role", "synth"}, {"chain", int(s.destination.chain)}, {"position", int(s.destination.position)}})}}));
    for (const auto& e : edits.catalogParameters)
        parameters.add(object({{"selection", int(e.selection)}, {"unit", int(e.unit)}, {"key", e.key}, {"normalized", double(e.normalized)}}));
    response.getDynamicObject()->setProperty("catalog_selections", selections);
    response.getDynamicObject()->setProperty("catalog_parameter_edits", parameters);
    juce::Array<juce::var> removed;
    for (const auto& id : edits.reconciliation.remove) removed.add(id.toDashedString());
    response["reconciliation"].getDynamicObject()->setProperty("remove", removed);
    return response;
}
ProposalEdits catalogWitnessIntent()
{
    const auto design = airack::test::frozenFactoryRackDesigns().front(); juce::MemoryBlock canonical;
    CHECK(design.name == "Copper Arc" && encodeRackDesign(design, canonical));
    ProposalEdits edits;
    edits.parameters = {{"global.master_level", .1f}};
    edits.macros = {{1, "catalog:7:0:analog.filter.cutoff", .25f}}; edits.pattern = phrase(67);
    edits.catalogSelections = {{7, design.id, design.revision, juce::SHA256(canonical).toHexString(), {RackRole::synth, masterChain, 1}}};
    edits.catalogParameters = {{7, 0, "analog.filter.cutoff", .25f}};
    return edits;
}
GenerationResult catalogWitnessResult(const GenerationRequest& request, const ProposalEdits& edits, unsigned& calls)
{
    return generateProposal(request, [&](const juce::String&, std::string_view, const juce::var&) {
        ++calls; CHECK(request.catalog && request.catalog->phase == CatalogPhase::select);
        rack::oauth::StructuredResult reply; reply.result.status = rack::oauth::Status::connected; reply.result.httpStatus = 200;
        reply.text = juce::JSON::toString(catalogWitnessJson(edits), true, 22); return reply;
    });
}
void catalog_pending_materialization_retirement()
{
    begin(__func__);
    for (int retirement = 0; retirement != 4; ++retirement)
    {
        auto owner = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *owner;
        auto original = std::make_unique<ProjectDocument>(setup(p));
        auto request = std::make_unique<std::optional<GenerationRequest>>(InstrumentAiTestAccess::beginHistorical25(p,options(), 120));
        CHECK(*request && (*request)->catalog); if (!*request || !(*request)->catalog) continue;
        unsigned calls = 0; auto selected = std::make_unique<GenerationResult>(catalogWitnessResult(**request, catalogWitnessIntent(), calls));
        CHECK(selected->status == GenerationStatus::catalogSelectionRequired && calls == 1 && selected->edits.catalogSelections.size() == 1);
        CHECK(p.receiveAiResult(std::move(*selected))); const auto* current = p.aiCurrentRequest();
        CHECK(current && current->catalog && current->catalog->phase == CatalogPhase::materialize && p.aiProposalPhase() == AiProposalPhase::pending);
        if (!current || !current->catalog) continue;
        auto pending = std::make_unique<GenerationRequest>(*current);
        CHECK(pending->catalog->selectionParent == (*request)->tag && pending->catalog->authority->definitions.size() == 1);
        auto ready = std::make_unique<GenerationResult>(catalogWitnessResult(*pending, {}, calls));
        CHECK(ready->status == GenerationStatus::ready && ready->candidate && ready->inferenceCalls == 1 && calls == 1);
        if (retirement == 0) p.discardAiProposal();
        if (retirement == 1) p.cancelAi();
        if (retirement == 2) { auto replacement = std::make_unique<std::optional<GenerationRequest>>(InstrumentAiTestAccess::beginHistorical25(p,options(), 121)); CHECK(*replacement && (*replacement)->tag != pending->tag); }
        if (retirement == 3) CHECK(recall(p, *original));
        CHECK(!p.receiveAiResult(std::move(*ready)));
        CHECK(!p.aiProposal() && project(p) == *original && !p.undoSoundEdit());
        CHECK(!p.applyAiProposal() && !p.startAiPreview()); p.discardAiProposal();
    }
}
void catalog_refined_aggregate_transaction()
{
    begin(__func__);
    for (bool reject : {false, true})
    {
        auto owner = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *owner;
        auto original = std::make_unique<ProjectDocument>(setup(p, PlaybackMode::internalPattern));
        auto parentRequest = std::make_unique<std::optional<GenerationRequest>>(InstrumentAiTestAccess::beginHistorical25(p,options(), 120));
        CHECK(*parentRequest && (*parentRequest)->catalog); if (!*parentRequest || !(*parentRequest)->catalog) continue;
        const auto intent = catalogWitnessIntent(); unsigned calls = 0;
        auto selected = std::make_unique<GenerationResult>(catalogWitnessResult(**parentRequest, intent, calls));
        CHECK(selected->status == GenerationStatus::catalogSelectionRequired); CHECK(p.receiveAiResult(std::move(*selected)));
        const auto* pending = p.aiCurrentRequest(); CHECK(pending && pending->catalog && pending->catalog->phase == CatalogPhase::materialize); if (!pending) continue;
        auto ready = std::make_unique<GenerationResult>(catalogWitnessResult(*pending, {}, calls));
        CHECK(ready->status == GenerationStatus::ready && calls == 1); CHECK(p.receiveAiResult(std::move(*ready)));
        CHECK(p.aiProposal() && p.aiProposal()->candidate && p.aiIntendedEdits()); if (!p.aiProposal() || !p.aiProposal()->candidate || !p.aiIntendedEdits()) continue;
        CHECK(p.aiProposal()->candidate->sound.personalities && p.aiProposal()->candidate->sound.personalities->instances.size() == 1);
        if (!p.aiProposal()->candidate->sound.personalities || p.aiProposal()->candidate->sound.personalities->instances.empty()) continue;
        const auto selectedId = p.aiProposal()->candidate->sound.personalities->instances.front().id;
        auto child = std::make_unique<std::optional<GenerationRequest>>(p.beginAiRefinement("Refine the selected brightness", "synthetic-selected_model:1.0"));
        CHECK(*child && (*child)->catalog); if (!*child || !(*child)->catalog) continue;
        CHECK((*child)->source == *original && (*child)->catalog->root == (*parentRequest)->tag);
        CHECK((*child)->catalog->baseIntent.parameters == intent.parameters && (*child)->catalog->baseIntent.macros == intent.macros);
        CHECK((*child)->catalog->baseIntent.pattern == intent.pattern && (*child)->catalog->baseIntent.racks.size() == intent.racks.size());
        if ((*child)->catalog->baseIntent.racks.size() != intent.racks.size()) continue;
        for (std::size_t n = 0; n < intent.racks.size(); ++n) {
            const auto& actual = (*child)->catalog->baseIntent.racks[n]; const auto& expected = intent.racks[n];
            CHECK(actual.kind == expected.kind && actual.slot == expected.slot && actual.destination == expected.destination
                && actual.position == expected.position && actual.effect == expected.effect && actual.allowReuse == expected.allowReuse);
        }
        ProposalEdits delta; delta.catalogSelections = intent.catalogSelections; delta.catalogParameters = {{7, 0, "analog.filter.cutoff", .35f}};
        auto refined = std::make_unique<GenerationResult>(catalogWitnessResult(**child, delta, calls));
        CHECK(refined->status == GenerationStatus::ready && refined->inferenceCalls == 1 && calls == 2 && refined->candidate);
        CHECK(p.receiveAiResult(std::move(*refined))); CHECK(p.aiIntendedEdits() && p.aiProposal()); if (!p.aiIntendedEdits() || !p.aiProposal()) continue;
        CHECK(p.aiIntendedEdits()->catalogSelections == intent.catalogSelections && p.aiIntendedEdits()->catalogParameters == delta.catalogParameters);
        CHECK(p.aiIntendedEdits()->parameters == intent.parameters && p.aiIntendedEdits()->macros == intent.macros && p.aiIntendedEdits()->pattern == intent.pattern);
        auto expected = std::make_unique<ProjectDocument>(*p.aiProposal()->candidate);
        CHECK(expected->sound.personalities && expected->sound.personalities->instances.size() == 1);
        if (!expected->sound.personalities || expected->sound.personalities->instances.empty()) continue;
        CHECK(expected->sound.personalities->instances.front().id == selectedId);
        const auto* binding = rackUnitBinding(expected->sound.personalities->instances.front(), 0); CHECK(binding); if (!binding) continue;
        const auto target = rackParameterIndex(NativeModule::analog, binding->slot, "analog.filter.cutoff"); CHECK(target); if (!target) continue;
        CHECK(expected->sound.values[*target] == .35f && expected->pattern == *intent.pattern);
        CHECK(expected->sound.macros[0] == (MacroBinding{parameterRegistry()[*target].stateKey, .25f}));
        CHECK(project(p) == *original);
        auto events = std::make_unique<Events>(p); const auto unrelated = index("output.gain_db");
        if (reject) {
            const auto invalid = index("synth.1.modulation.1.target");
            auto rejectedSource = std::make_unique<ProjectDocument>(*original); rejectedSource->sound.values[invalid] = 1;
            CHECK(!validateProjectDocument(*rejectedSource));
            auto scratch = std::make_unique<ProjectDocument>();
            CHECK(admitProposal(*rejectedSource, (*child)->policy, *p.aiIntendedEdits(), *(*child)->catalog->authority, *scratch) != ProposalStatus::accepted);
            events->onBegin = [&] { p.control(invalid).setValue(1); };
            CHECK(!p.applyAiProposal() && events->fired); CHECK(project(p) == *rejectedSource && !p.undoSoundEdit());
            for (std::size_t i = 0; i < parameterCount; ++i) CHECK(events->starts[i] == events->ends[i] && events->values[i] == 0);
        } else {
            p.control(unrelated).setValue(.61f); expected->sound.values[unrelated] = .61f;
            CHECK(p.applyAiProposal()); CHECK(project(p) == *expected && p.aiProposalPhase() == AiProposalPhase::idle);
            for (std::size_t i = 0; i < parameterCount; ++i) {
                const bool changed = i != unrelated && original->sound.values[i] != expected->sound.values[i];
                CHECK(events->starts[i] == events->ends[i] && events->starts[i] == int(changed) && events->values[i] == int(changed));
            }
            p.control(unrelated).setValue(.72f); CHECK(p.undoSoundEdit());
            auto undone = std::make_unique<ProjectDocument>(project(p)); CHECK(undone->sound.values[unrelated] == .72f);
            undone->sound.values[unrelated] = original->sound.values[unrelated]; CHECK(*undone == *original); CHECK(!p.undoSoundEdit());
        }
    }
}

#ifndef NDEBUG
using OwnedAiAccess=InstrumentDrumTestAccess;
void ownedAiSameAssignments(const OwnedAiAccess::State& a,const OwnedAiAccess::State& b)
{
    CHECK(a.valid && b.valid && a.slots==b.slots && a.owners==b.owners);
}
struct DormantAiFixture {
    std::unique_ptr<InstrumentProcessor> processor=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());
    std::array<juce::Uuid,2> racks;
    DormantAiFixture() {
        auto initial=airack::testing::fixtureValue([&]{return setup(p());});
        for(unsigned bank=0;bank<2;++bank) {
            RackDesign design; design.name="Owned AI Drum"; design.panel.sections={"Drum"};
            design.units={{0,NativeModule::drum,{},legacyAlgorithmRevision}};
            RackOperation op; op.design=design; op.newInstance=racks[bank]; op.allowSlotReuse=true;
            op.allocation={{0,std::uint8_t(bank)}}; op.destination={RackRole::synth,masterChain,std::uint8_t(bank)};
            CHECK(p().applyRackOperation(op,revision(p()))==RackCandidateStatus::ready);
        }
        assign(0,0,.25f,"dormant A"); assign(0,3,.375f,"dormant B"); assign(1,2,.5f,"neighbor");
        auto owned=airack::testing::fixtureValue([&]{return airack::testing::beginAiRequest(p(),options(RequestScope::sound),119);});
        CHECK(owned->has_value());
        if(*owned){CHECK((**owned).source==project(p())&&(**owned).catalog&&(**owned).catalog->authority->rendering==CatalogRendering::ownedLocal);}
        p().discardAiProposal();CHECK(p().aiProposalPhase()==AiProposalPhase::idle);
        const auto assigned=OwnedAiAccess::snapshot(p());
        for(unsigned bank=0;bank<2;++bank) {
            RackDesign melodic; melodic.name="Owned AI Melodic"; melodic.panel.sections={"Tone"};
            melodic.units={{0,NativeModule::analog,{},legacyAlgorithmRevision}};
            RackOperation swap; swap.kind=RackOperationKind::swap; swap.selected=racks[bank];
            swap.design=melodic; swap.allowSlotReuse=true; swap.destination={RackRole::synth,masterChain,std::uint8_t(bank)};
            CHECK(p().applyRackOperation(swap,revision(p()))==RackCandidateStatus::ready);
        }
        const auto melodic=OwnedAiAccess::snapshot(p()); ownedAiSameAssignments(*assigned,*melodic);
        CHECK(melodic->project.sound.personalities && melodic->project.sound.personalities->instances.size()==2);
        CHECK(melodic->project.sound.values[drumModeParameterStart]==0 && melodic->project.sound.values[drumModeParameterStart+1]==0);
    }
    InstrumentProcessor& p(){return *processor;}
    void assign(unsigned bank,unsigned pad,float marker,const char* name) {
        const std::array<float,4> samples {marker,-marker,.125f,0}; std::uint64_t ticket=0; DrumCommitError error;
        CHECK(p().prepareDrumAsset(samples,1,ticket,error) && error==DrumCommitError::none);
        const auto prepared=OwnedAiAccess::snapshot(p()); drums::DrumPad metadata; metadata.name=name;
        for(unsigned n=0;n<3;++n) if(prepared->tickets[n]==ticket) metadata.asset=prepared->candidates[n];
        CHECK(metadata.asset.has_value()); drums::SourceProvenance provenance;
        provenance.kind=drums::SourceKind::generated; provenance.sourceFrames=4; provenance.end=4;
        provenance.generator="literal-owned-ai"; provenance.condition=name; provenance.seed=bank*16+pad;
        metadata.source=provenance; DrumPadTarget target;
        CHECK(p().captureDrumPadTarget(racks[bank],0,std::uint8_t(pad),target,error));
        const std::array<ParameterGestureValue,1> trim {{{drumParameterStart+bank*drumParametersPerSlot+pad*drumParametersPerPad+3,.2f}}};
        CHECK(p().assignPreparedDrumPad(target,ticket,metadata,trim,error) && error==DrumCommitError::none);
    }
    bool retire(unsigned bank,bool neverUsed) {
        RackOperation remove; remove.kind=RackOperationKind::remove; remove.selected=racks[bank];
        const bool removed=p().applyRackOperation(remove,revision(p()))==RackCandidateStatus::ready; CHECK(removed); if(!removed)return false;
        if(neverUsed) {
            auto current=OwnedAiAccess::snapshot(p()); auto candidate=std::make_unique<SoundDocument>(current->project.sound);
            candidate->layout.synthUse[bank]=SlotUse::neverUsed;
            CHECK(validateSoundDocument(*candidate));
            // Ordinary candidate admission correctly forbids silently rebinding a dormant bank.
            // Reach the explicitly supported neverUsed closure through a genuine self-contained host bundle.
            juce::MemoryBlock saved; p().getStateInformation(saved); drums::DrumBundlePtr existing;
            drums::DrumError error; const auto bytes=std::span(static_cast<const std::uint8_t*>(saved.getData()),saved.getSize());
            if(!drums::decodeDrumBundle(bytes,existing,error)) {CHECK(false);return false;}
            auto numeric=std::make_unique<ProjectDocument>(current->project); numeric->sound=*candidate;
            juce::MemoryBlock compact; CHECK(encodeProjectState(*numeric,compact)); drums::DrumDocumentPtr document;
            CHECK(drums::makeDrumDocument(existing->document().slots(),document,error)); drums::DrumBundlePtr bundle;
            if(!drums::makeDrumBundle(document,std::span(static_cast<const std::uint8_t*>(compact.getData()),compact.getSize()),existing->assets(),bundle,error)) {CHECK(false);return false;}
            juce::MemoryBlock encoded; CHECK(drums::encodeDrumBundle(*bundle,encoded,error));
            const bool accepted=p().restoreProjectState(encoded.getData(),int(encoded.getSize())); CHECK(accepted); if(!accepted)return false;
            const auto restored=OwnedAiAccess::snapshot(p()); CHECK(restored->slots==current->slots && restored->history.count==0);
        }
        return true;
    }
};
bool ownedAiCatalogProposal(InstrumentProcessor& p)
{
    const auto design=airack::test::frozenFactoryRackDesigns().front(); CHECK(design.name=="Copper Arc");
    juce::MemoryBlock canonical; CHECK(encodeRackDesign(design,canonical));
    ProposalEdits intent; intent.name="Owned Catalog AI"; intent.parameters={{"global.master_level",.23f}};
    intent.catalogSelections={{7,design.id,design.revision,juce::SHA256(canonical).toHexString(),{RackRole::synth,masterChain,0}}};
    intent.catalogParameters={{7,0,"analog.filter.cutoff",.37f}};
    auto request=std::make_unique<std::optional<GenerationRequest>>(InstrumentAiTestAccess::beginHistorical25(p,options(RequestScope::sound),120));
    CHECK(*request && (*request)->catalog); if(!*request || !(*request)->catalog)return false;
    unsigned calls=0; auto selected=std::make_unique<GenerationResult>(catalogWitnessResult(**request,intent,calls));
    CHECK(selected->status==GenerationStatus::catalogSelectionRequired && calls==1);
    if(selected->status!=GenerationStatus::catalogSelectionRequired)return false;
    CHECK(p.receiveAiResult(std::move(*selected))); const auto* pending=p.aiCurrentRequest();
    CHECK(pending && pending->catalog && pending->catalog->phase==CatalogPhase::materialize); if(!pending)return false;
    auto ready=std::make_unique<GenerationResult>(catalogWitnessResult(*pending,{},calls));
    CHECK(ready->status==GenerationStatus::ready && ready->candidate && calls==1);
    CHECK(p.receiveAiResult(std::move(*ready))); return p.aiProposalPhase()==AiProposalPhase::proposed && p.aiProposal();
}
void owned_ai_numeric_and_raw_reuse()
{
    begin("numeric_ai_history_is_assignment_free"); DormantAiFixture fixture; auto& p=fixture.p();
    auto before=OwnedAiAccess::snapshot(p); ProposalEdits numeric; numeric.parameters={{"global.master_level",.23f}};
    if(!propose(p,numeric,options(RequestScope::sound)))return;
    const bool numericApplied=p.applyAiProposal(); CHECK(numericApplied); if(!numericApplied)return; auto after=OwnedAiAccess::snapshot(p); ownedAiSameAssignments(*before,*after);
    CHECK(after->history.count==before->history.count+1 && after->history.cursor==after->history.count);
    const auto last=after->history.count-1;
    CHECK(after->history.kinds[last]==0 && after->history.rootCounts[last]==0 && after->history.masks[last]==0);
    CHECK(after->lastActionType.find("AiDocumentAction")!=std::string::npos);
    CHECK(p.undoSoundEdit()); auto undone=OwnedAiAccess::snapshot(p);
    CHECK(undone->project==before->project); ownedAiSameAssignments(*before,*undone);
    CHECK(p.redoSoundEdit()); auto redone=OwnedAiAccess::snapshot(p);
    CHECK(redone->project==after->project); ownedAiSameAssignments(*after,*redone);
    begin("raw_ai_retired_reuse_preserves_existing_reach_boundary");
    if(!fixture.retire(0,false))return; before=OwnedAiAccess::snapshot(p);
    ProposalEdits raw; raw.name="Raw Reused AI"; raw.racks={{RackEditKind::addSynth,0,masterChain,0}};
    auto request=std::make_unique<std::optional<GenerationRequest>>(InstrumentAiTestAccess::beginHistorical25(p,options(RequestScope::sound),400));CHECK(*request);if(!*request)return;
    const auto text=proposalJson(raw,(*request)->catalog.has_value());
    auto rejected=std::make_unique<GenerationResult>(generateProposal(**request,[&](const juce::String&,std::string_view,const juce::var&){rack::oauth::StructuredResult reply;
        reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;reply.text=juce::String::fromUTF8(text.data(),int(text.size()));return reply;}));
    CHECK(rejected->status==GenerationStatus::budgetExhausted&&rejected->proposalStatus==ProposalStatus::outOfReach&&!rejected->candidate);
    CHECK(!p.receiveAiResult(std::move(*rejected)));auto unchanged=OwnedAiAccess::snapshot(p);CHECK(unchanged->project==before->project&&unchanged->history==before->history);ownedAiSameAssignments(*before,*unchanged);
    begin("admitted_raw_ai_move_preserves_dormant_assignments");raw.name="Raw Moved AI";raw.racks={{RackEditKind::moveSynth,1,masterChain,0}};
    if(!propose(p,raw,options(RequestScope::sound)))return;
    const bool rawApplied=p.applyAiProposal(); CHECK(rawApplied); if(!rawApplied)return; after=OwnedAiAccess::snapshot(p); ownedAiSameAssignments(*before,*after);
    CHECK(after->project.sound.layout.synthUse[0]==SlotUse::retired&&after->project.sound.layout.synthUse[1]==SlotUse::present);
    CHECK(after->history.count==before->history.count+1 && after->history.kinds[after->history.count-1]==2);
    CHECK(after->history.masks[after->history.count-1]==0 && after->history.rootCounts[after->history.count-1]==0);
    CHECK(after->lastActionType.find("OwnedDocumentAction")!=std::string::npos);
    CHECK(after->project.sound.values[drumParameterStart+3]==before->project.sound.values[drumParameterStart+3]);
    CHECK(p.undoSoundEdit()); undone=OwnedAiAccess::snapshot(p); CHECK(undone->project==before->project); ownedAiSameAssignments(*before,*undone);
    CHECK(p.redoSoundEdit()); redone=OwnedAiAccess::snapshot(p); CHECK(redone->project==after->project); ownedAiSameAssignments(*after,*redone);
}
void owned_ai_catalog_reuse_roundtrip()
{
    begin("typed_ai_owned_rack_entrypoint"); DormantAiFixture fixture; auto& p=fixture.p();
    if(!fixture.retire(0,true))return;
    const auto before=OwnedAiAccess::snapshot(p);
    CHECK(before->project.sound.layout.synthUse[0]==SlotUse::neverUsed && before->slots[0][0].asset && before->slots[0][3].asset);
    if(!ownedAiCatalogProposal(p)) {CHECK(false);return;}
    const bool applied=p.applyAiProposal(); CHECK(applied); if(!applied)return; const auto after=OwnedAiAccess::snapshot(p);
    CHECK(after->slots[0]==drums::DrumKit{} && after->owners[0]==nullptr && after->owners[3]==nullptr);
    CHECK(after->slots[1]==before->slots[1] && after->owners[18]==before->owners[18]);
    CHECK(after->project.sound.values[drumParameterStart+3]==defaultParameterValues()[drumParameterStart+3]);
    CHECK(after->project.sound.values[index("synth.1.analog.filter.cutoff")]==.37f);
    CHECK(after->history.count==before->history.count+1); const auto last=after->history.count-1;
    CHECK(after->history.kinds[last]==2 && after->history.masks[last]==0xffff && after->history.rootCounts[last]==2);
    CHECK(after->lastActionType.find("OwnedDocumentAction")!=std::string::npos && after->targets[0]!=before->targets[0]);
    CHECK(p.undoSoundEdit()); auto undone=OwnedAiAccess::snapshot(p); CHECK(undone->project==before->project); ownedAiSameAssignments(*before,*undone);
    CHECK(p.redoSoundEdit()); auto redone=OwnedAiAccess::snapshot(p); CHECK(redone->project==after->project); ownedAiSameAssignments(*after,*redone);
    // Sparse history replay leaves a later, unrelated host automation value current.
    p.control(index("output.gain_db")).setValue(.71f); CHECK(p.undoSoundEdit()); undone=OwnedAiAccess::snapshot(p);
    auto expected=std::make_unique<ProjectDocument>(before->project); expected->sound.values[index("output.gain_db")]=.71f;
    CHECK(undone->project==*expected); ownedAiSameAssignments(*before,*undone);
    CHECK(p.redoSoundEdit()); redone=OwnedAiAccess::snapshot(p); ownedAiSameAssignments(*after,*redone);
    CHECK(redone->project.sound.values[index("output.gain_db")]==.71f);
}
void owned_ai_external_begin_rejection()
{
    begin(__func__);
    for(unsigned interference=0;interference<2;++interference) {
        DormantAiFixture fixture; auto& p=fixture.p(); if(!fixture.retire(0,true))continue;
        if(!ownedAiCatalogProposal(p)){CHECK(false);continue;}
        std::uint64_t ticket=0; DrumCommitError error; const std::array<float,4> candidate {.7f,0,-.2f,.1f};
        CHECK(p.prepareDrumAsset(candidate,1,ticket,error));
        const auto prior=OwnedAiAccess::snapshot(p);
        auto events=std::make_unique<Events>(p); std::unique_ptr<OwnedAiAccess::State> external;
        events->onBegin=[&] {
            if(interference==0) CHECK(p.setAiMusicalContext({"bass","E","minor",{}}));
            else p.control(drumModeParameterStart+1).setValue(1); // Actual external source-family epoch invalidates admission.
            external=OwnedAiAccess::snapshot(p);
        };
        CHECK(!p.applyAiProposal() && events->fired && external);
        const auto after=OwnedAiAccess::snapshot(p); if(!external)continue;
        CHECK(after->project==external->project && after->history==external->history);
        CHECK(after->edit==external->edit && after->reset==external->reset && after->context==external->context && after->source==external->source);
        ownedAiSameAssignments(*external,*after);
        CHECK(after->generation==external->generation && after->targets==external->targets && after->tickets==external->tickets);
        CHECK(after->tickets==prior->tickets && after->candidates==prior->candidates && after->history==prior->history);
        ownedAiSameAssignments(*prior,*after); p.discardPreparedDrumAsset(ticket);
        CHECK(after->project.sound.name!="Owned Catalog AI" && after->slots[0][0].asset && after->slots[0][3].asset);
        for(std::size_t n=0;n<parameterCount;++n) CHECK(events->starts[n]==events->ends[n] && events->values[n]==0);
    }
}
void owned_ai_post_notification_reseal()
{
    begin(__func__);
    for(bool endCallback:{false,true}) for(bool explicitRecall:{false,true}) {
        DormantAiFixture fixture; auto& p=fixture.p(); if(!fixture.retire(0,true))continue;
        if(!ownedAiCatalogProposal(p)){CHECK(false);continue;}
        juce::MemoryBlock restore; p.getStateInformation(restore); CHECK(restore.getSize()>0);
        const auto before=OwnedAiAccess::snapshot(p); auto events=std::make_unique<Events>(p);
        std::unique_ptr<OwnedAiAccess::State> reentered;
        auto callback=[&] {
            // Value/end notifications run after the complete owned core; both halves are already installed.
            const auto installed=OwnedAiAccess::snapshot(p);
            CHECK(installed->slots[0]==drums::DrumKit{} && installed->history.cursor!=0);
            if(installed->history.cursor) CHECK(installed->history.kinds[installed->history.cursor-1]==2);
            CHECK(installed->project.sound.values[index("synth.1.analog.filter.cutoff")]==.37f);
            if(explicitRecall) CHECK(p.restoreProjectState(restore.getData(),int(restore.getSize())));
            else {
                auto current=OwnedAiAccess::snapshot(p); auto changed=std::make_unique<SoundDocument>(current->project.sound);
                changed->values[index("output.gain_db")]=.67f;
                CHECK(p.applySoundEdit(*changed,SoundEditOrigin::manual,current->edit));
            }
            reentered=OwnedAiAccess::snapshot(p);
        };
        if(endCallback)events->onEnd=callback; else events->onValue=callback;
        CHECK(p.applyAiProposal()); CHECK((endCallback?events->endFired:events->valueFired) && reentered);
        const auto after=OwnedAiAccess::snapshot(p); if(!reentered)continue;
        CHECK(after->project==reentered->project && after->history==reentered->history); ownedAiSameAssignments(*reentered,*after);
        CHECK(after->group==after->project && after->groupEdit==after->edit);
        if(explicitRecall) { ownedAiSameAssignments(*before,*after); CHECK(after->project==before->project && after->history.count==0); }
        else CHECK(after->slots[0]==drums::DrumKit{} && after->project.sound.values[index("output.gain_db")]==.67f);
        for(std::size_t n=0;n<parameterCount;++n) CHECK(events->starts[n]==events->ends[n]);
    }
}
void owned_ai_entrypoint_contracts()
{
    owned_ai_numeric_and_raw_reuse(); owned_ai_catalog_reuse_roundtrip(); owned_ai_external_begin_rejection(); owned_ai_post_notification_reseal();
}
#endif

void apply_names_the_sound()
{
    begin(__func__);
    const auto root = juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT", {});
    CHECK(juce::File::isAbsolutePath(root)); if (!juce::File::isAbsolutePath(root)) return;
    const auto folder = juce::File(root).getChildFile("Sounds"); folder.deleteRecursively();
    auto pStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& p = *pStorage; setup(p);
    const auto path = [&](const char* stem) { return folder.getChildFile(juce::String(stem) + ".airack").getFullPathName(); };
    CHECK(p.soundFolder() == folder && p.soundPresetPath().isEmpty());
    ProposalEdits named; named.name = "Ethereal Rolling Bass"; named.parameters = {{"output.gain_db", .5f}};
    CHECK(propose(p, named)); CHECK(p.applyAiProposal());
    CHECK(project(p).sound.name == "Ethereal Rolling Bass" && p.soundPresetPath() == path("Ethereal Rolling Bass"));
    CHECK(p.undoSoundEdit()); CHECK(project(p).sound.name.isEmpty());
    CHECK(p.redoSoundEdit()); CHECK(project(p).sound.name == "Ethereal Rolling Bass");
    // Another open instance's name and the folder's presets are in use, ignoring case.
    CHECK(folder.createDirectory()); CHECK(folder.getChildFile("ethereal rolling bass 2.airack").create());
    auto qStorage = std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()); auto& q = *qStorage; setup(q);
    CHECK((q.soundNamesInUse() == std::vector<juce::String>{"Ethereal Rolling Bass", "ethereal rolling bass 2"}));
    auto request = InstrumentAiTestAccess::beginHistorical25(q,options(), 400); CHECK(request.has_value()); if (!request) return;
    CHECK(request->namesInUse == q.soundNamesInUse());
    CHECK(q.receiveAiResult(validResult(*request, named))); CHECK(q.applyAiProposal());
    CHECK(project(q).sound.name == "Ethereal Rolling Bass 3" && q.soundPresetPath() == path("Ethereal Rolling Bass 3"));
    CHECK((p.soundNamesInUse() == std::vector<juce::String>{"Ethereal Rolling Bass 3", "ethereal rolling bass 2"}));
    // An answer without a name (a pattern request's version 1) keeps the sound's name and its path.
    q.setSoundPresetPath("Typed.airack");
    CHECK(propose(q, {}, options(RequestScope::pattern))); CHECK(q.applyAiProposal());
    CHECK(project(q).sound.name == "Ethereal Rolling Bass 3" && q.soundPresetPath() == "Typed.airack");
    // The folder follows the last preset saved.
    const auto other = folder.getChildFile("Other"); CHECK(q.saveSoundPreset(other.getChildFile("Mine.airack")));
    CHECK(q.soundFolder() == other); CHECK(propose(q, named)); CHECK(q.applyAiProposal());
    CHECK(project(q).sound.name == "Ethereal Rolling Bass 2" && q.soundPresetPath() == other.getChildFile("Ethereal Rolling Bass 2.airack").getFullPathName());
    folder.deleteRecursively();
}

template<class F> bool waitManualAi(F&& ready)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(std::chrono::steady_clock::now()<deadline){if(ready())return true;juce::MessageManager::getInstance()->runDispatchLoopUntil(10);}return ready();
}
juce::File manualAiSource()
{
    const auto root=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});
    CHECK(juce::File::isAbsolutePath(root));if(!juce::File::isAbsolutePath(root))return {};
    const auto folder=juce::File(root).getChildFile("manual-ai-source-"+juce::Uuid().toString());
    const auto directory=folder.createDirectory();CHECK(directory.wasOk());
    const auto file=folder.getChildFile("source.wav");juce::MemoryOutputStream bytes;
    bytes.write("RIFF",4);bytes.writeInt(36+48000*2);bytes.write("WAVEfmt ",8);bytes.writeInt(16);bytes.writeShort(1);bytes.writeShort(1);bytes.writeInt(48000);bytes.writeInt(96000);bytes.writeShort(2);bytes.writeShort(16);bytes.write("data",4);bytes.writeInt(48000*2);
    for(unsigned n=0;n<48000;++n)bytes.writeShort(8192);
    auto stream=file.createOutputStream();const bool opened=stream!=nullptr;CHECK(opened);
    bool written=false;if(stream){written=stream->write(bytes.getData(),bytes.getDataSize());stream->flush();written=written&&stream->getStatus().wasOk();}
    std::cout<<"MANUAL_AI_SOURCE path="<<file.getFullPathName()<<" directory_ok="<<directory.wasOk()
        <<" directory_error="<<directory.getErrorMessage()<<" opened="<<opened<<" written="<<written
        <<" expected_bytes="<<bytes.getDataSize()<<" actual_bytes="<<file.getSize()
        <<" stream_error="<<(stream?stream->getStatus().getErrorMessage():juce::String("createOutputStream returned null"))<<std::endl;
    CHECK(written);CHECK(file.getSize()==std::int64_t(bytes.getDataSize()));return file;
}
bool manualAiRack(InstrumentProcessor& p,const juce::Uuid& rack,NativeModule module,bool swap)
{
    RackOperation operation;operation.kind=swap?RackOperationKind::swap:RackOperationKind::place;operation.selected=swap?rack:juce::Uuid::null();operation.newInstance=rack;operation.allowSlotReuse=true;
    operation.design=RackDesign{};operation.design->name="Manual AI caller fixture";operation.design->panel.sections={"Source"};operation.design->units={{0,module,{},legacyAlgorithmRevision}};operation.destination={RackRole::synth,masterChain,0};if(!swap)operation.allocation={{0,0}};
    const bool ready=p.applyRackOperation(operation,revision(p))==RackCandidateStatus::ready;CHECK(ready);return ready;
}
bool prepareManualAi(InstrumentProcessor& p,const juce::Uuid& rack,const juce::File& file)
{
    DrumPadTarget target;DrumCommitError error;const bool captured=p.captureDrumPadTarget(rack,0,0,target,error);CHECK(captured);if(!captured)return false;
    const bool opened=waitManualAi([&]{return p.drumImport().open(target);});CHECK(opened);if(!opened)return false;p.drumImport().choose(file);
    const bool inspected=waitManualAi([&]{return p.drumImport().view().inspected;});CHECK(inspected);if(!inspected)return false;
    p.drumImport().edit(0,48000,1,"Manual AI caller source");p.drumImport().prepare();const bool ready=waitManualAi([&]{return p.drumImport().view().phase==DrumImportPhase::ready;});CHECK(ready);return ready;
}
void checkManualAiSound(InstrumentProcessor& p,juce::AudioBuffer<float>& audio,juce::MidiBuffer& midi)
{
    CHECK(InstrumentDrumTestAccess::manualPlaying(p)&&!p.aiPreviewPlaying()&&p.aiPreviewStopped());CHECK(!p.applyAiWhenStopped());
    p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,audio.getNumSamples())>0);
#ifndef NDEBUG
    CHECK((InstrumentDrumTestAccess::previewBytes(p)==std::array<std::uint64_t,2>{48000*8,48000*8}));
#endif
}
void drum_local_capture_entrypoint()
{
    begin(__func__);
    auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;
    const auto before=project(p);CHECK(validateProjectDocument(before));
    auto melodic=airack::testing::fixtureValue([&]{return airack::testing::beginAiRequest(p,options(RequestScope::sound));});CHECK(melodic->has_value());
    if(*melodic){juce::MemoryBlock wire;CHECK((**melodic).catalog&&(**melodic).catalog->authority->rendering==CatalogRendering::ownedLocal);
        auto local=InstrumentAiTestAccess::currentLocal(p);CHECK(local&&local->request.tag==(**melodic).tag);if(local){
            CHECK(companion::encodeLocalRequest(*local,wire));auto decoded=std::make_unique<LocalGenerationRequest>();
            CHECK(companion::decodeLocalRequest({static_cast<const std::byte*>(wire.getData()),wire.getSize()},*decoded));CHECK(decoded->local==local->local&&decoded->request.source==before);
            CHECK(!companion::encodeRequest(**melodic,wire));}}
    p.discardAiProposal();CHECK(project(p)==before);
    RackOperation operation;operation.kind=RackOperationKind::place;operation.newInstance=juce::Uuid();operation.design=airack::test::frozenTileArrayRackDesign();
    CHECK(validateRackDesign(*operation.design,RackValidationMode::portableDesign));
    const auto placed=p.applyRackOperation(operation,revision(p));CHECK(placed==RackCandidateStatus::ready);
    if(placed!=RackCandidateStatus::ready)return;
    const auto source=project(p);CHECK(sourceModule(source.sound,0)==NativeModule::drum);
    CHECK(validateProjectDocument(source));
    auto request=airack::testing::fixtureValue([&]{return airack::testing::beginAiRequest(p,options(RequestScope::sound));});
    CHECK(request->has_value());
    if(*request){CHECK((**request).source==source);CHECK(p.aiProposalPhase()==AiProposalPhase::pending);
        auto local=InstrumentAiTestAccess::currentLocal(p);CHECK(local&&local->request.tag==(**request).tag);if(local){juce::MemoryBlock wire;CHECK(companion::encodeLocalRequest(*local,wire));
            CHECK(local->local.sourceMap.banks[0].rack==operation.newInstance);CHECK(local->local.sourceMap.assets.empty());}}
    p.discardAiProposal();CHECK(project(p)==source);
}
void manual_preview_survives_real_ai_callers_and_handoffs()
{
    begin(__func__);companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    auto storage=std::make_unique<InstrumentProcessor>(offline, airack::testing::isolatedRackRuntime());auto& p=*storage;const auto file=manualAiSource();const auto rack=juce::Uuid();
    auto pending=airack::testing::fixtureValue([&]{return airack::testing::beginAiRequest(p,options(RequestScope::sound));});CHECK(*pending);
    if(manualAiRack(p,rack,NativeModule::drum,false)){
        p.refreshAiProposal();CHECK(p.aiProposalPhase()==AiProposalPhase::stale);
        p.prepareToPlay(48000,512);
        if(prepareManualAi(p,rack,file)){
            p.drumImport().audition();CHECK(waitManualAi([&]{return InstrumentDrumTestAccess::manualPlaying(p);}));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;
            const auto manualRevision=revision(p);checkManualAiSound(p,audio,midi);
            // Prepared manual input owns this session, so a new AI request is busy.
            CHECK(!p.beginAiRequest(options(RequestScope::sound)));
            if(*pending){auto late=std::make_unique<GenerationResult>();late->tag=(**pending).tag;late->status=GenerationStatus::providerFailure;CHECK(!p.receiveAiResult(std::move(*late)));CHECK(p.aiProposalPhase()==AiProposalPhase::stale);checkManualAiSound(p,audio,midi);}
            p.refreshAiProposal();checkManualAiSound(p,audio,midi);
            for(unsigned cause=0;cause<6;++cause){
                if(cause==0)p.stopAiPreview();if(cause==1)p.cancelAi();if(cause==2)p.discardAiProposal();if(cause==3)p.closeAiSession();if(cause==4)p.closeAiPanel();if(cause==5)CHECK(p.setAiMusicalContext({"manual drums","D","minor",117}));
                CHECK(revision(p)==manualRevision);checkManualAiSound(p,audio,midi);
            }
            CHECK(p.requestAiAccount({companion::AccountAction::status,{}}));
            CHECK(waitManualAi([&]{p.pollAi();return p.aiWorkflowStatus().connection.phase==companion::ClientPhase::failed;}));
            CHECK(p.aiWorkflowStatus().message.contains("Companion unavailable"));CHECK(revision(p)==manualRevision);checkManualAiSound(p,audio,midi);p.closeAiSession();checkManualAiSound(p,audio,midi);
            const auto master=index("global.master_level");CHECK(p.beginParameterGesture(master));CHECK(p.setParameterGestureValue(master,.31f));CHECK(p.endParameterGesture(master));
            for(unsigned n=0;n<2;++n)p.processBlock(audio,midi);CHECK(waitManualAi([&]{return !InstrumentDrumTestAccess::manualPlaying(p);}));
#ifndef NDEBUG
            CHECK(waitManualAi([&]{return InstrumentDrumTestAccess::previewBytes(p)==std::array<std::uint64_t,2>{0,0};}));
#endif
            p.drumImport().cancel();CHECK(waitManualAi([&]{return p.drumImport().view().phase==DrumImportPhase::closed;}));
            if(manualAiRack(p,rack,NativeModule::analog,true)){
                p.prepareToPlay(48000,512);auto edits=std::make_unique<ProposalEdits>();edits->parameters={{"global.master_level",.4f}};
                if(propose(p,*edits,options(RequestScope::sound))){
                    CHECK(p.startAiPreview());p.processBlock(audio,midi);CHECK(p.aiPreviewPlaying()&&audio.getMagnitude(0,512)>0);p.stopAiPreview();for(unsigned n=0;n<2;++n)p.processBlock(audio,midi);CHECK(p.aiPreviewStopped());
                    CHECK(p.startAiPreview());p.processBlock(audio,midi);p.cancelAi();for(unsigned n=0;n<2;++n)p.processBlock(audio,midi);CHECK(p.aiPreviewStopped()&&!p.aiPreviewPlaying());
                }
                if(propose(p,*edits,options(RequestScope::sound))){
                    CHECK(p.startAiPreview());p.processBlock(audio,midi);CHECK(p.aiPreviewPlaying());
                    if(manualAiRack(p,rack,NativeModule::drum,true)){
                        DrumPadTarget next;DrumCommitError nextError;CHECK(p.captureDrumPadTarget(rack,0,0,next,nextError));
                        CHECK(p.aiProposalPhase()==AiProposalPhase::stale);CHECK(!p.drumImport().open(next));
                        p.refreshAiProposal();for(unsigned n=0;n<2;++n)p.processBlock(audio,midi);
                        if(prepareManualAi(p,rack,file)){
                        CHECK(p.aiPreviewStopped()&&!p.aiPreviewPlaying());
                        p.drumImport().audition();CHECK(waitManualAi([&]{return InstrumentDrumTestAccess::manualPlaying(p);}));checkManualAiSound(p,audio,midi);
                        p.drumImport().stopPreview();for(unsigned n=0;n<2;++n)p.processBlock(audio,midi);
                        CHECK(waitManualAi([&]{p.drumImport().poll();return p.drumImport().view().phase==DrumImportPhase::ready&&p.drumImport().view().canAudition&&!InstrumentDrumTestAccess::manualPlaying(p);}));
                        p.drumImport().audition();CHECK(waitManualAi([&]{return InstrumentDrumTestAccess::manualPlaying(p);}));checkManualAiSound(p,audio,midi);
                        p.prepareToPlay(48000,512);CHECK(!InstrumentDrumTestAccess::manualPlaying(p));
#ifndef NDEBUG
                        CHECK((InstrumentDrumTestAccess::previewBytes(p)==std::array<std::uint64_t,2>{0,0}));
#endif
                        p.drumImport().audition();CHECK(waitManualAi([&]{return InstrumentDrumTestAccess::manualPlaying(p);}));checkManualAiSound(p,audio,midi);p.releaseResources();CHECK(!InstrumentDrumTestAccess::manualPlaying(p));
#ifndef NDEBUG
                        CHECK((InstrumentDrumTestAccess::previewBytes(p)==std::array<std::uint64_t,2>{0,0}));
#endif
                        }
                    }
                }
            }
        }
    }
    p.drumImport().cancel();CHECK(waitManualAi([&]{return p.drumImport().view().phase==DrumImportPhase::closed;}));p.releaseResources();storage.reset();CHECK(file.deleteFile());
}
}
static void installed_inventory_authority()
{
    begin("installed_inventory_authority");auto runtime=testing::isolatedRackRuntime();
    std::vector<RackPackageRef> refs;std::vector<RackDesign> designs;std::vector<RackPackageManifest> manifests;
    for(int n=0;n<96;++n){
        auto design=airack::test::frozenFactoryRackDesigns().front();design.id=juce::Uuid();design.revision=std::uint64_t(n)+100;design.name="Installed "+juce::String(n);
        juce::MemoryBlock recipe;CHECK(encodeRackDesign(design,recipe,RackValidationMode::instanceRecall));auto manifest=test::packageFixture("installed-"+juce::String(n));
        manifest.metadata={design.id,design.revision,juce::SHA256(recipe).toHexString(),design.name,"Fixture","Native installed recipe",{},design.role,{"analog"}};
        const auto png=test::packageFixturePng();manifest.files={{"rack.json",manifest.metadata.canonicalSha256,"design",recipe.getSize()},{"screenshot.png",juce::SHA256(png).toHexString(),"png",png.getSize()}};
        manifest.capabilities=juce::JSON::parse("{\"airMin\":\"0.1.0\",\"modules\":[{\"module\":0,\"algorithmRevision\":"+juce::String(int(design.units[0].algorithmRevision))+"}],\"nativeEditor\":\"\"}");
        const auto archives=runtime->root.getChildFile("archives");CHECK(archives.createDirectory().wasOk());const auto file=archives.getChildFile(manifest.ref.id+".zip");
        juce::ZipFile::Builder zip;const auto json=test::packageFixtureJson(manifest);const juce::Time stamp(1980,0,1,0,0);
        zip.addEntry(new juce::MemoryInputStream(json.toRawUTF8(),json.getNumBytesAsUTF8(),true),0,"package.json",stamp);
        zip.addEntry(new juce::MemoryInputStream(recipe,true),0,"rack.json",stamp);zip.addEntry(new juce::MemoryInputStream(png,true),0,"screenshot.png",stamp);
        {auto out=file.createOutputStream();CHECK(out);if(out)CHECK(zip.writeToStream(*out,nullptr));}
        auto ref=manifest.ref;juce::FileInputStream input(file);ref.sha256=juce::SHA256(input).toHexString();
        const auto installed=runtime->store->install(file,ref,RackPackageActivation::makeActive,{});if(installed!=RackLibraryStatus::ready)std::cerr<<"INSTALL_STATUS "<<n<<" "<<int(installed)<<"\n";CHECK(installed==RackLibraryStatus::ready);refs.push_back(ref);designs.push_back(std::move(design));manifests.push_back(std::move(manifest));
    }
    InstrumentProcessor processor(runtime);setup(processor);RackInventoryPtr root;CHECK(testing::readyRackInventory(processor,root,120000));
    CHECK(*runtime->gets==0);if(!root)return;CHECK(root->rows().size()==113&&root->rows().size()>maximumRackCatalogEntries);
    juce::var descriptor;CHECK(catalogInventoryFor(*root,CatalogRendering::ownedLocal,descriptor));CHECK(int(descriptor["format"])==2&&descriptor["entries"].size()==113);
    juce::var provider;CHECK(providerCatalogFor(project(processor),*root,CatalogRendering::companionPcm,provider));CHECK(provider["rack_catalog"]["entries"].size()==113);
    const auto original=project(processor);
    const auto prepare=[&](std::size_t which){
        auto captured=testing::beginAiRequest(processor,options(RequestScope::sound),120);CHECK(captured&&captured->catalog);if(!captured||!captured->catalog)return false;
        ProposalEdits edits;edits.catalogSelections={{7,designs[which].id,designs[which].revision,manifests[which].metadata.canonicalSha256,{RackRole::synth,masterChain,1}}};edits.catalogParameters={{7,0,"analog.filter.cutoff",.37f}};
        unsigned calls=0;auto selected=catalogWitnessResult(*captured,edits,calls);CHECK(selected.status==GenerationStatus::catalogSelectionRequired&&calls==1);
        CHECK(processor.receiveAiResult(std::move(selected)));const auto* pending=processor.aiCurrentRequest();CHECK(pending&&pending->catalog&&pending->catalog->authority->definitions.size()==1);if(!pending||!pending->catalog)return false;
        CHECK(pending->catalog->authority->definitions[0].contentSha256==manifests[which].metadata.canonicalSha256);
        auto ready=generateProposal(*pending,[&](const auto&,auto,const auto&){++calls;return rack::oauth::StructuredResult{};});CHECK(ready.status==GenerationStatus::ready&&calls==1&&ready.inferenceCalls==1);
        return processor.receiveAiResult(std::move(ready));
    };
    CHECK(prepare(95));CHECK(project(processor)==original);
    // An update retains the frozen exact old artwork release, even with identical design bytes.
    auto update=manifests.back();update.ref.version="2.0.0";const auto updateDir=runtime->root.getChildFile("update");CHECK(updateDir.createDirectory().wasOk());const auto updateFile=updateDir.getChildFile("update.zip");
    juce::MemoryBlock recipe;CHECK(encodeRackDesign(designs.back(),recipe,RackValidationMode::instanceRecall));
    {juce::ZipFile::Builder zip;const auto json=test::packageFixtureJson(update);const auto png=test::packageFixturePng();const juce::Time stamp(1980,0,1,0,0);
        zip.addEntry(new juce::MemoryInputStream(json.toRawUTF8(),json.getNumBytesAsUTF8(),true),0,"package.json",stamp);zip.addEntry(new juce::MemoryInputStream(recipe,true),0,"rack.json",stamp);zip.addEntry(new juce::MemoryInputStream(png,true),0,"screenshot.png",stamp);
        auto out=updateFile.createOutputStream();CHECK(out);if(out)CHECK(zip.writeToStream(*out,nullptr));}
    auto updatedRef=update.ref;{juce::FileInputStream in(updateFile);updatedRef.sha256=juce::SHA256(in).toHexString();}
    CHECK(runtime->store->install(updateFile,updatedRef,RackPackageActivation::makeActive,{})==RackLibraryStatus::ready);
    RackInventoryPtr updatedInventory;CHECK(testing::readyRackInventory(processor,updatedInventory,120000));
    CHECK(updatedInventory&&updatedInventory->storeGeneration()==97&&updatedInventory->storeGeneration()==runtime->store->observedGeneration());
    RackDesign frozenDesign;std::optional<RackPackageRef> frozenRef;CHECK(resolveInventoryDesign(*root,designs.back().id,designs.back().revision,manifests.back().metadata.canonicalSha256,frozenDesign,frozenRef));CHECK(frozenDesign==designs.back()&&frozenRef==refs.back());
    std::cout<<"INSTALLED_ROOT rows="<<root->rows().size()<<" id="<<root->id().toString()<<" revision="<<designs.back().revision<<" canonical="<<manifests.back().metadata.canonicalSha256<<" package="<<refs.back().sha256<<" update="<<updatedRef.sha256<<'\n';
    CHECK(!processor.applyAiProposal());CHECK(project(processor)==original);
    const bool applied=waitManualAi([&]{return processor.applyAiProposal();});
    if(!applied){const auto view=InstrumentAiTestAccess::placement(processor);std::cerr<<"INSTALLED_APPLY phase="<<int(processor.aiProposalPhase())<<" message="<<processor.aiWorkflowStatus().message<<" observed="<<runtime->store->observedGeneration();if(view)std::cerr<<" status="<<int(view->status)<<" leases="<<view->leases.size()<<" generation="<<view->storeGeneration<<" initial_root="<<(view->root==root);std::cerr<<std::endl;}
    CHECK(applied);const auto placed=project(processor);
    CHECK(placed.sound.personalities&&placed.sound.personalities->instances.size()==1);if(!placed.sound.personalities)return;
    CHECK(placed.sound.personalities->instances.front().package==refs.back());CHECK(processor.undoSoundEdit());CHECK(project(processor)==original);
    CHECK(prepare(94));const auto beforeRemoval=runtime->store->observedGeneration();CHECK(runtime->store->remove(refs[94],{})==RackLibraryStatus::ready);CHECK(!processor.applyAiProposal());
    // The client serializes placement with inventory refresh. Wait for the
    // removal generation to be observed before checking its placement result.
    RackInventoryPtr afterRemoval;CHECK(testing::readyRackInventory(processor,afterRemoval,120000));
    CHECK(afterRemoval&&afterRemoval->storeGeneration()==beforeRemoval+1&&afterRemoval->storeGeneration()==runtime->store->observedGeneration());
    const bool unavailable=waitManualAi([&]{const auto view=InstrumentAiTestAccess::placement(processor);return view&&view->status==RackLibraryStatus::unavailable;});
    if(!unavailable){const auto view=InstrumentAiTestAccess::placement(processor);std::cerr<<"REMOVED_PLACEMENT observed="<<runtime->store->observedGeneration();if(view)std::cerr<<" status="<<int(view->status)<<" generation="<<view->storeGeneration;std::cerr<<std::endl;}
    CHECK(unavailable);CHECK(!processor.applyAiProposal());CHECK(project(processor)==original);
    processor.control(0).setValue(.39f);const auto edited=project(processor);CHECK(!processor.applyAiProposal());CHECK(project(processor)==edited);processor.discardAiProposal();
    CHECK(recall(processor,placed));CHECK(waitManualAi([&]{return runtime->store->remove(refs.back(),{})==RackLibraryStatus::ready;}));CHECK(runtime->store->remove(updatedRef,{})==RackLibraryStatus::ready);
    RackInventoryPtr embedded;CHECK(testing::readyRackInventory(processor,embedded));CHECK(prepare(95));CHECK(processor.applyAiProposal());const auto duplicated=project(processor);
    CHECK(duplicated.sound.personalities&&duplicated.sound.personalities->instances.size()==2);if(duplicated.sound.personalities)for(const auto& rack:duplicated.sound.personalities->instances)CHECK(rack.package==refs.back());
    CHECK(*runtime->gets==0&&runtime->client->view()->job==0);
}

int main(int argc, char** argv)
{
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::cout.setf(std::ios::unitbuf); processIdentity(argc, argv);
    juce::ScopedJuceInitialiser_GUI gui;
    if(argc==2&&juce::String(argv[1])=="--installed-inventory"){installed_inventory_authority();std::cout<<checks<<" checks, "<<groups<<" groups, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--drum-local-capture"){
        drum_local_capture_entrypoint();
        std::cout<<checks<<" checks, "<<groups<<" groups, "<<failures<<" failures\n";return failures?1:0;
    }
    if(argc==2&&juce::String(argv[1])=="--manual-preview"){
        manual_preview_survives_real_ai_callers_and_handoffs();
        std::cout<<checks<<" checks, "<<groups<<" groups, "<<failures<<" failures\n";return failures?1:0;
    }
#ifndef NDEBUG
    if (argc == 2 && juce::String(argv[1]) == "--owned-ai") {
        owned_ai_entrypoint_contracts();
        std::cout << checks << " checks, " << groups << " groups, " << failures << " failures\n";
        return failures ? 1 : 0;
    }
#endif
    if (argc == 2 && juce::String(argv[1]) == "--catalog-setup-only") {
        catalog_request_inventory(); catalog_native_materialize_apply_undo();
        std::cout << checks << " checks, " << groups << " groups, " << failures << " failures\n";
        return failures ? 1 : 0;
    }
#ifndef NDEBUG
    owned_ai_entrypoint_contracts();
#endif
    generation_failure_diagnostics();
    if(argc>1&&juce::String(argv[1])=="--diagnostics-only"){std::cout<<checks<<" checks, "<<groups<<" groups, "<<failures<<" failures\n";return failures?1:0;}
    installed_inventory_authority();catalog_request_inventory();
    catalog_native_materialize_apply_undo();
    catalog_pending_materialization_retirement();
    catalog_refined_aggregate_transaction();
    capture_context_authority(); result_failure_and_invalidation(); sparse_apply_undo_automation(); reentrant_notifications_and_noop();
    required_pattern_result_injection(); required_parent_optional_refinement(); apply_names_the_sound();
    manual_preview_survives_real_ai_callers_and_handoffs();
    std::cout << checks << " checks, " << groups << " groups, " << failures << " failures\n";
    return failures ? 1 : 0;
}

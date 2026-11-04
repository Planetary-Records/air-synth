#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "InstrumentProcessor.h"
#include "AiUnit.h"
#include "RackExamples.h"
#include "CompanionWire.h"
#include "ScreenDriver.h"
#include "StudioSkin.h"
#include "PostedClick.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <iostream>
#include <stdexcept>
#include <windows.h>

namespace airack {
struct InstrumentAiTestAccess {
    static std::optional<ai::GenerationRequest> beginHistorical25(InstrumentProcessor& p,const AiRequestOptions& options,std::optional<double> bpm)
    try {
        p.refreshAiProposal();auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign().id;
        std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
        std::erase_if(factory,[&](const auto& design){return design.id==tile;});
        if(factory.size()!=25)throw std::runtime_error("historical25 library fixture");
        auto inventory=std::make_shared<RackCatalogSnapshot>();
        {const std::lock_guard lock(p.restoreMutex);auto source=std::make_unique<ProjectDocument>();std::uint64_t edit;
            if(!p.tryProjectSnapshotOwned(*source,edit))return {};
            std::vector<RackDesign> embedded;if(source->sound.personalities)for(const auto& instance:source->sound.personalities->instances)embedded.push_back(instance.design);
            if(!captureRackCatalog(factory,embedded,{},*inventory))return {};}
        return p.beginAiRequestWithCatalog(options,bpm,std::move(inventory));
    }catch(const std::bad_alloc&){return {};}
};
}

// Labelled historical25 admitted/rendered native results through production request construction
// APIs and the real read-only ai.diff. No service, account or helper is started.
namespace
{
using namespace airack;
using namespace airack::ai;
int checks=0,failures=0,groups=0;
const char* group="";
void check(bool value,const char* expression,int line)
{++checks;if(!value){++failures;std::cout<<"FAIL "<<group<<':'<<line<<' '<<expression<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){group=name;++groups;std::cout<<"GROUP "<<name<<std::endl;}
juce::Component* find(juce::Component& root,const juce::String& id)
{if(root.getComponentID()==id)return &root;for(auto* child:root.getChildren())if(auto* result=find(*child,id))return result;return nullptr;}
void click(juce::Button& button){CHECK(clickPosted(button));juce::MessageManager::getInstance()->runDispatchLoopUntil(2);}
ProjectDocument project(InstrumentProcessor& processor)
{ProjectDocument result;std::uint64_t revision;CHECK(processor.tryProjectSnapshot(result,revision));return result;}
ProjectDocument initial()
{ProjectDocument result;result.pattern.meter={1,4};result.pattern.noteCount=1;result.pattern.notes[0]={0,.05,60,80,1};return result;}
void applyRack(ProjectDocument& source,const RackEdit& edit)
{RackLayout next;CHECK(editRackLayout(source.sound.layout,edit,next)==RackEditResult::applied);source.sound.layout=next;}
// The user's rack over laid-out banks: an optional synth bank, then its or the FX rack's effect banks in chain order.
juce::Uuid adopt(ProjectDocument& source,std::optional<std::uint8_t> synth,std::vector<std::uint8_t> effects)
{
    RackDesign design;design.id=juce::Uuid();design.name="User rack";design.role=synth?RackRole::synth:RackRole::fx;design.panel.sections={"Native"};
    RackOperation op;op.kind=RackOperationKind::adopt;op.newInstance=juce::Uuid();op.destination.role=design.role;std::uint8_t unit=0;
    if(synth){design.units.push_back({unit,NativeModule::analog,{}});op.allocation.push_back({unit++,*synth});}
    for(const auto slot:effects){design.units.push_back({unit,NativeModule(unsigned(source.sound.layout.effects[slot].kind)+2),{}});op.allocation.push_back({unit++,slot});}
    op.design=design;SoundDocument next;RackDifference delta;CHECK(makeRackCandidate(source.sound,op,SoundEditOrigin::manual,next,delta)==RackCandidateStatus::ready);
    source.sound=std::move(next);return op.newInstance;
}
ProposalEdits proposed(const GenerationRequest& request,const std::vector<RackEdit>& racks,const std::vector<juce::Uuid>& remove)
{
    ProposalEdits edits;if(request.catalog)edits=request.catalog->baseIntent;edits.racks.insert(edits.racks.end(),racks.begin(),racks.end());
    for(const auto& id:remove)if(std::find(edits.reconciliation.remove.begin(),edits.reconciliation.remove.end(),id)==edits.reconciliation.remove.end())edits.reconciliation.remove.push_back(id);
    edits.characteristics="Synthetic chronological rack regression";edits.explanation="Native admission/render; no provider request";return edits;
}
GenerationResult syntheticResult(const GenerationRequest& request,const std::vector<RackEdit>& racks,const std::vector<juce::Uuid>& remove)
{
    GenerationResult result;result.tag=request.tag;result.inferenceCalls=1;result.providerStatus=rack::oauth::Status::connected;result.httpStatus=200;
    if(request.catalog)result.catalog=CatalogResultBinding{request.catalog->root,request.catalog->authority->snapshot};result.edits=proposed(request,racks,remove);
    ProjectDocument candidate;CHECK(admitProposal(request.source,request.policy,result.edits,candidate)==ProposalStatus::accepted);result.candidate=candidate;
    AuditionContext context;context.bpm=request.capturedBpm.value_or(120);context.tempoSource=request.capturedBpm?TempoSource::captured:TempoSource::defaultTempo;
    if(!request.capturedPhrase)throw std::runtime_error("fixture phrase missing");context.phrase=*request.capturedPhrase;context.phraseSource=PhraseSource::captured;
    auto rendered=renderPreview(candidate,context);CHECK(rendered.status==RenderStatus::ready);if(rendered.status!=RenderStatus::ready)throw std::runtime_error("native render failed");
    result.audio=std::move(rendered.audio);result.measurements=rendered.measurements;result.status=GenerationStatus::ready;result.proposalStatus=ProposalStatus::accepted;result.renderStatus=RenderStatus::ready;
    return result;
}
struct Fixture
{
    std::unique_ptr<InstrumentProcessor> processor=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());
    ProjectDocument committed;
    FakeServices files;
    std::unique_ptr<studio::AiUnit> unit;
    explicit Fixture(const ProjectDocument& source):committed(source)
    {
        juce::MemoryBlock state;CHECK(encodeProjectState(source,state));CHECK(processor->restoreProjectState(state.getData(),int(state.getSize())));
        CHECK(airack::testing::readyRackInventory(*processor));
        unit=std::make_unique<studio::AiUnit>(*processor,*studio::studioSkin(),files.make());
        CHECK(processor->aiWorkflowStatus().connection.phase==companion::ClientPhase::closed);CHECK(processor->aiAccountReply()==nullptr);CHECK(project(*processor)==committed);
    }
    std::optional<GenerationRequest> start(bool refine)
    {
        return refine?processor->beginAiRefinement("Synthetic explicit rack refinement","synthetic-rack-diff")
            :InstrumentAiTestAccess::beginHistorical25(*processor,{"Synthetic explicit rack proposal","synthetic-rack-diff",{RequestScope::sound,false,true}},120);
    }
    bool propose(const std::vector<RackEdit>& racks,bool refine=false,const std::vector<juce::Uuid>& remove={})
    {
        auto request=start(refine);
        CHECK(request.has_value());if(!request)return false;
        auto result=syntheticResult(*request,racks,remove);const auto candidate=*result.candidate;
        juce::MemoryBlock requestBefore,resultBefore;CHECK(companion::encodeRequest(*request,requestBefore));CHECK(companion::encodeResult(*request,result,resultBefore));
        CHECK(processor->receiveAiResult(std::move(result)));const auto* actual=processor->aiProposal();CHECK(actual!=nullptr);if(!actual)return false;
        CHECK(actual->candidate&&*actual->candidate==candidate);juce::MemoryBlock requestAfter,resultAfter;
        CHECK(companion::encodeRequest(*processor->aiCurrentRequest(),requestAfter));CHECK(requestBefore==requestAfter);
        CHECK(companion::encodeResult(*request,*actual,resultAfter));CHECK(resultBefore==resultAfter);
        CHECK(project(*processor)==committed);return true;
    }
    // The AI's rack edit the user couldn't make: the panel's request refuses it as outside reach. Ends the fixture's
    // request chain.
    ProposalStatus refusal(const std::vector<RackEdit>& racks,bool refine=false)
    {
        auto request=start(refine);CHECK(request.has_value());if(!request)return ProposalStatus::malformed;
        ProjectDocument candidate;const auto status=admitProposal(request->source,request->policy,proposed(*request,racks,{}),candidate);
        processor->cancelAi();CHECK(project(*processor)==committed);return status;
    }
    juce::String difference()
    {
        // The actual unit timer publishes text; no formatter/private API is called.
        juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
        auto* text=dynamic_cast<juce::TextEditor*>(find(*unit,"ai.diff"));CHECK(text!=nullptr);if(!text)return {};
        CHECK(text->isReadOnly());CHECK(processor->aiWorkflowStatus().connection.phase==companion::ClientPhase::closed);
        CHECK(processor->aiAccountReply()==nullptr);CHECK(project(*processor)==committed);return text->getText();
    }
};
void expect(const juce::String& text,const char* prefix,std::initializer_list<const char*> details)
{
    const auto lines=juce::StringArray::fromLines(text);juce::String selected;
    for(const auto& line:lines)if(line.startsWith(prefix)){selected=line;break;}
    CHECK(selected.isNotEmpty());if(selected.isEmpty()){std::cout<<"MISSING "<<prefix<<std::endl;return;}
    for(const auto* detail:details){const bool present=selected.contains(detail);CHECK(present);if(!present)std::cout<<"EXPECTED "<<detail<<" IN "<<selected<<std::endl;}
}
void chronologicalRefinement()
{
    begin("chronological_refinement");auto source=initial();
    // The user's racks: Drive, Reverb and Delay on the master, and a Compressor on synth 1's inserts.
    applyRack(source,{RackEditKind::addEffect,0,masterChain,0,EffectKind::drive});applyRack(source,{RackEditKind::addEffect,1,masterChain,1,EffectKind::reverb});
    applyRack(source,{RackEditKind::addEffect,3,masterChain,2,EffectKind::delay});applyRack(source,{RackEditKind::addEffect,2,0,0,EffectKind::compressor});
    adopt(source,{},{0});const auto reverb=adopt(source,{},{1});adopt(source,{},{3});adopt(source,{},{2});
    Fixture fixture(source);
    if(!fixture.propose({{RackEditKind::removeEffect,1},{RackEditKind::moveEffect,0,0,1}},false,{reverb}))return;
    auto text=fixture.difference();
    expect(text,"Remove effect bank 2",{"kind Reverb -> Reverb","state present -> retired","owner Master -> none","position 2 -> none"});
    expect(text,"Move effect bank 1",{"kind Drive -> Drive","owner Master -> Synth 1 insert","position 1 -> 2"});
    CHECK(text.contains("Physical bank reuse warning"));
    if(!fixture.propose({{RackEditKind::moveEffect,0,masterChain,1}},true))return;
    text=fixture.difference();
    const char* out="Move effect bank 1; kind Drive -> Drive; state present -> present; owner Master";
    const char* back="Move effect bank 1; kind Drive -> Drive; state present -> present; owner Synth 1 insert";
    expect(text,back,{"owner Synth 1 insert -> Master","position 2 -> 2"});
    expect(text,"Remove effect bank 2",{"kind Reverb -> Reverb","owner Master -> none","position 2 -> none"});
    CHECK(text.indexOf("Remove effect bank 2")<text.indexOf(out));
    CHECK(text.indexOf(out)<text.indexOf(back));
    // A placed rack's effect changes by swapping the rack; replacing its bank's kind is refused.
    CHECK(fixture.refusal({{RackEditKind::replaceEffect,0,masterChain,0,EffectKind::delay}},true)==ProposalStatus::outOfReach);
}
// A bank now comes with a library rack: the AI adds none itself, chosen or reused.
void selectedAndReusedBanks()
{
    begin("selected_and_reused_banks");auto source=initial();
    applyRack(source,{RackEditKind::addEffect,0,masterChain,0,EffectKind::reverb});applyRack(source,{RackEditKind::removeEffect,0});
    Fixture fixture(source);
    CHECK(fixture.refusal({{RackEditKind::addEffect,noSlot,masterChain,0,EffectKind::compressor}})==ProposalStatus::outOfReach);
    CHECK(fixture.refusal({{RackEditKind::addEffect,0,0,0,EffectKind::delay}})==ProposalStatus::outOfReach);
    CHECK(fixture.refusal({{RackEditKind::addSynth,noSlot,masterChain,1}})==ProposalStatus::outOfReach);
}
void synthOrderAndInsertRetirement()
{
    begin("synth_order_and_insert_retirement");auto source=initial();
    // The user's three synth racks, the third owning a Reverb and a Delay insert.
    applyRack(source,{RackEditKind::addSynth,1,masterChain,1});applyRack(source,{RackEditKind::addSynth,2,masterChain,2});
    applyRack(source,{RackEditKind::addEffect,0,2,0,EffectKind::reverb});applyRack(source,{RackEditKind::addEffect,1,2,1,EffectKind::delay});
    adopt(source,0,{});adopt(source,1,{});const auto third=adopt(source,2,{0,1});
    Fixture fixture(source);
    if(!fixture.propose({{RackEditKind::moveSynth,1,masterChain,0},{RackEditKind::removeSynth,2}},false,{third}))return;
    auto text=fixture.difference();
    expect(text,"Move synth bank 2",{"state present -> present","position 2 -> 1"});
    expect(text,"Remove synth bank 3",{"state present -> retired","position 3 -> none"});
    expect(text,"Retire effect bank 1 after Remove synth bank 3",{"kind Reverb -> Reverb","state present -> retired","owner Synth 3 insert -> none","position 1 -> none"});
    expect(text,"Retire effect bank 2 after Remove synth bank 3",{"kind Delay -> Delay","state present -> retired","owner Synth 3 insert -> none","position 2 -> none"});
    const auto& candidate=fixture.processor->aiProposal()->candidate->sound;
    CHECK(candidate.layout.effects[0].use==SlotUse::retired&&candidate.layout.effects[1].use==SlotUse::retired);
    CHECK(candidate.personalities&&candidate.personalities->instances.size()==2);
    // A synth bank comes with a library rack: adding one, reused or chosen, is refused.
    CHECK(fixture.refusal({{RackEditKind::addSynth,2,masterChain,1}},true)==ProposalStatus::outOfReach);
    CHECK(fixture.refusal({{RackEditKind::addSynth,noSlot,masterChain,1}})==ProposalStatus::outOfReach);
}
// The studio support spec's names in the AI difference: the user's EQ and mid/side split racks moved name the newer kinds.
void newerKindNames()
{
    begin("newer_kind_names");auto source=initial();
    applyRack(source,{RackEditKind::addEffect,0,masterChain,0,EffectKind::eq});applyRack(source,{RackEditKind::addEffect,1,masterChain,1,EffectKind::midSideSplit});
    adopt(source,{},{0});adopt(source,{},{1});
    Fixture fixture(source);
    if(!fixture.propose({{RackEditKind::moveEffect,0,0,0},{RackEditKind::moveEffect,1,masterChain,0}}))return;
    const auto moved=fixture.difference();
    expect(moved,"Move effect bank 1",{"kind EQ -> EQ","state present -> present","owner Master -> Synth 1 insert"});
    expect(moved,"Move effect bank 2",{"kind Mid/side split -> Mid/side split","state present -> present","owner Master -> Master"});
}
void postedClickBehindSlowMessage()
{
    begin(__func__);
    CHECK(clickRunsBehindSlowMessage([](juce::Button& button){click(button);}));
}
}
int main(int argc,char** argv)
{
    FILETIME created,exited,kernel,user;GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user);
    const auto stamp=(static_cast<unsigned long long>(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    std::cout<<"Labelled synthetic admitted/rendered AiPanel rack diff; PID="<<GetCurrentProcessId()<<" FILETIME="<<stamp<<" CWD="<<juce::File::getCurrentWorkingDirectory().getFullPathName()<<std::endl;
    for(int n=0;n<argc;++n)std::cout<<"ARGV["<<n<<"]="<<argv[n]<<std::endl;
    juce::ScopedJuceInitialiser_GUI gui;
    try{chronologicalRefinement();selectedAndReusedBanks();synthOrderAndInsertRetirement();newerKindNames();postedClickBehindSlowMessage();}
    catch(const std::exception& error){std::cout<<"SETUP_EXCEPTION "<<error.what()<<std::endl;return 2;}
    std::cout<<"AiPanelRackDiff checks="<<checks<<" groups="<<groups<<" failures="<<failures<<std::endl;
    return failures?1:0;
}


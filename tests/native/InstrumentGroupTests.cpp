#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "RackExamples.h"
#include "InstrumentProcessor.h"
#include "DrumImport.h"
#include "CompanionWire.h"
#include "PatternIO.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <functional>
#include <iostream>
#include <limits>
#include <thread>
#include <crtdbg.h>

namespace airack {
// Explicit historical melodic unit-fixture authority, sealed before normal request construction.
struct InstrumentAiTestAccess {
    static std::optional<ai::GenerationRequest> beginHistorical25(InstrumentProcessor& p,const AiRequestOptions& options,std::optional<double> bpm={})
    try {
        if(options.policy.scope==ai::RequestScope::pattern)return airack::testing::beginAiRequest(p,options,bpm);
        p.refreshAiProposal();auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
        std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
        if(factory.size()!=26||std::count(factory.begin(),factory.end(),tile)!=1)return {};
        std::erase(factory,tile);if(factory.size()!=25)return {};
        auto captured=std::make_shared<RackCatalogSnapshot>();
        {const std::lock_guard lock(p.restoreMutex);auto source=std::make_unique<ProjectDocument>();std::uint64_t edit=0;
            if(!p.tryProjectSnapshotOwned(*source,edit))return {};
            std::vector<RackDesign> embedded;if(source->sound.personalities)for(const auto& rack:source->sound.personalities->instances)embedded.push_back(rack.design);
            if(!captureRackCatalog(factory,embedded,{},*captured))return {};}
        return p.beginAiRequestWithCatalog(options,bpm,std::move(captured));
    }catch(const std::bad_alloc&){return {};}
};
}

namespace airack
{
struct InstrumentGroupTestAccess
{
    static bool startClient(InstrumentProcessor& p){return p.aiClient.start(p.aiClientOptions);}
    static void pollClient(InstrumentProcessor& p){p.aiClient.poll();}
    static companion::ClientStatus clientStatus(const InstrumentProcessor& p){return p.aiClient.status();}
    static void withhold(InstrumentProcessor& p,bool value){companion::detail::CompanionClientTestAccess::withholdCompletion(p.aiClient,value);}
    static bool started(const InstrumentProcessor& p){return companion::detail::CompanionClientTestAccess::hasStartedWrite(p.aiClient);}
    static bool reply(const InstrumentProcessor& p){return companion::detail::CompanionClientTestAccess::hasGroupReply(p.aiClient);}
    static std::uint64_t now(){const auto t=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();return t<0?0:std::uint64_t(t);}
    static bool submit(InstrumentProcessor& p,bool admit){return p.submitGroup(admit?InstrumentProcessor::GroupControl::Purpose::admit:InstrumentProcessor::GroupControl::Purpose::publish,now());}
    static void settle(InstrumentProcessor& p){
        // Keep the unrelated periodic heartbeat from submitting a second publication during this explicit ACK step.
        {const std::lock_guard lock(p.restoreMutex);p.groupControl->observationTime=now();}
        p.pollGroup();
    }
    static std::array<std::uint64_t,4> publicationState(const InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);
        return {p.aiGroupPublicationBytes,p.groupControl->nextOperation,std::uint64_t(bool(p.groupControl->pending)),std::uint64_t(p.groupControl->admitted)};}
    static bool manualPlaying(InstrumentProcessor& p) {return p.manualDrumPreviewPlaying();}
    static std::array<std::uint64_t,2> previewBytes(InstrumentProcessor& p)
    {const std::lock_guard lock(p.restoreMutex);return {p.manualPreviewBytes,p.renderedPreview.retainedPcmBytes()};}
    static void setLoadRevision(InstrumentProcessor& p,std::uint64_t v)
    {const std::lock_guard lock(p.restoreMutex);detail::GroupLinkStateTestAccess::setLoadRevision(p.groupLinkState,v);}
    static void setContextRevision(InstrumentProcessor& p,std::uint64_t v)
    {const std::lock_guard lock(p.restoreMutex);p.aiContextRevision=v;}
};
struct InstrumentDrumTestAccess
{
    static std::array<std::uint64_t,2> groupAccounting(const InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return {p.drumOwnership->retainedBytes(),p.drumOwnership->reservedDecoded_};}
    static bool reserve(InstrumentProcessor& p,std::uint64_t bytes){const std::lock_guard lock(p.restoreMutex);return p.drumOwnership->reserveDecoded(bytes);}
    static void release(InstrumentProcessor& p,std::uint64_t bytes){const std::lock_guard lock(p.restoreMutex);p.drumOwnership->releaseDecoded(bytes);}
};
}
namespace
{
using namespace airack;
int checks=0,failures=0,sections=0;const char* section="";
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;if(failures<100)std::cerr<<section<<':'<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){section=name;++sections;std::cout<<name<<'\n';}
GroupUuid id(std::uint8_t v){GroupUuid r;r.bytes[0]=v;r.bytes[15]=64;return r;}
GroupText text(std::string_view v){GroupText r;CHECK(setGroupText(v,r));return r;}
GroupIntent group(std::uint8_t v=1,bool maximum=false)
{GroupIntent r;r.group=id(v);r.member=id(v+20);r.name=text(maximum?std::string(128,'n'):"Session");r.role=text(maximum?std::string(128,'r'):"bass");
 r.key=text(maximum?std::string(128,'k'):"A");r.scale=text(maximum?std::string(128,'s'):"minor");r.bpm=120;CHECK(validateGroupIntent(r));return r;}
ProjectDocument rich(bool grouped=true)
{ProjectDocument d;d.sound.values[0]=.23f;d.sound.macros[0]={parameterRegistry()[0].stateKey,.15f};d.sound.controlLocks[757]=true;
 d.pattern.bars=2;d.pattern.noteCount=2;d.pattern.notes[0]={0,.25,48,90,1};d.pattern.notes[1]={.5,.25,55,80,2};d.mode=PlaybackMode::internalPattern;
 if(grouped)d.group=group();CHECK(validateProjectDocument(d));return d;}
ProjectDocument snapshot(InstrumentProcessor& p,std::uint64_t* edit=nullptr,std::uint64_t* reset=nullptr)
{ProjectDocument d;std::uint64_t e=0,r=0;CHECK(p.tryProjectSnapshot(d,e,r));if(edit)*edit=e;if(reset)*reset=r;return d;}
std::unique_ptr<ProjectDocument> snapshotOwned(InstrumentProcessor& p,std::uint64_t* edit=nullptr,std::uint64_t* reset=nullptr)
{return std::unique_ptr<ProjectDocument>(new ProjectDocument(snapshot(p,edit,reset)));}
bool recall(InstrumentProcessor& p,const ProjectDocument& d)
{juce::MemoryBlock b;CHECK(encodeProjectState(d,b));return p.restoreProjectState(b.getData(),int(b.getSize()));}
AiRequestOptions options(){return {"Local group authority fixture","synthetic-selected_model:1.0",{ai::RequestScope::sound,false,false}};}
std::optional<ai::GenerationRequest> request(InstrumentProcessor& p)
{auto r=InstrumentAiTestAccess::beginHistorical25(p,options(),111);CHECK(r.has_value());return r;}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{auto* o=new juce::DynamicObject;for(const auto& f:fields)o->setProperty(f.first,f.second);return o;}
ai::GenerationResult result(const ai::GenerationRequest& r,float gain=.55f)
{
    std::cout<<"GROUP_OBSERVE result-enter\n";
    juce::Array<juce::var> parameters;parameters.add(object({{"id","global.master_level"},{"normalized",double(gain)}}));
    const auto legacyJson=juce::JSON::toString(object({{"version",1},{"parameter_edits",parameters},{"rack_edits",juce::Array<juce::var>()},
        {"macro_edits",juce::Array<juce::var>()},{"pattern",juce::var()},{"characteristics","local fixture"},{"explanation","public native admission"}}),true);
    auto json=legacyJson;
    if(r.catalog){
        CHECK(r.catalog->authority);
        if(r.catalog->authority){
            ai::ProposalEdits rejected;ai::ProposalDecodeReason reason;
            CHECK(!ai::decodeProposal(legacyJson.toStdString(),r.policy,*r.catalog->authority,rejected,&reason)&&reason==ai::ProposalDecodeReason::rootFields);
            auto value=juce::JSON::parse(legacyJson);
            value.getDynamicObject()->setProperty("version",2);value.getDynamicObject()->setProperty("name","Local Test Sound");
            value.getDynamicObject()->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>()},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}}));
            value.getDynamicObject()->setProperty("catalog_selections",juce::Array<juce::var>());
            value.getDynamicObject()->setProperty("catalog_parameter_edits",juce::Array<juce::var>());
            json=juce::JSON::toString(value,true,22);
            ai::ProposalEdits preflight;
            CHECK(ai::decodeProposal(json.toStdString(),r.policy,*r.catalog->authority,preflight,&reason)&&reason==ai::ProposalDecodeReason::accepted);
            std::cout<<"GROUP_OBSERVE catalog-preflight-return\n";
        }
    }
    std::cout<<"GROUP_OBSERVE generation-enter\n";
    auto outcome=ai::generateProposal(r,[&](const juce::String&,std::string_view,const juce::var&){rack::oauth::StructuredResult reply;
        reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;reply.text=json;return reply;});
    std::cout<<"GROUP_OBSERVE generation-return\n";
    CHECK(outcome.status==ai::GenerationStatus::ready&&outcome.candidate&&outcome.inferenceCalls==1);return outcome;
}
bool receiveResult(InstrumentProcessor& p,const ai::GenerationRequest& request,float gain=.55f)
{auto storage=std::unique_ptr<ai::GenerationResult>(new ai::GenerationResult(result(request,gain)));return p.receiveAiResult(std::move(*storage));}
bool proposed(InstrumentProcessor& p,float value=.55f)
{auto r=request(p);if(!r)return false;CHECK(receiveResult(p,*r,value));CHECK(p.aiProposalPhase()==AiProposalPhase::proposed&&p.aiProposal());return p.aiProposal()!=nullptr;}
std::size_t index(const char* name)
{const auto registry=parameterRegistry();const auto i=std::find_if(registry.begin(),registry.end(),[&](const auto& v){return v.id==name;});CHECK(i!=registry.end());return i==registry.end()?0:std::size_t(i-registry.begin());}
void drain(){juce::MessageManager::getInstance()->runDispatchLoopUntil(10);}
struct Listener final:juce::AudioProcessorListener
{
    int values=0,begins=0,ends=0,changes=0;bool flags=true,reads=true;std::function<void()> action;
    void audioProcessorParameterChanged(juce::AudioProcessor*,int,float) override{++values;}
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*,int) override{++begins;}
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*,int) override{++ends;}
    void audioProcessorChanged(juce::AudioProcessor* p,const ChangeDetails& d) override
    {++changes;flags&=d.nonParameterStateChanged&&!d.latencyChanged&&!d.parameterInfoChanged&&!d.programChanged;
     juce::MemoryBlock b;p->getStateInformation(b);ProjectDocument out;reads&=decodeProjectState(b.getData(),b.getSize(),out);if(action)action();}
};
juce::File assetRoot()
{auto path=juce::File(RACKSYNTH_WORKSPACE).getChildFile(".superpowers/sdd/2026-10-06-m5-plugin-group-lifecycle/task-2/fixtures").getChildFile(juce::Uuid().toString());CHECK(path.createDirectory().wasOk());return path;}
void group_project_recall_and_presets()
{
    begin(__func__);auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;const auto initial=p.groupLinkView();
    CHECK(initial.runtime==GroupUuid(p.aiRuntime())&&!initial.runtime.isNull()&&initial.loadRevision==1&&!initial.intent&&initial.phase==GroupLinkPhase::unlinked);
    const auto d=rich();CHECK(recall(p,d));CHECK(snapshot(p)==d);const auto pending=p.groupLinkView();
    CHECK(pending.intent==d.group&&pending.runtime==initial.runtime&&pending.loadRevision==2&&pending.phase==GroupLinkPhase::pending);
    CHECK((p.aiMusicalContext()==AiMusicalContext{"bass","A","minor",120}));
    CHECK(!p.acknowledgeGroupIntent(pending.runtime,pending.loadRevision,*d.group,*d.group));CHECK(p.groupLinkView()==pending);
    CHECK(recall(p,d));CHECK(p.groupLinkView().loadRevision==pending.loadRevision+1&&p.groupLinkView().phase==GroupLinkPhase::pending);
    CHECK(p.resumeGroupIntent());auto authorized=p.groupLinkView();CHECK(authorized.phase==GroupLinkPhase::authorized&&authorized.intent==d.group);
    CHECK(p.resumeGroupIntent());CHECK(p.setGroupIntent(*d.group));CHECK(p.groupLinkView()==authorized);
    auto normalized=*d.group;normalized.member=id(99);CHECK(p.acknowledgeGroupIntent(authorized.runtime,authorized.loadRevision,*d.group,normalized));
    auto admitted=p.groupLinkView();CHECK(admitted.phase==GroupLinkPhase::admitted&&admitted.intent==normalized&&admitted.loadRevision==authorized.loadRevision);
    CHECK(snapshotOwned(p)->group==normalized);CHECK(!p.acknowledgeGroupIntent(authorized.runtime,authorized.loadRevision,*d.group,normalized));CHECK(p.groupLinkView()==admitted);
    juce::MemoryBlock saved;p.getStateInformation(saved);ProjectDocument decoded;CHECK(decodeProjectState(saved.getData(),saved.getSize(),decoded));CHECK(decoded==snapshot(p));
    auto copy=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(copy->restoreProjectState(saved.getData(),int(saved.getSize())));CHECK(snapshot(*copy)==decoded);
    CHECK(copy->groupLinkView().phase==GroupLinkPhase::pending&&copy->groupLinkView().runtime!=admitted.runtime);CHECK(copy->groupLinkView().intent==normalized);
    const auto root=assetRoot();auto sound=d.sound;sound.values[0]=.36f;juce::MemoryBlock preset;CHECK(encodeSoundState(sound,preset));auto soundFile=root.getChildFile("native.soundstate");CHECK(soundFile.replaceWithData(preset.getData(),preset.getSize()));
    CHECK(p.loadSoundPreset(soundFile));sound.name="native";CHECK(snapshotOwned(p)->sound==sound);CHECK(snapshotOwned(p)->group==normalized&&p.groupLinkView()==admitted);
    auto pattern=d.pattern;pattern.notes[0].pitch=61;const auto patternFile=root.getChildFile("native.pattern");CHECK(savePatternAsset(pattern,patternFile));CHECK(p.loadPattern(patternFile));
    CHECK(snapshotOwned(p)->pattern==pattern&&snapshotOwned(p)->group==normalized&&p.groupLinkView()==admitted);
    drain();auto before=snapshot(p);const auto permission=p.groupLinkView();auto corrupt=saved;static_cast<char*>(corrupt.getData())[20]^=1;
    CHECK(!p.restoreProjectState(corrupt.getData(),int(corrupt.getSize())));CHECK(p.stateLoadFailed());CHECK(snapshot(p)==before&&p.groupLinkView()==permission);
    CHECK(!p.restoreProjectState(saved.getData(),-1));CHECK(p.groupLinkView()==permission);
    CHECK(p.acknowledgeGroupIntent(admitted.runtime,admitted.loadRevision,normalized,normalized));p.groupCompanionDisconnected();
    auto disconnected=p.groupLinkView();CHECK(disconnected.phase==GroupLinkPhase::authorized&&disconnected.intent==admitted.intent&&disconnected.loadRevision==admitted.loadRevision);
    CHECK(p.acknowledgeGroupIntent(disconnected.runtime,disconnected.loadRevision,normalized,normalized));CHECK(p.groupLinkView()==admitted);
    const char* legacy[]{"tests/fixtures/m1-analog-pluck.soundstate","tests/fixtures/m2-analog-pluck.v2.soundstate","tests/fixtures/m2-calibrated-pluck.v3.soundstate"};
    for(const auto* path:legacy){juce::MemoryBlock raw;CHECK(juce::File(RACKSYNTH_WORKSPACE).getChildFile(path).loadFileAsData(raw));SoundDocument expected;CHECK(decodeSoundState(raw.getData(),raw.getSize(),expected));
        CHECK(p.setGroupIntent(normalized));const auto old=p.groupLinkView();CHECK(p.restoreProjectState(raw.getData(),int(raw.getSize())));const auto migrated=snapshot(p);
        CHECK(migrated.sound==expected&&!migrated.group&&migrated.pattern.noteCount==0&&migrated.mode==PlaybackMode::externalMidi);
        CHECK(!p.groupLinkView().intent&&p.groupLinkView().phase==GroupLinkPhase::unlinked&&p.groupLinkView().loadRevision>old.loadRevision);CHECK(p.aiMusicalContext()==AiMusicalContext{});}
    auto ungrouped=rich(false);CHECK(p.setGroupIntent(normalized));CHECK(recall(p,ungrouped));CHECK(snapshot(p)==ungrouped&&!p.groupLinkView().intent);CHECK(!p.stateLoadFailed());
    auto standalone=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(recall(*standalone,ungrouped));auto zero=request(*standalone);if(zero)CHECK(zero->tag.contextRevision==0);
    CHECK(standalone->setAiMusicalContext({"lead","E","major",111}));const auto choices=standalone->aiMusicalContext();auto independent=request(*standalone);
    CHECK(recall(*standalone,ungrouped));CHECK(standalone->aiMusicalContext()==choices);auto after=request(*standalone);if(independent&&after)CHECK(after->tag.contextRevision==independent->tag.contextRevision);
    if(proposed(*standalone)){const auto* proposal=standalone->aiProposal();CHECK(standalone->leaveGroupIntent());CHECK(standalone->aiProposal()==proposal&&standalone->aiMusicalContext()==choices);}
}
void group_history_and_stale_ai()
{
    begin(__func__);auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;auto d=rich();CHECK(recall(p,d));CHECK(p.resumeGroupIntent());
    std::uint64_t edit=0,reset=0;auto before=snapshot(p,&edit,&reset);auto sound=before.sound;sound.values[0]=.35f;CHECK(p.applySoundEdit(sound,SoundEditOrigin::manual,edit));
    auto current=group(2);CHECK(p.setGroupIntent(current));const auto live=p.groupLinkView();CHECK(p.undoSoundEdit());CHECK(snapshotOwned(p)->sound==before.sound&&snapshotOwned(p)->group==current&&p.groupLinkView()==live);
    CHECK(p.redoSoundEdit());snapshotOwned(p,&edit);auto pattern=d.pattern;pattern.notes[0].pitch=72;CHECK(p.applyPatternEdit(pattern,edit));
    current=group(3);CHECK(p.setGroupIntent(current));const auto linked=p.groupLinkView();CHECK(p.undoSoundEdit());CHECK(snapshotOwned(p)->pattern==d.pattern&&snapshotOwned(p)->group==current&&p.groupLinkView()==linked);CHECK(p.redoSoundEdit());
    before=snapshot(p);if(proposed(p,.61f)){CHECK(p.applyAiProposal());CHECK(snapshotOwned(p)->sound.values[0]==.61f);current=group(4);CHECK(p.setGroupIntent(current));const auto authority=p.groupLinkView();
        CHECK(p.undoSoundEdit());CHECK(snapshotOwned(p)->sound.values[0]==before.sound.values[0]&&snapshotOwned(p)->group==current&&p.groupLinkView()==authority);
        CHECK(p.redoSoundEdit());CHECK(snapshotOwned(p)->sound.values[0]==.61f&&snapshotOwned(p)->group==current&&p.groupLinkView()==authority);}
    if(proposed(p)){const auto* proposal=p.aiProposal();auto bad=current;bad.member={};CHECK(!p.setGroupIntent(bad));CHECK(p.aiProposal()==proposal&&p.groupLinkView().intent==current);}
    p.discardAiProposal();auto capturedStorage=std::unique_ptr<std::optional<ai::GenerationRequest>>(new std::optional<ai::GenerationRequest>(request(p)));auto& captured=*capturedStorage;const auto owner=p.groupLinkView();CHECK(p.acknowledgeGroupIntent(owner.runtime,owner.loadRevision,current,current));
    if(captured){CHECK(p.aiProposalPhase()==AiProposalPhase::pending);CHECK(receiveResult(p,*captured));CHECK(p.aiProposalPhase()==AiProposalPhase::proposed);}
    p.groupCompanionDisconnected();CHECK(p.aiProposalPhase()==AiProposalPhase::proposed);CHECK(p.groupLinkView().intent==current);
    captured=request(p);snapshotOwned(p,&edit,&reset);p.control(index("output.gain_db")).setValue(.42f);auto automateStorage=std::unique_ptr<std::optional<ai::GenerationRequest>>(new std::optional<ai::GenerationRequest>(request(p)));auto& automate=*automateStorage;if(captured&&automate)CHECK(automate->tag.contextRevision==captured->tag.contextRevision);
    const auto old=p.groupLinkView();auto normalized=current;normalized.member=id(98);CHECK(p.acknowledgeGroupIntent(old.runtime,old.loadRevision,current,normalized));
    CHECK(p.aiProposalPhase()==AiProposalPhase::stale);CHECK(!p.acknowledgeGroupIntent(old.runtime,old.loadRevision,current,normalized));
    CHECK(p.groupLinkView().intent==normalized&&snapshotOwned(p)->group==normalized);current=normalized;
    auto withGroupStorage=std::unique_ptr<std::optional<ai::GenerationRequest>>(new std::optional<ai::GenerationRequest>(request(p)));auto& withGroup=*withGroupStorage;if(withGroup)CHECK(withGroup->role=="bass"&&withGroup->key=="A"&&withGroup->scale=="minor"&&withGroup->capturedBpm==120&&withGroup->source.group==current);
    CHECK(p.setAiMusicalContext({"pluck","B","major",123}));const auto musicalIntent=p.groupLinkView().intent;CHECK(musicalIntent.has_value());
    if(musicalIntent)CHECK(musicalIntent->role==text("pluck")&&musicalIntent->key==text("B")&&musicalIntent->scale==text("major")&&musicalIntent->bpm==123);
    CHECK(p.aiProposalPhase()==AiProposalPhase::stale);auto settingsStorage=std::unique_ptr<std::optional<ai::GenerationRequest>>(new std::optional<ai::GenerationRequest>(request(p)));auto& settings=*settingsStorage;if(withGroup&&settings)CHECK(settings->tag.contextRevision==withGroup->tag.contextRevision+1);
    const auto loaded=snapshot(p);CHECK(recall(p,loaded));CHECK(p.groupLinkView().phase==GroupLinkPhase::pending);CHECK(p.setAiMusicalContext(p.aiMusicalContext()));CHECK(p.groupLinkView().phase==GroupLinkPhase::authorized);
    p.prepareToPlay(48000,64);if(proposed(p)){CHECK(p.startAiPreview());CHECK(p.applyAiWhenStopped());auto changed=current;changed.role=text("pad");CHECK(p.setGroupIntent(changed));
        CHECK(p.aiProposalPhase()==AiProposalPhase::stale&&!p.aiWorkflowStatus().applyPending);juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;p.processBlock(audio,midi);CHECK(p.aiPreviewStopped());}
    if(proposed(p)){auto child=p.beginAiRefinement("Darker local fixture","synthetic-selected_model:1.0");CHECK(child.has_value());CHECK(p.leaveGroupIntent());
        CHECK(!p.aiOriginalSource()&&!p.aiIntendedEdits()&&p.aiProposalPhase()==AiProposalPhase::stale);if(child)CHECK(!receiveResult(p,*child));}
    CHECK(p.aiMusicalContext()==AiMusicalContext{});CHECK(p.setGroupIntent(group(5)));if(proposed(p)){const auto view=p.groupLinkView();InstrumentGroupTestAccess::setLoadRevision(p,std::numeric_limits<std::uint64_t>::max());
        auto changed=group(5);changed.role=text("exhaustion");CHECK(!p.setGroupIntent(changed));const auto revoked=p.groupLinkView();
        CHECK(revoked.intent==view.intent&&revoked.exhausted&&revoked.phase==GroupLinkPhase::pending);CHECK(p.aiProposalPhase()==AiProposalPhase::stale);}
    CHECK(recall(p,d));CHECK(snapshot(p)==d&&p.groupLinkView().exhausted&&p.groupLinkView().phase==GroupLinkPhase::pending);CHECK(!p.resumeGroupIntent());CHECK(p.leaveGroupIntent());CHECK(!snapshotOwned(p)->group);
    auto saturated=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(recall(*saturated,d));CHECK(saturated->resumeGroupIntent());const auto saturatedView=saturated->groupLinkView();
    InstrumentGroupTestAccess::setContextRevision(*saturated,std::numeric_limits<std::uint64_t>::max());CHECK(!saturated->beginAiRequest(options(),111));CHECK(!saturated->beginAiRefinement("Darker","synthetic-selected_model:1.0"));
    CHECK(saturated->setGroupIntent(*d.group));CHECK(saturated->resumeGroupIntent());CHECK(saturated->groupLinkView()==saturatedView);
    CHECK(!saturated->setGroupIntent(group(6)));auto norm=*d.group;norm.member=id(97);CHECK(!saturated->acknowledgeGroupIntent(saturatedView.runtime,saturatedView.loadRevision,*d.group,norm));CHECK(saturated->groupLinkView()==saturatedView);
    auto replacement=d;replacement.sound.values[0]=.49f;replacement.group=group(7);CHECK(recall(*saturated,replacement));CHECK(snapshot(*saturated)==replacement&&saturated->groupLinkView().phase==GroupLinkPhase::pending);
    CHECK(!saturated->beginAiRequest(options(),111));CHECK(saturated->leaveGroupIntent());CHECK(!snapshot(*saturated).group&&!saturated->groupLinkView().intent);
}
struct Head final:juce::AudioPlayHead
{std::int64_t sample=0;juce::Optional<PositionInfo> getPosition() const override{PositionInfo p;p.setIsPlaying(true);p.setBpm(120);p.setPpqPosition(sample/24000.);p.setTimeInSamples(sample);return p;}};
thread_local bool tracking=false;thread_local int allocations=0,frees=0;
int __cdecl hook(int kind,void*,size_t,int,long,const unsigned char*,int){if(tracking){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE)++frees;}return 1;}
void group_coherent_publication_and_audio()
{
    begin(__func__);auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;auto a=rich();a.group=group(8,true);auto b=a;b.sound.values[0]=.72f;b.pattern.notes[0].pitch=77;b.group=group(9,true);
    CHECK(recall(p,a));drain();Listener listener;p.addListener(&listener);std::uint64_t edit,reset;snapshotOwned(p,&edit,&reset);const auto controls=snapshotOwned(p)->sound.values;
    CHECK(p.setGroupIntent(*b.group));CHECK(listener.changes==1&&listener.flags&&listener.reads&&listener.values==0&&listener.begins==0&&listener.ends==0);
    std::uint64_t afterEdit,afterReset;auto after=snapshot(p,&afterEdit,&afterReset);CHECK(after.sound.values==controls&&after.pattern==a.pattern&&afterEdit==edit&&afterReset==reset&&after.group==b.group);
    auto owner=p.groupLinkView();CHECK(p.resumeGroupIntent());CHECK(p.acknowledgeGroupIntent(owner.runtime,owner.loadRevision,*b.group,*b.group));p.groupCompanionDisconnected();CHECK(listener.changes==1);
    bool left=false;listener.action=[&]{if(!left){left=true;CHECK(p.leaveGroupIntent());}};CHECK(p.setGroupIntent(*a.group));CHECK(left&&listener.reads&&listener.flags&&!p.groupLinkView().intent&&!snapshotOwned(p)->group);
    drain();CHECK(listener.values==0&&listener.begins==0&&listener.ends==0);p.removeListener(&listener);
    CHECK(recall(p,a));juce::MemoryBlock ba,bb;CHECK(encodeProjectState(a,ba)&&encodeProjectState(b,bb));std::atomic<bool> go=false,writerGood=true;
    std::thread writer([&]{while(!go.load())std::this_thread::yield();for(int n=0;n<200;++n){const auto& raw=n%2?ba:bb;if(!p.restoreProjectState(raw.getData(),int(raw.getSize())))writerGood=false;}});
    go=true;int successful=0;for(int n=0;n<500;++n){auto out=rich(false);const auto sentinel=out;std::uint64_t e=999,r=888;const bool read=p.tryProjectSnapshot(out,e,r);
        CHECK(read?(out==a||out==b):(out==sentinel&&e==999&&r==888));if(read)++successful;}
    writer.join();CHECK(writerGood&&successful>0);CHECK(validateProjectDocument(snapshot(p)));
    // Independent invalid routing automation remains live while group metadata uses the last valid baseline.
    CHECK(recall(p,a));p.control(index("synth.1.modulation.1.target")).setValue(1);CHECK(!validateSoundDocument(snapshotOwned(p)->sound));const auto invalidValues=snapshotOwned(p)->sound.values;
    CHECK(p.setGroupIntent(*b.group));CHECK(snapshotOwned(p)->sound.values==invalidValues&&snapshotOwned(p)->group==b.group);CHECK(p.leaveGroupIntent());CHECK(snapshotOwned(p)->sound.values==invalidValues&&!snapshotOwned(p)->group);
    snapshot(p,&edit);auto repairedPattern=a.pattern;repairedPattern.notes[0].pitch=60;CHECK(p.applyPatternEdit(repairedPattern,edit));CHECK(validateProjectDocument(snapshot(p)));CHECK(p.undoSoundEdit());CHECK(validateProjectDocument(snapshot(p)));
    for(auto mode:{PlaybackMode::externalMidi,PlaybackMode::internalPattern}){auto grouped=a;grouped.mode=mode;auto plain=grouped;plain.group.reset();
        auto groupedProcessor=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()),plainProcessor=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(recall(*groupedProcessor,grouped)&&recall(*plainProcessor,plain));
        Head h1,h2;groupedProcessor->setPlayHead(&h1);plainProcessor->setPlayHead(&h2);groupedProcessor->prepareToPlay(48000,64);plainProcessor->prepareToPlay(48000,64);
        juce::AudioBuffer<float> actual(2,256),expected(2,256);juce::MidiBuffer m1,m2;m1.ensureSize(256);m2.ensureSize(256);m1.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(90)),0);m2.addEvent(juce::MidiMessage::noteOn(1,48,juce::uint8(90)),0);
        const auto original=groupedProcessor->groupLinkView();auto previous=_CrtSetAllocHook(hook);allocations=0;frees=0;float peak=0;
        for(int n=0;n<16;++n){if(n==8){auto changed=*grouped.group;changed.role=text("audio-only change");CHECK(groupedProcessor->setGroupIntent(changed));}
            h1.sample=h2.sample=n*256;tracking=true;groupedProcessor->processBlock(actual,m1);plainProcessor->processBlock(expected,m2);tracking=false;
            for(int ch=0;ch<2;++ch)for(int frame=0;frame<256;++frame){CHECK(std::bit_cast<std::uint32_t>(actual.getSample(ch,frame))==std::bit_cast<std::uint32_t>(expected.getSample(ch,frame)));peak=std::max(peak,std::abs(actual.getSample(ch,frame)));}m1.clear();m2.clear();}
        _CrtSetAllocHook(previous);CHECK(allocations==0&&frees==0);CHECK(peak>0);CHECK(groupedProcessor->groupLinkView().runtime==original.runtime);
        CHECK(snapshot(*groupedProcessor).pattern==grouped.pattern);groupedProcessor->setPlayHead(nullptr);plainProcessor->setPlayHead(nullptr);
        std::cout<<"AUDIO mode="<<int(mode)<<" allocations="<<allocations<<" frees="<<frees<<" exact samples=8192\n";}
}
void spectrum_view_lease_is_transient_and_never_resumes_saved_consent()
{
    begin(__func__);companion::ClientOptions offline;offline.allowLaunch=false;
    const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));offline.launch.fixtureRoot=root;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");
    auto storage=std::make_unique<InstrumentProcessor>(offline, airack::testing::isolatedRackRuntime());auto& p=*storage;auto document=rich();document.sound.layout.effects[7]={SlotUse::present,EffectKind::dynamicEq,masterChain};document.sound.layout.effectOrder[masterChain][0]=7;document.sound.layout.effectCount[masterChain]=1;CHECK(recall(p,document));
    const auto before=snapshotOwned(p);const auto selection=p.groupWorkflowView().selection;const auto owner=p.openEqSpectrumView();CHECK(owner!=0);CHECK(p.configureEqSpectrumView(owner,true,7,{}));CHECK(p.eqSpectrumView(owner).enabled);
    std::array<std::int16_t,128> levels;levels.fill(-3000);CHECK(!p.captureEqSpectrum(owner,7,1,1,48000,levels));CHECK(!p.captureEqSpectrum(owner,7,1,2,48000,levels));CHECK(!p.peerEqSpectrum(owner,id(99)));
    p.closeEqSpectrumView(owner);CHECK(!p.eqSpectrumView(owner).enabled);const auto next=p.openEqSpectrumView();CHECK(next&&next!=owner);CHECK(!p.configureEqSpectrumView(owner,true,7,{}));CHECK(p.configureEqSpectrumView(next,true,7,{}));p.closeEqSpectrumView(owner);CHECK(p.eqSpectrumView(next).enabled);CHECK(!p.captureEqSpectrum(owner,7,1,3,48000,levels));
    const auto state=p.eqSpectrumView(next);CHECK(!p.configureEqSpectrumView(next,true,8,{}));const std::array<GroupUuid,5> tooMany{};CHECK(!p.configureEqSpectrumView(next,true,7,tooMany));const std::array<GroupUuid,1> unknown{id(2)};CHECK(!p.configureEqSpectrumView(next,true,7,unknown));CHECK(p.eqSpectrumView(next)==state);
    CHECK(p.groupLinkView().phase==GroupLinkPhase::pending);CHECK(p.groupWorkflowView().connection.phase==companion::ClientPhase::closed);CHECK(p.groupWorkflowView().selection==selection);const auto after=snapshotOwned(p);juce::MemoryBlock a,b;CHECK(encodeProjectState(*before,a)&&encodeProjectState(*after,b)&&a==b);p.closeEqSpectrumView(next);
}

template<class F> bool waitManualGroup(F&& ready)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(std::chrono::steady_clock::now()<deadline){if(ready())return true;drain();}return ready();
}
juce::File manualGroupSource()
{
    const auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("manual-group-preview",".wav");juce::MemoryOutputStream bytes;
    bytes.write("RIFF",4);bytes.writeInt(36+48000*2);bytes.write("WAVEfmt ",8);bytes.writeInt(16);bytes.writeShort(1);bytes.writeShort(1);bytes.writeInt(48000);bytes.writeInt(96000);bytes.writeShort(2);bytes.writeShort(16);bytes.write("data",4);bytes.writeInt(48000*2);
    for(unsigned n=0;n<48000;++n)bytes.writeShort(8192);CHECK(file.replaceWithData(bytes.getData(),bytes.getDataSize()));return file;
}
void manual_preview_survives_public_group_context_and_disconnect_callers()
{
    begin(__func__);companion::ClientOptions offline;offline.allowLaunch=false;offline.launch.pipeSuffix=juce::Uuid().toString().removeCharacters("-");offline.launch.fixtureRoot=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
    auto storage=std::make_unique<InstrumentProcessor>(offline, airack::testing::isolatedRackRuntime());auto& p=*storage;const auto file=manualGroupSource();const auto rack=juce::Uuid();
    RackOperation operation;operation.newInstance=rack;operation.allowSlotReuse=true;operation.design=RackDesign{};operation.design->name="Manual group caller fixture";operation.design->panel.sections={"Drum"};operation.design->units={{0,NativeModule::drum,{},legacyAlgorithmRevision}};operation.allocation={{0,0}};operation.destination={RackRole::synth,masterChain,0};
    std::uint64_t revision=0,reset=0;snapshotOwned(p,&revision);CHECK(p.applyRackOperation(operation,revision)==RackCandidateStatus::ready);p.prepareToPlay(48000,512);
    DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,0,target,error));const bool opened=p.drumImport().open(target);CHECK(opened);
    if(opened){
        p.drumImport().choose(file);const bool inspected=waitManualGroup([&]{return p.drumImport().view().inspected;});CHECK(inspected);
        if(inspected){
            p.drumImport().edit(0,48000,1,"Manual group caller source");p.drumImport().prepare();const bool ready=waitManualGroup([&]{return p.drumImport().view().phase==DrumImportPhase::ready;});CHECK(ready);
            if(ready){
                p.drumImport().audition();CHECK(waitManualGroup([&]{return InstrumentGroupTestAccess::manualPlaying(p);}));
                const auto before=snapshotOwned(p,&revision,&reset);juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;
                const auto stillManual=[&]{
                    std::uint64_t edit=0,resetNow=0;const auto current=snapshotOwned(p,&edit,&resetNow);CHECK(current->sound==before->sound&&current->pattern==before->pattern&&current->mode==before->mode&&edit==revision&&resetNow==reset);
                    CHECK(InstrumentGroupTestAccess::manualPlaying(p)&&p.aiPreviewStopped()&&!p.aiPreviewPlaying());CHECK(!p.applyAiWhenStopped());p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);
                    CHECK((InstrumentGroupTestAccess::previewBytes(p)==std::array<std::uint64_t,2>{48000*8,48000*8}));
                };
                stillManual();auto intent=group(61);CHECK(p.setGroupIntent(intent));stillManual();CHECK(p.resumeGroupIntent());stillManual();
                auto link=p.groupLinkView();auto normalized=intent;normalized.member=id(99);CHECK(p.acknowledgeGroupIntent(link.runtime,link.loadRevision,intent,normalized));intent=normalized;stillManual();
                p.groupCompanionDisconnected();CHECK(p.groupLinkView().phase==GroupLinkPhase::authorized);stillManual();link=p.groupLinkView();CHECK(p.acknowledgeGroupIntent(link.runtime,link.loadRevision,intent,intent));stillManual();
                CHECK(p.setAiMusicalContext({"percussion","F","minor",109}));stillManual();CHECK(p.selectGroupPeers({}));stillManual();
                // Real reconciliation and a failed no-launch connection run the ordinary group cleanup callers.
                CHECK(p.requestGroupConnection());CHECK(waitManualGroup([&]{p.pollAi();return p.groupWorkflowView().connection.phase==companion::ClientPhase::failed;}));stillManual();
                p.closeAiPanel();stillManual();CHECK(p.leaveGroupIntent());CHECK(!p.groupLinkView().intent);p.pollAi();stillManual();
                p.drumImport().cancel();for(unsigned n=0;n<2;++n)p.processBlock(audio,midi);
                CHECK(waitManualGroup([&]{return p.drumImport().view().phase==DrumImportPhase::closed&&!InstrumentGroupTestAccess::manualPlaying(p)&&InstrumentGroupTestAccess::previewBytes(p)==std::array<std::uint64_t,2>{0,0};}));
            }
        }
    }
    p.drumImport().cancel();CHECK(waitManualGroup([&]{return p.drumImport().view().phase==DrumImportPhase::closed;}));p.releaseResources();storage.reset();CHECK(file.deleteFile());
}
using GroupAccess=InstrumentGroupTestAccess;
constexpr std::uint64_t publicationReservation=2*(8*(20+std::uint64_t(companion::maximumControlPayloadBytes))+4096)+65536;
companion::WireBytes groupBytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
template<class F> bool groupObserve(F&& ready){const auto end=GetTickCount64()+3000;do{if(ready())return true;SwitchToThread();}while(GetTickCount64()<end);return ready();}
struct LocalGroupPipe final
{
    juce::String name=juce::Uuid().toString().removeCharacters("-");
    companion::PipeListener listener;companion::PipeConnection pipe;
    std::unique_ptr<InstrumentProcessor> processor;juce::Uuid session=juce::Uuid();bool reading=false;
    explicit LocalGroupPipe(bool local=true){companion::ClientOptions options;options.allowLaunch=false;options.negotiateSpectrum=false;options.negotiateOwnedPreview=local;
        options.launch.pipeSuffix=name;options.launch.fixtureRoot=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{}));
        CHECK(listener.open(name)==companion::PipeStatus::ready);processor=std::make_unique<InstrumentProcessor>(options, airack::testing::isolatedRackRuntime());}
    ~LocalGroupPipe(){GroupAccess::withhold(*processor,false);processor->closeAiSession();processor.reset();pipe.close();listener.close();companion::detail::PipeTestAccess::reapRetired();}
    InstrumentProcessor& p(){return *processor;}
    bool receive(companion::Frame& frame){if(!reading){if(pipe.beginRead()!=companion::PipeStatus::ready)return false;reading=true;}
        const auto status=pipe.pollRead(frame);if(status!=companion::PipeStatus::pending)reading=false;return status==companion::PipeStatus::complete;}
    bool read(companion::Frame& frame,bool pump=true){const auto ok=groupObserve([&]{if(pump)GroupAccess::pollClient(p());return receive(frame);});CHECK(ok);return ok;}
    bool send(const companion::Frame& frame){const auto begun=pipe.beginWrite(frame)==companion::PipeStatus::ready;CHECK(begun);if(!begun)return false;
        companion::PipeStatus status=companion::PipeStatus::pending;const auto done=groupObserve([&]{status=pipe.pollWrite();if(status==companion::PipeStatus::pending)GroupAccess::pollClient(p());return status!=companion::PipeStatus::pending;});
        CHECK(done&&status==companion::PipeStatus::complete);return done&&status==companion::PipeStatus::complete;}
    bool open(bool local=true){if(!GroupAccess::startClient(p())){CHECK(false);return false;}
        const auto accepted=groupObserve([&]{GroupAccess::pollClient(p());return listener.pollAccept(pipe)==companion::PipeStatus::complete;});CHECK(accepted);if(!accepted)return false;
        CHECK(pipe.peer().processId==GetCurrentProcessId());auto hello=std::make_unique<companion::Frame>();if(!read(*hello))return false;
        companion::ControlMessage decoded;const auto valid=hello->kind==companion::MessageKind::hello&&companion::decodeControl(groupBytes(hello->payload),decoded);CHECK(valid);if(!valid)return false;
        CHECK(decoded.tag.runtime==p().aiRuntime()&&decoded.capabilities==(local?companion::ownedLocalPreviewCapability:0u));
        companion::Frame welcome{companion::MessageKind::welcome,session,{}};CHECK(companion::encodeControl({decoded.tag,companion::RuntimeStatus::ready,local?companion::ownedLocalPreviewCapability:0u},welcome.payload));
        if(!send(welcome))return false;const auto ready=groupObserve([&]{GroupAccess::pollClient(p());return GroupAccess::clientStatus(p()).phase==companion::ClientPhase::ready;});CHECK(ready);return ready;}
    bool acknowledge(const companion::GroupCommand& command){auto reply=std::make_unique<companion::GroupReply>();reply->operationId=command.operationId;reply->action=command.action;
        reply->runtime=command.runtime;reply->loadRevision=command.loadRevision;reply->proposal=command.proposal;reply->status=companion::GroupReplyStatus::ready;
        if(command.publication){reply->normalized=command.publication->project.group;reply->registryRevision=1;}
        companion::Frame frame{companion::MessageKind::groupReply,session,{}};const bool encoded=companion::encodeGroupReply(*reply,frame.payload);CHECK(encoded);return encoded&&send(frame);}
    bool settleReply(){const auto held=groupObserve([&]{GroupAccess::pollClient(p());return GroupAccess::reply(p());});CHECK(held);if(!held)return false;
        GroupAccess::settle(p());return true;}
};
bool addPublicationDrum(InstrumentProcessor& p,juce::Uuid rack,drums::AssetId& id)
{
    std::uint64_t edit=0;(void)snapshotOwned(p,&edit);
    RackOperation operation;operation.newInstance=rack;operation.allowSlotReuse=true;operation.design=RackDesign{};
    operation.design->name="Group source fixture";operation.design->panel.sections={"Drum"};operation.design->units={{0,NativeModule::drum,{},legacyAlgorithmRevision}};
    operation.allocation={{0,0}};operation.destination={RackRole::synth,masterChain,0};
    if(p.applyRackOperation(operation,edit)!=RackCandidateStatus::ready){CHECK(false);return false;}
    const std::array<float,4> samples{.25f,-.125f,.0625f,0};drums::AssetPtr expected;drums::AssetError assetError;
    if(!drums::makeSampleAsset(samples,1,expected,assetError)){CHECK(false);return false;}id=expected->id();expected.reset();
    std::uint64_t ticket=0;DrumCommitError error;if(!p.prepareDrumAsset(samples,1,ticket,error)){CHECK(false);return false;}
    DrumPadTarget target;if(!p.captureDrumPadTarget(rack,0,15,target,error)){CHECK(false);return false;}
    drums::DrumPad metadata;metadata.asset=id;metadata.name="Group Pad Sixteen";drums::SourceProvenance provenance;
    provenance.kind=drums::SourceKind::generated;provenance.sourceFrames=4;provenance.end=4;provenance.generator="private-generator-marker";provenance.condition="private-prompt-marker";provenance.seed=211;metadata.source=std::move(provenance);
    const auto assigned=p.assignPreparedDrumPad(target,ticket,metadata,{},error);CHECK(assigned);return assigned;
}
thread_local bool publicationTracking=false,publicationDenyFirst=false;
thread_local std::uint64_t publicationAllocations=0,publicationDenials=0,publicationDeniedBytes=0;
thread_local unsigned publicationWarnings=0,publicationReports=0;
int __cdecl publicationHook(int type,void*,size_t bytes,int use,long,const unsigned char*,int){
    if(!publicationTracking||(use&0xffff)!=_NORMAL_BLOCK||type!=_HOOK_ALLOC)return 1;
    ++publicationAllocations;if(publicationDenyFirst&&publicationDenials==0){++publicationDenials;publicationDeniedBytes=bytes;return 0;}return 1;}
int __cdecl publicationReport(int type,wchar_t* message,int* value){const std::wstring_view text(message?message:L"");
    if(type==_CRT_WARN&&publicationTracking&&publicationDenyFirst&&publicationDenials==1&&publicationWarnings==0
        &&(text==L"Client hook allocation failure.\n"||text==L"Client hook allocation failure.\r\n"||text==L"Client hook allocation failure."))++publicationWarnings;
    else ++publicationReports;if(value)*value=0;return 1;}
void local_group_publication_owned_metadata_and_retention()
{
    begin(__func__);auto fixture=std::make_unique<LocalGroupPipe>();auto& p=fixture->p();const auto rack=juce::Uuid();drums::AssetId asset;
    if(!addPublicationDrum(p,rack,asset))return;CHECK(p.setGroupIntent(group(71)));if(!fixture->open())return;
    std::uint64_t beforeEdit=0;const auto before=snapshotOwned(p,&beforeEdit);const auto baseline=InstrumentDrumTestAccess::groupAccounting(p);const auto initial=GroupAccess::publicationState(p);
    CHECK(initial[0]==0&&initial[2]==0);const auto oneShort=drums::retainedDecodedLimit-baseline[0]-publicationReservation+1;
    if(!InstrumentDrumTestAccess::reserve(p,oneShort)){CHECK(false);return;}
    publicationAllocations=publicationDenials=0;publicationDenyFirst=false;auto oldHook=_CrtSetAllocHook(publicationHook);publicationTracking=true;
    const auto denied=GroupAccess::submit(p,true);publicationTracking=false;_CrtSetAllocHook(oldHook);
    CHECK(!denied&&publicationAllocations==0&&GroupAccess::publicationState(p)==initial);InstrumentDrumTestAccess::release(p,oneShort);
    CHECK(InstrumentDrumTestAccess::groupAccounting(p)==baseline&&*snapshotOwned(p)==*before);
    publicationAllocations=publicationDenials=publicationDeniedBytes=0;publicationWarnings=publicationReports=0;publicationDenyFirst=true;
    _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,publicationReport);oldHook=_CrtSetAllocHook(publicationHook);publicationTracking=true;
    const auto allocated=GroupAccess::submit(p,true);publicationTracking=false;publicationDenyFirst=false;_CrtSetAllocHook(oldHook);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,publicationReport);
    CHECK(!allocated&&publicationDenials==1&&publicationDeniedBytes==sizeof(companion::GroupCommand));CHECK(publicationWarnings==1&&publicationReports==0);
    CHECK(GroupAccess::publicationState(p)==initial&&InstrumentDrumTestAccess::groupAccounting(p)==baseline&&*snapshotOwned(p)==*before);
    GroupAccess::withhold(p,true);if(!GroupAccess::submit(p,true)){CHECK(false);GroupAccess::withhold(p,false);return;}
    auto pending=GroupAccess::publicationState(p);CHECK(pending[0]==publicationReservation&&pending[1]==initial[1]+1&&pending[2]==1);
    CHECK(InstrumentDrumTestAccess::groupAccounting(p)[0]==baseline[0]+publicationReservation&&GroupAccess::started(p));
    p.cancelAi();CHECK(GroupAccess::publicationState(p)==pending);CHECK(InstrumentDrumTestAccess::groupAccounting(p)[0]==baseline[0]+publicationReservation);
    GroupAccess::withhold(p,false);auto frame=std::make_unique<companion::Frame>();if(!fixture->read(*frame))return;
    auto command=std::make_unique<companion::GroupCommand>();auto map=std::make_unique<ai::PreviewSampleMap>();
    const auto decoded=frame->kind==companion::MessageKind::groupCommandLocal&&companion::decodeLocalGroupPublication(groupBytes(frame->payload),*command,*map);CHECK(decoded);if(!decoded)return;
    CHECK(command->publication&&command->publication->project==*before&&command->publication->editRevision==beforeEdit);
    const bool shape=map->assets.size()==1&&map->pads[15].asset==1&&map->pads[15].name=="Group Pad Sixteen";CHECK(shape);if(!shape)return;
    CHECK(map->assets[0].id==asset&&map->assets[0].channels==1&&map->assets[0].frames==4&&map->banks[0].rack==rack);
    const std::string payload(static_cast<const char*>(frame->payload.getData()),frame->payload.getSize());CHECK(payload.find("private-generator-marker")==std::string::npos&&payload.find("private-prompt-marker")==std::string::npos);
    if(!fixture->acknowledge(*command))return;const auto held=groupObserve([&]{GroupAccess::pollClient(p);return GroupAccess::reply(p);});CHECK(held);
    CHECK(GroupAccess::publicationState(p)[0]==publicationReservation&&InstrumentDrumTestAccess::groupAccounting(p)[0]==baseline[0]+publicationReservation);
    CHECK(!GroupAccess::started(p));GroupAccess::settle(p);CHECK(GroupAccess::publicationState(p)[0]==0&&GroupAccess::publicationState(p)[2]==0&&GroupAccess::publicationState(p)[3]==1);
    CHECK(InstrumentDrumTestAccess::groupAccounting(p)==baseline);
    const auto committed=GroupAccess::publicationState(p);p.control(drumModeParameterStart).setValue(0);
    CHECK(!GroupAccess::submit(p,false));CHECK(GroupAccess::publicationState(p)==committed&&InstrumentDrumTestAccess::groupAccounting(p)==baseline);
    std::uint64_t edit=0;auto live=snapshotOwned(p,&edit);auto pattern=live->pattern;pattern.noteCount=1;pattern.notes[0]={0,.25,36,100,1};CHECK(p.applyPatternEdit(pattern,edit));
    if(!GroupAccess::submit(p,false)){CHECK(false);return;}CHECK(GroupAccess::publicationState(p)[0]==publicationReservation);
    p.closeAiSession();CHECK(GroupAccess::publicationState(p)[0]==0&&GroupAccess::publicationState(p)[2]==0);CHECK(InstrumentDrumTestAccess::groupAccounting(p)==baseline);
    CHECK(snapshotOwned(p)->group==before->group&&snapshotOwned(p)->pattern==pattern);
    std::cout<<"LOCAL_GROUP_PUBLICATION reservation="<<publicationReservation<<" payload="<<frame->payload.getSize()<<" pending_cancel_retained=1 one_byte_denied_before_alloc=1 first_application_new_denied=1\n";
}
void legacy_group_publication_keeps_the_no_map_wire_path()
{
    begin(__func__);auto fixture=std::make_unique<LocalGroupPipe>(false);auto& p=fixture->p();CHECK(p.setGroupIntent(group(72)));if(!fixture->open(false))return;
    if(!GroupAccess::submit(p,true)){CHECK(false);return;}CHECK(GroupAccess::publicationState(p)[0]==0);
    auto frame=std::make_unique<companion::Frame>();if(!fixture->read(*frame))return;auto command=std::make_unique<companion::GroupCommand>();
    CHECK(frame->kind==companion::MessageKind::groupCommand&&companion::decodeGroupCommand(groupBytes(frame->payload),*command));
    if(!command->publication){CHECK(false);return;}CHECK(command->publication->project.group==p.groupLinkView().intent);
    if(!fixture->acknowledge(*command)||!fixture->settleReply())return;CHECK(GroupAccess::publicationState(p)[0]==0&&GroupAccess::publicationState(p)[2]==0);
}

}
int main(int argc,char** argv)
{
    std::cout.setf(std::ios::unitbuf);std::cerr<<"GROUP_OBSERVE main-enter\n";
    juce::ScopedJuceInitialiser_GUI gui;FILETIME c{},e{},k{},u{};GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);ULARGE_INTEGER time{};time.LowPart=c.dwLowDateTime;time.HighPart=c.dwHighDateTime;
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<time.QuadPart<<" IMAGE="<<juce::String(module)<<" CWD="<<juce::File::getCurrentWorkingDirectory().getFullPathName()<<'\n';
    for(int n=0;n<argc;++n)std::cout<<"ARGV["<<n<<"]="<<argv[n]<<'\n';
    if(argc==2&&juce::String(argv[1])=="--local-publication"){local_group_publication_owned_metadata_and_retention();legacy_group_publication_keeps_the_no_map_wire_path();std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--spectrum"){spectrum_view_lease_is_transient_and_never_resumes_saved_consent();std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;}
    spectrum_view_lease_is_transient_and_never_resumes_saved_consent();group_project_recall_and_presets();group_history_and_stale_ai();group_coherent_publication_and_audio();CHECK(sections==4);
    manual_preview_survives_public_group_context_and_disconnect_callers();
    local_group_publication_owned_metadata_and_retention();legacy_group_publication_keeps_the_no_map_wire_path();
    std::cout<<checks<<" checks, "<<sections<<" sections, "<<failures<<" failures\n";return failures?1:0;
}

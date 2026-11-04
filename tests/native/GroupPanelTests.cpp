#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "InstrumentProcessor.h"
#include "AiUnit.h"
#include "CompanionRuntimeServer.h"
#include "LocalPreviewExchange.h"
#include "RackExamples.h"
#include <stdexcept>
#include "ScreenDriver.h"
#include "StudioSkin.h"
#include "PostedClick.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <iostream>
#include <mutex>
#include <source_location>
#include <thread>
using namespace airack;
using namespace airack::companion;
namespace airack{struct InstrumentGroupTestAccess{
 static std::optional<ai::GenerationRequest> beginHistorical25(InstrumentProcessor& p,const AiRequestOptions& options){
  p.refreshAiProposal();auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign().id;
  std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
  std::erase_if(factory,[&](const auto& design){return design.id==tile;});if(factory.size()!=25)throw std::runtime_error("historical25 library fixture");
  auto inventory=std::make_shared<RackCatalogSnapshot>();
  {const std::lock_guard lock(p.restoreMutex);auto source=std::make_unique<ProjectDocument>();std::uint64_t edit;
   if(!p.tryProjectSnapshotOwned(*source,edit))return {};std::vector<RackDesign> embedded;
   if(source->sound.personalities)for(const auto& instance:source->sound.personalities->instances)embedded.push_back(instance.design);
   if(!captureRackCatalog(factory,embedded,{},*inventory))return {};}
  return p.beginAiRequestWithCatalog(options,{},std::move(inventory));
 }

 static std::uint64_t pauseWithPublication(InstrumentProcessor& p){p.stopTimer();if(!p.groupControl->pending)p.submitGroup(InstrumentProcessor::GroupControl::Purpose::publish,GetTickCount64());const std::lock_guard lock(p.restoreMutex);return p.groupControl->pending?p.groupControl->pending->operationId:0;}
 static void resumePolling(InstrumentProcessor& p){p.startTimer(25);}
 static void pausePolling(InstrumentProcessor& p){p.stopTimer();}
 struct DiagnosticState
 {
  int phase=0,purpose=0;
  std::uint64_t request=0,pendingOperation=0,nextOperation=0,markerRequest=0;
  bool ready=false,waiting=false,pending=false,admitted=false,dirty=false,publishedBusy=false,conflicted=false,busy=false,marker=false,markerCurrent=false,markerProposal=false;
  bool operator==(const DiagnosticState&) const=default;
 };
 struct DiagnosticSample {DiagnosticState state;std::uint64_t tick=0,lastTime=0,observationTime=0,checkTime=0;};
 static DiagnosticSample diagnosticSample(const InstrumentProcessor& p,bool ready) noexcept
 {
  const auto& g=*p.groupControl;const auto* request=p.aiCurrentRequest();
  return {{int(p.aiProposalPhase()),int(g.purpose),request?request->tag.requestId:0,g.pending?g.pending->operationId:0,g.nextOperation,g.marker?g.marker->tag.requestId:0,
           ready,g.waiting.has_value(),bool(g.pending),g.admitted,g.dirty,g.publishedBusy,g.conflicted,p.groupBusyOwned(),g.marker.has_value(),g.marker&&g.marker->current,g.marker&&g.marker->proposal},
          GetTickCount64(),g.lastTime,g.observationTime,g.checkTime};
 }
 struct DiagnosticFinal {int connectionPhase=0,connectionStatus=0,linkPhase=0;std::uint64_t loadRevision=0;bool markerOwned=false;};
 static DiagnosticFinal diagnosticFinal(const InstrumentProcessor& p)
 {
  const auto connection=p.aiClient.status();const auto link=p.groupLinkState.view();const auto& g=*p.groupControl;
  return {int(connection.phase),int(connection.status),int(link.phase),link.loadRevision,g.marker&&p.groupMarkerCurrentOwned(*g.marker)};
 }
};}
namespace
{
std::atomic_int checks=0,failures=0;int sections=0;const char* section="";
void check(bool ok,const char* expression,int line){++checks;if(!ok){++failures;std::cerr<<section<<':'<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){section=name;++sections;std::cout<<name<<'\n';}
void progress(){juce::MessageManager::getInstance()->runDispatchLoopUntil(2);}
InstrumentProcessor* observeTarget=nullptr;
struct ObserveTargetScope {InstrumentProcessor* previous=observeTarget;explicit ObserveTargetScope(InstrumentProcessor& p){observeTarget=&p;}~ObserveTargetScope(){observeTarget=previous;}};
template<class F>bool observe(F f,const std::source_location at=std::source_location::current())
{const auto start=GetTickCount64(),end=start+5000;std::uint64_t turns=0,longest=0;
 using State=InstrumentGroupTestAccess::DiagnosticSample;struct Span{std::uint64_t tick=0,predicate=0,dispatch=0;bool result=false;State before{},after{};};
 Span first{},maximum{},last{},final{};std::uint64_t samples=0,predicateTotal=0,dispatchTotal=0;
 const auto state=[](){return observeTarget?InstrumentGroupTestAccess::diagnosticSample(*observeTarget,false):State{};};
 const auto retain=[&](const Span& span){if(!samples)first=span;++samples;last=span;if(span.predicate+span.dispatch>maximum.predicate+maximum.dispatch)maximum=span;predicateTotal+=span.predicate;dispatchTotal+=span.dispatch;};
 const auto finish=[&](bool result){std::cout<<"MEASURE observe line="<<at.line()<<" result="<<result<<" samples="<<samples<<" predicate_total_ms="<<predicateTotal<<" dispatch_total_ms="<<dispatchTotal<<" target="<<bool(observeTarget)<<std::endl;
  for(const auto& item:std::array<std::pair<const char*,Span>,4>{{{"first",first},{"maximum",maximum},{"last",last},{"final",final}}}){const auto& v=item.second;std::cout<<"MEASURE observe_span kind="<<item.first<<" tick="<<v.tick<<" predicate_ms="<<v.predicate<<" dispatch_ms="<<v.dispatch<<" result="<<v.result<<" request_before="<<v.before.state.request<<" request_after="<<v.after.state.request<<" phase_before="<<v.before.state.phase<<" phase_after="<<v.after.state.phase<<" pending_before="<<v.before.state.pending<<" pending_after="<<v.after.state.pending<<" marker_before="<<v.before.state.markerCurrent<<" marker_after="<<v.after.state.markerCurrent<<std::endl;}return result;};
 do{const auto turn=GetTickCount64();Span span;span.tick=turn;span.before=state();const auto predicateAt=GetTickCount64();span.result=f();span.predicate=GetTickCount64()-predicateAt;if(span.result){span.after=state();retain(span);return finish(true);}const auto dispatchAt=GetTickCount64();progress();span.dispatch=GetTickCount64()-dispatchAt;longest=std::max(longest,GetTickCount64()-turn);++turns;span.after=state();retain(span);}while(GetTickCount64()<end);
 final.tick=GetTickCount64();final.before=state();const auto finalAt=GetTickCount64();final.result=f();final.predicate=GetTickCount64()-finalAt;final.after=state();if(final.result)return finish(true);std::cout<<"  observe at line "<<at.line()<<" not done after "<<GetTickCount64()-start<<" ms, "<<turns<<" turns, the longest "<<longest<<" ms"<<std::endl;return finish(false);}
struct GroupObservationDiagnostic
{
 const char* label;const std::uint64_t start=GetTickCount64();
 std::array<InstrumentGroupTestAccess::DiagnosticSample,64> samples{};
 InstrumentGroupTestAccess::DiagnosticSample final{};std::size_t count=0,overflow=0;bool sampled=false;
 void sample(const InstrumentProcessor& p,bool ready) noexcept
 {
  const auto next=InstrumentGroupTestAccess::diagnosticSample(p,ready);
  if(!sampled||next.state!=final.state){if(count<samples.size())samples[count++]=next;else ++overflow;}
  final=next;sampled=true;
 }
 void dump(const InstrumentProcessor& p,bool ready) const
 {
  const auto end=GetTickCount64();const auto state=InstrumentGroupTestAccess::diagnosticFinal(p);
  std::cout<<"GROUP_OBSERVATION label="<<label<<" start_tick="<<start<<" end_tick="<<end<<" ready="<<ready<<" sampled="<<sampled<<" samples="<<count<<" overflow="<<overflow<<'\n';
  const auto print=[&](const char* kind,const InstrumentGroupTestAccess::DiagnosticSample& item){const auto& s=item.state;
   std::cout<<"GROUP_STATE label="<<label<<" kind="<<kind<<" tick="<<item.tick<<" phase="<<s.phase<<" request="<<s.request<<" ready="<<s.ready<<" waiting="<<s.waiting
    <<" pending="<<s.pending<<" pending_operation="<<s.pendingOperation<<" purpose="<<s.purpose<<" next_operation="<<s.nextOperation<<" admitted="<<s.admitted<<" dirty="<<s.dirty
    <<" published_busy="<<s.publishedBusy<<" conflicted="<<s.conflicted<<" busy="<<s.busy<<" marker="<<s.marker<<" marker_current="<<s.markerCurrent<<" marker_request="<<s.markerRequest
    <<" marker_proposal="<<s.markerProposal<<" steady_last="<<item.lastTime<<" steady_observation="<<item.observationTime<<" steady_check="<<item.checkTime<<'\n';};
  for(std::size_t i=0;i<count;++i)print("change",samples[i]);if(sampled)print("final",final);
  std::cout<<"GROUP_FINAL label="<<label<<" connection_phase="<<state.connectionPhase<<" connection_status="<<state.connectionStatus<<" link_phase="<<state.linkPhase
   <<" load_revision="<<state.loadRevision<<" marker_owned="<<state.markerOwned<<'\n';
 }
};
std::uint64_t creation(){FILETIME a{},b{},c{},d{};CHECK(GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d));return(std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
juce::String uuid(GroupUuid id){return juce::Uuid(id.getRawData()).toString();}
GroupUuid id(unsigned n){GroupUuid r;r.bytes[0]=std::uint8_t(n);r.bytes[15]=0x62;return r;}
GroupText gt(std::string_view s){GroupText r;CHECK(setGroupText(s,r));return r;}
GroupIntent intent(unsigned member,unsigned group=90,const char* name="Same band"){GroupIntent r;r.group=id(group);r.member=id(member);r.name=gt(name);r.role=gt("bass");r.key=gt("A");r.scale=gt("minor");r.bpm=120;CHECK(validateGroupIntent(r));return r;}
std::size_t index(const char* name){const auto r=parameterRegistry();const auto p=std::find_if(r.begin(),r.end(),[&](const auto& d){return d.id==name;});CHECK(p!=r.end());return p==r.end()?0:std::size_t(p-r.begin());}
WireBytes bytes(const juce::MemoryBlock& b){return{static_cast<const std::byte*>(b.getData()),b.getSize()};}
juce::Component* find(juce::Component& root,const juce::String& key){if(root.getComponentID()==key)return &root;for(auto* c:root.getChildren())if(auto* r=find(*c,key))return r;return nullptr;}
template<class T>T* widget(juce::Component& root,const char* key){auto* r=dynamic_cast<T*>(find(root,key));CHECK(r);return r;}
void click(juce::Button& b){CHECK(clickPosted(b));}
bool click(juce::Component& root,const char* key){auto* b=widget<juce::Button>(root,key);if(!b)return false;CHECK(b->isEnabled());if(!b->isEnabled())return false;click(*b);return true;}
juce::String status(juce::Component& root,const char* key="group.status"){if(auto* s=widget<juce::Label>(root,key))return s->getText();return{};}
// The 60 ms lets the AI unit refresh from the paused processor before the posted click.
bool clickDuringPublication(InstrumentProcessor& p,juce::Component& unit,const char* key,const char* waiting)
{
 CHECK(InstrumentGroupTestAccess::pauseWithPublication(p)!=0);juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
 const bool clicked=click(unit,key);CHECK(p.aiWorkflowStatus().generationWaiting);CHECK(status(unit,"ai.status").contains(waiting));
 InstrumentGroupTestAccess::resumePolling(p);return clicked;
}
const auto imageRoot=juce::File(RACKSYNTH_WORKSPACE).getChildFile(".superpowers/sdd/2026-10-06-m5-group-panel/task-1/images").getChildFile(juce::Uuid().toString());
void image(juce::Component& c,const char* name){CHECK(imageRoot.createDirectory().wasOk());const auto f=imageRoot.getChildFile(juce::String(name)+".png");auto stream=f.createOutputStream();const auto picture=c.createComponentSnapshot(c.getLocalBounds(),true);CHECK(stream&&picture.isValid());if(stream)CHECK(juce::PNGImageFormat().writeImageToStream(picture,*stream));std::cout<<"PNG "<<f.getFullPathName()<<'\n';}
struct Temp
{
 juce::File parent=juce::File(RACKSYNTH_WORKSPACE).getChildFile(".superpowers/sdd/2026-10-06-m5-group-panel/task-1/fixtures"),root=parent.getChildFile(juce::Uuid().toString());
 Temp(){CHECK(root.isAChildOf(parent)&&root.getParentDirectory()==parent);CHECK(root.createDirectory().wasOk());}
 ~Temp(){if(root.isAChildOf(parent)&&root.getParentDirectory()==parent)CHECK(root.deleteRecursively());}
};
AccountReply account(){AccountReply r;r.providerStatus=rack::oauth::Status::connected;r.httpStatus=200;r.selectedModel="synthetic-model";r.modelVerified=true;r.models={{"synthetic-model","Synthetic native fixture"}};return r;}
ai::GenerationResult generated(const ai::GenerationRequest& request)
{
 ai::GenerationResult r;r.tag=request.tag;r.inferenceCalls=1;r.providerStatus=rack::oauth::Status::connected;r.httpStatus=200;r.edits.characteristics="Synthetic requester-only edit";r.edits.explanation="Synthetic native fixture; no provider";
 if(request.instruction=="fail"){r.status=ai::GenerationStatus::providerFailure;r.providerStatus=rack::oauth::Status::networkFailure;r.errorCode="synthetic_failure";return r;} if(request.catalog)r.catalog=ai::CatalogResultBinding{request.catalog->root,request.catalog->authority->snapshot};
 if(request.policy.scope!=ai::RequestScope::pattern)r.edits.parameters.push_back({"output.gain_db",request.instruction=="child"?.45f:.35f});
 if(request.policy.scope!=ai::RequestScope::sound&&request.instruction!="child"){r.edits.pattern=PatternDocument{};r.edits.pattern->noteCount=1;r.edits.pattern->notes[0]={0,.25,64,90,1};}
 auto candidate=std::make_unique<ProjectDocument>();CHECK(ai::admitProposal(request.source,request.policy,r.edits,*candidate)==ai::ProposalStatus::accepted);r.candidate=*candidate;r.status=ai::GenerationStatus::ready;r.proposalStatus=ai::ProposalStatus::accepted;r.renderStatus=ai::RenderStatus::ready;
 r.audio.bodyFrames=1;r.audio.left.resize(ai::previewTailFrames+1);r.audio.right.resize(r.audio.left.size());r.audio.left[0]=.1f;r.audio.right[0]=.05f;r.audio.context.bpm=request.capturedBpm.value_or(120);r.audio.context.tempoSource=request.capturedBpm?ai::TempoSource::captured:ai::TempoSource::defaultTempo;if(r.edits.pattern){r.audio.context.phrase=*r.edits.pattern;r.audio.context.phraseSource=ai::PhraseSource::proposed;}else if(request.capturedPhrase)r.audio.context.phrase=*request.capturedPhrase;else{r.audio.context.phraseSource=ai::PhraseSource::defaultNotes;r.audio.context.phrase.noteCount=1;r.audio.context.phrase.notes[0]={0,.25,60,90,1};}
 CHECK(ai::measurePreview(r.audio.left,r.audio.right,r.audio.bodyFrames,r.measurements)==ai::RenderStatus::ready);return r;
}
// Public current-catalog UI requests use the actual local worker/receipt path.
Frame generatedLocal(const RuntimeJob& job,const ai::GenerationRequest& request){
 using namespace ai;
 if(!job.localPreview||!job.previewExchange)throw std::runtime_error("local preview pair missing");
 auto input=std::make_unique<LocalGenerationRequest>();input->request=request;input->local=*job.localPreview;
 unsigned attempts=request.catalog&&request.catalog->phase==CatalogPhase::materialize?request.catalog->inferenceCallsUsed:0;std::uint8_t ordinal=0;
 const auto object=[](std::initializer_list<std::pair<const char*,juce::var>> fields){auto* value=new juce::DynamicObject;for(const auto& item:fields)value->setProperty(item.first,item.second);return juce::var(value);};
 const CancelCheck cancelled=[&]{return job.cancellation->cancelled();};
 auto result=std::unique_ptr<LocalGenerationResult>(new LocalGenerationResult(generateLocalProposal(*input,
  [&](const auto&,auto,const auto&){++attempts;rack::oauth::StructuredResult response;
   if(request.instruction=="fail"){response.result.status=rack::oauth::Status::networkFailure;response.result.errorCode="synthetic_failure";return response;}
   response.result.status=rack::oauth::Status::connected;response.result.httpStatus=200;
   juce::Array<juce::var> parameters;
   if(request.policy.scope!=RequestScope::pattern)parameters.add(object({{"id","output.gain_db"},{"normalized",request.instruction=="child"?.45:.35}}));
   juce::var pattern;
   if(request.policy.scope!=RequestScope::sound&&request.instruction!="child")pattern=object({{"bars",1},{"meter_numerator",4},{"meter_denominator",4},{"grid","sixteenth"},{"swing",0.},
    {"notes",juce::Array<juce::var>{object({{"start_beats",0.},{"duration_beats",.25},{"pitch",64},{"velocity",90},{"channel",1}})}}});
   auto body=object({{"version",request.catalog?2:1},{"parameter_edits",parameters},{"rack_edits",juce::Array<juce::var>()},{"macro_edits",juce::Array<juce::var>()},
    {"pattern",pattern},{"characteristics","Synthetic requester-only edit"},{"explanation","Synthetic native fixture; no provider"}});
   if(request.catalog){auto* fields=body.getDynamicObject();fields->setProperty("name","Group Fixture");
    fields->setProperty("reconciliation",object({{"detach",juce::Array<juce::var>()},{"remove",juce::Array<juce::var>()},{"replace",juce::Array<juce::var>()},{"controls",juce::Array<juce::var>()}}));
    fields->setProperty("catalog_selections",juce::Array<juce::var>());fields->setProperty("catalog_parameter_edits",juce::Array<juce::var>());}
   response.text=juce::JSON::toString(body,true,22);return response;},
  [&](const auto& candidate,const auto& edits,const auto& audition,const auto& trace,const auto& stop){
   auto value=std::make_unique<PreviewEvaluate>();value->edits=edits;value->context=audition;auto& binding=value->binding;
   binding.request=request.tag;binding.root=input->local.root;binding.catalogSnapshot=input->local.catalogSnapshot;binding.job=job.jobId;
   binding.structuralGeneration=request.structuralGeneration;binding.reset=input->local.reset;binding.sourceWord=input->local.sourceWord;
   binding.phase=request.catalog?request.catalog->phase:CatalogPhase::select;binding.providerAttempt=std::uint8_t(attempts);binding.renderOrdinal=++ordinal;binding.sourceMap=input->local.sourceMapDigest;
   PreviewSampleMap staged;
   if(!stagePreviewMap(input->local.sourceMap,trace,staged)||!digestPreviewMap(staged,binding.stagedMap)||!digestPreviewCandidate(candidate,binding.candidate)||!digestPreviewContext(audition,binding.context))return PreviewReceipt{};
   return job.previewExchange->evaluate(*value,stop);
  },cancelled,job.groupContext.get())));
 Frame frame{MessageKind::resultLocal,job.session,{}};if(!encodeLocalResult(*input,*result,frame.payload))throw std::runtime_error("local result wire invalid");return frame;
}

struct Backend
{
 std::atomic_uint calls=0,accounts=0;std::mutex mutex;std::vector<RuntimeJob> jobs;
 Frame run(const RuntimeJob& job){if(const auto* request=std::get_if<ai::GenerationRequest>(&job.input)){++calls;{std::lock_guard lock(mutex);jobs.push_back(job);}if(job.localPreview||job.previewExchange)return generatedLocal(job,*request);Frame f{MessageKind::result,job.session,{}};CHECK(encodeResult(*request,generated(*request),f.payload));return f;}++accounts;Frame f{MessageKind::accountResult,job.session,{}};CHECK(encodeAccountReply(account(),f.payload));return f;}
 std::vector<RuntimeJob> captured(){std::lock_guard lock(mutex);return jobs;}
};
struct Service
{
 Temp temp;juce::String name=juce::Uuid().toString().removeCharacters("-");std::shared_ptr<Backend> backend=std::make_shared<Backend>();std::atomic_bool stop=false,returned=false;std::atomic_int result=-1;RuntimeServer server;std::thread io;
 Service():server([b=backend](const auto& j){return b->run(j);},[](const auto&){}),io([this]{const auto r=server.run(stop,name);result=int(r.status);returned=true;std::cout<<"SERVER_RETURN status="<<int(r.status)<<" retained="<<r.workerRetained<<" stop="<<stop.load()<<'\n';CHECK(!r.workerRetained);if(!stop.load()||r.status!=RuntimeStatus::ready)CHECK(false);})
 {
  auto probe=std::make_unique<CompanionClient>();CHECK(probe->start(options()));CHECK(observe([&]{probe->poll();return probe->status().phase==ClientPhase::ready||returned.load();}));CHECK(probe->status().phase==ClientPhase::ready);CHECK(probe->peer().processId==GetCurrentProcessId()&&probe->peer().creationTime==creation());CHECK(!probe->session().isNull());std::cout<<"STARTUP_WELCOME ready="<<(probe->status().phase==ClientPhase::ready)<<" serverReturned="<<returned.load()<<'\n';probe->close();
 }
 ~Service(){stop=true;io.join();}
 ClientOptions options()const{return{{juce::File{},name,temp.root},false};}
};
// A processor with the AI unit on the desktop, hidden until open() shows it as AI on the session bar does; or with the
// editor, whose AI unit open() shows by pressing AI.
struct Node
{
 std::unique_ptr<InstrumentProcessor> p;FakeServices files;std::unique_ptr<studio::AiUnit> unit;std::unique_ptr<juce::AudioProcessorEditor> editor;std::uint64_t edit=0;juce::AudioBuffer<float> audio{2,512};juce::MidiBuffer midi;
 explicit Node(const ClientOptions& o,bool withEditor=false):p(std::make_unique<InstrumentProcessor>(o, airack::testing::isolatedRackRuntime())){CHECK(airack::testing::readyRackInventory(*p));p->prepareToPlay(48000,512);midi.ensureSize(128);if(withEditor)reopenEditor();else reopen();}
 ~Node(){unit.reset();editor.reset();p->closeAiSession();p->releaseResources();}
 void reopen(){unit=std::make_unique<studio::AiUnit>(*p,*studio::studioSkin(),files.make());unit->addToDesktop(0);progress();}
 void reopenEditor(){editor.reset(p->createEditor());editor->setSize(1080,680);editor->addToDesktop(0);editor->setVisible(true);progress();}
 bool open(){if(editor)return click(*editor,"ai.toggle");unit->setVisible(true);progress();return true;}
 juce::Component& holder(){if(editor)return *editor;return *unit;}
 studio::Screen& screen(){return *screenOf(holder(),"ai.screen");}
 std::unique_ptr<ProjectDocument> snapshot(){auto d=std::make_unique<ProjectDocument>();CHECK(p->tryProjectSnapshot(*d,edit));return d;}
 juce::MemoryBlock saved(){juce::MemoryBlock r;p->getStateInformation(r);return r;}
 bool recall(const ProjectDocument& d){juce::MemoryBlock b;CHECK(encodeProjectState(d,b));return p->restoreProjectState(b.getData(),int(b.getSize()));}
 void callback(){p->processBlock(audio,midi);midi.clear();progress();}
};
bool ready(Node& n){const bool ok=observe([&]{n.p->pollAi();return n.p->groupWorkflowView().generationReady;});CHECK(ok);return ok;}
// The unit's first account reply, with `step` serving a scripted peer meanwhile.
bool signedIn(Node& n,const std::function<void()>& step={}){const bool ok=observe([&]{if(step)step();n.p->pollAi();return n.p->aiAccountReply()&&!n.p->aiWorkflowStatus().accountPending;});CHECK(ok);progress();return ok;}
// A changed intent re-admits the group, which drops the account: SIGN IN asks its status again once the companion is back.
bool signInAgain(Node& n){CHECK(observe([&]{n.p->pollAi();return n.screen().hitFor("ai.account")!=nullptr;}));CHECK(pressItem(n.screen(),"ai.account"));return signedIn(n);}
bool publicAccount(Node& n){CHECK(click(*n.unit,"ai.account"));const bool ok=observe([&]{n.p->pollAi();const auto* a=n.p->aiAccountReply();return a&&a->models.size()==1&&!n.p->aiWorkflowStatus().accountPending;});CHECK(ok);return ok;}
juce::String drawn(studio::Screen& s,const juce::String& key){const auto t=s.texts.find(key);return t==s.texts.end()?juce::String():t->second.text;}
juce::String contextValue(Node& n,const char* label){for(const auto& [field,value]:n.screen().shown.context)if(field==label)return value;return {};}
// Back on the create page by the toggle of the account, linked or join page.
void compose(Node& n){auto& s=n.screen();if(s.page=="connected")CHECK(click(n.holder(),"ai.account"));else if(s.page=="linked"||s.page=="join")CHECK(click(n.holder(),"ai.linked"));}
void context(Node& n,const char* role){compose(n);auto& s=n.screen();CHECK(enterText(s,"ai.role",role));CHECK(enterText(s,"ai.key","A"));CHECK(enterText(s,"ai.scale","minor"));CHECK(enterText(s,"ai.tempo","120"));}
void prompt(Node& n,const char* text){compose(n);CHECK(enterText(n.screen(),"ai.prompt",text));}
// LINKED opens the linked page; from the join page it goes there by the mode's page.
void linkedPage(Node& n){auto& s=n.screen();if(s.page=="linked")return;if(s.page=="join")CHECK(click(n.holder(),"ai.linked"));CHECK(click(n.holder(),"ai.linked"));CHECK(s.page=="linked");}
bool create(Node& n,const juce::String& name){linkedPage(n);const bool ok=enterText(n.screen(),"group.name",name);CHECK(ok);progress();return ok;}
bool browse(Node& n,std::size_t count){linkedPage(n);CHECK(pressItem(n.screen(),"group.refresh"));const bool ok=observe([&]{n.p->pollAi();return !n.p->groupWorkflowView().operationPending&&n.p->groupWorkflowView().members.size()==count;});CHECK(ok);return ok;}
// JOIN lists the discovered groups; the group's row joins it.
bool join(Node& n,GroupUuid group)
{
 linkedPage(n);auto& s=n.screen();CHECK(pressItem(s,"ai.page.join"));CHECK(s.page=="join");const auto shortId=uuid(group).substring(0,8);
 for(int k=0;k<s.shown.list.size();++k)if(s.shown.list[k].contains(shortId)){const bool ok=pressItem(s,"group.join",k);CHECK(ok&&s.page=="linked");return ok;}
 CHECK(false);return false;
}
const studio::PeerRow* peerRow(Node& n,GroupUuid member){for(const auto& r:n.screen().shown.peers)if(r.tip.contains(uuid(member)))return &r;return nullptr;}
bool peerClick(Node& n,GroupUuid member){linkedPage(n);const bool ok=clickHit(n.screen(),hitWithTip(n.screen(),"group.peer",uuid(member)));CHECK(ok);progress();return ok;}
void native_explicit_create_join_resume_leave()
{
 begin(__func__);auto service=std::make_unique<Service>();auto a=std::make_unique<Node>(service->options());CHECK(a->p->groupWorkflowView().connection.phase==ClientPhase::closed);CHECK(service->backend->accounts==0&&service->backend->calls==0);const auto manual=a->snapshot();CHECK(a->open());if(!signedIn(*a))return;
 context(*a,"bass");
 for(const auto& name:{juce::String("   "),juce::String::repeatedString("x",129),juce::String::repeatedString(juce::String::fromUTF8("\xC3\xA9"),65)}){CHECK(create(*a,name));CHECK(!a->p->groupLinkView().intent);CHECK(status(*a->unit).containsIgnoreCase("name"));}
 CHECK(create(*a,"  Explicit band  "));CHECK(ready(*a));const auto first=a->p->groupLinkView();CHECK(first.phase==GroupLinkPhase::admitted&&first.intent&&!first.intent->group.isNull()&&!first.intent->member.isNull());if(!first.intent)return;CHECK(groupTextView(first.intent->name)=="Explicit band");CHECK(groupTextView(first.intent->role)=="bass"&&groupTextView(first.intent->key)=="A"&&first.intent->bpm==120);CHECK(a->snapshot()->sound==manual->sound);CHECK(status(*a->unit).containsIgnoreCase("linked"));
 auto b=std::make_unique<Node>(service->options());CHECK(b->open());CHECK(signedIn(*b));context(*b,"pad");CHECK(browse(*b,1));CHECK(join(*b,first.intent->group));CHECK(ready(*b));const auto second=b->p->groupLinkView();CHECK(second.intent&&second.intent->group==first.intent->group&&second.intent->member!=first.intent->member);if(second.intent)CHECK(groupTextView(second.intent->role)=="pad"&&groupTextView(second.intent->key)=="A"&&second.intent->bpm==120);CHECK(b->snapshot()->sound==manual->sound);CHECK(browse(*a,2));
 auto copied=a->snapshot();auto c=std::make_unique<Node>(service->options());CHECK(c->recall(*copied));CHECK(c->p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(c->p->groupWorkflowView().connection.phase==ClientPhase::closed);CHECK(c->open());CHECK(observe([&]{c->p->pollAi();return c->p->aiWorkflowStatus().connection.phase==ClientPhase::ready&&!c->p->aiWorkflowStatus().accountPending;}));CHECK(c->p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(status(*c->unit).containsIgnoreCase("pending"));
 // RESUME stands for JOIN while the saved link waits.
 linkedPage(*c);auto& cs=c->screen();CHECK(cs.shown.joinLabel=="RESUME"&&cs.hitFor("group.resume")!=nullptr&&drawn(cs,"heading").startsWith("LINK PENDING"));image(*c->unit,"pending-restoration");CHECK(pressItem(cs,"group.resume"));CHECK(ready(*c));const auto normalized=c->p->groupLinkView();CHECK(normalized.intent&&normalized.intent->group==first.intent->group&&normalized.intent->member!=first.intent->member);CHECK(c->snapshot()->sound==copied->sound&&c->snapshot()->pattern==copied->pattern);
 const auto before=c->saved();CHECK(pressItem(cs,"group.leave"));CHECK(observe([&]{c->p->pollAi();return c->p->groupWorkflowView().connection.phase==ClientPhase::closed;}));CHECK(!c->p->groupLinkView().intent);CHECK(c->snapshot()->sound==copied->sound&&c->snapshot()->pattern==copied->pattern);CHECK(c->saved()!=before);CHECK(browse(*a,2));CHECK(service->backend->calls==0);
}
// Scripted restricted native peer supplies only authenticated display summaries;
// this is not a real registry capture or a provider/Apply authority injection.
struct NativePeer
{
 Temp temp;juce::String name=juce::Uuid().toString().removeCharacters("-");PipeListener listener;PipeConnection pipe;juce::Uuid session=juce::Uuid();std::unique_ptr<Node> node;bool reading=false;std::vector<GroupMemberSummary> members;
 explicit NativePeer(bool withEditor=false){CHECK(listener.open(name)==PipeStatus::ready);node=std::make_unique<Node>(ClientOptions{{juce::File{},name,temp.root},false},withEditor);}
 ~NativePeer(){node.reset();pipe.close();listener.close();airack::companion::detail::PipeTestAccess::reapRetired();}
 bool send(const Frame& f){if(pipe.beginWrite(f)!=PipeStatus::ready)return false;PipeStatus s=PipeStatus::pending;return observe([&]{s=pipe.pollWrite();node->p->pollAi();return s!=PipeStatus::pending;})&&s==PipeStatus::complete;}
 // A unit that closes its connection with a command in flight, as joining does, gets no reply: the companion drops
 // that connection, and so does this peer. A unit that failed, or a write that hangs, still fails.
 bool answer(const Frame& f){if(send(f))return true;const auto phase=node->p->groupWorkflowView().connection.phase;return !pipe.connected()&&(phase==ClientPhase::connecting||phase==ClientPhase::closed);}
 void step(){node->p->pollAi();if(!pipe.connected()){if(listener.pollAccept(pipe)!=PipeStatus::complete)return;CHECK(pipe.peer().processId==GetCurrentProcessId()&&pipe.peer().creationTime==creation());}if(!reading){if(pipe.beginRead()!=PipeStatus::ready)return;reading=true;}Frame f;const auto s=pipe.pollRead(f);if(s==PipeStatus::pending)return;reading=false;CHECK(s==PipeStatus::complete);if(s!=PipeStatus::complete)return;
  if(f.kind==MessageKind::hello){ControlMessage h;CHECK(decodeControl(bytes(f.payload),h));Frame r{MessageKind::welcome,session,{}};CHECK(encodeControl(h,r.payload));CHECK(answer(r));}
  else if(f.kind==MessageKind::account){AccountCommand c;CHECK(decodeAccountCommand(bytes(f.payload),c));Frame r{MessageKind::accountResult,session,{}};CHECK(encodeAccountReply(account(),r.payload));CHECK(answer(r));}
  else if(f.kind==MessageKind::groupCommand||f.kind==MessageKind::groupCommandLocal){auto c=std::make_unique<GroupCommand>();
   bool decoded=false;if(f.kind==MessageKind::groupCommandLocal){auto map=std::make_unique<ai::PreviewSampleMap>();decoded=decodeLocalGroupPublication(bytes(f.payload),*c,*map);std::cout<<"SCRIPTED_LOCAL_PUBLICATION kind="<<unsigned(f.kind)<<" action="<<int(c->action)<<" decoded="<<decoded<<std::endl;}
   else decoded=decodeGroupCommand(bytes(f.payload),*c);CHECK(decoded);if(!decoded)return;GroupReply r{c->operationId,c->action,c->runtime,c->loadRevision,GroupReplyStatus::ready,{},0,false,{},c->proposal};if(c->action==GroupAction::admit||c->action==GroupAction::publish){r.normalized=c->publication->project.group;r.registryRevision=1;}if(c->action==GroupAction::browse)r.members=members;Frame reply{MessageKind::groupReply,session,{}};CHECK(encodeGroupReply(r,reply.payload));CHECK(answer(reply));}else{std::cout<<"SCRIPTED_UNHANDLED_KIND kind="<<unsigned(f.kind)<<std::endl;CHECK(false);}
 }
 bool settle(std::size_t count){const bool done=observe([&]{progress();step();return node->p->groupWorkflowView().connection.phase==ClientPhase::ready&&!node->p->groupWorkflowView().operationPending&&node->p->groupWorkflowView().members.size()==count;});CHECK(done);return done;}
 void refresh(){linkedPage(*node);CHECK(pressItem(node->screen(),"group.refresh"));CHECK(settle(members.size()));}
};
GroupMemberSummary summary(unsigned member,unsigned group=90){GroupMemberSummary r;r.runtime=id(member+100);r.loadRevision=2;r.editRevision=7;r.intent=intent(member,group);r.noteAvailability=ObservedMidiAvailability::fresh;r.noteReason=ObservedMidiReason::none;r.noteCount=2;r.noteAgeMs=120;r.controlAvailability=ObservedMidiAvailability::fresh;r.controlReason=LiveControlReason::none;r.controlAgeMs=15;return r;}
// Joining closes the unit's connection with a refresh in flight; the peer reads the refresh after the unit has gone,
// and the unit comes back on its next connection.
void command_from_a_unit_that_left()
{
 begin(__func__);auto peer=std::make_unique<NativePeer>();auto& n=*peer->node;const auto step=[&]{peer->step();};CHECK(n.open());if(!signedIn(n,step))return;
 peer->members.push_back(summary(1));peer->refresh();context(n,"lead");
 linkedPage(n);CHECK(pressItem(n.screen(),"group.refresh"));n.p->pollAi();CHECK(n.p->groupWorkflowView().operationPending);
 CHECK(join(n,id(90)));n.p->pollAi();CHECK(n.p->groupWorkflowView().connection.phase!=ClientPhase::ready);
 CHECK(observe([&]{peer->step();return n.p->groupWorkflowView().generationReady;}));
 CHECK(n.p->groupLinkView().intent&&n.p->groupLinkView().intent->group==id(90));
}
void ordered_selection_conflict_and_unavailable_display()
{
 begin(__func__);auto peer=std::make_unique<NativePeer>();auto& n=*peer->node;const auto step=[&]{peer->step();};CHECK(n.open());if(!signedIn(n,step))return;for(unsigned i=1;i<=6;++i)peer->members.push_back(summary(i));peer->members.push_back(summary(8,91));peer->members[1].mode=PlaybackMode::internalPattern;peer->members[1].noteAvailability=ObservedMidiAvailability::unavailable;peer->members[1].noteReason=ObservedMidiReason::internalMode;peer->members[1].noteCount=0;peer->members[1].noteAgeMs=0;peer->members[2].noteAvailability=ObservedMidiAvailability::stale;peer->members[2].noteReason=ObservedMidiReason::stalePublisher;peer->members[2].noteAgeMs=3100;peer->members[2].notesTruncated=true;peer->members[3].controlAvailability=ObservedMidiAvailability::unavailable;peer->members[3].controlReason=LiveControlReason::invalidRouting;peer->members[3].controlAgeMs=0;
 // The join page lists each discovered group once; reordered discovery still joins the row's group.
 peer->refresh();auto& s=n.screen();CHECK(pressItem(s,"ai.page.join"));CHECK(s.page=="join"&&s.shown.list.size()==2);if(s.shown.list.size()==2)CHECK(s.shown.list[0]!=s.shown.list[1]);
 std::reverse(peer->members.begin(),peer->members.end());peer->refresh();context(n,"lead");CHECK(join(n,id(90)));CHECK(observe([&]{peer->step();return n.p->groupWorkflowView().generationReady;}));CHECK(n.p->groupLinkView().intent&&n.p->groupLinkView().intent->group==id(90));CHECK(n.p->aiMusicalContext().role=="lead");
 // M1: public Join has closed the preceding account owner, without changing readiness.
 CHECK(observe([&]{peer->step();const auto st=n.p->aiWorkflowStatus();return n.p->groupWorkflowView().generationReady&&st.connection.phase==ClientPhase::ready&&!st.accountPending&&!n.p->aiAccountReply()&&n.p->aiProposalPhase()==AiProposalPhase::idle;}));
 const auto joinedAccountStatus=n.p->aiWorkflowStatus();
 CHECK(joinedAccountStatus.connection.phase==ClientPhase::ready&&!joinedAccountStatus.accountPending);
 CHECK(!n.p->aiAccountReply()&&n.p->aiProposalPhase()==AiProposalPhase::idle);
 CHECK(joinedAccountStatus.message=="Connected; model compatibility verified");
 // No model is chosen, and GENERATE says why.
 auto* joinedModel=widget<studio::Readout>(*n.unit,"ai.model");if(joinedModel)CHECK(joinedModel->text.text=="NO MODEL");
 compose(n);CHECK(click(*n.unit,"ai.generate"));CHECK(n.screen().shown.status=="Sign in and connect to generate"&&!n.p->aiCurrentRequest());
 CHECK(observe([&]{peer->step();return status(*n.unit,"ai.status").containsIgnoreCase("Account");}));
 const std::array<GroupUuid,4> chosen{id(3),id(1),id(4),id(2)};for(const auto member:chosen)CHECK(peerClick(n,member));const auto selection=n.p->groupWorkflowView().selection;CHECK(selection.peerCount==4);for(std::size_t i=0;i<4;++i)CHECK(selection.members[i]==chosen[i]);CHECK(std::count_if(n.screen().shown.peers.begin(),n.screen().shown.peers.end(),[](const auto& r){return r.selected;})==4);CHECK(peerClick(n,id(5)));CHECK(n.p->groupWorkflowView().selection==selection);CHECK(status(*n.unit).contains("4")||status(*n.unit).containsIgnoreCase("four"));CHECK(drawn(n.screen(),"body.0")=="Select at most 4 peers; remove one before adding another");image(*n.unit,"selection-limit-same-name");
 std::reverse(peer->members.begin(),peer->members.end());peer->refresh();CHECK(n.p->groupWorkflowView().selection==selection);for(const auto member:chosen){const auto* r=peerRow(n,member);CHECK(r&&r->selected);}
 const auto* external=peerRow(n,id(1));const auto* internal=peerRow(n,id(2));const auto* stale=peerRow(n,id(3));const auto* unavailable=peerRow(n,id(4));
 if(external)CHECK(external->tip.containsIgnoreCase("External")&&external->tip.contains("120")&&external->tip.containsIgnoreCase("fresh")&&external->source=="EXTERNAL MIDI"&&external->state=="NOTES FRESH  CONTROLS FRESH");
 if(internal)CHECK(internal->tip.containsIgnoreCase("Internal")&&internal->tip.containsIgnoreCase("unavailable")&&internal->source=="INTERNAL PATTERN");
 if(stale)CHECK(stale->tip.containsIgnoreCase("stale")&&stale->tip.contains("3100")&&stale->tip.containsIgnoreCase("truncat"));
 if(unavailable)CHECK(unavailable->tip.containsIgnoreCase("unavailable")&&unavailable->tip.containsIgnoreCase("routing"));image(*n.unit,"expanded-peers-source-age");
 peer->members.erase(std::remove_if(peer->members.begin(),peer->members.end(),[](const auto& m){return m.intent.member==id(1);}),peer->members.end());peer->refresh();const auto* missing=peerRow(n,id(1));CHECK(missing&&missing->selected);if(missing)CHECK((missing->tip.containsIgnoreCase("missing")||missing->tip.containsIgnoreCase("unavailable"))&&missing->role=="MISSING PEER");CHECK(n.p->groupWorkflowView().selection==selection&&!n.p->groupWorkflowView().generationReady);image(*n.unit,"missing-selected-peer");CHECK(peerClick(n,id(1)));const auto removed=n.p->groupWorkflowView().selection;CHECK(removed.peerCount==3&&removed.members[0]==id(3)&&removed.members[1]==id(4)&&removed.members[2]==id(2));
 peer->members[0].busy=true;peer->members[0].conflicted=true;peer->refresh();CHECK(status(*n.unit).containsIgnoreCase("conflict"));const auto& peers=n.screen().shown.peers;CHECK(std::any_of(peers.begin(),peers.end(),[](const auto& r){return r.tip.containsIgnoreCase("busy")&&r.tip.containsIgnoreCase("conflict")&&r.state=="CONFLICTING SETTINGS";}));image(*n.unit,"conflict-display");
 peer->members.clear();for(unsigned i=30;i<46;++i)peer->members.push_back(summary(i));peer->refresh();CHECK(n.screen().shown.peers.size()==19);CHECK(n.p->groupWorkflowView().selection==removed);
}
void closed_editor_context_and_requester_apply()
{
 begin(__func__);auto service=std::make_unique<Service>();auto a=std::make_unique<Node>(service->options());auto b=std::make_unique<Node>(service->options());ObserveTargetScope observeScope(*a->p);CHECK(a->open());CHECK(b->open());if(!signedIn(*a)||!signedIn(*b))return;
 context(*a,"bass");context(*b,"pad");
 CHECK(create(*a,"Apply ensemble"));CHECK(ready(*a));const auto group=a->p->groupLinkView().intent->group;CHECK(browse(*b,1));CHECK(join(*b,group));CHECK(ready(*b));CHECK(browse(*a,2));const auto member=b->p->groupLinkView().intent->member;CHECK(peerClick(*a,member));CHECK(publicAccount(*a));CHECK(ready(*a));const auto peerBytes=b->saved();const auto peerDocument=b->snapshot();const auto peerEdit=b->edit;const auto original=a->snapshot();const auto originalEdit=a->edit;
 prompt(*a,"normal");CHECK(observe([&]{a->p->pollAi();const auto* r=a->p->aiAccountReply();return r&&r->selectedModel.isNotEmpty()&&!a->p->aiWorkflowStatus().accountPending&&a->p->groupWorkflowView().generationReady;}));CHECK(clickDuringPublication(*a->p,*a->unit,"ai.generate","Generate waits for the group heartbeat"));CHECK(observe([&]{a->p->pollAi();return a->p->aiProposalPhase()==AiProposalPhase::proposed;}));CHECK(observe([&]{a->p->pollAi();return a->p->groupWorkflowView().peerContextCurrent;}));const auto jobs=service->backend->captured();CHECK(jobs.size()==1);if(!jobs.empty())CHECK(jobs[0].groupContext&&jobs[0].groupContext->ownership.peers.size()==1&&jobs[0].groupContext->ownership.peers[0].project.group->member==member);CHECK(a->saved()!=juce::MemoryBlock{});CHECK(a->snapshot()->sound==original->sound);auto* diff=widget<juce::TextEditor>(*a->unit,"ai.diff");if(diff)CHECK(diff->getText().contains("output.gain_db")&&diff->getText().containsIgnoreCase("automation"));
 CHECK(click(*a->unit,"ai.preview"));a->callback();CHECK(a->p->aiPreviewPlaying());CHECK(click(*a->unit,"ai.stop"));a->callback();CHECK(observe([&]{return a->p->aiPreviewStopped();}));CHECK(!a->p->applyAiProposal());CHECK(click(*a->unit,"ai.apply"));CHECK(observe([&]{a->p->pollAi();return a->p->aiProposalPhase()==AiProposalPhase::idle&&!a->p->aiWorkflowStatus().applyPending;}));CHECK(a->snapshot()->sound.values[index("output.gain_db")]==.35f&&a->edit==originalEdit+1);CHECK(b->saved()==peerBytes&&b->snapshot()->sound==peerDocument->sound&&b->edit==peerEdit);CHECK(a->p->undoSoundEdit());CHECK(a->snapshot()->sound==original->sound&&b->saved()==peerBytes);
 CHECK(ready(*a));prompt(*a,"parent");CHECK(stepTo(a->screen(),"ai.scope",2));CHECK(observe([&]{a->p->pollAi();return a->p->groupWorkflowView().generationReady;}));CHECK(clickDuringPublication(*a->p,*a->unit,"ai.generate","Generate waits for the group heartbeat"));GroupObservationDiagnostic parentDiagnostic{"parent_proposal_current"};const bool parentReady=observe([&]{a->p->pollAi();const bool ready=a->p->aiProposalPhase()==AiProposalPhase::proposed&&a->p->groupWorkflowView().peerContextCurrent;parentDiagnostic.sample(*a->p,ready);return ready;});CHECK(parentReady);parentDiagnostic.dump(*a->p,parentReady);const auto parent=a->p->aiCurrentRequest()->tag;prompt(*a,"child");CHECK(observe([&]{a->p->pollAi();return a->p->groupWorkflowView().generationReady;}));CHECK(clickDuringPublication(*a->p,*a->unit,"ai.refine","Refine waits for the group heartbeat"));GroupObservationDiagnostic childDiagnostic{"child_proposal_current"};const bool childReady=observe([&]{a->p->pollAi();const bool ready=a->p->aiProposalPhase()==AiProposalPhase::proposed&&a->p->groupWorkflowView().peerContextCurrent;childDiagnostic.sample(*a->p,ready);return ready;});CHECK(childReady);childDiagnostic.dump(*a->p,childReady);auto childJobs=service->backend->captured();CHECK(childJobs.size()==3);if(childJobs.size()==3){const auto& child=std::get<ai::GenerationRequest>(childJobs[2].input);CHECK(!child.policy.requirePattern&&child.policy.scope==ai::RequestScope::both);CHECK(childJobs[2].groupContext&&childJobs[1].groupContext&&childJobs[2].groupContext->ownership==childJobs[1].groupContext->ownership&&childJobs[2].groupContext->requesterEditRevision==childJobs[1].groupContext->requesterEditRevision&&childJobs[2].groupContext->peerCount==childJobs[1].groupContext->peerCount);CHECK(child.tag.requestId>parent.requestId);}CHECK(b->saved()==peerBytes);
 CHECK(click(*a->unit,"ai.discard"));const auto linked=a->p->groupLinkView();a->unit.reset();CHECK(a->p->groupLinkView().phase==GroupLinkPhase::admitted);CHECK(b->p->beginParameterGesture(index("output.gain_db")));CHECK(b->p->setParameterGestureValue(index("output.gain_db"),.2f));CHECK(b->p->endParameterGesture(index("output.gain_db")));CHECK(observe([&]{a->p->pollAi();b->p->pollAi();const auto v=a->p->groupWorkflowView();return std::any_of(v.members.begin(),v.members.end(),[&](const auto& m){return m.intent.member==member&&m.editRevision>peerEdit;});}));CHECK(a->p->groupLinkView().runtime==linked.runtime&&a->p->groupLinkView().loadRevision==linked.loadRevision);
 // A new unit takes the processor's context; an invalid tempo stays in its field while valid context syncs.
 a->reopen();CHECK(a->open());progress();CHECK(contextValue(*a,"ROLE")=="bass");AiMusicalContext changed{"lead","A","minor",120};CHECK(a->p->setAiMusicalContext(changed));CHECK(observe([&]{a->p->pollAi();return contextValue(*a,"ROLE")=="lead";}));const auto context=a->p->aiMusicalContext();CHECK(signInAgain(*a));compose(*a);CHECK(enterText(a->screen(),"ai.tempo","invalid"));CHECK(a->p->aiMusicalContext()==context);CHECK(status(*a->unit,"ai.status").containsIgnoreCase("tempo"));changed.role="lead2";CHECK(a->p->setAiMusicalContext(changed));CHECK(observe([&]{a->p->pollAi();return a->p->aiMusicalContext().role=="lead2";}));CHECK(contextValue(*a,"BPM")=="invalid");CHECK(status(*a->unit,"ai.status").containsIgnoreCase("tempo"));CHECK(signInAgain(*a));compose(*a);CHECK(enterText(a->screen(),"ai.tempo","120"));CHECK(a->p->aiMusicalContext().auditionBpm==120);CHECK(!status(*a->unit,"ai.status").containsIgnoreCase("invalid tempo"));CHECK(b->snapshot()->pattern==peerDocument->pattern);
 // Actual UI guarding must not misclassify a low-level standalone proposal: AUDITION and APPLY act on it.
 linkedPage(*a);CHECK(pressItem(a->screen(),"group.leave"));a->p->pollAi();CHECK(a->p->setGroupIntent(intent(20)));CHECK(publicAccount(*a));auto request=InstrumentGroupTestAccess::beginHistorical25(*a->p,{"low-level","synthetic-model",{}});CHECK(request);if(request){CHECK(a->p->receiveAiResult(generated(*request)));a->p->pollAi();progress();CHECK(click(*a->unit,"ai.preview"));CHECK(a->p->aiPreviewPlaying());CHECK(click(*a->unit,"ai.apply"));CHECK(a->p->aiWorkflowStatus().applyPending);CHECK(!a->p->groupWorkflowView().peerContextCurrent);}a->p->discardAiProposal();
}
// The editor's AI unit at the minimum 1080 x 680 and at 1600 x 1200: inside the window at the stage's scale, the linked
// page's controls on its glass, and its list scrolling to the last of 16 peers.
void minimum_and_large_editor_layout()
{
 begin(__func__);auto peer=std::make_unique<NativePeer>(true);auto& n=*peer->node;const auto step=[&]{peer->step();};auto& e=*n.editor;
 CHECK(e.getWidth()==1080&&e.getHeight()==680);CHECK(n.open());if(!signedIn(n,step))return;
 auto* unit=find(e,"ai.panel");if(!unit){CHECK(false);return;}
 for(unsigned i=30;i<46;++i)peer->members.push_back(summary(i));
 peer->refresh();context(n,"lead");CHECK(join(n,id(90)));CHECK(observe([&]{peer->step();return n.p->groupWorkflowView().generationReady;}));peer->refresh();
 auto& s=n.screen();CHECK(s.page=="linked"&&s.shown.peers.size()==16);
 // Dispatch can start a publication; settle must answer it before returning.
 InstrumentGroupTestAccess::pausePolling(*n.p);CHECK(peer->settle(16));
 bool dispatched=false;
 juce::MessageManager::callAsync([&]{dispatched=InstrumentGroupTestAccess::pauseWithPublication(*n.p)!=0;});
 CHECK(peer->settle(16));CHECK(dispatched&&!n.p->groupWorkflowView().operationPending);
 CHECK(peer->settle(16));InstrumentGroupTestAccess::resumePolling(*n.p);
 for(const auto size:{juce::Point<int>{1080,680},juce::Point<int>{1600,1200}})
 {
  // The scripted peer answers only when stepped, and the unit's client fails a group operation unanswered for 5 s:
  // the heartbeat this progress() can send is answered before the rest of the size's work, which runs no messages.
  e.setSize(size.x,size.y);progress();CHECK(peer->settle(16));CHECK(e.getWidth()==size.x&&e.getHeight()==size.y);
  const auto k=std::min(size.x/1547.0f,size.y/965.0f);const auto shown=e.getLocalArea(unit,unit->getLocalBounds().toFloat());
  CHECK(std::abs(shown.getWidth()-401.0f*k)<=1.0f&&e.getLocalBounds().toFloat().expanded(1.0f).contains(shown));
  for(int i=0;i<64&&s.scrollOf(s.page)>0;++i)screendriver::wheel(s,1.0f);
  for(const char* item:{"group.refresh","ai.page.join","group.leave"}){const auto* h=s.hitFor(item);CHECK(h&&s.getLocalBounds().toFloat().contains(h->r));}
  image(e,size.x==1080?"minimum-editor-1080x680":"large-editor-1600x1200");
  if(size.x==1080)image(*unit,"unit-linked");
  const auto ticked=n.p->groupWorkflowView().selection.peerCount;CHECK(!n.p->groupWorkflowView().operationPending);CHECK(peerClick(n,id(45)));CHECK(s.scrollOf("linked")>0&&n.p->groupWorkflowView().selection.peerCount!=ticked);
 }
 CHECK(airack::companion::detail::PipeTestAccess::retainedOwners()==0);CHECK(airack::companion::detail::RuntimeServerTestAccess::retainedWorkers()==0);
}
// The linked page: LEAVE inert and LINKED dark while unlinked; CREATE names a group over the heading and LINKED lights
// once it is admitted; LEAVE unlinks; REFRESH then JOIN lists the groups and a row joins one; and RESUME stands for
// JOIN while a restored link waits. The peers' ticks are checked with the scripted peer above.
void side_linked_page()
{
 begin(__func__);auto service=std::make_unique<Service>();auto a=std::make_unique<Node>(service->options());auto b=std::make_unique<Node>(service->options());CHECK(a->open());CHECK(b->open());if(!signedIn(*a)||!signedIn(*b))return;
 context(*a,"lead");context(*b,"pad");linkedPage(*a);auto& s=a->screen();auto* linked=widget<studio::Target>(*a->unit,"ai.linked");if(!linked)return;
 CHECK(drawn(s,"heading")=="NOT LINKED"&&s.hitFor("group.leave")==nullptr&&linked->litLayer().isEmpty());
 CHECK(pressItem(s,"group.name"));CHECK(s.entry.isVisible()&&s.entry.getComponentID()=="group.name");s.entry.setText("Evening",false);if(s.entry.onReturnKey)s.entry.onReturnKey();
 CHECK(ready(*a));CHECK(drawn(s,"heading")=="LINKED: EVENING"&&linked->litLayer()=="lit"&&s.hitFor("group.leave")!=nullptr);
 CHECK(pressItem(s,"group.leave"));CHECK(observe([&]{a->p->pollAi();return !a->p->groupLinkView().intent;}));CHECK(drawn(s,"heading")=="NOT LINKED"&&s.hitFor("group.leave")==nullptr&&linked->litLayer().isEmpty());
 CHECK(create(*b,"Same band"));CHECK(ready(*b));if(!b->p->groupLinkView().intent)return;const auto group=b->p->groupLinkView().intent->group;
 CHECK(browse(*a,1));CHECK(pressItem(s,"ai.page.join"));CHECK(s.page=="join");const auto shortId=uuid(group).substring(0,8);
 CHECK(std::count_if(s.shown.list.begin(),s.shown.list.end(),[&](const juce::String& row){return row=="Same band ("+shortId+")";})==1);
 CHECK(join(*a,group));CHECK(ready(*a));CHECK(drawn(s,"heading")=="LINKED: SAME BAND"&&linked->litLayer()=="lit");
 const auto saved=a->snapshot();CHECK(a->recall(*saved));CHECK(a->p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(observe([&]{a->p->pollAi();return s.shown.joinLabel=="RESUME";}));
 CHECK(drawn(s,"button.1")=="RESUME"&&drawn(s,"heading")=="LINK PENDING: SAME BAND"&&linked->litLayer().isEmpty());
 CHECK(pressItem(s,"group.resume"));CHECK(ready(*a));CHECK(drawn(s,"heading")=="LINKED: SAME BAND"&&s.shown.joinLabel=="JOIN"&&linked->litLayer()=="lit");
}
void posted_click_behind_slow_message()
{
 begin(__func__);CHECK(clickRunsBehindSlowMessage([](juce::Button& b){click(b);}));
}
}
int main()
{
 juce::ScopedJuceInitialiser_GUI gui;std::cout<<std::unitbuf;std::cerr<<std::unitbuf;std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<creation()<<'\n';native_explicit_create_join_resume_leave();command_from_a_unit_that_left();ordered_selection_conflict_and_unavailable_display();closed_editor_context_and_requester_apply();minimum_and_large_editor_layout();side_linked_page();posted_click_behind_slow_message();CHECK(sections==7);std::cout<<"GroupPanel checks="<<checks.load()<<" sections="<<sections<<" failures="<<failures.load()<<'\n';return failures?1:0;
}

#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "OwnedFixture.h"
#include "InstrumentProcessor.h"
#include "BlankInstance.h"
#include "AiUnit.h"
#include "CompanionRuntimeServer.h"
#include "CompanionBackend.h"
#include "LocalPreviewExchange.h"
#include "StrictJson.h"
#include "GroupRegistry.h"
#include "RackLibrary.h"
#include "RackExamples.h"
#include "ScreenDriver.h"
#include "StudioSkin.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_gui_extra/juce_gui_extra.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#include <atomic>
#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>

using namespace airack;
using namespace airack::companion;
namespace airack {
struct InstrumentAiTestAccess {
 static std::optional<ai::GenerationRequest> beginHistorical25(InstrumentProcessor& p,const AiRequestOptions& options,std::optional<double> bpm)
 try {
  p.refreshAiProposal();auto factory=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
  std::erase_if(factory,[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});});
  if(factory.size()!=26||std::count_if(factory.begin(),factory.end(),[&](const auto& design){return design.id==tile.id&&design.revision==tile.revision;})!=1)return {};
  std::erase_if(factory,[&](const auto& design){return design.id==tile.id;});if(factory.size()!=25)return {};
  auto inventory=std::make_shared<RackCatalogSnapshot>();
  {const std::lock_guard lock(p.restoreMutex);auto source=std::make_unique<ProjectDocument>();std::uint64_t edit;
   if(!p.tryProjectSnapshotOwned(*source,edit))return {};std::vector<RackDesign> embedded;
   if(source->sound.personalities)for(const auto& instance:source->sound.personalities->instances)embedded.push_back(instance.design);
   if(!captureRackCatalog(factory,embedded,{},*inventory))return {};}
  return p.beginAiRequestWithCatalog(options,bpm,std::move(inventory));
 }catch(const std::bad_alloc&){return {};}

};
}
#ifndef NDEBUG
namespace airack {
struct InstrumentStructuralTestAccess {
 static std::uint64_t sourceWord(const InstrumentProcessor& p){return p.structuralGeneration.load();}
 static std::array<std::uint64_t,3> authority(const InstrumentProcessor& p){return {p.editRevision.load(),p.aiContextRevision,p.resetGeneration.load()};}
 static bool marker(const InstrumentProcessor& p,const ai::RequestTag& tag){return p.groupControl->marker&&p.groupControl->marker->tag==tag;}
 static bool parent(const InstrumentProcessor& p,const ai::RequestTag& tag){return p.groupControl->parent&&p.groupControl->parent->tag==tag;}
 static bool owned(const InstrumentProcessor& p){return p.groupControl->marker&&p.groupMarkerCurrentOwned(*p.groupControl->marker);}
 static bool applying(const InstrumentProcessor& p){return p.aiApplying;}
};
}
#endif
namespace
{
std::atomic_int checks=0,failures=0;int sections=0;const char* section="";
void check(bool pass,const char* expression,int line){++checks;if(!pass){++failures;if(failures<120)std::cerr<<section<<':'<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){section=name;++sections;std::cout<<name<<'\n';}
// Actual local completion is delivered by the existing control-pump timer.
// Dispatch ordinary GUI messages only after the predicate releases its locks;
// deliberate native-peer ACK/validation/publication holds remain unchanged.
template<class F>bool observe(F f,std::uint64_t ms=5000){const auto end=GetTickCount64()+ms;do{if(f())return true;juce::MessageManager::getInstance()->runDispatchLoopUntil(1);SwitchToThread();}while(GetTickCount64()<end);return false;}
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
std::uint64_t creation(){FILETIME a{},b{},c{},d{};CHECK(GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d));return(std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
bool selfPeer(PipePeer p){return p.processId==GetCurrentProcessId()&&p.creationTime==creation();}
GroupUuid id(unsigned n){GroupUuid r;r.bytes[0]=std::uint8_t(n);r.bytes[15]=0x71;return r;}
GroupText text(std::string_view s){GroupText r;CHECK(setGroupText(s,r));return r;}
GroupIntent intent(unsigned member=1,unsigned group=90){GroupIntent r;r.group=id(group);r.member=id(member);r.name=text("Explicit ensemble");r.role=text("bass");r.key=text("A");r.scale=text("minor");r.bpm=120;CHECK(validateGroupIntent(r));return r;}
std::size_t index(const char* name){const auto reg=parameterRegistry();const auto p=std::find_if(reg.begin(),reg.end(),[&](const auto& d){return d.id==name;});CHECK(p!=reg.end());return p==reg.end()?0:std::size_t(p-reg.begin());}
struct Temp
{
 juce::File parent=juce::File(RACKSYNTH_WORKSPACE).getChildFile(".superpowers/sdd/2026-10-06-m5-plugin-group-workflow/task-1/fixtures");juce::File root=parent.getChildFile(juce::Uuid().toString());
 Temp(){CHECK(root.isAChildOf(parent)&&root.getParentDirectory()==parent);CHECK(root.createDirectory().wasOk());}
 ~Temp(){if(root.isAChildOf(parent)&&root.getParentDirectory()==parent)CHECK(root.deleteRecursively());}
};
struct Head final:juce::AudioPlayHead
{
 bool available=true,playing=true;std::int64_t sample=0;
 juce::Optional<PositionInfo> getPosition() const override{if(!available)return {};PositionInfo p;p.setIsPlaying(playing);p.setBpm(120);p.setPpqPosition(sample/24000.);p.setTimeInSamples(sample);p.setTimeSignature(TimeSignature{4,4});return p;}
};
// Only scenarios requiring the complete installed catalog own this explicit fixture.
struct InstalledCatalogFixture
{
 std::shared_ptr<airack::testing::IsolatedRackRuntime> runtime=airack::testing::isolatedRackRuntime();
 const juce::File root=runtime->root;bool ready=false;
 InstalledCatalogFixture(){
  const juce::File directory(RACKSYNTH_GROUP_RACK_FIXTURE_DIR);const auto catalog=juce::JSON::parse(directory.getChildFile("catalog.json"));
  const auto* racks=catalog["racks"].getArray();CHECK(racks&&racks->size()==11);if(!racks||racks->size()!=11)return;
  for(const auto& row:*racks){const auto ref=row["releases"][0]["ref"];RackPackageRef exact{ref["id"].toString(),ref["version"].toString(),ref["sha256"].toString()};
   const auto archive=directory.getChildFile(exact.id+"-"+exact.version+".airrack");
   const auto status=runtime->store->install(archive,exact,RackPackageActivation::makeActive,{});CHECK(status==RackLibraryStatus::ready);if(status!=RackLibraryStatus::ready)return;
  }
  ready=runtime->store->active().size()==11;CHECK(ready);std::cout<<"INSTALLED_CATALOG root="<<root.getFullPathName()<<" active="<<runtime->store->active().size()<<'\n';
 }
 ~InstalledCatalogFixture(){
  CHECK(runtime.use_count()==1);CHECK(runtime->gets->load()==0);runtime.reset();CHECK(!root.exists());
  std::cout<<"INSTALLED_CATALOG_RELEASE root="<<root.getFullPathName()<<" removed="<<!root.exists()<<'\n';
 }
};
struct Node
{
 Head head;std::unique_ptr<InstrumentProcessor> p;std::uint64_t edit=0,reset=0;
 explicit Node(const ClientOptions& o,std::shared_ptr<RackLibraryRuntime> runtime=airack::testing::isolatedRackRuntime()):p(std::make_unique<InstrumentProcessor>(o,std::move(runtime))){CHECK(blankInstance(*p));CHECK(airack::testing::readyRackInventory(*p));p->setPlayHead(&head);}
 ~Node(){p->setPlayHead(nullptr);p->closeAiSession();}
 std::unique_ptr<ProjectDocument> snapshot(){auto d=std::make_unique<ProjectDocument>();CHECK(p->tryProjectSnapshot(*d,edit,reset));return d;}
 juce::MemoryBlock saved(){juce::MemoryBlock b;p->getStateInformation(b);return b;}
 bool recall(const ProjectDocument& d){juce::MemoryBlock b;CHECK(encodeProjectState(d,b));return p->restoreProjectState(b.getData(),int(b.getSize()));}
 bool set(unsigned member=1,unsigned group=90){return p->setGroupIntent(intent(member,group));}
 AiRequestOptions options(std::string instruction="normal"){return {std::move(instruction),"synthetic-model",{ai::RequestScope::both,false,false,false}};}
 bool generate(const AiRequestOptions& o)
 {
  if(airack::testing::generateAi(*p,o))return true;
  const auto v=p->groupWorkflowView();std::cerr<<"DIAG generate refused: "<<p->aiWorkflowStatus().message<<" | ready="<<v.generationReady<<" busy="<<v.busy<<" pending="<<v.operationPending<<" conflicted="<<v.conflicted<<" connection="<<int(v.connection.phase)<<" link="<<int(v.link.phase)<<" peers="<<int(v.selection.peerCount);
  for(const auto& m:v.members)std::cerr<<" member(busy="<<m.busy<<" conflicted="<<m.conflicted<<" edit="<<m.editRevision<<')';
  std::cerr<<" group="<<v.message<<'\n';return false;
 }
};
AccountReply accountResult(){AccountReply r;r.providerStatus=rack::oauth::Status::connected;r.httpStatus=200;r.selectedModel="synthetic-model";r.modelVerified=true;r.models.push_back({"synthetic-model","Synthetic fixture"});return r;}
// Only the dedicated catalog-witness instructions use this real native generator.
ai::GenerationResult generatedCatalogWitness(const ai::GenerationRequest& request)
{
 CHECK(request.catalog); if(!request.catalog)return {};
 const auto design=airack::test::frozenFactoryRackDesigns().front();juce::MemoryBlock canonical;
 CHECK(design.name=="Copper Arc"&&encodeRackDesign(design,canonical));
 const bool child=request.instruction=="catalog-witness-child";unsigned calls=0;
 auto response=juce::JSON::parse(R"json({"version":2,"parameter_edits":[{"id":"global.master_level","normalized":0.1}],"rack_edits":[],"macro_edits":[{"index":1,"target_id":"catalog:7:0:analog.filter.cutoff","amount":0.25}],"pattern":{"bars":1,"meter_numerator":1,"meter_denominator":4,"grid":"sixteenth","swing":0,"notes":[{"start_beats":0,"duration_beats":0.05,"pitch":67,"velocity":70,"channel":1}]},"characteristics":"Grouped selected voice","explanation":"Retain the captured peer ownership and aggregate intent","name":"Grouped Test Voice","reconciliation":{"detach":[],"remove":[],"replace":[],"controls":[]},"catalog_selections":[{"local_id":7,"design":"91387715-13d3-4c57-a479-12251842a401","revision":"1","content_sha256":"","destination":{"role":"synth","chain":4,"position":1}}],"catalog_parameter_edits":[{"selection":7,"unit":0,"key":"analog.filter.cutoff","normalized":0.25}]})json");
 auto& selection=response["catalog_selections"].getArray()->getReference(0);
 selection.getDynamicObject()->setProperty("design",design.id.toDashedString());
 selection.getDynamicObject()->setProperty("revision",juce::String(std::to_string(design.revision)));
 selection.getDynamicObject()->setProperty("content_sha256",juce::SHA256(canonical).toHexString());
 if(child){response.getDynamicObject()->setProperty("parameter_edits",juce::Array<juce::var>());response.getDynamicObject()->setProperty("macro_edits",juce::Array<juce::var>());response.getDynamicObject()->setProperty("pattern",juce::var());response["catalog_parameter_edits"].getArray()->getReference(0).getDynamicObject()->setProperty("normalized",.35);}
 auto result=ai::generateProposal(request,[&](const juce::String&,std::string_view,const juce::var&){
  ++calls;CHECK(request.catalog->phase==ai::CatalogPhase::select);rack::oauth::StructuredResult reply;reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;reply.text=juce::JSON::toString(response,true,22);return reply;
 });
 CHECK(calls==(request.catalog->phase==ai::CatalogPhase::materialize?0u:1u));
 CHECK(result.status==(request.catalog->phase==ai::CatalogPhase::select&&!child?ai::GenerationStatus::catalogSelectionRequired:ai::GenerationStatus::ready));
 return result;
}

ai::GenerationResult generated(const ai::GenerationRequest& request)
{
 if(request.instruction.starts_with("catalog-witness"))return generatedCatalogWitness(request);
 ai::GenerationResult r;r.tag=request.tag;r.inferenceCalls=1;
 if(request.instruction=="fail"){r.status=ai::GenerationStatus::providerFailure;r.providerStatus=rack::oauth::Status::networkFailure;r.httpStatus=503;r.errorCode="synthetic_failure";return r;}
 if(request.catalog){r.edits=request.catalog->baseIntent;r.catalog=ai::CatalogResultBinding{request.catalog->root,request.catalog->authority->snapshot};}
 r.status=ai::GenerationStatus::ready;r.providerStatus=rack::oauth::Status::connected;r.httpStatus=200;r.proposalStatus=ai::ProposalStatus::accepted;r.renderStatus=ai::RenderStatus::ready;
 {const float gain=request.instruction=="child"?.5f:.4f;auto it=std::find_if(r.edits.parameters.begin(),r.edits.parameters.end(),[](const auto& e){return e.id=="output.gain_db";});if(it==r.edits.parameters.end())r.edits.parameters.push_back({"output.gain_db",gain});else it->normalized=gain;}
 auto d=std::make_unique<ProjectDocument>();CHECK((request.catalog?ai::admitProposal(request.source,request.policy,r.edits,*request.catalog->authority,*d):ai::admitProposal(request.source,request.policy,r.edits,*d))==ai::ProposalStatus::accepted);r.candidate=*d;
 r.audio.bodyFrames=1;r.audio.left.resize(ai::previewTailFrames+1);r.audio.right.resize(r.audio.left.size());r.audio.left[0]=.1f;r.audio.right[0]=.05f;CHECK(ai::measurePreview(r.audio.left,r.audio.right,r.audio.bodyFrames,r.measurements)==ai::RenderStatus::ready);return r;
}
juce::var groupedResponse(const ai::GenerationRequest& request)
{
 if(request.instruction.starts_with("catalog-witness")){
 const auto design=airack::test::frozenFactoryRackDesigns().front();juce::MemoryBlock canonical;
 CHECK(design.name=="Copper Arc"&&encodeRackDesign(design,canonical));
 const bool child=request.instruction=="catalog-witness-child";
 auto response=juce::JSON::parse(R"json({"version":2,"parameter_edits":[{"id":"global.master_level","normalized":0.1}],"rack_edits":[],"macro_edits":[{"index":1,"target_id":"catalog:7:0:analog.filter.cutoff","amount":0.25}],"pattern":{"bars":1,"meter_numerator":1,"meter_denominator":4,"grid":"sixteenth","swing":0,"notes":[{"start_beats":0,"duration_beats":0.05,"pitch":67,"velocity":70,"channel":1}]},"characteristics":"Grouped selected voice","explanation":"Retain the captured peer ownership and aggregate intent","name":"Grouped Test Voice","reconciliation":{"detach":[],"remove":[],"replace":[],"controls":[]},"catalog_selections":[{"local_id":7,"design":"91387715-13d3-4c57-a479-12251842a401","revision":"1","content_sha256":"","destination":{"role":"synth","chain":4,"position":1}}],"catalog_parameter_edits":[{"selection":7,"unit":0,"key":"analog.filter.cutoff","normalized":0.25}]})json");
 auto& selection=response["catalog_selections"].getArray()->getReference(0);
 selection.getDynamicObject()->setProperty("design",design.id.toDashedString());
 selection.getDynamicObject()->setProperty("revision",juce::String(std::to_string(design.revision)));
 selection.getDynamicObject()->setProperty("content_sha256",juce::SHA256(canonical).toHexString());
 if(child){response.getDynamicObject()->setProperty("parameter_edits",juce::Array<juce::var>());response.getDynamicObject()->setProperty("macro_edits",juce::Array<juce::var>());response.getDynamicObject()->setProperty("pattern",juce::var());response["catalog_parameter_edits"].getArray()->getReference(0).getDynamicObject()->setProperty("normalized",.35);}
  return response;
 }
 auto response=juce::JSON::parse(R"json({"version":2,"parameter_edits":[{"id":"output.gain_db","normalized":0.4}],"rack_edits":[],"macro_edits":[],"pattern":null,"characteristics":"Grouped native voice","explanation":"Retain the captured source and group authority","name":"Grouped Test Voice","reconciliation":{"detach":[],"remove":[],"replace":[],"controls":[]},"catalog_selections":[],"catalog_parameter_edits":[]})json");
 response["parameter_edits"].getArray()->getReference(0).getDynamicObject()->setProperty("normalized",request.instruction=="child"?.5:.4);
 return response;
}
Frame generatedLocal(const RuntimeJob& job,const ai::CancelCheck& extra={})
{
 const auto& request=std::get<ai::GenerationRequest>(job.input);auto input=std::make_unique<ai::LocalGenerationRequest>();
 CHECK(job.localPreview&&job.previewExchange);if(!job.localPreview||!job.previewExchange)return {MessageKind::failure,job.session,{}};
 input->request=request;input->local=*job.localPreview;unsigned attempts=request.catalog&&request.catalog->phase==ai::CatalogPhase::materialize?request.catalog->inferenceCallsUsed:0;std::uint8_t ordinal=0;
 const ai::CancelCheck cancelled=[&]{return (extra&&extra())||(job.cancellation&&job.cancellation->cancelled());};
 auto result=std::unique_ptr<ai::LocalGenerationResult>(new ai::LocalGenerationResult(ai::generateLocalProposal(*input,
  [&](const auto&,auto,const auto&){++attempts;rack::oauth::StructuredResult reply;
   if(request.instruction=="fail"){reply.result.status=rack::oauth::Status::networkFailure;reply.result.httpStatus=503;reply.result.errorCode="synthetic_failure";return reply;}
   reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;reply.text=juce::JSON::toString(groupedResponse(request),true,22);return reply;},
  [&](const auto& candidate,const auto& edits,const auto& context,const auto& trace,const auto& stop){
   auto value=std::make_unique<ai::PreviewEvaluate>();value->edits=edits;value->context=context;auto& binding=value->binding;
   binding.request=request.tag;binding.root=input->local.root;binding.catalogSnapshot=input->local.catalogSnapshot;binding.job=job.jobId;
   binding.structuralGeneration=request.structuralGeneration;binding.reset=input->local.reset;binding.sourceWord=input->local.sourceWord;
   binding.phase=request.catalog?request.catalog->phase:ai::CatalogPhase::select;binding.providerAttempt=std::uint8_t(attempts);binding.renderOrdinal=++ordinal;binding.sourceMap=input->local.sourceMapDigest;
   ai::PreviewSampleMap staged;CHECK(ai::stagePreviewMap(input->local.sourceMap,trace,staged));
   CHECK(ai::digestPreviewMap(staged,binding.stagedMap)&&ai::digestPreviewCandidate(candidate,binding.candidate)&&ai::digestPreviewContext(context,binding.context));
   return job.previewExchange->evaluate(*value,stop);
  },cancelled,job.groupContext.get())));
 CHECK(result->numeric.audio.left.empty()&&result->numeric.audio.right.empty());
 Frame frame{MessageKind::resultLocal,job.session,{}};CHECK(encodeLocalResult(*input,*result,frame.payload));return frame;
}
bool decodePeerGroupCommand(const Frame& frame,GroupCommand& command)
{
 if(frame.kind==MessageKind::groupCommandLocal){ai::PreviewSampleMap map;return decodeLocalGroupPublication(bytes(frame.payload),command,map);}
 return decodeGroupCommand(bytes(frame.payload),command);
}
struct Backend
{
 std::mutex mutex;std::vector<RuntimeJob> jobs;std::atomic_uint generations=0,cancelled=0;std::atomic_bool holdAccount=false,accountStarted=false;HANDLE release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
 ~Backend(){CloseHandle(release);}
 Frame run(const RuntimeJob& job){if(const auto* r=std::get_if<ai::GenerationRequest>(&job.input)){{std::lock_guard lock(mutex);jobs.push_back(job);}generations.fetch_add(1);if(r->instruction=="hold"||(r->instruction=="catalog-witness-hold"&&r->catalog&&r->catalog->phase==ai::CatalogPhase::materialize))WaitForSingleObject(release,5000);if(job.localPreview)return generatedLocal(job);Frame f{MessageKind::result,job.session,{}};CHECK(encodeResult(*r,generated(*r),f.payload));return f;}accountStarted=true;if(holdAccount)WaitForSingleObject(release,5000);Frame f{MessageKind::accountResult,job.session,{}};CHECK(encodeAccountReply(accountResult(),f.payload));return f;}
 void cancel(const RuntimeJob& job){if(job.previewExchange)job.previewExchange->cancel();if(job.cancellation&&job.cancellation->cancelled())cancelled.fetch_add(1);SetEvent(release);}
 std::vector<RuntimeJob> captured(){std::lock_guard lock(mutex);return jobs;}
};
struct Service
{
 Temp temp;juce::String name=juce::Uuid().toString().removeCharacters("-");std::shared_ptr<Backend> backend=std::make_shared<Backend>();std::atomic_bool stop=false;RuntimeServer server;std::thread io;
 Service():server([b=backend](const auto& j){return b->run(j);},[b=backend](const auto& j){b->cancel(j);}),io([this]{const auto r=server.run(stop,name);std::cout<<"SYNTHETIC_SERVER_EXIT namespace="<<name<<" status="<<int(r.status)<<" stop="<<stop.load()<<" retained="<<r.workerRetained<<std::endl;if(r.workerRetained)std::terminate();}){PipeConnection probe;const auto fixtureRuntime=juce::Uuid();Frame fixtureHello{MessageKind::hello,juce::Uuid::null(),{}};if(!encodeControl({{fixtureRuntime,0,0,0},RuntimeStatus::ready},fixtureHello.payload))throw std::runtime_error("fixture hello encode");bool fixtureWriting=false,fixtureReading=false;CHECK(observe([&]{if(!probe.connected()&&probe.connect(name)!=PipeStatus::ready)return false;if(!fixtureWriting){if(probe.beginWrite(fixtureHello)!=PipeStatus::ready)throw std::runtime_error("fixture hello begin write");fixtureWriting=true;}if(!fixtureReading){const auto wrote=probe.pollWrite();if(wrote==PipeStatus::pending)return false;if(wrote!=PipeStatus::complete||probe.beginRead()!=PipeStatus::ready)throw std::runtime_error("fixture hello write/read");fixtureReading=true;}Frame welcome;const auto read=probe.pollRead(welcome);if(read==PipeStatus::pending)return false;ControlMessage control;const bool ready=read==PipeStatus::complete&&welcome.kind==MessageKind::welcome&&!welcome.session.isNull()&&decodeControl(bytes(welcome.payload),control)&&control.tag==ai::RequestTag{fixtureRuntime,0,0,0}&&control.status==RuntimeStatus::ready;std::cout<<"FIXTURE_WELCOME namespace="<<name<<" runtime="<<fixtureRuntime.toString()<<" ready="<<ready<<std::endl;return ready;}));CHECK(selfPeer(probe.peer()));probe.close();}
 ~Service(){SetEvent(backend->release);stop=true;io.join();}
 ClientOptions options() const{return {{juce::File{},name,temp.root},false};}
};
bool linked(Node& n){CHECK(n.p->requestGroupConnection());const bool done=observe([&]{n.p->pollAi();return n.p->groupWorkflowView().generationReady;});if(!done){const auto view=n.p->groupWorkflowView();std::cout<<"WAIT timeout client="<<int(view.connection.phase)<<" link="<<int(view.link.phase)<<" pending="<<view.operationPending<<" busy="<<view.busy<<" message="<<view.message<<" workflow="<<n.p->aiWorkflowStatus().message<<std::endl;}CHECK(done);return done;}
bool readyAccount(Node& n){CHECK(n.p->requestAiAccount({}));const bool done=observe([&]{n.p->pollAi();const auto* a=n.p->aiAccountReply();return a&&a->models.size()==1&&!n.p->aiWorkflowStatus().accountPending;});if(!done){const auto view=n.p->groupWorkflowView();std::cout<<"WAIT timeout client="<<int(view.connection.phase)<<" link="<<int(view.link.phase)<<" pending="<<view.operationPending<<" busy="<<view.busy<<" message="<<view.message<<" workflow="<<n.p->aiWorkflowStatus().message<<std::endl;}CHECK(done);return done;}
// A four-peer proposal at the size caps takes 2.5 s alone in Debug and up to 8 s beside seven other copies.
bool proposal(Node& n){const bool done=observe([&]{n.p->pollAi();return !n.p->aiWorkflowStatus().generationWaiting&&n.p->aiProposalPhase()!=AiProposalPhase::pending;},20000);CHECK(done);return done&&n.p->aiProposalPhase()==AiProposalPhase::proposed;}
bool roster(Node& n,std::size_t count){CHECK(n.p->browseGroups());const bool done=observe([&]{n.p->pollAi();auto v=n.p->groupWorkflowView();return !v.operationPending&&v.members.size()==count;});if(!done){const auto view=n.p->groupWorkflowView();std::cout<<"WAIT timeout client="<<int(view.connection.phase)<<" link="<<int(view.link.phase)<<" pending="<<view.operationPending<<" busy="<<view.busy<<" message="<<view.message<<" workflow="<<n.p->aiWorkflowStatus().message<<std::endl;}CHECK(done);return done;}
int isolatedHost(const juce::String& name,const juce::File& root,const juce::String& eventName)
{
 auto n=std::make_unique<Node>(ClientOptions{{juce::File{},name,root},false});CHECK(n->set(250));CHECK(linked(*n));auto event=OpenEventW(SYNCHRONIZE,FALSE,eventName.toWideCharPointer());CHECK(event!=nullptr);
 CHECK(root.getChildFile("child-ready.txt").replaceWithText(n->p->aiRuntime().toString()));std::cout<<"ISOLATED_HOST PID="<<GetCurrentProcessId()<<" FILETIME="<<creation()<<" runtime="<<n->p->aiRuntime().toString()<<'\n';
 const bool released=observe([&]{n->p->pollAi();return event&&WaitForSingleObject(event,0)==WAIT_OBJECT_0;},10000);CHECK(released);if(event)CloseHandle(event);n->p->closeAiSession();return failures.load()?1:0;
}
void isolationCase(Service& service,Node& owner)
{
 const auto ready=service.temp.root.getChildFile("child-ready.txt");const auto eventName="Local\\RackSynthGroupWorkflow-"+juce::Uuid().toString().removeCharacters("-");HANDLE event=CreateEventW(nullptr,TRUE,FALSE,eventName.toWideCharPointer());CHECK(event!=nullptr);if(!event)return;
 auto module=std::make_unique<std::array<wchar_t,32768>>();CHECK(GetModuleFileNameW(nullptr,module->data(),DWORD(module->size()))!=0);const juce::String executable(module->data());auto line="\""+executable+"\" --isolated-host \""+service.name+"\" \""+service.temp.root.getFullPathName()+"\" \""+eventName+"\"";auto wide=line.toWideCharPointer();std::vector<wchar_t> argv(wide,wide+line.length()+1);STARTUPINFOW start{};start.cb=sizeof(start);PROCESS_INFORMATION child{};
 const bool launched=CreateProcessW(module->data(),argv.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&child)!=0;CHECK(launched);if(!launched){CloseHandle(event);return;}CHECK(observe([&]{owner.p->pollAi();return ready.existsAsFile();}));const auto childRuntime=juce::Uuid(ready.loadFileAsString());CHECK(!childRuntime.isNull());CHECK(roster(owner,3));for(const auto& member:owner.p->groupWorkflowView().members)CHECK(member.runtime!=GroupUuid(childRuntime));
 SetEvent(event);const bool done=observe([&]{owner.p->pollAi();return WaitForSingleObject(child.hProcess,0)==WAIT_OBJECT_0;});CHECK(done);if(!done)TerminateProcess(child.hProcess,99);DWORD code=99;GetExitCodeProcess(child.hProcess,&code);CHECK(code==0);std::cout<<"ISOLATED_CHILD PID="<<child.dwProcessId<<" exit="<<code<<'\n';CloseHandle(child.hThread);CloseHandle(child.hProcess);CloseHandle(event);
}
// An actual restricted native peer reads original public commands and supplies
// wire-valid or deliberately malformed replies; it never edits client internals.
struct NativePeer
{
 Temp temp;juce::String name=juce::Uuid().toString().removeCharacters("-");PipeListener listener;PipeConnection pipe;juce::Uuid session=juce::Uuid();std::unique_ptr<Node> node;bool reading=false,holdValidation=false,holdAdmission=false,holdPublication=false,holdGeneration=false,holdSpectrum=false;
 std::vector<GroupSpectrum> spectra;
 std::vector<GroupMemberSummary> members;std::vector<std::unique_ptr<GroupCommand>> commands;std::vector<std::unique_ptr<GroupGenerationCommand>> requests;std::vector<std::unique_ptr<ai::LocalGroupGenerationCommand>> localRequests;std::shared_ptr<LocalPreviewExchange> activeExchange;std::shared_ptr<std::atomic_bool> activeCancel;std::uint64_t nextJob=1,nextTicket=1;std::optional<Frame> validation;std::vector<MessageKind> kinds;std::unique_ptr<GroupCommand> held;
 explicit NativePeer(std::shared_ptr<RackLibraryRuntime> runtime=airack::testing::isolatedRackRuntime()){CHECK(listener.open(name)==PipeStatus::ready);node=std::make_unique<Node>(ClientOptions{{juce::File{},name,temp.root},false},std::move(runtime));}
 ~NativePeer(){node.reset();pipe.close();listener.close();airack::companion::detail::PipeTestAccess::reapRetired();}
 bool send(const Frame& f,bool pump=true){if(pipe.beginWrite(f)!=PipeStatus::ready)return false;PipeStatus status=PipeStatus::pending;const bool done=observe([&]{status=pipe.pollWrite();if(pump)node->p->pollAi();return status!=PipeStatus::pending;});if(pump)node->p->pollAi();return done&&status==PipeStatus::complete;}
 bool groupReply(const GroupCommand& c,GroupReplyStatus status=GroupReplyStatus::ready,std::optional<ai::RequestTag> tag={},bool pump=true){GroupReply r{c.operationId,c.action,c.runtime,c.loadRevision,status,{},0,false,{},c.proposal};if(tag)r.proposal=tag;if(status==GroupReplyStatus::ready&&c.action==GroupAction::browse)r.members=members;if(status==GroupReplyStatus::ready&&c.action==GroupAction::spectrumRead)r.spectra=spectra;if(status==GroupReplyStatus::ready&&(c.action==GroupAction::admit||c.action==GroupAction::publish)){r.normalized=c.publication->project.group;r.registryRevision=1;}Frame f{spectrumAction(c.action)?MessageKind::spectrumReply:MessageKind::groupReply,session,{}};CHECK(encodeGroupReply(r,f.payload));return send(f,pump);}
 // The controlled peer retains all deliberate ACK/stale cases, but actual
 // owned generation still waits for the plugin's real worker receipt over IO.
 // Full server-owned four-peer captures are covered by Service above; this
 // restricted peer deliberately controls group ACKs rather than a registry.
 bool generationReply(const GroupGenerationCommand& c,bool pump=true)
 {
  const auto selected=std::find_if(localRequests.begin(),localRequests.end(),[&](const auto& p){return p->input.request.tag==c.request.tag;});
  if(selected==localRequests.end()){Frame r{MessageKind::result,session,{}};CHECK(encodeResult(c.request,generated(c.request),r.payload));return send(r,pump);}
  struct Work{RuntimeJob job;std::atomic_bool done=false;std::shared_ptr<std::atomic_bool> cancel=std::make_shared<std::atomic_bool>(false);Frame terminal;bool failed=false;};
  auto work=std::make_shared<Work>();auto& job=work->job;job.jobId=nextJob++;job.connectionId=1;job.session=session;job.input=c.request;
  job.cancellation=std::make_shared<JobCancellation>();job.localPreview=std::make_shared<ai::LocalPreviewContext>((*selected)->input.local);
  job.localDeadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(clientOperationTimeoutMs);
  CHECK(nextTicket&&nextTicket<=std::numeric_limits<std::uint64_t>::max()-3);job.previewExchange=std::make_shared<LocalPreviewExchange>(std::array<std::uint64_t,3>{nextTicket,nextTicket+1,nextTicket+2},job.localDeadline);nextTicket+=3;
  activeExchange=job.previewExchange;activeCancel=work->cancel;
  std::thread worker([work]{try{work->terminal=generatedLocal(work->job,[stop=work->cancel]{return stop->load();});}catch(...){work->failed=true;}work->done.store(true);});
  auto evaluation=std::make_unique<ai::PreviewEvaluate>();const bool completed=observe([&]{
   if(activeExchange->takeEvaluation(*evaluation)){Frame offer{MessageKind::previewEvaluate,session,{}};
    const bool encoded=encodePreviewEvaluate(*evaluation,offer.payload);CHECK(encoded);if(!encoded||!send(offer,pump)){work->cancel->store(true);activeExchange->cancel();}}
   step(pump);return work->done.load();
  },20000);
  if(!completed){work->cancel->store(true);activeExchange->cancel();}worker.join();CHECK(completed&&!work->failed);activeExchange.reset();activeCancel.reset();
  if(work->failed)return false;return send(work->terminal,pump);
 }
 bool step(bool pump=true){if(pump)node->p->pollAi();if(!pipe.connected()){const auto s=listener.pollAccept(pipe);if(s!=PipeStatus::complete)return false;CHECK(selfPeer(pipe.peer()));}if(!reading){if(pipe.beginRead()!=PipeStatus::ready)return false;reading=true;}Frame f;const auto s=pipe.pollRead(f);if(s==PipeStatus::pending)return false;reading=false;if(s!=PipeStatus::complete)return false;kinds.push_back(f.kind);
  if(f.kind==MessageKind::hello){ControlMessage c;CHECK(decodeControl(bytes(f.payload),c));Frame w{MessageKind::welcome,session,{}};CHECK(encodeControl(c,w.payload));CHECK(send(w,pump));}
  else if(f.kind==MessageKind::groupCommand||f.kind==MessageKind::groupCommandLocal||f.kind==MessageKind::spectrumCommand){auto c=std::make_unique<GroupCommand>();CHECK(decodePeerGroupCommand(f,*c));commands.push_back(std::move(c));if(commands.back()->action==GroupAction::validateProposal&&holdValidation)validation=f;else if(commands.back()->action==GroupAction::admit&&holdAdmission){}else if((commands.back()->action==GroupAction::publish&&holdPublication)||(commands.back()->action==GroupAction::spectrumRead&&holdSpectrum)){holdPublication=false;held=std::make_unique<GroupCommand>(*commands.back());}else CHECK(groupReply(*commands.back(),GroupReplyStatus::ready,{},pump));}
  else if(f.kind==MessageKind::account){Frame r{MessageKind::accountResult,session,{}};CHECK(encodeAccountReply(accountResult(),r.payload));CHECK(send(r,pump));}
  else if(f.kind==MessageKind::generateGroup||f.kind==MessageKind::generateGroupLocal){auto c=std::make_unique<GroupGenerationCommand>();
   if(f.kind==MessageKind::generateGroupLocal){auto local=std::make_unique<ai::LocalGroupGenerationCommand>();const bool decoded=decodeLocalGroupGeneration(bytes(f.payload),*local);CHECK(decoded);if(!decoded)return false;
    c->request=local->input.request;c->selection=local->selection;c->parentProposal=local->parentProposal;localRequests.push_back(std::move(local));}
   else CHECK(decodeGroupGeneration(bytes(f.payload),*c));requests.push_back(std::move(c));if(!holdGeneration)CHECK(generationReply(*requests.back(),pump));}
  else if(f.kind==MessageKind::previewMeasured){ai::PreviewReceipt receipt;const bool decoded=decodePreviewReceipt(bytes(f.payload),receipt);CHECK(decoded);CHECK(decoded&&activeExchange&&activeExchange->acceptReceipt(receipt));}
  else if(f.kind==MessageKind::cancel){if(activeCancel)activeCancel->store(true);if(activeExchange)activeExchange->cancel();ControlMessage c;CHECK(decodeControl(bytes(f.payload),c));c.status=RuntimeStatus::cancelled;Frame r{MessageKind::failure,session,{}};CHECK(encodeControl(c,r.payload));CHECK(send(r,pump));}
  else CHECK(false);return true;}
 bool connect(){CHECK(node->set());if(!node->p->requestGroupConnection()){CHECK(false);return false;}const bool done=observe([&]{step();return node->p->groupWorkflowView().generationReady;});CHECK(done);return done;}
 bool settle(){const bool done=observe([&]{step();return node->p->groupWorkflowView().generationReady;});CHECK(done);return done;}
 const GroupPublication* lastPublication() const{for(auto it=commands.rbegin();it!=commands.rend();++it)if((*it)->publication)return &*(*it)->publication;return nullptr;}
 bool published(bool busy,std::uint64_t edit){return observe([&]{step();const auto* p=lastPublication();return p&&p->busy==busy&&p->editRevision==edit;});}
 bool generatedProposal(std::string instruction="normal"){if(!node->p->requestAiAccount({})){CHECK(false);return false;}CHECK(observe([&]{step();const auto* a=node->p->aiAccountReply();return a&&!node->p->aiWorkflowStatus().accountPending&&a->models.size()==1;}));CHECK(settle());CHECK(airack::testing::generateAi(*node->p,node->options(instruction)));const bool done=observe([&]{step();return node->p->aiProposalPhase()==AiProposalPhase::proposed;});CHECK(done);return done;}
};
struct EndListener:juce::AudioProcessorParameter::Listener
{
 std::function<void()> end;void parameterValueChanged(int,float) override{}void parameterGestureChanged(int,bool start) override{if(!start&&end)end();}
};
void committedCase()
{
 auto peer=std::make_unique<NativePeer>();if(!peer->connect())return;auto& n=*peer->node;auto before=n.snapshot();const auto oldEdit=n.edit;const auto gain=index("output.gain_db");CHECK(peer->lastPublication()&&peer->lastPublication()->project==*before);
 CHECK(n.p->beginParameterGesture(gain));CHECK(n.p->setParameterGestureValue(gain,.33f));CHECK(peer->published(true,oldEdit));CHECK(peer->lastPublication()->project==*before);CHECK(n.p->groupWorkflowView().busy&&!n.p->groupWorkflowView().generationReady);
 auto moved=n.snapshot();CHECK(n.edit==oldEdit+1&&moved->sound.values[gain]==.33f);CHECK(n.p->setParameterGestureValue(gain,.44f));CHECK(peer->published(true,oldEdit));CHECK(peer->lastPublication()->project==*before);
 EndListener listener;bool callback=false;listener.end=[&]{callback=true;CHECK(n.p->groupWorkflowView().busy);n.p->pollAi();CHECK(peer->lastPublication()->project==*before);};n.p->control(gain).addListener(&listener);CHECK(n.p->endParameterGesture(gain));n.p->control(gain).removeListener(&listener);CHECK(callback);CHECK(peer->published(false,oldEdit+1));auto final=n.snapshot();CHECK(peer->lastPublication()->project==*final);
 const auto committed=std::make_unique<ProjectDocument>(peer->lastPublication()->project);const auto committedEdit=peer->lastPublication()->editRevision;const auto link=n.p->groupLinkView();n.p->control(gain).setValue(.61f);
 CHECK(observe([&]{peer->step();const auto* p=peer->lastPublication();return p&&p->playback.controls.availability==ObservedMidiAvailability::fresh&&p->playback.controls.values[gain]==.61f;}));CHECK(peer->lastPublication()->project==*committed&&peer->lastPublication()->editRevision==committedEdit);CHECK(n.p->groupLinkView()==link);CHECK(n.snapshot()->sound.values[gain]==.61f&&n.edit==committedEdit);
 const auto target=index("synth.1.modulation.1.target");const auto previousTarget=n.p->control(target).getValue();n.p->control(target).setValue(1);
 CHECK(observe([&]{peer->step();return peer->lastPublication()->playback.controls.reason==LiveControlReason::invalidRouting;}));CHECK(peer->lastPublication()->playback.controls.availability==ObservedMidiAvailability::unavailable);for(const auto v:peer->lastPublication()->playback.controls.values)CHECK(std::bit_cast<std::uint32_t>(v)==0);CHECK(peer->lastPublication()->project==*committed);CHECK(n.p->control(target).getValue()==1);n.p->control(target).setValue(previousTarget);
 n.p->prepareToPlay(48000,512);juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;midi.ensureSize(128);midi.addEvent(juce::MidiMessage::noteOn(1,48,std::uint8_t(100)),0);n.p->processBlock(audio,midi);
 CHECK(observe([&]{peer->step();const auto* p=peer->lastPublication();return p&&p->playback.notes.noteCount==1;}));const auto notes=peer->lastPublication()->playback.notes;CHECK(notes.notes[0].pitch==48&&notes.reason==ObservedMidiReason::none&&notes.ageMs<=observedMidiFreshnessMs);CHECK(peer->lastPublication()->project==*committed);n.head.available=false;midi.clear();n.p->processBlock(audio,midi);CHECK(observe([&]{peer->step();return peer->lastPublication()->playback.notes.reason==ObservedMidiReason::missingTiming;}));CHECK(peer->lastPublication()->playback.notes.noteCount==0);
 {FakeServices files;auto unit=std::make_unique<studio::AiUnit>(*n.p,*studio::studioSkin(),files.make());unit.reset();}CHECK(n.p->groupLinkView().phase==GroupLinkPhase::admitted);const auto count=peer->commands.size();auto pattern=n.snapshot()->pattern;pattern.noteCount=1;pattern.notes[0]={0,.25,55,90,1};n.snapshot();CHECK(n.p->applyPatternEdit(pattern,n.edit));
 CHECK(observe([&]{peer->step(false);juce::MessageManager::getInstance()->runDispatchLoopUntil(1);return peer->commands.size()>count&&peer->lastPublication()->project.pattern==pattern;}));CHECK(n.p->groupLinkView().phase==GroupLinkPhase::admitted);CHECK(peer->lastPublication()->project.sound.values[gain]==.61f);
 CHECK(n.p->undoSoundEdit());CHECK(peer->settle());CHECK(peer->lastPublication()->project.pattern==committed->pattern);CHECK(n.p->redoSoundEdit());CHECK(peer->settle());CHECK(peer->lastPublication()->project.pattern==pattern);
 n.p->closeAiSession();CHECK(n.p->groupWorkflowView().connection.phase==ClientPhase::closed);const auto lastOperation=peer->commands.back()->operationId;peer->pipe.close();peer->reading=false;peer->session=juce::Uuid();CHECK(n.p->requestGroupConnection());CHECK(peer->settle());CHECK(peer->commands.back()->operationId>lastOperation);
}
void groupMetadataAndEndCase()
{
 auto peer=std::make_unique<NativePeer>();auto& n=*peer->node;CHECK(n.set());peer->holdAdmission=true;if(!n.p->requestGroupConnection()){CHECK(false);return;}CHECK(observe([&]{peer->step();return !peer->commands.empty()&&peer->commands.back()->action==GroupAction::admit;}));if(peer->commands.empty())return;
 auto original=std::make_unique<GroupCommand>(*peer->commands.back());auto prior=n.snapshot();const auto edit=n.edit;const auto gain=index("output.gain_db");CHECK(n.p->beginParameterGesture(gain));CHECK(n.p->setParameterGestureValue(gain,.37f));
 GroupReply ack{original->operationId,original->action,original->runtime,original->loadRevision,GroupReplyStatus::ready,prior->group,1,false,{}, {}};ack.normalized->member=id(88);Frame f{MessageKind::groupReply,peer->session,{}};CHECK(encodeGroupReply(ack,f.payload));CHECK(peer->send(f));peer->holdAdmission=false;
 CHECK(peer->published(true,edit));const auto* busy=peer->lastPublication();CHECK(busy&&busy->project.group==ack.normalized);if(busy){CHECK(busy->project.sound==prior->sound&&busy->project.pattern==prior->pattern&&busy->project.mode==prior->mode);CHECK(busy->editRevision==edit);}CHECK(n.p->groupLinkView().intent==ack.normalized&&n.p->groupLinkView().loadRevision==original->loadRevision);
 auto recalled=std::make_unique<ProjectDocument>(*prior);recalled->group=ack.normalized;recalled->sound.values[gain]=.73f;recalled->pattern.noteCount=1;recalled->pattern.notes[0]={0,.5,71,90,1};EndListener listener;bool fired=false;
 listener.end=[&]{if(fired)return;fired=true;CHECK(n.recall(*recalled));CHECK(n.p->groupWorkflowView().busy);n.p->pollAi();CHECK(n.p->groupLinkView().phase==GroupLinkPhase::pending);};n.p->control(gain).addListener(&listener);CHECK(n.p->endParameterGesture(gain));n.p->control(gain).removeListener(&listener);CHECK(fired);CHECK(*n.snapshot()==*recalled);CHECK(n.p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(!n.p->requestGroupConnection());
 peer->pipe.close();peer->reading=false;peer->session=juce::Uuid();CHECK(n.p->resumeGroupIntent());CHECK(n.p->requestGroupConnection());CHECK(peer->settle());CHECK(peer->lastPublication()&&peer->lastPublication()->project==*recalled&&!peer->lastPublication()->busy);
}
void explicit_admission_closed_editor_and_committed_publication()
{
 begin(__func__);auto service=std::make_unique<Service>();auto a=std::make_unique<Node>(service->options());auto b=std::make_unique<Node>(service->options());auto c=std::make_unique<Node>(service->options());
 CHECK(!a->p->requestGroupConnection());CHECK(a->set());CHECK(b->set());auto imported=b->snapshot();CHECK(c->recall(*imported));CHECK(c->p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(!c->p->requestGroupConnection());CHECK(c->p->browseGroups());CHECK(observe([&]{c->p->pollAi();return c->p->groupWorkflowView().connection.phase==ClientPhase::ready;}));CHECK(c->p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(readyAccount(*c));CHECK(c->p->groupLinkView().phase==GroupLinkPhase::pending);
 if(a->p->requestGroupConnection()){CHECK(observe([&]{a->p->pollAi();return a->p->groupWorkflowView().generationReady;}));CHECK(linked(*b));CHECK(a->p->groupLinkView().intent->member!=b->p->groupLinkView().intent->member);CHECK(roster(*c,2));CHECK(c->p->resumeGroupIntent());CHECK(linked(*c));CHECK(roster(*a,3));CHECK(c->p->groupLinkView().intent->member!=b->p->groupLinkView().intent->member);isolationCase(*service,*a);}else CHECK(false);
 CHECK(service->backend->generations.load()==0);committedCase();groupMetadataAndEndCase();
 service->backend->holdAccount=true;service->backend->accountStarted=false;ResetEvent(service->backend->release);CHECK(c->p->requestAiAccount({}));CHECK(observe([&]{c->p->pollAi();return service->backend->accountStarted.load();}));c->p->closeAiPanel();CHECK(c->p->aiWorkflowStatus().accountPending);service->backend->holdAccount=false;SetEvent(service->backend->release);CHECK(observe([&]{c->p->pollAi();return !c->p->aiWorkflowStatus().accountPending&&c->p->aiAccountReply();}));CHECK(c->p->groupLinkView().phase==GroupLinkPhase::admitted||c->p->groupLinkView().phase==GroupLinkPhase::pending);
 {auto peer=std::make_unique<NativePeer>();auto& n=*peer->node;CHECK(n.set());const auto gain=index("output.gain_db");CHECK(n.p->beginParameterGesture(gain));if(n.p->requestGroupConnection()){CHECK(observe([&]{peer->step();return n.p->groupWorkflowView().connection.phase==ClientPhase::ready;}));for(const auto& command:peer->commands)CHECK(command->action!=GroupAction::admit);CHECK(n.p->groupWorkflowView().busy&&!n.p->groupWorkflowView().generationReady);CHECK(n.p->endParameterGesture(gain));CHECK(peer->settle());CHECK(peer->lastPublication()&&!peer->lastPublication()->busy);}else{CHECK(false);CHECK(n.p->endParameterGesture(gain));}}
}
void remoteCase(int failure)
{
 auto peer=std::make_unique<NativePeer>();if(!peer->connect()||!peer->generatedProposal())return;auto& n=*peer->node;const auto before=n.saved();const auto beforeProject=n.snapshot();const auto revision=n.edit;CHECK(!n.p->applyAiProposal());CHECK(peer->settle());
 CHECK(observe([&]{peer->step();return n.p->groupWorkflowView().peerContextCurrent;}));CHECK(!n.p->applyAiProposal());CHECK(n.saved()==before);peer->holdValidation=true;const auto oldOperation=peer->commands.back()->operationId;CHECK(n.p->applyAiWhenStopped());CHECK(observe([&]{peer->step();return peer->validation.has_value();}));CHECK(peer->commands.back()->operationId>oldOperation);auto command=std::make_unique<GroupCommand>(*peer->commands.back());CHECK(command->action==GroupAction::validateProposal&&command->proposal==n.p->aiCurrentRequest()->tag);
 if(failure==1){auto tag=*command->proposal;++tag.requestId;CHECK(peer->groupReply(*command,GroupReplyStatus::ready,tag));CHECK(observe([&]{peer->step();return n.p->groupWorkflowView().connection.phase==ClientPhase::failed;}));CHECK(n.saved()==before);CHECK(!n.p->applyAiProposal());}
 else if(failure==2){CHECK(peer->groupReply(*command,GroupReplyStatus::staleContext));CHECK(observe([&]{peer->step();return n.p->aiProposalPhase()==AiProposalPhase::stale;}));CHECK(n.saved()==before);CHECK(!n.p->startAiPreview());}
 else if(failure==3){const auto session=peer->session;peer->session=juce::Uuid();CHECK(peer->groupReply(*command));peer->session=session;CHECK(observe([&]{peer->step();return n.p->groupWorkflowView().connection.phase==ClientPhase::failed;}));CHECK(n.saved()==before);CHECK(!n.p->applyAiProposal());}
 else{CHECK(peer->groupReply(*command));CHECK(observe([&]{peer->step();return n.p->aiProposalPhase()==AiProposalPhase::idle;}));auto applied=n.snapshot();CHECK(n.edit==revision+1&&applied->sound.values[index("output.gain_db")]==.4f);CHECK(!n.p->applyAiProposal());CHECK(peer->groupReply(*command));CHECK(n.snapshot()->sound==applied->sound);CHECK(n.p->undoSoundEdit());CHECK(n.snapshot()->sound==beforeProject->sound);}
}
void aiSealCase()
{
 auto peer=std::make_unique<NativePeer>();if(!peer->connect()||!peer->generatedProposal())return;auto& n=*peer->node;const auto old=n.snapshot();auto pattern=old->pattern;pattern.noteCount=1;pattern.notes[0]={0,.25,69,80,1};EndListener listener;bool fired=false;
 listener.end=[&]{if(fired)return;fired=true;CHECK(n.p->groupWorkflowView().busy);n.snapshot();CHECK(n.p->applyPatternEdit(pattern,n.edit));n.p->pollAi();CHECK(peer->lastPublication()->project==*old);};const auto gain=index("output.gain_db");n.p->control(gain).addListener(&listener);CHECK(n.p->applyAiWhenStopped());CHECK(observe([&]{peer->step();return n.p->aiProposalPhase()==AiProposalPhase::idle;}));n.p->control(gain).removeListener(&listener);CHECK(fired);CHECK(peer->settle());auto final=n.snapshot();CHECK(final->pattern==pattern&&final->sound.values[gain]==.4f);CHECK(peer->lastPublication()->project==*final&&!peer->lastPublication()->busy);
}
void locally_admitted_historical25(const ClientOptions& client)
{
 InstalledCatalogFixture fixture;if(!fixture.ready)return;auto n=std::make_unique<Node>(client,fixture.runtime);
 CHECK(n->set(77));
 const auto v=n->p->groupLinkView();
 CHECK(n->p->acknowledgeGroupIntent(v.runtime,v.loadRevision,*v.intent,*v.intent));
 const auto library=airack::test::frozenLibraryRackDesigns();const auto tile=airack::test::frozenTileArrayRackDesign();
 const auto isSaturation=[](const auto& design){return std::any_of(design.units.begin(),design.units.end(),[](const auto& unit){return unit.module==NativeModule::saturation;});};
 CHECK(library.size()==28);
 CHECK(std::count(library.begin(),library.end(),tile)==1);
 CHECK(std::count_if(library.begin(),library.end(),isSaturation)==2);
 for(const auto* text:{"91387715-13d3-4c57-a479-12251842bf01","91387715-13d3-4c57-a479-12251842bf02"})
  CHECK(std::count_if(library.begin(),library.end(),[&](const auto& design){return design.id==juce::Uuid(text)&&isSaturation(design);})==1);
 RackInventoryPtr current;
 CHECK(airack::testing::readyRackInventory(*n->p,current)&&current&&current->rows().size()==28);
 if(current)for(const auto& design:library){
  juce::MemoryBlock canonical;CHECK(encodeRackDesign(design,canonical));const auto hash=juce::SHA256(canonical).toHexString();
  CHECK(std::count_if(current->rows().begin(),current->rows().end(),[&](const auto& row){return row.metadata.design==design.id&&row.metadata.revision==design.revision&&row.metadata.canonicalSha256==hash;})==1);
  RackDesign exact;std::optional<RackPackageRef> package;CHECK(resolveInventoryDesign(*current,design.id,design.revision,hash,exact,package)&&exact==design);
 }
 auto r=InstrumentAiTestAccess::beginHistorical25(*n->p,n->options(),120);
 CHECK(r.has_value());
 if(r){
  CHECK(r->catalog&&r->catalog->authority);
  if(r->catalog&&r->catalog->authority){
   const auto& json=r->catalog->authority->descriptorJson;
   const auto inventory=juce::JSON::parse(juce::String::fromUTF8(json.data(),int(json.size())));
   const auto* entries=inventory["entries"].getArray();
   CHECK(entries&&entries->size()==25&&bool(inventory["complete"]));
   if(entries)for(const auto& design:library){
    const bool historical=design.id!=tile.id&&!isSaturation(design);
    const auto count=std::count_if(entries->begin(),entries->end(),[&](const auto& entry){return juce::Uuid(entry["id"].toString())==design.id;});
    CHECK(count==(historical?1:0));
    if(historical){juce::MemoryBlock bytes;CHECK(encodeRackDesign(design,bytes));
     CHECK(std::count_if(entries->begin(),entries->end(),[&](const auto& entry){return juce::Uuid(entry["id"].toString())==design.id&&entry["revision"].toString()==juce::String(std::to_string(design.revision))&&entry["content_sha256"].toString()==juce::SHA256(bytes).toHexString();})==1);}
   }
  }
  CHECK(n->p->receiveAiResult(generated(*r)));
  CHECK(n->p->applyAiProposal());
 }
}
void selected_generation_refinement_and_apply_authority()
{
 begin(__func__);auto service=std::make_unique<Service>();std::vector<std::unique_ptr<Node>> nodes;for(unsigned i=0;i<6;++i){nodes.push_back(std::make_unique<Node>(service->options()));CHECK(nodes.back()->set(i+1));}
 auto& a=*nodes[0];if(!a.p->requestGroupConnection()){CHECK(false);}else{
 CHECK(observe([&]{a.p->pollAi();return a.p->groupWorkflowView().generationReady;}));for(std::size_t i=1;i<nodes.size();++i)CHECK(linked(*nodes[i]));CHECK(roster(a,6));CHECK(readyAccount(a));
 std::array<GroupUuid,4> selected{nodes[4]->p->groupLinkView().intent->member,nodes[2]->p->groupLinkView().intent->member,nodes[1]->p->groupLinkView().intent->member,nodes[3]->p->groupLinkView().intent->member};CHECK(a.p->selectGroupPeers(selected));const auto old=a.p->groupWorkflowView().selection;
 std::array<GroupUuid,5> overflow{selected[0],selected[1],selected[2],selected[3],nodes[5]->p->groupLinkView().intent->member};CHECK(!a.p->selectGroupPeers(overflow));CHECK(!a.p->selectGroupPeers(std::array<GroupUuid,2>{selected[0],selected[0]}));CHECK(!a.p->selectGroupPeers(std::array<GroupUuid,1>{a.p->groupLinkView().intent->member}));CHECK(!a.p->selectGroupPeers(std::array<GroupUuid,1>{id(66)}));CHECK(a.p->groupWorkflowView().selection==old);
 auto context=a.p->aiMusicalContext();context.role="changed explicit role";CHECK(a.p->setAiMusicalContext(context));a.p->pollAi();CHECK(a.p->groupWorkflowView().selection.peerCount==old.peerCount);for(std::size_t i=0;i<4;++i)CHECK(a.p->groupWorkflowView().selection.members[i]==selected[i]);CHECK(linked(a));CHECK(readyAccount(a));
 std::vector<juce::MemoryBlock> peerBytes;for(std::size_t i=1;i<nodes.size();++i)peerBytes.push_back(nodes[i]->saved());CHECK(observe([&]{a.p->pollAi();return a.p->groupWorkflowView().generationReady;}));std::shared_ptr<const RackCatalogSnapshot> readyInventory;RackCatalogStatus readyStatus=RackCatalogStatus::unavailable;CHECK(a.p->captureAvailableRackCatalog(readyInventory,&readyStatus)&&readyInventory&&readyStatus==RackCatalogStatus::ready);const bool submitted=airack::testing::generateAi(*a.p,a.options());if(!submitted){const auto view=a.p->groupWorkflowView();std::cout<<"GROUP_GENERATE_REJECTION message="<<a.p->aiWorkflowStatus().message<<" phase="<<int(a.p->aiProposalPhase())<<" request="<<(a.p->aiCurrentRequest()!=nullptr)<<" connection="<<int(view.connection.phase)<<" group_ready="<<view.generationReady<<" operation_pending="<<view.operationPending<<std::endl;std::shared_ptr<const RackCatalogSnapshot> inventory;RackCatalogStatus status=RackCatalogStatus::unavailable;const bool captured=a.p->captureAvailableRackCatalog(inventory,&status);RackCatalogPage page;RackCatalogStatus pageStatus=RackCatalogStatus::unavailable;const bool paged=captured&&inventory&&catalogPage(*inventory,{},0,65536,page,&pageStatus);ProjectDocument snapshot;std::uint64_t edit=0,reset=0;const bool snap=a.p->tryProjectSnapshot(snapshot,edit,reset);std::cout<<"GROUP_CAPTURE_WITNESS captured="<<captured<<" status="<<int(status)<<" entries="<<(inventory?inventory->entries.size():0)<<" page="<<paged<<" page_status="<<int(pageStatus)<<" complete="<<page.complete<<" descriptor_bytes="<<(paged?juce::JSON::toString(rackCatalogPageDescriptor(page),true,22).getNumBytesAsUTF8():0)<<" snapshot="<<snap<<" snapshot_valid="<<(snap&&validateProjectDocument(snapshot))<<std::endl;}CHECK(submitted);CHECK(proposal(a));auto jobs=service->backend->captured();CHECK(jobs.size()==1&&jobs[0].groupContext);if(jobs.size()==1&&jobs[0].groupContext){CHECK(jobs[0].groupContext->peerCount==4);for(std::size_t i=0;i<4;++i)CHECK(jobs[0].groupContext->ownership.peers[i].project.group->member==selected[i]);}
 CHECK(!a.p->applyAiProposal());CHECK(observe([&]{a.p->pollAi();return a.p->groupWorkflowView().peerContextCurrent&&!a.p->groupWorkflowView().operationPending;}));CHECK(a.p->aiCurrentRequest()!=nullptr);if(!a.p->aiCurrentRequest())return;const auto parent=a.p->aiCurrentRequest()->tag;CHECK(a.p->refineAi("fail"));CHECK(proposal(a));CHECK(a.p->aiCurrentRequest()->tag==parent&&a.p->groupWorkflowView().peerContextCurrent);CHECK(observe([&]{a.p->pollAi();const auto view=a.p->groupWorkflowView();return view.peerContextCurrent&&view.generationReady&&!view.operationPending;}));CHECK(a.p->refineAi("child"));CHECK(proposal(a));CHECK(a.p->aiCurrentRequest()->tag.requestId>parent.requestId);CHECK(a.p->applyAiWhenStopped());CHECK(observe([&]{a.p->pollAi();return a.p->aiProposalPhase()==AiProposalPhase::idle;}));CHECK(a.snapshot()->sound.values[index("output.gain_db")]==.5f);CHECK(a.p->undoSoundEdit());for(std::size_t i=1;i<nodes.size();++i)CHECK(nodes[i]->saved()==peerBytes[i-1]);
 const auto selectedBeforeLoss=a.p->groupWorkflowView().selection;nodes[4]->p->closeAiSession();CHECK(roster(a,5));CHECK(a.p->groupWorkflowView().selection==selectedBeforeLoss&&!a.p->groupWorkflowView().generationReady);CHECK(!a.p->generateAi(a.options()));std::array<GroupUuid,3> retained{selected[0],selected[1],selected[2]};CHECK(a.p->selectGroupPeers(retained));CHECK(!a.p->groupWorkflowView().generationReady);std::array<GroupUuid,2> available{selected[1],selected[2]};CHECK(a.p->selectGroupPeers(available));CHECK(observe([&]{a.p->pollAi();return a.p->groupWorkflowView().generationReady;}));CHECK(a.p->selectGroupPeers({}));CHECK(a.p->groupWorkflowView().selection.peerCount==0);
 }
 for(int failure=0;failure<4;++failure)remoteCase(failure);
 aiSealCase();
 // A genuine remote child command keeps the original parent tag/order;
 // low-level standalone injection with locally admitted saved intent stays legal.
 {auto peer=std::make_unique<NativePeer>();if(peer->connect()&&peer->generatedProposal()){const auto parent=peer->node->p->aiCurrentRequest()->tag;CHECK(peer->settle());CHECK(peer->node->p->refineAi("child"));CHECK(observe([&]{peer->step();return peer->requests.size()==2&&peer->node->p->aiProposalPhase()==AiProposalPhase::proposed;}));if(peer->requests.size()==2){CHECK(peer->requests[1]->parentProposal==parent);CHECK(peer->requests[1]->selection==peer->requests[0]->selection);}CHECK(!peer->node->p->applyAiProposal());}}
 locally_admitted_historical25(service->options());CHECK(service->backend->generations.load()==3);
}
thread_local bool tracking=false;thread_local unsigned allocations=0,frees=0;
int __cdecl allocationHook(int kind,void*,size_t,int,long,const unsigned char*,int){if(tracking){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++allocations;if(kind==_HOOK_FREE)++frees;}return 1;}
void audioCase(Service& service,PlaybackMode mode)
{
 auto linkedNode=std::make_unique<Node>(service.options());auto offline=std::make_unique<Node>(service.options());CHECK(linkedNode->set(41));if(!linkedNode->p->requestGroupConnection()){CHECK(false);return;}CHECK(observe([&]{linkedNode->p->pollAi();return linkedNode->p->groupWorkflowView().generationReady;}));auto d=linkedNode->snapshot();d->pattern.noteCount=1;d->pattern.notes[0]={0,2,48,100,1};d->mode=mode;CHECK(linkedNode->recall(*d)&&offline->recall(*d));CHECK(linkedNode->p->resumeGroupIntent());CHECK(linked(*linkedNode));linkedNode->p->prepareToPlay(48000,512);offline->p->prepareToPlay(48000,512);
 juce::AudioBuffer<float> a(2,512),b(2,512);juce::MidiBuffer midi;midi.ensureSize(256);midi.addEvent(juce::MidiMessage::noteOn(1,48,std::uint8_t(100)),0);const auto saved=linkedNode->saved();linkedNode->snapshot();const auto oldEdit=linkedNode->edit;const auto hook=_CrtSetAllocHook(allocationHook);allocations=frees=0;
 for(int n=0;n<24;++n){linkedNode->head.sample=offline->head.sample=n*512;tracking=true;linkedNode->p->processBlock(a,midi);offline->p->processBlock(b,midi);tracking=false;linkedNode->p->pollAi();for(int ch=0;ch<2;++ch)for(int frame=0;frame<512;++frame)CHECK(std::bit_cast<std::uint32_t>(a.getSample(ch,frame))==std::bit_cast<std::uint32_t>(b.getSample(ch,frame)));midi.clear();}
 _CrtSetAllocHook(hook);CHECK(allocations==0&&frees==0);CHECK(linkedNode->saved()==saved);linkedNode->snapshot();CHECK(linkedNode->edit==oldEdit);std::cout<<"AUDIO mode="<<int(mode)<<" allocations="<<allocations<<" frees="<<frees<<" exact samples=24576 control=between_callbacks\n";linkedNode->p->closeAiSession();CHECK(linkedNode->saved()==saved&&linkedNode->p->groupWorkflowView().connection.phase==ClientPhase::closed);
}
void staleCase(int stage)
{
 std::cout<<"staleCase stage="<<stage<<std::endl;
 auto service=std::make_unique<Service>();auto a=std::make_unique<Node>(service->options());auto b=std::make_unique<Node>(service->options());CHECK(a->set());CHECK(b->set(2));if(!a->p->requestGroupConnection()){CHECK(false);return;}CHECK(observe([&]{a->p->pollAi();return a->p->groupWorkflowView().generationReady;}));CHECK(linked(*b));CHECK(readyAccount(*a));CHECK(roster(*a,2));CHECK(a->p->selectGroupPeers(std::array<GroupUuid,1>{b->p->groupLinkView().intent->member}));const auto original=a->saved(),peerState=b->saved();const auto gain=index("output.gain_db");
 struct PickupGuard{~PickupGuard(){airack::companion::detail::RuntimeServerTestAccess::pauseBeforePickup(false);}} guard;
 if(stage==0){airack::companion::detail::RuntimeServerTestAccess::pauseBeforePickup(true);}
 CHECK(airack::testing::generateAi(*a->p,a->options(stage==1?"hold":"normal")));
 if(stage==0)CHECK(observe([&]{a->p->pollAi();return airack::companion::detail::RuntimeServerTestAccess::pickupPaused();}));
 if(stage==1)CHECK(observe([&]{a->p->pollAi();return service->backend->generations.load()==1;}));if(stage>=2){CHECK(proposal(*a));if(stage==3){a->p->prepareToPlay(48000,512);juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;CHECK(a->p->startAiPreview());a->p->processBlock(audio,midi);CHECK(a->p->aiPreviewPlaying());CHECK(a->p->applyAiWhenStopped());a->p->pollAi();CHECK(a->p->aiWorkflowStatus().applyPending);}}
 if(stage==1)b->p->closeAiSession();else{CHECK(b->p->beginParameterGesture(gain));CHECK(b->p->setParameterGestureValue(gain,.62f));CHECK(b->p->endParameterGesture(gain));}
 CHECK(observe([&]{b->p->pollAi();a->p->pollAi();return stage==1?service->backend->cancelled.load()>0:!b->p->groupWorkflowView().operationPending;}));airack::companion::detail::RuntimeServerTestAccess::pauseBeforePickup(false);
 if(stage==3){juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;a->p->processBlock(audio,midi);}
 CHECK(observe([&]{a->p->pollAi();b->p->pollAi();return a->p->aiProposalPhase()==AiProposalPhase::stale;}));CHECK(!a->p->applyAiProposal()&&!a->p->startAiPreview());CHECK(a->saved()==original);if(stage==1)CHECK(b->saved()==peerState);if(stage==0)CHECK(service->backend->generations.load()==0);
}
void lateAdmissionCase()
{
 auto peer=std::make_unique<NativePeer>();auto& n=*peer->node;CHECK(n.set());peer->holdAdmission=true;if(!n.p->requestGroupConnection()){CHECK(false);return;}CHECK(observe([&]{peer->step();return !peer->commands.empty()&&peer->commands.back()->action==GroupAction::admit;}));if(peer->commands.empty())return;auto original=std::make_unique<GroupCommand>(*peer->commands.back());auto saved=n.snapshot();CHECK(n.recall(*saved));n.p->pollAi();CHECK(n.p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(!n.p->acknowledgeGroupIntent(original->runtime,original->loadRevision,*original->publication->project.group,*original->publication->project.group));const bool delivered=peer->groupReply(*original);CHECK(!delivered||n.p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(!n.p->groupWorkflowView().generationReady);CHECK(!n.p->requestGroupConnection());peer->pipe.close();peer->reading=false;peer->session=juce::Uuid();peer->holdAdmission=false;CHECK(n.p->resumeGroupIntent());CHECK(n.p->requestGroupConnection());CHECK(peer->settle());CHECK(peer->commands.back()->operationId>original->operationId);
}
void restore_disconnect_conflict_and_audio_safety()
{
 begin(__func__);{auto service=std::make_unique<Service>();auto a=std::make_unique<Node>(service->options());auto b=std::make_unique<Node>(service->options());CHECK(a->set());CHECK(b->set(2));if(!a->p->requestGroupConnection()){CHECK(false);}else{
 CHECK(observe([&]{a->p->pollAi();return a->p->groupWorkflowView().generationReady;}));CHECK(linked(*b));CHECK(roster(*a,2));CHECK(readyAccount(*a));const std::array<GroupUuid,1> selected{b->p->groupLinkView().intent->member};CHECK(a->p->selectGroupPeers(selected));CHECK(a->generate(a->options("hold")));CHECK(observe([&]{a->p->pollAi();return service->backend->generations.load()==1;}));CHECK(b->p->beginParameterGesture(index("output.gain_db")));CHECK(observe([&]{a->p->pollAi();b->p->pollAi();return service->backend->cancelled.load()==1;}));CHECK(observe([&]{a->p->pollAi();return a->p->aiProposalPhase()==AiProposalPhase::stale;}));CHECK(!a->p->startAiPreview()&&!a->p->applyAiProposal());CHECK(b->p->endParameterGesture(index("output.gain_db")));CHECK(observe([&]{a->p->pollAi();b->p->pollAi();return a->p->groupWorkflowView().generationReady&&b->p->groupWorkflowView().generationReady;}));
 CHECK(a->generate(a->options()));CHECK(proposal(*a));const auto oldBytes=a->saved();auto bad=oldBytes;static_cast<char*>(bad.getData())[25]^=1;const auto authority=a->p->groupLinkView();CHECK(!a->p->restoreProjectState(bad.getData(),int(bad.getSize())));CHECK(a->p->groupLinkView()==authority&&a->saved()==oldBytes&&a->p->aiProposalPhase()==AiProposalPhase::proposed);
 auto restored=a->snapshot();CHECK(a->recall(*restored));a->p->pollAi();CHECK(a->p->groupWorkflowView().selection.peerCount==0);CHECK(a->p->groupLinkView().phase==GroupLinkPhase::pending);CHECK(!a->p->requestGroupConnection()&&!a->p->applyAiProposal());CHECK(roster(*b,1));CHECK(a->p->resumeGroupIntent());CHECK(linked(*a));CHECK(readyAccount(*a));CHECK(roster(*a,2));CHECK(a->p->selectGroupPeers(selected));CHECK(a->generate(a->options()));CHECK(proposal(*a));
 auto different=*b->p->groupLinkView().intent;different.key=text("D");CHECK(b->p->setGroupIntent(different));CHECK(b->p->requestGroupConnection());CHECK(observe([&]{b->p->pollAi();a->p->pollAi();return a->p->groupWorkflowView().conflicted;}));CHECK(!a->p->groupWorkflowView().generationReady);CHECK(a->p->aiProposalPhase()==AiProposalPhase::stale);CHECK(!a->p->generateAi(a->options())&&!a->p->applyAiWhenStopped());CHECK(b->p->leaveGroupIntent());b->p->pollAi();CHECK(roster(*a,1));CHECK(a->p->leaveGroupIntent());a->p->pollAi();CHECK(a->p->groupWorkflowView().selection.peerCount==0);CHECK(a->p->groupWorkflowView().connection.phase==ClientPhase::closed);
 }
 }
 for(int stage=0;stage<4;++stage)staleCase(stage);lateAdmissionCase();{auto service=std::make_unique<Service>();audioCase(*service,PlaybackMode::externalMidi);audioCase(*service,PlaybackMode::internalPattern);}CHECK(airack::companion::detail::PipeTestAccess::retainedOwners()==0);CHECK(airack::companion::detail::RuntimeServerTestAccess::retainedWorkers()==0);
}
// The requester's heartbeat holds a press: it is accepted, sends nothing while a routine command is pending, and is
// sent, from the state at that moment, once the command is answered; an answer that shows a change refuses it.
void heartbeat_presses_wait_then_act()
{
 begin(__func__);const auto gain=index("output.gain_db");
 {NativePeer peer;if(peer.connect()){auto& n=*peer.node;CHECK(n.p->requestAiAccount({}));CHECK(observe([&]{peer.step();return n.p->aiAccountReply()&&!n.p->aiWorkflowStatus().accountPending;}));CHECK(peer.settle());
  CHECK(n.p->beginParameterGesture(gain));CHECK(!n.p->groupWorkflowView().generationWaits);CHECK(!n.p->generateAi(n.options()));CHECK(n.p->aiWorkflowStatus().message=="Group context is unavailable; retry Generate"&&!n.p->aiWorkflowStatus().generationWaiting);CHECK(n.p->endParameterGesture(gain));CHECK(peer.settle());
  peer.holdPublication=true;CHECK(observe([&]{peer.step();return peer.held!=nullptr;}));auto view=n.p->groupWorkflowView();CHECK(view.operationPending&&!view.generationReady&&view.generationWaits);
  CHECK(airack::testing::generateAi(*n.p,n.options("waited")));CHECK(n.p->aiWorkflowStatus().generationWaiting&&n.p->aiWorkflowStatus().message=="Generate waits for the group heartbeat");CHECK(n.p->aiProposalPhase()==AiProposalPhase::idle);
  auto pattern=n.snapshot()->pattern;pattern.noteCount=1;pattern.notes[0]={0,.25,57,90,1};CHECK(n.p->applyPatternEdit(pattern,n.edit));for(int i=0;i<8;++i)peer.step();CHECK(peer.requests.empty());
  CHECK(peer.held&&peer.groupReply(*peer.held));CHECK(observe([&]{peer.step();return peer.requests.size()==1&&n.p->aiProposalPhase()==AiProposalPhase::proposed;}));CHECK(!n.p->aiWorkflowStatus().generationWaiting);
  const auto* published=peer.lastPublication();CHECK(published&&published->project.pattern==pattern);if(published&&peer.requests.size()==1){const auto& request=peer.requests[0]->request;CHECK(request.instruction=="waited"&&request.tag.editRevision==published->editRevision&&request.source==published->project);}
 }}
 {NativePeer peer;if(peer.connect()&&peer.generatedProposal()){auto& n=*peer.node;CHECK(observe([&]{peer.step();return n.p->groupWorkflowView().peerContextCurrent;}));const auto parent=n.p->aiCurrentRequest()->tag;
  peer.holdValidation=true;peer.validation.reset();CHECK(observe([&]{peer.step();return peer.validation.has_value();}));auto proposalCheck=std::make_unique<GroupCommand>(*peer.commands.back());CHECK(proposalCheck->action==GroupAction::validateProposal);
  CHECK(n.p->refineAi("child"));CHECK(n.p->aiWorkflowStatus().generationWaiting&&n.p->aiWorkflowStatus().message=="Refine waits for the group heartbeat");for(int i=0;i<8;++i)peer.step();CHECK(peer.requests.size()==1);
  peer.holdValidation=false;CHECK(peer.groupReply(*proposalCheck));CHECK(observe([&]{peer.step();return peer.requests.size()==2&&n.p->aiProposalPhase()==AiProposalPhase::proposed;}));if(peer.requests.size()==2)CHECK(peer.requests[1]->parentProposal==parent&&peer.requests[1]->request.instruction=="child");CHECK(!n.p->aiWorkflowStatus().generationWaiting);
  CHECK(observe([&]{peer.step();return n.p->groupWorkflowView().peerContextCurrent;}));peer.holdValidation=true;peer.validation.reset();CHECK(observe([&]{peer.step();return peer.validation.has_value();}));proposalCheck=std::make_unique<GroupCommand>(*peer.commands.back());
  CHECK(n.p->refineAi("again"));CHECK(n.p->aiWorkflowStatus().generationWaiting);CHECK(peer.groupReply(*proposalCheck,GroupReplyStatus::staleContext));CHECK(observe([&]{peer.step();return !n.p->aiWorkflowStatus().generationWaiting;}));
  CHECK(peer.requests.size()==2&&n.p->aiProposalPhase()==AiProposalPhase::stale&&n.p->aiWorkflowStatus().message.endsWith("Generate again"));
 }}
 {NativePeer peer;if(peer.connect()){auto& n=*peer.node;CHECK(n.p->requestAiAccount({}));CHECK(observe([&]{peer.step();return n.p->aiAccountReply()&&!n.p->aiWorkflowStatus().accountPending;}));CHECK(peer.settle());
  peer.holdPublication=true;CHECK(observe([&]{peer.step();return peer.held!=nullptr;}));CHECK(airack::testing::generateAi(*n.p,n.options()));CHECK(n.p->aiWorkflowStatus().generationWaiting);n.p->cancelAi();CHECK(!n.p->aiWorkflowStatus().generationWaiting);
  CHECK(peer.held&&peer.groupReply(*peer.held));peer.held.reset();CHECK(peer.settle());for(int i=0;i<8;++i)peer.step();CHECK(peer.requests.empty()&&n.p->aiProposalPhase()==AiProposalPhase::idle);
  peer.holdPublication=true;CHECK(observe([&]{peer.step();return peer.held!=nullptr;}));CHECK(airack::testing::generateAi(*n.p,n.options()));CHECK(n.p->aiWorkflowStatus().generationWaiting);
  peer.pipe.close();peer.reading=false;juce::String refused;CHECK(observe([&]{n.p->pollAi();refused=n.p->aiWorkflowStatus().message;return !n.p->aiWorkflowStatus().generationWaiting;}));CHECK(refused.startsWith("Companion unavailable (")&&peer.requests.empty());
 }}
}
// Normally invoked owning plugin witness; runtime implementation remains untouched.
void grouped_catalog_followup_lifecycle_transaction()
{
 begin(__func__);
 for(bool retire:{false,true}){
  auto service=std::make_unique<Service>();auto a=std::make_unique<Node>(service->options());auto b=std::make_unique<Node>(service->options());
  CHECK(a->set(101)&&b->set(102));if(!linked(*a)||!linked(*b)||!readyAccount(*a)||!roster(*a,2))continue;
  const std::array<GroupUuid,1> selected{b->p->groupLinkView().intent->member};CHECK(a->p->selectGroupPeers(selected));
  CHECK(observe([&]{a->p->pollAi();b->p->pollAi();return a->p->groupWorkflowView().generationReady;}));
  auto original=a->snapshot();const auto peerBytes=b->saved();const auto unrelated=index("output.gain_db");
  auto requestOptions=a->options(retire?"catalog-witness-hold":"catalog-witness");requestOptions.policy.allowMacroChanges=true;
  CHECK(airack::testing::generateAi(*a->p,requestOptions));
  CHECK(observe([&]{a->p->pollAi();b->p->pollAi();return service->backend->generations.load()==2;}));
  auto jobs=service->backend->captured();CHECK(jobs.size()==2);if(jobs.size()!=2){const auto view=a->p->groupWorkflowView();const auto workflow=a->p->aiWorkflowStatus();std::cout<<"CATALOG_FOLLOWUP_SNAPSHOT retire="<<retire<<" generations="<<service->backend->generations.load()<<" jobs="<<jobs.size()<<" proposal="<<int(a->p->aiProposalPhase())<<" client="<<int(view.connection.phase)<<" link="<<int(view.link.phase)<<" pending="<<view.operationPending<<" busy="<<view.busy<<" workflow="<<workflow.message<<std::endl;}if(jobs.size()!=2)continue;
  const auto* root=std::get_if<ai::GenerationRequest>(&jobs[0].input);const auto* pending=std::get_if<ai::GenerationRequest>(&jobs[1].input);
  CHECK(root&&pending&&root->catalog&&pending->catalog&&jobs[0].groupContext&&jobs[1].groupContext);
  if(!root||!pending||!root->catalog||!pending->catalog||!jobs[0].groupContext||!jobs[1].groupContext)continue;
  CHECK(root->catalog->phase==ai::CatalogPhase::select&&pending->catalog->phase==ai::CatalogPhase::materialize);
  CHECK(pending->catalog->root==root->tag&&pending->catalog->selectionParent==root->tag&&pending->tag.requestId>root->tag.requestId);
  CHECK(pending->source==root->source&&pending->catalog->baseIntent.catalogSelections.size()==1&&pending->catalog->authority->definitions.size()==1);
  CHECK(pending->catalog->baseIntent.parameters.size()==1&&pending->catalog->baseIntent.macros.size()==1&&pending->catalog->baseIntent.pattern&&pending->catalog->baseIntent.pattern->noteCount==1);
  CHECK(jobs[0].groupContext->peerCount==1&&jobs[1].groupContext->peerCount==1);
  CHECK(jobs[0].groupContext->ownership.peers[0].project==jobs[1].groupContext->ownership.peers[0].project);
  CHECK(jobs[1].groupContext->ownership.peers[0].project.group->member==selected[0]);
  if(retire){
   CHECK(a->p->aiProposalPhase()==AiProposalPhase::pending&&!a->p->aiProposal());
   b->p->closeAiSession();
   CHECK(observe([&]{a->p->pollAi();return service->backend->cancelled.load()>0&&a->p->aiProposalPhase()==AiProposalPhase::stale;}));
   CHECK(!a->p->aiProposal()&&!a->p->applyAiProposal()&&!a->p->startAiPreview());
   CHECK(*a->snapshot()==*original&&b->saved()==peerBytes&&!a->p->undoSoundEdit());
  }else{
   CHECK(proposal(*a));CHECK(observe([&]{a->p->pollAi();return a->p->groupWorkflowView().peerContextCurrent&&a->p->groupWorkflowView().generationReady&&!a->p->groupWorkflowView().operationPending;}));
   CHECK(a->p->aiProposal()&&a->p->aiProposal()->candidate&&a->p->aiIntendedEdits());
   if(!a->p->aiProposal()||!a->p->aiProposal()->candidate||!a->p->aiIntendedEdits())continue;
   CHECK(a->p->aiProposal()->candidate->sound.personalities&&a->p->aiProposal()->candidate->sound.personalities->instances.size()==1);
   if(!a->p->aiProposal()->candidate->sound.personalities)continue;
   const auto selectedId=a->p->aiProposal()->candidate->sound.personalities->instances.front().id;
   CHECK(a->p->refineAi("catalog-witness-child"));CHECK(proposal(*a));
   auto childJobs=service->backend->captured();CHECK(childJobs.size()==3&&childJobs.back().groupContext);
   const auto* child=childJobs.empty()?nullptr:std::get_if<ai::GenerationRequest>(&childJobs.back().input);
   CHECK(child&&child->catalog&&child->source==root->source&&child->catalog->root==root->tag);
   CHECK(a->p->aiIntendedEdits()&&a->p->aiProposal()&&a->p->aiProposal()->candidate);if(!a->p->aiIntendedEdits()||!a->p->aiProposal()||!a->p->aiProposal()->candidate)continue;
   auto expected=std::make_unique<ProjectDocument>(*a->p->aiProposal()->candidate);
   CHECK(expected->sound.personalities&&expected->sound.personalities->instances.front().id==selectedId);
   if(!expected->sound.personalities)continue;
   const auto* binding=rackUnitBinding(expected->sound.personalities->instances.front(),0);CHECK(binding);if(!binding)continue;
   const auto target=rackParameterIndex(NativeModule::analog,binding->slot,"analog.filter.cutoff");CHECK(target);if(!target)continue;
   CHECK(expected->sound.values[*target]==.35f&&expected->sound.values[index("global.master_level")]==.1f&&expected->pattern.noteCount==1&&expected->pattern.notes[0].pitch==67);
   CHECK(expected->sound.macros[0]==(MacroBinding{parameterRegistry()[*target].stateKey,.25f}));
   CHECK(a->p->aiIntendedEdits()->catalogSelections==pending->catalog->baseIntent.catalogSelections);
   CHECK(a->p->aiIntendedEdits()->parameters==pending->catalog->baseIntent.parameters&&a->p->aiIntendedEdits()->macros==pending->catalog->baseIntent.macros&&a->p->aiIntendedEdits()->pattern==pending->catalog->baseIntent.pattern);
   a->p->control(unrelated).setValue(.61f);expected->sound.values[unrelated]=.61f;
   CHECK(a->p->applyAiWhenStopped());CHECK(observe([&]{a->p->pollAi();return a->p->aiProposalPhase()==AiProposalPhase::idle;}));
   CHECK(*a->snapshot()==*expected&&b->saved()==peerBytes);
   a->p->control(unrelated).setValue(.72f);CHECK(a->p->undoSoundEdit());auto undone=a->snapshot();CHECK(undone->sound.values[unrelated]==.72f);
   undone->sound.values[unrelated]=original->sound.values[unrelated];CHECK(*undone==*original&&b->saved()==peerBytes);CHECK(!a->p->undoSoundEdit());
  }
 }
}

// Observe live markers made by public grouped generation, never assigned test structs.
void grouped_source_authority()
{
 begin(__func__);
#ifndef NDEBUG
 for(bool reuse:{false,true})for(int stage=0;stage<5;++stage){
  std::cout<<"GROUP_SOURCE reuse="<<reuse<<" stage="<<stage<<'\n';
  NativePeer peer;auto& n=*peer.node;n.snapshot();
  RackOperation placement;placement.kind=RackOperationKind::place;placement.allowSlotReuse=true;placement.newInstance=juce::Uuid();placement.design.emplace();placement.design->name="Grouped source";placement.design->panel.sections={"SOURCE"};placement.design->units={{0,NativeModule::analog,{},1}};placement.allocation={{0,0}};
  CHECK(n.p->applyRackOperation(placement,n.edit)==RackCandidateStatus::ready);
  if(!peer.connect())continue;
  GroupMemberSummary member;member.runtime=id(32);member.loadRevision=1;member.editRevision=3;member.intent=intent(2);peer.members={member};
  CHECK(n.p->browseGroups());CHECK(observe([&]{peer.step();return n.p->groupWorkflowView().members.size()==1;}));
  CHECK(n.p->selectGroupPeers(std::array{member.intent.member}));
  CHECK(n.p->requestAiAccount({}));CHECK(observe([&]{peer.step();return n.p->aiAccountReply()&&!n.p->aiWorkflowStatus().accountPending;}));CHECK(peer.settle());
  const auto selection=n.p->groupWorkflowView().selection;CHECK(selection.peerCount==1);
  const auto context=n.p->aiMusicalContext();const auto link=n.p->groupLinkView();
  peer.holdGeneration=true;auto options=n.options(stage==2?"catalog-witness":"normal");options.policy.allowMacroChanges=true;
  CHECK(airack::testing::generateAi(*n.p,options));CHECK(observe([&]{peer.step();return peer.requests.size()==1;}));if(peer.requests.size()!=1)continue;
  const auto root=std::make_unique<GroupGenerationCommand>(*peer.requests.front());
  CHECK(root->selection==selection&&!root->parentProposal&&InstrumentStructuralTestAccess::marker(*n.p,root->request.tag));
  if(stage!=0){CHECK(peer.generationReply(*root));
   CHECK(observe([&]{peer.step();return stage==2?peer.requests.size()==2:n.p->aiProposalPhase()==AiProposalPhase::proposed;}));
   if(stage==2){if(peer.requests.size()!=2)continue;const auto& followup=*peer.requests.back();
    CHECK(followup.parentProposal==root->request.tag&&followup.selection==selection&&followup.request.catalog&&followup.request.catalog->phase==ai::CatalogPhase::materialize);
    CHECK(followup.request.source==root->request.source&&followup.request.catalog->selectionParent==root->request.tag);
    CHECK(InstrumentStructuralTestAccess::marker(*n.p,followup.request.tag));
   }else{CHECK(observe([&]{peer.step();return n.p->groupWorkflowView().peerContextCurrent&&n.p->groupWorkflowView().generationReady&&!n.p->groupWorkflowView().operationPending;}));
    CHECK(InstrumentStructuralTestAccess::owned(*n.p));
    if(stage==1){CHECK(n.p->refineAi("child"));CHECK(observe([&]{peer.step();return peer.requests.size()==2;}));if(peer.requests.size()!=2)continue;
     CHECK(peer.requests.back()->parentProposal==root->request.tag&&peer.requests.back()->selection==selection);
     CHECK(InstrumentStructuralTestAccess::parent(*n.p,root->request.tag)&&InstrumentStructuralTestAccess::marker(*n.p,peer.requests.back()->request.tag));
    }
   }
  }
  const auto captured=std::make_unique<GroupGenerationCommand>(*peer.requests.back());
  auto before=n.snapshot();const auto word=InstrumentStructuralTestAccess::sourceWord(*n.p);const auto authority=InstrumentStructuralTestAccess::authority(*n.p);
  CHECK(captured->request.structuralGeneration==word&&captured->request.tag.editRevision==authority[0]&&captured->request.tag.contextRevision==authority[1]);
  std::unique_ptr<ProjectDocument> mutated;
  auto mutate=[&]{
   if(reuse){n.snapshot();RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=placement.newInstance;auto place=placement;place.kind=RackOperationKind::place;place.newInstance=juce::Uuid();place.allowSlotReuse=true;
    CHECK(n.p->applyRackOperations(std::array{remove,place},n.edit)==RackCandidateStatus::ready);
    CHECK(InstrumentStructuralTestAccess::sourceWord(*n.p)==word+256&&InstrumentStructuralTestAccess::authority(*n.p)[0]>authority[0]);
   }else{for(const auto key:{std::size_t(13),drumModeParameterStart}){const auto value=n.p->control(key).getValue();n.p->control(key).setValue(value==0?1.f:0.f);n.p->control(key).setValue(value);}
    CHECK(InstrumentStructuralTestAccess::sourceWord(*n.p)==word+1024&&InstrumentStructuralTestAccess::authority(*n.p)==authority);
   }
   CHECK((InstrumentStructuralTestAccess::sourceWord(*n.p)&255)==(word&255));
   CHECK(n.p->aiProposalPhase()==AiProposalPhase::stale&&!InstrumentStructuralTestAccess::owned(*n.p));
   n.p->control(index("output.gain_db")).setValue(.61f);mutated=n.snapshot();
   CHECK(mutated->pattern==before->pattern&&mutated->group==before->group);
  };
  if(stage>=3){peer.holdValidation=true;CHECK(n.p->applyAiWhenStopped());CHECK(observe([&]{peer.step();return peer.validation.has_value();}));
   if(!peer.validation)continue;auto command=std::make_unique<GroupCommand>();CHECK(decodePeerGroupCommand(*peer.validation,*command));CHECK(command->proposal==captured->request.tag);
   if(stage==3){mutate();CHECK(peer.groupReply(*command));}
   else{struct BeginListener final:juce::AudioProcessorParameter::Listener{std::function<void()> callback;int begins=0,ends=0;void parameterValueChanged(int,float) override{}void parameterGestureChanged(int,bool start) override{if(start){++begins;callback();}else ++ends;}} listener;
    bool fired=false;listener.callback=[&]{if(fired)return;fired=true;CHECK(InstrumentStructuralTestAccess::applying(*n.p)&&InstrumentStructuralTestAccess::marker(*n.p,captured->request.tag));mutate();};
    auto& gain=n.p->control(index("output.gain_db"));gain.addListener(&listener);CHECK(peer.groupReply(*command));gain.removeListener(&listener);CHECK(fired&&listener.begins>0&&listener.begins==listener.ends);
   }
  }else{mutate();CHECK(peer.generationReply(*captured));}
  for(int i=0;i<12;++i)peer.step();
  CHECK(mutated&&*n.snapshot()==*mutated);CHECK(n.p->aiProposalPhase()==AiProposalPhase::stale&&!n.p->aiProposal());
  CHECK(!n.p->applyAiProposal()&&!n.p->applyAiWhenStopped()&&!n.p->refineAi("stale child")&&!n.p->startAiPreview());
  CHECK(n.p->groupWorkflowView().selection==selection&&n.p->aiMusicalContext()==context&&n.p->groupLinkView()==link);
  CHECK(peer.members.size()==1&&peer.members.front()==member);
  CHECK(*n.snapshot()==*mutated);
 }
#endif
}

// Periodic group traffic runs every 200 ms, so a selection reply can land while
// a publication is still unanswered; the follow-up then waits instead of retiring.
void catalog_followup_waits_for_group_reply()
{
 begin(__func__);InstalledCatalogFixture fixture;if(!fixture.ready)return;
 for(bool rejected:{false,true}){
  NativePeer peer(fixture.runtime);if(!peer.connect())continue;auto& n=*peer.node;
  RackInventoryPtr inventory;CHECK(airack::testing::readyRackInventory(*n.p,inventory)&&inventory&&inventory->rows().size()==28);
  const auto tile=airack::test::frozenTileArrayRackDesign();CHECK(inventory&&std::count_if(inventory->rows().begin(),inventory->rows().end(),[&](const auto& row){return row.metadata.design==tile.id&&row.installed&&row.package&&row.package->id=="tile-array";})==1);
  CHECK(n.p->requestAiAccount({}));CHECK(observe([&]{peer.step();const auto* a=n.p->aiAccountReply();return a&&!n.p->aiWorkflowStatus().accountPending&&a->models.size()==1;}));CHECK(peer.settle());
  auto options=n.options("catalog-witness");options.policy.allowMacroChanges=true;const auto original=n.snapshot();
  auto unchanged=[&]{const auto now=n.snapshot();return now->sound==original->sound&&now->pattern==original->pattern;};
  peer.holdGeneration=true;CHECK(airack::testing::generateAi(*n.p,options));CHECK(observe([&]{peer.step();return peer.requests.size()==1;}));if(peer.requests.size()!=1)continue;
  const auto root=*peer.requests.front();CHECK(root.request.catalog&&root.request.catalog->phase==ai::CatalogPhase::select);
  CHECK(root.request.catalog&&root.request.catalog->authority&&root.request.catalog->authority->rendering==ai::CatalogRendering::ownedLocal&&peer.localRequests.size()==1);
  std::cout<<"LOCAL_CATALOG_FOLLOWUP rejected="<<rejected<<" rows="<<(inventory?inventory->rows().size():0)<<" local_requests="<<peer.localRequests.size()<<'\n';
  peer.holdPublication=true;const auto count=peer.commands.size();
  CHECK(observe([&]{peer.step();return peer.commands.size()>count&&peer.commands.back()->action==GroupAction::publish;}));if(peer.commands.size()<=count)continue;
  const auto held=*peer.commands.back();CHECK(peer.generationReply(root));for(int i=0;i<8;++i)n.p->pollAi();
  CHECK(n.p->aiProposalPhase()==AiProposalPhase::pending&&n.p->groupWorkflowView().connection.phase==ClientPhase::ready);
  CHECK(n.p->aiCurrentRequest()&&n.p->aiCurrentRequest()->catalog&&n.p->aiCurrentRequest()->catalog->phase==ai::CatalogPhase::materialize);
  peer.holdPublication=peer.holdGeneration=false;const auto received=peer.kinds.size();
  CHECK(peer.groupReply(held,rejected?GroupReplyStatus::invalidInput:GroupReplyStatus::ready));
  if(rejected){
   CHECK(peer.settle());CHECK(n.p->aiProposalPhase()==AiProposalPhase::stale&&peer.requests.size()==1);CHECK(unchanged());
   for(auto i=received;i<peer.kinds.size();++i)CHECK(peer.kinds[i]!=MessageKind::generateGroupLocal);
  }else{
   CHECK(observe([&]{peer.step();return peer.requests.size()==2;}));if(peer.requests.size()!=2)continue;
   if(peer.kinds.size()<=received||peer.kinds[received]!=MessageKind::generateGroupLocal){std::cerr<<"CATALOG_FOLLOWUP_ORDER received="<<received;for(auto i=received;i<peer.kinds.size();++i)std::cerr<<" kind="<<int(peer.kinds[i]);std::cerr<<'\n';}
   CHECK(peer.kinds.size()>received&&peer.kinds[received]==MessageKind::generateGroupLocal);
   CHECK(peer.localRequests.size()==2);
   const auto& followup=*peer.requests.back();
   CHECK(followup.request.catalog&&followup.request.catalog->phase==ai::CatalogPhase::materialize&&followup.request.catalog->selectionParent==root.request.tag);
   CHECK(followup.parentProposal==root.request.tag&&followup.request.source==root.request.source);
   CHECK(observe([&]{peer.step();return n.p->aiProposalPhase()==AiProposalPhase::proposed;}));CHECK(n.p->aiProposal()&&n.p->aiProposal()->candidate);CHECK(unchanged());
  }
 }
}

void spectrum_reply_cannot_cross_view_lifetime()
{
 begin(__func__);auto peer=std::make_unique<NativePeer>();auto& n=*peer->node;
 auto document=n.snapshot();document->sound.layout.effects[7]={SlotUse::present,EffectKind::dynamicEq,masterChain};document->sound.layout.effectOrder[masterChain][0]=7;document->sound.layout.effectCount[masterChain]=1;CHECK(n.recall(*document));
 if(!peer->connect())return;
 GroupMemberSummary member;member.runtime=id(32);member.loadRevision=1;member.editRevision=3;member.intent=intent(12);peer->members={member};CHECK(n.p->browseGroups());CHECK(observe([&]{peer->step();return n.p->groupWorkflowView().members.size()==1;}));
 GroupSpectrum value{member.runtime,member.loadRevision,1,7,7,48000,{}};value.postDb100.fill(-3000);peer->spectra={value};peer->holdSpectrum=true;
 const std::array peers{member.intent.member};const auto old=n.p->openEqSpectrumView();CHECK(old&&n.p->configureEqSpectrumView(old,true,7,peers));
 const bool requested=observe([&]{peer->step();return peer->held&&peer->held->action==GroupAction::spectrumRead;});CHECK(requested);if(!requested)return;
 auto previous=std::make_unique<GroupCommand>(*peer->held);n.p->closeEqSpectrumView(old);const auto current=n.p->openEqSpectrumView();CHECK(current&&current!=old&&n.p->configureEqSpectrumView(current,true,7,peers));
 CHECK(peer->groupReply(*previous));CHECK(observe([&]{peer->step();return peer->held&&peer->held->operationId>previous->operationId;}));
 CHECK(!n.p->peerEqSpectrum(current,member.runtime));
 CHECK(peer->groupReply(*peer->held));CHECK(observe([&]{peer->step();return n.p->peerEqSpectrum(current,member.runtime)!=nullptr;}));
 const auto nextRead=[&]{const auto operation=peer->held->operationId;return observe([&]{peer->step();return peer->held->operationId>operation;});};
 CHECK(nextRead());CHECK(peer->groupReply(*peer->held,GroupReplyStatus::invalidInput));
 CHECK(nextRead());CHECK(!n.p->peerEqSpectrum(current,member.runtime));CHECK(peer->groupReply(*peer->held));
 CHECK(nextRead());CHECK(!n.p->peerEqSpectrum(current,member.runtime));
 peer->spectra[0].sequence=2;CHECK(peer->groupReply(*peer->held));CHECK(observe([&]{peer->step();const auto* latest=n.p->peerEqSpectrum(current,member.runtime);return latest&&latest->sequence==2;}));
 CHECK(nextRead());CHECK(peer->groupReply(*peer->held,GroupReplyStatus::capacity));CHECK(nextRead());CHECK(n.p->peerEqSpectrum(current,member.runtime));
 n.p->closeEqSpectrumView(old);CHECK(n.p->eqSpectrumView(current).enabled);n.p->closeEqSpectrumView(current);
}

void spectrum_sharing_owns_view_consent_and_fresh_windows()
{
 begin(__func__);auto service=std::make_unique<Service>();auto a=std::make_unique<Node>(service->options());auto b=std::make_unique<Node>(service->options());
 for(auto* node:{a.get(),b.get()}){auto document=node->snapshot();document->sound.layout.effects[7]={SlotUse::present,EffectKind::dynamicEq,masterChain};document->sound.layout.effectOrder[masterChain][0]=7;document->sound.layout.effectCount[masterChain]=1;CHECK(node->recall(*document));}
 CHECK(a->set(11)&&b->set(12));CHECK(linked(*a)&&linked(*b));CHECK(roster(*a,2)&&roster(*b,2));
 const auto beforeA=a->saved(),beforeB=b->saved();const auto aiPeersA=a->p->groupWorkflowView().selection,aiPeersB=b->p->groupWorkflowView().selection;
 const auto receiver=a->p->openEqSpectrumView(),sender=b->p->openEqSpectrumView();const std::array<GroupUuid,1> peers{b->p->groupLinkView().intent->member};CHECK(receiver&&sender);CHECK(a->p->configureEqSpectrumView(receiver,true,7,peers));CHECK(b->p->configureEqSpectrumView(sender,true,7,{}));CHECK(a->p->eqSpectrumView(receiver).capable&&b->p->eqSpectrumView(sender).capable);
 std::array<std::int16_t,128> levels;levels.fill(-2400);CHECK(!b->p->captureEqSpectrum(sender,7,123,1,48000,levels));CHECK(!b->p->captureEqSpectrum(sender,7,123,1,48000,levels));CHECK(b->p->captureEqSpectrum(sender,7,123,2,48000,levels));
 const auto pump=[&]{a->p->pollAi();b->p->pollAi();};const auto runtime=b->p->groupLinkView().runtime;
 CHECK(observe([&]{pump();return a->p->peerEqSpectrum(receiver,runtime)!=nullptr;}));if(const auto* value=a->p->peerEqSpectrum(receiver,runtime))CHECK(value->slot==7&&value->generation==123&&value->sampleRate==48000&&value->postDb100==levels);
 bool refreshedOld=false;const auto deadline=GetTickCount64()+2200;while(GetTickCount64()<deadline){refreshedOld=b->p->captureEqSpectrum(sender,7,123,2,48000,levels)||refreshedOld;pump();SwitchToThread();}CHECK(!refreshedOld&&!a->p->peerEqSpectrum(receiver,runtime));
 levels.fill(-3000);CHECK(b->p->captureEqSpectrum(sender,7,123,3,48000,levels));CHECK(observe([&]{pump();const auto* value=a->p->peerEqSpectrum(receiver,runtime);return value&&value->postDb100==levels;}));
 b->p->closeEqSpectrumView(sender);CHECK(!b->p->captureEqSpectrum(sender,7,123,4,48000,levels));CHECK(observe([&]{pump();return !a->p->peerEqSpectrum(receiver,runtime);}));
 const auto replacement=b->p->openEqSpectrumView();CHECK(replacement&&replacement!=sender);CHECK(b->p->configureEqSpectrumView(replacement,true,7,{}));b->p->closeEqSpectrumView(sender);CHECK(b->p->eqSpectrumView(replacement).enabled);CHECK(!b->p->captureEqSpectrum(replacement,7,123,4,48000,levels));CHECK(b->p->captureEqSpectrum(replacement,7,123,5,48000,levels));CHECK(observe([&]{pump();return a->p->peerEqSpectrum(receiver,runtime)!=nullptr;}));
 CHECK(a->saved()==beforeA&&b->saved()==beforeB);CHECK(a->p->groupWorkflowView().selection==aiPeersA&&b->p->groupWorkflowView().selection==aiPeersB);CHECK(service->backend->generations.load()==0&&!service->backend->accountStarted.load());
 auto recalled=b->snapshot();CHECK(b->recall(*recalled));CHECK(!b->p->captureEqSpectrum(replacement,7,123,6,48000,levels));CHECK(observe([&]{pump();return !a->p->peerEqSpectrum(receiver,runtime);}));CHECK(b->p->groupLinkView().phase==GroupLinkPhase::pending);a->p->closeEqSpectrumView(receiver);b->p->closeEqSpectrumView(replacement);
}

}
int main(int argc,char** argv)
{
 juce::ScopedJuceInitialiser_GUI gui;std::cout<<std::unitbuf;std::cerr<<std::unitbuf;std::cout<<"NATIVE PID="<<GetCurrentProcessId()<<" FILETIME="<<creation()<<'\n';if(argc==2&&juce::String(argv[1])=="--historical25-fixture"){section="historical25_fixture";locally_admitted_historical25(ClientOptions{});std::cout<<"Historical25 checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--spectrum"){spectrum_reply_cannot_cross_view_lifetime();spectrum_sharing_owns_view_consent_and_fresh_windows();std::cout<<"Spectrum sharing checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}if(argc==2&&juce::String(argv[1])=="--catalog-followup-group-reply"){catalog_followup_waits_for_group_reply();std::cout<<"Isolated follow-up checks="<<checks<<" sections="<<sections<<" failures="<<failures<<'\n';return failures?1:0;}if(argc==5&&juce::String(argv[1])=="--isolated-host")return isolatedHost(argv[2],juce::File(argv[3]),argv[4]);spectrum_reply_cannot_cross_view_lifetime();spectrum_sharing_owns_view_consent_and_fresh_windows();explicit_admission_closed_editor_and_committed_publication();selected_generation_refinement_and_apply_authority();grouped_catalog_followup_lifecycle_transaction();grouped_source_authority();catalog_followup_waits_for_group_reply();restore_disconnect_conflict_and_audio_safety();heartbeat_presses_wait_then_act();CHECK(sections==9);std::cout<<"GroupWorkflow checks="<<checks<<" sections="<<sections<<" failures="<<failures<<'\n';return failures?1:0;
}

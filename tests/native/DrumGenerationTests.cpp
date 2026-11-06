#include "FrozenRackExamples.h"
#include "IsolatedRackLibrary.h"
#include "PreparedTileSkin.h"
#include "ComfyHttpFixture.h"
#include "InstrumentProcessor.h"
#include "DrumSourcePopup.h"
#include "DrumImport.h"
#include "ComfyDrumPreparation.h"
#include "RackExamples.h"
#include "MakerFace.h"
#include "TileArrayFace.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <future>
#include <winhttp.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <malloc.h>
#endif
namespace airack::companion::detail {
struct ComfyHttpTestAccess {static void control(ComfyHttpWindows& http,ComfyTestControl* control){http.setTestControl(control);}static std::array<unsigned,2> live(){return ComfyHttpWindows::liveNetworkState();}static std::unique_ptr<ComfyHttpWindows> fixture(juce::File root,std::uint16_t port){return std::unique_ptr<ComfyHttpWindows>(new ComfyHttpWindows(ComfyLoopback::ipv4,std::move(root),port));}};
}
namespace airack::drums::detail {
struct ComfyAudioJobTestAccess {static std::unique_ptr<ComfyAudioJob> make(std::unique_ptr<companion::ComfyHttpWindows> http){return std::unique_ptr<ComfyAudioJob>(new ComfyAudioJob(std::move(http)));}};
}
namespace airack {
struct InstrumentDrumTestAccess {
 using ProcessorPoint=InstrumentProcessor::DrumTestPoint;
 struct Counts {unsigned slots=0;std::uint64_t encoded=0,workspace=0,decoded=0,preview=0,reserved=0;bool aiRoots=false;};
 static Counts counts(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);Counts c;c.encoded=p.drumOwnership->reservedEncoded_;c.workspace=p.drumOwnership->reservedWorkspace_;c.decoded=p.drumOwnership->retainedBytes();c.reserved=p.drumOwnership->reservedDecoded_;c.preview=p.manualPreviewBytes;c.aiRoots=p.drumOwnership->aiImagesBusy();for(const auto& slot:p.drumOwnership->preparations_)c.slots+=slot.inputId!=0;return c;}
#ifndef NDEBUG
 static void factory(InstrumentProcessor& p,void* context,std::unique_ptr<drums::ComfyAudioJob> (*make)(void*,const juce::File&,const drums::ComfyAudioJobRequest&)){auto& input=p.drumImport();const std::lock_guard lock(input.mutex);input.testGenerationContext=context;input.testMakeJob=make;}
 static void hook(InstrumentProcessor& p,void* context,void (*point)(void*,DrumImport::TestPoint) noexcept){auto& input=p.drumImport();const std::lock_guard lock(input.mutex);input.testContext=context;input.testHook=point;}
#endif
 static void release(InstrumentProcessor& p,std::uint64_t id){p.releaseDrumInputWork(id);}
 static bool teardownAvailable(InstrumentProcessor& p,DrumImport& input){const bool restore=p.restoreMutex.try_lock();if(restore)p.restoreMutex.unlock();const bool operation=p.drumRestoreOperationMutex.try_lock();if(operation)p.drumRestoreOperationMutex.unlock();const bool mailbox=input.mutex.try_lock();if(mailbox)input.mutex.unlock();return restore&&operation&&mailbox;}
 static bool callbacksStopped(InstrumentProcessor& p){return !p.isTimerRunning()&&!p.isUpdatePending()&&!p.drumControlPump;}
 static std::size_t patternEvents(InstrumentProcessor& p){return p.performance->patternEvents.count;}
 static drums::AssetId preparedId(InstrumentProcessor& p,std::uint64_t ticket){const std::lock_guard lock(p.restoreMutex);drums::AssetId id{};for(const auto& slot:p.drumOwnership->preparations_)if(slot.ticket==ticket&&slot.asset)id=slot.asset->id();return id;}
 static std::array<std::uint64_t,2> admissionCounters(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return {p.drumOwnership->nextTicket_,p.drumOwnership->reservedWorkspace_};}
 static void admissionCounters(InstrumentProcessor& p,std::array<std::uint64_t,2> value){const std::lock_guard lock(p.restoreMutex);p.drumOwnership->nextTicket_=value[0];p.drumOwnership->reservedWorkspace_=value[1];}
 static DrumCommitError commitStatus(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return p.drumCommitStatus;}
 static void expireGeneration(InstrumentProcessor& p){auto& input=p.drumImport();const std::lock_guard lock(input.mutex);input.generationDeadline=drums::GenerationClock::now()-std::chrono::milliseconds(1);}
 struct GenerationCompletion {bool ready=false,success=false;drums::DrumGenerationStatus status=drums::DrumGenerationStatus::invalidRequest;unsigned available=0;};
 static GenerationCompletion generationCompletion(InstrumentProcessor& p){auto& input=p.drumImport();const std::lock_guard lock(input.mutex);GenerationCompletion state;if(input.completion&&input.completion->operation==DrumImport::Operation::generate){state.ready=true;state.success=input.completion->success;state.status=input.completion->generationStatus;for(const auto& candidate:input.completion->candidates)state.available+=candidate.available;}return state;}
 static bool generationDeadlineCleared(InstrumentProcessor& p){auto& input=p.drumImport();const std::lock_guard lock(input.mutex);return input.generationDeadline==drums::GenerationClock::time_point{};}
 static bool aiRootRetained(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);return bool(p.aiSourceOriginal);}
 static void observer(InstrumentProcessor& p,void* context,void(*callback)(void*,InstrumentProcessor::DrumTestPoint)){const std::lock_guard lock(p.restoreMutex);p.drumTestContext=context;p.drumTestHook=callback;}
 static std::pair<std::size_t,std::size_t> history(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);auto view=std::make_unique<InstrumentProcessor::DrumHistoryDebugView>();p.drumHistoryDebugSnapshot(*view);return {view->count,view->cursor};}
};
}
using namespace airack;
using namespace airack::drums;
using namespace airack::companion;
using namespace comfy_fixture;
namespace {
std::atomic_int checks{0},failures{0};juce::File output,serviceCondition;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<std::endl;}}while(false)
struct InputWaitSpan {std::uint64_t predicate=0,dispatch=0;};
struct InputWaitObservation {std::uint64_t count=0,predicateTotal=0,dispatchTotal=0,finalPredicate=0;InputWaitSpan first{},maximum{},last{};
 void retain(InputWaitSpan v){if(!count)first=v;++count;last=v;if(v.predicate+v.dispatch>maximum.predicate+maximum.dispatch)maximum=v;predicateTotal+=v.predicate;dispatchTotal+=v.dispatch;}
 void report(const char* stage)const{std::cout<<"MEASURE posted_wait stage="<<stage<<" count="<<count<<" predicate_total_ms="<<predicateTotal<<" dispatch_total_ms="<<dispatchTotal<<" first_predicate_ms="<<first.predicate<<" first_dispatch_ms="<<first.dispatch<<" max_predicate_ms="<<maximum.predicate<<" max_dispatch_ms="<<maximum.dispatch<<" last_predicate_ms="<<last.predicate<<" last_dispatch_ms="<<last.dispatch<<" final_predicate_ms="<<finalPredicate<<std::endl;}};
InputWaitObservation* inputWaitObservation=nullptr;
struct InputWaitScope {InputWaitObservation* previous=inputWaitObservation;~InputWaitScope(){inputWaitObservation=previous;}};
template<class F> bool wait(F predicate,Clock::time_point end=Clock::now()+std::chrono::seconds(20)){while(Clock::now()<end){const auto at=inputWaitObservation?GetTickCount64():0;const bool done=predicate();const auto after=inputWaitObservation?GetTickCount64():0;if(done){if(inputWaitObservation)inputWaitObservation->retain({after-at,0});return true;}juce::MessageManager::getInstance()->runDispatchLoopUntil(20);if(inputWaitObservation)inputWaitObservation->retain({after-at,GetTickCount64()-after});}const auto at=inputWaitObservation?GetTickCount64():0;const bool done=predicate();if(inputWaitObservation)inputWaitObservation->finalPredicate=GetTickCount64()-at;return done;}
struct Gate {
 std::mutex mutex;std::condition_variable wake;bool entered=false,released=false;
 void arrive(){std::unique_lock lock(mutex);entered=true;wake.notify_all();wake.wait(lock,[&]{return released;});}
 bool reached(){std::unique_lock lock(mutex);return wake.wait_for(lock,std::chrono::seconds(10),[&]{return entered;});}
 bool arrived(){const std::lock_guard lock(mutex);return entered;}
 void release(){{const std::lock_guard lock(mutex);released=true;}wake.notify_all();}
 void reset(){const std::lock_guard lock(mutex);entered=released=false;}
};
std::string quotedJson(const std::string& text){return juce::JSON::toString(juce::var(juce::String::fromUTF8(text.data(),int(text.size()))),true).toStdString();}
struct Fixture {
 std::string flac;std::unique_ptr<NativeComfyFixture> server;ComfyAudioJobRequest captured;juce::File root;
 std::atomic_bool started{false};std::atomic_uint connections{0};std::atomic_bool holdHttp{false},holdSource{false},holdPrepared{false},holdCompletion{false};int failedOrdinal=-1;std::atomic_uint joins{0};Gate httpGate,sourceGate,callbackGate,completionGate;bool holdFinalCallback=false,holdAfterDrain=false;std::atomic_bool selected{false},callbackHeld{false};airack::companion::detail::ComfyTestControl transportControl;
 Fixture(){juce::MemoryBlock block;std::unique_ptr<juce::OutputStream> stream=std::make_unique<juce::MemoryOutputStream>(block,false);juce::FlacAudioFormat format;auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions().withSampleRate(44100).withNumChannels(2).withBitsPerSample(16));CHECK(writer!=nullptr);juce::AudioBuffer<float> audio(2,442368);for(int c=0;c<2;++c)for(int n=0;n<442368;++n)audio.setSample(c,n,float(std::sin(n*.02+c)*.25));if(writer)CHECK(writer->writeFromAudioSampleBuffer(audio,0,442368));writer.reset();flac.assign(static_cast<const char*>(block.getData()),block.getSize());CHECK(flac.starts_with("fLaC")&&flac.size()>42);}
 ~Fixture(){httpGate.release();sourceGate.release();callbackGate.release();completionGate.release();if(server)server->join();}
 static std::unique_ptr<ComfyAudioJob> make(void* context,const juce::File& root,const ComfyAudioJobRequest& request){auto& f=*static_cast<Fixture*>(context);f.captured=request;f.root=root;f.connections=0;FixtureOptions script;
  script.accepted=[&f]{++f.connections;};script.beforeReply=[&f]{if(f.holdHttp&&f.connections==1)f.httpGate.arrive();};
  for(std::size_t n=0;n<3;++n){const auto id=request.attempts[n].toDashedString().toStdString();std::string body;CHECK(buildComfyAudioSubmit(request,n,body));script.script.push_back({"POST","/prompt",body,response("{\"prompt_id\":"+quotedJson(id)+",\"number\":0,\"node_errors\":{}}")});
   if(int(n)==f.failedOrdinal){script.script.push_back({"GET","/history/"+id,"",response("{"+quotedJson(id)+":{\"status\":{\"completed\":false,\"status_str\":\"error\"}}}")});break;}
   const auto folder="airack/"+request.runtime.toDashedString().toStdString()+"/"+std::to_string(request.request)+"/"+id;const auto name="seed-"+std::to_string(request.seeds[n])+"_00001.flac";
   script.script.push_back({"GET","/history/"+id,"",response("{"+quotedJson(id)+":{\"status\":{\"completed\":true,\"status_str\":\"success\"},\"outputs\":{\"7\":{\"audio\":[{\"filename\":"+quotedJson(name)+",\"subfolder\":"+quotedJson(folder)+",\"type\":\"output\"}]}}}}")});
   std::string encoded;for(char c:folder)encoded+=c=='/'?"%2F":std::string(1,c);script.script.push_back({"GET","/view?filename="+name+"&subfolder="+encoded+"&type=output","",response(f.flac,"audio/flac")});
  }
  f.server=std::make_unique<NativeComfyFixture>("","","","",std::move(script));CHECK(f.server->valid());auto http=airack::companion::detail::ComfyHttpTestAccess::fixture(root,f.server->port());
  if(f.holdFinalCallback){f.transportControl.context=&f;f.transportControl.networkPoint=[](void* raw,airack::companion::detail::ComfyNetworkTestPoint point,unsigned code) noexcept{auto& state=*static_cast<Fixture*>(raw);if(point==airack::companion::detail::ComfyNetworkTestPoint::resultSelected)state.selected=true;if(point==airack::companion::detail::ComfyNetworkTestPoint::callbackLeaving&&code==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING&&state.selected&&!state.callbackHeld.exchange(true))state.callbackGate.arrive();};airack::companion::detail::ComfyHttpTestAccess::control(*http,&f.transportControl);}
  auto job=airack::drums::detail::ComfyAudioJobTestAccess::make(std::move(http));f.started=true;return job;
 }
 static void point(void* context,DrumImport::TestPoint point) noexcept {auto& f=*static_cast<Fixture*>(context);if(point==DrumImport::TestPoint::beforeJoin)++f.joins;if(f.holdSource&&point==DrumImport::TestPoint::beforeCandidateInspect||f.holdPrepared&&point==DrumImport::TestPoint::afterHelper||f.holdAfterDrain&&point==DrumImport::TestPoint::afterGenerationDrain)f.sourceGate.arrive();if(f.holdCompletion&&point==DrumImport::TestPoint::beforeCompletion)f.completionGate.arrive();}
 void bind(InstrumentProcessor& p){InstrumentDrumTestAccess::factory(p,this,make);InstrumentDrumTestAccess::hook(p,this,point);}
 unsigned posts(){unsigned count=0;if(server)for(const auto& row:server->captures())count+=row.method=="POST";return count;}
 unsigned foreign(){unsigned count=0;if(server)for(const auto& row:server->captures())count+=!row.matched;return count;}
};
juce::Uuid place(InstrumentProcessor& p,bool tile=false,unsigned bank=0){
 ProjectDocument document;std::uint64_t edit=0;CHECK(p.tryProjectSnapshot(document,edit));CHECK(validateSoundDocument(document.sound));
 RackDesign design;design.name="Generation fixture";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},1}};
 if(tile)design=airack::test::frozenTileArrayRackDesign();
 RackOperation operation;operation.design=design;operation.newInstance=juce::Uuid();operation.allowSlotReuse=true;operation.allocation={{0,std::uint8_t(bank)}};operation.destination={RackRole::synth,masterChain,std::uint8_t(bank)};
 SoundDocument pure;RackDifference pureDifference;const auto pureStatus=makeRackCandidate(document.sound,operation,SoundEditOrigin::manual,pure,pureDifference);
 const auto defaults=newInstanceSound();std::size_t different=0;for(std::size_t n=0;n<parameterCount;++n)if(document.sound.values[n]!=defaults.values[n]){++different;std::cout<<"DEFAULT_DIFFERENCE "<<n<<" "<<document.sound.values[n]<<" "<<defaults.values[n]<<std::endl;}
 for(std::size_t bank=0;bank<8;++bank)for(std::size_t effect=0;effect<effectBankCount;++effect)if(!supportsEffectAlgorithmRevision(effect,defaults.compatibility.effects[bank][effect]))std::cout<<"COMPATIBILITY_DIAGNOSTIC bank="<<bank<<" effect="<<effect<<" value="<<int(defaults.compatibility.effects[bank][effect])<<std::endl;
 for(const auto& bank:defaults.compatibility.synth)for(auto value:bank)if(!supportsSynthAlgorithmRevision(value))std::cout<<"SYNTH_COMPATIBILITY_DIAGNOSTIC value="<<int(value)<<std::endl;
 std::cout<<"SOUND_DIAGNOSTIC defaults="<<validateSoundDocument(defaults)<<" parameters="<<validateParameterValues(document.sound.values)<<" compatibility="<<validateKernelCompatibility(document.sound.compatibility)<<" instances="<<validateRackInstances(document.sound)<<" layout="<<validateRackLayout(document.sound.layout)<<" different="<<different<<" same="<<(document.sound==defaults)<<std::endl;
 SoundDocument preview;RackDifference difference;const auto previewStatus=p.previewRackOperation(operation,edit,preview,difference);
 const auto applied=p.applyRackOperation(operation,edit);
 std::cout<<"PLACE_DIAGNOSTIC valid="<<validateRackDesign(design,RackValidationMode::portableDesign)<<" sound="<<validateSoundDocument(document.sound)<<" project="<<validateProjectDocument(document)<<" edit="<<edit<<" pure="<<int(pureStatus)<<" preview="<<int(previewStatus)<<" applied="<<int(applied)<<" commit="<<int(InstrumentDrumTestAccess::commitStatus(p))<<" layout="<<int(document.sound.layout.synthCount)<<","<<int(document.sound.layout.synthUse[0])<<" ids="<<operation.newInstance.toString()<<","<<design.id.toString()<<std::endl;
 CHECK(applied==RackCandidateStatus::ready);if(applied!=RackCandidateStatus::ready)return juce::Uuid::null();p.prepareToPlay(48000,512);return operation.newInstance;
}
DrumPadTarget target(InstrumentProcessor& p,juce::Uuid rack){DrumPadTarget t;DrumCommitError error{};CHECK(p.captureDrumPadTarget(rack,0,15,t,error));return t;}
bool ready(InstrumentProcessor& p){return wait([&]{return p.drumImport().view().inspected&&p.drumImport().view().generationStatus!=DrumGenerationStatus::invalidRequest;});}
void finish(InstrumentProcessor& p){p.drumImport().cancel();CHECK(wait([&]{return InstrumentDrumTestAccess::counts(p).slots==0&&!p.drumImport().busy();}));}
void exportFullKit(InstrumentProcessor&,const juce::Uuid&,std::uint64_t expectedSeed=73);
void workflow(){
 std::cout<<"GROUP generation_real_three_sources_prepare_audition_keep_recall"<<std::endl;
 Fixture fixture;auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto& p=*storage;const auto rack=place(p,true);if(rack.isNull())return;const auto captured=target(p,rack);fixture.bind(p);DrumCommitError error{};
 CHECK(p.beginDrumGeneration(captured,juce::String::fromUTF8("isolated \"dry\" test \\ caf\xc3\xa9"),{17,73,211},error));CHECK(!p.drumImport().keep());CHECK(ready(p));
 const auto initial=p.drumImport().view();CHECK(initial.target==captured&&initial.generation&&initial.generationStatus==DrumGenerationStatus::complete);CHECK(initial.begin==0&&initial.end==442368&&initial.gain==1&&!initial.canPrepare&&!initial.canKeep);CHECK(fixture.posts()==3&&fixture.foreign()==0);
 const std::array<std::uint64_t,3> seeds{17,73,211};for(std::size_t n=0;n<3;++n)CHECK(initial.candidates[n].available&&initial.candidates[n].seed==seeds[n]&&initial.candidates[n].source.md5Verified);
 std::uint64_t fourth=0;CHECK(!p.beginDrumInput(fourth,error)&&fourth==0);CHECK(!p.drumImport().open(captured));
 CHECK(p.selectDrumGenerationCandidate(initial.session,1));CHECK(wait([&]{auto v=p.drumImport().view();return v.inspected&&v.selectedCandidate==1;}));CHECK(p.drumImport().view().target==captured);
 p.drumImport().edit(0,441000,.125f,"Generated fixture");p.drumImport().prepare();CHECK(wait([&]{return p.drumImport().view().canKeep;}));const auto prepared=p.drumImport().view();CHECK(prepared.preparedFrames==480000&&prepared.preparedPeak>0&&prepared.preparedPeak<.04f);
 p.drumImport().audition();CHECK(wait([&]{return p.drumImport().view().phase==DrumImportPhase::auditioning;}));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);CHECK(InstrumentDrumTestAccess::counts(p).preview==480000*8);
 const auto trim=drumParameterStart+15*drumParametersPerPad+3;CHECK(p.beginParameterGesture(trim));CHECK(p.setParameterGestureValue(trim,.2f));CHECK(p.endParameterGesture(trim));
 CHECK(p.drumImport().keep());CHECK(wait([&]{p.processBlock(audio,midi);return InstrumentDrumTestAccess::counts(p).slots==0&&InstrumentDrumTestAccess::counts(p).preview==0;}));CHECK(!p.drumImport().keep());
 DrumPadTarget kept;CHECK(p.captureDrumPerformanceTarget(rack,0,15,kept,error));DrumPadReadback value;CHECK(p.readDrumPad(kept,value,error));CHECK(value.frames==480000&&value.channels==2&&value.metadata.name=="Generated fixture"&&value.metadata.source&&value.metadata.source->kind==SourceKind::generated&&value.metadata.source->seed==73&&value.metadata.source->end==441000&&value.metadata.source->gain==.125f);CHECK(p.control(trim).getValue()==.2f);
 for(unsigned n=0;n<15;++n){DrumPadTarget neighbour;DrumPadReadback empty;CHECK(p.captureDrumPerformanceTarget(rack,0,std::uint8_t(n),neighbour,error));CHECK(p.readDrumPad(neighbour,empty,error)&&!empty.metadata.asset);}
 CHECK(p.undoSoundEdit());CHECK(p.control(trim).getValue()==.2f);CHECK(p.redoSoundEdit());
 juce::MemoryBlock state;p.getStateInformation(state);CHECK(state.getSize()>0);const auto preset=output.getChildFile("generated-owned-fixture.airack");CHECK(p.saveSoundPreset(preset));
 storage.reset();if(fixture.server)fixture.server->join();CHECK(fixture.foreign()==0);
 auto recall=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(recall->restoreSoundState(state.getData(),int(state.getSize())));recall->prepareToPlay(48000,512);DrumPadTarget restored;CHECK(recall->captureDrumPerformanceTarget(rack,0,15,restored,error));DrumPadReadback read;CHECK(recall->readDrumPad(restored,read,error)&&read.metadata==value.metadata);CHECK(recall->enqueueDrumPad(restored,100));recall->processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);CHECK(recall->loadSoundPreset(preset));CHECK(fixture.posts()==3);exportFullKit(*recall,rack);CHECK(fixture.posts()==3);
}
void lifetime(){
 std::cout<<"GROUP real_owned_raw_cancel_draining_and_atomic_admission"<<std::endl;
 Fixture fixture;fixture.holdSource=true;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;const auto captured=target(*p,rack);fixture.bind(*p);DrumCommitError error{};
 CHECK(p->beginDrumGeneration(captured,"source barrier fixture",{17,73,211},error));CHECK(fixture.sourceGate.reached());auto before=InstrumentDrumTestAccess::counts(*p);CHECK(before.slots==3&&before.encoded==48ull*1024*1024&&before.workspace>=40ull*1024*1024+393224);
 p->cancelDrumGeneration(p->drumImport().view().session);auto cancelled=InstrumentDrumTestAccess::counts(*p);CHECK(cancelled.slots==3&&cancelled.encoded==before.encoded&&cancelled.workspace==before.workspace);std::uint64_t fourth=0;CHECK(!p->beginDrumInput(fourth,error)&&fourth==0);CHECK(!p->beginDrumGeneration(captured,"blocked until actual drain",{1,2,3},error));
 fixture.sourceGate.release();CHECK(wait([&]{return InstrumentDrumTestAccess::counts(*p).slots==0&&p->drumImport().view().phase==DrumImportPhase::closed;}));CHECK(InstrumentDrumTestAccess::counts(*p).encoded==0);CHECK(fixture.posts()==3&&fixture.foreign()==0);
 std::array<std::uint64_t,2> occupied{};CHECK(p->beginDrumInput(occupied[0],error));CHECK(!p->beginDrumGeneration(captured,"no partial atomic claim",{17,73,211},error));CHECK(InstrumentDrumTestAccess::counts(*p).slots==1);CHECK(p->beginDrumInput(occupied[1],error));CHECK(!p->beginDrumGeneration(captured,"two occupied",{17,73,211},error));CHECK(InstrumentDrumTestAccess::counts(*p).slots==2);for(auto id:occupied){p->cancelDrumInput(id);InstrumentDrumTestAccess::release(*p,id);}CHECK(InstrumentDrumTestAccess::counts(*p).slots==0);
}
void actual_http_callback_drain_before_processor_destruction(){
 std::cout<<"GROUP actual_HTTP_final_callback_held_through_processor_join"<<std::endl;
 Fixture fixture;fixture.holdFinalCallback=true;auto storage=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());auto* p=storage.get();const auto rack=place(*p);if(rack.isNull())return;const auto captured=target(*p,rack);fixture.bind(*p);DrumCommitError error{};
 CHECK(p->beginDrumGeneration(captured,"actual pending WinHTTP callback",{17,73,211},error));const bool held=fixture.callbackGate.reached();CHECK(held);
 if(held){const auto before=InstrumentDrumTestAccess::counts(*p);CHECK(before.slots==3&&before.encoded==48ull*1024*1024);const auto live=airack::companion::detail::ComfyHttpTestAccess::live();CHECK(live[0]>0&&live[1]>0);
  p->cancelDrumGeneration(p->drumImport().view().session);p->closeAiSession();const auto cancelling=InstrumentDrumTestAccess::counts(*p);CHECK(cancelling.slots==3&&cancelling.encoded==before.encoded&&cancelling.workspace==before.workspace);std::uint64_t fourth=0;CHECK(!p->beginDrumInput(fourth,error)&&fourth==0);
  auto* importer=&p->drumImport();std::atomic_bool destroyed{false},dispatched{false};std::thread retirement([&]{storage.reset();destroyed=true;});const bool joining=wait([&]{return fixture.joins.load()!=0;});CHECK(joining);
  if(joining&&!destroyed){CHECK(InstrumentDrumTestAccess::teardownAvailable(*p,*importer));CHECK(InstrumentDrumTestAccess::callbacksStopped(*p));const auto draining=InstrumentDrumTestAccess::counts(*p);CHECK(draining.slots==3&&draining.encoded==before.encoded&&draining.workspace==before.workspace);CHECK(juce::MessageManager::callAsync([&]{dispatched=true;}));CHECK(wait([&]{return dispatched.load();}));CHECK(!destroyed);}
  fixture.callbackGate.release();CHECK(wait([&]{return destroyed.load();}));retirement.join();
 }
 fixture.callbackGate.release();storage.reset();if(fixture.server)fixture.server->join();CHECK((airack::companion::detail::ComfyHttpTestAccess::live()==std::array<unsigned,2>{}));CHECK(fixture.posts()==1&&fixture.foreign()==0);
}
void instances(){
 std::cout<<"GROUP two_independent_processor_batches"<<std::endl;
 Fixture fa,fb;auto a=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()),b=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto ra=place(*a),rb=place(*b);if(ra.isNull()||rb.isNull())return;const auto ta=target(*a,ra),tb=target(*b,rb);fa.bind(*a);fb.bind(*b);DrumCommitError error{};
 CHECK(a->beginDrumGeneration(ta,"instance A",{17,73,211},error));CHECK(b->beginDrumGeneration(tb,"instance B",{0,1,INT64_MAX},error));CHECK(ready(*a)&&ready(*b));CHECK(fa.captured.runtime!=fb.captured.runtime&&fa.root!=fb.root);CHECK(InstrumentDrumTestAccess::counts(*a).encoded+InstrumentDrumTestAccess::counts(*b).encoded==96ull*1024*1024);
 finish(*a);CHECK(InstrumentDrumTestAccess::counts(*b).slots==3&&b->drumImport().view().inspected);std::uint64_t id=0;CHECK(!b->beginDrumInput(id,error));CHECK(fa.foreign()==0&&fb.foreign()==0);finish(*b);
}
bool prepareGenerated(Fixture& fixture,InstrumentProcessor& p,const DrumPadTarget& captured){
 fixture.bind(p);DrumCommitError error{};const bool admitted=p.beginDrumGeneration(captured,"isolated lifecycle fixture",{17,73,211},error);CHECK(admitted);if(!admitted)return false;
 const bool inspected=ready(p);CHECK(inspected);if(!inspected)return false;
 p.drumImport().edit(0,441000,.125f,"Generated lifecycle");p.drumImport().prepare();const bool prepared=wait([&]{return p.drumImport().view().canKeep;});CHECK(prepared);return prepared;
}
void committed_observer_failure(){
 std::cout<<"GROUP committed_keep_survives_observer_allocation_failure"<<std::endl;
 Fixture fixture;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;const auto captured=target(*p,rack);
 if(!prepareGenerated(fixture,*p,captured)){finish(*p);return;}
 const auto history=InstrumentDrumTestAccess::history(*p);unsigned observed=0;
 InstrumentDrumTestAccess::observer(*p,&observed,[](void* raw,InstrumentDrumTestAccess::ProcessorPoint point){if(point==InstrumentDrumTestAccess::ProcessorPoint::afterCoreInstall){++*static_cast<unsigned*>(raw);throw std::bad_alloc();}});
 const bool committed=p->drumImport().keep();InstrumentDrumTestAccess::observer(*p,nullptr,nullptr);
 CHECK(committed&&observed==1);CHECK(!p->drumImport().view().canKeep);CHECK(!p->drumImport().keep());
 DrumPadTarget current;DrumPadReadback pad;DrumCommitError error{};CHECK(p->captureDrumPerformanceTarget(rack,0,15,current,error));CHECK(p->readDrumPad(current,pad,error)&&pad.frames==480000&&pad.metadata.name=="Generated lifecycle");
 const auto after=InstrumentDrumTestAccess::history(*p);CHECK(after.first==history.first+1&&after.second==history.second+1);std::cout<<"OBSERVER_KEEP returned="<<committed<<" installed="<<bool(pad.metadata.asset)<<" observed="<<observed<<std::endl;finish(*p);
}
void partial_and_full_session_arbitration(){
 std::cout<<"GROUP partial_unrun_and_full_session_bidirectional_busy"<<std::endl;
 Fixture fixture;fixture.failedOrdinal=1;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;const auto captured=target(*p,rack);fixture.bind(*p);DrumCommitError error{};
 CHECK(p->drumImport().openGeneration(captured));CHECK(InstrumentDrumTestAccess::counts(*p).slots==0&&!fixture.started);
 AiRequestOptions options{"isolated pattern fixture","synthetic-selected_model:1.0",{ai::RequestScope::pattern,true,true}};
 CHECK(!p->beginAiRequest(options));CHECK(!p->drumImport().open(captured));
 std::uint64_t raw=0;CHECK(p->beginDrumInput(raw,error)&&raw!=0);CHECK(!p->beginAiRequest(options));p->cancelDrumInput(raw);InstrumentDrumTestAccess::release(*p,raw);
 for(const auto& text:{juce::String{},juce::String::repeatedString("x",2049),juce::String("control\ntext")}){CHECK(!p->beginDrumGeneration(captured,text,{17,73,211},error)&&error==DrumCommitError::invalid);CHECK(InstrumentDrumTestAccess::counts(*p).slots==0&&!fixture.started);}
 CHECK(!p->beginDrumGeneration(captured,"distinct seeds required",{17,17,211},error));CHECK(!p->beginDrumGeneration(captured,"native int63",{17,73,std::uint64_t(INT64_MAX)+1},error));
 CHECK(p->beginDrumGeneration(captured,"partial fixture",{17,73,211},error));CHECK(ready(*p));const auto partial=p->drumImport().view();CHECK(partial.generationStatus==DrumGenerationStatus::partial&&partial.candidates[0].available&&!partial.candidates[1].available&&!partial.candidates[2].available&&partial.candidates[2].status==ComfyAttemptStatus::unrun);CHECK(fixture.posts()==2&&fixture.foreign()==0);
 CHECK(!p->selectDrumGenerationCandidate(partial.session,2)&&p->drumImport().view().draft==partial.draft);CHECK(!p->beginAiRequest(options));CHECK(!p->drumImport().open(captured));
 p->closeAiSession();CHECK(p->drumImport().view().inspected&&p->drumImport().view().session==partial.session&&InstrumentDrumTestAccess::counts(*p).slots==3);
 finish(*p);auto source=std::make_unique<ProjectDocument>();std::uint64_t sourceRevision=0;CHECK(p->tryProjectSnapshot(*source,sourceRevision));
 CHECK(airack::testing::beginAiRequest(*p,options));const auto* localRequest=p->aiCurrentRequest();
 CHECK(localRequest&&localRequest->source==*source&&localRequest->tag.editRevision==sourceRevision&&p->aiProposalPhase()==AiProposalPhase::pending);
 CHECK(InstrumentDrumTestAccess::aiRootRetained(*p)&&p->aiOriginalSource()&&*p->aiOriginalSource()==*source);
 p->closeAiSession();CHECK(wait([&]{return !InstrumentDrumTestAccess::aiRootRetained(*p)&&!p->aiOriginalSource()&&!p->aiCurrentRequest();}));
 raw=0;CHECK(InstrumentDrumTestAccess::counts(*p).aiRoots);CHECK(!p->beginDrumInput(raw,error)&&raw==0&&error==DrumCommitError::busy);
 CHECK(wait([&]{return p->beginDrumInput(raw,error);}));CHECK(raw!=0&&!InstrumentDrumTestAccess::counts(*p).aiRoots);if(raw){p->cancelDrumInput(raw);InstrumentDrumTestAccess::release(*p,raw);}
 Fixture other;auto q=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());other.bind(*q);const auto aiRequest=airack::testing::beginAiRequest(*q,options,120);CHECK(aiRequest&&q->aiProposalPhase()==AiProposalPhase::pending);const auto qrack=place(*q);if(qrack.isNull())return;const auto qtarget=target(*q,qrack);CHECK(InstrumentDrumTestAccess::aiRootRetained(*q));
 CHECK(!q->beginDrumGeneration(qtarget,"AI owns its real retained root",{17,73,211},error)&&error==DrumCommitError::busy);CHECK(!q->drumImport().open(qtarget));CHECK(!other.started&&InstrumentDrumTestAccess::counts(*q).slots==0);q->closeAiSession();CHECK(q->drumImport().openGeneration(qtarget));q->drumImport().cancel();CHECK(q->drumImport().view().phase==DrumImportPhase::closed);
}
void stopped_preview_remains_busy_until_ack(){
 std::cout<<"GROUP canceled_manual_preview_storage_blocks_AI_until_actual_ack"<<std::endl;
 Fixture fixture;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;const auto captured=target(*p,rack);if(!prepareGenerated(fixture,*p,captured)){finish(*p);return;}
 p->drumImport().audition();CHECK(wait([&]{return p->drumImport().view().phase==DrumImportPhase::auditioning;}));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;p->processBlock(audio,midi);finish(*p);
 CHECK(InstrumentDrumTestAccess::counts(*p).preview==480000*8);AiRequestOptions options{"isolated pattern fixture","synthetic-selected_model:1.0",{ai::RequestScope::pattern,true,true}};
 DrumCommitError error{};CHECK(!p->beginDrumGeneration(captured,"player is really draining",{17,73,211},error)&&error==DrumCommitError::busy);
 ProjectDocument project;std::uint64_t edit=0;CHECK(p->tryProjectSnapshot(project,edit));RackOperation remove;remove.kind=RackOperationKind::remove;remove.selected=rack;CHECK(p->applyRackOperation(remove,edit)==RackCandidateStatus::ready);CHECK(InstrumentDrumTestAccess::counts(*p).preview==480000*8);
 const auto request=airack::testing::beginAiRequest(*p,options,120);CHECK(!request);if(request)p->closeAiSession();
 p->processBlock(audio,midi);CHECK(wait([&]{return InstrumentDrumTestAccess::counts(*p).preview==0;}));const auto accepted=airack::testing::beginAiRequest(*p,options,120);CHECK(accepted&&p->aiProposalPhase()==AiProposalPhase::pending);p->closeAiSession();
}
void stale_http_and_preparation_draft(){
 std::cout<<"GROUP actual_http_then_stale_target_and_held_preparation_draft"<<std::endl;
 for(unsigned mutation=0;mutation<3;++mutation){
  Fixture fixture;fixture.holdHttp=true;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;const auto captured=target(*p,rack);fixture.bind(*p);DrumCommitError error{};juce::MemoryBlock saved;p->getStateInformation(saved);
  CHECK(p->beginDrumGeneration(captured,"held submit authority",{17,73,211},error));const bool entered=fixture.httpGate.reached();CHECK(entered);if(entered){
   if(mutation==0)CHECK(p->restoreSoundState(saved.getData(),int(saved.getSize())));
   else if(mutation==1){SoundDocument state;std::uint64_t edit=0;CHECK(p->trySoundSnapshot(state,edit));state.controlLocks[drumParameterStart+15*drumParametersPerPad]=true;CHECK(p->applySoundEdit(state,SoundEditOrigin::manual,edit));}
   else p->control(drumModeParameterStart).setValue(0);
   DrumPadTarget fresh;CHECK(!p->captureDrumPadTarget(rack,0,15,fresh,error)||fresh!=captured);
  }
  fixture.httpGate.release();CHECK(wait([&]{return InstrumentDrumTestAccess::counts(*p).slots==0;}));CHECK(!p->drumImport().view().canKeep&&fixture.posts()==1&&fixture.foreign()==0);p->drumImport().cancel();
 }
 Fixture fixture;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;const auto captured=target(*p,rack);fixture.bind(*p);DrumCommitError error{};
 CHECK(p->beginDrumGeneration(captured,"held helper authority",{17,73,211},error));CHECK(ready(*p));fixture.holdPrepared=true;p->drumImport().edit(0,441000,.125f,"old tuple");p->drumImport().prepare();const bool entered=fixture.sourceGate.reached();CHECK(entered);if(entered){const auto old=p->drumImport().view().draft;const auto held=InstrumentDrumTestAccess::counts(*p);CHECK(held.slots==3&&held.decoded>=7680000);p->drumImport().edit(0,442369,.125f,"invalid new tuple");CHECK(p->drumImport().view().draft==old+1&&!p->drumImport().view().canKeep);CHECK(InstrumentDrumTestAccess::counts(*p).decoded==held.decoded);}
 fixture.sourceGate.release();CHECK(wait([&]{return InstrumentDrumTestAccess::counts(*p).decoded==0;}));CHECK(!p->drumImport().view().canKeep);DrumPadReadback pad;CHECK(p->readDrumPad(captured,pad,error)&&!pad.metadata.asset);finish(*p);
}
template<class T>T* componentOf(juce::Component& parent){if(auto* found=dynamic_cast<T*>(&parent))return found;for(auto* child:parent.getChildren())if(auto* found=componentOf<T>(*child))return found;return nullptr;}
bool accessiblePress(juce::Component& component){auto* handler=component.getAccessibilityHandler();return handler&&handler->getActions().invoke(juce::AccessibilityActionType::press);}
juce::String label(juce::Component& parent,const juce::String& id){if(parent.getComponentID()==id)if(auto* text=dynamic_cast<juce::Label*>(&parent))return text->getText();for(auto* child:parent.getChildren()){const auto text=label(*child,id);if(text.isNotEmpty())return text;}return {};}
juce::File evidenceOutput(const juce::File& explicitPath,const juce::File& root){return explicitPath!=juce::File{}?explicitPath:root.getChildFile("generation-evidence").getChildFile(juce::Uuid().toString());}
void outputPathContract(){
 const auto existing=juce::File::getSpecialLocation(juce::File::tempDirectory);const auto root=existing.getChildFile("air-path-contract-"+juce::Uuid().toString());const auto parent=root.getChildFile("generation-evidence");
 const auto first=evidenceOutput({},root),second=evidenceOutput({},root);CHECK(first!=second);CHECK(first.getParentDirectory()==parent&&second.getParentDirectory()==parent);CHECK(first.isAChildOf(parent)&&second.isAChildOf(parent));
 CHECK(existing.isDirectory());CHECK(evidenceOutput(existing,root)==existing);CHECK(evidenceOutput(existing.getChildFile("exact-output"),root)==existing.getChildFile("exact-output"));
}
void capture(juce::Component& component,const juce::String& name){const auto file=output.getChildFile(name+".png");CHECK(!file.exists());juce::FileOutputStream stream(file);CHECK(stream.openedOk());CHECK(juce::PNGImageFormat().writeImageToStream(component.createComponentSnapshot(component.getLocalBounds(),true,1),stream));}
bool postedText(juce::Component& peerOwner,juce::TextEditor& field,const juce::String& text,int bulkPrefixCharacters=0){
 InputWaitObservation focusWait,textWait;InputWaitScope restoreWait;inputWaitObservation=&focusWait;const auto begin=GetTickCount64();
 field.grabKeyboardFocus();CHECK(wait([&]{return field.hasKeyboardFocus(false);}));const auto focusEnd=GetTickCount64();field.selectAll();field.insertTextAtCaret({});juce::MessageManager::getInstance()->runDispatchLoopUntil(30);const auto resetEnd=GetTickCount64();auto* peer=peerOwner.getPeer();if(!peer){std::cout<<"MEASURE posted_input peer_missing=1"<<std::endl;return false;}const auto hwnd=static_cast<HWND>(peer->getNativeHandle());
 const auto inputDeadline=Clock::now()+std::chrono::seconds(20);
 const auto postBegin=GetTickCount64();if(bulkPrefixCharacters<0||bulkPrefixCharacters>text.length())return false;
 // Long boundary fixtures preload through the real text-input interface (also
 // used for bulk input), then cross the boundary with native WM_CHAR. Keep the
 // byte-limit/UI assertions without making2049 separate key layouts a CPU gate.
 if(bulkPrefixCharacters){juce::TextInputTarget& input=field;input.insertTextAtCaret(text.substring(0,bulkPrefixCharacters));}
 unsigned posted=0;for(auto character:text.substring(bulkPrefixCharacters)){if(!PostMessageW(hwnd,WM_CHAR,WPARAM(character),1)){std::cout<<"MEASURE posted_input post_failed_count="<<posted<<std::endl;return false;}++posted;}const auto postEnd=GetTickCount64();inputWaitObservation=&textWait;int sampledBytes=-1;
 const bool matched=wait([&]{const auto value=field.getText();sampledBytes=value.getNumBytesAsUTF8();return value==text;},inputDeadline)&&Clock::now()<=inputDeadline;const auto waitEnd=GetTickCount64();juce::MessageManager::getInstance()->runDispatchLoopUntil(40);const auto stabilityEnd=GetTickCount64();juce::String stableSample;const bool stable=matched&&(stableSample=field.getText())==text;
 std::cout<<"MEASURE native_character_delivery posted="<<posted<<" bulk_prefix_characters="<<bulkPrefixCharacters<<" input_budget_ms=20000 input_ms="<<waitEnd-postBegin<<std::endl;
 std::cout<<"MEASURE posted_input expected_bytes="<<text.getNumBytesAsUTF8()<<" posted="<<posted<<" begin_tick="<<begin<<" focus_end_tick="<<focusEnd<<" reset_end_tick="<<resetEnd<<" post_begin_tick="<<postBegin<<" post_end_tick="<<postEnd<<" wait_end_tick="<<waitEnd<<" stability_end_tick="<<stabilityEnd<<" sampled_bytes="<<sampledBytes<<" stable_sampled="<<matched<<" stable_bytes="<<(matched?stableSample.getNumBytesAsUTF8():-1)<<" stable="<<stable<<" focus_at_report="<<field.hasKeyboardFocus(false)<<std::endl;focusWait.report("focus");textWait.report("text");
 if(!stable)std::cout<<"POSTED_TEXT_DIAGNOSTIC id="<<field.getComponentID()<<" expected_bytes="<<text.getNumBytesAsUTF8()<<" actual_bytes="<<field.getText().getNumBytesAsUTF8()<<" actual="<<field.getText().substring(0,80)<<std::endl;
 return stable;
}
#if defined(_MSC_VER) && defined(_DEBUG)
namespace generationHeap {
struct Row{long request=0;std::size_t bytes=0;};
struct Result{std::size_t live=0,peak=0,overlap=0,allocated=0,freed=0,baselineFreed=0,denials=0;std::array<std::size_t,512> requests{};unsigned count=0;bool valid=true;};
static std::array<Row,16384> rows;
static Result result;
static SRWLOCK lock=SRWLOCK_INIT;
static std::atomic_bool active{false};
thread_local bool inside=false,allowDeny=false;
static std::size_t denySize=0;
void remove(void* data,int use) noexcept{
 if(!data)return;long id=0;const auto n=_msize_dbg(data,use);
 if(n>UINT_MAX||!_CrtIsMemoryBlock(data,unsigned(n),&id,nullptr,nullptr)){result.valid=false;return;}
 const auto row=std::find_if(rows.begin(),rows.end(),[&](const Row&r){return r.request==id;});
 if(row==rows.end()){result.baselineFreed+=n;return;}if(row->bytes>result.live){result.valid=false;return;}
 result.live-=row->bytes;result.freed+=row->bytes;*row={};
}
int __cdecl hook(int kind,void* data,std::size_t bytes,int use,long request,const unsigned char*,int){
 if(!active.load(std::memory_order_acquire)||inside)return 1;inside=true;AcquireSRWLockExclusive(&lock);
 struct Exit{~Exit(){ReleaseSRWLockExclusive(&lock);inside=false;}}exit;
 if(data&&(kind==_HOOK_FREE||kind==_HOOK_REALLOC)){use=_CrtReportBlockType(data);if(use==-1){result.valid=false;return 1;}}
 use&=0xffff;if(use!=_NORMAL_BLOCK&&use!=_CLIENT_BLOCK)return 1;
 if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC){
  if(result.count<result.requests.size())result.requests[result.count]=bytes;++result.count;
  if(allowDeny&&denySize&&bytes==denySize&&!result.denials){++result.denials;return 0;}
  if(kind==_HOOK_REALLOC){result.overlap=std::max(result.overlap,result.live+bytes);remove(data,use);}
  auto row=std::find_if(rows.begin(),rows.end(),[](const Row&r){return r.request==0;});if(request<=0||row==rows.end()){result.valid=false;return 1;}
  *row={request,bytes};result.live+=bytes;result.allocated+=bytes;result.peak=std::max(result.peak,result.live);
 }else if(kind==_HOOK_FREE)remove(data,use);return 1;
}
int __cdecl report(int type,wchar_t* text,int* value){
 if(allowDeny&&result.denials&&type==_CRT_WARN&&std::wstring_view(text?text:L"")==L"Client hook allocation failure.\n"){if(value)*value=0;return 1;}return 0;
}
template<class F>Result observe(F&& work,std::size_t deny=0){
 rows.fill({});result={};denySize=deny;inside=false;allowDeny=deny!=0;_CrtMemState before{},after{};_CrtMemCheckpoint(&before);
 const auto prior=_CrtSetAllocHook(hook);_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,report);active.store(true,std::memory_order_release);
 try{work();}catch(...){active=false;allowDeny=false;_CrtSetAllocHook(prior);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,report);throw;}
 active.store(false,std::memory_order_release);allowDeny=false;_CrtSetAllocHook(prior);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,report);_CrtMemCheckpoint(&after);
 const auto out=result;CHECK(out.valid);const auto delta=std::int64_t(after.lSizes[_NORMAL_BLOCK])-std::int64_t(before.lSizes[_NORMAL_BLOCK])+std::int64_t(after.lSizes[_CLIENT_BLOCK])-std::int64_t(before.lSizes[_CLIENT_BLOCK]);
 CHECK(std::int64_t(out.live)-std::int64_t(out.baselineFreed)==delta);return out;
}
bool saw(const Result&r,std::size_t bytes){return std::find(r.requests.begin(),r.requests.begin()+std::min<std::size_t>(r.count,r.requests.size()),bytes)!=r.requests.begin()+std::min<std::size_t>(r.count,r.requests.size());}
std::size_t peak(const Result&r){return std::max(r.peak,r.overlap);}
void calibrate(){
 auto block=observe([]{juce::MemoryBlock bytes(1024,false);});CHECK(block.live==0&&block.peak==1024&&block.allocated==1024&&block.freed==1024);
 const auto start=CreateEventW(nullptr,TRUE,FALSE,nullptr),done=CreateEventW(nullptr,TRUE,FALSE,nullptr),release=CreateEventW(nullptr,TRUE,FALSE,nullptr);CHECK(start&&done&&release);if(!start||!done||!release)return;
 std::atomic_bool allocated{false};std::thread other([&]{WaitForSingleObject(start,INFINITE);auto*p=_malloc_dbg(34567,_NORMAL_BLOCK,__FILE__,__LINE__);allocated=p!=nullptr;if(p)_free_dbg(p,_NORMAL_BLOCK);SetEvent(done);WaitForSingleObject(release,INFINITE);});
 bool complete=false;auto cross=observe([&]{SetEvent(start);complete=WaitForSingleObject(done,3000)==WAIT_OBJECT_0;});SetEvent(release);other.join();CloseHandle(start);CloseHandle(done);CloseHandle(release);
 CHECK(complete&&allocated&&saw(cross,34567)&&cross.allocated>=34567&&cross.live==0);
}
}
#endif
void optional_owned_host_dispatch_controls(){
 std::cout<<"GROUP existing_fixture_optional_bounded_dynamic_dispatch_default_ephemeral"<<std::endl;
 for(unsigned mode=0;mode<6;++mode){FixtureOptions options;std::atomic_uint calls{0};options.ownedHostReply=[&](const Capture& capture)->OwnedHostReply{++calls;if(mode==3)throw std::bad_alloc();if(mode==4)return {true,{}};if(mode==5)return {true,std::string(16u*1024*1024+4097,'x')};return {capture.method=="POST"&&capture.target=="/owned-control"&&capture.body=="{\"fixture\":1}",response("{\"owned\":true}")};};NativeComfyFixture fixture("","","","",std::move(options));CHECK(fixture.valid()&&fixture.port()!=0&&fixture.port()!=8189);std::string reply;const auto body=mode==2?"{\"fixture\":2}":"{\"fixture\":1}";const auto route=mode==1?"/foreign":"/owned-control";const auto wire="POST "+std::string(route)+" HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: "+std::to_string(std::strlen(body))+"\r\nConnection: close\r\n\r\n"+body;CHECK(independentExchange(fixture.port(),wire,reply));fixture.join();const auto capture=fixture.captured();CHECK(calls==1&&capture.complete&&capture.matched==(mode==0));CHECK(reply==(mode==0?response("{\"owned\":true}"):response("mismatch","text/plain",400)));}
 FixtureOptions bad;bad.fixedProtocol8189=true;NativeComfyFixture absent("","","","",std::move(bad));CHECK(!absent.valid()&&absent.port()==0);FixtureOptions mixed;mixed.ownedHostReply=[](const Capture&){return OwnedHostReply{};};mixed.script.push_back({"GET","/","",""});NativeComfyFixture invalid("","","","",std::move(mixed));CHECK(!invalid.valid()&&invalid.port()==0);
}
void admission_arithmetic_preserves_all_free_slots(){
 std::cout<<"GROUP atomic_batch_authority_and_workspace_overflow_before_factory"<<std::endl;
 Fixture fixture;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;fixture.bind(*p);const auto captured=target(*p,rack);DrumCommitError error{};const auto original=InstrumentDrumTestAccess::admissionCounters(*p);
 for(unsigned kind=0;kind<2;++kind){auto changed=original;if(kind==0)changed[0]=UINT64_MAX-2;else changed[1]=UINT64_MAX;InstrumentDrumTestAccess::admissionCounters(*p,changed);CHECK(!p->beginDrumGeneration(captured,"checked admission",{17,73,211},error));CHECK(error==(kind==0?DrumCommitError::authorityExhausted:DrumCommitError::workspaceBudget));const auto after=InstrumentDrumTestAccess::counts(*p);CHECK(after.slots==0&&after.encoded==0&&after.workspace==changed[1]&&!fixture.started);CHECK(InstrumentDrumTestAccess::admissionCounters(*p)==changed);InstrumentDrumTestAccess::admissionCounters(*p,original);}
 CHECK(p->beginDrumGeneration(captured,"positive after exact refusal",{17,73,211},error));CHECK(ready(*p));finish(*p);
}
void assignContinuityPad(InstrumentProcessor& p,const juce::Uuid& rack,unsigned pad,std::span<const float> samples,unsigned channels=1){
 DrumPadTarget captured;DrumCommitError error{};CHECK(p.captureDrumPadTarget(rack,0,std::uint8_t(pad),captured,error));std::uint64_t ticket=0;CHECK(p.prepareDrumAsset(samples,channels,ticket,error));DrumPad metadata;metadata.name="Continuity";metadata.asset=InstrumentDrumTestAccess::preparedId(p,ticket);CHECK(p.assignPreparedDrumPad(captured,ticket,metadata,{},error));
}
void exportFullKit(InstrumentProcessor& p,const juce::Uuid& rack,std::uint64_t expectedSeed){
 std::cout<<"GROUP actual_generated_and_manual_Keep_full16_playable_kit_Project_exports"<<std::endl;
 for(unsigned pad=0;pad<15;++pad){std::vector<float> pcm(9600);for(unsigned frame=0;frame<4800;++frame){pcm[2*frame]=float(std::sin((frame+1)*(.02+pad*.001))*.03);pcm[2*frame+1]=float(std::cos((frame+1)*(.025+pad*.001))*.02);}assignContinuityPad(p,rack,pad,pcm,2);}
 CHECK(wait([&]{return !p.drumImport().busy();}));const auto original=output.getChildFile("product-owned-manual.wav");CHECK(!original.exists());juce::WavAudioFormat format;std::unique_ptr<juce::AudioFormatWriter> writer(format.createWriterFor(new juce::FileOutputStream(original),48000,2,24,{},0));CHECK(writer!=nullptr);juce::AudioBuffer<float> signal(2,4800);for(int c=0;c<2;++c)for(int frame=0;frame<4800;++frame)signal.setSample(c,frame,float(std::sin(frame*.04+c)*.2));if(writer)CHECK(writer->writeFromAudioSampleBuffer(signal,0,4800));writer.reset();
 DrumPadTarget manual;DrumCommitError error{};CHECK(p.captureDrumPadTarget(rack,0,0,manual,error));CHECK(p.drumImport().open(manual));p.drumImport().choose(original);CHECK(wait([&]{return p.drumImport().view().inspected;}));p.drumImport().edit(100,4700,.5f,"Manual product hit");p.drumImport().prepare();CHECK(wait([&]{return p.drumImport().view().canKeep;}));CHECK(p.drumImport().view().preparedFrames==4600);CHECK(p.drumImport().keep());CHECK(wait([&]{return !p.drumImport().busy();}));CHECK(p.undoSoundEdit());CHECK(p.redoSoundEdit());
 const auto physical=[&](std::size_t index,float value){const auto& field=parameterRegistry()[index];juce::NormalisableRange<float> range(field.minimum,field.maximum,field.kind==ParameterKind::continuous?0.f:1.f,field.skew);CHECK(p.beginParameterGesture(index));CHECK(p.setParameterGestureValue(index,range.convertTo0to1(value)));CHECK(p.endParameterGesture(index));};
 for(unsigned pad=0;pad<16;++pad){const auto first=drumParameterStart+pad*drumParametersPerPad;physical(first+7,float(36+pad));if(pad<2){physical(first,-4.f-pad);physical(first+1,pad? .3f:-.25f);physical(first+2,pad? -2.f:3.f);physical(first+3,.05f);physical(first+4,.9f);physical(first+5,.002f);physical(first+6,.25f);physical(first+8,3);}}
 juce::Array<juce::var> pads;for(unsigned pad=0;pad<16;++pad){DrumPadTarget t;DrumPadReadback value;CHECK(p.captureDrumPerformanceTarget(rack,0,std::uint8_t(pad),t,error));CHECK(p.readDrumPad(t,value,error)&&value.metadata.asset&&value.frames&&value.channels==2);if(!value.metadata.asset)return;auto* row=new juce::DynamicObject;row->setProperty("pad",int(pad+1));row->setProperty("assetId",juce::String(assetIdText(*value.metadata.asset)));row->setProperty("name",juce::String(value.metadata.name));row->setProperty("frames",int(value.frames));row->setProperty("channels",int(value.channels));juce::Array<juce::var> numeric;for(unsigned field=0;field<9;++field)numeric.add(p.control(drumParameterStart+pad*9+field).getValue());row->setProperty("numericNormalized",numeric);if(value.metadata.source){const auto& source=*value.metadata.source;row->setProperty("sourceKind",source.kind==SourceKind::generated?"generated":"imported");row->setProperty("sourceFileId",juce::String(assetIdText(source.sourceFileId)));row->setProperty("sourceFrames",juce::int64(source.sourceFrames));row->setProperty("begin",juce::int64(source.begin));row->setProperty("end",juce::int64(source.end));row->setProperty("gain",source.gain);row->setProperty("seed",juce::int64(source.seed));}pads.add(juce::var(row));if(pad==0)CHECK(value.metadata.source&&value.metadata.source->kind==SourceKind::imported&&value.metadata.source->begin==100&&value.metadata.source->end==4700&&value.metadata.source->gain==.5f);if(pad==15)CHECK(value.metadata.source&&value.metadata.source->kind==SourceKind::generated&&value.metadata.source->seed==expectedSeed);}
 CHECK(original.isAChildOf(output)&&!original.isSymbolicLink()&&original.deleteFile());const auto preset=output.getChildFile("product-kit.airack");CHECK(p.saveSoundPreset(preset));juce::MemoryBlock state;p.getStateInformation(state);CHECK(state.getSize()>8&&std::memcmp(state.getData(),"ARBNDL01",8)==0);CHECK(output.getChildFile("product-kit-kept-project.arbndl").replaceWithData(state.getData(),state.getSize()));auto fresh=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(fresh->loadSoundPreset(preset));juce::MemoryBlock fromPreset;fresh->getStateInformation(fromPreset);CHECK(fromPreset.getSize()>8&&std::memcmp(fromPreset.getData(),"ARBNDL01",8)==0);CHECK(output.getChildFile("product-kit-preset-derived-project.arbndl").replaceWithData(fromPreset.getData(),fromPreset.getSize()));ProjectDocument snapshot;std::uint64_t edit=0;juce::MemoryBlock compact;CHECK(p.tryProjectSnapshot(snapshot,edit)&&encodeProjectState(snapshot,compact));CHECK(output.getChildFile("product-kit-expected-project.bin").replaceWithData(compact.getData(),compact.getSize()));auto* manifest=new juce::DynamicObject;manifest->setProperty("scope","Real generated PAD16 Keep and imported PAD1 Keep, fourteen synthetic canonical owned pads; full16 playable kit; original removed after actual worker drain; no human listening acceptance");manifest->setProperty("pads",pads);manifest->setProperty("rack",rack.toDashedString());manifest->setProperty("keptProjectSHA256",juce::SHA256(state).toHexString());manifest->setProperty("presetProjectSHA256",juce::SHA256(fromPreset).toHexString());CHECK(output.getChildFile("product-kit-expected.json").replaceWithText(juce::JSON::toString(juce::var(manifest),false)));
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool inAudioCallback=false;std::atomic_bool watchDspWorkers{false};std::atomic_uint callbackAllocations{0},callbackFrees{0};
int __cdecl continuityHeap(int kind,void*,std::size_t,int,long,const unsigned char*,int){if(inAudioCallback||(watchDspWorkers&&airack::detail::DspPipeline::renderingOnThisThread())){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++callbackAllocations;if(kind==_HOOK_FREE)++callbackFrees;}return 1;}
#endif
struct PatternHead final:juce::AudioPlayHead {std::int64_t frame=0;juce::Optional<PositionInfo> getPosition()const override{PositionInfo p;p.setIsPlaying(true);p.setBpm(120);p.setPpqPosition(double(frame)/24000);p.setTimeInSamples(frame);return p;}};
void audio_continuity_during_actual_generation(){
 std::cout<<"GROUP MIDI_pattern_exact_pad_controls_continue_during_HTTP_source_prepare_cancel"<<std::endl;
 for(bool higher:{false,true}){Fixture fixture;fixture.holdHttp=true;fixture.holdSource=true;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());std::array<juce::Uuid,4> racks;for(unsigned bank=0;bank<4;++bank)racks[bank]=place(*p,false,bank);if(racks[0].isNull())return;std::vector<float> pcm(48000,.001f);for(auto rack:racks)assignContinuityPad(*p,rack,0,pcm);
  for(unsigned slot=0;slot<2;++slot){ProjectDocument project;std::uint64_t edit=0;CHECK(p->tryProjectSnapshot(project,edit));CHECK(p->applyRackEdit({RackEditKind::addEffect,std::uint8_t(slot),masterChain,std::uint8_t(slot),EffectKind::dynamicEq},edit)==RackEditResult::applied);}
  ProjectDocument document;std::uint64_t edit=0;CHECK(p->tryProjectSnapshot(document,edit));document.pattern.noteCount=16;for(unsigned note=0;note<16;++note)document.pattern.notes[note]={double(note)/8,.01,36,100,1};CHECK(p->applyPatternEdit(document.pattern,edit));p->control(758).setValue(higher?1.f:0.f);p->setNonRealtime(higher);p->prepareToPlay(48000,512);CHECK(p->effectiveRenderQuality()==(higher?RenderQuality::higher:RenderQuality::realtime));PatternHead head;p->setPlayHead(&head);juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;midi.ensureSize(4096);p->processBlock(audio,midi);const auto workerBefore=p->dspWorkerStats().blocks;fixture.bind(*p);const auto captured=target(*p,racks[0]);DrumCommitError error{};CHECK(p->beginDrumGeneration(captured,"render while real source worker runs",{17,73,211},error));CHECK(fixture.httpGate.reached());
#if defined(_MSC_VER) && defined(_DEBUG)
  const auto prior=_CrtSetAllocHook(continuityHeap);callbackAllocations=0;callbackFrees=0;
#endif
  unsigned callbacks=0,events=0,patternEvents=0;double energy=0;const auto render=[&](const char* stage){for(unsigned n=0;n<24;++n){if(n==0||n==12){ProjectDocument current;std::uint64_t revision=0;CHECK(p->tryProjectSnapshot(current,revision));CHECK(p->setPlaybackMode(n==0?PlaybackMode::externalMidi:PlaybackMode::internalPattern,revision));}midi.clear();if(n%2==0){midi.addEvent(juce::MidiMessage::noteOn(1,36,juce::uint8(100)),0);++events;}if(n%4==0)CHECK(p->enqueueDrumPad(captured,100));const auto trim=drumParameterStart+3;CHECK(p->beginParameterGesture(trim));CHECK(p->setParameterGestureValue(trim,n%2?.4f:.7f));CHECK(p->endParameterGesture(trim));
#if defined(_MSC_VER) && defined(_DEBUG)
   inAudioCallback=true;watchDspWorkers=true;
#endif
   p->processBlock(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
   inAudioCallback=false;watchDspWorkers=false;
#endif
   ++callbacks;if(n>=12){CHECK(p->patternPlaybackStatus()==PatternPlaybackStatus::playing);patternEvents+=unsigned(InstrumentDrumTestAccess::patternEvents(*p));}head.frame+=512;for(int channel=0;channel<2;++channel)for(int sample=0;sample<512;++sample){const auto x=audio.getSample(channel,sample);if(!std::isfinite(x)){CHECK(false);return;}energy+=std::abs(x);}}
   std::cout<<"GENERATION_CONTINUITY stage="<<stage<<" quality="<<higher<<" callbacks="<<callbacks<<" energy="<<energy<<std::endl;};
  render("headers-held");fixture.httpGate.release();CHECK(fixture.sourceGate.reached());render("three-raw-before-inspection");fixture.sourceGate.release();CHECK(ready(*p));fixture.holdSource=false;fixture.sourceGate.reset();fixture.holdPrepared=true;p->drumImport().edit(0,441000,.125f,"Continuity generated");p->drumImport().prepare();CHECK(fixture.sourceGate.reached());render("canonical-helper-held");p->cancelDrumGeneration(p->drumImport().view().session);render("cancel-draining");fixture.sourceGate.release();finish(*p);
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetAllocHook(prior);CHECK(callbackAllocations==0&&callbackFrees==0);std::cout<<"GENERATION_RT_HEAP quality="<<higher<<" allocations="<<callbackAllocations<<" frees="<<callbackFrees<<std::endl;
#endif
  CHECK(callbacks==96&&events==48&&patternEvents>0&&energy>0&&p->dspWorkerStats().blocks>workerBefore);CHECK(fixture.posts()==3&&fixture.foreign()==0);p->setPlayHead(nullptr);
 }
}
void composed_generation_memory(){
 std::cout<<"GROUP process_wide_requested_heap_actual_raw_source_helper_C_player_overlap"<<std::endl;
#if defined(_MSC_VER) && defined(_DEBUG)
 generationHeap::calibrate();
 {Fixture fixture;fixture.flac.resize(16u*1024*1024,'\0');fixture.holdSource=true;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;const auto captured=target(*p,rack);fixture.bind(*p);DrumCommitError error{};std::uint64_t actualFiles=0;
  const auto heap=generationHeap::observe([&]{CHECK(p->beginDrumGeneration(captured,"three maximum encoded owners",{17,73,211},error));CHECK(fixture.sourceGate.reached());const auto held=InstrumentDrumTestAccess::counts(*p);CHECK(held.slots==3&&held.encoded==48ull*1024*1024&&held.workspace>=40ull*1024*1024+393224);for(const auto& file:fixture.root.findChildFiles(juce::File::findFiles,true))actualFiles+=std::uint64_t(file.getSize());CHECK(actualFiles==48ull*1024*1024);p->cancelDrumGeneration(p->drumImport().view().session);const auto canceled=InstrumentDrumTestAccess::counts(*p);CHECK(canceled.encoded==held.encoded&&canceled.workspace==held.workspace&&canceled.slots==3);fixture.sourceGate.release();finish(*p);if(fixture.server)fixture.server->join();});
  CHECK(fixture.root.findChildFiles(juce::File::findFiles,true).isEmpty());std::cout<<"GENERATION_MAX_RAW logical_encoded=50331648 actual_file_bytes="<<actualFiles<<" requested_heap_peak_including_fixture_responses="<<generationHeap::peak(heap)<<" requested_heap_live="<<heap.live<<" prototype_separate=393224 RSS_measured=0"<<std::endl;
 }
 {Fixture first,second;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;auto captured=target(*p,rack);CHECK(prepareGenerated(first,*p,captured));CHECK(p->drumImport().keep());CHECK(wait([&]{return !p->drumImport().busy();}));captured=target(*p,rack);second.bind(*p);second.holdPrepared=true;DrumCommitError error{};
  const auto heap=generationHeap::observe([&]{CHECK(p->beginDrumGeneration(captured,"old C plus helper plus new C plus player",{17,73,211},error));CHECK(ready(*p));p->drumImport().edit(0,441000,.125f,"New retained draft");p->drumImport().prepare();CHECK(second.sourceGate.reached());const auto held=InstrumentDrumTestAccess::counts(*p);CHECK(held.slots==3&&held.decoded>=3840000&&held.reserved==2ull*3840000&&held.encoded==48ull*1024*1024);second.sourceGate.release();CHECK(wait([&]{return p->drumImport().view().canKeep;}));p->drumImport().audition();CHECK(wait([&]{return p->drumImport().view().phase==DrumImportPhase::auditioning;}));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;p->processBlock(audio,midi);CHECK(InstrumentDrumTestAccess::counts(*p).preview==3840000);finish(*p);CHECK(InstrumentDrumTestAccess::counts(*p).preview==3840000);p->processBlock(audio,midi);CHECK(wait([&]{return InstrumentDrumTestAccess::counts(*p).preview==0;}));if(second.server)second.server->join();});
  std::cout<<"GENERATION_COMPOSED_HEAP requested_peak_including_fixture="<<generationHeap::peak(heap)<<" requested_live="<<heap.live<<" actual_old_C_bytes=3840000 helper_D_bytes=3840000 reserved_2D_bytes=7680000 player_P_bytes=3840000 prototype_separate=393224 RSS_measured=0 global_workspace_quota_added=0"<<std::endl;
 }
 {Fixture fa,fb;auto a=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()),b=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto ra=place(*a),rb=place(*b);if(ra.isNull()||rb.isNull())return;fa.bind(*a);fb.bind(*b);const auto ta=target(*a,ra),tb=target(*b,rb);DrumCommitError error{};const auto heap=generationHeap::observe([&]{CHECK(a->beginDrumGeneration(ta,"measured A",{17,73,211},error));CHECK(b->beginDrumGeneration(tb,"measured B",{17,73,211},error));CHECK(ready(*a)&&ready(*b));CHECK(InstrumentDrumTestAccess::counts(*a).encoded+InstrumentDrumTestAccess::counts(*b).encoded==96ull*1024*1024);finish(*a);CHECK(InstrumentDrumTestAccess::counts(*b).slots==3);finish(*b);if(fa.server)fa.server->join();if(fb.server)fb.server->join();});std::cout<<"GENERATION_TWO_INSTANCE_HEAP requested_peak_including_fixtures="<<generationHeap::peak(heap)<<" encoded_logical=100663296 unrelated_account_quota=0 RSS_measured=0"<<std::endl;}
#else
 CHECK(false&&"Actual Debug CRT memory observer required");
#endif
}
bool ownedServiceAlive(const juce::var& ticket){
 const auto pid=juce::int64(ticket["pid"]);if(pid<1||pid>UINT32_MAX||int(ticket["port"])!=8189)return false;const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(pid));if(!process)return false;FILETIME created{},exit{},kernel{},user{};const bool times=GetProcessTimes(process,&created,&exit,&kernel,&user)!=0;std::array<wchar_t,32768> image{};DWORD length=DWORD(image.size());const bool named=QueryFullProcessImageNameW(process,0,image.data(),&length)!=0;DWORD code=0;const bool running=GetExitCodeProcess(process,&code)&&code==STILL_ACTIVE;CloseHandle(process);const auto stamp=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;return times&&named&&running&&stamp==std::uint64_t(ticket["creationFileTime"].toString().getLargeIntValue())&&juce::String(image.data(),int(length)).equalsIgnoreCase(ticket["image"].toString());
}
bool ownedCondition(juce::var& ticket,juce::var& frozen){
 if(serviceCondition==juce::File{}||!serviceCondition.existsAsFile()||serviceCondition.getSize()>65536)return false;ticket=juce::JSON::parse(serviceCondition);if(!ticket.isObject()||!ownedServiceAlive(ticket))return false;const auto source=juce::File(ticket["frozenCondition"].toString());if(!source.existsAsFile()||source.getSize()>1048576)return false;juce::MemoryBlock bytes;if(!source.loadFileAsData(bytes)||juce::SHA256(bytes).toHexString()!="6af68ec8f66a204f88003c3ae75b9d6c6d8236bebfd39faf244ec7e644f5651c")return false;frozen=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),int(bytes.getSize())));const auto* matrix=frozen["matrix"].getArray();return frozen["condition"].toString()=="stable-audio3-approved-composite-clean-timing-20261022-v1"&&matrix&&matrix->size()==12;
}
void owned_warm(){
 std::cout<<"GROUP explicit_owned_service_native_product_job_adapter_inspection_warm"<<std::endl;juce::var ticket,frozen;const bool condition=ownedCondition(ticket,frozen);CHECK(condition);if(!condition)return;const auto* rows=frozen["matrix"].getArray();juce::Array<juce::var> records;
 for(unsigned turn=0;turn<5;++turn){const unsigned group=turn?turn-1:0;const auto& row=(*rows)[int(group*3)];const auto prompt=row["prompt"].toString();const auto name=row["class"].toString();const std::array<std::uint64_t,3> seeds=turn?std::array<std::uint64_t,3>{17,73,211}:std::array<std::uint64_t,3>{1009,1013,1019};CHECK(ownedServiceAlive(ticket));for(unsigned ordinal=0;ordinal<3;++ordinal){const auto& approved=(*rows)[int(group*3+ordinal)];CHECK((approved["class"].toString()==name&&approved["prompt"].toString()==prompt&&juce::int64(approved["seed"])==std::array<int,3>{17,73,211}[ordinal]));}
  auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p,true);if(rack.isNull())return;const auto captured=target(*p,rack);DrumCommitError error{};const auto began=Clock::now();const bool admitted=p->beginDrumGeneration(captured,prompt,seeds,error);CHECK(admitted);if(!admitted)return;const auto deadline=began+std::chrono::seconds(310);bool complete=false;while(Clock::now()<deadline){const auto view=p->drumImport().view();if(view.phase!=DrumImportPhase::generating){complete=view.inspected&&view.generationStatus==DrumGenerationStatus::complete;break;}juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}const auto seconds=std::chrono::duration<double>(Clock::now()-began).count();const auto view=p->drumImport().view();CHECK(complete);if(turn)CHECK(seconds<=30);CHECK(ownedServiceAlive(ticket));auto* result=new juce::DynamicObject;result->setProperty("role",turn?"timed":"cold-warmup-excluded");result->setProperty("class",name);result->setProperty("seconds_admitted_to_three_inspected",seconds);result->setProperty("complete",complete);result->setProperty("status",int(view.generationStatus));juce::Array<juce::var> sources;
  for(unsigned ordinal=0;ordinal<3;++ordinal){const auto& candidate=view.candidates[ordinal];CHECK(candidate.available&&candidate.seed==seeds[ordinal]&&candidate.source.rate==44100&&candidate.source.channels==2&&candidate.source.bits==16&&candidate.source.frames==442368&&candidate.source.md5Verified);auto* copy=new juce::DynamicObject;copy->setProperty("seed",juce::int64(candidate.seed));copy->setProperty("fileId",juce::String(assetIdText(candidate.fileId)));copy->setProperty("decodedPcmId",juce::String(assetIdText(candidate.source.decodedPcmId)));copy->setProperty("encodedBytes",juce::int64(candidate.encodedBytes));sources.add(juce::var(copy));}result->setProperty("sources",sources);records.add(juce::var(result));CHECK(output.getChildFile("native-warm-results.json").replaceWithText(juce::JSON::toString(records,false)));std::cout<<"NATIVE_PRODUCT_WARM class="<<name<<" role="<<(turn?"timed":"excluded-cold")<<" seconds="<<seconds<<" complete="<<complete<<" actual_transport_drain_hash_decode_inspection_control_completion=1"<<std::endl;
  if(!complete){finish(*p);return;}finish(*p);
 }
 CHECK(output.getChildFile("warm-scope.txt").replaceWithText("Explicit owned8189 condition only; timed native processor admission -> real fixed job/transport/drain/download/hash/strict qualified FLAC inspection -> three control-visible candidates. Four classes, seeds17/73/211. Cold three-candidate warmup seeds1009/1013/1019 excluded. Original approved prompt/config hashes fixed; product UI execution identity remains unverified. GPU cleanliness is independently observed by owned external sampler. No new quality/listening approval or universal performance claim.\n"));
}
void owned_service_desktop(){
 std::cout<<"GROUP explicit_owned_real_service_editor_visible_prompt_crop_gain_audition_Keep"<<std::endl;juce::var ticket,frozen;CHECK(ownedCondition(ticket,frozen));if(!ownedServiceAlive(ticket))return;auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p,true);if(rack.isNull())return;std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());CHECK(editor);if(!editor)return;editor->setSize(1080,680);editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);editor->setVisible(true);studio::MakerFace* maker=nullptr;CHECK(wait([&]{maker=componentOf<studio::MakerFace>(*editor);return maker&&maker->tileArray();}));if(!maker)return;CHECK(maker->tileArray()->selectPad(15));maker->setOpen(true,false);auto* generate=maker->revealed().button("generate_cancel");CHECK(generate&&generate->isEnabled());if(!generate)return;CHECK(accessiblePress(*generate));studio::DrumSourcePopup* popup=nullptr;CHECK(wait([&]{popup=componentOf<studio::DrumSourcePopup>(*editor);return popup&&popup->isVisible();}));if(!popup)return;CHECK(p->drumImport().view().target.pad==15);CHECK(maker->tileArray()->selectPad(0));const auto* matrix=frozen["matrix"].getArray();if(!matrix)return;CHECK(postedText(*editor,popup->prompt,(*matrix)[0]["prompt"].toString()));capture(*editor,"real-service-prompt1080");editor->setSize(1600,1000);juce::MessageManager::getInstance()->runDispatchLoopUntil(50);capture(*editor,"real-service-prompt1600");CHECK(accessiblePress(popup->generate));CHECK(wait([&]{return p->drumImport().view().phase==DrumImportPhase::generating;}));popup->refresh();capture(*popup,"real-service-generating-popup");const auto deadline=Clock::now()+std::chrono::seconds(310);while(Clock::now()<deadline&&p->drumImport().view().phase==DrumImportPhase::generating)juce::MessageManager::getInstance()->runDispatchLoopUntil(20);const auto inspected=p->drumImport().view();CHECK(inspected.inspected&&inspected.generationStatus==DrumGenerationStatus::complete);if(!inspected.inspected){editor.reset();finish(*p);return;}popup->refresh();CHECK(inspected.target.pad==15&&inspected.end==442368&&!popup->prepare.isEnabled()&&!popup->keep.isEnabled());for(unsigned n=0;n<3;++n)CHECK(popup->candidates[n].isEnabled()&&popup->candidates[n].getName()=="PAD 16 candidate "+juce::String(n+1));capture(*popup,"real-service-full-source-invalid-popup");CHECK(accessiblePress(popup->candidates[1]));CHECK(wait([&]{return p->drumImport().view().inspected&&p->drumImport().view().selectedCandidate==1;}));const auto seed=p->drumImport().view().candidates[1].seed;CHECK(postedText(*editor,popup->start,"0"));CHECK(postedText(*editor,popup->end,"441000"));CHECK(postedText(*editor,popup->gain,"0.125"));CHECK(postedText(*editor,popup->keptName,"Real local generated hit"));CHECK(popup->prepare.isEnabled());CHECK(accessiblePress(popup->prepare));CHECK(wait([&]{return p->drumImport().view().canKeep;}));popup->refresh();CHECK(p->drumImport().view().preparedFrames==480000&&label(*popup,"drum.source.selection").contains("peak"));capture(*popup,"real-service-canonical-ready-popup");CHECK(accessiblePress(popup->audition));CHECK(wait([&]{return p->drumImport().view().phase==DrumImportPhase::auditioning;}));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;double energy=0;for(unsigned block=0;block<32;++block){p->processBlock(audio,midi);energy+=audio.getMagnitude(0,512);}CHECK(energy>0);popup->refresh();capture(*popup,"real-service-canonical-audition-popup");CHECK(accessiblePress(popup->keep));CHECK(wait([&]{p->processBlock(audio,midi);return !popup->isVisible()&&!p->drumImport().busy()&&InstrumentDrumTestAccess::counts(*p).slots==0&&InstrumentDrumTestAccess::counts(*p).preview==0;}));DrumPadTarget kept;DrumPadReadback value;DrumCommitError error{};CHECK(p->captureDrumPerformanceTarget(rack,0,15,kept,error)&&p->readDrumPad(kept,value,error));CHECK(value.metadata.asset&&value.frames==480000&&value.metadata.source&&value.metadata.source->kind==SourceKind::generated&&value.metadata.source->seed==seed&&value.metadata.source->begin==0&&value.metadata.source->end==441000&&value.metadata.source->gain==.125f);CHECK(maker->tileArray()->selectPad(15));juce::MessageManager::getInstance()->runDispatchLoopUntil(50);capture(*editor,"real-service-kept1600");editor->setSize(1080,680);juce::MessageManager::getInstance()->runDispatchLoopUntil(50);capture(*editor,"real-service-kept1080");CHECK(p->undoSoundEdit());CHECK(p->redoSoundEdit());editor.reset();exportFullKit(*p,rack,seed);finish(*p);CHECK(ownedServiceAlive(ticket));CHECK(output.getChildFile("real-service-ui-scope.txt").replaceWithText("Actual native editor, production fixed8189 job/transport/helper/C/player and real visible posted prompt/crop/gain fields. UI random distinct seeds recorded in saved generated provenance; separately qualified warm uses17/73/211. Full16 kit export includes actual real-service generatedPAD16, importedPAD1 and14synthetic owned pads. Component images, not Standalone or human listening approval.\n"));
}

void held_generation_completion_preserves_controller_deadline(){
 std::cout<<"GROUP held_real_complete_partial_and_terminal_error_at_controller_deadline"<<std::endl;
 for(const int failedOrdinal:{-1,1,0})for(const bool expired:{false,true}){
  Fixture fixture;fixture.failedOrdinal=failedOrdinal;fixture.holdCompletion=true;
  auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;
  const auto captured=target(*p,rack);fixture.bind(*p);DrumCommitError error{};
  auto before=std::make_unique<SoundDocument>();std::uint64_t revision=0;CHECK(p->trySoundSnapshot(*before,revision));const auto history=InstrumentDrumTestAccess::history(*p);
  const bool begun=p->beginDrumGeneration(captured,"held completion deadline witness",{17,73,211},error);CHECK(begun);if(!begun)return;
  const bool held=fixture.completionGate.reached();CHECK(held);if(!held){fixture.holdCompletion=false;fixture.completionGate.release();finish(*p);return;}
  const bool success=failedOrdinal!=0;const unsigned available=failedOrdinal<0?3:failedOrdinal==1?1:0;
  const auto expected=failedOrdinal<0?DrumGenerationStatus::complete:failedOrdinal==1?DrumGenerationStatus::partial:DrumGenerationStatus::remoteFailed;
  const auto retained=InstrumentDrumTestAccess::counts(*p);CHECK(retained.slots==(success?3u:0u)&&retained.encoded==(success?48ull*1024*1024:0));
  CHECK(!InstrumentDrumTestAccess::generationCompletion(*p).ready);
  if(expired)InstrumentDrumTestAccess::expireGeneration(*p);
  fixture.holdCompletion=false;fixture.completionGate.release();
  // Pumping the Timer here would cancel before the held completion reaches public poll.
  const auto end=Clock::now()+std::chrono::seconds(20);auto completion=InstrumentDrumTestAccess::generationCompletion(*p);
  while(!completion.ready&&Clock::now()<end){std::this_thread::sleep_for(std::chrono::milliseconds(2));completion=InstrumentDrumTestAccess::generationCompletion(*p);}
  CHECK(completion.ready&&completion.success==success&&completion.status==expected&&completion.available==available);
  if(!completion.ready){finish(*p);return;}
  CHECK(p->drumImport().view().phase==DrumImportPhase::generating&&!p->drumImport().view().inspected);
  p->drumImport().poll();const auto shown=p->drumImport().view();
  std::cout<<"HELD_GENERATION_COMPLETION expired="<<expired<<" success="<<completion.success<<" candidates="<<completion.available<<" generated_status="<<int(completion.status)<<" admitted_status="<<int(shown.generationStatus)<<" phase="<<int(shown.phase)<<std::endl;
  if(expired&&success){
   CHECK(shown.generationStatus==DrumGenerationStatus::timeout);
   CHECK(!shown.inspected&&!shown.canPrepare&&!shown.canAudition&&!shown.canKeep&&std::none_of(shown.candidates.begin(),shown.candidates.end(),[](const auto& candidate){return candidate.available;}));
   CHECK(!p->drumImport().keep());
   if(shown.generationStatus==DrumGenerationStatus::timeout){
    CHECK(wait([&]{const auto counts=InstrumentDrumTestAccess::counts(*p);return counts.slots==0&&counts.encoded==0&&counts.workspace==393224&&!p->drumImport().busy();}));
    const auto drained=p->drumImport().view();CHECK(drained.phase==DrumImportPhase::closed&&drained.generationStatus==DrumGenerationStatus::timeout&&!drained.message.containsIgnoreCase("drain"));
   }
  }else if(success){
   CHECK(shown.phase==DrumImportPhase::editing&&shown.inspected&&shown.generationStatus==expected);
   CHECK(InstrumentDrumTestAccess::generationDeadlineCleared(*p));
   p->drumImport().edit(100,441000,.5f,"On-time candidate remains editable");p->drumImport().poll();const auto edited=p->drumImport().view();
   CHECK(edited.inspected&&edited.phase==DrumImportPhase::editing&&edited.generationStatus==expected&&edited.canPrepare);
  }else{
   CHECK(shown.phase==DrumImportPhase::error&&shown.generationStatus==expected&&!shown.inspected&&!shown.canPrepare&&!shown.canAudition&&!shown.canKeep);
   CHECK(InstrumentDrumTestAccess::generationDeadlineCleared(*p));
  }
  auto after=std::make_unique<SoundDocument>();std::uint64_t afterRevision=0;CHECK(p->trySoundSnapshot(*after,afterRevision));CHECK(*after==*before&&afterRevision==revision&&InstrumentDrumTestAccess::history(*p)==history);
  CHECK(fixture.posts()==(failedOrdinal<0?3u:unsigned(failedOrdinal+1))&&fixture.foreign()==0);
  finish(*p);const auto drained=InstrumentDrumTestAccess::counts(*p);CHECK(drained.slots==0&&drained.encoded==0&&drained.workspace==393224);
 }
}
void timeout_status_after_actual_drain(){
 std::cout<<"GROUP timeout_copy_status_after_actual_worker_and_HTTP_drain"<<std::endl;
 Fixture fixture;fixture.holdHttp=true;fixture.holdAfterDrain=true;
 auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());const auto rack=place(*p);if(rack.isNull())return;
 fixture.bind(*p);DrumCommitError error{};
 CHECK(p->beginDrumGeneration(target(*p,rack),"timeout status witness",{17,73,211},error));
 CHECK(fixture.httpGate.reached());
 InstrumentDrumTestAccess::expireGeneration(*p);p->drumImport().poll();
 CHECK(wait([&]{return fixture.sourceGate.arrived();}));
 CHECK(InstrumentDrumTestAccess::counts(*p).slots==3&&InstrumentDrumTestAccess::counts(*p).encoded==48ull*1024*1024);
 CHECK(p->drumImport().view().generationStatus==DrumGenerationStatus::timeout);
 const auto canceled=p->drumImport().view();
 CHECK(std::all_of(canceled.candidates.begin(),canceled.candidates.end(),[](const auto& c){return c.status==ComfyAttemptStatus::cancelled&&!c.available;}));
 fixture.httpGate.release();fixture.sourceGate.release();
 CHECK(wait([&]{return !p->drumImport().busy()&&InstrumentDrumTestAccess::counts(*p).slots==0;}));
 const auto drained=p->drumImport().view();
 CHECK(drained.phase==DrumImportPhase::closed&&drained.generationStatus==DrumGenerationStatus::timeout&&!drained.message.containsIgnoreCase("drain")&&drained.message.containsIgnoreCase("may finish"));
 CHECK(fixture.posts()==1&&fixture.foreign()==0);
 finish(*p);
}
void owned_service_instances(){
 std::cout<<"GROUP explicit_owned_service_two_processor_contention_separate_from_clean_warm"<<std::endl;
 juce::var ticket,frozen;
 const bool condition=ownedCondition(ticket,frozen);CHECK(condition);if(!condition)return;
 auto a=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime()),b=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());
 const auto ra=place(*a),rb=place(*b);if(ra.isNull()||rb.isNull())return;
 const auto ta=target(*a,ra),tb=target(*b,rb);
 const auto* matrix=frozen["matrix"].getArray();
 DrumCommitError error{};
 CHECK(a->beginDrumGeneration(ta,(*matrix)[0]["prompt"].toString(),{17,73,211},error));
 CHECK(b->beginDrumGeneration(tb,(*matrix)[3]["prompt"].toString(),{17,73,211},error));
 CHECK(InstrumentDrumTestAccess::counts(*a).slots==3&&InstrumentDrumTestAccess::counts(*b).slots==3);
 const auto began=Clock::now();
 juce::MessageManager::getInstance()->runDispatchLoopUntil(500);
 a->cancelDrumGeneration(a->drumImport().view().session);
 CHECK(wait([&]{return !a->drumImport().busy()&&InstrumentDrumTestAccess::counts(*a).slots==0;}));
 const auto deadline=began+std::chrono::seconds(310);
 while(Clock::now()<deadline&&b->drumImport().view().phase==DrumImportPhase::generating)
  juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
 const auto view=b->drumImport().view();
 CHECK(view.inspected&&view.generationStatus==DrumGenerationStatus::complete);
 for(const auto& candidate:view.candidates)CHECK(candidate.available&&candidate.source.md5Verified&&candidate.source.frames==442368);
 CHECK(InstrumentDrumTestAccess::counts(*b).slots==3&&!b->drumImport().open(tb));
 auto* record=new juce::DynamicObject;
 record->setProperty("scope","Actual fixed8189 two-processor contention; A cancel/drain, B three inspected sources. Separate functional condition, no <=30s warm claim.");
 record->setProperty("seconds",std::chrono::duration<double>(Clock::now()-began).count());
 record->setProperty("bComplete",view.inspected&&view.generationStatus==DrumGenerationStatus::complete);
 CHECK(output.getChildFile("real-service-instances.json").replaceWithText(juce::JSON::toString(juce::var(record),false)));
 finish(*a);finish(*b);CHECK(ownedServiceAlive(ticket));
}
void desktop_generation(){
 std::cout<<"GROUP actual_editor_accessible_generate_posted_prompt_crop_gain_keep"<<std::endl;
 Fixture fixture;auto runtime=airack::testing::isolatedRackRuntime();auto p=std::make_unique<InstrumentProcessor>(runtime);const auto rack=place(*p,true);if(rack.isNull())return;const auto skin=airack::testing::prepareTileSkin(runtime,*p,rack);CHECK(skin);if(!skin)return;fixture.bind(*p);
 std::unique_ptr<juce::AudioProcessorEditor> editor(p->createEditor());CHECK(editor);if(!editor)return;editor->setSize(1080,680);editor->addToDesktop(juce::ComponentPeer::windowIsTemporary);editor->setVisible(true);
 studio::MakerFace* maker=nullptr;CHECK(wait([&]{maker=componentOf<studio::MakerFace>(*editor);return maker!=nullptr;}));if(!maker)return;CHECK(maker->tileArray()&&maker->tileArray()->selectPad(15));maker->setOpen(true,false);
 auto* generate=maker->revealed().button("generate_cancel");CHECK(generate&&generate->isEnabled());if(!generate)return;CHECK(accessiblePress(*generate));
 studio::DrumSourcePopup* popup=nullptr;CHECK(wait([&]{popup=componentOf<studio::DrumSourcePopup>(*editor);return popup&&popup->isVisible();}));if(!popup)return;
 CHECK(p->drumImport().view().target.pad==15&&p->drumImport().view().phase==DrumImportPhase::choosing);CHECK(maker->tileArray()->selectPad(0));
 CHECK(postedText(*editor,popup->prompt,juce::String::repeatedString("x",2049),2048));CHECK(wait([&]{return !popup->generate.isEnabled()&&label(*popup,"drum.source.status").contains("2049 bytes");}));CHECK(!fixture.started&&InstrumentDrumTestAccess::counts(*p).slots==0);capture(*editor,"generation-2049-error1080");
 const auto prompt=juce::String::repeatedString(juce::String::fromUTF8("\xc3\xa9"),1024);CHECK(prompt.getNumBytesAsUTF8()==2048);CHECK(postedText(*editor,popup->prompt,prompt,1023));CHECK(wait([&]{return popup->generate.isEnabled();}));
 const auto hwnd=static_cast<HWND>(editor->getPeer()->getNativeHandle());CHECK(PostMessageW(hwnd,WM_KEYDOWN,VK_RETURN,1)&&PostMessageW(hwnd,WM_KEYUP,VK_RETURN,1));juce::MessageManager::getInstance()->runDispatchLoopUntil(40);CHECK(!fixture.started);
 fixture.holdHttp=true;CHECK(accessiblePress(popup->generate));CHECK(wait([&]{return fixture.httpGate.arrived();}));popup->refresh();CHECK(!popup->generate.isEnabled()&&!p->drumImport().open(p->drumImport().view().target));capture(*popup,"generation-busy-HTTP-held-popup");fixture.holdHttp=false;fixture.httpGate.release();CHECK(ready(*p));popup->refresh();CHECK(fixture.captured.prompt==prompt.toStdString()&&fixture.posts()==3&&fixture.foreign()==0);CHECK(p->drumImport().view().target.pad==15&&maker->tileArray()->selectedPad()==0);
 for(std::size_t n=0;n<3;++n)CHECK(popup->candidates[n].getName()=="PAD 16 candidate "+juce::String(int(n)+1)&&popup->candidates[n].isEnabled());CHECK(!popup->prepare.isEnabled()&&!popup->keep.isEnabled());capture(*popup,"generation-full-source-invalid-popup");
 CHECK(postedText(*editor,popup->start,"11"));CHECK(postedText(*editor,popup->end,"441000"));CHECK(postedText(*editor,popup->gain,"0.125"));
 CHECK(accessiblePress(popup->candidates[1]));CHECK(wait([&]{return p->drumImport().view().selectedCandidate==1&&p->drumImport().view().inspected;}));popup->refresh();CHECK(popup->start.getText()=="0"&&popup->end.getText()=="442368"&&popup->gain.getText().getFloatValue()==1);CHECK(popup->keptName.getText()=="Generated candidate 2");
 CHECK(postedText(*editor,popup->start,"0"));CHECK(postedText(*editor,popup->end,"441000"));CHECK(postedText(*editor,popup->gain,"0.125"));CHECK(postedText(*editor,popup->keptName,juce::String::fromUTF8("Generated caf\xc3\xa9")));CHECK(wait([&]{return popup->prepare.isEnabled();}));
 fixture.holdPrepared=true;fixture.sourceGate.reset();CHECK(accessiblePress(popup->prepare));CHECK(wait([&]{return fixture.sourceGate.arrived();}));popup->refresh();CHECK(p->drumImport().view().phase==DrumImportPhase::preparing&&!popup->keep.isEnabled());capture(*popup,"generation-preparing-helper-held-popup");fixture.holdPrepared=false;fixture.sourceGate.release();CHECK(wait([&]{return p->drumImport().view().canKeep;}));popup->refresh();CHECK(p->drumImport().view().preparedFrames==480000&&label(*popup,"drum.source.selection").contains("peak"));capture(*popup,"generation-canonical-ready-popup");
 CHECK(accessiblePress(popup->audition));CHECK(wait([&]{return p->drumImport().view().phase==DrumImportPhase::auditioning;}));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;p->processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);popup->refresh();capture(*popup,"generation-canonical-audition-popup");
 const auto chosenSeed=fixture.captured.seeds[1];CHECK(accessiblePress(popup->keep));CHECK(wait([&]{return !popup->isVisible()&&p->drumImport().view().phase==DrumImportPhase::kept;}));CHECK(wait([&]{p->processBlock(audio,midi);return InstrumentDrumTestAccess::counts(*p).slots==0&&InstrumentDrumTestAccess::counts(*p).preview==0;}));
 DrumPadTarget kept;DrumPadReadback pad;DrumCommitError error{};CHECK(p->captureDrumPerformanceTarget(rack,0,15,kept,error));CHECK(p->readDrumPad(kept,pad,error)&&pad.frames==480000&&pad.metadata.name==juce::String::fromUTF8("Generated caf\xc3\xa9").toStdString()&&pad.metadata.source&&pad.metadata.source->seed==chosenSeed&&pad.metadata.source->end==441000&&pad.metadata.source->gain==.125f);CHECK(maker->tileArray()->selectPad(15));juce::MessageManager::getInstance()->runDispatchLoopUntil(50);capture(*editor,"generation-kept1080");editor->setSize(1600,1000);juce::MessageManager::getInstance()->runDispatchLoopUntil(50);capture(*editor,"generation-kept1600");
 CHECK(p->undoSoundEdit());CHECK(p->captureDrumPerformanceTarget(rack,0,15,kept,error));CHECK(p->readDrumPad(kept,pad,error)&&!pad.metadata.asset);CHECK(p->redoSoundEdit());
 maker=componentOf<studio::MakerFace>(*editor);CHECK(maker);if(!maker)return;generate=maker->revealed().button("generate_cancel");CHECK(generate);if(!generate)return;
 fixture.failedOrdinal=1;CHECK(accessiblePress(*generate));CHECK(wait([&]{return popup->isVisible()&&p->drumImport().view().phase==DrumImportPhase::choosing;}));CHECK(postedText(*editor,popup->prompt,"isolated partial fixture"));CHECK(accessiblePress(popup->generate));CHECK(ready(*p));popup->refresh();CHECK(p->drumImport().view().generationStatus==DrumGenerationStatus::partial&&popup->candidates[0].isEnabled()&&!popup->candidates[1].isEnabled()&&!popup->candidates[2].isEnabled());capture(*popup,"generation-partial-unrun-popup");
 popup->end.grabKeyboardFocus();CHECK(wait([&]{return popup->end.hasKeyboardFocus(false);}));CHECK(PostMessageW(hwnd,WM_KEYDOWN,VK_ESCAPE,1)&&PostMessageW(hwnd,WM_KEYUP,VK_ESCAPE,1));CHECK(wait([&]{return !popup->isVisible()&&!p->drumImport().busy();}));CHECK(generate->hasKeyboardFocus(false));
 fixture.failedOrdinal=-1;fixture.holdHttp=true;fixture.holdAfterDrain=true;fixture.httpGate.reset();fixture.sourceGate.reset();CHECK(accessiblePress(*generate));CHECK(wait([&]{return popup->isVisible()&&p->drumImport().view().phase==DrumImportPhase::choosing;}));CHECK(postedText(*editor,popup->prompt,"captured pad becomes stale"));CHECK(accessiblePress(popup->generate));CHECK(wait([&]{return fixture.httpGate.arrived();}));SoundDocument locked;std::uint64_t revision=0;CHECK(p->trySoundSnapshot(locked,revision));locked.controlLocks[drumParameterStart+15*drumParametersPerPad]=true;CHECK(p->applySoundEdit(locked,SoundEditOrigin::manual,revision));DrumPadTarget denied;CHECK(!p->captureDrumPadTarget(rack,0,15,denied,error)&&error==DrumCommitError::locked);CHECK(wait([&]{const auto v=p->drumImport().view();return v.phase==DrumImportPhase::cancelling||v.phase==DrumImportPhase::closed||v.generationStatus==DrumGenerationStatus::stale;}));CHECK(wait([&]{return fixture.sourceGate.arrived();}));CHECK(InstrumentDrumTestAccess::counts(*p).slots==3&&InstrumentDrumTestAccess::counts(*p).encoded==48ull*1024*1024);popup->refresh();CHECK(!popup->keep.isEnabled()&&!p->drumImport().view().canKeep);const auto canceledView=p->drumImport().view();CHECK(std::all_of(canceledView.candidates.begin(),canceledView.candidates.end(),[](const auto& c){return c.status==ComfyAttemptStatus::cancelled&&!c.available;}));capture(*editor,"generation-stale-captured-pad-editor");fixture.holdHttp=false;fixture.holdAfterDrain=false;fixture.httpGate.release();fixture.sourceGate.release();CHECK(wait([&]{return InstrumentDrumTestAccess::counts(*p).slots==0&&!p->drumImport().busy();}));popup->refresh();CHECK(p->drumImport().view().phase==DrumImportPhase::closed&&!label(*popup,"drum.source.status").containsIgnoreCase("drain"));capture(*popup,"generation-stale-after-real-drain-popup");popup->close();
 editor.reset();finish(*p);CHECK(fixture.foreign()==0);
 CHECK(output.getChildFile("desktop-scope.txt").replaceWithText("Actual native InstrumentEditor and source popup; accessible actions, real posted WM_CHAR/Return/Escape after public focus/selectAll. Fixed PAD16 capture survives selected PAD1. Explicit crop/gain, canonical processBlock audition, real one-pad Keep/Undo and partial/unrun states. Component snapshots, not Standalone/physical hover. No service8189, attestation or human listening claim.\n"));
}
}
int main(int argc,char** argv){
 if(argc==2&&std::string_view(argv[1])=="--output-path-contract"){outputPathContract();std::cout<<"OUTPUT_PATH_CONTRACT checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
 juce::ScopedJuceInitialiser_GUI init;WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return 2;
 std::string section;for(int n=1;n<argc;++n){if(std::string_view(argv[n])=="--section"&&n+1<argc)section=argv[++n];else if(std::string_view(argv[n])=="--owned-service-condition"&&n+1<argc)serviceCondition=juce::File(juce::String::fromUTF8(argv[++n]));else if(std::string_view(argv[n])=="--output"&&n+1<argc)output=juce::File(juce::String::fromUTF8(argv[++n]));else{WSACleanup();return 2;}}
 if(output==juce::File{}){const auto* root=std::getenv("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT");if(!root){WSACleanup();return 2;}output=evidenceOutput({},juce::File(juce::String::fromUTF8(root)));}CHECK(output.createDirectory().wasOk());
 if(section=="workflow"||section=="red"){workflow();partial_and_full_session_arbitration();committed_observer_failure();}else if(section=="desktop")desktop_generation();else if(section=="preview-ack")stopped_preview_remains_busy_until_ack();else if(section=="observer")committed_observer_failure();else if(section=="setup"){KernelCompatibility legacy{};CHECK(sizeof(legacy)==4*2+8*effectBankCount&&offsetof(KernelCompatibility,effects)==8);CHECK(validateKernelCompatibility(legacy));for(const auto& bank:legacy.effects)for(auto revision:bank)CHECK(revision==1);auto p=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(!place(*p).isNull());Fixture fixture;auto second=std::make_unique<InstrumentProcessor>(airack::testing::isolatedRackRuntime());CHECK(!place(*second).isNull());}else if(section=="deadline-status"){held_generation_completion_preserves_controller_deadline();timeout_status_after_actual_drain();}else if(section=="warm")owned_warm();else if(section=="service-desktop")owned_service_desktop();else if(section=="service-instances")owned_service_instances();else if(section=="memory")composed_generation_memory();else if(section=="continuity")audio_continuity_during_actual_generation();else if(section=="lifetime"){optional_owned_host_dispatch_controls();admission_arithmetic_preserves_all_free_slots();lifetime();stale_http_and_preparation_draft();stopped_preview_remains_busy_until_ack();actual_http_callback_drain_before_processor_destruction();held_generation_completion_preserves_controller_deadline();timeout_status_after_actual_drain();}else if(section=="instances")instances();else{std::cout<<"Requested section not implemented yet; no service call made"<<std::endl;WSACleanup();return 2;}
 std::cout<<"DrumGeneration checks="<<checks<<" failures="<<failures<<std::endl;WSACleanup();return failures?1:0;
}

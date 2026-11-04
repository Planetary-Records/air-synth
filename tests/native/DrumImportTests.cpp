#include "DrumSourcePopup.h"
#include "DrumImport.h"
#include "RackExamples.h"
#include <cmath>
#include <iostream>
#include <thread>
#include <bit>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
namespace airack {
struct InstrumentDrumTestAccess {
 struct Accounting {std::uint64_t retained=0,reserved=0,workspace=0,preview=0,player=0;unsigned inputs=0,running=0;std::array<std::uint64_t,3> tickets{},ids{},decoded{};};
 static Accounting accounting(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);Accounting a;const auto& state=*p.drumOwnership;a.retained=state.retainedBytes();a.reserved=state.reservedDecoded_;a.workspace=state.reservedWorkspace_;a.preview=p.manualPreviewBytes;a.player=p.renderedPreview.retainedPcmBytes();for(unsigned n=0;n<3;++n){const auto& slot=state.preparations_[n];a.inputs+=slot.inputId!=0;a.running+=slot.running;a.tickets[n]=slot.ticket;a.ids[n]=slot.inputId;a.decoded[n]=slot.decoded;}return a;}
 static void releaseInputPreparation(InstrumentProcessor& p,std::uint64_t id){p.releaseDrumInputPreparationWork(id);}
 static void releaseInput(InstrumentProcessor& p,std::uint64_t id){p.releaseDrumInputWork(id);}
 static std::uint64_t workerBlocks(InstrumentProcessor& p){return p.engine.dspWorkerStats().blocks;}
 static bool inputReleased(DrumImport& importer){const std::lock_guard lock(importer.mutex);return importer.inputId==0;}
#ifndef NDEBUG
 using ProcessorPoint=InstrumentProcessor::DrumTestPoint;
 static void hook(DrumImport& importer,void* context,void (*callback)(void*,DrumImport::TestPoint) noexcept){const std::lock_guard lock(importer.mutex);importer.testContext=context;importer.testHook=callback;}
 static bool teardownLocksAvailable(InstrumentProcessor& p,DrumImport& importer){
  const bool restore=p.restoreMutex.try_lock();if(restore)p.restoreMutex.unlock();
  const bool operation=p.drumRestoreOperationMutex.try_lock();if(operation)p.drumRestoreOperationMutex.unlock();
  const bool mailbox=importer.mutex.try_lock();if(mailbox)importer.mutex.unlock();return restore&&operation&&mailbox;
 }
 static bool callbacksStopped(InstrumentProcessor& p){return !p.isTimerRunning()&&!p.isUpdatePending()&&!p.drumControlPump;}
 static bool completionPending(DrumImport& importer){const std::lock_guard lock(importer.mutex);return importer.completion.has_value();}
 static void processorHook(InstrumentProcessor& p,void* context,void (*callback)(void*,InstrumentProcessor::DrumTestPoint)){
  const std::lock_guard lock(p.restoreMutex);p.drumTestContext=context;p.drumTestHook=callback;
 }
#endif
};
}
using namespace airack;
namespace {
int checks=0,failures=0;
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cout<<"FAIL "<<__LINE__<<" "<<#x<<std::endl;}}while(false)
juce::File evidence;
void stateEvidence(InstrumentProcessor& p,const juce::String& name){if(evidence==juce::File{})return;studio::DrumSourcePopup view(p);view.refresh();juce::FileOutputStream file(evidence.getChildFile(name+".png"));CHECK(file.openedOk());CHECK(juce::PNGImageFormat().writeImageToStream(view.createComponentSnapshot(view.getLocalBounds(),true,1.f),file));}
void appendAudio(std::vector<float>& all,const juce::AudioBuffer<float>& block){if(evidence==juce::File{})return;for(int n=0;n<block.getNumSamples();++n)for(int c=0;c<2;++c)all.push_back(block.getSample(c,n));}
void audioEvidence(const juce::String& name,const std::vector<float>& pcm){if(evidence==juce::File{})return;juce::MemoryOutputStream wav;wav.write("RIFF",4);wav.writeInt(int(36+pcm.size()*4));wav.write("WAVEfmt ",8);wav.writeInt(16);wav.writeShort(3);wav.writeShort(2);wav.writeInt(48000);wav.writeInt(48000*8);wav.writeShort(8);wav.writeShort(32);wav.write("data",4);wav.writeInt(int(pcm.size()*4));for(const auto v:pcm)wav.writeInt(std::bit_cast<std::int32_t>(v));CHECK(evidence.getChildFile(name+".wav").replaceWithData(wav.getData(),wav.getDataSize()));}
void pump(){juce::MessageManager::getInstance()->runDispatchLoopUntil(20);}
juce::String labelText(juce::Component& parent,const juce::String& id){for(auto* child:parent.getChildren())if(child->getComponentID()==id)if(auto* label=dynamic_cast<juce::Label*>(child))return label->getText();return {};}
template<class F>bool wait(F&& predicate){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(std::chrono::steady_clock::now()<end){if(predicate())return true;pump();}return predicate();}
template<class F>bool waitWithoutDispatch(F&& predicate){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(15);while(std::chrono::steady_clock::now()<end){if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}return predicate();}
bool sourceDrained(InstrumentProcessor& p){return InstrumentDrumTestAccess::accounting(p).inputs==0&&InstrumentDrumTestAccess::inputReleased(p.drumImport());}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool trackManualCallback=false;std::atomic<bool> trackManualWorkers{false};std::atomic<unsigned> manualAllocations{0},manualFrees{0};
thread_local bool trackReadback=false,denyReadback=false;thread_local unsigned readbackCopies=0,readbackDenials=0,readbackWarnings=0;
int __cdecl readbackAllocationHook(int kind,void*,size_t bytes,int use,long,const unsigned char*,int){
 // The pinned MSVC string allocator reserves 80 bytes for a copied 64-byte name;
 // Debug iterator proxies are deliberately never denied.
 if(trackReadback&&kind==_HOOK_ALLOC&&(use&0xffff)==_NORMAL_BLOCK&&bytes==80){++readbackCopies;if(denyReadback){++readbackDenials;return 0;}}return 1;
}
int __cdecl readbackReport(int type,wchar_t* message,int* value){
 const std::wstring_view text(message?message:L"");if(trackReadback&&denyReadback&&type==_CRT_WARN&&text==L"Client hook allocation failure.\n"){++readbackWarnings;if(value)*value=0;return 1;}return 0;
}
int __cdecl manualHeapHook(int kind,void*,size_t,int,long,const unsigned char*,int){
 if(trackManualCallback||(trackManualWorkers&&detail::DspPipeline::renderingOnThisThread())){if(kind==_HOOK_ALLOC||kind==_HOOK_REALLOC)++manualAllocations;if(kind==_HOOK_FREE)++manualFrees;}return 1;
}
#endif
void processManualWithoutHeap(InstrumentProcessor& p,juce::AudioBuffer<float>& audio,juce::MidiBuffer& midi){
#if defined(_MSC_VER) && defined(_DEBUG)
 manualAllocations=0;manualFrees=0;const auto previous=_CrtSetAllocHook(manualHeapHook);trackManualCallback=true;trackManualWorkers=true;
#endif
 p.processBlock(audio,midi);
#if defined(_MSC_VER) && defined(_DEBUG)
 trackManualCallback=false;trackManualWorkers=false;_CrtSetAllocHook(previous);CHECK(manualAllocations==0&&manualFrees==0);
#endif
}
juce::File sourceFile(unsigned frames=4800,unsigned rate=48000){
 const auto file=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("tile-import-source",".wav");juce::MemoryOutputStream bytes;
 bytes.write("RIFF",4);bytes.writeInt(int(36+frames*2));bytes.write("WAVEfmt ",8);bytes.writeInt(16);bytes.writeShort(1);bytes.writeShort(1);bytes.writeInt(int(rate));bytes.writeInt(int(rate*2));bytes.writeShort(2);bytes.writeShort(16);bytes.write("data",4);bytes.writeInt(int(frames*2));
 for(unsigned n=0;n<frames;++n)bytes.writeShort(short(std::sin(double(n)*.05)*8192));CHECK(file.replaceWithData(bytes.getData(),bytes.getDataSize()));return file;
}
juce::Uuid place(InstrumentProcessor& p,unsigned bank=0){ProjectDocument d;std::uint64_t edit=0;CHECK(p.tryProjectSnapshot(d,edit));RackDesign design;design.name="Manual drum fixture";design.panel.sections={"Drum"};design.units={{0,NativeModule::drum,{},1}};RackOperation op;op.design=design;op.newInstance=juce::Uuid();op.allowSlotReuse=true;op.allocation={{0,std::uint8_t(bank)}};op.destination={RackRole::synth,masterChain,std::uint8_t(bank)};CHECK(p.applyRackOperation(op,edit)==RackCandidateStatus::ready);return op.newInstance;}
void unprepared_audition_preserves_source_for_retry_and_offline_keep(){
 std::cout<<"GROUP unprepared_audition_preserves_source_for_retry_and_offline_keep"<<std::endl;
 auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);auto& importer=p.drumImport();const auto file=sourceFile();
 const auto cleanup=[&]{importer.cancel();p.releaseResources();CHECK(wait([&]{return sourceDrained(p);}));CHECK(file.deleteFile());};
 DrumPadTarget target;DrumCommitError error;const bool captured=p.captureDrumPadTarget(rack,0,15,target,error);CHECK(captured);if(!captured){cleanup();return;}
 const bool opened=importer.open(target);CHECK(opened);if(!opened){cleanup();return;}
 importer.choose(file);const bool inspected=wait([&]{return importer.view().inspected;});CHECK(inspected);if(!inspected){cleanup();return;}
 importer.edit(240,2640,.5f,"Audio retry");importer.prepare();const bool ready=wait([&]{return importer.view().phase==DrumImportPhase::ready;});CHECK(ready);if(!ready){cleanup();return;}
 const auto prepared=importer.view();const auto held=InstrumentDrumTestAccess::accounting(p);
 CHECK(prepared.canAudition&&prepared.canKeep&&prepared.preparedFrames==2400&&prepared.preparedPeak>0);
 CHECK(held.inputs==1&&held.running==0&&held.preview==0&&held.player==0&&std::count_if(held.tickets.begin(),held.tickets.end(),[](auto ticket){return ticket!=0;})==1);
 juce::MemoryBlock kit,current;p.getStateInformation(kit);CHECK(kit.getSize()>0);
 const auto unchangedSource=[&](const DrumImportView& view){
  CHECK(view.target==prepared.target&&view.session==prepared.session&&view.draft==prepared.draft);
  CHECK(view.begin==prepared.begin&&view.end==prepared.end&&view.gain==prepared.gain&&view.keptName==prepared.keptName);
  CHECK(view.preparedFrames==prepared.preparedFrames&&view.preparedPeak==prepared.preparedPeak&&view.canAudition&&view.canKeep);
  const auto actual=InstrumentDrumTestAccess::accounting(p);
  CHECK(actual.tickets==held.tickets&&actual.ids==held.ids&&actual.decoded==held.decoded&&actual.inputs==held.inputs&&actual.running==held.running);
  CHECK(actual.retained==held.retained&&actual.reserved==held.reserved&&actual.workspace==held.workspace&&actual.preview==held.preview&&actual.player==held.player);
  p.getStateInformation(current);CHECK(current==kit);
 };
 importer.audition();const auto unavailable=importer.view();
 std::cout<<"UNPREPARED_AUDITION phase="<<int(unavailable.phase)<<" message="<<unavailable.message<<std::endl;
 const bool recoverable=unavailable.phase==DrumImportPhase::ready;CHECK(recoverable);if(!recoverable){cleanup();return;}
 CHECK(unavailable.message=="Audio playback is not ready. Check audio settings, then try Audition again.");unchangedSource(unavailable);
 p.prepareToPlay(48000,512);CHECK(InstrumentDrumTestAccess::accounting(p).tickets==held.tickets);importer.poll();CHECK(importer.view().phase==DrumImportPhase::ready);
 importer.audition();const bool started=importer.view().phase==DrumImportPhase::auditioning;CHECK(started);if(!started){cleanup();return;}
 juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;processManualWithoutHeap(p,audio,midi);CHECK(audio.getMagnitude(0,512)>0);
 importer.stopPreview();p.releaseResources();importer.poll();
 const bool stopped=importer.view().phase==DrumImportPhase::ready;CHECK(stopped);if(!stopped){cleanup();return;}
 importer.audition();const auto released=importer.view();const bool stillReady=released.phase==DrumImportPhase::ready;CHECK(stillReady);if(!stillReady){cleanup();return;}
 CHECK(released.message==unavailable.message);unchangedSource(released);
 const bool kept=importer.keep();CHECK(kept);if(kept){
  DrumPadTarget keptTarget;DrumPadReadback readback;CHECK(p.captureDrumPerformanceTarget(rack,0,15,keptTarget,error));CHECK(p.readDrumPad(keptTarget,readback,error));
  CHECK(readback.frames==2400&&readback.channels==1&&readback.metadata.name=="Audio retry"&&readback.metadata.source&&readback.metadata.source->begin==240&&readback.metadata.source->end==2640&&readback.metadata.source->gain==.5f);
 }
 cleanup();
}
void chooser_explicit_crop_prepare_audition_keep(){
 std::cout<<"GROUP chooser_explicit_crop_prepare_audition_keep"<<std::endl;
 std::cout<<"fixture processor"<<std::endl;auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;std::cout<<"fixture placement"<<std::endl;const auto rack=place(p);std::cout<<"fixture prepare"<<std::endl;p.prepareToPlay(48000,512);
 DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,15,target,error));
 std::cout<<"fixture popup"<<std::endl;studio::DrumSourcePopup popup(p);std::cout<<"fixture open"<<std::endl;CHECK(popup.open(target));const auto file=sourceFile();std::cout<<"fixture choose"<<std::endl;popup.chooseSourceForTests(file);
 CHECK(wait([&]{return p.drumImport().view().inspected;}));
 popup.start.setText("0");popup.end.setText("4800");popup.gain.setText("0.5");popup.keptName.setText("Manual marker");pump();popup.prepare.onClick();
 const bool ready=wait([&]{return p.drumImport().view().phase==DrumImportPhase::ready;});CHECK(ready);
 CHECK(p.drumImport().view().canAudition&&p.drumImport().view().canKeep);stateEvidence(p,"source-canonical-ready");popup.refresh();CHECK(labelText(popup,"drum.source.selection").contains("peak"));
 for(auto* child:popup.getChildren())if(child->getComponentID()=="drum.source.selection")if(auto* label=dynamic_cast<juce::Label*>(child)){const auto available=label->getWidth()-label->getBorderSize().getLeftAndRight();for(const auto& line:juce::StringArray::fromLines(label->getText()))CHECK(juce::GlyphArrangement::getStringWidth(label->getLookAndFeel().getLabelFont(*label),line)<=available);CHECK(label->getHeight()-label->getBorderSize().getTopAndBottom()>=int(juce::StringArray::fromLines(label->getText()).size()*label->getLookAndFeel().getLabelFont(*label).getHeight()));}
 if(ready){
  const auto prepared=p.drumImport().view();CHECK(prepared.begin==0&&prepared.end==4800&&prepared.gain==.5f&&prepared.preparedFrames==4800);
  popup.audition.onClick();CHECK(wait([&]{return p.drumImport().view().phase==DrumImportPhase::auditioning;}));juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);std::vector<float> canonicalAudio;appendAudio(canonicalAudio,audio);stateEvidence(p,"source-canonical-audition");
  for(int cause=0;cause<6;++cause){if(cause==0)p.stopAiPreview();if(cause==1)p.cancelAi();if(cause==2)p.discardAiProposal();if(cause==3)p.closeAiSession();if(cause==4)CHECK(p.setAiMusicalContext({"drums","C","minor",120}));if(cause==5)p.groupCompanionDisconnected();CHECK(p.aiPreviewStopped()&&!p.aiPreviewPlaying());CHECK(!p.applyAiWhenStopped());CHECK(InstrumentDrumTestAccess::accounting(p).preview==4800*8);p.processBlock(audio,midi);CHECK(audio.getMagnitude(0,512)>0);appendAudio(canonicalAudio,audio);}
  if(evidence!=juce::File{}){for(int n=0;n<3;++n){p.processBlock(audio,midi);appendAudio(canonicalAudio,audio);}audioEvidence("actual-canonical-audition-48000-stereo-float",canonicalAudio);}
  const auto trim=drumParameterStart+15*drumParametersPerPad+3;CHECK(p.beginParameterGesture(trim));CHECK(p.setParameterGestureValue(trim,.2f));CHECK(p.endParameterGesture(trim));
  CHECK(p.drumImport().keep());DrumPadTarget keptTarget;DrumPadReadback readback;CHECK(p.captureDrumPerformanceTarget(rack,0,15,keptTarget,error));CHECK(p.readDrumPad(keptTarget,readback,error));
  CHECK(readback.frames==4800&&readback.channels==1&&readback.metadata.name=="Manual marker"&&readback.metadata.source&&readback.metadata.source->gain==.5f&&readback.metadata.source->begin==0&&readback.metadata.source->end==4800);CHECK(p.control(trim).getValue()==.2f);stateEvidence(p,"source-kept");
  std::vector<float> expected(4800);for(unsigned n=0;n<4800;++n)expected[n]=float(short(std::sin(double(n)*.05)*8192))/32768*.5f;drums::AssetPtr asset;drums::AssetError assetError;CHECK(drums::makeSampleAsset(expected,1,asset,assetError));CHECK(readback.metadata.asset&&*readback.metadata.asset==asset->id());
  DrumPadTarget neighbour;DrumPadReadback empty;CHECK(p.captureDrumPerformanceTarget(rack,0,14,neighbour,error));CHECK(p.readDrumPad(neighbour,empty,error)&&!empty.metadata.asset);
  CHECK(p.undoSoundEdit());CHECK(p.captureDrumPerformanceTarget(rack,0,15,keptTarget,error));CHECK(p.readDrumPad(keptTarget,empty,error)&&!empty.metadata.asset);CHECK(p.control(trim).getValue()==.2f);CHECK(p.redoSoundEdit());
  juce::MemoryBlock saved;p.getStateInformation(saved);CHECK(saved.getSize()>0);
  popup.close();CHECK(wait([&]{return InstrumentDrumTestAccess::accounting(p).inputs==0;}));CHECK(file.deleteFile());
  auto recalled=std::make_unique<InstrumentProcessor>();CHECK(recalled->restoreProjectState(saved.getData(),int(saved.getSize())));DrumPadTarget recallTarget;DrumPadReadback recalledPad;CHECK(recalled->captureDrumPerformanceTarget(rack,0,15,recallTarget,error));CHECK(recalled->readDrumPad(recallTarget,recalledPad,error));CHECK(recalledPad.metadata==readback.metadata&&recalledPad.frames==readback.frames);recalled->prepareToPlay(48000,512);
  for(int n=0;n<12;++n)p.processBlock(audio,midi);p.drumImport().poll();CHECK(wait([&]{return InstrumentDrumTestAccess::accounting(p).preview==0;}));
  CHECK(p.captureDrumPerformanceTarget(rack,0,15,keptTarget,error));CHECK(p.enqueueDrumPad(keptTarget,100));CHECK(recalled->enqueueDrumPad(recallTarget,100));juce::AudioBuffer<float> other(2,512);p.processBlock(audio,midi);recalled->processBlock(other,midi);for(int c=0;c<2;++c)for(int n=0;n<512;++n)CHECK(std::bit_cast<std::uint32_t>(audio.getSample(c,n))==std::bit_cast<std::uint32_t>(other.getSample(c,n)));std::vector<float> keptAudio;appendAudio(keptAudio,audio);if(evidence!=juce::File{}){for(int n=0;n<9;++n){p.processBlock(audio,midi);appendAudio(keptAudio,audio);}audioEvidence("actual-kept-pad16-48000-stereo-float",keptAudio);}recalled->releaseResources();
 }else{popup.close();CHECK(wait([&]{return p.drumImport().view().phase==DrumImportPhase::closed||p.drumImport().view().phase==DrumImportPhase::kept;}));CHECK(file.deleteFile());}
 p.releaseResources();
}
void input_slots_reservations_and_fresh_tickets(){
 std::cout<<"GROUP input_slots_reservations_and_fresh_tickets"<<std::endl;auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);DrumCommitError error;
 std::array<std::uint64_t,3> ids{};for(auto& id:ids)CHECK(p.beginDrumInput(id,error));const auto raw=InstrumentDrumTestAccess::accounting(p);CHECK(raw.inputs==3&&raw.workspace==3*8388608ull+393224);if(evidence!=juce::File{}){DrumPadTarget target;CHECK(p.captureDrumPadTarget(rack,0,0,target,error));CHECK(!p.drumImport().open(target));const auto rejected=p.drumImport().view();CHECK(rejected.phase==DrumImportPhase::closed&&!rejected.inspected&&rejected.message.contains("All three preparation slots"));CHECK(InstrumentDrumTestAccess::accounting(p).inputs==3);stateEvidence(p,"source-three-slot-open-rejection-controller-readback-fixture");}
 std::uint64_t untouched=987;CHECK(!p.beginDrumInput(untouched,error)&&error==DrumCommitError::preparationFull&&untouched==987);CHECK(!p.prepareDrumAsset(std::array<float,2>{.1f,.2f},1,untouched,error)&&error==DrumCommitError::preparationFull&&untouched==987);
 p.cancelDrumInput(ids[0]);CHECK(InstrumentDrumTestAccess::accounting(p).inputs==3);CHECK(!p.beginDrumInput(untouched,error)&&error==DrumCommitError::preparationFull);InstrumentDrumTestAccess::releaseInput(p,ids[0]);std::uint64_t fresh=0;CHECK(p.beginDrumInput(fresh,error)&&fresh>ids[2]);
 CHECK(p.reserveDrumInputPreparation(fresh,480000,2,error));const auto peak=InstrumentDrumTestAccess::accounting(p);CHECK(peak.reserved==7680000);CHECK(peak.workspace==3*8388608ull+16777216ull+30724256ull+393224);CHECK(!p.reserveDrumInputPreparation(fresh,1,1,error)&&error==DrumCommitError::busy);
 p.cancelDrumInput(fresh);const auto cancelled=InstrumentDrumTestAccess::accounting(p);CHECK(cancelled.inputs==3&&cancelled.reserved==peak.reserved&&cancelled.workspace==peak.workspace);InstrumentDrumTestAccess::releaseInputPreparation(p,fresh);CHECK(InstrumentDrumTestAccess::accounting(p).workspace==3*8388608ull+393224);for(auto id:{fresh,ids[1],ids[2]}){p.cancelDrumInput(id);InstrumentDrumTestAccess::releaseInput(p,id);}CHECK(InstrumentDrumTestAccess::accounting(p).workspace==393224);
 std::uint64_t input=0;CHECK(p.beginDrumInput(input,error));std::vector<float> pcm(32,.125f);drums::AssetPtr expected;drums::AssetError ae;CHECK(drums::makeSampleAsset(pcm,1,expected,ae));CHECK(p.reserveDrumInputPreparation(input,32,1,error));std::uint64_t first=0;CHECK(p.finishDrumInput(input,pcm,1,expected->id(),first,error));auto transferred=InstrumentDrumTestAccess::accounting(p);CHECK(transferred.reserved==128&&transferred.retained==256);InstrumentDrumTestAccess::releaseInputPreparation(p,input);CHECK(InstrumentDrumTestAccess::accounting(p).reserved==0);
 CHECK(!p.reserveDrumInputPreparation(input,480001,1,error)&&error==DrumCommitError::invalid);CHECK(InstrumentDrumTestAccess::accounting(p).tickets[0]==first);
 CHECK(p.reserveDrumInputPreparation(input,32,1,error));std::uint64_t second=0;CHECK(p.finishDrumInput(input,pcm,1,expected->id(),second,error)&&second!=first&&second>first);InstrumentDrumTestAccess::releaseInputPreparation(p,input);
 DrumPadTarget target;CHECK(p.captureDrumPadTarget(rack,0,0,target,error));drums::DrumPad metadata;metadata.asset=expected->id();metadata.name="same-content";CHECK(!p.assignPreparedDrumPad(target,first,metadata,{},error)&&error==DrumCommitError::cancelled);CHECK(p.assignPreparedDrumPad(target,second,metadata,{},error));CHECK(InstrumentDrumTestAccess::accounting(p).inputs==1);std::array<std::uint64_t,2> other{};for(auto& id:other)CHECK(p.beginDrumInput(id,error));CHECK(!p.beginDrumInput(untouched,error)&&error==DrumCommitError::preparationFull);for(auto id:{input,other[0],other[1]}){p.cancelDrumInput(id);InstrumentDrumTestAccess::releaseInput(p,id);}
 CHECK(InstrumentDrumTestAccess::accounting(p).inputs==0&&InstrumentDrumTestAccess::accounting(p).reserved==0&&InstrumentDrumTestAccess::accounting(p).workspace==393224);
}
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
void wholeSourceWorkflow(InstrumentProcessor& p,const juce::Uuid& rack){
 DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,8,target,error));studio::DrumSourcePopup popup(p);CHECK(popup.open(target));
 auto file=sourceFile(442368,44100);const auto longFile=file.getParentDirectory().getNonexistentChildFile(juce::String::repeatedString("n",70),".wav");
 const bool renamed=file.moveFileTo(longFile);CHECK(renamed);if(renamed)file=longFile;
 popup.chooseSourceForTests(file);CHECK(wait([&]{return p.drumImport().view().inspected;}));popup.refresh();
 const auto whole=p.drumImport().view();CHECK(whole.source.rate==44100&&whole.source.channels==1&&whole.source.frames==442368&&whole.source.buckets==512);
 CHECK(whole.begin==0&&whole.end==442368&&whole.gain==1&&!whole.canPrepare&&!whole.canAudition&&!whole.canKeep);
 CHECK(whole.sourceName==file.getFileName()&&whole.keptName.isEmpty()&&whole.message.contains("441000"));stateEvidence(p,"source-whole-too-long");CHECK(labelText(popup,"drum.source.selection").contains(juce::String::charToString(0x2014)));
 popup.prepare.onClick();CHECK(p.drumImport().view().phase==DrumImportPhase::editing&&!p.drumImport().keep());
 CHECK(InstrumentDrumTestAccess::accounting(p).reserved==0);
 popup.end.setText("441000",false);popup.end.onReturnKey();CHECK(p.drumImport().view().canPrepare);
 popup.end.setText("441001",false);popup.end.onReturnKey();CHECK(!p.drumImport().view().canPrepare&&popup.end.getText()=="441001");
 popup.end.setText("18446744073709551615",false);popup.end.onReturnKey();popup.refresh();CHECK(popup.end.getText()=="18446744073709551615"&&!popup.prepare.isEnabled()&&!popup.keep.isEnabled()&&labelText(popup,"drum.source.selection").contains("Invalid selection"));
 popup.start.setText("-1",false);popup.start.onReturnKey();popup.refresh();CHECK(popup.start.getText()=="-1"&&!popup.prepare.isEnabled()&&!popup.keep.isEnabled());
 popup.start.setText("0",false);popup.end.setText("4410",false);popup.keptName.setText("explicit gain",false);
 for(const auto* invalid:{"nan","inf","0","-1","16.0001"}){
  popup.gain.setText(invalid,false);popup.gain.onReturnKey();popup.refresh();CHECK(popup.gain.getText()==invalid&&!popup.prepare.isEnabled()&&!popup.keep.isEnabled());CHECK(!p.drumImport().keep());
 }
 popup.gain.setText("8",false);popup.prepare.onClick();CHECK(wait([&]{return p.drumImport().view().phase==DrumImportPhase::error;}));
 const auto clipped=p.drumImport().view();CHECK(clipped.gain==8&&clipped.begin==0&&clipped.end==4410&&clipped.preparedFrames==0&&!clipped.canAudition&&!clipped.canKeep&&clipped.message.contains("lower gain"));
 CHECK(popup.gain.getText()=="8"&&!p.drumImport().keep());stateEvidence(p,"source-explicit-gain-required");DrumPadReadback empty;CHECK(p.readDrumPad(target,empty,error)&&!empty.metadata.asset);
 popup.end.setText("441000",false);popup.gain.setText("0.125",false);popup.keptName.setText("Ten seconds",false);popup.prepare.onClick();
 CHECK(wait([&]{return p.drumImport().view().phase==DrumImportPhase::ready;}));popup.refresh();CHECK(p.drumImport().view().preparedFrames==480000);stateEvidence(p,"source-ten-second-prepared-summary");
 for(auto* child:popup.getChildren())if(child->getComponentID()=="drum.source.selection")if(auto* label=dynamic_cast<juce::Label*>(child)){const auto available=label->getWidth()-label->getBorderSize().getLeftAndRight();std::cout<<"PREPARED_SUMMARY_WIDTH "<<juce::GlyphArrangement::getStringWidth(label->getLookAndFeel().getLabelFont(*label),label->getText())<<" available="<<available<<std::endl;for(const auto& line:juce::StringArray::fromLines(label->getText()))CHECK(juce::GlyphArrangement::getStringWidth(label->getLookAndFeel().getLabelFont(*label),line)<=available);CHECK(label->getHeight()-label->getBorderSize().getTopAndBottom()>=int(juce::StringArray::fromLines(label->getText()).size()*label->getLookAndFeel().getLabelFont(*label).getHeight()));}
 popup.end.setText("4410",false);popup.gain.setText("0.125",false);popup.keptName.setText("",false);popup.prepare.onClick();
 CHECK(wait([&]{return p.drumImport().view().phase==DrumImportPhase::ready;}));CHECK(p.drumImport().view().keptName.isEmpty()&&p.drumImport().view().canAudition&&!p.drumImport().view().canKeep&&!p.drumImport().keep());
 popup.keptName.setText(juce::String::repeatedString("x",65),false);popup.prepare.onClick();
 CHECK(wait([&]{return p.drumImport().view().phase==DrumImportPhase::ready;}));CHECK(p.drumImport().view().canAudition&&!p.drumImport().view().canKeep&&!p.drumImport().keep());
 const auto legalName=juce::String::repeatedString(juce::String::charToString(0x00e9),32);CHECK(legalName.getNumBytesAsUTF8()==64);
 popup.keptName.setText(legalName,false);popup.prepare.onClick();const bool ready=wait([&]{return p.drumImport().view().phase==DrumImportPhase::ready;});CHECK(ready);
 if(ready){CHECK(p.drumImport().keep());DrumPadTarget kept;DrumPadReadback readback;CHECK(p.captureDrumPerformanceTarget(rack,0,8,kept,error));CHECK(p.readDrumPad(kept,readback,error));
  CHECK(readback.frames==4800&&readback.metadata.name==legalName.toStdString()&&readback.metadata.source&&readback.metadata.source->begin==0&&readback.metadata.source->end==4410&&readback.metadata.source->gain==.125f);
 }
 popup.close();CHECK(wait([&]{return sourceDrained(p);}));CHECK(file.deleteFile());
 // A second real rate observes the same explicit ten-second boundary.
 const auto other=sourceFile(480001,48000);CHECK(p.captureDrumPadTarget(rack,0,9,target,error));CHECK(popup.open(target));popup.chooseSourceForTests(other);CHECK(wait([&]{return p.drumImport().view().inspected;}));
 CHECK(p.drumImport().view().end==480001&&!p.drumImport().view().canPrepare);p.drumImport().edit(0,480000,1,"boundary");CHECK(p.drumImport().view().canPrepare);p.drumImport().edit(0,480001,1,"boundary");CHECK(!p.drumImport().view().canPrepare);
 popup.close();CHECK(wait([&]{return sourceDrained(p);}));const auto closed=p.drumImport().view();popup.chooseSourceForTests(other);pump();CHECK(p.drumImport().view().session==closed.session&&p.drumImport().view().phase==DrumImportPhase::closed&&InstrumentDrumTestAccess::accounting(p).inputs==0);CHECK(other.deleteFile());
}
void whole_source_requires_explicit_legal_crop_gain_and_kept_name(){
 std::cout<<"GROUP whole_source_requires_explicit_legal_crop_gain_and_kept_name"<<std::endl;
 auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);
 wholeSourceWorkflow(p,rack);
}
void copied_readback_survives_locks_and_rejects_replaced_targets_in_every_bank(){
 std::cout<<"GROUP copied_readback_survives_locks_and_rejects_replaced_targets_in_every_bank"<<std::endl;
 auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;
 const std::array<float,4> pcm{.125f,-.25f,.375f,-.5f};drums::AssetPtr expected;drums::AssetError assetError;CHECK(drums::makeSampleAsset(pcm,1,expected,assetError));
 for(unsigned bank=0;bank<4;++bank){
  const auto rack=place(p,bank);DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,15,target,error));
  std::uint64_t ticket=0;CHECK(p.prepareDrumAsset(pcm,1,ticket,error));drums::DrumPad metadata;metadata.asset=expected->id();metadata.name="copied bank "+std::to_string(bank);
  CHECK(p.assignPreparedDrumPad(target,ticket,metadata,{},error));CHECK(p.captureDrumPerformanceTarget(rack,0,15,target,error));DrumPadReadback copied;CHECK(p.readDrumPad(target,copied,error));CHECK(copied.metadata==metadata&&copied.frames==4&&copied.channels==1);
  auto document=std::make_unique<ProjectDocument>();std::uint64_t revision=0;CHECK(p.tryProjectSnapshot(*document,revision));document->sound.synthLocks[bank]=true;CHECK(p.applySoundEdit(document->sound,SoundEditOrigin::manual,revision));
  DrumPadTarget locked;CHECK(p.captureDrumPerformanceTarget(rack,0,15,locked,error));CHECK(!p.captureDrumPadTarget(rack,0,15,locked,error)&&error==DrumCommitError::locked);
  CHECK(p.readDrumPad(target,copied,error)&&copied.metadata==metadata&&copied.frames==4);CHECK(!p.drumImport().open(target)&&InstrumentDrumTestAccess::accounting(p).inputs==0);CHECK(!p.clearDrumPad(target,error)&&error==DrumCommitError::locked);
  CHECK(p.tryProjectSnapshot(*document,revision));document->sound.synthLocks[bank]=false;CHECK(p.applySoundEdit(document->sound,SoundEditOrigin::manual,revision));
  CHECK(p.captureDrumPadTarget(rack,0,15,target,error));CHECK(p.clearDrumPad(target,error));const auto preserved=copied;
  CHECK(!p.readDrumPad(target,copied,error)&&error==DrumCommitError::staleTarget);CHECK(copied.metadata==preserved.metadata&&copied.frames==preserved.frames&&copied.channels==preserved.channels);
  CHECK(p.captureDrumPerformanceTarget(rack,0,15,target,error));CHECK(p.readDrumPad(target,copied,error)&&copied.metadata==drums::DrumPad{}&&copied.frames==0&&copied.channels==0);
  CHECK(p.undoSoundEdit());CHECK(p.captureDrumPerformanceTarget(rack,0,15,target,error));CHECK(p.readDrumPad(target,copied,error)&&copied.metadata==metadata&&copied.frames==4);CHECK(p.redoSoundEdit());
 }
}

void copied_readback_allocation_failure_preserves_every_output(){
 std::cout<<"GROUP copied_readback_allocation_failure_preserves_every_output"<<std::endl;
#if defined(_MSC_VER) && defined(_DEBUG)
 auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,15,target,error));
 const std::array<float,4> pcm{.1f,.2f,.3f,.4f};drums::AssetPtr expected;drums::AssetError assetError;CHECK(drums::makeSampleAsset(pcm,1,expected,assetError));std::uint64_t ticket=0;CHECK(p.prepareDrumAsset(pcm,1,ticket,error));drums::DrumPad metadata;metadata.asset=expected->id();metadata.name=std::string(64,'x');CHECK(p.assignPreparedDrumPad(target,ticket,metadata,{},error));CHECK(p.captureDrumPerformanceTarget(rack,0,15,target,error));
 DrumPadReadback result;const auto oldHook=_CrtSetAllocHook(readbackAllocationHook);readbackCopies=readbackDenials=readbackWarnings=0;trackReadback=true;const bool calibrated=p.readDrumPad(target,result,error);trackReadback=false;_CrtSetAllocHook(oldHook);CHECK(calibrated&&readbackCopies==1&&result.metadata==metadata);
 result.metadata.name="preserved output";result.frames=123;result.channels=2;const auto preserved=result;
 _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,readbackReport);const auto prior=_CrtSetAllocHook(readbackAllocationHook);denyReadback=true;trackReadback=true;const bool accepted=p.readDrumPad(target,result,error);trackReadback=false;denyReadback=false;_CrtSetAllocHook(prior);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,readbackReport);
 CHECK(!accepted&&error==DrumCommitError::allocationFailed&&readbackDenials==1&&readbackWarnings==1);CHECK(result.metadata==preserved.metadata&&result.frames==preserved.frames&&result.channels==preserved.channels);
 CHECK(p.readDrumPad(target,result,error)&&result.metadata==metadata&&result.frames==4&&result.channels==1);
#else
 CHECK(false&&"Assigned Debug MSVC readback allocation fault coverage is required");
#endif
}

#ifndef NDEBUG
// These gates own no source/result. Their callbacks run inside the actual importer worker.
class ImportGate final {
public:
 using Point=DrumImport::TestPoint;
 void arm(Point point){const std::lock_guard lock(mutex);blocked=false;released=false;target=point;targetVisit=visits[std::size_t(point)]+1;}
 void release(){const std::lock_guard lock(mutex);released=true;wake.notify_all();}
 bool entered(){const std::lock_guard lock(mutex);return blocked;}
 unsigned count(Point point){const std::lock_guard lock(mutex);return visits[std::size_t(point)];}
 bool timedOut(){const std::lock_guard lock(mutex);return expired;}
 static void observe(void* raw,Point point) noexcept {
  auto& gate=*static_cast<ImportGate*>(raw);std::unique_lock lock(gate.mutex);
  if(++gate.visits[std::size_t(point)]!=gate.targetVisit||point!=gate.target)return;
  gate.blocked=true;gate.wake.notify_all();
  if(!gate.wake.wait_for(lock,std::chrono::seconds(60),[&]{return gate.released;}))gate.expired=true;
 }
 ~ImportGate(){release();}
private:
 std::mutex mutex;std::condition_variable wake;std::array<unsigned,5> visits{};
 Point target=Point::beforeInspect;unsigned targetVisit=0;bool blocked=false,released=false,expired=false;
};
void real_worker_cancel_retains_admission_until_source_and_helper_die(){
 using Point=ImportGate::Point;
 const std::array<std::pair<Point,bool>,5> stages{{{Point::beforeInspect,false},{Point::beforeCompletion,false},{Point::beforePrepare,true},{Point::afterHelper,true},{Point::beforeCompletion,true}}};
 for(const auto [point,preparing]:stages){
  std::cout<<"GROUP real_worker_cancel_retains_admission phase="<<int(point)<<" prepare="<<preparing<<std::endl;
  ImportGate gate;auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);
  DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,0,target,error));
  auto& importer=p.drumImport();InstrumentDrumTestAccess::hook(importer,&gate,ImportGate::observe);
  studio::DrumSourcePopup popup(p);CHECK(popup.open(target));const auto file=sourceFile();
  if(!preparing)gate.arm(point);
  popup.chooseSourceForTests(file);
  if(preparing){CHECK(wait([&]{return importer.view().inspected;}));gate.arm(point);importer.edit(0,4800,.5f,"cancelled helper");importer.prepare();}
  const bool entered=wait([&]{return gate.entered();});CHECK(entered);
  if(entered){
   const auto live=InstrumentDrumTestAccess::accounting(p);CHECK(live.inputs==1);
   stateEvidence(p,"source-worker-held-"+juce::String(int(point))+"-"+juce::String(int(preparing)));
   const bool helperLive=preparing&&point!=Point::beforeCompletion;
   CHECK(live.workspace==8388608ull+393224+(helperLive?16777216ull+8*(20+4800*4)+4096:0));
   CHECK(live.reserved==(helperLive?2*4800*4:0));
   if(preparing&&!helperLive)CHECK(live.running==0&&live.retained==4800*4);
   // Popup retirement must return while this very worker is still held.
   popup.close();importer.cancel();const auto cancelled=InstrumentDrumTestAccess::accounting(p);
   CHECK(gate.entered()&&!gate.timedOut());CHECK(cancelled.ids==live.ids&&cancelled.inputs==1);stateEvidence(p,"source-cancel-draining-"+juce::String(int(point))+"-"+juce::String(int(preparing)));
   CHECK(cancelled.reserved==live.reserved&&cancelled.workspace==live.workspace);
   CHECK(std::all_of(cancelled.tickets.begin(),cancelled.tickets.end(),[](auto ticket){return ticket==0;}));
   CHECK(!importer.view().canAudition&&!importer.view().canKeep&&!importer.keep());
   std::array<std::uint64_t,2> other{};for(auto& id:other)CHECK(p.beginDrumInput(id,error));
   std::uint64_t untouched=17;CHECK(!p.beginDrumInput(untouched,error)&&error==DrumCommitError::preparationFull&&untouched==17);
   CHECK(!p.prepareDrumAsset(std::array<float,1>{.1f},1,untouched,error)&&error==DrumCommitError::preparationFull&&untouched==17);
   for(auto id:other){p.cancelDrumInput(id);InstrumentDrumTestAccess::releaseInput(p,id);}
  }
  importer.cancel();gate.release();CHECK(wait([&]{return sourceDrained(p);}));
  const auto drained=InstrumentDrumTestAccess::accounting(p);CHECK(drained.reserved==0&&drained.workspace==393224);
  CHECK(!importer.view().canKeep&&!importer.view().canAudition&&!importer.keep());
  CHECK(!gate.timedOut());CHECK(file.deleteFile());
 }
}
void successful_old_completion_cannot_ready_or_keep_an_edited_tuple(){
 std::cout<<"GROUP successful_old_completion_cannot_ready_or_keep_an_edited_tuple"<<std::endl;
 ImportGate gate;auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);
 DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,3,target,error));
 auto& importer=p.drumImport();InstrumentDrumTestAccess::hook(importer,&gate,ImportGate::observe);
 CHECK(importer.open(target));const auto file=sourceFile();importer.choose(file);CHECK(wait([&]{return importer.view().inspected;}));
 importer.edit(0,4800,.5f,"old tuple");gate.arm(ImportGate::Point::beforeCompletion);importer.prepare();
 const bool completed=wait([&]{return gate.entered();});CHECK(completed);
 if(completed){
  const auto admitted=InstrumentDrumTestAccess::accounting(p);const auto oldDraft=importer.view().draft;
  CHECK(admitted.inputs==1&&admitted.running==0&&admitted.reserved==0&&admitted.retained==4800*4);
  CHECK(admitted.workspace==8388608ull+393224);
  CHECK(std::count_if(admitted.tickets.begin(),admitted.tickets.end(),[](auto ticket){return ticket!=0;})==1);
  // Illegal new gain prevents an automatic replacement job from hiding an accepted stale completion.
  importer.edit(0,4800,0,"edited tuple");const auto edited=importer.view();CHECK(edited.draft>oldDraft);
  CHECK(!edited.canPrepare&&!edited.canAudition&&!edited.canKeep&&!importer.keep());
  gate.release();
  // Do not dispatch the control pump until the real old success reaches its mailbox.
  CHECK(waitWithoutDispatch([&]{return InstrumentDrumTestAccess::completionPending(importer);}));importer.poll();
  CHECK(!InstrumentDrumTestAccess::completionPending(importer));
  const auto stale=importer.view();CHECK(stale.draft==edited.draft&&stale.gain==0&&stale.keptName=="edited tuple");
  CHECK(stale.phase==DrumImportPhase::editing&&!stale.canAudition&&!stale.canKeep&&stale.preparedFrames==0);stateEvidence(p,"source-real-stale-draft-rejected");
  CHECK(!importer.keep());DrumPadReadback unchanged;CHECK(p.readDrumPad(target,unchanged,error)&&!unchanged.metadata.asset);
  importer.edit(0,4800,.25f,"new tuple");importer.prepare();CHECK(wait([&]{return importer.view().phase==DrumImportPhase::ready;}));
  const auto replacement=InstrumentDrumTestAccess::accounting(p);CHECK(replacement.tickets!=admitted.tickets);
  CHECK(importer.view().gain==.25f&&importer.view().preparedFrames==4800&&importer.view().canKeep);
 }
 gate.release();importer.cancel();CHECK(wait([&]{return sourceDrained(p);}));CHECK(file.deleteFile());CHECK(!gate.timedOut());
}
void three_edits_during_real_preparation_coalesce_to_the_newest_tuple(){
 std::cout<<"GROUP three_edits_during_real_preparation_coalesce_to_the_newest_tuple"<<std::endl;
 ImportGate gate;auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);
 DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,6,target,error));auto& importer=p.drumImport();
 InstrumentDrumTestAccess::hook(importer,&gate,ImportGate::observe);CHECK(importer.open(target));const auto file=sourceFile();importer.choose(file);CHECK(wait([&]{return importer.view().inspected;}));
 importer.edit(0,4800,.5f,"initial tuple");gate.arm(ImportGate::Point::beforePrepare);importer.prepare();
 const bool entered=wait([&]{return gate.entered();});CHECK(entered);
 if(entered){
  const auto held=InstrumentDrumTestAccess::accounting(p);const auto oldDraft=importer.view().draft;const auto helperCompletions=gate.count(ImportGate::Point::afterHelper);
  CHECK(held.inputs==1&&held.running==1&&held.reserved==2*4800*4);
  importer.edit(0,2400,.25f,"intermediate one");importer.edit(1200,3600,.125f,"intermediate two");importer.edit(2400,4800,.0625f,"newest tuple");
  CHECK(importer.view().draft==oldDraft+3&&!importer.view().canAudition&&!importer.view().canKeep&&!importer.keep());
  // There is deliberately no second Prepare call: the pending command must track every edit.
  gate.release();const bool ready=wait([&]{return importer.view().phase==DrumImportPhase::ready;});CHECK(ready);
  if(ready){
   const auto newest=importer.view();CHECK(newest.draft==oldDraft+3&&newest.begin==2400&&newest.end==4800&&newest.gain==.0625f&&newest.keptName=="newest tuple"&&newest.preparedFrames==2400&&newest.canKeep);
   CHECK(InstrumentDrumTestAccess::accounting(p).ids==held.ids);CHECK(gate.count(ImportGate::Point::afterHelper)==helperCompletions+1);
   CHECK(importer.keep());DrumPadTarget kept;DrumPadReadback readback;CHECK(p.captureDrumPerformanceTarget(rack,0,6,kept,error));CHECK(p.readDrumPad(kept,readback,error));
   CHECK(readback.frames==2400&&readback.metadata.name=="newest tuple"&&readback.metadata.source&&readback.metadata.source->begin==2400&&readback.metadata.source->end==4800&&readback.metadata.source->gain==.0625f);
  }
 }
 gate.release();importer.cancel();CHECK(wait([&]{return sourceDrained(p);}));CHECK(file.deleteFile());CHECK(!gate.timedOut());
}
void preview_stop_without_callback_retains_actual_player_capacity(){
 std::cout<<"GROUP preview_stop_without_callback_retains_actual_player_capacity"<<std::endl;
 auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);for(unsigned bank=1;bank<4;++bank)place(p,bank);
 for(unsigned slot=0;slot<2;++slot){auto document=std::make_unique<ProjectDocument>();std::uint64_t revision=0;CHECK(p.tryProjectSnapshot(*document,revision));CHECK(p.applyRackEdit({RackEditKind::addEffect,std::uint8_t(slot),masterChain,std::uint8_t(slot),EffectKind::dynamicEq},revision)==RackEditResult::applied);}
 p.setNonRealtime(true);p.prepareToPlay(48000,512);
 DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,4,target,error));auto& importer=p.drumImport();
 CHECK(importer.open(target));const auto file=sourceFile(48000);importer.choose(file);CHECK(wait([&]{return importer.view().inspected;}));importer.prepare();
 const bool ready=wait([&]{return importer.view().phase==DrumImportPhase::ready;});CHECK(ready);
 if(ready){
  importer.audition();CHECK(wait([&]{return importer.view().phase==DrumImportPhase::auditioning;}));
  juce::AudioBuffer<float> audio(2,512);juce::MidiBuffer midi;processManualWithoutHeap(p,audio,midi);CHECK(audio.getMagnitude(0,512)>0);CHECK(InstrumentDrumTestAccess::workerBlocks(p)>0);
  const auto playing=InstrumentDrumTestAccess::accounting(p);CHECK(playing.player==48000*8&&playing.preview==playing.player);
  importer.stopPreview();importer.cancel();CHECK(wait([&]{return sourceDrained(p);}));
  for(int n=0;n<5;++n){pump();const auto held=InstrumentDrumTestAccess::accounting(p);CHECK(held.player==playing.player&&held.preview==playing.preview&&held.reserved==playing.preview);}
  // Only the real callback can acknowledge its old borrow; the control pump then clears it.
  for(int n=0;n<4;++n)processManualWithoutHeap(p,audio,midi);
  CHECK(wait([&]{return InstrumentDrumTestAccess::accounting(p).player==0;}));
  const auto cleared=InstrumentDrumTestAccess::accounting(p);CHECK(cleared.preview==0&&cleared.reserved==0);
  CHECK(importer.open(target));importer.choose(file);CHECK(wait([&]{return importer.view().inspected;}));importer.prepare();CHECK(wait([&]{return importer.view().phase==DrumImportPhase::ready;}));importer.audition();CHECK(wait([&]{return importer.view().phase==DrumImportPhase::auditioning;}));
  CHECK(InstrumentDrumTestAccess::accounting(p).player==playing.player);p.releaseResources();
  const auto released=InstrumentDrumTestAccess::accounting(p);CHECK(released.player==0&&released.preview==0);
 }
 importer.cancel();CHECK(wait([&]{return sourceDrained(p);}));CHECK(file.deleteFile());p.releaseResources();
}
void actual_worker_join_leaves_processor_mailbox_and_dispatch_available(){
 std::cout<<"GROUP actual_worker_join_leaves_processor_mailbox_and_dispatch_available"<<std::endl;
 struct CallbackWitness {
  std::atomic<unsigned> callbacks{0};
  static void observe(void* raw,InstrumentDrumTestAccess::ProcessorPoint point){if(point==InstrumentDrumTestAccess::ProcessorPoint::beforeControlPumpWork)++static_cast<CallbackWitness*>(raw)->callbacks;}
 } callbacks;
 ImportGate gate;auto storage=std::make_unique<InstrumentProcessor>();auto* p=storage.get();const auto rack=place(*p);
 DrumPadTarget target;DrumCommitError error;CHECK(p->captureDrumPadTarget(rack,0,0,target,error));auto* importer=&p->drumImport();
 InstrumentDrumTestAccess::hook(*importer,&gate,ImportGate::observe);InstrumentDrumTestAccess::processorHook(*p,&callbacks,CallbackWitness::observe);
 CHECK(importer->open(target));const auto file=sourceFile();importer->choose(file);CHECK(wait([&]{return importer->view().inspected;}));
 gate.arm(ImportGate::Point::afterHelper);importer->prepare();const bool entered=wait([&]{return gate.entered();});CHECK(entered);
 if(entered){
  const auto live=InstrumentDrumTestAccess::accounting(*p);CHECK(live.reserved==2*4800*4&&live.running==1);
  std::atomic<bool> destroyed{false},dispatched{false};
  std::thread lifecycle([&]{storage.reset();destroyed.store(true,std::memory_order_release);});
  const bool joining=wait([&]{return gate.count(ImportGate::Point::beforeJoin)!=0;});CHECK(joining);
  if(joining&&!destroyed.load()&&!gate.timedOut()){
   CHECK(InstrumentDrumTestAccess::teardownLocksAvailable(*p,*importer));CHECK(InstrumentDrumTestAccess::callbacksStopped(*p));
   const auto held=InstrumentDrumTestAccess::accounting(*p);CHECK(held.inputs==1&&held.reserved==live.reserved&&held.workspace==live.workspace);
   const auto stoppedCount=callbacks.callbacks.load();
   CHECK(juce::MessageManager::callAsync([&]{dispatched=true;}));CHECK(wait([&]{return dispatched.load();}));
   CHECK(callbacks.callbacks==stoppedCount&&!destroyed.load());
  }
  // The controller releases its own helper barrier before waiting for actual destruction.
  gate.release();CHECK(wait([&]{return destroyed.load(std::memory_order_acquire);}));lifecycle.join();
  const auto stoppedCount=callbacks.callbacks.load();pump();CHECK(callbacks.callbacks==stoppedCount);
 }
 gate.release();storage.reset();CHECK(!gate.timedOut());CHECK(file.deleteFile());
}
void importer_teardown_preserves_message_thread_and_already_locked_callers(){
 std::cout<<"GROUP importer_teardown_preserves_message_thread_and_already_locked_callers"<<std::endl;
 struct JoinWitness {
  InstrumentProcessor* processor=nullptr;DrumImport* importer=nullptr;
  bool joined=false,stopped=false,messageLocked=false;
  static void observe(void* raw,DrumImport::TestPoint point) noexcept {
   if(point!=DrumImport::TestPoint::beforeJoin)return;auto& state=*static_cast<JoinWitness*>(raw);
   state.joined=true;state.stopped=InstrumentDrumTestAccess::callbacksStopped(*state.processor);
   state.messageLocked=juce::MessageManager::existsAndIsLockedByCurrentThread();
  }
 };
 const auto file=sourceFile();
 for(bool alreadyLocked:{false,true}){
  JoinWitness witness;auto storage=std::make_unique<InstrumentProcessor>();witness.processor=storage.get();witness.importer=&storage->drumImport();
  const auto rack=place(*storage);DrumPadTarget target;DrumCommitError error;CHECK(storage->captureDrumPadTarget(rack,0,0,target,error));
  InstrumentDrumTestAccess::hook(*witness.importer,&witness,JoinWitness::observe);CHECK(witness.importer->open(target));witness.importer->choose(file);CHECK(wait([&]{return witness.importer->view().inspected;}));
  if(alreadyLocked){std::atomic<bool> done{false};std::thread lifecycle([&]{const juce::MessageManagerLock callerLock;storage.reset();done=true;});CHECK(wait([&]{return done.load();}));lifecycle.join();}
  else storage.reset();
  CHECK(witness.joined&&witness.stopped&&witness.messageLocked);
 }
 CHECK(file.deleteFile());
}
#endif
void posted_field_escape_cancels_and_returns_focus(){
 std::cout<<"GROUP posted_field_escape_cancels_and_returns_focus"<<std::endl;auto storage=std::make_unique<InstrumentProcessor>();auto& p=*storage;const auto rack=place(p);DrumPadTarget target;DrumCommitError error;CHECK(p.captureDrumPadTarget(rack,0,15,target,error));
 juce::Component holder;juce::TextButton opener("Import source");studio::DrumSourcePopup popup(p);holder.setSize(840,470);opener.setBounds(730,10,100,35);holder.addAndMakeVisible(opener);holder.addAndMakeVisible(popup);holder.addToDesktop(juce::ComponentPeer::windowIsTemporary);holder.setVisible(true);bool closed=false;popup.onClosed=[&]{closed=true;};
 for(auto* field:{&popup.keptName,&popup.start,&popup.end,&popup.gain}){
  closed=false;CHECK(popup.open(target,&opener));field->grabKeyboardFocus();pump();CHECK(field->hasKeyboardFocus(false));auto* peer=holder.getPeer();CHECK(peer);if(peer){const auto window=static_cast<HWND>(peer->getNativeHandle());CHECK(PostMessageW(window,WM_KEYDOWN,VK_ESCAPE,1));CHECK(PostMessageW(window,WM_KEYUP,VK_ESCAPE,1));for(int n=0;n<50&&!closed;++n)pump();CHECK(closed&&!popup.isVisible()&&opener.hasKeyboardFocus(false));}
  if(popup.isVisible())popup.close();CHECK(wait([&]{return sourceDrained(p);}));
 }
}
}
int main(int argc,char** argv){juce::ScopedJuceInitialiser_GUI gui;for(int n=1;n+1<argc;++n)if(juce::String(argv[n])=="--evidence"){evidence=juce::File(argv[n+1]);CHECK(!evidence.exists());CHECK(evidence.createDirectory().wasOk());}if(argc>1&&juce::String(argv[1])=="--lifetime"){
 input_slots_reservations_and_fresh_tickets();
#ifndef NDEBUG
 real_worker_cancel_retains_admission_until_source_and_helper_die();successful_old_completion_cannot_ready_or_keep_an_edited_tuple();three_edits_during_real_preparation_coalesce_to_the_newest_tuple();preview_stop_without_callback_retains_actual_player_capacity();actual_worker_join_leaves_processor_mailbox_and_dispatch_available();importer_teardown_preserves_message_thread_and_already_locked_callers();
#else
 CHECK(false&&"Importer lifetime checks require the assigned Debug worker hooks");
#endif
 }else{unprepared_audition_preserves_source_for_retry_and_offline_keep();chooser_explicit_crop_prepare_audition_keep();whole_source_requires_explicit_legal_crop_gain_and_kept_name();copied_readback_survives_locks_and_rejects_replaced_targets_in_every_bank();copied_readback_allocation_failure_preserves_every_output();posted_field_escape_cancels_and_returns_focus();}if(evidence!=juce::File{})CHECK(evidence.getChildFile("evidence-scope.txt").replaceWithText("Images are native popup components reading actual controller states. Held/preparing/cancel/stale images use actual helper barriers; the three-slot rejection image is a copied closed-controller readback fixture, not an opened/inspected popup. WAVs are actual processBlock output at 48000 Hz, two-channel IEEE float, before pad trim/voice/effects for canonical audition and after pad trim/voice for the kept pad. No human listening acceptance.\n"));std::cout<<"DrumImport checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}

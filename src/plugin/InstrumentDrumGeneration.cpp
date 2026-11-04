#include "InstrumentProcessor.h"
#include "DrumImport.h"
#include "ComfyDrumPreparation.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <limits>
namespace airack {
namespace {
bool validPrompt(const std::string& prompt) noexcept {
 return !prompt.empty()&&prompt.size()<=2048&&juce::CharPointer_UTF8::isValidString(prompt.data(),int(prompt.size()))&&std::none_of(prompt.begin(),prompt.end(),[](unsigned char c){return c<32||c==127;});
}
}
bool InstrumentProcessor::beginDrumGeneration(const DrumPadTarget& target,const juce::String& prompt,
    const std::array<std::uint64_t,3>& seeds,DrumCommitError& error) noexcept {
 return drumImport().beginGeneration(target,prompt,seeds,error);
}
void InstrumentProcessor::cancelDrumGeneration(std::uint64_t session) {
    if(drumImport().view().session==session)drumImport().cancel();
}
bool InstrumentProcessor::selectDrumGenerationCandidate(std::uint64_t session,std::size_t ordinal){return drumImport().selectCandidate(session,ordinal);}
bool InstrumentProcessor::drumInputAiBusyOwned() const noexcept {
 return drumOwnership->aiImagesBusy()||aiLocal||aiSourceOriginal||aiRefinementBackup||aiPhase==AiProposalPhase::pending||aiPhase==AiProposalPhase::proposed||(previewOwner==PreviewOwner::ai&&renderedPreview.retainedPcmBytes()!=0);
}
bool InstrumentProcessor::beginDrumGenerationInputs(std::array<std::uint64_t,3>& output,std::uint64_t metadata,DrumCommitError& error) noexcept {
 const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();auto& state=*drumOwnership;
 if(drumInputAiBusyOwned()||manualPreviewBytes){error=DrumCommitError::busy;return false;}
 if(std::any_of(state.preparations_.begin(),state.preparations_.end(),[](const auto& p){return p.ticket||p.inputId||p.running||p.phase!=drums::DrumOwnership::Preparation::Phase::free;})){error=DrumCommitError::preparationFull;return false;}
 if(state.nextTicket_>UINT64_MAX-3){error=DrumCommitError::authorityExhausted;return false;}
 constexpr std::uint64_t source=8ull*1024*1024,operation=16ull*1024*1024,encoded=48ull*1024*1024,prototype=393224;
 const auto fixed=3*source+operation+(state.inputPrototypeCharged_?0:prototype);
 if(metadata>UINT64_MAX-fixed||state.reservedWorkspace_>UINT64_MAX-fixed-metadata||state.reservedEncoded_>UINT64_MAX-encoded){error=DrumCommitError::workspaceBudget;return false;}
 std::array<std::uint64_t,3> ids{};
 for(std::size_t n=0;n<3;++n){auto& p=state.preparations_[n];ids[n]=p.inputId=++state.nextTicket_;p.sourceWorkspace=source;p.encodedReservation=encoded/3;p.phase=drums::DrumOwnership::Preparation::Phase::rawInput;}
 state.preparations_[0].batchWorkspace=operation+metadata;state.reservedEncoded_+=encoded;state.reservedWorkspace_+=fixed+metadata;state.inputPrototypeCharged_=true;
 output=ids;error=DrumCommitError::none;return true;
}
void InstrumentProcessor::revokeDrumInputPreparation(std::uint64_t id) noexcept {
 const std::lock_guard lock(restoreMutex);auto& state=*drumOwnership;
 for(auto& p:state.preparations_)if(id&&p.inputId==id&&!p.running&&!p.cancelled){p.ticket=0;p.asset.reset();p.phase=drums::DrumOwnership::Preparation::Phase::rawInput;break;}
 state.collect();
}
bool DrumImport::busy() const {
 const std::lock_guard lock(mutex);return pending.operation==Operation::localPreview||activeOperation==Operation::localPreview||(completion&&completion->operation==Operation::localPreview)||inputId||std::any_of(generationInputs.begin(),generationInputs.end(),[](auto id){return id!=0;})||(shown.phase!=DrumImportPhase::closed&&shown.phase!=DrumImportPhase::kept);
}
bool DrumImport::openGeneration(const DrumPadTarget& target){
 {const std::lock_guard lock(processor.restoreMutex);if(processor.drumInputAiBusyOwned()||processor.manualPreviewBytes)return false;}
 {const std::lock_guard lock(mutex);if(!accepting)return false;if(shown.generation&&shown.phase==DrumImportPhase::choosing&&shown.target==target)return true;
  if(inputId||shown.phase!=DrumImportPhase::closed&&shown.phase!=DrumImportPhase::kept){shown.message="Busy: finish or cancel the current source session";return false;}if(nextSession==UINT64_MAX)return false;}
 DrumPadTarget fresh;DrumCommitError error;if(!processor.captureDrumPadTarget(target.rack,target.unit,target.pad,fresh,error)||fresh!=target)return false;
 DrumImportView next;next.target=target;next.generation=true;next.phase=DrumImportPhase::choosing;next.draft=1;next.message="Local generator: enter 1 to 2048 UTF-8 bytes; configured recipe / execution identity unverified";
 const std::lock_guard admission(processor.restoreMutex);if(processor.drumInputAiBusyOwned()||processor.manualPreviewBytes)return false;
 const std::lock_guard lock(mutex);if(!accepting||inputId||shown.phase!=DrumImportPhase::closed&&shown.phase!=DrumImportPhase::kept)return false;next.session=++nextSession;shown=std::move(next);cancelled=false;return true;
}
bool DrumImport::beginGeneration(const DrumPadTarget& target,const juce::String& prompt,const std::array<std::uint64_t,3>& seeds,DrumCommitError& error) noexcept {
 try{
  const auto promptBytes=prompt.getNumBytesAsUTF8();if(!promptBytes||promptBytes>2048){error=DrumCommitError::invalid;return false;}
  const auto text=prompt.toStdString();
  if(!validPrompt(text)||std::any_of(seeds.begin(),seeds.end(),[](auto seed){return seed>std::uint64_t(INT64_MAX);})||seeds[0]==seeds[1]||seeds[0]==seeds[2]||seeds[1]==seeds[2]){error=DrumCommitError::invalid;return false;}
  std::uint64_t session=0,request=0;bool existing=false;
  {const std::lock_guard lock(mutex);existing=shown.generation&&shown.phase==DrumImportPhase::choosing&&shown.target==target;
   if(!accepting||inputId||(!existing&&shown.phase!=DrumImportPhase::closed&&shown.phase!=DrumImportPhase::kept)){error=DrumCommitError::busy;return false;}
   if(nextGeneration==UINT64_MAX||(!existing&&nextSession==UINT64_MAX)){error=DrumCommitError::authorityExhausted;return false;}session=existing?shown.session:nextSession+1;request=nextGeneration+1;}
  DrumPadTarget fresh;if(!processor.captureDrumPadTarget(target.rack,target.unit,target.pad,fresh,error)||fresh!=target){error=DrumCommitError::staleTarget;return false;}
  Command command;command.operation=Operation::generate;command.session=session;command.draft=1;command.target=target;command.generation.emplace();auto& generation=*command.generation;
  generation.runtime=generationRuntime;generation.requestUuid=juce::Uuid();generation.request=request;generation.prompt=text;generation.seeds=seeds;
  for(std::size_t n=0;n<generation.attempts.size();++n)do{generation.attempts[n]=juce::Uuid();}while(generation.attempts[n].isNull()||std::find(generation.attempts.begin(),generation.attempts.begin()+std::ptrdiff_t(n),generation.attempts[n])!=generation.attempts.begin()+std::ptrdiff_t(n));
  generation.deadline=drums::GenerationClock::now()+std::chrono::seconds(300);
  command.file=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("AI Rack Synth drum inputs").getChildFile(generationRuntime.toDashedString()).getChildFile(generation.requestUuid.toDashedString());
  if(command.file.getFullPathName().getNumBytesAsUTF8()>4096){error=DrumCommitError::invalid;return false;}
  DrumImportView next;next.target=target;next.session=session;next.draft=1;next.generation=true;next.phase=DrumImportPhase::generating;next.message="Generating three candidates; configured recipe / execution identity unverified";for(std::size_t n=0;n<3;++n)next.candidates[n].seed=seeds[n];
  constexpr std::uint64_t metadata=sizeof(Command)+sizeof(Completion)+2*sizeof(DrumImportView)+sizeof(drums::ComfyAudioBatch)+3*65536+16384+2048+4096;
  if(!processor.beginDrumGenerationInputs(command.inputs,metadata,error))return false;command.input=command.inputs[0];
  {const std::lock_guard lock(mutex);nextSession=session;nextGeneration=request;shown=std::move(next);generationInputs=command.inputs;inputId=command.input;generationDeadline=generation.deadline;preparedId=preparedDraft=0;preparedMetadata={};completion.reset();cancelled=false;pending=std::move(command);}
  wake.notify_all();error=DrumCommitError::none;return true;
 }catch(const std::bad_alloc&){error=DrumCommitError::allocationFailed;return false;}
}
bool DrumImport::commandCurrent(const Command& command,bool deadline) noexcept {
 DrumPadTarget target;
 {const std::lock_guard lock(mutex);if(!accepting||cancelled.load()||shown.session!=command.session||shown.draft!=command.draft||shown.phase==DrumImportPhase::cancelling||shown.phase==DrumImportPhase::kept)return false;target=shown.target;}
 if(deadline&&command.generation&&drums::GenerationClock::now()>=command.generation->deadline)return false;
 DrumPadTarget fresh;DrumCommitError error;if(!processor.captureDrumPadTarget(target.rack,target.unit,target.pad,fresh,error)||fresh!=target)return false;
 const std::lock_guard lock(mutex);return accepting&&!cancelled.load()&&shown.session==command.session&&shown.draft==command.draft&&shown.target==fresh&&shown.phase!=DrumImportPhase::cancelling&&shown.phase!=DrumImportPhase::kept;
}
void DrumImport::runGeneration(Command& command,Completion& result,std::array<std::unique_ptr<drums::DrumSource>,3>& sources,std::array<std::uint64_t,3>& held,std::uint64_t& session){
 held=command.inputs;session=command.session;result.generationStatus=drums::DrumGenerationStatus::unavailable;
 if(!commandCurrent(command,true)){result.generationStatus=cancelled.load()?drums::DrumGenerationStatus::cancelled:drums::DrumGenerationStatus::stale;return;}
 drums::ComfyAudioBatch batch;
 try{
  if(command.file.createDirectory().failed()){result.generationStatus=drums::DrumGenerationStatus::storageFailure;return;}
  std::unique_ptr<drums::ComfyAudioJob> job;
#ifndef NDEBUG
  testPoint(TestPoint::beforeGeneration);if(testMakeJob)job=testMakeJob(testGenerationContext,command.file,*command.generation);else
#endif
  job=std::make_unique<drums::ComfyAudioJob>(companion::ComfyLoopback::ipv4,command.file);
  if(!job){result.generationStatus=drums::DrumGenerationStatus::allocationFailed;return;}
  {const std::lock_guard lock(mutex);activeJob=job.get();if(cancelled.load())activeJob->cancel();}
  struct Admission {DrumImport* self;Command* command;} admission{this,&command};
  const drums::ComfyJobAdmission current{[](void* raw,drums::ComfyAuthorityBoundary boundary,std::size_t ordinal) noexcept{
   const auto& a=*static_cast<Admission*>(raw);if(!a.self->commandCurrent(*a.command,true))return false;
   const std::lock_guard lock(a.self->mutex);if(a.self->shown.phase==DrumImportPhase::generating){a.self->shown.generationOrdinal=ordinal;a.self->shown.generationStage=boundary==drums::ComfyAuthorityBoundary::submit?DrumGenerationStage::submitting:boundary==drums::ComfyAuthorityBoundary::history?DrumGenerationStage::polling:DrumGenerationStage::downloading;}return true;
  },&admission};
  try{batch=job->run(*command.generation,current);}catch(...){const std::lock_guard lock(mutex);activeJob=nullptr;throw;}
  {const std::lock_guard lock(mutex);activeJob=nullptr;}job.reset();
#ifndef NDEBUG
  testPoint(TestPoint::afterGenerationDrain);
#endif
  result.generationStatus=batch.status;
  const auto stopped=[&]{return cancelled.load()?drums::DrumGenerationStatus::cancelled:drums::GenerationClock::now()>=command.generation->deadline?drums::DrumGenerationStatus::timeout:drums::DrumGenerationStatus::stale;};
  if(!commandCurrent(command,true)){result.generationStatus=stopped();return;}
  const auto promptHash=juce::SHA256(command.generation->prompt.data(),command.generation->prompt.size()).getRawData();std::size_t available=0;bool allocationFailed=false;
  for(std::size_t n=0;n<3;++n){auto& copy=result.candidates[n];auto& attempt=batch.attempts[n];copy.seed=command.generation->seeds[n];copy.status=attempt.status;copy.encodedBytes=attempt.encodedBytes;for(std::size_t b=0;b<32;++b)copy.fileId[b]=std::to_integer<std::uint8_t>(attempt.fileSha256[b]);if(!attempt.raw)continue;
   if(!commandCurrent(command,true)){result.generationStatus=stopped();return;}
   {const std::lock_guard lock(mutex);if(shown.phase==DrumImportPhase::generating){shown.generationStage=DrumGenerationStage::inspecting;shown.generationOrdinal=n;}}
   try{
   drums::GeneratedOrigin origin;origin.expectedFileId=copy.fileId;std::copy(promptHash.begin(),promptHash.end(),origin.promptId.begin());origin.generator="Stable Audio 3 Small SFX";origin.condition="stable-audio3-small-sfx-lcm8-flac-v1;execution=unverified";origin.seed=copy.seed;
#ifndef NDEBUG
   testPoint(TestPoint::beforeCandidateInspect);
#endif
   if(drums::inspectComfyDrumSource(attempt.raw,origin,cancelled,sources[n],result.preparationError)&&commandCurrent(command,true)){copy.source=sources[n]->info();copy.available=true;if(!available){result.ordinal=n;result.input=held[n];result.source=copy.source;}++available;}else{allocationFailed|=result.preparationError==drums::PreparationError::allocationFailed;copy.status=result.preparationError==drums::PreparationError::allocationFailed?drums::ComfyAttemptStatus::allocationFailed:drums::ComfyAttemptStatus::invalidAudio;sources[n].reset();}
   }catch(const std::bad_alloc&){allocationFailed=true;copy.status=drums::ComfyAttemptStatus::allocationFailed;sources[n].reset();}
  }
  if(!commandCurrent(command,true)){result.generationStatus=stopped();return;}
  result.success=available!=0;result.generationStatus=available==3?drums::DrumGenerationStatus::complete:available?drums::DrumGenerationStatus::partial:allocationFailed?drums::DrumGenerationStatus::allocationFailed:batch.rawCount?drums::DrumGenerationStatus::invalidAudio:batch.status;
 }catch(const std::bad_alloc&){result.generationStatus=drums::DrumGenerationStatus::allocationFailed;}
 catch(...){result.generationStatus=drums::DrumGenerationStatus::invalidResponse;}
}
bool DrumImport::selectCandidate(std::uint64_t session,std::size_t ordinal){
 Command command;
 {const std::lock_guard lock(mutex);if(!accepting||!shown.generation||session!=shown.session||ordinal>=3||!shown.candidates[ordinal].available||shown.phase==DrumImportPhase::generating||shown.phase==DrumImportPhase::cancelling||shown.phase==DrumImportPhase::kept||shown.draft==UINT64_MAX)return false;
  cancelled=true;++shown.draft;shown.selectedCandidate=ordinal;inputId=generationInputs[ordinal];preparedId=preparedDraft=0;previewRequested=false;shown.inspected=false;shown.canPrepare=shown.canAudition=shown.canKeep=false;shown.preparedFrames=0;shown.phase=DrumImportPhase::inspecting;shown.message="Selecting owned candidate; previous audition stopping";
  command.operation=Operation::select;command.session=session;command.draft=shown.draft;command.input=inputId;command.inputs=generationInputs;command.ordinal=ordinal;command.target=shown.target;pending=std::move(command);}
 processor.stopManualDrumPreview();wake.notify_all();return true;
}
}

#include "DrumImport.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace airack {
namespace {
bool legalName(const juce::String& value){const auto s=value.toStdString();return !s.empty()&&s.size()<=64&&std::none_of(s.begin(),s.end(),[](unsigned char c){return c<32||c==127;})&&juce::CharPointer_UTF8::isValidString(s.data(),int(s.size()));}
bool legalTuple(const DrumImportView& v){return v.inspected&&v.source.rate&&v.begin<v.end&&v.end<=v.source.frames&&v.end-v.begin<=std::uint64_t(v.source.rate)*10&&std::isfinite(v.gain)&&v.gain>0&&v.gain<=16;}
juce::String failure(DrumCommitError e){switch(e){case DrumCommitError::locked:return "Captured pad is locked";case DrumCommitError::staleTarget:return "Captured pad changed; cancel and select its source again";case DrumCommitError::preparationFull:return "All three preparation slots are occupied or draining";case DrumCommitError::decodedBudget:return "Retained decoded audio budget is full";case DrumCommitError::workspaceBudget:return "Source preparation workspace admission failed";case DrumCommitError::allocationFailed:return "Source preparation allocation failed";case DrumCommitError::audioNotReady:return "Audio playback is not ready. Check audio settings, then try Audition again.";case DrumCommitError::busy:return "Waiting for preview acknowledgement";default:return "Source operation rejected; kit preserved";}}
juce::String failure(drums::PreparationError e){switch(e){case drums::PreparationError::gainRequired:return "Prepared peak exceeds 1; choose a lower gain multiplier";case drums::PreparationError::cancelled:return "Source work cancelled";case drums::PreparationError::invalidSelection:return "Choose a legal explicit source-frame selection of at most ten seconds";case drums::PreparationError::invalidGain:return "Choose a finite gain multiplier greater than zero and at most sixteen";case drums::PreparationError::nonFinite:return "Source contains non-finite audio";case drums::PreparationError::unsupportedFormat:return "Unsupported source format; choose WAV, AIFF or FLAC";case drums::PreparationError::sourceBusy:return "Source is busy";default:return "Source inspection or preparation failed; kit preserved";}}
void readiness(DrumImportView& v){v.canPrepare=legalTuple(v)&&v.phase!=DrumImportPhase::preparing&&v.phase!=DrumImportPhase::cancelling;}
}
DrumImport::DrumImport(InstrumentProcessor& p):processor(p),worker([this](std::stop_token stop){run(stop);}){}
DrumImport::~DrumImport(){closeAdmission();worker.request_stop();wake.notify_all();
#ifndef NDEBUG
 testPoint(TestPoint::beforeJoin);
#endif
 worker.join();}
bool DrumImport::open(const DrumPadTarget& target){
 {const std::lock_guard lock(mutex);if(!accepting)return false;if(inputId||shown.phase!=DrumImportPhase::closed&&shown.phase!=DrumImportPhase::kept){shown.message="Finish or cancel PAD "+juce::String(int(shown.target.pad)+1).paddedLeft('0',2)+" source selection first";return false;}if(nextSession==UINT64_MAX)return false;}
 // This is independent edit admission; performance/view authority cannot open an importer.
 DrumPadTarget checked;DrumCommitError error;if(!processor.captureDrumPadTarget(target.rack,target.unit,target.pad,checked,error)||checked!=target)return false;
 std::uint64_t id=0;if(!processor.beginDrumInput(id,error)){const std::lock_guard lock(mutex);shown.message=failure(error);return false;}
 const std::lock_guard lock(mutex);shown={};shown.target=target;shown.session=++nextSession;shown.draft=1;shown.phase=DrumImportPhase::choosing;shown.message="Choose a WAV, AIFF or FLAC source";inputId=id;preparedId=preparedDraft=0;preparedMetadata={};completion.reset();cancelled=false;return true;
}
void DrumImport::enqueue(Command command){{const std::lock_guard lock(mutex);pending=std::move(command);}wake.notify_all();}
void DrumImport::choose(const juce::File& file){
 Command command;{const std::lock_guard lock(mutex);if(!accepting||!inputId||shown.phase!=DrumImportPhase::choosing)return;
  shown.sourceName=file.getFileName();const auto basename=file.getFileNameWithoutExtension();shown.keptName=legalName(basename)?basename:juce::String{};
  shown.phase=DrumImportPhase::inspecting;shown.message="Inspecting the whole source";command={Operation::inspect,shown.session,shown.draft,inputId,0,0,1,file};}
 enqueue(std::move(command));
}
void DrumImport::edit(std::uint64_t begin,std::uint64_t end,float gain,const juce::String& name){
 bool changed=false;{const std::lock_guard lock(mutex);if(!shown.inspected||!inputId||shown.phase==DrumImportPhase::cancelling)return;
  if(shown.begin==begin&&shown.end==end&&shown.gain==gain&&shown.keptName==name)return;
  if(shown.draft==UINT64_MAX){shown.message="Source draft authority exhausted; cancel this session";return;}
  const bool preparing=shown.phase==DrumImportPhase::preparing||activeOperation==Operation::prepare||pending.operation==Operation::prepare;cancelled=true;++shown.draft;shown.begin=begin;shown.end=end;shown.gain=gain;shown.keptName=shown.generation&&name.getNumBytesAsUTF8()>64?juce::String{}:name;
  shown.phase=DrumImportPhase::editing;shown.canAudition=shown.canKeep=false;shown.preparedFrames=0;shown.preparedPeak=0;previewRequested=false;readiness(shown);
  shown.message=!legalTuple(shown)?"Choose at most "+juce::String(std::uint64_t(shown.source.rate)*10)+" source frames, with valid START, END and gain":!legalName(name)?"Enter a legal kept name of at most 64 UTF-8 bytes":"Selection changed; prepare this exact source selection";
  if(preparing){if(shown.canPrepare)pending={Operation::prepare,shown.session,shown.draft,inputId,begin,end,gain,{}};else if(pending.operation==Operation::prepare)pending={};}changed=true;}
 if(changed){processor.stopManualDrumPreview();wake.notify_all();}
}
void DrumImport::prepare(){
 Command command;{const std::lock_guard lock(mutex);readiness(shown);if(!accepting||!inputId||!shown.canPrepare)return;
  cancelled=true;shown.phase=DrumImportPhase::preparing;shown.canPrepare=shown.canAudition=shown.canKeep=false;shown.preparedFrames=0;shown.message="Preparing the explicit source selection";previewRequested=false;
  command={Operation::prepare,shown.session,shown.draft,inputId,shown.begin,shown.end,shown.gain,{}};}
 processor.stopManualDrumPreview();enqueue(std::move(command));
}
DrumImportView DrumImport::view()const{const std::lock_guard lock(mutex);return shown;}
bool DrumImport::current(const DrumPadTarget& target,std::uint64_t ticket)const{const std::lock_guard lock(mutex);return accepting&&inputId&&preparedId==ticket&&ticket&&preparedDraft==shown.draft&&shown.target==target&&shown.inspected&&(shown.phase==DrumImportPhase::ready||shown.phase==DrumImportPhase::waitingPreview||shown.phase==DrumImportPhase::auditioning);}
void DrumImport::audition(){
 if(processor.manualDrumPreviewPlaying()){stopPreview();return;}
 {const std::lock_guard lock(mutex);if(!shown.canAudition)return;previewRequested=true;shown.phase=DrumImportPhase::waitingPreview;shown.message="Waiting for prepared source audition";}poll();
}
void DrumImport::stopPreview(){processor.stopManualDrumPreview();const std::lock_guard lock(mutex);previewRequested=false;shown.message="Stopping prepared source audition";}
bool DrumImport::keep(){
 DrumPadTarget target;std::uint64_t ticket=0,id=0,session=0,draft=0;drums::DrumPad metadata;
 {const std::lock_guard lock(mutex);if(!shown.canKeep||preparedDraft!=shown.draft||!legalName(shown.keptName))return false;target=shown.target;ticket=preparedId;metadata=preparedMetadata;metadata.name=shown.keptName.toStdString();id=inputId;session=shown.session;draft=shown.draft;}
 if(!current(target,ticket))return false;
 DrumCommitError error;if(!processor.assignPreparedDrumPad(target,ticket,metadata,{},error)){const std::lock_guard lock(mutex);shown.message=failure(error);return false;}
 processor.stopManualDrumPreview();{const std::lock_guard lock(mutex);shown.phase=DrumImportPhase::kept;shown.canPrepare=shown.canAudition=shown.canKeep=false;shown.message="Kept PAD "+juce::String(int(target.pad)+1).paddedLeft('0',2);preparedId=0;previewRequested=false;cancelled=true;}
 Command retirement;retirement.operation=Operation::cancel;retirement.session=session;retirement.draft=draft;retirement.input=id;
 {const std::lock_guard lock(mutex);retirement.inputs=generationInputs;generationDeadline={};for(auto& candidate:shown.candidates)candidate.available=false;}
 for(auto input:retirement.inputs)if(input)processor.cancelDrumInput(input);enqueue(std::move(retirement));return true;
}
void DrumImport::cancel(){
 Command command;
 {const std::lock_guard lock(mutex);cancelled=true;previewRequested=false;command.operation=Operation::cancel;command.input=inputId;command.session=shown.session;command.draft=shown.draft;command.inputs=generationInputs;
  shown.canPrepare=shown.canAudition=shown.canKeep=false;shown.phase=inputId?DrumImportPhase::cancelling:DrumImportPhase::closed;shown.message=shown.generation?"Canceled; the service may finish its work":"Source selection cancelled; kit preserved";preparedId=0;generationDeadline={};for(auto& candidate:shown.candidates){candidate.available=false;if(shown.generation)candidate.status=drums::ComfyAttemptStatus::cancelled;}
  if(activeJob)activeJob->cancel();}
 processor.stopManualDrumPreview();if(command.input){processor.cancelDrumInput(command.input);for(auto id:command.inputs)if(id)processor.cancelDrumInput(id);enqueue(std::move(command));}
}
void DrumImport::closeAdmission(){{const std::lock_guard lock(mutex);accepting=false;}cancel();}
void DrumImport::poll(){
 std::optional<Completion> completed;{const std::lock_guard lock(mutex);completed=std::move(completion);completion.reset();}
 if(completed&&completed->operation==Operation::localPreview){processor.finishAiLocalWork(std::move(completed->local));completed.reset();}
 if(completed){auto& c=*completed;const std::lock_guard lock(mutex);
  if(c.session==shown.session&&(c.input==inputId||c.operation==Operation::generate||c.operation==Operation::cancel)){
   if(c.operation==Operation::cancel){inputId=0;generationInputs={};preparedId=preparedDraft=0;preparedMetadata={};if(shown.phase!=DrumImportPhase::kept){shown.phase=DrumImportPhase::closed;if(shown.generation&&shown.generationStatus==drums::DrumGenerationStatus::stale)shown.message="Captured pad changed or locked; generated result discarded; the service may finish its work";else if(shown.generation&&shown.generationStatus==drums::DrumGenerationStatus::timeout)shown.message="Timed out; local result discarded; the service may finish its work";}}
   else if(c.operation==Operation::generate&&c.draft==shown.draft&&shown.phase==DrumImportPhase::generating){
    // Admit a queued success before clearing its deadline; failed terminals keep their status.
    if(!c.success||generationDeadline==drums::GenerationClock::time_point{}||drums::GenerationClock::now()<generationDeadline){
    shown.generationStatus=c.generationStatus;shown.generationStage=DrumGenerationStage::finished;shown.candidates=std::move(c.candidates);generationDeadline={};
    if(c.success){inputId=c.input;shown.selectedCandidate=c.ordinal;shown.source=c.source;shown.inspected=true;shown.begin=0;shown.end=c.source.frames;shown.gain=1;shown.keptName="Generated candidate "+juce::String(int(c.ordinal)+1);shown.sourceName="Candidate "+juce::String(int(c.ordinal)+1)+" / seed "+juce::String(shown.candidates[c.ordinal].seed);shown.phase=DrumImportPhase::editing;readiness(shown);shown.message=(c.generationStatus==drums::DrumGenerationStatus::partial?"Partial batch. ":"")+juce::String(legalTuple(shown)?"Choose source frames and gain, then Prepare":"Full source exceeds ten seconds; choose explicit source frames and gain");}
    else{inputId=0;generationInputs={};shown.inspected=false;shown.canPrepare=shown.canAudition=shown.canKeep=false;shown.phase=DrumImportPhase::error;
     switch(c.generationStatus){case drums::DrumGenerationStatus::recipeRejected:shown.message="Generation rejected by the service";break;case drums::DrumGenerationStatus::timeout:shown.message="Timed out; service execution identity remains unverified";break;case drums::DrumGenerationStatus::cancelled:shown.message="Canceled; the service may finish its work";break;case drums::DrumGenerationStatus::stale:shown.message="Captured pad changed; generated result discarded";break;case drums::DrumGenerationStatus::invalidAudio:shown.message="Generated source invalid";break;case drums::DrumGenerationStatus::submissionAmbiguous:shown.message="Submission outcome unknown; no retry was sent";break;default:shown.message="Local generator unavailable or response invalid; kit preserved";break;}
    }
    }
   }
   else if(c.draft==shown.draft&&shown.phase!=DrumImportPhase::cancelling&&shown.phase!=DrumImportPhase::kept){
    if((c.operation==Operation::inspect||c.operation==Operation::select)&&c.success){if(c.operation==Operation::select){shown.keptName="Generated candidate "+juce::String(int(c.ordinal)+1);shown.sourceName="Candidate "+juce::String(int(c.ordinal)+1)+" / seed "+juce::String(shown.candidates[c.ordinal].seed);}shown.source=c.source;shown.inspected=true;shown.begin=0;shown.end=c.source.frames;shown.gain=1;shown.phase=DrumImportPhase::editing;readiness(shown);shown.message=!legalTuple(shown)?"Choose at most "+juce::String(std::uint64_t(c.source.rate)*10)+" source frames":shown.keptName.isEmpty()?"Enter a legal kept name of at most 64 UTF-8 bytes":"Choose source frames and gain, then Prepare";}
    else if(c.operation==Operation::prepare&&c.success){preparedId=c.ticket;preparedDraft=c.draft;preparedMetadata=std::move(c.metadata);shown.phase=DrumImportPhase::ready;shown.preparedFrames=c.frames;shown.preparedPeak=c.peak;shown.canAudition=true;shown.canKeep=legalName(shown.keptName);readiness(shown);shown.message=shown.canKeep?"Prepared source before pad trim/voice/effects":"Prepared source; enter a legal kept name of 1 to 64 UTF-8 bytes before Keep";}
    else {shown.phase=DrumImportPhase::error;shown.canAudition=shown.canKeep=false;shown.preparedFrames=0;readiness(shown);shown.message=c.commitError!=DrumCommitError::none?failure(c.commitError):failure(c.preparationError);}
   }
  }
 }
 bool expired=false,generation=false;{const std::lock_guard lock(mutex);generation=shown.generation;expired=generationDeadline!=drums::GenerationClock::time_point{}&&drums::GenerationClock::now()>=generationDeadline;}
 if(expired){cancel();const std::lock_guard lock(mutex);shown.generationStatus=drums::DrumGenerationStatus::timeout;shown.message="Timed out; waiting for owned work to drain";return;}
 DrumPadTarget target;std::uint64_t ticket=0,session=0,draft=0;bool request=false,active=false;
 {const std::lock_guard lock(mutex);request=previewRequested;target=shown.target;ticket=preparedId;session=shown.session;draft=shown.draft;active=inputId&&shown.phase!=DrumImportPhase::choosing&&shown.phase!=DrumImportPhase::cancelling&&shown.phase!=DrumImportPhase::kept;}
 if(active&&generation){DrumPadTarget fresh;DrumCommitError error;if(!processor.captureDrumPadTarget(target.rack,target.unit,target.pad,fresh,error)||fresh!=target){cancel();const std::lock_guard lock(mutex);shown.generationStatus=drums::DrumGenerationStatus::stale;shown.message="Captured pad changed or locked; owned work draining";return;}}
 if(active){DrumPadReadback readback;DrumCommitError error;if(!processor.readDrumPad(target,readback,error)&&error==DrumCommitError::staleTarget){cancel();return;}}
 if(request){DrumCommitError error;const bool started=processor.startManualDrumPreview(target,ticket,processor.editRevision.load(),error);const std::lock_guard lock(mutex);
  if(shown.session==session&&shown.draft==draft&&previewRequested){if(started){previewRequested=false;shown.phase=DrumImportPhase::auditioning;shown.message="Auditioning prepared source before pad trim/voice/effects";}else if(error==DrumCommitError::audioNotReady){previewRequested=false;shown.phase=DrumImportPhase::ready;shown.message=failure(error);}else if(error!=DrumCommitError::busy){previewRequested=false;shown.phase=DrumImportPhase::error;shown.message=failure(error);shown.canAudition=shown.canKeep=false;}}}
 else if(!processor.manualDrumPreviewPlaying()){const std::lock_guard lock(mutex);if(shown.phase==DrumImportPhase::auditioning||shown.phase==DrumImportPhase::waitingPreview){shown.phase=DrumImportPhase::ready;shown.message="Prepared source audition stopped";}}
}
void DrumImport::run(std::stop_token stop){
 std::array<std::unique_ptr<drums::DrumSource>,3> sources;
 std::array<std::uint64_t,3> heldInputs{};std::uint64_t sourceSession=0;
 const auto release=[&]{for(auto& source:sources)source.reset();for(auto id:heldInputs)if(id)processor.releaseDrumInputWork(id);heldInputs={};sourceSession=0;};
 for(;;){Command command;
  {std::unique_lock lock(mutex);wake.wait(lock,stop,[&]{return pending.operation!=Operation::none;});if(stop.stop_requested()){const auto pendingInput=pending.input;const auto pendingInputs=pending.inputs;pending={};lock.unlock();release();if(pendingInput)processor.releaseDrumInputWork(pendingInput);for(auto id:pendingInputs)if(id)processor.releaseDrumInputWork(id);return;}command=std::move(pending);pending={};activeOperation=command.operation;if(command.operation!=Operation::localPreview)cancelled=false;}
  Completion result;result.operation=command.operation;result.session=command.session;result.draft=command.draft;result.input=command.input;
  if(command.operation==Operation::cancel){if(sourceSession==command.session)release();if(command.input)processor.releaseDrumInputWork(command.input);for(auto id:command.inputs)if(id)processor.releaseDrumInputWork(id);result.success=true;}
  else if(command.operation==Operation::localPreview){
   result.local=std::move(command.local);
#ifndef NDEBUG
   testPoint(TestPoint::beforeLocalRender);
#endif
   if(result.local)InstrumentProcessor::renderAiLocalWork(*result.local);
#ifndef NDEBUG
   testPoint(TestPoint::afterLocalRender);
#endif
  }
  else if(command.operation==Operation::generate){
   release();runGeneration(command,result,sources,heldInputs,sourceSession);
   if(!result.success){command.generation.reset();command.file={};release();for(auto& candidate:result.candidates)candidate.available=false;}
  }
  else if(command.operation==Operation::inspect){
   release();heldInputs[0]=command.input;sourceSession=command.session;
#ifndef NDEBUG
   testPoint(TestPoint::beforeInspect);
#endif
   result.success=drums::inspectDrumSource(command.file,{},cancelled,sources[0],result.preparationError);
   if(result.success)result.source=sources[0]->info();else release();
  }
  else if(command.operation==Operation::select){
   for(auto id:heldInputs)if(id)processor.revokeDrumInputPreparation(id);
   if(sourceSession==command.session&&command.ordinal<3&&heldInputs[command.ordinal]==command.input&&sources[command.ordinal]&&commandCurrent(command)){
    result.success=true;result.source=sources[command.ordinal]->info();result.ordinal=command.ordinal;
   }else result.commitError=DrumCommitError::cancelled;
  }
  else if(command.operation==Operation::prepare){
   const auto found=std::find(heldInputs.begin(),heldInputs.end(),command.input);const auto ordinal=std::size_t(found-heldInputs.begin());
   if(found==heldInputs.end()||!sources[ordinal]||sourceSession!=command.session){result.commitError=DrumCommitError::cancelled;}
   else {auto& source=*sources[ordinal];const auto& info=source.info();const auto span=command.end-command.begin;
    if(command.begin>=command.end||command.end>info.frames||span>std::uint64_t(info.rate)*10||!std::isfinite(command.gain)||command.gain<=0||command.gain>16)result.preparationError=drums::PreparationError::invalidSelection;
    else {const auto frames=std::uint32_t((span*48000+info.rate-1)/info.rate);
     if(commandCurrent(command)&&processor.reserveDrumInputPreparation(command.input,frames,info.channels,result.commitError)){
#ifndef NDEBUG
      testPoint(TestPoint::beforePrepare);
#endif
      {drums::PreparedDrumSample prepared;result.success=drums::prepareDrumSample(source,command.begin,command.end,command.gain,cancelled,prepared,result.preparationError);
#ifndef NDEBUG
       if(result.success&&prepared.asset)testPoint(TestPoint::afterHelper);
#endif
       if(result.success&&commandCurrent(command)){result.success=processor.finishDrumInput(command.input,prepared.asset->samples(),prepared.asset->channels(),prepared.asset->id(),result.ticket,result.commitError);if(result.success){result.metadata.asset=prepared.asset->id();result.metadata.source=std::move(prepared.source);result.frames=prepared.asset->frames();result.peak=prepared.peak;}}
       else {result.success=false;if(result.preparationError==drums::PreparationError::none)result.preparationError=drums::PreparationError::cancelled;}
      }
      processor.releaseDrumInputPreparationWork(command.input);
     }
    }
   }
  }
#ifndef NDEBUG
  testPoint(TestPoint::beforeCompletion);
#endif
  {const std::lock_guard lock(mutex);activeOperation=Operation::none;completion=std::move(result);}
 }
}
}

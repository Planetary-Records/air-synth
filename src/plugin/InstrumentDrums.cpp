#include "InstrumentProcessor.h"
#include "DrumImport.h"
#include "DrumBundle.h"
#include "FactorySounds.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <climits>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace airack
{
namespace {
bool rejected(DrumCommitError value,DrumCommitError& error) noexcept {error=value;return false;}
std::uint64_t padMetadataUnits(const drums::DrumPad& pad) noexcept {
    std::uint64_t bytes=pad.name.capacity()+1;
    if(pad.source)bytes+=pad.source->generator.capacity()+1+pad.source->condition.capacity()+1;
    return bytes;
}
template<class Images> std::uint64_t imageMetadataUnits(const Images& images) noexcept {
    std::uint64_t bytes=images.capacity()*sizeof(typename Images::value_type);
    for(const auto& image:images)bytes+=padMetadataUnits(image.metadata);
    return bytes;
}
std::uint64_t bankPads(unsigned bank) noexcept {return std::uint64_t(0xffff)<<(16*bank);}
bool hasMetadata(const drums::DrumPad& pad) noexcept {return pad.asset.has_value()||!pad.name.empty()||pad.source.has_value();}
bool validBeforeRecovery(ProjectDocument& document) {
    // Personality allocation failures must escape before choosing legacy recovery.
    auto personalities=std::move(document.sound.personalities);
    const bool numeric=validateProjectDocument(document);
    document.sound.personalities=std::move(personalities);
    return numeric&&validateRackInstances(document.sound);
}
}
struct InstrumentProcessor::DrumPadAction final : juce::UndoableAction {
    InstrumentProcessor& processor;
    DrumPadTarget target;
    OwnedPadImage before,after;
    std::array<PadNumericDelta,9> numeric {};
    std::uint8_t numericCount=0;
    int cachedUnits=0;
    DrumPadAction(InstrumentProcessor& p,const DrumPadTarget& t,const OwnedPadImage& a,const OwnedPadImage& b)
        :processor(p),target(t),before(a),after(b){}
    bool perform() override {return processor.replayOwnedPad(*this,true,processor.drumCommitStatus);}
    bool undo() override {return processor.replayOwnedPad(*this,false,processor.drumCommitStatus);}
    int getSizeInUnits() override {return cachedUnits;}
};
struct InstrumentProcessor::OwnedDocumentAction final : juce::UndoableAction {
    InstrumentProcessor& processor;
    const ProjectDocument before,after;
    const ChangedProjectFields fields;
    const OwnedAssignmentScope assignments;
    const OwnedDocumentKind kind;
    const bool reset;
    int cachedUnits=0;
    OwnedDocumentAction(InstrumentProcessor& p,const ProjectDocument& a,const ProjectDocument& b,
        ChangedProjectFields f,OwnedAssignmentScope scope,OwnedDocumentKind k,bool r)
        :processor(p),before(a),after(b),fields(std::move(f)),assignments(std::move(scope)),kind(k),reset(r){}
    bool perform() override {return processor.replayOwnedDocument(*this,true,processor.drumCommitStatus);}
    bool undo() override {return processor.replayOwnedDocument(*this,false,processor.drumCommitStatus);}
    int getSizeInUnits() override {return cachedUnits;}
};
#ifndef NDEBUG
void InstrumentProcessor::drumHistoryDebugSnapshot(DrumHistoryDebugView& output) const noexcept
{
    output.count=editHistory->count;output.cursor=editHistory->cursor;output.storedUnits=editHistory->storedUnits;
    output.identities.fill(nullptr);output.units.fill(0);output.kinds.fill(0);output.rootCounts.fill(0);output.masks.fill(0);
    for(auto& row:output.roots)row.fill(nullptr);for(auto& row:output.assetIds)for(auto& id:row)id.fill(0);
    for(auto& row:output.chargeTerms)row.fill(0);
    for(std::size_t n=0;n<editHistory->count;++n){
        const auto* action=editHistory->actions[n].get();output.identities[n]=action;output.units[n]=editHistory->units[n];
        const auto add=[&](const drums::AssetPtr& owner){if(owner){auto& count=output.rootCounts[n];
            output.roots[n][count]=owner.get();output.assetIds[n][count]=owner->id();++count;}};
        if(const auto* pad=dynamic_cast<const DrumPadAction*>(action)){
            output.kinds[n]=1;output.masks[n]=std::uint64_t(1)<<(pad->target.bank*16+pad->target.pad);
            add(pad->before.owner);add(pad->after.owner);
            output.chargeTerms[n]={sizeof(DrumPadAction),padMetadataUnits(pad->before.metadata),padMetadataUnits(pad->after.metadata),0,0,0,0,0};
        }
        if(const auto* document=dynamic_cast<const OwnedDocumentAction*>(action)){
            output.kinds[n]=document->kind==OwnedDocumentKind::rack?2:3;output.masks[n]=document->assignments.pads;
            for(const auto& image:document->assignments.before)add(image.owner);for(const auto& image:document->assignments.after)add(image.owner);
            output.chargeTerms[n]={sizeof(OwnedDocumentAction),historyPersonalityBytes(document->before.sound),historyPersonalityBytes(document->after.sound),
                static_cast<std::uint64_t>(document->before.sound.name.getNumBytesAsUTF8()+1),static_cast<std::uint64_t>(document->after.sound.name.getNumBytesAsUTF8()+1),
                document->fields.personalities.capacity()*sizeof(juce::Uuid),imageMetadataUnits(document->assignments.before),imageMetadataUnits(document->assignments.after)};
        }
    }
}
bool InstrumentProcessor::replayRetainedOwnedAction(const juce::UndoableAction* retained,bool undo)
{
    const auto found=std::find_if(editHistory->actions.begin(),editHistory->actions.begin()+editHistory->count,[&](const auto& action){return action.get()==retained;});
    if(found==editHistory->actions.begin()+editHistory->count
        ||(!dynamic_cast<const DrumPadAction*>(retained)&&!dynamic_cast<const OwnedDocumentAction*>(retained)))return false;
    return undo?(*found)->undo():(*found)->perform();
}
#endif
InstrumentProcessor::DrumControlPump::DrumControlPump(InstrumentProcessor& p):owner(p) {startTimer(25);}
InstrumentProcessor::DrumControlPump::~DrumControlPump() {stopTimer();}
void InstrumentProcessor::DrumControlPump::timerCallback() {
#ifndef NDEBUG
    owner.drumTestPoint(DrumTestPoint::beforeControlPumpWork);
#endif
    {const std::lock_guard lock(owner.restoreMutex);owner.drainDrumFeedbackOwned();owner.clearStoppedPreviewOwned();owner.drainAiLocalOwned();}
    owner.pollMcp();
    if(owner.drumImporter)owner.drumImporter->poll();
}
void InstrumentProcessor::refreshDrumHistoryRootsOwned() noexcept {
    auto& state=*drumOwnership;state.historyRootCount_=0;
    const auto add=[&](const drums::AssetPtr& owner){if(owner){
        assert(state.historyRootCount_<state.historyRoots_.size());
        state.historyRoots_[state.historyRootCount_++]=owner.get();}};
    for(std::size_t n=0;n<editHistory->count;++n){
        const auto* action=editHistory->actions[n].get();
        if(const auto* pad=dynamic_cast<const DrumPadAction*>(action)){add(pad->before.owner);add(pad->after.owner);}
        if(const auto* document=dynamic_cast<const OwnedDocumentAction*>(action)){
            for(const auto& image:document->assignments.before)add(image.owner);
            for(const auto& image:document->assignments.after)add(image.owner);
        }
    }
}
void InstrumentProcessor::drainDrumFeedbackOwned() noexcept {refreshDrumHistoryRootsOwned();drumOwnership->drain();}
void InstrumentProcessor::publishDrumFeedback() noexcept {
    auto report=engine.drumReferenceReport(drumOwnership->lifecycle_);
#ifndef NDEBUG
    drumTestPoint(DrumTestPoint::beforeReportPublish);
#endif
    (void)drumOwnership->enqueue(report);
}
bool InstrumentProcessor::invalidateDrumTargetsOwned(std::uint64_t mask,std::uint64_t acknowledged) noexcept {
    if(mask&&drumTargetEpoch==std::numeric_limits<std::uint64_t>::max())return false;
    if(mask){const auto token=++drumTargetEpoch;for(std::size_t n=0;n<64;++n)if(mask&(std::uint64_t(1)<<n))drumTargetTokens[n].store(token);}
    drumAcknowledgedSource.store(acknowledged);return true;
}
InstrumentProcessor::BankOwnerBinding InstrumentProcessor::drumBankOwner(const SoundDocument& sound,std::uint8_t bank) noexcept {
    BankOwnerBinding binding;binding.use=sound.layout.synthUse[bank];
    if(sound.personalities)for(const auto& instance:sound.personalities->instances)for(const auto& unit:instance.design.units)
        if(isSourceModule(unit.module))if(const auto* bound=rackUnitBinding(instance,unit.id);bound&&bound->slot==bank){binding.rack=instance.id;binding.unit=unit.id;return binding;}
    return binding;
}
bool InstrumentProcessor::resolveDrumTargetOwned(const ProjectDocument& current,const DrumPadTarget& target,bool asynchronous,DrumCommitError& error,bool requireUnlocked) const noexcept {
    if(target.rack.isNull()||target.bank>=4||target.pad>=16)return rejected(DrumCommitError::invalid,error);
    if(structuralGeneration.load()!=drumAcknowledgedSource.load())return rejected(DrumCommitError::staleTarget,error);
    const auto binding=drumBankOwner(current.sound,target.bank);
    if(binding.use!=SlotUse::present||binding.rack!=target.rack||binding.unit!=target.unit
        ||sourceModule(current.sound,target.bank)!=NativeModule::drum)return rejected(DrumCommitError::staleTarget,error);
    if(asynchronous&&(target.token==0||target.token!=drumTargetTokens[target.bank*16+target.pad].load()
        ||target.reset!=resetGeneration.load()))return rejected(DrumCommitError::staleTarget,error);
    if(requireUnlocked&&current.sound.synthLocks[target.bank])return rejected(DrumCommitError::locked,error);
    for(std::size_t n=0;requireUnlocked&&n<drumParametersPerPad;++n)
        if(current.sound.controlLocks[drumParameterStart+target.bank*drumParametersPerSlot+target.pad*drumParametersPerPad+n])return rejected(DrumCommitError::locked,error);
    error=DrumCommitError::none;return true;
}
bool InstrumentProcessor::captureDrumPadTarget(juce::Uuid rack,std::uint8_t unit,std::uint8_t pad,DrumPadTarget& output,DrumCommitError& error) noexcept {
    return captureDrumTarget(rack,unit,pad,output,error,true);
}
bool InstrumentProcessor::captureDrumPerformanceTarget(juce::Uuid rack,std::uint8_t unit,std::uint8_t pad,DrumPadTarget& output,DrumCommitError& error) noexcept {
    return captureDrumTarget(rack,unit,pad,output,error,false);
}
bool InstrumentProcessor::captureDrumTarget(juce::Uuid rack,std::uint8_t unit,std::uint8_t pad,DrumPadTarget& output,DrumCommitError& error,bool requireUnlocked) noexcept {
    try {
        const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();
        ProjectDocument current;std::uint64_t edit;
        if(rack.isNull()||pad>=16)return rejected(DrumCommitError::invalid,error);
        if(!tryProjectSnapshotOwned(current,edit))return rejected(DrumCommitError::busy,error);
        for(std::uint8_t bank=0;bank<4;++bank){const auto binding=drumBankOwner(current.sound,bank);
            if(binding.rack==rack&&binding.unit==unit){
                DrumPadTarget candidate{rack,unit,bank,pad,drumTargetTokens[bank*16+pad].load(),resetGeneration.load()};
                if(!resolveDrumTargetOwned(current,candidate,true,error,requireUnlocked))return false;
                output=candidate;return true;
            }
        }
        return rejected(DrumCommitError::invalid,error);
    }catch(const std::bad_alloc&){return rejected(DrumCommitError::allocationFailed,error);}
}
bool InstrumentProcessor::enqueueDrumPad(const DrumPadTarget& target,std::uint8_t velocity,std::uint8_t channel) noexcept {
    return enqueueDrumPadAtOffset(target,velocity,channel,0);
}
DrumPerformanceStatus InstrumentProcessor::drumPerformanceStatus() const noexcept {return performance->drumStatus();}
bool InstrumentProcessor::enqueueDrumPadAtOffset(const DrumPadTarget& target,std::uint8_t velocity,std::uint8_t channel,std::uint32_t hostOffset) noexcept {
    const auto reject=[&](DrumPerformanceStatus status){performance->reportDrumStatus(status);return false;};
    if(target.bank>=4||target.pad>=16||velocity<1||velocity>127||channel<1||channel>16)return reject(DrumPerformanceStatus::invalid);
    try {
        const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();
        ProjectDocument current;std::uint64_t edit;DrumCommitError error;
        if(!tryProjectSnapshotOwned(current,edit))return reject(DrumPerformanceStatus::unavailable);
        if(!resolveDrumTargetOwned(current,target,true,error,false))return reject(error==DrumCommitError::invalid?DrumPerformanceStatus::invalid:
            error==DrumCommitError::staleTarget?DrumPerformanceStatus::staleTarget:DrumPerformanceStatus::unavailable);
        return performance->enqueueDrum({target.token,hostOffset,target.bank,target.pad,velocity,channel});
    }catch(const std::bad_alloc&){return reject(DrumPerformanceStatus::unavailable);}
}
bool InstrumentProcessor::prepareDrumAsset(std::span<const float> input,std::uint32_t channels,std::uint64_t& output,DrumCommitError& error) noexcept {
    if((channels!=1&&channels!=2)||input.empty()||input.size()%channels||input.size()/channels>480000)
        return rejected(DrumCommitError::invalid,error);
    const auto bytes=std::uint64_t(input.size())*sizeof(float),workspace=8*(20+bytes)+4096;
    std::size_t slot=3;std::uint64_t ticket=0;
    {
        const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();auto& state=*drumOwnership;
        for(std::size_t n=0;n<3;++n)if(state.preparations_[n].ticket==0&&state.preparations_[n].inputId==0){slot=n;break;}
        if(slot==3)return rejected(DrumCommitError::preparationFull,error);
        if(state.nextTicket_==std::numeric_limits<std::uint64_t>::max())return rejected(DrumCommitError::authorityExhausted,error);
        if(!state.reserveDecoded(bytes))return rejected(DrumCommitError::decodedBudget,error);
        auto& preparation=state.preparations_[slot];ticket=++state.nextTicket_;
        preparation.ticket=ticket;preparation.running=true;preparation.decoded=bytes;preparation.workspace=workspace;
        state.reservedWorkspace_+=workspace;
    }
#ifndef NDEBUG
    drumTestPoint(DrumTestPoint::afterPreparationReservation);
#endif
    return finishDrumPreparation(slot,ticket,0,input,channels,nullptr,output,error);
}
bool InstrumentProcessor::finishDrumPreparation(std::size_t slot,std::uint64_t ticket,std::uint64_t inputId,
    std::span<const float> input,std::uint32_t channels,const drums::AssetId* expected,std::uint64_t& output,DrumCommitError& error) noexcept {
    drums::AssetPtr result;drums::AssetError assetError;
    const bool made=drums::makeSampleAsset(input,channels,result,assetError);
    const std::lock_guard lock(restoreMutex);auto& state=*drumOwnership;auto& preparation=state.preparations_[slot];
    const bool current=preparation.ticket==ticket&&preparation.inputId==inputId&&preparation.running;
    if(!current){result.reset();return rejected(DrumCommitError::cancelled,error);}
    const bool cancelled=preparation.cancelled;const bool idMatches=!expected||(made&&result->id()==*expected);
    bool accepted=made&&!cancelled&&idMatches;if(accepted)accepted=state.registerAsset(result);
    if(accepted)preparation.asset=std::move(result);result.reset();
    if(inputId){
        if(accepted){const auto bytes=std::uint64_t(input.size())*sizeof(float);state.releaseDecoded(bytes);preparation.decoded-=bytes;preparation.phase=drums::DrumOwnership::Preparation::Phase::prepared;}
    }else{
        state.releaseDecoded(preparation.decoded);state.reservedWorkspace_-=preparation.workspace;
        preparation.decoded=preparation.workspace=0;preparation.running=false;
        if(!accepted)preparation={};
    }
    if(!accepted){state.collect();return rejected(cancelled?DrumCommitError::cancelled:!idMatches?DrumCommitError::invalid:
        made?DrumCommitError::busy:assetError==drums::AssetError::allocationFailed?DrumCommitError::allocationFailed:DrumCommitError::invalid,error);}
    output=ticket;error=DrumCommitError::none;return true;
}
void InstrumentProcessor::discardPreparedDrumAsset(std::uint64_t ticket) noexcept {
    const std::lock_guard lock(restoreMutex);auto& state=*drumOwnership;
    for(auto& preparation:state.preparations_)if(preparation.ticket==ticket){
        if(preparation.inputId){preparation.ticket=0;preparation.asset.reset();preparation.phase=drums::DrumOwnership::Preparation::Phase::draining;}
        else if(preparation.running)preparation.cancelled=true;else preparation={};break;
    }
    drainDrumFeedbackOwned();
}
bool InstrumentProcessor::beginDrumInput(std::uint64_t& output,DrumCommitError& error) noexcept {
    const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();auto& state=*drumOwnership;
    if(drumInputAiBusyOwned())return rejected(DrumCommitError::busy,error);
    auto found=std::find_if(state.preparations_.begin(),state.preparations_.end(),[](const auto& p){return p.ticket==0&&p.inputId==0;});
    if(found==state.preparations_.end())return rejected(DrumCommitError::preparationFull,error);
    if(state.nextTicket_==UINT64_MAX)return rejected(DrumCommitError::authorityExhausted,error);
    constexpr std::uint64_t sourceBytes=8ull*1024*1024,prototypeBytes=393224;
    const auto charge=sourceBytes+(state.inputPrototypeCharged_?0:prototypeBytes);
    if(state.reservedWorkspace_>UINT64_MAX-charge)return rejected(DrumCommitError::workspaceBudget,error);
    found->inputId=++state.nextTicket_;found->sourceWorkspace=sourceBytes;found->phase=drums::DrumOwnership::Preparation::Phase::rawInput;
    state.reservedWorkspace_+=charge;state.inputPrototypeCharged_=true;output=found->inputId;error=DrumCommitError::none;return true;
}
bool InstrumentProcessor::reserveDrumInputPreparation(std::uint64_t id,std::uint32_t frames,std::uint32_t channels,DrumCommitError& error) noexcept {
    if(!id||!frames||frames>480000||(channels!=1&&channels!=2))return rejected(DrumCommitError::invalid,error);
    const auto bytes=std::uint64_t(frames)*channels*4,workspace=8*(20+bytes)+4096;
    const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();auto& state=*drumOwnership;
    auto found=std::find_if(state.preparations_.begin(),state.preparations_.end(),[&](const auto& p){return p.inputId==id;});
    if(found==state.preparations_.end()||found->cancelled||found->phase==drums::DrumOwnership::Preparation::Phase::draining)return rejected(DrumCommitError::cancelled,error);
    if(found->running||found->decoded||found->workspace||found->operationWorkspace)return rejected(DrumCommitError::busy,error);
    const std::uint64_t operation=found->encodedReservation?0:16ull*1024*1024;
    if(state.nextTicket_==UINT64_MAX)return rejected(DrumCommitError::authorityExhausted,error);
    if(state.reservedWorkspace_>UINT64_MAX-workspace-operation)return rejected(DrumCommitError::workspaceBudget,error);
    if(!state.reserveDecoded(2*bytes))return rejected(DrumCommitError::decodedBudget,error);
    // Admission precedes retiring the former tuple, so refusal preserves its ticket.
    found->asset.reset();found->ticket=++state.nextTicket_;found->frames=frames;found->channels=channels;
    found->decoded=2*bytes;found->workspace=workspace;found->operationWorkspace=operation;found->running=true;
    found->phase=drums::DrumOwnership::Preparation::Phase::preparing;state.reservedWorkspace_+=workspace+operation;state.collect();error=DrumCommitError::none;return true;
}
bool InstrumentProcessor::finishDrumInput(std::uint64_t id,std::span<const float> input,std::uint32_t channels,
    const drums::AssetId& expected,std::uint64_t& output,DrumCommitError& error) noexcept {
    std::size_t slot=3;std::uint64_t ticket=0;
    {const std::lock_guard lock(restoreMutex);const auto& state=*drumOwnership;for(std::size_t n=0;n<3;++n){const auto& p=state.preparations_[n];if(p.inputId==id&&id){
        if(p.cancelled||!p.running||p.phase!=drums::DrumOwnership::Preparation::Phase::preparing)return rejected(DrumCommitError::cancelled,error);
        if(channels!=p.channels||input.size()!=std::size_t(p.frames)*channels)return rejected(DrumCommitError::invalid,error);slot=n;ticket=p.ticket;break;}}}
    if(slot==3)return rejected(DrumCommitError::cancelled,error);
    return finishDrumPreparation(slot,ticket,id,input,channels,&expected,output,error);
}
void InstrumentProcessor::cancelDrumInput(std::uint64_t id) noexcept {
    const std::lock_guard lock(restoreMutex);auto& state=*drumOwnership;for(auto& p:state.preparations_)if(id&&p.inputId==id){
        p.cancelled=true;p.ticket=0;p.asset.reset();p.phase=drums::DrumOwnership::Preparation::Phase::draining;break;}state.collect();
}
void InstrumentProcessor::releaseDrumInputPreparationWork(std::uint64_t id) noexcept {
    const std::lock_guard lock(restoreMutex);auto& state=*drumOwnership;for(auto& p:state.preparations_)if(id&&p.inputId==id){
        state.releaseDecoded(p.decoded);state.reservedWorkspace_-=p.workspace+p.operationWorkspace;p.decoded=p.workspace=p.operationWorkspace=0;p.running=false;
        if(!p.cancelled&&p.phase!=drums::DrumOwnership::Preparation::Phase::draining){p.phase=p.asset?drums::DrumOwnership::Preparation::Phase::prepared:drums::DrumOwnership::Preparation::Phase::rawInput;if(!p.asset)p.ticket=0;}break;}state.collect();
}
void InstrumentProcessor::releaseDrumInputWork(std::uint64_t id) noexcept {
    const std::lock_guard lock(restoreMutex);auto& state=*drumOwnership;for(auto& p:state.preparations_)if(id&&p.inputId==id){
        state.releaseDecoded(p.decoded);state.reservedWorkspace_-=p.workspace+p.operationWorkspace+p.sourceWorkspace+p.batchWorkspace;state.reservedEncoded_-=p.encodedReservation;p={};break;}state.collect();
}
bool InstrumentProcessor::readDrumPad(const DrumPadTarget& target,DrumPadReadback& output,DrumCommitError& error) noexcept {
    try {const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();ProjectDocument current;std::uint64_t edit;
        if(!tryProjectSnapshotOwned(current,edit))return rejected(DrumCommitError::busy,error);
        if(!resolveDrumTargetOwned(current,target,true,error,false))return false;
        auto& state=*drumOwnership;DrumPadReadback candidate;candidate.metadata=state.current_->document->slots()[target.bank][target.pad];
        if(candidate.metadata.asset){const auto& owner=state.current_->owners[target.bank*16+target.pad];if(!owner||owner->id()!=*candidate.metadata.asset)return rejected(DrumCommitError::missingAsset,error);candidate.frames=owner->frames();candidate.channels=owner->channels();}
        output=std::move(candidate);error=DrumCommitError::none;return true;
    }catch(const std::bad_alloc&){return rejected(DrumCommitError::allocationFailed,error);}
}
void InstrumentProcessor::releaseManualPreviewChargeOwned() noexcept {
    if(manualPreviewBytes){drumOwnership->releaseDecoded(manualPreviewBytes);manualPreviewBytes=0;}
}
void InstrumentProcessor::clearStoppedPreviewOwned() noexcept {
    if(renderedPreview.stopped()&&renderedPreview.clearStopped()){releaseManualPreviewChargeOwned();releaseAiPreviewChargeOwned();releaseMcpPreviewChargeOwned();previewOwner=PreviewOwner::none;}
}
bool InstrumentProcessor::startManualDrumPreview(const DrumPadTarget& target,std::uint64_t ticket,std::uint64_t revision,DrumCommitError& error) noexcept {
    const std::lock_guard lock(restoreMutex);return startManualDrumPreviewOwned(target,ticket,revision,error);
}
bool InstrumentProcessor::startManualDrumPreviewOwned(const DrumPadTarget& target,std::uint64_t ticket,std::uint64_t revision,DrumCommitError& error) noexcept {
    try {
        drainDrumFeedbackOwned();ProjectDocument current;std::uint64_t edit;
        if(!tryProjectSnapshotOwned(current,edit)||edit!=revision||!drumImporter->current(target,ticket))return rejected(DrumCommitError::staleTarget,error);
        if(!resolveDrumTargetOwned(current,target,true,error,false))return false;
        auto& state=*drumOwnership;const auto found=std::find_if(state.preparations_.begin(),state.preparations_.end(),[&](const auto& p){return p.ticket==ticket&&ticket;});
        if(found==state.preparations_.end()||!found->inputId||found->running||found->cancelled||!found->asset||found->phase!=drums::DrumOwnership::Preparation::Phase::prepared)return rejected(DrumCommitError::cancelled,error);
        if(!prepared)return rejected(DrumCommitError::audioNotReady,error);
        if(!renderedPreview.stopped()){if(previewOwner==PreviewOwner::ai)stopAiPreviewOwned();return rejected(DrumCommitError::busy,error);}
        clearStoppedPreviewOwned();const auto bytes=std::uint64_t(found->asset->frames())*2*sizeof(float);
        if(!state.reserveDecoded(bytes))return rejected(DrumCommitError::decodedBudget,error);
        if(!renderedPreview.startCanonical(found->asset->samples(),found->asset->channels(),revision)){state.releaseDecoded(bytes);return rejected(DrumCommitError::allocationFailed,error);}
        manualPreviewBytes=bytes;previewOwner=PreviewOwner::manual;error=DrumCommitError::none;return true;
    }catch(const std::bad_alloc&){return rejected(DrumCommitError::allocationFailed,error);}
}
void InstrumentProcessor::stopManualDrumPreview() noexcept {const std::lock_guard lock(restoreMutex);if(previewOwner==PreviewOwner::manual)renderedPreview.stop();}
bool InstrumentProcessor::manualDrumPreviewPlaying() const noexcept {const std::lock_guard lock(restoreMutex);return previewOwner==PreviewOwner::manual&&renderedPreview.playing();}
InstrumentProcessor::ChangedProjectFields InstrumentProcessor::changedProjectFields(const ProjectDocument& before,const ProjectDocument& after) {
    ChangedProjectFields fields;
    for(std::size_t n=0;n<parameterCount;++n)fields.values[n]=before.sound.values[n]!=after.sound.values[n];
    for(std::size_t n=0;n<8;++n)fields.macros[n]=before.sound.macros[n]!=after.sound.macros[n];
    for(std::size_t bank=0;bank<4;++bank)for(std::size_t family=0;family<2;++family)fields.synthRevisions[bank][family]=before.sound.compatibility.synth[bank][family]!=after.sound.compatibility.synth[bank][family];
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t family=0;family<effectBankCount;++family)fields.effectRevisions[slot][family]=before.sound.compatibility.effects[slot][family]!=after.sound.compatibility.effects[slot][family];
    fields.layout=before.sound.layout!=after.sound.layout;fields.pattern=before.pattern!=after.pattern;fields.named=before.sound.name!=after.sound.name;
    const auto collect=[&](const SoundDocument& source,const SoundDocument& other){if(source.personalities)for(const auto& instance:source.personalities->instances){
        const RackInstance* found=nullptr;if(other.personalities)for(const auto& entry:other.personalities->instances)if(entry.id==instance.id){found=&entry;break;}
        if((!found||instance!=*found)&&std::find(fields.personalities.begin(),fields.personalities.end(),instance.id)==fields.personalities.end())fields.personalities.push_back(instance.id);
    }};
    collect(before.sound,after.sound);collect(after.sound,before.sound);return fields;
}
bool InstrumentProcessor::mergeChangedProjectFieldsOwned(const ProjectDocument& target,const ChangedProjectFields& fields,ProjectDocument& live) const {
    for(std::size_t n=0;n<parameterCount;++n)if(fields.values[n])live.sound.values[n]=target.sound.values[n];
    for(std::size_t n=0;n<8;++n)if(fields.macros[n])live.sound.macros[n]=target.sound.macros[n];
    for(std::size_t bank=0;bank<4;++bank)for(std::size_t family=0;family<2;++family)if(fields.synthRevisions[bank][family])live.sound.compatibility.synth[bank][family]=target.sound.compatibility.synth[bank][family];
    for(std::size_t slot=0;slot<8;++slot)for(std::size_t family=0;family<effectBankCount;++family)if(fields.effectRevisions[slot][family])live.sound.compatibility.effects[slot][family]=target.sound.compatibility.effects[slot][family];
    if(fields.layout)live.sound.layout=target.sound.layout;
    if(fields.pattern)live.pattern=target.pattern;
    if(fields.named)live.sound.name=target.sound.name;
    if(!fields.personalities.empty()){
        auto personalities=std::make_shared<RackPersonalityState>();if(live.sound.personalities)*personalities=*live.sound.personalities;
        for(const auto& id:fields.personalities){std::erase_if(personalities->instances,[&](const auto& entry){return entry.id==id;});
            if(target.sound.personalities)for(const auto& entry:target.sound.personalities->instances)if(entry.id==id)personalities->instances.push_back(entry);}
        live.sound.personalities=personalities->instances.empty()?nullptr:personalities;
    }
    return validateProjectDocument(live);
}
bool InstrumentProcessor::performOwnedPadEdit(const DrumPadTarget& target,const OwnedPadImage& after,std::span<const ParameterGestureValue> values,DrumCommitError& error) {
    auto before=std::make_unique<ProjectDocument>();std::uint64_t edit;
    if(gesture->index!=parameterCount||gesture->notifying||aiApplying)return rejected(DrumCommitError::busy,error);
    if(!tryProjectSnapshotOwned(*before,edit))return rejected(DrumCommitError::busy,error);
    if(!resolveDrumTargetOwned(*before,target,true,error))return false;
    auto& ownership=*drumOwnership;
    auto action=std::make_unique<DrumPadAction>(*this,target,
        OwnedPadImage{target.bank,target.pad,ownership.current_->document->slots()[target.bank][target.pad],ownership.current_->owners[target.bank*16+target.pad]},after);
    if(values.size()>9)return rejected(DrumCommitError::invalid,error);
    const auto first=drumParameterStart+target.bank*drumParametersPerSlot+target.pad*drumParametersPerPad;
    std::array<bool,9> seen{};
    for(const auto& value:values){
        if(value.index<first||value.index>=first+9||!std::isfinite(value.value)||value.value<0||value.value>1||seen[value.index-first])return rejected(DrumCommitError::invalid,error);
        seen[value.index-first]=true;
        if(before->sound.controlLocks[value.index])return rejected(DrumCommitError::locked,error);
        if(before->sound.values[value.index]!=value.value)action->numeric[action->numericCount++]={value.index,before->sound.values[value.index],value.value};
    }
    if(action->before.metadata==action->after.metadata&&action->before.owner==action->after.owner&&action->numericCount==0){error=DrumCommitError::none;return true;}
    const auto units=std::uint64_t(sizeof(DrumPadAction))+padMetadataUnits(action->before.metadata)+padMetadataUnits(action->after.metadata);
    if(units==0||units>INT_MAX)return rejected(DrumCommitError::invalid,error);
    action->cachedUnits=static_cast<int>(units);
    if(!performHistoryAction(std::move(action))){error=drumCommitStatus;return false;}
    error=DrumCommitError::none;return true;
}
bool InstrumentProcessor::replayOwnedPad(const DrumPadAction& action,bool forward,DrumCommitError& error) {
    try {
        drainDrumFeedbackOwned();auto current=std::make_unique<ProjectDocument>();std::uint64_t edit;
        if(gesture->index!=parameterCount||gesture->notifying||!tryProjectSnapshotOwned(*current,edit))return rejected(DrumCommitError::busy,error);
        if(!resolveDrumTargetOwned(*current,action.target,false,error))return false;
        const auto& image=forward?action.after:action.before;
        if(image.metadata.asset.has_value()!=bool(image.owner)||(image.owner&&*image.metadata.asset!=image.owner->id()))return rejected(DrumCommitError::missingAsset,error);
        auto stage=std::make_unique<AssignmentStage>();stage->slots=drumOwnership->current_->document->slots();stage->owners=drumOwnership->current_->owners;
        stage->slots[image.bank][image.pad]=image.metadata;stage->owners[image.bank*16+image.pad]=image.owner;
        std::array<bool,parameterCount> numericMask {};
        for(std::size_t n=0;n<action.numericCount;++n){const auto& delta=action.numeric[n];current->sound.values[delta.index]=forward?delta.after:delta.before;numericMask[delta.index]=true;}
        RoutedPatch patch;if(!validateProjectDocument(*current)||!makeRoutedPatch(current->sound,patch))return rejected(DrumCommitError::invalid,error);
        if(!drumOwnership->intendedFits(stage->owners))return rejected(DrumCommitError::decodedBudget,error);
        if(drumOwnership->freePublicationSlot()==drums::publicationCapacity)return rejected(DrumCommitError::publicationFull,error);
        if(drumOwnership->current_->borrowed.generation==std::numeric_limits<std::uint64_t>::max())return rejected(DrumCommitError::authorityExhausted,error);
        drums::DrumDocumentPtr document;drums::DrumError documentError;
        if(!drums::makeDrumDocument(stage->slots,document,documentError))return rejected(documentError==drums::DrumError::allocationFailed?DrumCommitError::allocationFailed:DrumCommitError::invalid,error);
        auto publication=std::make_unique<drums::DrumOwnership::PublicationRecord>(drumOwnership->current_->borrowed.generation+1,std::move(document),stage->owners);
        const auto invalidated=std::uint64_t(1)<<(image.bank*16+image.pad);
        if(!publishDrumCommit(*current,false,true,false,&numericMask,false,std::move(publication),invalidated))return rejected(drumCommitStatus,error);
        error=DrumCommitError::none;return true;
    }catch(const std::bad_alloc&){return rejected(DrumCommitError::allocationFailed,error);}
}
bool InstrumentProcessor::assignPreparedDrumPad(const DrumPadTarget& target,std::uint64_t ticket,const drums::DrumPad& metadata,
    std::span<const ParameterGestureValue> values,DrumCommitError& error) noexcept {
    bool applied=false;
    try {
        const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();
        auto& state=*drumOwnership;
        const auto found=std::find_if(state.preparations_.begin(),state.preparations_.end(),[&](const auto& p){return p.ticket==ticket&&ticket!=0;});
        if(found==state.preparations_.end()||found->running||found->cancelled||!found->asset)return rejected(DrumCommitError::cancelled,error);
        if(!metadata.asset||*metadata.asset!=found->asset->id())return rejected(DrumCommitError::invalid,error);
        if(!performOwnedPadEdit(target,{target.bank,target.pad,metadata,found->asset},values,error))return false;
        if(found->inputId){found->ticket=0;found->asset.reset();found->phase=drums::DrumOwnership::Preparation::Phase::draining;}
        else *found={};refreshDrumHistoryRootsOwned();state.collect();applied=true;
#ifndef NDEBUG
        drumTestPoint(DrumTestPoint::afterCoreInstall);
#endif
    }catch(const std::bad_alloc&){if(!applied)return rejected(DrumCommitError::allocationFailed,error);error=DrumCommitError::none;}
    catch(...){if(!applied)return rejected(DrumCommitError::invalid,error);error=DrumCommitError::none;}
    if(applied)triggerAsyncUpdate();return applied;
}
bool InstrumentProcessor::clearDrumPad(const DrumPadTarget& target,DrumCommitError& error) noexcept {
    bool applied=false;
    try {const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();
        applied=performOwnedPadEdit(target,{target.bank,target.pad,{},{}},{},error);
#ifndef NDEBUG
        if(applied)drumTestPoint(DrumTestPoint::afterCoreInstall);
#endif
    }catch(const std::bad_alloc&){return rejected(DrumCommitError::allocationFailed,error);}
    if(applied)triggerAsyncUpdate();return applied;
}

bool InstrumentProcessor::performOwnedDocumentEdit(const ProjectDocument& before,const ProjectDocument& after,
    OwnedAssignmentScope scope,ChangedProjectFields fields,OwnedDocumentKind kind,bool reset,DrumCommitError& error)
{
    if(scope.before.size()!=std::popcount(scope.pads)||scope.after.size()!=scope.before.size())return rejected(DrumCommitError::invalid,error);
    std::uint64_t observed=0;
    for(std::size_t n=0;n<scope.before.size();++n){const auto& a=scope.before[n];const auto& b=scope.after[n];
        if(a.bank>=4||a.pad>=16||a.bank!=b.bank||a.pad!=b.pad)return rejected(DrumCommitError::invalid,error);
        const auto bit=std::uint64_t(1)<<(a.bank*16+a.pad);
        if(!(scope.pads&bit)||(observed&bit))return rejected(DrumCommitError::invalid,error);observed|=bit;
        for(const auto* image:{&a,&b})if(image->metadata.asset.has_value()!=bool(image->owner)
            ||(image->owner&&*image->metadata.asset!=image->owner->id()))return rejected(DrumCommitError::missingAsset,error);
    }
    if(observed!=scope.pads||!validateProjectDocument(before)||!validateProjectDocument(after))return rejected(DrumCommitError::invalid,error);
    auto action=std::make_unique<OwnedDocumentAction>(*this,before,after,std::move(fields),std::move(scope),kind,reset);
    const auto units=std::uint64_t(sizeof(OwnedDocumentAction))+historyPersonalityBytes(before.sound)+historyPersonalityBytes(after.sound)
        +before.sound.name.getNumBytesAsUTF8()+1+after.sound.name.getNumBytesAsUTF8()+1
        +action->fields.personalities.capacity()*sizeof(juce::Uuid)+imageMetadataUnits(action->assignments.before)+imageMetadataUnits(action->assignments.after);
    if(units==0||units>INT_MAX)return rejected(DrumCommitError::invalid,error);
    action->cachedUnits=static_cast<int>(units);
    // Identical presets refresh playback authority without creating an undo step.
    const bool identicalPreset=kind==OwnedDocumentKind::soundPreset&&before==after
        &&std::equal(action->assignments.before.begin(),action->assignments.before.end(),action->assignments.after.begin(),
            [](const auto& a,const auto& b){return a.metadata==b.metadata;});
    if(!(identicalPreset?action->perform():performHistoryAction(std::move(action)))){error=drumCommitStatus;return false;}
    error=DrumCommitError::none;return true;
}
bool InstrumentProcessor::replayOwnedDocument(const OwnedDocumentAction& action,bool forward,DrumCommitError& error)
{
    try {
        drainDrumFeedbackOwned();auto live=std::make_unique<ProjectDocument>();std::uint64_t edit;
        if(gesture->index!=parameterCount||gesture->notifying||!tryProjectSnapshotOwned(*live,edit))return rejected(DrumCommitError::busy,error);
        const auto& expected=forward?action.before:action.after;
        const auto& target=forward?action.after:action.before;
        if(action.kind==OwnedDocumentKind::rack){
            if(action.fields.layout&&live->sound.layout!=expected.sound.layout)return rejected(DrumCommitError::staleTarget,error);
            for(unsigned bank=0;bank<4;++bank)if(action.assignments.guardedBanks&(1u<<bank)){
                const auto& binding=forward?action.assignments.beforeOwners[bank]:action.assignments.afterOwners[bank];
                if(drumBankOwner(live->sound,static_cast<std::uint8_t>(bank))!=binding)return rejected(DrumCommitError::staleTarget,error);
            }
            for(const auto& id:action.fields.personalities){
                const auto find=[&](const SoundDocument& sound)->const RackInstance*{if(sound.personalities)for(const auto& instance:sound.personalities->instances)if(instance.id==id)return &instance;return nullptr;};
                const auto* current=find(live->sound);const auto* previous=find(expected.sound);
                if((current==nullptr)!=(previous==nullptr)||(current&&*current!=*previous))return rejected(DrumCommitError::staleTarget,error);
            }
            if(!mergeChangedProjectFieldsOwned(target,action.fields,*live))return rejected(DrumCommitError::invalid,error);
        }else live->sound=target.sound;
        auto stage=std::make_unique<AssignmentStage>();stage->slots=drumOwnership->current_->document->slots();stage->owners=drumOwnership->current_->owners;
        const auto& images=forward?action.assignments.after:action.assignments.before;
        for(const auto& image:images){stage->slots[image.bank][image.pad]=image.metadata;stage->owners[image.bank*16+image.pad]=image.owner;}
        RoutedPatch patch;if(!validateProjectDocument(*live)||!makeRoutedPatch(live->sound,patch))return rejected(DrumCommitError::invalid,error);
        if(!drumOwnership->intendedFits(stage->owners))return rejected(DrumCommitError::decodedBudget,error);
        std::unique_ptr<drums::DrumOwnership::PublicationRecord> publication;
        if(action.kind==OwnedDocumentKind::soundPreset||stage->slots!=drumOwnership->current_->document->slots()){
            if(drumOwnership->freePublicationSlot()==drums::publicationCapacity)return rejected(DrumCommitError::publicationFull,error);
            if(drumOwnership->current_->borrowed.generation==std::numeric_limits<std::uint64_t>::max())return rejected(DrumCommitError::authorityExhausted,error);
            drums::DrumDocumentPtr document;drums::DrumError de;
            if(!drums::makeDrumDocument(stage->slots,document,de))return rejected(de==drums::DrumError::allocationFailed?DrumCommitError::allocationFailed:DrumCommitError::invalid,error);
            publication=std::make_unique<drums::DrumOwnership::PublicationRecord>(drumOwnership->current_->borrowed.generation+1,std::move(document),stage->owners);
        }
        std::uint64_t invalidated=action.assignments.pads;
        for(unsigned bank=0;bank<4;++bank)if(action.assignments.invalidateBanks&(1u<<bank))invalidated|=bankPads(bank);
        if(action.kind==OwnedDocumentKind::soundPreset)invalidated=~std::uint64_t(0);
        const auto* mask=action.kind==OwnedDocumentKind::rack?&action.fields.values:nullptr;
        if(!publishDrumCommit(*live,action.reset,true,false,mask,false,std::move(publication),invalidated,stagedAiPresetPath))return rejected(drumCommitStatus,error);
        error=DrumCommitError::none;return true;
    }catch(const std::bad_alloc&){return rejected(DrumCommitError::allocationFailed,error);}
}
bool InstrumentProcessor::validateFreshSourceOwnerAdmissionOwned(const SoundDocument& before,const SoundDocument& reconciled,
    const SourceAssignmentTrace* trace,DrumCommitError& error) const noexcept
{
    auto& state=*drumOwnership;state.protectedIdCount_=0;
    const auto add=[&](const juce::Uuid& id){
        if(id.isNull())return;
        for(std::size_t n=0;n<state.protectedIdCount_;++n)if(std::memcmp(state.protectedIds_[n].data(),id.getRawData(),16)==0)return;
        assert(state.protectedIdCount_<state.protectedIds_.size());
        std::memcpy(state.protectedIds_[state.protectedIdCount_++].data(),id.getRawData(),16);
    };
    for(std::size_t n=0;n<editHistory->count;++n){const auto* action=editHistory->actions[n].get();
        if(const auto* pad=dynamic_cast<const DrumPadAction*>(action))add(pad->target.rack);
        if(const auto* document=dynamic_cast<const OwnedDocumentAction*>(action))for(unsigned bank=0;bank<4;++bank){
            if(document->kind==OwnedDocumentKind::rack&&!(document->assignments.guardedBanks&(1u<<bank)))continue;
            bool content=false;
            for(const auto* images:{&document->assignments.before,&document->assignments.after})for(const auto& image:*images)
                if(image.bank==bank&&hasMetadata(image.metadata))content=true;
            if(content){add(document->assignments.beforeOwners[bank].rack);add(document->assignments.afterOwners[bank].rack);}
        }
    }
    for(std::uint8_t bank=0;bank<4;++bank){bool content=false;for(const auto& pad:state.current_->document->slots()[bank])content|=hasMetadata(pad);
        if(content)add(drumBankOwner(before,bank).rack);
    }
    const auto protectedId=[&](const juce::Uuid& id){if(id.isNull())return false;
        for(std::size_t n=0;n<state.protectedIdCount_;++n)if(std::memcmp(state.protectedIds_[n].data(),id.getRawData(),16)==0)return true;return false;};
    if(trace){for(std::size_t n=0;n<trace->count;++n)if(trace->effects[n].freshLogicalOwner&&protectedId(trace->effects[n].afterOwner))return rejected(DrumCommitError::invalid,error);}
    else for(std::uint8_t bank=0;bank<4;++bank){
        const auto proposed=drumBankOwner(reconciled,bank);bool already=false;
        for(std::uint8_t old=0;old<4;++old){const auto current=drumBankOwner(before,old);already|=old==bank&&proposed.rack==current.rack&&proposed.unit==current.unit;}
        if(!proposed.rack.isNull()&&!already&&protectedId(proposed.rack))return rejected(DrumCommitError::invalid,error);
        if(proposed!=drumBankOwner(before,bank))for(const auto& pad:state.current_->document->slots()[bank])
            if(hasMetadata(pad))return rejected(DrumCommitError::invalid,error);
    }
    error=DrumCommitError::none;return true;
}
bool InstrumentProcessor::performOwnedRackEdit(const ProjectDocument& before,const ProjectDocument& after,
    const SourceAssignmentTrace& trace,DrumCommitError& error)
{
    if(!validateFreshSourceOwnerAdmissionOwned(before.sound,after.sound,&trace,error))return false;
    auto stage=std::make_unique<AssignmentStage>();
    if(!buildAssignmentStageOwned(before.sound,trace,*stage,error))return false;
    auto fields=changedProjectFields(before,after);const auto registry=parameterRegistry();
    for(std::size_t bank=0;bank<4;++bank)if(stage->fullNumericBanks&(1u<<bank)){
        const auto prefix="synth."+juce::String(bank+1)+".";
        for(std::size_t n=0;n<parameterCount;++n)if(registry[n].id.startsWith(prefix))fields.values[n]=true;
        fields.synthRevisions[bank].fill(true);
    }
    return performOwnedDocumentEdit(before,after,std::move(stage->scope),std::move(fields),OwnedDocumentKind::rack,false,error);
}
bool InstrumentProcessor::buildAssignmentStageOwned(const SoundDocument& before,const SourceAssignmentTrace& trace,
    AssignmentStage& stage,DrumCommitError& error) const
{
    return buildAssignmentStageOwned(before,trace,drumOwnership->current_->document->slots(),drumOwnership->current_->owners,stage,error);
}
bool InstrumentProcessor::buildAssignmentStageOwned(const SoundDocument& before,const SourceAssignmentTrace& trace,
    const drums::DrumSlots& sourceSlots,const DrumOwners& sourceOwners,AssignmentStage& stage,DrumCommitError& error) const
{
    if(trace.count>trace.effects.size())return rejected(DrumCommitError::invalid,error);
    stage.slots=sourceSlots;stage.owners=sourceOwners;
    for(std::uint8_t bank=0;bank<4;++bank)stage.scope.beforeOwners[bank]=stage.scope.afterOwners[bank]=drumBankOwner(before,bank);
    auto entry=std::make_unique<AssignmentStage>();
    for(std::size_t begin=0;begin<trace.count;){
        const auto ordinal=trace.effects[begin].operation;std::size_t end=begin+1;
        while(end<trace.count&&trace.effects[end].operation==ordinal)++end;
        entry->slots=stage.slots;entry->owners=stage.owners;
        for(auto n=begin;n<end;++n){const auto& effect=trace.effects[n];
            if(effect.bank>=4)return rejected(DrumCommitError::invalid,error);
            const auto bit=std::uint8_t(1u<<effect.bank);
            if(!(stage.scope.guardedBanks&bit))stage.scope.beforeOwners[effect.bank]={effect.beforeUse,effect.beforeOwner,effect.beforeUnit};
            stage.scope.guardedBanks|=bit;stage.scope.afterOwners[effect.bank]={effect.afterUse,effect.afterOwner,effect.afterUnit};
            if(effect.invalidateTarget)stage.scope.invalidateBanks|=bit;
            if(effect.fullNumericBank)stage.fullNumericBanks|=bit;
            if(effect.kind==SourceAssignmentEffectKind::preserve)continue;
            if(effect.kind==SourceAssignmentEffectKind::copy&&effect.copyFrom>=4)return rejected(DrumCommitError::invalid,error);
            stage.scope.pads|=bankPads(effect.bank);
            for(std::size_t pad=0;pad<16;++pad){
                if(effect.kind==SourceAssignmentEffectKind::clear){stage.slots[effect.bank][pad]={};stage.owners[effect.bank*16+pad].reset();}
                else{stage.slots[effect.bank][pad]=entry->slots[effect.copyFrom][pad];stage.owners[effect.bank*16+pad]=entry->owners[effect.copyFrom*16+pad];}
            }
        }
        begin=end;
    }
    for(std::uint8_t bank=0;bank<4;++bank)for(std::uint8_t pad=0;pad<16;++pad)if(stage.scope.pads&(std::uint64_t(1)<<(bank*16+pad))){
        stage.scope.before.push_back({bank,pad,sourceSlots[bank][pad],sourceOwners[bank*16+pad]});
        stage.scope.after.push_back({bank,pad,stage.slots[bank][pad],stage.owners[bank*16+pad]});
    }
    error=DrumCommitError::none;return true;
}

namespace {
struct RestoreAdmissionFailure {};
constexpr std::uint64_t decodeWorkspace=8ull*3840052+2*drums::maxDrumDocumentBytes+2*drums::maxCompactDocumentBytes;
constexpr std::uint64_t saveWorkspace=drums::maxDrumBundleBytes+decodeWorkspace;
bool bundleEnvelope(const void* data,std::size_t size) noexcept {
    return data&&size>=4&&std::memcmp(data,"ARBN",4)==0;
}
}
bool InstrumentProcessor::beginDrumSaveOwned(ProjectDocument& document)
{
    drainDrumFeedbackOwned();auto& state=*drumOwnership;std::uint64_t edit;
    if(state.saving_||!tryProjectSnapshotOwned(document,edit))return false;
    state.saveOwners_=state.current_->owners;state.saveDocument_=state.current_->document;
    state.saving_=true;state.reservedWorkspace_+=saveWorkspace;return true;
}
void InstrumentProcessor::endDrumSaveOwned() noexcept
{
    auto& state=*drumOwnership;state.saveOwners_={};state.saveDocument_.reset();
    state.saving_=false;state.reservedWorkspace_-=saveWorkspace;drainDrumFeedbackOwned();
}
bool InstrumentProcessor::encodeOwnedDrumState(const ProjectDocument& document,bool project,juce::MemoryBlock& output)
{
    const auto failed=[&](DrumCommitError error){const std::lock_guard lock(restoreMutex);drumCommitStatus=error;return false;};
    juce::MemoryBlock compact;
    if(project?!encodeProjectState(document,compact):!encodeSoundState(document.sound,compact))return failed(DrumCommitError::invalid);
    bool content=false;for(const auto& bank:drumOwnership->saveDocument_->slots())for(const auto& pad:bank)content|=hasMetadata(pad);
    if(!content){output.swapWith(compact);return true;}
    std::array<drums::AssetPtr,64> assets{};std::size_t count=0;
    for(const auto& owner:drumOwnership->saveOwners_)if(owner){bool present=false;
        for(std::size_t n=0;n<count;++n)present|=assets[n].get()==owner.get();if(!present)assets[count++]=owner;}
    drums::DrumBundlePtr bundle;drums::DrumError error;
    if(!drums::makeDrumBundle(drumOwnership->saveDocument_,
        std::span(static_cast<const std::uint8_t*>(compact.getData()),compact.getSize()),
        std::span(assets).first(count),bundle,error))return failed(error==drums::DrumError::allocationFailed?DrumCommitError::allocationFailed:DrumCommitError::invalid);
    if(!drums::encodeDrumBundle(*bundle,output,error))return failed(error==drums::DrumError::allocationFailed?DrumCommitError::allocationFailed:DrumCommitError::invalid);
    return true;
}
void InstrumentProcessor::getStateInformation(juce::MemoryBlock& output)
{
    bool capture=false;
    try {
        auto document=std::make_unique<ProjectDocument>();
        {const std::lock_guard lock(restoreMutex);capture=beginDrumSaveOwned(*document);if(!capture)drumCommitStatus=DrumCommitError::busy;}
#ifndef NDEBUG
        if(capture)drumTestPoint(DrumTestPoint::afterSaveCapture);
#endif
        if(capture)(void)encodeOwnedDrumState(*document,true,output);
    }catch(const std::bad_alloc&){const std::lock_guard lock(restoreMutex);drumCommitStatus=DrumCommitError::allocationFailed;}
    if(capture){const std::lock_guard lock(restoreMutex);endDrumSaveOwned();}
}
bool InstrumentProcessor::saveSoundPreset(const juce::File& target)
{
    bool capture=false,saved=false;
    juce::String stem;juce::File folder;
    try {
        stem=target.getFileNameWithoutExtension();folder=target.getParentDirectory();
        auto document=std::make_unique<ProjectDocument>();juce::MemoryBlock encoded;
        {const std::lock_guard lock(restoreMutex);capture=beginDrumSaveOwned(*document);
            if(capture&&!stem.equalsIgnoreCase(document->sound.name)&&stem.getNumBytesAsUTF8()<=maximumSoundNameBytes){
                if(gesture->index!=parameterCount){endDrumSaveOwned();capture=false;return false;}document->sound.name=stem;}}
        if(!capture)return false;
#ifndef NDEBUG
        drumTestPoint(DrumTestPoint::afterSaveCapture);
#endif
        if(encodeOwnedDrumState(*document,false,encoded)&&folder.createDirectory().wasOk()){
            juce::TemporaryFile temporary(target);bool written=false;
            {auto stream=temporary.getFile().createOutputStream();
                if(stream&&stream->write(encoded.getData(),encoded.getSize())){stream->flush();written=stream->getStatus().wasOk();}}
            if(written)saved=temporary.overwriteTargetFileWithTemporary();
        }
    }catch(const std::bad_alloc&){const std::lock_guard lock(restoreMutex);drumCommitStatus=DrumCommitError::allocationFailed;}
    if(capture){const std::lock_guard lock(restoreMutex);endDrumSaveOwned();capture=false;}
    if(!saved)return false;
#ifndef NDEBUG
    drumTestPoint(DrumTestPoint::afterPresetFileCommit);
#endif
    try {
        const std::lock_guard lock(restoreMutex);SoundDocument current;std::uint64_t edit;
        if(!trySoundSnapshotOwned(current,edit)){drumCommitStatus=DrumCommitError::busy;return true;}
        const bool renames=!stem.equalsIgnoreCase(current.name)&&stem.getNumBytesAsUTF8()<=maximumSoundNameBytes;
        if(!renames){presetFolder=std::move(folder);return true;}
        if(gesture->index!=parameterCount){drumCommitStatus=DrumCommitError::busy;return true;}
        stagedPresetFolder=&folder;bool applied=false;
        try {applied=performSoundNameEdit(current,stem);}catch(...){stagedPresetFolder=nullptr;throw;}
        stagedPresetFolder=nullptr;if(!applied)return true;
    }catch(const std::bad_alloc&){const std::lock_guard lock(restoreMutex);drumCommitStatus=DrumCommitError::allocationFailed;return true;}
    triggerAsyncUpdate();return true;
}
bool InstrumentProcessor::restoreSoundState(const void* data,int size) {return restoreProjectState(data,size);}
bool InstrumentProcessor::restoreProjectState(const void* data,int size) {
    bool groupChanged=false,committed=false;
    {const std::lock_guard operation(drumRestoreOperationMutex);committed=restoreOwnedDrumState(data,size,false,nullptr,groupChanged);}
    if(committed){if(groupChanged)updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));triggerAsyncUpdate();}
    return committed;
}
bool InstrumentProcessor::restoreOwnedDrumState(const void* data,int size,bool preset,const juce::File* file,bool& savedGroupChanged)
{
    savedGroupChanged=false;
    if(size<0||(!data&&size!=0)||static_cast<std::size_t>(size)>drums::maxDrumBundleBytes){if(!preset)loadFailed.store(true);return false;}
    const bool envelope=bundleEnvelope(data,static_cast<std::size_t>(size));
    drums::BundleGeometry geometry;drums::DrumError de;
    if(envelope&&!drums::inspectDrumBundleGeometry(std::span(static_cast<const std::uint8_t*>(data),static_cast<std::size_t>(size)),geometry,de)){
        if(!preset)loadFailed.store(true);return false;}
    std::unique_ptr<ProjectDocument> incoming;std::unique_ptr<AssignmentStage> stage;
    drums::DrumBundlePtr bundle;std::uint64_t decodedReservation=envelope?geometry.decodedBytes+geometry.largestDecodedAsset:0;
    const auto workspace=envelope?decodeWorkspace:2ull*drums::maxCompactDocumentBytes;
    bool reserved=false,committed=false;
    {
        const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();auto& state=*drumOwnership;
        if((!file&&state.restoring_)||(file&&!state.restoring_))return false;
        if(!state.reserveDecoded(decodedReservation)){drumCommitStatus=DrumCommitError::decodedBudget;return false;}
        if(!file)state.restoring_=true;
        state.reservedWorkspace_+=workspace;reserved=true;
    }
#ifndef NDEBUG
    drumTestPoint(DrumTestPoint::afterRestoreReservation);
#endif
    const auto finish=[&]{if(!reserved)return;const std::lock_guard lock(restoreMutex);auto& state=*drumOwnership;
        if(stage)stage->owners={};bundle.reset();state.restoreOwners_={};state.releaseDecoded(decodedReservation);decodedReservation=0;
        state.reservedWorkspace_-=workspace;if(!file)state.restoring_=false;reserved=false;drainDrumFeedbackOwned();};
    try {
        incoming=std::make_unique<ProjectDocument>();stage=std::make_unique<AssignmentStage>();
        const void* compact=data;std::size_t compactBytes=static_cast<std::size_t>(size);
        if(envelope){
            if(!drums::decodeDrumBundle(std::span(static_cast<const std::uint8_t*>(data),static_cast<std::size_t>(size)),bundle,de)){finish();if(!preset)loadFailed.store(true);return false;}
            compact=bundle->compactDocument().data();compactBytes=bundle->compactDocument().size();stage->slots=bundle->document().slots();
        }
        const bool numeric=preset?decodeSoundState(compact,compactBytes,incoming->sound):decodeProjectState(compact,compactBytes,*incoming);
        if(!numeric){finish();if(!preset)loadFailed.store(true);return false;}
        juce::File folder;if(file)folder=file->getParentDirectory();
        {
            const std::lock_guard lock(restoreMutex);drainDrumFeedbackOwned();auto& state=*drumOwnership;
            if(envelope){state.releaseDecoded(geometry.largestDecodedAsset);decodedReservation-=geometry.largestDecodedAsset;
                for(const auto& asset:bundle->assets()){
                    auto canonical=asset;
                    if(!state.registerAsset(canonical)){drumCommitStatus=DrumCommitError::busy;throw RestoreAdmissionFailure{};}
                    if(canonical.get()==asset.get()){
                        const auto bytes=std::uint64_t(asset->frames())*asset->channels()*sizeof(float);state.releaseDecoded(bytes);decodedReservation-=bytes;
                    }
                    for(std::size_t bank=0;bank<4;++bank)for(std::size_t pad=0;pad<16;++pad)
                        if(stage->slots[bank][pad].asset&&*stage->slots[bank][pad].asset==canonical->id())stage->owners[bank*16+pad]=canonical;
                }
                state.restoreOwners_=stage->owners;bundle.reset();state.releaseDecoded(decodedReservation);decodedReservation=0;
            }
            bool reuseEmpty=!preset&&!envelope;
            if(reuseEmpty)for(std::size_t bank=0;bank<4;++bank)for(std::size_t pad=0;pad<16;++pad)
                reuseEmpty&=!hasMetadata(stage->slots[bank][pad])&&!hasMetadata(state.current_->document->slots()[bank][pad]);
            if(!state.intendedFits(stage->owners)){drumCommitStatus=DrumCommitError::decodedBudget;}
            else if(!reuseEmpty&&state.freePublicationSlot()==drums::publicationCapacity){drumCommitStatus=DrumCommitError::publicationFull;}
            else if(!reuseEmpty&&state.current_->borrowed.generation==std::numeric_limits<std::uint64_t>::max()){drumCommitStatus=DrumCommitError::authorityExhausted;}
            else if(preset){
                auto before=std::make_unique<ProjectDocument>();std::uint64_t edit;
                if(gesture->index==parameterCount&&tryProjectSnapshotOwned(*before,edit)){
                    if(!validBeforeRecovery(*before))*before=*recoveryDocument;
                    auto after=std::make_unique<ProjectDocument>(*before);after->sound=incoming->sound;
                    if(file&&after->sound.name.isEmpty()){const auto stem=file->getFileNameWithoutExtension();if(stem.getNumBytesAsUTF8()<=maximumSoundNameBytes)after->sound.name=stem;}
                    OwnedAssignmentScope scope;scope.pads=~std::uint64_t(0);scope.guardedBanks=scope.invalidateBanks=15;
                    for(std::uint8_t bank=0;bank<4;++bank){scope.beforeOwners[bank]=drumBankOwner(before->sound,bank);scope.afterOwners[bank]=drumBankOwner(after->sound,bank);
                        for(std::uint8_t pad=0;pad<16;++pad){scope.before.push_back({bank,pad,state.current_->document->slots()[bank][pad],state.current_->owners[bank*16+pad]});
                            scope.after.push_back({bank,pad,stage->slots[bank][pad],stage->owners[bank*16+pad]});}}
                    stagedPresetFolder=file?&folder:nullptr;
                    try {committed=performOwnedDocumentEdit(*before,*after,std::move(scope),changedProjectFields(*before,*after),OwnedDocumentKind::soundPreset,true,drumCommitStatus);}
                    catch(...){stagedPresetFolder=nullptr;throw;}
                    stagedPresetFolder=nullptr;
                }
            }else {
                drums::DrumDocumentPtr document;
                if(reuseEmpty||drums::makeDrumDocument(stage->slots,document,de)){
                    std::unique_ptr<drums::DrumOwnership::PublicationRecord> publication;
                    if(!reuseEmpty)publication=std::make_unique<drums::DrumOwnership::PublicationRecord>(state.current_->borrowed.generation+1,std::move(document),stage->owners);
                    savedGroupChanged=groupLinkState.view().intent!=incoming->group;
                    committed=publishDrumCommit(*incoming,true,true,false,nullptr,true,std::move(publication),~std::uint64_t(0));
                    if(committed)clearHistoryOwned();
                }
            }
#ifndef NDEBUG
            if(committed)drumTestPoint(DrumTestPoint::afterCoreInstall);
#endif
        }
    }catch(const RestoreAdmissionFailure&){}
    catch(const std::bad_alloc&){const std::lock_guard lock(restoreMutex);drumCommitStatus=DrumCommitError::allocationFailed;}
    finish();
    if(committed)rotateMcpEpoch();
    return committed;
}
bool InstrumentProcessor::loadFactorySound(std::size_t index)
{
    bool restored=false,groupChanged=false;
    try {
        auto sound=std::make_unique<SoundDocument>();juce::MemoryBlock bytes;
        if(!makeFactorySound(index,*sound)||!encodeSoundState(*sound,bytes))return false;
        const std::lock_guard operation(drumRestoreOperationMutex);
        restored=restoreOwnedDrumState(bytes.getData(),static_cast<int>(bytes.getSize()),true,nullptr,groupChanged);
        if(restored){const std::lock_guard lock(restoreMutex);presetPath.clear();}
    }catch(const std::bad_alloc&){const std::lock_guard lock(restoreMutex);drumCommitStatus=DrumCommitError::allocationFailed;return false;}
    if(restored)triggerAsyncUpdate();
    return restored;
}
bool InstrumentProcessor::loadSoundPreset(const juce::File& file)
{
    bool restored=false,groupChanged=false;
    try {
    std::unique_lock operation(drumRestoreOperationMutex);
    juce::FileInputStream stream(file);if(!stream.openedOk())return false;
    const auto length=stream.getTotalLength();if(length<0||length>static_cast<juce::int64>(drums::maxDrumBundleBytes))return false;
    std::array<std::uint8_t,8> magic{};const auto header=stream.read(magic.data(),static_cast<int>(magic.size()));
    if(header<0||!stream.setPosition(0))return false;
    if(!bundleEnvelope(magic.data(),static_cast<std::size_t>(header))&&length!=1720&&length!=7006&&length!=static_cast<juce::int64>(legacySoundStateSize)
        &&(length<static_cast<juce::int64>(legacySoundStateSize+8)||length>static_cast<juce::int64>(maximumSoundStateBytes)))return false;
    {const std::lock_guard lock(restoreMutex);if(drumOwnership->restoring_)return false;drumOwnership->restoring_=true;drumOwnership->reservedWorkspace_+=static_cast<std::uint64_t>(length);}
    try {juce::MemoryBlock input(static_cast<std::size_t>(length));
        const auto read=stream.read(input.getData(),static_cast<int>(length));std::uint8_t extra=0;
        if(read==length&&stream.read(&extra,1)==0)restored=restoreOwnedDrumState(input.getData(),static_cast<int>(length),true,&file,groupChanged);
    }catch(const std::bad_alloc&){const std::lock_guard lock(restoreMutex);drumCommitStatus=DrumCommitError::allocationFailed;}
    {const std::lock_guard lock(restoreMutex);drumOwnership->reservedWorkspace_-=static_cast<std::uint64_t>(length);drumOwnership->restoring_=false;drainDrumFeedbackOwned();}
    operation.unlock();
    }catch(const std::bad_alloc&){const std::lock_guard lock(restoreMutex);drumCommitStatus=DrumCommitError::allocationFailed;return false;}
    if(restored){if(groupChanged)updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));triggerAsyncUpdate();}
    return restored;
}
}

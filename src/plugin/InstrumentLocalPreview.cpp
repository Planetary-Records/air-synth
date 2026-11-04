#include "InstrumentProcessor.h"
#include "DrumImport.h"
#include "CompanionWire.h"
#include "LocalPreview.h"
#include <algorithm>
#include <cmath>

namespace airack
{
namespace
{
constexpr std::uint64_t frameEnvelope(std::uint64_t body) noexcept {return 8*(20+body)+4096;}
// Two receive bodies can overlap during the actual pipe->Frame handoff. Request,
// captured-context and control-frame copies have their smaller independent bounds.
constexpr std::uint64_t localMetadataReservation=2*frameEnvelope(companion::maximumResultPayloadBytes)
    +4*frameEnvelope(companion::maximumControlPayloadBytes)+4*65536;
constexpr std::uint64_t localRenderWorkspace=2*512*sizeof(float)+maximumPatternEvents*3
    +sizeof(PatternScheduler)+sizeof(PatternEventBuffer)+65536;
bool samePeer(const companion::PipePeer& a,const companion::PipePeer& b) noexcept
{return a.processId==b.processId&&a.creationTime==b.creationTime;}
bool hasDrum(const SoundDocument& sound) noexcept
{for(std::uint8_t bank=0;bank<4;++bank)if(sound.layout.synthUse[bank]==SlotUse::present&&sourceModule(sound,bank)==NativeModule::drum)return true;return false;}
}
void InstrumentProcessor::deleteAiLocalTrace(drums::DrumOwnership* owner,ai::PreviewCallTrace* trace) noexcept
{
    if(!trace)return;delete trace; // release C only after the real allocation dies
    if(owner)owner->releaseDecoded(sizeof(ai::PreviewCallTrace));
}
bool InstrumentProcessor::aiRequestUsesLocalPreview(const ai::GenerationRequest& request) noexcept
{return hasDrum(request.source.sound)||(request.catalog&&request.catalog->authority&&request.catalog->authority->rendering==ai::CatalogRendering::ownedLocal);}
bool InstrumentProcessor::buildAiPreviewMapOwned(const SoundDocument& sound,const drums::DrumDocument& document,
    const DrumOwners& owners,std::uint64_t generation,ai::PreviewSampleMap& output) const
{
    auto map=std::make_unique<ai::PreviewSampleMap>();map->assets.reserve(64);
    for(std::uint8_t bank=0;bank<4;++bank){const auto logical=drumBankOwner(sound,bank);map->banks[bank]={logical.rack,logical.unit,generation};}
    for(std::size_t n=0;n<64;++n){const auto& metadata=document.slots()[n/16][n%16];const auto& owner=owners[n];
        if(metadata.asset.has_value()!=bool(owner)||(owner&&owner->id()!=*metadata.asset))return false;
        if(owner){bool registered=false;for(const auto& entry:drumOwnership->ledger_)registered|=entry.get()==owner.get();if(!registered)return false;
            const ai::PreviewAssetRef ref{owner->id(),std::uint8_t(owner->channels()),owner->frames()};
            const auto existing=std::find_if(map->assets.begin(),map->assets.end(),[&](const auto& value){return value.id==ref.id;});
            if(existing==map->assets.end())map->assets.push_back(ref);else if(*existing!=ref)return false;}
        map->pads[n].name=metadata.name;if(!ai::digestPreviewPad(metadata,map->pads[n].metadata))return false;
    }
    std::sort(map->assets.begin(),map->assets.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    for(std::size_t n=0;n<64;++n)if(owners[n]){const auto found=std::find_if(map->assets.begin(),map->assets.end(),[&](const auto& value){return value.id==owners[n]->id();});map->pads[n].asset=std::uint8_t(found-map->assets.begin()+1);}
    if(!ai::validPreviewMap(*map))return false;output=std::move(*map);return true;
}
InstrumentProcessor::AiLocalCharge::~AiLocalCharge()
{if(owner)owner->releaseDecoded(base+work+output+parent);}
std::unique_ptr<InstrumentProcessor::AiLocalState> InstrumentProcessor::allocateAiLocalStateOwned()
{
    if(!drumOwnership->reserveDecoded(localMetadataReservation))return {};
    try{auto next=std::make_unique<AiLocalState>();next->charge.owner=drumOwnership.get();next->charge.base=localMetadataReservation;return next;}
    catch(...){drumOwnership->releaseDecoded(localMetadataReservation);throw;}
}
std::unique_ptr<InstrumentProcessor::AiLocalState> InstrumentProcessor::prepareAiLocalRootOwned(const ai::GenerationRequest& request,std::uint64_t reset,std::unique_ptr<AiLocalState> next)
{
    // The copied player PCM stays independently charged until its audio acknowledgement.
    if(aiLocal&&(aiLocal->rendering||aiPhase==AiProposalPhase::pending||(!aiLocal->receiptSent&&aiLocal->receipt)))return {};
    auto& state=*drumOwnership;
    if(!next)return {};
    next->current=std::make_unique<ai::LocalPreviewContext>();auto& context=*next->current;
    context.root=request.catalog?request.catalog->root:request.tag;context.catalogSnapshot=request.catalog?request.catalog->authority->snapshot:juce::Uuid::null();
    context.reset=reset;context.sourceWord=request.structuralGeneration;
    next->publicationGeneration=state.current_->borrowed.generation;
    for(std::size_t n=0;n<64;++n)next->targets[n]=drumTargetTokens[n].load();
    if(!buildAiPreviewMapOwned(request.source.sound,*state.current_->document,state.current_->owners,next->publicationGeneration,context.sourceMap)
        ||!ai::digestPreviewMap(context.sourceMap,context.sourceMapDigest))return {};
    next->rootMap=context.sourceMap;
    auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=request;input->local=context;
    juce::MemoryBlock encoded;if(!companion::encodeLocalRequest(*input,encoded))return {};
    if(reset!=resetGeneration.load()||request.tag.editRevision!=editRevision.load()||request.tag.contextRevision!=aiContextRevision
        ||request.structuralGeneration!=structuralGeneration.load()||next->publicationGeneration!=state.current_->borrowed.generation)return {};
    if(state.installAiImage(0,state.current_->document,state.current_->owners)!=drums::DrumOwnership::AiImageStatus::ready)return {};
    return next;
}
void InstrumentProcessor::replaceAiLocalRootOwned(std::unique_ptr<AiLocalState> next) noexcept
{
    drumOwnership->clearAiImage(1);drumOwnership->clearAiImage(2);if(!next)drumOwnership->clearAiImage(0);
    aiLocal=std::move(next);
}
bool InstrumentProcessor::aiLocalCurrentOwned() const noexcept
{
    if(!aiLocal||aiLocal->retiring||!aiLocal->current||!aiPayloads->request)return false;
    const auto& request=*aiPayloads->request;
    if(request.tag.editRevision!=editRevision.load()||request.tag.contextRevision!=aiContextRevision
        ||request.structuralGeneration!=structuralGeneration.load()||aiLocal->current->reset!=resetGeneration.load()
        ||aiLocal->publicationGeneration!=drumOwnership->current_->borrowed.generation)return false;
    for(std::size_t n=0;n<64;++n)if(aiLocal->targets[n]!=drumTargetTokens[n].load())return false;
    return !aiLocal->bound||(aiLocal->session==aiClient.session()&&samePeer(aiLocal->peer,aiClient.peer()));
}
bool InstrumentProcessor::submitAiGeneration(const ai::GenerationRequest& request,const GroupSelection* peers,const ai::RequestTag* parent)
{
    const std::lock_guard lock(restoreMutex);
    if(!aiLocal)return peers?aiClient.generateGroup({request,*peers,parent?std::optional(*parent):std::nullopt}):aiClient.generate(request);
    if(!aiLocalCurrentOwned()||!(aiClient.capabilities()&companion::ownedLocalPreviewCapability)){aiWorkflowMessage="Companion update required for local sample preview";return false;}
    auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=request;input->local=*aiLocal->current;
    const bool sent=peers?aiClient.generateGroupLocal({*input,*peers,parent?std::optional(*parent):std::nullopt}):aiClient.generateLocal(*input);
    if(sent){aiLocal->session=aiClient.session();aiLocal->peer=aiClient.peer();aiLocal->bound=true;}return sent;
}
bool DrumImport::submitLocalPreview(std::unique_ptr<InstrumentProcessor::AiLocalWork>& work)
{
    if(!work)return false;
    {const std::lock_guard lock(mutex);
        if(!accepting||pending.operation!=Operation::none||activeOperation!=Operation::none||completion||inputId
            ||std::any_of(generationInputs.begin(),generationInputs.end(),[](auto id){return id!=0;})
            ||(shown.phase!=DrumImportPhase::closed&&shown.phase!=DrumImportPhase::kept))return false;
        pending.operation=Operation::localPreview;pending.local=std::move(work);
    }
    wake.notify_all();return true;
}
void InstrumentProcessor::releaseAiPreviewChargeOwned() noexcept
{if(aiPreviewBytes){drumOwnership->releaseDecoded(aiPreviewBytes);aiPreviewBytes=0;}}
void InstrumentProcessor::retireAiLocalOwned() noexcept
{if(aiLocal){aiLocal->retiring=true;aiLocal->cancel.store(true,std::memory_order_release);stopAiPreviewOwned();}}
void InstrumentProcessor::drainAiLocalOwned(bool force) noexcept
{
    if(!aiLocal||(!force&&(!aiLocal->retiring||aiLocal->rendering||aiLocal->joining||aiPreviewBytes||aiPayloads->result||aiRefinementBackup)))return;
    const auto connection=aiClient.status();if(!force&&aiLocal->bound&&connection.phase==companion::ClientPhase::ready&&connection.status==companion::RuntimeStatus::busy)return;
    aiLocal.reset();for(std::size_t role=0;role<3;++role)drumOwnership->clearAiImage(role);drumOwnership->collect();
}
void InstrumentProcessor::restoreAiLocalParentOwned()
{
    if(!aiLocal)return;
    aiLocal->cancel.store(true,std::memory_order_release);
    aiLocal->audio={};if(aiLocal->charge.output&&!aiLocal->joining){drumOwnership->releaseDecoded(aiLocal->charge.output);aiLocal->charge.output=0;}
    if(aiLocal->backupContext){aiLocal->current=std::move(aiLocal->backupContext);aiLocal->receipt=aiLocal->parentReceipt;aiLocal->receiptSent=true;}
    if(!aiLocal->rendering)drumOwnership->clearAiImage(2);
}
void InstrumentProcessor::renderAiLocalWork(AiLocalWork& work) noexcept
{
    auto& receipt=work.receipt;receipt.binding=work.evaluation.binding;
    try{
        const auto stop=[&]{return work.cancel->load(std::memory_order_acquire);};
        if(stop()){receipt.status=ai::RenderStatus::cancelled;receipt.failure=ai::PreviewFailure::cancelled;return;}
        const auto requested=std::floor(patternLengthBeats(work.evaluation.context.phrase)*60/work.evaluation.context.bpm*ai::previewSampleRate+.5);
        const auto body=std::uint32_t(std::max(1.,std::min(requested,double(ai::maximumPreviewBodyFrames))));
        ai::detail::PreviewCallTraceScope traceScope(work.callTrace.get(),work.firstCallCompleted);
        auto rendered=std::unique_ptr<ai::RenderResult>(new ai::RenderResult(ai::renderPreview(work.candidate,work.evaluation.context,work.borrowed,stop)));
        work.renderWork=rendered->work;receipt.status=rendered->status;receipt.measurements=rendered->measurements;
        if(receipt.status==ai::RenderStatus::ready||receipt.status==ai::RenderStatus::clipped||receipt.status==ai::RenderStatus::nonFinite){
            receipt.bodyFrames=body;receipt.frames=body+std::uint32_t(ai::previewTailFrames);receipt.phraseTruncated=requested>ai::maximumPreviewBodyFrames;
            if(receipt.status==ai::RenderStatus::ready){
                if(rendered->audio.left.capacity()!=receipt.frames||rendered->audio.right.capacity()!=receipt.frames){receipt={};receipt.binding=work.evaluation.binding;receipt.status=ai::RenderStatus::renderFailure;receipt.failure=ai::PreviewFailure::allocation;return;}
                work.audio=std::move(rendered->audio);
            }
        }else if(receipt.status==ai::RenderStatus::cancelled)receipt.failure=ai::PreviewFailure::cancelled;
        if(stop()){work.audio={};receipt={};receipt.binding=work.evaluation.binding;receipt.status=ai::RenderStatus::cancelled;receipt.failure=ai::PreviewFailure::cancelled;}
    }catch(...){work.audio={};receipt={};receipt.binding=work.evaluation.binding;receipt.status=ai::RenderStatus::renderFailure;receipt.failure=ai::PreviewFailure::allocation;}
}
void InstrumentProcessor::finishAiLocalWork(std::unique_ptr<AiLocalWork> work)
{
    const std::lock_guard lock(restoreMutex);if(!work)return;
    if(aiLocalRenderObservationCount<aiLocalRenderObservations.size()){
        const auto& b=work->evaluation.binding;
        aiLocalRenderObservations[aiLocalRenderObservationCount++]={b.request.requestId,b.job,b.ticket,b.renderOrdinal,work->receipt.status,work->renderWork,std::move(work->callTrace)};
    }else aiLocalRenderObservationOverflow=true;
    if(!aiLocal){work.reset();return;}
    aiLocal->rendering=false;
    const bool current=aiLocalCurrentOwned()&&aiPayloads->request&&work->evaluation.binding.request==aiPayloads->request->tag&&!aiLocal->cancel.load(std::memory_order_acquire);
    const auto workBytes=aiLocal->charge.work;aiLocal->charge.work=0;
    if(current){aiLocal->receipt=work->receipt;aiLocal->receiptSent=false;aiLocal->audio=std::move(work->audio);
        aiLocal->charge.output=aiLocal->audio.left.empty()?0:std::uint64_t(aiLocal->audio.left.capacity()+aiLocal->audio.right.capacity())*sizeof(float);}
    work.reset();drumOwnership->releaseDecoded(workBytes-aiLocal->charge.output);
    if(!current)drumOwnership->clearAiImage(2);drainAiLocalOwned(false);
}
void InstrumentProcessor::pollAiLocal()
{
    const std::lock_guard lock(restoreMutex);
    if(!aiLocal)return;
    if(!aiLocalCurrentOwned()){retireAiLocalOwned();aiClient.cancelGeneration();drainAiLocalOwned(false);return;}
    if(aiLocal->receipt&&!aiLocal->receiptSent){if(aiClient.submitPreviewReceipt(*aiLocal->receipt))aiLocal->receiptSent=true;return;}
    if(aiLocal->rendering||aiPhase!=AiProposalPhase::pending)return;
    std::unique_ptr<ai::PreviewEvaluate> evaluation;
    try{evaluation=std::make_unique<ai::PreviewEvaluate>();}
    catch(const std::bad_alloc&){aiClient.cancelGeneration();failAiOwned("Local preview allocation failed");return;}
    if(!aiClient.takePreviewEvaluation(*evaluation))return;
    const auto& request=*aiPayloads->request;const auto binding=evaluation->binding;const auto& context=*aiLocal->current;
    const auto reject=[&](ai::PreviewFailure failure){if(failure==ai::PreviewFailure::invalidBinding){aiClient.close();retireAiLocalOwned();return;}ai::PreviewReceipt value;value.binding=binding;value.status=ai::RenderStatus::renderFailure;value.failure=failure;aiLocal->receipt=value;aiLocal->receiptSent=false;};
    ai::PreviewDigest advertisedContext;
    if(!ai::digestPreviewContext(evaluation->context,advertisedContext)||advertisedContext!=binding.context||!ai::validPreviewBinding(binding)||binding.request!=request.tag||binding.root!=context.root||binding.catalogSnapshot!=context.catalogSnapshot||binding.reset!=context.reset
        ||binding.sourceWord!=context.sourceWord||binding.structuralGeneration!=request.structuralGeneration||binding.sourceMap!=context.sourceMapDigest
        ||binding.phase!=(request.catalog?request.catalog->phase:ai::CatalogPhase::select)||binding.renderOrdinal!=aiLocal->lastOrdinal+1
        ||binding.ticket<=aiLocal->lastTicket||(aiLocal->lastJob&&binding.job!=aiLocal->lastJob)
        ||(request.catalog&&request.catalog->phase==ai::CatalogPhase::materialize
            ?aiLocal->lastOrdinal!=0||binding.providerAttempt!=request.catalog->inferenceCallsUsed:binding.providerAttempt<=aiLocal->lastAttempt)){aiClient.close();retireAiLocalOwned();return;}
    aiLocal->lastJob=binding.job;aiLocal->lastTicket=binding.ticket;aiLocal->lastOrdinal=binding.renderOrdinal;aiLocal->lastAttempt=binding.providerAttempt;
    const auto requested=std::floor(patternLengthBeats(evaluation->context.phrase)*60/evaluation->context.bpm*ai::previewSampleRate+.5);
    const auto frames=std::uint64_t(std::max(1.,std::min(requested,double(ai::maximumPreviewBodyFrames))))+ai::previewTailFrames;
    const auto workBytes=sizeof(AiLocalWork)+sizeof(AssignmentStage)*2+sizeof(SourceAssignmentTrace)+2*65536+localRenderWorkspace+frames*2*sizeof(float);
    const auto traceBytes=aiLocalProfileCallTrace?std::uint64_t(sizeof(ai::PreviewCallTrace)):0;
    if(!drumOwnership->reserveDecoded(workBytes+traceBytes)){reject(ai::PreviewFailure::budget);return;}
    bool owned=false,traceOwned=false;
    try{
        auto work=std::make_unique<AiLocalWork>();
        if(traceBytes){work->callTrace=AiLocalCallTrace(new ai::PreviewCallTrace,AiLocalTraceDelete{drumOwnership.get()});
            traceOwned=true;work->firstCallCompleted=&aiLocalFirstCallCompleted;}
        work->evaluation=std::move(*evaluation);work->cancel=&aiLocal->cancel;
        auto trace=std::make_unique<SourceAssignmentTrace>();
        const auto admitted=request.catalog?ai::admitProposal(request.source,request.policy,work->evaluation.edits,*request.catalog->authority,work->candidate,trace.get())
            :ai::admitProposal(request.source,request.policy,work->evaluation.edits,work->candidate,trace.get());
        ai::PreviewDigest digest;ai::PreviewSampleMap compact;
        const bool staged=admitted==ai::ProposalStatus::accepted&&ai::stagePreviewMap(context.sourceMap,*trace,compact);
        const auto audition=ai::previewContextFor(request,work->evaluation.edits.pattern,staged?&compact:nullptr,&work->candidate.sound);
        if(!staged||!ai::digestPreviewCandidate(work->candidate,digest)||digest!=binding.candidate
            ||!ai::samePreviewContext(audition,work->evaluation.context)||!ai::digestPreviewContext(audition,digest)||digest!=binding.context){aiClient.close();retireAiLocalOwned();}
        else{
            const auto role=request.catalog||!aiLocal->hasParent?0u:1u;const auto& source=drumOwnership->aiImages_[role];
            ai::PreviewSampleMap physical;bool sourceCurrent=bool(source);
            if(sourceCurrent&&(request.catalog||!aiLocal->hasParent)){
                // Root images carry the captured publication; currentness checked that publication and every pad token above.
#ifndef NDEBUG
                drumImporter->testPoint(DrumImport::TestPoint::beforeLocalRootValidation);
#endif
                sourceCurrent=buildAiPreviewMapOwned(request.source.sound,*source->document,source->owners,aiLocal->publicationGeneration,physical)
                    &&physical==context.sourceMap;
                physical={}; // Reuse this scratch after source validation; no extra map overlaps final staging.
            }
            auto stage=std::make_unique<AssignmentStage>();DrumCommitError error;
            if(!sourceCurrent||!buildAssignmentStageOwned(request.source.sound,*trace,source->document->slots(),source->owners,*stage,error))reject(ai::PreviewFailure::missingOwner);
            else{
                drums::DrumDocumentPtr document;drums::DrumError documentError;
                if(!ai::digestPreviewMap(compact,digest)||digest!=binding.stagedMap
                    ||!drums::makeDrumDocument(stage->slots,document,documentError)
                    ||!buildAiPreviewMapOwned(work->candidate.sound,*document,stage->owners,0,physical))reject(ai::PreviewFailure::invalidBinding);
                else{
                    for(std::size_t bank=0;bank<4;++bank)physical.banks[bank].generation=compact.banks[bank].generation;
                    if(physical!=compact)reject(ai::PreviewFailure::invalidBinding);
                    else{
                        const auto installed=drumOwnership->installAiImage(2,document,stage->owners);
                        if(installed!=drums::DrumOwnership::AiImageStatus::ready)reject(installed==drums::DrumOwnership::AiImageStatus::budget?ai::PreviewFailure::budget:ai::PreviewFailure::allocation);
                        else{
                            for(std::size_t n=0;n<64;++n)work->borrowed[n/16][n%16]=drumOwnership->aiImages_[2]->owners[n].get();
                            aiLocal->audio={};if(aiLocal->charge.output){drumOwnership->releaseDecoded(aiLocal->charge.output);aiLocal->charge.output=0;}
                            aiLocal->receipt.reset();aiLocal->receiptSent=false;aiLocal->cancel.store(false,std::memory_order_release);aiLocal->stagedMap=std::move(compact);
                            aiLocal->charge.work=workBytes;aiLocal->rendering=true;
                            if(drumImporter->submitLocalPreview(work))owned=true;else{aiLocal->rendering=false;aiLocal->charge.work=0;reject(ai::PreviewFailure::busy);}
                        }
                    }
                }
            }
        }
    }catch(const std::bad_alloc&){reject(ai::PreviewFailure::allocation);}
    if(!owned){drumOwnership->releaseDecoded(workBytes+(traceOwned?0:traceBytes));drumOwnership->clearAiImage(2);}
}
bool InstrumentProcessor::receiveAiLocalResult(ai::LocalGenerationResult&& result)
{
    const std::lock_guard lock(restoreMutex);
    if(!aiLocal||!aiLocalCurrentOwned()||!aiPayloads->request||aiPhase!=AiProposalPhase::pending||result.numeric.tag!=aiPayloads->request->tag)return false;
    try{
#ifndef NDEBUG
        drumImporter->testPoint(DrumImport::TestPoint::beforeLocalTerminal);
#endif
        auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=*aiPayloads->request;input->local=*aiLocal->current;
        bool allocationFailed=false;
        if(!ai::validateLocalGenerationResult(*input,result,&allocationFailed)){
            if(allocationFailed)failAiOwned("Local preview validation allocation refused");
            else{aiClient.close();retireAiLocalOwned();}
            return false;
        }
        if(result.numeric.status==ai::GenerationStatus::ready){
            if(aiLocal->rendering||!aiLocal->receiptSent||!aiLocal->receipt)return false;
            auto joined=std::make_unique<ai::GenerationResult>();
#ifndef NDEBUG
            drumImporter->testPoint(DrumImport::TestPoint::beforeLocalJoin);
#endif
            if(!ai::joinLocalPreview(*input,result,*aiLocal->receipt,std::move(aiLocal->audio),*joined))return false;
            aiLocal->joining=true;const bool accepted=receiveAiResultOwned(std::move(*joined),true);
            joined.reset();aiLocal->joining=false;
            if(!accepted&&aiLocal->charge.output){drumOwnership->releaseDecoded(aiLocal->charge.output);aiLocal->charge.output=0;}
            return accepted;
        }
        return receiveAiResultOwned(std::move(result.numeric),true);
    }catch(const std::bad_alloc&){
        // Any joined temporary has destroyed before this handler releases its output charge.
        aiLocal->audio={};aiLocal->joining=false;
        if(aiLocal->charge.output){drumOwnership->releaseDecoded(aiLocal->charge.output);aiLocal->charge.output=0;}
        // The ready terminal was consumed; preserve its admitted remote parent for Refine.
        failAiOwned("Local preview allocation failed");return false;
    }
}
bool InstrumentProcessor::validateAiLocalParentOwned(const ProjectDocument&,const SourceAssignmentTrace& trace)
{
    if(!aiLocal)return true;if(!aiLocalCurrentOwned()||!drumOwnership->aiImages_[2])return false;
    ai::PreviewSampleMap cumulative;if(!ai::stagePreviewMap(aiLocal->rootMap,trace,cumulative)||cumulative!=aiLocal->stagedMap)return false;
    return true;
}
void InstrumentProcessor::acceptAiLocalParentOwned() noexcept
{
    if(!aiLocal)return;
    const auto oldCharge=aiLocal->charge.parent;aiLocal->charge.parent=aiLocal->charge.output;aiLocal->charge.output=0;
    drumOwnership->clearAiImage(1);drumOwnership->aiImages_[1]=std::move(drumOwnership->aiImages_[2]);
    aiLocal->parentMap=std::move(aiLocal->stagedMap);aiLocal->parentReceipt=aiLocal->receipt;aiLocal->hasParent=true;aiLocal->backupContext.reset();
    if(oldCharge)drumOwnership->releaseDecoded(oldCharge);
}
bool InstrumentProcessor::validateAiLocalApplyOwned(const SoundDocument& before,const SourceAssignmentTrace& trace)
{
    if(!aiLocal)return true;if(!aiLocalCurrentOwned()||!aiLocal->hasParent)return false;
#ifndef NDEBUG
    drumImporter->testPoint(DrumImport::TestPoint::beforeLocalApplyValidation);
#endif
    try{
        ai::PreviewSampleMap current,staged;
        return buildAiPreviewMapOwned(before,*drumOwnership->current_->document,drumOwnership->current_->owners,aiLocal->publicationGeneration,current)
            &&ai::stagePreviewMap(current,trace,staged)&&staged==aiLocal->parentMap;
    }catch(const std::bad_alloc&){return false;}
}
}

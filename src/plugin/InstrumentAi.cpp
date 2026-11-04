#include "InstrumentProcessor.h"
#include "ParameterListenerRetirement.h"
#include "DrumImport.h"
#include "CompanionWire.h"
#include "AiCatalog.h"
#include "RackLibrary.h"
#include "GenerationLog.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace airack
{
bool InstrumentProcessor::startAiPreview()
{
    const std::lock_guard lock(restoreMutex);
    if(previewOwner==PreviewOwner::manual&&!renderedPreview.stopped())return false;
    if(aiProposalPhase()!=AiProposalPhase::proposed||!aiPayloads->request||!aiPayloads->result||aiPayloads->result->status!=ai::GenerationStatus::ready||aiPayloads->result->tag!=aiPayloads->request->tag
        ||(groupControl->marker&&!groupMarkerCurrentOwned(*groupControl->marker)))return false;
    if(aiLocal&&(!aiLocalCurrentOwned()||!aiLocal->hasParent||!aiLocal->parentReceipt))return false;
    clearStoppedPreviewOwned();
    const auto bytes=aiLocal?std::uint64_t(aiPayloads->result->audio.left.size()+aiPayloads->result->audio.right.size())*sizeof(float):0;
    if(bytes&&!drumOwnership->reserveDecoded(bytes))return false;
    const bool started=renderedPreview.start(aiPayloads->result->audio,aiPayloads->request->tag.editRevision);
    if(started){previewOwner=PreviewOwner::ai;aiPreviewBytes=bytes;}else if(bytes)drumOwnership->releaseDecoded(bytes);return started;
}
void InstrumentProcessor::stopAiPreviewOwned() noexcept {if(previewOwner==PreviewOwner::ai)renderedPreview.stop();}
void InstrumentProcessor::stopAiPreview() noexcept {const std::lock_guard lock(restoreMutex);stopAiPreviewOwned();}
bool InstrumentProcessor::aiPreviewStopped() const noexcept {const std::lock_guard lock(restoreMutex);return previewOwner!=PreviewOwner::ai||renderedPreview.stopped();}
bool InstrumentProcessor::aiPreviewPlaying() const noexcept {const std::lock_guard lock(restoreMutex);return previewOwner==PreviewOwner::ai&&renderedPreview.playing();}
std::optional<double> InstrumentProcessor::currentHostBpm() const noexcept
{const auto bpm=hostBpm.load(std::memory_order_relaxed);return bpm==0?std::nullopt:std::optional<double>(bpm);}
namespace
{
bool mergeChoices(RackReconcileChoices& aggregate,const RackReconcileChoices& next)
{
    for(auto pair:{std::pair{&aggregate.detach,&next.detach},std::pair{&aggregate.remove,&next.remove},std::pair{&aggregate.replace,&next.replace}})for(const auto& id:*pair.second){if(std::find(pair.first->begin(),pair.first->end(),id)==pair.first->end())pair.first->push_back(id);if(pair.first->size()>12)return false;}
    for(const auto& r:next.controls){auto old=std::find_if(aggregate.controls.begin(),aggregate.controls.end(),[&](const auto& x){return x.instance==r.instance&&x.control==r.control;});if(old==aggregate.controls.end())aggregate.controls.push_back(r);else *old=r;}return aggregate.controls.size()<=96;
}

bool validTempo(double bpm) {return std::isfinite(bpm)&&bpm>=20&&bpm<=400;}
bool validContextText(const std::string& text)
{
    return text.size()<=128&&text.find('\0')==std::string::npos
        &&juce::CharPointer_UTF8::isValidString(text.data(),static_cast<int>(text.size()));
}
const char* generationName(ai::GenerationStatus status)
{
    switch(status){case ai::GenerationStatus::ready:return "ready";case ai::GenerationStatus::invalidRequest:return "invalid_request";
        case ai::GenerationStatus::cancelled:return "cancelled";case ai::GenerationStatus::providerFailure:return "provider_failure";
        case ai::GenerationStatus::budgetExhausted:return "budget_exhausted";case ai::GenerationStatus::renderFailure:return "render_failure";
        case ai::GenerationStatus::catalogSelectionRequired:return "catalog_selection_required";}
    return "invalid_result";
}
const char* providerName(rack::oauth::Status status) noexcept
{
    using Status=rack::oauth::Status;
    switch(status){case Status::ready:return "ready";case Status::signedOut:return "signed_out";case Status::awaitingCallback:return "awaiting_callback";
        case Status::connected:return "connected";case Status::denied:return "denied";case Status::cancelled:return "cancelled";case Status::invalidCallback:return "invalid_callback";
        case Status::expiredAttempt:return "expired_attempt";case Status::tokenFailure:return "token_failure";case Status::invalidIdentity:return "invalid_identity";
        case Status::identityMismatch:return "identity_mismatch";case Status::inferenceNotGranted:return "inference_not_granted";case Status::networkFailure:return "network_failure";
        case Status::invalidResponse:return "invalid_response";case Status::accessExpired:return "access_expired";case Status::storageFailure:return "storage_failure";case Status::busy:return "busy";}
    return "unknown";
}
void logAi(const ai::GenerationRequest& request,diagnostics::Stage stage,const ai::GenerationResult* result=nullptr,bool valid=true) noexcept
{
    try{diagnostics::Event event;event.stage=stage;event.runtime=request.tag.runtime;event.requestId=request.tag.requestId;event.model=request.model;
        if(result){event.generationStatus=int(result->status);event.providerStatus=int(result->providerStatus);event.proposalStatus=int(result->proposalStatus);
            event.renderStatus=int(result->renderStatus);event.httpStatus=result->httpStatus;event.calls=result->inferenceCalls;event.errorCode=valid?result->errorCode:juce::String("invalid_result");}
        diagnostics::write(event);}catch(...){}
}
}
const companion::AccountReply* InstrumentProcessor::aiAccountReply() const noexcept {return aiAccount?&*aiAccount:nullptr;}
AiWorkflowStatus InstrumentProcessor::aiWorkflowStatus() const
{return {aiClient.status(),aiStartupAccount.has_value()||aiActiveAccount.has_value(),aiDelayedApply.has_value(),groupControl->waiting.has_value(),
         aiRefinementBackup!=nullptr||(groupControl->waiting&&groupControl->waiting->refine),aiWorkflowMessage};}
const ai::GenerationRequest* InstrumentProcessor::aiCurrentRequest() const noexcept {return aiPayloads->request?&*aiPayloads->request:nullptr;}
const ProjectDocument* InstrumentProcessor::aiOriginalSource() const noexcept
{return aiProposalPhase()!=AiProposalPhase::stale&&aiSourceOriginal?&*aiSourceOriginal:nullptr;}
const ai::ProposalEdits* InstrumentProcessor::aiIntendedEdits() const noexcept
{return aiProposalPhase()==AiProposalPhase::proposed&&aiIntent?&*aiIntent:nullptr;}
bool InstrumentProcessor::aiAccountReady(const juce::String& model) const
{
    return aiClient.status().phase==companion::ClientPhase::ready&&!aiStartupAccount&&!aiActiveAccount
        &&aiAccount&&aiAccount->status==companion::RuntimeStatus::ready&&aiAccount->providerStatus==rack::oauth::Status::connected
        &&model.isNotEmpty()&&model==aiAccount->selectedModel&&aiModelsDiscovered
        &&std::any_of(aiAccount->models.begin(),aiAccount->models.end(),[&](const auto& m){return m.slug==model;});
}
bool InstrumentProcessor::requestAiAccount(const companion::AccountCommand& command)
{
    reconcileGroupConnection();
    juce::MemoryBlock wire;if(!companion::encodeAccountCommand(command,wire)||aiStartupAccount||aiActiveAccount
        ||aiProposalPhase()==AiProposalPhase::pending)return false;
    if(command.action==companion::AccountAction::selectModel&&(!aiAccount||!aiModelsDiscovered
        ||std::none_of(aiAccount->models.begin(),aiAccount->models.end(),[&](const auto& m){return m.slug==command.model;})))return false;
    const auto phase=aiClient.status().phase;
    startTimer(25);
    if(phase==companion::ClientPhase::closed||phase==companion::ClientPhase::failed)
    {
        aiAccount.reset();aiModelsDiscovered=false;aiAccountSession=juce::Uuid::null();aiVerifiedModel.clear();
        if(!aiClient.start(aiClientOptions)){aiWorkflowMessage="Companion unavailable; retry Account";return false;}
        aiStartupAccount=command;aiWorkflowMessage="Connecting to companion";return true;
    }
    if(phase==companion::ClientPhase::connecting){aiStartupAccount=command;return true;}
    if(groupControl->pending){aiStartupAccount=command;aiWorkflowMessage="Account queued after the current group operation";return true;}
    if(!aiClient.account(command)){aiWorkflowMessage="Companion busy; retry Account";return false;}
    aiActiveAccount=command;aiWorkflowMessage=command.action==companion::AccountAction::signIn?"Sign-in requested":"Checking public account/model status";return true;
}
bool InstrumentProcessor::generateAi(const AiRequestOptions& options)
{
    reconcileGroupConnection();
    const bool grouped=groupLinkState.eligible();bool waits=false;
    if(grouped)
    {
        const auto group=groupWorkflowView();
        if(!group.generationReady&&!group.generationWaits){aiWorkflowMessage="Group context is unavailable; retry Generate";return false;}
        waits=!group.generationReady;
    }
    if(!aiAccountReady(options.model)){aiWorkflowMessage="Choose a discovered model from a connected, ready account; retry Account if unavailable";return false;}
    if(waits){groupControl->waiting=GroupControl::WaitingPress{false,options};aiWorkflowMessage="Generate waits for the group heartbeat";return true;}
    groupControl->waiting.reset();
    aiWorkflowMessage.clear();
    const auto request=beginAiRequest(options,currentHostBpm());if(!request){if(aiWorkflowMessage.isEmpty())aiWorkflowMessage="Invalid prompt, policy or context; current proposal preserved";return false;}
    groupControl->marker.reset();groupControl->parent.reset();groupControl->applyToken.reset();
    const bool submitted=submitAiGeneration(*request,grouped?&groupControl->selection:nullptr);
    if(!submitted){const std::lock_guard lock(restoreMutex);failAiOwned("Companion rejected the request; retry Generate");return false;}
    if(grouped)groupControl->marker=GroupControl::Marker{request->tag,aiClient.session(),groupControl->selection};
    startTimer(25);
    aiWorkflowMessage="Generating; committed sound remains available";return true;
}
bool InstrumentProcessor::refineAi(const std::string& instruction)
{
    const auto marker=groupControl->marker;bool waits=false;
    if(marker)
    {
        const auto group=groupWorkflowView();bool owned;{const std::lock_guard lock(restoreMutex);owned=groupMarkerCurrentOwned(*marker);}
        if(!owned||(!group.generationReady&&!group.generationWaits)){aiWorkflowMessage="Original peer context is unavailable; Generate again";return false;}
        waits=!marker->current||!group.generationReady;
    }
    if(!aiAccount||!aiAccountReady(aiAccount->selectedModel)){aiWorkflowMessage="Refine needs a connected account and discovered selected model";return false;}
    if(waits){groupControl->waiting=GroupControl::WaitingPress{true,{instruction,{},{}}};aiWorkflowMessage="Refine waits for the group heartbeat";return true;}
    groupControl->waiting.reset();
    const auto request=beginAiRefinement(instruction,aiAccount->selectedModel);
    if(!request){aiWorkflowMessage="Refine needs a current proposal and a valid instruction";return false;}
    if(marker)groupControl->parent=marker;
    const bool submitted=submitAiGeneration(*request,marker?&marker->selection:nullptr,marker?&marker->tag:nullptr);
    if(!submitted){const std::lock_guard lock(restoreMutex);failAiOwned("Companion rejected refinement");return false;}
    if(marker)groupControl->marker=GroupControl::Marker{request->tag,marker->session,marker->selection};
    aiWorkflowMessage="Refining; previous proposal held while the child is pending";return true;
}
std::optional<ai::GenerationRequest> InstrumentProcessor::beginAiRefinement(const std::string& instruction,const juce::String& model)
try
{
    auto names=soundNamesInUse();
    const std::lock_guard lock(restoreMutex);
    retireMcpStageOwned();
    if(aiApplying||(mcpState&&(mcpState->stage||mcpState->preview))){aiWorkflowMessage="Busy: an edit owns this instance";return {};}
    if((drumImporter&&drumImporter->busy())||std::any_of(drumOwnership->preparations_.begin(),drumOwnership->preparations_.end(),[](const auto& input){return input.inputId!=0;})){aiWorkflowMessage="Busy: finish or cancel the current source session";return {};}
    if(manualPreviewBytes){aiWorkflowMessage="Busy: prepared source audition is draining";return {};}
    if(gesture->index!=parameterCount||aiContextRevision==std::numeric_limits<std::uint64_t>::max()||aiNextRequestId==std::numeric_limits<std::uint64_t>::max()
        ||aiProposalPhase()!=AiProposalPhase::proposed||!aiPayloads->request||!aiPayloads->result||!aiPayloads->result->candidate||!aiIntent||!aiSourceOriginal)return {};
    const auto capturedReset=resetGeneration.load();
    if(aiLocal&&(!aiLocalCurrentOwned()||aiLocal->rendering||!aiLocal->hasParent))return {};
    auto childStorage=std::make_unique<ai::GenerationRequest>(*aiPayloads->request);auto& child=*childStorage;
    child.source=*aiPayloads->result->candidate;child.tag.requestId=aiNextRequestId;child.instruction=instruction;child.model=model;
    child.policy.requirePattern=false;child.namesInUse=std::move(names);
    if (child.catalog) {
        if (model != aiPayloads->request->model) return {};
        child.source = *aiSourceOriginal; child.catalog->phase = ai::CatalogPhase::select;
        child.catalog->selectionParent.reset(); child.catalog->baseIntent = *aiIntent;
        child.catalog->inferenceCallsUsed = 0; child.catalog->providerStatus = rack::oauth::Status::ready; child.catalog->httpStatus = 0;
    }
    child.capturedPhrase=(aiLocal?(aiPayloads->result->candidate->pattern.noteCount||aiIntent->pattern.has_value()):bool(child.catalog)||aiPayloads->result->candidate->pattern.noteCount)
        ?std::optional<PatternDocument>(aiPayloads->result->candidate->pattern):std::nullopt;
    std::unique_ptr<ai::LocalPreviewContext> localChild,localBackup;
    juce::MemoryBlock wire;
    if(aiLocal){localChild=std::make_unique<ai::LocalPreviewContext>(*aiLocal->current);localBackup=std::make_unique<ai::LocalPreviewContext>(*aiLocal->current);
        localChild->sourceMap=child.catalog?aiLocal->rootMap:aiLocal->parentMap;
        if(!ai::digestPreviewMap(localChild->sourceMap,localChild->sourceMapDigest))return {};
        auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=child;input->local=*localChild;if(!companion::encodeLocalRequest(*input,wire))return {};
    }else if(!companion::encodeRequest(child,wire))return {};
    // Prepare all allocating request/intent copies before moving the admitted parent.
    auto backup=std::make_unique<RefinementBackup>();backup->request=*aiPayloads->request;backup->intent=*aiIntent;
    auto returned=std::make_unique<ai::GenerationRequest>(child);
#ifndef NDEBUG
    sourceCaptureTestPoint();
#endif
    if(child.structuralGeneration!=structuralGeneration.load()||child.tag.editRevision!=editRevision.load()||child.tag.contextRevision!=aiContextRevision||capturedReset!=resetGeneration.load())return {};
    backup->result=std::move(*aiPayloads->result);
    stopAiPreviewOwned();aiDelayedApply.reset();aiRefinementBackup=std::move(backup);aiPayloads->result.reset();aiPayloads->request=std::move(child);
    if(aiLocal){aiLocal->backupContext=std::move(localBackup);aiLocal->current=std::move(localChild);aiLocal->receipt.reset();aiLocal->receiptSent=false;
        aiLocal->lastJob=0;aiLocal->lastOrdinal=aiLocal->lastAttempt=0;aiLocal->cancel.store(false,std::memory_order_release);}
    aiPhase=AiProposalPhase::pending;++aiNextRequestId;return std::move(*returned);
}
catch(const std::bad_alloc&){return {};}
void InstrumentProcessor::failAiOwned(const juce::String& message)
{
    libraryRuntime->client->cancelPlacement(placementPreparation);
    // Caller holds restoreMutex. A failed child can restore only original authority.
    stopAiPreviewOwned();aiDelayedApply.reset();
    if(aiRefinementBackup&&(!aiLocal||aiLocalCurrentOwned())&&aiPayloads->request&&aiPayloads->request->tag.editRevision==editRevision.load()&&aiPayloads->request->tag.contextRevision==aiContextRevision&&aiPayloads->request->structuralGeneration==structuralGeneration.load()
        &&(!groupControl->marker||(groupControl->parent&&groupControl->parent->current&&groupMarkerCurrentOwned(*groupControl->parent))))
    {
        auto restoredIntent=std::make_unique<ai::ProposalEdits>(std::move(aiRefinementBackup->intent));
        aiPayloads->request=std::move(aiRefinementBackup->request);aiPayloads->result=std::move(aiRefinementBackup->result);aiIntent=std::move(restoredIntent);
        aiRefinementBackup.reset();restoreAiLocalParentOwned();aiPhase=AiProposalPhase::proposed;aiWorkflowMessage=message+"; previous proposal retained";
        if(groupControl->parent){groupControl->marker=groupControl->parent;groupControl->marker->proposal=&*aiPayloads->result;groupControl->parent.reset();}
    }
    else {retireAiLocalOwned();aiRefinementBackup.reset();aiPayloads->result.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiRootCatalogSnapshot.reset();aiRootLegacyCatalogSnapshot.reset();aiPhase=AiProposalPhase::failed;aiWorkflowMessage=message;}
}
void InstrumentProcessor::pollAi()
{
    if(groupControl->pumping)return;
    struct PumpExit
    {
        bool& flag;std::function<void()>& notify;
        ~PumpExit(){const auto current=notify;if(current)current();flag=false;}
    } exit{groupControl->pumping,groupControl->refreshPanel};groupControl->pumping=true;
    SoundDocument observed;std::uint64_t observedEdit;
    (void)trySoundSnapshot(observed,observedEdit);
    reconcileGroupConnection();
    aiClient.poll();refreshAiProposal();pollAiLocal();
    const auto connection=aiClient.status();
    if(connection.phase==companion::ClientPhase::failed)
    {
        const bool grouped=bool(groupControl->marker)||groupControl->stale;pollGroup();
        const std::lock_guard lock(restoreMutex);retireAiLocalOwned();stopAiPreviewOwned();aiStartupAccount.reset();aiActiveAccount.reset();aiDelayedApply.reset();
        aiRefinementBackup.reset();aiPayloads->result.reset();aiPayloads->request.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiPhase=grouped?AiProposalPhase::stale:AiProposalPhase::failed;
        aiAccount.reset();aiModelsDiscovered=false;aiAccountSession=juce::Uuid::null();aiVerifiedModel.clear();
        aiWorkflowMessage="Companion unavailable ("+connection.errorCode+"); manual sound available. Retry Account";return;
    }
    if(connection.phase==companion::ClientPhase::ready&&aiAccountSession!=aiClient.session())
    {aiAccount.reset();aiModelsDiscovered=false;aiVerifiedModel.clear();aiAccountSession=aiClient.session();}
    if(connection.phase==companion::ClientPhase::ready&&aiStartupAccount)
    {if(aiClient.account(*aiStartupAccount)){aiActiveAccount=std::move(aiStartupAccount);aiStartupAccount.reset();}}
    companion::AccountReply reply;
    if(aiClient.takeAccount(reply))
    {
        const auto action=aiActiveAccount?aiActiveAccount->action:companion::AccountAction::status;aiActiveAccount.reset();
        if(aiAccount&&reply.activeAccount!=aiAccount->activeAccount){aiModelsDiscovered=false;aiVerifiedModel.clear();}
        const bool connected=reply.status==companion::RuntimeStatus::ready&&reply.providerStatus==rack::oauth::Status::connected;
        if(connected&&action!=companion::AccountAction::models&&reply.models.empty()&&aiModelsDiscovered&&aiAccount)reply.models=std::move(aiAccount->models);
        if(!connected){aiModelsDiscovered=false;aiVerifiedModel.clear();}
        if(action==companion::AccountAction::models)aiModelsDiscovered=connected;
        aiAccount=std::move(reply);
        const bool discovery=action==companion::AccountAction::models||action==companion::AccountAction::selectModel;
        if(connected&&!aiModelsDiscovered&&action!=companion::AccountAction::models)
        {aiStartupAccount=companion::AccountCommand{companion::AccountAction::models,{}};aiWorkflowMessage="Discovering public models";}
        // Discovery renews the expired access token through the saved refresh token; status never does.
        else if((action==companion::AccountAction::status||action==companion::AccountAction::selectAccount)&&aiAccount->status==companion::RuntimeStatus::ready&&aiAccount->providerStatus==rack::oauth::Status::accessExpired)
        {aiStartupAccount=companion::AccountCommand{companion::AccountAction::models,{}};aiWorkflowMessage="Renewing sign-in";}
        else if(action==companion::AccountAction::selectAccount&&aiAccount->providerStatus==rack::oauth::Status::signedOut)
        {aiStartupAccount=companion::AccountCommand{companion::AccountAction::signIn,{}};aiWorkflowMessage="Sign in to the selected account";}
        // A refused refresh token clears the saved sign-in, which the next status reports as signed out.
        else if(discovery&&aiAccount->providerStatus==rack::oauth::Status::tokenFailure&&(aiAccount->errorCode=="invalid_grant"||aiAccount->errorCode=="invalid_client"))
        {aiStartupAccount=companion::AccountCommand{companion::AccountAction::status,{}};aiWorkflowMessage="Sign-in refused; checking account";}
        else if(!connected)
        {
            switch(aiAccount->providerStatus)
            {
                case rack::oauth::Status::signedOut: aiWorkflowMessage="Signed out; click Sign in";break;
                case rack::oauth::Status::accessExpired: aiWorkflowMessage="Account expired; click Sign in";break;
                case rack::oauth::Status::tokenFailure: aiWorkflowMessage="Air could not complete sign-in. Check your browser, then choose an account.";break;
                case rack::oauth::Status::expiredAttempt: aiWorkflowMessage="Sign-in did not return to air. Check your browser, or choose an account.";break;
                case rack::oauth::Status::inferenceNotGranted: aiWorkflowMessage="Sound generation is unavailable for this account. Manual controls stay available.";break;
                case rack::oauth::Status::denied: aiWorkflowMessage="Permission was not granted. Choose an account or continue manually.";break;
                case rack::oauth::Status::busy: aiWorkflowMessage="Account busy; retry Account";break;
                default:aiWorkflowMessage="Account unavailable or busy; retry Account ("+aiAccount->errorCode+")";break;
            }
        }
        else if(!aiAccountReady(aiAccount->selectedModel))aiWorkflowMessage="Choose a selected model from the discovered public list";
        else aiWorkflowMessage=aiAccount->modelVerified||aiVerifiedModel==aiAccount->selectedModel?"Connected; model compatibility verified":"Connected; model compatibility unverified. An explicit trial is available";
    }
    companion::ClientGenerationReply outcome;
    if(aiClient.takeGeneration(outcome))
    {
        if(auto* result=std::get_if<ai::GenerationResult>(&outcome))
        {
            const bool selection = result->status == ai::GenerationStatus::catalogSelectionRequired;
            const auto intermediate = result->tag;
            const auto model=aiPayloads->request?aiPayloads->request->model:juce::String{};
            if(receiveAiResult(std::move(*result))) {
                if (selection) { groupControl->followupParent = intermediate; submitCatalogFollowup(); }
                else {aiVerifiedModel=model;aiWorkflowMessage="Proposal ready; model compatibility verified in this session";}
            } else if (selection) aiClient.close();
        }
        else if(auto* local=std::get_if<ai::LocalGenerationResult>(&outcome))
        {
            const bool selection=local->numeric.status==ai::GenerationStatus::catalogSelectionRequired;
            const auto intermediate=local->numeric.tag;const auto model=aiPayloads->request?aiPayloads->request->model:juce::String{};
            if(receiveAiLocalResult(std::move(*local))){if(selection){groupControl->followupParent=intermediate;submitCatalogFollowup();}
                else{aiVerifiedModel=model;aiWorkflowMessage="Proposal ready; local sample preview verified";}}
            else{
                if(selection)aiClient.close();
                const std::lock_guard lock(restoreMutex);
                if(aiPhase==AiProposalPhase::pending&&aiPayloads->request&&aiPayloads->request->tag==intermediate){
                    // The consumed terminal has settled its job; retain parent authority for retry.
                    failAiOwned("Local preview result could not be retained");}
            }
        }
        else
        {
            const auto& control=std::get<companion::ControlMessage>(outcome);const std::lock_guard lock(restoreMutex);
            if(aiPayloads->request&&aiPhase==AiProposalPhase::pending&&control.tag==aiPayloads->request->tag)
            {
                if(groupControl->marker&&control.status==companion::RuntimeStatus::staleSession)
                {retireAiLocalOwned();stopAiPreviewOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiPayloads->result.reset();aiPhase=AiProposalPhase::stale;groupControl->stale=true;groupControl->marker.reset();groupControl->parent.reset();}
                else failAiOwned("Companion rejected request (status "+juce::String(static_cast<int>(control.status))+")");
            }
        }
    }
    if(aiDelayedApply)
    {
        const auto captured=*aiDelayedApply;
        if(aiProposalPhase()!=AiProposalPhase::proposed||!aiPayloads->request||!aiPayloads->result||aiPayloads->request->tag!=captured.tag
            ||&*aiPayloads->result!=captured.proposal){aiDelayedApply.reset();aiWorkflowMessage="Apply cancelled because the proposal changed";}
        else if(renderedPreview.stopped()&&!groupControl->marker)
        {
            aiDelayedApply.reset();
            aiWorkflowMessage=applyAiProposal()?"Applied as one undoable edit":"Apply rejected; stop preview and retry";
        }
        else aiWorkflowMessage="Stopping preview; Apply waiting for audio";
    }
    pollGroup();
}
void InstrumentProcessor::cancelAi()
{
    aiClient.cancelGeneration();aiClient.cancelAccount();aiStartupAccount.reset();aiActiveAccount.reset();discardAiProposal();
    if(aiClient.status().phase==companion::ClientPhase::closed){aiAccount.reset();aiModelsDiscovered=false;aiVerifiedModel.clear();aiAccountSession=juce::Uuid::null();}
    aiWorkflowMessage="Cancelled; committed sound preserved";
}
void InstrumentProcessor::closeAiSession()
{
    stopTimer();groupControl->explicitConnection=false;groupControl->discovery=false;
    cancelAi();aiClient.close();aiAccount.reset();aiModelsDiscovered=false;aiVerifiedModel.clear();aiAccountSession=juce::Uuid::null();
    groupControl->pending.reset();{const std::lock_guard lock(restoreMutex);releaseAiGroupPublicationChargeOwned();}groupControl->admitted=false;groupControl->session=juce::Uuid::null();groupControl->dirty=true;groupControl->members.clear();groupCompanionDisconnected();
    aiWorkflowMessage="AI session closed; manual sound available";
}
bool InstrumentProcessor::applyAiWhenStopped()
{
    const std::lock_guard lock(restoreMutex);
    if(previewOwner==PreviewOwner::manual){aiWorkflowMessage="Manual source audition owns preview; stop it before Apply";return false;}
    if(aiApplying||gesture->index!=parameterCount||aiProposalPhase()!=AiProposalPhase::proposed||!aiPayloads->request||!aiPayloads->result||aiPayloads->result->status!=ai::GenerationStatus::ready
        ||(groupControl->marker&&!groupMarkerCurrentOwned(*groupControl->marker)))return false;
    aiDelayedApply=DelayedApply{aiPayloads->request->tag,&*aiPayloads->result};stopAiPreviewOwned();
    aiWorkflowMessage=renderedPreview.stopped()?"Apply queued":"Stopping preview; Apply waiting for audio";return true;
}
juce::Uuid InstrumentProcessor::aiRuntime() const noexcept {return aiRuntimeIdentity;}
AiMusicalContext InstrumentProcessor::aiMusicalContext() const
{
    const std::lock_guard lock(restoreMutex);return aiContext;
}
bool InstrumentProcessor::setAiMusicalContext(const AiMusicalContext& context)
{
    if(!validContextText(context.role)||!validContextText(context.key)||!validContextText(context.scale)
        ||(context.auditionBpm&&!validTempo(*context.auditionBpm)))return false;
    bool changed=false,accepted=false;
    {
        const std::lock_guard lock(restoreMutex);
        if(const auto previous=groupLinkState.view().intent)
        {
            auto intent=*previous;
            if(!setGroupText(context.role,intent.role)||!setGroupText(context.key,intent.key)||!setGroupText(context.scale,intent.scale))return false;
            intent.bpm=context.auditionBpm;accepted=setGroupIntentOwned(intent);
            changed=groupLinkState.view().intent!=previous;
        }
        else
        {
            if(context==aiContext)return true;
            if(aiContextRevision==std::numeric_limits<std::uint64_t>::max())return false;
            aiContext=context;invalidateAiContextOwned();accepted=true;
        }
    }
    if(changed)updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    return accepted;
}
void InstrumentProcessor::invalidateAiContextOwned() noexcept
{
    libraryRuntime->client->cancelPlacement(placementPreparation);
    aiRootCatalogSnapshot.reset();aiRootLegacyCatalogSnapshot.reset();
    retireAiLocalOwned();stopAiPreviewOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiPayloads->result.reset();
    if(aiPhase==AiProposalPhase::pending||aiPhase==AiProposalPhase::proposed)aiPhase=AiProposalPhase::stale;
    if(aiContextRevision!=std::numeric_limits<std::uint64_t>::max())++aiContextRevision;
}
std::optional<ai::GenerationRequest> InstrumentProcessor::beginAiRequest(const AiRequestOptions& options,
                                                                      std::optional<double> observedBpm)
try
{
    refreshAiProposal();
    {
        const std::lock_guard lock(restoreMutex);
        if((drumImporter&&drumImporter->busy())||std::any_of(drumOwnership->preparations_.begin(),drumOwnership->preparations_.end(),[](const auto& input){return input.inputId!=0;})){aiWorkflowMessage="Busy: finish or cancel the current source session";return {};}
    }
    RackInventoryPtr inventory;
    const bool catalogAware = options.policy.scope != ai::RequestScope::pattern;
    if(catalogAware){
        RackLibraryStatus status;if(!captureAvailableRackInventory(inventory,&status)){
            libraryRuntime->client->retryInventory(inventoryPreparation);
            const std::lock_guard lock(restoreMutex);aiWorkflowMessage=status==RackLibraryStatus::byteLimit?"Generation refused: installed rack descriptor or inventory exceeds its byte limit":"Preparing available rack inventory";return {};
        }
    }
    return beginAiRequestWithCatalog(options, observedBpm, {},std::move(inventory));
}
catch(const std::bad_alloc&){return {};}
std::optional<ai::GenerationRequest> InstrumentProcessor::beginAiRequestWithCatalog(const AiRequestOptions& options,
    std::optional<double> observedBpm, std::shared_ptr<const RackCatalogSnapshot> legacy,RackInventoryPtr inventory)
try
{
    const bool catalogAware = options.policy.scope != ai::RequestScope::pattern;
    juce::var descriptor;
    if(catalogAware&&!(inventory?ai::catalogInventoryFor(*inventory,ai::CatalogRendering::ownedLocal,descriptor):legacy&&ai::catalogInventoryFor(*legacy,ai::CatalogRendering::ownedLocal,descriptor))){
        const std::lock_guard lock(restoreMutex);aiWorkflowMessage="Generation refused: rack catalog descriptor exceeds its byte limit";return {};
    }
    bool inventoryDrums=false;
    if(inventory){if(const auto* rows=descriptor["entries"].getArray())for(const auto& row:*rows)if(const auto* modules=row[8].getArray())for(const auto& m:*modules)inventoryDrums|=int(m)==int(NativeModule::drum);}
    else if(legacy)inventoryDrums=std::any_of(legacy->entries.begin(),legacy->entries.end(),[](const auto& entry){return std::any_of(entry.definition.units.begin(),entry.definition.units.end(),[](const auto& unit){return unit.module==NativeModule::drum;});});
    auto names=soundNamesInUse();
    const std::lock_guard lock(restoreMutex);
    retireMcpStageOwned();
    if(aiApplying||(mcpState&&(mcpState->stage||mcpState->preview))){aiWorkflowMessage="Busy: an edit owns this instance";return {};}
    if(gesture->index!=parameterCount||aiContextRevision==std::numeric_limits<std::uint64_t>::max()||aiNextRequestId==std::numeric_limits<std::uint64_t>::max())return {};
    if((drumImporter&&drumImporter->busy())||std::any_of(drumOwnership->preparations_.begin(),drumOwnership->preparations_.end(),[](const auto& input){return input.inputId!=0;})){aiWorkflowMessage="Busy: finish or cancel the current source session";return {};}
    if(manualPreviewBytes){aiWorkflowMessage="Busy: prepared source audition is draining";return {};}
    drainAiLocalOwned();if(aiLocal&&(aiLocal->rendering||aiLocal->retiring||aiPhase==AiProposalPhase::pending)){aiWorkflowMessage="Busy: local preview work is draining";return {};}
    const bool localRequired=(structuralGeneration.load()&0xf0u)!=0||inventoryDrums;
    auto local=localRequired?allocateAiLocalStateOwned():nullptr;if(localRequired&&!local)return {};
    auto requestStorage=std::make_unique<ai::GenerationRequest>();auto& request=*requestStorage;std::uint64_t edit,reset;request.namesInUse=std::move(names);
    if(!tryProjectSnapshotOwned(request.source,edit,reset))return {};
    request.structuralGeneration=observedStructuralGeneration;if(request.structuralGeneration!=structuralGeneration.load())return {};
    request.tag={aiRuntimeIdentity,aiNextRequestId,edit,aiContextRevision};
    request.policy=options.policy;
    request.model=options.model;request.instruction=options.instruction;
    request.role=aiContext.role;request.key=aiContext.key;request.scale=aiContext.scale;
    request.capturedBpm=aiContext.auditionBpm;
    if(!request.capturedBpm&&observedBpm&&validTempo(*observedBpm))request.capturedBpm=observedBpm;
    if(request.source.pattern.noteCount!=0)request.capturedPhrase=request.source.pattern;
    if (catalogAware) {
        if (request.source.sound.personalities) for (const auto& i : request.source.sound.personalities->instances) {
            juce::MemoryBlock bytes; if (!encodeRackDesign(i.design, bytes, RackValidationMode::instanceRecall)) return {};
            const auto hash = juce::SHA256(bytes).toHexString();
            if(inventory){RackDesign exact;std::optional<RackPackageRef> ref;if(!resolveInventoryDesign(*inventory,i.design.id,i.design.revision,hash,exact,ref)||exact!=i.design)return {};}
            else if(!legacy||std::none_of(legacy->entries.begin(),legacy->entries.end(),[&](const auto& e){return e.definition==i.design&&e.contentSha256==hash;}))return {};
        }
        auto authority = std::make_shared<ai::CatalogAuthority>(); authority->root = request.tag; authority->snapshot = inventory?inventory->id():legacy->id;
        authority->rendering=localRequired?ai::CatalogRendering::ownedLocal:ai::CatalogRendering::companionPcm;
        authority->descriptorJson = juce::JSON::toString(descriptor, true, 22).toStdString();
        ai::CatalogRequestContext context; context.root = request.tag; context.authority = std::move(authority); request.catalog = std::move(context);
    }
    juce::MemoryBlock encoded;if(!localRequired&&!companion::encodeRequest(request,encoded))return {};
    // Prepare the original authority before publishing the new request.
    auto original=std::make_unique<ProjectDocument>(request.source);
    auto returned=std::make_unique<ai::GenerationRequest>(request);
#ifndef NDEBUG
    sourceCaptureTestPoint();
#endif
    if(request.structuralGeneration!=structuralGeneration.load()||request.tag.editRevision!=editRevision.load()
        ||request.tag.contextRevision!=aiContextRevision||reset!=resetGeneration.load())return {};
    if(localRequired){clearStoppedPreviewOwned();local=prepareAiLocalRootOwned(request,reset,std::move(local));if(!local)return {};}
    stopAiPreviewOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal=std::move(original);aiOriginalPolicy=request.policy;
    aiRootCatalogSnapshot = request.catalog ? std::move(inventory) : nullptr;
    aiRootLegacyCatalogSnapshot=request.catalog?std::move(legacy):nullptr;
    libraryRuntime->client->cancelPlacement(placementPreparation);
    aiPayloads->request=std::move(request);aiPayloads->result.reset();replaceAiLocalRootOwned(std::move(local));aiPhase=AiProposalPhase::pending;++aiNextRequestId;
    logAi(*aiPayloads->request,diagnostics::Stage::capture);
    groupControl->marker.reset();groupControl->parent.reset();groupControl->applyToken.reset();groupControl->stale=false;
    return std::move(*returned);
}
catch(const std::bad_alloc&){return {};}
bool InstrumentProcessor::receiveAiResult(ai::GenerationResult&& result)
{
    const std::lock_guard lock(restoreMutex);if(aiLocal)return false;return receiveAiResultOwned(std::move(result));
}
bool InstrumentProcessor::receiveAiResultOwned(ai::GenerationResult&& result,bool localValidated)
{
    const auto capturedReset=resetGeneration.load();
    if(!aiPayloads->request||aiPhase!=AiProposalPhase::pending||result.tag!=aiPayloads->request->tag)return false;
    if(aiPayloads->request->tag.editRevision!=editRevision.load()||aiPayloads->request->tag.contextRevision!=aiContextRevision||aiPayloads->request->structuralGeneration!=structuralGeneration.load())
    {retireAiLocalOwned();stopAiPreviewOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiPayloads->result.reset();aiPhase=AiProposalPhase::stale;return false;}
    juce::MemoryBlock encoded;
    const bool valid=localValidated?ai::validateGenerationResult(*aiPayloads->request,result):companion::encodeResult(*aiPayloads->request,result,encoded);
    logAi(*aiPayloads->request,diagnostics::Stage::terminal,&result,valid);
    if (valid && result.status == ai::GenerationStatus::catalogSelectionRequired) {
        if (!aiPayloads->request->catalog || (!aiRootCatalogSnapshot&&!aiRootLegacyCatalogSnapshot) || !result.catalog
            || result.catalog->root != aiPayloads->request->catalog->root || result.catalog->snapshot != (aiRootCatalogSnapshot?aiRootCatalogSnapshot->id():aiRootLegacyCatalogSnapshot->id)
            || aiNextRequestId == UINT64_MAX) { failAiOwned("Invalid captured catalog selection"); return false; }
        auto followup = *aiPayloads->request; auto authority = std::make_shared<ai::CatalogAuthority>(*aiPayloads->request->catalog->authority);
        for (const auto& selection : result.edits.catalogSelections) {
            RackDesign design;
            std::optional<RackPackageRef> package;
            if (!(aiRootCatalogSnapshot?resolveInventoryDesign(*aiRootCatalogSnapshot,selection.design,selection.revision,selection.contentSha256,design,package)
                :resolveCatalogDesign(*aiRootLegacyCatalogSnapshot,selection.design,selection.revision,selection.contentSha256,design))) {
                failAiOwned("Captured rack definition unavailable"); return false;
            }
            if (std::none_of(authority->definitions.begin(), authority->definitions.end(), [&](const auto& d) {
                return d.design == selection.design && d.revision == selection.revision && d.contentSha256 == selection.contentSha256;
            })) {
                ai::CatalogDefinition definition; definition.design = selection.design; definition.revision = selection.revision; definition.contentSha256 = selection.contentSha256;
                if (!encodeRackDesign(design, definition.canonical, RackValidationMode::instanceRecall)) { failAiOwned("Captured rack encoding failed"); return false; }
                authority->definitions.push_back(std::move(definition));
            }
        }
        followup.tag.requestId = aiNextRequestId; followup.catalog->phase = ai::CatalogPhase::materialize;
        followup.catalog->selectionParent = result.tag; followup.catalog->authority = std::move(authority);
        followup.catalog->baseIntent = result.edits; followup.catalog->inferenceCallsUsed = std::uint8_t(result.inferenceCalls);
        followup.catalog->providerStatus = result.providerStatus; followup.catalog->httpStatus = result.httpStatus;
        juce::MemoryBlock wire;
        bool encodedFollowup=false;
        if(aiLocal){auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=followup;input->local=*aiLocal->current;encodedFollowup=companion::encodeLocalRequest(*input,wire);}
        else encodedFollowup=companion::encodeRequest(followup,wire);
        if (!encodedFollowup) { failAiOwned("Captured selection exceeds request bounds"); return false; }
#ifndef NDEBUG
        sourceCaptureTestPoint();
#endif
        if(followup.structuralGeneration!=structuralGeneration.load()||followup.tag.editRevision!=editRevision.load()
            ||followup.tag.contextRevision!=aiContextRevision||capturedReset!=resetGeneration.load())return false;
        aiPayloads->request = std::move(followup); ++aiNextRequestId; aiWorkflowMessage = "Resolving captured selections"; return true;
    }
    if(!valid||result.status!=ai::GenerationStatus::ready)
    {
        if(valid&&result.errorCode=="account_changed"){
            aiAccount.reset();aiModelsDiscovered=false;aiVerifiedModel.clear();
            failAiOwned("The account changed in another air window. Check Account, then generate again.");return false;
        }
        auto message="Generation failed: "+juce::String(valid?generationName(result.status):"invalid_result")
            +"; "+providerName(result.providerStatus);
        if(valid&&result.status==ai::GenerationStatus::budgetExhausted&&result.proposalStatus==ai::ProposalStatus::malformed)message+="; proposal malformed after "+juce::String(result.inferenceCalls)+" attempts";
        if(valid&&result.status==ai::GenerationStatus::budgetExhausted&&result.proposalStatus==ai::ProposalStatus::outOfReach)message+="; proposal outside the rack controls after "+juce::String(result.inferenceCalls)+" attempts";
        if(valid&&result.httpStatus)message+="; HTTP "+juce::String(result.httpStatus);
        const auto code=diagnostics::safeErrorCode(result.errorCode);if(code.isNotEmpty())message+="; "+code;
        message+="; request "+juce::String(result.tag.requestId)+". Logs: "+diagnostics::directory().getFullPathName();
        failAiOwned(message);return false;
    }
    auto aggregate=aiRefinementBackup&&aiIntent?*aiIntent:ai::ProposalEdits{};
    if (aiPayloads->request->catalog) aggregate = result.edits;
    for(const auto& edit:result.edits.parameters)
    {const auto i=std::find_if(aggregate.parameters.begin(),aggregate.parameters.end(),[&](const auto& old){return old.id==edit.id;});if(i==aggregate.parameters.end())aggregate.parameters.push_back(edit);else *i=edit;}
    for(const auto& edit:result.edits.macros)
    {const auto i=std::find_if(aggregate.macros.begin(),aggregate.macros.end(),[&](const auto& old){return old.index==edit.index;});if(i==aggregate.macros.end())aggregate.macros.push_back(edit);else *i=edit;}
    if(aggregate.parameters.size()>parameterCount||aggregate.macros.size()>8||aggregate.racks.size()+(aiPayloads->request->catalog?0:result.edits.racks.size())>64)
    {failAiOwned("Refinement exceeds cumulative edit bounds");return false;}
    if (!aiPayloads->request->catalog) aggregate.racks.insert(aggregate.racks.end(),result.edits.racks.begin(),result.edits.racks.end());
    if(result.edits.pattern)aggregate.pattern=result.edits.pattern;
    if(!aiPayloads->request->catalog && !mergeChoices(aggregate.reconciliation,result.edits.reconciliation)){failAiOwned("Refinement exceeds reconciliation bounds");return false;}
    aggregate.characteristics=result.edits.characteristics;aggregate.explanation=result.edits.explanation;
    if(result.edits.name.isNotEmpty())aggregate.name=result.edits.name;
    auto cumulativeTrace=aiLocal?std::make_unique<SourceAssignmentTrace>():nullptr;
    ProjectDocument admitted;auto cumulativePolicy=aiOriginalPolicy.value_or(ai::ProposalPolicy{});cumulativePolicy.requirePattern=aiPayloads->request->policy.requirePattern;
    if(!aiSourceOriginal||!aiOriginalPolicy||!result.candidate
        ||(aiPayloads->request->catalog ? (aiRootCatalogSnapshot?ai::admitProposal(*aiSourceOriginal,cumulativePolicy,aggregate,*aiPayloads->request->catalog->authority,*aiRootCatalogSnapshot,admitted,cumulativeTrace.get()):ai::admitProposal(*aiSourceOriginal,cumulativePolicy,aggregate,*aiPayloads->request->catalog->authority,admitted,cumulativeTrace.get()))
                              : ai::admitProposal(*aiSourceOriginal,cumulativePolicy,aggregate,admitted,cumulativeTrace.get()))!=ai::ProposalStatus::accepted)
    {failAiOwned("Refinement cannot be admitted against the original sound");return false;}
    if(admitted.pattern!=result.candidate->pattern||admitted.sound.values!=result.candidate->sound.values||admitted.sound.layout!=result.candidate->sound.layout){failAiOwned("Refinement changes original physical authority");return false;}
    if(aiLocal&&!validateAiLocalParentOwned(admitted,*cumulativeTrace)){failAiOwned("Local sample ownership does not match the original intent");return false;}
    result.candidate=admitted;
    auto admittedIntent=std::make_unique<ai::ProposalEdits>(std::move(aggregate));
#ifndef NDEBUG
    sourceCaptureTestPoint();
#endif
    if(aiPayloads->request->structuralGeneration!=structuralGeneration.load()||aiPayloads->request->tag.editRevision!=editRevision.load()
        ||aiPayloads->request->tag.contextRevision!=aiContextRevision||capturedReset!=resetGeneration.load())return false;
    aiIntent=std::move(admittedIntent);aiPayloads->result=std::move(result);aiRefinementBackup.reset();acceptAiLocalParentOwned();aiPhase=AiProposalPhase::proposed;
    if(groupControl->marker&&groupControl->marker->tag==aiPayloads->request->tag)
    {groupControl->marker->proposal=&*aiPayloads->result;groupControl->marker->current=false;groupControl->parent.reset();groupControl->checkTime=0;}
    return true;
}
const ai::GenerationResult* InstrumentProcessor::aiProposal() const noexcept
{
    return aiProposalPhase()==AiProposalPhase::proposed&&aiPayloads->result?&*aiPayloads->result:nullptr;
}
AiProposalPhase InstrumentProcessor::aiProposalPhase() const noexcept
{
    if(aiPayloads->request&&(aiPhase==AiProposalPhase::pending||aiPhase==AiProposalPhase::proposed)
        &&(aiPayloads->request->tag.editRevision!=editRevision.load()||aiPayloads->request->tag.contextRevision!=aiContextRevision||aiPayloads->request->structuralGeneration!=structuralGeneration.load()))
        return AiProposalPhase::stale;
    return aiPhase;
}
void InstrumentProcessor::refreshAiProposal()
{
    if(aiProposalPhase()==AiProposalPhase::stale){aiClient.cancelGeneration();libraryRuntime->client->cancelPlacement(placementPreparation);}
    const std::lock_guard lock(restoreMutex);
    if(aiProposalPhase()==AiProposalPhase::stale){retireAiLocalOwned();stopAiPreviewOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiRootCatalogSnapshot.reset();aiRootLegacyCatalogSnapshot.reset();aiPayloads->result.reset();aiPhase=AiProposalPhase::stale;aiWorkflowMessage="Proposal stale after a manual or context edit; Generate again";}
}
void InstrumentProcessor::discardAiProposal()
{
    libraryRuntime->client->cancelPlacement(placementPreparation);
    aiClient.cancelGeneration();
    const std::lock_guard lock(restoreMutex);aiRootCatalogSnapshot.reset();aiRootLegacyCatalogSnapshot.reset();retireAiLocalOwned();stopAiPreviewOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiPayloads->result.reset();aiPayloads->request.reset();aiPhase=AiProposalPhase::idle;
    groupControl->marker.reset();groupControl->parent.reset();groupControl->applyToken.reset();groupControl->stale=false;
    groupControl->waiting.reset();
}

bool InstrumentProcessor::applyAiProposal()
{
    const detail::ParameterListenerDispatch dispatch(*this);
    ai::RequestTag tag;ai::ProposalPolicy policy;ai::ProposalEdits edits;std::uint64_t proposalReset=0;
    std::shared_ptr<const ai::CatalogAuthority> authority;
    RackInventoryPtr inventory;std::shared_ptr<const RackPersonalityState> embedded;
    std::shared_ptr<const RackLibraryPlacementView> placement;
    const auto grouped=groupControl->marker;
    {
        const std::lock_guard lock(restoreMutex);
        if(!renderedPreview.stopped()||aiApplying||gesture->index!=parameterCount||aiProposalPhase()!=AiProposalPhase::proposed
            ||!aiPayloads->request||!aiPayloads->result||aiPayloads->result->tag!=aiPayloads->request->tag)return false;
        if(grouped)
        {
            const auto token=groupControl->applyToken;
            if(!token||token->tag!=grouped->tag||token->proposal!=&*aiPayloads->result||token->session!=grouped->session
                ||token->selection!=grouped->selection||!groupMarkerCurrentOwned(*grouped))return false;
        }
        if(!aiOriginalPolicy||aiPayloads->result->status!=ai::GenerationStatus::ready)return false;
        tag=aiPayloads->request->tag;policy=*aiOriginalPolicy;policy.requirePattern=aiPayloads->request->policy.requirePattern;edits=aiIntent?*aiIntent:aiPayloads->result->edits;aiApplying=true;
        if (aiPayloads->request->catalog) authority = aiPayloads->request->catalog->authority;
        inventory=aiRootCatalogSnapshot;embedded=controlPersonalities;proposalReset=resetGeneration.load();
    }
    bool prepared=true;RackLibraryStatus preparationStatus=RackLibraryStatus::busy;
    try{
        std::vector<RackPackageRef> refs;
        if(inventory)for(const auto& selection:edits.catalogSelections){
            RackDesign design;std::optional<RackPackageRef> ref;
            if(!resolveInventoryDesign(*inventory,selection.design,selection.revision,selection.contentSha256,design,ref)){prepared=false;break;}
            const bool retained=embedded&&std::any_of(embedded->instances.begin(),embedded->instances.end(),[&](const auto& rack){return rack.design==design;});
            if(ref&&!retained&&std::find(refs.begin(),refs.end(),*ref)==refs.end())refs.push_back(*ref);
        }
        if(prepared&&!refs.empty()){
            libraryRuntime->client->requestPlacement(placementPreparation,inventory,refs);placement=libraryRuntime->client->placement(placementPreparation);
            preparationStatus=placement?placement->status:RackLibraryStatus::busy;
            prepared=placement&&placement->root==inventory&&libraryRuntime->client->placementCurrent(placementPreparation,*placement);
        }
    }catch(const std::bad_alloc&){prepared=false;preparationStatus=RackLibraryStatus::byteLimit;}
    if(!prepared){const std::lock_guard lock(restoreMutex);aiApplying=false;aiWorkflowMessage=preparationStatus==RackLibraryStatus::busy?"Preparing exact installed rack releases":"Apply refused: exact installed rack release is unavailable";return false;}
    if(grouped){const std::lock_guard lock(restoreMutex);groupControl->applyToken.reset();}
    juce::String name;
    if(edits.name.isNotEmpty())
    {
        const auto names=soundNamesInUse();
        const auto taken=[&](const juce::String& n){return std::any_of(names.begin(),names.end(),[&](const auto& u){return u.equalsIgnoreCase(n);});};
        name=edits.name;for(int n=2;taken(name);++n)name=edits.name+" "+juce::String(n);
    }
    std::array<bool,parameterCount> begun {},changed {};
    ProjectDocument before,after;
    bool applied=false;
    // Each field is announced at most once. Re-read/re-admit after every batch
    // of external begin callbacks; never replay unrelated host atomics.
    for(;;)
    {
        std::array<std::size_t,parameterCount> additions {};std::size_t count=0;
        bool rejected=false;
        {
            const std::lock_guard lock(restoreMutex);std::uint64_t edit,reset;
            std::unique_ptr<SourceAssignmentTrace> trace;
            try {trace=std::make_unique<SourceAssignmentTrace>();}catch(const std::bad_alloc&){rejected=true;}
            if(!trace||!renderedPreview.stopped()||gesture->index!=parameterCount||aiProposalPhase()!=AiProposalPhase::proposed
                ||!aiPayloads->request||!aiPayloads->result||aiPayloads->request->tag!=tag||aiPayloads->result->tag!=tag
                ||tag.runtime!=aiRuntimeIdentity||!tryProjectSnapshotOwned(before,edit,reset)
                ||edit!=tag.editRevision||reset!=proposalReset||aiContextRevision!=tag.contextRevision||aiPayloads->request->structuralGeneration!=structuralGeneration.load()
                ||aiRootCatalogSnapshot!=inventory||(placement&&!libraryRuntime->client->placementCurrent(placementPreparation,*placement))
                ||(grouped&&(!groupControl->marker||groupControl->marker->tag!=grouped->tag
                    ||groupControl->session!=grouped->session||groupControl->selection!=grouped->selection))
                ||(authority ? (inventory?ai::admitProposal(before,policy,edits,*authority,*inventory,after,trace.get()):ai::admitProposal(before,policy,edits,*authority,after,trace.get())) : ai::admitProposal(before,policy,edits,after,trace.get()))!=ai::ProposalStatus::accepted
                ||!validateAiLocalApplyOwned(before.sound,*trace))rejected=true;
            else
            {
                if(name.isNotEmpty())after.sound.name=name;
                for(std::size_t i=0;i<parameterCount;++i)
                {
                    changed[i]=before.sound.values[i]!=after.sound.values[i];
                    if(changed[i]&&!begun[i]){begun[i]=true;aiGestureReserved[i]=true;additions[count++]=i;}
                }
                if(count==0)
                {
                    juce::String preparedPath;
                    try {
                        if(name.isNotEmpty())preparedPath=soundFolderOwned().getChildFile(name+".airack").getFullPathName();
                        stagedAiPresetPath=name.isNotEmpty()?&preparedPath:nullptr;
                        applied=trace->count?performOwnedRackEdit(before,after,*trace,drumCommitStatus):performAiEdit(before,after);
                        stagedAiPresetPath=nullptr;
                    }catch(const std::bad_alloc&){stagedAiPresetPath=nullptr;applied=false;}
                    if(applied){retireAiLocalOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();aiOriginalPolicy.reset();aiRootCatalogSnapshot.reset();aiRootLegacyCatalogSnapshot.reset();aiPayloads->result.reset();aiPayloads->request.reset();aiPhase=AiProposalPhase::idle;}
                }
            }
        }
        if(rejected||count==0)break;
        for(std::size_t n=0;n<count;++n)controls[additions[n]]->beginChangeGesture();
    }
    if(applied)
        for(std::size_t i=0;i<parameterCount;++i)if(changed[i])
            controls[i]->sendValueChangedMessageToListeners(controls[i]->getValue());
    for(std::size_t i=0;i<parameterCount;++i)if(begun[i])controls[i]->endChangeGesture();
    {const std::lock_guard lock(restoreMutex);aiGestureReserved.fill(false);aiApplying=false;sealGroupCommitOwned();
        if(applied){groupControl->marker.reset();groupControl->parent.reset();groupControl->applyToken.reset();}}
    if(applied)libraryRuntime->client->cancelPlacement(placementPreparation);
    refreshAiProposal();return applied;
}
}

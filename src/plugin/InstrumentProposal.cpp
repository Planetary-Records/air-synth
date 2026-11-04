#include "InstrumentProcessor.h"
#include "ParameterListenerRetirement.h"
#include <algorithm>
namespace airack {
juce::String resolveProposalName(const juce::String& requested,const std::vector<juce::String>& names)
{
    if(requested.isEmpty())return {};
    const auto taken=[&](const juce::String& name){return std::any_of(names.begin(),names.end(),[&](const auto& used){return used.equalsIgnoreCase(name);});};
    auto name=requested;for(int n=2;taken(name);++n)name=requested+" "+juce::String(n);
    return name;
}
ProposalCommitResult InstrumentProcessor::commitProposal(const ProposalCommitInput& input,const std::function<bool()>& authorizeOwned)
{
    const detail::ParameterListenerDispatch dispatch(*this);
    ProposalCommitResult result;
    {
        const std::lock_guard lock(restoreMutex);
        if(aiApplying||!renderedPreview.stopped()||gesture->index!=parameterCount){result.status=mcp::Status::busy;return result;}
        if(!authorizeOwned()){result.status=mcp::Status::stale;return result;}
        aiApplying=true;
    }
    std::array<bool,parameterCount> begun{},changed{};
    try {
        const bool reviewed=input.tag.runtime!=aiRuntimeIdentity;
        juce::String name;
        if(reviewed)name=input.reviewedName;
        else if(input.edits.name.isNotEmpty())name=resolveProposalName(input.edits.name,soundNamesInUse());
        auto before=std::make_unique<ProjectDocument>(),after=std::make_unique<ProjectDocument>();
        for(;;){
            std::array<std::size_t,parameterCount> additions{};std::size_t count=0;bool rejected=false;
            // Read native names outside restoreMutex, including after reentrant begin callbacks.
            const bool nameCurrent=!reviewed||(input.edits.name.isEmpty()?name.isEmpty():name.isNotEmpty()&&resolveProposalName(name,soundNamesInUse())==name);
            {
                const std::lock_guard lock(restoreMutex);std::uint64_t edit=0,reset=0;
                auto trace=std::make_unique<SourceAssignmentTrace>();
                if(!nameCurrent||!renderedPreview.stopped()||gesture->index!=parameterCount||!authorizeOwned()
                    ||!tryProjectSnapshotOwned(*before,edit,reset)||edit!=input.tag.editRevision||reset!=input.resetGeneration
                    ||aiContextRevision!=input.tag.contextRevision||structuralGeneration.load()!=input.structuralGeneration
                    ||(input.inventory&&input.inventory->storeGeneration()!=libraryRuntime->store->observedGeneration())
                    ||(input.placement&&(!input.placementPreparation||!libraryRuntime->client->placementCurrent(input.placementPreparation,*input.placement)))){
                    result.status=mcp::Status::stale;rejected=true;
                }else if((input.authority?(input.inventory?ai::admitProposal(*before,input.policy,input.edits,*input.authority,*input.inventory,*after,trace.get()):ai::admitProposal(*before,input.policy,input.edits,*input.authority,*after,trace.get()))
                    :ai::admitProposal(*before,input.policy,input.edits,*after,trace.get()))!=ai::ProposalStatus::accepted){
                    result.status=mcp::Status::invalidInput;rejected=true;
                }else if(input.tag.runtime==aiRuntimeIdentity&&!validateAiLocalApplyOwned(before->sound,*trace)){
                    result.status=mcp::Status::stale;rejected=true;
                }else{
                    if(name.isNotEmpty())after->sound.name=name;
                    for(std::size_t i=0;i<parameterCount;++i){changed[i]=before->sound.values[i]!=after->sound.values[i];
                        if(changed[i]&&!begun[i]){begun[i]=true;aiGestureReserved[i]=true;additions[count++]=i;}}
                    if(count==0){
                        if(*before!=*after&&editHistory->nextActionId==UINT64_MAX){result.status=mcp::Status::capacity;rejected=true;break;}
                        if(!authorizeOwned()){result.status=mcp::Status::stale;rejected=true;break;}
                        juce::String preparedPath;if(name.isNotEmpty())preparedPath=soundFolderOwned().getChildFile(name+".airack").getFullPathName();
                        stagedAiPresetPath=name.isNotEmpty()?&preparedPath:nullptr;
                        const bool applied=trace->count?performOwnedRackEdit(*before,*after,*trace,drumCommitStatus):performAiEdit(*before,*after);
                        stagedAiPresetPath=nullptr;result.status=applied?mcp::Status::ready:mcp::Status::failed;
                        result.changed=applied&&*before!=*after;result.editRevision=editRevision.load();
                        if(result.changed&&editHistory->cursor)result.historyId=editHistory->actionIds[editHistory->cursor-1];
                        if(applied&&input.tag.runtime==aiRuntimeIdentity){
                            retireAiLocalOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();
                            aiOriginalPolicy.reset();aiPayloads->result.reset();aiPayloads->request.reset();aiPhase=AiProposalPhase::idle;
                        }
                    }
                }
            }
            if(rejected||count==0)break;
            // Begin callbacks may change the sound or authority. Re-read and re-admit before publishing.
            for(std::size_t n=0;n<count;++n)controls[additions[n]]->beginChangeGesture();
        }
    }catch(const std::bad_alloc&){stagedAiPresetPath=nullptr;result.status=mcp::Status::capacity;}
    if(result.status==mcp::Status::ready)for(std::size_t i=0;i<parameterCount;++i)if(changed[i])controls[i]->sendValueChangedMessageToListeners(controls[i]->getValue());
    for(std::size_t i=0;i<parameterCount;++i)if(begun[i])controls[i]->endChangeGesture();
    {const std::lock_guard lock(restoreMutex);aiGestureReserved.fill(false);aiApplying=false;sealGroupCommitOwned();
        if(result.status==mcp::Status::ready&&input.tag.runtime==aiRuntimeIdentity){groupControl->marker.reset();groupControl->parent.reset();groupControl->applyToken.reset();}}
    return result;
}
}

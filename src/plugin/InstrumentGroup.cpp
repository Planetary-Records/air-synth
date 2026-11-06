#include "InstrumentProcessor.h"
#include <limits>
#include <algorithm>
#include <chrono>
namespace airack
{
void InstrumentProcessor::AtomicGroup::store(const std::optional<GroupIntent>& intent) noexcept
{
    present.store(intent.has_value());if(!intent)return;
    for(std::size_t i=0;i<16;++i){identities[i].store(intent->group.bytes[i]);identities[i+16].store(intent->member.bytes[i]);}
    const GroupText* fields[]{&intent->name,&intent->role,&intent->key,&intent->scale};
    for(std::size_t field=0;field<4;++field)
    {
        lengths[field].store(fields[field]->length);
        for(std::size_t i=0;i<maximumGroupTextBytes;++i)texts[field][i].store(fields[field]->bytes[i]);
    }
    hasBpm.store(intent->bpm.has_value());bpm.store(intent->bpm.value_or(0));
}
void InstrumentProcessor::AtomicGroup::load(ProjectDocument& document) const noexcept
{
    document.group.reset();if(!present.load())return;
    GroupIntent intent;
    for(std::size_t i=0;i<16;++i){intent.group.bytes[i]=identities[i].load();intent.member.bytes[i]=identities[i+16].load();}
    GroupText* fields[]{&intent.name,&intent.role,&intent.key,&intent.scale};
    for(std::size_t field=0;field<4;++field)
    {
        fields[field]->length=lengths[field].load();
        for(std::size_t i=0;i<maximumGroupTextBytes;++i)fields[field]->bytes[i]=texts[field][i].load();
    }
    if(hasBpm.load())intent.bpm=bpm.load();document.group=intent;
}
GroupLinkView InstrumentProcessor::groupLinkView() const
{const std::lock_guard lock(restoreMutex);return groupLinkState.view();}
namespace {
constexpr std::uint64_t localGroupPublicationReservation=2*(8*(20+std::uint64_t(companion::maximumControlPayloadBytes))+4096)+65536;
AiMusicalContext prepareGroupContext(const std::optional<GroupIntent>& intent)
{
    AiMusicalContext context;if(!intent)return context;
    context.role=groupTextView(intent->role);context.key=groupTextView(intent->key);
    context.scale=groupTextView(intent->scale);context.auditionBpm=intent->bpm;
    return context;
}
}
void InstrumentProcessor::groupMusicalContextOwned(AiMusicalContext&& context) noexcept
{
    aiContext=std::move(context);invalidateAiContextOwned();
}
bool InstrumentProcessor::publishGroupOwned(const std::optional<GroupIntent>& intent)
{
    ProjectDocument live;std::uint64_t edit;
    if(!tryProjectSnapshotOwned(live,edit)||!validateProjectDocument(live))live=*recoveryDocument;
    live.group=intent;const std::array<bool,parameterCount> values{};
    return publish(live,false,false,false,&values);
}
bool InstrumentProcessor::setGroupIntentOwned(const GroupIntent& intent)
{
    if(!validateGroupIntent(intent))return false;
    const auto previous=groupLinkState.view();
    if(groupLinkState.eligible()&&previous.intent==intent)return true;
    if(aiContextRevision==std::numeric_limits<std::uint64_t>::max())return false;
    AiMusicalContext context;
    try {context=prepareGroupContext(previous.exhausted||previous.loadRevision==std::numeric_limits<std::uint64_t>::max()
        ?previous.intent:std::optional<GroupIntent>(intent));}
    catch(const std::bad_alloc&){return false;}
    // Link cannot fail after these preconditions under restoreMutex. Commit the
    // saved intent first, so a stale publication preserves consent and context.
    if(!previous.exhausted&&previous.loadRevision!=std::numeric_limits<std::uint64_t>::max()
        &&previous.intent!=intent&&!publishGroupOwned(intent))return false;
    const auto status=groupLinkState.link(intent);const auto current=groupLinkState.view();
    if(previous!=current)
    {
        groupMusicalContextOwned(std::move(context));
    }
    return status==GroupLinkStatus::ready;
}
bool InstrumentProcessor::setGroupIntent(const GroupIntent& intent)
{
    bool changed,accepted;
    {
        const std::lock_guard lock(restoreMutex);const auto previous=groupLinkState.view().intent;
        accepted=setGroupIntentOwned(intent);changed=previous!=groupLinkState.view().intent;
    }
    if(changed)updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    return accepted;
}
bool InstrumentProcessor::resumeGroupIntent()
{
    const std::lock_guard lock(restoreMutex);
    if(groupLinkState.eligible())return true;
    const auto previous=groupLinkState.view();
    if(!previous.intent||aiContextRevision==std::numeric_limits<std::uint64_t>::max())return false;
    AiMusicalContext context;
    try {context=prepareGroupContext(previous.intent);}catch(const std::bad_alloc&){return false;}
    const auto status=groupLinkState.resume();const auto current=groupLinkState.view();
    if(previous!=current)groupMusicalContextOwned(std::move(context));
    return status==GroupLinkStatus::ready;
}
bool InstrumentProcessor::leaveGroupIntent()
{
    bool changed;
    {
        const std::lock_guard lock(restoreMutex);changed=groupLinkState.view().intent.has_value();
        if(changed&&!publishGroupOwned({}))return false;
        groupLinkState.leave();
        if(changed){groupMusicalContextOwned({});}
    }
    if(changed)updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    return true;
}
bool InstrumentProcessor::acknowledgeGroupIntent(GroupUuid runtime,std::uint64_t load,const GroupIntent& requested,const GroupIntent& normalized)
{
    bool changed=false;
    {
        const std::lock_guard lock(restoreMutex);const auto previous=groupLinkState.view().intent;
        if(previous!=normalized&&aiContextRevision==std::numeric_limits<std::uint64_t>::max())return false;
        const auto link=groupLinkState.view();auto permitted=requested;permitted.member=normalized.member;
        if(!validateGroupIntent(requested)||!validateGroupIntent(normalized)||!groupLinkState.eligible()
            ||runtime!=link.runtime||load!=link.loadRevision||previous!=requested||permitted!=normalized)return false;
        const auto current=std::optional<GroupIntent>(normalized);changed=previous!=current;
        AiMusicalContext context;
        try {if(changed)context=prepareGroupContext(current);}catch(const std::bad_alloc&){return false;}
        if(changed&&!publishGroupOwned(current))return false;
        // Under restoreMutex these validated reply fields and link authority
        // cannot change between saved-intent publication and admission.
        if(groupLinkState.acknowledge(runtime,load,requested,normalized)!=GroupLinkStatus::ready)return false;
        if(changed){groupMusicalContextOwned(std::move(context));}
    }
    if(changed)updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    return true;
}
void InstrumentProcessor::groupCompanionDisconnected()
{const std::lock_guard lock(restoreMutex);groupLinkState.disconnected();}
bool InstrumentProcessor::groupBusyOwned() const noexcept
{return gesture->index!=parameterCount||gesture->notifying||aiApplying;}
void InstrumentProcessor::sealGroupCommitOwned() noexcept
{
    if(!groupControl||groupBusyOwned()||!validateProjectDocument(*recoveryDocument))return;
    auto& g=*groupControl;
    if(g.committed!=*recoveryDocument||g.committedEdit!=editRevision.load())
    {g.committed=*recoveryDocument;g.committedEdit=editRevision.load();g.dirty=true;}
}
void InstrumentProcessor::releaseAiGroupPublicationChargeOwned() noexcept
{if(aiGroupPublicationBytes){drumOwnership->releaseDecoded(aiGroupPublicationBytes);aiGroupPublicationBytes=0;}}
GroupWorkflowView InstrumentProcessor::groupWorkflowView() const
{
    const std::lock_guard lock(restoreMutex);const auto& g=*groupControl;
    GroupWorkflowView view{groupLinkState.view(),aiClient.status(),g.members,g.selection};
    view.selection.loadRevision=view.link.loadRevision;view.busy=groupBusyOwned();view.operationPending=bool(g.pending);
    view.conflicted=g.conflicted||std::any_of(g.members.begin(),g.members.end(),[&](const auto& m)
        {return view.link.intent&&m.intent.group==view.link.intent->group&&m.conflicted;});bool available=true;
    for(std::size_t i=0;i<g.selection.peerCount;++i)
    {
        const auto peer=std::find_if(g.members.begin(),g.members.end(),[&](const auto& m){return view.link.intent
            &&m.intent.group==view.link.intent->group&&m.intent.member==g.selection.members[i]&&m.runtime!=view.link.runtime;});
        if(peer==g.members.end()||peer->busy||peer->conflicted)available=false;
    }
    const bool reachable=g.admitted&&g.session==aiClient.session()&&view.connection.phase==companion::ClientPhase::ready
        &&groupLinkState.eligible()&&!view.busy&&!view.conflicted&&available;
    const bool routine=!g.pending||g.purpose==GroupControl::Purpose::publish||g.purpose==GroupControl::Purpose::browse||g.purpose==GroupControl::Purpose::check||g.purpose==GroupControl::Purpose::spectrumPublish||g.purpose==GroupControl::Purpose::spectrumRead||g.purpose==GroupControl::Purpose::spectrumWithdraw;
    view.generationReady=reachable&&!g.dirty&&!g.publishedBusy&&!g.pending;
    view.generationWaits=reachable&&!view.generationReady&&routine;
    view.peerContextCurrent=g.marker&&g.marker->current&&groupMarkerCurrentOwned(*g.marker);
    view.message=g.message;return view;
}
bool InstrumentProcessor::groupMarkerCurrentOwned(const GroupControl::Marker& marker) const noexcept
{
    const auto view=groupLinkState.view();const auto& g=*groupControl;
    for(std::size_t i=0;i<marker.selection.peerCount;++i)
    {
        const auto peer=std::find_if(g.members.begin(),g.members.end(),[&](const auto& m){return view.intent
            &&m.intent.group==view.intent->group&&m.intent.member==marker.selection.members[i]&&m.runtime!=view.runtime;});
        if(peer==g.members.end()||peer->busy||peer->conflicted)return false;
    }
    return g.admitted&&groupLinkState.eligible()&&!groupBusyOwned()&&!g.conflicted
        &&marker.session==aiClient.session()&&marker.session==g.session&&marker.selection.loadRevision==view.loadRevision
        &&marker.selection==g.selection&&marker.tag.runtime==aiRuntimeIdentity
        &&marker.tag.editRevision==editRevision.load()&&marker.tag.contextRevision==aiContextRevision&&aiPayloads->request&&aiPayloads->request->structuralGeneration==structuralGeneration.load();
}
void InstrumentProcessor::invalidateGroupedProposal()
{
    auto& g=*groupControl;if(!g.marker&&!g.parent)return;
    aiClient.cancelGeneration();const std::lock_guard lock(restoreMutex);
    retireAiLocalOwned();stopAiPreviewOwned();aiDelayedApply.reset();aiRefinementBackup.reset();aiIntent.reset();aiSourceOriginal.reset();aiPayloads->result.reset();
    aiPhase=AiProposalPhase::stale;g.stale=true;g.marker.reset();g.parent.reset();g.applyToken.reset();g.message="Peer context changed; Generate again";
}
void InstrumentProcessor::resetGroupConnection()
{
    resetEqSpectrumConnection();refuseGroupPresses();invalidateGroupedProposal();aiClient.close();groupCompanionDisconnected();
    aiStartupAccount.reset();aiActiveAccount.reset();aiAccount.reset();aiModelsDiscovered=false;
    aiAccountSession=juce::Uuid::null();aiVerifiedModel.clear();
    const std::lock_guard lock(restoreMutex);auto& g=*groupControl;g.admitted=false;g.session=juce::Uuid::null();
    g.pending.reset();releaseAiGroupPublicationChargeOwned();g.members.clear();
    g.conflicted=false;g.dirty=true;g.publishedBusy=false;g.observationTime=g.browseTime=g.checkTime=0;
}
void InstrumentProcessor::reconcileGroupConnection()
{
    const auto current=groupLinkView();auto& g=*groupControl;
    if(current.runtime==g.processed.runtime&&current.loadRevision==g.processed.loadRevision&&current.intent==g.processed.intent)return;
    const bool differentGroup=!current.intent||!g.processed.intent||current.intent->group!=g.processed.intent->group;
    const bool groupOwner=current.intent||g.processed.intent||g.explicitConnection||g.admitted||g.pending||g.marker||g.parent;
    if(groupOwner)resetGroupConnection();
    if(differentGroup||current.phase==GroupLinkPhase::pending)g.selection={};
    g.selection.loadRevision=current.loadRevision;g.processed=current;
    if(!current.intent||current.phase==GroupLinkPhase::pending||current.exhausted)g.explicitConnection=false;
}
bool InstrumentProcessor::requestGroupConnection()
{
    reconcileGroupConnection();{const std::lock_guard lock(restoreMutex);if(!groupLinkState.eligible())return false;}
    auto& g=*groupControl;g.explicitConnection=true;g.discovery=true;startTimer(25);
    const auto phase=aiClient.status().phase;
    if(phase==companion::ClientPhase::closed||phase==companion::ClientPhase::failed)
    {if(!aiClient.start(aiClientOptions)){g.message="Companion unavailable; retry connection";return false;}}
    return true;
}
bool InstrumentProcessor::browseGroups()
{
    reconcileGroupConnection();auto& g=*groupControl;g.discovery=true;g.browseRequested=true;startTimer(25);
    const auto phase=aiClient.status().phase;
    if(phase==companion::ClientPhase::closed||phase==companion::ClientPhase::failed)return aiClient.start(aiClientOptions);
    return true;
}
bool InstrumentProcessor::selectGroupPeers(std::span<const GroupUuid> selected)
{
    const auto view=groupLinkView();auto& g=*groupControl;if(!view.intent||selected.size()>maximumSelectedGroupPeers)return false;
    GroupSelection next;next.loadRevision=view.loadRevision;next.peerCount=static_cast<std::uint8_t>(selected.size());
    for(std::size_t i=0;i<selected.size();++i)
    {
        if(selected[i].isNull()||selected[i]==view.intent->member
            ||std::find(selected.begin(),selected.begin()+i,selected[i])!=selected.begin()+i)return false;
        const bool present=std::any_of(g.members.begin(),g.members.end(),[&](const auto& m){return m.intent.group==view.intent->group
            &&m.intent.member==selected[i]&&m.runtime!=view.runtime;});
        const bool incumbent=std::find(g.selection.members.begin(),g.selection.members.begin()+g.selection.peerCount,selected[i])
            !=g.selection.members.begin()+g.selection.peerCount;
        if(!present&&!incumbent)return false;next.members[i]=selected[i];
    }
    if(next!=g.selection){invalidateGroupedProposal();g.selection=next;}return true;
}
void InstrumentProcessor::closeAiPanel()
{
    discardAiProposal();
    if(!groupControl->admitted&&!aiStartupAccount&&!aiActiveAccount)closeAiSession();
}
void InstrumentProcessor::timerCallback(){pollAi();}
bool InstrumentProcessor::submitGroup(GroupControl::Purpose purpose,std::uint64_t now)
{
    auto& g=*groupControl;if(g.pending||g.nextOperation==std::numeric_limits<std::uint64_t>::max())return false;
    const bool publicationOperation=purpose==GroupControl::Purpose::admit||purpose==GroupControl::Purpose::publish;
    const bool localPublication=publicationOperation&&(aiClient.capabilities()&companion::ownedLocalPreviewCapability);
    struct PublicationCharge final {
        InstrumentProcessor& processor;std::uint64_t bytes=0;
        ~PublicationCharge(){if(bytes){const std::lock_guard lock(processor.restoreMutex);processor.drumOwnership->releaseDecoded(bytes);}}
    } charge{*this};
    try {
    if(localPublication){const std::lock_guard lock(restoreMutex);
        if(aiGroupPublicationBytes||!drumOwnership->reserveDecoded(localGroupPublicationReservation))return false;
        charge.bytes=localGroupPublicationReservation;}
    auto command=std::make_unique<companion::GroupCommand>();
    auto map=localPublication?std::make_unique<ai::PreviewSampleMap>():nullptr;
    std::uint64_t source=0,reset=0,publicationGeneration=0,edit=0;
    const auto view=groupLinkView();command->operationId=g.nextOperation;command->runtime=view.runtime;
    command->action=purpose==GroupControl::Purpose::admit?companion::GroupAction::admit:
        purpose==GroupControl::Purpose::publish?companion::GroupAction::publish:
        purpose==GroupControl::Purpose::browse?companion::GroupAction::browse:
        purpose==GroupControl::Purpose::spectrumPublish?companion::GroupAction::spectrumPublish:
        purpose==GroupControl::Purpose::spectrumRead?companion::GroupAction::spectrumRead:
        purpose==GroupControl::Purpose::spectrumWithdraw?companion::GroupAction::spectrumWithdraw:companion::GroupAction::validateProposal;
    command->loadRevision=command->action==companion::GroupAction::browse?0:view.loadRevision;
    if(publicationOperation)
    {
        command->publication.emplace();auto& publication=*command->publication;
        auto live=std::make_unique<ProjectDocument>();bool captured=false;
        {
            const std::lock_guard lock(restoreMutex);if(!groupLinkState.eligible()||!g.committed.group)return false;
            publication.runtime=view.runtime;publication.loadRevision=view.loadRevision;
            publication.project=g.committed;publication.editRevision=g.committedEdit;publication.busy=groupBusyOwned();
            if(localPublication){
                source=structuralGeneration.load();reset=resetGeneration.load();publicationGeneration=drumOwnership->current_->borrowed.generation;
                // A busy gesture advances live edit while publication keeps the committed project.
                if(groupLinkState.view()!=view||view.intent!=g.committed.group||!tryProjectSnapshotOwned(*live,edit)
                    ||(!publication.busy&&edit!=g.committedEdit)||source!=structuralGeneration.load()||source!=observedStructuralGeneration
                    ||source!=drumAcknowledgedSource.load()||reset!=resetGeneration.load())return false;
                for(std::uint8_t bank=0;bank<4;++bank)if(drumBankOwner(g.committed.sound,bank)!=drumBankOwner(live->sound,bank)
                    ||sourceModule(g.committed.sound,bank)!=sourceModule(live->sound,bank))return false;
                if(!buildAiPreviewMapOwned(publication.project.sound,*drumOwnership->current_->document,
                    drumOwnership->current_->owners,publicationGeneration,*map))return false;
                captured=true;
            }
        }
        if(purpose==GroupControl::Purpose::admit&&publication.busy)return false;
        auto& observation=publication.playback.controls;
        if(!captured&&!tryProjectSnapshot(*live,edit)){observation.reason=LiveControlReason::readConflict;}
        else
        {
            observation.values=live->sound.values;live->sound=publication.project.sound;live->sound.values=observation.values;
            if(validateSoundDocument(live->sound)){observation.availability=ObservedMidiAvailability::fresh;observation.reason=LiveControlReason::none;}
            else{observation.values={};observation.reason=LiveControlReason::invalidRouting;}
        }
        captureObservedMidi(publication.playback.notes);
    }
    else if(command->action==companion::GroupAction::spectrumPublish)
    {
        if(!g.spectrum.owner||!g.spectrum.view.enabled||!g.spectrum.latest)return false;
        command->spectrum=g.spectrum.latest;
    }
    else if(command->action==companion::GroupAction::spectrumRead)
    {
        if(!g.spectrum.owner||!g.spectrum.view.enabled||!g.spectrum.view.peerCount)return false;
        // Retain missing sources in the UI, but do not let one departed member
        // turn the remaining admitted sources into an invalid read selection.
        for(std::size_t i=0;i<g.spectrum.view.peerCount;++i){const auto member=g.spectrum.view.peers[i];
            if(view.intent&&std::any_of(g.members.begin(),g.members.end(),[&](const auto& m){return m.intent.group==view.intent->group&&m.intent.member==member&&m.runtime!=view.runtime;}))command->spectrumPeers.push_back(member);
        }
        if(command->spectrumPeers.empty())return false;
    }
    else if(command->action==companion::GroupAction::validateProposal)
    {
        const std::lock_guard lock(restoreMutex);
        if(!g.marker||!aiPayloads->request||!aiPayloads->result||aiProposalPhase()!=AiProposalPhase::proposed
            ||!groupMarkerCurrentOwned(*g.marker)||g.marker->proposal!=&*aiPayloads->result||g.marker->tag!=aiPayloads->request->tag)return false;
        command->proposal=g.marker->tag;
    }
    bool sent=false;
    if(localPublication){const std::lock_guard lock(restoreMutex);
        if(groupLinkState.view()!=view||source!=structuralGeneration.load()||source!=drumAcknowledgedSource.load()
            ||reset!=resetGeneration.load()||publicationGeneration!=drumOwnership->current_->borrowed.generation
            ||command->publication->project!=g.committed||command->publication->editRevision!=g.committedEdit
            ||edit!=editRevision.load()||command->publication->busy!=groupBusyOwned())return false;
        sent=aiClient.groupLocal(*command,*map);
    }else sent=aiClient.group(*command);
    if(!sent){if(!companion::spectrumAction(command->action))g.message="Group operation unavailable; retry";return false;}
    g.spectrum.pendingOwner=companion::spectrumAction(command->action)?g.spectrum.owner:0;
    g.spectrum.pendingView=companion::spectrumAction(command->action)?g.spectrum.view:EqSpectrumView{};
    {const std::lock_guard lock(restoreMutex);
        ++g.nextOperation;g.pendingSession=aiClient.session();g.purpose=purpose;g.pending=std::move(command);
        if(localPublication){aiGroupPublicationBytes=charge.bytes;charge.bytes=0;}}
    if(purpose==GroupControl::Purpose::browse){g.browseRequested=false;g.browseTime=now;}
    if(purpose==GroupControl::Purpose::check||purpose==GroupControl::Purpose::apply)g.checkTime=now;
    if(publicationOperation)g.observationTime=now;
    return true;
    }catch(const std::bad_alloc&){return false;}
}
void InstrumentProcessor::submitCatalogFollowup()
{
    // The client admits no generation while a group command awaits its reply.
    auto& g=*groupControl;if(!g.followupParent||g.pending)return;
    const auto parent=*g.followupParent;g.followupParent.reset();
    if(!aiPayloads->request||aiProposalPhase()!=AiProposalPhase::pending||!aiPayloads->request->catalog||aiPayloads->request->catalog->selectionParent!=parent)return;
    const auto marker=g.marker;
    if(submitAiGeneration(*aiPayloads->request,marker?&marker->selection:nullptr,marker?&parent:nullptr))
    {
        if(marker)g.marker=GroupControl::Marker{aiPayloads->request->tag,marker->session,marker->selection};
        aiWorkflowMessage="Materializing captured racks";return;
    }
    aiClient.close();const std::lock_guard lock(restoreMutex);failAiOwned("Captured rack submission failed; session retired");
}
void InstrumentProcessor::pollGroup()
{
    auto& g=*groupControl;const auto time=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto now=time<0?0:static_cast<std::uint64_t>(time);
    if(now<g.lastTime){g.observationTime=g.browseTime=g.checkTime=0;g.spectrum.publishTime=g.spectrum.readTime=0;}g.lastTime=now;
    const auto connection=aiClient.status();
    if(connection.phase==companion::ClientPhase::closed||connection.phase==companion::ClientPhase::failed)
    {
        if(g.waiting)refuseGroupPresses();
        if(connection.phase==companion::ClientPhase::failed)g.message=connection.errorCode;
        if(g.admitted||g.pending||aiGroupPublicationBytes){resetEqSpectrumConnection();invalidateGroupedProposal();groupCompanionDisconnected();
            const std::lock_guard lock(restoreMutex);g.admitted=false;g.pending.reset();releaseAiGroupPublicationChargeOwned();g.dirty=true;g.members.clear();}
        if(connection.phase==companion::ClientPhase::closed&&g.explicitConnection&&groupLinkView().phase==GroupLinkPhase::authorized)aiClient.start(aiClientOptions);
        return;
    }
    // This guard outlives the ACK and original command, including reset/error paths.
    struct PublicationCharge final {
        InstrumentProcessor& processor;std::uint64_t bytes=0;
        ~PublicationCharge(){if(bytes){const std::lock_guard lock(processor.restoreMutex);processor.drumOwnership->releaseDecoded(bytes);}}
    } charge{*this};
    companion::GroupReply reply;
    if(aiClient.takeGroup(reply))
    {
        {const std::lock_guard lock(restoreMutex);charge.bytes=aiGroupPublicationBytes;aiGroupPublicationBytes=0;}
        auto original=std::move(g.pending);const auto purpose=g.purpose;
        if(!original||g.pendingSession!=aiClient.session()||reply.operationId!=original->operationId||reply.action!=original->action
            ||reply.runtime!=original->runtime||reply.loadRevision!=original->loadRevision||reply.proposal!=original->proposal)
        {resetGroupConnection();return;}
        if(companion::spectrumAction(reply.action))receiveEqSpectrumReply(*original,reply);
        else {
        g.conflicted=reply.conflicted;
        if(g.conflicted)invalidateGroupedProposal();
        if(purpose==GroupControl::Purpose::admit)
        {
            if(reply.status==companion::GroupReplyStatus::ready&&reply.normalized&&original->publication
                &&acknowledgeGroupIntent(original->runtime,original->loadRevision,*original->publication->project.group,*reply.normalized))
            {
                g.admitted=true;g.session=aiClient.session();g.processed=groupLinkView();g.selection.loadRevision=g.processed.loadRevision;
                // Normalized member metadata belongs to this exact admitted load.
                original->publication->project.group=reply.normalized;
                g.dirty=original->publication->project!=g.committed||original->publication->editRevision!=g.committedEdit;
                g.publishedBusy=original->publication->busy;g.message="Group connected";
            }
            else{g.admitted=false;g.explicitConnection=false;g.message="Group admission unavailable; retry connection";}
        }
        else if(purpose==GroupControl::Purpose::publish)
        {
            if(reply.status==companion::GroupReplyStatus::ready&&original->publication)
            {g.dirty=original->publication->project!=g.committed||original->publication->editRevision!=g.committedEdit;g.publishedBusy=original->publication->busy;}
            else{invalidateGroupedProposal();g.admitted=false;g.message="Group publication rejected; reconnect";}
        }
        else if(purpose==GroupControl::Purpose::browse)
        {
            if(reply.status==companion::GroupReplyStatus::ready)g.members=std::move(reply.members);
            const auto own=groupLinkView();
            g.conflicted=std::any_of(g.members.begin(),g.members.end(),[&](const auto& m){return own.intent&&m.intent.group==own.intent->group&&m.conflicted;});
            bool changed=g.conflicted;
            for(std::size_t i=0;i<g.selection.peerCount;++i)
            {
                const auto member=std::find_if(g.members.begin(),g.members.end(),[&](const auto& m){return own.intent&&m.intent.group==own.intent->group&&m.intent.member==g.selection.members[i];});
                if(member==g.members.end()||member->busy)changed=true;
            }
            if(changed)invalidateGroupedProposal();
        }
        else if(reply.status!=companion::GroupReplyStatus::ready)invalidateGroupedProposal();
        else if(g.marker)
        {
            g.marker->current=true;
            if(purpose==GroupControl::Purpose::apply)
            {
                bool valid;
                {const std::lock_guard lock(restoreMutex);valid=aiDelayedApply&&aiPayloads->result&&aiPayloads->request
                    &&aiDelayedApply->proposal==&*aiPayloads->result&&aiDelayedApply->tag==*original->proposal
                    &&groupMarkerCurrentOwned(*g.marker)&&g.marker->proposal==&*aiPayloads->result;
                    valid=valid&&aiPayloads->result->status==ai::GenerationStatus::ready;if(valid)g.applyToken=g.marker;aiDelayedApply.reset();}
                aiWorkflowMessage=valid&&applyAiProposal()?"Applied as one undoable edit":"Apply rejected; Generate again";
                g.applyToken.reset();
            }
        }
        }
    }
    if(aiClient.status().phase!=companion::ClientPhase::ready||g.pending)return;
    if(g.followupParent){submitCatalogFollowup();return;}
    if(settleGroupPresses())return;
    const auto view=groupLinkView();
    if(g.explicitConnection&&view.intent&&(view.phase==GroupLinkPhase::authorized||view.phase==GroupLinkPhase::admitted)&&!g.admitted)
    {submitGroup(GroupControl::Purpose::admit,now);return;}
    if(g.admitted)
    {
        bool busy;{const std::lock_guard lock(restoreMutex);busy=groupBusyOwned();}
        if(busy&&g.marker&&!aiApplying)invalidateGroupedProposal();
        if(g.dirty||busy!=g.publishedBusy){submitGroup(GroupControl::Purpose::publish,now);return;}
        if(aiDelayedApply&&renderedPreview.stopped()&&g.marker){submitGroup(GroupControl::Purpose::apply,now);return;}
    }
    if(aiStartupAccount||aiActiveAccount)return;
    // Serve the oldest due periodic request. With slow UI polling, always
    // prioritising a 200ms heartbeat can otherwise starve every spectrum reply.
    const auto never=std::numeric_limits<std::uint64_t>::max();
    auto due=never;auto purpose=GroupControl::Purpose::publish;
    const auto consider=[&](GroupControl::Purpose candidate,std::uint64_t time){if(time<due){due=time;purpose=candidate;}};
    if(g.admitted&&now-g.observationTime>=200)consider(GroupControl::Purpose::publish,g.observationTime+200);
    if(g.admitted&&g.marker&&aiProposalPhase()==AiProposalPhase::proposed&&(g.checkTime==0||now-g.checkTime>=500))
        consider(GroupControl::Purpose::check,g.checkTime?g.checkTime+500:0);
    if(g.discovery&&(g.browseRequested||g.browseTime==0||now-g.browseTime>=500))
        consider(GroupControl::Purpose::browse,g.browseRequested||g.browseTime==0?0:g.browseTime+500);
    if(submitEqSpectrum(now,due))return;
    if(due!=never)(void)submitGroup(purpose,now);
}
void InstrumentProcessor::refuseGroupPresses()
{
    auto& g=*groupControl;
    if(g.waiting)aiWorkflowMessage=g.waiting->refine?"Original peer context is unavailable; Generate again":"Group context is unavailable; retry Generate";
    g.waiting.reset();
}
bool InstrumentProcessor::settleGroupPresses()
{
    auto& g=*groupControl;
    if(!g.waiting||aiStartupAccount||aiActiveAccount)return false;
    const auto view=groupWorkflowView();
    if(g.waiting->refine)
    {
        bool owned;{const std::lock_guard lock(restoreMutex);owned=g.marker&&groupMarkerCurrentOwned(*g.marker);}
        if(!owned||(!view.generationReady&&!view.generationWaits)){refuseGroupPresses();return false;}
        if(!view.generationReady||!g.marker->current)return false;
    }
    else if(!view.generationReady){if(!view.generationWaits)refuseGroupPresses();return false;}
    const auto press=std::move(*g.waiting);g.waiting.reset();
    return press.refine?refineAi(press.options.instruction):generateAi(press.options);
}
}

#include "CompanionRuntimeState.h"
#include "GroupRegistry.h"
#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace airack::companion
{
namespace
{
WireBytes bytes(const juce::MemoryBlock& b)
{ return {static_cast<const std::byte*>(b.getData()),b.getSize()}; }
bool zeroIds(const ai::RequestTag& t)
{ return t.requestId==0 && t.editRevision==0 && t.contextRevision==0; }
bool samePeer(PipePeer a,PipePeer b)
{ return a.processId==b.processId && a.creationTime==b.creationTime; }
// Zero permanently marks exhaustion; no identity/ordering counter wraps.
std::uint64_t issue(std::uint64_t& next) noexcept
{
    const auto value=next;
    if(next!=0)next=next==std::numeric_limits<std::uint64_t>::max()?0:next+1;
    return value;
}
}

struct RuntimeState::Impl
{
    enum class ReplyRole { control, generation, account, duplicateWelcome, preview };
    struct LocalGroupAuthority
    {
        std::shared_ptr<const RuntimeLocalGroupContext> peers;
        ai::PreviewDigest requester{};
    };
    struct ProposalRecord { ai::RequestTag tag;std::shared_ptr<const GroupPerformanceCapture> capture;std::shared_ptr<const ai::GenerationRequest> original;std::shared_ptr<const ProjectDocument> candidate;std::shared_ptr<const ai::ProposalEdits> aggregate;
        std::shared_ptr<const ai::LocalPreviewContext> local;std::shared_ptr<const ai::PreviewSampleMap> candidateMap;std::shared_ptr<const LocalGroupAuthority> localGroup; };
    struct PendingCatalog
    {
        ai::GenerationRequest request;
        ai::GenerationResult selection;
        std::shared_ptr<const GroupPerformanceCapture> capture;
        GroupSelection peers;
        std::optional<ProposalRecord> parent;
        std::shared_ptr<const ai::LocalPreviewContext> local;
        std::shared_ptr<const LocalGroupAuthority> localGroup;
        RuntimeTime deadline{};
    };
    struct ReplyMeta
    {
        ReplyRole role=ReplyRole::control;
        ai::RequestTag tag {juce::Uuid::null(),0,0,0};
        std::shared_ptr<const GroupPerformanceCapture> capture;
        std::optional<ProposalRecord> parent;
        bool ready=false;
        std::optional<ProposalRecord> proposal;
        std::shared_ptr<const PendingCatalog> pendingCatalog;
        std::shared_ptr<const LocalGroupAuthority> localGroup;
        std::optional<ai::PreviewBinding> preview;
    };
    struct Reply { Frame frame; ReplyMeta meta; std::uint64_t order=0; };
    struct CurrentGeneration
    {
        ai::RequestTag tag;std::shared_ptr<JobCancellation> cancellation;
        std::shared_ptr<const GroupPerformanceCapture> capture;
        GroupSelection selection;
        std::optional<ProposalRecord> parent;
        std::shared_ptr<const ai::GenerationRequest> catalogOriginal;
        std::shared_ptr<const ai::LocalPreviewContext> local;
        std::shared_ptr<LocalPreviewExchange> exchange;
        RuntimeTime deadline{};
        std::shared_ptr<const LocalGroupAuthority> localGroup;
        std::array<std::uint64_t,3> tickets{};
        std::uint8_t renderOrdinal=0,providerAttempt=0;
        std::optional<ai::PreviewBinding> preview;
        bool previewDelivered=false;
    };
    struct Connection
    {
        RuntimeConnectionId id=0;
        PipePeer peer;
        RuntimeTime opened;
        juce::Uuid runtime=juce::Uuid::null(), session=juce::Uuid::null();
        bool registered=false, rejected=false, accountOutstanding=false;
        std::uint32_t capabilities=0;
        std::uint64_t lastRequestId=0;
        std::uint64_t lastGroupOperation=0;
        std::optional<GroupPublication> publication;
        RuntimeTime publicationTime;
        std::shared_ptr<const ai::PreviewSampleMap> localPublication;
        ai::PreviewDigest localPublicationDigest{};
        std::optional<ProposalRecord> proposal;
        // One delivered local child may still be refused by the processor join.
        std::optional<ProposalRecord> localParent;
        std::shared_ptr<const PendingCatalog> pendingCatalog;
        std::optional<CurrentGeneration> generation;
        std::optional<Reply> controlReply, jobReply;
        std::optional<ReplyMeta> started;
    };
    // ponytail: linear scans have a hard ceiling of 16; no dynamic registry/queue.
    std::array<std::optional<Connection>,maximumRuntimeConnections> connections;
    std::array<std::optional<RuntimeJob>,maximumQueuedRuntimeJobs> queue;
    std::size_t queueCount=0;
    std::optional<RuntimeJob> running;
    std::uint64_t nextConnectionId=1, nextJobId=1, nextReplyOrder=1,nextPreviewTicket=1;
    void* exchangeAllocationContext=nullptr;
    void (*exchangeAllocationObserver)(void*) noexcept=nullptr;
    GroupRegistry registry;
    bool invalidating=false, invalidationDirty=false;

    Connection* find(RuntimeConnectionId id) noexcept
    {
        if(id!=0)for(auto& c:connections)if(c && c->id==id)return &*c;
        return nullptr;
    }
    const Connection* find(RuntimeConnectionId id) const noexcept
    {
        if(id!=0)for(const auto& c:connections)if(c && c->id==id)return &*c;
        return nullptr;
    }
    static void cancel(const std::shared_ptr<JobCancellation>& token) noexcept
    { if(token)token->requested.store(true,std::memory_order_release); }
    static void cancelJob(const RuntimeJob& job) noexcept
    {cancel(job.cancellation);if(job.previewExchange)job.previewExchange->cancel();}
    void removeQueued(RuntimeConnectionId id,bool generationsOnly=false) noexcept
    {
        for(std::size_t i=0;i<queueCount;){
            if(queue[i]->connectionId!=id||(generationsOnly&&!std::holds_alternative<ai::GenerationRequest>(queue[i]->input))){++i;continue;}
            cancelJob(*queue[i]);
            for(std::size_t j=i+1;j<queueCount;++j)queue[j-1]=std::move(queue[j]);
            queue[--queueCount].reset();
        }
    }
    void clearGeneration(Connection& c) noexcept
    {
        if(c.generation){cancel(c.generation->cancellation);if(c.generation->exchange)c.generation->exchange->cancel();}
        c.generation.reset();removeQueued(c.id,true);
        if(running && running->connectionId==c.id && std::holds_alternative<ai::GenerationRequest>(running->input))cancelJob(*running);
        if(c.jobReply && (c.jobReply->meta.role==ReplyRole::generation||c.jobReply->meta.role==ReplyRole::preview))c.jobReply.reset();
        c.pendingCatalog.reset();
        if(c.started)c.started->pendingCatalog.reset();
        // Started metadata remains with the native IO owner until completion.
    }
    static void eraseProposalAuthority(Connection& c) noexcept
    {
        c.proposal.reset();c.localParent.reset();c.pendingCatalog.reset();
        auto erase=[](ReplyMeta& meta){meta.capture.reset();meta.parent.reset();meta.proposal.reset();meta.pendingCatalog.reset();meta.localGroup.reset();};
        if(c.started)erase(*c.started);if(c.controlReply)erase(c.controlReply->meta);if(c.jobReply)erase(c.jobReply->meta);
    }
    void close(RuntimeConnectionId id) noexcept
    {
        auto* c=find(id);if(!c)return;
        if(c->generation){cancel(c->generation->cancellation);if(c->generation->exchange)c->generation->exchange->cancel();}
        removeQueued(id);if(running && running->connectionId==id)cancelJob(*running);
        registry.withdraw(id);
        for(auto& slot:connections)if(slot && slot->id==id){slot.reset();break;}
        invalidateGroups();
    }
    bool enqueueReply(Connection& c,Frame frame,ReplyMeta meta,bool job) noexcept
    {
        auto& slot=job?c.jobReply:c.controlReply;
        const auto cap=messagePayloadCap(frame.kind);
        const auto id=c.id;
        if(slot || frame.payload.getSize()>cap){close(id);return false;}
        const auto order=issue(nextReplyOrder);if(order==0){close(id);return false;}
        slot.emplace(Reply{std::move(frame),std::move(meta),order});return true;
    }
    bool controlReply(Connection& c,MessageKind kind,const ai::RequestTag& tag,RuntimeStatus status,ReplyRole role=ReplyRole::control,std::uint32_t capabilities=0)
    {
        Frame f{kind,c.session,{}};
        if(!encodeControl({tag,status,capabilities},f.payload)){close(c.id);return false;}
        return enqueueReply(c,std::move(f),{role,tag},false);
    }
    RuntimeStatus rejectGeneration(Connection& c,const ai::RequestTag& tag,RuntimeStatus status)
    { return controlReply(c,MessageKind::failure,tag,status)?status:RuntimeStatus::invalidMessage; }
    bool addJob(Connection& c,std::variant<ai::GenerationRequest,AccountCommand> input,std::shared_ptr<JobCancellation> token,
                std::shared_ptr<const GroupPerformanceCapture> capture={},std::shared_ptr<const ai::LocalPreviewContext> local={},
                std::shared_ptr<LocalPreviewExchange> exchange={},RuntimeTime deadline={},std::shared_ptr<const RuntimeLocalGroupContext> localGroup={})
    {
        if(queueCount==queue.size())return false;
        const auto id=issue(nextJobId);if(id==0){close(c.id);return false;}
        queue[queueCount].emplace(RuntimeJob{id,c.id,c.session,c.peer,std::move(input),std::move(token),std::move(capture),std::move(local),std::move(exchange),deadline,std::move(localGroup)});++queueCount;return true;
    }
    RuntimeStatus addGeneration(Connection& c,ai::GenerationRequest&& request,std::shared_ptr<JobCancellation> token,
        std::shared_ptr<const GroupPerformanceCapture> capture,GroupSelection selection,std::optional<ProposalRecord> parent,
        std::shared_ptr<const ai::GenerationRequest> original,std::shared_ptr<const ai::LocalPreviewContext> local,
        std::shared_ptr<const LocalGroupAuthority> localGroup,RuntimeTime now,RuntimeTime inheritedDeadline={})
    {
        const auto tag=request.tag;std::array<std::uint64_t,3> tickets{};std::shared_ptr<LocalPreviewExchange> exchange;RuntimeTime deadline{};
        const auto replacesQueued=std::any_of(queue.begin(),queue.begin()+queueCount,[&](const auto& job){
            return job->connectionId==c.id&&std::holds_alternative<ai::GenerationRequest>(job->input);});
        if(queueCount==queue.size()&&!replacesQueued)return rejectGeneration(c,tag,RuntimeStatus::queueFull);
        if(!nextJobId)return rejectGeneration(c,tag,RuntimeStatus::unavailable);
        auto nextTicket=nextPreviewTicket;
        if(local){
            if(!localTicketsAvailable())return rejectGeneration(c,tag,RuntimeStatus::unavailable);
            for(std::size_t i=0;i<tickets.size();++i)tickets[i]=nextPreviewTicket+i;
            nextTicket=tickets.back()==std::numeric_limits<std::uint64_t>::max()?0:tickets.back()+1;
            deadline=inheritedDeadline==RuntimeTime{}?now+std::chrono::milliseconds(360000):inheritedDeadline;
            try{
                if(exchangeAllocationObserver)exchangeAllocationObserver(exchangeAllocationContext);
                exchange=std::make_shared<LocalPreviewExchange>(tickets,deadline);
            }catch(const std::bad_alloc&){return rejectGeneration(c,tag,RuntimeStatus::unavailable);}
        }
        static_assert(std::is_nothrow_move_constructible_v<RuntimeJob>);
        static_assert(std::is_nothrow_move_constructible_v<CurrentGeneration>);
        const bool erase=local||capture||request.catalog||(c.proposal&&c.proposal->original&&c.proposal->original->catalog);
        clearGeneration(c);if(erase)eraseProposalAuthority(c);
        if(!addJob(c,std::move(request),token,capture,local,exchange,deadline,localGroup?localGroup->peers:nullptr))return RuntimeStatus::invalidMessage;
        c.generation.emplace(CurrentGeneration{tag,std::move(token),std::move(capture),std::move(selection),std::move(parent),std::move(original),
            std::move(local),std::move(exchange),deadline,std::move(localGroup),tickets});
        if(tickets[0])nextPreviewTicket=nextTicket;return RuntimeStatus::ready;
    }
    static bool matching(const RuntimeJob& a,const RuntimeJob& b)
    {
        if(a.jobId!=b.jobId || a.connectionId!=b.connectionId || a.session!=b.session || !samePeer(a.peer,b.peer)
            || a.cancellation!=b.cancellation || a.input.index()!=b.input.index() || a.groupContext!=b.groupContext
            ||a.localPreview!=b.localPreview||a.previewExchange!=b.previewExchange||a.localDeadline!=b.localDeadline||a.localGroupContext!=b.localGroupContext)return false;
        if(const auto* r=std::get_if<ai::GenerationRequest>(&a.input))return r->tag==std::get<ai::GenerationRequest>(b.input).tag;
        const auto& x=std::get<AccountCommand>(a.input);const auto& y=std::get<AccountCommand>(b.input);
        return x.action==y.action && x.model==y.model;
    }
    bool generationOutstanding(const Connection& c) const noexcept
    {
        return c.generation.has_value() || c.pendingCatalog || (c.started && c.started->role==ReplyRole::generation)
            || (running && running->connectionId==c.id && std::holds_alternative<ai::GenerationRequest>(running->input));
    }
    bool localTicketsAvailable() noexcept
    {
        if(nextPreviewTicket&&nextPreviewTicket<=std::numeric_limits<std::uint64_t>::max()-2)return true;
        nextPreviewTicket=0;return false;
    }
    Connection* publisher(GroupUuid runtime) noexcept
    { for(auto& c:connections)if(c&&c->publication&&c->publication->runtime==runtime)return &*c;return nullptr; }
    bool localCurrent(const Connection& c,const std::shared_ptr<const LocalGroupAuthority>& authority) const noexcept
    {
        if(!authority)return true;
        if(!c.localPublication||c.localPublicationDigest!=authority->requester||!authority->peers
            ||authority->peers->peerCount>maximumSelectedGroupPeers)return false;
        for(std::size_t i=0;i<authority->peers->peerCount;++i){const auto& peer=authority->peers->peers[i];const Connection* owner=nullptr;
            for(const auto& candidate:connections)if(candidate&&candidate->publication&&candidate->publication->runtime==peer.runtime){owner=&*candidate;break;}
            if(!owner||!samePeer(owner->peer,c.peer)||owner->publication->loadRevision!=peer.loadRevision||owner->publication->editRevision!=peer.editRevision
                ||bool(owner->localPublication)!=peer.metadataAvailable||(peer.metadataAvailable&&owner->localPublicationDigest!=peer.mapDigest))return false;
        }
        return true;
    }
    bool captureCurrent(const Connection& c,const GroupPerformanceCapture& capture,const std::shared_ptr<const LocalGroupAuthority>& local={}) const noexcept
    {
        if(!c.publication||c.publication->busy||c.publication->editRevision!=capture.requesterEditRevision
            ||!registry.current(c.id,capture.ownership)||!localCurrent(c,local))return false;
        for(const auto& peer:capture.ownership.peers)
        {
            const Connection* owner=nullptr;
            for(const auto& candidate:connections)if(candidate&&candidate->publication&&candidate->publication->runtime==peer.runtime){owner=&*candidate;break;}
            if(!owner||!samePeer(owner->peer,c.peer)||owner->publication->busy||owner->publication->loadRevision!=peer.loadRevision
                ||owner->publication->editRevision!=peer.editRevision)return false;
        }
        return true;
    }
    void invalidateGroups() noexcept
    {
        if(invalidating){invalidationDirty=true;return;}
        invalidating=true;
        do {
            invalidationDirty=false;
            for(auto& slot:connections)
            {
                if(!slot)continue;auto& c=*slot;
                auto retire=[&](ReplyMeta& meta){if(meta.capture&&!captureCurrent(c,*meta.capture,meta.localGroup)){meta.capture.reset();meta.parent.reset();meta.proposal.reset();meta.pendingCatalog.reset();meta.localGroup.reset();}};
                if(c.started)retire(*c.started);if(c.controlReply)retire(c.controlReply->meta);if(c.jobReply)retire(c.jobReply->meta);
                if(c.proposal&&c.proposal->capture&&!captureCurrent(c,*c.proposal->capture,c.proposal->localGroup)){c.proposal.reset();c.localParent.reset();}
                if(c.localParent&&c.localParent->capture&&!captureCurrent(c,*c.localParent->capture,c.localParent->localGroup))c.localParent.reset();
                if(c.pendingCatalog&&c.pendingCatalog->capture&&!captureCurrent(c,*c.pendingCatalog->capture,c.pendingCatalog->localGroup))c.pendingCatalog.reset();
                if(!c.generation||!c.generation->capture||captureCurrent(c,*c.generation->capture,c.generation->localGroup))continue;
                const auto tag=c.generation->tag;const auto id=c.id;
                clearGeneration(c);c.proposal.reset();c.localParent.reset();Frame reply{MessageKind::failure,c.session,{}};
                if(!encodeControl({tag,RuntimeStatus::staleSession},reply.payload)){close(id);continue;}
                enqueueReply(c,std::move(reply),{ReplyRole::generation,tag},true);
                // enqueueReply may retire this slot. No borrowed reference survives it.
            }
        } while(invalidationDirty);
        invalidating=false;
    }
    static GroupReplyStatus groupStatus(GroupStatus status) noexcept
    {
        switch(status){case GroupStatus::ready:return GroupReplyStatus::ready;case GroupStatus::notAdmitted:return GroupReplyStatus::notAdmitted;
            case GroupStatus::capacity:return GroupReplyStatus::capacity;case GroupStatus::invalidSelection:return GroupReplyStatus::invalidSelection;
            case GroupStatus::conflict:return GroupReplyStatus::conflict;case GroupStatus::revisionExhausted:return GroupReplyStatus::revisionExhausted;
            default:return GroupReplyStatus::invalidInput;}
    }
    static RuntimeStatus generationStatus(GroupStatus status) noexcept
    {
        switch(status){case GroupStatus::ready:return RuntimeStatus::ready;case GroupStatus::conflict:return RuntimeStatus::busy;
            case GroupStatus::capacity:return RuntimeStatus::queueFull;case GroupStatus::revisionExhausted:return RuntimeStatus::unavailable;
            case GroupStatus::invalidInput:case GroupStatus::duplicateRuntime:return RuntimeStatus::invalidMessage;default:return RuntimeStatus::staleSession;}
    }
    static void age(PeerPlaybackObservation& playback,RuntimeTime receipt,RuntimeTime now) noexcept
    {
        const bool rollback=now<receipt;
        const auto elapsed=rollback?0:static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now-receipt).count());
        agePeerPlaybackObservation(playback,elapsed,rollback);
    }
    RuntimeStatus groupCommand(Connection& c,const Frame& frame,RuntimeTime now)
    {
        const auto id=c.id;const auto session=c.session;GroupCommand command;
        std::shared_ptr<const ai::PreviewSampleMap> localMap;ai::PreviewDigest localDigest{};
        if(frame.kind==MessageKind::groupCommandLocal){
            auto map=std::make_unique<ai::PreviewSampleMap>();
            if(!decodeLocalGroupPublication(bytes(frame.payload),command,*map)||!ai::digestPreviewMap(*map,localDigest)){close(id);return RuntimeStatus::invalidMessage;}
            localMap=std::move(map);
        }else if(!decodeGroupCommand(bytes(frame.payload),command)){close(id);return RuntimeStatus::invalidMessage;}
        const bool spectrum=spectrumAction(command.action);
        if((frame.kind==MessageKind::spectrumCommand)!=spectrum||(spectrum&&!(c.capabilities&spectrumCapability))){close(id);return RuntimeStatus::invalidMessage;}
        if(command.runtime!=GroupUuid(c.runtime)){close(id);return RuntimeStatus::staleSession;}
        GroupReply reply{command.operationId,command.action,command.runtime,command.loadRevision,GroupReplyStatus::ready,{},0,false,{},command.proposal};
        if(command.operationId<=c.lastGroupOperation)reply.status=GroupReplyStatus::invalidInput;
        else
        {
            c.lastGroupOperation=command.operationId;
            if(command.action==GroupAction::spectrumPublish)reply.status=groupStatus(registry.publishSpectrum(id,*command.spectrum,now));
            else if(command.action==GroupAction::spectrumRead)reply.status=groupStatus(registry.readSpectra(id,command.runtime,command.loadRevision,command.spectrumPeers,now,reply.spectra));
            else if(command.action==GroupAction::spectrumWithdraw)reply.status=groupStatus(registry.withdrawSpectrum(id,command.runtime,command.loadRevision));
            else if(command.action==GroupAction::admit||command.action==GroupAction::publish)
            {
                GroupAdmission admission;GroupStatus status=GroupStatus::invalidInput;
                const auto& candidate=*command.publication;
                if(command.action==GroupAction::admit)
                {
                    if(!c.publication)status=registry.admit({id,c.peer,command.runtime,candidate.loadRevision,candidate.editRevision,candidate.project},admission);
                }
                else if(!c.publication)status=GroupStatus::notAdmitted;
                else if(candidate.runtime==c.publication->runtime&&candidate.loadRevision==c.publication->loadRevision
                    &&(!candidate.busy||candidate.editRevision==c.publication->editRevision))
                    status=registry.updatePublication(id,command.runtime,command.loadRevision,candidate.editRevision,candidate.project,admission);
                reply.status=groupStatus(status);
                if(status==GroupStatus::ready)
                {
                    if(!c.publication){clearGeneration(c);eraseProposalAuthority(c);}
                    c.publication=candidate;c.publication->project.group=admission.intent;c.publicationTime=now;
                    c.localPublication=std::move(localMap);c.localPublicationDigest=localDigest;
                    reply.normalized=admission.intent;reply.registryRevision=admission.revision;reply.conflicted=admission.conflicted;
                }
            }
            else if(command.action==GroupAction::withdraw)
            {
                if(!c.publication)reply.status=GroupReplyStatus::notAdmitted;
                else if(c.publication->loadRevision!=command.loadRevision)reply.status=GroupReplyStatus::invalidInput;
                else {registry.withdraw(id);c.publication.reset();c.localPublication.reset();c.localPublicationDigest={};c.proposal.reset();c.localParent.reset();}
            }
            else if(command.action==GroupAction::browse)
            {
                reply.members.reserve(maximumGroupMembers);
                for(const auto& owner:connections)if(owner&&owner->publication&&samePeer(owner->peer,c.peer))
                {
                    const auto& pub=*owner->publication;auto playback=pub.playback;age(playback,owner->publicationTime,now);
                    GroupContext empty;const bool conflicted=registry.capture(owner->id,{},empty)!=GroupStatus::ready;
                    const auto& n=playback.notes;const auto& controls=playback.controls;
                    reply.members.push_back({pub.runtime,pub.loadRevision,pub.editRevision,*pub.project.group,pub.project.mode,pub.busy,conflicted,
                        n.availability,n.reason,n.noteCount,n.ageMs,n.truncated,controls.availability,controls.reason,controls.ageMs});
                }
            }
            else
            {
                reply.status=c.publication&&c.publication->loadRevision==command.loadRevision&&c.proposal&&c.proposal->capture&&c.proposal->tag==*command.proposal
                    &&captureCurrent(c,*c.proposal->capture,c.proposal->localGroup)?GroupReplyStatus::ready:GroupReplyStatus::staleContext;
            }
        }
        auto* current=find(id);if(!current)return RuntimeStatus::invalidMessage;
        Frame encoded{spectrum?MessageKind::spectrumReply:MessageKind::groupReply,session,{}};
        if(!encodeGroupReply(reply,encoded.payload)){close(id);return RuntimeStatus::invalidMessage;}
        const bool queued=enqueueReply(*current,std::move(encoded),{},false);
        if(!spectrum)invalidateGroups();
        return queued?RuntimeStatus::ready:RuntimeStatus::invalidMessage;
    }

    static bool sameRequest(const ai::GenerationRequest& a,const ai::GenerationRequest& b,const ai::LocalPreviewContext* local=nullptr)
    {
        juce::MemoryBlock x,y;
        if(local){auto input=std::make_unique<ai::LocalGenerationRequest>();input->local=*local;input->request=a;
            if(!encodeLocalRequest(*input,x))return false;input->request=b;return encodeLocalRequest(*input,y)&&x==y;}
        return encodeRequest(a,x)&&encodeRequest(b,y)&&x==y;
    }
    static bool childAuthority(const ai::GenerationRequest& child,const ProposalRecord& parent,const ai::LocalPreviewContext* local=nullptr)
    {
        if(bool(local)!=bool(parent.local))return false;
        if(local){
            auto expected=*parent.local;
            if(!child.catalog){if(!parent.candidateMap)return false;expected.sourceMap=*parent.candidateMap;
                if(!ai::digestPreviewMap(expected.sourceMap,expected.sourceMapDigest))return false;}
            if(*local!=expected)return false;
        }
        if(child.catalog)
        {
            if(!parent.original||!parent.original->catalog||!parent.aggregate||!parent.candidate
                ||!child.catalog->authority||!parent.original->catalog->authority
                ||child.catalog->phase!=ai::CatalogPhase::select
                ||!exactProject(child.source,parent.original->source)
                ||child.structuralGeneration!=parent.original->structuralGeneration
                ||child.model!=parent.original->model||child.role!=parent.original->role||child.key!=parent.original->key
                ||child.scale!=parent.original->scale
                ||((local&&!parent.candidate->pattern.noteCount&&!parent.aggregate->pattern)?child.capturedPhrase.has_value()
                    :child.capturedPhrase!=std::optional<PatternDocument>(parent.candidate->pattern))||child.capturedBpm!=parent.original->capturedBpm
                ||*child.catalog->authority!=*parent.original->catalog->authority)return false;
            const auto& policy=parent.original->policy;
            if(child.policy.scope!=policy.scope||(child.policy.allowMacroChanges&&!policy.allowMacroChanges)
                ||(child.policy.allowSlotReuse&&!policy.allowSlotReuse))return false;
            auto expected=child;expected.policy=policy;expected.policy.allowMacroChanges=child.policy.allowMacroChanges;
            expected.policy.allowSlotReuse=child.policy.allowSlotReuse;expected.policy.requirePattern=child.policy.requirePattern;
            expected.catalog->root=parent.original->catalog->root;expected.catalog->baseIntent=*parent.aggregate;
            expected.catalog->selectionParent.reset();expected.catalog->inferenceCallsUsed=0;
            expected.catalog->providerStatus=rack::oauth::Status::ready;expected.catalog->httpStatus=0;
            ProjectDocument canonical;
            return sameRequest(child,expected,local)
                &&ai::admitProposal(parent.original->source,policy,*parent.aggregate,*child.catalog->authority,canonical)==ai::ProposalStatus::accepted
                &&exactProject(canonical,*parent.candidate);
        }
        if(parent.original&&parent.original->catalog)return false;
        if(!parent.original||!parent.candidate||!exactProject(child.source,*parent.candidate)||child.structuralGeneration!=parent.original->structuralGeneration)return false;
        const auto& policy=parent.original->policy;if(child.policy.scope!=policy.scope||(child.policy.allowMacroChanges&&!policy.allowMacroChanges)||(child.policy.allowSlotReuse&&!policy.allowSlotReuse))return false;
        auto expected=child;expected.policy=policy;expected.policy.allowMacroChanges=child.policy.allowMacroChanges;expected.policy.allowSlotReuse=child.policy.allowSlotReuse;expected.policy.requirePattern=child.policy.requirePattern;
        return sameRequest(child,expected,local);
    }
    const ProposalRecord* refinementParent(const Connection& c,const ai::GenerationRequest& child,const ai::LocalPreviewContext* local,
        const std::optional<ai::RequestTag>& required={}) const
    {
        // Reuse only the immediately admitted parent, never a proposal history.
        const std::array<const ProposalRecord*,2> choices{c.proposal?&*c.proposal:nullptr,
            local&&c.proposal&&c.proposal->local&&c.localParent?&*c.localParent:nullptr};
        for(const auto* candidate:choices){
            if(!candidate||(required&&candidate->tag!=*required)
                ||(candidate->capture&&!captureCurrent(c,*candidate->capture,candidate->localGroup)))continue;
            if(childAuthority(child,*candidate,local))return candidate;
        }
        return nullptr;
    }
    static bool mergeIntent(ai::ProposalEdits& all,const ai::ProposalEdits& next)
    {
        for(const auto& p:next.parameters){auto it=std::find_if(all.parameters.begin(),all.parameters.end(),[&](const auto& x){return x.id==p.id;});if(it==all.parameters.end())all.parameters.push_back(p);else *it=p;}
        for(const auto& m:next.macros){auto it=std::find_if(all.macros.begin(),all.macros.end(),[&](const auto& x){return x.index==m.index;});if(it==all.macros.end())all.macros.push_back(m);else *it=m;}
        if(all.parameters.size()>parameterCount||all.macros.size()>8||all.racks.size()+next.racks.size()>64)return false;
        for(const auto& selection:next.catalogSelections){auto it=std::find_if(all.catalogSelections.begin(),all.catalogSelections.end(),[&](const auto& x){return x.localId==selection.localId;});
            if(it==all.catalogSelections.end())all.catalogSelections.push_back(selection);else if(*it!=selection)return false;}
        for(const auto& p:next.catalogParameters){auto it=std::find_if(all.catalogParameters.begin(),all.catalogParameters.end(),[&](const auto& x){return x.selection==p.selection&&x.unit==p.unit&&x.key==p.key;});
            if(it==all.catalogParameters.end())all.catalogParameters.push_back(p);else *it=p;}
        if(all.catalogSelections.size()>12||all.catalogParameters.size()>12*9*82)return false;
        all.racks.insert(all.racks.end(),next.racks.begin(),next.racks.end());if(next.pattern)all.pattern=next.pattern;
        for(auto pair:{std::pair{&all.reconciliation.detach,&next.reconciliation.detach},std::pair{&all.reconciliation.remove,&next.reconciliation.remove},std::pair{&all.reconciliation.replace,&next.reconciliation.replace}})for(const auto& id:*pair.second){if(std::find(pair.first->begin(),pair.first->end(),id)==pair.first->end())pair.first->push_back(id);if(pair.first->size()>12)return false;}
        for(const auto& r:next.reconciliation.controls){auto it=std::find_if(all.reconciliation.controls.begin(),all.reconciliation.controls.end(),[&](const auto& x){return x.instance==r.instance&&x.control==r.control;});if(it==all.reconciliation.controls.end())all.reconciliation.controls.push_back(r);else *it=r;}
        all.characteristics=next.characteristics;all.explanation=next.explanation;if(next.name.isNotEmpty())all.name=next.name;return all.reconciliation.controls.size()<=96;
    }

    RuntimeStatus materializeCatalog(Connection& c,ai::GenerationRequest request,RuntimeTime now,const GroupGenerationCommand* group=nullptr,
        std::shared_ptr<const ai::LocalPreviewContext> local={})
    {
        const auto tag=request.tag;const auto pending=c.pendingCatalog;
        if(!pending||!request.catalog||!pending->request.catalog)return rejectGeneration(c,tag,RuntimeStatus::staleSession);
        if(bool(local)!=bool(pending->local)||(local&&*local!=*pending->local))return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        if(local&&now>=pending->deadline){clearGeneration(c);return rejectGeneration(c,tag,RuntimeStatus::cancelled);}
        if(local&&!localTicketsAvailable())return rejectGeneration(c,tag,RuntimeStatus::unavailable);
        if(bool(group)!=bool(pending->capture))return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        if(group&&(!captureCurrent(c,*pending->capture,pending->localGroup)||group->selection!=pending->peers
            ||group->parentProposal!=std::optional<ai::RequestTag>(pending->selection.tag)))return rejectGeneration(c,tag,RuntimeStatus::staleSession);
        const auto& context=*request.catalog;const auto& original=*pending->request.catalog;
        if(!context.authority||!original.authority||context.authority->root!=original.authority->root
            ||context.authority->snapshot!=original.authority->snapshot||context.authority->descriptorJson!=original.authority->descriptorJson)
            return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        // Hydration may add only definitions selected by the delivered aggregate.
        for(const auto& definition:context.authority->definitions)
            if(std::none_of(pending->selection.edits.catalogSelections.begin(),pending->selection.edits.catalogSelections.end(),[&](const auto& s){
                return s.design==definition.design&&s.revision==definition.revision&&s.contentSha256==definition.contentSha256;}))
                return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        for(const auto& definition:original.authority->definitions)
            if(std::find(context.authority->definitions.begin(),context.authority->definitions.end(),definition)==context.authority->definitions.end())
                return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        auto expected=pending->request;expected.tag=tag;auto& expectedContext=*expected.catalog;
        expectedContext.phase=ai::CatalogPhase::materialize;expectedContext.selectionParent=pending->selection.tag;
        expectedContext.authority=context.authority;expectedContext.baseIntent=pending->selection.edits;
        expectedContext.inferenceCallsUsed=std::uint8_t(pending->selection.inferenceCalls);
        expectedContext.providerStatus=pending->selection.providerStatus;expectedContext.httpStatus=pending->selection.httpStatus;
        if(!sameRequest(request,expected,local.get()))return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        auto token=std::make_shared<JobCancellation>();
        auto originalRequest=pending->parent?pending->parent->original:std::make_shared<const ai::GenerationRequest>(pending->request);
        const auto admitted=addGeneration(c,std::move(request),token,pending->capture,pending->peers,pending->parent,std::move(originalRequest),
            std::move(local),pending->localGroup,now,pending->deadline);if(admitted!=RuntimeStatus::ready)return admitted;
        c.lastRequestId=tag.requestId;return RuntimeStatus::ready;
    }

    RuntimeStatus groupGeneration(Connection& c,const Frame& frame,RuntimeTime now)
    {
        GroupGenerationCommand command;const auto id=c.id;std::shared_ptr<const ai::LocalPreviewContext> local;
        if(frame.kind==MessageKind::generateGroupLocal){
            auto input=std::make_unique<ai::LocalGroupGenerationCommand>();
            if(!decodeLocalGroupGeneration(bytes(frame.payload),*input)){close(id);return RuntimeStatus::invalidMessage;}
            command.request=std::move(input->input.request);command.selection=std::move(input->selection);command.parentProposal=std::move(input->parentProposal);
            local=std::make_shared<const ai::LocalPreviewContext>(std::move(input->input.local));
        }else if(!decodeGroupGeneration(bytes(frame.payload),command)){close(id);return RuntimeStatus::invalidMessage;}
        auto& request=command.request;const auto tag=request.tag;
        if(tag.runtime!=c.runtime){close(id);return RuntimeStatus::staleSession;}
        if(tag.requestId<=c.lastRequestId)return rejectGeneration(c,tag,RuntimeStatus::staleSession);
        if(local&&!localTicketsAvailable())return rejectGeneration(c,tag,RuntimeStatus::unavailable);
        if(!c.publication||c.publication->loadRevision!=command.selection.loadRevision)return rejectGeneration(c,tag,RuntimeStatus::staleSession);
        if(c.publication->busy||c.accountOutstanding)return rejectGeneration(c,tag,RuntimeStatus::busy);
        if(request.catalog&&request.catalog->phase==ai::CatalogPhase::materialize)return materializeCatalog(c,request,now,&command,local);
        if(local&&!c.localPublication)return rejectGeneration(c,tag,RuntimeStatus::staleSession);
        const auto& intent=*c.publication->project.group;
        if(tag.editRevision!=c.publication->editRevision||request.source.group!=c.publication->project.group
            ||request.role!=groupTextView(intent.role)||request.key!=groupTextView(intent.key)||request.scale!=groupTextView(intent.scale)
            ||(intent.bpm&&request.capturedBpm!=intent.bpm))return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        std::optional<ProposalRecord> parent;
        if(command.parentProposal)
        {
            const auto* retained=local?refinementParent(c,request,local.get(),command.parentProposal):c.proposal?&*c.proposal:nullptr;
            if(!retained||!retained->capture||retained->tag!=*command.parentProposal||!captureCurrent(c,*retained->capture,retained->localGroup)
                ||command.selection.peerCount!=retained->capture->ownership.peers.size())return rejectGeneration(c,tag,RuntimeStatus::staleSession);
            for(std::size_t i=0;i<command.selection.peerCount;++i)
                if(command.selection.members[i]!=retained->capture->ownership.peers[i].project.group->member)return rejectGeneration(c,tag,RuntimeStatus::staleSession);
            if(!local&&!childAuthority(request,*retained,nullptr))return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
            parent=*retained;
        }
        if(!command.parentProposal&&!exactProject(request.source,c.publication->project))return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        if(local&&!command.parentProposal&&local->sourceMapDigest!=c.localPublicationDigest)return rejectGeneration(c,tag,RuntimeStatus::staleSession);
        auto capture=std::make_unique<GroupPerformanceCapture>();
        const auto status=registry.capture(id,{command.selection.members.data(),command.selection.peerCount},capture->ownership);
        if(status!=GroupStatus::ready)return rejectGeneration(c,tag,generationStatus(status));
        capture->requesterEditRevision=c.publication->editRevision;capture->peerCount=command.selection.peerCount;
        auto localPeers=local?std::make_shared<RuntimeLocalGroupContext>():nullptr;
        if(localPeers)localPeers->peerCount=capture->peerCount;
        for(std::size_t i=0;i<capture->peerCount;++i)
        {
            const auto& peer=capture->ownership.peers[i];const auto* owner=publisher(peer.runtime);
            if(!owner||!samePeer(owner->peer,c.peer)||owner->publication->loadRevision!=peer.loadRevision||owner->publication->editRevision!=peer.editRevision)
                return rejectGeneration(c,tag,RuntimeStatus::staleSession);
            if(owner->publication->busy)return rejectGeneration(c,tag,RuntimeStatus::busy);
            capture->playback[i]=owner->publication->playback;age(capture->playback[i],owner->publicationTime,now);
            if(localPeers){auto& projection=localPeers->peers[i];projection.runtime=peer.runtime;projection.loadRevision=peer.loadRevision;projection.editRevision=peer.editRevision;
                projection.metadataAvailable=bool(owner->localPublication);if(owner->localPublication){projection.map=*owner->localPublication;projection.mapDigest=owner->localPublicationDigest;}}
        }
        if(!validateGroupPerformanceCapture(*capture))return rejectGeneration(c,tag,RuntimeStatus::invalidMessage);
        auto immutable=std::make_shared<const GroupPerformanceCapture>(std::move(*capture));auto token=std::make_shared<JobCancellation>();
        auto localAuthority=local?std::make_shared<const LocalGroupAuthority>(LocalGroupAuthority{std::move(localPeers),c.localPublicationDigest}):nullptr;
        const auto admitted=addGeneration(c,std::move(request),token,std::move(immutable),command.selection,std::move(parent),{},std::move(local),std::move(localAuthority),now);
        if(admitted!=RuntimeStatus::ready)return admitted;
        c.lastRequestId=tag.requestId;return RuntimeStatus::ready;
    }
    RuntimeStatus receipt(Connection& c,const Frame& frame,RuntimeTime now)
    {
        const auto id=c.id;ai::RequestTag tag;
        if(frame.payload.getSize()>maximumSmallPayloadBytes||!peekPreviewEvaluateTag(bytes(frame.payload),tag)||tag.runtime!=c.runtime){close(id);return RuntimeStatus::invalidMessage;}
        // A canceled old frame has no nested candidate or audio to decode.
        if((!c.generation||c.generation->tag!=tag)&&tag.requestId<=c.lastRequestId)return RuntimeStatus::ready;
        if(!c.generation||!c.generation->local||!c.generation->exchange||!running||running->connectionId!=id
            ||c.generation->cancellation->cancelled()||now>=c.generation->deadline||!c.generation->preview
            ||!c.generation->previewDelivered){close(id);return RuntimeStatus::invalidMessage;}
        if(c.generation->capture&&!captureCurrent(c,*c.generation->capture,c.generation->localGroup)){close(id);return RuntimeStatus::staleSession;}
        ai::PreviewReceipt measured;
        if(!decodePreviewReceipt(bytes(frame.payload),measured)||measured.binding!=*c.generation->preview
            ||measured.failure==ai::PreviewFailure::invalidBinding||!c.generation->exchange->acceptReceipt(measured)){close(id);return RuntimeStatus::invalidMessage;}
        c.generation->preview.reset();c.generation->previewDelivered=false;return RuntimeStatus::ready;
    }
    RuntimeStatus receive(Connection& c,const Frame& f,RuntimeTime now)
    {
        const auto id=c.id;
        auto invalid=[&](RuntimeStatus status=RuntimeStatus::invalidMessage){close(id);return status;};
        if(f.payload.getSize()>messagePayloadCap(f.kind))return invalid();
        if(!c.registered){
            ControlMessage hello;
            if(c.rejected || f.kind!=MessageKind::hello || !f.session.isNull() || !decodeControl(bytes(f.payload),hello)
                || !zeroIds(hello.tag) || hello.status!=RuntimeStatus::ready)return invalid();
            c.runtime=hello.tag.runtime;
            c.capabilities=hello.capabilities&knownCapabilityMask;
            do {
                c.session=juce::Uuid();
            } while(c.session.isNull() || std::any_of(connections.begin(),connections.end(),[&](const auto& other){return other && other->id!=id && other->session==c.session;}));
            const auto duplicate=std::any_of(connections.begin(),connections.end(),[&](const auto& other){return other && other->registered && other->runtime==c.runtime;});
            c.rejected=duplicate;c.registered=!duplicate;
            const auto status=duplicate?RuntimeStatus::duplicateInstance:RuntimeStatus::ready;
            if(!controlReply(c,MessageKind::welcome,{c.runtime,0,0,0},status,duplicate?ReplyRole::duplicateWelcome:ReplyRole::control,duplicate?0:c.capabilities))return RuntimeStatus::invalidMessage;
            return status;
        }
        if(f.kind!=MessageKind::generate && f.kind!=MessageKind::cancel && f.kind!=MessageKind::account && f.kind!=MessageKind::ping
            &&f.kind!=MessageKind::groupCommand&&f.kind!=MessageKind::spectrumCommand&&f.kind!=MessageKind::generateGroup
            &&f.kind!=MessageKind::generateLocal&&f.kind!=MessageKind::generateGroupLocal&&f.kind!=MessageKind::previewMeasured&&f.kind!=MessageKind::groupCommandLocal)return invalid();
        if(f.session!=c.session)return invalid(RuntimeStatus::staleSession);
        const bool localKind=f.kind==MessageKind::generateLocal||f.kind==MessageKind::generateGroupLocal||f.kind==MessageKind::previewMeasured||f.kind==MessageKind::groupCommandLocal;
        if(localKind&&!(c.capabilities&ownedLocalPreviewCapability))return invalid();
        if(f.kind==MessageKind::previewMeasured)return receipt(c,f,now);
        if(f.kind==MessageKind::groupCommand||f.kind==MessageKind::spectrumCommand||f.kind==MessageKind::groupCommandLocal)return groupCommand(c,f,now);
        if(f.kind==MessageKind::generateGroup||f.kind==MessageKind::generateGroupLocal)return groupGeneration(c,f,now);
        if(f.kind==MessageKind::generate||f.kind==MessageKind::generateLocal){
            ai::GenerationRequest request;std::shared_ptr<const ai::LocalPreviewContext> local;
            if(f.kind==MessageKind::generateLocal){auto input=std::make_unique<ai::LocalGenerationRequest>();
                if(!decodeLocalRequest(bytes(f.payload),*input))return invalid();request=std::move(input->request);local=std::make_shared<const ai::LocalPreviewContext>(std::move(input->local));
            }else if(!decodeRequest(bytes(f.payload),request))return invalid();
            if(request.tag.runtime!=c.runtime)return invalid(RuntimeStatus::staleSession);
            if(c.publication)return rejectGeneration(c,request.tag,RuntimeStatus::invalidMessage);
            if(request.tag.requestId<=c.lastRequestId)return rejectGeneration(c,request.tag,RuntimeStatus::staleSession);
            if(local&&!localTicketsAvailable())return rejectGeneration(c,request.tag,RuntimeStatus::unavailable);
            if(c.accountOutstanding)return rejectGeneration(c,request.tag,RuntimeStatus::busy);
            if(request.catalog&&request.catalog->phase==ai::CatalogPhase::materialize)return materializeCatalog(c,std::move(request),now,nullptr,local);
            std::optional<ProposalRecord> parent;
            if((request.catalog&&request.catalog->root!=request.tag)||(local&&local->root!=request.tag))
            {
                if(!c.proposal||!c.proposal->original)return rejectGeneration(c,request.tag,RuntimeStatus::staleSession);
                if(request.catalog&&(!c.proposal->original->catalog||c.proposal->original->catalog->root!=request.catalog->root))return rejectGeneration(c,request.tag,RuntimeStatus::staleSession);
                const auto* retained=local?refinementParent(c,request,local.get()):&*c.proposal;
                if(!retained||(!local&&!childAuthority(request,*retained,nullptr)))return rejectGeneration(c,request.tag,RuntimeStatus::invalidMessage);
                parent=*retained;
            }
            auto token=std::make_shared<JobCancellation>();const auto tag=request.tag;
            const auto admitted=addGeneration(c,std::move(request),token,{},{},std::move(parent),{},std::move(local),{},now);
            if(admitted!=RuntimeStatus::ready)return admitted;
            c.lastRequestId=tag.requestId;return RuntimeStatus::ready;
        }
        if(f.kind==MessageKind::account){
            AccountCommand command;if(!decodeAccountCommand(bytes(f.payload),command))return invalid();
            if(c.accountOutstanding)return invalid();
            if(generationOutstanding(c)){
                AccountReply busy;busy.status=RuntimeStatus::busy;Frame reply{MessageKind::accountResult,c.session,{}};
                if(!encodeAccountReply(busy,reply.payload))return invalid();
                c.accountOutstanding=true;
                return enqueueReply(c,std::move(reply),{ReplyRole::account,{}},false)?RuntimeStatus::busy:RuntimeStatus::invalidMessage;
            }
            if(queueCount==queue.size())return invalid();
            auto token=std::make_shared<JobCancellation>();
            if(!addJob(c,std::move(command),std::move(token)))return RuntimeStatus::invalidMessage;
            c.accountOutstanding=true;return RuntimeStatus::ready;
        }
        ControlMessage control;if(!decodeControl(bytes(f.payload),control) || control.status!=RuntimeStatus::ready)return invalid();
        if(control.tag.runtime!=c.runtime)return invalid(RuntimeStatus::staleSession);
        if(f.kind==MessageKind::ping){
            if(!zeroIds(control.tag))return invalid();
            return controlReply(c,MessageKind::pong,control.tag,RuntimeStatus::ready)?RuntimeStatus::ready:RuntimeStatus::invalidMessage;
        }
        if(control.tag.requestId==0)return invalid();
        if(c.pendingCatalog&&c.pendingCatalog->selection.tag==control.tag)
        {
            clearGeneration(c);return rejectGeneration(c,control.tag,RuntimeStatus::cancelled);
        }
        if(!c.generation&&c.proposal&&c.proposal->tag==control.tag){eraseProposalAuthority(c);return rejectGeneration(c,control.tag,RuntimeStatus::cancelled);}
        if(!c.generation || control.tag!=c.generation->tag)return rejectGeneration(c,control.tag,RuntimeStatus::staleSession);
        if(c.generation->capture)
        {
            ReplyMeta meta{ReplyRole::generation,control.tag,c.generation->capture,{},false};
            clearGeneration(c);eraseProposalAuthority(c);Frame reply{MessageKind::failure,c.session,{}};
            if(!encodeControl({control.tag,RuntimeStatus::cancelled},reply.payload)){close(id);return RuntimeStatus::invalidMessage;}
            return enqueueReply(c,std::move(reply),std::move(meta),false)?RuntimeStatus::cancelled:RuntimeStatus::invalidMessage;
        }
        clearGeneration(c);return rejectGeneration(c,control.tag,RuntimeStatus::cancelled);
    }
};

bool JobCancellation::cancelled() const noexcept
{ return requested.load(std::memory_order_acquire); }
RuntimeState::RuntimeState() : impl(std::make_unique<Impl>()) {}
RuntimeState::~RuntimeState()
{
    for(auto& c:impl->connections)if(c)impl->close(c->id);
    if(impl->running)Impl::cancelJob(*impl->running);
}
RuntimeConnectionId RuntimeState::open(PipePeer peer,RuntimeTime now)
{
    expire(now);if(peer.processId==0 || peer.creationTime==0)return 0;
    for(auto& slot:impl->connections)if(!slot){
        const auto id=issue(impl->nextConnectionId);if(id==0)return 0;
        slot.emplace();slot->id=id;slot->peer=peer;slot->opened=now;return id;
    }
    return 0;
}
bool RuntimeState::alive(RuntimeConnectionId id) const noexcept { return impl->find(id)!=nullptr; }
std::size_t RuntimeState::connectionCount() const noexcept
{ return std::count_if(impl->connections.begin(),impl->connections.end(),[](const auto& c){return c.has_value();}); }
std::size_t RuntimeState::registeredCount() const noexcept
{ return std::count_if(impl->connections.begin(),impl->connections.end(),[](const auto& c){return c && c->registered;}); }
std::size_t RuntimeState::queuedJobCount() const noexcept { return impl->queueCount; }
bool RuntimeState::jobRunning() const noexcept { return impl->running.has_value(); }
RuntimeStatus RuntimeState::receive(RuntimeConnectionId id,const Frame& frame,RuntimeTime now)
{
    expire(now);auto* c=impl->find(id);if(!c)return RuntimeStatus::staleSession;
    try { return impl->receive(*c,frame,now); }
    catch(const std::bad_alloc&) { impl->close(id);return RuntimeStatus::unavailable; }
    catch(const std::length_error&) { impl->close(id);return RuntimeStatus::unavailable; }
}
bool RuntimeState::takeJob(RuntimeJob& output)
{
    impl->invalidateGroups();
    if(impl->running || impl->queueCount==0)return false;
    try {
        auto copy=*impl->queue[0]; // A failed copy leaves output and ownership intact.
        impl->running=std::move(impl->queue[0]);
        for(std::size_t i=1;i<impl->queueCount;++i)impl->queue[i-1]=std::move(impl->queue[i]);
        impl->queue[--impl->queueCount].reset();output=std::move(copy);return true;
    } catch(const std::bad_alloc&) { return false; }
    catch(const std::length_error&) { return false; }
}
bool RuntimeState::finishJob(const RuntimeJob& job,Frame reply)
{
    impl->invalidateGroups();
    if(!impl->running || !Impl::matching(job,*impl->running))return false;
    auto binding=std::move(*impl->running);impl->running.reset();
    auto* c=impl->find(binding.connectionId);
    if(binding.cancellation->cancelled() || !c || c->session!=binding.session || !samePeer(c->peer,binding.peer))return false;
    try {
        Impl::ReplyMeta meta;
        bool valid=reply.session==binding.session;
        if(const auto* request=std::get_if<ai::GenerationRequest>(&binding.input)){
            if(!c->generation || c->generation->tag!=request->tag || c->generation->cancellation!=binding.cancellation
                ||c->generation->capture!=binding.groupContext)return false;
            meta={Impl::ReplyRole::generation,request->tag};
            if(request->catalog||binding.localPreview)meta.parent=c->generation->parent;
            meta.localGroup=c->generation->localGroup;
            if(binding.localPreview&&(c->generation->local!=binding.localPreview||c->generation->exchange!=binding.previewExchange
                ||c->generation->preview||std::chrono::steady_clock::now()>=binding.localDeadline))valid=false;
            if(binding.groupContext)
            {
                if(!impl->captureCurrent(*c,*binding.groupContext,c->generation->localGroup))return false;
                meta.capture=binding.groupContext;meta.parent=c->generation->parent;
            }
            ai::RequestTag tag;
            if(reply.kind==(binding.localPreview?MessageKind::resultLocal:MessageKind::result))
            {
                valid=valid && (binding.localPreview?peekLocalResultTag(bytes(reply.payload),tag):peekResultTag(bytes(reply.payload),tag)) && tag==request->tag;
                if(binding.localPreview||binding.groupContext||request->catalog)
                {
                    ai::GenerationResult decoded;
                    if(binding.localPreview){auto local=std::make_unique<ai::LocalGenerationResult>();auto input=std::make_unique<ai::LocalGenerationRequest>();
                        input->request=*request;input->local=*binding.localPreview;valid=valid&&decodeLocalResult(bytes(reply.payload),*input,*local);
                        if(valid&&local->numeric.status==ai::GenerationStatus::ready){ai::PreviewReceipt accepted;
                            valid=local->rendered&&binding.previewExchange->lastReceipt(accepted)&&ai::samePreviewReceipt(accepted,*local->rendered)&&accepted.binding.job==binding.jobId;}
                        if(valid)decoded=std::move(local->numeric);
                    }else valid=valid&&decodeResult(bytes(reply.payload),*request,decoded);
                    if(valid)
                    {
                        meta.ready=decoded.status==ai::GenerationStatus::ready;
                        if(decoded.status==ai::GenerationStatus::catalogSelectionRequired)
                        {
                            if(!request->catalog||request->catalog->phase!=ai::CatalogPhase::select)valid=false;
                            else meta.pendingCatalog=std::make_shared<const Impl::PendingCatalog>(Impl::PendingCatalog{
                                *request,std::move(decoded),binding.groupContext,c->generation->selection,c->generation->parent,binding.localPreview,c->generation->localGroup,binding.localDeadline});
                        }
                        else if(meta.ready&&(binding.localPreview||binding.groupContext||request->catalog))
                        {
                            const auto parent=c->generation->parent;
                            auto original=parent?parent->original:c->generation->catalogOriginal?c->generation->catalogOriginal:std::make_shared<const ai::GenerationRequest>(*request);
                            auto aggregate=parent&&parent->aggregate?*parent->aggregate:ai::ProposalEdits{};
                            std::shared_ptr<const ProjectDocument> canonical;
                            std::shared_ptr<const ai::PreviewSampleMap> candidateMap;
                            auto originalLocal=parent?parent->local:binding.localPreview;
                            if(request->catalog)
                            {
                                // Catalog results already carry the complete unlowered root intent.
                                aggregate=decoded.edits;
                                if(original&&original->catalog)
                                {
                                    auto retained=std::make_shared<ai::GenerationRequest>(*original);
                                    retained->catalog->authority=request->catalog->authority;original=std::move(retained);
                                }
                            }
                            if(decoded.status==ai::GenerationStatus::ready)
                            {
                                ProjectDocument admitted;auto policy=original?original->policy:ai::ProposalPolicy{};
                                policy.requirePattern=request->policy.requirePattern;
                                const bool merged=request->catalog||Impl::mergeIntent(aggregate,decoded.edits);
                                auto trace=binding.localPreview?std::make_unique<SourceAssignmentTrace>():nullptr;
                                const auto admission=original&&merged?(request->catalog
                                    ?ai::admitProposal(original->source,policy,aggregate,*request->catalog->authority,admitted,trace.get())
                                    :ai::admitProposal(original->source,policy,aggregate,admitted,trace.get())):ai::ProposalStatus::malformed;
                                if(admission!=ai::ProposalStatus::accepted||!decoded.candidate
                                    ||((request->catalog||binding.localPreview)?!exactProject(admitted,*decoded.candidate)
                                        :admitted.sound.values!=decoded.candidate->sound.values||admitted.sound.layout!=decoded.candidate->sound.layout||admitted.pattern!=decoded.candidate->pattern))valid=false;
                                else {
                                    if(binding.localPreview){auto staged=std::make_unique<ai::PreviewSampleMap>();ai::PreviewDigest digest{};ai::PreviewReceipt measured;
                                        valid=originalLocal&&trace&&ai::stagePreviewMap(originalLocal->sourceMap,*trace,*staged)&&ai::digestPreviewMap(*staged,digest)
                                            &&binding.previewExchange->lastReceipt(measured)&&measured.binding.stagedMap==digest;
                                        if(valid)candidateMap=std::move(staged);
                                    }
                                    if(valid)canonical=std::make_shared<const ProjectDocument>(std::move(admitted));
                                }
                            }
                            if(valid)meta.proposal=Impl::ProposalRecord{request->tag,binding.groupContext,std::move(original),std::move(canonical),std::make_shared<const ai::ProposalEdits>(std::move(aggregate)),std::move(originalLocal),std::move(candidateMap),c->generation->localGroup};
                        }
                    }
                }
            }
            else if(reply.kind==MessageKind::failure){
                ControlMessage control;valid=valid && reply.payload.getSize()<=maximumSmallPayloadBytes
                    && decodeControl(bytes(reply.payload),control) && control.tag==request->tag && control.status!=RuntimeStatus::ready;
            } else valid=false;
        } else {
            AccountReply decoded;meta.role=Impl::ReplyRole::account;
            valid=valid && c->accountOutstanding && reply.kind==MessageKind::accountResult
                && reply.payload.getSize()<=maximumSmallPayloadBytes && decodeAccountReply(bytes(reply.payload),decoded);
        }
        if(!valid){impl->close(binding.connectionId);return false;}
        return impl->enqueueReply(*c,std::move(reply),std::move(meta),true);
    } catch(const std::bad_alloc&) { impl->close(binding.connectionId);return false; }
    catch(const std::length_error&) { impl->close(binding.connectionId);return false; }
}
bool RuntimeState::offerPreview(const RuntimeJob& job,const ai::PreviewEvaluate& value)
{
    impl->invalidateGroups();auto* c=impl->find(job.connectionId);
    if(!impl->running||!Impl::matching(job,*impl->running)||!c||!c->generation||!job.localPreview||!job.previewExchange
        ||c->session!=job.session||!samePeer(c->peer,job.peer)||c->generation->cancellation!=job.cancellation
        ||job.cancellation->cancelled()||std::chrono::steady_clock::now()>=job.localDeadline)return false;
    if(c->jobReply||c->generation->preview)return false;
    try{
        const auto& request=std::get<ai::GenerationRequest>(job.input);const auto& local=*job.localPreview;const auto& b=value.binding;
        const auto ordinal=std::size_t(c->generation->renderOrdinal);const auto phase=request.catalog?request.catalog->phase:ai::CatalogPhase::select;
        bool valid=ai::validPreviewBinding(b)&&b.request==request.tag&&b.root==local.root&&b.catalogSnapshot==local.catalogSnapshot
            &&b.job==job.jobId&&ordinal<3&&b.renderOrdinal==ordinal+1&&b.ticket==c->generation->tickets[ordinal]
            &&b.providerAttempt>=c->generation->providerAttempt&&b.structuralGeneration==request.structuralGeneration
            &&b.sourceWord==local.sourceWord&&b.reset==local.reset&&b.sourceMap==local.sourceMapDigest&&b.phase==phase
            &&(!job.groupContext||impl->captureCurrent(*c,*job.groupContext,c->generation->localGroup));
        valid=valid&&(phase==ai::CatalogPhase::materialize?ordinal==0&&b.providerAttempt==request.catalog->inferenceCallsUsed:b.providerAttempt>c->generation->providerAttempt);
        auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
        auto staged=std::make_unique<ai::PreviewSampleMap>();ai::PreviewDigest digest{};
        const auto admission=request.catalog?ai::admitProposal(request.source,request.policy,value.edits,*request.catalog->authority,*candidate,trace.get())
            :ai::admitProposal(request.source,request.policy,value.edits,*candidate,trace.get());
        valid=valid&&admission==ai::ProposalStatus::accepted&&ai::stagePreviewMap(local.sourceMap,*trace,*staged)
            &&ai::digestPreviewMap(*staged,digest)&&digest==b.stagedMap&&ai::digestPreviewCandidate(*candidate,digest)&&digest==b.candidate
            &&ai::samePreviewContext(value.context,ai::previewContextFor(request,value.edits.pattern,staged.get(),&candidate->sound))
            &&ai::digestPreviewContext(value.context,digest)&&digest==b.context;
        Frame frame{MessageKind::previewEvaluate,job.session,{}};
        if(!valid||!encodePreviewEvaluate(value,frame.payload)){impl->close(job.connectionId);return false;}
        Impl::ReplyMeta meta{Impl::ReplyRole::preview,request.tag};meta.capture=job.groupContext;meta.localGroup=c->generation->localGroup;meta.preview=b;
        if(!impl->enqueueReply(*c,std::move(frame),std::move(meta),true))return false;
        c->generation->preview=b;c->generation->previewDelivered=false;c->generation->renderOrdinal=b.renderOrdinal;c->generation->providerAttempt=b.providerAttempt;return true;
    }catch(const std::bad_alloc&){impl->close(job.connectionId);return false;}
    catch(const std::length_error&){impl->close(job.connectionId);return false;}
}
bool RuntimeState::acceptPreviewReceipt(RuntimeConnectionId id,const Frame& frame,RuntimeTime now)
{
    return frame.kind==MessageKind::previewMeasured&&receive(id,frame,now)==RuntimeStatus::ready;
}
bool RuntimeState::takeReply(RuntimeConnectionId id,Frame& output)
{
    impl->invalidateGroups();
    auto* c=impl->find(id);if(!c || c->started || (!c->controlReply && !c->jobReply))return false;
    auto& slot=(!c->jobReply || (c->controlReply && c->controlReply->order<c->jobReply->order))?c->controlReply:c->jobReply;
    c->started=slot->meta;output=std::move(slot->frame);slot.reset();return true;
}
void RuntimeState::finishReply(RuntimeConnectionId id,bool delivered)
{
    impl->invalidateGroups();
    auto* c=impl->find(id);if(!c || !c->started)return;
    const auto meta=*c->started;c->started.reset();
    if(!delivered || meta.role==Impl::ReplyRole::duplicateWelcome){impl->close(id);return;}
    if(meta.role==Impl::ReplyRole::account)c->accountOutstanding=false;
    if(meta.role==Impl::ReplyRole::preview&&meta.preview&&c->generation&&c->generation->tag==meta.tag
        &&c->generation->preview==meta.preview&&(!meta.capture||impl->captureCurrent(*c,*meta.capture,meta.localGroup)))c->generation->previewDelivered=true;
    if(meta.role==Impl::ReplyRole::generation&&c->generation&&c->generation->tag==meta.tag
        &&(!meta.capture||impl->captureCurrent(*c,*meta.capture,meta.localGroup)))
    {
        if(meta.ready&&c->generation->capture==meta.capture){c->proposal=meta.proposal;
            c->localParent=meta.proposal&&meta.proposal->local?meta.parent:std::nullopt;}
        else if(!meta.ready&&meta.parent&&(!meta.parent->capture||impl->captureCurrent(*c,*meta.parent->capture,meta.parent->localGroup)))c->proposal=meta.parent;
    }
    if(meta.role==Impl::ReplyRole::generation&&meta.pendingCatalog&&c->generation&&c->generation->tag==meta.tag
        &&c->generation->capture==meta.capture&&(!meta.capture||impl->captureCurrent(*c,*meta.capture,meta.localGroup)))c->pendingCatalog=meta.pendingCatalog;
    if(meta.role==Impl::ReplyRole::generation && c->generation && c->generation->tag==meta.tag)c->generation.reset();
}
void RuntimeState::disconnect(RuntimeConnectionId id) noexcept { impl->close(id); }
void RuntimeState::expire(RuntimeTime now) noexcept
{
    for(auto& c:impl->connections)if(c && !c->registered && now-c->opened>=std::chrono::milliseconds(runtimeHelloTimeoutMs))impl->close(c->id);
    for(auto& slot:impl->connections)if(slot&&slot->generation&&slot->generation->local&&now>=slot->generation->deadline){
        const auto id=slot->id;const auto tag=slot->generation->tag;const auto session=slot->session;
        impl->clearGeneration(*slot);Impl::eraseProposalAuthority(*slot);
        try{Frame frame{MessageKind::failure,session,{}};
            if(!encodeControl({tag,RuntimeStatus::cancelled},frame.payload))impl->close(id);
            else impl->enqueueReply(*slot,std::move(frame),{Impl::ReplyRole::generation,tag},true);
        }catch(...){impl->close(id);}
    }
}
void detail::RuntimeStateTestAccess::setNextPreviewTicket(RuntimeState& state,std::uint64_t value) noexcept
{
    if(!state.impl->running&&state.impl->queueCount==0&&state.impl->nextPreviewTicket==1)state.impl->nextPreviewTicket=value;
}
void detail::RuntimeStateTestAccess::observePreviewExchangeAllocation(RuntimeState& state,void* context,void (*observer)(void*) noexcept) noexcept
{
    state.impl->exchangeAllocationContext=context;state.impl->exchangeAllocationObserver=observer;
}
}

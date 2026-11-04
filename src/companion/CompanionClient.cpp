#include "CompanionClient.h"
#include "GenerationLog.h"
#include <optional>
#include <cmath>

namespace airack::companion
{
namespace
{
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
bool validOptions(const ClientOptions& o)
{
    const auto& suffix=o.launch.pipeSuffix;const auto root=o.launch.fixtureRoot.getFullPathName();
    return suffix.isNotEmpty()==root.isNotEmpty()
        && (suffix.isEmpty() || (suffix.length()<=32 && suffix.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") && juce::File::isAbsolutePath(root)));
}
}
struct CompanionClient::Impl
{
    juce::Uuid runtime,session=juce::Uuid::null();ClientClock clock;ClientOptions options;
    PipeConnection pipe;CompanionLaunch launch;ClientStatus state;
    std::optional<Frame> started,newest,groupNewest;
    std::optional<GroupCommand> pendingGroup;
    std::optional<GroupReply> groupReply;
    std::optional<ai::GenerationRequest> captured;
    std::optional<ai::LocalPreviewContext> localCaptured;
    struct LocalCatalogClock {
        ai::RequestTag selection,root;juce::Uuid snapshot;
        std::chrono::steady_clock::time_point began;
    };
    std::optional<LocalCatalogClock> localCatalogClock;
    std::optional<ai::PreviewEvaluate> previewEvaluation;
    std::optional<ai::PreviewBinding> outstandingPreview;
    std::optional<ai::PreviewReceipt> lastPreviewReceipt;
    std::uint64_t previewJob=0,previewTicket=0;
    std::uint8_t previewOrdinal=0,previewAttempt=0;
    std::uint32_t previewBodyFrames=0;
    bool previewTruncated=false;
    std::optional<ai::RequestTag> latestTag,dispatchedTag,cancelTag,remoteProposalTag;
    std::optional<ClientGenerationReply> generationReply;
    std::optional<AccountReply> accountReply;
    std::optional<std::chrono::steady_clock::time_point> lastTime,startupTime,operationTime,groupTime;
    std::uint64_t highWater=0,groupHighWater=0;
    std::uint32_t negotiatedCapabilities=0;
    PipePeer helloPeer{},fallbackPeer{};
    bool legacyHandshake=false,helloSent=false;
    bool reading=false,launched=false,generation=false,suppressed=false,account=false,groupedGeneration=false;
    Impl(juce::Uuid r,ClientClock c):runtime(r),clock(std::move(c)){}

    void clearPreview() noexcept
    {
        previewEvaluation.reset();outstandingPreview.reset();lastPreviewReceipt.reset();
        previewJob=0;previewTicket=0;previewOrdinal=0;previewAttempt=0;previewBodyFrames=0;previewTruncated=false;
    }
    std::uint32_t offeredCapabilities() const noexcept
    {return (options.negotiateSpectrum&&!legacyHandshake?spectrumCapability:0u)|(options.negotiateOwnedPreview?ownedLocalPreviewCapability:0u);}
    void retire() noexcept
    {
        // Accepted native close owns cancellation retirement (normally <=250ms;
        // exceptional retained storage poisons admission). It never kills peers.
        pipe.close();launch.release();session=juce::Uuid::null();reading=false;launched=false;
        started.reset();newest.reset();captured.reset();latestTag.reset();dispatchedTag.reset();cancelTag.reset();remoteProposalTag.reset();
        generationReply.reset();accountReply.reset();startupTime.reset();operationTime.reset();generation=false;suppressed=false;account=false;
        groupNewest.reset();pendingGroup.reset();groupReply.reset();groupTime.reset();
        localCaptured.reset();localCatalogClock.reset();clearPreview();negotiatedCapabilities=0;helloPeer={};fallbackPeer={};legacyHandshake=false;helloSent=false;
    }
    void fail(const char* code,RuntimeStatus status=RuntimeStatus::invalidMessage)
    {
        try{diagnostics::Event event;event.stage=diagnostics::Stage::connection;event.runtime=runtime;event.session=session;event.errorCode=code;event.runtimeStatus=int(status);
            if(latestTag)event.requestId=latestTag->requestId;diagnostics::write(event);}catch(...){}
        retire();state={ClientPhase::failed,status,code};
    }
    void refresh() noexcept
    {
        if(state.phase==ClientPhase::ready)state.status=(generation || account || cancelTag || started || newest || pendingGroup)?RuntimeStatus::busy:RuntimeStatus::ready;
    }
    bool time(std::chrono::steady_clock::time_point& value)
    {
        try{value=clock?clock():std::chrono::steady_clock::now();if(lastTime && value<*lastTime){fail("clock_failure");return false;}lastTime=value;return true;}
        catch(...){fail("clock_failure");return false;}
    }
    bool ioFailure(PipeStatus s)
    {
        // Old wire5 decoders close the optional capability hello. Retry only
        // that startup disconnect, once, to the same authenticated process.
        if(s==PipeStatus::closed&&state.phase==ClientPhase::connecting&&options.negotiateSpectrum&&!options.negotiateOwnedPreview&&!legacyHandshake&&helloSent){
            fallbackPeer=helloPeer;legacyHandshake=true;helloSent=false;
            pipe.close();reading=false;started.reset();newest.reset();return false;
        }
        if(s==PipeStatus::unauthorized)fail("pipe_unauthorized",RuntimeStatus::unauthorized);
        else if(s==PipeStatus::invalidMessage)fail("invalid_message");
        else fail("pipe_unavailable",RuntimeStatus::unavailable);
        return false;
    }
    bool beginWrite()
    {
        if(started || (!newest && !groupNewest))return true;
        auto& next=newest?newest:groupNewest;
        const auto status=pipe.beginWrite(*next);if(status!=PipeStatus::ready)return ioFailure(status);
        // The exact submitted frame stays owned until native completion/close.
        started=std::move(next);next.reset();
        if(started->kind==MessageKind::generate || started->kind==MessageKind::generateGroup
            ||started->kind==MessageKind::generateLocal||started->kind==MessageKind::generateGroupLocal){
            ai::RequestTag tag;const bool local=started->kind==MessageKind::generateLocal||started->kind==MessageKind::generateGroupLocal;
            if(!(local?peekLocalResultTag(bytes(started->payload),tag):peekResultTag(bytes(started->payload),tag))){fail("invalid_message");return false;}dispatchedTag=tag;
        }
        return true;
    }
    bool canGenerate(const ai::GenerationRequest& request) const noexcept
    {return state.phase==ClientPhase::ready && !account && !pendingGroup && !localCaptured && request.tag.runtime==runtime && request.tag.requestId>highWater;}

    void cancelRemoteProposal(ai::RequestTag tag)
    {
        Frame cancel{MessageKind::cancel,session,{}};if(!encodeControl({tag,RuntimeStatus::ready},cancel.payload)){fail("invalid_message");return;}cancelTag=tag;newest=std::move(cancel);beginWrite();
    }

    bool submitGeneration(const ai::GenerationRequest& request,Frame frame,const ai::LocalPreviewContext* local=nullptr)
    {
        auto capture=request;auto localCopy=local?std::optional<ai::LocalPreviewContext>(*local):std::nullopt;juce::MemoryBlock allocated;if(!encodeFrame(frame,allocated))return false;
        std::chrono::steady_clock::time_point now;if(!time(now))return false;
        auto began=now;
        if(local&&request.catalog&&request.catalog->phase==ai::CatalogPhase::materialize){
            if(!localCatalogClock||request.catalog->selectionParent!=localCatalogClock->selection
                ||request.catalog->root!=localCatalogClock->root||local->catalogSnapshot!=localCatalogClock->snapshot)return false;
            began=localCatalogClock->began;
            if(now-began>=std::chrono::milliseconds(clientOperationTimeoutMs)){fail("request_timeout",RuntimeStatus::unavailable);return false;}
        }
        groupedGeneration=frame.kind==MessageKind::generateGroup||frame.kind==MessageKind::generateGroupLocal;localCaptured=std::move(localCopy);clearPreview();latestTag=capture.tag;highWater=capture.tag.requestId;captured=std::move(capture);newest=std::move(frame);generation=true;suppressed=false;generationReply.reset();localCatalogClock.reset();operationTime=began;
        if(local&&local->root==request.tag)remoteProposalTag.reset();
        // An unsent cancellation is replaceable; a submitted one stays intact.
        if(cancelTag && (!started || started->kind!=MessageKind::cancel))cancelTag.reset();
        const bool ok=beginWrite();refresh();return ok;
    }
    bool submitGroup(const GroupCommand& command,Frame frame)
    {
        auto capture=command;juce::MemoryBlock allocated;if(!encodeFrame(frame,allocated))return false;
        std::chrono::steady_clock::time_point now;if(!time(now))return false;
        groupHighWater=capture.operationId;pendingGroup=std::move(capture);groupNewest=std::move(frame);groupTime=now;
        const bool ok=beginWrite();refresh();return ok;
    }
    bool beginRead()
    {if(reading)return true;const auto s=pipe.beginRead();if(s!=PipeStatus::ready)return ioFailure(s);reading=true;return true;}
    void connect()
    {
        const auto s=pipe.connect(options.launch.pipeSuffix);
        if(s==PipeStatus::ready){
            const auto peer=pipe.peer();if(peer.processId==0 || peer.creationTime==0){fail("pipe_unavailable",RuntimeStatus::unavailable);return;}
            if(fallbackPeer.processId&&(peer.processId!=fallbackPeer.processId||peer.creationTime!=fallbackPeer.creationTime)){fail("pipe_unauthorized",RuntimeStatus::unauthorized);return;}
            Frame hello{MessageKind::hello,juce::Uuid::null(),{}};
            if(!encodeControl({{runtime,0,0,0},RuntimeStatus::ready,offeredCapabilities()},hello.payload)){fail("invalid_message");return;}
            helloPeer=peer;helloSent=true;
            newest=std::move(hello);if(beginWrite())beginRead();return;
        }
        if(s==PipeStatus::busy)return;
        if(s!=PipeStatus::unavailable){ioFailure(s);return;}
        if(fallbackPeer.processId)return;
        if(options.allowLaunch && !launched){
            launched=true;auto launchOptions=options.launch;
            if(launchOptions.executable.getFullPathName().isEmpty())launchOptions.executable=companionExecutable();
            if(launch.start(launchOptions).status==LaunchStatus::unavailable)fail("pipe_unavailable",RuntimeStatus::unavailable);
        }
    }
    bool authority(const ai::RequestTag& tag)
    {
        if(!latestTag || tag.runtime!=runtime || tag.requestId>latestTag->requestId || (tag.requestId==latestTag->requestId && tag!=*latestTag)){fail("invalid_message");return false;}
        return true;
    }
    bool previewAuthority(const ai::PreviewEvaluate& value)
    {
        if(!captured||!localCaptured)return false;
        const auto& request=*captured;const auto& local=*localCaptured;const auto& b=value.binding;
        if(b.request!=request.tag||b.root!=local.root||b.catalogSnapshot!=local.catalogSnapshot||b.reset!=local.reset
            ||b.sourceWord!=local.sourceWord||b.structuralGeneration!=request.structuralGeneration||b.sourceMap!=local.sourceMapDigest
            ||b.phase!=(request.catalog?request.catalog->phase:ai::CatalogPhase::select)
            ||b.renderOrdinal!=previewOrdinal+1
            ||(request.catalog&&request.catalog->phase==ai::CatalogPhase::materialize
                ?previewOrdinal!=0||b.providerAttempt!=request.catalog->inferenceCallsUsed:b.providerAttempt<=previewAttempt)
            ||(previewJob&&b.job!=previewJob)||b.ticket<=previewTicket)return false;
        auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
        const auto status=request.catalog?ai::admitProposal(request.source,request.policy,value.edits,*request.catalog->authority,*candidate,trace.get())
            :ai::admitProposal(request.source,request.policy,value.edits,*candidate,trace.get());
        ai::PreviewSampleMap staged;ai::PreviewDigest digest;
        if(status!=ai::ProposalStatus::accepted||!ai::stagePreviewMap(local.sourceMap,*trace,staged))return false;
        const auto context=ai::previewContextFor(request,value.edits.pattern,&staged,&candidate->sound);
        if(!ai::digestPreviewMap(staged,digest)||digest!=b.stagedMap||!ai::digestPreviewCandidate(*candidate,digest)||digest!=b.candidate
            ||!ai::samePreviewContext(value.context,context)||!ai::digestPreviewContext(context,digest)||digest!=b.context)return false;
        const auto frames=std::floor(patternLengthBeats(context.phrase)*60/context.bpm*ai::previewSampleRate+.5);
        previewBodyFrames=std::uint32_t(std::max(1.,std::min(frames,double(ai::maximumPreviewBodyFrames))));previewTruncated=frames>ai::maximumPreviewBodyFrames;
        return true;
    }
    void receive(const Frame& f)
    {
        if(state.phase==ClientPhase::connecting){
            ControlMessage welcome;
            if(f.kind!=MessageKind::welcome || f.session.isNull() || !decodeControl(bytes(f.payload),welcome)
                || welcome.tag!=ai::RequestTag{runtime,0,0,0}){fail("invalid_message");return;}
            if(welcome.status==RuntimeStatus::duplicateInstance){fail("invalid_message",RuntimeStatus::duplicateInstance);return;}
            if(welcome.status!=RuntimeStatus::ready){fail("invalid_message");return;}
            if(welcome.capabilities&~offeredCapabilities()){fail("invalid_message");return;}
            negotiatedCapabilities=welcome.capabilities;
            session=f.session;startupTime.reset();state={ClientPhase::ready,RuntimeStatus::ready,{}};
            try{diagnostics::Event event;event.stage=diagnostics::Stage::connection;event.runtime=runtime;event.session=session;diagnostics::write(event);}catch(...){}
            refresh();return;
        }
        if(state.phase!=ClientPhase::ready || f.session!=session){fail("invalid_message");return;}
        if(f.kind==MessageKind::groupReply||f.kind==MessageKind::spectrumReply){
            GroupReply reply;
            if(!decodeGroupReply(bytes(f.payload),reply) || reply.runtime!=GroupUuid(runtime)){fail("invalid_message");return;}
            if((f.kind==MessageKind::spectrumReply)!=spectrumAction(reply.action)||(spectrumAction(reply.action)&&!(negotiatedCapabilities&spectrumCapability))){fail("invalid_message");return;}
            // A fully decoded older reply grants no authority, even if its
            // original action/load differs from the outstanding command.
            if(reply.operationId<groupHighWater)return;
            if(!pendingGroup || groupReply || reply.operationId!=groupHighWater
                || reply.action!=pendingGroup->action || reply.loadRevision!=pendingGroup->loadRevision
                || reply.proposal!=pendingGroup->proposal){fail("invalid_message");return;}
            if(reply.status==GroupReplyStatus::ready && (reply.action==GroupAction::admit || reply.action==GroupAction::publish)){
                auto expected=pendingGroup->publication->project.group;
                if(reply.action==GroupAction::admit)expected->member=reply.normalized->member;
                if(reply.normalized!=expected){fail("invalid_message");return;}
            }
            groupReply.emplace(std::move(reply));refresh();return;
        }
        if(f.kind==MessageKind::previewEvaluate){
            ai::RequestTag tag;if(!(negotiatedCapabilities&ownedLocalPreviewCapability)||!peekPreviewEvaluateTag(bytes(f.payload),tag)||!authority(tag)){if(state.phase!=ClientPhase::failed)fail("invalid_message");return;}
            if(tag.requestId<latestTag->requestId||suppressed)return;
            if(!generation||!captured||!localCaptured||generationReply){fail("invalid_message");return;}
            auto value=std::make_unique<ai::PreviewEvaluate>();if(!decodePreviewEvaluate(bytes(f.payload),*value)){fail("invalid_message");return;}
            if((outstandingPreview&&value->binding==*outstandingPreview)||(lastPreviewReceipt&&value->binding==lastPreviewReceipt->binding))return;
            if(outstandingPreview||previewEvaluation||!previewAuthority(*value)){fail("invalid_message");return;}
            previewJob=value->binding.job;previewTicket=value->binding.ticket;previewOrdinal=value->binding.renderOrdinal;previewAttempt=value->binding.providerAttempt;
            outstandingPreview=value->binding;previewEvaluation=std::move(*value);return;
        }
        if(f.kind==MessageKind::resultLocal){
            ai::RequestTag tag;if(!(negotiatedCapabilities&ownedLocalPreviewCapability)||!peekLocalResultTag(bytes(f.payload),tag)||!authority(tag)){if(state.phase!=ClientPhase::failed)fail("invalid_message");return;}
            if(tag.requestId<latestTag->requestId||suppressed)return;
            if(!generation||!captured||!localCaptured||generationReply){fail("invalid_message");return;}
            auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=*captured;input->local=*localCaptured;
            auto result=std::make_unique<ai::LocalGenerationResult>();
            if(!decodeLocalResult(bytes(f.payload),*input,*result)
                ||(result->numeric.status==ai::GenerationStatus::ready&&(outstandingPreview||!lastPreviewReceipt||!result->rendered||!ai::samePreviewReceipt(*lastPreviewReceipt,*result->rendered)))){fail("invalid_message");return;}
            if(cancelTag&&cancelTag->requestId<tag.requestId)cancelTag.reset();
            if(result->numeric.status==ai::GenerationStatus::ready)remoteProposalTag=result->numeric.tag;
            if(result->numeric.status==ai::GenerationStatus::catalogSelectionRequired){
                if(!captured->catalog||!operationTime){fail("invalid_message");return;}
                localCatalogClock=LocalCatalogClock{result->numeric.tag,localCaptured->root,localCaptured->catalogSnapshot,*operationTime};
            }else localCatalogClock.reset();
            previewEvaluation.reset();outstandingPreview.reset();generationReply.emplace(std::move(*result));operationTime.reset();dispatchedTag.reset();refresh();return;
        }
        if(f.kind==MessageKind::result){
            ai::RequestTag tag;if(!peekResultTag(bytes(f.payload),tag) || !authority(tag)){if(state.phase!=ClientPhase::failed)fail("invalid_message");return;}
            // Discard grants no authority, so older/suppressed audio is never
            // allocated or decoded. F6.4 rechecks current edit/context revisions.
            if(tag.requestId<latestTag->requestId || suppressed)return;
            if(!generation || !captured || generationReply){fail("invalid_message");return;}
            ai::GenerationResult result;if(localCaptured||!decodeResult(bytes(f.payload),*captured,result)){fail("invalid_message");return;}
            // A validated newer terminal reply settles only obsolete logical
            // cancellation. Submitted native frame ownership stays intact.
            if(cancelTag && cancelTag->requestId<tag.requestId)cancelTag.reset();
            if(groupedGeneration&&result.status==ai::GenerationStatus::ready)remoteProposalTag=result.tag;generationReply.emplace(std::move(result));operationTime.reset();dispatchedTag.reset();refresh();return;
        }
        if(f.kind==MessageKind::failure){
            ControlMessage message;
            if(!decodeControl(bytes(f.payload),message) || message.status==RuntimeStatus::ready || !authority(message.tag)){if(state.phase!=ClientPhase::failed)fail("invalid_message");return;}
            if(cancelTag && message.tag==*cancelTag && (message.status==RuntimeStatus::cancelled || message.status==RuntimeStatus::staleSession)){
                // A delayed ACK belongs only to its cancelled remote request.
                // A newer generation may already own dispatchedTag.
                if(dispatchedTag && *dispatchedTag==*cancelTag)dispatchedTag.reset();
                cancelTag.reset();if(suppressed){generation=false;captured.reset();localCaptured.reset();clearPreview();operationTime.reset();}refresh();return;
            }
            if(message.tag.requestId<latestTag->requestId || suppressed)return;
            if(!generation || generationReply){fail("invalid_message");return;}
            if(cancelTag && cancelTag->requestId<message.tag.requestId)cancelTag.reset();
            previewEvaluation.reset();outstandingPreview.reset();generationReply.emplace(message);operationTime.reset();dispatchedTag.reset();refresh();return;
        }
        if(f.kind==MessageKind::accountResult && account && !accountReply){
            AccountReply reply;if(!decodeAccountReply(bytes(f.payload),reply)){fail("invalid_message");return;}
            try{diagnostics::Event event;event.stage=diagnostics::Stage::account;event.runtime=runtime;event.session=session;
                event.providerStatus=int(reply.providerStatus);event.httpStatus=reply.httpStatus;event.errorCode=reply.errorCode;event.model=reply.selectedModel;event.runtimeStatus=int(reply.status);diagnostics::write(event);}catch(...){}
            accountReply.emplace(std::move(reply));operationTime.reset();refresh();return;
        }
        fail("invalid_message");
    }
};
CompanionClient::CompanionClient(juce::Uuid runtime,ClientClock clock):impl(std::make_unique<Impl>(runtime,std::move(clock))){}
CompanionClient::~CompanionClient(){close();}
juce::Uuid CompanionClient::runtime() const noexcept{return impl->runtime;}
juce::Uuid CompanionClient::session() const noexcept{return impl->session;}
PipePeer CompanionClient::peer() const noexcept{return impl->pipe.peer();}
ClientStatus CompanionClient::status() const{return impl->state;}
std::uint32_t CompanionClient::capabilities() const noexcept{return impl->state.phase==ClientPhase::ready?impl->negotiatedCapabilities:0;}
bool CompanionClient::start(const ClientOptions& options)
{
    // Preparation failures reject this attempt while preserving the incumbent.
    std::optional<ClientOptions> copy;
    try{if(!validOptions(options))return false;copy.emplace(options);}
    catch(...){return false;}
    try{
        // Validate/copy before retiring an incumbent. Missing executable is valid
        // because a native service can be reused without any launcher handle.
        if(impl->runtime.isNull()){impl->fail("invalid_runtime");return false;}
        std::chrono::steady_clock::time_point now;if(!impl->time(now))return false;
        impl->retire();impl->options=std::move(*copy);impl->startupTime=now;impl->state={ClientPhase::connecting,RuntimeStatus::busy,{}};
        try{diagnostics::Event event;event.stage=diagnostics::Stage::startup;event.runtime=impl->runtime;diagnostics::write(event);}catch(...){}
        impl->connect();return impl->state.phase!=ClientPhase::failed;
    }catch(...){impl->fail("invalid_message");return false;}
}
void CompanionClient::poll()
{
    if(impl->state.phase!=ClientPhase::connecting && impl->state.phase!=ClientPhase::ready)return;
    try{
        std::chrono::steady_clock::time_point now;if(!impl->time(now))return;
        const bool heldGroupReply=impl->groupReply.has_value();
        if(!impl->pipe.connected())impl->connect();
        if(impl->state.phase==ClientPhase::failed)return;
        if(impl->pipe.connected()){
            for(unsigned i=0;i<clientPollBudget && impl->started;++i){
                const auto s=impl->pipe.pollWrite();if(s==PipeStatus::pending)continue;
                if(s!=PipeStatus::complete){impl->ioFailure(s);return;}impl->started.reset();if(!impl->beginWrite())return;
            }
            for(unsigned i=0;i<clientPollBudget;++i){
                if(!impl->beginRead())return;Frame f;const auto s=impl->pipe.pollRead(f);if(s==PipeStatus::pending)continue;
                impl->reading=false;if(s!=PipeStatus::complete){impl->ioFailure(s);return;}impl->receive(f);if(impl->state.phase==ClientPhase::failed)return;
            }
        }
        if(impl->startupTime && now-*impl->startupTime>=std::chrono::milliseconds(clientStartupTimeoutMs)){impl->fail("startup_timeout",RuntimeStatus::unavailable);return;}
        if(impl->operationTime && now-*impl->operationTime>=std::chrono::milliseconds(clientOperationTimeoutMs)){impl->fail("request_timeout",RuntimeStatus::unavailable);return;}
        if(impl->localCatalogClock&&!impl->generationReply&&now-impl->localCatalogClock->began>=std::chrono::milliseconds(clientOperationTimeoutMs)){impl->fail("request_timeout",RuntimeStatus::unavailable);return;}
        // A reply read by this poll gets one take before its deadline applies, however late it was read.
        if(impl->groupTime && (heldGroupReply || !impl->groupReply) && now-*impl->groupTime>=std::chrono::milliseconds(clientGroupOperationTimeoutMs)){impl->fail("group_timeout",RuntimeStatus::unavailable);return;}
        impl->refresh();
    }catch(...){impl->fail("invalid_message");}
}
bool CompanionClient::generate(const ai::GenerationRequest& request)
{
    if(!impl->canGenerate(request))return false;
    try{
        Frame frame{MessageKind::generate,impl->session,{}};if(!encodeRequest(request,frame.payload))return false;
        return impl->submitGeneration(request,std::move(frame));
    }catch(...){return false;}
}
bool CompanionClient::account(const AccountCommand& command)
{
    if(impl->state.phase!=ClientPhase::ready || impl->generation || impl->account || impl->cancelTag || impl->started || impl->newest || impl->pendingGroup)return false;
    try{
        Frame frame{MessageKind::account,impl->session,{}};if(!encodeAccountCommand(command,frame.payload))return false;
        juce::MemoryBlock allocated;if(!encodeFrame(frame,allocated))return false;
        std::chrono::steady_clock::time_point now;if(!impl->time(now))return false;
        impl->newest=std::move(frame);impl->account=true;impl->operationTime=now;const bool ok=impl->beginWrite();impl->refresh();return ok;
    }catch(...){return false;}
}
void CompanionClient::cancelGeneration() noexcept
{
    if(impl->state.phase!=ClientPhase::ready || (!impl->generation&&!impl->remoteProposalTag&&!impl->localCatalogClock))return;
    try{
        const auto selection=impl->localCatalogClock;
        // A ready parent owns any pending child selection on the server.
        const auto remote=impl->remoteProposalTag?impl->remoteProposalTag:(selection?std::optional<ai::RequestTag>(selection->selection):std::nullopt);
        if(selection)impl->operationTime=selection->began;
        impl->localCatalogClock.reset();impl->remoteProposalTag.reset();const bool active=impl->generation;impl->suppressed=true;impl->generation=false;impl->clearPreview();
        if(!active){if(remote)impl->cancelRemoteProposal(*remote);impl->refresh();return;}
        if(impl->generationReply){impl->generationReply.reset();impl->captured.reset();impl->localCaptured.reset();impl->operationTime=selection?std::optional(selection->began):std::nullopt;if(remote)impl->cancelRemoteProposal(*remote);impl->refresh();return;}
        if(impl->newest && (impl->newest->kind==MessageKind::generate || impl->newest->kind==MessageKind::generateGroup||impl->newest->kind==MessageKind::generateLocal||impl->newest->kind==MessageKind::generateGroupLocal||impl->newest->kind==MessageKind::previewMeasured))impl->newest.reset();
        if(impl->dispatchedTag){
            Frame cancel{MessageKind::cancel,impl->session,{}};
            if(!encodeControl({*impl->dispatchedTag,RuntimeStatus::ready},cancel.payload)){impl->fail("invalid_message");return;}
            impl->cancelTag=impl->dispatchedTag;impl->newest=std::move(cancel);impl->beginWrite();
        }else{impl->captured.reset();impl->localCaptured.reset();impl->operationTime.reset();if(remote)impl->cancelRemoteProposal(*remote);}
        impl->refresh();
    }catch(...){impl->retire();impl->state.phase=ClientPhase::failed;impl->state.status=RuntimeStatus::invalidMessage;}
}
void CompanionClient::cancelAccount() noexcept{if(impl->account)close();}
bool CompanionClient::takeGeneration(ClientGenerationReply& output)
{
    if(!impl->generationReply)return false;output=std::move(*impl->generationReply);impl->generationReply.reset();impl->generation=false;impl->captured.reset();impl->localCaptured.reset();impl->clearPreview();impl->operationTime.reset();impl->refresh();return true;
}
bool CompanionClient::takeAccount(AccountReply& output)
{
    if(!impl->accountReply)return false;output=std::move(*impl->accountReply);impl->accountReply.reset();impl->account=false;impl->operationTime.reset();impl->refresh();return true;
}
void CompanionClient::close() noexcept{impl->retire();impl->state={};}
bool detail::CompanionClientTestAccess::hasStartedWrite(const CompanionClient& c) noexcept{return c.impl->started.has_value();}
void* detail::CompanionClientTestAccess::borrowProcessHandle(const CompanionClient& c) noexcept{return CompanionLaunchTestAccess::borrowProcessHandle(c.impl->launch);}
bool CompanionClient::group(const GroupCommand& command)
{
    if(impl->state.phase!=ClientPhase::ready || impl->pendingGroup || command.runtime!=GroupUuid(impl->runtime)
        || command.operationId<=impl->groupHighWater||(spectrumAction(command.action)&&!(impl->negotiatedCapabilities&spectrumCapability)))return false;
    try{
        Frame frame{spectrumAction(command.action)?MessageKind::spectrumCommand:MessageKind::groupCommand,impl->session,{}};if(!encodeGroupCommand(command,frame.payload))return false;
        return impl->submitGroup(command,std::move(frame));
    }catch(...){return false;}
}
bool CompanionClient::generateGroup(const GroupGenerationCommand& command)
{
    if(!impl->canGenerate(command.request))return false;
    try{
        Frame frame{MessageKind::generateGroup,impl->session,{}};if(!encodeGroupGeneration(command,frame.payload))return false;
        const bool submitted=impl->submitGeneration(command.request,std::move(frame));if(submitted)impl->remoteProposalTag=command.parentProposal;return submitted;
    }catch(...){return false;}
}
bool CompanionClient::generateLocal(const ai::LocalGenerationRequest& request)
{
    if(!impl->canGenerate(request.request)||impl->generation||impl->cancelTag||!(impl->negotiatedCapabilities&ownedLocalPreviewCapability))return false;
    try{Frame frame{MessageKind::generateLocal,impl->session,{}};if(!encodeLocalRequest(request,frame.payload))return false;
        return impl->submitGeneration(request.request,std::move(frame),&request.local);}catch(...){return false;}
}
bool CompanionClient::generateGroupLocal(const ai::LocalGroupGenerationCommand& command)
{
    if(!impl->canGenerate(command.input.request)||impl->generation||impl->cancelTag||!(impl->negotiatedCapabilities&ownedLocalPreviewCapability))return false;
    try{Frame frame{MessageKind::generateGroupLocal,impl->session,{}};if(!encodeLocalGroupGeneration(command,frame.payload))return false;
        const bool submitted=impl->submitGeneration(command.input.request,std::move(frame),&command.input.local);if(submitted)impl->remoteProposalTag=command.parentProposal;return submitted;}catch(...){return false;}
}
bool CompanionClient::takePreviewEvaluation(ai::PreviewEvaluate& output)
{
    if(!impl->previewEvaluation||impl->suppressed)return false;
    output=std::move(*impl->previewEvaluation);impl->previewEvaluation.reset();return true;
}
bool CompanionClient::submitPreviewReceipt(const ai::PreviewReceipt& receipt)
{
    if(impl->state.phase!=ClientPhase::ready||!impl->generation||impl->suppressed||impl->newest||impl->cancelTag||impl->previewEvaluation
        ||!impl->outstandingPreview||receipt.binding!=*impl->outstandingPreview
        ||(impl->started&&(impl->started->kind==MessageKind::generateLocal||impl->started->kind==MessageKind::generateGroupLocal)))return false;
    try{
        const bool measured=receipt.status==ai::RenderStatus::ready||receipt.status==ai::RenderStatus::clipped||receipt.status==ai::RenderStatus::nonFinite;
        if(measured&&(receipt.bodyFrames!=impl->previewBodyFrames||receipt.phraseTruncated!=impl->previewTruncated))return false;
        Frame frame{MessageKind::previewMeasured,impl->session,{}};if(!encodePreviewReceipt(receipt,frame.payload))return false;
        juce::MemoryBlock represented;if(!encodeFrame(frame,represented))return false;
        impl->lastPreviewReceipt=receipt;impl->outstandingPreview.reset();impl->newest=std::move(frame);return impl->beginWrite();
    }catch(...){return false;}
}
bool CompanionClient::groupLocal(const GroupCommand& command,const ai::PreviewSampleMap& map)
{
    if(impl->state.phase!=ClientPhase::ready||impl->pendingGroup||!(impl->negotiatedCapabilities&ownedLocalPreviewCapability)
        ||command.runtime!=GroupUuid(impl->runtime)||command.operationId<=impl->groupHighWater)return false;
    try{Frame frame{MessageKind::groupCommandLocal,impl->session,{}};if(!encodeLocalGroupPublication(command,map,frame.payload))return false;
        return impl->submitGroup(command,std::move(frame));}catch(...){return false;}
}
bool CompanionClient::takeGroup(GroupReply& output)
{
    if(!impl->groupReply||(impl->started&&impl->started->kind==MessageKind::groupCommandLocal)
        ||(impl->groupNewest&&impl->groupNewest->kind==MessageKind::groupCommandLocal))return false;
    output=std::move(*impl->groupReply);impl->groupReply.reset();impl->pendingGroup.reset();impl->groupTime.reset();impl->refresh();return true;
}
void detail::CompanionClientTestAccess::withholdCompletion(CompanionClient& c,bool value){PipeTestAccess::withholdCompletion(c.impl->pipe,value);}
bool detail::CompanionClientTestAccess::hasGroupReply(const CompanionClient& c) noexcept{return c.impl->groupReply.has_value();}
bool detail::CompanionClientTestAccess::hasAccountReply(const CompanionClient& c) noexcept{return c.impl->accountReply.has_value();}
}

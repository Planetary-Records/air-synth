#include "CompanionBackend.h"
#include "GenerationLog.h"
#include "LocalPreviewExchange.h"
#include "StrictJson.h"
#include "RackLibrary.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace airack::companion
{
namespace
{
namespace oauth=rack::oauth;
struct SignInTiming { std::atomic_bool pause{false},observed{false}; };
SignInTiming& timing(){static SignInTiming value;return value;}
bool slug(const juce::String& value)
{return value.length()<=256 && value.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-");}
bool directoriesValid(const BackendOptions& options)
{
    const auto& a=options.credentialDirectory;const auto& b=options.preferenceDirectory;
    return a.getFullPathName().isNotEmpty() && b.getFullPathName().isNotEmpty()
        && juce::File::isAbsolutePath(a.getFullPathName()) && juce::File::isAbsolutePath(b.getFullPathName())
        && a!=b && !a.isAChildOf(b) && !b.isAChildOf(a);
}
bool preferenceValid(const juce::var& record,juce::String& selected)
{
    if(record.isVoid()){selected.clear();return true;}
    const auto* object=record.getDynamicObject();
    if(!object || object->getProperties().size()!=2 || !record.hasProperty("version") || !record.hasProperty("selectedModel"))return false;
    const auto& version=record["version"];const auto& model=record["selectedModel"];
    if((!version.isInt() && !version.isInt64()) || static_cast<juce::int64>(version)!=1 || !model.isString())return false;
    const auto value=model.toString();if(value.isNotEmpty() && !slug(value))return false;
    selected=value;return true;
}
juce::var preferenceFor(const juce::String& selected)
{auto* object=new juce::DynamicObject;object->setProperty("version",1);object->setProperty("selectedModel",selected);return object;}
RuntimeStatus accountStatus(oauth::Status status)
{
    switch(status)
    {
        case oauth::Status::cancelled:return RuntimeStatus::cancelled;
        case oauth::Status::storageFailure:case oauth::Status::networkFailure:case oauth::Status::invalidResponse:return RuntimeStatus::unavailable;
        case oauth::Status::busy:return RuntimeStatus::busy;
        default:return RuntimeStatus::ready;
    }
}
diagnostics::Event jobEvent(const RuntimeJob& job,diagnostics::Stage stage)
{
    diagnostics::Event event;event.component=diagnostics::Component::companion;event.stage=stage;
    event.session=job.session;event.connectionId=job.connectionId;event.jobId=job.jobId;
    if(const auto* request=std::get_if<ai::GenerationRequest>(&job.input)){event.runtime=request->tag.runtime;event.requestId=request->tag.requestId;event.model=request->model;}
    return event;
}
Frame controlFailure(const RuntimeJob& job,RuntimeStatus status)
{
    try{auto event=jobEvent(job,diagnostics::Stage::backend);event.errorCode=status==RuntimeStatus::modelNotConfigured?"model_not_configured":"backend_invalid_job";event.runtimeStatus=int(status);diagnostics::write(event);}catch(...){}
    const auto* request=std::get_if<ai::GenerationRequest>(&job.input);
    if(job.session.isNull() || !request || request->tag.runtime.isNull() || request->tag.requestId==0)return {};
    Frame frame{MessageKind::failure,job.session,{}};
    return encodeControl({request->tag,status},frame.payload)?frame:Frame{};
}
Frame accountFrame(const RuntimeJob& job,AccountReply reply,const juce::String& providerRequest={})
{
    try{auto event=jobEvent(job,diagnostics::Stage::account);event.providerStatus=int(reply.providerStatus);event.httpStatus=reply.httpStatus;
        event.errorCode=reply.errorCode;event.model=reply.selectedModel;event.providerRequestId=providerRequest;event.runtimeStatus=int(reply.status);diagnostics::write(event);}catch(...){}
    if(job.session.isNull())return {};
    Frame frame{MessageKind::accountResult,job.session,{}};
    if(!encodeAccountReply(reply,frame.payload))
    {
        // The accepted encoder owns every list/string/total-byte limit; no truncation.
        reply.status=RuntimeStatus::unavailable;reply.providerStatus=oauth::Status::invalidResponse;
        reply.httpStatus=0;reply.errorCode="backend_response_invalid";reply.models.clear();reply.modelVerified=false;
        if(!encodeAccountReply(reply,frame.payload))return {};
    }
    return frame;
}
Frame invalidJob(const RuntimeJob& job)
{
    if(std::holds_alternative<ai::GenerationRequest>(job.input))return controlFailure(job,RuntimeStatus::invalidMessage);
    AccountReply reply;reply.status=RuntimeStatus::invalidMessage;reply.providerStatus=oauth::Status::invalidResponse;reply.errorCode="backend_invalid_job";
    return accountFrame(job,std::move(reply));
}
void retainMaterializeFields(const ai::GenerationRequest& request,ai::GenerationResult& result)
{
    if(request.catalog&&request.catalog->phase==ai::CatalogPhase::materialize){const auto& c=*request.catalog;
        result.inferenceCalls=c.inferenceCallsUsed;result.providerStatus=c.providerStatus;result.httpStatus=c.httpStatus;}
}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{auto* o=new juce::DynamicObject;for(const auto& f:fields)o->setProperty(f.first,f.second);return o;}
juce::var sampleMetadata(const ai::PreviewSampleMap* map)
{
    if(!map)return object({{"availability","unavailable"}});
    juce::Array<juce::var> banks;
    for(std::size_t bank=0;bank<4;++bank){juce::Array<juce::var> pads;
        for(std::size_t pad=0;pad<16;++pad){const auto& p=map->pads[bank*16+pad];
            const auto* asset=p.asset?&map->assets[p.asset-1]:nullptr;
            pads.add(juce::Array<juce::var>{asset?"assigned":"empty",juce::String::fromUTF8(p.name.data(),int(p.name.size())),
                asset?int(asset->channels):0,asset?int(asset->frames):0,asset?double(asset->frames)/ai::previewSampleRate:0.});}
        banks.add(pads);}
    return object({{"availability","available"},{"pad_fields",juce::Array<juce::var>{"state","displayed_name","channels","frames","duration_seconds"}},
        {"banks",banks}});
}
bool localPrompt(const RuntimeJob& job,std::string_view prompt,std::string& output)
{
    juce::var payload;if(prompt.empty()||prompt.back()!='}'||!ai::parseStrictJson(prompt,payload,oauth::maximumStructuredPromptBytes)
        ||!payload.isObject()||payload.hasProperty("sample_metadata"))return false;
    juce::Array<juce::var> peers;
    if(job.localGroupContext){
        const auto numeric=payload["linked_context"]["peers"];
        if(!numeric.isArray()||std::size_t(numeric.size())!=job.localGroupContext->peerCount)return false;
        for(std::size_t i=0;i<job.localGroupContext->peerCount;++i){const auto& peer=job.localGroupContext->peers[i];
            peers.add(sampleMetadata(peer.metadataAvailable?&peer.map:nullptr));}
    }
    const auto metadata=juce::JSON::toString(object({{"requester",sampleMetadata(&job.localPreview->sourceMap)},
        {"peer_order","linked_context.peers"},{"peers",peers}}),true,17).toStdString();
    constexpr std::string_view key=",\"sample_metadata\":";
    if(prompt.size()>oauth::maximumStructuredPromptBytes-key.size()||metadata.size()>oauth::maximumStructuredPromptBytes-prompt.size()-key.size())return false;
    // Preserve the generator's compact numeric text and every existing prompt byte.
    std::string projected;projected.reserve(prompt.size()+key.size()+metadata.size());projected.append(prompt.data(),prompt.size()-1);
    projected.append(key);projected.append(metadata);projected.push_back('}');
    std::size_t escaped=0;
    if(!rackCatalogEscapedSize(juce::var(juce::String::fromUTF8(projected.data(),int(projected.size()))),oauth::maximumStructuredRequestBytes,escaped))return false;
    output=std::move(projected);return true;
}
Frame generationFrame(const RuntimeJob& job,const ai::GenerationResult& result,std::optional<ai::PreviewReceipt> receipt={})
{
    try{auto event=jobEvent(job,diagnostics::Stage::backend);event.generationStatus=int(result.status);event.providerStatus=int(result.providerStatus);
        event.proposalStatus=int(result.proposalStatus);event.renderStatus=int(result.renderStatus);event.httpStatus=result.httpStatus;event.calls=result.inferenceCalls;event.errorCode=result.errorCode;diagnostics::write(event);}catch(...){}
    const auto& request=std::get<ai::GenerationRequest>(job.input);
    Frame frame{job.localPreview?MessageKind::resultLocal:MessageKind::result,job.session,{}};
    const auto encode=[&](const ai::GenerationResult& numeric,const std::optional<ai::PreviewReceipt>& rendered){
        if(!job.localPreview)return encodeResult(request,numeric,frame.payload);
        auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=request;input->local=*job.localPreview;
        auto local=std::make_unique<ai::LocalGenerationResult>();local->numeric=numeric;local->rendered=rendered;return encodeLocalResult(*input,*local,frame.payload);
    };
    if(!encode(result,receipt))
    {
        auto boundedStorage=std::make_unique<ai::GenerationResult>();auto& bounded=*boundedStorage;bounded.tag=request.tag;bounded.status=ai::GenerationStatus::providerFailure;
        bounded.providerStatus=oauth::Status::invalidResponse;bounded.errorCode="backend_result_invalid";
        bounded.inferenceCalls=(std::min)(result.inferenceCalls,3u);retainMaterializeFields(request,bounded);
        if(!encode(bounded,std::nullopt))return controlFailure(job,RuntimeStatus::invalidMessage);
    }
    return frame;
}
}

struct CompanionBackend::Impl
{
    struct Context
    {
        RuntimeJob job;oauth::OAuthProbe probe;std::atomic_bool loopbackCancel{false};
        Context(const RuntimeJob& value,const BackendOptions& options)
            :job(value),probe(options.credentialDirectory,options.transport,options.clock){}
        bool stopped() const noexcept{return loopbackCancel.load(std::memory_order_acquire) || job.cancellation->cancelled();}
        void signal() noexcept{loopbackCancel.store(true,std::memory_order_release);if(job.previewExchange)job.previewExchange->cancel();probe.cancel();}
        bool matches(const RuntimeJob& value) const noexcept
        {return job.jobId==value.jobId && job.connectionId==value.connectionId && job.session==value.session && job.cancellation==value.cancellation;}
    };
    BackendOptions options;
    // shortcut: retain up to1024 observed sessions until idle exit; add disconnect notifications if long-lived bulk hosts reach this limit.
    std::map<juce::Uuid,juce::String> observedAccounts;
    std::mutex lifetimeMutex;std::shared_ptr<Context> active;
    explicit Impl(BackendOptions value):options(std::move(value))
    {if(!options.openBrowser)options.openBrowser=[](const auto& url){return juce::URL(url).launchInDefaultBrowser();};}

    oauth::Status loadSelection(const std::shared_ptr<Context>& context,juce::String& selected)
    {
        if(context->stopped()){context->signal();return oauth::Status::cancelled;}
        oauth::detail::Store store(options.preferenceDirectory);
        if(!store.lock(context->loopbackCancel))return context->stopped()?oauth::Status::cancelled:oauth::Status::storageFailure;
        if(context->stopped()){context->signal();return oauth::Status::cancelled;}
        juce::var record;
        if(!store.load(record) || !preferenceValid(record,selected))return oauth::Status::storageFailure;
        return oauth::Status::ready;
    }
    oauth::Status saveSelection(const std::shared_ptr<Context>& context,const juce::String& requested,juce::String& selected)
    {
        if(context->stopped()){context->signal();return oauth::Status::cancelled;}
        oauth::detail::Store store(options.preferenceDirectory);
        if(!store.lock(context->loopbackCancel))return context->stopped()?oauth::Status::cancelled:oauth::Status::storageFailure;
        juce::var current;
        if(!store.load(current) || !preferenceValid(current,selected))return oauth::Status::storageFailure;
        if(context->stopped()){context->signal();return oauth::Status::cancelled;}
        // Native atomic save may finish after cancellation; never roll it back.
        // No lifetime/IO-cancel mutex is held across persistence.
        if(!store.save(preferenceFor(requested)))return oauth::Status::storageFailure;
        selected=requested;return oauth::Status::ready;
    }
    oauth::Result signIn(const std::shared_ptr<Context>& context,const juce::String& savedAccount={})
    {
        if(context->stopped()){context->signal();return {oauth::Status::cancelled};}
        oauth::detail::Loopback loopback;
        if(loopback.port()<1){oauth::Result bad{oauth::Status::networkFailure};bad.errorCode="loopback_unavailable";return bad;}
        if(context->stopped()){context->signal();return {oauth::Status::cancelled};}
        // Default-disabled observation of the actual reset window, outside locks.
        auto& pause=timing();
        if(pause.pause.load(std::memory_order_acquire))
        {
            pause.observed.store(true,std::memory_order_release);
            while(pause.pause.load(std::memory_order_acquire))std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto mode=std::get<AccountCommand>(context->job.input).action==AccountAction::addAccount?oauth::SignInMode::addAccount:oauth::SignInMode::existingAccount;
        const auto attempt=context->probe.beginSignIn(loopback.port(),mode,savedAccount);
        // beginSignIn resets its own flag. The independently sticky token wins.
        if(context->stopped()){context->signal();return {oauth::Status::cancelled};}
        if(attempt.result.status!=oauth::Status::awaitingCallback)return attempt.result;
        bool opened=false;
        try{opened=options.openBrowser(attempt.browserUrl);}catch(...){opened=false;}
        if(context->stopped()){context->signal();return {oauth::Status::cancelled};}
        if(!opened){oauth::Result bad{oauth::Status::networkFailure};bad.errorCode="browser_open_failed";return bad;}
        const auto target=loopback.receive(context->loopbackCancel);
        if(context->stopped()){context->signal();return {oauth::Status::cancelled};}
        if(target.isEmpty()){oauth::Result bad{oauth::Status::expiredAttempt};bad.errorCode="sign_in_timeout";return bad;}
        return context->probe.acceptCallback(target);
    }
    Frame account(const std::shared_ptr<Context>& context,juce::String selected)
    {
        const auto& command=std::get<AccountCommand>(context->job.input);
        oauth::Result provider;
        switch(command.action)
        {
            case AccountAction::status:provider=context->probe.status();break;
            case AccountAction::models:case AccountAction::selectModel:provider=context->probe.listModels();break;
            case AccountAction::signIn:provider=signIn(context);break;
            case AccountAction::addAccount:provider=signIn(context);break;
            case AccountAction::selectAccount:
                provider=context->probe.selectAccount(command.model);
                if(provider.status==oauth::Status::signedOut||provider.status==oauth::Status::accessExpired||provider.status==oauth::Status::inferenceNotGranted)provider=signIn(context,command.model);
                break;
        }
        AccountReply reply;reply.selectedModel=selected;reply.status=accountStatus(provider.status);
        reply.providerStatus=provider.status;reply.httpStatus=provider.httpStatus;reply.errorCode=provider.errorCode;reply.models=std::move(provider.models);
        const auto accountInfo=context->probe.savedAccounts(reply.accounts,reply.activeAccount);
        if(accountInfo.status==oauth::Status::storageFailure||accountInfo.status==oauth::Status::busy){reply.status=accountStatus(accountInfo.status);reply.providerStatus=accountInfo.status;reply.errorCode="account_storage_unavailable";}
        if(context->stopped())return cancelledAccount(context,selected);
        if(command.action==AccountAction::models || command.action==AccountAction::selectModel)
        {
            juce::MemoryBlock represented;
            if(!encodeAccountReply(reply,represented))
            {
                reply.status=RuntimeStatus::unavailable;reply.providerStatus=oauth::Status::invalidResponse;
                reply.httpStatus=0;reply.errorCode="model_list_limit";reply.models.clear();
            }
            else if(command.action==AccountAction::selectModel && provider.status==oauth::Status::connected)
            {
                const auto authorized=std::any_of(reply.models.begin(),reply.models.end(),[&](const auto& model){return model.slug==command.model;});
                if(!authorized){reply.status=RuntimeStatus::modelNotConfigured;reply.errorCode="model_not_authorized";}
                else
                {
                    const auto saved=saveSelection(context,command.model,selected);reply.selectedModel=selected;
                    if(saved!=oauth::Status::ready){reply.providerStatus=saved;reply.status=accountStatus(saved);reply.errorCode=saved==oauth::Status::cancelled?juce::String():juce::String("preference_storage_failed");}
                }
            }
        }
        if(context->stopped())return cancelledAccount(context,selected);
        if(!observedAccounts.contains(context->job.session)&&observedAccounts.size()>=1024){reply.status=RuntimeStatus::busy;reply.providerStatus=oauth::Status::busy;reply.errorCode="account_connection_limit";}
        else observedAccounts[context->job.session]=reply.activeAccount;
        return accountFrame(context->job,std::move(reply),provider.requestId);
    }
    Frame cancelledAccount(const std::shared_ptr<Context>& context,const juce::String& selected)
    {
        context->signal();AccountReply reply;reply.status=RuntimeStatus::cancelled;reply.providerStatus=oauth::Status::cancelled;reply.selectedModel=selected;
        return accountFrame(context->job,std::move(reply));
    }
    Frame failure(const std::shared_ptr<Context>& context,oauth::Status status,const juce::String& code,juce::String selected={})
    {
        if(std::holds_alternative<AccountCommand>(context->job.input))
        {
            if(context->stopped() || status==oauth::Status::cancelled)return cancelledAccount(context,selected);
            AccountReply reply;reply.status=accountStatus(status);reply.providerStatus=status;reply.errorCode=code;reply.selectedModel=std::move(selected);
            return accountFrame(context->job,std::move(reply));
        }
        const auto& request=std::get<ai::GenerationRequest>(context->job.input);
        auto resultStorage=std::make_unique<ai::GenerationResult>();auto& result=*resultStorage;result.tag=request.tag;result.status=ai::GenerationStatus::providerFailure;result.providerStatus=status;result.errorCode=code;
        if(context->stopped() || status==oauth::Status::cancelled){context->signal();result.status=ai::GenerationStatus::cancelled;result.providerStatus=oauth::Status::cancelled;result.errorCode.clear();}
        retainMaterializeFields(request,result);return generationFrame(context->job,result);
    }
    Frame generate(const std::shared_ptr<Context>& context,const juce::String& selected)
    {
        const auto& request=std::get<ai::GenerationRequest>(context->job.input);
        if(selected.isEmpty() || selected!=request.model)return controlFailure(context->job,RuntimeStatus::modelNotConfigured);
        const auto observed=observedAccounts.find(context->job.session);
        if(observed!=observedAccounts.end()){
            std::vector<oauth::SavedAccount> accounts;juce::String activeAccount;const auto info=context->probe.savedAccounts(accounts,activeAccount);
            if(info.status==oauth::Status::storageFailure||info.status==oauth::Status::busy||info.status==oauth::Status::cancelled)return failure(context,info.status,"account_storage_unavailable");
            if(activeAccount!=observed->second)return failure(context,oauth::Status::identityMismatch,"account_changed");
        }else if(observedAccounts.size()>=1024)return failure(context,oauth::Status::busy,"account_connection_limit");
        unsigned providerAttempts=request.catalog&&request.catalog->phase==ai::CatalogPhase::materialize?request.catalog->inferenceCallsUsed:0;
        std::uint8_t renderOrdinal=0;
        const ai::StructuredCall provider=[context,&providerAttempts](const auto& model,std::string_view prompt,const juce::var& schema)
        {
            ++providerAttempts;
            if(context->stopped()){context->signal();return oauth::StructuredResult{{oauth::Status::cancelled},{}};}
            std::string projected;
            if(context->job.localPreview){if(!localPrompt(context->job,prompt,projected)){
                oauth::StructuredResult rejected;rejected.result.status=oauth::Status::invalidResponse;rejected.result.errorCode="structured_request_limit";return rejected;}
                prompt=projected;}
            const auto began=std::chrono::steady_clock::now();auto response=context->probe.requestStructured(model,prompt,schema);
            try{auto event=jobEvent(context->job,diagnostics::Stage::provider);event.providerStatus=int(response.result.status);event.httpStatus=response.result.httpStatus;
                event.errorCode=response.result.errorCode;event.providerRequestId=response.result.requestId;event.outputBytes=response.text.getNumBytesAsUTF8();
                event.elapsedMs=std::uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count());diagnostics::write(event);}catch(...){}
            return response;
        };
        const ai::CancelCheck cancelled=[context]{return context->stopped();};
        std::unique_ptr<ai::GenerationResult> result;std::optional<ai::PreviewReceipt> receipt;
        if(context->job.localPreview){
            auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=request;input->local=*context->job.localPreview;
            const ai::LocalPreviewEvaluator evaluate=[context,&providerAttempts,&renderOrdinal](const auto& candidate,const auto& edits,const auto& audition,const auto& trace,const auto& stop){
                const auto& request=std::get<ai::GenerationRequest>(context->job.input);const auto& local=*context->job.localPreview;
                auto value=std::make_unique<ai::PreviewEvaluate>();auto& binding=value->binding;
                binding.request=request.tag;binding.root=local.root;binding.catalogSnapshot=local.catalogSnapshot;binding.job=context->job.jobId;
                binding.structuralGeneration=request.structuralGeneration;binding.reset=local.reset;binding.sourceWord=local.sourceWord;
                binding.phase=request.catalog?request.catalog->phase:ai::CatalogPhase::select;binding.providerAttempt=std::uint8_t(providerAttempts);binding.renderOrdinal=++renderOrdinal;
                binding.sourceMap=local.sourceMapDigest;value->edits=edits;value->context=audition;
                ai::PreviewSampleMap staged;
                if(!ai::stagePreviewMap(local.sourceMap,trace,staged)||!ai::digestPreviewMap(staged,binding.stagedMap)
                    ||!ai::digestPreviewCandidate(candidate,binding.candidate)||!ai::digestPreviewContext(audition,binding.context))return ai::PreviewReceipt{};
                return context->job.previewExchange->evaluate(*value,stop);
            };
            auto generated=std::unique_ptr<ai::LocalGenerationResult>(new ai::LocalGenerationResult(ai::generateLocalProposal(*input,provider,evaluate,cancelled,context->job.groupContext.get())));
            result=std::make_unique<ai::GenerationResult>(std::move(generated->numeric));receipt=std::move(generated->rendered);
        }else result.reset(new ai::GenerationResult(ai::generateProposal(request,provider,cancelled,context->job.groupContext.get())));
        if(context->stopped())
        {
            context->signal();const auto calls=result->inferenceCalls;result=std::make_unique<ai::GenerationResult>();result->tag=request.tag;
            result->inferenceCalls=calls;result->status=ai::GenerationStatus::cancelled;result->providerStatus=oauth::Status::cancelled;receipt.reset();retainMaterializeFields(request,*result);
        }
        return generationFrame(context->job,*result,std::move(receipt));
    }
};

CompanionBackend::CompanionBackend(BackendOptions options):impl(std::make_unique<Impl>(std::move(options))){}
CompanionBackend::~CompanionBackend() = default;
Frame CompanionBackend::run(const RuntimeJob& job)
{
    if(job.jobId==0 || job.connectionId==0 || job.session.isNull() || !job.cancellation)return invalidJob(job);
    if(job.groupContext && (std::holds_alternative<AccountCommand>(job.input)
        || !validateGroupPerformanceCapture(*job.groupContext)))return invalidJob(job);
    juce::MemoryBlock validated;
    if(const auto* request=std::get_if<ai::GenerationRequest>(&job.input))
    {
        if(bool(job.localPreview)!=bool(job.previewExchange))return invalidJob(job);
        if(job.localPreview){auto local=std::make_unique<ai::LocalGenerationRequest>();local->request=*request;local->local=*job.localPreview;
            if(job.localDeadline==RuntimeTime{}||!encodeLocalRequest(*local,validated))return invalidJob(job);
        }else if(!encodeRequest(*request,validated))return invalidJob(job);
        if(job.localGroupContext){
            if(!job.localPreview||!job.groupContext||job.localGroupContext->peerCount!=job.groupContext->peerCount)return invalidJob(job);
            for(std::size_t i=0;i<job.localGroupContext->peerCount;++i){const auto& compact=job.localGroupContext->peers[i];const auto& peer=job.groupContext->ownership.peers[i];
                if(compact.runtime!=peer.runtime||compact.loadRevision!=peer.loadRevision||compact.editRevision!=peer.editRevision)return invalidJob(job);
                ai::PreviewDigest digest;
                if(compact.metadataAvailable&&(!ai::digestPreviewMap(compact.map,digest)||digest!=compact.mapDigest))return invalidJob(job);
            }
        }else if(job.localPreview&&job.groupContext)return invalidJob(job);
    }
    else if(job.localPreview||job.previewExchange||job.localGroupContext||!encodeAccountCommand(std::get<AccountCommand>(job.input),validated))return invalidJob(job);
    auto context=std::make_shared<Impl::Context>(job,impl->options);
    {std::lock_guard lock(impl->lifetimeMutex);if(impl->active)return invalidJob(job);impl->active=context;}
    struct Remove
    {
        Impl& impl;std::shared_ptr<Impl::Context> context;
        ~Remove(){std::lock_guard lock(impl.lifetimeMutex);if(impl.active==context)impl.active.reset();}
    }remove{*impl,context};
    try
    {
        if(context->stopped())return impl->failure(context,oauth::Status::cancelled,{});
        if(!directoriesValid(impl->options))return impl->failure(context,oauth::Status::storageFailure,"backend_directory_invalid");
        juce::String selected;const auto loaded=impl->loadSelection(context,selected);
        if(loaded!=oauth::Status::ready)return impl->failure(context,loaded,loaded==oauth::Status::cancelled?juce::String():juce::String("preference_storage_failed"));
        if(context->stopped())return impl->failure(context,oauth::Status::cancelled,{},selected);
        return std::holds_alternative<AccountCommand>(job.input)?impl->account(context,std::move(selected)):impl->generate(context,selected);
    }
    catch(...){return impl->failure(context,oauth::Status::networkFailure,"backend_unavailable");}
}
void CompanionBackend::cancel(const RuntimeJob& job) noexcept
{
    std::shared_ptr<Impl::Context> context;
    try
    {
        {std::lock_guard lock(impl->lifetimeMutex);if(impl->active && impl->active->matches(job))context=impl->active;}
        if(context)context->signal();
    }
    catch(...){} // State's sticky token still prevents obsolete publication.
}
void detail::CompanionBackendTestAccess::pauseBeforeSignIn(bool pause) noexcept
{auto& value=timing();if(pause)value.observed.store(false,std::memory_order_release);value.pause.store(pause,std::memory_order_release);}
bool detail::CompanionBackendTestAccess::signInPaused() noexcept
{return timing().observed.load(std::memory_order_acquire);}
}

#include "InstrumentProcessor.h"
#include "InstrumentMcp.h"
#include "LocalPreview.h"
#include <Windows.h>
#include <sddl.h>
#include <thread>
#include <cmath>
#include <chrono>

namespace airack {
namespace {
using mcp::ipc::object;
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& h) noexcept:value(std::exchange(h.value,INVALID_HANDLE_VALUE)){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE&&value)CloseHandle(value);}
};
std::wstring nativePath(const juce::File& file){auto path=file.getFullPathName().replaceCharacter('/','\\');if(path.length()==2&&path[1]==':')path+=juce::String::charToString('\\');return L"\\\\?\\"+std::wstring(path.toWideCharPointer());}
constexpr std::uint64_t maximumWavBytes=44+ai::maximumPreviewFrames*8;
const char* renderName(ai::RenderStatus status){constexpr const char* names[]={"ready","invalid_input","cancelled","non_finite","clipped","render_failure"};return names[unsigned(status)];}
juce::var measurements(const ai::AudioMeasurements& m){return object({{"finite",m.finite},{"clipped_samples",mcp::ipc::decimal(m.clippedSamples)},{"peak",std::isfinite(m.peak)?juce::var(m.peak):juce::var()},
    {"rms",std::isfinite(m.rms)?juce::var(m.rms):juce::var()},{"onset_seconds",m.onsetSeconds?juce::var(*m.onsetSeconds):juce::var()},
    {"activity_end_seconds",m.activityEndSeconds},{"decay_seconds",m.decaySeconds},{"band_fractions",juce::Array<juce::var>{m.bandFractions[0],m.bandFractions[1],m.bandFractions[2]}},{"tail_active_at_end",m.tailActiveAtEnd}});}
// Pin each plain directory against rename/reparse substitution until its file retires.
bool openDirectory(const juce::File& directory,std::vector<Handle>& pins){
    const auto parent=directory.getParentDirectory();if(parent!=directory&&!openDirectory(parent,pins))return false;
    auto h=CreateFileW(nativePath(directory).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;Handle pin(h);BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h,&info)||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    pins.push_back(std::move(pin));return true;
}
bool makePrivateDirectory(const juce::File& directory){
    Handle token;token.value=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return false;
    DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);if(!size||size>65536)return false;
    std::vector<std::byte> bytes(size);if(!GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size))return false;
    LPWSTR sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid))return false;
    const auto sddl=juce::String("D:P(A;OICI;FA;;;")+juce::String(sid)+")";LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.toWideCharPointer(),SDDL_REVISION_1,&descriptor,nullptr))return false;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
    const auto made=CreateDirectoryW(nativePath(directory).c_str(),&attributes);const auto error=GetLastError();LocalFree(descriptor);
    return made||error==ERROR_ALREADY_EXISTS;
}
bool sameFile(HANDLE file,const juce::File& path,std::uint64_t expected){
    BY_HANDLE_FILE_INFORMATION original{},current{};Handle opened(CreateFileW(nativePath(path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    return opened.value!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(file,&original)&&GetFileInformationByHandle(opened.value,&current)
        &&!(current.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&original.dwVolumeSerialNumber==current.dwVolumeSerialNumber
        &&original.nFileIndexHigh==current.nFileIndexHigh&&original.nFileIndexLow==current.nFileIndexLow
        &&current.nFileSizeHigh==0&&current.nFileSizeLow==expected&&expected<=maximumWavBytes;
}
}
struct McpState::Preview final {
    juce::Uuid id=mcp::newRuntimeId(),resource=mcp::newRuntimeId();mcp::Caller caller;
    Binding binding;juce::Uuid edit=juce::Uuid::null();std::uint64_t serial=0,touched=0,charge=0;
    std::unique_ptr<ProjectDocument> candidate;std::shared_ptr<const drums::DrumOwnership::McpImage> image;
    std::array<drums::AssetPtr,64> owners;BorrowedDrumAssignments borrowed;ai::AuditionContext context;
    std::atomic_bool cancel{false},done{false};std::thread worker;bool retired=false;juce::String blob;
    ai::RenderResult result;juce::String uri;juce::File path;std::vector<Handle> directoryPins;Handle file;std::uint64_t fileBytes=0;
    ~Preview(){cancel=true;if(worker.joinable())worker.join();}
};
McpState::McpState()=default;
McpState::~McpState()=default;
void InstrumentProcessor::retireMcpPreviewOwned(bool force) noexcept {
    if(!mcpState||!mcpState->preview)return;auto& job=*mcpState->preview;job.retired=true;job.cancel=true;
    if(previewOwner==PreviewOwner::external)renderedPreview.stop();
    if(!force&&(!job.done.load(std::memory_order_acquire)||mcpState->preview.use_count()!=1))return;
    const auto bytes=job.charge;mcpState->preview.reset();drumOwnership->releaseDecoded(bytes);drumOwnership->collect();
}
void InstrumentProcessor::pollMcpPreviewOwned(){
    if(previewOwner==PreviewOwner::external){
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(!prepared||isSuspended()||now<0||std::uint64_t(now)>=mcpAudioValidUntil.load(std::memory_order_acquire))
            renderedPreview.retireStoppedWithoutAudio();
    }
    if(previewOwner==PreviewOwner::external&&renderedPreview.stopped()&&renderedPreview.clearStopped()){releaseMcpPreviewChargeOwned();previewOwner=PreviewOwner::none;}
    if(!mcpState||!mcpState->preview)return;auto& job=*mcpState->preview;
    if(job.retired||job.caller.epoch!=mcpState->endpoint.metadata().epoch||GetTickCount64()-job.touched>=300000||!mcpBindingCurrentOwned(job.binding)||(!job.edit.isNull()&&(!mcpState->stage||mcpState->stage->id!=job.edit)))retireMcpPreviewOwned();
}
void InstrumentProcessor::releaseMcpPreviewChargeOwned() noexcept {if(mcpPreviewBytes){drumOwnership->releaseDecoded(mcpPreviewBytes);mcpPreviewBytes=0;}}
bool InstrumentProcessor::mcpPreviewOwnedByOther(const mcp::Command& command) const {
    if(!mcpState->preview||mcpState->preview->retired)return false;const auto& job=*mcpState->preview;
    return job.caller.owner!=command.caller.owner||job.caller.epoch!=command.caller.epoch;
}
void InstrumentProcessor::controlMcpPreviewOwned(const mcp::Command& command){
    if(!mcpState->preview)return;const auto& job=*mcpState->preview;
    if(command.control==mcp::CommandControl::epochLost||command.control==mcp::CommandControl::revoke
        ||(job.caller.owner==command.caller.owner&&job.caller.epoch==command.caller.epoch
            &&(command.control==mcp::CommandControl::ownerDisconnected||(command.control==mcp::CommandControl::cancel
                &&(command.cancelledSerial==job.serial||command.call.arguments["job_id"].toString()==job.id.toDashedString()||command.call.arguments["uri"].toString()==job.uri)))))retireMcpPreviewOwned();
}
mcp::ToolResult InstrumentProcessor::executeMcpPreview(const mcp::Command& command){
    using mcp::Status;using mcp::Tool;const auto& args=command.call.arguments;
    const std::lock_guard lock(restoreMutex);
    const auto previewEntryExpiredTick=GetTickCount64();if(mcp::commandExpired(command,previewEntryExpiredTick)){mcp::detail::EndpointTestAccess::recordDiagnostic(mcpState->endpoint,mcp::detail::RequestStage::previewEntryExpired,command,int(Status::hostControlUnavailable),previewEntryExpiredTick);return {Status::hostControlUnavailable,{}};}
    pollMcpPreviewOwned();
    const auto previewPollExpiredTick=GetTickCount64();if(mcp::commandExpired(command,previewPollExpiredTick)){mcp::detail::EndpointTestAccess::recordDiagnostic(mcpState->endpoint,mcp::detail::RequestStage::previewPollExpired,command,int(Status::hostControlUnavailable),previewPollExpiredTick);return {Status::hostControlUnavailable,{}};}
    if(command.call.tool==Tool::resourceList){
        if(!mcp::ipc::fields(args,{"instance_id"}))return {Status::invalidInput,{}};
        juce::Array<juce::var> resources;const auto* job=mcpState->preview.get();
        if(job&&!job->retired&&job->caller.owner==command.caller.owner&&job->caller.epoch==command.caller.epoch&&job->done.load(std::memory_order_acquire)
            &&job->result.status==ai::RenderStatus::ready&&sameFile(job->file.value,job->path,job->fileBytes))
            resources.add(object({{"uri",job->uri},{"name","AIR rendered preview"},{"mimeType","audio/wav"},{"size",int(job->fileBytes)}}));
        return {Status::ready,object({{"resources",resources}})};
    }
    if(command.call.tool==Tool::resourceRead){
        juce::Uuid instance,epoch,resource;if(!mcp::ipc::fields(args,{"instance_id","uri"})||!mcp::previewResourceTarget(args["uri"],instance,epoch,resource))return {Status::invalidInput,{}};
        auto* job=mcpState->preview.get();if(instance!=command.call.instance||epoch!=command.caller.epoch||!job||job->retired||job->resource!=resource)return {Status::stale,{}};
        if(mcpPreviewOwnedByOther(command))return {Status::unauthorized,{}};
        if(!job->done.load(std::memory_order_acquire))return {Status::busy,{}};
        if(job->result.status!=ai::RenderStatus::ready||!sameFile(job->file.value,job->path,job->fileBytes))return {Status::failed,{}};
        if(job->blob.isEmpty()){
            const auto charge=job->fileBytes+3*((job->fileBytes+2)/3*4)+65536;
            if(!drumOwnership->reserveDecoded(charge))return {Status::capacity,{}};
            try{
                juce::MemoryBlock bytes(std::size_t(job->fileBytes));LARGE_INTEGER zero{};DWORD read=0;
                if(!SetFilePointerEx(job->file.value,zero,nullptr,FILE_BEGIN)||!ReadFile(job->file.value,bytes.getData(),DWORD(bytes.getSize()),&read,nullptr)||read!=bytes.getSize()){
                    bytes.reset();drumOwnership->releaseDecoded(charge);return {Status::failed,{}};}
                job->blob=juce::Base64::toBase64(bytes.getData(),bytes.getSize());job->charge+=charge;
            }catch(...){job->blob.clear();drumOwnership->releaseDecoded(charge);throw;}
        }
        if(mcp::commandExpired(command,GetTickCount64()))return {Status::hostControlUnavailable,{}};
        job->touched=GetTickCount64();if(mcpState->stage&&mcpState->stage->id==job->edit)mcpState->stage->touched=job->touched;
        const auto content=object({{"uri",job->uri},{"mimeType","audio/wav"},{"blob",job->blob}});
        return {Status::ready,object({{"contents",juce::Array<juce::var>{content}}}),mcpState->preview,std::shared_ptr<const std::atomic_bool>(mcpState->preview,&job->cancel)};
    }
    if(command.call.tool==Tool::playPreview||command.call.tool==Tool::stopPreview||command.call.tool==Tool::cancelJob){
        if(!mcp::ipc::fields(args,{"instance_id","job_id"})||!args["job_id"].isString())return {Status::invalidInput,{}};
        auto* job=mcpState->preview.get();if(!job||job->retired||args["job_id"].toString()!=job->id.toDashedString())return {Status::stale,{}};
        if(mcpPreviewOwnedByOther(command))return {Status::unauthorized,{}};
        if(command.call.tool==Tool::cancelJob){const auto response=object({{"job_status","cancelled"}});if(mcp::commandExpired(command,GetTickCount64()))return {Status::hostControlUnavailable,{}};retireMcpPreviewOwned();return {Status::ready,response};}
        if(command.call.tool==Tool::stopPreview){const auto response=object({{"stopped",renderedPreview.stopped()}});if(mcp::commandExpired(command,GetTickCount64()))return {Status::hostControlUnavailable,{}};job->touched=GetTickCount64();if(previewOwner==PreviewOwner::external)renderedPreview.stop();return {Status::ready,response};}
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if(!prepared||isSuspended()||now<0||std::uint64_t(now)>=mcpAudioValidUntil.load(std::memory_order_acquire))return {Status::audioUnavailable,{}};
        if(!job->done.load(std::memory_order_acquire))return {Status::busy,{}};
        if(job->result.status!=ai::RenderStatus::ready)return {Status::failed,object({{"render_status",renderName(job->result.status)}})};
        if(!renderedPreview.stopped())return {Status::busy,{}};
        if(aiApplying||aiPhase==AiProposalPhase::pending||aiPhase==AiProposalPhase::proposed||aiRefinementBackup||aiDelayedApply||groupControl->waiting||gesture->index!=parameterCount)return {Status::busy,{}};
        const auto response=object({{"playing",true}});
        const auto bytes=std::uint64_t(job->result.audio.left.size()+job->result.audio.right.size())*sizeof(float)+256;
        if(mcp::commandExpired(command,GetTickCount64()))return {Status::hostControlUnavailable,{}};
        job->touched=GetTickCount64();
        if(!drumOwnership->reserveDecoded(bytes))return {Status::capacity,{}};
        clearStoppedPreviewOwned();
        if(!renderedPreview.start(job->result.audio,job->binding.edit)){drumOwnership->releaseDecoded(bytes);return {Status::audioUnavailable,{}};}
        previewOwner=PreviewOwner::external;mcpPreviewBytes=bytes;return {Status::ready,response};
    }
    if(command.call.tool==Tool::getJob){
        if(!mcp::ipc::fields(args,{"instance_id","job_id"})||!args["job_id"].isString())return {Status::invalidInput,{}};
        const auto* job=mcpState->preview.get();if(!job||job->retired||args["job_id"].toString()!=job->id.toDashedString())return {Status::stale,{}};
        if(job->caller.owner!=command.caller.owner||job->caller.epoch!=command.caller.epoch)return {Status::unauthorized,{}};
        mcpState->preview->touched=GetTickCount64();
        if(!job->done.load(std::memory_order_acquire))return {Status::ready,object({{"job_id",job->id.toDashedString()},{"job_status","running"}})};
        const auto& result=job->result;return {Status::ready,object({{"job_id",job->id.toDashedString()},{"job_status",result.status==ai::RenderStatus::ready?"ready":result.status==ai::RenderStatus::cancelled?"cancelled":"failed"},
            {"render_status",renderName(result.status)},{"sample_rate",48000},{"channels",2},{"frames",int(result.audio.left.size())},{"body_frames",int(result.audio.bodyFrames)},{"phrase_truncated",result.audio.phraseTruncated},
            {"measurements",measurements(result.measurements)},{"resource_uri",result.status==ai::RenderStatus::ready?juce::var(job->uri):juce::var()}})};
    }
    if(command.call.tool!=Tool::renderPreview)return {Status::unavailable,{}};
    if(!mcp::ipc::fields(args,{"instance_id","snapshot","edit_id"})||args.hasProperty("snapshot")==args.hasProperty("edit_id"))return {Status::invalidInput,{}};
    if(aiApplying||aiPhase==AiProposalPhase::pending||aiPhase==AiProposalPhase::proposed||aiRefinementBackup||aiDelayedApply||groupControl->waiting||gesture->index!=parameterCount)return {Status::busy,{}};
    if(mcpState->preview)return {Status::busy,{}};
    const auto stage=mcpState->stage;
    if(stage&&(stage->caller.owner!=command.caller.owner||stage->caller.epoch!=command.caller.epoch))return {Status::busy,{}};
    McpState::Binding binding;std::shared_ptr<const drums::DrumOwnership::McpImage> image;
    const bool staged=args.hasProperty("edit_id");
    if(staged){if(!stage||args["edit_id"].toString()!=stage->id.toDashedString())return {Status::stale,{}};binding=stage->binding;image=stage->image;}
    else{binding=mcpState->snapshot;if(args["snapshot"].toString()!=binding.token.toDashedString())return {Status::stale,{}};}
    // Reserve every capture, staging scratch, native renderer workspace and output before allocation.
    const auto candidateBytes=staged?RackCatalogCaptureLimits{}.maximumPreparedBytes:0;
    // Catalog capture owns up to its reviewed 40MiB; simultaneous placement/reconciliation
    // copies <=12 definitions (precharge <=72001 each) plus serial scratch need a second cohort.
    const auto scratchBytes=2*candidateBytes;
    const auto charge=candidateBytes+sizeof(McpState::Preview)+2*sizeof(ProjectDocument)+2*sizeof(AssignmentStage)+sizeof(SourceAssignmentTrace)+2*sizeof(ai::PreviewSampleMap)+sizeof(ai::GenerationRequest)
        +2*65536+2*512*sizeof(float)+maximumPatternEvents*3+sizeof(PatternScheduler)+sizeof(PatternEventBuffer)+65536+ai::maximumPreviewFrames*8;
    std::uint64_t reserved=charge+scratchBytes;
    if(!drumOwnership->reserveDecoded(reserved))return {Status::capacity,{}};
    std::shared_ptr<McpState::Preview> next;juce::var storageError;
    try{
        next=std::make_shared<McpState::Preview>();next->charge=charge;next->caller=command.caller;next->serial=command.serial;next->touched=GetTickCount64();next->binding=binding;
        const auto prepared=[&]() -> Status {
        auto source=std::make_unique<ProjectDocument>();if(!mcpBindingCurrentOwned(binding,source.get())){return Status::stale;}
        if(!image)image=drumOwnership->captureMcpImage();if(!image){return Status::capacity;}
        next->image=image;next->candidate=std::make_unique<ProjectDocument>();
        next->owners=image->owners;
        auto slots=image->document->slots();
        if(staged){auto trace=std::make_unique<SourceAssignmentTrace>();
            if(!binding.inventory||stage->input.inventory!=binding.inventory||!stage->input.authority
                ||stage->input.authority->snapshot!=binding.inventory->id())return Status::stale;
            if(ai::admitProposal(*source,stage->input.policy,stage->input.edits,*stage->input.authority,*binding.inventory,*next->candidate,trace.get())!=ai::ProposalStatus::accepted){return Status::invalidInput;}
            if(stage->input.reviewedName.isNotEmpty())next->candidate->sound.name=stage->input.reviewedName;
            if(!stage->candidate||*next->candidate!=*stage->candidate)return Status::stale;
            auto assignments=std::make_unique<AssignmentStage>();DrumCommitError error;
            if(!validateFreshSourceOwnerAdmissionOwned(source->sound,next->candidate->sound,trace.get(),error)||!buildAssignmentStageOwned(source->sound,*trace,image->document->slots(),image->owners,*assignments,error)){return Status::invalidInput;}
            next->owners=assignments->owners;slots=assignments->slots;next->edit=stage->id;
        }else *next->candidate=*source;
        for(std::size_t n=0;n<64;++n)next->borrowed[n/16][n%16]=next->owners[n].get();
        auto request=std::make_unique<ai::GenerationRequest>();request->source=*source;request->capturedBpm=aiContext.auditionBpm?aiContext.auditionBpm:currentHostBpm();
        auto map=std::make_unique<ai::PreviewSampleMap>();drums::DrumDocumentPtr document;drums::DrumError error;
        // The admitted assignment trace, not numeric bank position, supplies actual owned samples.
        if(!drums::makeDrumDocument(slots,document,error)||!buildAiPreviewMapOwned(next->candidate->sound,*document,next->owners,image->generation,*map)){return Status::invalidInput;}
        next->context=ai::previewContextFor(*request,staged?stage->input.edits.pattern:std::optional<PatternDocument>{},map.get(),&next->candidate->sound);
        auto root=mcpState->options->fixtureRoot;if(root.getFullPathName().isEmpty()){
            const auto local=mcp::ipc::environment("LOCALAPPDATA");if(!local||!mcp::ipc::safeRoot(*local)){return Status::unavailable;}root=juce::File(*local).getChildFile("AI Rack Synth");}
        if(!mcp::ipc::safeRoot(root.getFullPathName())){return Status::unavailable;}
        if(!openDirectory(root.getParentDirectory(),next->directoryPins)||!makePrivateDirectory(root)||!openDirectory(root,next->directoryPins)){const auto error=GetLastError();storageError=object({{"storage_step","root"},{"win32_error",int(error)}});return Status::failed;}
        for(const auto& part:{juce::String("MCP Previews"),binding.epoch.toDashedString(),command.call.instance.toDashedString()}){root=root.getChildFile(part);if(!makePrivateDirectory(root)||!openDirectory(root,next->directoryPins)){const auto error=GetLastError();storageError=object({{"storage_step","child"},{"win32_error",int(error)}});return Status::failed;}}
        if(!openDirectory(root,next->directoryPins)){const auto error=GetLastError();storageError=object({{"storage_step","pin"},{"win32_error",int(error)}});return Status::failed;}
        next->path=root.getChildFile(next->resource.toDashedString()+".wav");next->file.value=CreateFileW(nativePath(next->path).c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(next->file.value==INVALID_HANDLE_VALUE){const auto error=GetLastError();storageError=object({{"storage_step","file"},{"win32_error",int(error)}});return Status::failed;}
        next->uri="air-preview://"+command.call.instance.toDashedString()+"/"+binding.epoch.toDashedString()+"/"+next->resource.toDashedString();
        const auto previewPreparedExpiredTick=GetTickCount64();if(mcp::commandExpired(command,previewPreparedExpiredTick)){mcp::detail::EndpointTestAccess::recordDiagnostic(mcpState->endpoint,mcp::detail::RequestStage::previewPreparedExpired,command,int(Status::hostControlUnavailable),previewPreparedExpiredTick);return Status::hostControlUnavailable;}
        if(!mcpBindingCurrentOwned(binding))return Status::stale;
            return Status::ready;
        }();
        if(prepared!=Status::ready){next.reset();drumOwnership->releaseDecoded(std::exchange(reserved,0));return {prepared,storageError};}
        // Preparation locals are gone before their reservation is released on control.
        drumOwnership->releaseDecoded(scratchBytes);reserved-=scratchBytes;
        const auto response=object({{"job_id",next->id.toDashedString()},{"job_status","running"}});
        auto* job=next.get();next->worker=std::thread([job]{
            try{
                job->result=ai::renderPreview(*job->candidate,job->context,job->borrowed,[&]{return job->cancel.load(std::memory_order_acquire);});
                if(job->result.status==ai::RenderStatus::ready&&!job->cancel){
                    const auto frames=job->result.audio.left.size();const auto bytes=std::uint32_t(frames*8);
                    std::array<std::uint32_t,11> header{0x46464952,bytes+36,0x45564157,0x20746d66,16,0x00020003,48000,384000,0x00200008,0x61746164,bytes};DWORD written=0;
                    bool ok=frames&&frames<=ai::maximumPreviewFrames&&job->result.audio.right.size()==frames&&WriteFile(job->file.value,header.data(),44,&written,nullptr)&&written==44;
                    std::array<float,1024> block{};
                    for(std::size_t n=0;ok&&n<frames&&!job->cancel;n+=512){const auto count=(std::min)(std::size_t(512),frames-n);for(std::size_t k=0;k<count;++k){block[k*2]=job->result.audio.left[n+k];block[k*2+1]=job->result.audio.right[n+k];}ok=WriteFile(job->file.value,block.data(),DWORD(count*8),&written,nullptr)&&written==count*8;}
                    if(ok&&!job->cancel){job->fileBytes=44+bytes;if(!sameFile(job->file.value,job->path,job->fileBytes))ok=false;}
                    if(!ok)job->result.status=ai::RenderStatus::renderFailure;
                }
                if(job->cancel)job->result.status=ai::RenderStatus::cancelled;
            }catch(...){job->result.status=ai::RenderStatus::renderFailure;}
            job->done.store(true,std::memory_order_release);
        });
        mcpState->preview=std::move(next);reserved=0;return {Status::ready,response};
    }catch(...){next.reset();drumOwnership->releaseDecoded(std::exchange(reserved,0));throw;}
}
}

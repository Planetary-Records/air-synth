#include <new>
#include "McpEndpoint.h"
#include "CompanionLaunch.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <deque>
#include <mutex>
#include <thread>
#include <map>
namespace airack::mcp {
namespace {
using namespace companion;
juce::var metadataValue(const EndpointMetadata& m){return ipc::object({{"instance_id",m.instance.toDashedString()},{"epoch",m.epoch.toDashedString()},
    {"label",m.label},{"version",m.version},{"enabled",m.enabled},{"capabilities",m.capabilities.getDynamicObject()?m.capabilities:ipc::object({})}});}
Frame frame(MessageKind kind,const juce::Uuid& session,const juce::var& value){const auto text=encodeJson(value);return {kind,session,juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8())};}
bool valid(const EndpointMetadata& m){return !m.instance.isNull()&&!m.epoch.isNull()&&m.label.getNumBytesAsUTF8()<=256&&m.version.getNumBytesAsUTF8()<=256
    &&(!m.capabilities.getDynamicObject()||juce::JSON::toString(m.capabilities,true).getNumBytesAsUTF8()<=8192);}
}
struct Endpoint::Impl {
    std::mutex mutex;std::atomic_bool stop{true},online{false};std::thread worker;EndpointMetadata meta;ServiceOptions options;
    EndpointObservation observation;
    std::atomic_bool* testAtReply=nullptr;const std::atomic_bool* testHandoffHeld=nullptr;
    std::deque<Command> commands,priority;std::deque<std::pair<Reply,std::size_t>> replies;std::size_t inputBytes=0,outputBytes=0,activeReplyBytes=0,dispatchReserved=0;std::uint64_t dispatchSerial=0;Caller dispatchCaller;ToolCall dispatchCall;bool changed=false;std::shared_ptr<const void> activeResource;std::shared_ptr<const std::atomic_bool> activeRevoked;
    std::deque<Command> urgentCommands;std::deque<Reply> urgentCompletions;std::map<std::uint64_t,Caller> urgentAdmissions;std::uint64_t activeUrgentSerial=0;
    std::unique_ptr<juce::MemoryMappedFile> diagnosticMapping;detail::RequestDiagnosticData* diagnosticData=nullptr;
    void armDiagnostic(const juce::File& root,const juce::String& name) noexcept {
        diagnosticData=nullptr;diagnosticMapping.reset();if(root==juce::File{})return;
        try {if(!root.getChildFile("mcp-request-diagnostics.enabled").existsAsFile())return;const auto file=root.getChildFile(name+"-"+juce::String(GetCurrentProcessId())+"-"+juce::Uuid().toString()+".request-age.bin");auto stream=file.createOutputStream();
            if(stream&&stream->setPosition(sizeof(detail::RequestDiagnosticData)-1)&&stream->writeByte(0)){stream.reset();diagnosticMapping=std::make_unique<juce::MemoryMappedFile>(file,juce::MemoryMappedFile::readWrite);
                if(diagnosticMapping->getSize()==sizeof(detail::RequestDiagnosticData)&&diagnosticMapping->getData()){diagnosticData=new(diagnosticMapping->getData())detail::RequestDiagnosticData{};return;}}
        }catch(...){}diagnosticMapping.reset();diagnosticData=nullptr;
        try{root.getChildFile(name+".request-age-arm-failed.txt").replaceWithText("Fixture request diagnostics unavailable");}catch(...){}
    }
    void diagnostic(detail::RequestStage stage,std::uint64_t route,std::uint64_t client,std::uint64_t deadline,std::uint32_t tool,int status=-1,std::uint64_t sampledTick=0) noexcept {
        if(!diagnosticData)return;const auto n=diagnosticData->next.fetch_add(1,std::memory_order_relaxed);
        if(n>=diagnosticData->rows.size()){diagnosticData->overflow.fetch_add(1,std::memory_order_relaxed);return;}
        auto& row=diagnosticData->rows[std::size_t(n)];row.tick=GetTickCount64();row.deadline=deadline;row.route=route;row.client=client;row.sampledTick=sampledTick;row.stage=std::uint32_t(stage);row.tool=tool;row.status=status;row.ready.store(1,std::memory_order_release);
    }
    struct ReadPin {std::uint64_t serial=0,expires=0;Caller caller;ToolCall call;std::shared_ptr<const void> lifetime;std::shared_ptr<const std::atomic_bool> revoked;bool notified=false;};
    std::array<std::optional<ReadPin>,32> reads;
    bool queueReply(Reply&& reply){
        const auto bytes=ipc::encodedSize(ipc::reply(reply),maximumResultBytes);
        if(!bytes||replies.size()>=32||outputBytes+*bytes>16*1024*1024-32768)return false;
        outputBytes+=*bytes;replies.emplace_back(std::move(reply),*bytes);return true;
    }
    void retireDispatch(){outputBytes-=dispatchReserved;dispatchReserved=0;dispatchSerial=0;}
    bool popControl(Command& out){
        if(priority.empty()){if(urgentCommands.empty())return false;out=std::move(urgentCommands.front());urgentCommands.pop_front();inputBytes-=1024;return true;}out=std::move(priority.front());priority.pop_front();
        if(out.control==CommandControl::epochLost||out.control==CommandControl::revoke
            ||(out.control==CommandControl::cancel&&out.cancelledSerial==dispatchSerial)
            ||(out.control==CommandControl::ownerDisconnected&&out.caller.owner==dispatchCaller.owner))retireDispatch();
        return true;
    }
    void lost(){std::lock_guard lock(mutex);online=false;observation={};commands.clear();replies.clear();urgentCommands.clear();urgentCompletions.clear();urgentAdmissions.clear();reads={};inputBytes=activeReplyBytes=0;outputBytes=dispatchReserved;priority.clear();
        meta.epoch=newRuntimeId();changed=true;Command c;c.control=CommandControl::epochLost;c.caller.epoch=meta.epoch;priority.push_back(std::move(c));}
    void control(const juce::var& v){
        Command c;const auto name=v["event"].toString();
        if(name=="activity"){
            std::uint64_t requests=0;
            if(!ipc::fields(v,{"version","event","clients","requests"})||!v["clients"].isInt()||int(v["clients"])<0||int(v["clients"])>8
                ||!ipc::decimal(v["requests"],requests)||!ipc::encodedSize(v,256))throw 1;
            std::lock_guard lock(mutex);observation={true,unsigned(int(v["clients"])),requests};return;
        }
        if(!ipc::fields(v,{"version","event","owner","epoch","serial"})||!ipc::uuid(v["owner"],c.caller.owner)||!ipc::uuid(v["epoch"],c.caller.epoch)||!ipc::decimal(v["serial"],c.cancelledSerial))throw 1;
        if(name=="resource_finished"){
            std::lock_guard lock(mutex);for(auto& read:reads)if(read&&read->serial==c.cancelledSerial&&read->caller.owner==c.caller.owner&&read->caller.epoch==c.caller.epoch)read.reset();return;
        }
        if(name=="cancel")c.control=CommandControl::cancel;else if(name=="owner_disconnected")c.control=CommandControl::ownerDisconnected;else throw 1;
        std::lock_guard lock(mutex);if(c.caller.epoch!=meta.epoch)return;
        for(auto& read:reads)if(read&&read->caller.owner==c.caller.owner&&read->caller.epoch==c.caller.epoch
            &&(c.control==CommandControl::ownerDisconnected||read->serial==c.cancelledSerial)){if(c.control==CommandControl::cancel)c.call=read->call;read.reset();}
        for(auto it=commands.begin();it!=commands.end();)if(it->caller.owner==c.caller.owner&&(c.control==CommandControl::ownerDisconnected||it->serial==c.cancelledSerial)){
            if(c.control==CommandControl::cancel&&(it->call.tool==Tool::applyEdit||it->call.tool==Tool::renderPreview||it->call.tool==Tool::resourceRead||it->call.tool==Tool::getJob||priorityTool(it->call.tool)))c.call=it->call;
            inputBytes-=juce::JSON::toString(it->call.arguments,true).getNumBytesAsUTF8();it=commands.erase(it);}else ++it;
        for(auto it=urgentAdmissions.begin();it!=urgentAdmissions.end();)if(it->second.owner==c.caller.owner&&(c.control==CommandControl::ownerDisconnected||it->first==c.cancelledSerial)){
            const auto serial=it->first;for(auto q=urgentCommands.begin();q!=urgentCommands.end();)if(q->serial==serial){if(c.control==CommandControl::cancel)c.call=q->call;inputBytes-=1024;q=urgentCommands.erase(q);}else ++q;
            std::erase_if(urgentCompletions,[&](const auto& reply){return reply.serial==serial;});it=urgentAdmissions.erase(it);
        }else ++it;
        if(c.control==CommandControl::cancel&&c.cancelledSerial==dispatchSerial&&c.caller.owner==dispatchCaller.owner)c.call=dispatchCall;
        if(std::none_of(priority.begin(),priority.end(),[&](const auto& old){return old.control==c.control&&old.caller.owner==c.caller.owner&&old.cancelledSerial==c.cancelledSerial;})){
            if(priority.size()>=64)throw 1;priority.push_back(std::move(c));}
    }
    void run() noexcept {
        try {
            CompanionLaunch launch;bool seen=false;std::uint64_t nextLaunch=0;
            while(!stop.load(std::memory_order_acquire)) {
                PipeConnection pipe;
                if(pipe.connect(options.pipeSuffix,PipeDomain::mcp)!=PipeStatus::ready){
                    if(GetTickCount64()>=nextLaunch){launch.start({companionExecutable(),options.pipeSuffix,options.fixtureRoot,LaunchRole::mcpService});nextLaunch=GetTickCount64()+2000;}
                    Sleep(10);continue;}
                juce::Uuid session;EndpointMetadata current;{std::lock_guard lock(mutex);current=meta;changed=false;}
                auto hello=frame(MessageKind::mcpHello,session,ipc::object({{"version",1},{"role","endpoint"},{"metadata",metadataValue(current)}}));
                if(pipe.beginWrite(hello)!=PipeStatus::ready||pipe.beginRead()!=PipeStatus::ready){pipe.close();Sleep(10);continue;}
                bool writing=true,ready=false,failed=false;const auto opened=GetTickCount64();
                while(!stop.load()&&!failed){
                    {std::lock_guard lock(mutex);if(std::any_of(reads.begin(),reads.end(),[](const auto& read){return read&&GetTickCount64()>=read->expires;})){failed=true;break;}}
                    if(writing&&activeRevoked&&activeRevoked->load(std::memory_order_acquire)){failed=true;break;}
                    if(writing){const auto s=pipe.pollWrite();if(s==PipeStatus::complete){std::lock_guard lock(mutex);outputBytes-=activeReplyBytes;activeReplyBytes=0;if(activeUrgentSerial){urgentAdmissions.erase(activeUrgentSerial);activeUrgentSerial=0;}activeResource.reset();activeRevoked.reset();writing=false;}else if(s!=PipeStatus::pending){failed=true;break;}}
                    Frame received;const auto read=pipe.pollRead(received);
                    if(read==PipeStatus::complete){
                        juce::var v;if(received.kind<MessageKind::mcpHello||received.kind>MessageKind::mcpEvent||!ipc::parse(received.payload.getData(),received.payload.getSize(),messagePayloadCap(received.kind),v)){failed=true;break;}
                        if(!ready){
                            if(received.kind!=MessageKind::mcpHello||!ipc::fields(v,{"version","status","owner"})||!v["status"].isInt()||int(v["status"])!=int(Status::ready)){failed=true;break;}
                            session=received.session;ready=true;seen=true;online=true;
                        }else if(received.session!=session){failed=true;break;}
                        else if(received.kind==MessageKind::mcpRequest){
                            Command c;RpcError error;
                            if(!ipc::fields(v,{"version","serial","deadline","owner","epoch","call"})||!ipc::decimal(v["serial"],c.serial)||!c.serial||!ipc::decimal(v["deadline"],c.deadlineMs)
                                ||!ipc::uuid(v["owner"],c.caller.owner)||!ipc::uuid(v["epoch"],c.caller.epoch)||!decodeIpcCall(v["call"],c.call,error)){failed=true;break;}
                            const auto size=juce::JSON::toString(c.call.arguments,true).getNumBytesAsUTF8();std::lock_guard lock(mutex);
                            if(c.call.instance!=meta.instance||c.caller.epoch!=meta.epoch||!meta.enabled){queueReply({c.serial,{Status::stale,ipc::object({})}});}
                            else if(priorityTool(c.call.tool)){
                                if(urgentAdmissions.size()>=16||!ipc::encodedSize(v,1024)||inputBytes+1024>8*1024*1024)queueReply({c.serial,{Status::capacity,ipc::object({})}});
                                else{urgentAdmissions.emplace(c.serial,c.caller);inputBytes+=1024;urgentCommands.push_back(std::move(c));}
                            }
                            else if(commands.size()>=32||inputBytes+size>8*1024*1024-32768){queueReply({c.serial,{Status::capacity,ipc::object({})}});}
                            else{diagnostic(detail::RequestStage::enqueued,c.serial,0,c.deadlineMs,std::uint32_t(c.call.tool));inputBytes+=size;commands.push_back(std::move(c));}
                        }else if(received.kind==MessageKind::mcpEvent)control(v);else{failed=true;break;}
                        if(pipe.beginRead()!=PipeStatus::ready){failed=true;break;}
                    }else if(read!=PipeStatus::pending){failed=true;break;}
                    if(!ready&&GetTickCount64()-opened>=2000){failed=true;break;}
                    if(ready&&!writing){std::optional<Frame> next;
                        {std::lock_guard lock(mutex);
                            while(!urgentCompletions.empty()){
                                auto& reply=urgentCompletions.front();const auto size=ipc::encodedSize(ipc::reply(reply),512);if(!size||replies.size()>=32||outputBytes+*size>16*1024*1024)break;
                                outputBytes+=*size;replies.emplace_front(std::move(reply),*size);urgentCompletions.pop_front();}
                            if(changed){next=frame(MessageKind::mcpEvent,session,ipc::object({{"version",1},{"event","metadata"},{"metadata",metadataValue(meta)}}));activeReplyBytes=next->payload.getSize();if(outputBytes+activeReplyBytes>16*1024*1024){failed=true;break;}outputBytes+=activeReplyBytes;changed=false;}
                            else if(const auto read=std::find_if(reads.begin(),reads.end(),[](const auto& r){return r&&r->revoked&&r->revoked->load(std::memory_order_acquire)&&!r->notified;});read!=reads.end()){
                                const auto& r=**read;const auto event=ipc::object({{"version",1},{"event","resource_revoked"},{"serial",ipc::decimal(r.serial)},{"owner",r.caller.owner.toDashedString()},{"epoch",r.caller.epoch.toDashedString()}});
                                if(!ipc::encodedSize(event,256)){failed=true;break;}next=frame(MessageKind::mcpEvent,session,event);activeReplyBytes=next->payload.getSize();if(outputBytes+activeReplyBytes>16*1024*1024){failed=true;break;}outputBytes+=activeReplyBytes;(*read)->notified=true;
                            }
                            else if(!replies.empty()){auto reply=std::move(replies.front());replies.pop_front();activeReplyBytes=reply.second;if(urgentAdmissions.contains(reply.first.serial))activeUrgentSerial=reply.first.serial;
                                if(reply.first.result.resourceRevoked&&reply.first.result.resourceRevoked->load(std::memory_order_acquire)){outputBytes-=activeReplyBytes;activeReplyBytes=0;}
                                else {activeResource=reply.first.result.resourceLifetime;activeRevoked=reply.first.result.resourceRevoked;next=frame(MessageKind::mcpReply,session,ipc::reply(reply.first));}}}
                        if(next){if((activeRevoked&&activeRevoked->load(std::memory_order_acquire))||next->payload.getSize()>messagePayloadCap(next->kind)||pipe.beginWrite(*next)!=PipeStatus::ready){failed=true;break;}writing=true;}}
                    std::array<HANDLE,2> handles{};DWORD count=0;
                    for(auto handle:pipe.pendingEvents())if(handle)handles[count++]=handle;
                    // Borrowed owners stay unchanged through the finite wait.
                    if(count==0)Sleep(1);
                    else{const auto wait=WaitForMultipleObjects(count,handles.data(),FALSE,1);
                        if(wait==WAIT_FAILED||(wait!=WAIT_TIMEOUT&&wait-WAIT_OBJECT_0>=count)){failed=true;break;}}
                }
                pipe.close();activeResource.reset();activeRevoked.reset();if(seen){lost();seen=false;}else online=false;Sleep(10);
            }
        }catch(...){try{lost();}catch(...){}online=false;}
        online=false;
    }
};
Endpoint::Endpoint():impl(std::make_unique<Impl>()){}
Endpoint::~Endpoint(){close();}
bool Endpoint::start(const ServiceOptions& options,const EndpointMetadata& meta){
    if(!impl->stop.load()||!valid(meta)||options.pipeSuffix.isNotEmpty()!=options.fixtureRoot.getFullPathName().isNotEmpty()
        ||(options.pipeSuffix.isNotEmpty()&&(!ipc::validSuffix(options.pipeSuffix)||!ipc::safeRoot(options.fixtureRoot.getFullPathName()))))return false;
    try{impl->options=options;impl->meta=meta;impl->armDiagnostic(options.fixtureRoot,"endpoint-"+meta.instance.toString());impl->meta.capabilities=meta.capabilities.getDynamicObject()?meta.capabilities.clone():ipc::object({});impl->stop=false;impl->worker=std::thread([state=impl.get()]{state->run();});return true;}catch(...){impl->stop=true;return false;}
}
bool Endpoint::take(Command& out){
    std::unique_lock lock(impl->mutex,std::try_to_lock);if(!lock||impl->stop.load())return false;
    if(impl->popControl(out))return true;
    if(impl->dispatchSerial||impl->outputBytes>16*1024*1024-32768-maximumResultBytes||impl->replies.size()>=32)return false;
    if(impl->commands.empty())return false;out=std::move(impl->commands.front());impl->commands.pop_front();impl->inputBytes-=juce::JSON::toString(out.call.arguments,true).getNumBytesAsUTF8();
    impl->diagnostic(detail::RequestStage::taken,out.serial,0,out.deadlineMs,std::uint32_t(out.call.tool));
    const auto diagnosticNow=GetTickCount64();if(commandExpired(out,diagnosticNow)){impl->diagnostic(detail::RequestStage::takeExpired,out.serial,0,out.deadlineMs,std::uint32_t(out.call.tool),int(Status::hostControlUnavailable),diagnosticNow);impl->queueReply({out.serial,{Status::hostControlUnavailable,ipc::object({})}});return false;}
    impl->dispatchSerial=out.serial;impl->dispatchCaller=out.caller;
    // Only a small Apply handle is needed if cancellation arrives during a reentrant callback.
    impl->dispatchCall={out.call.tool,out.call.instance,(out.call.tool==Tool::applyEdit||out.call.tool==Tool::renderPreview||out.call.tool==Tool::resourceRead||out.call.tool==Tool::getJob)?out.call.arguments:juce::var()};impl->dispatchReserved=maximumResultBytes;impl->outputBytes+=impl->dispatchReserved;return true;
}
bool Endpoint::takeControl(Command& out){std::unique_lock lock(impl->mutex,std::try_to_lock);return lock&&!impl->stop.load()&&impl->popControl(out);}
bool Endpoint::reply(Reply&& reply){
    if(impl->testAtReply){impl->testAtReply->store(true);while(!impl->testHandoffHeld->load())std::this_thread::yield();}
    std::unique_lock lock(impl->mutex,std::try_to_lock);if(!lock||impl->stop.load()||!impl->online.load())return false;
    if(const auto admitted=impl->urgentAdmissions.find(reply.serial);admitted!=impl->urgentAdmissions.end()){
        if(admitted->second.epoch!=impl->meta.epoch||impl->urgentCompletions.size()>=16||!ipc::encodedSize(ipc::reply(reply),512)||reply.result.resourceLifetime)return false;
        impl->urgentCompletions.push_back(std::move(reply));return true;
    }
    if(reply.serial!=impl->dispatchSerial||impl->dispatchCaller.epoch!=impl->meta.epoch)return false;
    auto read=impl->reads.end();if(reply.result.resourceLifetime){read=std::find_if(impl->reads.begin(),impl->reads.end(),[](const auto& p){return !p;});if(read==impl->reads.end())return false;}
    const auto lifetime=reply.result.resourceLifetime;const auto revoked=reply.result.resourceRevoked;
    const auto reserved=impl->dispatchReserved;impl->outputBytes-=reserved;
    const auto diagnosticStatus=int(reply.result.status);
    if(!impl->queueReply(std::move(reply))){impl->outputBytes+=reserved;return false;}
    impl->diagnostic(detail::RequestStage::endpointReply,impl->dispatchSerial,0,0,std::uint32_t(impl->dispatchCall.tool),diagnosticStatus);
    if(read!=impl->reads.end())*read=Impl::ReadPin{impl->dispatchSerial,GetTickCount64()+5000,impl->dispatchCaller,impl->dispatchCall,lifetime,revoked};
    impl->dispatchReserved=0;impl->dispatchSerial=0;return true;
}
void Endpoint::update(const EndpointMetadata& meta){if(!valid(meta))return;std::lock_guard lock(impl->mutex);if(meta.instance!=impl->meta.instance)return;
    if(meta.epoch!=impl->meta.epoch||!meta.enabled){impl->commands.clear();impl->replies.clear();impl->urgentCommands.clear();impl->urgentCompletions.clear();impl->urgentAdmissions.clear();impl->reads={};impl->inputBytes=0;impl->outputBytes=impl->activeReplyBytes+impl->dispatchReserved;impl->priority.clear();Command c;c.control=meta.enabled?CommandControl::epochLost:CommandControl::revoke;c.caller.epoch=meta.epoch;impl->priority.push_back(std::move(c));}
    auto next=meta;
    // A stale status copy can race reconnect; a changed epoch always becomes a fresh identity.
    if(next.epoch!=impl->meta.epoch)next.epoch=newRuntimeId();
    next.capabilities=meta.capabilities.getDynamicObject()?meta.capabilities.clone():ipc::object({});impl->meta=std::move(next);impl->changed=true;
}
void Endpoint::close() noexcept {impl->stop=true;impl->online=false;if(impl->worker.joinable())impl->worker.join();std::lock_guard lock(impl->mutex);impl->commands.clear();impl->priority.clear();impl->replies.clear();impl->urgentCommands.clear();impl->urgentCompletions.clear();impl->urgentAdmissions.clear();impl->reads={};impl->inputBytes=impl->outputBytes=impl->activeReplyBytes=impl->dispatchReserved=0;impl->dispatchSerial=0;impl->observation={};}
bool Endpoint::connected() const noexcept{return impl->online.load(std::memory_order_acquire);}
EndpointObservation Endpoint::observation() const {std::lock_guard lock(impl->mutex);return impl->online.load()?impl->observation:EndpointObservation{};}
EndpointMetadata Endpoint::metadata() const {std::lock_guard lock(impl->mutex);auto copy=impl->meta;copy.capabilities=copy.capabilities.clone();return copy;}
void detail::EndpointTestAccess::armReplyContention(Endpoint& endpoint,std::atomic_bool* atReply,const std::atomic_bool* held){endpoint.impl->testAtReply=atReply;endpoint.impl->testHandoffHeld=held;}
void detail::EndpointTestAccess::holdHandoff(Endpoint& endpoint,std::atomic_bool& entered,const std::atomic_bool& release){std::lock_guard lock(endpoint.impl->mutex);entered=true;while(!release.load())std::this_thread::yield();}
bool detail::EndpointTestAccess::controlPending(Endpoint& endpoint,CommandControl control){std::lock_guard lock(endpoint.impl->mutex);return std::any_of(endpoint.impl->priority.begin(),endpoint.impl->priority.end(),[&](const auto& command){return command.control==control;});}
bool detail::EndpointTestAccess::commandPending(Endpoint& endpoint,Tool tool){std::lock_guard lock(endpoint.impl->mutex);return std::any_of(endpoint.impl->commands.begin(),endpoint.impl->commands.end(),[&](const auto& command){return command.call.tool==tool;})||std::any_of(endpoint.impl->urgentCommands.begin(),endpoint.impl->urgentCommands.end(),[&](const auto& command){return command.call.tool==tool;});}

std::size_t detail::EndpointTestAccess::pendingResourceReads(Endpoint& endpoint){std::lock_guard lock(endpoint.impl->mutex);return std::count_if(endpoint.impl->reads.begin(),endpoint.impl->reads.end(),[](const auto& read){return read.has_value();});}

std::array<std::size_t,4> detail::EndpointTestAccess::queueUsage(Endpoint& endpoint){std::lock_guard lock(endpoint.impl->mutex);return {endpoint.impl->commands.size(),endpoint.impl->urgentAdmissions.size(),endpoint.impl->inputBytes,endpoint.impl->outputBytes};}

void detail::EndpointTestAccess::recordDiagnostic(Endpoint& endpoint,RequestStage stage,const Command& command,int status,std::uint64_t sampledTick) noexcept {endpoint.impl->diagnostic(stage,command.serial,0,command.deadlineMs,std::uint32_t(command.call.tool),status,sampledTick);}

}

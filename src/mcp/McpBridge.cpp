#include "McpBridge.h"
#include "CompanionLaunch.h"
#include <functional>
#include <map>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

namespace airack::mcp
{
namespace {
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> fields)
{ auto* o=new juce::DynamicObject; for(const auto& [k,v]:fields)o->setProperty(k,v);return o; }
bool fields(const juce::var& v,std::initializer_list<const char*> keys)
{
    if(v.isVoid())return true;
    if(auto* o=v.getDynamicObject()) {
        for(const auto& p:o->getProperties()) { bool found=false;for(auto k:keys)if(p.name.toString()==k)found=true;if(!found)return false; }
        return true;
    }
    return false;
}
bool shortString(const juce::var& v) {return v.isString() && v.toString().getNumBytesAsUTF8()<=256;}
bool sameId(const juce::var& a,const juce::var& b) {return a.isString()==b.isString() && a==b;}
class Session {
public:
    explicit Session(const BridgeOptions& o):options(o) {}
    juce::String handle(const RpcMessage& r,const std::function<void(const RpcMessage&,const ToolCall&)>& dispatch) {
        auto failure=[&](int code,const char* message){return rpcFailure(r.id,{code,message});};
        if(r.notification) {
            if(r.method=="notifications/initialized" && state==1 && fields(r.params,{"_meta"}))state=2;
            return {};
        }
        if(r.method=="ping")return fields(r.params,{"_meta"})?rpcSuccess(r.id,object({})):failure(-32602,"Invalid ping params");
        if(r.method=="initialize") {
            if(state!=0)return failure(-32600,"Already initialized");
            const auto p=r.params,client=p["clientInfo"];
            if(!fields(p,{"protocolVersion","capabilities","clientInfo","_meta"}) || !shortString(p["protocolVersion"])
                || !p["capabilities"].getDynamicObject() || juce::JSON::toString(p["capabilities"],true).getNumBytesAsUTF8()>65536
                || !client.getDynamicObject() || !shortString(client["name"]) || !shortString(client["version"])
                || (client.hasProperty("title") && !shortString(client["title"])))return failure(-32602,"Invalid initialization params");
            state=1;
            return rpcSuccess(r.id,object({{"protocolVersion",protocolVersion},{"capabilities",object({{"tools",object({{"listChanged",false}})},{"resources",object({})}})},
                {"serverInfo",object({{"name","AIR"},{"version","0.1.0"}})}}));
        }
        // Modern discovery is deliberately unsupported so clients can negotiate legacy MCP.
        if(r.method!="tools/list" && r.method!="tools/call" && r.method!="resources/list" && r.method!="resources/read" && r.method!="resources/templates/list")return failure(-32601,"Unknown method");
        if(state!=2)return failure(-32600,"Initialization incomplete");
        if(r.method=="tools/list")return fields(r.params,{"_meta"})?rpcSuccess(r.id,object({{"tools",toolDefinitions()}})):failure(-32602,"Invalid tool list params");
        if(r.method=="resources/templates/list")return fields(r.params,{"_meta"})?rpcSuccess(r.id,object({{"resourceTemplates",juce::Array<juce::var>{}}})):failure(-32602,"Invalid resource template params");
        ToolCall call;RpcError error;
        if(r.method=="resources/list"||r.method=="resources/read"){
            if(!fields(r.params,r.method=="resources/list"?std::initializer_list<const char*>{"cursor","_meta"}:std::initializer_list<const char*>{"uri","_meta"}))return failure(-32602,"Invalid resource params");
            auto args=object({});const auto key=r.method=="resources/list"?"cursor":"uri";if(r.params.hasProperty(key))args.getDynamicObject()->setProperty(key,r.params[key]);
            if(r.method=="resources/list"&&options.instance)args.getDynamicObject()->setProperty("instance_id",options.instance->toDashedString());
            if(!decodeIpcCall(object({{"method",r.method},{"arguments",args}}),call,error))return rpcFailure(r.id,error);
        }else if(!decodeToolCall(r.params,call,error))return rpcFailure(r.id,error);
        if(options.instance && call.tool!=Tool::listInstances && !call.instance.isNull() && call.instance!=*options.instance)
            return call.tool==Tool::resourceRead||call.tool==Tool::resourceList?rpcFailure(r.id,{-32002,"AIR resource: unauthorized"}):rpcSuccess(r.id,toolResult({Status::unauthorized,{}}));
        dispatch(r,call);return {};
    }
private:
    const BridgeOptions& options;int state=0;
};

class Relay {
    using Pipe=companion::PipeConnection;using Kind=companion::MessageKind;using PS=companion::PipeStatus;
    struct Request {std::uint64_t serial,at;juce::var id,call;Tool tool;bool sent=false,completed=false;};
    struct Control {std::uint64_t serial;bool finished;};
    ServiceOptions options;std::function<void(const juce::var&,ToolResult,Tool)> completed;std::function<void(const juce::var&)> revoked;
    std::mutex mutex;std::map<std::uint64_t,Request> requests;std::deque<Control> controls;
    std::atomic_bool stop{false};std::uint64_t serial=0;std::size_t inputBytes=0,activeControlBytes=0;std::thread worker;
    companion::Frame frame(Kind kind,const juce::Uuid& session,const juce::var& v){const auto text=encodeJson(v);return {kind,session,juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8())};}
    void control(std::uint64_t n,bool finished){
        if(std::any_of(controls.begin(),controls.end(),[&](const auto& c){return c.serial==n&&c.finished==finished;}))return;
        if(std::count_if(controls.begin(),controls.end(),[&](const auto& c){return c.finished==finished;})>=32||inputBytes+256>8*1024*1024){stop=true;return;}
        controls.push_back({n,finished});inputBytes+=256;
    }
    void remove(std::map<std::uint64_t,Request>::iterator it){inputBytes-=juce::JSON::toString(it->second.call,true).getNumBytesAsUTF8();requests.erase(it);}
    void fail(){std::lock_guard lock(mutex);for(const auto& [n,r]:requests){if(r.tool==Tool::resourceRead&&r.completed)revoked(r.id);else completed(r.id,{Status::unavailable,{}},r.tool);}requests.clear();controls.clear();inputBytes=activeControlBytes=0;}
    void run() noexcept {
        try{companion::CompanionLaunch launch;std::uint64_t nextLaunch=0;
            while(!stop.load()){
                Pipe pipe;if(pipe.connect(options.pipeSuffix,companion::PipeDomain::mcp)!=PS::ready){
                    if(GetTickCount64()>=nextLaunch){launch.start({companion::companionExecutable(),options.pipeSuffix,options.fixtureRoot,companion::LaunchRole::mcpService});nextLaunch=GetTickCount64()+2000;}
                    {std::lock_guard lock(mutex);for(auto it=requests.begin();it!=requests.end();)if(GetTickCount64()-it->second.at>=2000){auto current=it++;completed(current->second.id,{Status::unavailable,{}},current->second.tool);remove(current);}else ++it;}
                    Sleep(5);continue;}
                auto session=newRuntimeId();auto hello=frame(Kind::mcpHello,session,ipc::object({{"version",1},{"role","bridge"}}));
                if(pipe.beginWrite(hello)!=PS::ready||pipe.beginRead()!=PS::ready){pipe.close();Sleep(5);continue;}
                bool writing=true,ready=false,failed=false;const auto at=GetTickCount64();
                while(!stop.load()&&!failed){
                    if(writing){const auto state=pipe.pollWrite();if(state==PS::complete){std::lock_guard lock(mutex);inputBytes-=activeControlBytes;if(activeControlBytes)controls.pop_front();activeControlBytes=0;writing=false;}else if(state!=PS::pending){failed=true;break;}}
                    companion::Frame f;const auto read=pipe.pollRead(f);
                    if(read==PS::complete){juce::var v;
                        if(!ipc::parse(f.payload.getData(),f.payload.getSize(),companion::messagePayloadCap(f.kind),v)){failed=true;break;}
                        if(!ready){if(f.kind!=Kind::mcpHello||!ipc::fields(v,{"version","status","owner"})||!v["status"].isInt()||int(v["status"])!=int(Status::ready)){failed=true;break;}session=f.session;ready=true;}
                        else {
                            if(f.session!=session){failed=true;break;}
                            std::uint64_t n=0;
                            if(f.kind==Kind::mcpEvent&&ipc::fields(v,{"version","event","serial"})&&v["event"].toString()=="resource_revoked"&&f.payload.getSize()<=256&&ipc::decimal(v["serial"],n)){
                                std::lock_guard lock(mutex);const auto it=requests.find(n);if(it!=requests.end()){if(it->second.tool!=Tool::resourceRead){failed=true;break;}revoked(it->second.id);control(n,true);remove(it);}
                            }else{
                                ToolResult result;
                                if(f.kind!=Kind::mcpReply||!ipc::fields(v,{"version","serial","status","data"})||!ipc::decimal(v["serial"],n)||!ipc::result(v,result)){failed=true;break;}
                                std::lock_guard lock(mutex);const auto it=requests.find(n);if(it!=requests.end()){
                                    if(it->second.completed){failed=true;break;}
                                    const bool retained=priorityTool(it->second.tool)||(it->second.tool==Tool::resourceRead&&result.status==Status::ready);
                                    it->second.completed=retained;completed(it->second.id,std::move(result),it->second.tool);if(!retained)remove(it);
                                }
                            }
                        }
                        if(pipe.beginRead()!=PS::ready){failed=true;break;}
                    }else if(read!=PS::pending){failed=true;break;}
                    if(!ready&&GetTickCount64()-at>=2000){failed=true;break;}
                    if(ready&&!writing){std::optional<companion::Frame> next;
                        {std::lock_guard lock(mutex);if(!controls.empty()){
                                const auto c=controls.front();const auto value=ipc::object({{"version",1},{"event",c.finished?"resource_finished":"cancel"},{"serial",ipc::decimal(c.serial)}});
                                if(!ipc::encodedSize(value,256)){failed=true;break;}next=frame(Kind::mcpEvent,session,value);activeControlBytes=256;
                            }else for(bool priority:{true,false}){for(auto& [n,r]:requests)if(!r.sent&&priorityTool(r.tool)==priority){next=frame(Kind::mcpRequest,session,ipc::object({{"version",1},{"serial",ipc::decimal(n)},{"call",r.call}}));r.sent=true;break;}if(next)break;}}
                        if(next){if(pipe.beginWrite(*next)!=PS::ready){failed=true;break;}writing=true;}}
                    std::array<HANDLE,2> handles{};DWORD count=0;
                    for(auto handle:pipe.pendingEvents())if(handle)handles[count++]=handle;
                    // Borrowed owners stay unchanged through the finite wait.
                    if(count==0)Sleep(1);
                    else{const auto wait=WaitForMultipleObjects(count,handles.data(),FALSE,1);
                        if(wait==WAIT_FAILED||(wait!=WAIT_TIMEOUT&&wait-WAIT_OBJECT_0>=count)){failed=true;break;}}
                }
                pipe.close();if(!stop.load())fail();Sleep(5);
            }
        }catch(...){if(!stop.load())fail();stop=true;}
    }
public:
    Relay(const ServiceOptions& o,std::function<void(const juce::var&,ToolResult,Tool)> f,std::function<void(const juce::var&)> r):options(o),completed(std::move(f)),revoked(std::move(r)),worker([this]{run();}){}
    ~Relay(){close();}
    void close(){stop=true;if(worker.joinable())worker.join();}
    void submit(const RpcMessage& rpc,const ToolCall& call){
        const auto value=ipc::call(call);const auto bytes=juce::JSON::toString(value,true).getNumBytesAsUTF8();
        if(stop.load()){completed(rpc.id,{Status::unavailable,{}},call.tool);return;}
        bool full=false;{std::lock_guard lock(mutex);const bool priority=priorityTool(call.tool);
            const auto count=std::count_if(requests.begin(),requests.end(),[&](const auto& r){return priorityTool(r.second.tool)==priority;});
            full=count>=(priority?2:8)||inputBytes+bytes>8*1024*1024-(priority?0:32768)||(priority&&bytes>1024)||serial==UINT64_MAX;
            if(!full){const auto n=++serial;requests.emplace(n,Request{n,GetTickCount64(),rpc.id,value,call.tool});inputBytes+=bytes;}}
        if(full)completed(rpc.id,{Status::capacity,{}},call.tool);
    }
    void releaseOutput(const juce::var& id){std::lock_guard lock(mutex);for(auto it=requests.begin();it!=requests.end();++it)if(it->second.completed&&sameId(it->second.id,id)){
        if(it->second.tool==Tool::resourceRead)control(it->first,true);remove(it);return;}}
    void cancel(const juce::var& id){std::lock_guard lock(mutex);for(auto it=requests.begin();it!=requests.end();)if(sameId(it->second.id,id)){
        auto current=it++;if(current->second.sent)control(current->first,false);remove(current);
    }else ++it;}
};

struct Output {juce::var id;std::string bytes;bool cancellable=true,capacity=false,resource=false;};
std::atomic_bool consoleStopped{false};
BOOL WINAPI control(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT || event==CTRL_LOGOFF_EVENT || event==CTRL_SHUTDOWN_EVENT) {
        consoleStopped.store(true,std::memory_order_release);return TRUE;
    }
    return FALSE;
}
}
int runStdio(const BridgeOptions& options)
{
    HANDLE input=nullptr,output=nullptr;
    if(!DuplicateHandle(GetCurrentProcess(),GetStdHandle(STD_INPUT_HANDLE),GetCurrentProcess(),&input,0,FALSE,DUPLICATE_SAME_ACCESS))return 1;
    if(!DuplicateHandle(GetCurrentProcess(),GetStdHandle(STD_OUTPUT_HANDLE),GetCurrentProcess(),&output,0,FALSE,DUPLICATE_SAME_ACCESS)) {CloseHandle(input);return 1;}
    std::atomic_bool stop{false},readerDone{false},writerDone{false},failed{false};
    std::mutex mutex;std::condition_variable changed;std::deque<Output> queue;std::size_t queuedBytes=0,activeBytes=0;std::uint64_t toolResults=0,refusedResults=0;bool writing=false,activeResource=false;juce::var activeId;std::uint64_t activeAt=0;
    std::deque<std::pair<juce::var,ToolResult>> priorityResults;
    Session session(options);
    auto observe=[&] {
        if(options.service.pipeSuffix.isEmpty()||!ipc::safeRoot(options.service.fixtureRoot.getFullPathName()))return;
        const auto record=ipc::object({{"processId",int(GetCurrentProcessId())},{"toolResults",ipc::decimal(toolResults)},{"refusedResults",ipc::decimal(refusedResults)},
            {"queuedBytes",ipc::decimal(queuedBytes)},{"activeBytes",ipc::decimal(activeBytes)},{"activeResource",activeResource},{"queuedResources",int(std::count_if(queue.begin(),queue.end(),[](const auto& q){return q.resource;}))}});
        options.service.fixtureRoot.getChildFile("mcp-bridge-"+juce::String(int(GetCurrentProcessId()))+".json").replaceWithText(juce::JSON::toString(record,true));
    };
    auto enqueue=[&](juce::String text,const juce::var& id,bool cancellable,bool tool=false,bool resource=false) {
        const auto length=text.getNumBytesAsUTF8();
        if(length>maximumResultBytes) {text=rpcFailure(id,{-32603,"Result exceeds limit"});}
        std::lock_guard lock(mutex);if(stop.load())return;
        bool capacity=false;
        if(queue.size()+std::size_t(writing)>=8 || queuedBytes+activeBytes+text.getNumBytesAsUTF8()+1>16*1024*1024-32768) {
            // One separately reserved refusal keeps cancellation/control reads independent of normal output admission.
            if(std::any_of(queue.begin(),queue.end(),[](const auto& q){return q.capacity;})) {failed=true;stop=true;changed.notify_all();return;}
            text=tool?rpcSuccess(id,toolResult({Status::capacity,{}})):rpcFailure(id,{-32000,"Bridge capacity"});capacity=true;
        }
        std::string bytes(text.toRawUTF8(),text.getNumBytesAsUTF8());bytes.push_back('\n');queuedBytes+=bytes.size();
        queue.push_back({id,std::move(bytes),cancellable,capacity,resource&&!capacity});if(tool){++toolResults;if(capacity)++refusedResults;}observe();changed.notify_all();
    };
    Relay relay(options.service,[&](const juce::var& id,ToolResult result,Tool tool){
        if(priorityTool(tool)&&result.status!=Status::capacity){std::lock_guard lock(mutex);if(priorityResults.size()>=2){failed=true;stop=true;}else priorityResults.emplace_back(id,std::move(result));changed.notify_all();return;}
        const bool resource=tool==Tool::resourceList||tool==Tool::resourceRead;
        if(resource){enqueue(result.status==Status::ready?rpcSuccess(id,result.data):rpcFailure(id,{-32002,"AIR resource: "+toolResult(result)["structuredContent"]["status"].toString()}),id,true,false,tool==Tool::resourceRead&&result.status==Status::ready);}
        else enqueue(rpcSuccess(id,toolResult(result)),id,true,true);
    },[&](const juce::var& id){
        bool removed=false;
        {std::lock_guard lock(mutex);
            if(writing&&activeResource&&sameId(activeId,id)){failed=true;stop=true;changed.notify_all();return;}
            for(auto it=queue.begin();it!=queue.end();)if(it->resource&&sameId(it->id,id)){queuedBytes-=it->bytes.size();it=queue.erase(it);removed=true;}else ++it;
        }
        if(removed)enqueue(rpcFailure(id,{-32002,"AIR resource: stale"}),id,true);
    });
    auto line=[&](std::string_view bytes) {
        RpcMessage r;RpcError error;
        if(!decodeRpc(bytes,r,error)) {if(!r.notification)enqueue(rpcFailure(r.id,error),r.id,false);return;}
        if(r.notification && r.method=="notifications/cancelled") {
            if(!fields(r.params,{"requestId","reason","_meta"}) || !validRpcId(r.params["requestId"]) ||
                (r.params.hasProperty("reason") && !shortString(r.params["reason"])))return;
            relay.cancel(r.params["requestId"]);
            std::lock_guard lock(mutex);
            if(writing&&activeResource&&sameId(activeId,r.params["requestId"])){failed=true;stop=true;changed.notify_all();}
            std::erase_if(priorityResults,[&](const auto& p){return sameId(p.first,r.params["requestId"]);});
            for(auto it=queue.begin();it!=queue.end();) {
                if(it->cancellable && sameId(it->id,r.params["requestId"])) {queuedBytes-=it->bytes.size();it=queue.erase(it);}else ++it;
            }
            return;
        }
        auto response=session.handle(r,[&](const RpcMessage& rpc,const ToolCall& call){relay.submit(rpc,call);});if(response.isNotEmpty())enqueue(response,r.id,r.method!="initialize",r.method=="tools/call");
    };
    consoleStopped=false;const bool consoleHandler=SetConsoleCtrlHandler(control,TRUE)!=FALSE;
    std::thread reader,writer;
    try {
        writer=std::thread([&,owned=output] {
            try {
                while(!stop.load()) {
                    Output next;
                    {std::unique_lock lock(mutex);changed.wait(lock,[&]{return stop.load() || !queue.empty() || !priorityResults.empty() || readerDone.load();});
                        if(stop.load() || (queue.empty()&&priorityResults.empty()))break;
                        if(!priorityResults.empty()&&queuedBytes+activeBytes<=16*1024*1024-4096){
                            auto result=std::move(priorityResults.front());priorityResults.pop_front();const auto text=rpcSuccess(result.first,toolResult(result.second));
                            if(text.getNumBytesAsUTF8()+1>4096){failed=true;stop=true;break;}
                            next={result.first,std::string(text.toRawUTF8(),text.getNumBytesAsUTF8())+"\n",true,false};
                        }else{if(queue.empty()){failed=true;stop=true;break;}next=std::move(queue.front());queue.pop_front();queuedBytes-=next.bytes.size();}
                        activeBytes=next.bytes.size();activeId=next.id;activeResource=next.resource;activeAt=GetTickCount64();writing=true;observe();}
                    std::size_t at=0;
                    while(at<next.bytes.size() && !stop.load()) {
                        DWORD n=0;
                        if(!WriteFile(owned,next.bytes.data()+at,static_cast<DWORD>(next.bytes.size()-at),&n,nullptr) || !n) {if(!stop.load())failed=true;stop=true;break;}
                        at+=n;
                    }
                    if(at==next.bytes.size()&&!stop.load())relay.releaseOutput(next.id);
                    {std::lock_guard lock(mutex);activeBytes=0;activeId=juce::var();activeResource=false;activeAt=0;writing=false;observe();}
                }
            }catch(...){failed=true;stop=true;}
            CloseHandle(owned);writerDone=true;changed.notify_all();
        });output=nullptr;
        reader=std::thread([&,owned=input] {
            try {
                std::string pending;pending.reserve(4096);bool oversized=false;
                char chunk[4096];
                while(!stop.load()) {
                    DWORD n=0;
                    if(!ReadFile(owned,chunk,sizeof(chunk),&n,nullptr)) {
                        const auto error=GetLastError();
                        if(!stop.load() && error!=ERROR_BROKEN_PIPE && error!=ERROR_HANDLE_EOF) {failed=true;stop=true;}
                        break;
                    }
                    if(!n)break;
                    for(DWORD i=0;i<n && !stop.load();++i) {
                        const char c=chunk[i];
                        if(c=='\n') {if(!oversized)line(pending);pending.clear();oversized=false;}
                        else if(!oversized) {
                            if(pending.size()==maximumRequestBytes) {pending.clear();oversized=true;enqueue(rpcFailure({}, {-32700,"Request exceeds limit"}),{},false);}
                            else pending.push_back(c);
                        }
                    }
                }
                if(!stop.load() && !oversized && !pending.empty())enqueue(rpcFailure({}, {-32700,"Truncated request"}),{},false);
            }catch(...){failed=true;stop=true;}
            CloseHandle(owned);
            {std::lock_guard lock(mutex);readerDone=true;}
            changed.notify_all();
        });input=nullptr;
        std::uint64_t eofAt=0;
        while(!stop.load() && !(readerDone.load() && writerDone.load())) {
            if(consoleStopped.load())stop=true;
            {std::lock_guard lock(mutex);if(writing&&activeAt&&GetTickCount64()-activeAt>=5000){std::cerr<<"AIR MCP stdio output deadline"<<std::endl;failed=true;stop=true;}}
            if(readerDone.load()) {if(!eofAt)eofAt=GetTickCount64();if(GetTickCount64()-eofAt>=1000)stop=true;}
            Sleep(1);
        }
    }catch(...){failed=true;stop=true;}
    relay.close();
    {std::lock_guard lock(mutex);stop=true;}
    changed.notify_all();
    // Repeat cancellation across the check/ReadFile or WriteFile race. Only these workers' IO is cancelled.
    while((reader.joinable() && !readerDone.load()) || (writer.joinable() && !writerDone.load())) {
        if(reader.joinable() && !readerDone.load())CancelSynchronousIo(reader.native_handle());
        if(writer.joinable() && !writerDone.load())CancelSynchronousIo(writer.native_handle());
        Sleep(1);
    }
    if(reader.joinable())reader.join();if(writer.joinable())writer.join();
    if(input)CloseHandle(input);if(output)CloseHandle(output);
    if(consoleHandler)SetConsoleCtrlHandler(control,FALSE);
    if(failed.load())std::cerr<<"AIR MCP stdio unavailable"<<std::endl;
    return failed.load()?1:0;
}
}

#include <new>
#include "McpService.h"
#include "CompanionPipe.h"
#include "McpEndpoint.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <deque>
#include <map>
#include <vector>
namespace airack::mcp {
namespace {
using namespace companion;
Frame frame(MessageKind kind,const juce::Uuid& session,const juce::var& value) {
    const auto text=encodeJson(value);return {kind,session,juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8())};
}
bool metadata(const juce::var& v,EndpointMetadata& m) {
    if(!ipc::fields(v,{"instance_id","epoch","label","version","enabled","capabilities"})||!ipc::uuid(v["instance_id"],m.instance)||!ipc::uuid(v["epoch"],m.epoch)
        ||!ipc::text(v["label"])||!ipc::text(v["version"])||!v["enabled"].isBool()||!v["capabilities"].getDynamicObject()
        ||juce::JSON::toString(v["capabilities"],true).getNumBytesAsUTF8()>8192)return false;
    m.label=v["label"].toString();m.version=v["version"].toString();m.enabled=bool(v["enabled"]);m.capabilities=v["capabilities"];return true;
}
struct Connection {
    PipeConnection pipe;juce::Uuid session=newRuntimeId(),owner=juce::Uuid::null();EndpointMetadata meta;PipePeer peer;
    enum Role { pending,bridge,endpoint } role=pending;
    std::uint64_t accepted=GetTickCount64();bool writing=false,dead=false,activeUrgent=false;std::uint64_t activeSerial=0;
    std::uint64_t activePrioritySerial=0;juce::Uuid activePriorityOwner=juce::Uuid::null();std::size_t activePriorityBytes=0;bool writePending=false;
    struct ResourcePage {juce::Uuid token;std::vector<std::pair<juce::Uuid,juce::Uuid>> registry;std::size_t next=0;};
    std::optional<ResourcePage> resourcePage;
    struct Output {Frame frame;std::uint64_t serial=0;bool activity=false;};
    std::uint64_t admittedRequests=0;
    std::optional<std::pair<unsigned,std::uint64_t>> lastObservation;
    bool activityQueued=false;
    bool activeActivity=false;
    std::deque<Output> output;std::deque<Frame> priority;std::deque<std::pair<Frame,std::size_t>> urgent;
    std::deque<Reply> urgentCompletions;std::size_t activeBytes=0;
};
struct Route {std::uint64_t serial=0,clientSerial=0,deadline=0;Connection* bridge=nullptr;Connection* endpoint=nullptr;std::size_t bytes=0;bool resourcePage=false,priority=false,resourceRead=false,forwarded=false,revoked=false;juce::Uuid epoch;};
class Service {
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
    PipeListener listener;std::vector<std::unique_ptr<Connection>> connections;
    std::map<std::uint64_t,Route> routes;std::uint64_t nextSerial=0;std::size_t inputs=0,outputs=0;
    std::uint64_t lastActiveActivityTick=0,lastActiveActivityBytes=0,lastActiveActivityOutputs=0;
    unsigned count(Connection::Role role) const {return unsigned(std::count_if(connections.begin(),connections.end(),[&](const auto& c){return !c->dead&&c->role==role;}));}
    bool send(Connection& c,MessageKind kind,juce::var v,bool priority=false,std::uint64_t serial=0,bool activity=false) {
        if(c.dead)return false;const auto cap=messagePayloadCap(kind);
        const auto binary=kind==MessageKind::mcpReply?encodeBinaryJson(v,cap):BinaryJsonEncoding{};
        if(binary.resource?binary.text.isEmpty():!ipc::encodedSize(v,cap))return false;
        auto f=binary.resource?Frame{kind,c.session,juce::MemoryBlock(binary.text.toRawUTF8(),binary.text.getNumBytesAsUTF8())}:frame(kind,c.session,v);
        const auto size=f.payload.getSize();
        if(size>messagePayloadCap(kind)){c.dead=true;return false;}
        if(priority){if(c.priority.size()>=64||outputs+size>16*1024*1024){c.dead=true;return false;}outputs+=size;c.priority.push_back(std::move(f));return true;}
        if(outputs+size>16*1024*1024-32768||c.output.size()>=40)return false;
        outputs+=size;c.output.push_back({std::move(f),serial,activity});return true;
    }
    void activity(){
        const auto clients=count(Connection::bridge);
        for(auto& c:connections)if(!c->dead&&c->role==Connection::endpoint&&!c->activityQueued){
            const auto latest=std::pair{clients,c->admittedRequests};if(c->lastObservation==latest)continue;
            const auto value=ipc::object({{"version",1},{"event","activity"},{"clients",int(clients)},{"requests",ipc::decimal(c->admittedRequests)}});
            // Normal admission only: retain one latest dirty value and retry congestion without closing a peer.
            if(ipc::encodedSize(value,256)&&send(*c,MessageKind::mcpEvent,value,false,0,true)){c->activityQueued=true;c->lastObservation=latest;}
        }
    }
    bool answer(Connection& c,std::uint64_t serial,Status status,juce::var data={},bool priority=false) {
        if(priority){Reply reply{serial,{status,data}};if(c.urgentCompletions.size()>=2||!ipc::encodedSize(ipc::reply(reply),512)){c.dead=true;return false;}c.urgentCompletions.push_back(std::move(reply));return true;}
        if(send(c,MessageKind::mcpReply,ipc::reply({serial,{status,data}}),false,serial))return true;
        send(c,MessageKind::mcpReply,ipc::reply({serial,{Status::capacity,{}}}),true);return false;
    }
    void event(Connection& c,const char* name,const Caller& caller,std::uint64_t serial=0) {
        send(c,MessageKind::mcpEvent,ipc::object({{"version",1},{"event",name},{"owner",caller.owner.toDashedString()},{"epoch",caller.epoch.toDashedString()},{"serial",ipc::decimal(serial)}}),true);
    }
    void erase(std::map<std::uint64_t,Route>::iterator it) {
        if(auto* endpoint=it->second.endpoint){auto& c=*endpoint;
        for(auto queued=c.output.begin();queued!=c.output.end();) {
            if(queued->frame.kind==MessageKind::mcpRequest&&queued->serial==it->first){outputs-=queued->frame.payload.getSize();queued=c.output.erase(queued);}else ++queued;
        }
        for(auto q=c.urgent.begin();q!=c.urgent.end();){juce::var value;std::uint64_t serial=0;
            if(q->first.kind==MessageKind::mcpRequest&&ipc::parse(q->first.payload.getData(),q->first.payload.getSize(),1024,value)&&ipc::decimal(value["serial"],serial)&&serial==it->first){outputs-=q->second;q=c.urgent.erase(q);}else ++q;}
        }
        inputs-=it->second.bytes;routes.erase(it);
    }
    void revoke(Route& r) {
        r.revoked=true;r.deadline=GetTickCount64()+5000;
        auto& bridge=*r.bridge;
        for(auto it=bridge.output.begin();it!=bridge.output.end();)if(it->serial==r.clientSerial){outputs-=it->frame.payload.getSize();it=bridge.output.erase(it);}else ++it;
        if(bridge.writing&&bridge.activeSerial==r.clientSerial){bridge.dead=true;return;}
        const auto value=ipc::object({{"version",1},{"event","resource_revoked"},{"serial",ipc::decimal(r.clientSerial)}});
        if(!ipc::encodedSize(value,256)||!send(bridge,MessageKind::mcpEvent,value,true))bridge.dead=true;
    }
    void invalidate(Connection& c,Status status) {
        for(auto it=routes.begin();it!=routes.end();){auto current=it++;auto& r=current->second;
            if(r.bridge==&c){if(r.endpoint)event(*r.endpoint,"cancel",{c.owner,r.epoch},r.serial);erase(current);}
            else if(r.endpoint==&c){if(r.resourceRead){revoke(r);if(c.dead)r.endpoint=nullptr;}else{answer(*r.bridge,r.clientSerial,status,{},r.priority);erase(current);}}}
    }
    void disconnect(Connection& c) {
        invalidate(c,Status::instanceGone);
        if(c.role==Connection::bridge)for(auto& endpoint:connections)if(!endpoint->dead&&endpoint->role==Connection::endpoint)event(*endpoint,"owner_disconnected",{c.owner,endpoint->meta.epoch});
        c.pipe.close(); // Retire owned IO before releasing its active frame credit.
        for(const auto& f:c.output)outputs-=f.frame.payload.getSize();for(const auto& f:c.priority)outputs-=f.payload.getSize();for(const auto& f:c.urgent)outputs-=f.second;outputs-=c.activeBytes;c.activeBytes=0;c.output.clear();c.priority.clear();c.urgent.clear();c.urgentCompletions.clear();c.activePrioritySerial=c.activePriorityBytes=0;c.activePriorityOwner=juce::Uuid::null();
    }
    bool hello(Connection& c,const Frame& f,const juce::var& v) {
        if(f.kind!=MessageKind::mcpHello||!ipc::fields(v,{"version","role","metadata"})||!v["role"].isString())return false;
        const auto role=v["role"].toString();Status status=Status::ready;
        if(role=="bridge") {if(v.hasProperty("metadata"))return false;if(count(Connection::bridge)>=8)status=Status::capacity;else c.role=Connection::bridge;}
        else if(role=="endpoint") {
            if(!metadata(v["metadata"],c.meta))return false;
            if(count(Connection::endpoint)>=16)status=Status::capacity;
            else if(std::any_of(connections.begin(),connections.end(),[&](const auto& other){return other.get()!=&c&&!other->dead&&other->role==Connection::endpoint&&other->meta.instance==c.meta.instance;}))status=Status::unauthorized;
            else c.role=Connection::endpoint;
        }else return false;
        c.owner=newRuntimeId();
        send(c,MessageKind::mcpHello,ipc::object({{"version",1},{"status",int(status)},{"owner",c.owner.toDashedString()}}));
        // A rejected handshake gets one bounded response, then cannot send commands.
        if(status!=Status::ready)c.accepted=0;
        return true;
    }
    std::vector<std::pair<juce::Uuid,juce::Uuid>> resourceRegistry() const {
        std::vector<std::pair<juce::Uuid,juce::Uuid>> result;
        for(const auto& c:connections)if(!c->dead&&c->role==Connection::endpoint&&c->meta.enabled)result.emplace_back(c->meta.instance,c->meta.epoch);
        return result;
    }
    bool request(Connection& c,const Frame& f,const juce::var& v,std::uint64_t now) {
        if(c.role!=Connection::bridge||f.kind!=MessageKind::mcpRequest||!ipc::fields(v,{"version","serial","call"}))return false;
        std::uint64_t serial=0;if(!ipc::decimal(v["serial"],serial)||!serial)return false;
        ToolCall call;RpcError error;if(!decodeIpcCall(v["call"],call,error))return false;
        if(std::any_of(routes.begin(),routes.end(),[&](const auto& p){return p.second.bridge==&c&&p.second.clientSerial==serial;}))return false;
        if(call.tool==Tool::listInstances) {
            juce::Array<juce::var> list;
            for(const auto& endpoint:connections)if(!endpoint->dead&&endpoint->role==Connection::endpoint){const auto& m=endpoint->meta;
                list.add(ipc::object({{"instance_id",m.instance.toDashedString()},{"epoch",m.epoch.toDashedString()},{"label",m.label},{"product","AIR"},{"version",m.version},{"enabled",m.enabled},{"capabilities",m.capabilities}}));}
            answer(c,serial,Status::ready,ipc::object({{"instances",list},{"endpoint_capacity",16},{"bridge_capacity",8}}));return true;
        }
        bool resourcePage=false;
        if(call.tool==Tool::resourceList&&call.instance.isNull()){
            if(std::any_of(routes.begin(),routes.end(),[&](const auto& r){return r.second.bridge==&c&&r.second.resourcePage;})){answer(c,serial,Status::busy);return true;}
            const auto registry=resourceRegistry();
            if(call.arguments.hasProperty("cursor")){
                if(!c.resourcePage||call.arguments["cursor"].toString()!=c.resourcePage->token.toDashedString()||c.resourcePage->registry!=registry||c.resourcePage->next>=registry.size()){answer(c,serial,Status::stale);return true;}
            }else c.resourcePage=Connection::ResourcePage{newRuntimeId(),registry,0};
            if(registry.empty()){answer(c,serial,Status::ready,ipc::object({{"resources",juce::Array<juce::var>{}}}));return true;}
            call.instance=registry[c.resourcePage->next].first;call.arguments=ipc::object({{"instance_id",call.instance.toDashedString()}});resourcePage=true;
        }
        auto target=std::find_if(connections.begin(),connections.end(),[&](const auto& e){return !e->dead&&e->role==Connection::endpoint&&e->meta.instance==call.instance;});
        if(target==connections.end()){answer(c,serial,Status::instanceGone);return true;}
        auto& e=**target;if(call.tool==Tool::resourceRead){if(juce::Uuid(call.arguments["uri"].toString().substring(51,87))!=e.meta.epoch){answer(c,serial,Status::stale);return true;}}if(!e.meta.enabled){answer(c,serial,Status::unauthorized);return true;}
        const bool priority=priorityTool(call.tool);const auto input=priority?std::size_t(1024):f.payload.getSize();
        auto total=std::count_if(routes.begin(),routes.end(),[&](const auto& r){return r.second.priority==priority;});
        auto own=std::count_if(routes.begin(),routes.end(),[&](const auto& r){return r.second.bridge==&c&&r.second.priority==priority;});
        if(priority){for(const auto& connection:connections)if(connection->role==Connection::bridge){const auto pending=connection->urgentCompletions.size()+connection->urgent.size()+unsigned(connection->activeUrgent);total+=pending;if(connection.get()==&c)own+=pending;}}
        if(priority)for(const auto& connection:connections)if(connection->writing&&connection->activePrioritySerial&&!routes.contains(connection->activePrioritySerial)){++total;if(connection->activePriorityOwner==c.owner)++own;}
        if(total>=(priority?16:32)||own>=(priority?2:8)||inputs+input>8*1024*1024-(priority?0:32768)){answer(c,serial,Status::capacity);return true;}
        if(nextSerial==UINT64_MAX){answer(c,serial,Status::capacity);return true;}
        const auto route=++nextSerial;
        auto command=ipc::object({{"version",1},{"serial",ipc::decimal(route)},{"deadline",ipc::decimal(now+2000)},
            {"owner",c.owner.toDashedString()},{"epoch",e.meta.epoch.toDashedString()},{"call",ipc::call(call)}});
        // Reserve input and route atomically before exposing the command.
        routes.emplace(route,Route{route,serial,now+2000,&c,&e,input,resourcePage,priority,call.tool==Tool::resourceRead,false,false,e.meta.epoch});inputs+=input;diagnostic(detail::RequestStage::admitted,route,serial,now+2000,std::uint32_t(call.tool),-1,now);
        if(priority){const auto bytes=ipc::encodedSize(command,1024);if(!bytes||e.urgent.size()>=16||outputs+*bytes>16*1024*1024){erase(routes.find(route));answer(c,serial,Status::capacity);}
            else{outputs+=*bytes;e.urgent.emplace_back(frame(MessageKind::mcpRequest,e.session,command),*bytes);} // Queued/active frame keeps its own charge after route retirement.
        }else if(!send(e,MessageKind::mcpRequest,command,false,route)){erase(routes.find(route));answer(c,serial,Status::capacity);}if(routes.contains(route)&&e.admittedRequests!=UINT64_MAX)++e.admittedRequests;return true;
    }
    bool receive(Connection& c,const Frame& f,std::uint64_t now) {
        if(f.kind<MessageKind::mcpHello||f.kind>MessageKind::mcpEvent)return false;
        juce::var v;if(!ipc::parse(f.payload.getData(),f.payload.getSize(),messagePayloadCap(f.kind),v))return false;
        if(c.role==Connection::pending){if(c.accepted==0)return false;return hello(c,f,v);}
        if(f.session!=c.session)return false;
        if(c.role==Connection::bridge){
            if(f.kind==MessageKind::mcpRequest)return request(c,f,v,GetTickCount64());
            if(f.kind!=MessageKind::mcpEvent||!ipc::fields(v,{"version","event","serial"})||(v["event"].toString()!="cancel"&&v["event"].toString()!="resource_finished"))return false;
            std::uint64_t serial=0;if(!ipc::decimal(v["serial"],serial)||!serial)return false;
            for(auto it=routes.begin();it!=routes.end();++it)if(it->second.bridge==&c&&it->second.clientSerial==serial){auto& r=it->second;const bool finished=v["event"].toString()=="resource_finished";if(finished){if(!r.resourceRead||(!r.forwarded&&!r.revoked))return false;if(r.endpoint)event(*r.endpoint,"resource_finished",{c.owner,r.epoch},r.serial);erase(it);}else if(r.resourceRead){if(r.endpoint)event(*r.endpoint,"cancel",{c.owner,r.epoch},r.serial);revoke(r);}else{event(*r.endpoint,"cancel",{c.owner,r.epoch},r.serial);erase(it);}break;}return true;
        }
        if(f.kind==MessageKind::mcpReply){
            if(!ipc::fields(v,{"version","serial","status","data"}))return false;std::uint64_t serial=0;ToolResult result;
            if(!ipc::decimal(v["serial"],serial)||!ipc::result(v,result))return false;
            const auto it=routes.find(serial);if(it==routes.end())return true;if(it->second.endpoint!=&c)return false;
            const auto r=it->second;if(r.forwarded||r.revoked)return true;
            if(r.resourcePage&&result.status==Status::ready){auto& page=r.bridge->resourcePage;
                if(!page||page->registry!=resourceRegistry()){result={Status::stale,{}};}
                else{++page->next;if(page->next<page->registry.size())result.data.getDynamicObject()->setProperty("nextCursor",page->token.toDashedString());else page.reset();}}
            diagnostic(detail::RequestStage::serviceReply,r.serial,r.clientSerial,r.deadline,UINT32_MAX,int(result.status),now);
            const bool valid=now<r.deadline;
            if(!valid)diagnostic(detail::RequestStage::serviceLate,r.serial,r.clientSerial,r.deadline,UINT32_MAX,int(Status::hostControlUnavailable),now);
            const bool sent=answer(*r.bridge,r.clientSerial,valid?result.status:Status::hostControlUnavailable,valid?result.data:juce::var(),r.priority);
            if(r.resourceRead&&valid&&result.status==Status::ready&&sent){it->second.forwarded=true;it->second.deadline=now+5000;}
            else{if(r.resourceRead)event(c,"resource_finished",{r.bridge->owner,r.epoch},r.serial);erase(it);}return true;
        }
        if(f.kind==MessageKind::mcpEvent&&ipc::fields(v,{"version","event","serial","owner","epoch"})&&v["event"].toString()=="resource_revoked"){
            std::uint64_t serial=0;juce::Uuid owner,epoch;
            if(f.payload.getSize()>256||!ipc::decimal(v["serial"],serial)||!ipc::uuid(v["owner"],owner)||!ipc::uuid(v["epoch"],epoch))return false;
            const auto it=routes.find(serial);if(it==routes.end())return true;auto& r=it->second;
            if(r.endpoint!=&c||!r.resourceRead||r.bridge->owner!=owner||r.epoch!=epoch)return false;
            if(!r.revoked)revoke(r);return true;
        }
        if(f.kind==MessageKind::mcpEvent&&ipc::fields(v,{"version","event","metadata"})&&v["event"].toString()=="metadata") {
            EndpointMetadata next;if(!metadata(v["metadata"],next)||next.instance!=c.meta.instance)return false;
            if(next.epoch!=c.meta.epoch||!next.enabled)invalidate(c,next.enabled?Status::stale:Status::unauthorized);c.meta=std::move(next);return true;
        }
        return false;
    }
    void observe(const ServiceOptions& options){
        std::size_t normal=0,urgent=0,pending=0,charged=0,orphans=0;for(const auto& [serial,route]:routes)(route.priority?urgent:normal)++;
        unsigned queuedActivity=0,activeActivity=0;
        std::uint64_t activityBytes=0;
        for(const auto& c:connections){queuedActivity=(std::max)(queuedActivity,unsigned(std::count_if(c->output.begin(),c->output.end(),[](const auto& output){return output.activity;})));activeActivity=(std::max)(activeActivity,unsigned(c->writing&&c->activeActivity));}
        for(const auto& c:connections){if(c->activeActivity)activityBytes+=c->activeBytes;for(const auto& output:c->output)if(output.activity)activityBytes+=output.frame.payload.getSize();}
        for(const auto& c:connections)if(c->writing&&c->writePending&&c->activePriorityBytes){pending+=c->activePriorityBytes;charged+=c->activeBytes;orphans+=!routes.contains(c->activePrioritySerial);}
        const auto snapshotTick=GetTickCount64();
        options.fixtureRoot.getChildFile("mcp-service-queue.json").replaceWithText(juce::JSON::toString(ipc::object({{"inputs",ipc::decimal(inputs)},{"outputs",ipc::decimal(outputs)},{"normalRoutes",int(normal)},{"urgentRoutes",int(urgent)},{"pendingPriorityBytes",ipc::decimal(pending)},{"pendingPriorityCharge",ipc::decimal(charged)},{"orphanPriority",int(orphans)},{"maxQueuedActivity",int(queuedActivity)},{"maxActiveActivity",int(activeActivity)},{"activityBytes",ipc::decimal(activityBytes)},
            {"snapshotTick",ipc::decimal(snapshotTick)},{"lastActiveActivityTick",ipc::decimal(lastActiveActivityTick)},{"lastActiveActivityBytes",ipc::decimal(lastActiveActivityBytes)},{"lastActiveActivityOutputs",ipc::decimal(lastActiveActivityOutputs)}}),true));
    }
public:
    int run(const ServiceOptions& options,const std::atomic_bool& stop) {
        if(options.pipeSuffix.isNotEmpty()!=options.fixtureRoot.getFullPathName().isNotEmpty()||(!options.pipeSuffix.isEmpty()&&(!ipc::validSuffix(options.pipeSuffix)||!ipc::safeRoot(options.fixtureRoot.getFullPathName()))))return 1;
        armDiagnostic(options.fixtureRoot,"service");
        if(listener.open(options.pipeSuffix,PipeDomain::mcp)!=PipeStatus::ready)return 1;
            if(options.pipeSuffix.isNotEmpty()){
                FILETIME created{},exited{},kernel{},user{};
                if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return 1;
                auto* record=new juce::DynamicObject;juce::var value(record);
                record->setProperty("processId",int(GetCurrentProcessId()));
                record->setProperty("creationTime",juce::String(std::to_string((std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime)));
                record->setProperty("mode","mcp-service");record->setProperty("pipe",currentUserPipeName(options.pipeSuffix,PipeDomain::mcp));
                if(options.fixtureRoot.createDirectory().failed()||!options.fixtureRoot.getChildFile("mcp-service-observation.json").replaceWithText(juce::JSON::toString(value,true)))return 1;
            }

        const bool observeQueue=options.pipeSuffix.isNotEmpty()&&options.fixtureRoot.getChildFile("mcp-queue-observation.enabled").existsAsFile();
        std::optional<std::uint64_t> idleSince=GetTickCount64();bool hadConnection=false;
        while(!stop.load(std::memory_order_acquire)) {
            PipeConnection incoming;const auto accepted=listener.pollAccept(incoming);
            if(accepted==PipeStatus::complete){hadConnection=true;
                if(connections.size()>=31)incoming.close();else{auto c=std::make_unique<Connection>();c->peer=incoming.peer();c->pipe=std::move(incoming);c->pipe.beginRead();connections.push_back(std::move(c));}}
            const auto now=GetTickCount64();
            for(auto& c:connections){if(c->dead)continue;
                if(c->role==Connection::pending&&((c->accepted==0&&!c->writing&&c->output.empty())||(c->accepted&&now-c->accepted>=2000))){c->dead=true;continue;}
                if(c->writing){const auto s=c->pipe.pollWrite();c->writePending=s==PipeStatus::pending;if(s==PipeStatus::complete){outputs-=c->activeBytes;c->activeBytes=0;c->activeUrgent=false;c->activeActivity=false;c->activeSerial=0;c->activePrioritySerial=c->activePriorityBytes=0;c->activePriorityOwner=juce::Uuid::null();c->writing=false;}else if(s!=PipeStatus::pending){c->dead=true;continue;}}
                Frame f;const auto read=c->pipe.pollRead(f);if(read==PipeStatus::complete){if(!receive(*c,f,GetTickCount64())){c->dead=true;continue;}if(c->pipe.beginRead()!=PipeStatus::ready){c->dead=true;continue;}}
                else if(read!=PipeStatus::pending){c->dead=true;continue;}
                while(!c->urgentCompletions.empty()){
                    const auto& reply=c->urgentCompletions.front();const auto size=ipc::encodedSize(ipc::reply(reply),512);
                    if(!size||outputs+*size>16*1024*1024||c->urgent.size()>=16)break;
                    auto next=frame(MessageKind::mcpReply,c->session,ipc::reply(reply));outputs+=next.payload.getSize();c->urgent.emplace_back(std::move(next),*size);c->urgentCompletions.pop_front();}
                if(!c->writing&&(!c->priority.empty()||!c->urgent.empty()||!c->output.empty())){
                    std::optional<Frame> next;c->activeActivity=false;
                    if(!c->priority.empty()){next=std::move(c->priority.front());c->priority.pop_front();c->activeBytes=next->payload.getSize();c->activeSerial=0;}
                    else if(!c->urgent.empty()){next=std::move(c->urgent.front().first);c->activeBytes=c->urgent.front().second;c->activeUrgent=c->activeBytes!=0;c->urgent.pop_front();}
                    else{c->activeActivity=c->output.front().activity;if(c->output.front().activity)c->activityQueued=false;next=std::move(c->output.front().frame);c->activeSerial=c->output.front().serial;c->output.pop_front();c->activeBytes=next->payload.getSize();}
                    if(next->kind==MessageKind::mcpRequest&&next->payload.getSize()<=1024&&c->role==Connection::endpoint){juce::var value;std::uint64_t serial=0;if(!ipc::parse(next->payload.getData(),next->payload.getSize(),maximumControlPayloadBytes,value)||!ipc::decimal(value["serial"],serial)){c->dead=true;continue;}const auto route=routes.find(serial);if(route!=routes.end()&&route->second.priority){c->activePrioritySerial=serial;c->activePriorityOwner=route->second.bridge->owner;c->activePriorityBytes=next->payload.getSize();}}
                    if(c->pipe.beginWrite(*next)!=PipeStatus::ready){c->dead=true;continue;}c->writing=true;
                    if(observeQueue&&c->activeActivity){lastActiveActivityTick=GetTickCount64();lastActiveActivityBytes=c->activeBytes;lastActiveActivityOutputs=outputs;}}
            }
            for(auto it=routes.begin();it!=routes.end();){if(now<it->second.deadline){++it;continue;}auto current=it++;const auto r=current->second;diagnostic(detail::RequestStage::serviceExpired,r.serial,r.clientSerial,r.deadline,UINT32_MAX,int(Status::hostControlUnavailable),now);
                if(r.endpoint)event(*r.endpoint,"cancel",{r.bridge->owner,r.epoch},r.serial);if(r.resourceRead&&(r.forwarded||r.revoked))r.bridge->dead=true;else answer(*r.bridge,r.clientSerial,Status::hostControlUnavailable,{},r.priority);erase(current);}
            for(auto& c:connections)if(c->dead)disconnect(*c);
            std::erase_if(connections,[](const auto& c){return c->dead;});activity();if(observeQueue)observe(options);
            // Keep live producers/agents; release the executable after the last client closes.
            if(connections.empty()&&routes.empty()){
                if(!idleSince)idleSince=now;
                if(now-*idleSince>=(hadConnection?2000u:30000u))break;
            }else idleSince.reset();
            std::array<HANDLE,MAXIMUM_WAIT_OBJECTS> handles{};DWORD count=0;
            auto add=[&](void* handle){if(!handle||std::find(handles.begin(),handles.begin()+count,handle)!=handles.begin()+count)return true;
                if(count==handles.size())return false;handles[count++]=handle;return true;};
            if(!add(listener.pendingEvent()))return 1;
            for(const auto& c:connections)for(auto handle:c->pipe.pendingEvents())if(!add(handle))return 1;
            // Borrowed owners stay unchanged through the finite wait (at most 31*2+1 handles).
            if(count==0)Sleep(1);
            else{const auto wait=WaitForMultipleObjects(count,handles.data(),FALSE,1);
                if(wait==WAIT_FAILED||(wait!=WAIT_TIMEOUT&&wait-WAIT_OBJECT_0>=count))return 1;}
        }
        for(auto& c:connections)c->pipe.close();listener.close();return 0;
    }
};
}
int runService(const ServiceOptions& options,const std::atomic_bool& stop){try{return Service{}.run(options,stop);}catch(...){return 1;}}
}

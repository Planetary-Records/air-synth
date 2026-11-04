#include "McpEndpoint.h"
#include "CompanionLaunch.h"
#include "CompanionRuntimeState.h"
#include "InstrumentProcessor.h"
#include "OwnedFixture.h"
#include "IsolatedRackLibrary.h"
#include "BlankInstance.h"
#include "LocalPreview.h"
#include <bit>
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <iostream>
#include <set>
#include <thread>
namespace airack {struct InstrumentMcpTestAccess {static mcp::Endpoint& endpoint(InstrumentProcessor& p){return p.mcpState->endpoint;}static bool pending(const InstrumentProcessor& p){return p.mcpState->pendingReply.has_value();}static std::size_t historyCount(const InstrumentProcessor& p){return p.editHistory->count;}static bool hasStage(const InstrumentProcessor& p){return p.mcpState->stage!=nullptr;}
static std::size_t historyCursor(const InstrumentProcessor& p){return p.editHistory->cursor;}
static std::vector<std::uint64_t> historyIds(const InstrumentProcessor& p){return {p.editHistory->actionIds.begin(),p.editHistory->actionIds.begin()+p.editHistory->count};}
static bool exhaustFresh(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);if(p.editHistory->count||p.editHistory->cursor||std::any_of(p.mcpState->receipts.begin(),p.mcpState->receipts.end(),[](const auto& r){return r.has_value();}))return false;p.editHistory->nextActionId=UINT64_MAX;return true;}
static void expire(InstrumentProcessor& p){const std::lock_guard lock(p.restoreMutex);p.retireMcpStageOwned(GetTickCount64()+300000);}};}
using namespace airack;
namespace {
using namespace companion;
int checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<line<<" "<<text<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
template<class F> bool until(F f,unsigned ms=5000){const auto end=GetTickCount64()+ms;do{if(f())return true;Sleep(1);}while(GetTickCount64()<end);return false;}
Frame frame(MessageKind kind,const juce::Uuid& session,const juce::var& v){const auto text=juce::JSON::toString(v,true);return {kind,session,juce::MemoryBlock(text.toRawUTF8(),text.getNumBytesAsUTF8())};}
juce::var object(std::initializer_list<std::pair<const char*,juce::var>> f){return mcp::ipc::object(f);}
struct Child {
    HANDLE handle=nullptr;DWORD pid=0;
    bool start(const juce::File& exe,const juce::StringArray& arguments){juce::String command="\""+exe.getFullPathName()+"\"";
        for(const auto& a:arguments)command+=" \""+a+"\"";std::wstring text(command.toWideCharPointer());std::vector<wchar_t> buffer(text.begin(),text.end());buffer.push_back(0);
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        if(!CreateProcessW(exe.getFullPathName().toWideCharPointer(),buffer.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return false;
        CloseHandle(pi.hThread);handle=pi.hProcess;pid=pi.dwProcessId;return true;}
    bool alive()const{return handle&&WaitForSingleObject(handle,0)==WAIT_TIMEOUT;}
    void kill(){if(alive()){TerminateProcess(handle,97);WaitForSingleObject(handle,5000);}}
    ~Child(){kill();if(handle)CloseHandle(handle);}
};
struct StdioChild {
    HANDLE process=nullptr,input=nullptr,output=nullptr;DWORD pid=0;std::string buffered;
    bool diagnostics=false;
    void report(const char* event,ULONGLONG began,DWORD error=0)const{if(!diagnostics)return;DWORD exit=0;const auto queried=GetExitCodeProcess(process,&exit);const auto now=GetTickCount64();std::cout<<"MEASURE stdio event="<<event<<" tick_ms="<<now<<" elapsed_ms="<<now-began<<" pid="<<pid<<" wait="<<WaitForSingleObject(process,0)<<" exit_query="<<queried<<" exit="<<exit<<" error="<<error<<" buffered="<<buffered.size()<<std::endl;}
    bool start(const juce::File& exe,const juce::String& arguments,const juce::File& errorFile={}){
        diagnostics=errorFile!=juce::File();
        SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};HANDLE read=nullptr,write=nullptr;
        if(!CreatePipe(&read,&input,&security,4096)||!CreatePipe(&output,&write,&security,4096))return false;
        SetHandleInformation(input,HANDLE_FLAG_INHERIT,0);SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
        const auto errorPath=diagnostics?errorFile.getFullPathName():juce::String("NUL");HANDLE error=CreateFileW(errorPath.toWideCharPointer(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,diagnostics?CREATE_ALWAYS:OPEN_EXISTING,0,nullptr);
        STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;si.StartupInfo.hStdInput=read;si.StartupInfo.hStdOutput=write;si.StartupInfo.hStdError=error;
        SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<std::byte> attributes(bytes);si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&bytes);HANDLE inherited[]{read,write,error};UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr);
        const auto command="\""+exe.getFullPathName()+"\" "+arguments;std::wstring text(command.toWideCharPointer());std::vector<wchar_t> buffer(text.begin(),text.end());buffer.push_back(0);PROCESS_INFORMATION pi{};
        const bool ok=CreateProcessW(exe.getFullPathName().toWideCharPointer(),buffer.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT,nullptr,nullptr,&si.StartupInfo,&pi)!=FALSE;
        CloseHandle(read);CloseHandle(write);CloseHandle(error);DeleteProcThreadAttributeList(si.lpAttributeList);if(!ok)return false;process=pi.hProcess;pid=pi.dwProcessId;CloseHandle(pi.hThread);return true;
    }
    bool send(const juce::var& value){const auto began=GetTickCount64();auto text=juce::JSON::toString(value,true)+"\n";DWORD n=0;const bool written=WriteFile(input,text.toRawUTF8(),DWORD(text.getNumBytesAsUTF8()),&n,nullptr)!=FALSE;const auto error=written?0:GetLastError();report(written&&n==text.getNumBytesAsUTF8()?"send_complete":"send_failed",began,error);return written&&n==text.getNumBytesAsUTF8();}
    juce::var receive(){const auto end=GetTickCount64()+5000;const auto began=end-5000;const char* reason="deadline";DWORD error=0;report("receive_begin",began);do{const auto at=buffered.find('\n');if(at!=std::string::npos){const auto line=buffered.substr(0,at);buffered.erase(0,at+1);juce::var v;CHECK(ai::parseStrictJson(line,v,mcp::maximumResultBytes));report("complete_line",began);return v;}
            DWORD available=0;if(!PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr)){error=GetLastError();reason="peek_error";break;}if(available){char chunk[65536];DWORD n=0;if(!ReadFile(output,chunk,(std::min)(available,DWORD(sizeof(chunk))),&n,nullptr)){error=GetLastError();reason="read_error";break;}buffered.append(chunk,n);}else Sleep(1);
        }while(GetTickCount64()<end);report(reason,began,error);return {};}
    ~StdioChild(){if(input)CloseHandle(input);if(process){if(WaitForSingleObject(process,5000)!=WAIT_OBJECT_0){TerminateProcess(process,97);WaitForSingleObject(process,5000);}CloseHandle(process);}if(output)CloseHandle(output);}
};
struct MeasuredSnapshot {
    juce::String raw,priorValid,lastValid,readError,parseError;ULONGLONG tick=0,priorTick=0,lastValidTick=0;juce::int64 modified=0;bool valid=false;
    juce::var read(const juce::File& file){priorValid=lastValid;priorTick=lastValidTick;tick=GetTickCount64();modified=file.getLastModificationTime().toMilliseconds();juce::FileInputStream stream(file);readError=stream.getStatus().getErrorMessage();raw=stream.openedOk()?stream.readEntireStreamAsString():juce::String();if(stream.getStatus().failed())readError=stream.getStatus().getErrorMessage();juce::var value;const auto parsed=juce::JSON::parse(raw,value);parseError=parsed.getErrorMessage();if(parsed.failed())value=juce::var();valid=value.getDynamicObject()!=nullptr;if(valid){lastValid=raw;lastValidTick=tick;}return value;}
    void report(const char* stage)const{std::cout<<"MEASURE snapshot stage="<<stage<<" tick_ms="<<tick<<" mtime_ms="<<modified<<" valid="<<valid<<" bytes="<<raw.getNumBytesAsUTF8()<<" read_error="<<readError<<" parse_error="<<parseError<<" prior_valid_tick_ms="<<priorTick<<" raw="<<raw.substring(0,4096).replaceCharacters("\r\n","  ")<<" prior_valid="<<priorValid.substring(0,4096).replaceCharacters("\r\n","  ")<<std::endl;}
};
struct Client {
    PipeConnection pipe;juce::Uuid session;std::uint64_t serial=0;
    bool send(MessageKind k,const juce::var& value){auto f=frame(k,session,value);if(pipe.beginWrite(f)!=PipeStatus::ready)return false;
        return until([&]{return pipe.pollWrite()!=PipeStatus::pending;});}
    juce::var receive(unsigned timeout=4000,ULONGLONG deadline=0){for(unsigned events=0;events<32;++events){unsigned wait=timeout;if(deadline){const auto now=GetTickCount64();if(now>=deadline)return {};wait=unsigned((std::min)(ULONGLONG(timeout),deadline-now));}Frame f;if(pipe.beginRead()!=PipeStatus::ready)return {};
        PipeStatus status=PipeStatus::pending;if(!until([&]{status=pipe.pollRead(f);return status!=PipeStatus::pending;},wait)||status!=PipeStatus::complete)return {};
        session=f.session;juce::var v;if(!mcp::ipc::parse(f.payload.getData(),f.payload.getSize(),messagePayloadCap(f.kind),v))return {};
        if(f.kind==MessageKind::mcpEvent&&v["event"].toString()=="activity"){std::uint64_t requests;CHECK(f.payload.getSize()<=256&&mcp::ipc::fields(v,{"version","event","clients","requests"})&&v["clients"].isInt()&&int(v["clients"])>=0&&int(v["clients"])<=8&&mcp::ipc::decimal(v["requests"],requests));continue;}return v;}return {};}
    bool open(const juce::String& suffix){if(!until([&]{return pipe.connect(suffix,PipeDomain::mcp)==PipeStatus::ready;}))return false;
        if(!send(MessageKind::mcpHello,object({{"version",1},{"role","bridge"}})))return false;const auto reply=receive();return reply["status"].isInt()&&int(reply["status"])==0;}
    std::uint64_t request(const juce::Uuid& id=juce::Uuid::null()){
        const auto n=++serial;const auto name=id.isNull()?"list_instances":"get_state";const auto args=id.isNull()?object({}):object({{"instance_id",id.toDashedString()}});
        CHECK(send(MessageKind::mcpRequest,object({{"version",1},{"serial",mcp::ipc::decimal(n)},{"call",object({{"name",name},{"arguments",args}})}})));return n;}
    juce::var list(){request();const auto r=receive();CHECK(r["status"].isInt()&&int(r["status"])==0);return r["data"]["instances"];}
};
struct ServiceFixture {
    mcp::ServiceOptions options;juce::File helper;std::unique_ptr<Child> service,provider;
    struct Entry {juce::String label;juce::Uuid id;std::unique_ptr<Child> child;};std::vector<Entry> endpoints;
    explicit ServiceFixture(const juce::File& executable):helper(executable){const auto name=juce::Uuid().toString().removeCharacters("-");
        const auto diagnostic=juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{});
        options={name,juce::File(diagnostic).getChildFile("mcp-service-"+name)};options.fixtureRoot.createDirectory();if(!options.fixtureRoot.getChildFile("mcp-request-diagnostics.enabled").replaceWithText("bounded request ages"))std::cerr<<"MEASURE request_diagnostic_arm_failed"<<std::endl;}
    bool start(){service=std::make_unique<Child>();return service->start(helper,{"--mcp-service","--fixture-root",options.fixtureRoot.getFullPathName(),"--pipe-suffix",options.pipeSuffix});}
    bool addEndpoint(const juce::String& label,juce::Uuid id=juce::Uuid(),bool hold=false,bool admitted=true){auto child=std::make_unique<Child>();const auto exe=juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        if(!child->start(exe,{"--endpoint",options.fixtureRoot.getFullPathName(),options.pipeSuffix,label,id.toDashedString(),hold?"hold":"pump"}))return false;
        const auto result=options.fixtureRoot.getChildFile(label+".json");
        const bool reported=until([&]{return result.existsAsFile();},6000);const auto value=juce::JSON::parse(result.loadFileAsString());
        endpoints.push_back({label,id,std::move(child)});return reported&&bool(value["connected"])==admitted;}
    juce::var list(){Client c;if(!c.open(options.pipeSuffix))return {};return c.list();}
    void disconnect(const juce::Uuid& id){for(auto& e:endpoints)if(e.id==id&&e.child){options.fixtureRoot.getChildFile(e.label+".stop").replaceWithText("stop");
            if(!until([&]{return !e.child->alive();}))e.child->kill();e.child.reset();return;}}
    void stop(){for(auto& e:endpoints)if(e.child){options.fixtureRoot.getChildFile(e.label+".stop").replaceWithText("stop");if(!until([&]{return !e.child->alive();}))e.child->kill();e.child.reset();}
        if(service)service->kill();if(provider)provider->kill();}
    ~ServiceFixture(){stop();}
};
int endpointChild(const juce::StringArray& args){
    const mcp::ServiceOptions options{args[3],juce::File(args[2])};const auto label=args[4];mcp::Endpoint endpoint;
    const juce::Uuid id(args[5]);const bool started=endpoint.start(options,{id,juce::Uuid(),label,"fixture-1",true,object({})});
    const bool connected=started&&until([&]{return endpoint.connected();},2500);
    FILETIME created{},a{},b{},c{};GetProcessTimes(GetCurrentProcess(),&created,&a,&b,&c);
    const auto identity=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    options.fixtureRoot.getChildFile(label+".json").replaceWithText(juce::JSON::toString(object({{"connected",connected},{"instance_id",id.toDashedString()},
        {"process_id",int(GetCurrentProcessId())},{"creation_time",mcp::ipc::decimal(identity)},{"epoch",endpoint.metadata().epoch.toDashedString()}}),true));
    std::optional<mcp::Reply> pending; mcp::Caller pendingCaller;
    const auto retire=[&](const mcp::Command& c){if(pending&&(c.control==mcp::CommandControl::epochLost||c.control==mcp::CommandControl::revoke
        ||(c.control==mcp::CommandControl::cancel&&c.cancelledSerial==pending->serial)
        ||(c.control==mcp::CommandControl::ownerDisconnected&&c.caller.owner==pendingCaller.owner)))pending.reset();};
    while(!options.fixtureRoot.getChildFile(label+".stop").existsAsFile()){
        if(args[6]!="hold"){
            mcp::Command control;while(endpoint.takeControl(control))retire(control);
            if(pending&&endpoint.reply(std::move(*pending)))pending.reset();
            if(!pending){mcp::Command command;for(unsigned i=0;i<2&&endpoint.take(command);++i){
                if(command.control!=mcp::CommandControl::none){retire(command);continue;}
                pendingCaller=command.caller;pending=mcp::Reply{command.serial,{mcp::Status::ready,object({{"label",label},{"owner",command.caller.owner.toDashedString()},{"epoch",command.caller.epoch.toDashedString()}})}};
                if(endpoint.reply(std::move(*pending)))pending.reset();else break;
            }}
        }
        Sleep(5);
    }endpoint.close();return 0;
}
int processorChild(const juce::StringArray& args){
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",args[2].toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",args[3].toWideCharPointer());
    juce::ScopedJuceInitialiser_GUI gui;auto processor=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());
    const bool preview=args.size()>5&&args[5]=="preview";juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;
    if(preview){if(!copperArcInstance(*processor)||!processor->setAiMusicalContext({"bass","C","minor",30}))return 1;processor->prepareToPlay(48000,128);processor->processBlock(audio,midi);}
    if(!testing::readyRackInventory(*processor))return 1;
    const bool connected=until([&]{juce::MessageManager::getInstance()->runDispatchLoopUntil(5);return processor->mcpView().connected;});
    const auto view=processor->mcpView();FILETIME created{},a{},b{},c{};GetProcessTimes(GetCurrentProcess(),&created,&a,&b,&c);
    const auto identity=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    const juce::File root(args[2]);root.getChildFile(args[4]+".json").replaceWithText(juce::JSON::toString(object({{"connected",connected},{"instance_id",view.instance.toDashedString()},
        {"process_id",int(GetCurrentProcessId())},{"creation_time",mcp::ipc::decimal(identity)},{"epoch",view.epoch.toDashedString()},{"editor_closed",processor->getActiveEditor()==nullptr}}),true));
    while(!root.getChildFile(args[4]+".stop").existsAsFile()){if(preview)processor->processBlock(audio,midi);juce::MessageManager::getInstance()->runDispatchLoopUntil(5);}
    processor.reset();return 0;
}
void observationDecode(){
    std::cout<<"synthetic_same_user_service_observation_decode_boundaries"<<std::endl;
    for(int mode=0;mode<7;++mode){const auto suffix=juce::Uuid().toString().removeCharacters("-");const auto root=juce::File(juce::SystemStats::getEnvironmentVariable("AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",{})).getChildFile("observation-"+suffix);CHECK(root.createDirectory().wasOk());
        PipeListener listener;CHECK(listener.open(suffix,PipeDomain::mcp)==PipeStatus::ready);mcp::Endpoint endpoint;CHECK(endpoint.start({suffix,root},{juce::Uuid(),juce::Uuid(),"decoder fixture","1",true,object({})}));
        Client service;CHECK(until([&]{return listener.pollAccept(service.pipe)==PipeStatus::complete;}));const auto hello=service.receive();CHECK(hello["role"].toString()=="endpoint");service.session=juce::Uuid();
        CHECK(service.send(MessageKind::mcpHello,object({{"version",1},{"status",0},{"owner",juce::Uuid().toDashedString()}})));CHECK(until([&]{return endpoint.connected();}));
        CHECK(service.send(MessageKind::mcpEvent,object({{"version",1},{"event","activity"},{"clients",8},{"requests",mcp::ipc::decimal(UINT64_MAX)}})));
        CHECK(until([&]{const auto view=endpoint.observation();return view.available&&view.clients==8&&view.requests==UINT64_MAX;}));
        auto bad=object({{"version",1},{"event","activity"},{"clients",0},{"requests","0"}});
        if(mode==0)bad.getDynamicObject()->setProperty("clients",9);if(mode==1)bad.getDynamicObject()->setProperty("clients",-1);if(mode==2)bad.getDynamicObject()->setProperty("clients",.5);
        if(mode==3)bad.getDynamicObject()->setProperty("requests","18446744073709551616");if(mode==4)bad.getDynamicObject()->setProperty("extra",true);if(mode==5)bad.getDynamicObject()->setProperty("version",2);if(mode==6)service.session=juce::Uuid();
        CHECK(service.send(MessageKind::mcpEvent,bad));CHECK(until([&]{return !endpoint.connected();}));CHECK(!endpoint.observation().available);endpoint.close();listener.close();
    }
}
struct FixtureSampleSpan {
    std::uint64_t count=0,firstBegin=0,firstEnd=0,maxBegin=0,maxEnd=0,lastBegin=0,lastEnd=0;
    void add(std::uint64_t begin,std::uint64_t end) noexcept {
        if(!count){firstBegin=begin;firstEnd=end;}if(!count||end-begin>maxEnd-maxBegin){maxBegin=begin;maxEnd=end;}
        ++count;lastBegin=begin;lastEnd=end;
    }
};
bool decodeActivityObservation(const juce::String& text,juce::var& snapshot){
    return ai::parseStrictJson({text.toRawUTF8(),text.getNumBytesAsUTF8()},snapshot,4096)&&snapshot.getDynamicObject()!=nullptr;
}
bool activityEventWithinHeldWindow(const juce::var& snapshot,std::uint64_t heldBegin,std::uint64_t sampledAt){
    std::uint64_t active=0,observed=0,bytes=0,outputs=0;
    return mcp::ipc::decimal(snapshot["lastActiveActivityTick"],active)&&mcp::ipc::decimal(snapshot["snapshotTick"],observed)
        &&mcp::ipc::decimal(snapshot["lastActiveActivityBytes"],bytes)&&mcp::ipc::decimal(snapshot["lastActiveActivityOutputs"],outputs)
        &&heldBegin<active&&active<=observed&&observed<=sampledAt&&bytes>0&&bytes<=256&&outputs>=bytes&&outputs<=16*1024*1024;
}
void activityObservation(){
    const auto snapshot=[](std::uint64_t tick=101,std::uint64_t observed=110,std::uint64_t bytes=74,std::uint64_t outputs=74){
        return object({{"lastActiveActivityTick",mcp::ipc::decimal(tick)},{"snapshotTick",mcp::ipc::decimal(observed)},
            {"lastActiveActivityBytes",mcp::ipc::decimal(bytes)},{"lastActiveActivityOutputs",mcp::ipc::decimal(outputs)},{"maxActiveActivity",0}});
    };
    CHECK(activityEventWithinHeldWindow(snapshot(),100,120));
    CHECK(activityEventWithinHeldWindow(snapshot(110,110,256,16*1024*1024),100,110));
    CHECK(!activityEventWithinHeldWindow(snapshot(99),100,120));
    CHECK(!activityEventWithinHeldWindow(snapshot(100),100,120));
    CHECK(!activityEventWithinHeldWindow(snapshot(111,110),100,120));
    CHECK(!activityEventWithinHeldWindow(snapshot(101,121),100,120));
    CHECK(!activityEventWithinHeldWindow(snapshot(),120,100));
    CHECK(!activityEventWithinHeldWindow(snapshot(0),0,120));
    CHECK(!activityEventWithinHeldWindow(snapshot(101,110,0),100,120));
    CHECK(!activityEventWithinHeldWindow(snapshot(101,110,257,257),100,120));
    CHECK(!activityEventWithinHeldWindow(snapshot(101,110,74,73),100,120));
    CHECK(!activityEventWithinHeldWindow(snapshot(101,110,74,16*1024*1024+1),100,120));
    auto stale=snapshot(99);stale.getDynamicObject()->setProperty("maxActiveActivity",1);
    CHECK(!activityEventWithinHeldWindow(stale,100,120));
    CHECK(!activityEventWithinHeldWindow({},100,120));
    CHECK(!activityEventWithinHeldWindow(juce::Array<juce::var>{snapshot()},100,120));
    for(const auto* field:{"lastActiveActivityTick","snapshotTick","lastActiveActivityBytes","lastActiveActivityOutputs"}){
        auto missing=snapshot();missing.getDynamicObject()->removeProperty(field);CHECK(!activityEventWithinHeldWindow(missing,100,120));
        for(const auto& invalid:juce::Array<juce::var>{juce::var{},juce::var(101),juce::var(101.0),juce::var(true),juce::var(""),juce::var("-1"),juce::var("1.5"),juce::var("1x"),juce::var("18446744073709551616")}){
            auto malformed=snapshot();malformed.getDynamicObject()->setProperty(field,invalid);CHECK(!activityEventWithinHeldWindow(malformed,100,120));
        }
    }
    const auto raw=juce::JSON::toString(snapshot(),true);juce::var decoded;
    CHECK(decodeActivityObservation(raw,decoded)&&activityEventWithinHeldWindow(decoded,100,120));
    CHECK(!decodeActivityObservation(raw+" {}",decoded));
    CHECK(!decodeActivityObservation(raw.dropLastCharacters(1),decoded));
    CHECK(!decodeActivityObservation("{\"lastActiveActivityTick\":\"99\",\"lastActiveActivityTick\":\"101\",\"snapshotTick\":\"110\",\"lastActiveActivityBytes\":\"74\",\"lastActiveActivityOutputs\":\"74\"}",decoded));
    CHECK(!decodeActivityObservation("[]",decoded));
    CHECK(!decodeActivityObservation({},decoded));
    CHECK(!decodeActivityObservation("{\"padding\":\""+juce::String::repeatedString("x",4096)+"\"}",decoded));
}
void activityAndDiff(const juce::File& helper){
    std::cout<<"actual_authenticated_activity_and_native_commit_diff"<<std::endl;ServiceFixture fixture(helper);CHECK(fixture.options.fixtureRoot.getChildFile("mcp-queue-observation.enabled").replaceWithText("observe bounded activity"));CHECK(fixture.start());
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));CHECK(testing::readyRackInventory(*p));CHECK(until([&]{p->pollMcp();return p->mcpView().connected;}));
    auto& endpoint=InstrumentMcpTestAccess::endpoint(*p);CHECK(until([&]{return endpoint.observation().available;}));CHECK(endpoint.observation().clients==0);CHECK(endpoint.observation().requests==0);
    Client client;CHECK(client.open(fixture.options.pipeSuffix));CHECK(until([&]{return endpoint.observation().available&&endpoint.observation().clients==1;}));
    struct ActivitySample {int batch=-1;ULONGLONG serial=0,began=0,sent=0,received=0,pollTotal=0,pollMaximum=0,dispatchTotal=0,dispatchMaximum=0;unsigned polls=0;juce::String actualSerial;int status=0;bool statusInt=false,dataObject=false,snapshotString=false;};
    std::array<ActivitySample,11> activitySamples{};std::size_t activityCount=0;int activityBatch=-1;std::array<std::array<ULONGLONG,5>,8> batchTimes{};
    const auto call=[&](const char* name,juce::var arguments){auto& sample=activitySamples[activityCount++];sample.batch=activityBatch;sample.began=GetTickCount64();const auto id=++client.serial;sample.serial=id;CHECK(client.send(MessageKind::mcpRequest,object({{"version",1},{"serial",mcp::ipc::decimal(id)},{"call",object({{"name",name},{"arguments",arguments}})}})));
        sample.sent=GetTickCount64();juce::var result;std::atomic_bool done=false;std::thread receive([&]{result=client.receive();done=true;});CHECK(until([&]{const auto pollAt=GetTickCount64();p->pollMcp();const auto elapsed=GetTickCount64()-pollAt;++sample.polls;sample.pollTotal+=elapsed;sample.pollMaximum=(std::max)(sample.pollMaximum,elapsed);const auto dispatchAt=GetTickCount64();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);const auto dispatchMs=GetTickCount64()-dispatchAt;sample.dispatchTotal+=dispatchMs;sample.dispatchMaximum=(std::max)(sample.dispatchMaximum,dispatchMs);return done.load();}));receive.join();sample.received=GetTickCount64();sample.actualSerial=result["serial"].toString();sample.statusInt=result["status"].isInt();sample.status=int(result["status"]);sample.dataObject=result["data"].getDynamicObject()!=nullptr;sample.snapshotString=result["data"]["snapshot"].isString();CHECK(int(result["status"])==int(mcp::Status::ready));return result["data"];};
    const auto target=object({{"instance_id",p->mcpView().instance.toDashedString()}});const auto state=call("get_state",target);CHECK(state["snapshot"].isString());
    CHECK(until([&]{return endpoint.observation().requests==1;}));CHECK(p->mcpView().lastEdit.isEmpty());
    const auto prepared=call("prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]},{"policy",object({})},{"edits",object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.23}})}}})}}));CHECK(prepared["edit_id"].isString());
    const auto committed=call("apply_edit",object({{"instance_id",target["instance_id"]},{"edit_id",prepared["edit_id"]}}));CHECK(bool(committed["changed"]));CHECK(p->mcpView().lastEdit.contains("global.master_level"));CHECK(p->mcpView().lastEdit.contains(committed["transaction_id"].toString()));
    CHECK(until([&]{return endpoint.observation().requests==3;}));
    {std::vector<std::unique_ptr<Client>> clients;for(int n=0;n<7;++n){auto peer=std::make_unique<Client>();CHECK(peer->open(fixture.options.pipeSuffix));clients.push_back(std::move(peer));}CHECK(until([&]{return endpoint.observation().clients==8;}));}
    CHECK(until([&]{return endpoint.observation().clients==1;}));const auto epoch=endpoint.metadata().epoch;
    bool sawQueued=false,sawSampledActive=false,sawEventActive=false,sawCharged=false;std::uint64_t maximumHeld=0;FixtureSampleSpan churnSamples;std::uint64_t churnValid=0,churnMissing=0;
    for(int batch=0;batch<8;++batch){activityBatch=batch;batchTimes[batch][0]=GetTickCount64();p->pollMcp();batchTimes[batch][1]=GetTickCount64();std::atomic_bool entered=false,release=false;std::thread hold([&]{mcp::detail::EndpointTestAccess::holdHandoff(endpoint,entered,release);});CHECK(until([&]{return entered.load();}));batchTimes[batch][2]=GetTickCount64();const auto began=GetTickCount64();
        for(int n=0;n<64;++n){Client churn;CHECK(churn.open(fixture.options.pipeSuffix));CHECK(churn.list().size()==1);churn.pipe.close();
            const auto sampleBegin=GetTickCount64();const auto text=fixture.options.fixtureRoot.getChildFile("mcp-service-queue.json").loadFileAsString();juce::var queue;const bool decoded=decodeActivityObservation(text,queue);
            const bool valid=decoded&&queue["maxQueuedActivity"].isInt()&&queue["maxActiveActivity"].isInt();const auto sampleEnd=GetTickCount64();churnSamples.add(sampleBegin,sampleEnd);if(valid)++churnValid;else ++churnMissing;
            if(valid){
                CHECK(int(queue["maxQueuedActivity"])<=1&&int(queue["maxActiveActivity"])<=1);sawQueued|=int(queue["maxQueuedActivity"])==1;sawSampledActive|=int(queue["maxActiveActivity"])==1;
                // A completed write may disappear before this file read; require a real charged active state strictly inside this held batch.
                sawEventActive|=activityEventWithinHeldWindow(queue,began,sampleEnd);
                std::uint64_t bytes=0,outputs=0;CHECK(mcp::ipc::decimal(queue["activityBytes"],bytes)&&mcp::ipc::decimal(queue["outputs"],outputs));CHECK(outputs>=bytes);sawCharged|=bytes>0;}}
        Client witness;CHECK(witness.open(fixture.options.pipeSuffix));maximumHeld=(std::max)(maximumHeld,GetTickCount64()-began);batchTimes[batch][3]=GetTickCount64();release=true;hold.join();
        CHECK(call("get_state",target)["snapshot"].isString());CHECK(until([&]{return endpoint.observation().available&&endpoint.observation().clients==2;}));CHECK(endpoint.metadata().epoch==epoch);CHECK(endpoint.connected());witness.pipe.close();CHECK(until([&]{return endpoint.observation().clients==1;}));batchTimes[batch][4]=GetTickCount64();}
    std::cout<<"activity churn=512 batches=8 maximum_held_ms="<<maximumHeld<<" queued="<<sawQueued<<" sampled_active="<<sawSampledActive<<" event_active="<<sawEventActive<<" charged="<<sawCharged<<std::endl;CHECK(maximumHeld<5000);CHECK(sawQueued&&sawEventActive&&sawCharged);
    std::cout<<"MEASURE churn_samples count="<<churnSamples.count<<" valid="<<churnValid<<" missing="<<churnMissing
        <<" first_begin="<<churnSamples.firstBegin<<" first_end="<<churnSamples.firstEnd<<" max_begin="<<churnSamples.maxBegin<<" max_end="<<churnSamples.maxEnd
        <<" last_begin="<<churnSamples.lastBegin<<" last_end="<<churnSamples.lastEnd<<std::endl;
    CHECK(until([&]{return endpoint.observation().clients==1;}));client.pipe.close();CHECK(until([&]{return endpoint.observation().available&&endpoint.observation().clients==0;}));
    fixture.service->kill();CHECK(until([&]{return !endpoint.connected();}));CHECK(!endpoint.observation().available);p.reset();
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
    for(std::size_t i=0;i<activityCount;++i){const auto& v=activitySamples[i];std::cout<<"MEASURE activity_reply index="<<i<<" batch="<<v.batch<<" expected_serial="<<v.serial<<" actual_serial="<<v.actualSerial<<" status_int="<<v.statusInt<<" status="<<v.status<<" data_object="<<v.dataObject<<" snapshot_string="<<v.snapshotString<<" begin_tick="<<v.began<<" sent_tick="<<v.sent<<" received_tick="<<v.received<<" polls="<<v.polls<<" poll_total_ms="<<v.pollTotal<<" poll_max_ms="<<v.pollMaximum<<" dispatch_total_ms="<<v.dispatchTotal<<" dispatch_max_ms="<<v.dispatchMaximum<<std::endl;}
    for(std::size_t i=0;i<batchTimes.size();++i){const auto& v=batchTimes[i];std::cout<<"MEASURE activity_batch batch="<<i<<" begin_tick="<<v[0]<<" prepoll_end_tick="<<v[1]<<" hold_entered_tick="<<v[2]<<" released_tick="<<v[3]<<" end_tick="<<v[4]<<std::endl;}
}
void admittedContext(const juce::File& helper){
    std::cout<<"actual_explicit_group_admission_readonly_MCP_context"<<std::endl;ServiceFixture fixture(helper);CHECK(fixture.start());
    fixture.provider=std::make_unique<Child>();const auto providerRoot=fixture.options.fixtureRoot.getChildFile("provider");
    CHECK(fixture.provider->start(helper,{"--fixture-root",providerRoot.getFullPathName(),"--pipe-suffix",fixture.options.pipeSuffix}));
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    const ClientOptions options{{helper,fixture.options.pipeSuffix,providerRoot},false};auto a=std::make_unique<InstrumentProcessor>(options,testing::isolatedRackRuntime()),b=std::make_unique<InstrumentProcessor>(options,testing::isolatedRackRuntime());CHECK(copperArcInstance(*a)&&copperArcInstance(*b));CHECK(testing::readyRackInventory(*a)&&testing::readyRackInventory(*b));
    const auto pump=[&]{a->pollAi();b->pollAi();a->pollMcp();b->pollMcp();juce::MessageManager::getInstance()->runDispatchLoopUntil(1);};
    CHECK(until([&]{pump();return a->mcpView().connected&&b->mcpView().connected;}));Client client;CHECK(client.open(fixture.options.pipeSuffix));
    const auto read=[&]{const auto id=client.request(a->mcpView().instance);juce::var result;std::atomic_bool done=false;std::thread reader([&]{result=client.receive();done=true;});CHECK(until([&]{pump();return done.load();}));reader.join();CHECK(result["serial"].toString()==mcp::ipc::decimal(id));CHECK(int(result["status"])==int(mcp::Status::ready));return result["data"];};
    CHECK(read()["linked_context"].size()==0);CHECK(!a->groupLinkView().intent&&!b->groupLinkView().intent);
    GroupIntent ai;ai.group=juce::Uuid();ai.member=juce::Uuid();CHECK(setGroupText("Task6 explicitly admitted fixture",ai.name));CHECK(setGroupText("bass",ai.role));CHECK(setGroupText("C",ai.key));CHECK(setGroupText("minor",ai.scale));ai.bpm=120;
    auto bi=ai;bi.member=juce::Uuid();CHECK(a->setGroupIntent(ai)&&b->setGroupIntent(bi));CHECK(read()["linked_context"].size()==0);
    CHECK(a->requestGroupConnection()&&b->requestGroupConnection());const bool groupReady=until([&]{pump();return a->groupWorkflowView().generationReady&&b->groupWorkflowView().generationReady;});CHECK(groupReady);
    if(!groupReady){for(auto* p:{a.get(),b.get()}){const auto view=p->groupWorkflowView();std::cout<<"GROUP_PREREQUISITE connection="<<int(view.connection.phase)<<" error="<<view.connection.errorCode<<" message="<<view.message<<" pending="<<view.operationPending<<" members="<<view.members.size()<<std::endl;}
        DWORD code=STILL_ACTIVE;GetExitCodeProcess(fixture.provider->handle,&code);std::cout<<"OWNED_PROVIDER pid="<<fixture.provider->pid<<" exit="<<code<<std::endl;SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);return;}
    CHECK(a->browseGroups());CHECK(until([&]{pump();return a->groupWorkflowView().members.size()==2&&!a->groupWorkflowView().operationPending;}));
    const std::array<GroupUuid,1> selected{bi.member};CHECK(a->selectGroupPeers(selected));CHECK(until([&]{pump();return a->groupWorkflowView().generationReady;}));
    juce::var state;const bool peerReady=until([&]{state=read();return state["linked_context"]["peers"].size()==1;});CHECK(peerReady);
    if(!peerReady){SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);return;}
    CHECK(state["linked_context"]["peers"][0]["member"].toString()==juce::Uuid(bi.member.getRawData()).toDashedString());
    CHECK(a->groupLinkView().intent==ai&&b->groupLinkView().intent==bi);CHECK(a->groupWorkflowView().selection.peerCount==1);
    CHECK(a->leaveGroupIntent());CHECK(read()["linked_context"].size()==0);CHECK(!a->groupLinkView().intent);CHECK(b->groupLinkView().intent==bi);
    const auto observation=juce::JSON::parse(providerRoot.getChildFile("launch-observation.json").loadFileAsString());CHECK(observation["transportCalls"].isInt()||observation["transportCalls"].isInt64());CHECK(juce::int64(observation["transportCalls"])==0&&juce::int64(observation["browserCalls"])==0);
    CHECK(!providerRoot.getChildFile("OAuthProbe").exists());a.reset();b.reset();client.pipe.close();SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
}
void previewWorkflow(const juce::File& helper){
    std::cout<<"actual_maximum_native_wav_draining_stdio_client"<<std::endl;
    ServiceFixture fixture(helper);CHECK(fixture.start());
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    std::unique_ptr<InstrumentProcessor> peer;auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));CHECK(testing::readyRackInventory(*p));CHECK(p->setAiMusicalContext({"bass","C","minor",30}));
    CHECK(until([&]{p->pollMcp();return p->mcpView().connected;}));
    StdioChild bridge;CHECK(bridge.start(helper,"--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix));int serial=0;
    const auto rpcOn=[&](StdioChild& client,const char* method,juce::var args){const auto began=GetTickCount64();CHECK(client.send(object({{"jsonrpc","2.0"},{"id",++serial},{"method",method},{"params",args}})));juce::var reply;std::atomic_bool done=false;
        const auto sent=GetTickCount64();std::thread reader([&]{reply=client.receive();done=true;});std::uint64_t maximumPoll=0,pollTotal=0;CHECK(until([&]{const auto before=GetTickCount64();p->pollMcp();if(peer)peer->pollMcp();const auto elapsed=GetTickCount64()-before;maximumPoll=(std::max)(maximumPoll,elapsed);pollTotal+=elapsed;return done.load();},5500));reader.join();std::cout<<"MEASURE max_wav_rpc method="<<method<<" expected_id="<<serial<<" actual_id="<<reply["id"].toString()<<" begin_tick="<<began<<" sent_tick="<<sent<<" received_tick="<<GetTickCount64()<<" poll_total_ms="<<pollTotal<<" poll_max_ms="<<maximumPoll<<" status="<<reply["result"]["structuredContent"]["status"].toString()<<std::endl;if(juce::String(method)=="resources/read")std::cout<<"actual resource control maximum_poll_ms="<<maximumPoll<<std::endl;CHECK(int(reply["id"])==serial);return reply;};
    const auto rpc=[&](const char* method,juce::var args){return rpcOn(bridge,method,args);};
    const auto tool=[&](const char* name,juce::var args){auto reply=rpc("tools/call",object({{"name",name},{"arguments",args}}));if(juce::String(name)=="render_preview")std::cout<<"maximum render reply "<<juce::JSON::toString(reply,true)<<std::endl;return reply["result"]["structuredContent"];};
    CHECK(rpc("initialize",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","actual-wav-client"},{"version","1"}})}}))["result"].getDynamicObject());
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));
    const auto target=object({{"instance_id",p->mcpView().instance.toDashedString()}});auto state=tool("get_state",target);CHECK(state["status"].toString()=="ready");
    auto start=tool("render_preview",object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]}}));CHECK(start["status"].toString()=="ready");CHECK(start["job_id"].isString());if(!start["job_id"].isString())return;
    const auto job=object({{"instance_id",target["instance_id"]},{"job_id",start["job_id"]}});juce::var ready;const auto deadline=GetTickCount64()+60000;
    do{ready=tool("get_job",job);if(ready["status"].toString()!="ready"||ready["job_status"].toString()=="ready"||ready["job_status"].toString()=="failed")break;}while(GetTickCount64()<deadline);
    std::cout<<"maximum job "<<juce::JSON::toString(ready,true)<<std::endl;CHECK(ready["status"].toString()=="ready");CHECK(ready["job_status"].toString()=="ready");CHECK(int(ready["frames"])==576000);if(ready["job_status"].toString()!="ready")return;
    const auto parts=juce::StringArray::fromTokens(ready["resource_uri"].toString().substring(14),"/","");CHECK(parts.size()==3);
    if(parts.size()==3){const auto source=fixture.options.fixtureRoot.getChildFile("MCP Previews").getChildFile(parts[1]).getChildFile(parts[0]).getChildFile(parts[2]+".wav");
        const auto path=L"\\\\?\\"+std::wstring(source.getFullPathName().toWideCharPointer());HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);CHECK(h!=INVALID_HANDLE_VALUE);
        if(h!=INVALID_HANDLE_VALUE){juce::MemoryBlock actual(44+576000*8);DWORD count=0;CHECK(ReadFile(h,actual.getData(),DWORD(actual.getSize()),&count,nullptr)&&count==actual.getSize());CloseHandle(h);CHECK(fixture.options.fixtureRoot.getChildFile("maximum-native.wav").replaceWithData(actual.getData(),actual.getSize()));std::cout<<"retained actual native maximum WAV "<<fixture.options.fixtureRoot.getChildFile("maximum-native.wav").getFullPathName()<<std::endl;}
    }
    auto templates=rpc("resources/templates/list",object({}));CHECK(templates["result"]["resourceTemplates"].isArray());CHECK(templates["result"]["resourceTemplates"].size()==0);
    auto list=rpc("resources/list",object({}));CHECK(list["result"]["resources"].isArray());CHECK(list["result"]["resources"].size()==1);CHECK(list["result"]["resources"][0]["uri"]==ready["resource_uri"]);
    auto read=rpc("resources/read",object({{"uri",ready["resource_uri"]}}));const auto contents=read["result"]["contents"];CHECK(contents.isArray()&&contents.size()==1);if(!contents.isArray()){std::cout<<"actual maximum read failure "<<juce::JSON::toString(read,true)<<std::endl;return;}
    CHECK(contents[0]["mimeType"].toString()=="audio/wav");juce::MemoryOutputStream decoded;CHECK(juce::Base64::convertFromBase64(decoded,contents[0]["blob"].toString()));CHECK(decoded.getDataSize()==44+576000*8);
    if(decoded.getDataSize()!=44+576000*8)return;const auto* bytes=static_cast<const std::byte*>(decoded.getData());CHECK(std::memcmp(bytes,"RIFF",4)==0&&std::memcmp(bytes+8,"WAVE",4)==0);
    const auto u16=[&](int at){return juce::ByteOrder::littleEndianShort(bytes+at);};const auto u32=[&](int at){return juce::ByteOrder::littleEndianInt(bytes+at);};
    CHECK(u16(20)==3&&u16(22)==2&&u32(24)==48000&&u16(34)==32&&u32(40)==576000*8);
    auto captured=std::make_unique<ProjectDocument>();std::uint64_t revision=0;CHECK(p->tryProjectSnapshot(*captured,revision));auto request=std::make_unique<ai::GenerationRequest>();request->source=*captured;request->capturedBpm=30;
    auto native=testing::fixtureValue([&]{return ai::renderPreview(*captured,ai::previewContextFor(*request));});CHECK(native->status==ai::RenderStatus::ready);CHECK(native->audio.left.size()==576000);
    bool exact=true,finite=true;for(std::size_t n=0;n<native->audio.left.size();++n){float left=0,right=0;std::memcpy(&left,bytes+44+n*8,4);std::memcpy(&right,bytes+48+n*8,4);finite&=std::isfinite(left)&&std::isfinite(right);exact&=std::bit_cast<std::uint32_t>(left)==std::bit_cast<std::uint32_t>(native->audio.left[n])&&std::bit_cast<std::uint32_t>(right)==std::bit_cast<std::uint32_t>(native->audio.right[n]);}CHECK(exact&&finite);
    CHECK(fixture.options.fixtureRoot.getChildFile("maximum-client.wav").replaceWithData(decoded.getData(),decoded.getDataSize()));
    std::cout<<"actual WAV bytes="<<decoded.getDataSize()<<" float_samples="<<576000*2<<" sha256="<<juce::SHA256(decoded.getData(),decoded.getDataSize()).toHexString()<<" exact="<<exact<<" finite="<<finite<<std::endl;
    peer=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(until([&]{p->pollMcp();peer->pollMcp();return peer->mcpView().connected&&fixture.list().size()==2;}));
    auto page=rpc("resources/list",object({}));CHECK(page["result"]["resources"].isArray());CHECK(page["result"]["nextCursor"].isString());const auto cursor=page["result"]["nextCursor"];int ownResources=page["result"]["resources"].size();
    StdioChild foreign;CHECK(foreign.start(helper,"--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix));
    CHECK(rpcOn(foreign,"initialize",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","foreign-cursor"},{"version","1"}})}}))["result"].getDynamicObject());CHECK(foreign.send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));
    CHECK(rpcOn(foreign,"resources/list",object({{"cursor",cursor}}))["error"].getDynamicObject());auto second=rpc("resources/list",object({{"cursor",cursor}}));CHECK(second["result"]["resources"].isArray());CHECK(!second["result"].hasProperty("nextCursor"));ownResources+=second["result"]["resources"].size();CHECK(ownResources==1);
    page=rpc("resources/list",object({}));CHECK(page["result"]["nextCursor"].isString());peer->setMcpEnabled(false);CHECK(until([&]{peer->pollMcp();const auto entries=fixture.list();for(int n=0;n<entries.size();++n)if(entries[n]["instance_id"].toString()==peer->mcpView().instance.toDashedString())return !bool(entries[n]["enabled"]);return false;}));CHECK(rpc("resources/list",object({{"cursor",page["result"]["nextCursor"]}}))["error"].getDynamicObject());
    CHECK(tool("play_preview",job)["status"].toString()=="audio_unavailable");CHECK(tool("stop_preview",job)["status"].toString()=="ready");CHECK(tool("cancel_job",job)["status"].toString()=="ready");
}
bool resourceReplyHasWavMime(const juce::var& reply){const auto contents=reply["result"]["contents"];return contents.isArray()&&contents.size()>0&&contents[0]["mimeType"].toString()=="audio/wav";}
void resourceReplyShape(){
    const auto response=[](juce::var contents){return object({{"result",object({{"contents",contents}})}});};
    const auto entry=[](const char* mime){return object({{"mimeType",mime}});};
    CHECK(resourceReplyHasWavMime(response(juce::Array<juce::var>{entry("audio/wav")})));
    CHECK(!resourceReplyHasWavMime(response(juce::Array<juce::var>{entry("text/plain")})));
    CHECK(!resourceReplyHasWavMime(response(juce::Array<juce::var>{})));
    CHECK(!resourceReplyHasWavMime(response("not an array")));
    CHECK(!resourceReplyHasWavMime(juce::var()));
}
void previewDelivery(const juce::File& helper,int first=0,int last=8){
    struct DeliverySample {juce::String fixture,raw;juce::var observation;DWORD pid=0,available=0,peekError=0,childWait=0,exitCode=0;BOOL peeked=FALSE,exitQuery=FALSE;int readId=0,predicates=0,reads=0;std::size_t pending=0;ULONGLONG sendBegin=0,sendEnd=0,predicateBegin=0,lastPredicate=0,lastObservation=0,peekTick=0;
        FixtureSampleSpan spans;std::uint64_t valid=0,missing=0;bool firstSeen=false,firstValid=false,firstActive=false,lastValid=false,lastActive=false;std::size_t firstPending=0,lastPending=0;
        void record(ULONGLONG begin,ULONGLONG end,std::size_t pendingValue,bool observed,bool validValue,bool activeValue) noexcept {
            spans.add(begin,end);if(observed){if(validValue)++valid;else ++missing;}if(!firstSeen){firstSeen=true;firstPending=pendingValue;firstValid=validValue;firstActive=activeValue;}
            lastPending=pendingValue;lastValid=validValue;lastActive=activeValue;
        }
    };
    std::array<DeliverySample,8> deliverySamples{};
    for(int mode=first;mode<last;++mode){auto& measured=deliverySamples[mode];std::cout<<"actual_resource_final_delivery mode="<<mode<<std::endl;
        ServiceFixture fixture(helper);measured.fixture=fixture.options.fixtureRoot.getFullPathName();CHECK(fixture.start());
        SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
        auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));CHECK(testing::readyRackInventory(*p));CHECK(p->setAiMusicalContext({"bass","C","minor",30}));p->prepareToPlay(48000,128);
        juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;
        const auto pump=[&]{p->processBlock(audio,midi);p->pollMcp();};
        CHECK(until([&]{pump();return p->mcpView().connected;}));
        StdioChild bridge;const auto args="--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix;CHECK(bridge.start(helper,args,fixture.options.fixtureRoot.getChildFile("delivery-bridge-stderr.log")));bridge.diagnostics=false;measured.pid=bridge.pid;int serial=0;
        const auto rpcOn=[&](StdioChild& peer,const char* method,juce::var args){const auto id=++serial;CHECK(peer.send(object({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",args}})));juce::var reply;std::atomic_bool done=false;std::thread read([&]{reply=peer.receive();done=true;});CHECK(until([&]{pump();return done.load();},5500));read.join();CHECK(int(reply["id"])==id);return reply;};
        const auto init=[&](StdioChild& peer){CHECK(rpcOn(peer,"initialize",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","delivery"},{"version","1"}})}}))["result"].getDynamicObject());CHECK(peer.send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));};init(bridge);
        const auto toolOn=[&](StdioChild& peer,const char* name,juce::var args){return rpcOn(peer,"tools/call",object({{"name",name},{"arguments",args}}))["result"]["structuredContent"];};
        const auto target=object({{"instance_id",p->mcpView().instance.toDashedString()}});const auto state=toolOn(bridge,"get_state",target);CHECK(state["status"].toString()=="ready");
        const auto started=toolOn(bridge,"render_preview",object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]}}));CHECK(started["status"].toString()=="ready");if(!started["job_id"].isString())continue;
        const auto job=object({{"instance_id",target["instance_id"]},{"job_id",started["job_id"]}});juce::var ready;const auto end=GetTickCount64()+60000;
        do{ready=toolOn(bridge,"get_job",job);if(ready["job_status"].toString()!="running")break;}while(GetTickCount64()<end);
        CHECK(ready["status"].toString()=="ready"&&ready["job_status"].toString()=="ready");if(ready["job_status"].toString()!="ready")continue;
        CHECK(int(ready["frames"])==576000);CHECK(toolOn(bridge,"play_preview",job)["status"].toString()=="ready");
        const auto view=p->mcpView();const auto parts=juce::StringArray::fromTokens(ready["resource_uri"].toString().substring(14),"/","");
        const auto file=fixture.options.fixtureRoot.getChildFile("MCP Previews").getChildFile(parts[1]).getChildFile(parts[0]).getChildFile(parts[2]+".wav");
        const auto exists=[&]{return GetFileAttributesW((L"\\\\?\\"+std::wstring(file.getFullPathName().toWideCharPointer())).c_str())!=INVALID_FILE_ATTRIBUTES;};
        const auto observation=fixture.options.fixtureRoot.getChildFile("mcp-bridge-"+juce::String(int(bridge.pid))+".json");
        const auto readId=++serial;measured.readId=readId;measured.sendBegin=GetTickCount64();CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",readId},{"method","resources/read"},{"params",object({{"uri",ready["resource_uri"]}})}})));
        measured.sendEnd=GetTickCount64();auto& endpoint=InstrumentMcpTestAccess::endpoint(*p);
        measured.predicateBegin=GetTickCount64();CHECK(until([&]{const auto begin=GetTickCount64();pump();++measured.predicates;measured.lastPredicate=GetTickCount64();measured.pending=mcp::detail::EndpointTestAccess::pendingResourceReads(endpoint);if(measured.pending!=1){measured.record(begin,GetTickCount64(),measured.pending,false,false,false);return false;}measured.lastObservation=GetTickCount64();measured.raw=observation.loadFileAsString();measured.observation=juce::JSON::parse(measured.raw);++measured.reads;const bool active=bool(measured.observation["activeResource"]);measured.record(begin,GetTickCount64(),measured.pending,true,measured.observation.getDynamicObject()!=nullptr,active);return active;},4000));
        DWORD available=0;measured.peekTick=GetTickCount64();measured.peeked=PeekNamedPipe(bridge.output,nullptr,0,nullptr,&available,nullptr);measured.peekError=measured.peeked?ERROR_SUCCESS:GetLastError();measured.available=available;measured.childWait=WaitForSingleObject(bridge.process,0);measured.exitQuery=GetExitCodeProcess(bridge.process,&measured.exitCode);CHECK(measured.peeked&&available>0);CHECK(exists());
        if(mode==0){
            // One blocked read remains admitted while seven real job lookups fill the remaining normal slots.
            for(int n=0;n<8;++n)CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",++serial},{"method","tools/call"},{"params",object({{"name","get_job"},{"arguments",job}})}})));
            CHECK(until([&]{return mcp::detail::EndpointTestAccess::commandPending(endpoint,mcp::Tool::getJob)&&juce::JSON::parse(observation.loadFileAsString())["queuedBytes"].toString().getLargeIntValue()>0;}));
            CHECK(mcp::detail::EndpointTestAccess::pendingResourceReads(endpoint)==1);
            juce::var received;std::atomic_bool done=false;std::thread reader([&]{received=bridge.receive();done=true;});CHECK(until([&]{pump();return done.load();}));reader.join();CHECK(int(received["id"])==readId);CHECK(resourceReplyHasWavMime(received));
            int capacity=0,normal=0;for(int n=0;n<8;++n){std::atomic_bool got=false;std::thread next([&]{received=bridge.receive();got=true;});CHECK(until([&]{pump();return got.load();}));next.join();const auto status=received["result"]["structuredContent"]["status"].toString();std::cout<<"admission terminal status="<<status<<" id="<<received["id"].toString()<<std::endl;capacity+=status=="capacity";normal+=status=="ready";}
            std::cout<<"retained read normal slots: capacity="<<capacity<<" ready="<<normal<<std::endl;CHECK(capacity==1&&normal==7);CHECK(until([&]{pump();return mcp::detail::EndpointTestAccess::pendingResourceReads(endpoint)==0;}));
            CHECK(toolOn(bridge,"cancel_job",job)["status"].toString()=="ready");
        }else if(mode==1){
            StdioChild foreign;CHECK(foreign.start(helper,args));init(foreign);CHECK(toolOn(foreign,"cancel_job",job)["status"].toString()=="unauthorized");
            CHECK(toolOn(foreign,"cancel_job",object({{"instance_id",target["instance_id"]},{"job_id",juce::Uuid().toDashedString()}}))["status"].toString()=="stale");CHECK(mcp::detail::EndpointTestAccess::pendingResourceReads(endpoint)==1);CHECK(WaitForSingleObject(bridge.process,0)==WAIT_TIMEOUT);
            CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",++serial},{"method","tools/call"},{"params",object({{"name","cancel_job"},{"arguments",job}})}})));
            CHECK(until([&]{return mcp::detail::EndpointTestAccess::commandPending(endpoint,mcp::Tool::cancelJob);}));pump();
        }else if(mode==2)p->setMcpEnabled(false);
        else if(mode==3)CHECK(copperArcInstance(*p));
        else if(mode==4){CloseHandle(bridge.input);bridge.input=nullptr;}
        else if(mode==5)fixture.service->kill();
        else if(mode==6)CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",object({{"requestId",readId}})}})));
        if(mode!=0)CHECK(until([&]{pump();return WaitForSingleObject(bridge.process,0)==WAIT_OBJECT_0;},7000));
        CHECK(until([&]{pump();return mcp::detail::EndpointTestAccess::pendingResourceReads(endpoint)==0&&!exists();},7000));
        if(mode!=2&&mode!=3&&mode!=5&&mode!=7)CHECK(p->mcpView().epoch==view.epoch);
        midi.addEvent(juce::MidiMessage::noteOn(1,60,.5f),0);for(int n=0;n<8;++n){p->processBlock(audio,midi);midi.clear();}CHECK(audio.getMagnitude(0,128)>0);CHECK(!fixture.options.fixtureRoot.getChildFile("OAuthProbe").exists());
    }
    for(int mode=first;mode<last;++mode){const auto& v=deliverySamples[mode];const auto& t=v.spans;
        std::cout<<"MEASURE delivery_samples mode="<<mode<<" count="<<t.count<<" valid="<<v.valid<<" missing="<<v.missing
            <<" first_begin="<<t.firstBegin<<" first_end="<<t.firstEnd<<" max_begin="<<t.maxBegin<<" max_end="<<t.maxEnd<<" last_begin="<<t.lastBegin<<" last_end="<<t.lastEnd
            <<" first_pending="<<v.firstPending<<" first_valid="<<v.firstValid<<" first_active="<<v.firstActive<<" last_pending="<<v.lastPending<<" last_valid="<<v.lastValid<<" last_active="<<v.lastActive<<std::endl;
        std::cout<<"MEASURE delivery mode="<<mode<<" fixture="<<v.fixture<<" bridge_pid="<<v.pid<<" read_id="<<v.readId<<" send_begin="<<v.sendBegin<<" send_end="<<v.sendEnd<<" predicate_begin="<<v.predicateBegin<<" last_predicate="<<v.lastPredicate<<" pending="<<v.pending<<" predicates="<<v.predicates<<" observation_tick="<<v.lastObservation<<" observations="<<v.reads<<" valid="<<(v.observation.getDynamicObject()!=nullptr)<<" active_resource="<<bool(v.observation["activeResource"])<<" raw_bytes="<<v.raw.getNumBytesAsUTF8()<<" peek_tick="<<v.peekTick<<" peek_ok="<<v.peeked<<" available="<<v.available<<" peek_error="<<v.peekError<<" child_wait="<<v.childWait<<" exit_query="<<v.exitQuery<<" exit="<<v.exitCode<<" raw="<<v.raw.substring(0,4096).replaceCharacters("\r\n","  ")<<std::endl;}
}

void pendingPriorityWrite(const juce::File& helper){
    for(int mode=0;mode<4;++mode){std::cout<<"actual_pending_priority_write mode="<<mode<<std::endl;ServiceFixture fixture(helper);CHECK(fixture.options.fixtureRoot.getChildFile("mcp-queue-observation.enabled").replaceWithText("observe only"));CHECK(fixture.start());
        struct Raw {HANDLE pipe=INVALID_HANDLE_VALUE;juce::Uuid session;~Raw(){close();}void close(){if(pipe!=INVALID_HANDLE_VALUE){CloseHandle(pipe);pipe=INVALID_HANDLE_VALUE;}}
            bool read(void* destination,std::size_t bytes){auto* out=static_cast<std::byte*>(destination);const auto end=GetTickCount64()+1500;while(bytes&&GetTickCount64()<end){DWORD available=0;if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))return false;if(!available){std::this_thread::yield();continue;}DWORD count=0;const auto size=DWORD((std::min)(bytes,std::size_t(available)));if(!ReadFile(pipe,out,size,&count,nullptr)||!count)return false;out+=count;bytes-=count;}return !bytes;}
            bool send(MessageKind kind,const juce::var& value){juce::MemoryBlock bytes;if(!encodeFrame(frame(kind,session,value),bytes))return false;DWORD count=0;return WriteFile(pipe,bytes.getData(),DWORD(bytes.getSize()),&count,nullptr)&&count==bytes.getSize();}
        } raw;
        CHECK(until([&]{raw.pipe=CreateFileW(currentUserPipeName(fixture.options.pipeSuffix,PipeDomain::mcp).toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);return raw.pipe!=INVALID_HANDLE_VALUE;}));const auto instance=juce::Uuid(),epoch=juce::Uuid();
        CHECK(raw.send(MessageKind::mcpHello,object({{"version",1},{"role","endpoint"},{"metadata",object({{"instance_id",instance.toDashedString()},{"epoch",epoch.toDashedString()},{"label","actual pipe pressure"},{"version","test"},{"enabled",true},{"capabilities",object({})}})}})));
        std::array<std::byte,wireHeaderBytes> header{};CHECK(raw.read(header.data(),header.size()));FrameHeader hello;CHECK(decodeFrameHeader(header,hello));raw.session=hello.session;juce::MemoryBlock helloBody(hello.payloadBytes);CHECK(raw.read(helloBody.getData(),helloBody.getSize()));juce::var accepted;CHECK(mcp::ipc::parse(helloBody.getData(),helloBody.getSize(),65536,accepted));CHECK(int(accepted["status"])==0);
        Client caller;CHECK(caller.open(fixture.options.pipeSuffix));Client witness;if(mode==2)CHECK(witness.open(fixture.options.pipeSuffix));const auto normal=++caller.serial;CHECK(caller.send(MessageKind::mcpRequest,object({{"version",1},{"serial",mcp::ipc::decimal(normal)},{"call",object({{"name","prepare_edit"},{"arguments",object({{"instance_id",instance.toDashedString()},{"snapshot",juce::Uuid().toDashedString()},{"policy",object({})},{"edits",object({{"explanation",juce::String(std::string(260000,'x'))}})}})}})}})));
        const auto urgent=[&](Client& peer){const auto serial=++peer.serial;CHECK(peer.send(MessageKind::mcpRequest,object({{"version",1},{"serial",mcp::ipc::decimal(serial)},{"call",object({{"name","stop_preview"},{"arguments",object({{"instance_id",instance.toDashedString()},{"job_id",juce::Uuid().toDashedString()}})}})}})));return serial;};const auto first=urgent(caller);
        FrameHeader normalHeader;for(unsigned events=0;events<8;++events){CHECK(raw.read(header.data(),header.size()));CHECK(decodeFrameHeader(header,normalHeader));if(normalHeader.kind!=MessageKind::mcpEvent)break;CHECK(normalHeader.payloadBytes<=256);juce::MemoryBlock activity(normalHeader.payloadBytes);CHECK(raw.read(activity.getData(),activity.getSize()));juce::var value;CHECK(mcp::ipc::parse(activity.getData(),activity.getSize(),256,value)&&value["event"].toString()=="activity");}CHECK(normalHeader.kind==MessageKind::mcpRequest);DWORD outQuota=0,inQuota=0;CHECK(GetNamedPipeInfo(raw.pipe,nullptr,&outQuota,&inQuota,nullptr));const auto quota=(std::max)(outQuota,inQuota);CHECK(quota>0&&normalHeader.payloadBytes>quota);juce::MemoryBlock prefix(normalHeader.payloadBytes-quota);CHECK(raw.read(prefix.getData(),prefix.getSize()));std::size_t remaining=quota;
        const auto observation=fixture.options.fixtureRoot.getChildFile("mcp-service-queue.json");MeasuredSnapshot sample;const auto view=[&]{return sample.read(observation);};bool pending=until([&]{return view()["pendingPriorityBytes"].toString().getLargeIntValue()>0;},100);
        while(!pending&&remaining){const auto stamp=observation.getLastModificationTime();std::array<std::byte,1024> block;const auto n=(std::min)(remaining,block.size());CHECK(raw.read(block.data(),n));remaining-=n;CHECK(until([&]{return observation.getLastModificationTime()!=stamp;},200));pending=view()["pendingPriorityBytes"].toString().getLargeIntValue()>0;}
        std::cout<<"pending precondition="<<pending<<" normal_tail_bytes="<<remaining<<" pipe_quota="<<quota<<" observed="<<juce::JSON::toString(view(),true)<<std::endl;CHECK(pending);if(!pending)continue;
        std::cout<<"MEASURE pending mode="<<mode<<" fixture="<<fixture.options.fixtureRoot.getFullPathName()<<" stage=before_terminal tick_ms="<<GetTickCount64()<<std::endl;sample.report("pending_precondition");
        juce::var retained;const auto terminal=[&]{auto v=view();if(!v.getDynamicObject()||int(v["orphanPriority"])!=1||(mode==1&&int(v["urgentRoutes"])!=0))return false;retained=std::move(v);sample.report("terminal_predicate_capture");return true;};
        if(mode==1){CHECK(until(terminal,2500));}
        else if(mode==2){caller.pipe.close();CHECK(until(terminal));sample.report("orphan_before_witness");CHECK(witness.list().size()==1);std::cout<<"MEASURE pending stage=after_witness tick_ms="<<GetTickCount64()<<std::endl;}
        else{CHECK(caller.send(MessageKind::mcpEvent,object({{"version",1},{"event","cancel"},{"serial",mcp::ipc::decimal(first)}})));CHECK(until(terminal));}
        sample.report("terminal_bound_sample");CHECK(retained.getDynamicObject()!=nullptr);std::cout<<"terminal route while pending="<<juce::JSON::toString(retained,true)<<std::endl;const auto payload=retained["pendingPriorityBytes"].toString().getLargeIntValue();CHECK(payload>0);CHECK(retained["pendingPriorityCharge"].toString().getLargeIntValue()==payload);CHECK(retained["outputs"].toString().getLargeIntValue()>=payload);CHECK(retained["inputs"].toString().getLargeIntValue()<=8*1024*1024&&retained["outputs"].toString().getLargeIntValue()<=16*1024*1024);
        std::vector<std::unique_ptr<Client>> others;if(mode==0){urgent(caller);const auto refused=urgent(caller);const auto response=caller.receive(300);CHECK(response["serial"].toString()==mcp::ipc::decimal(refused)&&int(response["status"])==int(mcp::Status::capacity));
            for(int n=0;n<7;++n){auto peer=std::make_unique<Client>();CHECK(peer->open(fixture.options.pipeSuffix));urgent(*peer);urgent(*peer);others.push_back(std::move(peer));}CHECK(others.back()->list().size()==1);CHECK(until([&]{auto observed=view();if(!observed.getDynamicObject())return false;retained=std::move(observed);return int(retained["urgentRoutes"])>=15;}));std::cout<<"actual replacement admission snapshot="<<juce::JSON::toString(retained,true)<<std::endl;CHECK(int(retained["urgentRoutes"])+int(retained["orphanPriority"])==16);CHECK(int(retained["urgentRoutes"])==15);}
        if(mode==3){juce::MemoryBlock tail(remaining);CHECK(raw.read(tail.getData(),tail.getSize()));CHECK(until([&]{DWORD available=0;if(!PeekNamedPipe(raw.pipe,nullptr,0,nullptr,&available,nullptr))return false;if(available){juce::MemoryBlock bytes(available);if(!raw.read(bytes.getData(),bytes.getSize()))return false;}return int(view()["orphanPriority"])==0&&view()["pendingPriorityCharge"].toString()=="0";}));CHECK(caller.send(MessageKind::mcpEvent,object({{"version",1},{"event","cancel"},{"serial",mcp::ipc::decimal(normal)}})));}
        raw.close();CHECK(until([&]{const auto v=view();return v["inputs"].toString()=="0"&&v["outputs"].toString()=="0"&&int(v["orphanPriority"])==0&&int(v["urgentRoutes"])==0;}));const auto clean=view();std::cout<<"actual pipe retirement="<<juce::JSON::toString(clean,true)<<std::endl;if(mode==2)CHECK(witness.list().size()==0);
    }
}
void priorityPressure(const juce::File& helper){
    std::cout<<"actual_native_owner_controls_under_full_normal_input_and_stdio_output"<<std::endl;
    const auto measurementStart=GetTickCount64();const auto stage=[&](const char* name){const auto now=GetTickCount64();std::cout<<"MEASURE pressure stage="<<name<<" tick_ms="<<now<<" elapsed_ms="<<now-measurementStart<<std::endl;};
    std::array<std::array<ULONGLONG,2>,32> normalSendTimes{};std::array<std::array<std::size_t,4>,3> admissionUsage{};std::array<ULONGLONG,3> admissionTicks{};
    const auto reportAdmissions=[&]{for(std::size_t i=0;i<normalSendTimes.size();++i)std::cout<<"MEASURE normal_send index="<<i<<" begin_tick="<<normalSendTimes[i][0]<<" end_tick="<<normalSendTimes[i][1]<<std::endl;for(std::size_t i=0;i<admissionUsage.size();++i){const auto& v=admissionUsage[i];std::cout<<"MEASURE admission index="<<i<<" tick_ms="<<admissionTicks[i]<<" normal="<<v[0]<<" urgent="<<v[1]<<" input="<<v[2]<<" output="<<v[3]<<std::endl;}};
    ServiceFixture fixture(helper);CHECK(fixture.start());SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(copperArcInstance(*p));CHECK(testing::readyRackInventory(*p));p->prepareToPlay(48000,128);juce::AudioBuffer<float> audio(2,128);juce::MidiBuffer midi;
    const auto pump=[&]{p->processBlock(audio,midi);p->pollMcp();};CHECK(until([&]{pump();return p->mcpView().connected;}));
    StdioChild bridge;CHECK(bridge.start(helper,"--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix,fixture.options.fixtureRoot.getChildFile("pressure-bridge-stderr.log")));int serial=0;std::cout<<"MEASURE pressure fixture="<<fixture.options.fixtureRoot.getFullPathName()<<" bridge_pid="<<bridge.pid<<std::endl;
    const auto rpc=[&](const char* method,juce::var args){const auto id=++serial;CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",args}})));juce::var reply;std::atomic_bool done=false;std::thread reader([&]{reply=bridge.receive();done=true;});CHECK(until([&]{pump();return done.load();},5500));reader.join();std::cout<<"MEASURE rpc method="<<method<<" expected_id="<<id<<" actual_id="<<reply["id"].toString()<<" tick_ms="<<GetTickCount64()<<std::endl;CHECK(int(reply["id"])==id);return reply;};
    const auto tool=[&](const char* name,juce::var args){return rpc("tools/call",object({{"name",name},{"arguments",args}}))["result"]["structuredContent"];};
    CHECK(rpc("initialize",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","priority-pressure"},{"version","1"}})}}))["result"].getDynamicObject());CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));
    const auto target=object({{"instance_id",p->mcpView().instance.toDashedString()}});const auto state=tool("get_state",target);CHECK(state["status"].toString()=="ready");const auto started=tool("render_preview",object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]}}));CHECK(started["status"].toString()=="ready");
    const auto job=object({{"instance_id",target["instance_id"]},{"job_id",started["job_id"]}});juce::var ready;const auto end=GetTickCount64()+60000;do{ready=tool("get_job",job);if(ready["job_status"].toString()!="running")break;}while(GetTickCount64()<end);CHECK(ready["job_status"].toString()=="ready");if(ready["job_status"].toString()!="ready"){reportAdmissions();return;}CHECK(tool("play_preview",job)["status"].toString()=="ready");
    Client pressureEndpoint;const auto pressureId=juce::Uuid(),pressureEpoch=juce::Uuid();CHECK(until([&]{return pressureEndpoint.pipe.connect(fixture.options.pipeSuffix,PipeDomain::mcp)==PipeStatus::ready;}));CHECK(pressureEndpoint.send(MessageKind::mcpHello,object({{"version",1},{"role","endpoint"},{"metadata",object({{"instance_id",pressureId.toDashedString()},{"epoch",pressureEpoch.toDashedString()},{"label","queue pressure only"},{"version","test"},{"enabled",true},{"capabilities",object({})}})}})));CHECK(int(pressureEndpoint.receive()["status"])==0);
    std::vector<std::unique_ptr<Client>> callers;for(int n=0;n<7;++n){auto client=std::make_unique<Client>();CHECK(client->open(fixture.options.pipeSuffix));callers.push_back(std::move(client));}
    struct Pending {Client* client;Frame wire;};std::vector<Pending> normal;std::array<std::set<std::uint64_t>,7> normalSerials;
    for(int n=0;n<32;++n){auto& client=*callers[n/8];const auto number=++client.serial;const auto value=object({{"version",1},{"serial",mcp::ipc::decimal(number)},{"call",object({{"name","prepare_edit"},{"arguments",object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]},{"policy",object({})},{"edits",object({{"explanation",""}})}})}})}});
        const auto missing=260992-juce::JSON::toString(value,true).getNumBytesAsUTF8();juce::String text(std::string(missing/2,'\n'));if(missing%2)text+="x";value["call"]["arguments"]["edits"].getDynamicObject()->setProperty("explanation",text);CHECK(juce::JSON::toString(value,true).getNumBytesAsUTF8()==260992);auto wire=frame(MessageKind::mcpRequest,client.session,value);CHECK(wire.payload.getSize()==260992);normalSerials[n/8].insert(number);normal.push_back({&client,std::move(wire)});}
    // Frame-count pressure is separate from the unchanged maximum-byte fixtures.
    constexpr int retainedFrames=8,pressureFrames=9;const auto payload=juce::String(std::string(8192,'\n'));const auto data=object({{"payload",payload}});const auto encoded=juce::JSON::toString(data,true);std::array<int,pressureFrames> pressureRpcIds{};std::array<juce::var,pressureFrames> pressureRequests;std::array<std::size_t,pressureFrames> pressureBytes{};std::size_t readyBytes=0,allReadyBytes=0;
    for(int n=0;n<pressureFrames;++n){pressureRpcIds[n]=++serial;pressureRequests[n]=object({{"jsonrpc","2.0"},{"id",pressureRpcIds[n]},{"method","tools/call"},{"params",object({{"name","get_state"},{"arguments",object({{"instance_id",pressureId.toDashedString()}})}})}});const auto reply=mcp::rpcSuccess(pressureRpcIds[n],mcp::toolResult({mcp::Status::ready,data}));pressureBytes[n]=reply.getNumBytesAsUTF8()+1;CHECK(pressureBytes[n]<=mcp::maximumResultBytes+1);allReadyBytes+=pressureBytes[n];if(n<retainedFrames)readyBytes+=pressureBytes[n];}
    DWORD outQuota=0,inQuota=0;CHECK(GetNamedPipeInfo(bridge.output,nullptr,&outQuota,&inQuota,nullptr));const auto pipeQuota=(std::max)(outQuota,inQuota);CHECK(pipeQuota>0);for(const auto bytes:pressureBytes)CHECK(bytes>pipeQuota);
    constexpr std::size_t normalOutputCap=16*1024*1024-32768;const auto capacityText=mcp::rpcSuccess(pressureRpcIds[retainedFrames],mcp::toolResult({mcp::Status::capacity,{}}));const std::size_t capacityBytes=capacityText.getNumBytesAsUTF8()+1,expectedOutput=readyBytes+capacityBytes;CHECK(allReadyBytes<=normalOutputCap);CHECK(expectedOutput<=normalOutputCap);CHECK(capacityBytes<=4096);CHECK(normalOutputCap+capacityBytes<=16*1024*1024-4096);CHECK(normalOutputCap+capacityBytes+2*4096<=16*1024*1024);std::cout<<"MEASURE frame_quota expected_successes="<<retainedFrames<<" attempts="<<pressureFrames<<" first_frame="<<pressureBytes[0]<<" pipe_quota="<<pipeQuota<<" ready_bytes="<<readyBytes<<" capacity="<<capacityBytes<<" total="<<expectedOutput<<" all_ready="<<allReadyBytes<<" normal_cap="<<normalOutputCap<<std::endl;
    const auto observation=fixture.options.fixtureRoot.getChildFile("mcp-bridge-"+juce::String(int(bridge.pid))+".json");MeasuredSnapshot outputSample;juce::var initial;CHECK(until([&]{auto v=outputSample.read(observation);if(!v.getDynamicObject()||v["queuedBytes"].toString()!="0"||v["activeBytes"].toString()!="0"||v["refusedResults"].toString()!="0")return false;initial=std::move(v);return true;}));CHECK(initial.getDynamicObject()!=nullptr&&initial["queuedBytes"].toString()=="0"&&initial["activeBytes"].toString()=="0");const auto before=initial["toolResults"].toString().getLargeIntValue();CHECK(initial["refusedResults"].toString()=="0");juce::var pressureObserved;std::size_t admittedBytes=0;stage("frame_replies_begin");
    for(int n=0;n<pressureFrames;++n){CHECK(bridge.send(pressureRequests[n]));const auto request=pressureEndpoint.receive();const auto number=request["serial"].toString();std::uint64_t route=0;CHECK(mcp::ipc::decimal(request["serial"],route)&&route>0&&request["call"]["name"].toString()=="get_state"&&request["call"]["arguments"]["instance_id"].toString()==pressureId.toDashedString());const auto response="{\"version\":1,\"serial\":\""+number+"\",\"status\":0,\"data\":"+encoded+"}";CHECK(pressureEndpoint.pipe.beginWrite({MessageKind::mcpReply,pressureEndpoint.session,juce::MemoryBlock(response.toRawUTF8(),response.getNumBytesAsUTF8())})==PipeStatus::ready);CHECK(until([&]{return pressureEndpoint.pipe.pollWrite()==PipeStatus::complete;}));admittedBytes+=n<retainedFrames?pressureBytes[n]:capacityBytes;CHECK(until([&]{auto v=outputSample.read(observation);if(!v.getDynamicObject()||v["toolResults"].toString().getLargeIntValue()!=before+n+1||int(v["refusedResults"].toString().getIntValue())!=(n<retainedFrames?0:1)||bool(v["activeResource"])||int(v["queuedResources"])!=0||v["activeBytes"].toString().getLargeIntValue()!=pressureBytes[0]||v["queuedBytes"].toString().getLargeIntValue()+v["activeBytes"].toString().getLargeIntValue()!=admittedBytes)return false;pressureObserved=std::move(v);return true;}));std::cout<<"MEASURE fake_tool index="<<n<<" rpc_id="<<pressureRpcIds[n]<<" endpoint_serial="<<number<<" tick_ms="<<GetTickCount64()<<std::endl;outputSample.report("frame_admission");if(n==0)stage("first_blocked_stdout_observed");}
    CHECK(pressureObserved.getDynamicObject()!=nullptr);CHECK(pressureObserved["toolResults"].toString().getLargeIntValue()==before+pressureFrames&&pressureObserved["refusedResults"].toString()=="1");CHECK(pressureObserved["queuedBytes"].toString().getLargeIntValue()+pressureObserved["activeBytes"].toString().getLargeIntValue()==expectedOutput);
    stage("normal_send_batch_begin");std::size_t sendIndex=0;for(const auto& command:normal){auto& times=normalSendTimes[sendIndex++];times[0]=GetTickCount64();CHECK(command.client->pipe.beginWrite(command.wire)==PipeStatus::ready);CHECK(until([&]{return command.client->pipe.pollWrite()!=PipeStatus::pending;}));times[1]=GetTickCount64();}stage("normal_send_batch_end");auto& endpoint=InstrumentMcpTestAccess::endpoint(*p);std::array<std::size_t,4> usage{};const bool full=until([&]{usage=mcp::detail::EndpointTestAccess::queueUsage(endpoint);admissionUsage[0]=usage;admissionTicks[0]=GetTickCount64();return usage[0]==32;},1500);CHECK(full);CHECK(usage[2]>8*1024*1024-65536&&usage[2]<=8*1024*1024-32768);if(!full){reportAdmissions();return;}
    stage("normal_admission_complete");std::array<std::array<std::uint64_t,2>,7> expectedUrgent{};for(std::size_t i=0;i<callers.size();++i)for(int n=0;n<2;++n)expectedUrgent[i][n]=callers[i]->serial+n+1;
    const auto urgentSent=GetTickCount64();for(auto& client:callers)for(int n=0;n<2;++n)CHECK(client->send(MessageKind::mcpRequest,object({{"version",1},{"serial",mcp::ipc::decimal(++client->serial)},{"call",object({{"name","stop_preview"},{"arguments",job}})}})));
    CHECK(until([&]{admissionUsage[1]=mcp::detail::EndpointTestAccess::queueUsage(endpoint);admissionTicks[1]=GetTickCount64();return admissionUsage[1][1]==14;},1000));
    stage("foreign_urgent_admission_complete");
    const int stopId=++serial;CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",stopId},{"method","tools/call"},{"params",object({{"name","stop_preview"},{"arguments",job}})}})));
    const int cancelId=++serial;CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",cancelId},{"method","tools/call"},{"params",object({{"name","cancel_job"},{"arguments",job}})}})));
    CHECK(until([&]{usage=mcp::detail::EndpointTestAccess::queueUsage(endpoint);admissionUsage[2]=usage;admissionTicks[2]=GetTickCount64();return usage[1]==16;},1000));CHECK(usage[0]==32&&usage[2]<=8*1024*1024&&usage[3]<=16*1024*1024);
    stage("all_urgent_admission_complete");
    auto& extra=*callers.back();const auto overflow=++extra.serial;CHECK(extra.send(MessageKind::mcpRequest,object({{"version",1},{"serial",mcp::ipc::decimal(overflow)},{"call",object({{"name","cancel_job"},{"arguments",job}})}})));const auto refused=extra.receive();CHECK(refused["serial"].toString()==mcp::ipc::decimal(overflow)&&int(refused["status"])==int(mcp::Status::capacity));
    CHECK(mcp::detail::EndpointTestAccess::queueUsage(endpoint)[1]==16);const auto beforePoll=GetTickCount64();unsigned controlPolls=0;do{pump();++controlPolls;usage=mcp::detail::EndpointTestAccess::queueUsage(endpoint);if(usage[1]==0&&usage[0]<=30)break;}while(GetTickCount64()<urgentSent+2000);std::cout<<"priority dispatch urgent_elapsed_ms="<<beforePoll-urgentSent<<" poll_ms="<<GetTickCount64()-beforePoll<<" polls="<<controlPolls<<" native_normal="<<usage[0]<<" native_urgent="<<usage[1]<<" input="<<usage[2]<<" output="<<usage[3]<<std::endl;CHECK(GetTickCount64()<urgentSent+2000);CHECK(usage[1]==0);CHECK(usage[0]<=30&&usage[2]<=8*1024*1024&&usage[3]<=16*1024*1024);
    stage("dispatch_complete_foreign_receives_begin");std::size_t callerIndex=0;unsigned matchedControls=0;for(auto& client:callers){std::set<std::uint64_t> seen;unsigned matched=0,reads=0,normalDrained=0;const auto readDeadline=GetTickCount64()+8000;const auto readCap=normalSerials[callerIndex].size()+2;while(matched<2&&reads<readCap&&GetTickCount64()<readDeadline){const auto began=GetTickCount64();const auto remaining=began<readDeadline?readDeadline-began:0;const auto reply=client->receive(unsigned((std::min)(ULONGLONG(4000),remaining)),readDeadline);++reads;std::uint64_t id=0;const bool valid=mcp::ipc::decimal(reply["serial"],id)&&id>0;CHECK(valid);if(!valid)break;const bool unique=seen.insert(id).second;CHECK(unique);if(!unique)break;const bool urgent=id==expectedUrgent[callerIndex][0]||id==expectedUrgent[callerIndex][1],knownNormal=normalSerials[callerIndex].contains(id);CHECK(urgent||knownNormal);if(!urgent&&!knownNormal)break;mcp::ToolResult result;CHECK(mcp::ipc::result(reply,result));std::cout<<"MEASURE foreign caller="<<callerIndex<<" urgent="<<urgent<<" actual_serial="<<id<<" status="<<reply["status"].toString()<<" tick_ms="<<GetTickCount64()<<" elapsed_ms="<<GetTickCount64()-began<<std::endl;if(urgent){CHECK(reply["status"].isInt()&&int(reply["status"])==int(mcp::Status::unauthorized));++matched;++matchedControls;}else ++normalDrained;}CHECK(matched==2);CHECK(reads<=readCap&&GetTickCount64()<=readDeadline);std::cout<<"MEASURE foreign_summary caller="<<callerIndex<<" matched="<<matched<<" normal_drained="<<normalDrained<<" reads="<<reads<<" read_cap="<<readCap<<std::endl;++callerIndex;}CHECK(matchedControls==14);stage("foreign_receives_end");
    const auto parts=juce::StringArray::fromTokens(ready["resource_uri"].toString().substring(14),"/","");const auto file=fixture.options.fixtureRoot.getChildFile("MCP Previews").getChildFile(parts[1]).getChildFile(parts[0]).getChildFile(parts[2]+".wav");CHECK(GetFileAttributesW((L"\\\\?\\"+std::wstring(file.getFullPathName().toWideCharPointer())).c_str())==INVALID_FILE_ATTRIBUTES);
    // Native control actions have run while stdout still owns the original blocked frame.
    const auto activeSample=outputSample.read(observation);stage("active_bytes_assertion");outputSample.report("active_bytes_same_sample");bridge.report("before_owner_drains",measurementStart);CHECK(activeSample.getDynamicObject()!=nullptr&&!bool(activeSample["activeResource"]));CHECK(activeSample["activeBytes"].toString().getLargeIntValue()==pressureBytes[0]);CHECK(int(activeSample["queuedResources"])==0&&activeSample["queuedBytes"].toString().getLargeIntValue()+activeSample["activeBytes"].toString().getLargeIntValue()==expectedOutput);CHECK(activeSample["toolResults"].toString().getLargeIntValue()==before+pressureFrames&&activeSample["refusedResults"].toString()=="1");
    int controls=0,budget=0,capacity=0;std::set<int> outputIds;for(int n=0;n<pressureFrames+2;++n){stage("owner_drain_begin");juce::var reply;std::atomic_bool done=false;std::thread reader([&]{reply=bridge.receive();done=true;});CHECK(until([&]{pump();return done.load();}));reader.join();const auto id=int(reply["id"]);CHECK(outputIds.insert(id).second);CHECK(reply["id"].isInt()&&reply["jsonrpc"].toString()=="2.0");const auto content=reply["result"]["structuredContent"];const auto status=content["status"].toString();std::cout<<"actual priority owner/output reply id="<<id<<" status="<<status<<std::endl;if(id==stopId||id==cancelId){CHECK(status=="ready");++controls;}else{int index=-1;for(int i=0;i<pressureFrames;++i)if(id==pressureRpcIds[i])index=i;CHECK(index>=0);if(index>=0&&index<retainedFrames){CHECK(status=="ready"&&content["payload"].toString()==payload);CHECK(mcp::rpcSuccess(id,reply["result"]).getNumBytesAsUTF8()+1==pressureBytes[index]);++budget;}else if(index==retainedFrames){CHECK(status=="capacity"&&!content.hasProperty("payload"));CHECK(mcp::rpcSuccess(id,reply["result"]).getNumBytesAsUTF8()+1==capacityBytes);++capacity;}}stage("owner_drain_end");}
    CHECK(controls==2&&budget==retainedFrames&&capacity==1);CHECK(outputIds.size()==pressureFrames+2);CHECK(until([&]{pump();return mcp::detail::EndpointTestAccess::queueUsage(endpoint)[0]==0;}));stage("final_get_job_begin");CHECK(tool("get_job",job)["status"].toString()=="stale");stage("final_get_job_end");bridge.report("pressure_complete",measurementStart);
    reportAdmissions();
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
}
void routedEdits(const juce::File& helper,bool stageOnly=false){
    std::cout<<"real_bridge_two_processor_edits stage_only="<<stageOnly<<std::endl;
    ServiceFixture fixture(helper);CHECK(fixture.start());
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    auto a=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime()),b=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());
    CHECK(testing::readyRackInventory(*a)&&testing::readyRackInventory(*b));
    CHECK(until([&]{return a->mcpView().connected&&b->mcpView().connected;}));
    StdioChild bridge;CHECK(bridge.start(helper,"--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix));
    const auto same=[](const juce::var& x,const juce::var& y){return juce::JSON::toString(x,true)==juce::JSON::toString(y,true);};
    InstrumentProcessor* extra=nullptr;
    int serial=0;
    const auto rpcOn=[&](StdioChild& peer,const char* method,juce::var params){
        CHECK(peer.send(object({{"jsonrpc","2.0"},{"id",++serial},{"method",method},{"params",params}})));
        juce::var reply;std::atomic_bool done=false;std::thread read([&]{reply=peer.receive();done=true;});
        CHECK(until([&]{a->pollMcp();b->pollMcp();if(extra)extra->pollMcp();return done.load();}));read.join();CHECK(int(reply["id"])==serial);return reply;
    };
    const auto rpc=[&](const char* method,juce::var params){return rpcOn(bridge,method,params);};
    const auto toolOn=[&](StdioChild& peer,const char* name,juce::var args){auto reply=rpcOn(peer,"tools/call",object({{"name",name},{"arguments",args}}));std::cout<<"routed "<<name<<" "<<juce::JSON::toString(reply,true).substring(0,700)<<std::endl;return reply;};
    const auto tool=[&](const char* name,juce::var args){return toolOn(bridge,name,args);};
    CHECK(rpc("initialize",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","edit-fixture"},{"version","1"}})}}))["result"].getDynamicObject());
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));
    const auto target=object({{"instance_id",a->mcpView().instance.toDashedString()}}),other=object({{"instance_id",b->mcpView().instance.toDashedString()}});
    auto before=tool("get_state",target);CHECK(before["result"]["structuredContent"]["status"].toString()=="ready");CHECK(before["result"]["structuredContent"]["snapshot"].isString());
    auto bBefore=tool("get_state",other)["result"]["structuredContent"];
    auto catalog=tool("get_catalog",target);CHECK(catalog["result"]["structuredContent"]["status"].toString()=="ready");
    auto edits=object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.23}})}}});
    auto prepared=tool("prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",before["result"]["structuredContent"]["snapshot"]},{"policy",object({})},{"edits",edits}}));
    CHECK(prepared["result"]["structuredContent"]["status"].toString()=="ready");CHECK(prepared["result"]["structuredContent"]["edit_id"].isString());
    if(!prepared["result"]["structuredContent"]["edit_id"].isString()||stageOnly)return;
    const auto edit=object({{"instance_id",target["instance_id"]},{"edit_id",prepared["result"]["structuredContent"]["edit_id"]}});
    a->closeAiPanel();CHECK(tool("get_edit",edit)["result"]["structuredContent"]["status"].toString()=="ready");
    StdioChild intruder;CHECK(intruder.start(helper,"--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix));
    CHECK(rpcOn(intruder,"initialize",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","other-owner"},{"version","1"}})}}))["result"].getDynamicObject());
    CHECK(intruder.send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));
    for(auto name:{"get_edit","apply_edit","discard_edit"}){auto denied=toolOn(intruder,name,edit)["result"];CHECK(bool(denied["isError"]));CHECK(denied["structuredContent"]["status"].toString()=="unauthorized");}
    const auto count=InstrumentMcpTestAccess::historyCount(*a);
    auto first=tool("apply_edit",edit)["result"]["structuredContent"];
    CHECK(first["status"].toString()=="ready");
    if(first["status"].toString()!="ready")return;
    CHECK(first["transaction_id"].isString());CHECK(first["revision"].isString());CHECK(bool(first["changed"]));CHECK(InstrumentMcpTestAccess::historyCount(*a)==count+1);
    auto replay=tool("apply_edit",edit)["result"]["structuredContent"];CHECK(same(replay,first));CHECK(InstrumentMcpTestAccess::historyCount(*a)==count+1);
    auto committed=tool("get_state",target)["result"]["structuredContent"];CHECK(committed["revision"]==first["revision"]);CHECK(!same(committed["values"],before["result"]["structuredContent"]["values"]));
    const auto historyArgs=[&](const juce::var& revision){return object({{"instance_id",target["instance_id"]},{"transaction_id",first["transaction_id"]},{"expected_revision",revision}});};
    const auto undone=tool("undo_edit",historyArgs(first["revision"]))["result"]["structuredContent"];CHECK(undone["status"].toString()=="ready");
    CHECK(same(tool("get_state",target)["result"]["structuredContent"]["values"],before["result"]["structuredContent"]["values"]));
    const auto redone=tool("redo_edit",historyArgs(undone["revision"]))["result"]["structuredContent"];CHECK(redone["status"].toString()=="ready");CHECK(redone["revision"]!=first["revision"]);
    CHECK(same(tool("get_state",target)["result"]["structuredContent"]["values"],committed["values"]));CHECK(same(tool("get_state",other)["result"]["structuredContent"],bBefore));
    CHECK(InstrumentMcpTestAccess::historyCount(*a)==count+1);CHECK(!fixture.options.fixtureRoot.getChildFile("OAuthProbe").exists());CHECK(!fixture.options.fixtureRoot.getChildFile("CompanionPreferences").exists());
    auto deniedHistory=toolOn(intruder,"undo_edit",historyArgs(redone["revision"]))["result"];CHECK(bool(deniedHistory["isError"]));CHECK(deniedHistory["structuredContent"]["status"].toString()=="unauthorized");
    CHECK(tool("undo_edit",historyArgs(first["revision"]))["result"]["structuredContent"]["status"].toString()=="stale");
    CHECK(tool("redo_edit",historyArgs(redone["revision"]))["result"]["structuredContent"]["status"].toString()=="stale");
    CHECK(toolOn(intruder,"apply_edit",edit)["result"]["structuredContent"]["status"].toString()=="unauthorized");
    const auto field=std::size_t(std::find_if(parameterRegistry().begin(),parameterRegistry().end(),[](const auto& p){return p.id=="global.master_level";})-parameterRegistry().begin());
    const auto state=[&]{return tool("get_state",target)["result"]["structuredContent"];};
    const auto stage=[&](juce::var body){const auto current=state();auto prepared=tool("prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",current["snapshot"]},{"policy",object({})},{"edits",body}}))["result"]["structuredContent"];CHECK(prepared["status"].toString()=="ready");CHECK(prepared["edit_id"].isString());return object({{"instance_id",target["instance_id"]},{"edit_id",prepared["edit_id"]}});};
    const auto nativeValue=[&](float value){CHECK(a->beginParameterGesture(field));CHECK(a->setParameterGestureValue(field,value));CHECK(a->endParameterGesture(field));};
    // The native UI uses the same actions and IDs. Replacing a redo slot cannot authorize its former owner.
    CHECK(a->undoSoundEdit());auto live=state();CHECK(tool("redo_edit",historyArgs(live["revision"]))["result"]["structuredContent"]["status"].toString()=="ready");
    CHECK(a->undoSoundEdit());nativeValue(.42f);live=state();CHECK(tool("redo_edit",historyArgs(live["revision"]))["result"]["structuredContent"]["status"].toString()=="stale");CHECK(tool("undo_edit",historyArgs(live["revision"]))["result"]["structuredContent"]["status"].toString()=="stale");
    auto busyEdit=stage(edits);CHECK(a->beginParameterGesture(field));CHECK(tool("apply_edit",busyEdit)["result"]["structuredContent"]["status"].toString()=="busy");CHECK(tool("get_edit",busyEdit)["result"]["structuredContent"]["status"].toString()=="ready");CHECK(a->endParameterGesture(field));
    auto second=tool("apply_edit",busyEdit)["result"]["structuredContent"];CHECK(second["status"].toString()=="ready");std::uint64_t firstId=0,secondId=0;CHECK(mcp::ipc::decimal(first["transaction_id"],firstId));CHECK(mcp::ipc::decimal(second["transaction_id"],secondId));CHECK(secondId>firstId);
    const auto noOpBefore=state();const auto noOpCount=InstrumentMcpTestAccess::historyCount(*a);const auto noOpEdit=stage(object({}));const auto noOp=tool("apply_edit",noOpEdit)["result"]["structuredContent"];CHECK(noOp["status"].toString()=="ready");CHECK(!bool(noOp["changed"]));CHECK(noOp["transaction_id"].isVoid());CHECK(noOp["revision"]==noOpBefore["revision"]);CHECK(InstrumentMcpTestAccess::historyCount(*a)==noOpCount);CHECK(same(tool("apply_edit",noOpEdit)["result"]["structuredContent"],noOp));
    // Real strict native field validation: malformed nested edits never allocate stage authority or change sound.
    for(auto bad:{juce::JSON::parse(R"({"parameter_edits":[{"id":"global.master_level","normalized":0.3,"extra":true}]})"),juce::JSON::parse(R"({"parameter_edits":{}})"),juce::JSON::parse(R"({"rack_edits":[{"kind":"remove_synth","slot":0}]})")}){
        const auto current=state();auto refused=tool("prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",current["snapshot"]},{"policy",object({})},{"edits",bad}}))["result"];CHECK(bool(refused["isError"]));CHECK(refused["structuredContent"]["status"].toString()=="invalid_input");CHECK(same(state(),current));}
    auto staleEdit=stage(edits);nativeValue(.43f);auto manual=state();CHECK(tool("apply_edit",staleEdit)["result"]["structuredContent"]["status"].toString()=="stale");CHECK(same(state(),manual));
    staleEdit=stage(edits);auto locked=std::make_unique<SoundDocument>();std::uint64_t currentRevision=0;CHECK(a->trySoundSnapshot(*locked,currentRevision));locked->controlLocks[field]=true;CHECK(a->applySoundEdit(*locked,SoundEditOrigin::manual,currentRevision));manual=state();CHECK(tool("apply_edit",staleEdit)["result"]["structuredContent"]["status"].toString()=="stale");CHECK(same(state(),manual));CHECK(a->undoSoundEdit());
    struct Listener final:juce::AudioProcessorParameter::Listener {std::function<void()> begin;int starts=0,ends=0,values=0;void parameterValueChanged(int,float)override{++values;}void parameterGestureChanged(int,bool started)override{if(started){++starts;if(begin)begin();}else ++ends;}};
    auto timed=stage(edits);const auto beforeDeadline=state();Listener listener;listener.begin=[] {Sleep(2100);};a->control(field).addListener(&listener);auto timedReply=tool("apply_edit",timed)["result"];a->control(field).removeListener(&listener);CHECK(bool(timedReply["isError"]));CHECK(timedReply["structuredContent"]["status"].toString()=="host_control_unavailable");CHECK(listener.starts==1&&listener.ends==1&&listener.values==0);CHECK(same(state(),beforeDeadline));
    // Fill the bridge's eight-request queue, then cancel its queued Apply through the priority path.
    auto queued=stage(edits);const auto beforeCancel=state();const auto cancelledId=++serial;CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",cancelledId},{"method","tools/call"},{"params",object({{"name","apply_edit"},{"arguments",queued}})}})));
    for(int n=0;n<7;++n)CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",++serial},{"method","tools/call"},{"params",object({{"name","get_state"},{"arguments",target}})}})));
    CHECK(until([&]{return mcp::detail::EndpointTestAccess::commandPending(InstrumentMcpTestAccess::endpoint(*a),mcp::Tool::applyEdit);}));
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",object({{"requestId",cancelledId}})}})));
    CHECK(until([&]{return mcp::detail::EndpointTestAccess::controlPending(InstrumentMcpTestAccess::endpoint(*a),mcp::CommandControl::cancel);}));
    for(int n=0;n<7;++n){juce::var reply;std::atomic_bool done=false;std::thread reader([&]{reply=bridge.receive();done=true;});CHECK(until([&]{a->pollMcp();b->pollMcp();if(extra)extra->pollMcp();return done.load();}));reader.join();CHECK(int(reply["id"])!=cancelledId);const auto status=reply["result"]["structuredContent"]["status"].toString();CHECK(status=="ready"||status=="host_control_unavailable");CHECK(bool(reply["result"]["isError"])==(status!="ready"));}
    CHECK(same(state(),beforeCancel));CHECK(tool("get_edit",queued)["result"]["structuredContent"]["status"].toString()=="stale");
    // Retain a completed receipt when delivery is cancelled; replay must never re-execute the native action.
    auto lost=stage(edits);const auto lostCount=InstrumentMcpTestAccess::historyCursor(*a);const auto lostId=++serial;
    auto& endpoint=InstrumentMcpTestAccess::endpoint(*a);std::atomic_bool atReply=false,held=false,release=false;mcp::detail::EndpointTestAccess::armReplyContention(endpoint,&atReply,&held);
    std::thread locker([&]{while(!atReply.load()&&!release.load())std::this_thread::yield();if(!release.load())mcp::detail::EndpointTestAccess::holdHandoff(endpoint,held,release);});
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",lostId},{"method","tools/call"},{"params",object({{"name","apply_edit"},{"arguments",lost}})}})));
    CHECK(until([&]{a->pollMcp();return atReply.load();}));CHECK(InstrumentMcpTestAccess::pending(*a));release=true;locker.join();mcp::detail::EndpointTestAccess::armReplyContention(endpoint,nullptr,nullptr);
    CHECK(InstrumentMcpTestAccess::historyCount(*a)==lostCount+1);CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",object({{"requestId",lostId}})}})));
    CHECK(until([&]{return mcp::detail::EndpointTestAccess::controlPending(endpoint,mcp::CommandControl::cancel);}));CHECK(until([&]{a->pollMcp();return !InstrumentMcpTestAccess::pending(*a);}));
    const auto recovered=tool("apply_edit",lost)["result"]["structuredContent"];CHECK(recovered["status"].toString()=="ready");CHECK(bool(recovered["changed"]));CHECK(InstrumentMcpTestAccess::historyCount(*a)==lostCount+1);
    // No-op receipts can evict a handle while its native action still exists; old handles stay invalid.
    const auto snapshot=state()["snapshot"];const auto retainedCount=InstrumentMcpTestAccess::historyCount(*a);
    for(int n=0;n<64;++n){auto next=tool("prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",snapshot},{"policy",object({})},{"edits",object({})}}))["result"]["structuredContent"];CHECK(next["edit_id"].isString());auto receipt=tool("apply_edit",object({{"instance_id",target["instance_id"]},{"edit_id",next["edit_id"]}}))["result"]["structuredContent"];CHECK(receipt["status"].toString()=="ready"&&!bool(receipt["changed"]));}
    CHECK(InstrumentMcpTestAccess::historyCount(*a)==retainedCount);CHECK(tool("apply_edit",lost)["result"]["structuredContent"]["status"].toString()=="stale");CHECK(tool("undo_edit",object({{"instance_id",target["instance_id"]},{"transaction_id",recovered["transaction_id"]},{"expected_revision",state()["revision"]}}))["result"]["structuredContent"]["status"].toString()=="stale");
    auto epochStage=stage(object({}));const auto oldEpoch=a->mcpView().epoch;a->setMcpEnabled(false);CHECK(tool("apply_edit",epochStage)["result"]["structuredContent"]["status"].toString()=="unauthorized");a->setMcpEnabled(true);CHECK(a->mcpView().epoch!=oldEpoch);CHECK(until([&]{a->pollMcp();const auto listed=fixture.list();for(int n=0;n<listed.size();++n)if(listed[n]["instance_id"].toString()==a->mcpView().instance.toDashedString())return bool(listed[n]["enabled"])&&listed[n]["epoch"].toString()==a->mcpView().epoch.toDashedString();return false;}));CHECK(tool("apply_edit",epochStage)["result"]["structuredContent"]["status"].toString()=="stale");
    // A full native restore clears slots but never rewinds the ID counter.
    juce::MemoryBlock restored;a->getStateInformation(restored);CHECK(a->restoreSoundState(restored.getData(),int(restored.getSize())));CHECK(testing::readyRackInventory(*a));
    CHECK(until([&]{a->pollMcp();const auto listed=fixture.list();for(int n=0;n<listed.size();++n)if(listed[n]["instance_id"].toString()==a->mcpView().instance.toDashedString())return bool(listed[n]["enabled"])&&listed[n]["epoch"].toString()==a->mcpView().epoch.toDashedString();return false;}));
    auto afterRestore=stage(object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",.31}})}}}));auto third=tool("apply_edit",afterRestore)["result"]["structuredContent"];std::uint64_t thirdId=0;CHECK(third["status"].toString()=="ready");CHECK(mcp::ipc::decimal(third["transaction_id"],thirdId)&&thirdId>secondId);
    CHECK(same(tool("get_state",other)["result"]["structuredContent"],bBefore));CHECK(!fixture.options.fixtureRoot.getChildFile("launch-observation.json").exists());
    // Overflow the existing native action slots; identities move with surviving actions and never repeat.
    for(int n=0;n<70;++n)nativeValue(n%2?.42f:.41f);const auto surviving=InstrumentMcpTestAccess::historyIds(*a);CHECK(!surviving.empty()&&surviving.size()<=64&&surviving.front()>thirdId);CHECK(std::adjacent_find(surviving.begin(),surviving.end(),[](auto x,auto y){return x>=y;})==surviving.end());
    CHECK(tool("undo_edit",object({{"instance_id",target["instance_id"]},{"transaction_id",third["transaction_id"]},{"expected_revision",state()["revision"]}}))["result"]["structuredContent"]["status"].toString()=="stale");
    for(std::size_t n=0;n<surviving.size();++n)CHECK(a->undoSoundEdit());CHECK(InstrumentMcpTestAccess::historyCursor(*a)==0);for(std::size_t n=0;n<surviving.size();++n)CHECK(a->redoSoundEdit());CHECK(InstrumentMcpTestAccess::historyIds(*a)==surviving);CHECK(a->control(field).getValue()==.42f);
    // Capacity fault injection changes only the next-ID counter of an empty fresh history.
    auto capacity=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(testing::readyRackInventory(*capacity));extra=capacity.get();CHECK(until([&]{return capacity->mcpView().connected;}));CHECK(InstrumentMcpTestAccess::exhaustFresh(*capacity));
    const auto capacityTarget=object({{"instance_id",capacity->mcpView().instance.toDashedString()}});const auto capacityBefore=tool("get_state",capacityTarget)["result"]["structuredContent"];
    auto capacityProject=std::make_unique<ProjectDocument>();std::uint64_t capacityRevision=0,capacityReset=0;CHECK(capacity->tryProjectSnapshot(*capacityProject,capacityRevision,capacityReset));
    auto capacityPrepared=tool("prepare_edit",object({{"instance_id",capacityTarget["instance_id"]},{"snapshot",capacityBefore["snapshot"]},{"policy",object({})},{"edits",edits}}))["result"]["structuredContent"];CHECK(capacityPrepared["edit_id"].isString());
    Listener capacityListener;capacity->control(field).addListener(&capacityListener);auto refusedCapacity=tool("apply_edit",object({{"instance_id",capacityTarget["instance_id"]},{"edit_id",capacityPrepared["edit_id"]}}))["result"];capacity->control(field).removeListener(&capacityListener);
    CHECK(bool(refusedCapacity["isError"]));CHECK(refusedCapacity["structuredContent"]["status"].toString()=="capacity");CHECK(same(tool("get_state",capacityTarget)["result"]["structuredContent"],capacityBefore));CHECK(InstrumentMcpTestAccess::historyCount(*capacity)==0);CHECK(capacityListener.values==0&&capacityListener.starts==capacityListener.ends);
    auto capacityAfter=std::make_unique<ProjectDocument>();std::uint64_t capacityObserved=0;CHECK(capacity->tryProjectSnapshot(*capacityAfter,capacityObserved));CHECK(*capacityAfter==*capacityProject&&capacityObserved==capacityRevision);extra=nullptr;capacity.reset();
    auto abandoned=stage(object({}));CloseHandle(bridge.input);bridge.input=nullptr;CHECK(until([&]{return mcp::detail::EndpointTestAccess::controlPending(endpoint,mcp::CommandControl::ownerDisconnected);}));a->pollMcp();CHECK(toolOn(intruder,"get_edit",abandoned)["result"]["structuredContent"]["status"].toString()=="stale");
    const auto foreignState=toolOn(intruder,"get_state",target)["result"]["structuredContent"];const auto expiring=toolOn(intruder,"prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",foreignState["snapshot"]},{"policy",object({})},{"edits",object({})}}))["result"]["structuredContent"];CHECK(expiring["edit_id"].isString());const auto expiringArgs=object({{"instance_id",target["instance_id"]},{"edit_id",expiring["edit_id"]}});InstrumentMcpTestAccess::expire(*a);CHECK(toolOn(intruder,"get_edit",expiringArgs)["result"]["structuredContent"]["status"].toString()=="stale");
    const auto receiptStage=toolOn(intruder,"prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",foreignState["snapshot"]},{"policy",object({})},{"edits",object({})}}))["result"]["structuredContent"];const auto expiringReceipt=object({{"instance_id",target["instance_id"]},{"edit_id",receiptStage["edit_id"]}});CHECK(toolOn(intruder,"apply_edit",expiringReceipt)["result"]["structuredContent"]["status"].toString()=="ready");InstrumentMcpTestAccess::expire(*a);CHECK(toolOn(intruder,"apply_edit",expiringReceipt)["result"]["structuredContent"]["status"].toString()=="stale");
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
}
void historyContention(const juce::File& helper){
    ServiceFixture fixture(helper);CHECK(fixture.start());SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    for(int mode=0;mode<5;++mode)for(bool redo:{false,true}){
        std::cout<<"HISTORY_CONTENTION mode="<<mode<<" redo="<<redo<<std::endl;
        auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(testing::readyRackInventory(*p));CHECK(until([&]{return p->mcpView().connected;}));StdioChild a,b;const auto arguments="--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix;CHECK(a.start(helper,arguments));CHECK(b.start(helper,arguments));int serial=0;
        const auto rpc=[&](StdioChild& peer,const char* method,juce::var params){const auto id=++serial;CHECK(peer.send(object({{"jsonrpc","2.0"},{"id",id},{"method",method},{"params",params}})));juce::var reply;std::atomic_bool done=false;std::thread reader([&]{reply=peer.receive();done=true;});CHECK(until([&]{p->pollMcp();return done.load();}));reader.join();CHECK(int(reply["id"])==id);return reply;};
        for(auto* peer:{&a,&b}){CHECK(rpc(*peer,"initialize",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","history-contention"},{"version","1"}})}}))["result"].getDynamicObject());CHECK(peer->send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));}
        const auto target=object({{"instance_id",p->mcpView().instance.toDashedString()}});
        const auto tool=[&](StdioChild& peer,const char* name,juce::var args){return rpc(peer,"tools/call",object({{"name",name},{"arguments",args}}))["result"];};
        const auto stage=[&](StdioChild& peer,double value){auto state=tool(peer,"get_state",target)["structuredContent"];CHECK(state["status"].toString()=="ready"&&state["snapshot"].isString());auto prepared=tool(peer,"prepare_edit",object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]},{"policy",object({})},{"edits",object({{"parameter_edits",juce::Array<juce::var>{object({{"id","global.master_level"},{"normalized",value}})}}})}}))["structuredContent"];CHECK(prepared["status"].toString()=="ready"&&prepared["edit_id"].isString());return object({{"instance_id",target["instance_id"]},{"edit_id",prepared["edit_id"]}});};
        const auto applied=tool(a,"apply_edit",stage(a,.23))["structuredContent"];CHECK(applied["status"].toString()=="ready"&&applied["transaction_id"].isString());if(applied["status"].toString()!="ready")continue;
        const auto history=[&](juce::var revision){return object({{"instance_id",target["instance_id"]},{"transaction_id",applied["transaction_id"]},{"expected_revision",revision}});};
        if(redo){auto undone=tool(a,"undo_edit",history(applied["revision"]))["structuredContent"];CHECK(undone["status"].toString()=="ready");if(undone["status"].toString()!="ready")continue;}
        juce::var staged;std::unique_ptr<std::optional<ai::GenerationRequest>> captured;
        if(mode==0||mode==3||mode==4){staged=stage(mode==3?a:b,.37);CHECK(tool(mode==3?a:b,"get_edit",staged)["structuredContent"]["status"].toString()=="ready");}
        else {
            AiRequestOptions options{"A native pending pattern","synthetic-selected_model:1.0",{ai::RequestScope::pattern,false,false,false}};
            captured=testing::fixtureValue([&]{return p->beginAiRequest(options,120);});CHECK(captured->has_value());CHECK(p->aiProposalPhase()==AiProposalPhase::pending);if(!*captured)continue;
            if(mode==2){auto result=testing::fixtureValue([&]{return ai::generateProposal(**captured,[&](const juce::String&,std::string_view,const juce::var&){rack::oauth::StructuredResult reply;reply.result.status=rack::oauth::Status::connected;reply.result.httpStatus=200;reply.text=R"({"version":1,"parameter_edits":[],"rack_edits":[],"macro_edits":[],"pattern":null,"characteristics":"native","explanation":"built-in contention fixture"})";return reply;});});CHECK(result->status==ai::GenerationStatus::ready&&result->candidate);if(result->status!=ai::GenerationStatus::ready)continue;CHECK(p->receiveAiResult(std::move(*result)));CHECK(p->aiProposalPhase()==AiProposalPhase::proposed&&p->aiProposal());if(p->aiProposalPhase()!=AiProposalPhase::proposed)continue;}
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
        auto before=std::make_unique<ProjectDocument>();std::uint64_t revision=0,reset=0;CHECK(p->tryProjectSnapshot(*before,revision,reset));const auto count=InstrumentMcpTestAccess::historyCount(*p),cursor=InstrumentMcpTestAccess::historyCursor(*p);const auto ids=InstrumentMcpTestAccess::historyIds(*p);const auto path=p->soundPresetPath();
        struct Listener final:juce::AudioProcessorParameter::Listener {int values=0,gestures=0;void parameterValueChanged(int,float)override{++values;}void parameterGestureChanged(int,bool)override{++gestures;}} listener;const auto registry=parameterRegistry();const auto field=std::size_t(std::find_if(registry.begin(),registry.end(),[](const auto& d){return d.id=="global.master_level";})-registry.begin());p->control(field).addListener(&listener);
        const auto result=mode==4?object({{"structuredContent",object({{"status",(redo?p->redoSoundEdit():p->undoSoundEdit())?"ready":"failed"}})}}):tool(a,redo?"redo_edit":"undo_edit",history(mcp::ipc::decimal(revision)));const auto status=result["structuredContent"]["status"].toString();std::cout<<"HISTORY_RESULT mode="<<mode<<" redo="<<redo<<" status="<<status<<std::endl;
        if(mode<3){CHECK(status=="busy"&&bool(result["isError"]));auto after=std::make_unique<ProjectDocument>();std::uint64_t observed=0,observedReset=0;CHECK(p->tryProjectSnapshot(*after,observed,observedReset));CHECK(*after==*before&&observed==revision&&observedReset==reset&&p->soundPresetPath()==path);CHECK(InstrumentMcpTestAccess::historyCount(*p)==count&&InstrumentMcpTestAccess::historyCursor(*p)==cursor&&InstrumentMcpTestAccess::historyIds(*p)==ids);
            if(mode==0)CHECK(tool(b,"get_edit",staged)["structuredContent"]["status"].toString()=="ready");else{CHECK(p->aiProposalPhase()==(mode==1?AiProposalPhase::pending:AiProposalPhase::proposed));CHECK(p->aiCurrentRequest()&&p->aiCurrentRequest()->tag==(**captured).tag);if(mode==2)CHECK(p->aiProposal()&&p->aiProposal()->tag==(**captured).tag);}
            juce::MessageManager::getInstance()->runDispatchLoopUntil(2);CHECK(listener.values==0&&listener.gestures==0);
        }else{CHECK(status=="ready");CHECK(InstrumentMcpTestAccess::historyCursor(*p)==(redo?cursor+1:cursor-1));CHECK(tool(mode==3?a:b,"get_edit",staged)["structuredContent"]["status"].toString()=="stale");}
        p->control(field).removeListener(&listener);p->discardAiProposal();
    }
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
}
void cancelPayload(const juce::File& helper){
    ServiceFixture fixture(helper);CHECK(fixture.start());SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    auto p=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(testing::readyRackInventory(*p));CHECK(until([&]{return p->mcpView().connected;}));StdioChild bridge;CHECK(bridge.start(helper,"--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix));
    const auto receive=[&]{juce::var reply;std::atomic_bool done=false;std::thread reader([&]{reply=bridge.receive();done=true;});CHECK(until([&]{p->pollMcp();return done.load();}));reader.join();return reply;};
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",1},{"method","initialize"},{"params",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","cancel-payload"},{"version","1"}})}})}})));CHECK(receive()["result"].getDynamicObject());CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));
    const auto target=object({{"instance_id",p->mcpView().instance.toDashedString()}});CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",2},{"method","tools/call"},{"params",object({{"name","get_state"},{"arguments",target}})}})));auto state=receive()["result"]["structuredContent"];CHECK(state["status"].toString()=="ready"&&state["snapshot"].isString());
    const auto edits=object({{"characteristics",juce::String(std::string(2048,'x'))},{"explanation",juce::String(std::string(2048,'y'))}});const auto args=object({{"instance_id",target["instance_id"]},{"snapshot",state["snapshot"]},{"policy",object({})},{"edits",edits}});
    auto& endpoint=InstrumentMcpTestAccess::endpoint(*p);
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",3},{"method","tools/call"},{"params",object({{"name","prepare_edit"},{"arguments",args}})}})));CHECK(until([&]{return mcp::detail::EndpointTestAccess::commandPending(endpoint,mcp::Tool::prepareEdit);}));CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",object({{"requestId",3}})}})));
    mcp::Command actual;CHECK(until([&]{if(!endpoint.takeControl(actual))return false;if(actual.control==mcp::CommandControl::cancel)return true;p->executeMcp(actual);return false;}));
    CHECK(actual.control==mcp::CommandControl::cancel);CHECK(actual.call.arguments.isVoid());std::cout<<"cancelled_prepare_control_argument_bytes="<<juce::JSON::toString(actual.call.arguments,true).getNumBytesAsUTF8()<<std::endl;p->executeMcp(actual);
    const auto call=[&](int id,const char* name,juce::var body){CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",id},{"method","tools/call"},{"params",object({{"name",name},{"arguments",body}})}})));auto reply=receive();CHECK(int(reply["id"])==id);return reply["result"]["structuredContent"];};
    const auto old=call(4,"prepare_edit",args);CHECK(old["status"].toString()=="ready"&&old["edit_id"].isString());
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",5},{"method","tools/call"},{"params",object({{"name","prepare_edit"},{"arguments",args}})}})));CHECK(until([&]{return mcp::detail::EndpointTestAccess::commandPending(endpoint,mcp::Tool::prepareEdit);}));CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",object({{"requestId",5}})}})));
    CHECK(until([&]{return endpoint.takeControl(actual)&&actual.control==mcp::CommandControl::cancel;}));CHECK(actual.call.arguments.isVoid());p->executeMcp(actual);
    const auto oldArgs=object({{"instance_id",target["instance_id"]},{"edit_id",old["edit_id"]}});CHECK(call(6,"get_edit",oldArgs)["status"].toString()=="ready");
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",60},{"method","tools/call"},{"params",object({{"name","render_preview"},{"arguments",oldArgs}})}})));CHECK(until([&]{return mcp::detail::EndpointTestAccess::commandPending(endpoint,mcp::Tool::renderPreview);}));
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",object({{"requestId",60}})}})));CHECK(until([&]{return mcp::detail::EndpointTestAccess::controlPending(endpoint,mcp::CommandControl::cancel);}));p->pollMcp();
    CHECK(call(61,"get_edit",oldArgs)["status"].toString()=="ready");
    CHECK(call(7,"discard_edit",oldArgs)["status"].toString()=="ready");
    std::atomic_bool atReply=false,held=false,release=false;mcp::detail::EndpointTestAccess::armReplyContention(endpoint,&atReply,&held);
    std::thread locker([&]{while(!atReply.load()&&!release.load())std::this_thread::yield();if(!release.load())mcp::detail::EndpointTestAccess::holdHandoff(endpoint,held,release);});
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",8},{"method","tools/call"},{"params",object({{"name","prepare_edit"},{"arguments",args}})}})));CHECK(until([&]{p->pollMcp();return atReply.load();}));CHECK(InstrumentMcpTestAccess::hasStage(*p));CHECK(InstrumentMcpTestAccess::pending(*p));release=true;locker.join();mcp::detail::EndpointTestAccess::armReplyContention(endpoint,nullptr,nullptr);
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/cancelled"},{"params",object({{"requestId",8}})}})));CHECK(until([&]{return mcp::detail::EndpointTestAccess::controlPending(endpoint,mcp::CommandControl::cancel);}));CHECK(until([&]{p->pollMcp();return !InstrumentMcpTestAccess::pending(*p);}));CHECK(!InstrumentMcpTestAccess::hasStage(*p));CHECK(call(9,"get_state",target)["status"].toString()=="ready");
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
}
void environment(){
    mcp::Command command;command.deadlineMs=2000;CHECK(!mcp::commandExpired(command,1999));CHECK(mcp::commandExpired(command,2000));CHECK(mcp::commandExpired(command,2001));
    CHECK(!mcp::ipc::encodedSize(object({{"data",juce::String(std::string(1024,'x'))}}),128));
    struct Env {const wchar_t* key;std::wstring value;bool present=false;Env(const wchar_t* k):key(k){const auto n=GetEnvironmentVariableW(k,nullptr,0);if(n){std::vector<wchar_t> v(n);GetEnvironmentVariableW(k,v.data(),n);value=v.data();present=true;}}~Env(){SetEnvironmentVariableW(key,present?value.c_str():nullptr);}};
    Env root(L"AIRACK_MCP_FIXTURE_ROOT"),suffix(L"AIRACK_MCP_PIPE_SUFFIX"),diag(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT");
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",nullptr);
    CHECK(mcp::serviceOptionsForProcess().has_value());CHECK(mcp::serviceOptionsForProcess()->pipeSuffix.isEmpty());
    CHECK(SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",L"")!=FALSE);CHECK(SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",L"")!=FALSE);
    SetLastError(ERROR_SUCCESS);(void)GetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr,0);CHECK(GetLastError()!=ERROR_ENVVAR_NOT_FOUND);
    CHECK(!mcp::serviceOptionsForProcess());
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
    SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",L"one");CHECK(!mcp::serviceOptionsForProcess());
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",L"relative");CHECK(!mcp::serviceOptionsForProcess());
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",L"C:/air-mcp-fixture");CHECK(mcp::serviceOptionsForProcess()->pipeSuffix=="one");
    SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",L"invalid/suffix");CHECK(!mcp::serviceOptionsForProcess());
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
    SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",L"C:/air-mcp-fixture/diagnostic");const auto first=mcp::serviceOptionsForProcess();
    CHECK(first&&first->pipeSuffix.length()==32);CHECK(first->pipeSuffix==mcp::serviceOptionsForProcess()->pipeSuffix);
    SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",L"C:/air-mcp-fixture/diagnostic/../diagnostic");CHECK(first->pipeSuffix==mcp::serviceOptionsForProcess()->pipeSuffix);
    SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",L"C:/air-mcp-fixture/other");CHECK(first->pipeSuffix!=mcp::serviceOptionsForProcess()->pipeSuffix);
    SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",L"relative");CHECK(!mcp::serviceOptionsForProcess());
    SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",nullptr);
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",L"__AIR_MCP_ENV_MISSING__");CHECK(!mcp::serviceOptionsForProcess());
    SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",L"__AIR_MCP_ENV_MISSING__");CHECK(!mcp::serviceOptionsForProcess());
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
    SetEnvironmentVariableW(L"AIRACK_SYNTH_DIAGNOSTIC_TEST_ROOT",L"__AIR_MCP_ENV_MISSING__");CHECK(!mcp::serviceOptionsForProcess());
}
void outputCapacity(ServiceFixture& fixture){
    Client endpoint;const auto id=juce::Uuid(),epoch=juce::Uuid();
    CHECK(until([&]{return endpoint.pipe.connect(fixture.options.pipeSuffix,PipeDomain::mcp)==PipeStatus::ready;}));
    auto metadata=[&](const char* label){return object({{"instance_id",id.toDashedString()},{"epoch",epoch.toDashedString()},{"label",label},{"version","test"},{"enabled",true},{"capabilities",object({})}});};
    CHECK(endpoint.send(MessageKind::mcpHello,object({{"version",1},{"role","endpoint"},{"metadata",metadata("output-budget")}})));CHECK(int(endpoint.receive()["status"])==0);
    Client slow;CHECK(slow.open(fixture.options.pipeSuffix));
    const auto payload=juce::String(std::string(3*1024*1024,'\n'));
    // Three MiB of newlines encodes to six MiB without the slow ordinary-character fixture path.
    CHECK(juce::JSON::toString(payload,true).getNumBytesAsUTF8()==6*1024*1024+2);
    // Each dispatch gets its unchanged two-second deadline; completed outputs remain blocked together.
    for(int i=0;i<3;++i){slow.request(id);const auto serial=endpoint.receive()["serial"].toString();
        CHECK(endpoint.send(MessageKind::mcpReply,object({{"version",1},{"serial",serial},{"status",0},{"data",object({{"payload",payload}})}})));}
    CHECK(endpoint.send(MessageKind::mcpEvent,object({{"version",1},{"event","metadata"},{"metadata",metadata("flushed")}})));
    Client observer;CHECK(observer.open(fixture.options.pipeSuffix));
    CHECK(until([&]{const auto list=observer.list();for(int i=0;i<list.size();++i)if(list[i]["instance_id"].toString()==id.toDashedString())return list[i]["label"].toString()=="flushed";return false;}));
    int ready=0,capacity=0;for(int i=0;i<3;++i){const auto reply=slow.receive();std::cout<<"output-budget reply_status="<<reply["status"].toString()<<" payload_bytes="<<reply["data"]["payload"].toString().length()<<std::endl;if(int(reply["status"])==0&&reply["data"]["payload"].toString()==payload)++ready;else if(int(reply["status"])==int(mcp::Status::capacity))++capacity;}
    CHECK(ready==2);CHECK(capacity==1);
}
void replyContention(ServiceFixture& fixture){
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",fixture.options.fixtureRoot.getFullPathName().toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    auto processor=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(testing::readyRackInventory(*processor));CHECK(until([&]{return processor->mcpView().connected;}));
    Client client;CHECK(client.open(fixture.options.pipeSuffix));
    const auto awaitReply=[&]{juce::var reply;std::atomic_bool done{false};std::thread reader([&]{reply=client.receive();done=true;});CHECK(until([&]{processor->pollMcp();return done.load();}));reader.join();return reply;};
    for(unsigned action=0;action<4;++action){
        std::cout<<"reply-contention action="<<action<<std::endl;
        const auto serial=client.request(processor->mcpView().instance);CHECK(client.list().size()>0);
        auto& endpoint=InstrumentMcpTestAccess::endpoint(*processor);std::atomic_bool atReply{false},held{false},release{false};
        mcp::detail::EndpointTestAccess::armReplyContention(endpoint,&atReply,&held);
        std::thread locker([&]{while(!atReply.load()&&!release.load())std::this_thread::yield();if(!release.load())mcp::detail::EndpointTestAccess::holdHandoff(endpoint,held,release);});
        CHECK(until([&]{processor->pollMcp();return atReply.load();}));CHECK(held.load());CHECK(InstrumentMcpTestAccess::pending(*processor));
        release=true;locker.join();mcp::detail::EndpointTestAccess::armReplyContention(endpoint,nullptr,nullptr);
        if(action==0){const auto reply=awaitReply();CHECK(reply["serial"].toString()==mcp::ipc::decimal(serial));CHECK(int(reply["status"])==int(mcp::Status::ready));CHECK(reply["data"]["snapshot"].isString());}
        else{
            if(action==1){CHECK(client.send(MessageKind::mcpEvent,object({{"version",1},{"event","cancel"},{"serial",mcp::ipc::decimal(serial)}})));CHECK(client.list().size()>0);CHECK(until([&]{return mcp::detail::EndpointTestAccess::controlPending(endpoint,mcp::CommandControl::cancel);}));}
            if(action==2)processor->setMcpEnabled(false);
            if(action==3){juce::MemoryBlock state;processor->getStateInformation(state);CHECK(processor->restoreSoundState(state.getData(),int(state.getSize())));}
            CHECK(until([&]{processor->pollMcp();return !InstrumentMcpTestAccess::pending(*processor);}));
            // Epoch/access retirement can answer the old route; a cancelled route never does.
            if(action!=1){const auto retired=client.receive();CHECK(retired["serial"].toString()==mcp::ipc::decimal(serial));CHECK(int(retired["status"])==int(action==2?mcp::Status::unauthorized:mcp::Status::stale));}
            if(action==2)processor->setMcpEnabled(true);
        }
        CHECK(testing::readyRackInventory(*processor));
        const auto current=processor->mcpView();Client observer;CHECK(observer.open(fixture.options.pipeSuffix));
        CHECK(until([&]{const auto list=observer.list();for(int n=0;n<list.size();++n)if(list[n]["instance_id"].toString()==current.instance.toDashedString())return bool(list[n]["enabled"])&&list[n]["epoch"].toString()==current.epoch.toDashedString();return false;}));
        const auto fresh=client.request(current.instance);CHECK(client.list().size()>0);const auto reply=awaitReply();CHECK(reply["serial"].toString()==mcp::ipc::decimal(fresh));CHECK(int(reply["status"])==int(mcp::Status::ready));CHECK(reply["data"]["snapshot"].isString());
    }
    processor.reset();SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
}
void replyEpochReuse(const juce::File& helper){
    ServiceFixture fixture(helper);CHECK(fixture.start());mcp::Endpoint endpoint;
    const mcp::EndpointMetadata initial{juce::Uuid(),juce::Uuid(),"epoch-reuse","test",true,object({})};
    CHECK(endpoint.start(fixture.options,initial));CHECK(until([&]{return endpoint.connected();}));
    Client oldClient;CHECK(oldClient.open(fixture.options.pipeSuffix));oldClient.request(initial.instance);mcp::Command old;
    CHECK(until([&]{return endpoint.take(old);}));CHECK(old.serial==1);
    mcp::Reply retained{old.serial,{mcp::Status::ready,object({{"payload","old-epoch"}})}};
    const auto withHandoff=[&](const auto& action){std::atomic_bool held{false},release{false};std::thread locker([&]{mcp::detail::EndpointTestAccess::holdHandoff(endpoint,held,release);});
        CHECK(until([&]{return held.load();}));action();release=true;locker.join();};
    withHandoff([&]{CHECK(!endpoint.reply(std::move(retained)));});
    fixture.service->kill();oldClient.pipe.close();
    CHECK(until([&]{return endpoint.metadata().epoch!=initial.epoch;}));CHECK(fixture.start());
    CHECK(until([&]{return endpoint.connected();}));const auto current=endpoint.metadata();CHECK(current.epoch!=old.caller.epoch);
    CHECK(mcp::detail::EndpointTestAccess::controlPending(endpoint,mcp::CommandControl::epochLost));
    Client freshClient;CHECK(freshClient.open(fixture.options.pipeSuffix));const auto freshSerial=freshClient.request(initial.instance);CHECK(freshClient.list().size()==1);
    mcp::Command missed;withHandoff([&]{CHECK(!endpoint.takeControl(missed));});
    // Publication retries precede lifecycle consumption, exactly as a missed priority try-lock permits.
    const bool stalePublished=until([&]{return endpoint.reply(std::move(retained));},100);CHECK(!stalePublished);
    mcp::Command retired;CHECK(until([&]{return endpoint.takeControl(retired);}));CHECK(retired.control==mcp::CommandControl::epochLost);
    mcp::Command fresh;CHECK(until([&]{return endpoint.take(fresh);}));CHECK(fresh.serial==old.serial);CHECK(fresh.caller.epoch==current.epoch);CHECK(fresh.caller.owner!=old.caller.owner);
    mcp::Reply reply{fresh.serial,{mcp::Status::ready,object({{"payload","fresh-epoch"}})}};CHECK(until([&]{return endpoint.reply(std::move(reply));}));
    const auto delivered=freshClient.receive();CHECK(delivered["serial"].toString()==mcp::ipc::decimal(freshSerial));CHECK(delivered["data"]["payload"].toString()=="fresh-epoch");
    const auto barrier=freshClient.request();const auto listed=freshClient.receive();CHECK(listed["serial"].toString()==mcp::ipc::decimal(barrier));CHECK(listed["data"]["instances"].size()==1);
    const auto proof=object({{"old_serial",mcp::ipc::decimal(old.serial)},{"fresh_serial",mcp::ipc::decimal(fresh.serial)},{"old_epoch",old.caller.epoch.toDashedString()},
        {"fresh_epoch",fresh.caller.epoch.toDashedString()},{"stale_published",stalePublished},{"delivered",delivered}});
    fixture.options.fixtureRoot.getChildFile("epoch-reuse-proof.json").replaceWithText(juce::JSON::toString(proof,true));std::cout<<"epoch-reuse "<<juce::JSON::toString(proof,true)<<std::endl;
    endpoint.close();
}
void staleProcess(ServiceFixture& fixture){
    const auto id=juce::Uuid();CHECK(fixture.addEndpoint("incarnation-old",id,true));
    const auto old=juce::JSON::parse(fixture.options.fixtureRoot.getChildFile("incarnation-old.json").loadFileAsString());
    const auto identity=[](const Child& child){FILETIME created{},a{},b{},c{};CHECK(GetProcessTimes(child.handle,&created,&a,&b,&c)!=FALSE);return mcp::ipc::decimal((std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime);};
    CHECK(int(old["process_id"])==int(fixture.endpoints.back().child->pid));CHECK(old["creation_time"].toString()==identity(*fixture.endpoints.back().child));
    Client client;CHECK(client.open(fixture.options.pipeSuffix));const auto serial=client.request(id);CHECK(client.list().size()>0);
    fixture.endpoints.back().child->kill();fixture.endpoints.back().child.reset();
    const auto gone=client.receive();CHECK(gone["serial"].toString()==mcp::ipc::decimal(serial));CHECK(int(gone["status"])==int(mcp::Status::instanceGone));
    CHECK(fixture.addEndpoint("incarnation-new",id));const auto fresh=juce::JSON::parse(fixture.options.fixtureRoot.getChildFile("incarnation-new.json").loadFileAsString());
    CHECK(int(fresh["process_id"])==int(fixture.endpoints.back().child->pid));CHECK(fresh["creation_time"].toString()==identity(*fixture.endpoints.back().child));
    CHECK(int(old["process_id"])!=int(fresh["process_id"]));CHECK(old["creation_time"].toString()!=fresh["creation_time"].toString());
    client.request(id);const auto reply=client.receive();CHECK(int(reply["status"])==0);CHECK(reply["data"]["label"].toString()=="incarnation-new");
    fixture.options.fixtureRoot.getChildFile("incarnation-proof.json").replaceWithText(juce::JSON::toString(object({{"old",old},{"replacement",fresh},{"old_route_status",gone["status"]},{"new_route",reply}}),true));
    fixture.disconnect(id);
}
void bridgeOutputCapacity(ServiceFixture& fixture){
    Client endpoint;const auto id=juce::Uuid(),epoch=juce::Uuid();CHECK(until([&]{return endpoint.pipe.connect(fixture.options.pipeSuffix,PipeDomain::mcp)==PipeStatus::ready;}));
    const auto metadata=object({{"instance_id",id.toDashedString()},{"epoch",epoch.toDashedString()},{"label","stdio-budget"},{"version","test"},{"enabled",true},{"capabilities",object({})}});
    CHECK(endpoint.send(MessageKind::mcpHello,object({{"version",1},{"role","endpoint"},{"metadata",metadata}})));CHECK(int(endpoint.receive()["status"])==0);
    StdioChild bridge;CHECK(bridge.start(fixture.helper,"--mcp-stdio --fixture-root \""+fixture.options.fixtureRoot.getFullPathName()+"\" --pipe-suffix "+fixture.options.pipeSuffix));
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",1},{"method","initialize"},{"params",object({{"protocolVersion","2025-11-25"},{"capabilities",object({})},{"clientInfo",object({{"name","byte-fixture"},{"version","1"}})}})}})));CHECK(bridge.receive()["result"].getDynamicObject());
    CHECK(bridge.send(object({{"jsonrpc","2.0"},{"method","notifications/initialized"}})));
    const auto payload=juce::String(std::string(3*1024*1024,'\n'));
    CHECK(juce::JSON::toString(payload,true).getNumBytesAsUTF8()==6*1024*1024+2);
    const auto data=juce::JSON::toString(object({{"payload",payload}}),true);
    const auto receipt=fixture.options.fixtureRoot.getChildFile("mcp-bridge-"+juce::String(int(bridge.pid))+".json");juce::var observed;
    for(int i=0;i<3;++i){CHECK(bridge.send(object({{"jsonrpc","2.0"},{"id",i+2},{"method","tools/call"},{"params",object({{"name","get_state"},{"arguments",object({{"instance_id",id.toDashedString()}})}})}})));
        const auto serial=endpoint.receive()["serial"].toString();std::uint64_t number=0;CHECK(mcp::ipc::decimal(serial,number));
        const auto response="{\"version\":1,\"serial\":\""+serial+"\",\"status\":0,\"data\":"+data+"}";
        CHECK(endpoint.pipe.beginWrite({MessageKind::mcpReply,endpoint.session,juce::MemoryBlock(response.toRawUTF8(),response.getNumBytesAsUTF8())})==PipeStatus::ready);
        CHECK(until([&]{return endpoint.pipe.pollWrite()==PipeStatus::complete;}));
        // Observe delivery into the real bridge before allowing the next service reply.
        CHECK(until([&]{observed=juce::JSON::parse(receipt.loadFileAsString());return observed["toolResults"].toString()==juce::String(i+1);}));}
    std::uint64_t queued=0,active=0;CHECK(mcp::ipc::decimal(observed["queuedBytes"],queued));CHECK(mcp::ipc::decimal(observed["activeBytes"],active));CHECK(active>6*1024*1024);CHECK(active+queued<=16*1024*1024);CHECK(observed["refusedResults"].toString()=="1");
    std::cout<<"stdio-output queued="<<queued<<" active="<<active<<" refused="<<observed["refusedResults"].toString()<<std::endl;
    int ready=0,capacity=0;for(int i=0;i<3;++i){const auto reply=bridge.receive();const auto content=reply["result"]["structuredContent"];std::cout<<"stdio-output reply="<<i<<" status="<<content["status"].toString()<<std::endl;if(content["status"].toString()=="ready"){CHECK(content["payload"].toString()==payload);++ready;}else if(content["status"].toString()=="capacity")++capacity;}
    CHECK(ready==2);CHECK(capacity==1);
}
void discovery(const juce::File& helper,unsigned instances){
    CHECK(currentUserPipeName("McpTest",PipeDomain::mcp)!=currentUserPipeName("McpTest",PipeDomain::companion));
    CHECK(currentUserPipeName("McpTest",PipeDomain::mcp).startsWith("\\\\.\\pipe\\AIRackSynth-MCP-v1-S-"));CHECK(maximumRuntimeConnections==16);CHECK(maximumPipeOwners==34);CHECK(maximumPipeInstances==18);
    ServiceFixture fixture(helper);CHECK(fixture.start());Client bridge;CHECK(bridge.open(fixture.options.pipeSuffix));CHECK(bridge.list().size()==0);
    Child incumbent;CHECK(incumbent.start(helper,{"--mcp-service","--fixture-root",fixture.options.fixtureRoot.getFullPathName(),"--pipe-suffix",fixture.options.pipeSuffix}));CHECK(until([&]{return !incumbent.alive();}));CHECK(fixture.service->alive());
    PipeConnection wrongDomain;CHECK(wrongDomain.connect(fixture.options.pipeSuffix,PipeDomain::companion)==PipeStatus::unavailable);
    PipeConnection wrongSid;companion::detail::PipeTestAccess::expectPeerSid(wrongSid,"S-1-5-18");CHECK(wrongSid.connect(fixture.options.pipeSuffix,PipeDomain::mcp)==PipeStatus::unauthorized);
    for(unsigned i=0;i<instances;++i)CHECK(fixture.addEndpoint("endpoint"+juce::String(i)));
    auto all=bridge.list();CHECK(all.size()==int(instances));std::set<juce::String> ids;
    for(int i=0;i<all.size();++i){const auto id=all[i]["instance_id"].toString();CHECK(ids.insert(id).second);bridge.request(juce::Uuid(id));const auto reply=bridge.receive();
        CHECK(int(reply["status"])==0);CHECK(reply["data"]["label"].isString());CHECK(reply["data"]["owner"].isString());}
    // The provider retains an independent connection despite all sixteen MCP endpoints.
    fixture.provider=std::make_unique<Child>();const auto providerRoot=fixture.options.fixtureRoot.getChildFile("provider");
    CHECK(fixture.provider->start(helper,{"--fixture-root",providerRoot.getFullPathName(),"--pipe-suffix",fixture.options.pipeSuffix}));PipeConnection provider;
    CHECK(until([&]{return provider.connect(fixture.options.pipeSuffix)==PipeStatus::ready;}));Frame hello;hello.kind=MessageKind::hello;hello.session=juce::Uuid::null();ControlMessage control;control.tag.runtime=juce::Uuid();CHECK(encodeControl(control,hello.payload));
    CHECK(provider.beginWrite(hello)==PipeStatus::ready);CHECK(until([&]{return provider.pollWrite()==PipeStatus::complete;}));CHECK(provider.beginRead()==PipeStatus::ready);Frame welcome;CHECK(until([&]{return provider.pollRead(welcome)==PipeStatus::complete;}));CHECK(welcome.kind==MessageKind::welcome);
    CHECK(fixture.addEndpoint("refused17",juce::Uuid(),false,false));CHECK(bridge.list().size()==16);fixture.endpoints.back().child->kill();CHECK(provider.connected());CHECK(bridge.list().size()==16);
    CHECK(fixture.addEndpoint("duplicate",fixture.endpoints.front().id,false,false));CHECK(bridge.list().size()==16);fixture.endpoints.back().child->kill();CHECK(provider.connected());
    Frame ping{MessageKind::ping,welcome.session,{}};CHECK(encodeControl(control,ping.payload));
    CHECK(provider.beginWrite(ping)==PipeStatus::ready);CHECK(until([&]{return provider.pollWrite()==PipeStatus::complete;}));CHECK(provider.beginRead()==PipeStatus::ready);
    Frame pong;CHECK(until([&]{return provider.pollRead(pong)==PipeStatus::complete;}));CHECK(pong.kind==MessageKind::pong&&pong.session==welcome.session);
    fixture.disconnect(fixture.endpoints[0].id);fixture.disconnect(fixture.endpoints[1].id);CHECK(until([&]{return bridge.list().size()==14;}));
    const auto rootName=fixture.options.fixtureRoot.getFullPathName();SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",rootName.toWideCharPointer());SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",fixture.options.pipeSuffix.toWideCharPointer());
    {
        auto a=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime()),b=std::make_unique<InstrumentProcessor>(testing::isolatedRackRuntime());CHECK(!a->getActiveEditor()&&!b->getActiveEditor());CHECK(testing::readyRackInventory(*a)&&testing::readyRackInventory(*b));
        CHECK(until([&]{a->pollMcp();b->pollMcp();return a->mcpView().connected&&b->mcpView().connected;}));CHECK(a->mcpView().instance!=b->mcpView().instance);CHECK(until([&]{return bridge.list().size()==16;}));
        const auto n=bridge.request(a->mcpView().instance);juce::var response;std::thread read([&]{response=bridge.receive();});
        for(unsigned i=0;i<50;++i)juce::MessageManager::getInstance()->runDispatchLoopUntil(5);read.join();CHECK(response["serial"].toString()==mcp::ipc::decimal(n));CHECK(int(response["status"])==int(mcp::Status::ready));CHECK(response["data"]["snapshot"].isString());
        const auto old=a->mcpView();juce::MemoryBlock state;a->getStateInformation(state);CHECK(a->restoreSoundState(state.getData(),int(state.getSize())));CHECK(a->mcpView().instance==old.instance);CHECK(a->mcpView().epoch!=old.epoch);
        a->setMcpEnabled(false);CHECK(!a->mcpView().enabled);
        CHECK(until([&]{const auto list=bridge.list();for(int i=0;i<list.size();++i)if(list[i]["instance_id"].toString()==old.instance.toDashedString())return !bool(list[i]["enabled"]);return false;}));bridge.request(old.instance);CHECK(int(bridge.receive()["status"])==int(mcp::Status::unauthorized));
        a->setMcpEnabled(true);CHECK(a->mcpView().enabled);CHECK(a->mcpView().epoch!=old.epoch);
    }
    SetEnvironmentVariableW(L"AIRACK_MCP_FIXTURE_ROOT",nullptr);SetEnvironmentVariableW(L"AIRACK_MCP_PIPE_SUFFIX",nullptr);
    CHECK(until([&]{return bridge.list().size()==14;}));CHECK(provider.connected());
    // Requests without a safe dispatcher expire; control cancellation releases capacity first.
    fixture.disconnect(fixture.endpoints[2].id);CHECK(fixture.addEndpoint("held",juce::Uuid(),true));const auto held=fixture.endpoints.back().id;
    std::vector<std::uint64_t> waiting;for(int i=0;i<8;++i)waiting.push_back(bridge.request(held));bridge.request(held);CHECK(int(bridge.receive()["status"])==int(mcp::Status::capacity));
    CHECK(bridge.send(MessageKind::mcpEvent,object({{"version",1},{"event","cancel"},{"serial",mcp::ipc::decimal(waiting[0])}})));
    bridge.request(held);
    for(int i=0;i<8;++i){const auto expired=bridge.receive();CHECK(int(expired["status"])==int(mcp::Status::hostControlUnavailable));CHECK(expired["serial"].toString()!=mcp::ipc::decimal(waiting[0]));}
    {
        std::vector<std::unique_ptr<Client>> clients;
        for(int c=0;c<4;++c){auto client=std::make_unique<Client>();CHECK(client->open(fixture.options.pipeSuffix));for(int r=0;r<8;++r)client->request(held);CHECK(client->list().size()==14);clients.push_back(std::move(client));}
        bridge.request(held);CHECK(int(bridge.receive()["status"])==int(mcp::Status::capacity));
        clients[0]->pipe.close();
        for(int c=1;c<4;++c)for(int r=0;r<8;++r)CHECK(int(clients[c]->receive()["status"])==int(mcp::Status::hostControlUnavailable));
    }
    {
        mcp::Endpoint local;const mcp::EndpointMetadata meta{juce::Uuid(),juce::Uuid(),"priority","test",true,object({})};CHECK(local.start(fixture.options,meta));CHECK(until([&]{return local.connected();}));
        Client caller;CHECK(caller.open(fixture.options.pipeSuffix));const auto clientSerial=caller.request(meta.instance);mcp::Command first;
        CHECK(until([&]{return local.take(first)&&first.control==mcp::CommandControl::none; }));CHECK(first.control==mcp::CommandControl::none);CHECK(!first.caller.owner.isNull());CHECK(first.caller.epoch==meta.epoch);
        for(int n=0;n<7;++n)caller.request(meta.instance);
        CHECK(caller.send(MessageKind::mcpEvent,object({{"version",1},{"event","cancel"},{"serial",mcp::ipc::decimal(clientSerial)}})));
        mcp::Command cancelled;bool priority=false;
        CHECK(until([&]{if(!local.take(cancelled))return false;if(cancelled.control==mcp::CommandControl::cancel){priority=true;return true;}return false;}));CHECK(priority);CHECK(cancelled.cancelledSerial==first.serial);
        caller.pipe.close();mcp::Command disconnected;CHECK(until([&]{return local.take(disconnected)&&disconnected.control==mcp::CommandControl::ownerDisconnected&&disconnected.caller.owner==first.caller.owner;}));CHECK(disconnected.caller.owner==first.caller.owner);
        local.close();
    }
    replyContention(fixture);replyEpochReuse(helper);staleProcess(fixture);
    // Quota pressure has its own real service, independent of preceding held/lifecycle routes.
    {ServiceFixture budget(helper);CHECK(budget.start());outputCapacity(budget);}
    {ServiceFixture budget(helper);CHECK(budget.start());bridgeOutputCapacity(budget);}
    Client spoof;CHECK(spoof.open(fixture.options.pipeSuffix));CHECK(spoof.send(MessageKind::mcpRequest,object({{"version",1},{"serial","1"},{"owner",juce::Uuid().toDashedString()},{"call",object({{"name","list_instances"},{"arguments",object({})}})}})));CHECK(spoof.receive(1000).isVoid());
    Client role;CHECK(role.open(fixture.options.pipeSuffix));CHECK(role.send(MessageKind::mcpReply,object({{"version",1},{"serial","1"},{"status",0},{"data",object({})}})));CHECK(role.receive(1000).isVoid());
    Client epoch;CHECK(epoch.open(fixture.options.pipeSuffix));epoch.session=juce::Uuid();CHECK(epoch.send(MessageKind::mcpRequest,object({{"version",1},{"serial","1"},{"call",object({{"name","list_instances"},{"arguments",object({})}})}})));CHECK(epoch.receive(1000).isVoid());
    const auto before=fixture.endpoints[3].id;const auto oldList=bridge.list();juce::String oldEpoch;for(int i=0;i<oldList.size();++i)if(oldList[i]["instance_id"].toString()==before.toDashedString())oldEpoch=oldList[i]["epoch"].toString();
    fixture.service->kill();bridge.pipe.close();CHECK(fixture.start());Client restarted;CHECK(restarted.open(fixture.options.pipeSuffix));CHECK(until([&]{return restarted.list().size()==14;}));
    const auto newList=restarted.list();juce::String newEpoch;for(int i=0;i<newList.size();++i)if(newList[i]["instance_id"].toString()==before.toDashedString())newEpoch=newList[i]["epoch"].toString();CHECK(oldEpoch.isNotEmpty()&&newEpoch.isNotEmpty()&&oldEpoch!=newEpoch);
    provider.close();fixture.stop();
    mcp::Endpoint missing;const auto absentSuffix=juce::Uuid().toString().removeCharacters("-");
    CHECK(missing.start({absentSuffix,fixture.options.fixtureRoot.getChildFile("missing")},{juce::Uuid(),juce::Uuid(),"missing helper","test",true,object({})}));
    CHECK(!until([&]{return missing.connected();},100));missing.close();
    CHECK(!fixture.options.fixtureRoot.getChildFile("OAuthProbe").exists());CHECK(!fixture.options.fixtureRoot.getChildFile("CompanionPreferences").exists());
}
}
void idleExit(const juce::File& helper){
    ServiceFixture fixture(helper);CHECK(fixture.start());Client bridge;CHECK(bridge.open(fixture.options.pipeSuffix));
    Sleep(2500);CHECK(fixture.service->alive());CHECK(bridge.list().size()==0);
    const auto id=juce::Uuid();CHECK(fixture.addEndpoint("idle-owner",id));bridge.pipe.close();
    Sleep(2500);CHECK(fixture.service->alive());
    fixture.disconnect(id);Sleep(500);Client returning;CHECK(returning.open(fixture.options.pipeSuffix));
    Sleep(2500);CHECK(fixture.service->alive());CHECK(returning.list().size()==0);returning.pipe.close();
    CHECK(until([&]{return !fixture.service->alive();},5000));
    DWORD exit=STILL_ACTIVE;CHECK(GetExitCodeProcess(fixture.service->handle,&exit)&&exit==0);
}
int main(int argc,char** argv){auto args=juce::StringArray::fromTokens(juce::String(GetCommandLineW()),true);for(auto& arg:args)arg=arg.unquoted();if(args.size()>1&&args[1]=="--endpoint")return endpointChild(args);if(args.size()>1&&args[1]=="--processor")return processorChild(args);
    if(args.size()==3&&args[1]=="--idle-exit"){idleExit(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==2&&args[1]=="--activity-observation"){activityObservation();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==2&&args[1]=="--resource-reply-shape"){resourceReplyShape();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--activity"){juce::ScopedJuceInitialiser_GUI gui;activityAndDiff(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==2&&args[1]=="--observation-decode"){observationDecode();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--context"){juce::ScopedJuceInitialiser_GUI gui;admittedContext(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--pending-priority"){juce::ScopedJuceInitialiser_GUI gui;pendingPriorityWrite(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--priority"){juce::ScopedJuceInitialiser_GUI gui;priorityPressure(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--admission"){juce::ScopedJuceInitialiser_GUI gui;previewDelivery(juce::File(args[2]),0,1);std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--delivery"){juce::ScopedJuceInitialiser_GUI gui;previewDelivery(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--preview"){juce::ScopedJuceInitialiser_GUI gui;previewWorkflow(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--history-contention"){juce::ScopedJuceInitialiser_GUI gui;historyContention(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--cancel-payload"){juce::ScopedJuceInitialiser_GUI gui;cancelPayload(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&(args[1]=="--edits-stage"||args[1]=="--edits")){juce::ScopedJuceInitialiser_GUI gui;routedEdits(juce::File(args[2]),args[1]=="--edits-stage");std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==2&&args[1]=="--config"){environment();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--bridge-budget"){ServiceFixture fixture{juce::File(args[2])};CHECK(fixture.start());bridgeOutputCapacity(fixture);std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--reply-contention"){juce::ScopedJuceInitialiser_GUI gui;ServiceFixture fixture{juce::File(args[2])};CHECK(fixture.start());replyContention(fixture);std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--reply-epoch"){replyEpochReuse(juce::File(args[2]));std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(args.size()==3&&args[1]=="--output-budget"){ServiceFixture fixture{juce::File(args[2])};CHECK(fixture.start());outputCapacity(fixture);std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    juce::ScopedJuceInitialiser_GUI gui;if(argc<2)return 2;idleExit(juce::File(argv[1]));environment();observationDecode();activityObservation();activityAndDiff(juce::File(argv[1]));admittedContext(juce::File(argv[1]));pendingPriorityWrite(juce::File(argv[1]));previewWorkflow(juce::File(argv[1]));previewDelivery(juce::File(argv[1]));priorityPressure(juce::File(argv[1]));discovery(juce::File(argv[1]),16);routedEdits(juce::File(argv[1]));cancelPayload(juce::File(argv[1]));historyContention(juce::File(argv[1]));
    std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}

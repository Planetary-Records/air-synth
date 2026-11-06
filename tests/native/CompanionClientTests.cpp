#include "FrozenRackExamples.h"
#include "OwnedFixture.h"
#include "CompanionClient.h"
#include <juce_cryptography/juce_cryptography.h>
#include "CompanionRuntimeServer.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Sddl.h>
#include <TlHelp32.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>

using namespace airack;
using namespace airack::companion;
namespace
{
int checks=0,failures=0,groups=0,previousChecks=0,previousFailures=0;
const char* group="";
void check(bool pass,const char* text,int line){++checks;if(!pass){++failures;std::cerr<<group<<':'<<line<<' '<<text<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void finishGroup(){if(groups)std::cout<<group<<" checks="<<checks-previousChecks<<" failures="<<failures-previousFailures<<std::endl;}
void begin(const char* name){finishGroup();group=name;++groups;previousChecks=checks;previousFailures=failures;std::cout<<name<<std::endl;}
template<class F> bool observe(F f,std::uint64_t ms=3000){const auto end=GetTickCount64()+ms;do{if(f())return true;Sleep(1);}while(GetTickCount64()<end);return false;}
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
juce::String suffix(){return juce::Uuid().toString().removeCharacters("-");}
bool same(PipePeer a,PipePeer b){return a.processId==b.processId && a.creationTime==b.creationTime;}
std::uint64_t incarnation(HANDLE p){FILETIME a{},b{},c{},d{};return GetProcessTimes(p,&a,&b,&c,&d)?(std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime:0;}
struct Temp
{
    const juce::File parent=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/host-fixtures");
    const juce::File root=parent.getChildFile("client "+juce::Uuid().toString()+juce::String::fromUTF8(" éŸ³ Ã¼"));
    Temp(){if(!juce::File::isAbsolutePath(root.getFullPathName()) || root.getParentDirectory()!=parent || root.createDirectory().failed())throw std::runtime_error("owned workspace root");}
    ~Temp(){const bool owned=juce::File::isAbsolutePath(root.getFullPathName()) && root.isAChildOf(parent) && root.getParentDirectory()==parent;CHECK(owned);if(owned)CHECK(root.deleteRecursively());}
};
struct Child
{
    HANDLE process=nullptr;
    ~Child(){if(process){retire();CloseHandle(process);}}
    bool duplicate(const CompanionClient& client)
    {
        const auto source=static_cast<HANDLE>(airack::companion::detail::CompanionClientTestAccess::borrowProcessHandle(client));
        if(!source)return false;
        if(DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&process,0,FALSE,DUPLICATE_SAME_ACCESS))return true;
        CHECK(TerminateProcess(source,97)!=FALSE);CHECK(WaitForSingleObject(source,5000)==WAIT_OBJECT_0);return false;
    }
    bool alive() const{return process && WaitForSingleObject(process,0)==WAIT_TIMEOUT;}
    PipePeer peer() const{return process?PipePeer{GetProcessId(process),incarnation(process)}:PipePeer{};}
    void retire(){if(alive())CHECK(TerminateProcess(process,97)!=FALSE);const bool done=process && WaitForSingleObject(process,5000)==WAIT_OBJECT_0;CHECK(done);DWORD code=STILL_ACTIVE;if(process)GetExitCodeProcess(process,&code);std::cout<<"owned child pid="<<GetProcessId(process)<<" terminationObserved="<<done<<" exit="<<code<<std::endl;}
};
ClientOptions options(const Temp& temp,const juce::String& name,const juce::File& exe={},bool launch=false){return {{exe,name,temp.root},launch,false,false};}
bool ready(CompanionClient& client,const ClientOptions& o){const bool started=client.start(o);CHECK(started);if(!started)return false;const bool ok=observe([&]{client.poll();return client.status().phase!=ClientPhase::connecting;});CHECK(ok);CHECK(client.status().phase==ClientPhase::ready);return ok && client.status().phase==ClientPhase::ready;}
bool take(CompanionClient& c,ClientGenerationReply& r){return observe([&]{c.poll();return c.takeGeneration(r);});}
bool take(CompanionClient& c,AccountReply& r){return observe([&]{c.poll();return c.takeAccount(r);});}
ai::GenerationRequest request(const CompanionClient& c,std::uint64_t id,std::string text="normal"){ai::GenerationRequest r;r.tag={c.runtime(),id,7,11};r.model="synthetic-model";r.instruction=std::move(text);return r;}
Frame control(MessageKind kind,juce::Uuid session,ai::RequestTag tag,RuntimeStatus status=RuntimeStatus::ready){Frame f{kind,session,{}};if(!encodeControl({tag,status},f.payload))throw std::runtime_error("control fixture");return f;}
ai::GenerationResult result(const ai::GenerationRequest& r,bool maximum=false)
{
    ai::GenerationResult g;g.tag=r.tag;g.status=ai::GenerationStatus::ready;g.inferenceCalls=1;g.proposalStatus=ai::ProposalStatus::accepted;g.renderStatus=ai::RenderStatus::ready;g.providerStatus=rack::oauth::Status::connected;g.httpStatus=200;
    g.edits.parameters.push_back({"output.gain_db",.4f});ProjectDocument candidate;
    if(ai::admitProposal(r.source,r.policy,g.edits,candidate)!=ai::ProposalStatus::accepted)throw std::runtime_error("native candidate admission");g.candidate=candidate;
    g.audio.bodyFrames=maximum?ai::maximumPreviewBodyFrames:1;g.audio.left.resize(g.audio.bodyFrames+ai::previewTailFrames);g.audio.right.resize(g.audio.left.size());
    for(std::size_t i=0;i<g.audio.bodyFrames;++i){g.audio.left[i]=.1f*std::sin(float(i)*.071f);g.audio.right[i]=g.audio.left[i]*.5f;}
    if(ai::measurePreview(g.audio.left,g.audio.right,g.audio.bodyFrames,g.measurements)!=ai::RenderStatus::ready)throw std::runtime_error("raw measured audio");return g;
}
Frame resultFrame(const ai::GenerationRequest& r,juce::Uuid session,bool maximum=false){Frame f{MessageKind::result,session,{}};const auto g=result(r,maximum);if(!encodeResult(r,g,f.payload))throw std::runtime_error("result fixture");return f;}
void u64(juce::MemoryBlock& b,std::size_t offset,std::uint64_t v){CHECK(offset+8<=b.getSize());if(offset+8>b.getSize())return;auto* p=static_cast<unsigned char*>(b.getData())+offset;for(unsigned i=0;i<8;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
void u32(juce::MemoryBlock& b,std::size_t offset,std::uint32_t v){CHECK(offset+4<=b.getSize());if(offset+4>b.getSize())return;auto* p=static_cast<unsigned char*>(b.getData())+offset;for(unsigned i=0;i<4;++i)p[i]=static_cast<unsigned char>(v>>(8*i));}
bool sourceExact(const ai::GenerationRequest& r,const juce::MemoryBlock& saved){juce::MemoryBlock now;return encodeProjectState(r.source,now) && now==saved;}
void observation(const Temp& temp,const Child& child)
{
    const auto file=temp.root.getChildFile("launch-observation.json");CHECK(observe([&]{return file.existsAsFile();}));const auto v=juce::JSON::parse(file.loadFileAsString());CHECK(v.isObject());CHECK(v.hasProperty("processId") && v.hasProperty("creationTime") && v.hasProperty("consoleWindow") && v.hasProperty("transportCalls") && v.hasProperty("browserCalls"));
    CHECK(static_cast<juce::int64>(v["processId"])==child.peer().processId);CHECK(v["creationTime"].toString()==juce::String(child.peer().creationTime));CHECK(!static_cast<bool>(v["consoleWindow"]));CHECK(static_cast<juce::int64>(v["transportCalls"])==0);CHECK(static_cast<juce::int64>(v["browserCalls"])==0);
}
struct Backend
{
    std::atomic_bool release{false};std::atomic_int calls{0},cancels{0};std::mutex mutex;std::vector<RuntimeJob> jobs;
    Frame run(const RuntimeJob& job)
    {
        calls.fetch_add(1);{std::lock_guard lock(mutex);jobs.push_back(job);}
        if(const auto* r=std::get_if<ai::GenerationRequest>(&job.input)){
            if(r->instruction=="hold")while(!release.load())Sleep(1);
            if(r->instruction=="candidate")return resultFrame(*r,job.session);
            if(r->instruction=="maximum")return resultFrame(*r,job.session,true);
            return control(MessageKind::failure,job.session,r->tag,RuntimeStatus::modelNotConfigured);
        }
        const auto& command=std::get<AccountCommand>(job.input);if(command.model=="hold")while(!release.load())Sleep(1);
        AccountReply r;r.providerStatus=command.model=="denied"?rack::oauth::Status::denied:command.model=="expired"?rack::oauth::Status::accessExpired:command.model=="refusal"?rack::oauth::Status::inferenceNotGranted:rack::oauth::Status::signedOut;
        Frame f{MessageKind::accountResult,job.session,{}};if(!encodeAccountReply(r,f.payload))throw std::runtime_error("account fixture");return f;
    }
    bool ids(std::initializer_list<std::uint64_t> expected){std::vector<std::uint64_t> got;std::lock_guard lock(mutex);for(const auto& j:jobs)if(const auto* r=std::get_if<ai::GenerationRequest>(&j.input))got.push_back(r->tag.requestId);return got==std::vector<std::uint64_t>(expected);}
};
struct Service
{
    std::shared_ptr<Backend> backend=std::make_shared<Backend>();std::atomic_bool stop{false};juce::String name=suffix();RuntimeServer server;std::thread io;
    Service():server([b=backend](const auto& j){return b->run(j);},[b=backend](const auto& j){if(j.cancellation && j.cancellation->cancelled())b->cancels.fetch_add(1);}),io([this]{const auto r=server.run(stop,name);if(r.workerRetained)std::terminate();}){}
    ~Service(){backend->release.store(true);stop.store(true);if(io.joinable())io.join();}
};
// A real restricted listener controls only outgoing fixture bytes, not client IO.
struct Pair
{
    Temp temp;juce::String name=suffix();PipeListener listener;PipeConnection pipe;CompanionClient client;juce::Uuid session=juce::Uuid();bool reading=false;
    explicit Pair(ClientClock clock={}):client(juce::Uuid(),std::move(clock)){CHECK(listener.open(name)==PipeStatus::ready);}
    void pump(){client.poll();}
    bool receive(Frame& f){if(!reading){if(pipe.beginRead()!=PipeStatus::ready)return false;reading=true;}const auto s=pipe.pollRead(f);if(s!=PipeStatus::pending)reading=false;return s==PipeStatus::complete;}
    bool send(const Frame& f){const bool started=pipe.beginWrite(f)==PipeStatus::ready;CHECK(started);if(!started)return false;PipeStatus status=PipeStatus::pending;const bool done=observe([&]{pump();status=pipe.pollWrite();return status!=PipeStatus::pending;});CHECK(done);CHECK(status==PipeStatus::complete);return done && status==PipeStatus::complete;}
    bool open(std::function<void(Frame&)> welcome={},bool negotiate=false,bool local=false)
    {
        auto startOptions=options(temp,name);startOptions.negotiateSpectrum=negotiate;startOptions.negotiateOwnedPreview=local;const bool started=client.start(startOptions);CHECK(started);if(!started)return false;
        CHECK(observe([&]{pump();return listener.pollAccept(pipe)==PipeStatus::complete;}));if(!pipe.connected())return false;
        Frame hello;CHECK(observe([&]{pump();return receive(hello);}));ControlMessage h;CHECK(hello.kind==MessageKind::hello && hello.session.isNull() && decodeControl(bytes(hello.payload),h));CHECK((h.tag==ai::RequestTag{client.runtime(),0,0,0}));
        auto f=control(MessageKind::welcome,session,{client.runtime(),0,0,0});if(welcome)welcome(f);if(!send(f))return false;
        if(welcome)return true;CHECK(observe([&]{pump();return client.status().phase==ClientPhase::ready;}));return client.status().phase==ClientPhase::ready;
    }
    bool accepted(const ai::GenerationRequest& r){const bool ok=client.generate(r);CHECK(ok);if(!ok)return false;Frame f;CHECK(observe([&]{pump();return receive(f);}));ai::GenerationRequest actual;CHECK(f.kind==MessageKind::generate && decodeRequest(bytes(f.payload),actual) && actual.tag==r.tag);return f.kind==MessageKind::generate;}
    bool failed(){const bool done=observe([&]{pump();return client.status().phase==ClientPhase::failed;});CHECK(done);CHECK(client.status().status==RuntimeStatus::invalidMessage);CHECK(client.status().errorCode=="invalid_message");CHECK(client.session().isNull() && client.peer().processId==0);ClientGenerationReply sentinel=ControlMessage{{juce::Uuid(),91,0,0},RuntimeStatus::busy};const auto old=std::get<ControlMessage>(sentinel);CHECK(!client.takeGeneration(sentinel));CHECK(std::get<ControlMessage>(sentinel).tag==old.tag);return done;}
};

// Parent ruling: encoder-forbidden headers require an independently owned raw
// listener. Current-user DACL, unique first instance, actual overlapped lifetime.
struct RawHandshake
{
    Temp temp;juce::String name=suffix();HANDLE pipe=INVALID_HANDLE_VALUE,event=nullptr;OVERLAPPED operation{};bool pending=false;
    std::array<unsigned char,69> input{};juce::MemoryBlock outgoing;
    RawHandshake()
    {
        HANDLE token=nullptr;CHECK(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)!=FALSE);if(!token)return;
        DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<std::byte> storage(size);const bool ok=size>0 && GetTokenInformation(token,TokenUser,storage.data(),size,&size);CHECK(ok);CloseHandle(token);if(!ok)return;
        LPWSTR sid=nullptr;CHECK(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,&sid)!=FALSE);if(!sid)return;
        const auto sddl="D:P(A;;GA;;;"+juce::String(sid)+")";LocalFree(sid);PSECURITY_DESCRIPTOR descriptor=nullptr;CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.toWideCharPointer(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE);if(!descriptor)return;
        SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};pipe=CreateNamedPipeW(currentUserPipeName(name).toWideCharPointer(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,5000,&security);LocalFree(descriptor);CHECK(pipe!=INVALID_HANDLE_VALUE);
        event=CreateEventW(nullptr,TRUE,FALSE,nullptr);CHECK(event!=nullptr);
    }
    ~RawHandshake(){retire();if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);if(event)CloseHandle(event);}
    void reset(){retire();operation={};operation.hEvent=event;ResetEvent(event);}
    void retire()
    {
        if(!pending)return;CancelIoEx(pipe,&operation);const auto end=GetTickCount64()+5000;DWORD count=0;
        while(!GetOverlappedResult(pipe,&operation,&count,FALSE) && GetLastError()==ERROR_IO_INCOMPLETE){if(GetTickCount64()>=end){CHECK(false);std::terminate();}Sleep(1);}pending=false;
    }
    bool completed(CompanionClient& c)
    {
        DWORD count=0;const bool seen=observe([&]{c.poll();if(GetOverlappedResult(pipe,&operation,&count,FALSE)){pending=false;return true;}if(GetLastError()!=ERROR_IO_INCOMPLETE){pending=false;return true;}return false;});CHECK(seen);if(!seen)retire();return seen;
    }
    void exercise(int mode)
    {
        if(pipe==INVALID_HANDLE_VALUE || !event)return;CompanionClient c;reset();const BOOL connected=ConnectNamedPipe(pipe,&operation);const auto error=connected?ERROR_SUCCESS:GetLastError();pending=connected || error==ERROR_IO_PENDING;CHECK(connected || error==ERROR_IO_PENDING || error==ERROR_PIPE_CONNECTED);
        const bool started=c.start(options(temp,name));CHECK(started);if(!started)return;if(pending && !completed(c))return;
        reset();DWORD got=0;const BOOL read=ReadFile(pipe,input.data(),static_cast<DWORD>(input.size()),&got,&operation);pending=read || GetLastError()==ERROR_IO_PENDING;CHECK(pending);if(pending && !completed(c))return;
        Frame welcome=control(MessageKind::welcome,juce::Uuid(),{c.runtime(),0,0,0});CHECK(encodeFrame(welcome,outgoing));auto* raw=static_cast<unsigned char*>(outgoing.getData());
        if(mode==0)raw[4]=2;if(mode==1)std::memset(raw+12,0,16);if(mode==2)raw[28+16]=1;if(mode==3)raw[28+40]=255;
        reset();DWORD written=0;const BOOL sent=WriteFile(pipe,outgoing.getData(),static_cast<DWORD>(outgoing.getSize()),&written,&operation);pending=sent || GetLastError()==ERROR_IO_PENDING;CHECK(pending);if(pending)completed(c);
        CHECK(observe([&]{c.poll();return c.status().phase==ClientPhase::failed;}));CHECK(c.status().status==RuntimeStatus::invalidMessage && c.status().errorCode=="invalid_message");CHECK(c.session().isNull() && c.peer().processId==0);c.close();
    }
};

void startup_reuse_and_authority(const juce::File& helper)
{
    begin(__func__);Temp temp;const auto name=suffix();CompanionClient client;const auto runtime=client.runtime();CHECK(!runtime.isNull());Child child;
    if(ready(client,options(temp,name,helper,true))){
        CHECK(child.duplicate(client));CHECK(child.alive());CHECK(same(client.peer(),child.peer()));CHECK(client.peer().processId>0 && client.peer().creationTime>0);observation(temp,child);
        const auto firstSession=client.session();CompanionClient other;CHECK(other.runtime()!=runtime);if(ready(other,options(temp,name,temp.root.getChildFile("missing.exe"),true))){CHECK(!other.session().isNull() && other.session()!=firstSession);CHECK(same(other.peer(),client.peer()));CHECK(airack::companion::detail::CompanionClientTestAccess::borrowProcessHandle(other)==nullptr);}
        auto invalid=options(temp,name,helper,true);invalid.launch.pipeSuffix="invalid!";CHECK(!client.start(invalid));CHECK(client.session()==firstSession && client.status().phase==ClientPhase::ready);
        invalid=options(temp,name,helper,true);invalid.launch.fixtureRoot={};CHECK(!client.start(invalid));CHECK(client.session()==firstSession);
        CompanionClient duplicate(runtime);CHECK(duplicate.start(options(temp,name)));CHECK(observe([&]{duplicate.poll();return duplicate.status().phase==ClientPhase::failed;}));CHECK(duplicate.status().status==RuntimeStatus::duplicateInstance);CHECK(client.status().phase==ClientPhase::ready);
        CHECK(client.generate(request(client,10)));ClientGenerationReply reply;CHECK(take(client,reply));CHECK(std::holds_alternative<ControlMessage>(reply));if(const auto* r=std::get_if<ControlMessage>(&reply))CHECK(r->status==RuntimeStatus::modelNotConfigured);observation(temp,child);
        child.retire();CHECK(observe([&]{client.poll();return client.status().phase==ClientPhase::failed;}));CHECK(client.session().isNull());
        Temp second;Child replacement;if(ready(client,options(second,name,helper,true))){CHECK(replacement.duplicate(client));CHECK(client.runtime()==runtime);CHECK(client.session()!=firstSession && !client.session().isNull());CHECK(!same(client.peer(),child.peer()));CHECK(!client.generate(request(client,10)));CHECK(client.generate(request(client,11)));CHECK(take(client,reply));observation(second,replacement);client.close();CHECK(replacement.alive());}
        other.close();
    }
    for(int mode=0;mode<3;++mode){Pair p;if(p.open([&](Frame& f){if(mode==0)f.kind=MessageKind::pong;if(mode==1)f=control(MessageKind::welcome,p.session,{juce::Uuid(),0,0,0});if(mode==2)f=control(MessageKind::welcome,p.session,{p.client.runtime(),0,0,0},RuntimeStatus::busy);})){p.failed();}}
    for(int mode=0;mode<4;++mode){RawHandshake raw;raw.exercise(mode);}
    {Temp t;PipeListener listener;const auto n=suffix();CHECK(listener.open(n)==PipeStatus::ready);auto now=std::chrono::steady_clock::now();CompanionClient c(juce::Uuid(),[&]{return now;});PipeConnection server;const bool started=c.start(options(t,n));CHECK(started);if(started){CHECK(observe([&]{c.poll();return listener.pollAccept(server)==PipeStatus::complete;}));now+=std::chrono::milliseconds(clientStartupTimeoutMs);c.poll();CHECK(c.status().phase==ClientPhase::failed && c.status().errorCode=="startup_timeout");CHECK(c.session().isNull());}}
    CompanionClient null(juce::Uuid::null());Temp nullRoot;CHECK(!null.start(options(nullRoot,suffix())));CHECK(null.status().phase==ClientPhase::failed && null.status().errorCode=="invalid_runtime");
}
void bounded_account_and_cancellation(const juce::File& helper)
{
    begin(__func__);Temp temp;CompanionClient client;Child child;const auto n=suffix();
    if(ready(client,options(temp,n,helper,true))){CHECK(child.duplicate(client));AccountReply sentinel;sentinel.errorCode="sentinel";CHECK(!client.takeAccount(sentinel));CHECK(sentinel.errorCode=="sentinel");CHECK(client.account({AccountAction::status,{}}));CHECK(!client.account({AccountAction::status,{}}));CHECK(!client.generate(request(client,1)));CHECK(observe([&]{client.poll();return client.status().phase==ClientPhase::failed || !airack::companion::detail::CompanionClientTestAccess::hasStartedWrite(client);}));
        // A buffered account reply continues to hold exclusive account authority.
        CHECK(observe([&]{client.poll();return airack::companion::detail::CompanionClientTestAccess::hasAccountReply(client);},8000));CHECK(client.status().status==RuntimeStatus::busy);CHECK(!client.account({AccountAction::status,{}}));AccountReply accountReply;CHECK(client.takeAccount(accountReply));CHECK(accountReply.providerStatus==rack::oauth::Status::signedOut);CHECK(client.status().status==RuntimeStatus::ready);CHECK(!client.takeAccount(sentinel) && sentinel.errorCode=="sentinel");
        CHECK(client.generate(request(client,2)));ClientGenerationReply r;CHECK(take(client,r));CHECK(std::holds_alternative<ControlMessage>(r));if(const auto* f=std::get_if<ControlMessage>(&r))CHECK(f->status==RuntimeStatus::modelNotConfigured);observation(temp,child);client.close();CHECK(child.alive());
    }
    {Service service;Temp t;CompanionClient a,b;if(ready(a,options(t,service.name)) && ready(b,options(t,service.name))){
        CHECK(a.account({AccountAction::selectModel,"hold"}));CHECK(observe([&]{a.poll();return service.backend->calls.load()==1;}));const auto old=a.session();a.cancelAccount();CHECK(a.status().phase==ClientPhase::closed && a.session().isNull());CHECK(observe([&]{return service.backend->cancels.load()==1;}));CHECK(b.status().phase==ClientPhase::ready);service.backend->release.store(true);
        if(ready(a,options(t,service.name))){CHECK(a.session()!=old);for(int i=0;i<20;++i)a.poll();CHECK(service.backend->calls.load()==1);AccountReply out;CHECK(!a.takeAccount(out));CHECK(b.account({AccountAction::status,{}}));CHECK(take(b,out));CHECK(out.providerStatus==rack::oauth::Status::signedOut);
            for(const auto& [model,status]:std::array<std::pair<const char*,rack::oauth::Status>,3>{{{"denied",rack::oauth::Status::denied},{"expired",rack::oauth::Status::accessExpired},{"refusal",rack::oauth::Status::inferenceNotGranted}}}){CHECK(a.account({AccountAction::selectModel,model}));CHECK(take(a,out));CHECK(out.providerStatus==status);}}
    }}
    for(int mode=0;mode<3;++mode){Service service;Temp t;auto now=std::chrono::steady_clock::now();bool throwing=false;CompanionClient c(juce::Uuid(),[&]{if(throwing)throw std::runtime_error("synthetic clock");return now;});if(ready(c,options(t,service.name))){CHECK(c.account({AccountAction::selectModel,"hold"}));CHECK(observe([&]{c.poll();return service.backend->calls.load()==1;}));if(mode==0)now+=std::chrono::milliseconds(clientOperationTimeoutMs);if(mode==1)now-=std::chrono::milliseconds(1);if(mode==2)throwing=true;c.poll();CHECK(c.status().phase==ClientPhase::failed);CHECK(c.status().errorCode==(mode==0?"request_timeout":"clock_failure"));CHECK(c.session().isNull());CHECK(observe([&]{return service.backend->cancels.load()==1;}));}}
}
void newest_generation_and_strict_receipt()
{
    begin(__func__);
    std::cout<<"TASK13_BOUNDARY 45 service_client_setup.before"<<std::endl;{Service service;Temp t;CompanionClient c;std::cout<<"TASK13_BOUNDARY 45 service_client_setup.after"<<std::endl;if(ready(c,options(t,service.name))){std::cout<<"TASK13_BOUNDARY 45 request_triple.before"<<std::endl;const auto task13Next1136_aStorage=std::unique_ptr<ai::GenerationRequest>(new ai::GenerationRequest(request(c,1,"hold")));const auto& a=*task13Next1136_aStorage;const auto task13Next1136_bStorage=std::unique_ptr<ai::GenerationRequest>(new ai::GenerationRequest(request(c,2)));const auto& b=*task13Next1136_bStorage;const auto task13Next1136_dStorage=std::unique_ptr<ai::GenerationRequest>(new ai::GenerationRequest(request(c,3)));const auto& d=*task13Next1136_dStorage;std::cout<<"TASK13_BOUNDARY 45 request_triple.after"<<std::endl;juce::MemoryBlock saved;CHECK(encodeProjectState(a.source,saved));std::cout<<"TASK13_BOUNDARY 45 first_generate.before"<<std::endl;CHECK(c.generate(a));std::cout<<"TASK13_BOUNDARY 45 first_generate.after"<<std::endl;CHECK(airack::companion::detail::CompanionClientTestAccess::hasStartedWrite(c));CHECK(c.generate(b));CHECK(c.generate(d));auto ownedFixture1=airack::testing::fixtureValue([&]{return request(c,4);});auto& invalid=*ownedFixture1;invalid.instruction.clear();CHECK(!c.generate(invalid));invalid=request(c,3);CHECK(!c.generate(invalid));invalid=request(c,4);invalid.tag.runtime=juce::Uuid();CHECK(!c.generate(invalid));invalid=request(c,4);invalid.policy.scope=static_cast<ai::RequestScope>(255);CHECK(!c.generate(invalid));CHECK(!c.account({AccountAction::status,{}}));CHECK(observe([&]{c.poll();return service.backend->calls.load()>=1;}));CHECK(observe([&]{c.poll();return service.backend->cancels.load()>=1;}));service.backend->release.store(true);auto task13Next1136_rStorage=std::make_unique<ClientGenerationReply>();auto& r=*task13Next1136_rStorage;std::cout<<"TASK13_BOUNDARY 45 first_reply.before"<<std::endl;CHECK(take(c,r));std::cout<<"TASK13_BOUNDARY 45 first_reply.after"<<std::endl;CHECK(std::holds_alternative<ControlMessage>(r));if(const auto* f=std::get_if<ControlMessage>(&r))CHECK(f->tag==d.tag);CHECK(service.backend->ids({1,3}));CHECK(sourceExact(a,saved));}}
    {Pair p;if(p.open()){auto ownedFixture2=airack::testing::fixtureValue([&]{return request(p.client,1);});const auto& a=*ownedFixture2;auto ownedFixture3=airack::testing::fixtureValue([&]{return request(p.client,3);});auto& c=*ownedFixture3;CHECK(p.client.generate(a));CHECK(airack::companion::detail::CompanionClientTestAccess::hasStartedWrite(p.client));CHECK(p.client.generate(c));Frame f;CHECK(observe([&]{p.pump();return p.receive(f);}));CHECK(observe([&]{p.pump();return p.receive(f);}));auto old=resultFrame(a,p.session);old.payload.setSize(40);CHECK(p.send(old));CHECK(p.client.status().phase==ClientPhase::ready);auto current=resultFrame(c,p.session);CHECK(p.send(current));auto ownedFixture13=airack::testing::fixtureValue([&]{return ClientGenerationReply{};});auto& out=*ownedFixture13;CHECK(take(p.client,out));CHECK(std::holds_alternative<ai::GenerationResult>(out));if(const auto* r=std::get_if<ai::GenerationResult>(&out)){CHECK(r->tag==c.tag);CHECK(r->candidate.has_value() && r->edits.parameters.size()==1);CHECK(r->audio.left.size()==ai::previewTailFrames+1);}CHECK(!p.client.takeGeneration(out));}}
    for(int mode=0;mode<12;++mode){Pair p;if(p.open()){auto ownedFixture4=airack::testing::fixtureValue([&]{return request(p.client,1);});auto& r=*ownedFixture4;if(mode==8)r.source.sound.controlLocks[756]=true;if(mode==9)r.policy.scope=ai::RequestScope::pattern;if(!p.accepted(r))continue;auto ownedFixture5=airack::testing::fixtureValue([&]{return r;});auto& accepted=*ownedFixture5;accepted.source.sound.controlLocks[756]=false;accepted.policy.scope=ai::RequestScope::sound;auto f=resultFrame(accepted,p.session);
        if(mode==0)f.session=juce::Uuid();if(mode==1)std::memcpy(f.payload.getData(),juce::Uuid().getRawData(),16);if(mode==2)u64(f.payload,16,2);if(mode==3)u64(f.payload,24,8);if(mode==4)u64(f.payload,32,12);if(mode==5)f.payload.setSize(39);if(mode==6){const unsigned char extra=7;f.payload.append(&extra,1);}if(mode==7)u32(f.payload,f.payload.getSize()-8,0x7fc00001);if(mode==10){f.kind=MessageKind::accountResult;AccountReply q;CHECK(encodeAccountReply(q,f.payload));}if(mode==11){f.kind=MessageKind::result;static_cast<unsigned char*>(f.payload.getData())[40]=255;}
        CHECK(p.send(f));p.failed();}}
    for(int mode=0;mode<3;++mode){Pair p;if(p.open()){auto ownedFixture6=airack::testing::fixtureValue([&]{return request(p.client,1);});auto& a=*ownedFixture6;auto ownedFixture7=airack::testing::fixtureValue([&]{return request(p.client,2);});auto& b=*ownedFixture7;CHECK(p.client.generate(a));CHECK(p.client.generate(b));if(mode==0){p.client.cancelGeneration();CHECK(!p.client.account({AccountAction::status,{}}));Frame f;CHECK(observe([&]{p.pump();return p.receive(f);}));CHECK(observe([&]{p.pump();return p.receive(f);}));CHECK(f.kind==MessageKind::cancel);ControlMessage cancel;CHECK(decodeControl(bytes(f.payload),cancel));CHECK(cancel.tag==a.tag);CHECK(p.send(control(MessageKind::failure,p.session,a.tag,RuntimeStatus::cancelled)));CHECK(observe([&]{p.pump();return p.client.status().status==RuntimeStatus::ready;}));}
        else {Frame f;CHECK(observe([&]{p.pump();return p.receive(f);}));CHECK(observe([&]{p.pump();return p.receive(f);}));if(mode==1){p.client.cancelGeneration();CHECK(!p.client.account({AccountAction::status,{}}));CHECK(p.send(resultFrame(b,p.session)));auto ownedFixture14=airack::testing::fixtureValue([&]{return ClientGenerationReply{};});auto& no=*ownedFixture14;CHECK(!p.client.takeGeneration(no));CHECK(observe([&]{p.pump();return p.receive(f);}));CHECK(f.kind==MessageKind::cancel);CHECK(p.send(control(MessageKind::failure,p.session,b.tag,RuntimeStatus::cancelled)));CHECK(observe([&]{p.pump();return p.client.status().status==RuntimeStatus::ready;}));}else {CHECK(p.send(resultFrame(b,p.session)));for(int i=0;i<15;++i)p.pump();CHECK(p.client.status().status==RuntimeStatus::busy);p.client.cancelGeneration();CHECK(p.client.status().status==RuntimeStatus::ready);CHECK(p.client.account({AccountAction::status,{}}));CHECK(observe([&]{p.pump();return p.receive(f);}));CHECK(f.kind==MessageKind::account);}}
    }}
    {Service service;Temp t;CompanionClient c;if(ready(c,options(t,service.name))){CHECK(c.generate(request(c,1,"hold")));CHECK(observe([&]{c.poll();return service.backend->calls.load()==1;}));c.cancelGeneration();CHECK(observe([&]{c.poll();return c.status().status==RuntimeStatus::ready;}));CHECK(c.account({AccountAction::status,{}}));AccountReply q;CHECK(take(c,q));CHECK(q.status==RuntimeStatus::busy);service.backend->release.store(true);}}
    // Parent-authorized regression: a delayed cancellation ACK for A must not
    // erase B's already-dispatched remote ownership. All peer IO is actual.
    {
        Pair p;
        if(p.open()){
            auto ownedFixture8=airack::testing::fixtureValue([&]{return request(p.client,1);});const auto& a=*ownedFixture8;auto ownedFixture9=airack::testing::fixtureValue([&]{return request(p.client,2);});auto& b=*ownedFixture9;
            juce::MemoryBlock source;
            CHECK(encodeProjectState(b.source,source));
            CHECK(p.accepted(a));
            p.client.cancelGeneration();
            CHECK(airack::companion::detail::CompanionClientTestAccess::hasStartedWrite(p.client));
            CHECK(p.client.generate(b));
            Frame frame;
            CHECK(observe([&]{p.pump();return p.receive(frame);}));
            CHECK(frame.kind==MessageKind::cancel);
            ControlMessage cancelled;
            CHECK(decodeControl(bytes(frame.payload),cancelled));
            CHECK(cancelled.tag==a.tag);
            CHECK(observe([&]{p.pump();return p.receive(frame);}));
            auto ownedFixture15=airack::testing::fixtureValue([&]{return ai::GenerationRequest{};});auto& dispatched=*ownedFixture15;
            CHECK(frame.kind==MessageKind::generate && decodeRequest(bytes(frame.payload),dispatched));
            CHECK(dispatched.tag==b.tag);
            CHECK(p.send(control(MessageKind::failure,p.session,a.tag,RuntimeStatus::cancelled)));
            CHECK(p.client.status().phase==ClientPhase::ready && p.client.status().status==RuntimeStatus::busy);
            p.client.cancelGeneration();
            CHECK(p.client.status().status==RuntimeStatus::busy);
            CHECK(!p.client.account({AccountAction::status,{}}));
            CHECK(observe([&]{p.pump();return p.receive(frame);}));
            CHECK(frame.kind==MessageKind::cancel && decodeControl(bytes(frame.payload),cancelled));
            CHECK(cancelled.tag==b.tag);
            CHECK(p.send(resultFrame(b,p.session)));
            ClientGenerationReply sentinel=ControlMessage{{p.client.runtime(),99,0,0},RuntimeStatus::busy};
            CHECK(!p.client.takeGeneration(sentinel));
            CHECK(std::get<ControlMessage>(sentinel).tag.requestId==99);
            CHECK(p.send(control(MessageKind::failure,p.session,b.tag,RuntimeStatus::cancelled)));
            CHECK(observe([&]{p.pump();return p.client.status().status==RuntimeStatus::ready;}));
            CHECK(sourceExact(b,source));
        }
    }
    // I1: B's valid terminal reply precedes the old cancellation ACK for A.
    // The peer then remains silent; a completed B cannot strand a live barrier.
    for(int mode=0;mode<2;++mode){
        auto now=std::chrono::steady_clock::now();
        Pair p([&]{return now;});
        if(p.open()){
            auto ownedFixture10=airack::testing::fixtureValue([&]{return request(p.client,1);});const auto& a=*ownedFixture10;auto ownedFixture11=airack::testing::fixtureValue([&]{return request(p.client,2);});auto& b=*ownedFixture11;
            const auto runtime=p.client.runtime(),session=p.client.session();
            juce::MemoryBlock source;
            CHECK(encodeProjectState(b.source,source));
            CHECK(p.accepted(a));
            p.client.cancelGeneration();
            CHECK(airack::companion::detail::CompanionClientTestAccess::hasStartedWrite(p.client));
            CHECK(p.client.generate(b));
            Frame frame;
            CHECK(observe([&]{p.pump();return p.receive(frame);}));
            ControlMessage cancelled;
            CHECK(frame.kind==MessageKind::cancel && decodeControl(bytes(frame.payload),cancelled));
            CHECK(cancelled.tag==a.tag);
            CHECK(observe([&]{p.pump();return p.receive(frame);}));
            auto ownedFixture16=airack::testing::fixtureValue([&]{return ai::GenerationRequest{};});auto& dispatched=*ownedFixture16;
            CHECK(frame.kind==MessageKind::generate && decodeRequest(bytes(frame.payload),dispatched));
            CHECK(dispatched.tag==b.tag);
            CHECK(p.send(mode==0?resultFrame(b,p.session):control(MessageKind::failure,p.session,b.tag,RuntimeStatus::modelNotConfigured)));
            auto ownedFixture17=airack::testing::fixtureValue([&]{return ClientGenerationReply{};});auto& reply=*ownedFixture17;
            const bool consumed=take(p.client,reply);
            CHECK(consumed);
            CHECK(mode==0?std::holds_alternative<ai::GenerationResult>(reply):std::holds_alternative<ControlMessage>(reply));
            if(const auto* r=std::get_if<ai::GenerationResult>(&reply))CHECK(r->tag==b.tag && r->candidate.has_value());
            if(const auto* r=std::get_if<ControlMessage>(&reply))CHECK(r->tag==b.tag && r->status==RuntimeStatus::modelNotConfigured);
            now+=std::chrono::milliseconds(clientOperationTimeoutMs+1);
            p.pump();
            const auto state=p.client.status();
            const bool settled=state.phase==ClientPhase::ready && state.status==RuntimeStatus::ready;
            const bool timedOut=state.phase==ClientPhase::failed && state.errorCode=="request_timeout";
            std::cout<<"i1_terminal="<<(mode==0?"success":"typed_failure")<<" cancelA_frame_observed=1 generateB_frame_observed=1 b_consumed="<<consumed<<" old_ack_withheld=1 after_ceiling_phase="<<int(state.phase)<<" after_ceiling_status="<<int(state.status)<<" error="<<state.errorCode<<std::endl;
            CHECK(settled || timedOut);
            CHECK(p.client.runtime()==runtime);
            CHECK(timedOut?p.client.session().isNull():p.client.session()==session);
            ClientGenerationReply sentinel=ControlMessage{{runtime,99,0,0},RuntimeStatus::busy};
            CHECK(!p.client.takeGeneration(sentinel));
            CHECK(std::get<ControlMessage>(sentinel).tag.requestId==99);
            CHECK(sourceExact(b,source));
        }
    }
    // A complete buffered reply without a live barrier remains takeable beyond
    // the waiting ceiling; success and typed failure both retain this contract.
    for(int mode=0;mode<2;++mode){
        auto now=std::chrono::steady_clock::now();
        Pair p([&]{return now;});
        if(p.open()){
            auto ownedFixture12=airack::testing::fixtureValue([&]{return request(p.client,1);});const auto& r=*ownedFixture12;
            CHECK(p.accepted(r));
            CHECK(p.send(mode==0?resultFrame(r,p.session):control(MessageKind::failure,p.session,r.tag,RuntimeStatus::modelNotConfigured)));
            // Native send has completed. These bounded real polls drain the
            // receiver's remaining chunk and complete frame before advancing.
            for(unsigned i=0;i<=clientPollBudget;++i)p.pump();
            now+=std::chrono::milliseconds(clientOperationTimeoutMs+1);
            p.pump();
            CHECK(p.client.status().phase==ClientPhase::ready && p.client.status().status==RuntimeStatus::busy);
            auto ownedFixture18=airack::testing::fixtureValue([&]{return ClientGenerationReply{};});auto& reply=*ownedFixture18;
            CHECK(p.client.takeGeneration(reply));
            CHECK(mode==0?std::holds_alternative<ai::GenerationResult>(reply):std::holds_alternative<ControlMessage>(reply));
            if(const auto* g=std::get_if<ai::GenerationResult>(&reply))CHECK(g->tag==r.tag && g->candidate.has_value());
            if(const auto* g=std::get_if<ControlMessage>(&reply))CHECK(g->tag==r.tag && g->status==RuntimeStatus::modelNotConfigured);
            CHECK(p.client.status().phase==ClientPhase::ready && p.client.status().status==RuntimeStatus::ready);
        }
    }
}
// A result, a welcome or a reply already waiting in the pipe settles its operation however late the client polls.
void late_and_slow_polls()
{
    begin(__func__);
    {Service service;Temp t;CompanionClient c;if(ready(c,options(t,service.name))){
        const auto r=request(c,1,"maximum");juce::MemoryBlock encoded;CHECK(encodeFrame(resultFrame(r,c.session(),true),encoded));std::cout<<"maximum_result_frame_bytes="<<encoded.getSize()<<std::endl;
        CHECK(c.generate(r));Sleep(6000);ClientGenerationReply reply=ControlMessage{};bool taken=false;const auto end=GetTickCount64()+10000;
        do{c.poll();taken=c.takeGeneration(reply);if(!taken)Sleep(600);}while(!taken && GetTickCount64()<end);
        CHECK(taken);CHECK(c.status().phase==ClientPhase::ready);if(const auto* g=std::get_if<ai::GenerationResult>(&reply))CHECK(g->tag==r.tag && g->audio.bodyFrames==ai::maximumPreviewBodyFrames);else CHECK(false);
    }}
    {auto now=std::chrono::steady_clock::now();Pair p([&]{return now;});CHECK(p.client.start(options(p.temp,p.name)));
        CHECK(observe([&]{p.pump();return p.listener.pollAccept(p.pipe)==PipeStatus::complete;}));Frame hello;CHECK(observe([&]{p.pump();return p.receive(hello);}));
        CHECK(p.pipe.beginWrite(control(MessageKind::welcome,p.session,{p.client.runtime(),0,0,0}))==PipeStatus::ready);CHECK(observe([&]{return p.pipe.pollWrite()==PipeStatus::complete;}));
        now+=std::chrono::milliseconds(clientStartupTimeoutMs);p.pump();CHECK(p.client.status().phase==ClientPhase::ready && p.client.session()==p.session);}
    {auto now=std::chrono::steady_clock::now();Pair p([&]{return now;});if(p.open()){const auto r=request(p.client,1);if(p.accepted(r)){
        CHECK(p.pipe.beginWrite(resultFrame(r,p.session))==PipeStatus::ready);now+=std::chrono::milliseconds(clientOperationTimeoutMs+1);p.pump();
        ClientGenerationReply reply=ControlMessage{};CHECK(p.client.takeGeneration(reply));CHECK(std::holds_alternative<ai::GenerationResult>(reply));CHECK(observe([&]{return p.pipe.pollWrite()==PipeStatus::complete;}));}}}
}
using ThreadIdentity=std::pair<DWORD,std::uint64_t>;
std::vector<ThreadIdentity> applicationThreads()
{
    using Query=LONG(WINAPI*)(HANDLE,ULONG,PVOID,ULONG,PULONG);
    const auto system=GetModuleHandleW(L"ntdll.dll");
    const auto query=reinterpret_cast<Query>(GetProcAddress(system,"NtQueryInformationThread"));
    CHECK(system&&query);std::vector<ThreadIdentity> result;
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);CHECK(snapshot!=INVALID_HANDLE_VALUE);if(snapshot==INVALID_HANDLE_VALUE)return result;
    THREADENTRY32 entry{};entry.dwSize=sizeof(entry);CHECK(Thread32First(snapshot,&entry));
    do{if(entry.th32OwnerProcessID!=GetCurrentProcessId())continue;
        HANDLE h=OpenThread(THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID);CHECK(h);if(!h)continue;
        FILETIME created{},exited{},kernel{},user{};void* start=nullptr;HMODULE module=nullptr;
        const bool known=GetThreadTimes(h,&created,&exited,&kernel,&user)&&query&&query(h,9 /* ThreadQuerySetWin32StartAddress */,&start,sizeof(start),nullptr)==0&&start
            &&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(start),&module);
        CHECK(known);CloseHandle(h);
        // Windows pool workers may retire or grow independently of the clients.
        if(known&&module!=system)result.emplace_back(entry.th32ThreadID,(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime);
    }while(Thread32Next(snapshot,&entry));CloseHandle(snapshot);std::sort(result.begin(),result.end());return result;
}
void thread_lifetime_observer_controls()
{
    const auto baseline=applicationThreads();CHECK(!baseline.empty());
    auto wait=[](std::stop_token stop){while(!stop.stop_requested())Sleep(1);};
    std::jthread first(wait);const auto before=applicationThreads();CHECK(before.size()==baseline.size()+1);
    std::jthread replacement(wait);first.request_stop();first.join();const auto after=applicationThreads();
    CHECK(after.size()==before.size());CHECK(after!=before); // Equal counts must not hide a replacement/retained application thread.
    replacement.request_stop();replacement.join();CHECK(applicationThreads()==baseline);
}
void catalog_reply_and_materialize_capture()
{
    begin(__func__);Pair p;if(!p.open())return;auto r=request(p.client,1);
    RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(airack::test::frozenFactoryRackDesigns(),{},{},snapshot));RackCatalogPage page;
    CHECK(catalogPage(snapshot,{},0,65536,page));CHECK(page.complete);auto authority=std::make_shared<ai::CatalogAuthority>();
    authority->root=r.tag;authority->snapshot=snapshot.id;authority->descriptorJson=juce::JSON::toString(rackCatalogPageDescriptor(page),true,22).toStdString();
    r.catalog.emplace();r.catalog->root=r.tag;r.catalog->authority=authority;CHECK(p.accepted(r));
    const auto factory=airack::test::frozenFactoryRackDesigns().front();juce::MemoryBlock canonical;CHECK(encodeRackDesign(factory,canonical,RackValidationMode::instanceRecall));
    ai::GenerationResult selected;selected.tag=r.tag;selected.status=ai::GenerationStatus::catalogSelectionRequired;selected.inferenceCalls=3;
    selected.proposalStatus=ai::ProposalStatus::accepted;selected.providerStatus=rack::oauth::Status::connected;selected.httpStatus=200;
    selected.catalog=ai::CatalogResultBinding{r.tag,snapshot.id};selected.edits.characteristics="warm";selected.edits.explanation="native grounding";
    selected.edits.catalogSelections.push_back({9,factory.id,factory.revision,juce::SHA256(canonical).toHexString(),{RackRole::synth,masterChain,0}});
    Frame frame{MessageKind::result,p.session,{}};CHECK(encodeResult(r,selected,frame.payload));CHECK(p.send(frame));ClientGenerationReply out;
    CHECK(take(p.client,out));CHECK(std::holds_alternative<ai::GenerationResult>(out));if(const auto* g=std::get_if<ai::GenerationResult>(&out)){
        CHECK(g->status==ai::GenerationStatus::catalogSelectionRequired && g->inferenceCalls==3 && !g->candidate);
        CHECK(g->catalog==selected.catalog && g->edits.catalogSelections==selected.edits.catalogSelections);
        CHECK(g->audio.left.empty() && g->audio.right.empty());}
    CHECK(!p.client.takeGeneration(out));CHECK(p.client.status().status==RuntimeStatus::ready);
    // Taking the intermediate consumes the protected wrapper's original capture.
    p.client.cancelGeneration();CHECK(!p.receive(frame));
    auto next=r;next.tag.requestId=2;auto& context=*next.catalog;context.phase=ai::CatalogPhase::materialize;
    context.selectionParent=r.tag;context.baseIntent=selected.edits;context.inferenceCallsUsed=3;context.providerStatus=selected.providerStatus;context.httpStatus=200;
    auto definitions=std::make_shared<ai::CatalogAuthority>(*authority);definitions->definitions.push_back({factory.id,factory.revision,juce::SHA256(canonical).toHexString(),canonical});
    context.authority=std::move(definitions);CHECK(p.client.generate(next));CHECK(!p.client.generate(next));
    CHECK(observe([&]{p.pump();return p.receive(frame);}));ai::GenerationRequest decoded;
    CHECK(frame.kind==MessageKind::generate && decodeRequest(bytes(frame.payload),decoded));CHECK(decoded.catalog && decoded.tag==next.tag);
    if(decoded.catalog){CHECK(decoded.catalog->root==r.tag && decoded.catalog->selectionParent==r.tag && decoded.catalog->inferenceCallsUsed==3);
        CHECK(decoded.catalog->authority && *decoded.catalog->authority==*next.catalog->authority);
        CHECK(decoded.catalog->baseIntent.catalogSelections==selected.edits.catalogSelections);CHECK(decoded.source==r.source);}
    p.client.cancelGeneration();CHECK(observe([&]{p.pump();return p.receive(frame);}));ControlMessage cancelled;
    CHECK(frame.kind==MessageKind::cancel && decodeControl(bytes(frame.payload),cancelled) && cancelled.tag==next.tag);
    CHECK(p.send(control(MessageKind::failure,p.session,next.tag,RuntimeStatus::cancelled)));
    CHECK(observe([&]{p.pump();return p.client.status().status==RuntimeStatus::ready;}));CHECK(!p.client.takeGeneration(out));
    p.client.close();CHECK(p.client.session().isNull());
}
void lifetime_capacity_and_deployment(const juce::File& helper,const juce::File& dll,const juce::File& directory)
{
    begin(__func__);thread_lifetime_observer_controls();static_assert(!std::is_copy_constructible_v<CompanionClient> && !std::is_copy_assignable_v<CompanionClient> && !std::is_move_constructible_v<CompanionClient> && !std::is_move_assignable_v<CompanionClient>);
    {Pair p;if(p.open()){auto r=request(p.client,1);CHECK(p.accepted(r));auto f=resultFrame(r,p.session,true);CHECK(f.payload.getSize()<=maximumResultPayloadBytes);const auto timer=std::chrono::steady_clock::now();CHECK(p.send(f));ClientGenerationReply out;CHECK(take(p.client,out));const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-timer).count();CHECK(std::holds_alternative<ai::GenerationResult>(out));if(const auto* g=std::get_if<ai::GenerationResult>(&out)){const auto expected=result(r,true);CHECK(g->audio.left.size()==576000 && g->audio.right.size()==576000);CHECK(std::memcmp(g->audio.left.data(),expected.audio.left.data(),576000*sizeof(float))==0);CHECK(std::memcmp(g->audio.right.data(),expected.audio.right.data(),576000*sizeof(float))==0);CHECK(g->candidate==expected.candidate && g->tag==r.tag);}std::cout<<"maximum_payload_bytes="<<f.payload.getSize()<<" matching_transfer_poll_decode_ms="<<ms<<" logical_processors="<<std::thread::hardware_concurrency()<<" test_image="<<juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName()<<std::endl;}}
    {Service service;Temp t;CompanionClient other;if(ready(other,options(t,service.name))){const auto threads=applicationThreads();CHECK(!threads.empty());for(std::uint64_t i=1;i<=24;++i){auto c=std::make_unique<CompanionClient>();if(ready(*c,options(t,service.name))){CHECK(c->generate(request(*c,i)));CHECK(airack::companion::detail::CompanionClientTestAccess::hasStartedWrite(*c));c.reset();CHECK(other.status().phase==ClientPhase::ready);}}CHECK(applicationThreads()==threads);CHECK(other.account({AccountAction::status,{}}));AccountReply q;CHECK(take(other,q));CHECK(airack::companion::detail::PipeTestAccess::retainedOwners()==0);}}

    {Temp t;const auto name=suffix();const auto previous=currentUserPipeName(name).replace("AIRackSynth-v9-","AIRackSynth-v8-");CHECK(previous!=currentUserPipeName(name));HANDLE older=CreateNamedPipeW(previous.toWideCharPointer(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,1,65536,65536,0,nullptr);CHECK(older!=INVALID_HANDLE_VALUE);if(older!=INVALID_HANDLE_VALUE){OVERLAPPED pending{};pending.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);CHECK(pending.hEvent);const bool connected=ConnectNamedPipe(older,&pending)!=FALSE;CHECK(!connected&&GetLastError()==ERROR_IO_PENDING);{CompanionClient current;Child child;const bool matched=ready(current,options(t,name,helper,true));CHECK(matched);if(matched){CHECK(child.duplicate(current));CHECK(currentUserPipeName(name).contains("AIRackSynth-v9-"));CHECK(WaitForSingleObject(pending.hEvent,0)==WAIT_TIMEOUT);CHECK(GetNamedPipeInfo(older,nullptr,nullptr,nullptr,nullptr)!=FALSE);current.close();CHECK(child.alive());}}CancelIoEx(older,&pending);DWORD retired=0;CHECK(GetOverlappedResult(older,&pending,&retired,TRUE)==FALSE&&GetLastError()==ERROR_OPERATION_ABORTED);CHECK(GetNamedPipeInfo(older,nullptr,nullptr,nullptr,nullptr)!=FALSE);CloseHandle(older);if(pending.hEvent)CloseHandle(pending.hEvent);}std::cout<<"v9 real matched helper launch; synthetic v8 namespace remained open/unconnected; no existing helper terminated\n";}
    const auto deployed=directory.getChildFile(companionExecutableName);CHECK(deployed.existsAsFile());if(deployed.existsAsFile()){juce::MemoryBlock a,b;CHECK(helper.loadFileAsData(a));CHECK(deployed.loadFileAsData(b));CHECK(a==b);}
    {Temp t;const auto copy=t.root.getChildFile("fixture.dll"),sidecar=t.root.getChildFile(companionExecutableName);CHECK(dll.copyFileTo(copy));CHECK(helper.copyFileTo(sidecar));HMODULE module=LoadLibraryW(copy.getFullPathName().toWideCharPointer());CHECK(module!=nullptr);if(module){using Fn=std::uint32_t(__cdecl*)(wchar_t*,std::uint32_t);const auto fn=reinterpret_cast<Fn>(GetProcAddress(module,"rackSynthFixtureCompanionPath"));CHECK(fn!=nullptr);if(fn){const auto n=fn(nullptr,0);CHECK(n>1 && n<=32768);if(n>1 && n<=32768){std::vector<wchar_t> text(n);CHECK(fn(text.data(),n)==n);CHECK(juce::File(juce::String(text.data()))==sidecar);}}CHECK(FreeLibrary(module)!=FALSE);}}
}
void spectrum_capability_and_legacy_fallback()
{
 begin(__func__);
 {Pair p;CHECK(p.open([&](Frame& frame){CHECK(encodeControl({{p.client.runtime(),0,0,0},RuntimeStatus::ready,spectrumCapability},frame.payload));},true));CHECK(observe([&]{p.pump();return p.client.status().phase==ClientPhase::ready;}));CHECK(p.client.capabilities()==spectrumCapability);
  auto command=std::make_unique<GroupCommand>();command->operationId=1;command->action=GroupAction::spectrumRead;command->runtime=GroupUuid(p.client.runtime());command->loadRevision=1;CHECK(p.client.group(*command));Frame sent;CHECK(observe([&]{p.pump();return p.receive(sent);}));CHECK(sent.kind==MessageKind::spectrumCommand);
  GroupReply reply;reply.operationId=1;reply.action=command->action;reply.runtime=command->runtime;reply.loadRevision=1;Frame response{MessageKind::spectrumReply,p.session,{}};CHECK(encodeGroupReply(reply,response.payload));CHECK(p.send(response));GroupReply received;CHECK(observe([&]{p.pump();return p.client.takeGroup(received);}));CHECK(received==reply);p.client.close();CHECK(p.client.capabilities()==0);
 }
 {Pair p;auto startOptions=options(p.temp,p.name);startOptions.negotiateSpectrum=true;CHECK(p.client.start(startOptions));CHECK(observe([&]{p.pump();return p.listener.pollAccept(p.pipe)==PipeStatus::complete;}));Frame hello;CHECK(observe([&]{p.pump();return p.receive(hello);}));ControlMessage offered;CHECK(decodeControl(bytes(hello.payload),offered)&&offered.capabilities==spectrumCapability);
  // An old decoder closes the capability hello; retry once to this same verified process.
  p.pipe.close();p.reading=false;CHECK(observe([&]{p.pump();return p.listener.pollAccept(p.pipe)==PipeStatus::complete;}));CHECK(observe([&]{p.pump();return p.receive(hello);}));CHECK(hello.payload.getSize()==41&&decodeControl(bytes(hello.payload),offered)&&offered.capabilities==0);
  CHECK(p.send(control(MessageKind::welcome,p.session,{p.client.runtime(),0,0,0})));CHECK(observe([&]{p.pump();return p.client.status().phase==ClientPhase::ready;}));CHECK(p.client.capabilities()==0);
  auto denied=std::make_unique<GroupCommand>();denied->operationId=1;denied->action=GroupAction::spectrumRead;denied->runtime=GroupUuid(p.client.runtime());denied->loadRevision=1;CHECK(!p.client.group(*denied));CHECK(p.client.account({AccountAction::status,{}}));Frame account;CHECK(observe([&]{p.pump();return p.receive(account);}));CHECK(account.kind==MessageKind::account);
 }
 {auto now=std::chrono::steady_clock::time_point{};Pair p([&]{return now;});auto startOptions=options(p.temp,p.name);startOptions.negotiateSpectrum=true;CHECK(p.client.start(startOptions));CHECK(observe([&]{p.pump();return p.listener.pollAccept(p.pipe)==PipeStatus::complete;}));Frame hello;CHECK(observe([&]{p.pump();return p.receive(hello);}));p.pipe.close();p.reading=false;p.pump();now+=std::chrono::milliseconds(clientStartupTimeoutMs+1);CHECK(observe([&]{p.pump();return p.client.status().phase==ClientPhase::failed;}));CHECK(p.client.status().errorCode=="startup_timeout"&&p.client.capabilities()==0);}
 {Pair p;CHECK(p.open([](Frame& frame){frame.kind=MessageKind::pong;},true));CHECK(p.failed());CHECK(p.client.capabilities()==0);}

}

struct LocalFixture
{
    ai::LocalGenerationRequest input;
    ai::PreviewEvaluate evaluation;
    ai::PreviewReceipt receipt;
    ai::LocalGenerationResult terminal;
    LocalFixture(const CompanionClient& client,std::uint64_t id=1)
    {
        input.request=request(client,id);input.local.root=input.request.tag;input.local.reset=19;
        CHECK(ai::digestPreviewMap(input.local.sourceMap,input.local.sourceMapDigest));CHECK(ai::validLocalPreviewContext(input));
        evaluation.edits.parameters.push_back({"output.gain_db",.4f});
        auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
        CHECK(ai::admitProposal(input.request.source,input.request.policy,evaluation.edits,*candidate,trace.get())==ai::ProposalStatus::accepted);
        evaluation.context=ai::previewContextFor(input.request,evaluation.edits.pattern);
        auto rendered=ai::renderPreview(*candidate,evaluation.context);CHECK(rendered.status==ai::RenderStatus::ready);
        auto& b=evaluation.binding;b.request=input.request.tag;b.root=input.local.root;b.job=41;b.ticket=101;b.reset=input.local.reset;
        b.sourceWord=input.local.sourceWord;b.structuralGeneration=input.request.structuralGeneration;b.providerAttempt=b.renderOrdinal=1;b.sourceMap=input.local.sourceMapDigest;
        ai::PreviewSampleMap staged;CHECK(ai::stagePreviewMap(input.local.sourceMap,*trace,staged));CHECK(ai::digestPreviewMap(staged,b.stagedMap));
        CHECK(ai::digestPreviewCandidate(*candidate,b.candidate));CHECK(ai::digestPreviewContext(evaluation.context,b.context));
        receipt.binding=b;receipt.status=rendered.status;receipt.measurements=rendered.measurements;receipt.bodyFrames=std::uint32_t(rendered.audio.bodyFrames);
        receipt.frames=std::uint32_t(rendered.audio.left.size());receipt.phraseTruncated=rendered.audio.phraseTruncated;CHECK(ai::validPreviewReceipt(receipt));
        auto& n=terminal.numeric;n.tag=input.request.tag;n.status=ai::GenerationStatus::ready;n.inferenceCalls=1;n.proposalStatus=ai::ProposalStatus::accepted;
        n.renderStatus=ai::RenderStatus::ready;n.providerStatus=rack::oauth::Status::connected;n.httpStatus=200;n.edits=evaluation.edits;n.candidate=*candidate;n.measurements=receipt.measurements;
        terminal.rendered=receipt;CHECK(ai::validateLocalGenerationResult(input,terminal));
    }
    Frame offered(juce::Uuid session){Frame f{MessageKind::previewEvaluate,session,{}};CHECK(encodePreviewEvaluate(evaluation,f.payload));return f;}
    Frame result(juce::Uuid session){Frame f{MessageKind::resultLocal,session,{}};CHECK(encodeLocalResult(input,terminal,f.payload));return f;}
};
bool localOpen(Pair& p)
{
    if(!p.open([&](Frame& frame){CHECK(encodeControl({{p.client.runtime(),0,0,0},RuntimeStatus::ready,ownedLocalPreviewCapability},frame.payload));},false,true))return false;
    CHECK(observe([&]{p.pump();return p.client.status().phase==ClientPhase::ready;}));CHECK(p.client.capabilities()==ownedLocalPreviewCapability);
    return p.client.status().phase==ClientPhase::ready;
}
bool localAccepted(Pair& p,const ai::LocalGenerationRequest& input)
{
    const bool accepted=p.client.generateLocal(input);CHECK(accepted);if(!accepted)return false;
    Frame sent;CHECK(observe([&]{p.pump();return p.receive(sent);}));auto decoded=std::make_unique<ai::LocalGenerationRequest>();
    CHECK(sent.kind==MessageKind::generateLocal&&decodeLocalRequest(bytes(sent.payload),*decoded));CHECK(decoded->request.tag==input.request.tag&&decoded->local==input.local);
    CHECK(observe([&]{p.pump();return !airack::companion::detail::CompanionClientTestAccess::hasStartedWrite(p.client);}));return sent.kind==MessageKind::generateLocal;
}
void local_preview_client_receipts()
{
    begin(__func__);
    {Pair p;if(p.open()){auto local=std::make_unique<LocalFixture>(p.client);CHECK(!p.client.generateLocal(local->input));CHECK(p.client.status().phase==ClientPhase::ready);}}
    for(int mode=0;mode<7;++mode){Pair p;if(!localOpen(p))continue;auto local=std::make_unique<LocalFixture>(p.client);if(!localAccepted(p,local->input))continue;
        CHECK(!p.client.generateLocal(local->input));auto newer=request(p.client,2);CHECK(!p.client.generate(newer));
        if(mode==3){auto forged=local->evaluation;forged.binding.candidate[0]^=1;Frame f{MessageKind::previewEvaluate,p.session,{}};CHECK(encodePreviewEvaluate(forged,f.payload));CHECK(p.send(f));CHECK(p.failed());continue;}
        if(mode==4){CHECK(p.send(local->result(p.session)));CHECK(p.failed());continue;}
        CHECK(p.send(local->offered(p.session)));auto evaluation=std::make_unique<ai::PreviewEvaluate>();CHECK(observe([&]{p.pump();return p.client.takePreviewEvaluation(*evaluation);}));CHECK(evaluation->binding==local->evaluation.binding);
        CHECK(!p.client.takePreviewEvaluation(*evaluation));CHECK(p.send(local->offered(p.session)));CHECK(!p.client.takePreviewEvaluation(*evaluation));
        if(mode==1){p.client.cancelGeneration();CHECK(!p.client.submitPreviewReceipt(local->receipt));Frame cancel;CHECK(observe([&]{p.pump();return p.receive(cancel);}));ControlMessage control;
            CHECK(cancel.kind==MessageKind::cancel&&decodeControl(bytes(cancel.payload),control)&&control.tag==local->input.request.tag);
            // A valid older canceled prefix is discarded before nested allocation.
            auto old=local->offered(p.session);old.payload.setSize(42);CHECK(p.send(old));CHECK(p.client.status().phase==ClientPhase::ready);
            CHECK(p.send(::control(MessageKind::failure,p.session,local->input.request.tag,RuntimeStatus::cancelled)));
            CHECK(observe([&]{p.pump();return p.client.status().status==RuntimeStatus::ready;}));CHECK(p.client.generate(newer));continue;}
        if(mode==5){ai::LocalGenerationResult stopped;stopped.numeric.tag=local->input.request.tag;stopped.numeric.status=ai::GenerationStatus::cancelled;
            Frame f{MessageKind::resultLocal,p.session,{}};CHECK(encodeLocalResult(local->input,stopped,f.payload));CHECK(p.send(f));auto reply=std::make_unique<ClientGenerationReply>();CHECK(take(p.client,*reply));
            CHECK(std::holds_alternative<ai::LocalGenerationResult>(*reply));CHECK(!p.client.submitPreviewReceipt(local->receipt));continue;}
        auto wrong=local->receipt;wrong.binding.ticket++;CHECK(!p.client.submitPreviewReceipt(wrong));wrong=local->receipt;wrong.bodyFrames--;CHECK(!p.client.submitPreviewReceipt(wrong));
        CHECK(p.client.submitPreviewReceipt(local->receipt));CHECK(!p.client.submitPreviewReceipt(local->receipt));Frame sent;CHECK(observe([&]{p.pump();return p.receive(sent);}));ai::PreviewReceipt measured;
        CHECK(sent.kind==MessageKind::previewMeasured&&decodePreviewReceipt(bytes(sent.payload),measured)&&ai::samePreviewReceipt(local->receipt,measured));
        if(mode==2){local->terminal.numeric.measurements.peak=.9;local->terminal.rendered->measurements.peak=.9;CHECK(ai::validateLocalGenerationResult(local->input,local->terminal));CHECK(p.send(local->result(p.session)));CHECK(p.failed());continue;}
        if(mode==6){local->terminal.rendered->binding.ticket++;CHECK(p.send(local->result(p.session)));CHECK(p.failed());continue;}
        CHECK(p.send(local->result(p.session)));auto reply=std::make_unique<ClientGenerationReply>();CHECK(take(p.client,*reply));CHECK(std::holds_alternative<ai::LocalGenerationResult>(*reply));
        if(const auto* received=std::get_if<ai::LocalGenerationResult>(reply.get())){CHECK(received->numeric.audio.left.empty()&&received->numeric.audio.right.empty());CHECK(received->rendered&&ai::samePreviewReceipt(*received->rendered,local->receipt));}
        CHECK(p.client.generate(newer));
    }
    {auto now=std::chrono::steady_clock::time_point{};Pair p([&]{return now;});if(localOpen(p)){auto local=std::make_unique<LocalFixture>(p.client);if(localAccepted(p,local->input)){
        for(unsigned i=1;i<=3;++i){local->evaluation.binding.renderOrdinal=local->evaluation.binding.providerAttempt=std::uint8_t(i);local->evaluation.binding.ticket=100+i;local->receipt.binding=local->evaluation.binding;
            now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(100000*i);CHECK(p.send(local->offered(p.session)));auto evaluation=std::make_unique<ai::PreviewEvaluate>();CHECK(observe([&]{p.pump();return p.client.takePreviewEvaluation(*evaluation);}));
            CHECK(p.client.submitPreviewReceipt(local->receipt));Frame sent;CHECK(observe([&]{p.pump();return p.receive(sent);}));CHECK(sent.kind==MessageKind::previewMeasured);}
        now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(clientOperationTimeoutMs-1);p.pump();CHECK(p.client.status().phase==ClientPhase::ready);
        now+=std::chrono::milliseconds(1);p.pump();CHECK(p.client.status().phase==ClientPhase::failed&&p.client.status().errorCode=="request_timeout");
    }}}
    {Pair p;auto o=options(p.temp,p.name);o.negotiateOwnedPreview=true;CHECK(p.client.start(o));CHECK(observe([&]{p.pump();return p.listener.pollAccept(p.pipe)==PipeStatus::complete;}));Frame hello;CHECK(observe([&]{p.pump();return p.receive(hello);}));
        ControlMessage offered;CHECK(decodeControl(bytes(hello.payload),offered)&&offered.capabilities==ownedLocalPreviewCapability);p.pipe.close();p.reading=false;
        CHECK(observe([&]{p.pump();return p.client.status().phase==ClientPhase::failed;}));CHECK(p.client.status().errorCode=="pipe_unavailable");}
}

// Catalog materialization is the same owned operation; selecting a rack must
// not grant another six-minute waiting interval. A distinct root gets its own.
void local_catalog_materialize_keeps_deadline()
{
    begin(__func__);
    for(unsigned mode=0;mode<7;++mode){
        const bool newRoot=mode==1;std::cout<<"LOCAL_CATALOG_CASE mode="<<mode<<std::endl;
        auto now=std::chrono::steady_clock::time_point{};Pair p([&]{return now;});if(!localOpen(p))continue;
        auto input=std::make_unique<ai::LocalGenerationRequest>();input->request=request(p.client,1);
        RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(airack::test::frozenFactoryRackDesigns(),{},{},snapshot));RackCatalogPage page;
        CHECK(catalogPage(snapshot,{},0,65536,page)&&page.complete);
        auto authority=std::make_shared<ai::CatalogAuthority>();authority->root=input->request.tag;authority->snapshot=snapshot.id;
        authority->rendering=ai::CatalogRendering::ownedLocal;authority->descriptorJson=juce::JSON::toString(rackCatalogPageDescriptor(page),true,22).toStdString();
        input->request.catalog.emplace();input->request.catalog->root=input->request.tag;input->request.catalog->authority=authority;
        input->local.root=input->request.tag;input->local.catalogSnapshot=snapshot.id;input->local.reset=19;
        CHECK(ai::digestPreviewMap(input->local.sourceMap,input->local.sourceMapDigest));const bool valid=ai::validLocalPreviewContext(*input);CHECK(valid);if(!valid)continue;
        if(!localAccepted(p,*input))continue;
        const auto design=airack::test::frozenFactoryRackDesigns().front();juce::MemoryBlock canonical;CHECK(encodeRackDesign(design,canonical,RackValidationMode::instanceRecall));
        auto selected=std::make_unique<ai::LocalGenerationResult>();auto& n=selected->numeric;n.tag=input->request.tag;n.status=ai::GenerationStatus::catalogSelectionRequired;
        n.inferenceCalls=1;n.proposalStatus=ai::ProposalStatus::accepted;n.providerStatus=rack::oauth::Status::connected;n.httpStatus=200;
        n.catalog=ai::CatalogResultBinding{input->request.catalog->root,snapshot.id};n.edits.characteristics="Warm grouped voice";n.edits.explanation="Choose the captured native definition";
        n.edits.catalogSelections.push_back({7,design.id,design.revision,juce::SHA256(canonical).toHexString(),{RackRole::synth,masterChain,0}});
        const bool admitted=ai::validateLocalGenerationResult(*input,*selected);CHECK(admitted);if(!admitted)continue;
        Frame selection{MessageKind::resultLocal,p.session,{}};const bool encoded=encodeLocalResult(*input,*selected,selection.payload);CHECK(encoded);if(!encoded)continue;
        now+=std::chrono::milliseconds(300000);CHECK(p.send(selection));
        if(mode==6){for(unsigned i=0;i<=clientPollBudget;++i)p.pump();now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(clientOperationTimeoutMs+1);p.pump();CHECK(p.client.status().phase==ClientPhase::ready);}
        auto reply=std::make_unique<ClientGenerationReply>();const bool consumed=take(p.client,*reply);CHECK(consumed);
        CHECK(consumed&&std::holds_alternative<ai::LocalGenerationResult>(*reply));if(!consumed)continue;
        if(const auto* actual=std::get_if<ai::LocalGenerationResult>(reply.get()))CHECK(actual->numeric.status==ai::GenerationStatus::catalogSelectionRequired&&!actual->rendered);
        CHECK(p.client.status().phase==ClientPhase::ready);
        auto next=std::make_unique<ai::LocalGenerationRequest>(*input);next->request.tag.requestId=2;
        auto nextAuthority=std::make_shared<ai::CatalogAuthority>(*authority);
        if(newRoot){
            next->request.catalog->root=next->request.tag;nextAuthority->root=next->request.tag;next->local.root=next->request.tag;
        }else{
            auto& catalog=*next->request.catalog;catalog.phase=ai::CatalogPhase::materialize;catalog.selectionParent=input->request.tag;
            catalog.baseIntent=n.edits;catalog.inferenceCallsUsed=1;catalog.providerStatus=n.providerStatus;catalog.httpStatus=200;
            nextAuthority->definitions.push_back({design.id,design.revision,juce::SHA256(canonical).toHexString(),canonical});
        }
        next->request.catalog->authority=std::move(nextAuthority);CHECK(ai::validLocalPreviewContext(*next));
        now+=std::chrono::milliseconds(10000);
        if(mode==5){
            now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(clientOperationTimeoutMs-1);p.pump();CHECK(p.client.status().phase==ClientPhase::ready);
            now+=std::chrono::milliseconds(1);p.pump();CHECK(p.client.status().phase==ClientPhase::failed&&p.client.status().errorCode=="request_timeout");continue;
        }
        if(mode==2){
            auto wrong=std::make_unique<ai::LocalGenerationRequest>(*next);wrong->request.tag.requestId=3;
            wrong->request.catalog->selectionParent=next->request.tag;juce::MemoryBlock validWire;
            const bool validMismatch=encodeLocalRequest(*wrong,validWire);CHECK(validMismatch);if(!validMismatch)continue;
            const bool refused=!p.client.generateLocal(*wrong);CHECK(refused&&p.client.status().phase==ClientPhase::ready);if(!refused)continue;
            // Refusal has not advanced highWater or consumed the exact selected continuation.
            next->request.tag.requestId=3;
        }
        if(mode==3||mode==6){
            if(mode==3)now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(clientOperationTimeoutMs+1);
            const bool refused=!p.client.generateLocal(*next);const auto state=p.client.status();
            std::cout<<"LOCAL_CATALOG_EXPIRED_DISPATCH refused="<<refused<<" phase="<<int(state.phase)<<" error="<<state.errorCode<<std::endl;
            CHECK(refused&&state.phase==ClientPhase::failed&&state.errorCode=="request_timeout");
            if(refused){Frame unexpected;CHECK(!p.receive(unexpected));}continue;
        }
        if(mode==4){
            p.client.cancelGeneration();Frame cancelled;const bool sent=observe([&]{p.pump();return p.receive(cancelled);});CHECK(sent);if(!sent)continue;
            ControlMessage command;const bool validCancel=cancelled.kind==MessageKind::cancel&&decodeControl(bytes(cancelled.payload),command)&&command.tag==input->request.tag;
            CHECK(validCancel);if(!validCancel)continue;
            CHECK(p.send(control(MessageKind::failure,p.session,command.tag,RuntimeStatus::cancelled)));
            CHECK(observe([&]{p.pump();return p.client.status().phase==ClientPhase::ready&&p.client.status().status==RuntimeStatus::ready;}));
            now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(clientOperationTimeoutMs+1);p.pump();CHECK(p.client.status().phase==ClientPhase::ready);
            CHECK(!p.client.generateLocal(*next)&&p.client.status().phase==ClientPhase::ready);
            auto fresh=std::make_unique<ai::LocalGenerationRequest>(*input);fresh->request.tag.requestId=2;
            auto freshAuthority=std::make_shared<ai::CatalogAuthority>(*authority);freshAuthority->root=fresh->request.tag;
            fresh->request.catalog->root=fresh->request.tag;fresh->request.catalog->authority=std::move(freshAuthority);fresh->local.root=fresh->request.tag;
            CHECK(ai::validLocalPreviewContext(*fresh));CHECK(localAccepted(p,*fresh));continue;
        }
        if(!localAccepted(p,*next))continue;
        now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(clientOperationTimeoutMs-1);p.pump();CHECK(p.client.status().phase==ClientPhase::ready);
        now+=std::chrono::milliseconds(1);p.pump();const auto state=p.client.status();
        std::cout<<"LOCAL_CATALOG_CLOCK new_root="<<newRoot<<" original_elapsed="<<clientOperationTimeoutMs<<" phase="<<int(state.phase)<<" error="<<state.errorCode<<std::endl;
        if(newRoot){CHECK(state.phase==ClientPhase::ready);now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(310000+clientOperationTimeoutMs);p.pump();}
        CHECK(p.client.status().phase==ClientPhase::failed&&p.client.status().errorCode=="request_timeout");
    }
}

// An explicit child after an accepted ready receipt starts its own window,
// even when it retains the same local root (the non-catalog Refine shape).
void local_refinement_starts_deadline()
{
    begin(__func__);auto now=std::chrono::steady_clock::time_point{};Pair p([&]{return now;});if(!localOpen(p))return;
    auto local=std::make_unique<LocalFixture>(p.client);if(!localAccepted(p,local->input))return;
    now+=std::chrono::milliseconds(300000);CHECK(p.send(local->offered(p.session)));auto evaluation=std::make_unique<ai::PreviewEvaluate>();
    CHECK(observe([&]{p.pump();return p.client.takePreviewEvaluation(*evaluation);}));CHECK(evaluation->binding==local->evaluation.binding);
    CHECK(p.client.submitPreviewReceipt(local->receipt));Frame sent;CHECK(observe([&]{p.pump();return p.receive(sent);}));CHECK(sent.kind==MessageKind::previewMeasured);
    CHECK(p.send(local->result(p.session)));auto reply=std::make_unique<ClientGenerationReply>();const bool consumed=take(p.client,*reply);CHECK(consumed);
    CHECK(consumed&&std::holds_alternative<ai::LocalGenerationResult>(*reply));if(!consumed)return;
    auto child=std::make_unique<ai::LocalGenerationRequest>(local->input);child->request.tag.requestId=2;child->request.instruction="Refine accepted parent";
    child->request.source=*local->terminal.numeric.candidate;child->request.policy.requirePattern=false;CHECK(child->local.root==local->input.request.tag&&child->local.root!=child->request.tag);
    CHECK(ai::validLocalPreviewContext(*child));now+=std::chrono::milliseconds(10000);if(!localAccepted(p,*child))return;
    now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(clientOperationTimeoutMs);p.pump();CHECK(p.client.status().phase==ClientPhase::ready);
    now=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(310000+clientOperationTimeoutMs-1);p.pump();CHECK(p.client.status().phase==ClientPhase::ready);
    now+=std::chrono::milliseconds(1);p.pump();CHECK(p.client.status().phase==ClientPhase::failed&&p.client.status().errorCode=="request_timeout");
}

}
int main(int argc,char** argv)
{
    if(argc==2&&juce::String(argv[1])=="--local-preview"){local_preview_client_receipts();local_catalog_materialize_keeps_deadline();local_refinement_starts_deadline();finishGroup();std::cout<<"checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
    if(argc==2&&juce::String(argv[1])=="--spectrum"){spectrum_capability_and_legacy_fallback();finishGroup();std::cout<<"checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;}
    if(argc!=4){std::cerr<<"expected helper DLL module-directory arguments"<<std::endl;return 2;}
    try{local_preview_client_receipts();local_catalog_materialize_keeps_deadline();local_refinement_starts_deadline();const juce::File helper{juce::String(argv[1])},dll{juce::String(argv[2])},directory{juce::String(argv[3])};spectrum_capability_and_legacy_fallback();startup_reuse_and_authority(helper);bounded_account_and_cancellation(helper);newest_generation_and_strict_receipt();catalog_reply_and_materialize_capture();late_and_slow_polls();lifetime_capacity_and_deployment(helper,dll,directory);}catch(const std::exception& e){CHECK(false);std::cerr<<"synthetic fixture failure: "<<e.what()<<std::endl;}finishGroup();std::cout<<"checks="<<checks<<" groups="<<groups<<" failures="<<failures<<std::endl;return failures?1:0;
}

#include "FrozenRackExamples.h"
#include "CompanionRuntimeServer.h"
#include "GenerationRequest.h"
#include "RackExamples.h"
#include "RoutedSynthEngine.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace airack;
using namespace airack::companion;
namespace
{
int checks=0, failures=0, groups=0, groupChecks=0, groupFailures=0;
const char* group="";
void check(bool pass,const char* text,int line)
{++checks;if(!pass){++failures;std::cerr<<group<<':'<<line<<' '<<text<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void finishGroup(){if(groups)std::cout<<group<<" checks="<<checks-groupChecks<<" failures="<<failures-groupFailures<<std::endl;}
void begin(const char* name){finishGroup();group=name;++groups;groupChecks=checks;groupFailures=failures;std::cout<<name<<std::endl;}
using Access=airack::companion::detail::RuntimeServerTestAccess;
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
juce::String suffix(){return juce::Uuid().toString().removeCharacters("-");}
template<class F> bool observe(F f,std::uint64_t ms=3000)
{const auto end=GetTickCount64()+ms;do{if(f())return true;Sleep(1);}while(GetTickCount64()<end);return false;}
std::uint64_t incarnation(HANDLE p)
{FILETIME a{},b{},c{},d{};if(!GetProcessTimes(p,&a,&b,&c,&d))return 0;return (std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
bool realPeer(PipePeer peer,DWORD expected=0)
{
    if(!peer.processId || (expected && peer.processId!=expected))return false;
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,peer.processId);if(!p)return false;
    const bool valid=peer.creationTime!=0 && peer.creationTime==incarnation(p);CloseHandle(p);return valid;
}
Frame control(MessageKind kind,juce::Uuid session,ai::RequestTag tag,RuntimeStatus status=RuntimeStatus::ready)
{Frame f{kind,session,{}};if(!encodeControl({tag,status},f.payload))throw std::runtime_error("control fixture");return f;}
ai::GenerationRequest request(juce::Uuid runtime,std::uint64_t id,std::string instruction="normal")
{
    ai::GenerationRequest r;r.tag={runtime,id,7,11};r.model="captured-model";r.instruction=std::move(instruction);
    r.capturedPhrase=PatternDocument{};r.capturedPhrase->noteCount=1;r.capturedPhrase->notes[0]={.25,.125,65,87,2};r.capturedBpm=143;
    return r;
}
Frame failure(const RuntimeJob& job)
{
    if(const auto* r=std::get_if<ai::GenerationRequest>(&job.input))return control(MessageKind::failure,job.session,r->tag,RuntimeStatus::unavailable);
    AccountReply reply;reply.status=RuntimeStatus::ready;reply.selectedModel=std::get<AccountCommand>(job.input).model;
    Frame f{MessageKind::accountResult,job.session,{}};if(!encodeAccountReply(reply,f.payload))throw std::runtime_error("account fixture");return f;
}
Frame stress(const RuntimeJob& job)
{
    const auto& r=std::get<ai::GenerationRequest>(job.input);ai::GenerationResult result;result.tag=r.tag;
    result.status=ai::GenerationStatus::providerFailure;result.providerStatus=rack::oauth::Status::networkFailure;
    Frame f{MessageKind::result,job.session,{}};
    if(!encodeResult(r,result,f.payload))throw std::runtime_error("stress prefix");
    const auto prefix=f.payload.getSize();f.payload.setSize(maximumResultPayloadBytes-64);
    std::memset(static_cast<std::byte*>(f.payload.getData())+prefix,0x6d,f.payload.getSize()-prefix);return f;
}
struct Backend
{
    std::atomic_bool release{false}, priority{true}, peer{true}, cancelToken{true};
    std::atomic_int calls{0},active{0},peak{0},cancels{0};
    std::mutex mutex;std::vector<RuntimeJob> jobs,cancelled;
    DWORD expectedPid=GetCurrentProcessId();
    Frame run(const RuntimeJob& job)
    {
        priority.store(priority.load() && GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_BELOW_NORMAL);
        peer.store(peer.load() && realPeer(job.peer,expectedPid));
        const auto n=active.fetch_add(1)+1;peak.store((std::max)(peak.load(),n));calls.fetch_add(1);
        struct Done{std::atomic_int& value;~Done(){value.fetch_sub(1);}} done{active};
        {std::lock_guard lock(mutex);jobs.push_back(job);}
        const auto* r=std::get_if<ai::GenerationRequest>(&job.input);
        const auto name=r?juce::String(r->instruction):std::get<AccountCommand>(job.input).model;
        if(name=="hold")while(!release.load())Sleep(1);
        if(name=="throw")throw std::runtime_error("owned callback exception");
        if(name=="invalid"){auto f=failure(job);f.session=juce::Uuid();return f;}
        if(name=="large")return stress(job);
        return failure(job);
    }
    void cancel(const RuntimeJob& job)
    {
        cancelToken.store(cancelToken.load() && job.cancellation && job.cancellation->cancelled());
        {std::lock_guard lock(mutex);cancelled.push_back(job);}cancels.fetch_add(1);
    }
};
struct Service
{
    std::shared_ptr<Backend> backend=std::make_shared<Backend>();
    std::atomic_bool stop{false},done{false};juce::String name=suffix();
    std::unique_ptr<RuntimeServer> server;RuntimeServerResult result;std::thread io;
    Service(RuntimeRunJob run={},RuntimeCancelJob cancel={})
    {
        if(!run)run=[b=backend](const auto& job){return b->run(job);};
        if(!cancel)cancel=[b=backend](const auto& job){b->cancel(job);};
        server=std::make_unique<RuntimeServer>(std::move(run),std::move(cancel));
        io=std::thread([this]{result=server->run(stop,name);done.store(true);});
    }
    void finish(){stop.store(true);if(io.joinable())io.join();}
    ~Service(){backend->release.store(true);finish();}
};
struct Client
{
    PipeConnection pipe;juce::Uuid runtime=juce::Uuid(),session=juce::Uuid::null();bool reading=false;
    bool connect(const juce::String& name,std::uint64_t ms=2000)
    {return observe([&]{return pipe.connect(name)==PipeStatus::ready;},ms);}
    bool send(const Frame& f,std::uint64_t ms=3000)
    {
        if(pipe.beginWrite(f)!=PipeStatus::ready)return false;
        PipeStatus s=PipeStatus::pending;observe([&]{s=pipe.pollWrite();return s!=PipeStatus::pending;},ms);return s==PipeStatus::complete;
    }
    std::optional<Frame> receive(std::uint64_t ms=8000)
    {
        if(!reading){if(pipe.beginRead()!=PipeStatus::ready)return {};reading=true;}
        Frame f;PipeStatus s=PipeStatus::pending;
        observe([&]{s=pipe.pollRead(f);return s!=PipeStatus::pending;},ms);
        if(s!=PipeStatus::pending)reading=false;
        if(s==PipeStatus::complete)return f;return {};
    }
    bool hello(RuntimeStatus expected=RuntimeStatus::ready,std::uint32_t capabilities=0)
    {
        Frame greeting{MessageKind::hello,juce::Uuid::null(),{}};
        if(!encodeControl({{runtime,0,0,0},RuntimeStatus::ready,capabilities},greeting.payload)||!send(greeting))return false;
        auto f=receive();ControlMessage m;
        if(!f || f->kind!=MessageKind::welcome || f->session.isNull() || !decodeControl(bytes(f->payload),m)
            || m.tag.runtime!=runtime || m.status!=expected || m.capabilities!=(capabilities&knownCapabilityMask))return false;
        session=f->session;return true;
    }
    bool open(const juce::String& name){return connect(name) && hello();}
    bool ping(std::uint64_t ms=1500)
    {
        const auto started=GetTickCount64();if(!send(control(MessageKind::ping,session,{runtime,0,0,0}),ms))return false;
        auto f=receive(ms);ControlMessage m;return f && f->kind==MessageKind::pong && f->session==session
            && decodeControl(bytes(f->payload),m) && m.tag==ai::RequestTag{runtime,0,0,0} && m.status==RuntimeStatus::ready && GetTickCount64()-started<ms;
    }
    bool generate(const ai::GenerationRequest& r)
    {Frame f{MessageKind::generate,session,{}};return encodeRequest(r,f.payload) && send(f);}
    bool account(const juce::String& model="model")
    {Frame f{MessageKind::account,session,{}};return encodeAccountCommand({AccountAction::selectModel,model},f.payload) && send(f);}
    bool replyTag(const ai::RequestTag& tag,RuntimeStatus status=RuntimeStatus::unavailable,std::uint64_t ms=8000)
    {auto f=receive(ms);ControlMessage m;return f && f->kind==MessageKind::failure && f->session==session && decodeControl(bytes(f->payload),m) && m.tag==tag && m.status==status;}
    bool closed(std::uint64_t ms=8000)
    {return observe([&]{if(!pipe.connected())return true;if(!reading){const auto s=pipe.beginRead();if(s!=PipeStatus::ready)return !pipe.connected();reading=true;}Frame f;const auto s=pipe.pollRead(f);if(s!=PipeStatus::pending)reading=false;return !pipe.connected();},ms);}
};
struct Child
{
    juce::ChildProcess process;bool started=false,queuedStop=false;std::uint64_t deadline=0;
    int remaining(std::uint64_t reserve=0) const{auto now=GetTickCount64();auto left=now<deadline?deadline-now:0;return left>reserve?static_cast<int>(left-reserve):0;}
    bool start(juce::StringArray args,bool exceptional=false,const juce::String& executable={})
    {
        juce::StringArray command{executable.isEmpty()?juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName():executable};command.addArray(args);
        queuedStop=args[0]=="queued-stop";const auto duration=queuedStop?10000:(exceptional?80000:60000);
        deadline=GetTickCount64()+duration;
        std::cout<<"child argv="<<juce::JSON::toString(juce::var(command),true)<<" deadlineMs="<<duration<<std::endl;
        started=process.start(command);return started;
    }
    void terminate(){const bool killed=process.kill();const auto left=remaining();const bool ended=left>0 && process.waitForProcessToFinish((std::min)(left,1000));if(queuedStop){std::cout<<"queued-stop ownedKill="<<killed<<" terminationObserved="<<ended;if(ended)std::cout<<" actualExit="<<process.getExitCode();std::cout<<std::endl;}}
    bool finish()
    {
        if(!started)return false;const auto ms=remaining(1000);
        if(ms<=0 || !process.waitForProcessToFinish(ms)){if(queuedStop)std::cout<<"queued-stop ownedTimeout=1"<<std::endl;terminate();return false;}
        auto out=process.readAllProcessOutput();std::cout<<out<<"child actualExit="<<process.getExitCode()<<std::endl;return process.getExitCode()==0;
    }
    ~Child(){if(started && process.isRunning())terminate();}
};
int drumLocalProfileRetention(const juce::String& executable){
    begin("actual_drum_local_profile_callback_retention");const juce::File profile(executable);
    const bool valid=juce::File::isAbsolutePath(executable)&&profile.existsAsFile();CHECK(valid);if(!valid)return 1;
    Child child;CHECK(child.start({"--drum-local-callback-retention-child"},true,profile.getFullPathName()));CHECK(child.finish());
    finishGroup();std::cout<<"actual local callback retention checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;
}
void native_registration_and_limits()
{
    begin(__func__);const auto name=suffix(),eventName="Local\\AIRackTestStop"+suffix();
    HANDLE event=CreateEventW(nullptr,TRUE,FALSE,eventName.toWideCharPointer());CHECK(event!=nullptr);if(!event)return;
    Child child;CHECK(child.start({"server",name,eventName,juce::String(GetCurrentProcessId())}));
    Client healthy;const bool opened=healthy.open(name);CHECK(opened);
    if(opened){
        CHECK(healthy.pipe.peer().processId!=GetCurrentProcessId());CHECK(realPeer(healthy.pipe.peer()));CHECK(healthy.ping());
        Client duplicate;duplicate.runtime=healthy.runtime;CHECK(duplicate.connect(name));CHECK(duplicate.hello(RuntimeStatus::duplicateInstance));CHECK(duplicate.closed());CHECK(healthy.ping());
        const auto first=healthy.session;healthy.pipe.close();Sleep(50);healthy.reading=false;
        CHECK(healthy.open(name));CHECK(healthy.session!=first);CHECK(healthy.ping());
        for(int i=0;i<40;++i){Client one;CHECK(one.open(name));CHECK(one.ping());one.pipe.close();Sleep(25);airack::companion::detail::PipeTestAccess::reapRetired();}
        std::array<Client,15> pending;for(auto& p:pending)CHECK(p.connect(name));Sleep(100);
        Client over;CHECK(over.connect(name));CHECK(over.closed(2000));CHECK(healthy.ping());
        const auto expiry=GetTickCount64();Sleep(5100);for(auto& p:pending)CHECK(p.closed(1000));CHECK(GetTickCount64()-expiry>=runtimeHelloTimeoutMs);CHECK(healthy.ping());
        Client after;CHECK(after.open(name));CHECK(after.ping());CHECK(after.account());auto account=after.receive();AccountReply reply;
        CHECK(account && account->kind==MessageKind::accountResult && decodeAccountReply(bytes(account->payload),reply) && reply.selectedModel=="model");
    }
    SetEvent(event);CHECK(child.finish());CloseHandle(event);airack::companion::detail::PipeTestAccess::reapRetired();CHECK(airack::companion::detail::PipeTestAccess::retainedOwners()==0);
}
void serial_jobs_and_cancellation()
{
    begin(__func__);Service service;Client a,b;const bool opened=a.open(service.name) && b.open(service.name);CHECK(opened);if(!opened)return;
    auto old=request(a.runtime,1,"hold"),replacement=request(a.runtime,2),other=request(b.runtime,1);
    CHECK(a.generate(old));CHECK(observe([&]{return service.backend->active.load()==1;}));
    CHECK(a.generate(replacement));CHECK(observe([&]{return service.backend->cancels.load()==1;}));CHECK(b.ping());CHECK(b.generate(other));
    CHECK(service.backend->active.load()==1);CHECK(service.backend->calls.load()==1);service.backend->release.store(true);
    CHECK(a.replyTag(replacement.tag));CHECK(b.replyTag(other.tag));CHECK(!a.receive(100));
    CHECK(service.backend->peak.load()==1 && service.backend->priority.load() && service.backend->peer.load());CHECK(service.backend->cancelToken.load());
    {std::lock_guard lock(service.backend->mutex);CHECK(service.backend->cancelled.size()==1);if(!service.backend->cancelled.empty())CHECK(std::get<ai::GenerationRequest>(service.backend->cancelled[0].input).tag==old.tag);}
    service.backend->release.store(false);auto explicitOld=request(a.runtime,3,"hold");CHECK(a.generate(explicitOld));CHECK(observe([&]{return service.backend->active.load()==1;}));
    CHECK(a.send(control(MessageKind::cancel,a.session,explicitOld.tag)));CHECK(a.replyTag(explicitOld.tag,RuntimeStatus::cancelled));
    CHECK(observe([&]{return service.backend->cancels.load()==2;}));CHECK(b.ping());service.backend->release.store(true);
    CHECK(observe([&]{return service.backend->active.load()==0;}));CHECK(a.account());auto account=a.receive();CHECK(account && account->kind==MessageKind::accountResult);
    service.backend->release.store(false);auto lost=request(a.runtime,4,"hold");CHECK(a.generate(lost));CHECK(observe([&]{return service.backend->active.load()==1;}));
    auto nonce=a.session;a.pipe.close();Sleep(100);a.reading=false;CHECK(a.open(service.name));CHECK(a.session!=nonce);
    auto fresh=request(a.runtime,1);CHECK(a.generate(fresh));CHECK(b.ping());CHECK(observe([&]{return service.backend->cancels.load()==3;}));
    service.backend->release.store(true);CHECK(a.replyTag(fresh.tag));CHECK(!a.receive(100));
    CHECK(service.backend->peak.load()==1 && service.backend->cancelToken.load());service.finish();CHECK(service.result.status==RuntimeStatus::ready && !service.result.workerRetained);
}
// A request sent as soon as the reply before it arrives: the runtime counts that reply delivered before it reads the
// request, however late its IO runs on after starting the reply's write.
void request_after_its_reply()
{
    begin(__func__);Service service;Client a;const bool opened=a.open(service.name);CHECK(opened);if(!opened)return;
    struct Release{~Release(){Access::pauseAfterReplyStart(false);}} release;
    Access::pauseAfterReplyStart(true);
    CHECK(a.account("first"));auto first=a.receive();AccountReply reply;
    CHECK(first && first->kind==MessageKind::accountResult && decodeAccountReply(bytes(first->payload),reply) && reply.selectedModel=="first");
    CHECK(Access::replyStartPaused());
    CHECK(a.account("second"));
    Access::pauseAfterReplyStart(false);
    auto second=a.receive();
    CHECK(second && second->kind==MessageKind::accountResult && second->session==a.session && decodeAccountReply(bytes(second->payload),reply) && reply.selectedModel=="second");
    CHECK(a.ping());
    service.finish();CHECK(service.result.status==RuntimeStatus::ready && !service.result.workerRetained);
}
void duplex_backpressure_and_delivery()
{
    begin(__func__);Service service;Client slow,healthy;const bool opened=slow.open(service.name) && healthy.open(service.name);CHECK(opened);if(!opened)return;
    auto old=request(slow.runtime,1,"large"),next=request(slow.runtime,2);CHECK(slow.generate(old));CHECK(observe([&]{return service.backend->calls.load()==1 && service.backend->active.load()==0;}));Sleep(300);
    CHECK(slow.generate(next));CHECK(healthy.ping());auto first=slow.receive();
    // Routing stress preserves exact started bytes, without pretending this is admitted audio.
    RuntimeJob original;original.session=slow.session;original.input=old;const auto expected=stress(original);
    if(first){CHECK(first->kind==MessageKind::result && first->session==slow.session);CHECK(first->payload==expected.payload);CHECK(slow.replyTag(next.tag));}
    else CHECK(slow.closed(2000));CHECK(healthy.ping());
    Client timed;CHECK(timed.open(service.name));auto large=request(timed.runtime,1,"large");const auto prior=service.backend->calls.load();CHECK(timed.generate(large));
    CHECK(observe([&]{return service.backend->calls.load()>prior && service.backend->active.load()==0;}));const auto unread=GetTickCount64();Sleep(300);
    CHECK(timed.send(control(MessageKind::cancel,timed.session,large.tag)));CHECK(healthy.ping());
    Client late;CHECK(late.open(service.name));auto delayed=request(late.runtime,1,"large");const auto before=service.backend->calls.load();CHECK(late.generate(delayed));
    CHECK(observe([&]{return service.backend->calls.load()>before && service.backend->active.load()==0;}));Sleep(6000);
    RuntimeJob job;job.session=late.session;job.input=delayed;const auto delivered=late.receive();CHECK(delivered && delivered->kind==MessageKind::result && delivered->payload==stress(job).payload);CHECK(healthy.ping());
    const auto waited=GetTickCount64()-unread;if(waited<runtimeReplyTimeoutMs+1000)Sleep(static_cast<DWORD>(runtimeReplyTimeoutMs+1000-waited));CHECK(timed.closed(1000));CHECK(healthy.ping());
    Client account;CHECK(account.open(service.name));service.backend->release.store(false);CHECK(account.account("hold"));CHECK(observe([&]{return service.backend->active.load()==1;}));
    auto rejected=request(account.runtime,1);CHECK(account.generate(rejected));CHECK(account.replyTag(rejected.tag,RuntimeStatus::busy));CHECK(healthy.ping());service.backend->release.store(true);
    auto reply=account.receive();AccountReply decoded;CHECK(reply && reply->kind==MessageKind::accountResult && decodeAccountReply(bytes(reply->payload),decoded) && decoded.selectedModel=="hold");
    CHECK(account.generate(rejected));CHECK(account.replyTag(rejected.tag));CHECK(service.backend->peak.load()==1);service.finish();CHECK(service.result.status==RuntimeStatus::ready);
}
void native_failures_and_expiry()
{
    begin(__func__);std::atomic_bool stopped{true};RuntimeServer empty({},{});CHECK(empty.run(stopped,suffix()).status==RuntimeStatus::unavailable);CHECK(empty.run(stopped,suffix()).status==RuntimeStatus::busy);
    Service service;Client healthy;const bool opened=healthy.open(service.name);CHECK(opened);if(!opened)return;
    RuntimeServer competitor([](const auto& j){return failure(j);},[](const auto&){});CHECK(competitor.run(stopped,suffix()).status==RuntimeStatus::busy);
    for(int mode=0;mode<4;++mode){Client bad;CHECK(bad.open(service.name));Frame frame=control(MessageKind::ping,bad.session,{bad.runtime,0,0,0});
        if(mode==0){frame.kind=MessageKind::account;frame.payload=juce::MemoryBlock("bad",3);}if(mode==1)frame.kind=MessageKind::pong;if(mode==2)frame.session=juce::Uuid();if(mode==3)frame=control(MessageKind::ping,bad.session,{juce::Uuid(),0,0,0});
        CHECK(bad.send(frame));CHECK(bad.closed());CHECK(healthy.ping());}
    auto throwing=request(healthy.runtime,1,"throw");CHECK(healthy.generate(throwing));CHECK(healthy.replyTag(throwing.tag));auto later=request(healthy.runtime,2);CHECK(healthy.generate(later));CHECK(healthy.replyTag(later.tag));
    CHECK(healthy.account("throw"));auto account=healthy.receive();AccountReply reply;CHECK(account && account->kind==MessageKind::accountResult && decodeAccountReply(bytes(account->payload),reply) && reply.status==RuntimeStatus::unavailable);
    Client invalid;CHECK(invalid.open(service.name));CHECK(invalid.generate(request(invalid.runtime,1,"invalid")));CHECK(invalid.closed());CHECK(healthy.ping());
    Client quiet;CHECK(quiet.connect(service.name));const auto start=GetTickCount64();CHECK(quiet.closed(6500));CHECK(GetTickCount64()-start>=4500);CHECK(healthy.ping());
    const auto stopAt=GetTickCount64();service.finish();CHECK(GetTickCount64()-stopAt<1500);CHECK(service.result.status==RuntimeStatus::ready && !service.result.workerRetained);CHECK(service.server->run(stopped,service.name).status==RuntimeStatus::busy);
    CHECK(Access::retainedWorkers()==0 && Access::admissionAvailable());
    auto own=std::make_shared<Backend>();Service cancelThrows([own](const auto& job){return own->run(job);},[own](const auto&){own->cancels.fetch_add(1);own->release.store(true);throw std::runtime_error("cancel");});
    Client c;CHECK(c.open(cancelThrows.name));CHECK(c.generate(request(c.runtime,1,"hold")));CHECK(observe([&]{return own->active.load()==1;}));CHECK(c.send(control(MessageKind::cancel,c.session,{c.runtime,1,7,11})));
    CHECK(observe([&]{return cancelThrows.done.load();}));cancelThrows.finish();CHECK(cancelThrows.result.status==RuntimeStatus::unavailable && !cancelThrows.result.workerRetained);CHECK(own->cancels.load()==1);
}
void captured_generation_handoff()
{
    begin(__func__);auto b=std::make_shared<Backend>();auto callCount=std::make_shared<std::atomic_int>(0);
    Service service([b,callCount](const RuntimeJob& job){
        b->priority.store(GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_BELOW_NORMAL);b->peer.store(realPeer(job.peer,GetCurrentProcessId()));
        const auto& captured=std::get<ai::GenerationRequest>(job.input);
        auto result=ai::generateProposal(captured,[callCount](const juce::String&,std::string_view prompt,const juce::var& schema){
            callCount->fetch_add(1);rack::oauth::StructuredResult r;
            if(prompt.find("143")==std::string_view::npos || !schema.isObject())return r;
            r.result.status=rack::oauth::Status::connected;r.result.httpStatus=200;
            r.text=R"({"version":1,"parameter_edits":[{"id":"output.gain_db","normalized":0.5}],"rack_edits":[],"macro_edits":[],"pattern":null,"characteristics":"warm","explanation":"captured offline edit"})";return r;
        },[token=job.cancellation]{return token->cancelled();});
        Frame f{MessageKind::result,job.session,{}};if(!encodeResult(captured,result,f.payload))throw std::runtime_error("captured encode");return f;
    });
    Client client;const bool opened=client.open(service.name);CHECK(opened);if(!opened)return;const auto captured=request(client.runtime,71);CHECK(client.generate(captured));auto frame=client.receive(20000);ai::GenerationResult result;
    const bool decoded=frame && frame->kind==MessageKind::result && frame->session==client.session && decodeResult(bytes(frame->payload),captured,result);CHECK(decoded);
    if(decoded){CHECK(result.tag==captured.tag);CHECK(result.status==ai::GenerationStatus::ready && result.inferenceCalls==1);CHECK(result.candidate.has_value());CHECK(result.edits.parameters==std::vector<ai::ParameterEdit>({{"output.gain_db",.5f}}));CHECK(result.audio.context.phrase==*captured.capturedPhrase && result.audio.context.bpm==143);
        CHECK(result.measurements.finite && !result.audio.left.empty() && result.audio.left.size()==result.audio.right.size());CHECK(std::all_of(result.audio.left.begin(),result.audio.left.end(),[](float v){return std::isfinite(v);}));ProjectDocument admitted;CHECK(ai::admitProposal(captured.source,captured.policy,result.edits,admitted)==ai::ProposalStatus::accepted && admitted==*result.candidate);}
    CHECK(callCount->load()==1 && b->priority.load() && b->peer.load());service.finish();CHECK(service.result.status==RuntimeStatus::ready);
}
int retentionChild()
{
    begin("retained_worker_shutdown_child");auto owned=std::make_shared<Backend>();std::weak_ptr<Backend> lifetime=owned;
    auto release=std::make_shared<std::atomic_bool>(false);auto exited=std::make_shared<std::atomic_bool>(false);auto entered=std::make_shared<std::atomic_bool>(false);
    std::atomic_bool stop{false};RuntimeServerResult result;const auto name=suffix();
    auto server=std::make_unique<RuntimeServer>([owned,release,exited,entered](const RuntimeJob& job){
        owned->priority.store(GetThreadPriority(GetCurrentThread())==THREAD_PRIORITY_BELOW_NORMAL);entered->store(true);while(!release->load())Sleep(1);auto f=failure(job);exited->store(true);return f;
    },[owned](const auto& job){owned->cancel(job);});
    std::thread io([&]{result=server->run(stop,name);});Client c;const bool opened=c.open(name);CHECK(opened);
    if(opened){CHECK(c.generate(request(c.runtime,1)));CHECK(observe([&]{return entered->load();}));}else CHECK(entered->load());
    const auto start=GetTickCount64();stop.store(true);io.join();
    if(!entered->load()){CHECK(result.workerRetained);finishGroup();return 1;}
    CHECK(GetTickCount64()-start>=runtimeWorkerDrainMs-100);CHECK(GetTickCount64()-start<runtimeWorkerDrainMs+5000);
    CHECK(result.status==RuntimeStatus::unavailable && result.workerRetained);CHECK(Access::retainedWorkers()==1 && !Access::admissionAvailable());CHECK(owned->cancels.load()==1 && owned->cancelToken.load() && owned->priority.load());
    server.reset();owned.reset();CHECK(!lifetime.expired());Access::reapRetained();CHECK(Access::retainedWorkers()==1);
    auto calls=std::make_shared<std::atomic_int>(0);auto fresh=[calls](const RuntimeJob& job){calls->fetch_add(1);return failure(job);};
    RuntimeServer blocked(fresh,[](const auto&){});CHECK(blocked.run(stop,suffix()).status==RuntimeStatus::unavailable);CHECK(calls->load()==0);
    release->store(true);CHECK(observe([&]{return exited->load();}));CHECK(observe([&]{Access::reapRetained();return Access::retainedWorkers()==0;}));
    CHECK(lifetime.expired());CHECK(!Access::admissionAvailable());RuntimeServer after(fresh,[](const auto&){});CHECK(after.run(stop,suffix()).status==RuntimeStatus::unavailable);CHECK(calls->load()==0);
    finishGroup();std::cout<<"retention child checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;
}
void retained_worker_shutdown(){begin(__func__);Child child;CHECK(child.start({"retention"},true));CHECK(child.finish());}
juce::File queuedFixtureRoot()
{
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory()
        .getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("host-fixtures");
}
bool queuedDiagnostic(const juce::File& file,const juce::String& stage,bool pass)
{
    if(!file.isAChildOf(queuedFixtureRoot()) || !file.getFileName().startsWith("runtime-queued-stop-"))return false;
    SYSTEMTIME utc{};GetSystemTime(&utc);
    const auto line=juce::String::formatted("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ ",utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds)
        +stage+"="+(pass?"1":"0")+"\n";
    if(file.getSize()+static_cast<juce::int64>(line.getNumBytesAsUTF8())>8192)return false;
    juce::FileOutputStream output(file);if(output.failedToOpen())return false;
    output.writeText(line,false,false,nullptr);output.flush();return output.getStatus().wasOk();
}
int queuedStopChild(const juce::File& diagnostic)
{
    begin("queued_handoff_stop_child");CHECK(queuedDiagnostic(diagnostic,"entry",true));
    Service service;Client active,healthy;const bool opened=active.open(service.name) && healthy.open(service.name);CHECK(opened);
    Access::pauseBeforePickup(true);
    const auto pending=request(active.runtime,91);const bool sent=opened && active.generate(pending);CHECK(sent);
    const bool paused=sent && observe([]{return Access::pickupPaused();},2000);CHECK(paused);
    const bool stopFalse=!service.stop.load();CHECK(stopFalse);CHECK(queuedDiagnostic(diagnostic,"pickupPaused_stopFalse",paused && stopFalse));
    const bool ping=paused && healthy.ping();CHECK(ping);CHECK(queuedDiagnostic(diagnostic,"healthyPing",ping));
    const auto stopAt=GetTickCount64();service.stop.store(true);
    const bool quit=observe([]{return Access::quitPublished();},2000);CHECK(quit);CHECK(queuedDiagnostic(diagnostic,"quitPublished",quit));
    Access::pauseBeforePickup(false);CHECK(queuedDiagnostic(diagnostic,"pauseReleased",true));
    const bool prompt=observe([&]{return service.done.load();},1500);CHECK(prompt);CHECK(queuedDiagnostic(diagnostic,"promptCompletion",prompt));
    // In RED the parent terminates only this owned child at its finite deadline;
    // no live worker storage is freed or detached to disguise that failed drain.
    service.finish();CHECK(GetTickCount64()-stopAt<2500);CHECK(service.result.status==RuntimeStatus::ready && !service.result.workerRetained);
    CHECK(service.backend->calls.load()==0);CHECK(service.backend->cancels.load()==1 && service.backend->cancelToken.load());
    {std::lock_guard lock(service.backend->mutex);CHECK(service.backend->cancelled.size()==1);if(!service.backend->cancelled.empty()){
        const auto& job=service.backend->cancelled[0];CHECK(job.jobId!=0 && job.connectionId!=0 && job.session==active.session && std::get<ai::GenerationRequest>(job.input).tag==pending.tag);
        CHECK(job.cancellation && job.cancellation->cancelled());CHECK(realPeer(job.peer,GetCurrentProcessId()));}}
    CHECK(Access::retainedWorkers()==0 && Access::admissionAvailable());
    Service fresh;Client admitted;CHECK(admitted.open(fresh.name));fresh.finish();CHECK(fresh.result.status==RuntimeStatus::ready && !fresh.result.workerRetained);CHECK(fresh.backend->calls.load()==0);
    CHECK(Access::retainedWorkers()==0 && Access::admissionAvailable());CHECK(queuedDiagnostic(diagnostic,"freshAdmission_orderlyStop",failures==0));
    finishGroup();std::cout<<"queued child checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;
}
void queued_handoff_stop()
{
    begin(__func__);const auto root=queuedFixtureRoot();CHECK(root.createDirectory().wasOk());
    const auto diagnostic=root.getChildFile("runtime-queued-stop-"+suffix()+".txt");Child child;CHECK(child.start({"queued-stop",diagnostic.getFullPathName()}));CHECK(child.finish());
    const bool terminated=child.process.waitForProcessToFinish(1);CHECK(terminated);
    if(terminated){
        const bool bounded=diagnostic.existsAsFile() && diagnostic.getSize()>0 && diagnostic.getSize()<=8192;CHECK(bounded);
        if(bounded){
            std::array<char,8192> data{};int size=0;{juce::FileInputStream input(diagnostic);size=input.read(data.data(),static_cast<int>(data.size()));}
            CHECK(size>0 && size==diagnostic.getSize());if(size>0)std::cout<<"queued-stop bounded diagnostic\n"<<juce::String::fromUTF8(data.data(),size)<<std::endl;
            const bool owned=diagnostic.isAChildOf(root) && diagnostic.getFullPathName()==root.getChildFile(diagnostic.getFileName()).getFullPathName();CHECK(owned);
            if(owned && size>0 && size==diagnostic.getSize())CHECK(diagnostic.deleteFile());
        }
    }
}
int serverChild(const juce::StringArray& args)
{
    auto backend=std::make_shared<Backend>();backend->expectedPid=static_cast<DWORD>(args[4].getLargeIntValue());
    HANDLE stopEvent=OpenEventW(SYNCHRONIZE,FALSE,args[3].toWideCharPointer());if(!stopEvent)return 3;
    std::atomic_bool stop{false};std::thread watcher([&]{WaitForSingleObject(stopEvent,55000);stop.store(true);});
    RuntimeServer server([backend](const auto& job){return backend->run(job);},[backend](const auto& job){backend->cancel(job);});
    const auto result=server.run(stop,args[2]);SetEvent(stopEvent);watcher.join();CloseHandle(stopEvent);
    std::cout<<"server child priority="<<backend->priority.load()<<" peer="<<backend->peer.load()<<" callbacks="<<backend->calls.load()<<std::endl;
    return result.status==RuntimeStatus::ready && !result.workerRetained && backend->priority.load() && backend->peer.load() && backend->calls.load()>0?0:1;
}
struct LocalBackendObservation
{
    std::atomic_int calls=0,evaluations=0,completed=0,cancels=0,active=0,peak=0;
};
Frame runLocalParserJob(const RuntimeJob& job,const std::shared_ptr<LocalBackendObservation>& observation)
{
    if(!job.localPreview||!job.previewExchange)throw std::runtime_error("local fixture authority");
    const auto& request=std::get<ai::GenerationRequest>(job.input);ai::LocalGenerationRequest input{request,*job.localPreview};
    const auto current=observation->active.fetch_add(1)+1;observation->peak.store(std::max(observation->peak.load(),current));
    struct Drain{std::atomic_int& active;~Drain(){active.fetch_sub(1);}} drain{observation->active};unsigned attempts=0;std::uint8_t ordinal=0;
    const auto call=[&](const juce::String&,std::string_view,const juce::var&){++attempts;observation->calls.fetch_add(1);rack::oauth::StructuredResult response;
        response.result.status=rack::oauth::Status::connected;response.result.httpStatus=200;
        response.text=R"({"version":1,"parameter_edits":[],"rack_edits":[],"macro_edits":[],"pattern":null,"characteristics":"dry","explanation":"Retain the owned kit"})";return response;};
    const auto evaluator=[&](const ProjectDocument& candidate,const ai::ProposalEdits& edits,const ai::AuditionContext& context,const SourceAssignmentTrace& trace,const ai::CancelCheck& stop){
        ai::PreviewEvaluate value;value.edits=edits;value.context=context;auto& binding=value.binding;binding.request=request.tag;binding.root=input.local.root;
        binding.catalogSnapshot=input.local.catalogSnapshot;binding.job=job.jobId;binding.structuralGeneration=request.structuralGeneration;binding.reset=input.local.reset;binding.sourceWord=input.local.sourceWord;
        binding.sourceMap=input.local.sourceMapDigest;binding.providerAttempt=std::uint8_t(attempts);binding.renderOrdinal=++ordinal;
        auto staged=std::make_unique<ai::PreviewSampleMap>();if(!ai::stagePreviewMap(input.local.sourceMap,trace,*staged)||!ai::digestPreviewMap(*staged,binding.stagedMap)
            ||!ai::digestPreviewCandidate(candidate,binding.candidate)||!ai::digestPreviewContext(context,binding.context))throw std::runtime_error("local fixture digest");
        observation->evaluations.fetch_add(1);return job.previewExchange->evaluate(value,stop);
    };
    auto result=std::make_unique<ai::LocalGenerationResult>(ai::generateLocalProposal(input,call,evaluator,[&]{return job.cancellation->cancelled();},job.groupContext.get()));
    Frame frame{MessageKind::resultLocal,job.session,{}};if(!encodeLocalResult(input,*result,frame.payload))throw std::runtime_error("local fixture terminal");observation->completed.fetch_add(1);return frame;
}
std::unique_ptr<ai::LocalGenerationRequest> nativeLocalRequest(const Client& client,std::uint64_t id,const drums::AssetPtr& asset)
{
    auto input=std::make_unique<ai::LocalGenerationRequest>();auto& r=input->request;r=request(client.runtime,id);r.instruction="Retain the owned kit";r.capturedBpm=400;
    r.capturedPhrase->notes[0]={0,.125,36,96,1};r.source.sound=newInstanceSound();RackOperation operation;operation.kind=RackOperationKind::place;
    operation.newInstance=juce::Uuid();operation.design=airack::test::frozenTileArrayRackDesign();auto candidate=std::make_unique<SoundDocument>();RackDifference difference;
    CHECK(makeRackCandidate(r.source.sound,operation,SoundEditOrigin::manual,*candidate,difference)==RackCandidateStatus::ready);r.source.sound=std::move(*candidate);
    input->local.root=r.tag;input->local.sourceWord=r.structuralGeneration;
    for(const auto& rack:r.source.sound.personalities->instances)for(const auto& unit:rack.design.units)
        if(isSourceModule(unit.module))if(const auto* binding=rackUnitBinding(rack,unit.id))input->local.sourceMap.banks[binding->slot]={rack.id,unit.id,1};
    input->local.sourceMap.assets.push_back({asset->id(),std::uint8_t(asset->channels()),asset->frames()});input->local.sourceMap.pads[0].asset=1;input->local.sourceMap.pads[0].name="Owned native test hit";
    CHECK(ai::digestPreviewMap(input->local.sourceMap,input->local.sourceMapDigest));CHECK(ai::validLocalPreviewContext(*input));return input;
}
void native_local_exchange_keeps_io_live_and_returns_no_pcm_terminal()
{
    begin(__func__);auto observation=std::make_shared<LocalBackendObservation>();
    Service service([observation](const RuntimeJob& job){return runLocalParserJob(job,observation);},[observation](const RuntimeJob& job){observation->cancels.fetch_add(1);if(job.previewExchange)job.previewExchange->cancel();});
    Client client,heartbeat;CHECK(client.connect(service.name));CHECK(client.hello(RuntimeStatus::ready,ownedLocalPreviewCapability));CHECK(heartbeat.open(service.name));
    std::vector<float> pcm(4800);for(std::size_t i=0;i<pcm.size();++i)pcm[i]=float(.1*std::exp(-double(i)/700)*std::sin(double(i)*.1));
    drums::AssetPtr asset;drums::AssetError error;CHECK(drums::makeSampleAsset(pcm,1,asset,error));if(!asset)return;
    auto input=nativeLocalRequest(client,1,asset);Frame frame{MessageKind::generateLocal,client.session,{}};CHECK(encodeLocalRequest(*input,frame.payload));CHECK(client.send(frame));
    auto received=client.receive();CHECK(received&&received->kind==MessageKind::previewEvaluate);if(!received||received->kind!=MessageKind::previewEvaluate)return;
    auto evaluation=std::make_unique<ai::PreviewEvaluate>();CHECK(decodePreviewEvaluate(bytes(received->payload),*evaluation));CHECK(evaluation->binding.request==input->request.tag);CHECK(evaluation->binding.job&&evaluation->binding.ticket);
    CHECK(observation->calls.load()==1&&observation->evaluations.load()==1&&observation->completed.load()==0&&observation->active.load()==1);
    CHECK(!client.receive(100));CHECK(client.ping());CHECK(heartbeat.ping());CHECK(observation->completed.load()==0);
    GroupCommand browse{1,GroupAction::browse,client.runtime,0};frame={MessageKind::groupCommand,client.session,{}};
    const bool browseEncoded=encodeGroupCommand(browse,frame.payload);CHECK(browseEncoded);if(!browseEncoded)return;
    const bool browseSent=client.send(frame);CHECK(browseSent);if(!browseSent)return;
    received=client.receive();GroupReply groupReply;const bool browseReceived=received&&received->kind==MessageKind::groupReply&&decodeGroupReply(bytes(received->payload),groupReply)&&groupReply.status==GroupReplyStatus::ready;
    CHECK(browseReceived);if(!browseReceived)return;CHECK(observation->active.load()==1&&observation->completed.load()==0);
    auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();CHECK(ai::admitProposal(input->request.source,input->request.policy,evaluation->edits,*candidate,trace.get())==ai::ProposalStatus::accepted);
    BorrowedDrumAssignments borrowed;borrowed[0][0]=asset.get();auto rendered=std::make_unique<ai::RenderResult>(ai::renderPreview(*candidate,evaluation->context,borrowed));
    CHECK(rendered->status==ai::RenderStatus::ready);CHECK(rendered->measurements.peak>0);ai::PreviewReceipt receipt;receipt.binding=evaluation->binding;receipt.status=rendered->status;receipt.measurements=rendered->measurements;
    receipt.bodyFrames=std::uint32_t(rendered->audio.bodyFrames);receipt.frames=std::uint32_t(rendered->audio.left.size());receipt.phraseTruncated=rendered->audio.phraseTruncated;
    frame={MessageKind::previewMeasured,client.session,{}};CHECK(encodePreviewReceipt(receipt,frame.payload));CHECK(client.send(frame));received=client.receive();
    CHECK(received&&received->kind==MessageKind::resultLocal);auto result=std::make_unique<ai::LocalGenerationResult>();if(received){CHECK(decodeLocalResult(bytes(received->payload),*input,*result));
        CHECK(result->numeric.status==ai::GenerationStatus::ready&&result->numeric.audio.left.empty()&&result->numeric.audio.right.empty());CHECK(result->rendered&&ai::samePreviewReceipt(*result->rendered,receipt));
        ai::GenerationResult legacy;CHECK(!decodeResult(bytes(received->payload),input->request,legacy));}
    CHECK(observation->calls.load()==1&&observation->evaluations.load()==1&&observation->completed.load()==1&&observation->peak.load()==1);CHECK(heartbeat.ping());
    auto canceled=nativeLocalRequest(client,2,asset);frame={MessageKind::generateLocal,client.session,{}};CHECK(encodeLocalRequest(*canceled,frame.payload));CHECK(client.send(frame));received=client.receive();
    CHECK(received&&received->kind==MessageKind::previewEvaluate);CHECK(observe([&]{return observation->active.load()==1&&observation->evaluations.load()==2;}));
    CHECK(client.send(control(MessageKind::cancel,client.session,canceled->request.tag)));CHECK(client.replyTag(canceled->request.tag,RuntimeStatus::cancelled));CHECK(heartbeat.ping());
    CHECK(observe([&]{return observation->active.load()==0&&observation->cancels.load()==1;}));CHECK(!client.receive(100));
    auto disconnected=nativeLocalRequest(client,3,asset);frame={MessageKind::generateLocal,client.session,{}};CHECK(encodeLocalRequest(*disconnected,frame.payload));CHECK(client.send(frame));received=client.receive();
    CHECK(received&&received->kind==MessageKind::previewEvaluate);client.pipe.close();CHECK(observe([&]{return observation->active.load()==0&&observation->cancels.load()==2;}));CHECK(heartbeat.ping());
    service.finish();CHECK(service.result.status==RuntimeStatus::ready&&!service.result.workerRetained);CHECK(observation->peak.load()==1);
}

void idle_exit()
{
    begin(__func__);Service service;Client first,second;
    CHECK(first.open(service.name));Sleep(2500);CHECK(!service.done.load()&&first.ping());
    CHECK(second.open(service.name));first.pipe.close();Sleep(2500);CHECK(!service.done.load()&&second.ping());
    second.pipe.close();Sleep(500);Client returning;CHECK(returning.open(service.name));
    Sleep(2500);CHECK(!service.done.load()&&returning.ping());returning.pipe.close();
    CHECK(observe([&]{return service.done.load();},5000));service.finish();
    CHECK(service.result.status==RuntimeStatus::ready&&!service.result.workerRetained);
}
}
int main(int argc,char** argv)
{
    juce::StringArray args;for(int i=0;i<argc;++i)args.add(juce::String::fromUTF8(argv[i]));
    if(argc==3&&args[1]=="--drum-local-profile-retention")return drumLocalProfileRetention(args[2]);
    if(argc==2&&args[1]=="--idle-exit"){idle_exit();finishGroup();std::cout<<checks<<" checks, "<<failures<<" failures"<<std::endl;return failures?1:0;}
    if(argc>1){if(args[1]=="server")return serverChild(args);if(args[1]=="retention")return retentionChild();if(args[1]=="queued-stop" && argc==3)return queuedStopChild(juce::File(args[2]));return 2;}
    idle_exit();native_local_exchange_keeps_io_live_and_returns_no_pcm_terminal();
    native_registration_and_limits();serial_jobs_and_cancellation();request_after_its_reply();duplex_backpressure_and_delivery();native_failures_and_expiry();captured_generation_handoff();retained_worker_shutdown();queued_handoff_stop();finishGroup();
    std::cout<<"checks="<<checks<<" groups="<<groups<<" failures="<<failures<<std::endl;return failures?1:0;
}

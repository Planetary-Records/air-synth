#include "FrozenRackExamples.h"
#include "OwnedFixture.h"
#include "CompanionRuntimeState.h"
#include "GroupRegistry.h"
#include "LocalPreview.h"
#include "RackExamples.h"
#include "RackLibrary.h"
#include <juce_cryptography/juce_cryptography.h>
#include <array>
#include <iostream>
#include <limits>
#include <type_traits>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <string_view>
#endif

using namespace airack;
using namespace airack::ai;
using namespace airack::companion;
namespace
{
int checks=0, failures=0, groups=0, groupChecks=0, groupFailures=0;
const char* group="";
void check(bool v,const char* expression,int line)
{
    ++checks; ++groupChecks;
    if(!v){++failures;++groupFailures;if(failures<=120)std::cerr<<group<<':'<<line<<' '<<expression<<'\n';}
}
#define CHECK(x) check(bool(x),#x,__LINE__)
void begin(const char* name){group=name;groupChecks=groupFailures=0;++groups;std::cout<<name<<std::endl;}
void end(){std::cout<<group<<" checks="<<groupChecks<<" failures="<<groupFailures<<'\n';}
RuntimeTime at(std::uint64_t ms){return RuntimeTime{}+std::chrono::milliseconds(ms);}
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
PipePeer peer(std::uint32_t pid=100,std::uint64_t incarnation=200){return {pid,incarnation};}
Frame control(MessageKind kind,juce::Uuid session,RequestTag tag,RuntimeStatus status=RuntimeStatus::ready)
{
    Frame f{kind,session,{}};CHECK(encodeControl({tag,status},f.payload));return f;
}
Frame hello(juce::Uuid runtime,std::uint32_t capabilities=0){Frame f{MessageKind::hello,juce::Uuid::null(),{}};CHECK(encodeControl({{runtime,0,0,0},RuntimeStatus::ready,capabilities},f.payload));return f;}
struct Client{RuntimeConnectionId id=0;juce::Uuid runtime;juce::Uuid session=juce::Uuid::null();PipePeer process;};
Client connect(RuntimeState& s,PipePeer p=peer(),std::uint64_t ms=0,juce::Uuid runtime=juce::Uuid(),std::uint32_t capabilities=0)
{
    Client c{ s.open(p,at(ms)),runtime,juce::Uuid::null(),p};CHECK(c.id!=0);
    CHECK(s.receive(c.id,hello(runtime,capabilities),at(ms))==RuntimeStatus::ready);
    Frame f;CHECK(s.takeReply(c.id,f));CHECK(f.kind==MessageKind::welcome);CHECK(!f.session.isNull());
    ControlMessage m;CHECK(decodeControl(bytes(f.payload),m));CHECK(m.status==RuntimeStatus::ready);
    CHECK(m.tag==RequestTag(runtime,0,0,0));CHECK(m.capabilities==(capabilities&knownCapabilityMask));c.session=f.session;s.finishReply(c.id,true);CHECK(s.alive(c.id));return c;
}
GenerationRequest request(const Client& c,std::uint64_t id=1,std::uint64_t edit=17,std::uint64_t context=29)
{
    auto ownedRuntimeFixture1=std::make_unique<GenerationRequest>();auto& r=*ownedRuntimeFixture1;r.tag={c.runtime,id,edit,context};r.model="selected-local-test-model";r.instruction="A soft sustained lead";
    r.role="lead";r.key="D";r.scale="minor";r.policy.scope=RequestScope::both;r.capturedBpm=123;
    r.source.sound.macros[0]={0x31924c5b,.25f};return r;
}
Frame generation(const Client& c,const GenerationRequest& r)
{
    Frame f{MessageKind::generate,c.session,{}};CHECK(encodeRequest(r,f.payload));return f;
}
Frame result(const Client& c,const GenerationRequest& r)
{
    auto ownedRuntimeFixture2=std::make_unique<GenerationResult>();auto& failure=*ownedRuntimeFixture2;failure.tag=r.tag;failure.status=GenerationStatus::providerFailure;
    failure.providerStatus=rack::oauth::Status::signedOut;failure.errorCode="fixture_failure";
    if(r.catalog&&r.catalog->phase==CatalogPhase::materialize){failure.inferenceCalls=r.catalog->inferenceCallsUsed;
        failure.providerStatus=r.catalog->providerStatus;failure.httpStatus=r.catalog->httpStatus;}
    Frame f{MessageKind::result,c.session,{}};CHECK(encodeResult(r,failure,f.payload));return f;
}
Frame account(const Client& c,AccountAction action=AccountAction::status)
{
    Frame f{MessageKind::account,c.session,{}};AccountCommand command{action,{}};
    if(action==AccountAction::selectModel)command.model="selected-local-test-model";
    CHECK(encodeAccountCommand(command,f.payload));return f;
}
Frame accountResult(const Client& c,RuntimeStatus status=RuntimeStatus::ready)
{
    Frame f{MessageKind::accountResult,c.session,{}};AccountReply r;r.status=status;
    r.providerStatus=status==RuntimeStatus::ready?rack::oauth::Status::connected:rack::oauth::Status::signedOut;
    CHECK(encodeAccountReply(r,f.payload));return f;
}
bool cancelled(const RuntimeJob& j){return j.cancellation && j.cancellation->cancelled();}
bool tagIs(const RuntimeJob& j,RequestTag tag){const auto* r=std::get_if<GenerationRequest>(&j.input);return r && r->tag==tag;}
void expectControl(RuntimeState& s,const Client& c,MessageKind kind,RequestTag tag,RuntimeStatus status,bool delivered=true)
{
    Frame f;CHECK(s.takeReply(c.id,f));CHECK(f.kind==kind);CHECK(f.session==c.session);
    ControlMessage m;CHECK(decodeControl(bytes(f.payload),m));CHECK(m.tag==tag);CHECK(m.status==status);s.finishReply(c.id,delivered);
}
void expectResult(RuntimeState& s,const Client& c,const GenerationRequest& r,bool delivered=true)
{
    Frame f;CHECK(s.takeReply(c.id,f));CHECK(f.kind==MessageKind::result);CHECK(f.session==c.session);
    RequestTag tag;CHECK(peekResultTag(bytes(f.payload),tag));CHECK(tag==r.tag);
    // These real failure fixtures also decode fully; state itself only routes by tag.
    auto ownedRuntimeFixture3=std::make_unique<GenerationResult>();auto& decoded=*ownedRuntimeFixture3;CHECK(decodeResult(bytes(f.payload),r,decoded));CHECK(decoded.status==GenerationStatus::providerFailure);
    s.finishReply(c.id,delivered);
}
void expectAccount(RuntimeState& s,const Client& c,RuntimeStatus status,bool finish=true)
{
    Frame f;CHECK(s.takeReply(c.id,f));CHECK(f.kind==MessageKind::accountResult);CHECK(f.session==c.session);
    AccountReply decoded;CHECK(decodeAccountReply(bytes(f.payload),decoded));CHECK(decoded.status==status);
    if(finish)s.finishReply(c.id,true);
}
void hello_and_live_ownership()
{
    begin(__func__);
    CHECK(maximumRuntimeConnections==16);CHECK(maximumQueuedRuntimeJobs==16);CHECK(runtimeHelloTimeoutMs==5000);
    CHECK(maximumUnsentRuntimeReplyBytes==9437184);
    static_assert(!std::is_copy_constructible_v<RuntimeState>);
    RuntimeState s;CHECK(s.open(peer(0,1),at(0))==0);CHECK(s.open(peer(1,0),at(0))==0);CHECK(s.connectionCount()==0);
    std::array<RuntimeConnectionId,16> pending{};
    for(std::size_t i=0;i<pending.size();++i){pending[i]=s.open(peer(101+static_cast<std::uint32_t>(i),201),at(0));CHECK(pending[i]!=0);if(i)CHECK(pending[i]>pending[i-1]);}
    CHECK(s.connectionCount()==16);CHECK(s.registeredCount()==0);CHECK(s.open(peer(),at(4999))==0);
    s.expire(at(4999));CHECK(s.connectionCount()==16);s.expire(at(5000));CHECK(s.connectionCount()==0);
    for(auto id:pending)CHECK(!s.alive(id));
    const auto expired=s.open(peer(),at(10000));CHECK(expired>pending.back());
    CHECK(s.receive(expired,hello(juce::Uuid()),at(15000))==RuntimeStatus::staleSession);CHECK(!s.alive(expired));
    const auto old=s.open(peer(),at(20000));const auto fresh=s.open(peer(),at(25000));CHECK(fresh>old);CHECK(!s.alive(old));CHECK(s.connectionCount()==1);s.disconnect(fresh);
    const auto incumbent=connect(s,peer(300,400),30000);s.expire(at(999999));CHECK(s.alive(incumbent.id));CHECK(s.registeredCount()==1);
    for(auto p:{peer(300,400),peer(300,401),peer(301,401)}){
        const auto duplicate=s.open(p,at(1000000));CHECK(duplicate!=0);
        CHECK(s.receive(duplicate,hello(incumbent.runtime),at(1000000))==RuntimeStatus::duplicateInstance);
        CHECK(s.registeredCount()==1);CHECK(s.alive(incumbent.id));Frame f;CHECK(s.takeReply(duplicate,f));
        CHECK(f.kind==MessageKind::welcome);CHECK(f.session!=incumbent.session);CHECK(!f.session.isNull());
        ControlMessage m;CHECK(decodeControl(bytes(f.payload),m));CHECK(m.status==RuntimeStatus::duplicateInstance);
        CHECK(m.tag==RequestTag(incumbent.runtime,0,0,0));s.finishReply(duplicate,true);CHECK(!s.alive(duplicate));CHECK(s.alive(incumbent.id));
    }
    const auto rejected=s.open(peer(),at(1000100));CHECK(s.receive(rejected,hello(incumbent.runtime),at(1000100))==RuntimeStatus::duplicateInstance);
    s.expire(at(1005099));CHECK(s.alive(rejected));s.expire(at(1005100));CHECK(!s.alive(rejected));CHECK(s.alive(incumbent.id));
    s.disconnect(incumbent.id);const auto replacement=connect(s,peer(300,400),1005200,incumbent.runtime);
    CHECK(replacement.id>incumbent.id);CHECK(replacement.session!=incumbent.session);
    RuntimeState cap;std::array<Client,16> clients;
    for(std::size_t i=0;i<clients.size();++i){clients[i]=connect(cap,peer(400+static_cast<std::uint32_t>(i),500));for(std::size_t j=0;j<i;++j)CHECK(clients[i].session!=clients[j].session);}
    CHECK(cap.connectionCount()==16);CHECK(cap.registeredCount()==16);CHECK(cap.open(peer(),at(5000))==0);CHECK(cap.registeredCount()==16);
    end();
}
void request_queue_and_supersede()
{
    begin(__func__);RuntimeState s;const auto a=connect(s),b=connect(s,peer(101,201)),c=connect(s,peer(102,202));
    auto ownedRuntimeFixture33=airack::testing::fixtureValue([&]{return request(a);});const auto& ar=*ownedRuntimeFixture33;auto ownedRuntimeFixture34=airack::testing::fixtureValue([&]{return request(b);});const auto& br=*ownedRuntimeFixture34;auto ownedRuntimeFixture35=airack::testing::fixtureValue([&]{return request(c);});const auto& cr=*ownedRuntimeFixture35;auto ownedRuntimeFixture36=airack::testing::fixtureValue([&]{return request(a,2,18,30);});const auto& ar2=*ownedRuntimeFixture36;
    CHECK(s.receive(a.id,generation(a,ar),at(1))==RuntimeStatus::ready);CHECK(s.receive(b.id,generation(b,br),at(1))==RuntimeStatus::ready);
    CHECK(s.receive(c.id,generation(c,cr),at(1))==RuntimeStatus::ready);CHECK(s.queuedJobCount()==3);
    CHECK(s.receive(a.id,generation(a,ar2),at(2))==RuntimeStatus::ready);CHECK(s.queuedJobCount()==3);
    auto ownedRuntimeFixture4=std::make_unique<RuntimeJob>();auto& j=*ownedRuntimeFixture4;CHECK(s.takeJob(j));CHECK(j.connectionId==b.id);CHECK(tagIs(j,br.tag));CHECK(j.peer.processId==b.process.processId);CHECK(j.peer.creationTime==b.process.creationTime);
    CHECK(j.session==b.session);CHECK(j.jobId!=0);CHECK(j.cancellation && !cancelled(j));CHECK(s.jobRunning());CHECK(s.queuedJobCount()==2);
    const auto previousId=j.jobId;auto ownedRuntimeFixture5=std::make_unique<RuntimeJob>();auto& sentinel=*ownedRuntimeFixture5;sentinel.jobId=999;CHECK(!s.takeJob(sentinel));CHECK(sentinel.jobId==999);
    CHECK(s.finishJob(j,result(b,br)));CHECK(!s.jobRunning());CHECK(s.takeJob(j));CHECK(j.connectionId==c.id);CHECK(j.jobId>previousId);
    CHECK(s.finishJob(j,result(c,cr)));CHECK(s.takeJob(j));CHECK(j.connectionId==a.id);CHECK(tagIs(j,ar2.tag));
    if(const auto* r=std::get_if<GenerationRequest>(&j.input)){CHECK(r->source==ar2.source);CHECK(r->capturedBpm==ar2.capturedBpm);CHECK(r->model==ar2.model);CHECK(r->instruction==ar2.instruction);CHECK(r->policy.scope==ar2.policy.scope);}
    CHECK(s.finishJob(j,result(a,ar2)));expectResult(s,a,ar2);expectResult(s,b,br);expectResult(s,c,cr);
    auto ownedRuntimeFixture37=airack::testing::fixtureValue([&]{return request(a,3);});const auto& ar3=*ownedRuntimeFixture37;CHECK(s.receive(a.id,generation(a,ar3),at(3))==RuntimeStatus::ready);auto ownedRuntimeFixture6=std::make_unique<RuntimeJob>();auto& running=*ownedRuntimeFixture6;CHECK(s.takeJob(running));
    CHECK(s.receive(b.id,generation(b,request(b,2)),at(3))==RuntimeStatus::ready);
    auto ownedRuntimeFixture38=airack::testing::fixtureValue([&]{return request(a,4,19,31);});const auto& ar4=*ownedRuntimeFixture38;CHECK(s.receive(a.id,generation(a,ar4),at(4))==RuntimeStatus::ready);CHECK(cancelled(running));
    CHECK(s.queuedJobCount()==2);CHECK(!s.takeJob(sentinel));CHECK(sentinel.jobId==999);CHECK(s.jobRunning());
    CHECK(s.receive(a.id,generation(a,ar4),at(5))==RuntimeStatus::staleSession);expectControl(s,a,MessageKind::failure,ar4.tag,RuntimeStatus::staleSession);
    CHECK(s.receive(a.id,generation(a,ar3),at(5))==RuntimeStatus::staleSession);expectControl(s,a,MessageKind::failure,ar3.tag,RuntimeStatus::staleSession);
    CHECK(s.queuedJobCount()==2);CHECK(!s.finishJob(running,result(a,ar3)));CHECK(!s.jobRunning());
    CHECK(s.takeJob(j));CHECK(j.connectionId==b.id);CHECK(!cancelled(j));CHECK(s.finishJob(j,result(b,request(b,2))));
    CHECK(s.takeJob(j));CHECK(tagIs(j,ar4.tag));CHECK(!cancelled(j));s.disconnect(a.id);CHECK(cancelled(j));CHECK(s.jobRunning());CHECK(!s.finishJob(j,result(a,ar4)));CHECK(!s.jobRunning());
    RuntimeState full;std::array<Client,16> clients;auto requestsStorage=std::make_unique<std::array<GenerationRequest,16>>();auto& requests=*requestsStorage;
    for(std::size_t i=0;i<clients.size();++i){clients[i]=connect(full,peer(1000+static_cast<std::uint32_t>(i),2000));requests[i]=request(clients[i]);CHECK(full.receive(clients[i].id,generation(clients[i],requests[i]),at(1))==RuntimeStatus::ready);}
    CHECK(full.queuedJobCount()==16);auto ownedRuntimeFixture7=std::make_unique<RuntimeJob>();auto& first=*ownedRuntimeFixture7;CHECK(full.takeJob(first));CHECK(first.connectionId==clients[0].id);
    auto ownedRuntimeFixture39=airack::testing::fixtureValue([&]{return request(clients[0],2);});const auto& supersede=*ownedRuntimeFixture39;CHECK(full.receive(clients[0].id,generation(clients[0],supersede),at(2))==RuntimeStatus::ready);
    CHECK(full.queuedJobCount()==16);CHECK(cancelled(first));CHECK(!full.takeJob(sentinel));CHECK(!full.finishJob(first,result(clients[0],requests[0])));
    for(std::size_t i=1;i<clients.size();++i){CHECK(full.takeJob(j));CHECK(j.connectionId==clients[i].id);CHECK(!cancelled(j));CHECK(full.finishJob(j,result(clients[i],requests[i])));}
    CHECK(full.takeJob(j));CHECK(tagIs(j,supersede.tag));CHECK(full.finishJob(j,result(clients[0],supersede)));CHECK(full.queuedJobCount()==0);
    auto ownedRuntimeFixture40=airack::testing::fixtureValue([&]{return request(b,UINT64_MAX);});const auto& max=*ownedRuntimeFixture40;expectResult(s,b,request(b,2));CHECK(s.receive(b.id,generation(b,max),at(8))==RuntimeStatus::ready);
    CHECK(s.receive(b.id,generation(b,request(b,3)),at(9))==RuntimeStatus::staleSession);expectControl(s,b,MessageKind::failure,request(b,3).tag,RuntimeStatus::staleSession);
    end();
}
void cancel_and_late_completions()
{
    begin(__func__);RuntimeState s;const auto a=connect(s),b=connect(s,peer(101,201));auto ownedRuntimeFixture41=airack::testing::fixtureValue([&]{return request(a);});const auto& ar=*ownedRuntimeFixture41;auto ownedRuntimeFixture42=airack::testing::fixtureValue([&]{return request(b);});const auto& br=*ownedRuntimeFixture42;
    CHECK(s.receive(a.id,generation(a,ar),at(1))==RuntimeStatus::ready);CHECK(s.receive(b.id,generation(b,br),at(1))==RuntimeStatus::ready);
    for(auto bad:{RequestTag(a.runtime,2,17,29),RequestTag(a.runtime,1,18,29),RequestTag(a.runtime,1,17,30)}){
        CHECK(s.receive(a.id,control(MessageKind::cancel,a.session,bad),at(2))==RuntimeStatus::staleSession);
        expectControl(s,a,MessageKind::failure,bad,RuntimeStatus::staleSession);CHECK(s.queuedJobCount()==2);
    }
    CHECK(s.receive(a.id,control(MessageKind::cancel,a.session,ar.tag),at(3))==RuntimeStatus::cancelled);CHECK(s.queuedJobCount()==1);
    expectControl(s,a,MessageKind::failure,ar.tag,RuntimeStatus::cancelled);auto ownedRuntimeFixture8=std::make_unique<RuntimeJob>();auto& j=*ownedRuntimeFixture8;CHECK(s.takeJob(j));CHECK(j.connectionId==b.id);CHECK(!cancelled(j));
    CHECK(s.receive(b.id,control(MessageKind::cancel,b.session,br.tag),at(4))==RuntimeStatus::cancelled);CHECK(cancelled(j));CHECK(s.jobRunning());
    expectControl(s,b,MessageKind::failure,br.tag,RuntimeStatus::cancelled);auto ownedRuntimeFixture9=std::make_unique<RuntimeJob>();auto& sentinel=*ownedRuntimeFixture9;sentinel.jobId=55;CHECK(!s.takeJob(sentinel));CHECK(sentinel.jobId==55);
    CHECK(!s.finishJob(j,result(b,br)));CHECK(!s.jobRunning());Frame noReply;CHECK(!s.takeReply(b.id,noReply));
    auto ownedRuntimeFixture43=airack::testing::fixtureValue([&]{return request(a,2);});const auto& next=*ownedRuntimeFixture43;CHECK(s.receive(a.id,generation(a,next),at(5))==RuntimeStatus::ready);CHECK(s.takeJob(j));
    auto ownedRuntimeFixture44=airack::testing::fixtureValue([&]{return j;});const auto& binding=*ownedRuntimeFixture44;
    for(int mutation=0;mutation<8;++mutation){auto fake=binding;
        switch(mutation){case 0:++fake.jobId;break;case 1:++fake.connectionId;break;case 2:fake.session=juce::Uuid();break;
            case 3:++fake.peer.processId;break;case 4:++fake.peer.creationTime;break;case 5:fake.cancellation=std::make_shared<JobCancellation>();break;
            case 6:fake.input=AccountCommand{};break;case 7:if(auto* r=std::get_if<GenerationRequest>(&fake.input))++r->tag.contextRevision;break;}
        CHECK(!s.finishJob(fake,result(a,next)));CHECK(s.jobRunning());CHECK(!cancelled(binding));CHECK(!s.takeJob(sentinel));
    }
    s.disconnect(a.id);CHECK(cancelled(binding));CHECK(s.jobRunning());const auto replacement=connect(s,a.process,6,a.runtime);CHECK(replacement.id>a.id);CHECK(replacement.session!=a.session);
    auto ownedRuntimeFixture45=airack::testing::fixtureValue([&]{return request(replacement);});const auto& replacementRequest=*ownedRuntimeFixture45;CHECK(s.receive(replacement.id,generation(replacement,replacementRequest),at(7))==RuntimeStatus::ready);
    CHECK(!s.takeJob(sentinel));CHECK(!s.finishJob(binding,result(a,next)));CHECK(!s.jobRunning());CHECK(!s.takeReply(replacement.id,noReply));
    CHECK(s.takeJob(j));CHECK(j.connectionId==replacement.id);CHECK(!cancelled(j));CHECK(s.finishJob(j,result(replacement,replacementRequest)));expectResult(s,replacement,replacementRequest);
    // Cancelling completed-but-unsent output removes it, and has a single bounded acknowledgement.
    auto ownedRuntimeFixture46=airack::testing::fixtureValue([&]{return request(replacement,2);});const auto& done=*ownedRuntimeFixture46;CHECK(s.receive(replacement.id,generation(replacement,done),at(8))==RuntimeStatus::ready);CHECK(s.takeJob(j));CHECK(s.finishJob(j,result(replacement,done)));
    CHECK(s.receive(replacement.id,control(MessageKind::cancel,replacement.session,done.tag),at(9))==RuntimeStatus::cancelled);CHECK(cancelled(j));
    expectControl(s,replacement,MessageKind::failure,done.tag,RuntimeStatus::cancelled);CHECK(!s.takeReply(replacement.id,noReply));
    end();
}
void account_correlation_and_busy()
{
    begin(__func__);
    for(auto status:{RuntimeStatus::ready,RuntimeStatus::unauthorized,RuntimeStatus::unavailable,RuntimeStatus::modelNotConfigured}){
        RuntimeState s;const auto a=connect(s),b=connect(s,peer(101,201));CHECK(s.receive(a.id,account(a,AccountAction::models),at(1))==RuntimeStatus::ready);
        auto ownedRuntimeFixture10=std::make_unique<RuntimeJob>();auto& j=*ownedRuntimeFixture10;CHECK(s.takeJob(j));CHECK(std::holds_alternative<AccountCommand>(j.input));CHECK(j.connectionId==a.id);
        CHECK(s.receive(b.id,generation(b,request(b)),at(2))==RuntimeStatus::ready);CHECK(s.queuedJobCount()==1);
        CHECK(s.finishJob(j,accountResult(a,status)));CHECK(!s.jobRunning());auto ownedRuntimeFixture47=airack::testing::fixtureValue([&]{return request(a);});const auto& ar=*ownedRuntimeFixture47;
        expectAccount(s,a,status,false);CHECK(s.receive(a.id,generation(a,ar),at(3))==RuntimeStatus::busy);
        s.finishReply(a.id,true);expectControl(s,a,MessageKind::failure,ar.tag,RuntimeStatus::busy);
        CHECK(s.alive(a.id));
    }
    // One command remains outstanding while queued, running, unsent, or started.
    for(int phase=0;phase<4;++phase){RuntimeState s;const auto a=connect(s);CHECK(s.receive(a.id,account(a),at(1))==RuntimeStatus::ready);auto ownedRuntimeFixture11=std::make_unique<RuntimeJob>();auto& j=*ownedRuntimeFixture11;
        if(phase>=1)CHECK(s.takeJob(j));if(phase>=2)CHECK(s.finishJob(j,accountResult(a)));if(phase>=3)expectAccount(s,a,RuntimeStatus::ready,false);
        CHECK(s.receive(a.id,account(a,AccountAction::signIn),at(2))==RuntimeStatus::invalidMessage);CHECK(!s.alive(a.id));
        if(phase==1){CHECK(cancelled(j));CHECK(s.jobRunning());CHECK(!s.finishJob(j,accountResult(a)));CHECK(!s.jobRunning());}
    }
    for(int phase=0;phase<4;++phase){RuntimeState busy;const auto c=connect(busy);auto ownedRuntimeFixture48=airack::testing::fixtureValue([&]{return request(c);});const auto& cr=*ownedRuntimeFixture48;
        CHECK(busy.receive(c.id,generation(c,cr),at(1))==RuntimeStatus::ready);auto ownedRuntimeFixture12=std::make_unique<RuntimeJob>();auto& work=*ownedRuntimeFixture12;
        if(phase>=1)CHECK(busy.takeJob(work));if(phase>=2)CHECK(busy.finishJob(work,result(c,cr)));
        Frame started;if(phase>=3)CHECK(busy.takeReply(c.id,started));
        CHECK(busy.receive(c.id,account(c),at(2))==RuntimeStatus::busy);CHECK(busy.alive(c.id));
        if(phase>=1)CHECK(!cancelled(work));
        if(phase==0){expectAccount(busy,c,RuntimeStatus::busy);CHECK(busy.takeJob(work));CHECK(busy.finishJob(work,result(c,cr)));expectResult(busy,c,cr);}
        if(phase==1){expectAccount(busy,c,RuntimeStatus::busy);CHECK(busy.finishJob(work,result(c,cr)));expectResult(busy,c,cr);}
        if(phase==2){expectResult(busy,c,cr);expectAccount(busy,c,RuntimeStatus::busy);}
        if(phase==3){busy.finishReply(c.id,true);expectAccount(busy,c,RuntimeStatus::busy);}
    }
    RuntimeState s;const auto a=connect(s),b=connect(s,peer(101,201));auto ownedRuntimeFixture49=airack::testing::fixtureValue([&]{return request(a);});const auto& ar=*ownedRuntimeFixture49;
    CHECK(s.receive(a.id,generation(a,ar),at(1))==RuntimeStatus::ready);auto ownedRuntimeFixture13=std::make_unique<RuntimeJob>();auto& j=*ownedRuntimeFixture13;CHECK(s.takeJob(j));
    CHECK(s.receive(a.id,account(a),at(2))==RuntimeStatus::busy);CHECK(!cancelled(j));CHECK(s.finishJob(j,result(a,ar)));
    expectAccount(s,a,RuntimeStatus::busy,false);auto ownedRuntimeFixture50=airack::testing::fixtureValue([&]{return request(a,2);});const auto& ar2=*ownedRuntimeFixture50;
    CHECK(s.receive(a.id,generation(a,ar2),at(3))==RuntimeStatus::busy);CHECK(s.queuedJobCount()==0);
    s.finishReply(a.id,true);expectResult(s,a,ar);expectControl(s,a,MessageKind::failure,ar2.tag,RuntimeStatus::busy);
    CHECK(s.receive(a.id,generation(a,ar2),at(4))==RuntimeStatus::ready);CHECK(s.takeJob(j));CHECK(s.finishJob(j,result(a,ar2)));expectResult(s,a,ar2);
    CHECK(s.receive(a.id,account(a,AccountAction::selectModel),at(5))==RuntimeStatus::ready);CHECK(s.receive(b.id,account(b),at(5))==RuntimeStatus::ready);
    CHECK(s.takeJob(j));CHECK(j.connectionId==a.id);CHECK(s.finishJob(j,accountResult(a)));expectAccount(s,a,RuntimeStatus::ready,false);
    CHECK(s.receive(a.id,generation(a,request(a,3)),at(6))==RuntimeStatus::busy);s.finishReply(a.id,true);
    expectControl(s,a,MessageKind::failure,request(a,3).tag,RuntimeStatus::busy);CHECK(s.takeJob(j));CHECK(j.connectionId==b.id);CHECK(s.finishJob(j,accountResult(b)));expectAccount(s,b,RuntimeStatus::ready);
    CHECK(s.receive(a.id,generation(a,request(a,3)),at(7))==RuntimeStatus::ready);
    // Generation output already started still makes an account command busy after supersede/cancel.
    RuntimeState started;const auto c=connect(started);auto ownedRuntimeFixture51=airack::testing::fixtureValue([&]{return request(c);});const auto& cr=*ownedRuntimeFixture51;CHECK(started.receive(c.id,generation(c,cr),at(1))==RuntimeStatus::ready);
    CHECK(started.takeJob(j));CHECK(started.finishJob(j,result(c,cr)));Frame f;CHECK(started.takeReply(c.id,f));CHECK(f.kind==MessageKind::result);
    CHECK(started.receive(c.id,control(MessageKind::cancel,c.session,cr.tag),at(2))==RuntimeStatus::cancelled);
    // Drain cancellation control after finishing the native started frame before testing account busy with a started replacement.
    started.finishReply(c.id,true);expectControl(started,c,MessageKind::failure,cr.tag,RuntimeStatus::cancelled);
    auto ownedRuntimeFixture52=airack::testing::fixtureValue([&]{return request(c,2);});const auto& cr2=*ownedRuntimeFixture52;CHECK(started.receive(c.id,generation(c,cr2),at(3))==RuntimeStatus::ready);CHECK(started.takeJob(j));CHECK(started.finishJob(j,result(c,cr2)));CHECK(started.takeReply(c.id,f));
    CHECK(started.receive(c.id,account(c),at(4))==RuntimeStatus::busy);started.finishReply(c.id,true);expectAccount(started,c,RuntimeStatus::busy);
    end();
}
void reply_bounds_and_started_frames()
{
    begin(__func__);RuntimeState s;const auto a=connect(s),b=connect(s,peer(101,201));auto ownedRuntimeFixture53=airack::testing::fixtureValue([&]{return request(a);});const auto& ar=*ownedRuntimeFixture53;
    Frame sentinel{MessageKind::failure,juce::Uuid(),juce::MemoryBlock("hold",4)};const auto sentinelSession=sentinel.session;
    CHECK(!s.takeReply(a.id,sentinel));CHECK(sentinel.kind==MessageKind::failure);CHECK(sentinel.session==sentinelSession);CHECK(sentinel.payload==juce::MemoryBlock("hold",4));
    CHECK(s.receive(a.id,generation(a,ar),at(1))==RuntimeStatus::ready);auto ownedRuntimeFixture14=std::make_unique<RuntimeJob>();auto& j=*ownedRuntimeFixture14;CHECK(s.takeJob(j));CHECK(s.finishJob(j,result(a,ar)));
    CHECK(s.receive(a.id,control(MessageKind::ping,a.session,{a.runtime,0,0,0}),at(2))==RuntimeStatus::ready);
    Frame f;CHECK(s.takeReply(a.id,f));CHECK(f.kind==MessageKind::result);CHECK(f.payload.getSize()<=maximumResultPayloadBytes);
    CHECK(!s.takeReply(a.id,sentinel));CHECK(sentinel.session==sentinelSession);CHECK(sentinel.payload==juce::MemoryBlock("hold",4));
    auto ownedRuntimeFixture54=airack::testing::fixtureValue([&]{return request(a,2);});const auto& ar2=*ownedRuntimeFixture54;CHECK(s.receive(a.id,generation(a,ar2),at(3))==RuntimeStatus::ready);
    CHECK(s.takeJob(j));CHECK(s.finishJob(j,result(a,ar2)));CHECK(!s.takeReply(a.id,sentinel));s.finishReply(a.id,true);
    expectControl(s,a,MessageKind::pong,{a.runtime,0,0,0},RuntimeStatus::ready);expectResult(s,a,ar2);CHECK(s.alive(a.id));
    // A control inserted first is delivered first; repeated pings cannot jump ahead of a job.
    auto ownedRuntimeFixture55=airack::testing::fixtureValue([&]{return request(a,3);});const auto& ar3=*ownedRuntimeFixture55;CHECK(s.receive(a.id,control(MessageKind::ping,a.session,{a.runtime,0,0,0}),at(4))==RuntimeStatus::ready);
    CHECK(s.receive(a.id,generation(a,ar3),at(4))==RuntimeStatus::ready);CHECK(s.takeJob(j));CHECK(s.finishJob(j,result(a,ar3)));
    expectControl(s,a,MessageKind::pong,{a.runtime,0,0,0},RuntimeStatus::ready);
    CHECK(s.receive(a.id,control(MessageKind::ping,a.session,{a.runtime,0,0,0}),at(5))==RuntimeStatus::ready);expectResult(s,a,ar3);
    expectControl(s,a,MessageKind::pong,{a.runtime,0,0,0},RuntimeStatus::ready);
    // Superseding unsent output removes only that job slot, preserving the control.
    auto ownedRuntimeFixture56=airack::testing::fixtureValue([&]{return request(a,4);});const auto& ar4=*ownedRuntimeFixture56;auto ownedRuntimeFixture57=airack::testing::fixtureValue([&]{return request(a,5);});const auto& ar5=*ownedRuntimeFixture57;CHECK(s.receive(a.id,generation(a,ar4),at(6))==RuntimeStatus::ready);CHECK(s.takeJob(j));CHECK(s.finishJob(j,result(a,ar4)));
    CHECK(s.receive(a.id,control(MessageKind::ping,a.session,{a.runtime,0,0,0}),at(6))==RuntimeStatus::ready);CHECK(s.receive(a.id,generation(a,ar5),at(7))==RuntimeStatus::ready);
    expectControl(s,a,MessageKind::pong,{a.runtime,0,0,0},RuntimeStatus::ready);CHECK(!s.takeReply(a.id,sentinel));CHECK(s.takeJob(j));CHECK(s.finishJob(j,result(a,ar5)));expectResult(s,a,ar5);
    CHECK(s.receive(b.id,generation(b,request(b)),at(8))==RuntimeStatus::ready);auto ownedRuntimeFixture15=std::make_unique<RuntimeJob>();auto& other=*ownedRuntimeFixture15;CHECK(s.takeJob(other));
    CHECK(s.receive(a.id,control(MessageKind::ping,a.session,{a.runtime,0,0,0}),at(8))==RuntimeStatus::ready);
    CHECK(s.receive(a.id,control(MessageKind::ping,a.session,{a.runtime,0,0,0}),at(8))==RuntimeStatus::invalidMessage);CHECK(!s.alive(a.id));CHECK(s.alive(b.id));CHECK(!cancelled(other));
    CHECK(s.finishJob(other,result(b,request(b))));expectResult(s,b,request(b));
    // A started old frame is preserved; failed native completion closes and cancels replacement.
    auto ownedRuntimeFixture58=airack::testing::fixtureValue([&]{return request(b,2);});const auto& br2=*ownedRuntimeFixture58;auto ownedRuntimeFixture59=airack::testing::fixtureValue([&]{return request(b,3);});const auto& br3=*ownedRuntimeFixture59;CHECK(s.receive(b.id,generation(b,br2),at(9))==RuntimeStatus::ready);CHECK(s.takeJob(j));CHECK(s.finishJob(j,result(b,br2)));
    CHECK(s.takeReply(b.id,f));CHECK(s.receive(b.id,generation(b,br3),at(10))==RuntimeStatus::ready);CHECK(s.takeJob(other));CHECK(!s.takeReply(b.id,sentinel));
    s.finishReply(b.id,false);CHECK(!s.alive(b.id));CHECK(cancelled(other));CHECK(s.jobRunning());CHECK(!s.finishJob(other,result(b,br3)));CHECK(!s.jobRunning());
    for(int bad=0;bad<5;++bad){RuntimeState broken;const auto c=connect(broken);auto ownedRuntimeFixture60=airack::testing::fixtureValue([&]{return request(c);});const auto& cr=*ownedRuntimeFixture60;CHECK(broken.receive(c.id,generation(c,cr),at(1))==RuntimeStatus::ready);CHECK(broken.takeJob(j));auto reply=result(c,cr);
        if(bad==0)reply.payload.setSize(maximumResultPayloadBytes+1,true);
        if(bad==1)reply.session=juce::Uuid();if(bad==2)reply=control(MessageKind::failure,c.session,{c.runtime,2,17,29},RuntimeStatus::unavailable);
        if(bad==3)reply=accountResult(c);if(bad==4)reply.payload.setSize(39);
        CHECK(!broken.finishJob(j,std::move(reply)));CHECK(!broken.alive(c.id));CHECK(!broken.jobRunning());CHECK(!broken.takeReply(c.id,sentinel));
    }
    RuntimeState failure;const auto c=connect(failure);auto ownedRuntimeFixture61=airack::testing::fixtureValue([&]{return request(c);});const auto& cr=*ownedRuntimeFixture61;CHECK(failure.receive(c.id,generation(c,cr),at(1))==RuntimeStatus::ready);CHECK(failure.takeJob(j));
    CHECK(failure.finishJob(j,control(MessageKind::failure,c.session,cr.tag,RuntimeStatus::modelNotConfigured)));expectControl(failure,c,MessageKind::failure,cr.tag,RuntimeStatus::modelNotConfigured);
    CHECK(failure.receive(c.id,account(c),at(2))==RuntimeStatus::ready);
    end();
}
void malformed_and_shutdown()
{
    begin(__func__);
    for(int bad=0;bad<8;++bad){RuntimeState s;const auto runtime=juce::Uuid();const auto id=s.open(peer(),at(0));auto f=hello(runtime);
        if(bad==0)f.session=juce::Uuid();if(bad==1)f.kind=MessageKind::generate;if(bad==2)f.payload.setSize(0);
        if(bad==3)f=control(MessageKind::hello,juce::Uuid::null(),{runtime,1,0,0});if(bad==4)f=control(MessageKind::hello,juce::Uuid::null(),{runtime,0,1,0});
        if(bad==5)f=control(MessageKind::hello,juce::Uuid::null(),{runtime,0,0,1});if(bad==6)f=control(MessageKind::hello,juce::Uuid::null(),{runtime,0,0,0},RuntimeStatus::busy);
        if(bad==7)f.payload.setSize(maximumControlPayloadBytes+1,true);
        CHECK(s.receive(id,f,at(1))==RuntimeStatus::invalidMessage);CHECK(!s.alive(id));CHECK(s.connectionCount()==0);
    }
    for(int bad=0;bad<16;++bad){RuntimeState s;const auto c=connect(s);auto f=generation(c,request(c));auto expected=RuntimeStatus::invalidMessage;
        switch(bad){case 0:f.session=juce::Uuid();expected=RuntimeStatus::staleSession;break;case 1:f=hello(c.runtime);break;
            case 2:f.kind=MessageKind::welcome;break;case 3:f.kind=MessageKind::result;break;case 4:f.kind=MessageKind::accountResult;break;
            case 5:f.kind=MessageKind::pong;break;case 6:f.kind=MessageKind::failure;break;case 7:f.payload.setSize(maximumControlPayloadBytes+1,true);break;
            case 8:f=generation(c,request(Client{c.id,juce::Uuid(),c.session,c.process}));expected=RuntimeStatus::staleSession;break;
            case 9:{const char extra=42;f.payload.append(&extra,1);break;}case 10:f.payload.setSize(f.payload.getSize()-1);break;
            case 11:f=control(MessageKind::ping,c.session,{c.runtime,1,0,0});break;case 12:f=control(MessageKind::ping,c.session,{juce::Uuid(),0,0,0});expected=RuntimeStatus::staleSession;break;
            case 13:f=control(MessageKind::cancel,c.session,{c.runtime,1,17,29},RuntimeStatus::busy);break;
            case 14:f=account(c);{const char extra=42;f.payload.append(&extra,1);}break;case 15:f=control(MessageKind::cancel,c.session,{juce::Uuid(),1,17,29});expected=RuntimeStatus::staleSession;break;}
        CHECK(s.receive(c.id,f,at(1))==expected);CHECK(!s.alive(c.id));CHECK(s.queuedJobCount()==0);
    }
    for(int bad=0;bad<4;++bad){RuntimeState s;const auto c=connect(s);CHECK(s.receive(c.id,account(c),at(1))==RuntimeStatus::ready);auto ownedRuntimeFixture16=std::make_unique<RuntimeJob>();auto& j=*ownedRuntimeFixture16;CHECK(s.takeJob(j));auto f=accountResult(c);
        if(bad==0)f.session=juce::Uuid();if(bad==1)f.kind=MessageKind::result;if(bad==2){const char extra=42;f.payload.append(&extra,1);}if(bad==3)f.payload.setSize(maximumControlPayloadBytes+1,true);
        CHECK(!s.finishJob(j,std::move(f)));CHECK(!s.alive(c.id));CHECK(!s.jobRunning());
    }
    std::shared_ptr<JobCancellation> runningToken;
    {RuntimeState s;const auto a=connect(s),b=connect(s,peer(101,201));CHECK(s.receive(a.id,generation(a,request(a)),at(1))==RuntimeStatus::ready);auto ownedRuntimeFixture17=std::make_unique<RuntimeJob>();auto& j=*ownedRuntimeFixture17;CHECK(s.takeJob(j));runningToken=j.cancellation;
        CHECK(s.receive(b.id,generation(b,request(b)),at(1))==RuntimeStatus::ready);CHECK(runningToken && !runningToken->cancelled());}
    CHECK(runningToken && runningToken->cancelled());CHECK(runningToken.use_count()==1);
    RuntimeState idle;Frame sentinel{MessageKind::failure,juce::Uuid(),juce::MemoryBlock("hold",4)};auto ownedRuntimeFixture18=std::make_unique<RuntimeJob>();auto& job=*ownedRuntimeFixture18;job.jobId=123;
    CHECK(!idle.takeJob(job));CHECK(job.jobId==123);CHECK(!idle.takeReply(0,sentinel));CHECK(sentinel.payload==juce::MemoryBlock("hold",4));
    idle.finishReply(0,true);idle.disconnect(0);idle.expire(at(5000));CHECK(idle.connectionCount()==0);CHECK(!idle.finishJob(job,sentinel));
    end();
}
GenerationRequest catalogRequest(const Client& c,std::uint64_t id=1)
{
    auto ownedRuntimeFixture62=airack::testing::fixtureValue([&]{return request(c,id);});auto& r=*ownedRuntimeFixture62;r.policy.scope=RequestScope::sound;
    RackCatalogSnapshot snapshot;CHECK(captureRackCatalog(airack::test::frozenFactoryRackDesigns(),{},{},snapshot));
    RackCatalogPage page;CHECK(catalogPage(snapshot,{},0,65536,page));CHECK(page.complete);
    auto authority=std::make_shared<CatalogAuthority>();authority->root=r.tag;authority->snapshot=snapshot.id;
    authority->descriptorJson=juce::JSON::toString(rackCatalogPageDescriptor(page),true,22).toStdString();
    r.catalog.emplace();r.catalog->root=r.tag;r.catalog->authority=std::move(authority);return r;
}
GenerationResult catalogSelection(const GenerationRequest& r)
{
    const auto factory=airack::test::frozenFactoryRackDesigns().front();juce::MemoryBlock canonical;
    CHECK(encodeRackDesign(factory,canonical,RackValidationMode::instanceRecall));
    auto ownedRuntimeFixture19=std::make_unique<GenerationResult>();auto& g=*ownedRuntimeFixture19;g.tag=r.tag;g.status=GenerationStatus::catalogSelectionRequired;g.inferenceCalls=2;
    g.proposalStatus=ProposalStatus::accepted;g.providerStatus=rack::oauth::Status::connected;g.httpStatus=200;
    g.catalog=CatalogResultBinding{r.catalog->root,r.catalog->authority->snapshot};
    g.edits.catalogSelections.push_back({7,factory.id,factory.revision,juce::SHA256(canonical).toHexString(),{RackRole::synth,masterChain,0}});
    g.edits.parameters.push_back({"output.gain_db",.4f});g.edits.characteristics="rounded";g.edits.explanation="captured selection";
    return g;
}
GenerationRequest materialize(const GenerationRequest& r,const GenerationResult& g,std::uint64_t id=2)
{
    auto ownedRuntimeFixture63=airack::testing::fixtureValue([&]{return r;});auto& next=*ownedRuntimeFixture63;next.tag.requestId=id;auto& context=*next.catalog;context.phase=CatalogPhase::materialize;
    context.selectionParent=g.tag;context.baseIntent=g.edits;context.inferenceCallsUsed=std::uint8_t(g.inferenceCalls);
    context.providerStatus=g.providerStatus;context.httpStatus=g.httpStatus;
    auto authority=std::make_shared<CatalogAuthority>(*context.authority);const auto factory=airack::test::frozenFactoryRackDesigns().front();
    juce::MemoryBlock canonical;CHECK(encodeRackDesign(factory,canonical,RackValidationMode::instanceRecall));
    authority->definitions.push_back({factory.id,factory.revision,juce::SHA256(canonical).toHexString(),canonical});
    context.authority=std::move(authority);return next;
}
Frame selectionFrame(const Client& c,const GenerationRequest& r,const GenerationResult& g)
{Frame f{MessageKind::result,c.session,{}};CHECK(encodeResult(r,g,f.payload));return f;}
RuntimeJob queueSelection(RuntimeState& s,const Client& c,const GenerationRequest& r,const GenerationResult& g)
{
    CHECK(s.receive(c.id,generation(c,r),at(1))==RuntimeStatus::ready);auto ownedRuntimeFixture20=std::make_unique<RuntimeJob>();auto& job=*ownedRuntimeFixture20;CHECK(s.takeJob(job));
    CHECK(s.finishJob(job,selectionFrame(c,r,g)));return job;
}
void deliverSelection(RuntimeState& s,const Client& c,const GenerationRequest& r)
{
    Frame f;CHECK(s.takeReply(c.id,f));auto ownedRuntimeFixture21=std::make_unique<GenerationResult>();auto& g=*ownedRuntimeFixture21;CHECK(decodeResult(bytes(f.payload),r,g));
    CHECK(g.status==GenerationStatus::catalogSelectionRequired && !g.candidate);
    CHECK(g.audio.left.empty() && g.audio.right.empty());s.finishReply(c.id,true);
}
void catalog_pending_is_delivered_once()
{
    begin(__func__);RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture64=airack::testing::fixtureValue([&]{return catalogRequest(c);});const auto& r=*ownedRuntimeFixture64;auto ownedRuntimeFixture65=airack::testing::fixtureValue([&]{return catalogSelection(r);});const auto& g=*ownedRuntimeFixture65;
    queueSelection(s,c,r,g);auto ownedRuntimeFixture66=airack::testing::fixtureValue([&]{return materialize(r,g);});const auto& next=*ownedRuntimeFixture66;
    // Producing bytes is insufficient: native delivery commits the pending selection.
    CHECK(s.receive(c.id,generation(c,next),at(2))==RuntimeStatus::staleSession);
    CHECK(s.queuedJobCount()==0);deliverSelection(s,c,r);
    expectControl(s,c,MessageKind::failure,next.tag,RuntimeStatus::staleSession);
    CHECK(s.receive(c.id,generation(c,next),at(3))==RuntimeStatus::ready);
    auto ownedRuntimeFixture22=std::make_unique<RuntimeJob>();auto& job=*ownedRuntimeFixture22;CHECK(s.takeJob(job));const auto* actual=std::get_if<GenerationRequest>(&job.input);CHECK(actual && actual->catalog);
    if(actual&&actual->catalog){CHECK(actual->catalog->root==r.tag);CHECK(actual->catalog->selectionParent==g.tag);
        CHECK(actual->catalog->inferenceCallsUsed==2 && actual->catalog->httpStatus==200);
        CHECK(actual->catalog->baseIntent.catalogSelections==g.edits.catalogSelections);
        CHECK(actual->source==r.source);CHECK(actual->catalog->authority->definitions.size()==1);}
    CHECK(s.receive(c.id,generation(c,next),at(4))==RuntimeStatus::staleSession);
    expectControl(s,c,MessageKind::failure,next.tag,RuntimeStatus::staleSession);CHECK(!cancelled(job));
    CHECK(s.finishJob(job,result(c,next)));expectResult(s,c,next);auto replay=next;replay.tag.requestId=3;
    CHECK(s.receive(c.id,generation(c,replay),at(5))==RuntimeStatus::staleSession);CHECK(s.queuedJobCount()==0);
    expectControl(s,c,MessageKind::failure,replay.tag,RuntimeStatus::staleSession);end();
}
void catalog_pending_rejects_changed_capture()
{
    begin(__func__);
    for(int mutation=0;mutation<11;++mutation){RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture67=airack::testing::fixtureValue([&]{return catalogRequest(c);});const auto& r=*ownedRuntimeFixture67;auto ownedRuntimeFixture68=airack::testing::fixtureValue([&]{return catalogSelection(r);});const auto& g=*ownedRuntimeFixture68;
        queueSelection(s,c,r,g);deliverSelection(s,c,r);auto ownedRuntimeFixture69=airack::testing::fixtureValue([&]{return materialize(r,g);});auto& changed=*ownedRuntimeFixture69;
        switch(mutation){case 0:changed.source.sound.values.back()=r.source.sound.values.back()==0.f?1.f:0.f;CHECK(changed.source.sound.values.back()!=r.source.sound.values.back());break;case 1:changed.model="other-valid-model";break;
            case 2:changed.instruction="substituted instruction";break;case 3:changed.policy.allowMacroChanges=true;break;
            case 4:changed.capturedBpm=124;break;case 5:changed.role="other";break;case 6:changed.structuralGeneration=1;break;
            case 7:changed.catalog->baseIntent.parameters[0].normalized=.45f;break;case 8:changed.catalog->inferenceCallsUsed=1;break;
            case 9:changed.tag.requestId=3;changed.catalog->selectionParent->requestId=2;break;case 10:changed.catalog->httpStatus=201;break;}
        CHECK(s.receive(c.id,generation(c,changed),at(2))==RuntimeStatus::invalidMessage);
        expectControl(s,c,MessageKind::failure,changed.tag,RuntimeStatus::invalidMessage);CHECK(s.queuedJobCount()==0);
        // A rejected substitution cannot consume or mutate the captured authority.
        auto ownedRuntimeFixture70=airack::testing::fixtureValue([&]{return materialize(r,g);});const auto& correct=*ownedRuntimeFixture70;CHECK(s.receive(c.id,generation(c,correct),at(3))==RuntimeStatus::ready);
        auto ownedRuntimeFixture23=std::make_unique<RuntimeJob>();auto& job=*ownedRuntimeFixture23;CHECK(s.takeJob(job));CHECK(!cancelled(job));CHECK(s.finishJob(job,result(c,correct)));expectResult(s,c,correct);
    }
    end();
}
void catalog_pending_retirement()
{
    begin(__func__);
    for(int phase=0;phase<3;++phase){RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture71=airack::testing::fixtureValue([&]{return catalogRequest(c);});const auto& r=*ownedRuntimeFixture71;auto ownedRuntimeFixture72=airack::testing::fixtureValue([&]{return catalogSelection(r);});const auto& g=*ownedRuntimeFixture72;
        queueSelection(s,c,r,g);Frame started;if(phase==1)CHECK(s.takeReply(c.id,started));if(phase==2)deliverSelection(s,c,r);
        CHECK(s.receive(c.id,control(MessageKind::cancel,c.session,r.tag),at(2))==RuntimeStatus::cancelled);
        if(phase==1)s.finishReply(c.id,true);expectControl(s,c,MessageKind::failure,r.tag,RuntimeStatus::cancelled);
        auto ownedRuntimeFixture73=airack::testing::fixtureValue([&]{return materialize(r,g);});const auto& next=*ownedRuntimeFixture73;CHECK(s.receive(c.id,generation(c,next),at(3))==RuntimeStatus::staleSession);
        expectControl(s,c,MessageKind::failure,next.tag,RuntimeStatus::staleSession);CHECK(s.queuedJobCount()==0);}
    {RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture74=airack::testing::fixtureValue([&]{return catalogRequest(c);});const auto& r=*ownedRuntimeFixture74;auto ownedRuntimeFixture75=airack::testing::fixtureValue([&]{return catalogSelection(r);});const auto& g=*ownedRuntimeFixture75;
        queueSelection(s,c,r,g);deliverSelection(s,c,r);auto ownedRuntimeFixture76=airack::testing::fixtureValue([&]{return catalogRequest(c,3);});const auto& replacement=*ownedRuntimeFixture76;
        CHECK(s.receive(c.id,generation(c,replacement),at(2))==RuntimeStatus::ready);auto ownedRuntimeFixture77=airack::testing::fixtureValue([&]{return materialize(r,g,4);});auto& old=*ownedRuntimeFixture77;
        CHECK(s.receive(c.id,generation(c,old),at(3))==RuntimeStatus::staleSession);
        expectControl(s,c,MessageKind::failure,old.tag,RuntimeStatus::staleSession);CHECK(s.queuedJobCount()==1);}
    {RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture78=airack::testing::fixtureValue([&]{return catalogRequest(c);});const auto& r=*ownedRuntimeFixture78;auto ownedRuntimeFixture79=airack::testing::fixtureValue([&]{return catalogSelection(r);});const auto& g=*ownedRuntimeFixture79;
        queueSelection(s,c,r,g);Frame f;CHECK(s.takeReply(c.id,f));s.finishReply(c.id,false);CHECK(!s.alive(c.id));
        const auto newSession=connect(s,c.process,2,c.runtime);auto ownedRuntimeFixture80=airack::testing::fixtureValue([&]{return materialize(r,g);});const auto& next=*ownedRuntimeFixture80;
        CHECK(s.receive(newSession.id,generation(newSession,next),at(3))==RuntimeStatus::staleSession);
        expectControl(s,newSession,MessageKind::failure,next.tag,RuntimeStatus::staleSession);}
    {RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture81=airack::testing::fixtureValue([&]{return catalogRequest(c);});const auto& r=*ownedRuntimeFixture81;auto ownedRuntimeFixture82=airack::testing::fixtureValue([&]{return catalogSelection(r);});const auto& g=*ownedRuntimeFixture82;
        queueSelection(s,c,r,g);deliverSelection(s,c,r);s.disconnect(c.id);CHECK(!s.alive(c.id));
        const auto newSession=connect(s,c.process,2,c.runtime);auto ownedRuntimeFixture83=airack::testing::fixtureValue([&]{return materialize(r,g);});const auto& next=*ownedRuntimeFixture83;
        CHECK(s.receive(newSession.id,generation(newSession,next),at(3))==RuntimeStatus::staleSession);
        expectControl(s,newSession,MessageKind::failure,next.tag,RuntimeStatus::staleSession);}
    end();
}
GroupPublication publishCatalogGroup(RuntimeState& s,const Client& c,juce::Uuid ensemble)
{
    GroupIntent intent;intent.group=ensemble;intent.member=juce::Uuid();CHECK(setGroupText("Session",intent.name));
    CHECK(setGroupText("lead",intent.role));CHECK(setGroupText("D",intent.key));CHECK(setGroupText("minor",intent.scale));intent.bpm=123;
    auto ownedRuntimeFixture24=std::make_unique<GroupPublication>();auto& p=*ownedRuntimeFixture24;p.runtime=c.runtime;p.loadRevision=1;p.editRevision=17;p.project.group=intent;
    GroupCommand command{1,GroupAction::admit,p.runtime,1,p,{}};Frame f{MessageKind::groupCommand,c.session,{}};
    CHECK(encodeGroupCommand(command,f.payload));CHECK(s.receive(c.id,f,at(1))==RuntimeStatus::ready);
    CHECK(s.takeReply(c.id,f));GroupReply reply;CHECK(decodeGroupReply(bytes(f.payload),reply));
    CHECK(reply.status==GroupReplyStatus::ready && reply.normalized);if(reply.normalized)p.project.group=reply.normalized;
    s.finishReply(c.id,true);return p;
}
void catalog_group_retains_original_peers()
{
    begin(__func__);
    for(bool invalidate:{false,true}){RuntimeState s;const auto c=connect(s),other=connect(s);const auto ensemble=juce::Uuid();
        auto ownedRuntimeFixture84=airack::testing::fixtureValue([&]{return publishCatalogGroup(s,c,ensemble);});const auto& requester=*ownedRuntimeFixture84;auto ownedRuntimeFixture85=airack::testing::fixtureValue([&]{return publishCatalogGroup(s,other,ensemble);});const auto& peerPublication=*ownedRuntimeFixture85;
        auto ownedRuntimeFixture86=airack::testing::fixtureValue([&]{return catalogRequest(c);});auto& r=*ownedRuntimeFixture86;r.source=requester.project;GroupSelection selection;selection.loadRevision=1;selection.peerCount=1;
        selection.members[0]=peerPublication.project.group->member;GroupGenerationCommand command{r,selection,{}};
        Frame frame{MessageKind::generateGroup,c.session,{}};CHECK(encodeGroupGeneration(command,frame.payload));
        CHECK(s.receive(c.id,frame,at(2))==RuntimeStatus::ready);auto ownedRuntimeFixture25=std::make_unique<RuntimeJob>();auto& first=*ownedRuntimeFixture25;CHECK(s.takeJob(first));CHECK(first.groupContext);
        auto ownedRuntimeFixture87=airack::testing::fixtureValue([&]{return catalogSelection(r);});const auto& g=*ownedRuntimeFixture87;CHECK(s.finishJob(first,selectionFrame(c,r,g)));deliverSelection(s,c,r);
        if(invalidate)s.disconnect(other.id);
        auto ownedRuntimeFixture88=airack::testing::fixtureValue([&]{return materialize(r,g);});auto& next=*ownedRuntimeFixture88;command={next,selection,g.tag};CHECK(encodeGroupGeneration(command,frame.payload));
        CHECK(s.receive(c.id,frame,at(3))==(invalidate?RuntimeStatus::staleSession:RuntimeStatus::ready));
        if(invalidate){CHECK(s.queuedJobCount()==0);expectControl(s,c,MessageKind::failure,next.tag,RuntimeStatus::staleSession);}
        else {auto ownedRuntimeFixture26=std::make_unique<RuntimeJob>();auto& grounded=*ownedRuntimeFixture26;CHECK(s.takeJob(grounded));CHECK(grounded.groupContext==first.groupContext);
            CHECK(grounded.groupContext && grounded.groupContext->ownership.peers[0].project==peerPublication.project);
            CHECK(s.finishJob(grounded,result(c,next)));expectResult(s,c,next);}}
    end();
}
Frame admittedFrame(const Client& c,const GenerationRequest& r,const ProposalEdits& edits)
{
    auto ownedRuntimeFixture27=std::make_unique<GenerationResult>();auto& ready=*ownedRuntimeFixture27;ready.tag=r.tag;ready.status=GenerationStatus::ready;ready.inferenceCalls=1;
    ready.proposalStatus=ProposalStatus::accepted;ready.renderStatus=RenderStatus::ready;
    ready.providerStatus=rack::oauth::Status::connected;ready.httpStatus=200;ready.edits=edits;auto ownedRuntimeFixture28=std::make_unique<ProjectDocument>();auto& candidate=*ownedRuntimeFixture28;
    const auto admission=r.catalog?admitProposal(r.source,r.policy,edits,*r.catalog->authority,candidate):admitProposal(r.source,r.policy,edits,candidate);
    CHECK(admission==ProposalStatus::accepted);ready.candidate=std::move(candidate);
    if(r.catalog){ready.catalog=CatalogResultBinding{r.catalog->root,r.catalog->authority->snapshot};
        if(r.catalog->phase==CatalogPhase::materialize){ready.inferenceCalls=r.catalog->inferenceCallsUsed;
            ready.providerStatus=r.catalog->providerStatus;ready.httpStatus=r.catalog->httpStatus;}}
    // Transport authority fixture only; this silent buffer makes no listening claim.
    ready.audio.bodyFrames=1;ready.audio.left.resize(previewTailFrames+1);ready.audio.right.resize(previewTailFrames+1);
    Frame f{MessageKind::result,c.session,{}};CHECK(encodeResult(r,ready,f.payload));return f;
}
void catalog_plain_refinement_retains_root()
{
    begin(__func__);
    for(bool altered:{false,true}){RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture89=airack::testing::fixtureValue([&]{return catalogRequest(c);});const auto& root=*ownedRuntimeFixture89;auto ownedRuntimeFixture90=airack::testing::fixtureValue([&]{return catalogSelection(root);});const auto& selected=*ownedRuntimeFixture90;
        queueSelection(s,c,root,selected);deliverSelection(s,c,root);auto ownedRuntimeFixture91=airack::testing::fixtureValue([&]{return materialize(root,selected);});const auto& grounded=*ownedRuntimeFixture91;
        CHECK(s.receive(c.id,generation(c,grounded),at(2))==RuntimeStatus::ready);auto ownedRuntimeFixture29=std::make_unique<RuntimeJob>();auto& job=*ownedRuntimeFixture29;CHECK(s.takeJob(job));
        CHECK(s.finishJob(job,admittedFrame(c,grounded,selected.edits)));Frame delivered;CHECK(s.takeReply(c.id,delivered));s.finishReply(c.id,true);
        auto ownedRuntimeFixture92=airack::testing::fixtureValue([&]{return root;});auto& child=*ownedRuntimeFixture92;child.tag.requestId=3;child.instruction="A brighter refinement";child.catalog->authority=grounded.catalog->authority;
        child.capturedPhrase=root.source.pattern;child.catalog->baseIntent=selected.edits;if(altered)child.catalog->baseIntent.parameters[0].normalized=.6f;
        CHECK(s.receive(c.id,generation(c,child),at(3))==(altered?RuntimeStatus::invalidMessage:RuntimeStatus::ready));
        if(altered){expectControl(s,c,MessageKind::failure,child.tag,RuntimeStatus::invalidMessage);CHECK(s.queuedJobCount()==0);}
        else{CHECK(s.takeJob(job));const auto* request=std::get_if<GenerationRequest>(&job.input);CHECK(request&&request->catalog);
            if(request&&request->catalog){CHECK(request->catalog->root==root.tag);CHECK(request->source==root.source);CHECK(request->catalog->baseIntent.parameters==selected.edits.parameters);}}
    }
    end();
}
void catalog_refinement_requires_parent_candidate_phrase()
{
    begin(__func__);
    for(bool cleared:{false,true})for(int mutation=0;mutation<3;++mutation){
        RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture93=airack::testing::fixtureValue([&]{return catalogRequest(c);});auto& root=*ownedRuntimeFixture93;root.policy.scope=RequestScope::both;
        root.source.pattern.noteCount=1;root.source.pattern.notes[0]={0,.25,48,96,1};root.capturedPhrase=root.source.pattern;
        auto ownedRuntimeFixture94=airack::testing::fixtureValue([&]{return catalogSelection(root);});auto& selected=*ownedRuntimeFixture94;auto phrase=root.source.pattern;
        if(cleared){phrase.noteCount=0;phrase.notes[0]={};}else phrase.notes[0].pitch=65;
        selected.edits.pattern=phrase;queueSelection(s,c,root,selected);deliverSelection(s,c,root);
        auto ownedRuntimeFixture95=airack::testing::fixtureValue([&]{return materialize(root,selected);});const auto& grounded=*ownedRuntimeFixture95;CHECK(s.receive(c.id,generation(c,grounded),at(2))==RuntimeStatus::ready);
        auto ownedRuntimeFixture30=std::make_unique<RuntimeJob>();auto& job=*ownedRuntimeFixture30;CHECK(s.takeJob(job));CHECK(s.finishJob(job,admittedFrame(c,grounded,selected.edits)));
        Frame delivered;CHECK(s.takeReply(c.id,delivered));auto ownedRuntimeFixture31=std::make_unique<GenerationResult>();auto& parent=*ownedRuntimeFixture31;
        CHECK(decodeResult(bytes(delivered.payload),grounded,parent));CHECK(parent.candidate&&parent.candidate->pattern==phrase);
        s.finishReply(c.id,true);auto ownedRuntimeFixture96=airack::testing::fixtureValue([&]{return root;});auto& child=*ownedRuntimeFixture96;child.tag.requestId=3;child.instruction="Refine the admitted phrase";
        child.catalog->authority=grounded.catalog->authority;child.catalog->baseIntent=selected.edits;child.capturedPhrase=phrase;
        if(mutation==1)child.capturedPhrase.reset();if(mutation==2)child.capturedPhrase=root.capturedPhrase;
        const auto status=s.receive(c.id,generation(c,child),at(3));
        CHECK(status==(mutation?RuntimeStatus::invalidMessage:RuntimeStatus::ready));
        if(mutation&&status==RuntimeStatus::invalidMessage){expectControl(s,c,MessageKind::failure,child.tag,RuntimeStatus::invalidMessage);CHECK(s.queuedJobCount()==0);}
        if(!mutation&&status==RuntimeStatus::ready){CHECK(s.takeJob(job));const auto* actual=std::get_if<GenerationRequest>(&job.input);
            CHECK(actual&&actual->catalog);if(actual&&actual->catalog){CHECK(actual->source==root.source);
                CHECK(actual->catalog->root==root.tag);CHECK(*actual->catalog->authority==*grounded.catalog->authority);
                CHECK(actual->capturedPhrase==std::optional<PatternDocument>(phrase));CHECK(actual->catalog->baseIntent.pattern==selected.edits.pattern);}}
    }
    end();
}
void group_aggregate_accepts_complete_host_controls()
{
    begin(__func__);RuntimeState s;const auto c=connect(s);auto ownedRuntimeFixture97=airack::testing::fixtureValue([&]{return publishCatalogGroup(s,c,juce::Uuid());});const auto& pub=*ownedRuntimeFixture97;
    auto ownedRuntimeFixture98=airack::testing::fixtureValue([&]{return request(c);});auto& r=*ownedRuntimeFixture98;r.source=pub.project;GroupGenerationCommand command{r,{1,0,{}},{}};Frame frame{MessageKind::generateGroup,c.session,{}};
    CHECK(encodeGroupGeneration(command,frame.payload));CHECK(s.receive(c.id,frame,at(2))==RuntimeStatus::ready);auto ownedRuntimeFixture32=std::make_unique<RuntimeJob>();auto& job=*ownedRuntimeFixture32;CHECK(s.takeJob(job));
    ProposalEdits complete;for(const auto& p:parameterRegistry())complete.parameters.push_back({p.id,r.source.sound.values[complete.parameters.size()]});
    CHECK(complete.parameters.size()==parameterCount);CHECK(s.finishJob(job,admittedFrame(c,r,complete)));CHECK(s.takeReply(c.id,frame));s.finishReply(c.id,true);
    GroupCommand validate{2,GroupAction::validateProposal,c.runtime,1,{},r.tag};frame={MessageKind::groupCommand,c.session,{}};
    CHECK(encodeGroupCommand(validate,frame.payload));CHECK(s.receive(c.id,frame,at(3))==RuntimeStatus::ready);CHECK(s.takeReply(c.id,frame));
    GroupReply reply;CHECK(decodeGroupReply(bytes(frame.payload),reply));CHECK(reply.status==GroupReplyStatus::ready);s.finishReply(c.id,true);end();
}
void owned_local_request_uses_existing_runtime_job()
{
    begin(__func__);RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);
    auto legacy=airack::testing::fixtureValue([&]{return request(c);});
    CHECK(state.receive(c.id,generation(c,*legacy),at(1))==RuntimeStatus::ready);
    auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));CHECK(state.finishJob(*job,result(c,*legacy)));expectResult(state,c,*legacy);
    auto local=std::make_unique<LocalGenerationRequest>();local->request=request(c,2);local->local.root=local->request.tag;
    local->local.sourceWord=local->request.structuralGeneration;CHECK(digestPreviewMap(local->local.sourceMap,local->local.sourceMapDigest));
    CHECK(validLocalPreviewContext(*local));Frame frame{MessageKind::generateLocal,c.session,{}};
    CHECK(encodeLocalRequest(*local,frame.payload));auto decoded=std::make_unique<LocalGenerationRequest>();
    CHECK(decodeLocalRequest(bytes(frame.payload),*decoded));CHECK(decoded->request.tag==local->request.tag);CHECK(decoded->local==local->local);
    const auto admitted=state.receive(c.id,frame,at(2));CHECK(admitted==RuntimeStatus::ready);
    if(admitted==RuntimeStatus::ready){CHECK(state.queuedJobCount()==1);CHECK(state.takeJob(*job));CHECK(tagIs(*job,local->request.tag));CHECK(state.jobRunning());CHECK(state.queuedJobCount()==0);}
    end();
}
template<class Predicate> bool awaitLocal(Predicate predicate)
{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do{if(predicate())return true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}while(std::chrono::steady_clock::now()<deadline);return false;
}
std::unique_ptr<LocalGenerationRequest> localRequest(const Client& c,std::uint64_t id=1)
{
    auto input=std::make_unique<LocalGenerationRequest>();input->request=request(c,id);input->request.capturedPhrase=PatternDocument{};input->request.capturedBpm=400;
    input->local.root=input->request.tag;input->local.sourceWord=input->request.structuralGeneration;
    CHECK(digestPreviewMap(input->local.sourceMap,input->local.sourceMapDigest));CHECK(validLocalPreviewContext(*input));return input;
}
Frame localGeneration(const Client& c,const LocalGenerationRequest& input)
{Frame frame{MessageKind::generateLocal,c.session,{}};CHECK(encodeLocalRequest(input,frame.payload));return frame;}
std::unique_ptr<PreviewEvaluate> localEvaluation(const RuntimeJob& job,const LocalGenerationRequest& input,std::uint8_t ordinal=1,std::uint8_t attempt=1,const ProposalEdits& edits={})
{
    auto value=std::make_unique<PreviewEvaluate>();value->edits=edits;value->context=previewContextFor(input.request,edits.pattern);
    auto candidate=std::make_unique<ProjectDocument>();auto trace=std::make_unique<SourceAssignmentTrace>();
    CHECK((input.request.catalog?admitProposal(input.request.source,input.request.policy,edits,*input.request.catalog->authority,*candidate,trace.get())
        :admitProposal(input.request.source,input.request.policy,edits,*candidate,trace.get()))==ProposalStatus::accepted);
    auto& b=value->binding;b.request=input.request.tag;b.root=input.local.root;b.catalogSnapshot=input.local.catalogSnapshot;b.job=job.jobId;
    b.structuralGeneration=input.request.structuralGeneration;b.reset=input.local.reset;b.sourceWord=input.local.sourceWord;
    b.sourceMap=input.local.sourceMapDigest;b.phase=input.request.catalog?input.request.catalog->phase:CatalogPhase::select;b.providerAttempt=attempt;b.renderOrdinal=ordinal;
    auto staged=std::make_unique<PreviewSampleMap>();CHECK(stagePreviewMap(input.local.sourceMap,*trace,*staged));CHECK(digestPreviewMap(*staged,b.stagedMap));
    CHECK(digestPreviewCandidate(*candidate,b.candidate));CHECK(digestPreviewContext(value->context,b.context));return value;
}
PreviewReceipt localMeasurement(const LocalGenerationRequest& input,const PreviewEvaluate& value)
{
    auto candidate=std::make_unique<ProjectDocument>();
    CHECK((input.request.catalog?admitProposal(input.request.source,input.request.policy,value.edits,*input.request.catalog->authority,*candidate)
        :admitProposal(input.request.source,input.request.policy,value.edits,*candidate))==ProposalStatus::accepted);
    auto rendered=std::make_unique<RenderResult>(renderPreview(*candidate,value.context));CHECK(rendered->status==RenderStatus::ready);
    PreviewReceipt receipt;receipt.binding=value.binding;receipt.status=rendered->status;receipt.measurements=rendered->measurements;
    receipt.bodyFrames=std::uint32_t(rendered->audio.bodyFrames);receipt.frames=std::uint32_t(rendered->audio.left.size());receipt.phraseTruncated=rendered->audio.phraseTruncated;
    CHECK(validPreviewReceipt(receipt));return receipt;
}
struct LocalWait
{
    std::shared_ptr<LocalPreviewExchange> exchange;std::optional<PreviewReceipt> returned;std::atomic_bool done=false;std::thread worker;
    LocalWait(const RuntimeJob& job,const PreviewEvaluate& value):exchange(job.previewExchange),worker([this,value,token=job.cancellation]{returned=exchange->evaluate(value,[token]{return token->cancelled();});done.store(true,std::memory_order_release);}){}
    bool finish(){const bool observed=awaitLocal([&]{return done.load(std::memory_order_acquire);});if(!observed)exchange->cancel();if(worker.joinable())worker.join();return observed;}
    ~LocalWait(){if(worker.joinable()){exchange->cancel();worker.join();}}
};
Frame measuredFrame(const Client& c,const PreviewReceipt& receipt)
{Frame frame{MessageKind::previewMeasured,c.session,{}};CHECK(encodePreviewReceipt(receipt,frame.payload));return frame;}
Frame localReadyFrame(const Client& c,const LocalGenerationRequest& input,const PreviewEvaluate& value,const PreviewReceipt& receipt)
{
    auto result=std::make_unique<LocalGenerationResult>();auto& n=result->numeric;n.tag=input.request.tag;n.status=GenerationStatus::ready;n.proposalStatus=ProposalStatus::accepted;
    n.renderStatus=RenderStatus::ready;n.providerStatus=rack::oauth::Status::connected;n.httpStatus=200;n.inferenceCalls=receipt.binding.providerAttempt;n.edits=value.edits;n.measurements=receipt.measurements;
    auto candidate=std::make_unique<ProjectDocument>();CHECK((input.request.catalog?admitProposal(input.request.source,input.request.policy,n.edits,*input.request.catalog->authority,*candidate)
        :admitProposal(input.request.source,input.request.policy,n.edits,*candidate))==ProposalStatus::accepted);n.candidate=std::move(*candidate);
    if(input.request.catalog)n.catalog=CatalogResultBinding{input.request.catalog->root,input.request.catalog->authority->snapshot};result->rendered=receipt;
    Frame frame{MessageKind::resultLocal,c.session,{}};CHECK(encodeLocalResult(input,*result,frame.payload));return frame;
}
void local_interim_is_one_job_and_terminal_settles_after_delivery()
{
    begin(__func__);RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();
    auto input=localRequest(c);CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));
    CHECK(job->localPreview&&*job->localPreview==input->local);CHECK(job->previewExchange);CHECK(job->localDeadline==now+std::chrono::milliseconds(360000));CHECK(!job->localGroupContext);
    auto value=localEvaluation(*job,*input);LocalWait wait(*job,*value);PreviewEvaluate offered;CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));
    CHECK(offered.binding.ticket!=0);CHECK(!job->previewExchange->takeEvaluation(*value));CHECK(state.offerPreview(*job,offered));CHECK(!state.offerPreview(*job,offered));
    Frame frame;CHECK(state.takeReply(c.id,frame));CHECK(frame.kind==MessageKind::previewEvaluate);CHECK(state.jobRunning());CHECK(state.queuedJobCount()==0);
    PreviewEvaluate decoded;CHECK(decodePreviewEvaluate(bytes(frame.payload),decoded));CHECK(decoded.binding==offered.binding);CHECK(!state.takeReply(c.id,frame));
    CHECK(state.receive(c.id,control(MessageKind::ping,c.session,{c.runtime,0,0,0}),now+std::chrono::seconds(1))==RuntimeStatus::ready);
    CHECK(!wait.done.load());state.finishReply(c.id,true);expectControl(state,c,MessageKind::pong,{c.runtime,0,0,0},RuntimeStatus::ready);
    const auto receipt=localMeasurement(*input,offered);CHECK(state.acceptPreviewReceipt(c.id,measuredFrame(c,receipt),now+std::chrono::seconds(2)));CHECK(wait.finish());
    CHECK(wait.returned&&samePreviewReceipt(*wait.returned,receipt));CHECK(state.jobRunning());CHECK(state.queuedJobCount()==0);CHECK(!state.takeReply(c.id,frame));
    CHECK(state.finishJob(*job,localReadyFrame(c,*input,offered,receipt)));CHECK(!state.jobRunning());CHECK(state.takeReply(c.id,frame));CHECK(frame.kind==MessageKind::resultLocal);
    auto child=std::make_unique<LocalGenerationRequest>(*input);child->request.tag.requestId=2;auto ready=std::make_unique<LocalGenerationResult>();CHECK(decodeLocalResult(bytes(frame.payload),*input,*ready));
    CHECK(ready->numeric.candidate);if(ready->numeric.candidate)child->request.source=*ready->numeric.candidate;
    CHECK(state.receive(c.id,localGeneration(c,*child),now+std::chrono::seconds(3))==RuntimeStatus::staleSession);state.finishReply(c.id,true);
    expectControl(state,c,MessageKind::failure,child->request.tag,RuntimeStatus::staleSession);
    CHECK(state.receive(c.id,localGeneration(c,*child),now+std::chrono::seconds(4))==RuntimeStatus::ready);CHECK(state.takeJob(*job));CHECK(job->localDeadline==now+std::chrono::seconds(364));
    end();
}
void local_receipt_rejects_forged_active_authority_and_wakes_worker()
{
    begin(__func__);
    for(int mutation=0;mutation<14;++mutation){RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();
        auto input=localRequest(c);CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));
        auto value=localEvaluation(*job,*input);LocalWait wait(*job,*value);PreviewEvaluate offered;CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));
        CHECK(state.offerPreview(*job,offered));Frame sent;CHECK(state.takeReply(c.id,sent));state.finishReply(c.id,true);auto receipt=localMeasurement(*input,offered);
        switch(mutation){case 0:++receipt.binding.job;break;case 1:++receipt.binding.ticket;break;case 2:++receipt.binding.reset;break;case 3:receipt.binding.catalogSnapshot=juce::Uuid();break;
            case 4:receipt.binding.sourceMap[0]^=1;break;case 5:receipt.binding.stagedMap[0]^=1;break;case 6:receipt.binding.candidate[0]^=1;break;case 7:receipt.binding.context[0]^=1;break;
            case 8:receipt.binding.providerAttempt=2;break;case 9:receipt.binding.renderOrdinal=2;break;case 10:++receipt.bodyFrames;++receipt.frames;break;
            case 11:receipt.phraseTruncated=!receipt.phraseTruncated;break;case 12:receipt.binding.phase=CatalogPhase::materialize;break;case 13:receipt.binding.root.requestId=2;receipt.binding.request.requestId=2;break;}
        Frame frame=measuredFrame(c,receipt);
        CHECK(!state.acceptPreviewReceipt(c.id,frame,now+std::chrono::seconds(1)));CHECK(!state.alive(c.id));CHECK(cancelled(*job));CHECK(wait.finish());CHECK(wait.returned&&wait.returned->failure==PreviewFailure::cancelled);
        CHECK(!state.finishJob(*job,Frame{}));CHECK(!state.jobRunning());
    }
    end();
}
void local_cancel_delivery_deadline_and_old_receipt_are_separate()
{
    begin(__func__);
    for(int phase=0;phase<4;++phase){RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();
        auto input=localRequest(c);CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));
        auto value=localEvaluation(*job,*input);LocalWait wait(*job,*value);PreviewEvaluate offered;CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));
        Frame frame;if(phase){CHECK(state.offerPreview(*job,offered));if(phase>1){CHECK(state.takeReply(c.id,frame));if(phase>2)state.finishReply(c.id,true);}}
        CHECK(state.receive(c.id,control(MessageKind::cancel,c.session,input->request.tag),now+std::chrono::seconds(1))==RuntimeStatus::cancelled);
        if(phase==2)state.finishReply(c.id,true);CHECK(wait.finish());CHECK(wait.returned&&wait.returned->failure==PreviewFailure::cancelled);
        expectControl(state,c,MessageKind::failure,input->request.tag,RuntimeStatus::cancelled);CHECK(!state.finishJob(*job,Frame{}));CHECK(!state.jobRunning());
        const auto receipt=localMeasurement(*input,offered);CHECK(state.acceptPreviewReceipt(c.id,measuredFrame(c,receipt),now+std::chrono::seconds(2)));CHECK(state.alive(c.id));
    }
    {RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();auto input=localRequest(c);
        CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));auto value=localEvaluation(*job,*input);LocalWait wait(*job,*value);
        PreviewEvaluate offered;CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));CHECK(state.offerPreview(*job,offered));Frame frame;CHECK(state.takeReply(c.id,frame));state.finishReply(c.id,true);
        state.expire(now+std::chrono::milliseconds(359999));CHECK(!cancelled(*job));CHECK(state.jobRunning());
        CHECK(state.receive(c.id,control(MessageKind::ping,c.session,{c.runtime,0,0,0}),now+std::chrono::milliseconds(359999))==RuntimeStatus::ready);expectControl(state,c,MessageKind::pong,{c.runtime,0,0,0},RuntimeStatus::ready);
        CHECK(job->localDeadline==now+std::chrono::milliseconds(360000));state.expire(now+std::chrono::milliseconds(360000));CHECK(cancelled(*job));CHECK(wait.finish());
        expectControl(state,c,MessageKind::failure,input->request.tag,RuntimeStatus::cancelled);CHECK(!state.finishJob(*job,Frame{}));CHECK(state.alive(c.id));}
    end();
}
void local_ticket_reservation_never_wraps_or_retires_other_work()
{
    begin(__func__);
    for(auto next:{UINT64_MAX-1,UINT64_MAX,std::uint64_t(0)}){RuntimeState state;airack::companion::detail::RuntimeStateTestAccess::setNextPreviewTicket(state,next);
        const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto other=connect(state,peer(901,902));const auto now=std::chrono::steady_clock::now();
        auto legacy=airack::testing::fixtureValue([&]{return request(other);});CHECK(state.receive(other.id,generation(other,*legacy),now)==RuntimeStatus::ready);
        auto input=localRequest(c);CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::unavailable);expectControl(state,c,MessageKind::failure,input->request.tag,RuntimeStatus::unavailable);
        CHECK(state.alive(c.id));CHECK(state.queuedJobCount()==1);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));CHECK(tagIs(*job,legacy->tag));CHECK(!cancelled(*job));
        CHECK(state.finishJob(*job,result(other,*legacy)));expectResult(state,other,*legacy);CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::unavailable);expectControl(state,c,MessageKind::failure,input->request.tag,RuntimeStatus::unavailable);
    }
    {RuntimeState state;airack::companion::detail::RuntimeStateTestAccess::setNextPreviewTicket(state,UINT64_MAX-2);const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();
        auto input=localRequest(c);CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));
        for(std::uint8_t i=1;i<=3;++i){auto value=localEvaluation(*job,*input,i,i);LocalWait wait(*job,*value);PreviewEvaluate offered;
            CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));CHECK(offered.binding.ticket==UINT64_MAX-3+i);CHECK(state.offerPreview(*job,offered));Frame frame;CHECK(state.takeReply(c.id,frame));state.finishReply(c.id,true);
            auto receipt=localMeasurement(*input,offered);CHECK(state.acceptPreviewReceipt(c.id,measuredFrame(c,receipt),now+std::chrono::seconds(i)));CHECK(wait.finish());}
        CHECK(state.receive(c.id,control(MessageKind::cancel,c.session,input->request.tag),now+std::chrono::seconds(4))==RuntimeStatus::cancelled);expectControl(state,c,MessageKind::failure,input->request.tag,RuntimeStatus::cancelled);CHECK(!state.finishJob(*job,Frame{}));
        auto next=localRequest(c,2);CHECK(state.receive(c.id,localGeneration(c,*next),now+std::chrono::seconds(5))==RuntimeStatus::unavailable);expectControl(state,c,MessageKind::failure,next->request.tag,RuntimeStatus::unavailable);CHECK(state.queuedJobCount()==0);CHECK(state.alive(c.id));}
    end();
}
GroupPublication publishLocalGroup(RuntimeState& state,const Client& c,juce::Uuid ensemble,const ai::PreviewSampleMap* map)
{
    auto publication=std::make_unique<GroupPublication>();auto& p=*publication;p.runtime=c.runtime;p.loadRevision=1;p.editRevision=17;
    GroupIntent intent;intent.group=ensemble;intent.member=juce::Uuid();CHECK(setGroupText("Local group",intent.name));CHECK(setGroupText("lead",intent.role));
    CHECK(setGroupText("D",intent.key));CHECK(setGroupText("minor",intent.scale));intent.bpm=123;p.project.group=intent;
    GroupCommand command{1,GroupAction::admit,p.runtime,1,p,{}};Frame frame{map?MessageKind::groupCommandLocal:MessageKind::groupCommand,c.session,{}};
    CHECK(map?encodeLocalGroupPublication(command,*map,frame.payload):encodeGroupCommand(command,frame.payload));CHECK(state.receive(c.id,frame,std::chrono::steady_clock::now())==RuntimeStatus::ready);
    CHECK(state.takeReply(c.id,frame));GroupReply reply;CHECK(decodeGroupReply(bytes(frame.payload),reply));CHECK(reply.status==GroupReplyStatus::ready&&reply.normalized);
    if(reply.normalized)p.project.group=reply.normalized;state.finishReply(c.id,true);return p;
}
void local_group_maps_capture_unavailable_and_cancel_on_metadata_change()
{
    begin(__func__);RuntimeState state;const auto requester=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);
    const auto known=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability),legacy=connect(state);const auto ensemble=juce::Uuid();
    auto rootMap=std::make_unique<PreviewSampleMap>();auto knownMap=std::make_unique<PreviewSampleMap>();knownMap->pads[0].name="Known peer pad";
    auto rp=airack::testing::fixtureValue([&]{return publishLocalGroup(state,requester,ensemble,rootMap.get());});
    auto kp=airack::testing::fixtureValue([&]{return publishLocalGroup(state,known,ensemble,knownMap.get());});
    auto lp=airack::testing::fixtureValue([&]{return publishLocalGroup(state,legacy,ensemble,nullptr);});
    auto input=localRequest(requester);input->request.source=rp->project;input->request.capturedBpm=123;CHECK(validLocalPreviewContext(*input));
    ai::LocalGroupGenerationCommand command{*input,{1,2,{}},{}};command.selection.members[0]=kp->project.group->member;command.selection.members[1]=lp->project.group->member;
    Frame frame{MessageKind::generateGroupLocal,requester.session,{}};CHECK(encodeLocalGroupGeneration(command,frame.payload));const auto now=std::chrono::steady_clock::now();
    CHECK(state.receive(requester.id,frame,now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));CHECK(job->localGroupContext&&job->groupContext);
    if(job->localGroupContext){const auto& projection=*job->localGroupContext;CHECK(projection.peerCount==2);CHECK(projection.peers[0].runtime==kp->runtime);
        CHECK(projection.peers[0].metadataAvailable);CHECK(projection.peers[0].map==*knownMap);CHECK(projection.peers[0].loadRevision==kp->loadRevision);CHECK(projection.peers[0].editRevision==kp->editRevision);
        CHECK(projection.peers[1].runtime==lp->runtime);CHECK(!projection.peers[1].metadataAvailable);}
    auto value=localEvaluation(*job,*input);LocalWait wait(*job,*value);PreviewEvaluate offered;CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));
    knownMap->pads[0].name="Metadata changed with identical numeric revision";GroupCommand update{2,GroupAction::publish,kp->runtime,1,*kp,{}};
    frame={MessageKind::groupCommandLocal,known.session,{}};CHECK(encodeLocalGroupPublication(update,*knownMap,frame.payload));CHECK(state.receive(known.id,frame,now+std::chrono::seconds(1))==RuntimeStatus::ready);
    CHECK(cancelled(*job));CHECK(wait.finish());CHECK(wait.returned&&wait.returned->failure==PreviewFailure::cancelled);CHECK(!state.offerPreview(*job,offered));
    expectControl(state,requester,MessageKind::failure,input->request.tag,RuntimeStatus::staleSession);CHECK(!state.finishJob(*job,Frame{}));CHECK(!state.jobRunning());end();
}
void local_catalog_selection_preserves_mode_delivery_and_operation_deadline()
{
    begin(__func__);
    for(bool late:{false,true}){RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();
        auto input=localRequest(c);input->request=catalogRequest(c);auto authority=std::make_shared<CatalogAuthority>(*input->request.catalog->authority);authority->rendering=CatalogRendering::ownedLocal;
        input->request.catalog->authority=std::move(authority);input->local.catalogSnapshot=input->request.catalog->authority->snapshot;CHECK(validLocalPreviewContext(*input));
        auto selected=airack::testing::fixtureValue([&]{return catalogSelection(input->request);});CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);
        auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));const auto deadline=job->localDeadline;LocalGenerationResult result;result.numeric=*selected;
        Frame selection{MessageKind::resultLocal,c.session,{}};CHECK(encodeLocalResult(*input,result,selection.payload));CHECK(state.finishJob(*job,std::move(selection)));Frame frame;CHECK(state.takeReply(c.id,frame));
        auto grounded=std::make_unique<LocalGenerationRequest>(*input);grounded->request=materialize(input->request,*selected);CHECK(grounded->request.catalog->authority->rendering==CatalogRendering::ownedLocal);
        CHECK(state.receive(c.id,localGeneration(c,*grounded),now+std::chrono::seconds(1))==RuntimeStatus::staleSession);state.finishReply(c.id,true);
        expectControl(state,c,MessageKind::failure,grounded->request.tag,RuntimeStatus::staleSession);
        const auto status=state.receive(c.id,localGeneration(c,*grounded),now+std::chrono::milliseconds(late?360000:359999));CHECK(status==(late?RuntimeStatus::cancelled:RuntimeStatus::ready));
        if(late){CHECK(state.queuedJobCount()==0);expectControl(state,c,MessageKind::failure,grounded->request.tag,RuntimeStatus::cancelled);}
        else{CHECK(state.takeJob(*job));CHECK(job->localDeadline==deadline);CHECK(job->localPreview&&*job->localPreview==input->local);CHECK(job->previewExchange);
            const auto* actual=std::get_if<GenerationRequest>(&job->input);CHECK(actual&&actual->catalog&&actual->catalog->authority->rendering==CatalogRendering::ownedLocal);
            state.expire(deadline);CHECK(cancelled(*job));expectControl(state,c,MessageKind::failure,grounded->request.tag,RuntimeStatus::cancelled);CHECK(!state.finishJob(*job,Frame{}));}
    }
    end();
}

void local_exchange_entry_failures_keep_valid_reserved_binding()
{
    begin(__func__);RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();
    auto input=localRequest(c);CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));auto value=localEvaluation(*job,*input);
    CHECK(value->binding.ticket==0);
    for(int mode=0;mode<3;++mode){LocalPreviewExchange exchange({101,102,103},mode==1?now-std::chrono::seconds(1):now+std::chrono::seconds(20));if(mode==0)exchange.cancel();
        const auto receipt=exchange.evaluate(*value,[mode]{return mode==2;});CHECK(receipt.failure==PreviewFailure::cancelled);CHECK(receipt.binding.ticket==101);CHECK(validPreviewReceipt(receipt));}
    auto isolated=std::make_unique<RuntimeJob>(*job);isolated->previewExchange=std::make_shared<LocalPreviewExchange>(std::array<std::uint64_t,3>{101,102,103},now+std::chrono::seconds(30));
    for(std::uint8_t ordinal=1;ordinal<=3;++ordinal){auto candidate=localEvaluation(*job,*input,ordinal,ordinal);LocalWait wait(*isolated,*candidate);PreviewEvaluate offered;
        CHECK(awaitLocal([&]{return isolated->previewExchange->takeEvaluation(offered);}));CHECK(offered.binding.ticket==100+ordinal);
        const auto busy=isolated->previewExchange->evaluate(*candidate,{});CHECK(busy.failure==PreviewFailure::busy);CHECK(validPreviewReceipt(busy));
        const auto measured=localMeasurement(*input,offered);auto forgedContext=measured;forgedContext.binding.context[0]^=1;
        CHECK(validPreviewReceipt(forgedContext));CHECK(!isolated->previewExchange->acceptReceipt(forgedContext));CHECK(!wait.done.load());
        CHECK(isolated->previewExchange->acceptReceipt(measured));CHECK(wait.finish());CHECK(wait.returned&&samePreviewReceipt(*wait.returned,measured));}
    value->binding.renderOrdinal=value->binding.providerAttempt=3;const auto budget=isolated->previewExchange->evaluate(*value,{});
    CHECK(budget.failure==PreviewFailure::budget);CHECK(budget.binding.ticket==103);CHECK(validPreviewReceipt(budget));end();
}
void local_direction_receipt_delivery_and_terminal_authority_fail_closed()
{
    begin(__func__);
    {RuntimeState state;const auto c=connect(state);auto input=localRequest(c);CHECK(state.receive(c.id,localGeneration(c,*input),std::chrono::steady_clock::now())==RuntimeStatus::invalidMessage);CHECK(!state.alive(c.id));CHECK(state.queuedJobCount()==0);}
    for(auto kind:{MessageKind::previewEvaluate,MessageKind::resultLocal}){RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);auto input=localRequest(c);
        auto frame=localGeneration(c,*input);frame.kind=kind;CHECK(state.receive(c.id,frame,std::chrono::steady_clock::now())==RuntimeStatus::invalidMessage);CHECK(!state.alive(c.id));CHECK(state.queuedJobCount()==0);}
    for(int mode=0;mode<5;++mode){RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();auto input=localRequest(c);
        CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));auto value=localEvaluation(*job,*input);LocalWait wait(*job,*value);PreviewEvaluate offered;
        CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));CHECK(state.offerPreview(*job,offered));Frame frame;
        if(mode>0){CHECK(state.takeReply(c.id,frame));if(mode>1)state.finishReply(c.id,true);}
        auto measured=localMeasurement(*input,offered);const auto accepted=state.acceptPreviewReceipt(c.id,measuredFrame(c,measured),now+std::chrono::seconds(1));
        CHECK(accepted==(mode>1));CHECK(wait.finish());
        if(mode<2){CHECK(!state.alive(c.id));CHECK(cancelled(*job));CHECK(!state.finishJob(*job,Frame{}));continue;}
        if(mode==2){CHECK(!state.acceptPreviewReceipt(c.id,measuredFrame(c,measured),now+std::chrono::seconds(2)));CHECK(!state.alive(c.id));CHECK(cancelled(*job));CHECK(!state.finishJob(*job,Frame{}));}
        if(mode==3){auto substituted=measured;substituted.measurements.peak+=.01;CHECK(validPreviewReceipt(substituted));CHECK(!samePreviewReceipt(substituted,measured));
            CHECK(!state.finishJob(*job,localReadyFrame(c,*input,offered,substituted)));CHECK(!state.alive(c.id));}
        if(mode==4){CHECK(state.finishJob(*job,localReadyFrame(c,*input,offered,measured)));CHECK(state.takeReply(c.id,frame));state.finishReply(c.id,false);CHECK(!state.alive(c.id));
            const auto fresh=connect(state,c.process,0,c.runtime,ownedLocalPreviewCapability);CHECK(fresh.session!=c.session);auto child=std::make_unique<LocalGenerationRequest>(*input);child->request.tag.requestId=2;
            CHECK(state.receive(fresh.id,localGeneration(fresh,*child),now+std::chrono::seconds(2))==RuntimeStatus::staleSession);CHECK(state.queuedJobCount()==0);expectControl(state,fresh,MessageKind::failure,child->request.tag,RuntimeStatus::staleSession);}
    }
    {RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();auto input=localRequest(c);
        CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));auto value=localEvaluation(*job,*input);value->binding.ticket=1;
        const auto measured=localMeasurement(*input,*value);CHECK(!state.finishJob(*job,localReadyFrame(c,*input,*value,measured)));CHECK(!state.alive(c.id));CHECK(!state.jobRunning());}
    end();
}
void local_retry_parent_is_one_bounded_current_record()
{
    begin(__func__);RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();
    const auto deliver=[&](const LocalGenerationRequest& input,const ProposalEdits& edits){
        auto result=std::make_unique<LocalGenerationResult>();const auto admitted=state.receive(c.id,localGeneration(c,input),now);CHECK(admitted==RuntimeStatus::ready);if(admitted!=RuntimeStatus::ready)return result;
        auto job=std::make_unique<RuntimeJob>();const bool taken=state.takeJob(*job);CHECK(taken);if(!taken)return result;
        auto value=localEvaluation(*job,input,1,1,edits);LocalWait wait(*job,*value);PreviewEvaluate offered;
        const bool offeredReady=awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);});CHECK(offeredReady);if(!offeredReady)return result;
        CHECK(state.offerPreview(*job,offered));Frame frame;CHECK(state.takeReply(c.id,frame));state.finishReply(c.id,true);const auto measured=localMeasurement(input,offered);
        CHECK(state.acceptPreviewReceipt(c.id,measuredFrame(c,measured),now));CHECK(wait.finish());CHECK(wait.returned&&samePreviewReceipt(*wait.returned,measured));
        CHECK(state.finishJob(*job,localReadyFrame(c,input,offered,measured)));CHECK(state.takeReply(c.id,frame));CHECK(decodeLocalResult(bytes(frame.payload),input,*result));state.finishReply(c.id,true);return result;
    };
    auto root=localRequest(c);root->request.policy.scope=RequestScope::both;ProposalEdits initial;initial.pattern=PatternDocument{};
    initial.pattern->noteCount=1;initial.pattern->notes[0]={0,.125,60,96,1};const auto parent=deliver(*root,initial);CHECK(parent->numeric.candidate);if(!parent->numeric.candidate){end();return;}
    auto child=std::make_unique<LocalGenerationRequest>(*root);child->request.tag.requestId=2;child->request.source=*parent->numeric.candidate;child->request.capturedPhrase=parent->numeric.candidate->pattern;
    auto changed=initial;changed.pattern->notes[0].startBeats=.5;const auto rejectedChild=deliver(*child,changed);CHECK(rejectedChild->numeric.candidate);if(!rejectedChild->numeric.candidate){end();return;}
    CHECK(rejectedChild->numeric.candidate->pattern!=parent->numeric.candidate->pattern);
    auto retry=std::make_unique<LocalGenerationRequest>(*root);retry->request.tag.requestId=3;retry->request.source=*parent->numeric.candidate;retry->request.capturedPhrase=parent->numeric.candidate->pattern;
    const auto kept=deliver(*retry,initial);CHECK(kept->numeric.candidate&&kept->numeric.candidate->pattern==parent->numeric.candidate->pattern);if(!kept->numeric.candidate){end();return;}
    // A canceled older child cannot revoke the newer admitted result.
    CHECK(state.receive(c.id,control(MessageKind::cancel,c.session,child->request.tag),now)==RuntimeStatus::staleSession);expectControl(state,c,MessageKind::failure,child->request.tag,RuntimeStatus::staleSession);
    auto forged=std::make_unique<LocalGenerationRequest>(*retry);forged->request.tag.requestId=4;++forged->local.reset;
    CHECK(state.receive(c.id,localGeneration(c,*forged),now)==RuntimeStatus::invalidMessage);expectControl(state,c,MessageKind::failure,forged->request.tag,RuntimeStatus::invalidMessage);
    // The replaced child is not a second historical parent.
    auto historical=std::make_unique<LocalGenerationRequest>(*child);historical->request.tag.requestId=5;historical->request.source=*rejectedChild->numeric.candidate;historical->request.capturedPhrase=rejectedChild->numeric.candidate->pattern;
    CHECK(state.receive(c.id,localGeneration(c,*historical),now)==RuntimeStatus::invalidMessage);expectControl(state,c,MessageKind::failure,historical->request.tag,RuntimeStatus::invalidMessage);
    CHECK(state.receive(c.id,control(MessageKind::cancel,c.session,retry->request.tag),now)==RuntimeStatus::cancelled);expectControl(state,c,MessageKind::failure,retry->request.tag,RuntimeStatus::cancelled);
    retry->request.tag.requestId=6;CHECK(state.receive(c.id,localGeneration(c,*retry),now)==RuntimeStatus::staleSession);expectControl(state,c,MessageKind::failure,retry->request.tag,RuntimeStatus::staleSession);
    auto fresh=localRequest(c,7);fresh->request.policy.scope=RequestScope::both;CHECK(state.receive(c.id,localGeneration(c,*fresh),now)==RuntimeStatus::ready);
    CHECK(state.queuedJobCount()==1);CHECK(state.receive(c.id,control(MessageKind::cancel,c.session,retry->request.tag),now)==RuntimeStatus::staleSession);expectControl(state,c,MessageKind::failure,retry->request.tag,RuntimeStatus::staleSession);
    CHECK(state.queuedJobCount()==1);CHECK(state.receive(c.id,control(MessageKind::cancel,c.session,fresh->request.tag),now)==RuntimeStatus::cancelled);expectControl(state,c,MessageKind::failure,fresh->request.tag,RuntimeStatus::cancelled);
    CHECK(state.queuedJobCount()==0&&state.alive(c.id));end();
}
void local_runtime_job_identity_includes_all_appended_authority()
{
    begin(__func__);RuntimeState state;const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();auto input=localRequest(c);
    CHECK(state.receive(c.id,localGeneration(c,*input),now)==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));auto value=localEvaluation(*job,*input);
    for(int mutation=0;mutation<4;++mutation){auto forged=std::make_unique<RuntimeJob>(*job);
        if(mutation==0)forged->localPreview=std::make_shared<const LocalPreviewContext>(*job->localPreview);
        if(mutation==1)forged->previewExchange=std::make_shared<LocalPreviewExchange>(std::array<std::uint64_t,3>{101,102,103},job->localDeadline);
        if(mutation==2)forged->localDeadline+=std::chrono::milliseconds(1);
        if(mutation==3)forged->localGroupContext=std::make_shared<const RuntimeLocalGroupContext>();
        CHECK(!state.offerPreview(*forged,*value));CHECK(!state.finishJob(*forged,Frame{}));CHECK(state.jobRunning());CHECK(!cancelled(*job));CHECK(state.alive(c.id));}
    end();
}
#if defined(_MSC_VER) && defined(_DEBUG)
namespace exchangeAllocation
{
struct Observation
{
    bool deny=false;
    unsigned sites=0,requests=0,denials=0,warnings=0,pendingWarnings=0;
    std::size_t bytes=0;
};
thread_local Observation* current=nullptr;
thread_local bool armed=false;
void observe(void* context) noexcept
{current=static_cast<Observation*>(context);++current->sites;armed=true;}
int __cdecl allocationHook(int kind,void*,std::size_t bytes,int use,long,const unsigned char*,int)
{
    if(!armed||!current||kind!=_HOOK_ALLOC||((use&0xffff)!=_NORMAL_BLOCK&&(use&0xffff)!=_CLIENT_BLOCK))return 1;
    armed=false;++current->requests;current->bytes=bytes;
    if(!current->deny)return 1;
    ++current->denials;++current->pendingWarnings;return 0;
}
int __cdecl reportHook(int kind,wchar_t* message,int* result)
{
    if(current&&current->pendingWarnings&&kind==_CRT_WARN&&std::wstring_view(message?message:L"")==L"Client hook allocation failure.\n"){
        --current->pendingWarnings;++current->warnings;if(result)*result=0;return 1;}
    return 0;
}
struct Scope
{
    RuntimeState& state;_CRT_ALLOC_HOOK previous=nullptr;
    Scope(RuntimeState& s,Observation& observation):state(s)
    {
        _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL,reportHook);previous=_CrtSetAllocHook(allocationHook);
        airack::companion::detail::RuntimeStateTestAccess::observePreviewExchangeAllocation(state,&observation,observe);
    }
    ~Scope()
    {
        airack::companion::detail::RuntimeStateTestAccess::observePreviewExchangeAllocation(state,nullptr,nullptr);
        armed=false;_CrtSetAllocHook(previous);_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE,reportHook);current=nullptr;
    }
};
RuntimeStatus receive(RuntimeState& state,const Client& c,const Frame& frame,RuntimeTime now,Observation& observation)
{Scope scope(state,observation);return state.receive(c.id,frame,now);}
bool deniedOnce(const Observation& observation)
{
    CHECK(observation.sites==1);CHECK(observation.requests==1);CHECK(observation.denials==1);CHECK(observation.warnings==1);CHECK(observation.pendingWarnings==0);
    CHECK(observation.bytes>=sizeof(LocalPreviewExchange));
    std::cout<<"EXCHANGE_APPLICATION_ALLOCATION bytes="<<observation.bytes<<" sites="<<observation.sites<<" requests="<<observation.requests<<" denied="<<observation.denials<<" warnings="<<observation.warnings<<'\n';
    return observation.sites==1&&observation.requests==1&&observation.denials==1&&observation.warnings==1&&observation.pendingWarnings==0&&observation.bytes>=sizeof(LocalPreviewExchange);
}
}
void local_allocation_legacy_control(RuntimeState& state,const Client& c,RuntimeTime now)
{
    auto old=airack::testing::fixtureValue([&]{return request(c);});CHECK(state.receive(c.id,generation(c,*old),now)==RuntimeStatus::ready);
    auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));CHECK(state.finishJob(*job,result(c,*old)));expectResult(state,c,*old);
}
Frame local_allocation_roundtrip(const Client& c,const LocalGenerationRequest& input)
{
    CHECK(validLocalPreviewContext(input));auto frame=localGeneration(c,input);auto decoded=std::make_unique<LocalGenerationRequest>();
    CHECK(decodeLocalRequest(bytes(frame.payload),*decoded));CHECK(decoded->request.tag==input.request.tag);CHECK(decoded->local==input.local);return frame;
}
void local_exchange_allocation_refusal_preserves_last_three_tickets()
{
    begin(__func__);
    {RuntimeState calibration;const auto c=connect(calibration,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();local_allocation_legacy_control(calibration,c,now);
        auto input=localRequest(c,2);const auto frame=local_allocation_roundtrip(c,*input);exchangeAllocation::Observation observed;
        CHECK(exchangeAllocation::receive(calibration,c,frame,now+std::chrono::seconds(1),observed)==RuntimeStatus::ready);
        CHECK(observed.sites==1&&observed.requests==1&&observed.denials==0&&observed.warnings==0&&observed.pendingWarnings==0);CHECK(observed.bytes>=sizeof(LocalPreviewExchange));
        auto job=std::make_unique<RuntimeJob>();CHECK(calibration.takeJob(*job));CHECK(job->localPreview&&*job->localPreview==input->local);CHECK(job->previewExchange);}
    RuntimeState state;airack::companion::detail::RuntimeStateTestAccess::setNextPreviewTicket(state,UINT64_MAX-2);
    const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();local_allocation_legacy_control(state,c,now);
    auto input=localRequest(c,2);const auto frame=local_allocation_roundtrip(c,*input);exchangeAllocation::Observation rejected;rejected.deny=true;
    const auto failed=exchangeAllocation::receive(state,c,frame,now+std::chrono::seconds(1),rejected);CHECK(failed==RuntimeStatus::unavailable);
    const bool supported=exchangeAllocation::deniedOnce(rejected);CHECK(state.queuedJobCount()==0);CHECK(!state.jobRunning());
    if(failed!=RuntimeStatus::unavailable||!supported){end();return;}
    state.disconnect(c.id);const auto fresh=connect(state,c.process,0,c.runtime,ownedLocalPreviewCapability);CHECK(fresh.session!=c.session);
    auto retry=localRequest(fresh);const auto retried=state.receive(fresh.id,local_allocation_roundtrip(fresh,*retry),now+std::chrono::seconds(2));
    CHECK(retried==RuntimeStatus::ready);
    if(retried==RuntimeStatus::ready){auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));auto value=localEvaluation(*job,*retry);LocalWait wait(*job,*value);PreviewEvaluate offered;
        CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));CHECK(offered.binding.ticket==UINT64_MAX-2);
        CHECK(state.receive(fresh.id,control(MessageKind::cancel,fresh.session,retry->request.tag),now+std::chrono::seconds(3))==RuntimeStatus::cancelled);CHECK(wait.finish());
        expectControl(state,fresh,MessageKind::failure,retry->request.tag,RuntimeStatus::cancelled);CHECK(!state.finishJob(*job,Frame{}));}
    end();
}
void local_exchange_allocation_refusal_preserves_delivered_parent()
{
    begin(__func__);RuntimeState state;airack::companion::detail::RuntimeStateTestAccess::setNextPreviewTicket(state,UINT64_MAX-5);
    const auto c=connect(state,peer(),0,juce::Uuid(),ownedLocalPreviewCapability);const auto now=std::chrono::steady_clock::now();local_allocation_legacy_control(state,c,now);
    auto input=localRequest(c,2);CHECK(state.receive(c.id,local_allocation_roundtrip(c,*input),now+std::chrono::seconds(1))==RuntimeStatus::ready);auto job=std::make_unique<RuntimeJob>();CHECK(state.takeJob(*job));
    auto value=localEvaluation(*job,*input);LocalWait wait(*job,*value);PreviewEvaluate offered;CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(offered);}));CHECK(offered.binding.ticket==UINT64_MAX-5);
    CHECK(state.offerPreview(*job,offered));Frame frame;CHECK(state.takeReply(c.id,frame));state.finishReply(c.id,true);const auto measured=localMeasurement(*input,offered);
    CHECK(state.acceptPreviewReceipt(c.id,measuredFrame(c,measured),now+std::chrono::seconds(2)));CHECK(wait.finish());CHECK(wait.returned&&samePreviewReceipt(*wait.returned,measured));
    CHECK(state.finishJob(*job,localReadyFrame(c,*input,offered,measured)));CHECK(state.takeReply(c.id,frame));auto parent=std::make_unique<LocalGenerationResult>();CHECK(decodeLocalResult(bytes(frame.payload),*input,*parent));
    CHECK(parent->numeric.candidate);state.finishReply(c.id,true);if(!parent->numeric.candidate){end();return;}
    auto child=std::make_unique<LocalGenerationRequest>(*input);child->request.tag.requestId=3;child->request.source=*parent->numeric.candidate;
    auto admitted=std::make_unique<ProjectDocument>();auto effects=std::make_unique<SourceAssignmentTrace>();
    CHECK(admitProposal(input->request.source,input->request.policy,parent->numeric.edits,*admitted,effects.get())==ProposalStatus::accepted);
    CHECK(exactProject(*admitted,*parent->numeric.candidate));CHECK(stagePreviewMap(input->local.sourceMap,*effects,child->local.sourceMap));CHECK(digestPreviewMap(child->local.sourceMap,child->local.sourceMapDigest));
    const auto childFrame=local_allocation_roundtrip(c,*child);exchangeAllocation::Observation rejected;rejected.deny=true;
    const auto failed=exchangeAllocation::receive(state,c,childFrame,now+std::chrono::seconds(3),rejected);CHECK(failed==RuntimeStatus::unavailable);
    const bool supported=exchangeAllocation::deniedOnce(rejected);CHECK(state.queuedJobCount()==0);CHECK(!state.jobRunning());CHECK(state.alive(c.id));
    if(failed!=RuntimeStatus::unavailable||!supported||!state.alive(c.id)){end();return;}
    expectControl(state,c,MessageKind::failure,child->request.tag,RuntimeStatus::unavailable);
    const auto retried=state.receive(c.id,childFrame,now+std::chrono::seconds(4));CHECK(retried==RuntimeStatus::ready);
    if(retried==RuntimeStatus::ready){CHECK(state.takeJob(*job));CHECK(job->localPreview&&*job->localPreview==child->local);CHECK(tagIs(*job,child->request.tag));
        const auto* actual=std::get_if<GenerationRequest>(&job->input);CHECK(actual&&actual->source==*parent->numeric.candidate);auto next=localEvaluation(*job,*child);LocalWait retryWait(*job,*next);PreviewEvaluate retryOffer;
        CHECK(awaitLocal([&]{return job->previewExchange->takeEvaluation(retryOffer);}));CHECK(retryOffer.binding.ticket==UINT64_MAX-2);
        CHECK(state.receive(c.id,control(MessageKind::cancel,c.session,child->request.tag),now+std::chrono::seconds(5))==RuntimeStatus::cancelled);CHECK(retryWait.finish());
        expectControl(state,c,MessageKind::failure,child->request.tag,RuntimeStatus::cancelled);CHECK(!state.finishJob(*job,Frame{}));}
    end();
}
#endif
}
int main()
{
    hello_and_live_ownership();request_queue_and_supersede();cancel_and_late_completions();account_correlation_and_busy();reply_bounds_and_started_frames();malformed_and_shutdown();
    catalog_pending_is_delivered_once();catalog_pending_rejects_changed_capture();catalog_pending_retirement();catalog_group_retains_original_peers();
    catalog_plain_refinement_retains_root();catalog_refinement_requires_parent_candidate_phrase();group_aggregate_accepts_complete_host_controls();
    owned_local_request_uses_existing_runtime_job();
    local_interim_is_one_job_and_terminal_settles_after_delivery();local_receipt_rejects_forged_active_authority_and_wakes_worker();
    local_cancel_delivery_deadline_and_old_receipt_are_separate();local_ticket_reservation_never_wraps_or_retires_other_work();
    local_group_maps_capture_unavailable_and_cancel_on_metadata_change();local_catalog_selection_preserves_mode_delivery_and_operation_deadline();
    local_exchange_entry_failures_keep_valid_reserved_binding();local_direction_receipt_delivery_and_terminal_authority_fail_closed();local_retry_parent_is_one_bounded_current_record();local_runtime_job_identity_includes_all_appended_authority();
#if defined(_MSC_VER) && defined(_DEBUG)
    local_exchange_allocation_refusal_preserves_last_three_tickets();local_exchange_allocation_refusal_preserves_delivered_parent();
#endif
    std::cout<<"RuntimeState "<<checks<<" checks / "<<groups<<" groups / "<<failures<<" failures\n";return failures?1:0;
}

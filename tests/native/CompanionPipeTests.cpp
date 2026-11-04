#include "CompanionPipe.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <Sddl.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

using namespace airack::companion;
namespace
{
int checks=0, failures=0, groups=0, groupStartChecks=0, groupStartFailures=0;
const char* group="";
void check(bool ok,const char* text,int line)
{ ++checks; if(!ok){++failures;std::cerr<<group<<':'<<line<<' '<<text<<'\n';} }
#define CHECK(x) check(bool(x),#x,__LINE__)
void finishGroup() {if(groups!=0)std::cout<<group<<" checks="<<checks-groupStartChecks<<" failures="<<failures-groupStartFailures<<std::endl;}
void begin(const char* name) {finishGroup();group=name;++groups;groupStartChecks=checks;groupStartFailures=failures;std::cout<<name<<std::endl;}
using Test=detail::PipeTestAccess;
juce::String suffix() { return juce::Uuid().toString().removeCharacters("-"); }
WireBytes bytes(const juce::MemoryBlock& b) {return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
std::uint64_t creation(HANDLE h)
{FILETIME a{},b{},c{},d{};if(!GetProcessTimes(h,&a,&b,&c,&d))return 0;return (std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime;}
juce::String sidOf(HANDLE process)
{
    HANDLE token=nullptr;if(!OpenProcessToken(process,TOKEN_QUERY,&token))return {};
    DWORD n=0;GetTokenInformation(token,TokenUser,nullptr,0,&n);
    std::vector<std::byte> storage(n<=65536?n:0);juce::String result;
    if(!storage.empty() && GetTokenInformation(token,TokenUser,storage.data(),n,&n)) {
        LPWSTR s=nullptr;auto* user=reinterpret_cast<TOKEN_USER*>(storage.data());
        if(IsValidSid(user->User.Sid) && ConvertSidToStringSidW(user->User.Sid,&s)){result=juce::String(s);LocalFree(s);}
    }
    CloseHandle(token);return result;
}
juce::String wrongSid() {return sidOf(GetCurrentProcess())=="S-1-5-18"?"S-1-5-19":"S-1-5-18";}
bool peerIs(const PipePeer& peer,DWORD pid)
{
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(p==nullptr)return false;
    const bool ok=peer.processId==pid && peer.creationTime!=0 && peer.creationTime==creation(p)
        && sidOf(p)==sidOf(GetCurrentProcess());CloseHandle(p);return ok;
}
void waitEvents(const PipeConnection& c,DWORD ms=2)
{
    auto e=c.pendingEvents();HANDLE h[2]{};DWORD n=0;for(auto v:e)if(v!=nullptr)h[n++]=v;
    if(n!=0)WaitForMultipleObjects(n,h,FALSE,ms);else Sleep(ms);
}
template<class F> PipeStatus until(F f,std::uint64_t ms=8000)
{
    const auto end=GetTickCount64()+ms;PipeStatus s=PipeStatus::pending;
    do{s=f();if(s!=PipeStatus::pending)return s;Sleep(1);}while(GetTickCount64()<end);return s;
}
Frame dataFrame(std::size_t n=3,MessageKind kind=MessageKind::generate)
{
    Frame f;f.kind=kind;f.session=juce::Uuid("01000000000000000000000000000000");
    f.payload.setSize(n);std::memset(f.payload.getData(),0x55,n);return f;
}
Frame cancelFrame()
{
    Frame f;f.kind=MessageKind::cancel;f.session=juce::Uuid("01000000000000000000000000000000");
    ControlMessage c;c.tag.runtime=juce::Uuid("02000000000000000000000000000000");c.tag.requestId=7;
    if(!encodeControl(c,f.payload))f.payload.reset();return f;
}
Frame sentinel()
{Frame f;f.kind=MessageKind::account;f.session=juce::Uuid("03000000000000000000000000000000");const unsigned char b[]{0xde,0xad,0xbe,0xef};f.payload=juce::MemoryBlock(b,sizeof b);return f;}
bool isSentinel(const Frame& f)
{const unsigned char b[]{0xde,0xad,0xbe,0xef};return f.kind==MessageKind::account && f.session==juce::Uuid("03000000000000000000000000000000") && f.payload.getSize()==4 && std::memcmp(f.payload.getData(),b,4)==0;}
struct Pair
{
    PipeListener listener;PipeConnection client,server;juce::String name=suffix();
    bool open()
    {
        const auto s=listener.open(name);CHECK(s==PipeStatus::ready);if(s!=PipeStatus::ready)return false;
        CHECK(listener.pendingEvent()!=nullptr);
        const auto c=client.connect(name);CHECK(c==PipeStatus::ready);if(c!=PipeStatus::ready)return false;
        const auto a=until([&]{return listener.pollAccept(server);});CHECK(a==PipeStatus::complete);return a==PipeStatus::complete;
    }
};
// Every child is this test executable, launched through the argument-array API.
// Only this ChildProcess object's native handle is waited on or terminated.
struct Child
{
    juce::ChildProcess process;bool started=false;std::uint64_t deadline=0;
    int remainingWait(std::uint64_t reserve=0) const
    {
        const auto now=GetTickCount64();const auto left=now<deadline?deadline-now:0;
        return left>reserve?static_cast<int>(left-reserve):0;
    }
    void terminateWithinDeadline()
    {
        process.kill();const auto left=remainingWait();
        if(left>0)process.waitForProcessToFinish((std::min)(left,1000));
    }
    bool start(const juce::StringArray& args)
    {
        juce::StringArray command;command.add(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName());
        command.addArray(args);deadline=GetTickCount64()+60000;started=process.start(command);return started;
    }
    bool finish(int expected=0)
    {
        if(!started)return false;
        const auto wait=remainingWait(1000);
        if(wait>0){if(!process.waitForProcessToFinish(wait)){terminateWithinDeadline();return false;}}
        else if(process.isRunning()){terminateWithinDeadline();return false;}
        const auto out=process.readAllProcessOutput();if(out.isNotEmpty())std::cout<<out;
        return process.getExitCode()==static_cast<juce::uint32>(expected);
    }
    ~Child(){if(started && process.isRunning())terminateWithinDeadline();}
};
struct Raw
{
    HANDLE pipe=INVALID_HANDLE_VALUE;
    ~Raw(){if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);}
    bool connect(const juce::String& s)
    {
        auto path=currentUserPipeName(s);if(path.isEmpty())return false;
        const auto end=GetTickCount64()+8000;
        do{pipe=CreateFileW(path.toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
            if(pipe!=INVALID_HANDLE_VALUE)return true;Sleep(2);}while(GetTickCount64()<end);return false;
    }
    bool io(bool write,void* p,DWORD n,DWORD& done)
    {
        OVERLAPPED o{};o.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(o.hEvent==nullptr)return false;
        BOOL ok=write?WriteFile(pipe,p,n,&done,&o):ReadFile(pipe,p,n,&done,&o);
        const auto error=ok?ERROR_SUCCESS:GetLastError();
        if(!ok && error==ERROR_IO_PENDING){
            if(WaitForSingleObject(o.hEvent,10000)!=WAIT_OBJECT_0){CancelIoEx(pipe,&o);WaitForSingleObject(o.hEvent,1000);}
            ok=GetOverlappedResult(pipe,&o,&done,FALSE);
            // Raw IO runs only in an owned child. An unexpectedly unobserved
            // cancel exits that child without freeing still-pending stack IO.
            if(!ok && GetLastError()==ERROR_IO_INCOMPLETE)ExitProcess(90);
        }
        CloseHandle(o.hEvent);return ok && done!=0;
    }
    bool write(const void* p,std::size_t n,std::size_t chunk=65536,DWORD delay=0)
    {
        auto* data=static_cast<const std::byte*>(p);std::size_t at=0;
        while(at<n){DWORD done=0;if(!io(true,const_cast<std::byte*>(data+at),static_cast<DWORD>((std::min)(n-at,chunk)),done))return false;at+=done;if(delay!=0)Sleep(delay);}return true;
    }
};
// Literal ARS4 framing; current version/caps share the approved wire constants.
juce::MemoryBlock rawFixture(int which)
{
    unsigned char h[wireHeaderBytes]{0x41,0x52,0x53,0x34,static_cast<unsigned char>(wireVersion),static_cast<unsigned char>(wireVersion>>8),3,0,3,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    const auto length=[&](std::size_t n){for(unsigned j=0;j<4;++j)h[8+j]=static_cast<unsigned char>(n>>(8*j));};
    std::size_t payload=3;
    switch(which){
        case 1:h[0]=0;break;case 2:h[4]=static_cast<unsigned char>(wireVersion-1);break;case 3:h[6]=0;break;case 4:h[12]=0;break;
        case 5:h[6]=4;h[8]=1;payload=1;break;
        case 6:return {h,7};
        case 7:h[8]=h[9]=h[10]=h[11]=255;payload=0;break;
        case 8:length(maximumGenerationRequestBytes+1);payload=0;break;
        case 9:h[6]=5;length(maximumResultPayloadBytes+1);payload=0;break;
        case 10:length(maximumGenerationRequestBytes);payload=maximumGenerationRequestBytes;break;
        case 11:h[6]=5;length(maximumResultPayloadBytes);payload=maximumResultPayloadBytes;break;
        case 12:h[7]=0x80;break; // Reserved/unknown kind bits.
        default:break;
    }
    juce::MemoryBlock b(h,28);b.setSize(28+payload);if(payload!=0)std::memset(static_cast<std::byte*>(b.getData())+28,0x55,payload);return b;
}
int childMain(const juce::StringArray& a)
{
    const auto mode=a[1],s=a[2];
    if(mode=="retention"){
        Pair p;if(!p.open())return 2;
        Pair healthyBefore,healthyAfter,inFlight;
        if(!healthyBefore.open() || !healthyAfter.open() || !inFlight.open())return 2;
        PipeListener pausedBefore,pausedAfter,rejectedBefore,rejectedAfter;
        const auto pausedNameBefore=suffix(),pausedNameAfter=suffix(),rejectedNameBefore=suffix(),rejectedNameAfter=suffix();
        Test::pauseConnect(pausedBefore,true);Test::pauseConnect(pausedAfter,true);
        const auto pb=pausedBefore.open(pausedNameBefore),pa=pausedAfter.open(pausedNameAfter);
        CHECK(pb==PipeStatus::ready);CHECK(pa==PipeStatus::ready);if(pb!=PipeStatus::ready || pa!=PipeStatus::ready)return 2;
        CHECK(pausedBefore.pendingEvent()==nullptr && pausedAfter.pendingEvent()==nullptr);
        Test::expectPeerSid(rejectedBefore,wrongSid());Test::expectPeerSid(rejectedAfter,wrongSid());
        const auto rb=rejectedBefore.open(rejectedNameBefore),ra=rejectedAfter.open(rejectedNameAfter);
        CHECK(rb==PipeStatus::ready);CHECK(ra==PipeStatus::ready);if(rb!=PipeStatus::ready || ra!=PipeStatus::ready)return 2;
        PipeConnection rejectedPeerBefore,rejectedPeerAfter;
        const auto cb=rejectedPeerBefore.connect(rejectedNameBefore),ca=rejectedPeerAfter.connect(rejectedNameAfter);
        CHECK(cb==PipeStatus::ready);CHECK(ca==PipeStatus::ready);if(cb!=PipeStatus::ready || ca!=PipeStatus::ready)return 2;
        CHECK(rejectedBefore.pendingEvent()!=nullptr && rejectedAfter.pendingEvent()!=nullptr);

        // Capacity alone must not close new frame admission on existing owners.
        // Poison is a distinct gate, even though admissionAvailable reports both.
        std::vector<PipeListener> capacity;
        for(int i=0;i<34 && Test::admissionAvailable();++i){capacity.emplace_back();const auto opened=capacity.back().open(suffix());CHECK(opened==PipeStatus::ready);if(opened!=PipeStatus::ready)return 2;}
        CHECK(!capacity.empty());CHECK(!Test::admissionAvailable());CHECK(Test::retainedOwners()==0);
        CHECK(healthyBefore.server.beginRead()==PipeStatus::ready);CHECK(healthyBefore.client.beginWrite(dataFrame())==PipeStatus::ready);
        CHECK(until([&]{return healthyBefore.client.pollWrite();})==PipeStatus::complete);Frame capacityFrame;
        CHECK(until([&]{return healthyBefore.server.pollRead(capacityFrame);})==PipeStatus::complete);CHECK(capacityFrame.payload==dataFrame().payload);
        CHECK(healthyBefore.client.beginRead()==PipeStatus::ready);CHECK(healthyBefore.server.beginWrite(dataFrame())==PipeStatus::ready);
        CHECK(until([&]{return healthyBefore.server.pollWrite();})==PipeStatus::complete);
        CHECK(until([&]{return healthyBefore.client.pollRead(capacityFrame);})==PipeStatus::complete);
        capacity.clear();CHECK(Test::admissionAvailable());

        const auto large=dataFrame(5*1024*1024,MessageKind::result);
        CHECK(inFlight.server.beginRead()==PipeStatus::ready);CHECK(inFlight.client.beginRead()==PipeStatus::ready);
        CHECK(inFlight.server.beginWrite(large)==PipeStatus::ready);CHECK(inFlight.client.beginWrite(large)==PipeStatus::ready);
        auto incomingServer=sentinel(),incomingClient=sentinel();
        // A poll would now take each whole frame; both must stay in flight, first pieces arrived and unread.
        CHECK(WaitForSingleObject(inFlight.server.pendingEvents()[0],5000)==WAIT_OBJECT_0);CHECK(WaitForSingleObject(inFlight.client.pendingEvents()[0],5000)==WAIT_OBJECT_0);
        CHECK(inFlight.server.pollWrite()==PipeStatus::pending);CHECK(inFlight.client.pollWrite()==PipeStatus::pending);

        CHECK(p.server.beginRead()==PipeStatus::ready);CHECK(p.listener.pendingEvent()!=nullptr);
        Test::withholdCompletion(p.server,true);Test::withholdCompletion(p.listener,true);
        const auto t=GetTickCount64();p.server.close();p.listener.close();
        CHECK(GetTickCount64()-t<=1000);CHECK(Test::retainedOwners()>0);CHECK(!Test::admissionAvailable());
        PipeListener blocked;CHECK(blocked.open(suffix())==PipeStatus::unavailable);
        PipeConnection blockedClient;CHECK(blockedClient.connect(suffix())==PipeStatus::unavailable);

        auto refuseNewFrames=[&](Pair& healthy){
            const auto peer=healthy.server.peer();auto output=sentinel();
            CHECK(healthy.server.beginRead()==PipeStatus::unavailable);
            CHECK(healthy.server.beginWrite(dataFrame())==PipeStatus::unavailable);
            CHECK(healthy.server.pollRead(output)==PipeStatus::ready);CHECK(isSentinel(output));
            CHECK(healthy.server.pollWrite()==PipeStatus::ready);
            CHECK(healthy.server.pendingEvents()[0]==nullptr && healthy.server.pendingEvents()[1]==nullptr);
            CHECK(healthy.server.connected());CHECK(healthy.server.peer().processId==peer.processId && healthy.server.peer().creationTime==peer.creationTime);
            CHECK(healthy.client.beginRead()==PipeStatus::unavailable);CHECK(healthy.client.beginWrite(dataFrame())==PipeStatus::unavailable);
            CHECK(healthy.client.pendingEvents()[0]==nullptr && healthy.client.pendingEvents()[1]==nullptr);
        };
        auto endpointStillOwned=[&](const juce::String& name){
            const auto path=currentUserPipeName(name);
            HANDLE collision=CreateNamedPipeW(path.toWideCharPointer(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
                PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,18,65536,65536,5000,nullptr);
            const auto error=GetLastError();CHECK(collision==INVALID_HANDLE_VALUE);CHECK(error==ERROR_ACCESS_DENIED);
            if(collision!=INVALID_HANDLE_VALUE)CloseHandle(collision);
        };
        auto refuseListenerStarts=[&](PipeListener& paused,const juce::String& pausedName,PipeListener& rejected,const juce::String& rejectedName){
            PipeConnection output;Test::pauseConnect(paused,false);
            CHECK(paused.pollAccept(output)==PipeStatus::unavailable);CHECK(paused.pendingEvent()==nullptr);
            CHECK(!output.connected() && output.peer().processId==0 && output.pendingEvents()[0]==nullptr && output.pendingEvents()[1]==nullptr);
            CHECK(until([&]{return rejected.pollAccept(output);})==PipeStatus::unavailable);CHECK(rejected.pendingEvent()==nullptr);
            CHECK(!output.connected() && output.peer().processId==0);
            endpointStillOwned(pausedName);endpointStillOwned(rejectedName);
        };
        refuseNewFrames(healthyBefore);refuseListenerStarts(pausedBefore,pausedNameBefore,rejectedBefore,rejectedNameBefore);
        CHECK(inFlight.server.beginRead()==PipeStatus::busy);CHECK(inFlight.server.beginWrite(large)==PipeStatus::busy);
        CHECK(inFlight.client.beginRead()==PipeStatus::busy);CHECK(inFlight.client.beginWrite(large)==PipeStatus::busy);
        const auto serverEvents=inFlight.server.pendingEvents(),clientEvents=inFlight.client.pendingEvents();
        CHECK(serverEvents[0]!=nullptr && serverEvents[1]!=nullptr && serverEvents[0]!=serverEvents[1]);
        CHECK(clientEvents[0]!=nullptr && clientEvents[1]!=nullptr && clientEvents[0]!=clientEvents[1]);
        PipeStatus readServer=PipeStatus::pending,readClient=PipeStatus::pending,writeServer=PipeStatus::pending,writeClient=PipeStatus::pending;
        const auto finish=GetTickCount64()+8000;
        while((readServer==PipeStatus::pending || readClient==PipeStatus::pending || writeServer==PipeStatus::pending || writeClient==PipeStatus::pending) && GetTickCount64()<finish){
            if(readServer==PipeStatus::pending)readServer=inFlight.server.pollRead(incomingServer);
            if(readClient==PipeStatus::pending)readClient=inFlight.client.pollRead(incomingClient);
            if(writeServer==PipeStatus::pending)writeServer=inFlight.server.pollWrite();
            if(writeClient==PipeStatus::pending)writeClient=inFlight.client.pollWrite();
            Sleep(1);
        }
        CHECK(readServer==PipeStatus::complete && readClient==PipeStatus::complete);
        CHECK(writeServer==PipeStatus::complete && writeClient==PipeStatus::complete);
        CHECK(incomingServer.kind==MessageKind::result && incomingServer.session==large.session && incomingServer.payload==large.payload);
        CHECK(incomingClient.kind==MessageKind::result && incomingClient.session==large.session && incomingClient.payload==large.payload);
        CHECK(inFlight.server.pendingEvents()[0]==nullptr && inFlight.server.pendingEvents()[1]==nullptr);
        CHECK(inFlight.client.pendingEvents()[0]==nullptr && inFlight.client.pendingEvents()[1]==nullptr);

        Test::releaseWithheldCompletions();const auto end=GetTickCount64()+1000;
        do{Test::reapRetired();if(Test::retainedOwners()==0)break;Sleep(1);}while(GetTickCount64()<end);
        CHECK(Test::retainedOwners()==0);CHECK(!Test::admissionAvailable());p.client.close();
        refuseNewFrames(healthyAfter);refuseNewFrames(inFlight);
        refuseListenerStarts(pausedAfter,pausedNameAfter,rejectedAfter,rejectedNameAfter);
        PipeConnection output;CHECK(pausedBefore.pollAccept(output)==PipeStatus::unavailable);CHECK(pausedBefore.pendingEvent()==nullptr);
        CHECK(rejectedBefore.pollAccept(output)==PipeStatus::unavailable);CHECK(rejectedBefore.pendingEvent()==nullptr);
        endpointStillOwned(pausedNameBefore);endpointStillOwned(rejectedNameBefore);
        CHECK(blocked.open(suffix())==PipeStatus::unavailable);CHECK(blockedClient.connect(suffix())==PipeStatus::unavailable);
        healthyBefore.server.close();healthyBefore.client.close();healthyAfter.server.close();healthyAfter.client.close();
        inFlight.server.close();inFlight.client.close();pausedBefore.close();pausedAfter.close();rejectedBefore.close();rejectedAfter.close();
        rejectedPeerBefore.close();rejectedPeerAfter.close();
        CHECK(Test::retainedOwners()==0);CHECK(!Test::admissionAvailable());
        std::cout<<"exceptional child checks="<<checks<<" failures="<<failures<<"\n";return failures==0?0:1;
    }
    if(mode=="echo" || mode=="wrong-client"){
        PipeConnection c;if(mode=="wrong-client")Test::expectPeerSid(c,wrongSid());
        const auto status=c.connect(s);if(mode=="wrong-client")return status==PipeStatus::unauthorized?0:3;
        if(status!=PipeStatus::ready)return 4;
        if(!peerIs(c.peer(),static_cast<DWORD>(a[3].getLargeIntValue())))return 5;
        Frame f=dataFrame();if(c.beginWrite(f)!=PipeStatus::ready)return 6;
        if(until([&]{return c.pollWrite();})!=PipeStatus::complete)return 7;
        if(c.beginRead()!=PipeStatus::ready)return 8;
        Frame out;if(until([&]{return c.pollRead(out);})!=PipeStatus::complete)return 9;
        return out.kind==f.kind && out.session==f.session && out.payload==f.payload?0:10;
    }
    Raw r;if(!r.connect(s))return 11;
    if(mode=="hold"){Sleep(350);return 0;}
    if(mode=="stall"){Sleep(6200);return 0;}
    if(mode=="idle"){Sleep(5200);auto b=rawFixture(0);return r.write(b.getData(),b.getSize())?0:12;}
    if(mode=="fragment-slow"){
        auto b=rawFixture(0);r.write(b.getData(),1);Sleep(2700);r.write(static_cast<std::byte*>(b.getData())+1,1);Sleep(2700);
        r.write(static_cast<std::byte*>(b.getData())+2,b.getSize()-2);return 0;
    }
    if(mode=="late-rest"){auto b=rawFixture(0);if(!r.write(b.getData(),1))return 19;Sleep(300);return r.write(static_cast<std::byte*>(b.getData())+1,b.getSize()-1)?0:20;}
    if(mode=="raw"){
        auto b=rawFixture(a[3].getIntValue());r.write(b.getData(),b.getSize(),a[3]=="0"?1:65536,a[3]=="0"?1:0);return 0;
    }
    if(mode=="mid-close"){std::byte data[32]{};DWORD done=0;return r.io(false,data,32,done)?0:13;}
    if(mode=="slow"){
        auto f=cancelFrame();juce::MemoryBlock b;if(!encodeFrame(f,b) || !r.write(b.getData(),b.getSize()))return 14;
        Sleep(350);std::vector<std::byte> all;all.reserve(28+5*1024*1024);std::byte chunk[4096];
        while(all.size()<28+5*1024*1024){DWORD done=0;if(!r.io(false,chunk,static_cast<DWORD>((std::min)(sizeof chunk,28+5*1024*1024-all.size())),done))return 15;
            all.insert(all.end(),chunk,chunk+done);Sleep(1);}
        Frame received;if(!decodeFrame({all.data(),all.size()},received))return 16;
        return received.kind==MessageKind::result && received.payload.getSize()==5*1024*1024
            && std::all_of(static_cast<const unsigned char*>(received.payload.getData()),static_cast<const unsigned char*>(received.payload.getData())+received.payload.getSize(),[](auto v){return v==0x55;})?0:17;
    }
    return 18;
}
bool childConnection(PipeListener& listener,PipeConnection& server,Child& child,const juce::String& mode,const juce::String& s,const juce::String& extra={})
{
    auto o=listener.open(s);CHECK(o==PipeStatus::ready);if(o!=PipeStatus::ready)return false;
    CHECK(child.start({"--child",mode,s,extra}));if(!child.started)return false;
    auto a=until([&]{return listener.pollAccept(server);});CHECK(a==PipeStatus::complete);return a==PipeStatus::complete;
}
void endpointAndIdentity()
{
    begin("endpoint_and_peer_identity");
    const auto user=sidOf(GetCurrentProcess());CHECK(user.isNotEmpty());
    CHECK(currentUserPipeName()=="\\\\.\\pipe\\AIRackSynth-v"+juce::String(wireVersion)+"-accounts"+juce::String(providerAccountVersion)+"-"+user);
    CHECK(currentUserPipeName("bad/path").isEmpty());CHECK(currentUserPipeName(juce::String::repeatedString("a",33)).isEmpty());
    CHECK(currentUserPipeName("ABC_12-xy")=="\\\\.\\pipe\\AIRackSynth-v"+juce::String(wireVersion)+"-accounts"+juce::String(providerAccountVersion)+"-"+user+"-ABC_12-xy");
    PipeListener invalid;CHECK(invalid.open("..")==PipeStatus::unavailable);
    PipeListener dacl;auto ds=suffix();auto opened=dacl.open(ds);CHECK(opened==PipeStatus::ready);
    if(opened==PipeStatus::ready){
        Raw raw;CHECK(raw.connect(ds));if(raw.pipe!=INVALID_HANDLE_VALUE){
            PSECURITY_DESCRIPTOR sd=nullptr;PACL acl=nullptr;
            auto err=GetSecurityInfo(raw.pipe,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&sd);CHECK(err==ERROR_SUCCESS);
            if(err==ERROR_SUCCESS){SECURITY_DESCRIPTOR_CONTROL control{};DWORD revision=0;CHECK(GetSecurityDescriptorControl(sd,&control,&revision));CHECK((control&SE_DACL_PROTECTED)!=0);
                CHECK(acl!=nullptr && acl->AceCount==1);if(acl!=nullptr && acl->AceCount==1){void* ace=nullptr;CHECK(GetAce(acl,0,&ace));if(ace!=nullptr){auto* allow=static_cast<ACCESS_ALLOWED_ACE*>(ace);CHECK(allow->Header.AceType==ACCESS_ALLOWED_ACE_TYPE);CHECK(allow->Header.AceFlags==0);
                    LPWSTR sid=nullptr;CHECK(ConvertSidToStringSidW(&allow->SidStart,&sid));if(sid!=nullptr){CHECK(juce::String(sid)==user);LocalFree(sid);}CHECK((allow->Mask&FILE_ALL_ACCESS)==FILE_ALL_ACCESS);}}
                LocalFree(sd);}
        }dacl.close();
    }
    PipeListener l;PipeConnection server;Child child;auto s=suffix();
    if(childConnection(l,server,child,"echo",s,juce::String(GetCurrentProcessId()))){
        CHECK(server.peer().processId!=GetCurrentProcessId());CHECK(peerIs(server.peer(),server.peer().processId));
        PipeListener collision;CHECK(collision.open(s)==PipeStatus::busy);CHECK(l.open(s)==PipeStatus::busy);
        CHECK(server.beginRead()==PipeStatus::ready);Frame f;
        CHECK(until([&]{return server.pollRead(f);})==PipeStatus::complete);
        CHECK(server.beginWrite(f)==PipeStatus::ready);CHECK(until([&]{return server.pollWrite();})==PipeStatus::complete);CHECK(child.finish());
        l.close();CHECK(collision.open(s)==PipeStatus::busy);server.close();CHECK(collision.open(s)==PipeStatus::ready);
    }
    PipeListener wrong;Test::expectPeerSid(wrong,wrongSid());auto ws=suffix();auto wo=wrong.open(ws);CHECK(wo==PipeStatus::ready);
    if(wo==PipeStatus::ready){Child c;CHECK(c.start({"--child","hold",ws}));PipeConnection out;
        CHECK(until([&]{return wrong.pollAccept(out);})==PipeStatus::unauthorized);CHECK(!out.connected());CHECK(out.peer().processId==0);
        PipeListener collision;CHECK(collision.open(ws)==PipeStatus::busy);CHECK(c.finish());
        // Rejection must retain and rearm the same endpoint; another child still reaches it.
        Child again;CHECK(again.start({"--child","hold",ws}));CHECK(until([&]{return wrong.pollAccept(out);})==PipeStatus::unauthorized);CHECK(again.finish());
    }
    PipeListener clientReject;auto cs=suffix();auto co=clientReject.open(cs);CHECK(co==PipeStatus::ready);
    if(co==PipeStatus::ready){Child c;CHECK(c.start({"--child","wrong-client",cs}));CHECK(c.finish());}
}
void fragmentedAndBounded()
{
    begin("fragmented_and_bounded_frames");
    for(int fixture=0;fixture<=12;++fixture){
        PipeListener l;PipeConnection c;Child raw;auto s=suffix();
        if(!childConnection(l,c,raw,"raw",s,juce::String(fixture)))continue;
        CHECK(c.beginRead()==PipeStatus::ready);CHECK(c.beginRead()==PipeStatus::busy);auto out=sentinel();
        const auto status=until([&]{return c.pollRead(out);});
        if(fixture==0 || fixture==10 || fixture==11){CHECK(status==PipeStatus::complete);CHECK(out.payload.getSize()==(fixture==0?3u:fixture==10?maximumGenerationRequestBytes:maximumResultPayloadBytes));
            CHECK(out.kind==(fixture==11?MessageKind::result:MessageKind::generate));CHECK(c.pollRead(out)==PipeStatus::ready);
            CHECK(static_cast<const unsigned char*>(out.payload.getData())[out.payload.getSize()-1]==0x55);
        }else{CHECK(status==(fixture==6?PipeStatus::closed:PipeStatus::invalidMessage));CHECK(isSentinel(out));CHECK(!c.connected());}
        CHECK(raw.finish());
    }
    Pair p;if(p.open()){
        auto bad=dataFrame(maximumGenerationRequestBytes+1);CHECK(p.client.beginWrite(bad)==PipeStatus::invalidMessage);CHECK(p.client.connected());
        bad=dataFrame(maximumResultPayloadBytes+1,MessageKind::result);CHECK(p.client.beginWrite(bad)==PipeStatus::invalidMessage);
        bad=cancelFrame();bad.payload.setSize(1);CHECK(p.client.beginWrite(bad)==PipeStatus::invalidMessage);
        bad=cancelFrame();bad.payload.setSize(maximumSmallPayloadBytes+1);CHECK(p.client.beginWrite(bad)==PipeStatus::invalidMessage);
    }
}
void duplexAndPartialWrite()
{
    begin("duplex_and_partial_write");
    PipeListener l;PipeConnection c;Child slow;auto s=suffix();
    if(childConnection(l,c,slow,"slow",s)){
        auto large=dataFrame(5*1024*1024,MessageKind::result);
        CHECK(c.beginRead()==PipeStatus::ready);CHECK(c.beginWrite(large)==PipeStatus::ready);CHECK(c.beginWrite(large)==PipeStatus::busy);
        auto e=c.pendingEvents();CHECK(e[0]!=nullptr && e[1]!=nullptr && e[0]!=e[1]);
        Frame control;PipeStatus read=PipeStatus::pending,write=PipeStatus::pending;const auto end=GetTickCount64()+8000;
        while(read==PipeStatus::pending && GetTickCount64()<end){read=c.pollRead(control);write=c.pollWrite();waitEvents(c);}
        CHECK(read==PipeStatus::complete);CHECK(control.kind==MessageKind::cancel);CHECK(write==PipeStatus::pending);
        CHECK(until([&]{return c.pollWrite();})==PipeStatus::complete);CHECK(c.pendingEvents()[1]==nullptr);CHECK(slow.finish());
    }
    PipeListener brokenListener;PipeConnection broken;Child closer;auto bs=suffix();
    if(childConnection(brokenListener,broken,closer,"mid-close",bs)){
        CHECK(broken.beginWrite(dataFrame(5*1024*1024,MessageKind::result))==PipeStatus::ready);
        auto status=until([&]{return broken.pollWrite();});CHECK(status==PipeStatus::closed);CHECK(!broken.connected());
        CHECK(broken.beginWrite(dataFrame())==PipeStatus::closed);CHECK(closer.finish());
        PipeConnection fresh;CHECK(fresh.connect(bs)==PipeStatus::ready);PipeConnection accepted;
        CHECK(until([&]{return brokenListener.pollAccept(accepted);})==PipeStatus::complete);CHECK(accepted.connected());
    }
}
void deadlinesIdleAndReconnect()
{
    begin("deadlines_idle_and_reconnect");
    PipeConnection empty;auto out=sentinel();CHECK(empty.pollRead(out)==PipeStatus::closed);CHECK(isSentinel(out));CHECK(empty.pollWrite()==PipeStatus::closed);
    CHECK(empty.connect(suffix())==PipeStatus::unavailable);CHECK(empty.peer().processId==0);CHECK(empty.pendingEvents()[0]==nullptr);
    PipeListener early;Test::pauseConnect(early,true);auto es=suffix();auto eo=early.open(es);CHECK(eo==PipeStatus::ready);
    if(eo==PipeStatus::ready){CHECK(early.pendingEvent()==nullptr);PipeConnection client;CHECK(client.connect(es)==PipeStatus::ready);
        PipeConnection server;Test::pauseConnect(early,false);CHECK(until([&]{return early.pollAccept(server);})==PipeStatus::complete);
        CHECK(early.pollAccept(server)==PipeStatus::busy);CHECK(server.beginRead()==PipeStatus::ready);
        PipeConnection moved=std::move(server);CHECK(!server.connected());CHECK(server.peer().processId==0);CHECK(moved.connected());
        PipeConnection assigned;assigned=std::move(moved);CHECK(!moved.connected());CHECK(assigned.connected());
        CHECK(client.beginWrite(dataFrame())==PipeStatus::ready);CHECK(until([&]{return client.pollWrite();})==PipeStatus::complete);
        Frame received;CHECK(until([&]{return assigned.pollRead(received);})==PipeStatus::complete);CHECK(assigned.pollRead(received)==PipeStatus::ready);
        PipeListener transferred=std::move(early);CHECK(early.pendingEvent()==nullptr);CHECK(transferred.pendingEvent()!=nullptr);
    }
    {PipeListener l;PipeConnection c;Child slow;auto s=suffix();if(childConnection(l,c,slow,"fragment-slow",s)){
        CHECK(c.beginRead()==PipeStatus::ready);auto f=sentinel();const auto t=GetTickCount64();
        auto status=until([&]{return c.pollRead(f);},7000);CHECK(status==PipeStatus::timeout);CHECK(GetTickCount64()-t>=5000 && GetTickCount64()-t<6200);CHECK(isSentinel(f));CHECK(!c.connected());CHECK(slow.finish());}}
    {PipeListener l;PipeConnection c;Child idle;auto s=suffix();if(childConnection(l,c,idle,"idle",s)){
        CHECK(c.beginRead()==PipeStatus::ready);Frame f;const auto t=GetTickCount64();
        CHECK(until([&]{return c.pollRead(f);},7000)==PipeStatus::complete);CHECK(GetTickCount64()-t>=5000);CHECK(c.connected());CHECK(idle.finish());}}
    {PipeListener l;PipeConnection c;Child stall;auto s=suffix();if(childConnection(l,c,stall,"stall",s)){
        CHECK(c.beginWrite(dataFrame(5*1024*1024,MessageKind::result))==PipeStatus::ready);const auto t=GetTickCount64();
        CHECK(until([&]{return c.pollWrite();},7000)==PipeStatus::timeout);CHECK(GetTickCount64()-t>=4900 && GetTickCount64()-t<6200);CHECK(!c.connected());CHECK(stall.finish());}}
}
// A transfer that finished counts however late it is polled, and a frame written at once is read by one poll.
void latePollsAndWholeFrames()
{
    begin("late_polls_and_whole_frames");
    {Pair p;if(p.open()){CHECK(p.client.beginWrite(dataFrame())==PipeStatus::ready);Sleep(5200);CHECK(p.client.pollWrite()==PipeStatus::complete);
        Frame f;CHECK(p.server.beginRead()==PipeStatus::ready);CHECK(until([&]{return p.server.pollRead(f);})==PipeStatus::complete);}}
    {Pair p;if(p.open()){const auto large=dataFrame(1730000,MessageKind::result);CHECK(p.client.beginRead()==PipeStatus::ready);CHECK(p.server.beginWrite(large)==PipeStatus::ready);
        const auto event=p.client.pendingEvents()[0];CHECK(event!=nullptr && WaitForSingleObject(event,5000)==WAIT_OBJECT_0);
        Frame f;CHECK(p.client.pollRead(f)==PipeStatus::complete);CHECK(f.kind==MessageKind::result && f.payload==large.payload);CHECK(until([&]{return p.server.pollWrite();})==PipeStatus::complete);}}
    {PipeListener l;PipeConnection c;Child late;auto s=suffix();if(childConnection(l,c,late,"late-rest",s)){
        CHECK(c.beginRead()==PipeStatus::ready);const auto event=c.pendingEvents()[0];CHECK(event!=nullptr && WaitForSingleObject(event,5000)==WAIT_OBJECT_0);
        Frame f;CHECK(c.pollRead(f)==PipeStatus::pending);Sleep(5200);CHECK(c.pollRead(f)==PipeStatus::complete);CHECK(late.finish());}}
}
void cancelAndRetirement()
{
    begin("cancel_and_handle_retirement");
    DWORD baseline=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&baseline));
    for(int i=0;i<32;++i){Pair p;if(!p.open())continue;
        CHECK(p.server.beginRead()==PipeStatus::ready);CHECK(p.server.pendingEvents()[0]!=nullptr);CHECK(p.server.pendingEvents()[1]==nullptr);
        if(i%2==0){CHECK(p.client.beginWrite(dataFrame())==PipeStatus::ready);CHECK(until([&]{return p.client.pollWrite();})==PipeStatus::complete);}
        CHECK(p.server.beginWrite(dataFrame(5*1024*1024,MessageKind::result))==PipeStatus::ready);
        const auto t=GetTickCount64();p.server.close();p.server.close();p.listener.close();p.client.close();
        CHECK(GetTickCount64()-t<=1000);CHECK(p.server.pendingEvents()[0]==nullptr && p.server.pendingEvents()[1]==nullptr);CHECK(p.server.peer().processId==0);
        CHECK(Test::retainedOwners()==0);CHECK(Test::admissionAvailable());
    }
    DWORD after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));CHECK(after<=baseline+2);
    std::vector<PipeListener> owners;owners.reserve(34);
    for(int i=0;i<34;++i){owners.emplace_back();CHECK(owners.back().open(suffix())==PipeStatus::ready);}
    DWORD atLimit=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&atLimit));PipeListener extra;
    CHECK(extra.open(suffix())==PipeStatus::busy);DWORD refused=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&refused));CHECK(refused==atLimit);
    owners.clear();CHECK(Test::retainedOwners()==0);CHECK(Test::admissionAvailable());
}
void exceptionalRetention()
{
    begin("exceptional_retention");Child c;CHECK(c.start({"--child","retention",suffix()}));CHECK(c.finish());
    CHECK(Test::admissionAvailable());CHECK(Test::retainedOwners()==0);
}
}
int main(int argc,char** argv)
{
    juce::StringArray args;for(int i=1;i<argc;++i)args.add(juce::String::fromUTF8(argv[i]));
    if(args[0]=="--child")return childMain(args);
    endpointAndIdentity();fragmentedAndBounded();duplexAndPartialWrite();deadlinesIdleAndReconnect();latePollsAndWholeFrames();cancelAndRetirement();exceptionalRetention();
    finishGroup();
    std::cout<<"Pipe checks="<<checks<<" groups="<<groups<<" failures="<<failures<<"\n";return failures==0?0:1;
}

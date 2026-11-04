#include "CompanionPipe.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Sddl.h>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

namespace airack::companion
{
namespace
{
struct Native;
struct Owners
{
    std::mutex mutex;
    std::size_t count=0;
    bool poisoned=false;
    // Raw fixed slots have no owning destructor: OS teardown, never destruction
    // of pending IO, owns process exit. Only observed completion permits delete.
    std::array<Native*,maximumPipeOwners> retained{};
};
Owners& owners() {static Owners value;return value;}
bool nativeIoPoisoned()
{auto& state=owners();std::lock_guard lock(state.mutex);return state.poisoned;}
struct Handle
{
    HANDLE value=nullptr;
    ~Handle(){if(value!=nullptr && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
juce::String userSid(HANDLE process)
{
    Handle token;if(!OpenProcessToken(process,TOKEN_QUERY,&token.value))return {};
    DWORD required=0;
    if(GetTokenInformation(token.value,TokenUser,nullptr,0,&required) || GetLastError()!=ERROR_INSUFFICIENT_BUFFER
        || required==0 || required>65536)return {};
    std::vector<std::byte> storage(required);
    if(!GetTokenInformation(token.value,TokenUser,storage.data(),required,&required))return {};
    auto* user=reinterpret_cast<TOKEN_USER*>(storage.data());if(!IsValidSid(user->User.Sid))return {};
    LPWSTR text=nullptr;if(!ConvertSidToStringSidW(user->User.Sid,&text))return {};
    try{juce::String result(text);LocalFree(text);return result;}catch(...){LocalFree(text);throw;}
}
PipeStatus identity(HANDLE pipe,bool server,const juce::String& expected,PipePeer& out)
{
    ULONG pid=0;
    if(!(server?GetNamedPipeClientProcessId(pipe,&pid):GetNamedPipeServerProcessId(pipe,&pid)) || pid==0)return PipeStatus::unavailable;
    Handle process;process.value=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(process.value==nullptr)return PipeStatus::unavailable;
    FILETIME created{},exited{},kernel{},user{};
    if(!GetProcessTimes(process.value,&created,&exited,&kernel,&user))return PipeStatus::unavailable;
    const auto actual=userSid(process.value);if(actual.isEmpty() || expected.isEmpty())return PipeStatus::unavailable;
    PSID actualSid=nullptr,expectedSid=nullptr;
    const bool converted=ConvertStringSidToSidW(actual.toWideCharPointer(),&actualSid)
        && ConvertStringSidToSidW(expected.toWideCharPointer(),&expectedSid);
    const bool same=converted && IsValidSid(actualSid) && IsValidSid(expectedSid) && EqualSid(actualSid,expectedSid);
    if(actualSid!=nullptr)LocalFree(actualSid);if(expectedSid!=nullptr)LocalFree(expectedSid);
    if(!converted)return PipeStatus::unavailable;if(!same)return PipeStatus::unauthorized;
    const auto time=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    if(time==0)return PipeStatus::unavailable;out={pid,time};return PipeStatus::ready;
}
struct Operation
{
    HANDLE event=nullptr;
    OVERLAPPED overlap{};
    bool active=false,pending=false;
    void reset(){overlap={};overlap.hEvent=event;ResetEvent(event);active=false;pending=false;}
};
struct Native
{
    HANDLE pipe=INVALID_HANDLE_VALUE;
    Operation connect,read,write;
    PipePeer peer{};
    bool withhold=false,connectFailed=false;
    std::array<std::byte,pipeChunkBytes> chunk{};
    std::array<std::byte,wireHeaderBytes> header{};
    std::size_t headerRead=0,payloadRead=0,totalRead=0,writePosition=0;
    std::uint64_t readStarted=0,writeStarted=0,writeTimeout=pipeFrameTimeoutMs;
    juce::MemoryBlock incoming,outgoing;
    ~Native()
    {
        if(pipe!=INVALID_HANDLE_VALUE)CloseHandle(pipe);
        for(auto* op:{&connect,&read,&write})if(op->event!=nullptr)CloseHandle(op->event);
        auto& state=owners();std::lock_guard lock(state.mutex);--state.count;
    }
};
std::unique_ptr<Native> allocateNative(PipeStatus& status)
{
    auto& state=owners();
    {std::lock_guard lock(state.mutex);if(state.poisoned){status=PipeStatus::unavailable;return {};}
        if(state.count>=maximumPipeOwners){status=PipeStatus::busy;return {};}++state.count;}
    std::unique_ptr<Native> n;
    try{n=std::make_unique<Native>();}catch(const std::bad_alloc&){std::lock_guard lock(state.mutex);--state.count;status=PipeStatus::unavailable;return {};}
    for(auto* op:{&n->connect,&n->read,&n->write}){
        op->event=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(op->event==nullptr){status=PipeStatus::unavailable;return {};}op->reset();
    }
    status=PipeStatus::ready;return n;
}
// CancelIoEx/ERROR_NOT_FOUND is never completion proof. Normal completion may
// win cancellation; only an observed result ends native operation lifetime.
bool observed(Native& n,Operation& op,DWORD& bytes,DWORD& error)
{
    bytes=0;error=ERROR_SUCCESS;if(!op.pending)return true;
    if(n.withhold){error=ERROR_IO_INCOMPLETE;return false;}
    if(GetOverlappedResult(n.pipe,&op.overlap,&bytes,FALSE)){op.pending=false;return true;}
    error=GetLastError();if(error==ERROR_IO_INCOMPLETE)return false;op.pending=false;return true;
}
bool completed(Native& n)
{
    bool done=true;for(auto* op:{&n.connect,&n.read,&n.write}){DWORD bytes=0,error=0;if(!observed(n,*op,bytes,error))done=false;}return done;
}
void retire(std::unique_ptr<Native>& n) noexcept
{
    if(!n)return;n->peer={};
    for(auto* op:{&n->connect,&n->read,&n->write})if(op->pending)CancelIoEx(n->pipe,&op->overlap);
    const auto end=GetTickCount64()+pipeRetirementWaitMs;
    while(!completed(*n)){
        const auto now=GetTickCount64();if(now>=end)break;
        HANDLE events[3]{};DWORD count=0;for(auto* op:{&n->connect,&n->read,&n->write})if(op->pending)events[count++]=op->event;
        if(n->withhold)Sleep(1);
        else if(count!=0)WaitForMultipleObjects(count,events,FALSE,static_cast<DWORD>((std::min)(std::uint64_t(10),end-now)));
    }
    if(completed(*n)){n.reset();return;}
    auto& state=owners();std::lock_guard lock(state.mutex);state.poisoned=true;
    // Count is reserved before native IO; live plus retained owners <=34.
    for(auto& slot:state.retained)if(slot==nullptr){slot=n.release();return;}
    std::terminate(); // An impossible count violation must never free pending IO.
}
bool startConnect(Native& n)
{
    if(nativeIoPoisoned()){n.connectFailed=true;return false;}
    n.connect.reset();n.connect.active=true;
    const BOOL done=ConnectNamedPipe(n.pipe,&n.connect.overlap);const auto error=done?ERROR_SUCCESS:GetLastError();
    if(done || error==ERROR_IO_PENDING){n.connect.pending=true;if(done)SetEvent(n.connect.event);return true;}
    if(error==ERROR_PIPE_CONNECTED){SetEvent(n.connect.event);return true;}
    n.connect.active=false;n.connectFailed=true;return false;
}
std::unique_ptr<Native> serverNative(const juce::String& name,bool first,bool pause,PipeStatus& status,PipeDomain domain)
{
    auto n=allocateNative(status);if(!n)return {};
    const auto sid=userSid(GetCurrentProcess());if(sid.isEmpty()){status=PipeStatus::unavailable;return {};}
    PSECURITY_DESCRIPTOR descriptor=nullptr;const auto sddl="D:P(A;;GA;;;"+sid+")";
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.toWideCharPointer(),SDDL_REVISION_1,&descriptor,nullptr)){status=PipeStatus::unavailable;return {};}
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
    n->pipe=CreateNamedPipeW(name.toWideCharPointer(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|(first?FILE_FLAG_FIRST_PIPE_INSTANCE:0),
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,domain==PipeDomain::mcp?32:maximumPipeInstances,
        static_cast<DWORD>(pipeChunkBytes),static_cast<DWORD>(pipeChunkBytes),static_cast<DWORD>(pipeFrameTimeoutMs),&security);
    const auto error=n->pipe==INVALID_HANDLE_VALUE?GetLastError():ERROR_SUCCESS;LocalFree(descriptor);
    if(n->pipe==INVALID_HANDLE_VALUE){status=(error==ERROR_ACCESS_DENIED || error==ERROR_PIPE_BUSY)?PipeStatus::busy:PipeStatus::unavailable;return {};}
    if(!pause && !startConnect(*n)){status=PipeStatus::unavailable;return {};}
    status=PipeStatus::ready;return n;
}
bool issueRead(Native& n)
{
    n.read.reset();n.read.active=true;
    const auto left=n.headerRead<wireHeaderBytes?wireHeaderBytes-n.headerRead:n.totalRead-wireHeaderBytes-n.payloadRead;
    DWORD bytes=0;const BOOL ok=ReadFile(n.pipe,n.chunk.data(),static_cast<DWORD>((std::min)(left,pipeChunkBytes)),&bytes,&n.read.overlap);
    if(ok || GetLastError()==ERROR_IO_PENDING){n.read.pending=true;if(ok)SetEvent(n.read.event);return true;}
    n.read.active=false;return false;
}
bool issueWrite(Native& n)
{
    n.write.reset();n.write.active=true;DWORD bytes=0;
    const BOOL ok=WriteFile(n.pipe,static_cast<const std::byte*>(n.outgoing.getData())+n.writePosition,
        static_cast<DWORD>(n.outgoing.getSize()-n.writePosition),&bytes,&n.write.overlap);
    if(ok || GetLastError()==ERROR_IO_PENDING){n.write.pending=true;if(ok)SetEvent(n.write.event);return true;}
    n.write.active=false;return false;
}
template<class F> PipeStatus recover(F&& f)
{try{return f();}catch(const std::bad_alloc&){return PipeStatus::unavailable;}catch(const std::length_error&){return PipeStatus::unavailable;}}
}
struct PipeConnection::Impl {std::unique_ptr<Native> native;juce::String expected;};
struct PipeListener::Impl {std::unique_ptr<Native> native;juce::String expected,name;bool pause=false;PipeDomain domain=PipeDomain::companion;};
juce::String currentUserPipeName(const juce::String& testSuffix,PipeDomain domain)
{
    try{
        if(testSuffix.length()>32 || !testSuffix.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-"))return {};
        const auto sid=userSid(GetCurrentProcess());if(sid.isEmpty())return {};
        auto name=(domain==PipeDomain::mcp?juce::String("\\\\.\\pipe\\AIRackSynth-MCP-v1-"):"\\\\.\\pipe\\AIRackSynth-v"+juce::String(int(wireVersion))+"-accounts"+juce::String(int(providerAccountVersion))+"-")+sid;if(testSuffix.isNotEmpty())name+="-"+testSuffix;
        return name.length()<=256?name:juce::String{};
    }catch(const std::bad_alloc&){return {};}catch(const std::length_error&){return {};}
}
PipeConnection::PipeConnection()=default;
PipeConnection::~PipeConnection(){close();}
PipeConnection::PipeConnection(PipeConnection&&) noexcept=default;
PipeConnection& PipeConnection::operator=(PipeConnection&& other) noexcept
{if(this!=&other){close();impl=std::move(other.impl);}return *this;}
bool PipeConnection::connected() const noexcept {return impl && impl->native && impl->native->peer.processId!=0;}
PipePeer PipeConnection::peer() const noexcept {return connected()?impl->native->peer:PipePeer{};}
PipeStatus PipeConnection::connect(const juce::String& testSuffix,PipeDomain domain)
{
    if(connected())return PipeStatus::busy;
    return recover([&]{
        const auto name=currentUserPipeName(testSuffix,domain);if(name.isEmpty())return PipeStatus::unavailable;
        if(!impl)impl=std::make_unique<Impl>();
        PipeStatus status;auto n=allocateNative(status);if(!n)return status;
        n->pipe=CreateFileW(name.toWideCharPointer(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(n->pipe==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_PIPE_BUSY?PipeStatus::busy:PipeStatus::unavailable;
        status=identity(n->pipe,false,impl->expected.isEmpty()?userSid(GetCurrentProcess()):impl->expected,n->peer);
        if(status!=PipeStatus::ready)return status;impl->native=std::move(n);return PipeStatus::ready;
    });
}
PipeStatus PipeConnection::beginRead()
{
    if(!connected())return PipeStatus::closed;auto& n=*impl->native;if(n.read.active)return PipeStatus::busy;
    if(nativeIoPoisoned())return PipeStatus::unavailable;
    n.headerRead=n.payloadRead=n.totalRead=0;n.readStarted=0;n.incoming.reset();
    if(issueRead(n))return PipeStatus::ready;close();return PipeStatus::closed;
}
PipeStatus PipeConnection::pollRead(Frame& out)
{
    if(!connected())return PipeStatus::closed;auto& n=*impl->native;if(!n.read.active)return PipeStatus::ready;
    // A finished read counts however late it is seen; the frame's deadline bounds only waiting.
    for(;;){
        DWORD size=0,error=0;
        if(!observed(n,n.read,size,error)){
            if(n.readStarted!=0 && GetTickCount64()-n.readStarted>=pipeFrameTimeoutMs){close();return PipeStatus::timeout;}
            return PipeStatus::pending;
        }
        n.read.active=false;
        if(error!=ERROR_SUCCESS || size==0){close();return PipeStatus::closed;}if(n.readStarted==0)n.readStarted=GetTickCount64();
        const auto status=recover([&]{
            if(n.headerRead<wireHeaderBytes){
                if(size>wireHeaderBytes-n.headerRead)return PipeStatus::invalidMessage;
                std::memcpy(n.header.data()+n.headerRead,n.chunk.data(),size);n.headerRead+=size;
                if(n.headerRead==wireHeaderBytes){FrameHeader h;if(!decodeFrameHeader({n.header.data(),n.header.size()},h))return PipeStatus::invalidMessage;
                    n.totalRead=wireHeaderBytes+h.payloadBytes;n.incoming.setSize(n.totalRead);std::memcpy(n.incoming.getData(),n.header.data(),wireHeaderBytes);}
            }else{
                if(size>n.totalRead-wireHeaderBytes-n.payloadRead)return PipeStatus::invalidMessage;
                std::memcpy(static_cast<std::byte*>(n.incoming.getData())+wireHeaderBytes+n.payloadRead,n.chunk.data(),size);n.payloadRead+=size;
            }
            if(n.headerRead==wireHeaderBytes && wireHeaderBytes+n.payloadRead==n.totalRead){
                Frame decoded;if(!decodeFrame({static_cast<const std::byte*>(n.incoming.getData()),n.incoming.getSize()},decoded))return PipeStatus::invalidMessage;
                out=std::move(decoded);n.incoming.reset();n.readStarted=0;return PipeStatus::complete;
            }
            return issueRead(n)?PipeStatus::pending:PipeStatus::closed;
        });
        if(status!=PipeStatus::pending){if(status!=PipeStatus::complete)close();return status;}
    }
}
PipeStatus PipeConnection::beginWrite(const Frame& frame,std::uint64_t timeoutMs)
{
    if(!connected())return PipeStatus::closed;auto& n=*impl->native;if(n.write.active)return PipeStatus::busy;
    if(nativeIoPoisoned())return PipeStatus::unavailable;
    juce::MemoryBlock block;if(!encodeFrame(frame,block))return PipeStatus::invalidMessage;
    n.outgoing.swapWith(block);n.writePosition=0;n.writeStarted=GetTickCount64();n.writeTimeout=timeoutMs;
    if(issueWrite(n))return PipeStatus::ready;close();return PipeStatus::closed;
}
PipeStatus PipeConnection::pollWrite()
{
    if(!connected())return PipeStatus::closed;auto& n=*impl->native;if(!n.write.active)return PipeStatus::ready;
    DWORD size=0,error=0;
    if(!observed(n,n.write,size,error)){if(GetTickCount64()-n.writeStarted>=n.writeTimeout){close();return PipeStatus::timeout;}return PipeStatus::pending;}
    n.write.active=false;
    if(error!=ERROR_SUCCESS || size==0 || size>n.outgoing.getSize()-n.writePosition){close();return PipeStatus::closed;}
    n.writePosition+=size;if(n.writePosition==n.outgoing.getSize()){n.outgoing.reset();n.writeStarted=0;return PipeStatus::complete;}
    if(issueWrite(n))return PipeStatus::pending;close();return PipeStatus::closed;
}
std::array<void*,2> PipeConnection::pendingEvents() const noexcept
{
    if(!connected())return {};const auto& n=*impl->native;
    return {n.read.active?n.read.event:nullptr,n.write.active?n.write.event:nullptr};
}
void PipeConnection::close() noexcept {if(impl)retire(impl->native);}
PipeListener::PipeListener()=default;
PipeListener::~PipeListener(){close();}
PipeListener::PipeListener(PipeListener&&) noexcept=default;
PipeListener& PipeListener::operator=(PipeListener&& other) noexcept
{if(this!=&other){close();impl=std::move(other.impl);}return *this;}
PipeStatus PipeListener::open(const juce::String& testSuffix,PipeDomain domain)
{
    if(impl && impl->native)return PipeStatus::busy;
    return recover([&]{
        const auto name=currentUserPipeName(testSuffix,domain);if(name.isEmpty())return PipeStatus::unavailable;
        if(!impl)impl=std::make_unique<Impl>();
        // Finish potentially allocating configuration before starting pending
        // ConnectNamedPipe, so an exception cannot destroy unobserved native IO.
        impl->name=name;impl->domain=domain;
        PipeStatus status;auto n=serverNative(name,true,impl->pause,status,domain);if(!n)return status;
        impl->native=std::move(n);return PipeStatus::ready;
    });
}
PipeStatus PipeListener::pollAccept(PipeConnection& out)
{
    if(!impl || !impl->native)return PipeStatus::closed;if(out.connected())return PipeStatus::busy;
    auto& n=*impl->native;if(n.connectFailed)return PipeStatus::unavailable;
    if(!n.connect.active){if(impl->pause)return PipeStatus::pending;if(!startConnect(n))return PipeStatus::unavailable;}
    DWORD size=0,error=0;if(!observed(n,n.connect,size,error))return PipeStatus::pending;n.connect.active=false;
    auto reject=[&](PipeStatus status){
        n.peer={};if(!DisconnectNamedPipe(n.pipe)){n.connectFailed=true;return PipeStatus::unavailable;}
        if(!startConnect(n))return PipeStatus::unavailable;return status;
    };
    if(error!=ERROR_SUCCESS)return reject(PipeStatus::unavailable);
    const auto status=recover([&]{
        PipePeer peer;auto auth=identity(n.pipe,true,impl->expected.isEmpty()?userSid(GetCurrentProcess()):impl->expected,peer);
        if(auth!=PipeStatus::ready)return reject(auth);
        if(!out.impl)out.impl=std::make_unique<PipeConnection::Impl>();
        PipeStatus replacementStatus;auto replacement=serverNative(impl->name,false,false,replacementStatus,impl->domain);
        if(!replacement)return reject(PipeStatus::unavailable);
        // Replenish before handoff; transfer the admitted Native allocation.
        // A new destination Native would transiently violate the 34 owner cap.
        n.peer=peer;out.impl->native=std::move(impl->native);impl->native=std::move(replacement);return PipeStatus::complete;
    });
    if(status==PipeStatus::unavailable && impl->native.get()==&n && !n.connect.active && !n.connectFailed)return reject(status);
    return status;
}
void* PipeListener::pendingEvent() const noexcept
{return impl && impl->native && impl->native->connect.active?impl->native->connect.event:nullptr;}
void PipeListener::close() noexcept {if(impl)retire(impl->native);}
void detail::PipeTestAccess::expectPeerSid(PipeConnection& owner,const juce::String& sid)
{if(!owner.connected()){if(!owner.impl)owner.impl=std::make_unique<PipeConnection::Impl>();owner.impl->expected=sid;}}
void detail::PipeTestAccess::expectPeerSid(PipeListener& owner,const juce::String& sid)
{if(!owner.impl)owner.impl=std::make_unique<PipeListener::Impl>();if(!owner.impl->native)owner.impl->expected=sid;}
void detail::PipeTestAccess::pauseConnect(PipeListener& owner,bool pause)
{if(!owner.impl)owner.impl=std::make_unique<PipeListener::Impl>();owner.impl->pause=pause;}
void detail::PipeTestAccess::withholdCompletion(PipeConnection& owner,bool value)
{if(owner.impl && owner.impl->native)owner.impl->native->withhold=value;}
void detail::PipeTestAccess::withholdCompletion(PipeListener& owner,bool value)
{if(owner.impl && owner.impl->native)owner.impl->native->withhold=value;}
void detail::PipeTestAccess::releaseWithheldCompletions()
{auto& state=owners();std::lock_guard lock(state.mutex);for(auto* n:state.retained)if(n!=nullptr)n->withhold=false;}
void detail::PipeTestAccess::reapRetired()
{
    std::array<Native*,maximumPipeOwners> released{};std::size_t count=0;auto& state=owners();
    {std::lock_guard lock(state.mutex);for(auto& n:state.retained)if(n!=nullptr && completed(*n)){released[count++]=n;n=nullptr;}}
    for(std::size_t i=0;i<count;++i)delete released[i];
}
std::size_t detail::PipeTestAccess::retainedOwners() noexcept
{auto& state=owners();std::lock_guard lock(state.mutex);return static_cast<std::size_t>(std::count_if(state.retained.begin(),state.retained.end(),[](auto* n){return n!=nullptr;}));}
bool detail::PipeTestAccess::admissionAvailable() noexcept
{auto& state=owners();std::lock_guard lock(state.mutex);return !state.poisoned && state.count<maximumPipeOwners;}
}

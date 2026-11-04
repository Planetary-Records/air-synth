#include "CompanionLaunch.h"
#include "GenerationRequest.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <array>
#include <barrier>
#include <charconv>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>

using namespace airack;
using namespace airack::companion;
namespace
{
int checks=0,failures=0,groups=0,groupChecks=0,groupFailures=0;
const char* group="";
void check(bool pass,const char* expression,int line)
{++checks;if(!pass){++failures;std::cerr<<group<<':'<<line<<' '<<expression<<std::endl;}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void finishGroup(){if(groups)std::cout<<group<<" checks="<<checks-groupChecks<<" failures="<<failures-groupFailures<<std::endl;}
void begin(const char* name){finishGroup();group=name;++groups;groupChecks=checks;groupFailures=failures;std::cout<<name<<std::endl;}
template<class F> bool observe(F f,std::uint64_t ms)
{const auto end=GetTickCount64()+ms;do{if(f())return true;Sleep(1);}while(GetTickCount64()<end);return false;}
WireBytes bytes(const juce::MemoryBlock& b){return {static_cast<const std::byte*>(b.getData()),b.getSize()};}
juce::String suffix(){return juce::Uuid().toString().removeCharacters("-");}
bool same(PipePeer a,PipePeer b){return a.processId==b.processId && a.creationTime==b.creationTime;}
std::uint64_t incarnation(HANDLE process)
{FILETIME a{},b{},c{},d{};return GetProcessTimes(process,&a,&b,&c,&d)?(std::uint64_t(a.dwHighDateTime)<<32)|a.dwLowDateTime:0;}

struct Temp
{
    const juce::File parent=juce::File(RACKSYNTH_WORKSPACE).getChildFile("build/host-fixtures");
    const juce::File root=parent.getChildFile("helper "+juce::Uuid().toString()+juce::String::fromUTF8(" 音 ü"));
    Temp(){if(!juce::File::isAbsolutePath(parent.getFullPathName()) || !root.isAChildOf(parent) || root.createDirectory().failed())throw std::runtime_error("unique owned fixture root");}
    ~Temp()
    {
        // The root is resolved and checked before the only recursive deletion.
        const bool owned=juce::File::isAbsolutePath(root.getFullPathName()) && root.isAChildOf(parent) && root.getParentDirectory()==parent;
        CHECK(owned);if(owned)CHECK(root.deleteRecursively());
    }
    juce::File child(const char* name) const{return root.getChildFile(name);}
};

// Native handles originate only from this test's CreateProcessW or DuplicateHandle.
struct Child
{
    HANDLE process=nullptr;
    Child()=default;
    Child(const Child&)=delete;Child& operator=(const Child&)=delete;
    ~Child(){if(process){retire();CloseHandle(process);}}
    PipePeer peer() const{return process?PipePeer{GetProcessId(process),incarnation(process)}:PipePeer{};}
    bool alive() const{return process && WaitForSingleObject(process,0)==WAIT_TIMEOUT;}
    bool duplicate(const CompanionLaunch& launcher)
    {
        auto source=static_cast<HANDLE>(airack::companion::detail::CompanionLaunchTestAccess::borrowProcessHandle(launcher));
        if(!source)return false;
        if(DuplicateHandle(GetCurrentProcess(),source,GetCurrentProcess(),&process,0,FALSE,DUPLICATE_SAME_ACCESS))return true;
        // Failed setup still retires the known test-created child before releasing its owner.
        CHECK(TerminateProcess(source,97)!=FALSE);CHECK(WaitForSingleObject(source,5000)==WAIT_OBJECT_0);return false;
    }
    bool ended(DWORD ms=5000) const{return process && WaitForSingleObject(process,ms)==WAIT_OBJECT_0;}
    DWORD exitCode() const{DWORD code=STILL_ACTIVE;if(process)GetExitCodeProcess(process,&code);return code;}
    void retire()
    {
        if(alive())CHECK(TerminateProcess(process,97)!=FALSE);
        const bool stopped=ended();CHECK(stopped);
        std::cout<<"owned child pid="<<GetProcessId(process)<<" terminationObserved="<<stopped<<" exit="<<exitCode()<<std::endl;
    }
};
// Independent fixture quoting supports empty text, embedded quotes and trailing slashes.
std::wstring quote(const std::wstring& input)
{
    std::wstring out=L"\"";std::size_t slashes=0;
    for(const auto c:input){if(c==L'\\'){++slashes;continue;}out.append(slashes*(c==L'"'?2:1),L'\\');slashes=0;if(c==L'"')out.push_back(L'\\');out.push_back(c);}
    out.append(slashes*2,L'\\');out.push_back(L'"');return out;
}
bool rawStart(Child& child,const juce::File& helper,const std::vector<std::wstring>& arguments)
{
    const std::wstring executable=helper.getFullPathName().toWideCharPointer();std::wstring command=quote(executable);
    for(const auto& a:arguments){command.push_back(L' ');command+=quote(a);}
    std::vector<wchar_t> buffer(command.begin(),command.end());buffer.push_back(0);
    STARTUPINFOW start{};start.cb=sizeof(start);start.dwFlags=STARTF_USESHOWWINDOW;start.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
    if(!CreateProcessW(executable.c_str(),buffer.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&start,&info))return false;
    CloseHandle(info.hThread);child.process=info.hProcess;return true;
}

struct Observation { PipePeer peer;bool console=false;std::int64_t transport=0,browser=0; };
bool observation(const juce::File& root,Observation& result)
{
    const auto file=root.getChildFile("launch-observation.json");juce::FileInputStream stream(file);
    if(!stream.openedOk())return false;const auto size=stream.getTotalLength();if(size<1 || size>4096)return false;
    juce::MemoryBlock data(static_cast<std::size_t>(size));if(stream.read(data.getData(),static_cast<int>(size))!=size)return false;
    const auto value=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(data.getData()),static_cast<int>(size)));
    const auto* o=value.getDynamicObject();if(!o || o->getProperties().size()!=6)return false;
    for(const auto* key:{"version","processId","creationTime","consoleWindow","transportCalls","browserCalls"})if(!value.hasProperty(key))return false;
    const auto integer=[](const juce::var& v){return v.isInt() || v.isInt64();};
    if(!integer(value["version"]) || static_cast<juce::int64>(value["version"])!=1
        || !integer(value["processId"]) || static_cast<juce::int64>(value["processId"])<1 || static_cast<juce::int64>(value["processId"])>MAXDWORD
        || !value["creationTime"].isString() || !value["consoleWindow"].isBool()
        || !integer(value["transportCalls"]) || !integer(value["browserCalls"]))return false;
    const auto creation=value["creationTime"].toString().toStdString();std::uint64_t parsed=0;
    if(creation.empty() || creation.size()>20 || !std::all_of(creation.begin(),creation.end(),[](char c){return c>='0' && c<='9';}))return false;
    const auto [end,error]=std::from_chars(creation.data(),creation.data()+creation.size(),parsed);
    if(error!=std::errc{} || end!=creation.data()+creation.size() || !parsed)return false;
    result={{static_cast<std::uint32_t>(static_cast<juce::int64>(value["processId"])),parsed},static_cast<bool>(value["consoleWindow"]),static_cast<juce::int64>(value["transportCalls"]),static_cast<juce::int64>(value["browserCalls"])};
    return result.transport>=0 && result.browser>=0;
}
void checkObservation(const juce::File& root,const Child& child)
{
    Observation record;const bool valid=observation(root,record);CHECK(valid);
    if(valid){CHECK(same(record.peer,child.peer()));CHECK(!record.console);CHECK(record.transport==0 && record.browser==0);}
}
Frame control(MessageKind kind,juce::Uuid session,ai::RequestTag tag)
{Frame f{kind,session,{}};if(!encodeControl({tag,RuntimeStatus::ready},f.payload))throw std::runtime_error("control fixture");return f;}
struct Client
{
    PipeConnection pipe;juce::Uuid runtime=juce::Uuid(),session=juce::Uuid::null();bool reading=false;
    bool send(const Frame& f,std::uint64_t ms=1500)
    {if(pipe.beginWrite(f)!=PipeStatus::ready)return false;PipeStatus s=PipeStatus::pending;observe([&]{s=pipe.pollWrite();return s!=PipeStatus::pending;},ms);return s==PipeStatus::complete;}
    std::optional<Frame> receive(std::uint64_t ms=1500)
    {
        if(!reading){if(pipe.beginRead()!=PipeStatus::ready)return {};reading=true;}
        Frame f;PipeStatus s=PipeStatus::pending;observe([&]{s=pipe.pollRead(f);return s!=PipeStatus::pending;},ms);
        if(s!=PipeStatus::pending)reading=false;if(s==PipeStatus::complete)return f;return {};
    }
    bool open(const juce::String& suffix)
    {
        const auto began=GetTickCount64();if(!observe([&]{return pipe.connect(suffix)==PipeStatus::ready;},5000))return false;
        const auto elapsed=GetTickCount64()-began;if(elapsed>=5000)return false;const auto left=5000-elapsed;
        if(!send(control(MessageKind::hello,juce::Uuid::null(),{runtime,0,0,0}),left))return false;
        const auto elapsedAfter=GetTickCount64()-began;if(elapsedAfter>=5000)return false;
        const auto frame=receive(5000-elapsedAfter);ControlMessage m;
        if(!frame || frame->kind!=MessageKind::welcome || frame->session.isNull() || !decodeControl(bytes(frame->payload),m)
            || m.tag!=ai::RequestTag{runtime,0,0,0} || m.status!=RuntimeStatus::ready)return false;
        session=frame->session;return true;
    }
    bool ping()
    {
        const auto began=GetTickCount64();if(!send(control(MessageKind::ping,session,{runtime,0,0,0})))return false;
        const auto elapsed=GetTickCount64()-began;if(elapsed>=1500)return false;
        const auto f=receive(1500-elapsed);ControlMessage m;
        return f && f->kind==MessageKind::pong && f->session==session && decodeControl(bytes(f->payload),m)
            && m.tag==ai::RequestTag{runtime,0,0,0} && m.status==RuntimeStatus::ready && GetTickCount64()-began<1500;
    }
    bool status()
    {
        Frame f{MessageKind::account,session,{}};if(!encodeAccountCommand({AccountAction::status,{}},f.payload) || !send(f))return false;
        const auto reply=receive(5000);AccountReply value;
        return reply && reply->kind==MessageKind::accountResult && reply->session==session && decodeAccountReply(bytes(reply->payload),value)
            && value.status==RuntimeStatus::ready && value.providerStatus==rack::oauth::Status::signedOut
            && value.selectedModel.isEmpty() && value.models.empty() && !value.modelVerified;
    }
    bool closed()
    {
        return observe([&]{if(!pipe.connected())return true;if(!reading){if(pipe.beginRead()!=PipeStatus::ready)return !pipe.connected();reading=true;}
            Frame f;const auto s=pipe.pollRead(f);if(s!=PipeStatus::pending)reading=false;return !pipe.connected();},1500);
    }
};

void native_placement_and_arguments(const juce::File& helper,const juce::File& dll)
{
    begin(__func__);PWSTR local=nullptr;const bool folderOkay=SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&local)) && local;
    CHECK(folderOkay);const auto directories=companionDirectories();
    if(folderOkay){const auto parent=juce::File(juce::String(local)).getChildFile("AI Rack Synth");CHECK(directories.credentials==parent.getChildFile("OAuthProbe"));CHECK(directories.preferences==parent.getChildFile("CompanionPreferences"));}
    CoTaskMemFree(local);CHECK(juce::File::isAbsolutePath(directories.credentials.getFullPathName()));CHECK(directories.credentials!=directories.preferences);
    const auto host=juce::File::getSpecialLocation(juce::File::currentExecutableFile);const auto cwd=juce::File::getCurrentWorkingDirectory();
    CHECK(companionExecutable()==host.getSiblingFile("AI Rack Synth Companion.exe"));
    const HMODULE module=LoadLibraryExW(dll.getFullPathName().toWideCharPointer(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);CHECK(module!=nullptr);
    if(module){
        using Path=std::uint32_t(__cdecl*)(wchar_t*,std::uint32_t);const auto path=reinterpret_cast<Path>(GetProcAddress(module,"rackSynthFixtureCompanionPath"));CHECK(path!=nullptr);
        if(path){
            const auto expected=dll.getSiblingFile("AI Rack Synth Companion.exe");const auto wide=std::wstring(expected.getFullPathName().toWideCharPointer());const auto want=static_cast<std::uint32_t>(wide.size()+1);
            CHECK(expected!=host.getSiblingFile("AI Rack Synth Companion.exe") && expected!=cwd.getChildFile("AI Rack Synth Companion.exe"));
            CHECK(path(nullptr,0)==want);CHECK(path(nullptr,want)==want);
            std::vector<wchar_t> buffer(want+4,L'!');CHECK(path(buffer.data(),want-1)==want);CHECK(std::all_of(buffer.begin(),buffer.end(),[](wchar_t c){return c==L'!';}));
            CHECK(path(buffer.data(),want)==want);CHECK(std::equal(wide.begin(),wide.end(),buffer.begin()));CHECK(buffer[want-1]==L'\0');CHECK(buffer[want]==L'!');
            std::fill(buffer.begin(),buffer.end(),L'!');CHECK(path(buffer.data(),static_cast<std::uint32_t>(buffer.size()))==want);CHECK(std::equal(wide.begin(),wide.end(),buffer.begin()));CHECK(buffer[want-1]==L'\0');CHECK(buffer[want]==L'!');
        }
        CHECK(FreeLibrary(module)!=FALSE);
    }
    Temp t;const auto name=suffix();CompanionLaunch owner;
    CHECK(owner.start({{},name,t.root}).status==LaunchStatus::unavailable);
    CHECK(owner.start({t.child("AI Rack Synth Companion.exe"),name,t.root}).status==LaunchStatus::unavailable);
    const auto wrong=t.child("wrong.exe");CHECK(helper.copyFileTo(wrong));CHECK(owner.start({wrong,name,t.root}).status==LaunchStatus::unavailable);
    for(const auto& bad:juce::StringArray{"bad space","bad!",juce::String::repeatedString("x",33),juce::String::fromUTF8("音")})CHECK(owner.start({helper,bad,t.root}).status==LaunchStatus::unavailable);
    CHECK(owner.start({helper,name,{}}).status==LaunchStatus::unavailable);CHECK(owner.start({helper,{},t.root}).status==LaunchStatus::unavailable);
    CHECK(!owner.running());CHECK(owner.peer().processId==0 && owner.peer().creationTime==0);
    const std::wstring root=t.root.getFullPathName().toWideCharPointer(),s=name.toWideCharPointer();
    const std::vector<std::vector<std::wstring>> rejected{
        {L"--fixture-root",root},{L"--pipe-suffix",s},{L"--unknown",L"x",L"--fixture-root",root,L"--pipe-suffix",s},
        {L"--fixture-root",root,L"--pipe-suffix",s,L"--pipe-suffix",s},
        {L"--fixture-root",root,L"--fixture-root",root,L"--pipe-suffix",s},
        {L"--fixture-root",L"",L"--pipe-suffix",s},{L"--fixture-root",L"relative",L"--pipe-suffix",s},
        {L"--fixture-root",root,L"--pipe-suffix",L""},{L"--fixture-root",root,L"--pipe-suffix",L"bad space"},
        {L"--fixture-root",root,L"--pipe-suffix",std::wstring(33,L'x')},{L"--fixture-root",root,L"--pipe-suffix",L"音"},
        {L"--fixture-root",L"C:relative",L"--pipe-suffix",s},{L"--fixture-root",L"\\rooted",L"--pipe-suffix",s}
    };
    for(const auto& args:rejected){Child child;const bool created=rawStart(child,helper,args);CHECK(created);if(created){CHECK(child.ended());CHECK(child.exitCode()==2);}
        CHECK(!t.child("launch-observation.json").exists());CHECK(!t.child("OAuthProbe").exists());CHECK(!t.child("CompanionPreferences").exists());}
    // Complete raw argv in the other accepted order includes a trailing separator.
    Child ordered;const bool created=rawStart(ordered,helper,{L"--pipe-suffix",s,L"--fixture-root",root+L"\\"});CHECK(created);
    if(created){Client c;const bool opened=c.open(name);CHECK(opened);if(opened){CHECK(c.ping());checkObservation(t.root,ordered);}}
}

void hidden_launch_and_ownership(const juce::File& helper)
{
    begin(__func__);Temp t;const auto bin=t.child("binaries spaced ü");CHECK(bin.createDirectory().wasOk());const auto copied=bin.getChildFile("AI Rack Synth Companion.exe");CHECK(helper.copyFileTo(copied));
    const auto name=suffix();const LaunchOptions options{copied,name,t.root};std::array<CompanionLaunch,8> owners;std::array<LaunchResult,8> results;
    std::barrier gate(8);std::array<std::thread,8> threads;
    for(std::size_t i=0;i<threads.size();++i)threads[i]=std::thread([&,i]{gate.arrive_and_wait();results[i]=owners[i].start(options);});
    for(auto& thread:threads)thread.join();int started=0;for(const auto& result:results){if(result.status==LaunchStatus::started)++started;CHECK(result.status==LaunchStatus::started || result.status==LaunchStatus::reused);}
    CHECK(started==1);Child child;const bool duplicated=child.duplicate(owners[0]);CHECK(duplicated);if(!duplicated)return;
    const auto peer=child.peer();CHECK(peer.processId>0 && peer.creationTime>0 && peer.processId!=GetCurrentProcessId());
    for(std::size_t i=0;i<owners.size();++i){CHECK(owners[i].running());CHECK(same(owners[i].peer(),peer));CHECK(same(results[i].peer,peer));}
    Client client;const bool opened=client.open(name);CHECK(opened);if(!opened)return;CHECK(same(client.pipe.peer(),peer));CHECK(client.ping());checkObservation(t.root,child);
    CHECK(owners[0].start({t.child("AI Rack Synth Companion.exe"),name,t.root}).status==LaunchStatus::unavailable);CHECK(owners[0].running());CHECK(same(owners[0].peer(),peer));CHECK(client.ping());
    // CreateProcess failure with a valid basename also preserves the incumbent.
    const auto broken=t.child("AI Rack Synth Companion.exe");CHECK(broken.replaceWithText("not an executable"));CHECK(owners[0].start({broken,name,t.root}).status==LaunchStatus::unavailable);CHECK(same(owners[0].peer(),peer));
    CompanionLaunch moved=std::move(owners[0]);CHECK(!owners[0].running());CHECK(moved.running());owners[0]=std::move(moved);CHECK(!moved.running());CHECK(owners[0].running());
    for(auto& owner:owners){owner.release();CHECK(!owner.running());CHECK(owner.peer().processId==0 && owner.peer().creationTime==0);}
    CHECK(child.alive());CHECK(client.ping());checkObservation(t.root,child);
    client.pipe.close();CHECK(child.alive());
}

void composed_service_and_restart(const juce::File& helper)
{
    begin(__func__);Temp t;const auto name=suffix();const LaunchOptions options{helper,name,t.root};CompanionLaunch owner;
    const auto launch=owner.start(options);CHECK(launch.status==LaunchStatus::started);Child first;const bool duplicated=first.duplicate(owner);CHECK(duplicated);if(!duplicated)return;
    CHECK(same(launch.peer,first.peer()));Client a,b;const bool opened=a.open(name) && b.open(name);CHECK(opened);if(!opened)return;
    CHECK(a.session!=b.session);CHECK(same(a.pipe.peer(),first.peer()) && same(b.pipe.peer(),first.peer()));CHECK(a.ping());CHECK(b.ping());CHECK(a.status());checkObservation(t.root,first);
    ai::GenerationRequest request;request.tag={a.runtime,1,7,11};request.model="captured-model";request.instruction="synthetic helper check";
    request.capturedPhrase=PatternDocument{};request.capturedPhrase->noteCount=1;request.capturedPhrase->notes[0]={.25,.125,65,87,2};request.capturedBpm=143;
    Frame frame{MessageKind::generate,a.session,{}};CHECK(encodeRequest(request,frame.payload));CHECK(a.send(frame));const auto reply=a.receive(5000);ControlMessage failure;
    CHECK(reply && reply->kind==MessageKind::failure && reply->session==a.session && decodeControl(bytes(reply->payload),failure));CHECK(failure.tag==request.tag && failure.status==RuntimeStatus::modelNotConfigured);
    CHECK(b.ping());checkObservation(t.root,first);const auto oldSession=a.session;const auto oldPeer=first.peer();a.pipe.close();CHECK(b.ping());
    // Separate OS process, same suffix: first native pipe owner remains authoritative.
    Temp race;Child loser;const bool second=rawStart(loser,helper,{L"--fixture-root",race.root.getFullPathName().toWideCharPointer(),L"--pipe-suffix",name.toWideCharPointer()});CHECK(second);
    if(second){CHECK(loser.ended());CHECK(loser.exitCode()==1);checkObservation(race.root,loser);CHECK(b.ping());CHECK(first.alive());}
    first.retire();CHECK(!owner.running());CHECK(owner.peer().processId==0 && owner.peer().creationTime==0);CHECK(b.closed());
    const auto restarted=owner.start(options);CHECK(restarted.status==LaunchStatus::started);Child current;const bool again=current.duplicate(owner);CHECK(again);if(!again)return;
    CHECK(!same(current.peer(),oldPeer));CHECK(same(restarted.peer,current.peer()));Client fresh;fresh.runtime=a.runtime;const bool reconnected=fresh.open(name);CHECK(reconnected);if(!reconnected)return;
    CHECK(fresh.session!=oldSession);CHECK(same(fresh.pipe.peer(),current.peer()));CHECK(fresh.ping());CHECK(fresh.send(control(MessageKind::ping,oldSession,{fresh.runtime,0,0,0})));CHECK(fresh.closed());
    Client after;CHECK(after.open(name));CHECK(after.status());CHECK(after.ping());checkObservation(t.root,current);
    owner.release();CHECK(current.alive());CHECK(after.ping());
}
}
int main(int argc,char** argv)
{
    try{
        CHECK(argc==3);if(argc!=3)return 1;const juce::File helper(juce::String::fromUTF8(argv[1])),dll(juce::String::fromUTF8(argv[2]));
        CHECK(helper.existsAsFile() && helper.getFileName()=="AI Rack Synth Companion.exe");CHECK(dll.existsAsFile());
        native_placement_and_arguments(helper,dll);hidden_launch_and_ownership(helper);composed_service_and_restart(helper);
    }catch(const std::exception& e){CHECK(false);std::cerr<<"fixture exception: "<<e.what()<<std::endl;}
    finishGroup();std::cout<<"CompanionLaunchTests checks="<<checks<<" groups="<<groups<<" failures="<<failures<<std::endl;return failures?1:0;
}

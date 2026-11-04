#include "CompanionLaunch.h"
#include "GenerationLog.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShlObj.h>
#include <mutex>
#include <string>
#include <vector>

namespace airack::companion
{
namespace
{
struct Handle
{
    HANDLE value=nullptr;
    explicit Handle(HANDLE h=nullptr):value(h){}
    ~Handle(){if(value)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
PipePeer processIdentity(HANDLE process) noexcept
{
    FILETIME created{},exited{},kernel{},user{};const auto pid=GetProcessId(process);
    if(!pid || !GetProcessTimes(process,&created,&exited,&kernel,&user))return {};
    return {pid,(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime};
}
struct ProcessOwner
{
    Handle process;PipePeer identity;std::wstring executable,command;
    bool alive() const noexcept
    {
        if(!process.value || WaitForSingleObject(process.value,0)!=WAIT_TIMEOUT)return false;
        const auto observed=processIdentity(process.value);
        return identity.processId!=0 && identity.creationTime!=0
            && observed.processId==identity.processId && observed.creationTime==identity.creationTime;
    }
};
struct Cache { std::mutex mutex;std::array<std::weak_ptr<ProcessOwner>,2> owners; };
Cache& cache(){static Cache value;return value;}
bool validSuffix(const juce::String& value)
{return value.isNotEmpty() && value.length()<=32 && value.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-");}
bool absolute(const juce::File& file)
{return file.getFullPathName().isNotEmpty() && juce::File::isAbsolutePath(file.getFullPathName());}
std::wstring quote(const std::wstring& text)
{
    std::wstring output=L"\"";std::size_t backslashes=0;
    for(const auto c:text){
        if(c==L'\\'){++backslashes;continue;}
        output.append(backslashes*(c==L'"'?2:1),L'\\');backslashes=0;
        if(c==L'"')output.push_back(L'\\');output.push_back(c);
    }
    output.append(backslashes*2,L'\\');output.push_back(L'"');return output;
}
}
struct CompanionLaunch::Impl { std::shared_ptr<ProcessOwner> owner; };
CompanionDirectories companionDirectories()
{
    PWSTR path=nullptr;
    const auto result=SHGetKnownFolderPath(FOLDERID_LocalAppData,KF_FLAG_DEFAULT,nullptr,&path);
    struct Allocation { PWSTR value;~Allocation(){CoTaskMemFree(value);} } allocation{path};
    if(FAILED(result) || !path || !*path)return {};
    const juce::File local{juce::String(path)};if(!absolute(local))return {};
    const auto parent=local.getChildFile("AI Rack Synth");
    return {parent.getChildFile("OAuthProbe"),parent.getChildFile("CompanionPreferences")};
}
juce::File companionExecutable()
{
    HMODULE module=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&companionExecutable),&module))return {};
    // The caller guarantees this borrowed module's lifetime; never FreeLibrary it.
    for(DWORD capacity=256;;capacity=(std::min)(capacity*2,DWORD{32768})){
        std::vector<wchar_t> path(capacity,0);const auto count=GetModuleFileNameW(module,path.data(),capacity);
        if(!count)return {};
        if(count<capacity){const juce::File file{juce::String(path.data())};return absolute(file)?file.getSiblingFile(companionExecutableName):juce::File{};}
        if(capacity==32768)return {};
    }
}
CompanionLaunch::CompanionLaunch():impl(std::make_unique<Impl>()){}
CompanionLaunch::~CompanionLaunch()=default;
CompanionLaunch::CompanionLaunch(CompanionLaunch&&) noexcept=default;
CompanionLaunch& CompanionLaunch::operator=(CompanionLaunch&&) noexcept=default;
LaunchResult CompanionLaunch::start(const LaunchOptions& options)
{
    try{
        const auto unavailable=[](const char* code,DWORD nativeError=0){try{diagnostics::Event event;event.stage=diagnostics::Stage::launch;
            event.errorCode=code;event.nativeError=nativeError;diagnostics::write(event);}catch(...){}return LaunchResult{};};
        // JUCE native string conversions may preallocate even through const File
        // references. Serialize these too when callers share LaunchOptions.
        auto& shared=cache();std::lock_guard lock(shared.mutex);
        auto& cached=shared.owners[options.role==LaunchRole::mcpService?1:0];
        if(!absolute(options.executable) || !options.executable.existsAsFile() || options.executable.getFileName()!=companionExecutableName)return unavailable("launch_executable_missing");
        const bool fixture=options.fixtureRoot.getFullPathName().isNotEmpty();
        if(fixture!=options.pipeSuffix.isNotEmpty() || (fixture && (!absolute(options.fixtureRoot) || !validSuffix(options.pipeSuffix))))return unavailable("launch_options_invalid");
        // Allocate the complete key, command and owning control storage before creation.
        auto candidate=std::make_shared<ProcessOwner>();candidate->executable=options.executable.getFullPathName().toWideCharPointer();
        candidate->command=quote(candidate->executable);
        if(options.role==LaunchRole::mcpService)candidate->command+=L" --mcp-service";
        if(fixture){candidate->command+=L" --fixture-root "+quote(options.fixtureRoot.getFullPathName().toWideCharPointer());candidate->command+=L" --pipe-suffix "+quote(options.pipeSuffix.toWideCharPointer());}
        if(candidate->command.size()+1>32767)return {};
        std::vector<wchar_t> buffer(candidate->command.begin(),candidate->command.end());buffer.push_back(0);
        if(!impl)impl=std::make_unique<Impl>();
        // One weak key per role. Different fixture keys
        // may launch independently; the accepted first pipe instance is cross-process authority.
        if(auto previous=cached.lock();previous && previous->executable==candidate->executable && previous->command==candidate->command && previous->alive()){
            impl->owner=std::move(previous);return {LaunchStatus::reused,impl->owner->identity};
        }
        STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        if(!CreateProcessW(candidate->executable.c_str(),buffer.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info))return unavailable("launch_failed",GetLastError());
        {Handle thread{info.hThread};} // No thread handle escapes process creation.
        candidate->process.value=info.hProcess;candidate->identity=processIdentity(info.hProcess);
        if(!candidate->identity.processId || !candidate->identity.creationTime || candidate->identity.processId!=info.dwProcessId)return unavailable("launch_identity_invalid");
        // Closing a failed observation or replaced owner never terminates the helper.
        impl->owner=candidate;cached=candidate;try{diagnostics::Event event;event.stage=diagnostics::Stage::launch;diagnostics::write(event);}catch(...){}return {LaunchStatus::started,candidate->identity};
    }catch(...){return {};}
}
bool CompanionLaunch::running() const noexcept{return impl && impl->owner && impl->owner->alive();}
PipePeer CompanionLaunch::peer() const noexcept{return running()?impl->owner->identity:PipePeer{};}
void CompanionLaunch::release() noexcept{if(impl)impl->owner.reset();}
void* detail::CompanionLaunchTestAccess::borrowProcessHandle(const CompanionLaunch& owner) noexcept
{return owner.impl && owner.impl->owner?owner.impl->owner->process.value:nullptr;}
}

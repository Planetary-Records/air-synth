#include "CompanionLaunch.h"
#include "CompanionBackend.h"
#include "CompanionRuntimeServer.h"
#include "GenerationLog.h"
#include "McpBridge.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <ShellApi.h>
#include <mutex>
#include <string>
#include <vector>

using namespace airack::companion;
namespace
{
std::atomic_bool stop{false};
BOOL WINAPI consoleControl(DWORD event)
{
    switch(event){
        case CTRL_C_EVENT:case CTRL_BREAK_EVENT:case CTRL_CLOSE_EVENT:case CTRL_LOGOFF_EVENT:case CTRL_SHUTDOWN_EVENT:
            stop.store(true,std::memory_order_release);return TRUE;
        default:return FALSE;
    }
}
bool rawAbsolute(const std::wstring& path)
{
    // GetFullPathName accepts drive-relative paths, so reject those on the raw
    // text before the native resolution, and before JUCE File normalization.
    if(path.empty() || path.size()>=32767)return false;
    const bool drive=path.size()>=3 && ((path[0]>=L'A' && path[0]<=L'Z') || (path[0]>=L'a' && path[0]<=L'z'))
        && path[1]==L':' && (path[2]==L'\\' || path[2]==L'/');
    const auto serverEnd=path.find(L'\\',2);
    const bool unc=path.size()>2 && path[0]==L'\\' && path[1]==L'\\' && serverEnd!=std::wstring::npos && serverEnd>2 && serverEnd+1<path.size();
    if(!drive && !unc)return false;
    std::vector<wchar_t> complete(32768);const auto count=GetFullPathNameW(path.c_str(),static_cast<DWORD>(complete.size()),complete.data(),nullptr);
    return count>0 && count<complete.size();
}
bool validSuffix(const std::wstring& suffix)
{
    return !suffix.empty() && suffix.size()<=32 && std::all_of(suffix.begin(),suffix.end(),[](wchar_t c){
        return (c>=L'A' && c<=L'Z') || (c>=L'a' && c<=L'z') || (c>=L'0' && c<=L'9') || c==L'_' || c==L'-';});
}
struct Arguments { bool fixture=false,mcp=false,service=false;juce::File root;juce::String suffix;std::optional<juce::Uuid> instance; };
bool parse(Arguments& parsed)
{
    int count=0;auto** argv=CommandLineToArgvW(GetCommandLineW(),&count);
    struct Allocation { LPWSTR* value;~Allocation(){LocalFree(value);} } allocation{argv};
    if(!argv || count<1)return false;
    bool gotRoot=false,gotSuffix=false,gotInstance=false;std::wstring root,suffix;
    for(int i=1;i<count;++i){
        const std::wstring key=argv[i];
        if(key==L"--mcp-service" && !parsed.mcp && !parsed.service){parsed.service=true;continue;}
        if(key==L"--mcp-stdio" && !parsed.mcp && !parsed.service){parsed.mcp=true;continue;}
        if(i+1>=count)return false;
        if(key==L"--fixture-root" && !gotRoot){root=argv[++i];gotRoot=true;}
        else if(key==L"--pipe-suffix" && !gotSuffix){suffix=argv[++i];gotSuffix=true;}
        else if(key==L"--instance" && !gotInstance){
            gotInstance=true;const juce::String id(argv[++i]);const auto raw=id.removeCharacters("-");
            if(id.length()!=36 || id[8]!='-' || id[13]!='-' || id[18]!='-' || id[23]!='-' || raw.length()!=32 || !raw.containsOnly("0123456789abcdefABCDEF"))return false;
            parsed.instance=juce::Uuid(id);if(parsed.instance->isNull())return false;
        }
        else return false;
    }
    if(gotInstance && !parsed.mcp)return false;
    if(!gotRoot && !gotSuffix)return true;
    if(!gotRoot || !gotSuffix || !rawAbsolute(root) || !validSuffix(suffix))return false;
    parsed.fixture=true;parsed.root=juce::File(juce::String(root.c_str()));parsed.suffix=juce::String(suffix.c_str());return true;
}
struct Observation
{
    juce::File root;PipePeer peer;bool console=false;
    std::mutex mutex;std::int64_t transportCalls=0,browserCalls=0;std::atomic_bool good{true};
    explicit Observation(juce::File path):root(std::move(path))
    {
        FILETIME created{},exited{},kernel{},user{};
        peer.processId=GetCurrentProcessId();
        if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))peer.creationTime=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;
        console=GetConsoleWindow()!=nullptr;
    }
    bool publish(bool transport=false,bool browser=false)
    {
        std::lock_guard lock(mutex);if(transport)++transportCalls;if(browser)++browserCalls;
        auto* object=new juce::DynamicObject;juce::var record(object);
        object->setProperty("version",1);object->setProperty("processId",static_cast<juce::int64>(peer.processId));
        object->setProperty("creationTime",juce::String(std::to_string(peer.creationTime)));
        object->setProperty("consoleWindow",console);object->setProperty("transportCalls",static_cast<juce::int64>(transportCalls));object->setProperty("browserCalls",static_cast<juce::int64>(browserCalls));
        const auto text=juce::JSON::toString(record,true);
        const bool written=peer.processId && peer.creationTime && text.getNumBytesAsUTF8()<=4096 && root.createDirectory().wasOk()
            && root.getChildFile("launch-observation.json").replaceWithText(text);
        if(!written){good.store(false,std::memory_order_release);stop.store(true,std::memory_order_release);}return written;
    }
};
}
int main()
{
    try{
        Arguments arguments;if(!parse(arguments))return 2;
        if(arguments.mcp||arguments.service){
            auto service=arguments.fixture?std::optional<airack::mcp::ServiceOptions>({arguments.suffix,arguments.root}):airack::mcp::serviceOptionsForProcess();
            if(!service)return 2;
            if(arguments.mcp)return airack::mcp::runStdio({*service,arguments.instance});
            if(!SetConsoleCtrlHandler(consoleControl,TRUE))return 1;
            return airack::mcp::runService(*service,stop);
        }
        try{airack::diagnostics::Event event;event.component=airack::diagnostics::Component::companion;event.stage=airack::diagnostics::Stage::startup;airack::diagnostics::write(event);}catch(...){}
        BackendOptions options;std::shared_ptr<Observation> observation;
        if(arguments.fixture){
            observation=std::make_shared<Observation>(arguments.root);if(!observation->publish())return 1;
            options.credentialDirectory=arguments.root.getChildFile("OAuthProbe");options.preferenceDirectory=arguments.root.getChildFile("CompanionPreferences");
            options.transport=[observation](const rack::oauth::HttpRequest&,const std::atomic_bool&)->rack::oauth::HttpResponse{
                observation->publish(true,false);return {0,{},{},true};
            };
            options.openBrowser=[observation](const juce::String&){observation->publish(false,true);return false;};
        }else{
            const auto directories=companionDirectories();if(directories.credentials.getFullPathName().isEmpty() || directories.preferences.getFullPathName().isEmpty())return 1;
            options.credentialDirectory=directories.credentials;options.preferenceDirectory=directories.preferences;
        }
        if(!SetConsoleCtrlHandler(consoleControl,TRUE))return 1;
        auto backend=std::make_shared<CompanionBackend>(std::move(options));
        // Both closures retain the concrete backend through accepted worker termination
        // or exceptional retention. The only cross-thread backend method is cancel.
        RuntimeServer server([backend](const RuntimeJob& job){return backend->run(job);},[backend](const RuntimeJob& job){backend->cancel(job);});
        const auto result=server.run(stop,arguments.suffix);
        try{airack::diagnostics::Event event;event.component=airack::diagnostics::Component::companion;event.stage=airack::diagnostics::Stage::terminal;event.runtimeStatus=int(result.status);airack::diagnostics::write(event);}catch(...){}
        return result.status==RuntimeStatus::ready && !result.workerRetained && (!observation || observation->good.load(std::memory_order_acquire))?0:1;
    }catch(...){return 1;}
}

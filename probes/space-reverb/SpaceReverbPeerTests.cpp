#include "ProbePeerGuard.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
using namespace airack::space::probe;
#if defined(_WIN32)
struct Child {HANDLE event=nullptr;PROCESS_INFORMATION process{};};
Child launch(const std::filesystem::path& path)
{
    Child child;SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    child.event=CreateEventW(&security,TRUE,FALSE,nullptr);if(!child.event)return child;
    auto command=L"\""+std::filesystem::absolute(path).wstring()+L"\" --fixture-event "+std::to_wstring(reinterpret_cast<std::uintptr_t>(child.event));
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.process)){CloseHandle(child.event);child.event=nullptr;}
    return child;
}
std::pair<bool,DWORD> release(Child& child)
{
    SetEvent(child.event);const auto waited=WaitForSingleObject(child.process.hProcess,5000);
    if(waited!=WAIT_OBJECT_0){TerminateProcess(child.process.hProcess,2);WaitForSingleObject(child.process.hProcess,5000);}
    DWORD code=2;GetExitCodeProcess(child.process.hProcess,&code);
    CloseHandle(child.event);CloseHandle(child.process.hThread);CloseHandle(child.process.hProcess);return {waited==WAIT_OBJECT_0,code};
}
#endif
int main(int argc,char** argv)
{
#if defined(_WIN32)
    if(argc==3&&std::string_view(argv[1])=="--fixture-event"){
        const auto event=reinterpret_cast<HANDLE>(std::uintptr_t(std::stoull(argv[2])));
        const auto result=WaitForSingleObject(event,10000);CloseHandle(event);return result==WAIT_OBJECT_0?0:2;
    }
    if(argc==2&&std::string_view(argv[1])=="--observe-exited"){
        auto child=launch(argv[0]);if(!child.event)return 2;
        const auto held=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,child.process.dwProcessId);
        if(!held){release(child);return 2;}
        SetEvent(child.event);const auto joined=WaitForSingleObject(child.process.hProcess,5000);
        std::array<wchar_t,1024> path{};DWORD length=DWORD(path.size());
        const bool queried=QueryFullProcessImageNameW(held,0,path.data(),&length)!=FALSE;
        const auto imageError=queried?0:GetLastError();DWORD code=0;
        const bool known=GetExitCodeProcess(held,&code)!=FALSE;const auto exitError=known?0:GetLastError();
        CloseHandle(held);const auto [released,finalCode]=release(child);
        std::cout<<"EXITED_IMAGE_DIAGNOSTIC joined="<<(joined==WAIT_OBJECT_0)<<" query_success="<<queried<<" win32_error="<<imageError
                 <<" exit_code_known="<<known<<" exit_code="<<code<<" exit_query_error="<<exitError<<" held_access=query_limited same_handle=1 child_exit="<<finalCode<<'\n';
        return joined==WAIT_OBJECT_0&&known&&code==0&&released&&finalCode==0&&processTerminated(known,code)?0:1;
    }
#endif
    int checks=0,failures=0;
    const auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<name<<'\n';}};
    for(const auto path:{L"C:\\Users\\user\\ai-rack-synth\\build\\pipeline-probe.exe",L"c:/work/AI-RACK-SYNTH/build/private.exe",L"C:\\a\\ai-rack-synth/build/Private.EXE"})
        check(projectNativePath(path),"project native path admitted");
    for(const auto path:{L"C:/other/build/probe.exe",L"C:/ai-rack-synthesizer/build/probe.exe",L"C:/ai-rack-synth/build/probe.dll",L"C:/other/ai-rack-synth.exe"})
        check(!projectNativePath(path),"unrelated path excluded");
#if defined(_WIN32)
    if(argc!=2||!std::filesystem::exists(argv[1])){std::cerr<<"Fixture binary missing\n";return 2;}
    auto noise=launch(std::filesystem::path(argv[1]).parent_path()/"RackSynthForeignFixture.exe");
    check(noise.event!=nullptr,"foreign native fixture created");if(!noise.event)return 2;
    check(nativePeerStatus()==PeerStatus::present,"default inventory still detects foreign native work");
    check(nativePeerStatus(GetCurrentProcessId())==PeerStatus::idle,"scoped inventory excludes self despite foreign work");
    auto child=launch(argv[1]);check(child.event!=nullptr,"hidden private child created");
    if(child.event){
        const auto childPid=child.process.dwProcessId;
        PeerDetails observed;
        check(nativePeerStatus(childPid,false,&observed)==PeerStatus::present,"non-RackSynth child detected by its path");
        check(observed.parentPid==GetCurrentProcessId(),"interruption preserves the actual parent PID");
        check(_wcsicmp(observed.imagePath.data(),std::filesystem::path(argv[1]).make_preferred().native().c_str())==0,"interruption preserves the queried full path while the child is alive");
        check(observed.creationFileTime>0&&observed.observedFileTime>=observed.creationFileTime,"interruption timestamps bind the observed process generation");
        const auto [joined,code]=release(child);check(joined,"private child joined");check(code==0,"child released normally");
        check(nativePeerStatus(childPid)==PeerStatus::idle,"released child scope ignores other native work");
    }
    const auto [joined,code]=release(noise);check(joined,"foreign fixture joined");check(code==0,"foreign fixture released normally");
    std::cout<<"LEGACY_PEER checks="<<checks<<" failures="<<failures<<'\n';
    for(const auto path:{L"C:/air/p.exe",L"C:\\work\\AIR\\private.EXE",L"C:/actions-runner/air/_work/air/air/build/ci/private.exe",L"C:/ai-rack-synth/private.exe"})
        check(projectNativePath(path),"AIR and legacy descendants admitted");
    for(const auto path:{L"C:/chair/p.exe",L"C:/airship/p.exe",L"C:/not-air/p.exe",L"C:/air/p.dll",L"C:/other/air.exe",L"C:/air/p.exe.other"})
        check(!projectNativePath(path),"AIR path boundary excluded");
    const auto temp=std::filesystem::canonical(std::filesystem::temp_directory_path());
    bool outsideLegacy=true;for(const auto& part:temp)if(_wcsicmp(part.c_str(),L"ai-rack-synth")==0)outsideLegacy=false;
    check(outsideLegacy,"AIR fixture temp base contains no legacy component");if(!outsideLegacy)return 2;
    const auto root=temp/(L"racksynth-peer-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const bool created=std::filesystem::create_directory(root);check(created,"AIR fixture owns a fresh temp root");if(!created)return 2;
    if(std::filesystem::canonical(root).parent_path()!=temp)return 2;
    const auto executable=root/"air/private-peer/SpacePrivatePeer.exe";
    std::filesystem::create_directories(executable.parent_path());
    check(std::filesystem::copy_file(argv[1],executable),"AIR fixture executable copied");
    auto air=launch(executable);check(air.event!=nullptr,"AIR-only private child created");
    bool airJoined=false;
    if(air.event){
        PeerDetails observed;const auto pid=air.process.dwProcessId;
        const auto state=nativePeerStatus(pid,false,&observed);
        const bool pathMatches=_wcsicmp(observed.imagePath.data(),std::filesystem::path(executable).make_preferred().native().c_str())==0;
        std::cout<<"AIR_PEER outside_legacy="<<outsideLegacy<<" pid="<<pid<<" status="<<int(state)<<" parent="<<observed.parentPid
                 <<" expected_parent="<<GetCurrentProcessId()<<" path_match="<<pathMatches<<" creation="<<observed.creationFileTime<<" observed="<<observed.observedFileTime<<'\n';
        check(state==PeerStatus::present,"AIR-only child detected by its path");
        check(observed.pid==pid&&observed.parentPid==GetCurrentProcessId(),"AIR observation binds actual PID and parent");
        check(pathMatches,"AIR observation preserves queried full path");
        check(observed.creationFileTime>0&&observed.observedFileTime>=observed.creationFileTime,"AIR observation binds process generation");
        const auto [joined,code]=release(air);airJoined=joined;
        check(joined,"AIR child joined");check(code==0,"AIR child released normally");
        check(nativePeerStatus(pid)==PeerStatus::idle,"released AIR child scope is idle");
    }
    if(airJoined)for(const auto& path:{executable,executable.parent_path(),root/"air",root}){
        check(std::filesystem::weakly_canonical(path)==path,"AIR cleanup resolved path stays in owned root");
        if(std::filesystem::weakly_canonical(path)==path)check(std::filesystem::remove(path),"AIR fixture removed after join");
    }
#endif
    std::cout<<"PEER_PATH_SELF checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

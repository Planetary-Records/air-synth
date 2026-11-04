#include "ProbePeerGuard.h"
#include <filesystem>
#include <iostream>
#include <string>
using namespace airack::space::probe;
#if defined(_WIN32)
namespace {
struct Child {HANDLE event=nullptr;PROCESS_INFORMATION process{};};
Child start(const std::filesystem::path& path,const std::filesystem::path& work={}){
    Child c;SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    c.event=CreateEventW(&security,TRUE,FALSE,nullptr);if(!c.event)return c;
    auto command=L"\""+path.wstring()+(work.empty()?L"\" --fixture-event ":L"\" --fixture-coordinator ")+std::to_wstring(reinterpret_cast<std::uintptr_t>(c.event));
    if(!work.empty())command+=L" \""+work.wstring()+L"\"";
    STARTUPINFOW info{};info.cb=sizeof(info);
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&info,&c.process)){CloseHandle(c.event);c.event=nullptr;}
    return c;
}
bool finish(Child& c){SetEvent(c.event);const auto result=WaitForSingleObject(c.process.hProcess,5000);
    if(result!=WAIT_OBJECT_0){TerminateProcess(c.process.hProcess,2);WaitForSingleObject(c.process.hProcess,5000);}
    DWORD exit=2;GetExitCodeProcess(c.process.hProcess,&exit);CloseHandle(c.event);CloseHandle(c.process.hThread);CloseHandle(c.process.hProcess);
    return result==WAIT_OBJECT_0&&exit==0;
}
}
#endif
int main(int argc,char** argv){
#if defined(_WIN32)
    if(argc==3&&std::string_view(argv[1])=="--fixture-event"){
        const auto event=reinterpret_cast<HANDLE>(std::uintptr_t(std::stoull(argv[2])));
        const auto result=WaitForSingleObject(event,10000);CloseHandle(event);return result==WAIT_OBJECT_0?0:2;
    }
    if(argc==4&&std::string_view(argv[1])=="--fixture-coordinator"){
        const auto event=reinterpret_cast<HANDLE>(std::uintptr_t(std::stoull(argv[2])));
        auto work=start(argv[3]);if(!work.event){CloseHandle(event);return 2;}
        const auto result=WaitForSingleObject(event,10000);const bool joined=finish(work);CloseHandle(event);
        return result==WAIT_OBJECT_0&&joined?0:2;
    }
    unsigned checks=0,failures=0;const auto check=[&](bool v){++checks;if(!v)++failures;};
    std::array<wchar_t,32> old{};const auto length=GetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",old.data(),DWORD(old.size()));
    if(length>=old.size())return 2;
    const auto directory=argc==2?std::filesystem::absolute(argv[1]):std::filesystem::absolute(argv[0]).parent_path()/"coordinated-peer";
    for(const auto path:{L"C:/work/ai-rack-synth/build/ci/coordination-launch.exe",L"C:\\work\\AI-RACK-SYNTH\\BUILD\\CI\\COORDINATION-LAUNCH.EXE"})check(ciCoordinatorImagePath(path));
    for(const auto path:{L"C:/work/ai-rack-synth/build/private/coordination-launch.exe",L"C:/work/other/build/ci/coordination-launch.exe",L"C:/work/not-ai-rack-synth/build/ci/coordination-launch.exe",L"C:/work/ai-rack-synth/build/ci/coordination-launch.exe.other"})check(!ciCoordinatorImagePath(path));
    for(const auto name:{L"Runner.Worker.exe",L"ctest.exe",L"cmake.exe",L"RackSynthActualPeer.exe",L"coordination-launch.exe"}){
        const auto path=std::wstring_view(name)==L"coordination-launch.exe"?directory/"ordinary-fixture/ai-rack-synth/build/private/coordination-launch.exe":directory/name;
        auto child=start(path);check(child.event!=nullptr);if(!child.event)continue;
        check(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",L"shared")!=FALSE);
        check(nativePeerStatus(child.process.dwProcessId,true)==PeerStatus::present);
        check(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",L"exclusive")!=FALSE);
        const bool orchestration=std::wstring_view(name)==L"Runner.Worker.exe"||std::wstring_view(name)==L"ctest.exe";
        check(nativePeerStatus(child.process.dwProcessId,true)==(orchestration?PeerStatus::idle:PeerStatus::present));
        check(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",nullptr)!=FALSE);
        check(nativePeerStatus(child.process.dwProcessId,true)==PeerStatus::present);
        check(finish(child));
    }
    auto clone=start(directory/"ci-fixture/ai-rack-synth/build/ci/coordination-launch.exe");check(clone.event!=nullptr);
    if(clone.event){
        check(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",L"shared")!=FALSE);
        check(nativePeerStatus(clone.process.dwProcessId,true)==PeerStatus::present);
        check(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",L"exclusive")!=FALSE);
        check(nativePeerStatus(clone.process.dwProcessId,true)==PeerStatus::idle);
        auto work=start(directory/"RackSynthActualPeer.exe");check(work.event!=nullptr);
        if(work.event){check(nativePeerStatus(work.process.dwProcessId,true)==PeerStatus::present);check(finish(work));}
        check(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",nullptr)!=FALSE);
        check(nativePeerStatus(clone.process.dwProcessId,true)==PeerStatus::present);
        check(finish(clone));
    }
    std::cout<<"LEGACY_COORDINATED_PEER checks="<<checks<<" failures="<<failures<<'\n';
    const auto airCheck=[&](bool ok,const char* name){check(ok);if(!ok)std::cerr<<"FAIL "<<name<<'\n';};
    for(const auto path:{L"/air/build/ci/coordination-launch.exe",L"C:/air/build/ci/coordination-launch.exe",L"C:\\work\\AIR\\BUILD\\CI\\COORDINATION-LAUNCH.EXE"})
        airCheck(ciCoordinatorImagePath(path),"AIR exact coordinator suffix admitted");
    for(const auto path:{L"C:/not-air/build/ci/coordination-launch.exe",L"C:/chair/build/ci/coordination-launch.exe",L"C:/air/build/private/coordination-launch.exe",L"C:/air/build/ci/coordination-launch.exe.other",L"C:/air/build/ci/coordination-launch.dll"})
        airCheck(!ciCoordinatorImagePath(path),"AIR coordinator boundary excluded");
    const auto temp=std::filesystem::canonical(std::filesystem::temp_directory_path());
    bool outsideLegacy=true;for(const auto& part:temp)if(_wcsicmp(part.c_str(),L"ai-rack-synth")==0)outsideLegacy=false;
    airCheck(outsideLegacy,"AIR coordinator temp base contains no legacy component");if(!outsideLegacy)return 2;
    const auto root=temp/(L"racksynth-coordinator-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const bool created=std::filesystem::create_directory(root);airCheck(created,"AIR coordinator owns a fresh temp root");if(!created)return 2;
    if(std::filesystem::canonical(root).parent_path()!=temp)return 2;
    const auto coordinator=root/"air/build/ci/coordination-launch.exe",workPath=root/"air/build/ci/SpaceActualPeer.exe",ordinary=root/"air/build/private/coordination-launch.exe";
    for(const auto& path:{coordinator,workPath,ordinary}){
        std::filesystem::create_directories(path.parent_path());airCheck(std::filesystem::copy_file(argv[0],path),"AIR coordinator fixture executable copied");
    }
    auto air=start(coordinator,workPath);airCheck(air.event!=nullptr,"AIR coordinator created");bool airJoined=false;
    if(air.event){
        DWORD workPid=0;const auto until=GetTickCount64()+5000;
        do{
            const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
            if(snapshot!=INVALID_HANDLE_VALUE){
                if(Process32FirstW(snapshot,&entry))do{if(entry.th32ParentProcessID==air.process.dwProcessId&&_wcsicmp(entry.szExeFile,L"SpaceActualPeer.exe")==0){workPid=entry.th32ProcessID;break;}}while(Process32NextW(snapshot,&entry));
                CloseHandle(snapshot);
            }
            if(!workPid)Sleep(1);
        }while(!workPid&&GetTickCount64()<until);
        airCheck(workPid!=0,"AIR coordinator created its actual native child");
        airCheck(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",L"shared")!=FALSE,"AIR shared lease set");
        const auto shared=nativePeerStatus(air.process.dwProcessId,true);
        airCheck(shared==PeerStatus::present,"AIR coordinator present without exclusive lease");
        airCheck(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",L"exclusive")!=FALSE,"AIR exclusive lease set");
        const auto exclusive=nativePeerStatus(air.process.dwProcessId,true);
        airCheck(exclusive==PeerStatus::idle,"only exact AIR coordinator excluded under exclusive lease");
        if(workPid){
            PeerDetails observed;const auto state=nativePeerStatus(workPid,true,&observed);
            const bool pathMatches=_wcsicmp(observed.imagePath.data(),std::filesystem::path(workPath).make_preferred().native().c_str())==0;
            std::cout<<"AIR_COORDINATOR outside_legacy="<<outsideLegacy<<" coordinator="<<air.process.dwProcessId<<" child="<<workPid<<" shared="<<int(shared)<<" exclusive="<<int(exclusive)
                     <<" child_status="<<int(state)<<" parent="<<observed.parentPid<<" path_match="<<pathMatches<<" creation="<<observed.creationFileTime<<" observed="<<observed.observedFileTime<<'\n';
            airCheck(state==PeerStatus::present,"AIR coordinator actual native child stays visible");
            airCheck(observed.pid==workPid&&observed.parentPid==air.process.dwProcessId,"AIR child observation binds actual PID and parent");
            airCheck(pathMatches,"AIR child observation preserves queried full path");
            airCheck(observed.creationFileTime>0&&observed.observedFileTime>=observed.creationFileTime,"AIR child observation binds process generation");
        }
        airCheck(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",nullptr)!=FALSE,"AIR lease removed");
        airCheck(nativePeerStatus(air.process.dwProcessId,true)==PeerStatus::present,"AIR coordinator present with no lease");
        airJoined=finish(air);airCheck(airJoined,"AIR coordinator and its child joined normally");
    }
    auto wrong=start(ordinary);airCheck(wrong.event!=nullptr,"AIR wrong-subpath same-name child created");bool wrongJoined=false;
    if(wrong.event){
        airCheck(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",L"exclusive")!=FALSE,"AIR ordinary exclusive lease set");
        airCheck(nativePeerStatus(wrong.process.dwProcessId,true)==PeerStatus::present,"AIR wrong-subpath coordinator stays visible under exclusive lease");
        wrongJoined=finish(wrong);airCheck(wrongJoined,"AIR wrong-subpath child joined normally");
    }
    check(SetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",length?old.data():nullptr)!=FALSE);
    if(airJoined&&wrongJoined)for(const auto& path:{coordinator,workPath,ordinary,coordinator.parent_path(),ordinary.parent_path(),root/"air/build",root/"air",root}){
        airCheck(std::filesystem::weakly_canonical(path)==path,"AIR coordinator cleanup resolved path stays in owned root");
        if(std::filesystem::weakly_canonical(path)==path)airCheck(std::filesystem::remove(path),"AIR coordinator fixture removed after joins");
    }
    std::cout<<"COORDINATED_PEER checks="<<checks<<" failures="<<failures<<" actual_timing=0\n";return failures?1:0;
#else
    return 2;
#endif
}

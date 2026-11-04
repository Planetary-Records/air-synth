#include "ProbePeerGuard.h"
#include <iostream>
#include <thread>
#include <string_view>
using namespace airack::space::probe;
int main(int argc,char** argv){
    if(argc==2&&std::string_view(argv[1])=="--inventory-check"){
        PeerDetails observed;const auto status=nativePeerStatus(0,true,&observed);const auto name=peerImageAscii(observed);
        std::cout<<"INVENTORY status="<<(status==PeerStatus::idle?"idle":status==PeerStatus::present?"present":"unavailable")
                 <<" pid="<<observed.pid<<" image="<<name.data()<<" site="<<inventorySiteName(observed.inventorySite)<<" win32_error="<<observed.win32Error<<'\n';
        return status==PeerStatus::idle?0:status==PeerStatus::present?3:4;
    }
    if(argc==2&&std::string_view(argv[1])=="--observe-peers"){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        do{PeerDetails observed;const auto status=nativePeerStatus(0,true,&observed);
            if(status!=PeerStatus::idle){const auto name=peerImageAscii(observed);std::cout<<"PEER_OBSERVATION status="<<(status==PeerStatus::present?"present":"unavailable")<<" pid="<<observed.pid<<" image="<<name.data()<<'\n';return status==PeerStatus::present?0:3;}
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }while(std::chrono::steady_clock::now()<deadline);
        std::cout<<"PEER_OBSERVATION status=not_observed pid=0 image=none\n";return 3;
    }
    int checks=0,failed=0;const auto check=[&](bool value){++checks;if(!value)++failed;};
    for(const auto code:{std::uint32_t(0),std::uint32_t(1),std::uint32_t(259),std::uint32_t(0xffffffff)}){
        check(!processTerminated(false,code));check(processTerminated(true,code)==(code!=259));
    }
    check(kernelProcessEntry(4,0,L"System"));
    for(const auto name:{L"System.exe",L"RackSynthProbe.exe",L"python.exe",L"System ",L""})check(!kernelProcessEntry(4,0,name));
    for(const auto pid:{0u,1u,3u,5u,1234u,0xffffffffu})check(!kernelProcessEntry(pid,0,L"System"));
    for(const auto name:{L"Secure System",L"Registry",L"Memory Compression"}){
        check(kernelProcessEntry(332,4,name));
        for(const auto parent:{0u,3u,5u,1234u})check(!kernelProcessEntry(332,parent,name));
        check(!kernelProcessEntry(4,4,name));
    }
    for(const auto name:{L"Secure System.exe",L"Registry.exe",L"Memory Compression.exe",L"python.exe",L"RackSynthWork.exe"})check(!kernelProcessEntry(332,4,name));
    for(const auto name:{L"cmake.exe",L"CMAKE.EXE",L"ctest.exe",L"CTest.exe"})check(buildControllerName(name));
    for(const auto name:{L"Runner.Listener.exe",L"not-cmake.exe",L"cmake.exe.extra",L"foo.exe"})check(!buildControllerName(name));
    bool parentCovered=false;
#if defined(_WIN32)
    DWORD parent=0;const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(snapshot!=INVALID_HANDLE_VALUE){if(Process32FirstW(snapshot,&entry))do{if(entry.th32ProcessID==GetCurrentProcessId()){parent=entry.th32ParentProcessID;break;}}while(Process32NextW(snapshot,&entry));CloseHandle(snapshot);}
    const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,parent);std::array<wchar_t,1024> path{};DWORD length=DWORD(path.size());
    if(process){if(QueryFullProcessImageNameW(process,0,path.data(),&length)){
        const std::wstring_view full(path.data(),length);const auto split=full.find_last_of(L"\\/");const auto name=full.substr(split==std::wstring_view::npos?0:split+1);
        if(_wcsicmp(std::wstring(name).c_str(),L"RackSynthNativeRun.exe")==0){parentCovered=true;check(nativePeerStatus(parent,false)==PeerStatus::idle);check(nativePeerStatus(parent,true)==PeerStatus::idle);}
        if((name==L"ctest.exe"||name==L"CTest.exe"||name==L"cmake.exe")&&!projectNativePath(full)){parentCovered=true;check(nativePeerStatus(parent,false)==PeerStatus::idle);PeerDetails observed;check(nativePeerStatus(parent,true,&observed)==PeerStatus::present);check(observed.pid==parent);check(_wcsicmp(observed.imageName.data(),name.data())==0);
            PeerDetails failure;check(nativePeerStatus(parent,false,&failure,1)==PeerStatus::unavailable);check(failure.inventorySite==InventorySite::queryImage);check(failure.win32Error==ERROR_INSUFFICIENT_BUFFER);check(failure.pid==parent);check(_wcsicmp(failure.imageName.data(),name.data())==0);check(failure.exitCodeKnown);check(failure.exitCode==STILL_ACTIVE);check(failure.exitQueryError==0);}}
        CloseHandle(process);}
#endif
    std::cout<<"SpaceReverbAdmission checks="<<checks<<" failures="<<failed<<" parent_controller_covered="<<parentCovered<<'\n';return failed?1:0;
}

#pragma once
#include <chrono>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include "ProbeProcessSnapshot.h"
#endif
namespace airack::space::probe {
enum class InventorySite {none,snapshot,first,openProcess,queryImage,next};
inline const char* inventorySiteName(InventorySite site) noexcept {
    switch(site){case InventorySite::none:return "none";case InventorySite::snapshot:return "snapshot";case InventorySite::first:return "first";
        case InventorySite::openProcess:return "open_process";case InventorySite::queryImage:return "query_image";case InventorySite::next:return "next";}
    return "unknown";
}
struct PeerDetails {std::uint32_t pid=0,parentPid=0;std::array<wchar_t,260> imageName{};std::array<wchar_t,1024> imagePath{};std::uint64_t observedFileTime=0,creationFileTime=0;InventorySite inventorySite=InventorySite::none;std::uint32_t win32Error=0;bool exitCodeKnown=false;std::uint32_t exitCode=0,exitQueryError=0;};
struct PeerObserved {const char* reason="peer_native_work";PeerDetails details;};
inline std::array<char,260> peerImageAscii(const PeerDetails& details) noexcept {
    std::array<char,260> result{};
    for(std::size_t i=0;i+1<result.size()&&details.imageName[i];++i){const auto c=details.imageName[i];result[i]=c>=33&&c<=126?char(c):'_';}
    if(!result[0]){const char unknown[]="unknown";for(std::size_t i=0;i<sizeof(unknown);++i)result[i]=unknown[i];}
    return result;
}
enum class PeerStatus {idle,present,unavailable};
// STILL_ACTIVE is ambiguous even when an application uses 259 as its exit code.
inline bool processTerminated(bool exitCodeKnown,std::uint32_t exitCode) noexcept {return exitCodeKnown&&exitCode!=259;}
// These kernel records have no application image; elevated queries can return ERROR_GEN_FAILURE.
// Match the known names AND System ownership. Ordinary executables retain the existing inventory rules.
inline bool kernelProcessEntry(std::uint32_t pid,std::uint32_t parent,std::wstring_view name) noexcept {
    if(pid==4&&parent==0&&name==L"System")return true;
    return pid>4&&parent==4&&(name==L"Secure System"||name==L"Registry"||name==L"Memory Compression");
}
inline bool buildControllerName(std::wstring_view name) noexcept {
    for(const auto expected:{std::wstring_view(L"cmake.exe"),std::wstring_view(L"ctest.exe")}){
        if(name.size()!=expected.size())continue;bool equal=true;
        for(std::size_t i=0;i<name.size();++i){const auto value=name[i]>=L'A'&&name[i]<=L'Z'?wchar_t(name[i]+(L'a'-L'A')):name[i];if(value!=expected[i]){equal=false;break;}}
        if(equal)return true;
    }
    return false;
}
inline bool projectNativePath(std::wstring_view path) noexcept
{
    const auto lower=[](wchar_t c)->wchar_t {if(c==L'\\')return L'/';return c>=L'A'&&c<=L'Z'?wchar_t(c+(L'a'-L'A')):c;};
    const auto equal=[&](std::wstring_view a,std::wstring_view b){if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(lower(a[i])!=lower(b[i]))return false;return true;};
    if(path.size()<4||!equal(path.substr(path.size()-4),L".exe"))return false;
    for(const auto component:{std::wstring_view(L"ai-rack-synth"),std::wstring_view(L"air")}){
        if(path.size()<component.size()+5)continue;
        for(std::size_t start=0;start+component.size()<path.size();++start)
            if((start==0||lower(path[start-1])==L'/')&&lower(path[start+component.size()])==L'/'&&equal(path.substr(start,component.size()),component))return true;
    }
    return false;
}
inline bool ciCoordinatorImagePath(std::wstring_view path) noexcept {
    for(const auto suffix:{std::wstring_view(L"/ai-rack-synth/build/ci/coordination-launch.exe"),std::wstring_view(L"/air/build/ci/coordination-launch.exe")}){
        if(path.size()<suffix.size())continue;
        const auto tail=path.substr(path.size()-suffix.size());bool equal=true;
        for(std::size_t i=0;i<suffix.size();++i){
            auto c=tail[i];if(c==L'\\')c=L'/';if(c>=L'A'&&c<=L'Z')c=wchar_t(c+(L'a'-L'A'));
            if(c!=suffix[i]){equal=false;break;}
        }
        if(equal)return true;
    }
    return false;
}
#if defined(_WIN32)
using PeerImageQuery=BOOL(WINAPI*)(HANDLE,DWORD,LPWSTR,PDWORD);
inline bool queryPeerImage(HANDLE process,wchar_t* path,DWORD& length,DWORD& error,
                          PeerImageQuery query=QueryFullProcessImageNameW) noexcept {
    const bool queried=query(process,0,path,&length)!=FALSE;
    error=queried?0:GetLastError();return queried;
}
#endif
inline bool exclusiveNativeLease() noexcept {
#if defined(_WIN32)
    std::array<wchar_t,10> lease{};
    return GetEnvironmentVariableW(L"AIRACK_NATIVE_LEASE",lease.data(),DWORD(lease.size()))==9&&std::wstring_view(lease.data())==L"exclusive";
#else
    return false;
#endif
}
inline PeerStatus nativePeerStatus(std::uint32_t onlyPid=0,bool includeBuildControllers=false,PeerDetails* observation=nullptr,[[maybe_unused]] std::size_t imagePathCapacity=1024)
{
    if(observation)*observation={};
#if defined(_WIN32)
    const bool exclusive=exclusiveNativeLease();
    PeerProcessEntry entry{};bool found=false,uncertain=false;
    const auto remember=[&]{if(observation){observation->pid=entry.th32ProcessID;observation->parentPid=entry.th32ParentProcessID;
        FILETIME now{};GetSystemTimeAsFileTime(&now);observation->observedFileTime=std::uint64_t(now.dwLowDateTime)|(std::uint64_t(now.dwHighDateTime)<<32);
        const std::wstring_view name(entry.szExeFile);
        for(std::size_t i=0;i+1<observation->imageName.size()&&i<name.size();++i)observation->imageName[i]=name[i];}};
    const auto failure=[&](InventorySite site,DWORD error,bool haveEntry){if(observation){observation->inventorySite=site;observation->win32Error=error;if(haveEntry)remember();}};
    thread_local std::vector<PeerProcessEntry> entries;SnapshotError snapshotError;
    if(processSnapshot(entries,snapshotError)==SnapshotBackend::none){
        const auto site=snapshotError.site==SnapshotSite::first?InventorySite::first:snapshotError.site==SnapshotSite::next?InventorySite::next:InventorySite::snapshot;
        failure(site,snapshotError.code,false);return PeerStatus::unavailable;
    }
    for(const auto& current:entries){entry=current;if(onlyPid!=0&&entry.th32ProcessID!=onlyPid)continue;
        const std::wstring_view name(entry.szExeFile);
        // Coordinators wait on OS leases; their child native work remains visible to this guard.
        if(_wcsicmp(entry.szExeFile,L"RackSynthNativeRun.exe")==0)continue;
        // Under an exclusive lease these orchestrators can queue; actual native children stay checked.
        if(exclusive&&(_wcsicmp(entry.szExeFile,L"Runner.Worker.exe")==0||_wcsicmp(entry.szExeFile,L"ctest.exe")==0))continue;
        if(entry.th32ProcessID==GetCurrentProcessId()||_wcsicmp(entry.szExeFile,L"Runner.Listener.exe")==0)continue;
        if(kernelProcessEntry(entry.th32ProcessID,entry.th32ParentProcessID,name))continue;
        if((includeBuildControllers&&buildControllerName(name))||name.starts_with(L"RackSynth")||_wcsicmp(entry.szExeFile,L"Runner.Worker.exe")==0||_wcsicmp(entry.szExeFile,L"cl.exe")==0
            ||_wcsicmp(entry.szExeFile,L"link.exe")==0||_wcsicmp(entry.szExeFile,L"pluginval.exe")==0){remember();found=true;break;}
        const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
        if(!process){const auto error=GetLastError();if(error==ERROR_ACCESS_DENIED||error==ERROR_INVALID_PARAMETER)continue;failure(InventorySite::openProcess,error,true);uncertain=true;break;}
        std::array<wchar_t,1024> path{};DWORD length=DWORD(imagePathCapacity<path.size()?imagePathCapacity:path.size());
        DWORD error=0;const bool queried=queryPeerImage(process,path.data(),length,error);
        DWORD exitCode=0,exitError=0;bool exitKnown=false;
        if(!queried){exitKnown=GetExitCodeProcess(process,&exitCode)!=FALSE;if(!exitKnown)exitError=GetLastError();}
        const bool projectPath=queried&&projectNativePath(std::wstring_view(path.data(),length));
        if(projectPath&&observation&&!(exclusive&&ciCoordinatorImagePath(std::wstring_view(path.data(),length)))){
            for(std::size_t i=0;i+1<observation->imagePath.size()&&i<length;++i)observation->imagePath[i]=path[i];
            FILETIME created{},exited{},kernel{},user{};
            if(GetProcessTimes(process,&created,&exited,&kernel,&user))observation->creationFileTime=std::uint64_t(created.dwLowDateTime)|(std::uint64_t(created.dwHighDateTime)<<32);
        }
        CloseHandle(process);
        if(!queried){if(error==ERROR_GEN_FAILURE&&processTerminated(exitKnown,exitCode))continue;
            if(error==ERROR_ACCESS_DENIED||error==ERROR_INVALID_PARAMETER||error==ERROR_INVALID_HANDLE)continue;failure(InventorySite::queryImage,error,true);
            if(observation){observation->exitCodeKnown=exitKnown;observation->exitCode=exitCode;observation->exitQueryError=exitError;}
            uncertain=true;break;}
        if(projectPath){
            if(exclusive&&ciCoordinatorImagePath(std::wstring_view(path.data(),length)))continue;
            remember();found=true;break;
        }
    }
    if(found)return PeerStatus::present;if(uncertain)return PeerStatus::unavailable;return PeerStatus::idle;
#else
    (void)onlyPid;
    return PeerStatus::unavailable;
#endif
}
class PeerGuard {
    using Clock=std::chrono::steady_clock;
    Clock::time_point next_{};
    bool includeBuildControllers_=false;
public:
    explicit PeerGuard(bool includeBuildControllers=false) noexcept:includeBuildControllers_(includeBuildControllers){}
    void operator()(){const auto now=Clock::now();if(now<next_)return;next_=now+std::chrono::milliseconds(100);PeerDetails observed;const auto state=nativePeerStatus(0,includeBuildControllers_,&observed);if(state!=PeerStatus::idle)throw PeerObserved{state==PeerStatus::present?"peer_native_work":"process_inventory_unavailable",observed};}
    void force(){next_={};(*this)();}
};
}

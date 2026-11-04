#pragma once
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>
namespace airack::space::probe {
struct PeerProcessEntry {DWORD th32ProcessID=0,th32ParentProcessID=0;wchar_t szExeFile[MAX_PATH]{};};
enum class SnapshotBackend {none,basic,toolhelp};
enum class SnapshotSite {snapshot,first,next};
struct SnapshotError {SnapshotSite site=SnapshotSite::snapshot;DWORD code=0;};
struct BasicProcessName {USHORT length=0,maximum=0;wchar_t* buffer=nullptr;};
struct BasicProcessRecord {ULONG next=0;HANDLE pid=nullptr,parent=nullptr;ULONGLONG sequence=0;BasicProcessName name;};
using BasicProcessQuery=LONG(WINAPI*)(ULONG,void*,ULONG,ULONG*);
inline BasicProcessQuery basicProcessQuery() noexcept {
    static const auto query=[] {const auto module=GetModuleHandleW(L"ntdll.dll");return module
        ?reinterpret_cast<BasicProcessQuery>(GetProcAddress(module,"NtQuerySystemInformation")):nullptr;}();
    return query;
}
inline bool decodeBasicProcesses(std::span<const std::byte> bytes,std::vector<PeerProcessEntry>& entries) {
    entries.clear();const auto invalid=[&]{entries.clear();return false;};
    const auto base=reinterpret_cast<std::uintptr_t>(bytes.data());std::size_t offset=0;
    for(;;){
        if(offset>bytes.size()||bytes.size()-offset<sizeof(BasicProcessRecord))return invalid();
        BasicProcessRecord row;std::memcpy(&row,bytes.data()+offset,sizeof(row));
        const auto pid=reinterpret_cast<std::uintptr_t>(row.pid),parent=reinterpret_cast<std::uintptr_t>(row.parent);
        const auto name=reinterpret_cast<std::uintptr_t>(row.name.buffer);
        if(pid>MAXDWORD||parent>MAXDWORD||row.name.length%sizeof(wchar_t)||row.name.length>row.name.maximum
            ||row.name.length/sizeof(wchar_t)>=MAX_PATH)return invalid();
        if(row.name.length&&(name<base||name-base>bytes.size()||row.name.length>bytes.size()-(name-base)||name%alignof(wchar_t)))return invalid();
        const auto chars=row.name.length/sizeof(wchar_t);
        if(chars&&std::find(row.name.buffer,row.name.buffer+chars,L'\0')!=row.name.buffer+chars)return invalid();
        PeerProcessEntry entry{DWORD(pid),DWORD(parent)};
        if(chars)std::copy_n(row.name.buffer,chars,entry.szExeFile);
        entries.push_back(entry);
        if(row.next==0)return true;
        if(row.next<sizeof(row)||row.next%alignof(BasicProcessRecord)||row.next>bytes.size()-offset)return invalid();
        offset+=row.next;
    }
}
inline SnapshotBackend basicProcessSnapshot(std::vector<PeerProcessEntry>& entries,SnapshotError& error,BasicProcessQuery query=basicProcessQuery()) noexcept {
    entries.clear();error={};if(!query)return SnapshotBackend::none;
#if defined(_WIN64)
    static_assert(sizeof(BasicProcessRecord)==48&&offsetof(BasicProcessRecord,name)==32);
    try{
        thread_local std::vector<std::byte> bytes(65536);constexpr std::size_t maximum=4*1024*1024;
        for(unsigned attempt=0;attempt<8;++attempt){
            ULONG used=0;
            // SystemBasicProcessInformation, Win11 26100.4770+: no CPU timing synchronization.
            // https://learn.microsoft.com/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation
            const auto status=std::uint32_t(query(252,bytes.data(),ULONG(bytes.size()),&used));
            if(status==0xc0000002u||status==0xc0000003u)return SnapshotBackend::none;
            if(status==0xc0000004u||status==0xc0000023u){
                const auto needed=std::max(bytes.size()*2,std::size_t(used));
                if(needed>maximum){error.code=ERROR_INSUFFICIENT_BUFFER;return SnapshotBackend::none;}
                bytes.resize(needed);continue;
            }
            if(status){
                using Map=ULONG(WINAPI*)(LONG);const auto map=reinterpret_cast<Map>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlNtStatusToDosError"));
                error.code=map?map(LONG(status)):ERROR_GEN_FAILURE;return SnapshotBackend::none;
            }
            if(used>bytes.size()||!decodeBasicProcesses(std::span(bytes).first(used),entries)){error.code=ERROR_INVALID_DATA;return SnapshotBackend::none;}
            return SnapshotBackend::basic;
        }
        error.code=ERROR_INSUFFICIENT_BUFFER;
    }catch(...){entries.clear();error.code=ERROR_NOT_ENOUGH_MEMORY;}
#else
    (void)query;
#endif
    return SnapshotBackend::none;
}
inline SnapshotBackend toolhelpProcessSnapshot(std::vector<PeerProcessEntry>& entries,SnapshotError& error) noexcept {
    entries.clear();error={};const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snapshot==INVALID_HANDLE_VALUE){error.code=GetLastError();return SnapshotBackend::none;}
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    if(!Process32FirstW(snapshot,&entry)){error={SnapshotSite::first,GetLastError()};CloseHandle(snapshot);return SnapshotBackend::none;}
    try{
        do{PeerProcessEntry value{entry.th32ProcessID,entry.th32ParentProcessID};std::copy_n(entry.szExeFile,MAX_PATH,value.szExeFile);entries.push_back(value);}while(Process32NextW(snapshot,&entry));
        const auto last=GetLastError();CloseHandle(snapshot);
        if(last==ERROR_NO_MORE_FILES)return SnapshotBackend::toolhelp;
        entries.clear();error={SnapshotSite::next,last};
    }catch(...){CloseHandle(snapshot);entries.clear();error.code=ERROR_NOT_ENOUGH_MEMORY;}
    return SnapshotBackend::none;
}
inline SnapshotBackend processSnapshot(std::vector<PeerProcessEntry>& entries,SnapshotError& error,BasicProcessQuery query=basicProcessQuery()) noexcept {
    const auto backend=basicProcessSnapshot(entries,error,query);
    if(backend!=SnapshotBackend::none||error.code)return backend;
    return toolhelpProcessSnapshot(entries,error);
}
}
#endif

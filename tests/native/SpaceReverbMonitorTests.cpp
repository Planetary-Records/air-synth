#include "ProbePeerMonitor.h"
#include "ProbeProcessSnapshot.h"
#include <atomic>
#include <iostream>
#include <thread>
#include <string_view>
using namespace airack::space::probe;
namespace {
struct Context {
    std::atomic<PeerStatus> state{PeerStatus::idle};std::atomic<unsigned> calls{0};std::atomic<bool> onCaller{false};
    std::thread::id caller=std::this_thread::get_id();PeerDetails details;
    static PeerStatus scan(void* p,bool,PeerDetails& out){auto& c=*static_cast<Context*>(p);if(std::this_thread::get_id()==c.caller)c.onCaller=true;++c.calls;out=c.details;return c.state.load();}
};
struct RefreshContext {
    std::atomic<unsigned> waits{0};
    static PeerStatus scan(void* p,bool,PeerDetails&){
        auto& c=*static_cast<RefreshContext*>(p);
        const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(50);
        while(c.waits.load()==0&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return PeerStatus::idle;
    }
    static void wait(void* p) noexcept {
        ++static_cast<RefreshContext*>(p)->waits;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};
template<class Work> PeerObserved error(Work work){try{work();return {nullptr,{}};}catch(const PeerObserved& e){return e;}}
bool reason(const PeerObserved& e,const char* expected){return e.reason&&std::string_view(e.reason)==expected;}
#if defined(_WIN64)
struct ImageQueryFixture {
    std::wstring native=L"\\Device\\HarddiskVolume3\\Windows\\foreign.exe",dos=L"C:\\Windows\\foreign.exe";
    std::vector<DWORD> flags;bool nativeFails=false,dosFails=false;
};
ImageQueryFixture imageQueryFixture;
BOOL WINAPI fakeImageQuery(HANDLE,DWORD flags,LPWSTR path,PDWORD length){
    auto& f=imageQueryFixture;f.flags.push_back(flags);
    if((flags==PROCESS_NAME_NATIVE&&f.nativeFails)||(flags==0&&f.dosFails)){SetLastError(ERROR_GEN_FAILURE);return FALSE;}
    const auto& value=flags==PROCESS_NAME_NATIVE?f.native:f.dos;
    if(*length<=value.size()){SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
    std::copy(value.begin(),value.end(),path);path[value.size()]=0;*length=DWORD(value.size());return TRUE;
}
template<class Check>void imageQueryTests(Check check){
    std::array<wchar_t,1024> path{};DWORD length=DWORD(path.size()),error=0;
    check(queryPeerImage(nullptr,path.data(),length,error,fakeImageQuery)&&error==0);
    check(imageQueryFixture.flags==std::vector<DWORD>{0});
    check(!projectNativePath(std::wstring_view(path.data(),length)));
    imageQueryFixture={};imageQueryFixture.native=L"\\Device\\HarddiskVolume3\\work\\ai-rack-synth\\private.exe";
    imageQueryFixture.dos=L"C:\\work\\ai-rack-synth\\private.exe";length=DWORD(path.size());
    check(queryPeerImage(nullptr,path.data(),length,error,fakeImageQuery));
    check(imageQueryFixture.flags==std::vector<DWORD>({0}));
    check(std::wstring_view(path.data(),length)==imageQueryFixture.dos);
    imageQueryFixture={};imageQueryFixture.dos=L"C:\\ai-rack-synth\\mounted\\foreign.exe";length=DWORD(path.size());
    check(queryPeerImage(nullptr,path.data(),length,error,fakeImageQuery));
    check(imageQueryFixture.flags==std::vector<DWORD>({0}));
    check(projectNativePath(std::wstring_view(path.data(),length)));
    imageQueryFixture={};imageQueryFixture.native=L"\\Device\\HarddiskVolume30\\foreign.exe";length=DWORD(path.size());
    check(queryPeerImage(nullptr,path.data(),length,error,fakeImageQuery));
    check(imageQueryFixture.flags==std::vector<DWORD>({0}));
    imageQueryFixture={};imageQueryFixture.nativeFails=true;length=DWORD(path.size());
    check(queryPeerImage(nullptr,path.data(),length,error,fakeImageQuery)&&error==0);
    check(imageQueryFixture.flags==std::vector<DWORD>({0}));
    imageQueryFixture={};imageQueryFixture.nativeFails=imageQueryFixture.dosFails=true;length=DWORD(path.size());
    check(!queryPeerImage(nullptr,path.data(),length,error,fakeImageQuery)&&error==ERROR_GEN_FAILURE);
    imageQueryFixture={};length=0;
    check(!queryPeerImage(nullptr,path.data(),length,error,fakeImageQuery)&&error==ERROR_INSUFFICIENT_BUFFER);
}
LONG WINAPI unsupportedSnapshot(ULONG,void*,ULONG,ULONG*){return LONG(0xc0000003u);}
LONG WINAPI deniedSnapshot(ULONG,void*,ULONG,ULONG*){return LONG(0xc0000022u);}
LONG WINAPI shortSnapshot(ULONG,void*,ULONG,ULONG* used){*used=1;return 0;}
template<class Check>void snapshotTests(Check check){
    alignas(void*) std::array<std::byte,256> bytes{};
    constexpr wchar_t first[]=L"RackSynthA.exe",second[]=L"foreign.exe";
    BasicProcessRecord a{96,reinterpret_cast<HANDLE>(41),reinterpret_cast<HANDLE>(40),1,
        {USHORT(sizeof(first)-sizeof(wchar_t)),USHORT(sizeof(first)),reinterpret_cast<wchar_t*>(bytes.data()+48)}};
    BasicProcessRecord b{0,reinterpret_cast<HANDLE>(42),reinterpret_cast<HANDLE>(41),2,
        {USHORT(sizeof(second)-sizeof(wchar_t)),USHORT(sizeof(second)),reinterpret_cast<wchar_t*>(bytes.data()+144)}};
    std::memcpy(bytes.data(),&a,sizeof(a));std::memcpy(bytes.data()+96,&b,sizeof(b));
    std::memcpy(bytes.data()+48,first,sizeof(first));std::memcpy(bytes.data()+144,second,sizeof(second));
    std::vector<PeerProcessEntry> entries;
    check(decodeBasicProcesses(bytes,entries));check(entries.size()==2);
    check(entries[0].th32ProcessID==41&&entries[1].th32ParentProcessID==41&&std::wstring_view(entries[0].szExeFile)==first);
    const auto bad=[&](BasicProcessRecord broken){std::memcpy(bytes.data(),&broken,sizeof(broken));check(!decodeBasicProcesses(bytes,entries)&&entries.empty());std::memcpy(bytes.data(),&a,sizeof(a));};
    auto broken=a;broken.next=1;bad(broken);broken=a;broken.next=264;bad(broken);
    broken=a;broken.pid=reinterpret_cast<HANDLE>(std::uint64_t(MAXDWORD)+1);bad(broken);
    broken=a;broken.name.length=1;bad(broken);broken=a;broken.name.maximum=0;bad(broken);
    broken=a;broken.name.buffer=reinterpret_cast<wchar_t*>(reinterpret_cast<std::uintptr_t>(bytes.data())-2);bad(broken);
    broken=a;broken.name.buffer=reinterpret_cast<wchar_t*>(bytes.data()+254);bad(broken);
    broken=a;broken.name.buffer=reinterpret_cast<wchar_t*>(bytes.data()+49);bad(broken);
    broken=a;broken.name.length=broken.name.maximum=520;bad(broken);
    broken=b;broken.name.length=1;std::memcpy(bytes.data()+96,&broken,sizeof(broken));
    check(!decodeBasicProcesses(bytes,entries)&&entries.empty());std::memcpy(bytes.data()+96,&b,sizeof(b));
    reinterpret_cast<wchar_t*>(bytes.data()+48)[1]=0;check(!decodeBasicProcesses(bytes,entries)&&entries.empty());
    std::memcpy(bytes.data()+48,first,sizeof(first));check(!decodeBasicProcesses({},entries)&&entries.empty());
    SnapshotError error;const auto basic=basicProcessSnapshot(entries,error);
    check(basic==SnapshotBackend::basic||error.code==0);
    const auto backend=processSnapshot(entries,error);check(backend!=SnapshotBackend::none);
    check(basic!=SnapshotBackend::basic||backend==SnapshotBackend::basic);
    const auto own=std::find_if(entries.begin(),entries.end(),[](const auto& e){return e.th32ProcessID==GetCurrentProcessId();});
    check(own!=entries.end());const auto ownCopy=own!=entries.end()?*own:PeerProcessEntry{};
    check(processSnapshot(entries,error,unsupportedSnapshot)==SnapshotBackend::toolhelp);
    const auto fallback=std::find_if(entries.begin(),entries.end(),[](const auto& e){return e.th32ProcessID==GetCurrentProcessId();});
    check(fallback!=entries.end()&&fallback->th32ParentProcessID==ownCopy.th32ParentProcessID&&std::wstring_view(fallback->szExeFile)==ownCopy.szExeFile);
    check(processSnapshot(entries,error,deniedSnapshot)==SnapshotBackend::none&&error.code!=0&&entries.empty());
    check(processSnapshot(entries,error,shortSnapshot)==SnapshotBackend::none&&error.code==ERROR_INVALID_DATA&&entries.empty());
    std::vector<PeerProcessEntry> before,after;check(toolhelpProcessSnapshot(before,error)==SnapshotBackend::toolhelp);
    check(processSnapshot(entries,error)!=SnapshotBackend::none);check(toolhelpProcessSnapshot(after,error)==SnapshotBackend::toolhelp);
    bool covered=true;std::size_t stable=0;
    for(const auto& entry:before){
        const auto same=[&](const auto& other){return other.th32ProcessID==entry.th32ProcessID&&other.th32ParentProcessID==entry.th32ParentProcessID
            &&std::wstring_view(other.szExeFile)==entry.szExeFile;};
        if(std::find_if(after.begin(),after.end(),same)==after.end())continue;
        ++stable;covered&=std::any_of(entries.begin(),entries.end(),[&](const auto& other){return other.th32ProcessID==entry.th32ProcessID
            &&other.th32ParentProcessID==entry.th32ParentProcessID&&(entry.th32ProcessID==0||std::wstring_view(other.szExeFile)==entry.szExeFile);});
    }
    check(stable>0&&covered);
    std::cout<<"PROCESS_SNAPSHOT_NATIVE basic_supported="<<(basic==SnapshotBackend::basic)<<" selected_basic="<<(backend==SnapshotBackend::basic)<<" fallback_checked=1\n";
}
#endif
}
int main(){
    unsigned checks=0,failures=0;const auto check=[&](bool ok){++checks;if(!ok){++failures;std::cerr<<"ENDPOINT_FAIL monitor check="<<checks<<'\n';}};
#if defined(_WIN64)
    imageQueryTests(check);
    snapshotTests(check);
#endif
    Context idle;idle.details.pid=4242;idle.details.imageName[0]=L'F';ConcurrentPeerGuard guard(true,Context::scan,&idle);
    check(error([&]{guard.force();}).reason==nullptr);check(idle.calls>0);check(!idle.onCaller);
    bool good=true;for(unsigned i=0;i<1000;++i)good=error([&]{guard();}).reason==nullptr&&good;check(good);
    idle.state=PeerStatus::present;
    // Metadata is immutable from construction; only status changes atomically.
    const auto present=error([&]{guard.force();});check(reason(present,"peer_native_work"));check(present.details.pid==4242);check(present.details.imageName[0]==L'F');
    idle.state=PeerStatus::idle;check(reason(error([&]{guard.force();}),"peer_native_work"));guard.stop();check(!idle.onCaller);
    Context unavailable;unavailable.state=PeerStatus::unavailable;unavailable.details.pid=777;unavailable.details.inventorySite=InventorySite::queryImage;unavailable.details.win32Error=122;unavailable.details.exitCodeKnown=true;unavailable.details.exitCode=259;
    ConcurrentPeerGuard failed(true,Context::scan,&unavailable);const auto missing=error([&]{failed.force();});
    check(reason(missing,"process_inventory_unavailable"));check(missing.details.pid==777);check(missing.details.inventorySite==InventorySite::queryImage);check(missing.details.win32Error==122);check(missing.details.exitCodeKnown);check(missing.details.exitCode==259);check(missing.details.exitQueryError==0);failed.stop();check(!unavailable.onCaller);
    Context stopped;ConcurrentPeerGuard done(true,Context::scan,&stopped);(void)error([&]{done.force();});done.stop();check(reason(error([&]{done();}),"inventory_monitor_stopped"));
    RefreshContext refresh;ConcurrentPeerGuard blocking(true,RefreshContext::scan,&refresh,RefreshContext::wait,&refresh);
    check(error([&]{blocking.force();}).reason==nullptr);check(refresh.waits.load()>0);
    const auto before=refresh.waits.load();for(unsigned i=0;i<1000;++i)blocking();
    check(refresh.waits.load()==before);blocking.stop();
    Context cadence;ConcurrentPeerGuard periodic(true,Context::scan,&cadence);
    check(error([&]{periodic.force();}).reason==nullptr);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(cadence.calls.load()<4&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    periodic.stop();const auto statistics=periodic.statistics();
    check(statistics.scans>=4);check(statistics.scans==cadence.calls.load());
    check(statistics.waits<=statistics.scans+1);check(!cadence.onCaller);
    std::cout<<"MONITOR_IDLE scans="<<statistics.scans<<" waits="<<statistics.waits<<" cadence_ms=100 freshness_ms=250\n";
    std::cout<<"SpaceReverbMonitor checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

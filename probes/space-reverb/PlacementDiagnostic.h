#pragma once
// Caller-only diagnostic. This header is absent from the capacity probe's build.
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>
namespace airack::space::probe {
class PlacementDiagnostic {
    struct Endpoint {unsigned phase;std::uint64_t ordinal;PROCESSOR_NUMBER before{},after{};};
    struct Cpu {DWORD id;WORD group;BYTE index,core,efficiency,flags;};
public:
    PlacementDiagnostic(std::uint64_t warm,std::uint64_t measured){endpoints_.reserve(std::size_t(warm+measured));}
    bool captureTopology(){
        ULONG needed=0;
        if(GetSystemCpuSetInformation(nullptr,0,&needed,GetCurrentProcess(),0)||GetLastError()!=ERROR_INSUFFICIENT_BUFFER||!needed){topologyError_=ERROR_INVALID_DATA;return false;}
        bool read=false;std::vector<std::uint64_t> buffer;
        for(unsigned attempt=0;attempt<3&&!read;++attempt){
            buffer.resize((needed+7ULL)/8);ULONG returned=0;
            read=GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),ULONG(buffer.size()*8),&returned,GetCurrentProcess(),0)!=FALSE;
            if(!read){topologyError_=GetLastError();if(topologyError_!=ERROR_INSUFFICIENT_BUFFER)return false;needed=returned;}
            else needed=returned;
        }
        if(!read)return false;
        const auto* bytes=reinterpret_cast<const unsigned char*>(buffer.data());
        for(ULONG offset=0;offset<needed;){
            if(needed-offset<sizeof(DWORD)*2){topologyError_=ERROR_BAD_LENGTH;return false;}
            const auto* entry=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(bytes+offset);
            if(entry->Size<sizeof(DWORD)*2||entry->Size>needed-offset){topologyError_=ERROR_BAD_LENGTH;return false;}
            if(entry->Type==CpuSetInformation){
                if(entry->Size<sizeof(SYSTEM_CPU_SET_INFORMATION)){topologyError_=ERROR_BAD_LENGTH;return false;}
                const auto& c=entry->CpuSet;cpus_.push_back({c.Id,c.Group,c.LogicalProcessorIndex,c.CoreIndex,c.EfficiencyClass,c.AllFlags});
            }
            offset+=entry->Size;
        }
        if(cpus_.empty()){topologyError_=ERROR_NOT_FOUND;return false;}
        topologyError_=0;
        power_.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        powerOk_=GetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&power_,sizeof(power_))!=FALSE;powerError_=powerOk_?0:GetLastError();
        idealOk_=GetThreadIdealProcessorEx(GetCurrentThread(),&ideal_)!=FALSE;idealError_=idealOk_?0:GetLastError();
        affinityOk_=GetThreadGroupAffinity(GetCurrentThread(),&affinity_)!=FALSE;affinityError_=affinityOk_?0:GetLastError();
        return true;
    }
    void begin(unsigned phase,std::uint64_t ordinal) noexcept {pending_.phase=phase;pending_.ordinal=ordinal;GetCurrentProcessorNumberEx(&pending_.before);}
    void finish(){GetCurrentProcessorNumberEx(&pending_.after);endpoints_.push_back(pending_);}
    bool save(const std::filesystem::path& path,std::uint64_t warm,std::uint64_t measured) const {
        if(std::filesystem::exists(path))return false;std::ofstream out(path);if(!out)return false;
        out<<"{\"version\":1,\"diagnostic_only\":true,\"topology_error\":"<<topologyError_
           <<",\"process_power_before\":{\"ok\":"<<int(powerOk_)<<",\"error\":"<<powerError_<<",\"control\":"<<power_.ControlMask<<",\"state\":"<<power_.StateMask<<"}"
           <<",\"ideal_before\":{\"ok\":"<<int(idealOk_)<<",\"error\":"<<idealError_<<",\"group\":"<<ideal_.Group<<",\"index\":"<<unsigned(ideal_.Number)<<"}"
           <<",\"affinity_before\":{\"ok\":"<<int(affinityOk_)<<",\"error\":"<<affinityError_<<",\"group\":"<<affinity_.Group<<",\"mask\":"<<std::uint64_t(affinity_.Mask)<<"}"
           <<",\"warm_completed\":"<<warm<<",\"measured\":"<<measured<<",\"cpu_sets\":[";
        bool comma=false;for(const auto& c:cpus_){if(comma)out<<',';comma=true;out<<"{\"id\":"<<c.id<<",\"group\":"<<c.group<<",\"index\":"<<unsigned(c.index)<<",\"core\":"<<unsigned(c.core)<<",\"efficiency\":"<<unsigned(c.efficiency)<<",\"flags\":"<<unsigned(c.flags)<<'}';}
        out<<"],\"placements\":[";comma=false;for(const auto& e:endpoints_){if(comma)out<<',';comma=true;out<<'['<<e.phase<<','<<e.ordinal<<','<<e.before.Group<<','<<unsigned(e.before.Number)<<','<<e.after.Group<<','<<unsigned(e.after.Number)<<']';}
        out<<"]}\n";out.close();return bool(out);
    }
private:
    std::vector<Endpoint> endpoints_;std::vector<Cpu> cpus_;Endpoint pending_{};
    DWORD topologyError_=0,powerError_=0,idealError_=0,affinityError_=0;
    bool powerOk_=false,idealOk_=false,affinityOk_=false;
    PROCESS_POWER_THROTTLING_STATE power_{};PROCESSOR_NUMBER ideal_{};GROUP_AFFINITY affinity_{};
};
}

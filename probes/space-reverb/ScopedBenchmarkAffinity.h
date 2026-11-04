#pragma once
// Benchmark threads only. Never include or link this helper in the product.
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static_assert(sizeof(KAFFINITY)==sizeof(std::uint64_t),"CPU28 benchmark condition is Windows x64 only");
#endif

namespace airack::space::probe {
struct BenchmarkGroupAffinity {
    std::uint64_t mask=0;
    std::uint16_t group=0;
    bool operator==(const BenchmarkGroupAffinity&) const = default;
};
struct BenchmarkCpuSet {
    std::uint32_t id=0;
    std::uint16_t group=0;
    std::uint8_t index=0,core=0,efficiency=0,flags=0;
};
struct BenchmarkAffinityApi {
    void* context=nullptr;
    bool (*get)(void*,BenchmarkGroupAffinity&,std::uint32_t&) noexcept=nullptr;
    bool (*set)(void*,const BenchmarkGroupAffinity&,std::uint32_t&) noexcept=nullptr;
};
enum class BenchmarkPlacement { pinnedCaller, movableTimer, reservedCaller, stableCaller };
inline BenchmarkGroupAffinity timerAffinityAwayFromCaller(BenchmarkGroupAffinity available,
        std::span<const BenchmarkCpuSet> topology,std::uint16_t group,std::uint8_t index) noexcept {
    if(group!=available.group||index>=64)return available;
    bool found=false;std::uint8_t core=0;
    for(const auto& cpu:topology)if(cpu.group==group&&cpu.index==index){core=cpu.core;found=true;break;}
    if(!found)return available;
    auto desired=available;
    for(const auto& cpu:topology)if(cpu.group==group&&cpu.core==core&&cpu.index<64)
        desired.mask&=~(std::uint64_t(1)<<cpu.index);
    return std::popcount(desired.mask)>=4?desired:available;
}
inline BenchmarkGroupAffinity timerAffinityOnCaller(BenchmarkGroupAffinity inherited,
        std::uint16_t group,std::uint8_t index) noexcept {
    if(group!=inherited.group||index>=64)return {};
    const auto bit=std::uint64_t(1)<<index;
    return (inherited.mask&bit)?BenchmarkGroupAffinity{bit,group}:BenchmarkGroupAffinity{};
}
inline BenchmarkAffinityApi nativeBenchmarkAffinityApi() noexcept {
#if defined(_WIN32)
    return {nullptr,
        [](void*,BenchmarkGroupAffinity& output,std::uint32_t& error) noexcept {
            GROUP_AFFINITY value{};
            if(!GetThreadGroupAffinity(GetCurrentThread(),&value)){error=GetLastError();return false;}
            output={std::uint64_t(value.Mask),value.Group};return true;
        },
        [](void*,const BenchmarkGroupAffinity& input,std::uint32_t& error) noexcept {
            GROUP_AFFINITY value{};value.Group=input.group;value.Mask=KAFFINITY(input.mask);
            if(SetThreadGroupAffinity(GetCurrentThread(),&value,nullptr))return true;
            error=GetLastError();return false;
        }};
#else
    return {};
#endif
}

class ScopedBenchmarkAffinity {
public:
    ScopedBenchmarkAffinity()=default;
    ~ScopedBenchmarkAffinity(){(void)close();}
    ScopedBenchmarkAffinity(const ScopedBenchmarkAffinity&)=delete;
    ScopedBenchmarkAffinity& operator=(const ScopedBenchmarkAffinity&)=delete;

    // Owning caller setup only, after every benchmark worker has been created.
    bool openNative(BenchmarkPlacement placement=BenchmarkPlacement::pinnedCaller) noexcept {
        if(attempted_)return reject(87);
#if defined(_WIN32)
        groupCount_=GetActiveProcessorGroupCount();
        if(groupCount_!=1){attempted_=true;topologyError_=50;return reject(topologyError_);}
        std::vector<BenchmarkCpuSet> cpus;std::uint32_t error=0;
        if(!readNativeTopology(cpus,error)){attempted_=true;topologyError_=error;return reject(error);}
        return open(nativeBenchmarkAffinityApi(),cpus,groupCount_,placement);
#else
        attempted_=true;topologyError_=50;return reject(topologyError_);
#endif
    }
    // The injected functions model only this caller's Get/SetThreadGroupAffinity.
    bool open(BenchmarkAffinityApi api,std::span<const BenchmarkCpuSet> cpus,unsigned groups,BenchmarkPlacement placement=BenchmarkPlacement::pinnedCaller) noexcept {
        if(attempted_)return reject(87);
        attempted_=true;api_=api;groupCount_=groups;
        if(!api_.get||!api_.set)return reject(87);
        if(groups!=1)return reject(50); // Exact restoration here is single-group only.
        try{topology_.assign(cpus.begin(),cpus.end());}catch(...){return reject(8);}
        std::uint32_t error=0;
        if(!api_.get(api_.context,before_,error))return reject(error?error:31);
        beforeOk_=true;
        if(before_.group!=0||before_.mask==0)return reject(13);
        bool found=false;
        for(std::size_t i=0;i<topology_.size();++i){const auto& cpu=topology_[i];
            if(cpu.group!=0||cpu.index>=64)return reject(13);
            for(std::size_t j=0;j<i;++j)if(cpu.id==topology_[j].id||cpu.index==topology_[j].index)return reject(13);
            // Binding wakes a parked core; stable placement must not follow transient parking.
            if(((cpu.index==0||(cpu.flags&1))&&placement!=BenchmarkPlacement::movableTimer&&placement!=BenchmarkPlacement::stableCaller)||((cpu.flags&2)&&!(cpu.flags&4)))continue;
            if(placement==BenchmarkPlacement::reservedCaller||placement==BenchmarkPlacement::stableCaller){
                bool reserved=false;
                for(const auto& host:topology_)if(host.index<2&&host.core==cpu.core)reserved=true;
                if(reserved)continue;
            }
            const auto bit=std::uint64_t(1)<<cpu.index;if(!(before_.mask&bit))continue;
            eligibleMask_|=bit;
            if(!found||cpu.efficiency>selected_.efficiency){selected_=cpu;performanceMask_=bit;found=true;}
            else if(cpu.efficiency==selected_.efficiency){performanceMask_|=bit;
                if(placement==BenchmarkPlacement::stableCaller?cpu.index>selected_.index:cpu.index<selected_.index)selected_=cpu;}
        }
        if(!found)return reject(1168);
        desired_={std::uint64_t(1)<<selected_.index,selected_.group};
        if(placement==BenchmarkPlacement::movableTimer){
            std::uint64_t physical=0;
            for(const auto& cpu:topology_){
                if(!(performanceMask_&(std::uint64_t(1)<<cpu.index)))continue;
                bool first=true;
                for(const auto& other:topology_)if(other.core==cpu.core&&other.index<cpu.index&&(performanceMask_&(std::uint64_t(1)<<other.index)))first=false;
                if(first)physical|=std::uint64_t(1)<<cpu.index;
            }
            if(std::popcount(physical)<4)return reject(50);
            // Keep four or more physical cores, leaving the first two for host/driver work when possible.
            if(std::popcount(physical)>=6){physical&=physical-1;physical&=physical-1;}
            desired_.mask=physical;
        }
        // Restore even if a failing injected/OS call changed state before reporting failure.
        restoreNeeded_=true;error=0;
        if(!api_.set(api_.context,desired_,error)){reject(error?error:31);(void)close();return false;}
        error=0;if(!api_.get(api_.context,during_,error)){reject(error?error:31);(void)close();return false;}
        duringOk_=true;
        if(during_!=desired_){reject(13);(void)close();return false;}
        opened_=true;return true;
    }
    // Outside timing, after MMCSS setup and before its cleanup: detect overrides.
    bool verifyCurrent() noexcept {
        if(!opened_||!restoreNeeded_)return reject(87);
        std::uint32_t error=0;BenchmarkGroupAffinity current{};
        if(!api_.get(api_.context,current,error))return reject(error?error:31);
        during_=current;duringOk_=true;
        return current==desired_?true:reject(13);
    }
    bool close() noexcept {
        if(!restoreNeeded_)return true;
        std::uint32_t error=0;
        if(!api_.set(api_.context,before_,error)){cleanup(error?error:31);return false;}
        error=0;if(!api_.get(api_.context,after_,error)){cleanup(error?error:31);return false;}
        afterOk_=true;
        if(after_!=before_){cleanup(13);return false;}
        restoreNeeded_=false;restored_=true;return true;
    }
    bool qualified() const noexcept {return attempted_&&opened_&&restored_&&beforeOk_&&duringOk_&&afterOk_&&!setupError_&&!cleanupError_&&!topologyError_&&!restoreNeeded_;}
    bool attempted() const noexcept{return attempted_;}
    bool opened() const noexcept{return opened_;}
    bool restored() const noexcept{return restored_;}
    bool beforeOk() const noexcept{return beforeOk_;}
    bool duringOk() const noexcept{return duringOk_;}
    bool afterOk() const noexcept{return afterOk_;}
    std::uint32_t setupError() const noexcept{return setupError_;}
    std::uint32_t cleanupError() const noexcept{return cleanupError_;}
    std::uint32_t topologyError() const noexcept{return topologyError_;}
    unsigned groupCount() const noexcept{return groupCount_;}
    BenchmarkGroupAffinity before() const noexcept{return before_;}
    BenchmarkGroupAffinity during() const noexcept{return during_;}
    BenchmarkGroupAffinity after() const noexcept{return after_;}
    BenchmarkCpuSet selected() const noexcept{return selected_;}
    std::uint64_t eligibleMask() const noexcept{return eligibleMask_;}
    std::uint64_t performanceMask() const noexcept{return performanceMask_;}
    std::span<const BenchmarkCpuSet> topology() const noexcept{return topology_;}
private:
    bool reject(std::uint32_t error) noexcept{if(!setupError_)setupError_=error;return false;}
    void cleanup(std::uint32_t error) noexcept{if(!cleanupError_)cleanupError_=error;}
#if defined(_WIN32)
    static bool readNativeTopology(std::vector<BenchmarkCpuSet>& output,std::uint32_t& error) noexcept try {
        ULONG needed=0;
        if(GetSystemCpuSetInformation(nullptr,0,&needed,GetCurrentProcess(),0)||GetLastError()!=ERROR_INSUFFICIENT_BUFFER||!needed){error=ERROR_INVALID_DATA;return false;}
        std::vector<std::uint64_t> storage;bool read=false;
        for(unsigned attempt=0;attempt<3&&!read;++attempt){
            storage.resize((std::uint64_t(needed)+7)/8);ULONG returned=0;
            if(storage.size()>std::numeric_limits<ULONG>::max()/8){error=ERROR_BAD_LENGTH;return false;}
            read=GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(storage.data()),ULONG(storage.size()*8),&returned,GetCurrentProcess(),0)!=FALSE;
            if(!read){error=GetLastError();if(error!=ERROR_INSUFFICIENT_BUFFER)return false;}
            needed=returned;
        }
        if(!read){error=ERROR_INSUFFICIENT_BUFFER;return false;}
        if(needed>storage.size()*8){error=ERROR_BAD_LENGTH;return false;}
        const auto* bytes=reinterpret_cast<const unsigned char*>(storage.data());
        for(ULONG offset=0;offset<needed;){
            if(needed-offset<sizeof(DWORD)*2){error=ERROR_BAD_LENGTH;return false;}
            const auto* entry=reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(bytes+offset);
            if(entry->Size<sizeof(DWORD)*2||entry->Size>needed-offset){error=ERROR_BAD_LENGTH;return false;}
            if(entry->Type==CpuSetInformation){
                if(entry->Size<sizeof(SYSTEM_CPU_SET_INFORMATION)){error=ERROR_BAD_LENGTH;return false;}
                const auto& cpu=entry->CpuSet;output.push_back({cpu.Id,cpu.Group,cpu.LogicalProcessorIndex,cpu.CoreIndex,cpu.EfficiencyClass,cpu.AllFlags});
            }
            offset+=entry->Size;
        }
        if(output.empty()){error=ERROR_NOT_FOUND;return false;}error=0;return true;
    }catch(...){error=ERROR_NOT_ENOUGH_MEMORY;return false;}
#endif
    BenchmarkAffinityApi api_{};
    std::vector<BenchmarkCpuSet> topology_;
    BenchmarkGroupAffinity before_{},during_{},after_{},desired_{};
    BenchmarkCpuSet selected_{};
    std::uint64_t eligibleMask_=0,performanceMask_=0;
    unsigned groupCount_=0;
    std::uint32_t setupError_=0,cleanupError_=0,topologyError_=0;
    bool attempted_=false,opened_=false,restored_=false,beforeOk_=false,duringOk_=false,afterOk_=false,restoreNeeded_=false;
};
}

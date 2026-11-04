#pragma once
#include "ModalService.h"
#include <optional>
namespace airack::space::probe {
struct ServiceCpuCase {std::uint32_t row=0,rate=0,block=0,factor=0,scenario=0;bool operator==(const ServiceCpuCase&) const=default;};
std::optional<ServiceCpuCase> serviceCpuCase(unsigned row) noexcept;
inline constexpr std::uint64_t serviceCpuMagic=0x3155504356525353ULL;
struct ServiceCpuHeader {
    std::uint64_t magic=serviceCpuMagic;std::uint32_t version=1,row=0,rate=0,block=0,factor=0,scenario=0;
    std::uint64_t warmExpected=0,measureExpected=0,ticksPerSecond=0,measureAnchor=0,warmStart=0,warmEnd=0,warmCompleted=0,recordCount=0,measureEnd=0,callbackAllocations=0,callbackFrees=0,productBytes=0;
};
struct ServiceCpuRecord {
    std::uint64_t release=0,start=0,beginFinish=0,finish=0,cycles=0,roomCopy=0,postAdmission=0,coldInstall=0,maximumRooms=0,allocations=0,frees=0,rawPeakBits=0,checksumBits=0,valid=0;
};
static_assert(sizeof(ServiceCpuHeader)==128&&sizeof(ServiceCpuRecord)==112);
struct ServiceCpuAssessment {
    bool schemaValid=false,complete=false,knownFailure=false,processingPass=false,workloadComplete=false;
    std::uint64_t overruns=0,dispatchMisses=0,roomCopies=0,postAdmissions=0,coldInstalls=0,nineRoomCallbacks=0;
    double meanSeconds=0,p99Seconds=0,maximumSeconds=0,copyP99Seconds=0,coldP99Seconds=0,startLatenessP99Seconds=0,releaseFinishP99Seconds=0;
};
ServiceCpuAssessment assessServiceCpu(const ServiceCpuHeader&,std::span<const ServiceCpuRecord>);
struct ServiceCpuObservation {bool valid=false;std::uint64_t roomCopy=0,postAdmission=0,coldInstall=0,maximumRooms=0;double peak=0,checksum=0;ModalServiceStatus status;};
class ServiceCpuWorkload {
public:
    ServiceCpuWorkload();~ServiceCpuWorkload();
    bool prepare(unsigned row,bool threaded=true);
    bool prepare(unsigned row,bool threaded,std::size_t modes,ModalConverterBackend converter=ModalConverterBackend::automatic);
    bool begin(std::uint64_t ordinal) noexcept;
    bool renderAndEnd() noexcept;
    ServiceCpuObservation observe() const noexcept;
    bool pump() noexcept;
    const ModalReadyBundle* prepared(unsigned bank) const noexcept;
    std::size_t storageBytes() const noexcept;
    bool usesPreparedCopies() const noexcept;
    bool usesBlockedScatter() const noexcept;
    bool usesPairedReduction() const noexcept;
    std::optional<ModalConverterBackend> converterBackend() const noexcept;
private:struct State;std::unique_ptr<State> state_;
};
}

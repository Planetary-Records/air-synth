#pragma once
#include "SpaceReverbModel.h"
#include "SpaceReverbPreparation.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
namespace airack::space::detail {
inline constexpr std::size_t tankLines=8,maximumTankStages=38;
struct TankCoefficients {
    double b0=1,b1=0,b2=0,a1=0,a2=0;
    bool operator==(const TankCoefficients&) const = default;
};
struct PreparedTankLine {
    std::uint8_t fitVariant=0;
    std::uint32_t length=0;
    double scalar=1;
    std::size_t count=0;
    std::array<TankCoefficients,maximumTankStages> coefficients {};
    bool operator==(const PreparedTankLine&) const = default;
};
struct PreparedDiffuser {
    std::uint32_t length=0;
    double attenuation=1;
    bool operator==(const PreparedDiffuser&) const = default;
};
struct TankAdmission {
    bool passed=false;
    double timeBound=46,maxLoopMagnitude=1,maxInternalPole=0;
    std::size_t intervals=0;
    bool operator==(const TankAdmission&) const = default;
};
struct PreparedTank {
    double sampleRate=0,size=.5;
    DecayCurve curve;
    std::array<PreparedTankLine,tankLines> lines {};
    std::array<std::array<PreparedDiffuser,4>,2> diffusion {};
    double maximumFitError=0,maximumNominalError=0;
    std::size_t stageCount=0;
    std::uint32_t warmLines=0;
    TankAdmission admission;
    bool operator==(const PreparedTank&) const = default;
};
enum class TankPreparationFailure {none,invalidInput,fitRejected,tailRejected,allocation,cancelled};
struct TankPreparationReport {
    TankPreparationFailure failure=TankPreparationFailure::none;
    std::uint32_t line=8;
    double maximumFitError=0,maximumNominalError=0;
    const char* detail="";
    double worstFrequencyHz=0,maximumPole=0,maximumMagnitude=1;
    bool stabilityCertified=false;
    std::uint32_t warmLines=0;
};
bool prepareTank(const DecayCurve&,double size,double physicalRate,PreparedTank&,TankPreparationReport* report=nullptr,PreparationControl ={}) noexcept;
enum class RenderBackend {scalar,sse2,automatic};
void rotateSpaceFeedback(std::array<double,8>&,double character,const std::array<double,4>& sinePhases) noexcept;
// One render owner. Only prepareStorage may allocate/free. Cached ISA dispatch.
class TankRuntime {
public:
    using Frame=std::array<double,2>;
    static constexpr std::size_t maximumBlockFrames=512;
    TankRuntime();~TankRuntime();
    TankRuntime(const TankRuntime&)=delete;
    TankRuntime& operator=(const TankRuntime&)=delete;
    TankRuntime(TankRuntime&&) noexcept;
    TankRuntime& operator=(TankRuntime&&) noexcept;
    bool prepareStorage(double physicalRate,RenderBackend=RenderBackend::automatic);
    bool configure(const PreparedTank&) noexcept;
    bool adoptCopied(const PreparedTank&,const TankRuntime& source) noexcept;
    void reset() noexcept;
    bool processBlock(std::span<Frame>,std::span<const double> character={}) noexcept;
    std::array<double,2> tick(std::array<double,2>) noexcept;
    std::array<double,2> tick(std::array<double,2>,double character) noexcept;
    const PreparedTank& prepared() const noexcept;
    std::size_t storageBytes() const noexcept;
    const char* backendName() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}

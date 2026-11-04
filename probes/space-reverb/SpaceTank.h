#pragma once
#include "LossFit.h"
#include "PackedLoss.h"
#include <array>
#include <vector>
namespace airack::space::probe
{
enum class TankGeometry { eight, sixteen };
enum class TankBackend {sse2,automatic,avx2};
class SpaceTank
{
public:
    using LineView=detail::LossLineView;
    bool prepare(double sampleRate,int maxBlock,const DecayCurve&,double size,FitGeometry,bool inputDiffusion=true,bool packed=true,TankGeometry tank=TankGeometry::eight,FitDomain domain=FitDomain::unitCircle,TankBackend backend=TankBackend::sse2);
    void reset() noexcept;
    std::array<double,2> tick(std::array<double,2>) noexcept;
    std::size_t preparedBytes() const noexcept;
    std::size_t lineCount() const noexcept {return ready_?lines_:0;}
    double sampleRate() const noexcept {return rate_;}
    double maximumFitError() const noexcept {return maximumFitError_;}
    double maximumNominalError() const noexcept {return maximumNominalError_;}
    const char* backendName() const noexcept {return !packed_?"scalar":accelerated_?"avx2":"sse2";}
    LineView inspectLine(std::size_t index) const noexcept;
private:
    struct Ring {std::vector<double> data;std::size_t cursor=0;double attenuation=1;};
    std::array<Ring,16> delays_;
    std::array<std::array<Ring,4>,2> diffusion_;
    std::array<LossBank,16> losses_;
    std::array<PackedLossStage,56> packedStages_;
    PackedLossKernel accelerated_=nullptr;
    std::size_t stageCount_=0;
    bool packed_=true;
    std::size_t lines_=8;
    double rate_=0;
    double maximumFitError_=0;
    double maximumNominalError_=0;
    bool ready_=false;
    bool diffuse_=true;
};
}


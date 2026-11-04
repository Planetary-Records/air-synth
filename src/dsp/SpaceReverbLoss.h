#pragma once
#include "SpaceReverbModel.h"
#include "SpaceReverbPreparation.h"
#include <vector>
#include <functional>
#include <limits>
namespace airack::space::detail
{
enum class FitGeometry { compact, dense };
enum class FitDomain {unitCircle,poleContour};
enum class FitVariant {original,edgeBandwidth};
struct Biquad
{
    double b0=1,b1=0,b2=0,a1=0,a2=0,z1=0,z2=0;
    double frequency=0,q=1,cutDb=0;
    int shape=0;
    double tick(double x) noexcept;
};
struct LossBank
{
    double scalar=1;
    std::vector<Biquad> stages;
    double tick(double x) noexcept;
};
struct LossLineView {double delaySeconds=0;const LossBank* loss=nullptr;};
struct LossNetwork {
    double sampleRate=48000;
    std::vector<LossLineView> lines;
    std::vector<double> mixing;
    std::function<void()> poll;
    PreparationControl control;
};
struct FitResult
{
    LossBank bank;
    double maxRelativeError=std::numeric_limits<double>::infinity();
    double rmsRelativeError=std::numeric_limits<double>::infinity();
    double maxPole=0,maxMagnitude=1;
    int iterations=0;
    bool converged=false;
    bool stabilityCertified=false;
    double certifiedMaxMagnitude=1;
    std::size_t certificateIntervals=0;
    FitDomain domain=FitDomain::unitCircle;
    double maxNominalRelativeError=std::numeric_limits<double>::infinity();
    double worstFrequencyHz=0;
    bool refinementTried=false,refinementCertified=false;
    double refinementMaximumError=std::numeric_limits<double>::infinity();
};
Biquad peakCut(double frequency,double q,double cutDb,double rate) noexcept;
Biquad shelfCut(double frequency,bool high,double cutDb,double rate) noexcept;
double poleRadius(const Biquad&) noexcept;
bool certifyLoss(const LossBank&,double sampleRate,double& maxMagnitude,std::size_t& intervals,double* rejectedHz=nullptr,PreparationControl ={});
double magnitude(const LossBank&,double frequencyHz,double sampleRate) noexcept;
double nominalTime(const LossBank&,double frequencyHz,double sampleRate,double delaySeconds) noexcept;
// These detail preparation functions propagate PreparationCancelled.
FitResult fitLoss(const DecayCurve&,double sampleRate,double delaySeconds,FitGeometry,FitDomain=FitDomain::unitCircle,FitVariant=FitVariant::original,PreparationControl ={});
FitResult fitLossWarm(const DecayCurve&,double sampleRate,double delaySeconds,double previousDelaySeconds,const LossBank&,PreparationControl ={});
}


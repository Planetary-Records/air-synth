#include "SpaceReverbModel.h"
#include <algorithm>
#include <cmath>

namespace airack::space
{
namespace
{
bool range(double x,double lo,double hi) noexcept
{return std::isfinite(x)&&x>=lo&&x<=hi;}
}
bool valid(const DecayCurve& c) noexcept
{
    if(!range(c.decaySeconds,.1,20)||!range(c.brightness,0,1))return false;
    for(const auto& b:c.bands)
        if((b.shape!=DecayShape::bell&&b.shape!=DecayShape::lowShelf&&b.shape!=DecayShape::highShelf)
            ||!range(b.frequencyHz,20,20000)||!range(b.multiplier,.25,4)||!range(b.q,.25,4))
            return false;
    return true;
}
std::optional<DecayPoint> evaluate(const DecayCurve& c,double requestedHz,double rate) noexcept
{
    if(!valid(c)||!range(rate,8000,384000)||!range(requestedHz,20,20000))return {};
    const double limit=.45*rate, f=std::min(requestedHz,limit);
    const double highWeight=f*f/(f*f+4000.*4000.);
    double logTime=std::log(c.decaySeconds)+(1-c.brightness)*std::log(.05)*highWeight;
    for(const auto& b:c.bands){
        if(!b.active||b.bypassed)continue;
        const double centre=std::min(b.frequencyHz,limit), ratio=f/centre;
        double weight=0;
        if(b.shape==DecayShape::bell){
            const double x=b.q*(ratio-1/ratio);weight=1/(1+x*x);
        }else{
            const double low=1/(1+std::pow(ratio,2*b.q));
            weight=b.shape==DecayShape::lowShelf?low:1-low;
        }
        logTime+=std::log(b.multiplier)*weight;
    }
    const double raw=std::exp(logTime), seconds=std::clamp(raw,.1,40.);
    return DecayPoint{f,seconds,raw,f!=requestedHz,raw<.1||raw>40};
}
}


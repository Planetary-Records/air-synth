#include "SpaceReverbLoss.h"
#include "SpaceReverbTail.h"
#include <cmath>
#include <iomanip>
#include <iostream>
using namespace airack::space;
int main(){
    int checks=0,failures=0;const auto check=[&](bool value){++checks;if(!value)++failures;};
    DecayCurve curve;curve.decaySeconds=20;curve.brightness=1;
    for(std::size_t i=0;i<6;++i)curve.bands[i]={true,false,DecayShape::bell,125*std::pow(2.,double(i)),i%2?4.:.25,4};
    for(double rate:{44100.,48000.,96000.}){
        std::size_t length=std::size_t(std::llround(.0193*1.25*rate));
        const auto prime=[](std::size_t n){for(std::size_t d=2;d*d<=n;++d)if(n%d==0)return false;return true;};while(!prime(length))++length;
        const double delay=double(length)/rate;
        const auto fit=detail::fitLoss(curve,rate,delay,detail::FitGeometry::compact,detail::FitDomain::poleContour,detail::FitVariant::edgeBandwidth);
        detail::LossNetwork network;network.sampleRate=rate;network.lines.push_back({delay,&fit.bank});network.mixing.push_back(1);
        const auto tail=detail::certifyTail(network,46);
        check(fit.converged);check(fit.maxRelativeError<=.1);check(fit.stabilityCertified);check(fit.bank.stages.size()<=38);check(tail.passed);
        std::cout<<std::setprecision(17)<<"LONG_DECAY rate="<<rate<<" error="<<fit.maxRelativeError<<" worst_hz="<<fit.worstFrequencyHz<<" passive="<<fit.stabilityCertified<<" tail="<<tail.passed<<std::endl;
    }
    std::cout<<"SpaceReverbLongDecay checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;
}

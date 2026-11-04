#include "SpaceReverbLoss.h"
#include "SpaceReverbTail.h"
#include <cmath>
#include <iomanip>
#include <iostream>
using namespace airack::space;
int main(){int checks=0,failures=0;const auto check=[&](bool value){++checks;if(!value)++failures;};
    DecayCurve c;c.brightness=1;std::array<double,6> f{20,80,320,1280,5120,20000};for(std::size_t i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,f[i],i%2?4.:.25,4};
    std::size_t length=std::size_t(std::llround(.0719*1.25*768000));const auto prime=[](std::size_t n){for(std::size_t d=2;d*d<=n;++d)if(n%d==0)return false;return true;};while(!prime(length))++length;
    const double delay=double(length)/768000;
    const auto fit=detail::fitLoss(c,768000,delay,detail::FitGeometry::compact,detail::FitDomain::poleContour,detail::FitVariant::edgeBandwidth);
    detail::LossNetwork network;network.sampleRate=768000;network.lines.push_back({delay,&fit.bank});network.mixing.push_back(1);const auto tail=detail::certifyTail(network,46);
    check(fit.converged);check(fit.maxRelativeError<=.1);check(fit.stabilityCertified);check(fit.bank.stages.size()<=38);check(tail.passed);
    std::cout<<std::setprecision(17)<<"ENDPOINT_BANK length="<<length<<" error="<<fit.maxRelativeError<<" worst_hz="<<fit.worstFrequencyHz<<" passive="<<fit.stabilityCertified<<" tail="<<tail.passed<<" tail_reason="<<tail.reason<<" refined="<<fit.refinementTried<<" refinement_error="<<fit.refinementMaximumError<<" refinement_passive="<<fit.refinementCertified<<'\n';
    std::cout<<"SpaceReverbEndpointBank checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

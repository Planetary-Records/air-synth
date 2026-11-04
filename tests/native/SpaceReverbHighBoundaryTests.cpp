#include "SpaceReverbLoss.h"
#include "SpaceReverbTail.h"
#include <cmath>
#include <iomanip>
#include <iostream>
using namespace airack::space;
int main(){
    int checks=0,failures=0;const auto check=[&](bool value){++checks;if(!value)++failures;};
    DecayCurve curve;curve.brightness=1;for(auto& band:curve.bands)band={true,false,DecayShape::bell,20000,.25,4};
    struct Case{double rate,size,lineSeconds;};
    for(const auto c:{Case{44100,1,.0371},Case{48000,.5,.0533},Case{48000,1,.0437},Case{96000,1,.0533},Case{192000,1,.0437}}){
        auto length=std::size_t(std::llround(c.lineSeconds*(.5+1.5*c.size)*c.rate));
        const auto prime=[](std::size_t n){for(std::size_t d=2;d*d<=n;++d)if(n%d==0)return false;return n>=2;};while(!prime(length))++length;
        const double delay=double(length)/c.rate;
        const auto fit=detail::fitLoss(curve,c.rate,delay,detail::FitGeometry::compact,detail::FitDomain::poleContour,detail::FitVariant::edgeBandwidth);
        detail::LossNetwork network;network.sampleRate=c.rate;network.lines.push_back({delay,&fit.bank});network.mixing.push_back(1);const auto tail=detail::certifyTail(network,46);
        check(fit.converged);check(fit.maxRelativeError<=.1);check(fit.stabilityCertified);check(fit.bank.stages.size()<=38);check(tail.passed);
        std::cout<<std::setprecision(17)<<"HIGH_BOUNDARY rate="<<c.rate<<" size="<<c.size<<" length="<<length<<" error="<<fit.maxRelativeError<<" worst_hz="<<fit.worstFrequencyHz<<" passive="<<fit.stabilityCertified<<" tail="<<tail.passed<<std::endl;
    }
    std::cout<<"SpaceReverbHighBoundary checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;
}

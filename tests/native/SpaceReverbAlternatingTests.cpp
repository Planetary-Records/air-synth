#include "SpaceReverbLoss.h"
#include "SpaceReverbTail.h"
#include <cmath>
#include <iomanip>
#include <iostream>
using namespace airack::space;
int main(){
    int checks=0,failures=0;const auto check=[&](bool value){++checks;if(!value)++failures;};
    DecayCurve curve;curve.brightness=1;
    constexpr std::array<double,6> frequencies{20,80,320,1280,5120,20000};
    for(std::size_t i=0;i<6;++i)curve.bands[i]={true,false,DecayShape::bell,frequencies[i],i%2?4.:.25,4};
    struct Case{double rate,size,lineSeconds;};
    for(const auto c:{Case{44100,0,.0371},Case{48000,0,.0719},Case{192000,0,.0617},Case{192000,.5,.0193}}){
        const int failuresBefore=failures;
        auto length=std::size_t(std::llround(c.lineSeconds*(.5+1.5*c.size)*c.rate));
        const auto prime=[](std::size_t n){for(std::size_t d=2;d*d<=n;++d)if(n%d==0)return false;return n>=2;};while(!prime(length))++length;
        const double delay=double(length)/c.rate;
        const auto fit=detail::fitLoss(curve,c.rate,delay,detail::FitGeometry::compact,detail::FitDomain::poleContour,detail::FitVariant::edgeBandwidth);
        detail::LossNetwork network;network.sampleRate=c.rate;network.lines.push_back({delay,&fit.bank});network.mixing.push_back(1);const auto tail=detail::certifyTail(network,46);
        check(fit.converged);check(fit.maxRelativeError<=.1);check(fit.stabilityCertified);check(fit.bank.stages.size()<=38);check(tail.passed);
        if(failures!=failuresBefore)std::cerr<<"ENDPOINT_FAIL alternating rate="<<c.rate<<" size="<<c.size<<" length="<<length<<std::endl;
        std::cout<<std::setprecision(17)<<"ALTERNATING rate="<<c.rate<<" size="<<c.size<<" length="<<length<<" error="<<fit.maxRelativeError<<" worst_hz="<<fit.worstFrequencyHz<<" passive="<<fit.stabilityCertified<<" tail="<<tail.passed<<std::endl;
    }
    std::cout<<"SpaceReverbAlternating checks="<<checks<<" failures="<<failures<<std::endl;return failures?1:0;
}

#include "SpaceReverbTail.h"
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>

using namespace airack::space::detail;
int main(){
    int checks=0,failures=0;const auto check=[&](bool value,const char* label){++checks;if(!value){++failures;std::cout<<"FAIL "<<label<<'\n';}};
    constexpr double rate=768000,time=46,delay=.03;
    const double radius=std::exp(-std::log(1000.)/(rate*time));
    // 80-digit Decimal evaluation of original binary64 coefficients and this
    // binary64 radius. Values are independent of the production wide arithmetic.
    constexpr std::array<double,3> dcDb{.9140631320859928,1.8789102546219865,3.9859706272004374};
    for(std::size_t i=0;i<dcDb.size();++i){
        const double p=1-std::ldexp(1.,-int(18+i));
        Biquad section;section.b0=(1-p)*(1-p);section.b1=section.b2=0;section.a1=-2*p;section.a2=p*p;
        const auto bound=boundSectionMagnitude(section,rate,radius);
        const double boundedDb=bound.maximumLog*20/std::log(10.);
        check(bound.passed,"section_prepared");check(boundedDb>=dcDb[i],"section_encloses_dc");check(boundedDb<=dcDb[i]+1.01e-6,"section_tight_margin");
        const double delayDb=-20*delay*rate*std::log10(radius);
        LossBank bank;bank.scalar=std::pow(10.,(.0001-delayDb-dcDb[i])/20);bank.stages.push_back(section);
        LossNetwork network;network.sampleRate=rate;network.lines.push_back({delay,&bank});network.mixing.push_back(1);
        const auto certificate=certifyTail(network,time);
        check(!certificate.passed,"above_unity_loop_rejected");
        std::cout<<std::setprecision(17)<<"CERTIFICATE_PRECISION bits="<<18+i<<" exact_dc_db="<<dcDb[i]<<" bounded_db="<<boundedDb<<" loop_pass="<<certificate.passed<<" loop_bound="<<certificate.maxLoopMagnitude<<'\n';
    }
    std::cout<<"SpaceReverbCertificatePrecision checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

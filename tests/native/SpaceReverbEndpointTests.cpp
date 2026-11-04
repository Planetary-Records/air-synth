#include "SpaceReverbTank.h"
#include <iostream>
#include <cmath>
using namespace airack::space;
int main() {
    int checks=0,failures=0;
    auto check=[&](DecayCurve c,double rate){detail::PreparedTank p;detail::TankPreparationReport r;const bool ok=detail::prepareTank(c,.5,rate,p,&r);++checks;
        if(!ok||!p.admission.passed||p.maximumFitError>.1||p.stageCount>38){++failures;std::cerr<<"ENDPOINT_FAIL test="<<checks<<" reason="<<r.detail<<" error="<<r.maximumFitError<<'\n';}};
    DecayCurve c;check(c,8000);
    c.brightness=1;c.bands[0]={true,false,DecayShape::bell,20,.25,4};check(c,48000);
    c.bands[0]={true,false,DecayShape::bell,20000,.25,4};check(c,44100);
    c.bands[0].multiplier=4;check(c,48000);
    c.bands[0]={true,false,DecayShape::lowShelf,20000,.25,4};check(c,48000);
    c.bands[0].multiplier=4;check(c,48000);
    c.bands[0]={true,false,DecayShape::highShelf,20000,.25,4};check(c,48000);
    for(std::size_t i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,20*std::pow(4.,double(i)),i%2?4.:.25,4};c.bands[5].frequencyHz=20000;check(c,768000);
    std::cout<<"SpaceReverbEndpoints checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

#include "SpaceReverbTank.h"
#include "ProbePeerGuard.h"
#include <iomanip>
#include <iostream>
#include <string_view>
using namespace airack::space;
int main(int argc,char** argv){const bool functional=argc==2&&std::string_view(argv[1])=="--functional";if(argc!=1&&!functional)return 2;probe::PeerGuard guard;const auto check=[&](){if(!functional)guard.force();};try{check();DecayCurve c;c.brightness=1;std::array<double,6> f{20,80,320,1280,5120,20000};for(std::size_t i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,f[i],i%2?4.:.25,4};
    detail::PreparedTank p;detail::TankPreparationReport r;const bool passed=detail::prepareTank(c,.5,768000,p,&r);check();
    std::cout<<std::setprecision(17)<<"ENDPOINT_DIAGNOSTIC pass="<<passed<<" line="<<r.line<<" reason="<<r.detail<<" max_error="<<r.maximumFitError<<" worst_hz="<<r.worstFrequencyHz<<" certified="<<r.stabilityCertified<<" pole="<<r.maximumPole<<" max_magnitude="<<r.maximumMagnitude<<'\n';
    return 0;
}catch(const probe::PeerObserved&){return 3;}}

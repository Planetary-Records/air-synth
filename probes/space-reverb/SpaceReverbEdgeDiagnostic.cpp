#include "SpaceReverbLoss.h"
#include "ProbePeerGuard.h"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <complex>
using namespace airack::space;
int main() {
    probe::PeerGuard guard;try{guard.force();
    for(int test=1;test<2;++test){DecayCurve c;c.brightness=1;double rate=test?768000:48000;
        c.bands[0]={true,false,DecayShape::bell,20,.25,4};
        if(test){std::array<double,6> f{20,80,320,1280,5120,20000};for(std::size_t i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,f[i],i%2?4.:.25,4};}
        std::size_t length=std::size_t(std::llround(.0193*1.25*rate));const auto prime=[](std::size_t n){for(std::size_t d=2;d*d<=n;++d)if(n%d==0)return false;return true;};while(!prime(length))++length;const double delay=double(length)/rate;
        const auto fit=detail::fitLoss(c,rate,delay,detail::FitGeometry::compact,detail::FitDomain::poleContour,detail::FitVariant::edgeBandwidth);
        std::cout<<std::setprecision(17)<<"FIT test="<<test<<" iterations="<<fit.iterations<<" stages="<<fit.bank.stages.size()<<" error="<<fit.maxRelativeError<<" rms="<<fit.rmsRelativeError<<" magnitude="<<fit.maxMagnitude<<" intervals="<<fit.certificateIntervals<<" certified="<<fit.stabilityCertified<<" pole="<<fit.maxPole<<'\n';
        std::vector<std::array<double,4>> rows;
        for(int i=0;i<769;++i){const double f=20*std::pow(1000.,i/768.),target=evaluate(c,f,std::min(rate,384000.))->seconds;
            const auto z=std::polar(std::exp(std::log(1000.)/(rate*target)),-2*std::numbers::pi*f/rate);std::complex<double> h=fit.bank.scalar;
            for(const auto& s:fit.bank.stages)h*=(s.b0+s.b1*z+s.b2*z*z)/(1.+s.a1*z+s.a2*z*z);
            const double actual=-60*delay/(20*std::log10(std::abs(h)));rows.push_back({std::abs(actual/target-1),f,target,actual});}
        std::sort(rows.begin(),rows.end(),[](auto a,auto b){return a[0]>b[0];});for(std::size_t i=0;i<12;++i)std::cout<<"RESIDUAL test="<<test<<" error="<<rows[i][0]<<" f="<<rows[i][1]<<" target="<<rows[i][2]<<" actual="<<rows[i][3]<<'\n';
        double maximum=detail::magnitude(fit.bank,0,rate),maximumHz=0;
        for(int i=0;i<16385;++i){const double f=std::pow(rate*.5,i/16384.);const double m=detail::magnitude(fit.bank,f,rate);if(m>maximum){maximum=m;maximumHz=f;}}
        std::cout<<"UNIT_CIRCLE maximum="<<maximum<<" f="<<maximumHz<<'\n';
        for(const auto& s:fit.bank.stages)if(s.frequency<100)std::cout<<"LOW_STAGE f="<<s.frequency<<" q="<<s.q<<" cut="<<s.cutDb<<" pole="<<detail::poleRadius(s)<<'\n';guard.force();
    }
    }catch(const probe::PeerObserved&){return 3;}return 0;
}

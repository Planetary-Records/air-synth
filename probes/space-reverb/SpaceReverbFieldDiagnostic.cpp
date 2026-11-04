#include "SpaceReverbField.h"
#include "SpaceReverbLoss.h"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <complex>
#include <numbers>
#include <algorithm>
#include <vector>
using namespace airack::space;
int main(int argc,char** argv) {
    // Functional diagnosis only; the invoking controller owns CI admission.
    if(argc==2&&std::string_view(argv[1])=="--storage-functional"){
        int failed=0;
        for(double rate:{8000.,16000.,44100.,48000.,88200.,96000.,176400.,192000.,384000.,385000.,768000.}){
            FieldRuntime first,second;const bool ready=first.prepareStorage(rate)&&second.prepareStorage(rate);
            const auto bytes=first.storageBytes()+second.storageBytes();const bool pass=ready&&bytes<=16*1024*1024;
            if(!pass)++failed;
            std::cout<<"FIELD_STORAGE rate="<<rate<<" two_runtime_bytes="<<bytes<<" pass="<<pass<<std::endl;
        }
        std::cout<<"FieldStorage rows=11 failures="<<failed<<std::endl;return failed?1:0;
    }
    const bool coincident=argc==2&&std::string_view(argv[1])=="--coincident-functional";
    const bool bank96=argc==2&&std::string_view(argv[1])=="--bank96-functional";
    const bool bank=coincident||bank96||(argc==2&&std::string_view(argv[1])=="--bank-functional");
    if(argc!=2||(!bank&&std::string_view(argv[1])!="--functional"))return 2;
    for(double rate:{44100.,48000.,96000.}) {
        if(bank96&&rate!=96000)continue;
        if(coincident&&rate!=48000)continue;
        Settings s;s.predelayMs=250;s.character=1;s.distance=.5;s.width=2;s.mix=1;
        s.decay.decaySeconds=20;s.decay.brightness=1;
        for(std::size_t i=0;i<6;++i) {
            s.decay.bands[i]={true,false,DecayShape::bell,125*std::pow(2.,double(i)),i%2?4.:.25,4};
            s.post[i]={true,false,PostShape::bell,1000,18,12,4,CutSlope::db24};
        }
        if(coincident){s.decay.decaySeconds=2;for(auto& band:s.decay.bands)band={true,false,DecayShape::bell,20,.25,4};}
        if(bank){
            std::size_t length=std::size_t(std::llround(.0193*1.25*rate));
            const auto prime=[](std::size_t n){for(std::size_t d=2;d*d<=n;++d)if(n%d==0)return false;return true;};while(!prime(length))++length;
            const auto fit=detail::fitLoss(s.decay,rate,double(length)/rate,detail::FitGeometry::compact,detail::FitDomain::poleContour,detail::FitVariant::edgeBandwidth);
            std::cout<<std::setprecision(17)<<"FIELD_BANK rate="<<rate<<" pass="<<fit.converged<<" error="<<fit.maxRelativeError<<" worst_hz="<<fit.worstFrequencyHz<<" passive="<<fit.stabilityCertified<<" scalar="<<fit.bank.scalar<<std::endl;
            for(const auto& section:fit.bank.stages)std::cout<<"FIELD_STAGE f="<<section.frequency<<" q="<<section.q<<" cut="<<section.cutDb<<" shape="<<section.shape<<" pole_time="<<-std::log(1000.)/(rate*std::log(detail::poleRadius(section)))<<std::endl;
            std::vector<std::array<double,4>> residuals;
            const double upper=std::min(20000.,.45*rate);
            for(int i=0;i<769;++i){const double f=20*std::pow(upper/20,i/768.),target=evaluate(s.decay,f,rate)->seconds;
                const auto z=std::polar(std::exp(std::log(1000.)/(rate*target)),-2*std::numbers::pi*f/rate);std::complex<double> h=fit.bank.scalar;
                for(const auto& section:fit.bank.stages)h*=(section.b0+section.b1*z+section.b2*z*z)/(1.+section.a1*z+section.a2*z*z);
                const double actual=-60*double(length)/rate/(20*std::log10(std::abs(h)));residuals.push_back({std::abs(actual/target-1),f,target,actual});}
            std::sort(residuals.begin(),residuals.end(),[](const auto& a,const auto& b){return a[0]>b[0];});
            for(std::size_t i=0;i<16;++i)std::cout<<"FIELD_RESIDUAL error="<<residuals[i][0]<<" f="<<residuals[i][1]<<" target="<<residuals[i][2]<<" actual="<<residuals[i][3]<<std::endl;
            break;
        }
        PreparedField p;FieldPreparationReport r;
        const bool pass=prepareField(s,rate,p,&r);
        std::cout<<std::setprecision(17)<<"FIELD_DIAGNOSTIC rate="<<rate<<" pass="<<pass<<" reason="<<r.reason
                 <<" tank_reason="<<r.tank.detail<<" line="<<r.tank.line<<" error="<<r.tank.maximumFitError
                 <<" worst_hz="<<r.tank.worstFrequencyHz<<" passive="<<r.tank.stabilityCertified<<" warm_lines="<<r.tank.warmLines<<std::endl;
    }
    return 0;
}

// Development-only inspection of the exact unseeded helper and bounded warm
// paths. Compile the owned implementation once in this probe translation unit;
// this target never links the product Loss library or enters shipped DSP.
#include "SpaceReverbLoss.cpp"
#include "SpaceReverbTail.h"
#include <iomanip>
#include <iostream>
using namespace airack::space;
namespace d=airack::space::detail;
int main(){
    constexpr double rate=44100,delay=821./rate;
    DecayCurve curve;curve.brightness=1;
    constexpr std::array<double,6> frequencies{20,80,320,1280,5120,20000};
    for(std::size_t i=0;i<6;++i)curve.bands[i]={true,false,DecayShape::bell,frequencies[i],i%2?4.:.25,4};
    auto referenceLength=std::size_t(std::llround(.0386*rate));
    const auto prime=[](std::size_t n){for(std::size_t k=2;k*k<=n;++k)if(n%k==0)return false;return n>=2;};while(!prime(referenceLength))++referenceLength;
    std::cout<<std::setprecision(17);
    const auto report=[&](const char* phase,double anchor,double requested,const d::FitResult& fit){
        d::LossNetwork network;network.sampleRate=rate;network.lines.push_back({requested,&fit.bank});network.mixing.push_back(1);
        const auto tail=d::certifyTail(network,46);
        std::cout<<"DELAY_DIAGNOSTIC phase="<<phase<<" anchor="<<anchor<<" requested="<<requested<<" converged="<<fit.converged<<" error="<<fit.maxRelativeError<<" passive="<<fit.stabilityCertified<<" tail="<<tail.passed<<" worst_hz="<<fit.worstFrequencyHz<<" stages="<<fit.bank.stages.size()<<std::endl;
    };
    const auto initial=[&](double anchor,const d::LossBank& source){
        double longest=.1;const double upper=std::min(20000.,.45*rate);
        for(std::size_t j=0;j<769;++j)longest=std::max(longest,evaluate(curve,20*std::pow(upper/20,double(j)/768),rate)->seconds);
        for(const auto& band:curve.bands)if(band.active&&!band.bypassed)longest=std::max(longest,evaluate(curve,std::min(band.frequencyHz,upper),rate)->seconds);
        d::LossBank seed;seed.scalar=std::pow(10.,-(60*delay/longest)/20);
        bool poles=true;const double poleLimit=std::exp(-std::log(1000.)/(rate*45));
        for(const auto& s:source.stages){const double cut=s.cutDb*delay/anchor;
            const auto section=s.shape==0?d::peakCut(s.frequency,s.q,cut,rate):d::shelfCut(s.frequency,s.shape==2,cut,rate);
            poles=poles&&d::poleRadius(section)<poleLimit;seed.stages.push_back(section);}
        double minimum=std::numeric_limits<double>::infinity(),worstHz=0;std::size_t nonpositive=0;
        for(std::size_t j=0;j<769;++j){const double hz=20*std::pow(upper/20,double(j)/768),time=evaluate(curve,hz,rate)->seconds;
            const auto z=std::polar(std::exp(std::log(1000.)/(rate*time)),-2*std::numbers::pi*hz/rate);std::complex<double> h=seed.scalar;
            for(const auto& c:seed.stages)h*=(c.b0+c.b1*z+c.b2*z*z)/(1.+c.a1*z+c.a2*z*z);
            const double loss=-20*std::log10(std::abs(h));if(loss<=0)++nonpositive;if(loss<minimum){minimum=loss;worstHz=hz;}}
        std::cout<<"INITIAL_SEED anchor="<<anchor<<" requested="<<delay<<" minimum_loss_db="<<minimum<<" worst_hz="<<worstHz<<" nonpositive_points="<<nonpositive<<" poles_already_inside_45s="<<poles<<" zero_amount_loss_db="<<-20*std::log10(seed.scalar)<<std::endl;
    };
    for(double anchor:{2*delay,4*delay,double(referenceLength)/rate})for(const auto variant:{d::FitVariant::original,d::FitVariant::edgeBandwidth}){
        const auto seed=d::fitLossCold(curve,rate,anchor,d::FitGeometry::compact,d::FitDomain::poleContour,variant,{});
        report(variant==d::FitVariant::original?"original_anchor":"edge_anchor",anchor,anchor,seed);
        if(!seed.converged)continue;
        initial(anchor,seed.bank);
        const auto direct=d::fitLossWarm(curve,rate,delay,anchor,seed.bank,{});report("direct_warm",anchor,delay,direct);
        const double intermediate=std::sqrt(anchor*delay);
        const auto middle=d::fitLossWarm(curve,rate,intermediate,anchor,seed.bank,{});report("middle_warm",anchor,intermediate,middle);
        if(middle.converged){const auto final=d::fitLossWarm(curve,rate,delay,intermediate,middle.bank,{});report("two_step_warm",anchor,delay,final);}
    }
    std::cout<<"DELAY_DIAGNOSTIC complete=1 acceptance=0"<<std::endl;return 0;
}

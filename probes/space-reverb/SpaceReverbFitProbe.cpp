#include "LossFit.h"
#include "ProbeCases.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <numbers>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

using namespace airack::space;
using namespace airack::space::probe;
namespace
{
int checks=0,failures=0;
void check(bool ok,const char* text,int line){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<line<<" "<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
bool near(double a,double b,double e=1e-6){return std::abs(a-b)<=e*std::max(1.,std::abs(b));}
DecayCurve flat(){DecayCurve c;c.brightness=1;return c;}
void selfTest()
{
    LossBank test;test.stages.push_back(peakCut(1000,1,6,48000));
    CHECK(near(magnitude(test,1000,48000),.5011872336272722));
    CHECK(poleRadius(test.stages[0])<1);
    test.stages[0]=peakCut(1000,1,24,48000);
    CHECK(near(magnitude(test,1000,48000),.06309573444801933));
    test.stages[0]=shelfCut(1000,false,6,48000);
    CHECK(near(magnitude(test,0,48000),.5011872336272722));
    CHECK(near(magnitude(test,24000,48000),1));
    test.stages[0]=shelfCut(1000,true,6,48000);
    CHECK(near(magnitude(test,0,48000),1));
    CHECK(near(magnitude(test,24000,48000),.5011872336272722));
    test.stages[0]=peakCut(1000,1,-6,48000);
    CHECK(near(magnitude(test,1000,48000),1.9952623149688795));
    double certified=1;std::size_t intervals=0;
    CHECK(!certifyLoss(test,48000,certified,intervals));
    test.scalar=.4;
    CHECK(certifyLoss(test,48000,certified,intervals));
    CHECK(certified<1&&intervals>0);
    auto c=flat();auto f=fitLoss(c,48000,.037,FitGeometry::dense);
    CHECK(f.converged);CHECK(near(nominalTime(f.bank,1000,48000,.037),2,1e-4));
    c.bands[0]={true,false,DecayShape::bell,1000,4,1};
    f=fitLoss(c,48000,.037,FitGeometry::dense);
    CHECK(f.converged);
    CHECK(nominalTime(f.bank,1000,48000,.037)>4);
    CHECK(f.maxRelativeError<=.1);
    CHECK(f.bank.stages.size()<=56);
    CHECK(f.bank.scalar>0&&f.bank.scalar<1);
    CHECK(f.stabilityCertified&&f.certifiedMaxMagnitude<1);
    for(auto& s:f.bank.stages)CHECK(poleRadius(s)<1);
    for(int i=0;i<20000;++i)CHECK(std::isfinite(f.bank.tick(i==0?1:0)));
    c.bands[0].bypassed=true;
    f=fitLoss(c,48000,.037,FitGeometry::dense);
    CHECK(f.converged);CHECK(near(nominalTime(f.bank,1000,48000,.037),2,1e-4));
    c.bands[0].q=0;CHECK(!fitLoss(c,48000,.037,FitGeometry::dense).converged);
    CHECK(!fitLoss(flat(),0,.037,FitGeometry::dense).converged);
    CHECK(!fitLoss(flat(),48000,0,FitGeometry::dense).converged);
    c=flat();for(auto& b:c.bands)b={true,false,DecayShape::bell,1000,4,4};
    f=fitLoss(c,48000,.037,FitGeometry::compact);
    CHECK(f.converged&&f.maxRelativeError<=.1);
    f=fitLoss(c,44100,.1438095238095238,FitGeometry::compact);
    CHECK(f.converged&&f.maxRelativeError<=.1);
    c=flat();c.brightness=0;
    f=fitLoss(c,44100,.1438095238095238,FitGeometry::compact);
    CHECK(f.converged&&f.maxRelativeError<=.1);
    std::cout<<"LossFit checks="<<checks<<" failures="<<failures<<'\n';
}
int compare(bool smoke)
{
    const auto curves=corpus();
    std::cout<<"CORPUS curves="<<curves.size()<<" geometries=2 rates=44100,48000,96000 sizes=0,.5,1 lines=8 grid=769\n";
    std::cout<<"geometry,curve,rate,size,line,delay,stages,iterations,max_relative_error,rms_relative_error,max_pole,max_magnitude,certified_upper,certificate_intervals,pass\n";
    constexpr std::array<double,8> seconds{.0193,.0239,.0297,.0371,.0437,.0533,.0617,.0719};
    int count=0,bad=0;
    for(auto geometry:{FitGeometry::compact,FitGeometry::dense})
    for(const auto& row:curves)
    for(double rate:{44100.,48000.,96000.})
    for(double size:{0.,.5,1.})
    for(size_t line=0;line<8;++line){
        if(smoke&&(rate!=48000||size!=.5||line!=3))continue;
        const double delay=std::round(seconds[line]*(.5+1.5*size)*rate)/rate;
        const auto f=fitLoss(row.curve,rate,delay,geometry);
        const bool pass=f.converged&&f.maxRelativeError<=(row.isFlat?.05:.1);
        ++count;if(!pass)++bad;
        std::cout<<std::setprecision(12)<<(geometry==FitGeometry::compact?"compact":"dense")<<','<<row.name<<','<<rate<<','<<size<<','<<line<<','<<delay<<','<<f.bank.stages.size()<<','<<f.iterations<<','<<f.maxRelativeError<<','<<f.rmsRelativeError<<','<<f.maxPole<<','<<f.maxMagnitude<<','<<f.certifiedMaxMagnitude<<','<<f.certificateIntervals<<','<<pass<<'\n';
        std::cout.flush();
    }
    std::cout<<"FIT_SUMMARY rows="<<count<<" failures="<<bad<<"\n";return bad?1:0;
}
}
int main(int argc,char**argv)
{
    const std::string_view arg=argc>1?argv[1]:"--self-test";
    if(arg=="--self-test"){selfTest();return failures?1:0;}
    if(arg=="--compare")return compare(false);
    if(arg=="--smoke")return compare(true);
    if(arg=="--debug-overlap"){
        auto c=flat();for(auto& b:c.bands)b={true,false,DecayShape::bell,1000,4,4};
        const auto fit=fitLoss(c,48000,.037,FitGeometry::dense);
        std::vector<std::array<double,4>> points;
        for(int i=0;i<769;++i){const double hz=20*std::pow(1000.,i/768.);const double wanted=evaluate(c,hz,48000)->seconds;
            const double actual=nominalTime(fit.bank,hz,48000,.037);points.push_back({std::abs(actual/wanted-1),hz,wanted,actual});}
        std::sort(points.begin(),points.end(),[](auto a,auto b){return a[0]>b[0];});
        std::cout<<std::setprecision(14);for(int i=0;i<12;++i)for(auto x:points[size_t(i)])std::cout<<x<<' ';
        std::cout<<'\n';return 0;
    }
    if(arg=="--phase-short"){
        auto c=flat();c.bands[0]={true,false,DecayShape::bell,1000,.25,1};
        for(double line:{.0193,.0239,.0297,.0371,.0437,.0533,.0617,.0719}){
            const double delay=std::round(line*1.25*48000)/48000;
            const auto fit=fitLoss(c,48000,delay,FitGeometry::compact);
            auto h=[&](double hz){const auto z=std::polar(1.,-2*std::numbers::pi*hz/48000);auto value=std::complex<double>(fit.bank.scalar);
                for(const auto& s:fit.bank.stages)value*=(s.b0+s.b1*z+s.b2*z*z)/(1.+s.a1*z+s.a2*z*z);return value;};
            for(double hz:{500.,1000.}){
                const double gd=-std::arg(h(hz+.001)*std::conj(h(hz-.001)))/(4*std::numbers::pi*.001);
                const double t=nominalTime(fit.bank,hz,48000,delay);
                std::cout<<"PHASE f="<<hz<<" delay="<<delay<<" nominal="<<t<<" gd="<<gd<<" first_order_time="<<t*(1+gd/delay)<<'\n';
            }
        }return 0;
    }
    return 2;
}


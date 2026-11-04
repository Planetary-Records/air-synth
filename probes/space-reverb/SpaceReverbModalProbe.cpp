#include "ModalAnalysis.h"
#include "LossTail.h"
#include "ProbeCases.h"
#include "ProbePeerGuard.h"
#include <algorithm>
#include <cmath>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <string_view>
namespace {
using namespace airack::space;
using namespace airack::space::probe;
int checks=0,failures=0;
void check(bool good,const char* text,int line){++checks;if(!good){++failures;std::cerr<<"FAIL "<<line<<" "<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void tailTest()
{
    LossBank a,b;a.scalar=std::exp(-std::log(1000.)*.01/2);
    ModalNetwork one{48000,{{.01,&a}},{1}};
    CHECK(certifyTail(one,2.3).passed);CHECK(!certifyTail(one,1.8).passed);
    b.scalar=std::exp(-std::log(1000.)*.013/2);const double norm=1/std::sqrt(2.);
    ModalNetwork two{48000,{{.01,&a},{.013,&b}},{norm,norm,norm,-norm}};
    CHECK(certifyTail(two,2.3).passed);CHECK(!certifyTail(two,1.8).passed);
    one.mixing={2};CHECK(!certifyTail(one,46).passed);one.mixing={1};
    for(double frequency:{20.,1234.}){
        a.stages={peakCut(frequency,12,-6,48000)};a.scalar=.45;CHECK(certifyTail(one,46).passed);
        a.scalar=.55;CHECK(!certifyTail(one,46).passed);
    }
    a.stages={peakCut(20,24,-80,48000)};a.scalar=1e-5;CHECK(!certifyTail(one,46).passed);
}
void coverageTest()
{
    LossBank a,b;a.scalar=std::exp(-std::log(1000.)*.01/.5);
    ModalNetwork one{48000,{{.01,&a}},{1}};
    const auto first=countModes(one,-100,1e-6,175,325);CHECK(first.converged&&first.roots==2);
    a.scalar=std::exp(-std::log(1000.)*.01/1.5);b.scalar=std::exp(-std::log(1000.)*.013/1.5);
    ModalNetwork two{48000,{{.01,&a},{.013,&b}},{1,0,0,1}};
    const auto diagonal=countModes(two,-100,1e-6,175,325);CHECK(diagonal.converged&&diagonal.roots==4);
    const double norm=1/std::sqrt(2.);two.mixing={norm,norm,norm,-norm};
    const auto coupled=countModes(two,-100,1e-6,175,325);CHECK(coupled.converged&&coupled.roots==4);
    one.mixing={0};const auto zero=countModes(one,-100,1e-6,175,325);CHECK(zero.converged&&zero.roots==0);
    a.stages.push_back(peakCut(250,4,6,48000));
    const auto storage=countModes(one,-1000,1e-6,175,325);CHECK(storage.converged&&storage.roots==1);
    std::cout<<"COUNT_SELF counts="<<first.roots<<','<<diagonal.roots<<','<<coupled.roots<<','<<zero.roots<<','<<storage.roots<<" samples="<<first.samples<<','<<diagonal.samples<<','<<coupled.samples<<','<<zero.samples<<','<<storage.samples<<'\n';
}
void selfTest()
{
    LossBank a,b;
    a.scalar=std::exp(-std::log(1000.)*.01/.5);
    ModalNetwork one{48000,{{.01,&a}},{1}};
    const auto first=searchModes(one,175,325,[](double){return .5;});
    CHECK(first.modes.size()==2);
    if(first.modes.size()==2){CHECK(std::abs(first.modes[0].frequency-200)<1e-6);CHECK(std::abs(first.modes[1].frequency-300)<1e-6);}
    for(const auto& mode:first.modes){CHECK(std::abs(mode.time/.5-1)<1e-7);CHECK(mode.residual<1e-9);}
    a.scalar=std::exp(-std::log(1000.)*.01/1.5);b.scalar=std::exp(-std::log(1000.)*.013/1.5);
    ModalNetwork two{48000,{{.01,&a},{.013,&b}},{1,0,0,1}};
    const auto diagonal=searchModes(two,175,325,[](double){return 1.5;});CHECK(diagonal.modes.size()==4);
    for(const auto& mode:diagonal.modes){CHECK(std::abs(mode.time/1.5-1)<1e-7);CHECK(mode.residual<1e-9);}
    const double norm=1/std::sqrt(2.);two.mixing={norm,norm,norm,-norm};
    const auto coupled=searchModes(two,175,325,[](double){return 1.5;});CHECK(coupled.modes.size()>=3);
    for(const auto& mode:coupled.modes){CHECK(std::abs(mode.time/1.5-1)<1e-7);CHECK(mode.residual<1e-9);}
    coverageTest();
    tailTest();
    std::cout<<"MODAL_SELF checks="<<checks<<" failures="<<failures<<" counts="<<first.modes.size()<<','<<diagonal.modes.size()<<','<<coupled.modes.size()<<'\n';
}
void backendTest()
{
    auto c=flatForCorpus();c.bands[0]={true,false,DecayShape::bell,1000,.25,1};
    SpaceTank tank;CHECK(tank.prepare(48000,512,c,.5,FitGeometry::compact));
    auto network=inspectNetwork(tank),reference=network;reference.logarithmicReference=true;
    for(double centre:{500.,1000.}){
        auto target=[&](double f){return evaluate(c,f,48000)->seconds;};
        const auto first=searchModes(network,.95*centre,1.05*centre,target),second=searchModes(reference,.95*centre,1.05*centre,target);
        CHECK(!first.modes.empty()&&first.modes.size()==second.modes.size());
        if(first.modes.size()==second.modes.size())for(std::size_t i=0;i<first.modes.size();++i){
            CHECK(std::abs(first.modes[i].frequency/second.modes[i].frequency-1)<1e-7);
            CHECK(std::abs(first.modes[i].time/second.modes[i].time-1)<1e-7);
        }
        std::cout<<"BACKEND centre="<<centre<<" primary="<<first.modes.size()<<" reference="<<second.modes.size()<<'\n';
    }
    std::cout<<"BACKEND_SELF checks="<<checks<<" failures="<<failures<<'\n';
}
int tailSmoke()
{
    const auto cases=corpus();int bad=0;
    for(std::size_t row:{std::size_t(5),std::size_t(13),std::size_t(14),std::size_t(36)}){
        SpaceTank tank;if(!tank.prepare(48000,512,cases[row].curve,.5,FitGeometry::compact)){++bad;continue;}
        const auto result=certifyTail(inspectNetwork(tank),46);if(!result.passed)++bad;
        std::cout<<std::setprecision(14)<<"TAIL_SMOKE case="<<cases[row].name<<" bound="<<result.timeBound<<" radius="<<result.radius<<" max_internal_pole="<<result.maxInternalPole<<" max_loop="<<result.maxLoopMagnitude<<" intervals="<<result.intervals<<" pass="<<result.passed<<'\n';
    }
    return bad?1:0;
}
void poleFitTest()
{
    const auto cases=corpus();
    const std::array<std::array<double,3>,3> settings{{{44100,1,125},{48000,.5,125},{48000,.5,1000}}};
    for(const auto& setting:settings){const auto curve=cases[setting[2]==125?11:13].curve;
        SpaceTank tank;const bool ready=tank.prepare(setting[0],512,curve,setting[1],FitGeometry::compact,true,true,TankGeometry::eight,FitDomain::poleContour);CHECK(ready);
        if(!ready)continue;const auto network=inspectNetwork(tank);double total=0;for(const auto& line:network.lines)total+=line.delaySeconds;
        const double centre=setting[2],half=std::max(2/total,centre/24),low=centre-half,high=centre+half;
        double minimum=40;for(int i=0;i<=128;++i)minimum=std::min(minimum,evaluate(curve,low+(high-low)*i/128.,setting[0])->seconds);
        const double left=-2*std::log(1000.)/minimum;
        const auto modes=searchModes(network,low,high,[&](double f){return evaluate(curve,f,setting[0])->seconds;});
        const auto count=countModes(network,left,1e-6,low,high);std::size_t inside=0;double error=0;
        for(const auto& mode:modes.modes)if(mode.s.real()>=left&&mode.s.real()<=1e-6){++inside;error=std::max(error,std::abs(mode.time/evaluate(curve,mode.frequency,setting[0])->seconds-1));}
        CHECK(count.converged&&inside>0&&inside==count.roots);CHECK(error<=.10);CHECK(certifyTail(network,46).passed);
        std::cout<<"POLE_FIT rate="<<setting[0]<<" size="<<setting[1]<<" centre="<<centre<<" modes="<<inside<<" counted="<<count.roots<<" error="<<error<<" fit_error="<<tank.maximumFitError()<<" nominal_error="<<tank.maximumNominalError()<<'\n';
    }
    auto flat=flatForCorpus();SpaceTank mutant;CHECK(mutant.prepare(48000,512,flat,.5,FitGeometry::compact));
    std::array<Biquad,2> outputEq{peakCut(1000,1,-6,48000),peakCut(1000,1,-6,48000)};
    double originalEnergy=0,equalizedEnergy=0;
    for(std::size_t n=0;n<120000;++n){const auto wet=mutant.tick(n==0?std::array<double,2>{.5,.25}:std::array<double,2>{0,0});
        for(std::size_t ch=0;ch<2;++ch){originalEnergy+=wet[ch]*wet[ch];const double eq=outputEq[ch].tick(wet[ch]);equalizedEnergy+=eq*eq;}}
    CHECK(originalEnergy>0&&equalizedEnergy>originalEnergy*1.001);
    auto displayed=flat;displayed.bands[0]={true,false,DecayShape::bell,1000,4,1};
    const auto modes=searchModes(inspectNetwork(mutant),975,1025,[&](double){return 2.;});
    CHECK(!modes.modes.empty());bool rejects=true;
    for(const auto& mode:modes.modes)rejects=rejects&&std::abs(mode.time/evaluate(displayed,mode.frequency,48000)->seconds-1)>.5;
    CHECK(rejects);
    std::cout<<"POLE_FIT_SELF checks="<<checks<<" failures="<<failures<<'\n';
}
int inspect(bool flat,bool covered=false)
{
    auto c=flatForCorpus();if(flat)c.decaySeconds=1.5;else c.bands[0]={true,false,DecayShape::bell,1000,.25,1};
    SpaceTank tank;if(!tank.prepare(48000,512,c,.5,FitGeometry::compact))return 2;
    const auto network=inspectNetwork(tank);int bad=0;
    std::cout<<"frequency,target,pole_time,relative_error,null_residual,real_s\n";
    for(double centre:{500.,1000.}){
        double total=0;for(const auto& line:network.lines)total+=line.delaySeconds;
        const double half=covered?std::max(2/total,centre/24):.05*centre,low=centre-half,high=centre+half;
        double minimum=40;for(int i=0;i<=128;++i)minimum=std::min(minimum,evaluate(c,low+(high-low)*i/128.,48000)->seconds);
        const double left=-2*std::log(1000.)/minimum,right=1e-6;
        const auto result=searchModes(network,low,high,[&](double f){return evaluate(c,f,48000)->seconds;});
        std::size_t inside=0;
        for(const auto& mode:result.modes){const bool admitted=!covered||(mode.s.real()>=left&&mode.s.real()<=right);if(admitted)++inside;
            const double target=evaluate(c,mode.frequency,48000)->seconds,error=std::abs(mode.time/target-1);if(admitted&&error>.15)++bad;
            std::cout<<std::setprecision(13)<<mode.frequency<<','<<target<<','<<mode.time<<','<<error<<','<<mode.residual<<','<<mode.s.real()<<'\n';}
        std::cout<<"MODAL_SEARCH centre="<<centre<<" modes="<<result.modes.size()<<" seeds="<<result.seeds<<" converged="<<result.converged<<" duplicate="<<result.duplicates<<" outside="<<result.outside<<" failed="<<result.failed<<" complete=0\n";
        if(covered){const auto count=countModes(network,left,right,low,high);const bool complete=count.converged&&count.roots==inside&&inside>0;if(!complete)++bad;
            std::cout<<"MODAL_COVERAGE centre="<<centre<<" counted="<<count.roots<<" found_inside="<<inside<<" levels="<<count.levels<<" samples="<<count.samples<<" max_phase="<<count.maxPhaseIncrement<<" winding_residual="<<count.windingResidual<<" complete="<<complete<<'\n';}
    }
    std::cout<<"MODAL_DIAGNOSTIC failures="<<bad<<" acceptance=0\n";return bad?1:0;
}
int feasibilityRow(int index,FitDomain domain=FitDomain::unitCircle)
{
    PeerGuard guard;guard.force();const auto cases=corpus();
    if(index<0||index>=int(cases.size()*9))return 2;
    const auto& named=cases[std::size_t(index/9)];const auto& curve=named.curve;
    constexpr std::array<double,3> rates{44100,48000,96000},sizes{0,.5,1};
    constexpr std::array<double,7> centres{125,250,500,1000,2000,4000,8000};
    const double rate=rates[std::size_t(index%9/3)],size=sizes[std::size_t(index%3)];
    SpaceTank tank;const bool prepared=tank.prepare(rate,512,curve,size,FitGeometry::compact,true,true,TankGeometry::eight,domain);guard.force();
    const char* domainName=domain==FitDomain::poleContour?"poleContour":"unitCircle";
    std::cout<<std::setprecision(14)<<"CONFIG index="<<index<<" case="<<named.name<<" rate="<<rate<<" size="<<size<<" domain="<<domainName<<" prepared="<<prepared<<'\n';
    auto network=inspectNetwork(tank);network.poll=[&]{guard();};
    double total=0;
    for(std::size_t line=0;line<network.lines.size();++line){const auto& view=network.lines[line];total+=view.delaySeconds;
        std::cout<<std::hexfloat<<"BANK,"<<index<<','<<line<<','<<view.delaySeconds<<','<<view.loss->scalar<<','<<view.loss->stages.size()<<'\n';
        for(std::size_t stage=0;stage<view.loss->stages.size();++stage){const auto& s=view.loss->stages[stage];
            std::cout<<"COEFFICIENT,"<<index<<','<<line<<','<<stage<<','<<s.b0<<','<<s.b1<<','<<s.b2<<','<<s.a1<<','<<s.a2<<'\n';}}
    std::cout<<std::defaultfloat<<std::setprecision(14);
    const auto tail=prepared?certifyTail(network,46):TailCertificate{};
    std::cout<<"configuration,case,rate,size,centre,low,high,left_real,counted,found_inside,coverage,mode_max_error,min_mode_time,max_mode_time,max_null,seeds,failed_seeds,count_levels,count_samples,max_phase,winding_residual,tail_pass,tail_loop_upper,tail_intervals,fit_max_error,nominal_max_error,fit_domain,prepared_bytes,pass,reason\n";
    int bad=0;
    for(double centre:centres){
        const double half=prepared?std::max(2/total,centre/24):0,low=centre-half,high=centre+half;
        double minimum=40;for(int i=0;i<=128;++i)minimum=std::min(minimum,evaluate(curve,low+(high-low)*i/128.,rate)->seconds);
        const double left=-2*std::log(1000.)/minimum,right=1e-6;
        ModalSearch modes;ModalCount count;if(prepared){modes=searchModes(network,low,high,[&](double f){return evaluate(curve,f,rate)->seconds;});count=countModes(network,left,right,low,high);}
        std::size_t inside=0;double error=0,minTime=1e100,maxTime=0,maxResidual=0;
        for(const auto& mode:modes.modes){const double target=evaluate(curve,mode.frequency,rate)->seconds,relative=std::abs(mode.time/target-1);
            const bool admitted=mode.s.real()>=left&&mode.s.real()<=right;
            std::cout<<"MODE,"<<index<<','<<centre<<','<<mode.frequency<<','<<target<<','<<mode.time<<','<<relative<<','<<mode.residual<<','<<mode.s.real()<<','<<admitted<<'\n';
            if(admitted){++inside;error=std::max(error,relative);minTime=std::min(minTime,mode.time);maxTime=std::max(maxTime,mode.time);maxResidual=std::max(maxResidual,mode.residual);}}
        const bool coverage=count.converged&&count.roots==inside&&inside>0;
        const bool fit=prepared&&tank.maximumFitError()<=(named.isFlat?.05:.1);
        const bool resource=prepared&&tank.preparedBytes()<=16*1024*1024;
        const bool pass=prepared&&coverage&&error<=.15&&maxResidual<=1e-9&&tail.passed&&fit&&resource;
        const char* reason=!prepared?"preparation":!tail.passed?tail.reason:!fit?"nominal_fit":!resource?"resource":!count.converged?"count_unresolved":inside==0?"no_modes":count.roots!=inside?"count_mismatch":error>.15?"modal_error":maxResidual>1e-9?"null_residual":"pass";
        if(!pass)++bad;
        std::cout<<index<<','<<named.name<<','<<rate<<','<<size<<','<<centre<<','<<low<<','<<high<<','<<left<<','<<count.roots<<','<<inside<<','<<coverage<<','<<error<<','<<(inside?minTime:0)<<','<<maxTime<<','<<maxResidual<<','<<modes.seeds<<','<<modes.failed<<','<<count.levels<<','<<count.samples<<','<<count.maxPhaseIncrement<<','<<count.windingResidual<<','<<tail.passed<<','<<tail.maxLoopMagnitude<<','<<tail.intervals<<','<<tank.maximumFitError()<<','<<tank.maximumNominalError()<<','<<domainName<<','<<tank.preparedBytes()<<','<<pass<<','<<reason<<'\n';
        std::cout.flush();
    }
    guard.force();std::cout<<"CONFIG_SUMMARY index="<<index<<" bands=7 failures="<<bad<<" acceptance=0\n";return prepared?(bad?1:0):2;
}
}
int main(int argc,char** argv)
{
  try {
    const std::string_view arg=argc>1?argv[1]:"--self-test";
    if(arg=="--self-test"){selfTest();return failures?1:0;}
    if(arg=="--backend-test"){backendTest();return failures?1:0;}
    if(arg=="--tail-smoke")return tailSmoke();
    if(arg=="--pole-fit-test"){poleFitTest();return failures?1:0;}
    if(arg=="--short")return inspect(false);
    if(arg=="--flat")return inspect(true);
    if(arg=="--covered-short")return inspect(false,true);
    if(arg=="--covered-flat")return inspect(true,true);
    if((arg=="--feasibility-row"||arg=="--feasibility-pole-row")&&argc==3){int index=-1;const std::string_view value(argv[2]);const auto parsed=std::from_chars(value.data(),value.data()+value.size(),index);
        if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size())return 2;return feasibilityRow(index,arg=="--feasibility-pole-row"?FitDomain::poleContour:FitDomain::unitCircle);}
    return 2;
  }catch(const PeerObserved& interruption){std::cout<<"CONFIG_INTERRUPTED "<<interruption.reason<<'\n';return 3;}
}

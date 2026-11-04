#include "ModalAnalysis.h"
#include "SpaceReverbTank.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <string_view>
#include <vector>
using namespace airack::space;
namespace detail=airack::space::detail;
namespace probe=airack::space::probe;
namespace {
constexpr std::array<int,24> rows{0,20,42,62,168,188,1365,1372,1385,1412,1414,1415,1418,1421,1424,1427,1428,1448,1449,1452,1455,1461,1462,1469};
constexpr std::array<double,7> rates{8000,44100,48000,96000,192000,384000,768000};
constexpr std::array<double,3> sizes{0,.5,1};
DecayCurve curveFor(int row) {
    DecayCurve c;c.brightness=1;
    switch(row/21){
        case 0:c.decaySeconds=.1;c.brightness=0;break;
        case 2:c.decaySeconds=.1;break;
        case 8:c.decaySeconds=20;break;
        case 65:case 67:case 68:
            for(auto& b:c.bands)b={true,false,DecayShape::bell,row/21==65?20.:20000.,row/21==68?4.:.25,4};break;
        case 69:{constexpr std::array<double,6> f{20,80,320,1280,5120,20000};
            for(std::size_t i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,f[i],i%2?4.:.25,4};break;}
        default:return {};
    }
    return c;
}
struct Network {
    std::array<detail::LossBank,8> banks;
    probe::ModalNetwork view;
    explicit Network(const detail::PreparedTank& p) {
        view.sampleRate=p.sampleRate;view.mixing.resize(64);
        for(std::size_t i=0;i<8;++i){auto& b=banks[i];const auto& source=p.lines[i];b.scalar=source.scalar;
            for(std::size_t j=0;j<source.count;++j){const auto& c=source.coefficients[j];b.stages.push_back({c.b0,c.b1,c.b2,c.a1,c.a2});}
            view.lines.push_back({double(source.length)/p.sampleRate,&b});
            for(std::size_t j=0;j<8;++j){std::size_t bits=i&j;bool odd=false;while(bits){odd=!odd;bits&=bits-1;}
                view.mixing[i*8+j]=odd?-.3535533905932737622:.3535533905932737622;}
        }
    }
    Network(const Network&)=delete;Network& operator=(const Network&)=delete;
};
void printBank(int row,const detail::PreparedTank& p) {
    std::cout<<"WITNESS_SETTINGS row="<<row<<" rate="<<p.sampleRate<<" size="<<p.size<<" decay="<<p.curve.decaySeconds<<" brightness="<<p.curve.brightness<<'\n';
    for(std::size_t i=0;i<6;++i){const auto& b=p.curve.bands[i];std::cout<<"WITNESS_BAND row="<<row<<" band="<<i<<" active="<<b.active<<" bypass="<<b.bypassed<<" shape="<<int(b.shape)<<" frequency="<<b.frequencyHz<<" multiplier="<<b.multiplier<<" q="<<b.q<<'\n';}
    std::cout<<std::hexfloat;
    for(std::size_t i=0;i<8;++i){const auto& b=p.lines[i];std::cout<<"WITNESS_BANK row="<<row<<" line="<<i<<" length="<<b.length<<" scalar="<<b.scalar<<" count="<<b.count<<'\n';
        for(std::size_t j=0;j<b.count;++j){const auto& c=b.coefficients[j];std::cout<<"WITNESS_COEFFICIENT row="<<row<<" line="<<i<<" stage="<<j<<" b0="<<c.b0<<" b1="<<c.b1<<" b2="<<c.b2<<" a1="<<c.a1<<" a2="<<c.a2<<'\n';}}
    std::cout<<std::defaultfloat<<std::setprecision(17);
}
int selfTest() {
    detail::PreparedTank p;p.sampleRate=48000;
    constexpr std::array<unsigned,8> lengths{463,577,713,887,1049,1279,1481,1723};
    for(std::size_t i=0;i<8;++i){p.lines[i].length=lengths[i];p.lines[i].scalar=std::exp(-std::log(1000.)*lengths[i]/(48000*2.));}
    Network n(p);int checks=0,failures=0;const auto check=[&](bool ok){++checks;if(!ok)++failures;};
    for(std::size_t i=0;i<8;++i){check(n.view.lines[i].delaySeconds==double(lengths[i])/48000);check(n.banks[i].scalar==p.lines[i].scalar);check(n.banks[i].stages.empty());}
    const auto original=probe::searchModes(n.view,975,1025,[](double){return 2.;});
    const auto count=probe::countModes(n.view,-2*std::log(1000.)/2,1e-6,975,1025);
    check(count.converged&&!original.modes.empty()&&count.roots==original.modes.size());
    for(const auto& m:original.modes)check(std::abs(m.time/2-1)<1e-7&&m.residual<=1e-9);
    n.view.logarithmicReference=true;const auto reference=probe::searchModes(n.view,975,1025,[](double){return 2.;});
    check(reference.modes.size()==original.modes.size());
    if(reference.modes.size()==original.modes.size())for(std::size_t i=0;i<reference.modes.size();++i){check(std::abs(reference.modes[i].time/original.modes[i].time-1)<1e-7);check(std::abs(reference.modes[i].frequency/original.modes[i].frequency-1)<1e-7);}
    // A uniform scalar change shifts every mode's real rate by ln(2)/delay only
    // when delays match. For this unequal-delay mutant merely require a clear change.
    n.view.logarithmicReference=false;for(auto& b:n.banks)b.scalar*=1.001;
    const auto changed=probe::searchModes(n.view,975,1025,[](double){return 2.;});
    check(!changed.modes.empty());for(const auto& m:changed.modes)check(m.time>2.001);
    // Exercise a nonidentity copied section independently of all fitted fixtures.
    p.lines[3].count=1;p.lines[3].coefficients[0]={.2,.3,.4,-.5,.6};Network copy(p);
    const auto& c=copy.banks[3].stages[0];check(c.b0==.2&&c.b1==.3&&c.b2==.4&&c.a1==-.5&&c.a2==.6);
    detail::LossBank storage;storage.scalar=.5;storage.stages.push_back(detail::peakCut(250,4,6,48000));
    probe::ModalNetwork one{48000,{{.01,&storage}},{0}};
    const auto& section=storage.stages[0];const auto pole=(-section.a1+std::sqrt(std::complex<double>(section.a1*section.a1-4*section.a2)))*.5;
    const auto known=48000.*std::log(pole);const std::array<std::complex<double>,1> extra{known};
    const auto seeded=probe::searchModes(one,200,300,[](double){return std::numeric_limits<double>::quiet_NaN();},extra);
    check(seeded.modes.size()==1);
    if(seeded.modes.size()==1){check(std::abs(seeded.modes[0].s-known)<1e-5);check(seeded.modes[0].residual<=1e-9);}
    else {check(false);check(false);}
    std::cout<<"WITNESS_SELF checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}
int modal(int ordinal,int row,const detail::PreparedTank& p) {
    Network n(p);double total=0;for(const auto& l:n.view.lines)total+=l.delaySeconds;
    const double rate=p.sampleRate,targetRate=std::min(rate,384000.),upper=std::min(20000.,.45*rate);
    const auto target=[&](double f){return evaluate(p.curve,std::clamp(f,20.,upper),targetRate)->seconds;};
    std::vector<double> centers{20,125,250,500,1000,2000,4000,std::min(8000.,upper),upper};
    for(const auto& b:p.curve.bands)if(b.active&&!b.bypassed)centers.push_back(std::min(b.frequencyHz,upper));
    for(auto& c:centers)c=std::min(c,upper);std::sort(centers.begin(),centers.end());centers.erase(std::unique(centers.begin(),centers.end()),centers.end());
    std::size_t failed=0;
    for(double center:centers){const double half=std::max(2/total,center/24),low=std::max(20.,center-half),high=std::min(upper,center+half);
        double minimum=40;for(int i=0;i<=128;++i)minimum=std::min(minimum,target(low+(high-low)*i/128.));
        const double left=-2*std::log(1000.)/minimum;
        probe::ModalSearch modes;probe::ModalCount count;count.converged=true;count.windingResidual=0;
        const auto slabs=std::max(std::size_t(1),std::size_t(std::ceil((high-low)*total/128)));
        for(std::size_t slab=0;slab<slabs;++slab){
            const double begin=low+(high-low)*double(slab)/slabs,end=low+(high-low)*double(slab+1)/slabs;
            std::vector<std::complex<double>> seeds;
            for(const auto& b:n.banks)for(const auto& c:b.stages){
                const auto discriminant=std::sqrt(std::complex<double>(c.a1*c.a1-4*c.a2));
                for(const auto pole:std::array<std::complex<double>,2>{(-c.a1+discriminant)*.5,(-c.a1-discriminant)*.5}){
                    const auto s=rate*std::log(pole);const double f=s.imag()/(2*std::numbers::pi);
                    if(f>=begin&&f<=end)for(double scale:{.8,1.,1.2})if(s.real()*scale>=left&&s.real()*scale<0)seeds.emplace_back(s.real()*scale,s.imag());
                }
            }
            const auto found=probe::searchModes(n.view,begin,end,target,seeds);const auto counted=probe::countModes(n.view,left,1e-6,begin,end);
            std::size_t insideSlab=0;for(const auto& m:found.modes)if(m.s.real()>=left&&m.s.real()<=1e-6)++insideSlab;
            const bool complete=counted.converged&&insideSlab==counted.roots;
            count.converged=count.converged&&complete;count.roots+=counted.roots;count.samples+=counted.samples;count.levels=std::max(count.levels,counted.levels);
            count.maxPhaseIncrement=std::max(count.maxPhaseIncrement,counted.maxPhaseIncrement);count.windingResidual=std::max(count.windingResidual,counted.windingResidual);
            modes.seeds+=found.seeds;modes.failed+=found.failed;
            for(const auto& m:found.modes)if(std::none_of(modes.modes.begin(),modes.modes.end(),[&](const auto& old){return std::abs(old.s-m.s)<1e-5;}))modes.modes.push_back(m);
            std::cout<<"WITNESS_SLAB row="<<row<<" center="<<center<<" slab="<<slab<<" slabs="<<slabs<<" low="<<begin<<" high="<<end<<" left="<<left<<" pole_seeds="<<seeds.size()<<" found="<<insideSlab<<" counted="<<counted.roots<<" coverage="<<complete<<" count_converged="<<counted.converged<<" count_levels="<<counted.levels<<" count_samples="<<counted.samples<<" max_phase="<<counted.maxPhaseIncrement<<" winding_residual="<<counted.windingResidual<<" pass="<<complete<<'\n'<<std::flush;
        }
        std::sort(modes.modes.begin(),modes.modes.end(),[](const auto& a,const auto& b){return a.frequency<b.frequency;});
        std::size_t inside=0;double error=0,residual=0;
        for(const auto& m:modes.modes){const bool admitted=m.s.real()>=left&&m.s.real()<=1e-6;const double expected=target(m.frequency),e=std::abs(m.time/expected-1);
            std::cout<<"WITNESS_MODE row="<<row<<" center="<<center<<" frequency="<<m.frequency<<" target="<<expected<<" time="<<m.time<<" error="<<e<<" residual="<<m.residual<<" real="<<m.s.real()<<" admitted="<<admitted<<'\n';
            if(admitted){++inside;error=std::max(error,e);residual=std::max(residual,m.residual);}}
        const bool coverage=count.converged&&inside>0&&inside==count.roots;
        const bool pass=coverage&&error<=.15&&residual<=1e-9&&p.maximumFitError<=.1&&p.stageCount<=38&&p.admission.passed;
        if(!pass)++failed;
        std::cout<<"WITNESS_REGION row="<<row<<" center="<<center<<" low="<<low<<" high="<<high<<" left="<<left<<" found="<<inside<<" counted="<<count.roots<<" coverage="<<coverage<<" error="<<error<<" residual="<<residual<<" seeds="<<modes.seeds<<" failed_seeds="<<modes.failed<<" count_levels="<<count.levels<<" count_samples="<<count.samples<<" max_phase="<<count.maxPhaseIncrement<<" winding_residual="<<count.windingResidual<<" pass="<<pass<<'\n'<<std::flush;
    }
    std::cout<<"WITNESS_FOOTER ordinal="<<ordinal<<" row="<<row<<" mode=modal regions="<<centers.size()<<" failed="<<failed<<" complete=1\n";return failed?1:0;
}
int impulse(int ordinal,int row,const detail::PreparedTank& p) {
    const double rate=p.sampleRate,upper=std::min(20000.,.45*rate),targetRate=std::min(rate,384000.);double longest=.1;
    for(int i=0;i<=768;++i)longest=std::max(longest,evaluate(p.curve,20*std::pow(upper/20,i/768.),targetRate)->seconds);
    for(const auto& b:p.curve.bands)if(b.active&&!b.bypassed)longest=std::max(longest,evaluate(p.curve,b.frequencyHz,targetRate)->seconds);
    const auto frames=std::size_t(std::ceil(std::max(2.,1.25*longest+.3)*rate)),tailStart=frames-std::min(frames,std::size_t(std::ceil(.25*rate)));
    detail::TankRuntime runtime;bool ok=runtime.prepareStorage(rate)&&runtime.configure(p);const auto bytes=runtime.storageBytes();
    std::array<detail::TankRuntime::Frame,512> scratch{};double energy=0,tailEnergy=0,peak=0;bool finite=true;
    if(ok)for(std::size_t offset=0;offset<frames;){const auto count=std::min(scratch.size(),frames-offset);scratch.fill({});if(offset==0)scratch[0]={.5,.25};
        ok=runtime.processBlock(std::span(scratch).first(count))&&ok;
        for(std::size_t j=0;j<count;++j){const auto& y=scratch[j];finite=finite&&std::isfinite(y[0])&&std::isfinite(y[1]);peak=std::max({peak,std::abs(y[0]),std::abs(y[1])});const double e=y[0]*y[0]+y[1]*y[1];energy+=e;if(offset+j>=tailStart)tailEnergy+=e;}
        offset+=count;
    }
    const double fraction=energy>0?tailEnergy/energy:std::numeric_limits<double>::infinity();
    const bool pass=ok&&finite&&std::isfinite(energy)&&energy>0&&peak>0&&peak<=4&&fraction<=1e-6&&bytes<=16*1024*1024;
    std::cout<<"WITNESS_IMPULSE row="<<row<<" frames="<<frames<<" longest="<<longest<<" energy="<<energy<<" tail_energy="<<tailEnergy<<" tail_fraction="<<fraction<<" peak="<<peak<<" finite="<<finite<<" bytes="<<bytes<<" pass="<<pass<<'\n';
    std::cout<<"WITNESS_FOOTER ordinal="<<ordinal<<" row="<<row<<" mode=impulse regions=1 failed="<<!pass<<" complete=1\n";return pass?0:1;
}
}
int main(int argc,char** argv) {
    std::cout<<std::setprecision(17);
    if(argc==2&&std::string_view(argv[1])=="--self-test")return selfTest();
    if(argc!=3)return 2;const std::string_view mode(argv[1]),value(argv[2]);int ordinal=-1;const auto parsed=std::from_chars(value.data(),value.data()+value.size(),ordinal);
    if(parsed.ec!=std::errc{}||parsed.ptr!=value.data()+value.size()||ordinal<0||ordinal>=int(rows.size())||(mode!="--modal"&&mode!="--impulse"))return 2;
    const int row=rows[std::size_t(ordinal)];const double rate=rates[std::size_t(row%21/3)],size=sizes[std::size_t(row%3)];
    detail::PreparedTank p;detail::TankPreparationReport report;
    if(!detail::prepareTank(curveFor(row),size,rate,p,&report)){std::cout<<"WITNESS_PREPARATION ordinal="<<ordinal<<" row="<<row<<" pass=0 reason="<<report.detail<<'\n';return 1;}
    printBank(row,p);return mode=="--modal"?modal(ordinal,row,p):impulse(ordinal,row,p);
}

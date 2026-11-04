#include "SpaceReverbTank.h"
#include "ProbeCases.h"
#include "ProbePeerGuard.h"
#include <charconv>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string_view>
using namespace airack::space;
namespace probe=airack::space::probe;
namespace detail=airack::space::detail;
namespace {
std::vector<probe::NamedCurve> curves() {
    auto rows=probe::corpus();
    auto add=[&](DecayCurve c){rows.push_back({"endpoint-"+std::to_string(rows.size()),c,false});};
    for(double f:{20.,20000.})for(double m:{.25,4.})for(double q:{.25,1.,4.}){auto c=probe::flatForCorpus();c.bands[0]={true,false,DecayShape::bell,f,m,q};add(c);}
    for(auto shape:{DecayShape::lowShelf,DecayShape::highShelf})for(double f:{20.,20000.})for(double m:{.25,4.})for(double q:{.25,4.}){auto c=probe::flatForCorpus();c.bands[0]={true,false,shape,f,m,q};add(c);}
    for(double f:{20.,20000.})for(double m:{.25,4.}){auto c=probe::flatForCorpus();for(auto& b:c.bands)b={true,false,DecayShape::bell,f,m,4};add(c);}
    auto c=probe::flatForCorpus();std::array<double,6> f{20,80,320,1280,5120,20000};
    for(std::size_t i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,f[i],i%2?4.:.25,4};add(c);return rows;
}
}
int main(int argc,char** argv) {
    const auto cases=curves();constexpr std::array<double,7> rates{8000,44100,48000,96000,192000,384000,768000};constexpr std::array<double,3> sizes{0,.5,1};
    if(cases.size()!=70)return 2;const std::size_t total=cases.size()*21;
    if(argc==2&&std::string_view(argv[1])=="--describe"){std::cout<<"RANGE_SCHEMA curves="<<cases.size()<<" rates=7 sizes=3 rows="<<total<<'\n';return 0;}
    int start=0;if(argc!=3)return 2;const std::string_view mode=argv[1];
    const bool functional=mode=="--range-functional"||mode=="--row-functional";
    if(mode!="--range"&&mode!="--row"&&!functional)return 2;
    const std::string_view value=argv[2];const auto parse=std::from_chars(value.data(),value.data()+value.size(),start);if(parse.ec!=std::errc{}||parse.ptr!=value.data()+value.size()||start<0||std::size_t(start)>total)return 2;
    const auto end=(mode=="--row"||mode=="--row-functional")?std::min(total,std::size_t(start)+1):total;
    std::cout<<std::setprecision(17);std::size_t completed=0,failed=0;probe::PeerGuard guard;
    const auto checkPeer=[&](){if(!functional)guard.force();};
    try {checkPeer();
        for(std::size_t row=std::size_t(start);row<end;++row){checkPeer();const auto curve=row/21,rate=(row%21)/3,size=row%3;
            detail::PreparedTank p;detail::TankPreparationReport report;bool passed=detail::prepareTank(cases[curve].curve,sizes[size],rates[rate],p,&report);
            detail::TankRuntime runtime;std::size_t bytes=0;
            if(passed){const bool storage=runtime.prepareStorage(rates[rate]);bytes=runtime.storageBytes();if(!storage||bytes>16*1024*1024||!runtime.configure(p)){passed=false;report.detail="runtime_admission";}}
            checkPeer();
            std::cout<<"RANGE_ROW row="<<row<<" curve="<<curve<<" rate="<<rates[rate]<<" size="<<sizes[size]<<" pass="<<passed<<" reason="<<report.detail<<" line="<<report.line
                <<" fit_probe_hz="<<report.worstFrequencyHz<<" fit_probe_pole="<<report.maximumPole<<" fit_probe_passive="<<report.stabilityCertified<<" contour_error="<<report.maximumFitError<<" nominal_error="<<report.maximumNominalError<<" edge_lines="<<std::count_if(p.lines.begin(),p.lines.end(),[](const auto& line){return line.fitVariant==1;})<<" bytes="<<bytes<<" stages="<<p.stageCount<<" tail="<<p.admission.passed<<" loop="<<p.admission.maxLoopMagnitude<<'\n'<<std::flush;
            ++completed;if(!passed)++failed;
        }
    }catch(const probe::PeerObserved& e){std::cout<<"RANGE_INTERRUPTED next="<<std::size_t(start)+completed<<" reason="<<e.reason<<'\n';return 3;}
    std::cout<<"RANGE_FOOTER start="<<start<<" completed="<<completed<<" failed="<<failed<<" complete=1\n";return failed?1:0;
}

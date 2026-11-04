#include "ModalTank.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>
using namespace airack::space;using namespace airack::space::probe;
int main(){
    unsigned checks=0,failures=0;const auto check=[&](bool value){++checks;if(!value){++failures;if(failures<40)std::cerr<<"DIRECT_GRID_FAIL check="<<checks<<'\n';}};
    auto p=std::make_unique<PreparedModal>(),q=std::make_unique<PreparedModal>();auto flat=DecayCurve{};flat.brightness=1;
    auto mixed=flat;for(std::size_t k=0;k<6;++k)mixed.bands[k]={true,false,DecayShape::bell,std::array<double,6>{53.,167.,713.,1807.,5001.,16000.}[k],k%2?4.:.25,4};
    for(const auto& curve:{flat,mixed})for(auto count:{1024U,2048U})for(double rate:{8000.,48000.,768000.})for(double size:{0.,.5,1.}){
        check(prepareModal(curve,size,rate,count,*p));check(prepareModal(curve,size,rate,count,*q));check(*p==*q);check(p->count==count);
        const double upper=std::min(20000.,.45*rate),range=std::log((upper+228.83)/(20+228.83));
        std::vector<double> anchors{20,upper};for(double f:{125.,250.,500.,1000.,2000.,4000.,8000.})anchors.push_back(std::min(f,upper));
        for(const auto& b:curve.bands)if(b.active&&!b.bypassed)anchors.push_back(std::min(b.frequencyHz,upper));
        std::sort(anchors.begin(),anchors.end());anchors.erase(std::unique(anchors.begin(),anchors.end()),anchors.end());
        std::vector<double> frequencies,unanchored;bool cells=p->count==count,covered=true;
        for(std::size_t k=0;k<p->count;++k){const double f=p->frequency[k];frequencies.push_back(f);
            cells=cells&&std::isfinite(f)&&f>=20&&f<=upper;
            if(std::find(anchors.begin(),anchors.end(),f)!=anchors.end())continue;unanchored.push_back(f);
            const double cell=(k/8)*4+k%4,t=std::log((f+228.83)/(20+228.83))/range*(count/2);
            const bool side=(k/4)%2!=0;const double low=cell+(side?.55:.05),high=cell+(side?.95:.45);
            cells=cells&&t>=low-1e-10&&t<=high+1e-10;
        }
        for(unsigned branch=0;branch<2;++branch)for(double anchor:anchors){bool found=false;for(std::size_t k=0;k<p->count;++k)found=found||((k/4)%2==branch&&p->frequency[k]==anchor);covered=covered&&found;}
        check(cells);check(covered);
        std::sort(frequencies.begin(),frequencies.end());const auto unique=std::unique(frequencies.begin(),frequencies.end())-frequencies.begin();
        std::sort(unanchored.begin(),unanchored.end());const auto remaining=std::unique(unanchored.begin(),unanchored.end())-unanchored.begin();
        check(std::size_t(remaining)==unanchored.size());check(std::size_t(unique)>=count-2*anchors.size());
    }
    const auto resonantFactor=[](const PreparedModal& bank){for(std::size_t k=0;k<bank.count;++k)if(bank.frequency[k]==1000)return bank.pickup/(1-std::hypot(bank.rc[k],bank.rs[k]));return 0.;};
    for(auto count:{1024U,2048U}){
        check(prepareModal(flat,.5,48000,count,*p));const double reference=resonantFactor(*p);check(std::isfinite(reference)&&reference>0);
        for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.,768000.}){check(prepareModal(flat,.5,rate,count,*p));check(std::abs(resonantFactor(*p)/reference-1)<=1e-6);}
    }
    std::cout<<"SpaceReverbDirectModalGrid checks="<<checks<<" failures="<<failures<<" configurations=36 level_rates=7\n";return failures?1:0;
}

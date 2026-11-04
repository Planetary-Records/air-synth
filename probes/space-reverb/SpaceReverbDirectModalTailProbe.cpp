#include "ModalField.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>
using namespace airack::space;using namespace airack::space::probe;
int main(){
    std::cout<<std::setprecision(17);std::size_t row=0,sections=0,failed=0;
    for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.,768000.})for(unsigned shape=0;shape<5;++shape)for(double f:{20.,80.,320.,1280.,5120.,20000.})for(double gain:{-18.,-6.,0.,6.,18.}){
        if(shape>=3&&gain!=0)continue;std::vector<double> qs=shape==0?std::vector<double>{.25,.5,1,4,12}:shape<3?std::vector<double>{.25,.5,1,4}:std::vector<double>{1};
        if(shape==0){const double critical=.5/std::pow(10.,gain/40);for(double scale:{1-1e-7,1-1e-12,1.,1+1e-12,1+1e-7})if(critical*scale>=.25&&critical*scale<=12)qs.push_back(critical*scale);std::sort(qs.begin(),qs.end());qs.erase(std::unique(qs.begin(),qs.end()),qs.end());}
        for(double q:qs)for(unsigned slope=0;slope<(shape>=3?2U:1U);++slope){Settings s;auto& b=s.post[0];b.active=true;b.shape=static_cast<PostShape>(shape);b.frequencyHz=f;b.gainDb=gain;if(shape==0)b.bellQ=q;else if(shape<3)b.shelfQ=q;b.slope=static_cast<CutSlope>(slope);
            PreparedPostEq p;const bool ready=preparePostEq(s,rate,p);bool pass=ready;
            std::cout<<"TAIL_GRID_CONFIG row="<<row<<" rate="<<rate<<" shape="<<shape<<" frequency="<<f<<" gain="<<gain<<" q="<<q<<" slope="<<slope<<" stages="<<p.count<<" effective="<<p.effectiveHz[0]<<" prepared="<<ready<<'\n';
            if(ready)for(std::size_t j=0;j<p.count;++j){const auto& c=p.stages[j];const double slow=std::exp(-std::log(1000.)/(rate*46));const auto probe=boundModalPostSection(c,slow);const double poleTime=probe.maximumPole>0&&probe.maximumPole<1?-std::log(1000.)/(rate*std::log(probe.maximumPole)):0;
                for(unsigned circle=0;circle<2;++circle){const double rho=circle?std::exp(-std::log(1000.)/(rate*std::max(.115,1.15*poleTime))):slow;const auto bound=boundModalPostSection(c,rho);pass=pass&&bound.passed;++sections;
                    std::cout<<"TAIL_GRID_SECTION row="<<row<<" stage="<<j<<" circle="<<circle<<" rho="<<rho<<" b0="<<c.b0<<" b1="<<c.b1<<" b2="<<c.b2<<" a1="<<c.a1<<" a2="<<c.a2<<" log_l1="<<bound.logL1<<" maximum_pole="<<bound.maximumPole<<" pass="<<bound.passed<<'\n';}}
            std::cout<<"TAIL_GRID_ROW row="<<row<<" pass="<<pass<<'\n';if(!pass)++failed;++row;
        }
    }
    std::cout<<"TAIL_GRID_FOOTER rows="<<row<<" sections="<<sections<<" failed="<<failed<<" complete=1\n";return failed?1:0;
}

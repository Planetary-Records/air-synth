#pragma once
#include "SpaceReverbModel.h"
#include <vector>
#include <string>
#include <cmath>
namespace airack::space::probe {
inline DecayCurve flatForCorpus(){DecayCurve c;c.brightness=1;return c;}
struct NamedCurve {std::string name;DecayCurve curve;bool isFlat=false;};
inline std::vector<NamedCurve> corpus()
{
    std::vector<NamedCurve> rows;
    for(double t:{.1,2.,20.})for(double b:{0.,.5,1.}){
        auto c=flatForCorpus();c.decaySeconds=t;c.brightness=b;
        rows.push_back({"base-"+std::to_string(t)+"-brightness-"+std::to_string(b),c,b==1});
    }
    for(double m:{.25,4.})for(double f:{125.,1000.,8000.})for(double q:{.25,1.,4.}){
        auto c=flatForCorpus();c.bands[0]={true,false,DecayShape::bell,f,m,q};
        rows.push_back({"bell-"+std::to_string(f)+"-m"+std::to_string(m)+"-q"+std::to_string(q),c,false});
    }
    for(auto shape:{DecayShape::lowShelf,DecayShape::highShelf})for(double f:{250.,4000.})for(double m:{.25,4.}){
        auto c=flatForCorpus();c.bands[0]={true,false,shape,f,m,1};
        rows.push_back({std::string(shape==DecayShape::lowShelf?"low-":"high-")+std::to_string(f)+"-m"+std::to_string(m),c,false});
    }
    auto c=flatForCorpus();
    for(size_t i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,125*std::pow(2.,double(i)),i%2?4.:.25,4};
    rows.push_back({"six-alternating",c,false});
    for(auto& b:c.bands)b={true,false,DecayShape::bell,1000,4,4};
    rows.push_back({"six-overlap",c,false});
    return rows;
}
}

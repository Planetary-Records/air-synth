#pragma once
#include "ProbeCases.h"
namespace airack::space::probe {
inline std::vector<NamedCurve> directModalCurves() {
    auto rows=corpus();
    auto add=[&](DecayCurve c){rows.push_back({"endpoint-"+std::to_string(rows.size()),c,false});};
    for(double f:{20.,20000.})for(double m:{.25,4.})for(double q:{.25,1.,4.}){auto c=flatForCorpus();c.bands[0]={true,false,DecayShape::bell,f,m,q};add(c);}
    for(auto shape:{DecayShape::lowShelf,DecayShape::highShelf})for(double f:{20.,20000.})for(double m:{.25,4.})for(double q:{.25,4.}){auto c=flatForCorpus();c.bands[0]={true,false,shape,f,m,q};add(c);}
    for(double f:{20.,20000.})for(double m:{.25,4.}){auto c=flatForCorpus();for(auto& b:c.bands)b={true,false,DecayShape::bell,f,m,4};add(c);}
    auto c=flatForCorpus();std::array<double,6> f{20,80,320,1280,5120,20000};
    for(std::size_t i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,f[i],i%2?4.:.25,4};add(c);return rows;
}
}

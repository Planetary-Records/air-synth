#include "SpaceReverbModel.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
using namespace airack::space;
int checks=0, failures=0;
void check(bool ok,const char* text,int line)
{ ++checks; if(!ok){++failures;std::cerr<<"FAIL "<<line<<" "<<text<<'\n';} }
#define CHECK(x) check(bool(x),#x,__LINE__)
bool near(double a,double b,double e=1e-10){return std::abs(a-b)<=e*std::max(1.,std::abs(b));}
DecayCurve flat(){DecayCurve c;c.brightness=1;return c;}
double time(const DecayCurve& c,double f,double rate=48000)
{auto p=evaluate(c,f,rate);CHECK(p.has_value());return p?p->seconds:-1;}
void targets()
{
    auto c=flat();
    for(double f:{20.,125.,1000.,4000.,16000.,20000.})CHECK(near(time(c,f),2));
    c.bands[0]={true,false,DecayShape::bell,1000,4,1};
    CHECK(near(time(c,1000),8));
    // A one-octave offset has the same time above and below centre.
    CHECK(near(time(c,500),time(c,2000)));
    CHECK(time(c,2000)<8&&time(c,2000)>2);
    c.bands[0].q=4;
    CHECK(time(c,2000)<2.1);
    c.bands[0].bypassed=true;CHECK(near(time(c,1000),2));
    c.bands[0].bypassed=false;c.bands[0].active=false;CHECK(near(time(c,1000),2));
    c.bands[0]={true,false,DecayShape::lowShelf,1000,4,1};
    CHECK(near(time(c,1000),4));
    CHECK(time(c,125)>time(c,1000));CHECK(time(c,8000)<time(c,1000));
    c.bands[1]={true,false,DecayShape::highShelf,1000,4,1};
    for(double f:{20.,125.,1000.,8000.,20000.})CHECK(near(time(c,f),8));
    const auto saved=c;
    std::reverse(c.bands.begin(),c.bands.end());
    for(double f:{125.,1000.,8000.})CHECK(near(time(c,f),time(saved,f)));
    CHECK(c.bands[4].shape==DecayShape::highShelf);
    c=flat();c.brightness=0;
    CHECK(near(time(c,4000),.4472135954999579));
    CHECK(time(c,125)>time(c,1000));CHECK(time(c,1000)>time(c,8000));
    c.brightness=.5;const double middle=time(c,4000);
    c.brightness=1;CHECK(time(c,4000)>middle);
}
void bounds_and_retention()
{
    auto c=flat();c.decaySeconds=20;
    for(auto& b:c.bands)b={true,false,DecayShape::bell,1000,4,1};
    auto p=evaluate(c,1000,48000);CHECK(p&&near(p->seconds,40));
    CHECK(p&&near(p->unclampedSeconds,81920));CHECK(p&&p->decayLimited);
    c.decaySeconds=.1;for(auto& b:c.bands)b.multiplier=.25;
    p=evaluate(c,1000,48000);CHECK(p&&near(p->seconds,.1));
    CHECK(p&&p->unclampedSeconds<.1&&p->decayLimited);
    c=flat();c.bands[0]={true,false,DecayShape::bell,20000,4,1};
    p=evaluate(c,20000,44100);
    CHECK(p&&near(p->effectiveHz,19845));CHECK(p&&p->frequencyLimited);
    CHECK(p&&near(p->seconds,8));
    p=evaluate(c,20000,96000);CHECK(p&&!p->frequencyLimited&&near(p->effectiveHz,20000));
    const auto saved=c;
    for(double rate:{44100.,48000.,96000.})
        for(int n=0;n<200;++n){
            p=evaluate(c,20*std::pow(1000.,n/199.),rate);
            CHECK(p&&std::isfinite(p->seconds)&&p->seconds>=.1&&p->seconds<=40);
        }
    CHECK(c==saved);
}
void admission()
{
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double inf=std::numeric_limits<double>::infinity();
    auto c=flat();CHECK(valid(c));
    for(double v:{nan,inf,-inf,0.,.09,20.01}){
        c=flat();c.decaySeconds=v;CHECK(!valid(c));CHECK(!evaluate(c,1000,48000));
    }
    for(double v:{nan,inf,-.01,1.01}){c=flat();c.brightness=v;CHECK(!valid(c));}
    // Inactive fields remain subject to validation.
    for(double v:{nan,inf,19.,20001.}){c=flat();c.bands[5].frequencyHz=v;CHECK(!valid(c));}
    for(double v:{nan,inf,.249,4.001}){c=flat();c.bands[5].multiplier=v;CHECK(!valid(c));}
    for(double v:{nan,inf,.249,4.001}){c=flat();c.bands[5].q=v;CHECK(!valid(c));}
    c=flat();c.bands[5].shape=static_cast<DecayShape>(99);CHECK(!valid(c));
    c=flat();
    for(double rate:{nan,inf,7999.,384001.})CHECK(!evaluate(c,1000,rate));
    for(double f:{nan,inf,0.,19.,20001.})CHECK(!evaluate(c,f,48000));
    CHECK(evaluate(c,20,8000));CHECK(evaluate(c,20000,384000));
}
}
int main()
{
    targets();bounds_and_retention();admission();
    std::cout<<"SpaceReverbModel checks="<<checks<<" failures="<<failures<<'\n';
    return failures?1:0;
}


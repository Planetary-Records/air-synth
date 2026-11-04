#include "SpaceReverbControls.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace airack::space {
namespace {
bool range(double x,double lo,double hi) noexcept {return std::isfinite(x)&&x>=lo&&x<=hi;}
bool rateValid(double rate) noexcept {return range(rate,8000,768000);}
bool stable(const PostCoefficients& c) noexcept
{
    if(!std::isfinite(c.b0)||!std::isfinite(c.b1)||!std::isfinite(c.b2)||!std::isfinite(c.a1)||!std::isfinite(c.a2))return false;
    const double d=c.a1*c.a1-4*c.a2;
    if(d<0)return c.a2>0&&c.a2<1;
    const double root=std::sqrt(d);
    return std::abs((c.a1+root)*.5)<1&&std::abs((c.a1-root)*.5)<1;
}
bool admitted(const PreparedPostEq& p) noexcept
{
    if(!rateValid(p.sampleRate)||p.count>p.stages.size())return false;
    std::size_t count=0;
    for(std::size_t i=0;i<6;++i){
        if(p.starts[i]!=count||p.counts[i]>2||!range(p.effectiveHz[i],20,std::min(20000.,.45*p.sampleRate)))return false;
        count+=p.counts[i];if(count>p.stages.size())return false;
    }
    if(count!=p.count)return false;
    for(std::size_t i=0;i<count;++i)if(!stable(p.stages[i]))return false;
    return true;
}
PostCoefficients normalize(double b0,double b1,double b2,double a0,double a1,double a2) noexcept
{return {b0/a0,b1/a0,b2/a0,a1/a0,a2/a0};}
PostCoefficients bell(double f,double gain,double q,double rate) noexcept
{
    const double w=2*std::numbers::pi*f/rate,c=std::cos(w),a=std::sin(w)/(2*q),A=std::pow(10.,gain/40);
    return normalize(1+a*A,-2*c,1-a*A,1+a/A,-2*c,1-a/A);
}
PostCoefficients shelf(double f,double gain,double q,bool high,double rate) noexcept
{
    const double w=2*std::numbers::pi*f/rate,c=std::cos(w),a=std::sin(w)/(2*q),A=std::pow(10.,gain/40);
    const double p=A+1,m=A-1,beta=2*std::sqrt(A)*a;
    return high?normalize(A*(p+m*c+beta),-2*A*(m+p*c),A*(p+m*c-beta),p-m*c+beta,2*(m-p*c),p-m*c-beta)
        :normalize(A*(p-m*c+beta),2*A*(m-p*c),A*(p-m*c-beta),p+m*c+beta,-2*(m+p*c),p+m*c-beta);
}
PostCoefficients cut(double f,double q,bool highpass,double rate) noexcept
{
    const double w=2*std::numbers::pi*f/rate,c=std::cos(w),a=std::sin(w)/(2*q);
    return highpass?normalize((1+c)*.5,-(1+c),(1+c)*.5,1+a,-2*c,1-a)
        :normalize((1-c)*.5,1-c,(1-c)*.5,1+a,-2*c,1-a);
}
bool admitted(const PreparedDuck& p) noexcept
{
    return rateValid(p.sampleRate)&&range(p.attack,0,1)&&p.attack<1&&range(p.release,0,1)&&p.release<1
        &&range(p.rangeDb,0,24)&&range(p.thresholdDb,-60,0);
}
}
bool valid(const Settings& s) noexcept
{
    if(!valid(s.decay)||!range(s.size,0,1)||!range(s.predelayMs,0,250)||!range(s.character,0,1)
        ||!range(s.distance,0,1)||!range(s.width,0,2)||!range(s.mix,0,1)||!range(s.duckRangeDb,0,24)
        ||!range(s.duckThresholdDb,-60,0)||!range(s.duckAttackMs,.1,200)||!range(s.duckReleaseMs,10,2000))return false;
    for(const auto& b:s.post)if(int(b.shape)<0||int(b.shape)>int(PostShape::highCut)||int(b.slope)<0||int(b.slope)>int(CutSlope::db24)
        ||!range(b.frequencyHz,20,20000)||!range(b.gainDb,-18,18)||!range(b.bellQ,.25,12)||!range(b.shelfQ,.25,4))return false;
    return true;
}
bool preparePostEq(const Settings& s,double rate,PreparedPostEq& out) noexcept
{
    if(!valid(s)||!rateValid(rate))return false;
    PreparedPostEq p;p.sampleRate=rate;
    for(std::size_t i=0;i<6;++i){
        const auto& b=s.post[i];const double f=std::min(b.frequencyHz,.45*rate);p.effectiveHz[i]=f;p.starts[i]=p.count;
        if(!b.active||b.bypassed)continue;
        const auto append=[&](const PostCoefficients& c){if(p.count>=p.stages.size()||!stable(c))return false;p.stages[p.count++]=c;++p.counts[i];return true;};
        if(b.shape==PostShape::lowCut||b.shape==PostShape::highCut){
            const bool highpass=b.shape==PostShape::lowCut;
            if(b.slope==CutSlope::db12){if(!append(cut(f,std::numbers::sqrt2*.5,highpass,rate)))return false;}
            else if(!append(cut(f,.541196100146197,highpass,rate))||!append(cut(f,1.306562964876377,highpass,rate)))return false;
        }else if(b.gainDb!=0){
            const auto c=b.shape==PostShape::bell?bell(f,b.gainDb,b.bellQ,rate):shelf(f,b.gainDb,b.shelfQ,b.shape==PostShape::highShelf,rate);
            if(!append(c))return false;
        }
    }
    if(!admitted(p))return false;
    out=p;return true;
}
std::optional<std::complex<double>> postEqResponse(const PreparedPostEq& p,double f,std::optional<std::size_t> band) noexcept
{
    if(!admitted(p)||!range(f,0,p.sampleRate*.5)||(band&&*band>=6))return std::nullopt;
    const auto z=std::polar(1.,-2*std::numbers::pi*f/p.sampleRate),z2=z*z;std::complex<double> result(1,0);
    const auto first=band?p.starts[*band]:0,count=band?p.counts[*band]:p.count;
    for(std::size_t i=first;i<first+count;++i){const auto& c=p.stages[i];result*=(c.b0+c.b1*z+c.b2*z2)/(1.+c.a1*z+c.a2*z2);}
    return result;
}
bool PostEq::prepare(const PreparedPostEq& p) noexcept {if(!admitted(p))return false;prepared_=p;ready_=true;reset();return true;}
void PostEq::reset() noexcept {history_={};}
std::array<double,2> PostEq::tick(std::array<double,2> input) noexcept
{
    if(!ready_||prepared_.count==0)return input;
    for(std::size_t channel=0;channel<2;++channel)for(std::size_t i=0;i<prepared_.count;++i){
        const auto& c=prepared_.stages[i];auto& h=history_[channel][i];const double x=input[channel],y=c.b0*x+h.z1;
        h.z1=(c.b1*x-c.a1*y)+h.z2;h.z2=c.b2*x-c.a2*y;input[channel]=y;
    }
    return input;
}
bool PostEq::processBlock(std::span<std::array<double,2>> frames) noexcept
{
    if(!ready_||frames.size()>512)return false;
    if(frames.empty()||prepared_.count==0)return true;
    for(std::size_t i=0;i<prepared_.count;++i){
        const auto& c=prepared_.stages[i];
#if defined(_M_X64) || defined(__SSE2__)
        const auto b0=_mm_set1_pd(c.b0),b1=_mm_set1_pd(c.b1),b2=_mm_set1_pd(c.b2);
        const auto a1=_mm_set1_pd(c.a1),a2=_mm_set1_pd(c.a2);
        auto z1=_mm_set_pd(history_[1][i].z1,history_[0][i].z1);
        auto z2=_mm_set_pd(history_[1][i].z2,history_[0][i].z2);
        for(auto& frame:frames){
            const auto x=_mm_loadu_pd(frame.data());
            const auto y=_mm_add_pd(_mm_mul_pd(b0,x),z1);
            z1=_mm_add_pd(_mm_sub_pd(_mm_mul_pd(b1,x),_mm_mul_pd(a1,y)),z2);
            z2=_mm_sub_pd(_mm_mul_pd(b2,x),_mm_mul_pd(a2,y));
            _mm_storeu_pd(frame.data(),y);
        }
        _mm_store_sd(&history_[0][i].z1,z1);_mm_storeh_pd(&history_[1][i].z1,z1);
        _mm_store_sd(&history_[0][i].z2,z2);_mm_storeh_pd(&history_[1][i].z2,z2);
#else
        for(std::size_t channel=0;channel<2;++channel){
            auto h=history_[channel][i];
            for(auto& frame:frames){const double x=frame[channel],y=c.b0*x+h.z1;
                h.z1=(c.b1*x-c.a1*y)+h.z2;h.z2=c.b2*x-c.a2*y;frame[channel]=y;
            }
            history_[channel][i]=h;
        }
#endif
    }
    return true;
}
bool prepareDuck(const Settings& s,double rate,PreparedDuck& out) noexcept
{
    if(!valid(s)||!rateValid(rate))return false;
    const PreparedDuck p{std::exp(-1/(rate*.001*s.duckAttackMs)),std::exp(-1/(rate*.001*s.duckReleaseMs)),s.duckThresholdDb,s.duckRangeDb,rate};
    if(!admitted(p))return false;out=p;return true;
}
bool DuckDetector::prepare(const PreparedDuck& p) noexcept {if(!admitted(p))return false;prepared_=p;ready_=true;reset();return true;}
void DuckDetector::reset() noexcept {envelope_=attenuationDb_=0;}
double DuckDetector::tick(std::array<double,2> input) noexcept
{
    if(!ready_)return 1;
    const double level=std::max(std::abs(input[0]),std::abs(input[1])),a=level>envelope_?prepared_.attack:prepared_.release;
    envelope_=a*envelope_+(1-a)*level;
    if(prepared_.rangeDb==0){attenuationDb_=0;return 1;}
    const double envelopeDb=20*std::log10(std::max(envelope_,1e-15));
    attenuationDb_=std::min(prepared_.rangeDb,std::max(0.,envelopeDb-prepared_.thresholdDb));return std::pow(10.,-attenuationDb_/20);
}
}

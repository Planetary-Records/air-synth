#include "SpaceReverbTail.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#include <vector>
namespace airack::space::detail {
namespace {
struct Wide {double hi=0,lo=0;Wide()=default;Wide(double value):hi(value){}Wide(double h,double l):hi(h),lo(l){}double value() const{return hi+lo;}};
Wide sum(double a,double b){const double h=a+b,v=h-a;return {h,(a-(h-v))+(b-v)};}
Wide operator+(Wide a,Wide b){const auto s=sum(a.hi,b.hi);return sum(s.hi,s.lo+a.lo+b.lo);}
Wide operator-(Wide a){return {-a.hi,-a.lo};}
Wide operator-(Wide a,Wide b){return a+(-b);}
Wide operator*(Wide a,Wide b){const double h=a.hi*b.hi,l=std::fma(a.hi,b.hi,-h)+a.hi*b.lo+a.lo*b.hi+a.lo*b.lo;return sum(h,l);}
Wide operator/(Wide a,Wide b){const double q=a.hi/b.hi;const auto remainder=a-b*Wide(q);return sum(q,remainder.value()/b.hi);}
Wide squareRoot(Wide a){const double root=std::sqrt(a.hi);if(root==0)return {};return Wide(root)+(a-Wide(root)*Wide(root))/Wide(2*root);}
bool zero(Wide a){return a.hi==0&&a.lo==0;}
std::array<Wide,3> powerPolynomial(Wide a,Wide b,Wide c)
{
    const Wide all=a+b+c;
    return {all*all,Wide(-4)*(b*(a+c)+Wide(4)*a*c),Wide(16)*a*c};
}
struct SectionBound {
    std::array<Wide,3> numerator,denominator;
    std::array<std::pair<double,double>,2> extrema{};
    std::size_t count=0;
};
double responseDb(const SectionBound& section,Wide u)
{
    const auto& n=section.numerator;const auto& d=section.denominator;
    const double numerator=((n[2]*u+n[1])*u+n[0]).value(),denominator=((d[2]*u+d[1])*u+d[0]).value();
    if(!std::isfinite(numerator)||!std::isfinite(denominator)||denominator<=0)return std::numeric_limits<double>::quiet_NaN();
    return 10*std::log10(std::max(0.,numerator)/denominator);
}
double responseDb(const SectionBound& section,double frequency,double rate)
{const Wide sine(std::sin(std::numbers::pi*frequency/rate));return responseDb(section,sine*sine);}
bool validDb(double db){return !std::isnan(db)&&db!=std::numeric_limits<double>::infinity();}
bool prepareBound(const Biquad& original,double radius,double rate,SectionBound& result)
{
    const Wide r(radius),r2=r*r;
    result.numerator=powerPolynomial(Wide(original.b0),Wide(original.b1)/r,Wide(original.b2)/r2);
    result.denominator=powerPolynomial(Wide(1),Wide(original.a1)/r,Wide(original.a2)/r2);
    const auto& n=result.numerator;const auto& d=result.denominator;
    const Wide a=n[2]*d[1]-n[1]*d[2],b=Wide(2)*(n[2]*d[0]-n[0]*d[2]),c=n[1]*d[0]-n[0]*d[1];
    auto add=[&](Wide root){const double u=root.value();if(!std::isfinite(u))return false;if(u<0||u>1)return true;
        const double frequency=rate/std::numbers::pi*std::asin(std::sqrt(u)),db=responseDb(result,root);
        if(!validDb(db))return false;result.extrema[result.count++]={frequency,db};return true;};
    if(zero(a)){return zero(b)||add((-c)/b);}
    const Wide discriminant=b*b-Wide(4)*a*c;
    if(discriminant.hi<0)return true;
    if(zero(discriminant))return add((-b)/(Wide(2)*a));
    auto root=squareRoot(discriminant);if(b.hi<0)root=-root;
    const Wide q=Wide(-.5)*(b+root);
    if(zero(q))return false;
    return add(q/a)&&add(c/q);
}
struct BankBound {double scalarDb=0;std::vector<SectionBound> stages;};
}
TailCertificate certifyTail(const LossNetwork& network,double timeBound)
{
    network.control.checkpoint();
    TailCertificate result;result.timeBound=timeBound;result.reason="invalid_input";
    const auto count=network.lines.size();const double rate=network.sampleRate;
    if(count<1||count>16||network.mixing.size()!=count*count||!std::isfinite(rate)||rate<8000||rate>768000
        ||!std::isfinite(timeBound)||timeBound<=0||timeBound>1000)return result;
    const double radius=std::exp(-std::log(1000.)/(rate*timeBound));result.radius=radius;
    double gramBound=0;
    for(std::size_t i=0;i<count;++i){double row=0;for(std::size_t j=0;j<count;++j){double gram=0;
        for(std::size_t k=0;k<count;++k){const double x=network.mixing[k*count+i],y=network.mixing[k*count+j];if(!std::isfinite(x)||!std::isfinite(y))return result;gram+=x*y;}
        row+=std::abs(gram);}gramBound=std::max(gramBound,row);}
    const double norm=std::sqrt(gramBound);result.reason="mixing_norm";if(!std::isfinite(norm)||norm>1+1e-12)return result;
    std::vector<BankBound> banks; banks.reserve(count);
    for(const auto& line:network.lines){
        network.control.checkpoint();
        if(network.poll)network.poll();
        result.reason="invalid_line";
        if(!line.loss||!std::isfinite(line.delaySeconds)||line.delaySeconds<=0||!std::isfinite(line.loss->scalar)||line.loss->scalar<=0||line.loss->stages.size()>56)return result;
        BankBound bank;bank.scalarDb=20*std::log10(line.loss->scalar)-20*line.delaySeconds*rate*std::log10(radius)+(norm>0?20*std::log10(norm):0)+1e-10;
        for(const auto& section:line.loss->stages){
            network.control.checkpoint();
            result.reason="invalid_coefficients";
            if(!std::isfinite(section.b0)||!std::isfinite(section.b1)||!std::isfinite(section.b2)||!std::isfinite(section.a1)||!std::isfinite(section.a2))return result;
            const double pole=poleRadius(section);result.maxInternalPole=std::max(result.maxInternalPole,pole);result.reason="internal_pole";
            if(!std::isfinite(pole)||pole>=radius)return result;
            SectionBound prepared;result.reason="section_extrema";if(!prepareBound(section,radius,rate,prepared))return result;
            bank.stages.push_back(prepared);
        }
        banks.push_back(std::move(bank));
    }
    if(norm==0){result.passed=true;result.maxLoopMagnitude=0;result.reason="pass";return result;}
    std::vector<std::pair<double,double>> pending;
    const double upper=std::min(20000.,.45*rate);pending.emplace_back(0,20);pending.emplace_back(upper,.5*rate);
    for(int i=0;i<128;++i)pending.emplace_back(20*std::pow(upper/20,i/128.),20*std::pow(upper/20,(i+1)/128.));
    double accepted=-std::numeric_limits<double>::infinity();
    while(!pending.empty()){
        network.control.checkpoint();
        if(network.poll)network.poll();
        const auto [low,high]=pending.back();pending.pop_back();result.reason="interval_limit";if(++result.intervals>131072)return result;
        double bound=-std::numeric_limits<double>::infinity();
        for(const auto& bank:banks){double line=bank.scalarDb;
            for(const auto& stage:bank.stages){
                const double a=responseDb(stage,low,rate),b=responseDb(stage,high,rate);result.reason="nonfinite_bound";
                if(!validDb(a)||!validDb(b))return result;double maximum=std::max(a,b);
                for(std::size_t i=0;i<stage.count;++i)if(stage.extrema[i].first>=low&&stage.extrema[i].first<=high)maximum=std::max(maximum,stage.extrema[i].second);
                line+=maximum+1e-6;
            }
            bound=std::max(bound,line);
        }
        result.reason="nonfinite_bound";if(!validDb(bound))return result;
        if(bound<=-1e-7){accepted=std::max(accepted,bound);continue;}
        const double middle=low>0?std::sqrt(low*high):.5*high;
        for(const auto& bank:banks){double actual=bank.scalarDb;
            for(const auto& stage:bank.stages)actual+=responseDb(stage,middle,rate);
            result.reason="weighted_loop_gain";
            if(!validDb(actual)||actual>=-1e-7){result.rejectedHz=middle;result.maxLoopMagnitude=std::pow(10.,actual/20);return result;}
        }
        result.reason="frequency_resolution";if(middle<=low||middle>=high)return result;
        pending.emplace_back(low,middle);pending.emplace_back(middle,high);
    }
    result.maxLoopMagnitude=std::pow(10.,accepted/20);result.passed=result.maxLoopMagnitude<1;result.reason=result.passed?"pass":"loop_margin";return result;
}
SectionMagnitudeBound boundSectionMagnitude(const Biquad& section,double rate,double radius) noexcept {
    SectionMagnitudeBound result;
    if(!std::isfinite(rate)||rate<8000||rate>768000||!std::isfinite(radius)||radius<=0||radius>1
       ||!std::isfinite(section.b0)||!std::isfinite(section.b1)||!std::isfinite(section.b2)||!std::isfinite(section.a1)||!std::isfinite(section.a2))return result;
    result.maximumPole=poleRadius(section);if(!std::isfinite(result.maximumPole)||result.maximumPole>=radius)return result;
    SectionBound prepared;if(!prepareBound(section,radius,rate,prepared))return result;
    const double dc=responseDb(prepared,Wide(0)),nyquist=responseDb(prepared,Wide(1));
    if(!validDb(dc)||!validDb(nyquist))return result;
    double maximum=std::max(dc,nyquist);
    for(std::size_t i=0;i<prepared.count;++i)maximum=std::max(maximum,prepared.extrema[i].second);
    result.maximumLog=(maximum+1e-6)*std::log(10.)/20;
    result.passed=std::isfinite(result.maximumLog);return result;
}

}

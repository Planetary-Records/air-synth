#include "SpaceReverbLoss.h"
#include "SpaceReverbTail.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <numeric>

namespace airack::space::detail
{
namespace
{
bool bounded(double x,double lo,double hi) noexcept {return std::isfinite(x)&&x>=lo&&x<=hi;}
std::optional<DecayPoint> evaluateTarget(const DecayCurve& curve,double frequency,double physicalRate) noexcept
{return ::airack::space::evaluate(curve,frequency,std::min(physicalRate,384000.));}
Biquad invalid() noexcept {Biquad b;b.b0=std::numeric_limits<double>::quiet_NaN();return b;}
Biquad normalized(double b0,double b1,double b2,double a0,double a1,double a2) noexcept
{return {b0/a0,b1/a0,b2/a0,a1/a0,a2/a0};}
std::complex<double> response(const Biquad& b,std::complex<double> z) noexcept
{return (b.b0+b.b1*z+b.b2*z*z)/(1.+b.a1*z+b.a2*z*z);}
struct Spec {double frequency,q;int shape;};
struct SolverOptions {
    double refinementFrequency=0;
    const LossBank* seed=nullptr;
    bool balanced=false;
    double refinementQ=2;
    bool feasibility=false;
    bool fastBoundary=false;
    bool tailPoleBound=false;
    double boundaryOutsideTime=40;
    double feasibilityTolerance=.085;
    bool highBoundary=false;
    bool analyticBells=false;
    PreparationControl control;
};
Biquad coefficients(const Spec& s,double cut,double rate) noexcept
{return s.shape==0?peakCut(s.frequency,s.q,cut,rate):shelfCut(s.frequency,s.shape==2,cut,rate);}
constexpr size_t validationPoints=769;
double timeResidual(double actual,double desired,bool feasibility,double tolerance=.085) noexcept {
    const double residual=desired/actual-1;
    return feasibility?std::copysign(std::max(0.,std::abs(residual)-tolerance),residual):residual;
}
template<size_t trainingPoints> double cost(const std::array<double,trainingPoints>& total,const std::array<double,trainingPoints>& desired,const std::array<double,trainingPoints>& weights,bool feasibility=false,double tolerance=.085) noexcept
{
    double result=0;
    for(size_t i=0;i<trainingPoints;++i){
        double d;if constexpr(trainingPoints==899){if(i>=769)d=std::max(0.,1-total[i]/desired[i]);else {if(total[i]<=0)return std::numeric_limits<double>::infinity();d=timeResidual(total[i],desired[i],feasibility,tolerance);}}
        else {if(total[i]<=0)return std::numeric_limits<double>::infinity();d=desired[i]/total[i]-1;}
        result+=weights[i]*d*d;
    }
    return result;
}
}
double Biquad::tick(double x) noexcept
{const double y=b0*x+z1;z1=b1*x-a1*y+z2;z2=b2*x-a2*y;return y;}
double LossBank::tick(double x) noexcept
{for(auto& s:stages)x=s.tick(x);return scalar*x;}
bool certifyLoss(const LossBank& bank,double rate,double& maximum,std::size_t& examined,double* rejectedHz,PreparationControl control)
{
    control.checkpoint();
    maximum=1;examined=0;
    if(!bounded(rate,8000,768000)||!bounded(bank.scalar,1e-100,1)||bank.scalar>=1||bank.stages.size()>56)return false;
    for(const auto& b:bank.stages){
        control.checkpoint();
        const auto expected=b.shape==0?peakCut(b.frequency,b.q,b.cutDb,rate):shelfCut(b.frequency,b.shape==2,b.cutDb,rate);
        if(b.shape<0||b.shape>2||!std::isfinite(expected.b0)||b.b0!=expected.b0||b.b1!=expected.b1
            ||b.b2!=expected.b2||b.a1!=expected.a1||b.a2!=expected.a2||!(poleRadius(b)<1))return false;
    }
    const double upper=std::min(20000.,.45*rate), scalarDb=20*std::log10(bank.scalar);
    std::vector<std::pair<double,double>> pending;
    pending.emplace_back(0,20);pending.emplace_back(upper,.5*rate);
    for(int i=0;i<128;++i)pending.emplace_back(20*std::pow(upper/20,i/128.),20*std::pow(upper/20,(i+1)/128.));
    double acceptedMax=-std::numeric_limits<double>::infinity();
    while(!pending.empty()){
        control.checkpoint();
        const auto [lo,hi]=pending.back();pending.pop_back();
        if(++examined>131072)return false;
        double bound=scalarDb;
        for(const auto& b:bank.stages){
            const auto zlo=std::polar(1.,-2*std::numbers::pi*lo/rate),zhi=std::polar(1.,-2*std::numbers::pi*hi/rate);
            double m=std::max(20*std::log10(std::abs(response(b,zlo))),20*std::log10(std::abs(response(b,zhi))));
            if(b.shape==0&&b.frequency>=lo&&b.frequency<=hi)m=std::max(m,-b.cutDb);
            bound+=m+1e-8;
        }
        if(!std::isfinite(bound))return false;
        if(bound<=-1e-7){acceptedMax=std::max(acceptedMax,bound);continue;}
        const double middle=lo>0?std::sqrt(lo*hi):.5*hi;
        if(middle<=lo||middle>=hi)return false;
        // A sampled violation is a rejection; subdivisions cannot repair it.
        if(magnitude(bank,middle,rate)>=1){if(rejectedHz)*rejectedHz=middle;return false;}
        pending.emplace_back(lo,middle);pending.emplace_back(middle,hi);
    }
    maximum=std::pow(10.,acceptedMax/20);return maximum<1;
}
Biquad peakCut(double f,double q,double cut,double rate) noexcept
{
    if(!bounded(rate,8000,768000)||!bounded(f,1,.499*rate)||!bounded(q,.1,24)||!bounded(cut,-80,80))return invalid();
    const double w=2*std::numbers::pi*f/rate, A=std::pow(10.,-cut/40);
    const double c=std::cos(w),alpha=std::sin(w)/(2*q);
    auto b=normalized(1+alpha*A,-2*c,1-alpha*A,1+alpha/A,-2*c,1-alpha/A);
    b.frequency=f;b.q=q;b.cutDb=cut;return b;
}
Biquad shelfCut(double f,bool high,double cut,double rate) noexcept
{
    if(!bounded(rate,8000,768000)||!bounded(f,1,.499*rate)||!bounded(cut,-80,80))return invalid();
    const double w=2*std::numbers::pi*f/rate,A=std::pow(10.,-cut/40),c=std::cos(w);
    const double beta=std::sqrt(2*A)*std::sin(w),p=A+1,m=A-1;
    Biquad b=high?normalized(A*(p+m*c+beta),-2*A*(m+p*c),A*(p+m*c-beta),p-m*c+beta,2*(m-p*c),p-m*c-beta)
        :normalized(A*(p-m*c+beta),2*A*(m-p*c),A*(p-m*c-beta),p+m*c+beta,-2*(m+p*c),p+m*c-beta);
    b.frequency=f;b.cutDb=cut;b.shape=high?2:1;return b;
}
double poleRadius(const Biquad& b) noexcept
{
    const auto discriminant=std::sqrt(std::complex<double>(b.a1*b.a1-4*b.a2,0));
    return std::max(std::abs((-b.a1+discriminant)*.5),std::abs((-b.a1-discriminant)*.5));
}
double magnitude(const LossBank& b,double f,double rate) noexcept
{
    const auto z=std::polar(1.,-2*std::numbers::pi*f/rate);
    auto h=std::complex<double>(b.scalar);
    for(const auto& s:b.stages)h*=response(s,z);
    return std::abs(h);
}
double nominalTime(const LossBank& b,double f,double rate,double seconds) noexcept
{return -60*seconds/(20*std::log10(magnitude(b,f,rate)));}

template<size_t trainingPoints> FitResult fitLossImpl(const DecayCurve& curve,double rate,double delay,FitGeometry geometry,FitDomain domain,FitVariant variant,const SolverOptions& options={})
{
    const auto [refinementFrequency,seed,balanced,refinementQ,feasibility,fastBoundary,tailPoleBound,boundaryOutsideTime,feasibilityTolerance,highBoundary,analyticBells,control]=options;
    control.checkpoint();
    using Samples=std::array<double,trainingPoints>;
    FitResult result;result.domain=domain;
    if(!valid(curve)||!bounded(rate,8000,768000)||!bounded(delay,1e-5,1)
        ||(geometry!=FitGeometry::compact&&geometry!=FitGeometry::dense)||(domain!=FitDomain::unitCircle&&domain!=FitDomain::poleContour)
        ||(variant!=FitVariant::original&&variant!=FitVariant::edgeBandwidth))return result;
    const double upper=std::min(20000.,.45*rate);
    std::array<double,8> anchors{};
    const auto trainingHz=[&](size_t i){
        if constexpr(trainingPoints==899){
            if(i<769)return 20*std::pow(upper/20,double(i)/768);
            if(i<777&&anchors[i-769]>0)return anchors[i-769];
            if(i<833)return 20*double(i-769+1)/65;
            if(i<897)return upper+(.5*rate-upper)*double(i-833+1)/65;
            return i==897?0:.5*rate;
        } else return i==trainingPoints-2?0:i==trainingPoints-1?.5*rate:20*std::pow(upper/20,double(i)/double(trainingPoints-3));
    };
    std::array<std::complex<double>,trainingPoints> zs;
    Samples desired;
    double longest=.1;
    for(size_t i=0;i<trainingPoints;++i){
        control.checkpoint();
        const double f=trainingHz(i);
        const auto target=evaluateTarget(curve,std::clamp(f,20.,upper),rate);
        longest=std::max(longest,target->seconds);
        const double trainingTime=trainingPoints==899&&i>=769?46:target->seconds;
        desired[i]=60*delay/trainingTime;
        const double inverseRadius=domain==FitDomain::poleContour?std::exp(std::log(1000.)/(rate*trainingTime)):1;
        zs[i]=std::polar(inverseRadius,-2*std::numbers::pi*f/rate);
    }
    for(const auto& band:curve.bands)if(band.active&&!band.bypassed)
        longest=std::max(longest,evaluateTarget(curve,std::min(band.frequencyHz,upper),rate)->seconds);
    if constexpr(trainingPoints==899)for(size_t j=769;j<trainingPoints;++j)desired[j]=60*delay/(fastBoundary?boundaryOutsideTime:longest);
    const double scalarLoss=60*delay/longest;
    result.bank.scalar=std::pow(10.,-scalarLoss/20);
    std::vector<Spec> specs;
    const int count=geometry==FitGeometry::compact?(variant==FitVariant::edgeBandwidth?14:18):36;
    for(int i=0;i<count;++i)specs.push_back({20*std::pow(upper/20,double(i)/(count-1)),geometry==FitGeometry::compact?1.5:3.,0});
    specs.push_back({std::min(40.,upper),1,1});
    specs.push_back({std::min(12000.,upper),1,2});
    std::vector<Spec> adaptive;
    auto appendUnique=[](std::vector<Spec>& list,Spec value){
        for(const auto& previous:list)
            if(previous.shape==value.shape&&std::abs(previous.frequency/value.frequency-1)<1e-10&&std::abs(previous.q-value.q)<1e-10)return;
        list.push_back(value);
    };
    const std::size_t adaptiveLimit=variant==FitVariant::edgeBandwidth?22:18;
    auto addAdaptive=[&](double f,double q){if(adaptive.size()<adaptiveLimit)appendUnique(adaptive,{std::clamp(f,20.,upper),q,0});};
    if(curve.brightness<1){appendUnique(adaptive,{std::min(4000.,upper),1,2});appendUnique(adaptive,{upper,1,2});}
    double previousHz=20;bool previousLimited=evaluateTarget(curve,20,rate)->decayLimited;
    int knees=0;
    for(int i=1;i<193;++i){
        const double hz=20*std::pow(upper/20,i/192.);
        const bool limited=evaluateTarget(curve,hz,rate)->decayLimited;
        if(limited!=previousLimited&&knees<4){
            double lo=previousHz,hi=hz;
            for(int iteration=0;iteration<40;++iteration){const double mid=std::sqrt(lo*hi);if(evaluateTarget(curve,mid,rate)->decayLimited==previousLimited)lo=mid;else hi=mid;}
            const double knee=std::sqrt(lo*hi);addAdaptive(knee,3);addAdaptive(knee,8);addAdaptive(knee,20);++knees;
        }
        previousHz=hz;previousLimited=limited;
    }
    for(const auto& band:curve.bands)if(band.active&&!band.bypassed){
        const double q=std::clamp(band.q,.5,12.);
        if(variant==FitVariant::edgeBandwidth&&band.shape==DecayShape::bell&&band.frequencyHz<=20){
            for(const auto spec:std::array<Spec,7>{{{14,1,0},{18,2,0},{18,4,0},{20,2,0},{20,4,0},{22,2,0},{26,2,0}}})if(adaptive.size()<adaptiveLimit)appendUnique(adaptive,spec);
            continue;
        }
        for(int side=-1;side<=1;++side)addAdaptive(band.frequencyHz*std::pow(2.,side*.5/q),side==0?q:std::min(2*q,12.));
    }
    for(const auto& spec:adaptive)appendUnique(specs,spec);
    if(balanced&&!seed){
        specs.clear();
        for(int i=0;i<6;++i)specs.push_back({20*std::pow(upper/20,i/5.),1.5,0});
        specs.push_back({std::min(40.,upper),1,1});specs.push_back({std::min(12000.,upper),1,2});
        const auto add=[&](double f,double q){if(specs.size()<38)appendUnique(specs,{std::clamp(f,20.,upper),q,0});};
        for(const auto& band:curve.bands)if(band.active&&!band.bypassed)add(band.frequencyHz,std::clamp(band.q,.5,12.));
        std::vector<double> crossings;double previous=20;bool wasLimited=evaluateTarget(curve,20,rate)->decayLimited;
        for(int i=1;i<193;++i){const double hz=20*std::pow(upper/20,i/192.);const bool limited=evaluateTarget(curve,hz,rate)->decayLimited;
            if(limited!=wasLimited){double lo=previous,hi=hz;for(int j=0;j<40;++j){const double mid=std::sqrt(lo*hi);if(evaluateTarget(curve,mid,rate)->decayLimited==wasLimited)lo=mid;else hi=mid;}crossings.push_back(std::sqrt(lo*hi));}
            previous=hz;wasLimited=limited;}
        for(double f:crossings)add(f,3);
        for(const auto& band:curve.bands)if(band.active&&!band.bypassed){const double q=std::clamp(band.q,.5,12.);for(int side:{-1,1})add(band.frequencyHz*std::pow(2.,side*.5/q),std::min(2*q,12.));}
        for(double f:crossings)add(f,8);
        if(curve.brightness<1&&specs.size()<38)appendUnique(specs,{std::min(4000.,upper),1,2});
    }
    double shortest=40;
    if(fastBoundary){
        for(int i=0;i<193;++i)shortest=std::min(shortest,evaluateTarget(curve,20*std::pow(upper/20,i/192.),rate)->seconds);
        const auto fastQ=[&](double frequency){return std::max(.5,std::numbers::pi*frequency*.9*shortest/std::log(1000.));};
        specs.clear();for(int i=0;i<6;++i)specs.push_back({20*std::pow(upper/20,i/5.),.5,0});
        specs.push_back({std::min(40.,upper),1,1});specs.push_back({std::min(12000.,upper),1,2});
        for(double f:{20.,24.,28.,32.,40.,60.})for(int i=0;i<4;++i)specs.push_back({f,.5+(fastQ(f)-.5)*i/3.,0});
        double knee=28,previous=20;bool wasLimited=evaluateTarget(curve,20,rate)->decayLimited;bool found=false;
        for(int i=0;i<193;++i){const double hz=20*std::pow(upper/20,i/192.);const auto target=evaluateTarget(curve,hz,rate);shortest=std::min(shortest,target->seconds);
            if(!found&&target->decayLimited!=wasLimited){double lo=previous,hi=hz;for(int j=0;j<40;++j){const double mid=std::sqrt(lo*hi);if(evaluateTarget(curve,mid,rate)->decayLimited==wasLimited)lo=mid;else hi=mid;}knee=std::sqrt(lo*hi);found=true;}
            previous=hz;wasLimited=target->decayLimited;}
        for(int i=0;i<6;++i)specs.push_back({knee,.5+(fastQ(knee)-.5)*i/5.,0});
    }
    if(highBoundary){
        // Keep the six broad bells/two shelves; concentrate the other thirty
        // sections around the last entry into the high-frequency minimum clamp.
        double knee=.85*upper,previous=20;bool wasMinimum=evaluateTarget(curve,20,rate)->seconds==.1;
        for(int i=1;i<193;++i){control.checkpoint();const double hz=20*std::pow(upper/20,i/192.);const bool minimum=evaluateTarget(curve,hz,rate)->seconds==.1;
            if(!wasMinimum&&minimum){double lo=previous,hi=hz;for(int j=0;j<40;++j){const double mid=std::sqrt(lo*hi);if(evaluateTarget(curve,mid,rate)->seconds==.1)hi=mid;else lo=mid;}knee=std::sqrt(lo*hi);}
            previous=hz;wasMinimum=minimum;}
        specs.resize(8);
        for(double scale:{.85,.925,1.,1.04,1.1})for(double q:{.5,1.,2.,4.,8.,16.})specs.push_back({std::clamp(knee*scale,20.,upper),q,0});
    }
    if(variant==FitVariant::edgeBandwidth)for(auto& spec:specs)if(spec.shape==0){const double w=2*std::numbers::pi*spec.frequency/rate;spec.q=std::max(.1,spec.q*std::sin(w)/w);}
    if(seed&&seed->stages.size()<=38&&(feasibility||(refinementFrequency>40&&refinementFrequency<upper/1.05))){
        specs.clear();size_t weakest=0;double least=std::numeric_limits<double>::infinity();
        for(const auto& section:seed->stages){if(std::abs(section.cutDb)<least){least=std::abs(section.cutDb);weakest=specs.size();}specs.push_back({section.frequency,section.q,section.shape});}
        if(refinementFrequency>=20&&refinementFrequency<upper/1.05){const double w=2*std::numbers::pi*refinementFrequency/rate;const Spec extra{refinementFrequency,std::max(.1,refinementQ*std::sin(w)/w),0};
            if(specs.size()<38)specs.push_back(extra);else specs[weakest]=extra;}
    }
    const size_t n=specs.size();
    std::vector<double> minimum(n,-80),maximum(n,80);
    if(variant==FitVariant::edgeBandwidth){const double poleTime=fastBoundary&&!tailPoleBound?std::min(45.,.95*shortest):45.;const double radius=std::exp(-std::log(1000.)/(rate*poleTime));
        for(size_t s=0;s<n;++s){
            if(!(poleRadius(coefficients(specs[s],-80,rate))<radius)){double low=-80,high=0;for(int i=0;i<64;++i){const double middle=(low+high)*.5;if(poleRadius(coefficients(specs[s],middle,rate))<radius)high=middle;else low=middle;}minimum[s]=std::min(0.,high+1e-6);}
            if(!(poleRadius(coefficients(specs[s],80,rate))<radius)){double low=0,high=80;for(int i=0;i<64;++i){const double middle=(low+high)*.5;if(poleRadius(coefficients(specs[s],middle,rate))<radius)low=middle;else high=middle;}maximum[s]=std::max(0.,low-1e-6);}
        }
    }
    const auto minimumAmount=[&](size_t s){return minimum[s];};
    std::vector<double> amounts(n,0),step(n),matrix(n*n),rhs(n),gradient(n);
    std::vector<bool> consumed(seed?seed->stages.size():0,false);
    if(seed)for(size_t i=0;i<n;++i)for(size_t j=0;j<seed->stages.size();++j){const auto& section=seed->stages[j];
        if(!consumed[j]&&section.shape==specs[i].shape&&section.frequency==specs[i].frequency&&section.q==specs[i].q){amounts[i]=std::clamp(section.cutDb,minimum[i],maximum[i]);consumed[j]=true;break;}}
    std::vector<Samples> losses(n),jacobian(n);
    Samples total;total.fill(scalarLoss);
    auto stageLoss=[&](size_t s,double amount){
        control.checkpoint();
        Samples out;const auto b=coefficients(specs[s],amount,rate);
        for(size_t j=0;j<trainingPoints;++j)out[j]=-20*std::log10(std::abs(response(b,zs[j])));
        return out;
    };
    Samples weights;weights.fill(1);
    const auto objective=[&](const Samples& values){return cost(values,desired,weights,feasibility,feasibilityTolerance);};
    double current=objective(total);
    const bool continuation=variant==FitVariant::edgeBandwidth&&domain==FitDomain::poleContour;
    std::vector<double> bestAmounts;double bestMaximum=std::numeric_limits<double>::infinity();
    const auto remember=[&](){if constexpr(trainingPoints==899){double worst=0,shortfall=0;
        for(size_t j=0;j<769;++j)worst=std::max(worst,std::abs(desired[j]/total[j]-1));
        for(size_t j=769;j<trainingPoints;++j)shortfall=std::max(shortfall,1-total[j]/desired[j]);
        if(shortfall<=.01&&std::isfinite(worst)&&worst<bestMaximum){bestMaximum=worst;bestAmounts=amounts;}
    }};
    for(int phase=continuation&&seed?10:0;phase<(continuation?19:1);++phase){
    control.checkpoint();
    if(continuation){
        for(size_t j=0;j<trainingPoints;++j){const double f=trainingHz(j);
            const double target=trainingPoints==899&&j>=769?46:evaluateTarget(curve,std::clamp(f,20.,upper),rate)->seconds;
            zs[j]=std::polar(std::exp(std::min(1.,phase/10.)*std::log(1000.)/(rate*target)),-2*std::numbers::pi*f/rate);}
        total.fill(scalarLoss);for(size_t s=0;s<n;++s){const auto loss=stageLoss(s,amounts[s]);for(size_t j=0;j<trainingPoints;++j)total[j]+=loss[j];}
        current=objective(total);
    }
    double damping=1e-3;
    for(int iteration=0;iteration<(variant==FitVariant::original?40:160)&&current>1e-16;++iteration){
        control.checkpoint();
        result.iterations=iteration+1;
        if(continuation&&phase>=10){remember();if(bestMaximum<.08)break;}
        weights.fill(1);
        if constexpr(trainingPoints==899){if(!feasibility){
            double maximumResidual=1e-12;if(seed)for(size_t j=0;j<769;++j)maximumResidual=std::max(maximumResidual,std::abs(desired[j]/total[j]-1));
            for(size_t j=0;j<769;++j){const double residual=desired[j]/total[j]-1;
                if(seed){const double ratio=std::min(1.,std::abs(residual)/maximumResidual),square=ratio*ratio;weights[j]=1+100*square*square*square;}
                else weights[j]=1+10000*residual*residual;
            }
        }}
        current=objective(total);
        for(size_t s=0;s<n;++s){
            losses[s]=stageLoss(s,amounts[s]);
            if((fastBoundary||analyticBells)&&specs[s].shape==0){
                const double w=2*std::numbers::pi*specs[s].frequency/rate,alpha=std::sin(w)/(2*specs[s].q),a=std::pow(10.,-amounts[s]/40),halfSine=std::sin(w*.5);
                for(size_t j=0;j<trainingPoints;++j){const auto z=zs[j],u=1.-z*z,b=(1.-z)*(1.-z)+4*halfSine*halfSine*z;
                    const auto numerator=alpha*a*u,denominator=alpha/a*u;
                    const double derivative=.5*std::real(numerator/(b+numerator)+denominator/(b+denominator));
                    if(j>=769){jacobian[s][j]=total[j]<desired[j]?-derivative/desired[j]:0;continue;}
                    jacobian[s][j]=feasibility&&std::abs(desired[j]/total[j]-1)<=feasibilityTolerance?0:-derivative*desired[j]*std::sqrt(weights[j])/(total[j]*total[j]);
                }
                continue;
            }
            const double epsilon=amounts[s]<79.99?.01:-.01;
            const auto perturbed=stageLoss(s,amounts[s]+epsilon);
            for(size_t j=0;j<trainingPoints;++j){
                if constexpr(trainingPoints==899){if(j>=769){jacobian[s][j]=total[j]<desired[j]?-(perturbed[j]-losses[s][j])*std::sqrt(weights[j])/(epsilon*desired[j]):0;continue;}}
                if(feasibility&&std::abs(desired[j]/total[j]-1)<=feasibilityTolerance){jacobian[s][j]=0;continue;}
                jacobian[s][j]=-(perturbed[j]-losses[s][j])*desired[j]*std::sqrt(weights[j])/(epsilon*total[j]*total[j]);
            }
        }
        for(size_t s=0;s<n;++s){
            rhs[s]=0;
            control.checkpoint();
            for(size_t j=0;j<trainingPoints;++j){const double residual=trainingPoints==899&&j>=769?std::max(0.,1-total[j]/desired[j]):timeResidual(total[j],desired[j],feasibility,feasibilityTolerance);
                rhs[s]-=jacobian[s][j]*std::sqrt(weights[j])*residual;}
            for(size_t t=0;t<=s;++t){
                double value=0;
                const auto* const rowS=jacobian[s].data();
                const auto* const rowT=jacobian[t].data();
                for(size_t j=0;j<trainingPoints;++j)value+=rowS[j]*rowT[j];
                if(s==t)value+=1e-8;
                matrix[s*n+t]=matrix[t*n+s]=value;
            }
        }
        // Solve the regularized coupled quadratic with partial pivoting.
        auto system=matrix;auto solution=rhs;
        if(variant==FitVariant::edgeBandwidth)for(size_t s=0;s<n;++s)system[s*n+s]+=damping*std::max(1e-8,matrix[s*n+s]);
        bool solvable=true;
        for(size_t column=0;column<n;++column){
            control.checkpoint();
            size_t pivot=column;
            for(size_t row=column+1;row<n;++row)if(std::abs(system[row*n+column])>std::abs(system[pivot*n+column]))pivot=row;
            if(std::abs(system[pivot*n+column])<1e-18){solvable=false;break;}
            if(pivot!=column){for(size_t k=column;k<n;++k)std::swap(system[pivot*n+k],system[column*n+k]);std::swap(solution[pivot],solution[column]);}
            for(size_t row=column+1;row<n;++row){
                const double factor=system[row*n+column]/system[column*n+column];system[row*n+column]=0;
                for(size_t k=column+1;k<n;++k)system[row*n+k]-=factor*system[column*n+k];
                solution[row]-=factor*solution[column];
            }
        }
        if(!solvable)break;
        std::fill(step.begin(),step.end(),0);
        for(size_t row=n;row-->0;){double value=solution[row];for(size_t k=row+1;k<n;++k)value-=system[row*n+k]*step[k];step[row]=value/system[row*n+row];}
        double largest=0;for(double value:step)largest=std::max(largest,std::abs(value));
        if(largest>6)for(double& value:step)value*=6/largest;
        for(size_t k=0;k<n;++k)step[k]=std::clamp(step[k],minimumAmount(k)-amounts[k],maximum[k]-amounts[k]);
        bool improved=false;
        for(double scale:{1.,.5,.25,.125,.0625}){
            control.checkpoint();
            Samples trial;trial.fill(scalarLoss);
            for(size_t s=0;s<n;++s){
                const auto loss=stageLoss(s,std::clamp(amounts[s]+scale*step[s],minimumAmount(s),maximum[s]));
                for(size_t j=0;j<trainingPoints;++j)trial[j]+=loss[j];
            }
            const double next=objective(trial);
            if(std::isfinite(next)&&next<current-1e-15){
                for(size_t s=0;s<n;++s)amounts[s]=std::clamp(amounts[s]+scale*step[s],minimumAmount(s),maximum[s]);
                total=trial;current=next;improved=true;break;
            }
        }
        if(variant==FitVariant::edgeBandwidth){if(improved)damping=std::max(1e-12,.3*damping);else {damping*=10;if(damping>1e12)break;}}
        else if(!improved)break;
    }
    if(continuation&&phase>=10){
        remember();if(!bestAmounts.empty())amounts=bestAmounts;
        LossBank candidate;candidate.scalar=result.bank.scalar;
        for(size_t s=0;s<n;++s)if(std::abs(amounts[s])>=1e-8)candidate.stages.push_back(coefficients(specs[s],amounts[s],rate));
        LossNetwork network;network.sampleRate=rate;network.control=control;network.lines.push_back({delay,&candidate});network.mixing.push_back(1);
        const auto tail=certifyTail(network,46);double maximum=1,rejected=0;size_t intervals=0;
        const bool passive=certifyLoss(candidate,rate,maximum,intervals,&rejected,control);
        if(tail.passed&&passive)break;
        if(!tail.passed)rejected=tail.rejectedHz;
        if(phase==18||!(rejected>0)||!std::isfinite(rejected))break;
        anchors[size_t(phase-10)]=rejected;bestMaximum=std::numeric_limits<double>::infinity();bestAmounts.clear();
    }
    }
    if(continuation){remember();if(!bestAmounts.empty())amounts=bestAmounts;}
    for(size_t s=0;s<n;++s)if(std::abs(amounts[s])>=1e-8){
        auto b=coefficients(specs[s],amounts[s],rate);
        const double pole=poleRadius(b);
        if(!std::isfinite(pole)||pole>=1)return result;
        result.maxPole=std::max(result.maxPole,pole);result.bank.stages.push_back(b);
    }
    result.maxRelativeError=result.rmsRelativeError=result.maxMagnitude=result.maxNominalRelativeError=0;
    for(size_t i=0;i<validationPoints;++i){
        control.checkpoint();
        const double f=20*std::pow(upper/20,double(i)/(validationPoints-1));
        const double target=evaluateTarget(curve,f,rate)->seconds;
        const double mag=magnitude(result.bank,f,rate),nominal=nominalTime(result.bank,f,rate,delay);
        double actual=nominal;
        if(domain==FitDomain::poleContour){
            const auto z=std::polar(std::exp(std::log(1000.)/(rate*target)),-2*std::numbers::pi*f/rate);
            auto h=std::complex<double>(result.bank.scalar);for(const auto& stage:result.bank.stages)h*=response(stage,z);
            actual=-60*delay/(20*std::log10(std::abs(h)));
        }
        const double error=std::isfinite(actual)?std::abs(actual/target-1):std::numeric_limits<double>::infinity();
        const double nominalError=std::isfinite(nominal)?std::abs(nominal/target-1):std::numeric_limits<double>::infinity();
        result.maxNominalRelativeError=std::max(result.maxNominalRelativeError,nominalError);
        result.maxMagnitude=std::max(result.maxMagnitude,mag);
        if(error>result.maxRelativeError)result.worstFrequencyHz=f;
        result.maxRelativeError=std::max(result.maxRelativeError,error);
        result.rmsRelativeError+=error*error;
    }
    result.rmsRelativeError=std::sqrt(result.rmsRelativeError/validationPoints);
    result.stabilityCertified=certifyLoss(result.bank,rate,result.certifiedMaxMagnitude,result.certificateIntervals,nullptr,control);
    result.converged=result.stabilityCertified&&std::isfinite(result.maxRelativeError)&&result.maxRelativeError<=.1
        &&result.maxPole<1&&result.maxMagnitude<1&&result.bank.stages.size()<=56;
    return result;
}
LossBank moveBoundaryFrequencies(const DecayCurve& curve,double rate,double delay,const LossBank& source,double outsideTime,PreparationControl control) {
    control.checkpoint();
    using Samples=std::array<double,899>;
    LossBank bank=source;Samples desired,total,weights;weights.fill(1);
    std::array<std::complex<double>,899> zs;
    const double upper=std::min(20000.,.45*rate);double shortest=40;
    for(size_t i=0;i<899;++i){
        control.checkpoint();
        const double frequency=i<769?20*std::pow(upper/20,i/768.):i<833?20*double(i-769+1)/65:i<897?upper+(.5*rate-upper)*double(i-833+1)/65:i==897?0:.5*rate;
        const double target=i<769?evaluateTarget(curve,frequency,rate)->seconds:46;
        if(i<769)shortest=std::min(shortest,target);
        desired[i]=60*delay/(i<769?target:outsideTime);
        zs[i]=std::polar(std::exp(std::log(1000.)/(rate*target)),-2*std::numbers::pi*frequency/rate);
    }
    const double radius=std::exp(-std::log(1000.)/(rate*.95*shortest));
    const auto loss=[&](const Biquad& section){Samples values;for(size_t j=0;j<899;++j)values[j]=-20*std::log10(std::abs(response(section,zs[j])));return values;};
    total.fill(-20*std::log10(bank.scalar));for(const auto& section:bank.stages){const auto values=loss(section);for(size_t j=0;j<899;++j)total[j]+=values[j];}
    double current=cost(total,desired,weights,true);
    for(int sweep=0;sweep<8;++sweep){bool changed=false;const double step=.01*std::pow(.5,sweep/2);
        control.checkpoint();
        for(auto& section:bank.stages){if(section.shape!=0||section.frequency>=100)continue;
            const auto old=loss(section);double best=current;Biquad selected=section;Samples selectedTotal=total;
            for(int direction:{-1,1}){
                control.checkpoint();
                const double f=section.frequency*(direction<0?1/(1+step):1+step);if(f<1||f>=std::min(100.,.499*rate))continue;
                const auto candidate=peakCut(f,section.q,section.cutDb,rate);if(!(poleRadius(candidate)<radius))continue;
                const auto values=loss(candidate);Samples trial;for(size_t j=0;j<899;++j)trial[j]=total[j]-old[j]+values[j];
                const double value=cost(trial,desired,weights,true);
                if(std::isfinite(value)&&value<best-1e-15){best=value;selected=candidate;selectedTotal=trial;}
            }
            if(best<current){section=selected;total=selectedTotal;current=best;changed=true;}
        }
        if(!changed&&sweep%2==1)break;
    }
    return bank;
}
FitResult fitLossWarm(const DecayCurve& curve,double rate,double delay,double previousDelay,const LossBank& previous,PreparationControl control) {
    control.checkpoint();
    if(!valid(curve)||!bounded(rate,8000,768000)||!bounded(delay,1e-5,1)||!bounded(previousDelay,1e-5,1)
       ||previous.stages.size()>38||!bounded(previous.scalar,1e-100,1))return {};
    auto seed=previous;
    for(auto& stage:seed.stages){
        if(stage.shape<0||stage.shape>2||!bounded(stage.frequency,1,.499*rate)||!bounded(stage.q,.1,24)||!bounded(stage.cutDb,-80,80))return {};
        stage.cutDb*=delay/previousDelay;
    }
    auto result=fitLossImpl<899>(curve,rate,delay,FitGeometry::compact,FitDomain::poleContour,FitVariant::edgeBandwidth,
        {.seed=&seed,.balanced=true,.feasibility=true,.control=control});
    const auto tailPassed=[&](const FitResult& fit){LossNetwork network;network.sampleRate=rate;network.control=control;network.lines.push_back({delay,&fit.bank});network.mixing.push_back(1);return certifyTail(network,46).passed;};
    if(result.converged&&!tailPassed(result))result.converged=false;
    if(!result.converged){
        auto refined=fitLossImpl<899>(curve,rate,delay,FitGeometry::compact,FitDomain::poleContour,FitVariant::edgeBandwidth,
            {.seed=&seed,.balanced=true,.feasibility=true,.analyticBells=true,.control=control});
        if(refined.stabilityCertified&&(refined.converged||refined.maxRelativeError<result.maxRelativeError)&&tailPassed(refined))result=std::move(refined);
    }
    if(!result.converged){
        auto zero=seed;
        for(auto& section:zero.stages){control.checkpoint();section=coefficients({section.frequency,section.q,section.shape},0,rate);}
        auto restarted=fitLossImpl<899>(curve,rate,delay,FitGeometry::compact,FitDomain::poleContour,FitVariant::edgeBandwidth,
            {.seed=&zero,.balanced=true,.feasibility=true,.analyticBells=true,.control=control});
        if(restarted.stabilityCertified&&(restarted.converged||restarted.maxRelativeError<result.maxRelativeError)&&tailPassed(restarted))result=std::move(restarted);
    }
    return result;
}
static FitResult fitLossCold(const DecayCurve& curve,double rate,double delay,FitGeometry geometry,FitDomain domain,FitVariant variant,PreparationControl control) {
    control.checkpoint();
    if(variant==FitVariant::original)return fitLossImpl<195>(curve,rate,delay,geometry,domain,variant,{.control=control});
    auto result=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,{.control=control});
    const double upper=std::min(20000.,.45*rate);
    if(!result.converged&&result.stabilityCertified&&result.worstFrequencyHz>40&&result.worstFrequencyHz<upper/1.05){
        auto refined=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
            {.refinementFrequency=result.worstFrequencyHz,.seed=&result.bank,.control=control});
        const auto annotate=[&](FitResult& selected){selected.refinementTried=true;selected.refinementCertified=refined.stabilityCertified;selected.refinementMaximumError=refined.maxRelativeError;};
        if(refined.converged||(refined.stabilityCertified&&refined.maxRelativeError<result.maxRelativeError)){annotate(refined);result=std::move(refined);}
        else annotate(result);
    }
    if(!result.converged){
        auto alternative=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,{.balanced=true,.control=control});
        if(!alternative.converged&&alternative.stabilityCertified&&alternative.worstFrequencyHz>40&&alternative.worstFrequencyHz<upper/1.05){
            auto refined=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
                {.refinementFrequency=alternative.worstFrequencyHz,.seed=&alternative.bank,.balanced=true,.control=control});
            if(refined.converged||(refined.stabilityCertified&&refined.maxRelativeError<alternative.maxRelativeError))alternative=std::move(refined);
        }
        if(alternative.converged||(alternative.stabilityCertified&&alternative.maxRelativeError<result.maxRelativeError))result=std::move(alternative);
    }
    for(int exchange=0;exchange<6&&!result.converged&&result.stabilityCertified&&result.worstFrequencyHz>40&&result.worstFrequencyHz<upper/1.05;++exchange){
        bool improved=false;
        for(double q:{2.,4.,8.}){
            auto refined=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
                {.refinementFrequency=result.worstFrequencyHz,.seed=&result.bank,.balanced=true,.refinementQ=q,.control=control});
            if(refined.converged||(refined.stabilityCertified&&refined.maxRelativeError<result.maxRelativeError-1e-6)){result=std::move(refined);improved=true;break;}
        }
        if(!improved)break;
    }
    if(!result.converged&&result.stabilityCertified){
        auto relaxed=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
            {.seed=&result.bank,.balanced=true,.feasibility=true,.control=control});
        if(relaxed.converged||(relaxed.stabilityCertified&&relaxed.maxRelativeError<result.maxRelativeError))result=std::move(relaxed);
        if(!result.converged&&result.worstFrequencyHz>40&&result.worstFrequencyHz<upper/1.05){
            auto refined=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
                {.refinementFrequency=result.worstFrequencyHz,.seed=&result.bank,.balanced=true,.refinementQ=1,.feasibility=true,.control=control});
            if(refined.converged||(refined.stabilityCertified&&refined.maxRelativeError<result.maxRelativeError))result=std::move(refined);
        }
    }
    if(!result.converged&&evaluateTarget(curve,20,rate)->seconds<.2){
      for(double initialOutsideTime:{40.,46.}){
        auto fast=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
            {.feasibility=true,.fastBoundary=true,.boundaryOutsideTime=initialOutsideTime,.control=control});
        for(int exchange=0;exchange<4&&!fast.converged&&fast.stabilityCertified&&fast.worstFrequencyHz>=20&&fast.worstFrequencyHz<upper/1.05;++exchange){
            const double minimumTarget=evaluateTarget(curve,20,rate)->seconds;
            const double q=std::clamp(std::numbers::pi*fast.worstFrequencyHz*.9*minimumTarget/std::log(1000.),.5,24.);
            auto refined=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
                {.refinementFrequency=fast.worstFrequencyHz,.seed=&fast.bank,.refinementQ=q,.feasibility=true,.fastBoundary=true,.boundaryOutsideTime=initialOutsideTime,.control=control});
            if(refined.converged||(refined.stabilityCertified&&refined.maxRelativeError<fast.maxRelativeError-1e-6))fast=std::move(refined);else break;
        }
        auto working=fast;
        for(int round=0;round<3&&!fast.converged&&working.stabilityCertified;++round){
            const auto moved=moveBoundaryFrequencies(curve,rate,delay,working.bank,initialOutsideTime,control);
            auto refined=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
                {.seed=&moved,.feasibility=true,.fastBoundary=true,.boundaryOutsideTime=initialOutsideTime,.control=control});
            LossNetwork network;network.sampleRate=rate;network.control=control;network.lines.push_back({delay,&refined.bank});network.mixing.push_back(1);
            if(!refined.stabilityCertified||!certifyTail(network,46).passed)break;
            working=std::move(refined);
            if(working.converged||working.maxRelativeError<fast.maxRelativeError)fast=working;
        }
        if(!fast.converged&&fast.stabilityCertified){
            auto refineTail=[&](double frequency,double q,double outsideTime=0,double tolerance=.085){
                if(outsideTime==0)outsideTime=initialOutsideTime;
                auto refined=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
                    {.refinementFrequency=frequency,.seed=&fast.bank,.refinementQ=q,.feasibility=true,.fastBoundary=true,.tailPoleBound=true,.boundaryOutsideTime=outsideTime,.feasibilityTolerance=tolerance,.control=control});
                LossNetwork network;network.sampleRate=rate;network.control=control;network.lines.push_back({delay,&refined.bank});network.mixing.push_back(1);
                if(refined.stabilityCertified&&certifyTail(network,46).passed&&(refined.converged||refined.maxRelativeError<fast.maxRelativeError))fast=std::move(refined);
            };
            refineTail(0,2);
            if(!fast.converged&&fast.worstFrequencyHz>=20&&fast.worstFrequencyHz<upper/1.05)refineTail(fast.worstFrequencyHz,2);
            for(double q:{1.,.5,4.})if(!fast.converged&&fast.worstFrequencyHz>=20&&fast.worstFrequencyHz<upper/1.05)refineTail(fast.worstFrequencyHz,q);
            for(double outsideTime:{45.,46.})if(!fast.converged)refineTail(0,2,outsideTime);
            for(double outsideTime:{45.,40.})if(!fast.converged)refineTail(0,2,outsideTime,.095);
            for(double amountScale:{0.,.5})if(!fast.converged){
                auto seed=fast.bank;
                for(auto& section:seed.stages)section=coefficients({section.frequency,section.q,section.shape},section.cutDb*amountScale,rate);
                auto refined=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
                    {.seed=&seed,.feasibility=true,.fastBoundary=true,.tailPoleBound=true,.boundaryOutsideTime=amountScale==0?45.:40.,.feasibilityTolerance=.095,.control=control});
                LossNetwork network;network.sampleRate=rate;network.control=control;network.lines.push_back({delay,&refined.bank});network.mixing.push_back(1);
                if(refined.stabilityCertified&&certifyTail(network,46).passed&&(refined.converged||refined.maxRelativeError<fast.maxRelativeError))fast=std::move(refined);
            }
            for(double outsideTime:{45.,40.})if(!fast.converged)refineTail(0,2,outsideTime,.0999);
        }
        if(fast.converged||(fast.stabilityCertified&&fast.maxRelativeError<result.maxRelativeError))result=std::move(fast);
        if(result.converged)break;
      }
    }
    if(!result.converged&&evaluateTarget(curve,upper,rate)->seconds<.2){
        for(double outsideTime:{40.,46.}){
            auto high=fitLossImpl<899>(curve,rate,delay,geometry,domain,variant,
                {.feasibility=true,.fastBoundary=true,.boundaryOutsideTime=outsideTime,.highBoundary=true,.control=control});
            LossNetwork network;network.sampleRate=rate;network.control=control;network.lines.push_back({delay,&high.bank});network.mixing.push_back(1);
            if(!certifyTail(network,46).passed)high.converged=false;
            if(high.converged||(high.stabilityCertified&&high.maxRelativeError<result.maxRelativeError))result=std::move(high);
            if(result.converged)break;
        }
    }
    return result;
}

FitResult fitLoss(const DecayCurve& curve,double rate,double delay,FitGeometry geometry,FitDomain domain,FitVariant variant,PreparationControl control) {
    auto result=fitLossCold(curve,rate,delay,geometry,domain,variant,control);
    if(result.converged||geometry!=FitGeometry::compact||domain!=FitDomain::poleContour||variant!=FitVariant::edgeBandwidth)return result;
    double previousAnchor=0;
    for(double scale:{2.,4.}){
        control.checkpoint();
        const double anchorDelay=std::min(.5,delay*scale);
        if(anchorDelay<=delay||anchorDelay==previousAnchor)continue;
        previousAnchor=anchorDelay;
        const auto anchor=fitLossCold(curve,rate,anchorDelay,geometry,domain,variant,control);
        if(!anchor.converged)continue;
        auto candidate=fitLossWarm(curve,rate,delay,anchorDelay,anchor.bank,control);
        if(!candidate.stabilityCertified||candidate.bank.stages.size()>38
            ||(!candidate.converged&&!(candidate.maxRelativeError<result.maxRelativeError)))continue;
        LossNetwork network;network.sampleRate=rate;network.control=control;network.lines.push_back({delay,&candidate.bank});network.mixing.push_back(1);
        if(!certifyTail(network,46).passed)continue;
        result=std::move(candidate);
        if(result.converged)break;
    }
    return result;
}

}


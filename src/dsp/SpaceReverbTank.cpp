#include "SpaceReverbTank.h"
#include "SpaceReverbLoss.h"
#include "SpaceReverbTail.h"
#include "SpaceReverbPacked.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <numbers>
#include <type_traits>
#include <vector>
namespace airack::space::detail {
static_assert(std::is_trivially_copyable_v<PreparedTank>);
namespace {
constexpr std::array<double,8> lineSeconds{.0193,.0239,.0297,.0371,.0437,.0533,.0617,.0719};
constexpr std::array<std::array<double,4>,2> diffusionSeconds{{{.0037,.0053,.0071,.0109},{.0043,.0061,.0083,.0127}}};
constexpr double normalization=.3535533905932737622;
bool rateValid(double rate) noexcept {return std::isfinite(rate)&&rate>=8000&&rate<=768000;}
bool prime(std::size_t n) noexcept {if(n<2)return false;for(std::size_t d=2;d*d<=n;++d)if(n%d==0)return false;return true;}
std::size_t nextPrime(std::size_t n) noexcept {while(!prime(n))++n;return n;}
std::size_t lineLength(std::size_t i,double size,double rate) noexcept {return nextPrime(std::size_t(std::llround(lineSeconds[i]*(.5+1.5*size)*rate)));}
std::size_t diffuserLength(std::size_t ch,std::size_t i,double size,double rate) noexcept {return std::max(std::size_t(1),std::size_t(std::llround(diffusionSeconds[ch][i]*(.75+.5*size)*rate)));}
bool coefficientValid(const TankCoefficients& c) noexcept {
    return std::isfinite(c.b0)&&std::isfinite(c.b1)&&std::isfinite(c.b2)&&std::isfinite(c.a1)&&std::isfinite(c.a2)
        &&c.a2>-1&&c.a2<1&&1+c.a1+c.a2>0&&1-c.a1+c.a2>0;
}
struct Ring {
    std::vector<double> data;std::size_t length=0,cursor=0,written=0;
    void reset() noexcept {cursor=written=0;}
    double read() const noexcept {return written<length?0:data[cursor];}
    void write(double x) noexcept {data[cursor]=x;if(++cursor==length)cursor=0;if(written<length)++written;}
    void copyFrom(const Ring& source) noexcept {
        const auto count=std::min({source.written,source.length,length});
        const auto begin=source.cursor>=count?source.cursor-count:source.cursor+source.length-count;
        const auto first=std::min(count,source.length-begin),destination=length-count;
        std::copy_n(source.data.data()+begin,first,data.data()+destination);
        std::copy_n(source.data.data(),count-first,data.data()+destination+first);
        cursor=0;written=count;
    }
};
}
bool prepareTank(const DecayCurve& c,double size,double rate,PreparedTank& out,TankPreparationReport* report,PreparationControl control) noexcept {
    TankPreparationReport result;
    auto fail=[&](TankPreparationFailure why,const char* text){result.failure=why;result.detail=text;if(report)*report=result;return false;};
    if(!valid(c)||!rateValid(rate)||!std::isfinite(size)||size<0||size>1)return fail(TankPreparationFailure::invalidInput,"invalid_input");
    try {
        control.checkpoint();
        PreparedTank p;p.sampleRate=rate;p.size=size;p.curve=c;std::array<LossBank,8> banks;
        for(std::size_t i=0;i<tankLines;++i){
            control.checkpoint();
            result.line=std::uint32_t(i);auto& line=p.lines[i];line.length=std::uint32_t(lineLength(i,size,rate));
            auto fit=fitLoss(c,rate,double(line.length)/rate,FitGeometry::compact,FitDomain::poleContour,FitVariant::original,control);
            if(!fit.converged){
                line.fitVariant=1;
                if(i>0){fit=fitLossWarm(c,rate,double(line.length)/rate,double(p.lines[i-1].length)/rate,banks[i-1],control);
                    if(fit.converged){++p.warmLines;result.warmLines=p.warmLines;}}
                if(!fit.converged)fit=fitLoss(c,rate,double(line.length)/rate,FitGeometry::compact,FitDomain::poleContour,FitVariant::edgeBandwidth,control);
            }
            p.maximumFitError=std::max(p.maximumFitError,fit.maxRelativeError);p.maximumNominalError=std::max(p.maximumNominalError,fit.maxNominalRelativeError);
            result.maximumFitError=p.maximumFitError;result.maximumNominalError=p.maximumNominalError;
            result.worstFrequencyHz=fit.worstFrequencyHz;result.maximumPole=fit.maxPole;result.maximumMagnitude=fit.maxMagnitude;result.stabilityCertified=fit.stabilityCertified;
            if(!fit.converged||!fit.stabilityCertified||fit.bank.stages.size()>maximumTankStages)return fail(TankPreparationFailure::fitRejected,"compact_pole_fit");
            line.scalar=fit.bank.scalar;line.count=fit.bank.stages.size();p.stageCount=std::max(p.stageCount,line.count);
            for(std::size_t j=0;j<line.count;++j){const auto& s=fit.bank.stages[j];line.coefficients[j]={s.b0,s.b1,s.b2,s.a1,s.a2};}
            banks[i]=std::move(fit.bank);
        }
        double shortest=40;const double targetRate=std::min(rate,384000.),upper=std::min(20000.,.45*rate);
        for(int i=0;i<193;++i)shortest=std::min(shortest,evaluate(c,20*std::pow(upper/20,i/192.),targetRate)->seconds);
        for(const auto& b:c.bands)if(b.active&&!b.bypassed)shortest=std::min(shortest,evaluate(c,b.frequencyHz,targetRate)->seconds);
        for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i){auto& d=p.diffusion[ch][i];d.length=std::uint32_t(diffuserLength(ch,i,size,rate));d.attenuation=std::pow(10.,-3*double(d.length)/(rate*shortest));}
        LossNetwork network;network.sampleRate=rate;network.control=control;network.mixing.resize(64);
        for(std::size_t i=0;i<tankLines;++i){network.lines.push_back({double(p.lines[i].length)/rate,&banks[i]});
            for(std::size_t j=0;j<tankLines;++j){std::size_t bits=i&j;bool odd=false;while(bits){odd=!odd;bits&=bits-1;}network.mixing[i*8+j]=odd?-normalization:normalization;}}
        const auto admission=certifyTail(network,46);
        if(!admission.passed)return fail(TankPreparationFailure::tailRejected,admission.reason);
        p.admission={true,admission.timeBound,admission.maxLoopMagnitude,admission.maxInternalPole,admission.intervals};
        control.checkpoint();out=p;result.line=8;result.detail="pass";if(report)*report=result;return true;
    } catch(const PreparationCancelled&) {return fail(TankPreparationFailure::cancelled,"cancelled");}
      catch(const std::bad_alloc&) {return fail(TankPreparationFailure::allocation,"allocation");}
      catch(...) {return fail(TankPreparationFailure::invalidInput,"preparation_exception");}
}
struct TankRuntime::State {
    PreparedTank prepared;double rate=0;RenderBackend backend=RenderBackend::automatic;bool ready=false;
    std::array<Ring,8> delays;std::array<std::array<Ring,4>,2> diffusion;
    std::array<PackedStage<8>,maximumTankStages> packed;
    PackedKernel<8> kernel=&tickPackedSse2;bool avx=false;
    std::array<double,4> sine{},cosine{1,1,1,1},stepSine{},stepCosine{};
    std::uint32_t phaseFrames=0;
};
TankRuntime::TankRuntime()=default;TankRuntime::~TankRuntime()=default;
TankRuntime::TankRuntime(TankRuntime&&) noexcept=default;TankRuntime& TankRuntime::operator=(TankRuntime&&) noexcept=default;
bool TankRuntime::prepareStorage(double rate,RenderBackend backend) {
    if(!rateValid(rate)||int(backend)<0||int(backend)>2)return false;
    auto s=std::make_unique<State>();s->rate=rate;s->backend=backend;
    constexpr std::array<double,4> frequencies{.07,.11,.13,.17};
    for(std::size_t i=0;i<4;++i){const double angle=2*std::numbers::pi*frequencies[i]/rate;s->stepSine[i]=std::sin(angle);s->stepCosine[i]=std::cos(angle);}
    if(backend==RenderBackend::automatic)if(auto kernel=availableAvx2Kernel8()){s->kernel=kernel;s->avx=true;}
    for(std::size_t i=0;i<8;++i)s->delays[i].data.resize(lineLength(i,1,rate));
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i)s->diffusion[ch][i].data.resize(diffuserLength(ch,i,1,rate));
    state_=std::move(s);return true;
}
bool TankRuntime::configure(const PreparedTank& p) noexcept {
    if(!state_||p.sampleRate!=state_->rate||!valid(p.curve)||!std::isfinite(p.size)||p.size<0||p.size>1
       ||!p.admission.passed||p.admission.timeBound!=46||!std::isfinite(p.admission.maxLoopMagnitude)||p.admission.maxLoopMagnitude>=1||p.admission.maxLoopMagnitude<0
       ||!std::isfinite(p.admission.maxInternalPole)||p.admission.maxInternalPole<0||p.admission.maxInternalPole>=1
       ||!std::isfinite(p.maximumFitError)||p.maximumFitError<0||p.maximumFitError>.1||!std::isfinite(p.maximumNominalError)||p.maximumNominalError<0||p.stageCount>maximumTankStages||p.warmLines>7)return false;
    std::size_t stages=0;
    for(std::size_t i=0;i<8;++i){const auto& line=p.lines[i];
        if(line.fitVariant>1||line.length==0||line.length>state_->delays[i].data.size()||line.count>maximumTankStages||!std::isfinite(line.scalar)||line.scalar<=0||line.scalar>=1)return false;
        stages=std::max(stages,line.count);for(std::size_t j=0;j<line.count;++j)if(!coefficientValid(line.coefficients[j]))return false;
    }
    if(stages!=p.stageCount)return false;
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i){const auto& d=p.diffusion[ch][i];if(d.length==0||d.length>state_->diffusion[ch][i].data.size()||!std::isfinite(d.attenuation)||d.attenuation<=0||d.attenuation>1)return false;}
    auto& s=*state_;s.prepared=p;for(auto& stage:s.packed)stage={};
    for(std::size_t i=0;i<8;++i){s.delays[i].length=p.lines[i].length;
        for(std::size_t j=0;j<p.lines[i].count;++j){const auto& c=p.lines[i].coefficients[j];auto& d=s.packed[j];d.b0[i]=c.b0;d.b1[i]=c.b1;d.b2[i]=c.b2;d.a1[i]=c.a1;d.a2[i]=c.a2;}}
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i)s.diffusion[ch][i].length=p.diffusion[ch][i].length;
    s.ready=true;reset();return true;
}
void TankRuntime::reset() noexcept {
    if(!state_)return;for(auto& ring:state_->delays)ring.reset();for(auto& ch:state_->diffusion)for(auto& ring:ch)ring.reset();
    for(auto& stage:state_->packed){stage.z1.fill(0);stage.z2.fill(0);}
    state_->sine.fill(0);state_->cosine.fill(1);state_->phaseFrames=0;
}
bool TankRuntime::processBlock(std::span<Frame> frames,std::span<const double> character) noexcept {
    if(!state_||!state_->ready||frames.size()>maximumBlockFrames||(!character.empty()&&character.size()!=frames.size()))return false;
    for(std::size_t n=0;n<frames.size();++n)frames[n]=tick(frames[n],character.empty()?0:character[n]);
    return true;
}
bool TankRuntime::adoptCopied(const PreparedTank& p,const TankRuntime& source) noexcept {
    if(this==&source||!state_||!source.state_||!source.state_->ready||state_->rate!=source.state_->rate)return false;
    const auto& origin=*source.state_;
    if(!configure(p))return false;
    auto& destination=*state_;
    for(std::size_t i=0;i<8;++i){
        destination.delays[i].copyFrom(origin.delays[i]);
        const auto& old=origin.prepared.lines[i];const auto& next=p.lines[i];
        bool same=old.count==next.count&&std::bit_cast<std::uint64_t>(old.scalar)==std::bit_cast<std::uint64_t>(next.scalar);
        static_assert(sizeof(TankCoefficients)==5*sizeof(std::uint64_t));
        for(std::size_t j=0;j<next.count&&same;++j)same=std::bit_cast<std::array<std::uint64_t,5>>(old.coefficients[j])==std::bit_cast<std::array<std::uint64_t,5>>(next.coefficients[j]);
        if(same)for(std::size_t j=0;j<maximumTankStages;++j){destination.packed[j].z1[i]=origin.packed[j].z1[i];destination.packed[j].z2[i]=origin.packed[j].z2[i];}
    }
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i)destination.diffusion[ch][i].copyFrom(origin.diffusion[ch][i]);
    destination.sine=origin.sine;destination.cosine=origin.cosine;destination.phaseFrames=origin.phaseFrames;
    return true;
}
void rotateSpaceFeedback(std::array<double,8>& values,double character,const std::array<double,4>& phases) noexcept {
    if(!(character>0))return;character=std::min(1.,character);
    for(std::size_t i=0;i<4;++i){const double t=.06*character*phases[i],denominator=1+t*t,c=(1-t*t)/denominator,s=2*t/denominator;
        const double a=values[2*i],b=values[2*i+1];values[2*i]=c*a-s*b;values[2*i+1]=s*a+c*b;}
}
std::array<double,2> TankRuntime::tick(std::array<double,2> excitation) noexcept {return tick(excitation,0);}
std::array<double,2> TankRuntime::tick(std::array<double,2> excitation,double character) noexcept {
    if(!state_||!state_->ready)return {};auto& s=*state_;
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<4;++i){auto& ring=s.diffusion[ch][i];
        const double delayed=s.prepared.diffusion[ch][i].attenuation*ring.read(),next=delayed-.6*excitation[ch];ring.write(excitation[ch]+.6*next);excitation[ch]=next;
    }
    std::array<double,8> values;for(std::size_t i=0;i<8;++i)values[i]=s.delays[i].read();
    if(s.backend==RenderBackend::scalar){
        for(std::size_t i=0;i<8;++i)for(std::size_t j=0;j<s.prepared.lines[i].count;++j){auto& c=s.packed[j];const double x=values[i],y=c.b0[i]*x+c.z1[i];c.z1[i]=c.b1[i]*x-c.a1[i]*y+c.z2[i];c.z2[i]=c.b2[i]*x-c.a2[i]*y;values[i]=y;}
    }else s.kernel(s.packed.data(),s.prepared.stageCount,values.data(),8);
    for(std::size_t i=0;i<8;++i)values[i]*=s.prepared.lines[i].scalar;
    auto mixed=values;
    for(std::size_t stride=1;stride<8;stride*=2)for(std::size_t start=0;start<8;start+=2*stride)for(std::size_t j=0;j<stride;++j){const double a=mixed[start+j],b=mixed[start+j+stride];mixed[start+j]=a+b;mixed[start+j+stride]=a-b;}
    if(character>0){
        rotateSpaceFeedback(mixed,character,s.sine);
        for(std::size_t i=0;i<4;++i){const double a=s.sine[i],b=s.cosine[i];s.sine[i]=a*s.stepCosine[i]+b*s.stepSine[i];s.cosine[i]=b*s.stepCosine[i]-a*s.stepSine[i];}
        if(++s.phaseFrames==4096){s.phaseFrames=0;for(std::size_t i=0;i<4;++i){const double length=std::hypot(s.sine[i],s.cosine[i]);s.sine[i]/=length;s.cosine[i]/=length;}}
    }
    std::array<double,2> wet{};
    for(std::size_t i=0;i<8;++i){const double sign=i%2?-1.:1.;s.delays[i].write(.25*(excitation[0]+sign*excitation[1])+normalization*mixed[i]);wet[0]+=normalization*values[i];wet[1]+=normalization*sign*values[i];}
    return wet;
}
const PreparedTank& TankRuntime::prepared() const noexcept {static constexpr PreparedTank empty;return state_?state_->prepared:empty;}
std::size_t TankRuntime::storageBytes() const noexcept {
    std::size_t bytes=sizeof(*this);if(!state_)return bytes;bytes+=sizeof(State);
    for(const auto& ring:state_->delays)bytes+=ring.data.capacity()*sizeof(double);
    for(const auto& ch:state_->diffusion)for(const auto& ring:ch)bytes+=ring.data.capacity()*sizeof(double);return bytes;
}
const char* TankRuntime::backendName() const noexcept {return state_?(state_->backend==RenderBackend::scalar?"scalar":state_->avx?"avx2":"sse2"):"unprepared";}
}

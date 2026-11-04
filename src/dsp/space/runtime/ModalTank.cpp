#include "ModalTank.h"
#include "ModalKernel.h"
#include "ModalPreparedCopy.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>
namespace airack::space::runtime {
namespace {
constexpr double rootHalf=.7071067811865475244;
bool rateValid(double rate) noexcept {return std::isfinite(rate)&&rate>=8000&&rate<=768000;}
bool countValid(std::size_t count) noexcept {return count==128||count==192||count==256||count==512||count==1024||count==2048;}
double unit(std::uint32_t value) noexcept {
    value^=value>>16;value*=0x7feb352dU;value^=value>>15;value*=0x846ca68bU;value^=value>>16;
    return double(value&0xffffffU)/16777216.;
}
}
void advanceModal(ModalSample& state,double rc,double rs,double br,double bi,double input) noexcept {
    const double a=state.real,b=state.imag;
    state.real=(rc*a-rs*b)+br*input;state.imag=(rs*a+rc*b)+bi*input;
}
bool prepareModal(const DecayCurve& curve,double size,double rate,std::size_t count,PreparedModal& out,detail::PreparationControl control) noexcept {
    if(!valid(curve)||!rateValid(rate)||!std::isfinite(size)||size<0||size>1||!countValid(count))return false;
    try {
        control.checkpoint();auto p=std::make_unique<PreparedModal>();
        p->sampleRate=rate;p->size=size;p->count=count;p->curve=curve;
        const double reference=std::sqrt(-std::expm1(-std::log(1000.)/48000.)),referenceLoss=-std::expm1(-std::log(1000.)/(2*48000.));
        p->pickup=std::sqrt(4./count)*reference*(-std::expm1(-std::log(1000.)/(2*rate))/referenceLoss);
        const double upper=std::min(20000.,.45*rate),range=std::log((upper+228.83)/(20+228.83));
        for(std::size_t k=0;k<count;++k){
            if(k%32==0)control.checkpoint();
            const auto cell=(k/8)*4+k%4;
            const double jitter=.2*(2*size-1)*(2*unit(std::uint32_t(k)+0x9e3779b9U)-1);
            const double offset=(k/4)%2?.75:.25;
            const double t=(double(cell)+offset+jitter)/double(count/2);
            p->frequency[k]=(20+228.83)*std::exp(t*range)-228.83;
        }
        std::array<double,17> anchors{};std::size_t anchorCount=0;
        const auto add=[&](double f){f=std::clamp(f,20.,upper);for(std::size_t j=0;j<anchorCount;++j)if(anchors[j]==f)return;anchors[anchorCount++]=f;};
        add(20);add(upper);for(double f:{125.,250.,500.,1000.,2000.,4000.,8000.})add(f);
        for(const auto& band:curve.bands)if(band.active&&!band.bypassed)add(band.frequencyHz);
        std::array<bool,maximumDirectModes> claimed{};
        for(unsigned branch=0;branch<2;++branch)for(std::size_t a=0;a<anchorCount;++a){
            std::size_t selected=count;double distance=std::numeric_limits<double>::infinity();
            for(std::size_t k=0;k<count;++k)if((k/4)%2==branch&&!claimed[k]){
                const double delta=std::abs(p->frequency[k]-anchors[a]);if(delta<distance){distance=delta;selected=k;}
            }
            if(selected==count)return false;p->frequency[selected]=anchors[a];claimed[selected]=true;
        }
        const double r46=std::exp(-std::log(1000.)/(rate*46));p->weightedRadius=0;
        for(std::size_t k=0;k<count;++k){
            if(k%32==0)control.checkpoint();
            const auto target=evaluate(curve,p->frequency[k],std::min(rate,384000.));if(!target)return false;
            const double r=std::exp(-std::log(1000.)/(rate*target->seconds)),angle=2*std::numbers::pi*p->frequency[k]/rate;
            p->target[k]=target->seconds;p->rc[k]=r*std::cos(angle);p->rs[k]=r*std::sin(angle);
            const auto group=k/4;const bool imaginary=((group/4)+(group%4)/2)%2!=0;
            const double sign=unit(std::uint32_t(k)+0x3c6ef372U)<.5?-1.:1.;
            p->bReal[k]=imaginary?0:sign;p->bImag[k]=imaginary?sign:0;
            p->weightedRadius=std::max(p->weightedRadius,std::hypot(p->rc[k],p->rs[k])/r46);
        }
        for(std::size_t g=0;g<count/4;++g){
            switch(g%4){case 0:p->inputWeights[g]={1,0};break;case 1:p->inputWeights[g]={0,1};break;
                case 2:p->inputWeights[g]={rootHalf,rootHalf};break;case 3:p->inputWeights[g]={rootHalf,-rootHalf};break;}
        }
        if(!(p->weightedRadius<1))return false;control.checkpoint();out=*p;return true;
    } catch(...) {return false;}
}
struct ModalTank::State {
    double rate=0,r46=0;bool ready=false,unusedZero=true,pairedReduction=false;
    ModalKernel kernel=&modalScalar,baseKernel=&modalScalar;ModalBackend backend=ModalBackend::scalar;
    PreparedModal prepared;
    alignas(64) std::array<double,maximumDirectModes> real{},imag{};
    std::array<Frame,maximumBlockFrames> input{},accum{};
};
ModalTank::ModalTank()=default;ModalTank::~ModalTank()=default;
bool ModalTank::prepareStorage(double rate,ModalBackend backend,bool pairedReduction){
    if(!rateValid(rate))return false;
    const bool sse=modalSse2Available();const auto avx=availableModalAvx2Kernel();const auto kernel=modalKernelFor(backend,sse,avx);if(!kernel)return false;
    try{auto fresh=std::make_unique<State>();fresh->rate=rate;fresh->r46=std::exp(-std::log(1000.)/(rate*46));fresh->baseKernel=fresh->kernel=kernel;fresh->pairedReduction=pairedReduction;
        fresh->backend=backend==ModalBackend::automatic?(avx?ModalBackend::avx2:sse?ModalBackend::sse2:ModalBackend::scalar):backend;
        state_=std::move(fresh);return true;}
    catch(const std::bad_alloc&){return false;}
}
bool ModalTank::configure(const PreparedModal& p) noexcept {
    return configureImpl(p,false);
}
bool ModalTank::configureImpl(const PreparedModal& p,bool factory) noexcept {
    if(!state_||p.sampleRate!=state_->rate||!countValid(p.count)||!valid(p.curve)||!std::isfinite(p.size)||p.size<0||p.size>1
       ||!std::isfinite(p.pickup)||p.pickup<=0||p.pickup>1||!std::isfinite(p.weightedRadius)||p.weightedRadius<0||p.weightedRadius>=1)return false;
    double maximum=0;const double limit=state_->r46*state_->r46,upper=std::min(20000.,.45*state_->rate);
    for(std::size_t k=0;k<p.count;++k){
        const double norm=p.rc[k]*p.rc[k]+p.rs[k]*p.rs[k];
        if(!std::isfinite(norm)||norm<=0||norm>=limit||!std::isfinite(p.bReal[k])||!std::isfinite(p.bImag[k])
           ||p.bReal[k]*p.bReal[k]+p.bImag[k]*p.bImag[k]>1+1e-12
           ||!std::isfinite(p.frequency[k])||p.frequency[k]<20||p.frequency[k]>upper||!std::isfinite(p.target[k])||p.target[k]<.1||p.target[k]>40)return false;
        maximum=std::max(maximum,norm);
    }
    if(std::abs(std::sqrt(maximum)/state_->r46-p.weightedRadius)>1e-12)return false;
    for(std::size_t g=0;g<p.count/4;++g)for(double w:p.inputWeights[g])if(!std::isfinite(w)||std::abs(w)>1)return false;
    auto& s=*state_;
    if(factory)ModalFactoryCopy::assign(s.prepared,p,s.unusedZero);
    else {s.prepared=p;s.unusedZero=false;}
    s.kernel=specializeModalKernel(s.baseKernel,p,s.pairedReduction);s.ready=true;
    if(factory){std::fill_n(s.real.begin(),p.count,0.);std::fill_n(s.imag.begin(),p.count,0.);}
    else reset();
    return true;
}
bool ModalTank::adoptCopied(const PreparedModal& p,const ModalTank& source) noexcept {
    if(this==&source||!state_||!source.state_||!source.state_->ready||state_->rate!=source.state_->rate||p.count!=source.state_->prepared.count)return false;
    if(!configure(p))return false;
    std::copy_n(source.state_->real.data(),p.count,state_->real.data());
    std::copy_n(source.state_->imag.data(),p.count,state_->imag.data());return true;
}
bool ModalTank::adoptPreparedCopied(const PreparedModal& p,const ModalTank& source) noexcept {
    if(this==&source||!state_||!source.state_||!source.state_->ready||state_->rate!=source.state_->rate
       ||p.sampleRate!=state_->rate||!countValid(p.count)||p.count!=source.state_->prepared.count
       ||!std::isfinite(p.pickup)||p.pickup<=0||p.pickup>1||!std::isfinite(p.weightedRadius)||p.weightedRadius<0||p.weightedRadius>=1)return false;
    auto& s=*state_;ModalFactoryCopy::assign(s.prepared,p,s.unusedZero);
    s.kernel=specializeModalKernel(s.baseKernel,p,s.pairedReduction);s.ready=true;
    // Every active state is overwritten; public configure/reset still clear all states.
    std::copy_n(source.state_->real.data(),p.count,s.real.data());
    std::copy_n(source.state_->imag.data(),p.count,s.imag.data());return true;
}
void ModalTank::reset() noexcept {if(state_){state_->real.fill(0);state_->imag.fill(0);}}
bool ModalTank::processBlock(std::span<Frame> frames) noexcept {
    if(!state_||!state_->ready||frames.size()>maximumBlockFrames)return false;
    if(frames.empty())return true;
    auto& s=*state_;const auto& p=s.prepared;const auto count=frames.size();
    std::copy(frames.begin(),frames.end(),s.input.begin());std::fill_n(s.accum.begin(),count,Frame{});
    s.kernel(p,s.real.data(),s.imag.data(),std::span(s.input).first(count),std::span(s.accum).first(count));
    for(std::size_t n=0;n<count;++n){const double mid=s.accum[n][0]*p.pickup,side=s.accum[n][1]*p.pickup;frames[n]={(mid+side)*rootHalf,(mid-side)*rootHalf};}
    return true;
}
ModalTank::Frame ModalTank::tick(Frame input) noexcept {return processBlock(std::span(&input,1))?input:Frame{};}
ModalSample ModalTank::modeState(std::size_t index) const noexcept {
    return state_&&state_->ready&&index<state_->prepared.count?ModalSample{state_->real[index],state_->imag[index]}:ModalSample{};
}
const PreparedModal& ModalTank::prepared() const noexcept {static constexpr PreparedModal empty;return state_?state_->prepared:empty;}
std::size_t ModalTank::storageBytes() const noexcept {return sizeof(*this)+(state_?sizeof(State):0);}
bool ModalTank::usesPairedReduction() const noexcept {return state_&&state_->ready&&modalKernelUsesPairedOutput(state_->kernel);}
const char* ModalTank::backendName() const noexcept {
    if(!state_)return "unprepared";
    switch(state_->backend){case ModalBackend::scalar:return "scalar";case ModalBackend::sse2:return "sse2";case ModalBackend::avx2:return "avx2";case ModalBackend::automatic:break;}
    return "invalid";
}
}

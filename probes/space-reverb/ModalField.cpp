#include "ModalField.h"
#include "space/runtime/ModalTailMath.h"
using airack::space::runtime::boundModalPostSection;
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <vector>
namespace airack::space::probe {
namespace {
constexpr std::array<double,8> diffusion{.00071,.00113,.00173,.00231,.00317,.00419,.00527,.00661};
constexpr std::array<std::array<double,3>,2> early{{{.0073,.0111,.0179},{.0089,.0137,.0193}}};
constexpr std::array<double,3> earlyGains{.42,.25,.18};
bool rateValid(double rate) noexcept {return std::isfinite(rate)&&rate>=8000&&rate<=768000;}
using runtime::tail_detail::roots;using runtime::tail_detail::rootMargin;
double logAdd(double a,double b) noexcept {const double top=std::max(a,b);return top+std::log(std::exp(a-top)+std::exp(b-top));}
bool tail(PreparedModalField& p,detail::PreparationControl control) {
    double maximumRadius=0;for(std::size_t k=0;k<p.modal.count;++k)maximumRadius=std::max(maximumRadius,std::hypot(p.modal.rc[k],p.modal.rs[k]));
    maximumRadius+=64*std::numeric_limits<double>::epsilon();if(!(maximumRadius>0&&maximumRadius<1))return false;
    const double rate=p.modal.sampleRate;double longest=-std::log(1000.)/(rate*std::log(maximumRadius));
    for(std::size_t j=0;j<p.post.count;++j){control.checkpoint();const auto& c=p.post.stages[j];const auto r=roots(c);const double maximum=std::max(std::abs(r[0]),std::abs(r[1]))+rootMargin(c);
        if(!(maximum>=0&&maximum<1))return false;if(maximum>0)longest=std::max(longest,-std::log(1000.)/(rate*std::log(maximum)));}
    if(!std::isfinite(longest)||longest<=0)return false;const double time=std::min(46.,1.15*longest),rho=std::exp(-std::log(1000.)/(rate*time));if(!(maximumRadius<rho))return false;double logPost=0;
    for(std::size_t j=0;j<p.post.count;++j){control.checkpoint();const auto bound=boundModalPostSection(p.post.stages[j],rho);if(!bound.passed)return false;logPost+=bound.logL1;}
    std::size_t diffuse=0;for(double d:p.diffusionSamples)diffuse+=std::size_t(std::ceil(d))+1;double maxEarly=0;for(const auto& channel:p.earlySamples)for(double d:channel)maxEarly=std::max(maxEarly,d);
    const auto support=std::size_t(std::ceil(p.settings.predelayMs*.001*rate))+1+std::max(diffuse+std::size_t(std::ceil(.013*rate))+1,std::size_t(std::ceil(maxEarly))+1);
    const double logModal=std::log(p.modal.pickup)+std::log(double(p.modal.count))-.5*std::log(2.)+8*std::log(1.4)-std::log1p(-maximumRadius);
    const double logRoom=logAdd(std::log(.68),logModal)+1e-12,logPeak=logRoom+logPost+std::log(std::max(1.,p.settings.width));
    const double frames=double(support)+std::ceil(std::max(0.,(logPeak-std::log(1e-6))/(-std::log(rho))))+1;
    if(!std::isfinite(frames)||frames<=0)return false;p.tail={true,frames/rate,rho,time,logPost,logRoom,1,support};return true;
}
struct Position {std::size_t whole=0;double fraction=0;};
Position position(double samples) noexcept {const double n=std::floor(samples);return {std::size_t(n),samples-n};}
struct Ring {
    std::vector<double> data;std::size_t cursor=0,written=0;
    void reset() noexcept {cursor=written=0;}
    double age(std::size_t n) const noexcept {if(n==0||n>written||n>data.size())return 0;return data[cursor>=n?cursor-n:cursor+data.size()-n];}
    double read(Position p,double now) const noexcept {const double a=p.whole?age(p.whole):now;if(p.fraction==0)return a;return (1-p.fraction)*a+p.fraction*age(p.whole+1);}
    void write(double v) noexcept {data[cursor]=v;if(++cursor==data.size())cursor=0;if(written<data.size())++written;}
};
}
bool prepareModalField(const Settings& settings,double rate,std::size_t modes,PreparedModalField& out,detail::PreparationControl control) noexcept {
    if(!valid(settings)||!rateValid(rate)||(modes!=128&&modes!=192&&modes!=256&&modes!=512&&modes!=1024&&modes!=2048))return false;
    try {control.checkpoint();auto p=std::make_unique<PreparedModalField>();p->settings=settings;
        if(!prepareModal(settings.decay,settings.size,rate,modes,p->modal,control)||!preparePostEq(settings,rate,p->post)||!prepareDuck(settings,rate,p->duck))return false;
        for(std::size_t j=0;j<8;++j)p->diffusionSamples[j]=diffusion[j]*(.5+1.5*settings.size)*rate;
        for(std::size_t c=0;c<2;++c){for(std::size_t j=0;j<3;++j)p->earlySamples[c][j]=early[c][j]*(.75+.5*settings.size)*rate;const double angle=2*std::numbers::pi*(c?.31:.23)/rate;p->oscillatorStep[c]={std::sin(angle),std::cos(angle)};}
        if(!tail(*p,control))return false;control.checkpoint();out=*p;return true;
    }catch(...){return false;}
}
struct ModalField::State {
    PreparedModalField prepared;double rate=0;bool ready=false;ModalTank modal;PostEq post;DuckDetector duck;
    std::array<Ring,2> predelay,reflections,chorus;std::array<Ring,8> scatter;
    Position predelayPosition;std::array<Position,8> scatterPosition{};std::array<std::array<Position,3>,2> earlyPosition{};
    std::array<std::array<double,2>,2> phase{},initial{};std::size_t clock=0;
    std::array<Frame,512> input{},earlyBlock{};
};
ModalField::ModalField()=default;ModalField::~ModalField()=default;
bool ModalField::prepareStorage(double rate,ModalBackend backend){
    if(!rateValid(rate))return false;try {auto s=std::make_unique<State>();s->rate=rate;if(!s->modal.prepareStorage(rate,backend))return false;
        for(auto& r:s->predelay)r.data.resize(std::size_t(std::ceil(.25*rate))+2);
        for(auto& r:s->reflections)r.data.resize(std::size_t(std::ceil(.0193*1.25*rate))+2);
        for(auto& r:s->chorus)r.data.resize(std::size_t(std::ceil(.013*rate))+2);
        for(std::size_t j=0;j<8;++j)s->scatter[j].data.resize(std::size_t(std::ceil(diffusion[j]*2*rate))+2);
        s->initial={{{0,1},{std::sin(.37*std::numbers::pi),std::cos(.37*std::numbers::pi)}}};s->phase=s->initial;state_=std::move(s);return true;
    }catch(const std::bad_alloc&){return false;}
}
bool ModalField::configure(const PreparedModalField& p) noexcept {
    if(!state_||!valid(p.settings)||p.modal.sampleRate!=state_->rate||p.modal.curve!=p.settings.decay||p.modal.size!=p.settings.size||p.post.sampleRate!=state_->rate||p.duck.sampleRate!=state_->rate
       ||p.duck.rangeDb!=p.settings.duckRangeDb||p.duck.thresholdDb!=p.settings.duckThresholdDb||!p.tail.passed||p.tail.referenceInputPeak!=1||!std::isfinite(p.tail.seconds)||p.tail.seconds<=0
       ||!std::isfinite(p.tail.radius)||p.tail.radius<=0||p.tail.radius>=1||!std::isfinite(p.tail.timeBound)||p.tail.timeBound<=0||p.tail.timeBound>46||!std::isfinite(p.tail.logPost)||!std::isfinite(p.tail.logRoom))return false;
    for(std::size_t j=0;j<8;++j)if(p.diffusionSamples[j]!=diffusion[j]*(.5+1.5*p.settings.size)*state_->rate)return false;
    for(std::size_t c=0;c<2;++c){for(std::size_t j=0;j<3;++j)if(p.earlySamples[c][j]!=early[c][j]*(.75+.5*p.settings.size)*state_->rate)return false;
        const auto step=p.oscillatorStep[c];if(!std::isfinite(step[0])||!std::isfinite(step[1])||step[0]<=0||step[0]>=.001||step[1]<=0||std::abs(step[0]*step[0]+step[1]*step[1]-1)>1e-12)return false;}
    PostEq post;DuckDetector duck;if(!post.prepare(p.post)||!duck.prepare(p.duck))return false;if(!state_->modal.configure(p.modal))return false;
    auto& s=*state_;s.prepared=p;s.post=post;s.duck=duck;s.predelayPosition=position(p.settings.predelayMs*.001*s.rate);
    for(std::size_t j=0;j<8;++j)s.scatterPosition[j]=position(p.diffusionSamples[j]);for(std::size_t c=0;c<2;++c)for(std::size_t j=0;j<3;++j)s.earlyPosition[c][j]=position(p.earlySamples[c][j]);
    s.ready=true;reset();return true;
}
bool ModalField::processBlock(std::span<Frame> frames) noexcept {
    if(!state_||!state_->ready||frames.size()>512)return false;if(frames.empty())return true;auto& s=*state_;const auto& settings=s.prepared.settings;
    std::copy(frames.begin(),frames.end(),s.input.begin());
    for(std::size_t n=0;n<frames.size();++n){Frame x{},e{};
        for(std::size_t c=0;c<2;++c){x[c]=s.predelay[c].read(s.predelayPosition,s.input[n][c]);s.predelay[c].write(s.input[n][c]);}
        for(std::size_t c=0;c<2;++c)for(std::size_t j=0;j<3;++j){const auto source=j==1?1-c:c;e[c]+=earlyGains[j]*s.reflections[source].read(s.earlyPosition[c][j],x[source]);}
        for(std::size_t c=0;c<2;++c)s.reflections[c].write(x[c]);s.earlyBlock[n]=e;
        for(std::size_t j=0;j<8;++j){const double delayed=s.scatter[j].read(s.scatterPosition[j],x[1]);s.scatter[j].write(x[1]);const double left=x[0];x={.6*left+.8*delayed,-.8*left+.6*delayed};}frames[n]=x;
    }
    if(!s.modal.processBlock(frames))return false;
    for(std::size_t n=0;n<frames.size();++n){auto wet=frames[n];const double gain=s.duck.tick(s.input[n]);
        for(std::size_t c=0;c<2;++c){const double sine=std::clamp(s.phase[c][0],-1.,1.);const double samples=(c?.008+.005*sine:.006+.004*sine)*s.rate;
            const double moved=s.chorus[c].read(position(samples),wet[c]);s.chorus[c].write(wet[c]);if(settings.character!=0)wet[c]=(1-settings.character)*wet[c]+settings.character*moved;
            const auto phase=s.phase[c],step=s.prepared.oscillatorStep[c];s.phase[c]={phase[0]*step[1]+phase[1]*step[0],phase[1]*step[1]-phase[0]*step[0]};}
        if((s.clock=(s.clock+1)&4095)==0)for(auto& phase:s.phase){const double norm=std::hypot(phase[0],phase[1]);phase[0]/=norm;phase[1]/=norm;}
        if(settings.distance!=1)for(std::size_t c=0;c<2;++c)wet[c]=.8*(1-settings.distance)*s.earlyBlock[n][c]+(.2+.8*settings.distance)*wet[c];wet=s.post.tick(wet);
        if(settings.width!=1){const double mid=(wet[0]+wet[1])*.5,side=(wet[0]-wet[1])*.5;wet={mid+settings.width*side,mid-settings.width*side};}
        if(gain!=1)for(auto& v:wet)v*=gain;
        if(settings.mix==0)wet=s.input[n];else if(settings.mix!=1)for(std::size_t c=0;c<2;++c)wet[c]=(1-settings.mix)*s.input[n][c]+settings.mix*wet[c];frames[n]=wet;
    }return true;
}
void ModalField::reset() noexcept {if(!state_)return;auto& s=*state_;s.modal.reset();s.post.reset();s.duck.reset();for(auto& r:s.predelay)r.reset();for(auto& r:s.reflections)r.reset();for(auto& r:s.chorus)r.reset();for(auto& r:s.scatter)r.reset();s.phase=s.initial;s.clock=0;}
std::size_t ModalField::storageBytes() const noexcept {if(!state_)return sizeof(*this);const auto& s=*state_;std::size_t bytes=sizeof(*this)+sizeof(State)+s.modal.storageBytes()-sizeof(ModalTank);
    for(const auto& r:s.predelay)bytes+=r.data.capacity()*sizeof(double);for(const auto& r:s.reflections)bytes+=r.data.capacity()*sizeof(double);for(const auto& r:s.chorus)bytes+=r.data.capacity()*sizeof(double);for(const auto& r:s.scatter)bytes+=r.data.capacity()*sizeof(double);return bytes;}
double ModalField::duckEnvelope() const noexcept {return state_?state_->duck.envelope():0;}
const PreparedModalField& ModalField::prepared() const noexcept {static constexpr PreparedModalField empty;return state_?state_->prepared:empty;}
ModalSample ModalField::modeState(std::size_t k) const noexcept {return state_?state_->modal.modeState(k):ModalSample{};}
}

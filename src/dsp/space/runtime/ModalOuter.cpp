#include "ModalOuter.h"
#include "ModalTailMath.h"
#include <algorithm>
#include <cmath>
#include <vector>
namespace airack::space::runtime {
namespace {
constexpr std::array<std::array<double,3>,2> early{{{.0073,.0111,.0179},{.0089,.0137,.0193}}};
constexpr std::array<double,3> gains{.42,.25,.18};
bool range(double v,double lo,double hi) noexcept {return std::isfinite(v)&&v>=lo&&v<=hi;}
bool rateValid(double r) noexcept {return range(r,8000,768000);}
bool validTargets(ModalWetTargets t) noexcept {return range(t.predelayMs,0,250)&&range(t.character,0,1)&&range(t.distance,0,1);}
ModalPosition position(double samples) noexcept {const double n=std::floor(samples);return {std::size_t(n),samples-n};}
bool validEarly(const PreparedModalEarly& p,double rate) noexcept {
    if(p.rate!=rate||!range(p.size,0,1))return false;
    for(std::size_t c=0;c<2;++c)for(std::size_t j=0;j<3;++j){const double sample=early[c][j]*(.75+.5*p.size)*rate;if(p.samples[c][j]!=sample||p.positions[c][j]!=position(sample))return false;}return true;
}
bool validPost(const PreparedModalPost& p,double rate) noexcept {PostEq check;return p.coefficients.sampleRate==rate&&std::isfinite(p.commonLogL1)&&p.commonLogL1>=0&&check.prepare(p.coefficients);}
bool validDuck(const PreparedModalDuck& p,double rate) noexcept {
    DuckDetector check;return p.coefficients.sampleRate==rate&&check.prepare(p.coefficients)&&p.coefficients.attack>0&&p.coefficients.release>0
        &&range(p.thresholdLinear,.001,1)&&range(p.minimumGain,.0630957344480193,1)
        &&(p.coefficients.rangeDb!=0||p.minimumGain==1)&&(p.coefficients.thresholdDb!=0||p.thresholdLinear==1);
}
struct Ramp {
    double current=0,target=0,delta=0,low=0,high=0;std::size_t remaining=0;
    void snap(double v) noexcept {current=target=low=high=v;delta=0;remaining=0;}
    void snapTarget() noexcept {snap(target);}
    void set(double v,std::size_t duration) noexcept {if(v==target)return;target=v;low=std::min(current,v);high=std::max(current,v);remaining=duration;delta=(v-current)/double(duration);}
    double tick() noexcept {const double value=current;if(remaining){if(--remaining==0)current=target;else current=std::clamp(current+delta,low,high);}return value;}
};
struct Ring {
    std::vector<double> data;std::size_t cursor=0,written=0;
    void reset() noexcept {cursor=written=0;}
    double age(std::size_t n) const noexcept {if(n==0||n>written||n>data.size())return 0;return data[cursor>=n?cursor-n:cursor+data.size()-n];}
    double read(ModalPosition p,double now) const noexcept {const double a=p.whole?age(p.whole):now;if(p.fraction==0)return a;return (1-p.fraction)*a+p.fraction*age(p.whole+1);}
    void write(double v) noexcept {data[cursor]=v;if(++cursor==data.size())cursor=0;if(written<data.size())++written;}
};
bool finiteFrames(std::span<const ModalRoom::Frame> frames) noexcept {for(auto f:frames)if(!std::isfinite(f[0])||!std::isfinite(f[1]))return false;return true;}
bool alphas(std::span<const double> a) noexcept {for(double v:a)if(!range(v,0,1))return false;return true;}
}
bool prepareModalEarly(double size,double rate,PreparedModalEarly& out) noexcept {
    if(!rateValid(rate)||!range(size,0,1))return false;PreparedModalEarly p;p.rate=rate;p.size=size;
    for(std::size_t c=0;c<2;++c)for(std::size_t j=0;j<3;++j){p.samples[c][j]=early[c][j]*(.75+.5*size)*rate;p.positions[c][j]=position(p.samples[c][j]);}out=p;return true;
}
bool prepareModalPost(const Settings& s,double rate,PreparedModalPost& out) noexcept {
    PreparedModalPost p;if(!preparePostEq(s,rate,p.coefficients))return false;const double rho=std::exp(-std::log(1000.)/(rate*46));
    for(std::size_t j=0;j<p.coefficients.count;++j){const auto b=boundModalPostSection(p.coefficients.stages[j],rho);if(!b.passed)return false;p.commonLogL1+=b.logL1;}
    p.commonLogL1=std::max(0.,p.commonLogL1);out=p;return true;
}
bool prepareModalDuck(const Settings& s,double rate,PreparedModalDuck& out) noexcept {
    PreparedModalDuck p;if(!prepareDuck(s,rate,p.coefficients))return false;p.thresholdLinear=s.duckThresholdDb==0?1:std::pow(10.,s.duckThresholdDb/20);p.minimumGain=s.duckRangeDb==0?1:std::pow(10.,-s.duckRangeDb/20);out=p;return true;
}
bool prepareModalWet(const Settings& s,double rate,PreparedModalWet& out) noexcept {
    if(!valid(s)||!rateValid(rate))return false;PreparedModalWet p;p.rate=rate;p.targets={s.predelayMs,s.character,s.distance};if(!prepareModalEarly(s.size,rate,p.early)||!prepareModalPost(s,rate,p.post))return false;out=p;return true;
}
bool prepareModalFinal(const Settings& s,double rate,PreparedModalFinal& out) noexcept {
    if(!valid(s)||!rateValid(rate))return false;PreparedModalFinal p;p.rate=rate;p.width=s.width;p.mix=s.mix;if(!prepareModalDuck(s,rate,p.duck))return false;out=p;return true;
}
struct ModalWetOuter::State {
    double rate=0;bool ready=false,open=false,combined=false,pending=false,alphaStarted=false;std::size_t duration=0,count=0,active=0;double lastAlpha=0;
    std::array<Ring,2> predelay,reflections;PreparedModalEarly oldEarly,newEarly;std::array<PostEq,2> post;std::array<PreparedModalPost,2> descriptors;
    Ramp character,distance;double targetDelayMs=20,currentDelayMs=20,toDelayMs=20;ModalPosition delay{},from{},to{};bool delayFading=false;std::size_t delayPhase=0;
    std::array<Frame,512> earlyOld{},earlyNew{};std::array<double,512> characters{},distances{};
};
ModalWetOuter::ModalWetOuter()=default;ModalWetOuter::~ModalWetOuter()=default;
bool ModalWetOuter::prepareStorage(double rate){
    if(!rateValid(rate))return false;try {auto s=std::make_unique<State>();s->rate=rate;s->duration=std::size_t(std::ceil(.005*rate));for(auto& r:s->predelay)r.data.resize(std::size_t(std::ceil(.25*rate))+2);for(auto& r:s->reflections)r.data.resize(std::size_t(std::ceil(.0193*1.25*rate))+2);state_=std::move(s);return true;}catch(const std::bad_alloc&){return false;}
}
bool ModalWetOuter::configure(const PreparedModalWet& p) noexcept {
    if(!state_||p.rate!=state_->rate||!validTargets(p.targets)||!validEarly(p.early,p.rate)||!validPost(p.post,p.rate))return false;PostEq post;if(!post.prepare(p.post.coefficients))return false;
    auto& s=*state_;s.post={post,post};s.descriptors={p.post,p.post};s.active=0;s.oldEarly=s.newEarly=p.early;s.targetDelayMs=p.targets.predelayMs;s.character.snap(p.targets.character);s.distance.snap(p.targets.distance);s.ready=true;reset();return true;
}
bool ModalWetOuter::setFastTargets(ModalWetTargets p) noexcept {if(!state_||!state_->ready||!validTargets(p))return false;auto& s=*state_;s.targetDelayMs=p.predelayMs;s.character.set(p.character,s.duration);s.distance.set(p.distance,s.duration);return true;}
bool ModalWetOuter::setEarlyGeometries(const PreparedModalEarly& a,const PreparedModalEarly& b) noexcept {if(!state_||!state_->ready||state_->open||!validEarly(a,state_->rate)||!validEarly(b,state_->rate))return false;state_->oldEarly=a;state_->newEarly=b;return true;}
bool ModalWetOuter::excite(std::span<Frame> frames) noexcept {
    if(!state_||!state_->ready||state_->open||frames.size()>512||!finiteFrames(frames))return false;auto& s=*state_;s.count=frames.size();if(frames.empty())return true;
    const bool sameEarly=s.oldEarly.positions==s.newEarly.positions;
    for(std::size_t n=0;n<frames.size();++n){if(!s.delayFading&&s.targetDelayMs!=s.currentDelayMs){s.from=s.delay;s.toDelayMs=s.targetDelayMs;s.to=position(s.toDelayMs*.001*s.rate);s.delayPhase=0;s.delayFading=true;}
        Frame x{};for(std::size_t c=0;c<2;++c){const double raw=frames[n][c];
            if(s.delayFading){const double old=s.predelay[c].read(s.from,raw),next=s.predelay[c].read(s.to,raw);if(s.delayPhase==0)x[c]=old;else if(s.delayPhase>=s.duration)x[c]=next;else {const double alpha=double(s.delayPhase)/s.duration;x[c]=(1-alpha)*old+alpha*next;}}
            else x[c]=s.predelay[c].read(s.delay,raw);s.predelay[c].write(raw);}
        if(s.delayFading){if(s.delayPhase>=s.duration){s.delay=s.to;s.currentDelayMs=s.toDelayMs;s.delayFading=false;}else ++s.delayPhase;}
        Frame a{},b{};for(std::size_t c=0;c<2;++c)for(std::size_t j=0;j<3;++j){const auto source=j==1?1-c:c;a[c]+=gains[j]*s.reflections[source].read(s.oldEarly.positions[c][j],x[source]);}
        // Both rooms use the same early field outside a geometry transition.
        if(sameEarly)b=a;else for(std::size_t c=0;c<2;++c)for(std::size_t j=0;j<3;++j){const auto source=j==1?1-c:c;b[c]+=gains[j]*s.reflections[source].read(s.newEarly.positions[c][j],x[source]);}
        for(std::size_t c=0;c<2;++c)s.reflections[c].write(x[c]);s.earlyOld[n]=a;s.earlyNew[n]=b;s.characters[n]=s.character.tick();s.distances[n]=s.distance.tick();frames[n]=x;
    }s.open=true;s.combined=false;return true;
}
std::span<const double> ModalWetOuter::characters() const noexcept {return state_&&state_->ready?std::span(state_->characters).first(state_->count):std::span<const double>{};}
bool ModalWetOuter::mixRooms(std::span<Frame> old,std::span<const Frame> next,std::span<const double> alpha) noexcept {
    if(!state_||!state_->ready)return false;auto& s=*state_;if(old.empty()&&!s.open&&s.count==0)return next.empty()&&alpha.empty();
    const bool fading=!next.empty();if(!s.open||s.combined||old.size()!=s.count||(fading?(next.size()!=s.count||alpha.size()!=s.count||!alphas(alpha)):!alpha.empty()))return false;
    for(std::size_t n=0;n<s.count;++n){const double d=s.distances[n];auto a=old[n];Frame b{};if(fading)b=next[n];if(d!=1)for(std::size_t c=0;c<2;++c){a[c]=.8*(1-d)*s.earlyOld[n][c]+(.2+.8*d)*a[c];if(fading)b[c]=.8*(1-d)*s.earlyNew[n][c]+(.2+.8*d)*b[c];}
        if(fading&&alpha[n]==1)a=b;else if(fading&&alpha[n]!=0)for(std::size_t c=0;c<2;++c)a[c]=(1-alpha[n])*a[c]+alpha[n]*b[c];old[n]=a;
    }s.combined=true;return true;
}
bool ModalWetOuter::adoptPost(const PreparedModalPost& p) noexcept {if(!state_||!state_->ready||state_->open||state_->pending||!validPost(p,state_->rate))return false;PostEq next;if(!next.prepare(p.coefficients))return false;auto& s=*state_;s.post[1-s.active]=next;s.descriptors[1-s.active]=p;s.pending=true;s.alphaStarted=false;s.lastAlpha=0;return true;}
bool ModalWetOuter::post(std::span<Frame> frames,std::span<const double> alpha) noexcept {
    if(!state_||!state_->ready)return false;auto& s=*state_;if(frames.empty()&&!s.open&&s.count==0)return alpha.empty();
    if(!s.open||!s.combined||frames.size()!=s.count)return false;
    if(s.pending){if(alpha.size()!=s.count||!alphas(alpha)||(!s.alphaStarted&&alpha.front()!=0)||alpha.front()<s.lastAlpha)return false;for(std::size_t n=1;n<alpha.size();++n)if(alpha[n]<alpha[n-1])return false;}
    else if(!alpha.empty())return false;
    if(s.pending){
        // mixRooms consumed the early audio; this slot owns it until the next excite.
        const auto next=std::span(s.earlyOld).first(s.count);std::copy(frames.begin(),frames.end(),next.begin());
        if(!s.post[s.active].processBlock(frames)||!s.post[1-s.active].processBlock(next))return false;
        for(std::size_t n=0;n<s.count;++n){if(alpha[n]==1)frames[n]=next[n];else if(alpha[n]!=0)for(std::size_t c=0;c<2;++c)frames[n][c]=(1-alpha[n])*frames[n][c]+alpha[n]*next[n][c];}
    }else if(!s.post[s.active].processBlock(frames))return false;
    if(s.pending){s.lastAlpha=alpha.back();s.alphaStarted=true;}s.open=s.combined=false;return true;
}
bool ModalWetOuter::commitPost() noexcept {if(!state_||!state_->ready||state_->open||!state_->pending||!state_->alphaStarted||state_->lastAlpha!=1)return false;auto& s=*state_;s.active=1-s.active;s.pending=false;s.alphaStarted=false;s.lastAlpha=0;return true;}
bool ModalWetOuter::postPending() const noexcept {return state_&&state_->pending;}
const PreparedModalPost& ModalWetOuter::activePost() const noexcept {static constexpr PreparedModalPost empty;return state_?state_->descriptors[state_->active]:empty;}
void ModalWetOuter::reset() noexcept {if(!state_)return;auto& s=*state_;for(auto& r:s.predelay)r.reset();for(auto& r:s.reflections)r.reset();for(auto& p:s.post)p.reset();s.character.snapTarget();s.distance.snapTarget();s.currentDelayMs=s.targetDelayMs;s.delay=position(s.targetDelayMs*.001*s.rate);s.delayFading=false;s.delayPhase=0;s.open=s.combined=s.pending=s.alphaStarted=false;s.lastAlpha=0;s.count=0;}
std::size_t ModalWetOuter::storageBytes() const noexcept {if(!state_)return sizeof(*this);std::size_t bytes=sizeof(*this)+sizeof(State);for(const auto& r:state_->predelay)bytes+=r.data.capacity()*sizeof(double);for(const auto& r:state_->reflections)bytes+=r.data.capacity()*sizeof(double);return bytes;}
struct ModalFinalOuter::State {
    double rate=0,envelope=0,gain=1,peak=1;bool ready=false,open=false;std::size_t duration=0,count=0;
    Ramp width,mix,attack,release,minimumGain,threshold;
    std::array<Frame,1024> raw{};std::array<double,1024> widths{},mixes{},gains{};
};
ModalFinalOuter::ModalFinalOuter()=default;ModalFinalOuter::~ModalFinalOuter()=default;
bool ModalFinalOuter::prepareStorage(double rate){if(!rateValid(rate))return false;try{auto s=std::make_unique<State>();s->rate=rate;s->duration=std::size_t(std::ceil(.005*rate));state_=std::move(s);return true;}catch(const std::bad_alloc&){return false;}}
bool ModalFinalOuter::configure(const PreparedModalFinal& p) noexcept {if(!state_||p.rate!=state_->rate||!range(p.width,0,2)||!range(p.mix,0,1)||!validDuck(p.duck,p.rate))return false;auto& s=*state_;s.width.snap(p.width);s.mix.snap(p.mix);s.attack.snap(p.duck.coefficients.attack);s.release.snap(p.duck.coefficients.release);s.minimumGain.snap(p.duck.minimumGain);s.threshold.snap(p.duck.thresholdLinear);s.ready=true;reset();return true;}
bool ModalFinalOuter::setFastTargets(double width,double mix) noexcept {if(!state_||!state_->ready||!range(width,0,2)||!range(mix,0,1))return false;auto& s=*state_;s.width.set(width,s.duration);s.mix.set(mix,s.duration);return true;}
bool ModalFinalOuter::adoptDuck(const PreparedModalDuck& p) noexcept {if(!state_||!state_->ready||!validDuck(p,state_->rate))return false;auto& s=*state_;s.attack.set(p.coefficients.attack,s.duration);s.release.set(p.coefficients.release,s.duration);s.minimumGain.set(p.minimumGain,s.duration);s.threshold.set(p.thresholdLinear,s.duration);return true;}
bool ModalFinalOuter::capture(std::span<const Frame> frames) noexcept {
    if(!state_||!state_->ready||state_->open||frames.size()>1024||!finiteFrames(frames))return false;auto& s=*state_;s.count=frames.size();if(frames.empty())return true;std::copy(frames.begin(),frames.end(),s.raw.begin());
    for(std::size_t n=0;n<s.count;++n){const double level=std::max(std::abs(frames[n][0]),std::abs(frames[n][1]));s.peak=std::max(s.peak,level);const double attack=s.attack.tick(),release=s.release.tick(),floor=s.minimumGain.tick(),threshold=s.threshold.tick(),a=level>s.envelope?attack:release;
        const double limit=std::max(s.envelope,level);s.envelope=std::clamp(a*s.envelope+(1-a)*level,0.,limit);s.gain=floor==1||s.envelope<=threshold?1:std::max(floor,threshold/s.envelope);
        s.gains[n]=s.gain;s.widths[n]=s.width.tick();s.mixes[n]=s.mix.tick();
    }s.open=true;return true;
}
bool ModalFinalOuter::finish(std::span<Frame> frames) noexcept {
    if(!state_||!state_->ready)return false;auto& s=*state_;if(frames.empty()&&!s.open&&s.count==0)return true;if(!s.open||frames.size()!=s.count)return false;
    for(std::size_t n=0;n<s.count;++n){auto wet=frames[n];if(s.widths[n]!=1){const double m=(wet[0]+wet[1])*.5,side=(wet[0]-wet[1])*.5;wet={m+s.widths[n]*side,m-s.widths[n]*side};}
        if(s.gains[n]!=1)for(auto& v:wet)v*=s.gains[n];if(s.mixes[n]==0)wet=s.raw[n];else if(s.mixes[n]!=1)for(std::size_t c=0;c<2;++c)wet[c]=(1-s.mixes[n])*s.raw[n][c]+s.mixes[n]*wet[c];frames[n]=wet;}
    s.open=false;return true;
}
double ModalFinalOuter::duckEnvelope() const noexcept {return state_?state_->envelope:0;}
double ModalFinalOuter::duckGain() const noexcept {return state_?state_->gain:1;}
double ModalFinalOuter::observedPeak() const noexcept {return state_?state_->peak:1;}
void ModalFinalOuter::reset() noexcept {if(!state_)return;auto& s=*state_;s.width.snapTarget();s.mix.snapTarget();s.attack.snapTarget();s.release.snapTarget();s.minimumGain.snapTarget();s.threshold.snapTarget();s.envelope=0;s.gain=1;s.peak=1;s.open=false;s.count=0;}
std::size_t ModalFinalOuter::storageBytes() const noexcept {return sizeof(*this)+(state_?sizeof(State):0);}
}

#include "SpaceReverbField.h"
#include "SpaceReverbTail.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>
namespace airack::space {
static_assert(std::is_trivially_copyable_v<PreparedField>);
namespace {
constexpr std::array<std::array<double,3>,2> earlySeconds{{{.0073,.0111,.0179},{.0089,.0137,.0193}}};
constexpr std::array<double,3> earlyGains{.42,.25,.18};
bool rateValid(double rate) noexcept {return std::isfinite(rate)&&rate>=8000&&rate<=768000;}
struct History {
    std::vector<double> data;std::size_t cursor=0,written=0;
    void reset() noexcept {cursor=written=0;}
    double age(std::size_t n) const noexcept {if(n==0||n>written||n>data.size())return 0;return data[cursor>=n?cursor-n:cursor+data.size()-n];}
    void write(double x) noexcept {data[cursor]=x;if(++cursor==data.size())cursor=0;if(written<data.size())++written;}
};
struct ReadPosition {std::size_t whole=0;double fraction=0;};
ReadPosition position(double samples) noexcept {const double whole=std::floor(samples);return {std::size_t(whole),samples-whole};}
double read(const History& history,double current,ReadPosition p) noexcept {
    const double a=p.whole?history.age(p.whole):current;if(p.fraction==0)return a;
    return (1-p.fraction)*a+p.fraction*history.age(p.whole+1);
}
bool fieldTail(PreparedField& p,detail::PreparationControl control) {
    control.checkpoint();
    const double rate=p.tank.sampleRate;std::array<detail::LossBank,8> banks;detail::LossNetwork network;network.sampleRate=rate;network.control=control;network.mixing.resize(64);
    constexpr double normalization=.3535533905932737622;
    for(std::size_t i=0;i<8;++i){control.checkpoint();const auto& source=p.tank.lines[i];auto& bank=banks[i];bank.scalar=source.scalar;
        for(std::size_t j=0;j<source.count;++j){const auto& c=source.coefficients[j];bank.stages.push_back({c.b0,c.b1,c.b2,c.a1,c.a2});}
        network.lines.push_back({double(source.length)/rate,&bank});
        for(std::size_t j=0;j<8;++j){std::size_t bits=i&j;bool odd=false;while(bits){odd=!odd;bits&=bits-1;}network.mixing[i*8+j]=odd?-normalization:normalization;}
    }
    double maximumTime=.1;const double upper=std::min(20000.,.45*rate),targetRate=std::min(rate,384000.);
    for(int i=0;i<193;++i){control.checkpoint();maximumTime=std::max(maximumTime,evaluate(p.settings.decay,20*std::pow(upper/20,i/192.),targetRate)->seconds);}
    for(const auto& b:p.settings.decay.bands){control.checkpoint();if(b.active&&!b.bypassed)maximumTime=std::max(maximumTime,evaluate(p.settings.decay,b.frequencyHz,targetRate)->seconds);}
    const auto attempt=[&](double timeBound){control.checkpoint();const auto certificate=detail::certifyTail(network,timeBound);if(!certificate.passed)return false;
        const double radius=certificate.radius,logRadius=std::log(radius),m=certificate.maxLoopMagnitude;
        if(!(m>0&&m<1))return false;
        for(const auto& channel:p.tank.diffusion)for(const auto& d:channel)if(!(d.attenuation*std::exp(-double(d.length)*logRadius)<1))return false;
        control.checkpoint();const auto eq=boundPostEq(p.post,radius);if(!eq.passed)return false;
        double maximumEarly=0;for(const auto& channel:p.early)for(const auto& tap:channel)maximumEarly=std::max(maximumEarly,tap.samples);
        const double earlyGain=.8*(1-p.settings.distance),lateGain=.2+.8*p.settings.distance;
        const double earlyLog=earlyGain>0?std::log(earlyGain*.85)-std::ceil(maximumEarly)*logRadius:-std::numeric_limits<double>::infinity();
        const double lateLog=std::log(lateGain)+.5*std::log(.5)+std::log(m)-std::log1p(-m);
        const double largest=std::max(earlyLog,lateLog),roomLog=largest+std::log(std::exp(earlyLog-largest)+std::exp(lateLog-largest));
        const double logTransfer=roomLog-std::ceil(p.settings.predelayMs*.001*rate)*logRadius+eq.logMagnitude+std::log(std::max(1.,p.settings.width));
        const auto tail=detail::makeSpaceTailBound(logTransfer,rate,timeBound);if(!tail.passed)return false;p.tail=tail;return true;
    };
    const double tighter=std::min(46.,1.15*maximumTime);if(attempt(tighter))return true;return tighter!=46&&attempt(46);
}
}
namespace detail {
TailBound makeSpaceTailBound(double logTransfer,double rate,double timeBound) noexcept {
    TailBound result;if(!std::isfinite(logTransfer)||!rateValid(rate)||!std::isfinite(timeBound)||timeBound<=0||timeBound>1000)return result;
    const double radius=std::exp(-std::log(1000.)/(rate*timeBound));if(!(radius>0&&radius<1))return result;
    const double logRadius=std::log(radius),logStop=logTransfer+.5*std::log(2.)-.5*std::log(-std::expm1(2*logRadius));
    const double frames=std::ceil(std::max(0.,(logStop-std::log(1e-6))/(-logRadius)))+1,seconds=frames/rate;
    if(!std::isfinite(seconds)||seconds<0)return result;
    return {true,seconds,timeBound,radius,logTransfer,logStop};
}
}
PostEqBound boundPostEq(const PreparedPostEq& p,double radius) noexcept {
    PostEqBound result;PostEq validation;if(!validation.prepare(p)||!std::isfinite(radius)||radius<=0||radius>1)return result;
    for(std::size_t i=0;i<p.count;++i){const auto& c=p.stages[i];const detail::Biquad section{c.b0,c.b1,c.b2,c.a1,c.a2};
        const auto bound=detail::boundSectionMagnitude(section,p.sampleRate,radius);if(!bound.passed)return result;
        result.logMagnitude+=bound.maximumLog;result.maximumPole=std::max(result.maximumPole,bound.maximumPole);
    }
    result.passed=std::isfinite(result.logMagnitude);return result;
}
bool prepareField(const Settings& settings,double rate,PreparedField& out,FieldPreparationReport* report,detail::PreparationControl control) noexcept {
    FieldPreparationReport status;const auto fail=[&](const char* reason){status.reason=reason;if(report)*report=status;return false;};
    if(!valid(settings)||!rateValid(rate))return fail("invalid_input");
    try {
        control.checkpoint();
        PreparedField p;p.settings=settings;
        if(!detail::prepareTank(settings.decay,settings.size,rate,p.tank,&status.tank,control))return fail(status.tank.detail);
        control.checkpoint();if(!preparePostEq(settings,rate,p.post))return fail("post_eq");
        control.checkpoint();if(!prepareDuck(settings,rate,p.duck))return fail("duck");
        const double scale=.75+.5*settings.size;
        for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<3;++i){control.checkpoint();p.early[ch][i]={earlySeconds[ch][i]*scale*rate,earlyGains[i],i==1};}
        if(!fieldTail(p,control))return fail("field_tail");
        control.checkpoint();out=p;status.reason="pass";if(report)*report=status;return true;
    }catch(const detail::PreparationCancelled&){return fail("cancelled");}
    catch(const std::bad_alloc&){return fail("allocation");}catch(...){return fail("preparation_exception");}
}
struct FieldRuntime::State {
    PreparedField prepared;double rate=0;bool ready=false;
    detail::TankRuntime tank;PostEq post;DuckDetector duck;
    std::array<History,2> predelay,early;
    ReadPosition predelayPosition;std::array<std::array<ReadPosition,3>,2> earlyPositions{};
};
FieldRuntime::FieldRuntime()=default;FieldRuntime::~FieldRuntime()=default;
FieldRuntime::FieldRuntime(FieldRuntime&&) noexcept=default;FieldRuntime& FieldRuntime::operator=(FieldRuntime&&) noexcept=default;
bool FieldRuntime::prepareStorage(double rate,detail::RenderBackend backend) {
    if(!rateValid(rate))return false;
    try {auto candidate=std::make_unique<State>();candidate->rate=rate;if(!candidate->tank.prepareStorage(rate,backend))return false;
        for(auto& ring:candidate->predelay)ring.data.resize(std::size_t(std::ceil(.25*rate))+2);
        for(auto& ring:candidate->early)ring.data.resize(std::size_t(std::ceil(.0193*1.25*rate))+2);
        state_=std::move(candidate);return true;
    }catch(const std::bad_alloc&){return false;}
}
bool FieldRuntime::configure(const PreparedField& p) noexcept {
    if(!state_||!valid(p.settings)||p.tank.sampleRate!=state_->rate||p.tank.curve!=p.settings.decay||p.tank.size!=p.settings.size
        ||p.post.sampleRate!=state_->rate||p.duck.sampleRate!=state_->rate||p.duck.thresholdDb!=p.settings.duckThresholdDb||p.duck.rangeDb!=p.settings.duckRangeDb
        ||!p.tail.passed||!std::isfinite(p.tail.seconds)||p.tail.seconds<=0||!std::isfinite(p.tail.timeBound)||p.tail.timeBound<=0
        ||!std::isfinite(p.tail.radius)||p.tail.radius<=0||p.tail.radius>=1||!std::isfinite(p.tail.logTransferBound)||!std::isfinite(p.tail.logOutputBoundAtStop))return false;
    const double scale=.75+.5*p.settings.size;
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<3;++i){const auto& tap=p.early[ch][i];
        if(tap.samples!=earlySeconds[ch][i]*scale*state_->rate||tap.gain!=earlyGains[i]||tap.cross!=(i==1))return false;}
    PostEq post;DuckDetector duck;if(!post.prepare(p.post)||!duck.prepare(p.duck))return false;
    if(!state_->tank.configure(p.tank))return false;
    auto& s=*state_;s.prepared=p;s.post=post;s.duck=duck;s.predelayPosition=position(p.settings.predelayMs*.001*s.rate);
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<3;++i)s.earlyPositions[ch][i]=position(p.early[ch][i].samples);
    s.ready=true;reset();return true;
}
void FieldRuntime::reset() noexcept {
    if(!state_)return;state_->tank.reset();state_->post.reset();state_->duck.reset();
    for(auto& ring:state_->predelay)ring.reset();for(auto& ring:state_->early)ring.reset();
}
std::array<double,2> FieldRuntime::tick(std::array<double,2> input) noexcept {
    if(!state_||!state_->ready)return {};auto& s=*state_;const auto& settings=s.prepared.settings;
    const double gain=s.duck.tick(input);std::array<double,2> excitation,early{};
    for(std::size_t ch=0;ch<2;++ch){excitation[ch]=read(s.predelay[ch],input[ch],s.predelayPosition);s.predelay[ch].write(input[ch]);}
    for(std::size_t ch=0;ch<2;++ch)for(std::size_t i=0;i<3;++i){const auto& tap=s.prepared.early[ch][i];const auto source=tap.cross?1-ch:ch;
        early[ch]+=tap.gain*read(s.early[source],excitation[source],s.earlyPositions[ch][i]);}
    for(std::size_t ch=0;ch<2;++ch)s.early[ch].write(excitation[ch]);
    auto wet=s.tank.tick(excitation,settings.character);
    if(settings.distance!=1){const double earlyGain=.8*(1-settings.distance),lateGain=.2+.8*settings.distance;for(std::size_t ch=0;ch<2;++ch)wet[ch]=earlyGain*early[ch]+lateGain*wet[ch];}
    wet=s.post.tick(wet);
    if(settings.width!=1){const double mid=(wet[0]+wet[1])*.5,side=(wet[0]-wet[1])*.5;wet={mid+settings.width*side,mid-settings.width*side};}
    if(gain!=1)for(auto& x:wet)x*=gain;
    if(settings.mix==0)return input;if(settings.mix==1)return wet;
    for(std::size_t ch=0;ch<2;++ch)wet[ch]=(1-settings.mix)*input[ch]+settings.mix*wet[ch];return wet;
}
const PreparedField& FieldRuntime::prepared() const noexcept {static constexpr PreparedField empty;return state_?state_->prepared:empty;}
std::size_t FieldRuntime::storageBytes() const noexcept {
    if(!state_)return sizeof(*this);std::size_t bytes=sizeof(*this)+sizeof(State)+state_->tank.storageBytes()-sizeof(detail::TankRuntime);
    for(const auto& ring:state_->predelay)bytes+=ring.data.capacity()*sizeof(double);for(const auto& ring:state_->early)bytes+=ring.data.capacity()*sizeof(double);return bytes;
}
double FieldRuntime::duckEnvelope() const noexcept {return state_?state_->duck.envelope():0;}
}

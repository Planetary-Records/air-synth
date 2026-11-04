#include "ModalRoom.h"
#include "ModalPreparedCopy.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace airack::space::runtime {
namespace {
constexpr std::array<double,8> diffusion{.00071,.00113,.00173,.00231,.00317,.00419,.00527,.00661};
bool rateValid(double rate) noexcept {return std::isfinite(rate)&&rate>=8000&&rate<=768000;}
struct Position {std::size_t whole=0;double fraction=0;};
Position position(double samples) noexcept {const double n=std::floor(samples);return {std::size_t(n),samples-n};}
struct Ring {
    std::vector<double> data;std::size_t cursor=0,written=0;
    void reset() noexcept {cursor=written=0;}
    double age(std::size_t n) const noexcept {if(n==0||n>written||n>data.size())return 0;return data[cursor>=n?cursor-n:cursor+data.size()-n];}
    double read(Position p,double now) const noexcept {const double a=p.whole?age(p.whole):now;if(p.fraction==0)return a;return (1-p.fraction)*a+p.fraction*age(p.whole+1);}
    void write(double v) noexcept {data[cursor]=v;if(++cursor==data.size())cursor=0;if(written<data.size())++written;}
    void clone(const Ring& source) noexcept {std::copy(source.data.begin(),source.data.end(),data.begin());cursor=source.cursor;written=source.written;}
    // Ages decrease as the block advances; missing initial history remains zero.
    void readAges(std::size_t oldest,std::span<double> out) const noexcept {
        if(out.empty())return;
        const auto missing=std::min(out.size(),oldest>written?oldest-written:0);
        std::fill_n(out.begin(),missing,0.);out=out.subspan(missing);if(out.empty())return;
        const auto firstAge=oldest-missing,index=cursor>=firstAge?cursor-firstAge:cursor+data.size()-firstAge;
        const auto first=std::min(out.size(),data.size()-index);
        std::copy_n(data.begin()+index,first,out.begin());std::copy_n(data.begin(),out.size()-first,out.begin()+first);
    }
    void writeBlock(std::span<const double> input) noexcept {
        if(input.empty())return;const auto count=input.size(),capacity=data.size(),next=(cursor+count)%capacity;
        auto at=cursor;if(count>=capacity){input=input.last(capacity);at=next;}
        const auto first=std::min(input.size(),capacity-at);
        std::copy_n(input.begin(),first,data.begin()+at);std::copy_n(input.begin()+first,input.size()-first,data.begin());
        cursor=next;written=count>=capacity?capacity:std::min(capacity,written+count);
    }
};
bool validRoom(const PreparedModalRoom& p,double rate) noexcept {
    if(p.modal.sampleRate!=rate||!valid(p.modal.curve)||!std::isfinite(p.modal.size)||p.modal.size<0||p.modal.size>1)return false;
    for(std::size_t j=0;j<8;++j)if(p.diffusionSamples[j]!=diffusion[j]*(.5+1.5*p.modal.size)*rate)return false;
    for(auto step:p.oscillatorStep)if(!std::isfinite(step[0])||!std::isfinite(step[1])||step[0]<=0||step[0]>=.001||step[1]<=0||std::abs(step[0]*step[0]+step[1]*step[1]-1)>1e-12)return false;
    return true;
}
}
struct ModalRoom::State {
    PreparedModalRoom prepared;ModalTank modal;double rate=0;bool ready=false,unusedZero=true,blockedScatter=false;
    std::array<Ring,8> scatter;std::array<Ring,2> chorus;std::array<Position,8> positions{};
    std::array<std::array<double,2>,2> phase{},initial{};std::size_t clock=0;
    std::array<double,512> scatterInput{};std::array<double,513> scatterDelay{};
};
ModalRoom::ModalRoom()=default;ModalRoom::~ModalRoom()=default;
bool prepareModalRoom(const DecayCurve& curve,double size,double rate,std::size_t modes,PreparedModalRoom& out,detail::PreparationControl control) noexcept {
    if(!valid(curve)||!rateValid(rate)||!std::isfinite(size)||size<0||size>1||(modes!=128&&modes!=192&&modes!=256&&modes!=512&&modes!=1024&&modes!=2048))return false;
    try {control.checkpoint();auto p=std::make_unique<PreparedModalRoom>();if(!prepareModal(curve,size,rate,modes,p->modal,control))return false;
        for(std::size_t j=0;j<8;++j)p->diffusionSamples[j]=diffusion[j]*(.5+1.5*size)*rate;
        for(std::size_t c=0;c<2;++c){const double angle=2*std::numbers::pi*(c?.31:.23)/rate;p->oscillatorStep[c]={std::sin(angle),std::cos(angle)};}
        control.checkpoint();out=*p;return true;
    }catch(...){return false;}
}
bool ModalRoom::prepareStorage(double rate,ModalBackend backend,bool blockedScatter,bool pairedReduction){
    if(!rateValid(rate))return false;try {auto s=std::make_unique<State>();s->rate=rate;s->blockedScatter=blockedScatter;if(!s->modal.prepareStorage(rate,backend,pairedReduction))return false;
        for(std::size_t j=0;j<8;++j)s->scatter[j].data.resize(std::size_t(std::ceil(diffusion[j]*2*rate))+2);
        for(auto& r:s->chorus)r.data.resize(std::size_t(std::ceil(.013*rate))+2);
        s->initial={{{0,1},{std::sin(.37*std::numbers::pi),std::cos(.37*std::numbers::pi)}}};s->phase=s->initial;state_=std::move(s);return true;
    }catch(const std::bad_alloc&){return false;}
}
bool ModalRoom::configure(const PreparedModalRoom& p) noexcept {
    return configureImpl(p,false);
}
bool ModalRoom::configureImpl(const PreparedModalRoom& p,bool factory) noexcept {
    if(!state_||!validRoom(p,state_->rate)||!state_->modal.configureImpl(p.modal,factory))return false;
    auto& s=*state_;
    if(factory){ModalFactoryCopy::assign(s.prepared.modal,p.modal,s.unusedZero);s.prepared.diffusionSamples=p.diffusionSamples;s.prepared.oscillatorStep=p.oscillatorStep;}
    else {s.prepared=p;s.unusedZero=false;}
    for(std::size_t j=0;j<8;++j)s.positions[j]=position(p.diffusionSamples[j]);s.ready=true;
    if(factory)resetHistory();else reset();
    return true;
}
bool ModalRoom::adoptCopied(const PreparedModalRoom& p,const ModalRoom& source) noexcept {
    if(this==&source||!state_||!source.state_||!source.state_->ready||state_->rate!=source.state_->rate||p.modal.count!=source.state_->prepared.modal.count||!validRoom(p,state_->rate))return false;
    auto& s=*state_;const auto& from=*source.state_;
    for(std::size_t j=0;j<8;++j)if(s.scatter[j].data.size()!=from.scatter[j].data.size())return false;
    for(std::size_t c=0;c<2;++c)if(s.chorus[c].data.size()!=from.chorus[c].data.size())return false;
    if(!s.modal.adoptCopied(p.modal,from.modal))return false;
    s.prepared=p;s.unusedZero=false;for(std::size_t j=0;j<8;++j){s.positions[j]=position(p.diffusionSamples[j]);s.scatter[j].clone(from.scatter[j]);}
    for(std::size_t c=0;c<2;++c)s.chorus[c].clone(from.chorus[c]);s.phase=from.phase;s.clock=from.clock;s.ready=true;return true;
}
bool ModalRoom::adoptPreparedCopied(const PreparedModalRoom& p,const ModalRoom& source) noexcept {
    if(this==&source||!state_||!source.state_||!source.state_->ready||state_->rate!=source.state_->rate||p.modal.count!=source.state_->prepared.modal.count||!validRoom(p,state_->rate))return false;
    auto& s=*state_;const auto& from=*source.state_;
    for(std::size_t j=0;j<8;++j)if(s.scatter[j].data.size()!=from.scatter[j].data.size())return false;
    for(std::size_t c=0;c<2;++c)if(s.chorus[c].data.size()!=from.chorus[c].data.size())return false;
    if(!s.modal.adoptPreparedCopied(p.modal,from.modal))return false;
    ModalFactoryCopy::assign(s.prepared.modal,p.modal,s.unusedZero);
    s.prepared.diffusionSamples=p.diffusionSamples;s.prepared.oscillatorStep=p.oscillatorStep;
    for(std::size_t j=0;j<8;++j){s.positions[j]=position(p.diffusionSamples[j]);s.scatter[j].clone(from.scatter[j]);}
    for(std::size_t c=0;c<2;++c)s.chorus[c].clone(from.chorus[c]);s.phase=from.phase;s.clock=from.clock;s.ready=true;return true;
}
bool ModalRoom::processScatterBlocked(std::span<Frame> frames) noexcept {
    auto& s=*state_;const auto count=frames.size();
    // Valid prepared room geometry always leaves space for the adjacent tap.
    for(std::size_t j=0;j<8;++j)if(s.scatter[j].data.size()<2||s.positions[j].whole>=s.scatter[j].data.size()-1)return false;
    for(std::size_t j=0;j<8;++j){auto& ring=s.scatter[j];const auto tap=s.positions[j];std::size_t copied=0;
#if defined(_M_X64) || defined(__SSE2__)
        for(;copied+1<count;copied+=2){const auto a=_mm_loadu_pd(frames[copied].data()),b=_mm_loadu_pd(frames[copied+1].data());_mm_storeu_pd(s.scatterInput.data()+copied,_mm_unpackhi_pd(a,b));}
#endif
        for(;copied<count;++copied)s.scatterInput[copied]=frames[copied][1];
        s.scatterDelay[0]=tap.fraction==0?0.:ring.age(tap.whole+1);
        auto delayed=std::span(s.scatterDelay).subspan(1,count);const auto oldCount=std::min(count,tap.whole);
        ring.readAges(tap.whole,delayed.first(oldCount));
        if(oldCount<count)std::copy_n(s.scatterInput.begin(),count-oldCount,delayed.begin()+oldCount);
        std::size_t n=0;
#if defined(_M_X64) || defined(__SSE2__)
        const auto c6=_mm_set1_pd(.6),c8=_mm_set1_pd(.8),negative8=_mm_set1_pd(-.8);
        const auto fraction=_mm_set1_pd(tap.fraction),complement=_mm_set1_pd(1-tap.fraction);
        for(;n+1<count;n+=2){auto d=_mm_loadu_pd(s.scatterDelay.data()+n+1);
            if(tap.fraction!=0)d=_mm_add_pd(_mm_mul_pd(complement,d),_mm_mul_pd(fraction,_mm_loadu_pd(s.scatterDelay.data()+n)));
            const auto a=_mm_loadu_pd(frames[n].data()),b=_mm_loadu_pd(frames[n+1].data()),left=_mm_unpacklo_pd(a,b);
            const auto outLeft=_mm_add_pd(_mm_mul_pd(c6,left),_mm_mul_pd(c8,d)),outRight=_mm_add_pd(_mm_mul_pd(negative8,left),_mm_mul_pd(c6,d));
            _mm_storeu_pd(frames[n].data(),_mm_unpacklo_pd(outLeft,outRight));_mm_storeu_pd(frames[n+1].data(),_mm_unpackhi_pd(outLeft,outRight));
        }
#endif
        for(;n<count;++n){const double d=tap.fraction==0?s.scatterDelay[n+1]:(1-tap.fraction)*s.scatterDelay[n+1]+tap.fraction*s.scatterDelay[n];
            const double left=frames[n][0];frames[n]={.6*left+.8*d,-.8*left+.6*d};}
        ring.writeBlock(std::span(s.scatterInput).first(count));
    }return true;
}
bool ModalRoom::processBlock(std::span<Frame> frames,std::span<const double> characters) noexcept {
    if(!state_||!state_->ready||frames.size()>512||characters.size()!=frames.size())return false;
    for(double c:characters)if(!std::isfinite(c)||c<0||c>1)return false;if(frames.empty())return true;auto& s=*state_;
    if(s.blockedScatter){if(!processScatterBlocked(frames))return false;}else {
    for(std::size_t j=0;j<8;++j){auto& ring=s.scatter[j];const auto tap=s.positions[j];
        for(auto& x:frames){const double delayed=ring.read(tap,x[1]);ring.write(x[1]);const double left=x[0];x={.6*left+.8*delayed,-.8*left+.6*delayed};}
    }
    }
    if(!s.modal.processBlock(frames))return false;
    for(std::size_t n=0;n<frames.size();++n){auto& wet=frames[n];const double character=characters[n];
        for(std::size_t c=0;c<2;++c){const double sine=std::clamp(s.phase[c][0],-1.,1.),samples=(c?.008+.005*sine:.006+.004*sine)*s.rate;
            const double moved=s.chorus[c].read(position(samples),wet[c]);s.chorus[c].write(wet[c]);if(character!=0)wet[c]=(1-character)*wet[c]+character*moved;
            const auto phase=s.phase[c],step=s.prepared.oscillatorStep[c];s.phase[c]={phase[0]*step[1]+phase[1]*step[0],phase[1]*step[1]-phase[0]*step[0]};}
        if((s.clock=(s.clock+1)&4095)==0)for(auto& phase:s.phase){const double norm=std::hypot(phase[0],phase[1]);phase[0]/=norm;phase[1]/=norm;}
    }return true;
}
void ModalRoom::resetHistory() noexcept {auto& s=*state_;for(auto& r:s.scatter)r.reset();for(auto& r:s.chorus)r.reset();s.phase=s.initial;s.clock=0;}
void ModalRoom::reset() noexcept {if(!state_)return;state_->modal.reset();resetHistory();}
const PreparedModalRoom& ModalRoom::prepared() const noexcept {static constexpr PreparedModalRoom empty;return state_?state_->prepared:empty;}
ModalSample ModalRoom::modeState(std::size_t k) const noexcept {return state_?state_->modal.modeState(k):ModalSample{};}
std::size_t ModalRoom::storageBytes() const noexcept {if(!state_)return sizeof(*this);const auto& s=*state_;std::size_t bytes=sizeof(*this)+sizeof(State)+s.modal.storageBytes()-sizeof(ModalTank);
    for(const auto& r:s.scatter)bytes+=r.data.capacity()*sizeof(double);for(const auto& r:s.chorus)bytes+=r.data.capacity()*sizeof(double);return bytes;}
bool ModalRoom::usesBlockedScatter() const noexcept {return state_&&state_->blockedScatter;}
bool ModalRoom::usesPairedReduction() const noexcept {return state_&&state_->modal.usesPairedReduction();}
std::size_t ModalRoom::historyCopyBytes() const noexcept {if(!state_||!state_->ready)return 0;const auto& s=*state_;std::size_t bytes=2*s.prepared.modal.count*sizeof(double)+sizeof(s.phase)+sizeof(s.clock);
    for(const auto& r:s.scatter)bytes+=r.data.size()*sizeof(double)+sizeof(r.cursor)+sizeof(r.written);for(const auto& r:s.chorus)bytes+=r.data.size()*sizeof(double)+sizeof(r.cursor)+sizeof(r.written);return bytes;}
}

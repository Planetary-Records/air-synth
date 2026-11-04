#include "ModalLiveTail.h"
#include "ModalTailMath.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace airack::space::runtime {
namespace {constexpr unsigned lastCircle=1U<<11;constexpr std::array<double,8> diffusion{.00071,.00113,.00173,.00231,.00317,.00419,.00527,.00661};}
bool prepareModalTailContext(double rate,std::size_t modes,ModalTailContext& out,ModalTailTransport transport) noexcept {
    if(!std::isfinite(transport.logGain)||transport.logGain<0)return false;
    if(!std::isfinite(rate)||rate<8000||rate>768000||(modes!=128&&modes!=192&&modes!=256&&modes!=512&&modes!=1024&&modes!=2048))return false;
    ModalTailContext c;c.rate_=rate;c.modes_=modes;c.ln2_=std::log(2.);c.negativeLogFloor_=-std::log(1e-6);
    const double reference=std::sqrt(-std::expm1(-std::log(1000.)/48000.)),referenceLoss=-std::expm1(-std::log(1000.)/(2*48000.));
    c.pickup_=std::sqrt(4./modes)*reference*(-std::expm1(-std::log(1000.)/(2*rate))/referenceLoss);
    for(unsigned b=0;b<12;++b){c.rho_[b]=std::exp(-std::log(1000.)/(rate*modalTailTimes[b]));c.inverse_[b]=1/(-std::log(c.rho_[b]));}
    for(unsigned n=0;n<8;++n)c.sourceLogs_[n]=std::log(double(n+1));
    std::uint64_t diffuse=0;for(double d:diffusion)diffuse+=std::uint64_t(std::ceil(2*d*rate))+1;
    c.support_=std::uint64_t(std::ceil(.25*rate))+1+std::max(diffuse+std::uint64_t(std::ceil(.013*rate))+1,std::uint64_t(std::ceil(.0193*1.25*rate))+1);
    if(transport.supportFrames>UINT64_MAX-c.support_)return false;c.support_+=transport.supportFrames;c.transportLogGain_=transport.logGain;out=c;return true;
}
bool prepareModalRoomTail(const ModalTailContext& c,const PreparedModalRoom& p,PreparedModalTail& out) noexcept {
    if(c.rate()==0||p.modal.sampleRate!=c.rate()||p.modal.count!=c.modes()||p.modal.pickup!=c.pickup())return false;
    // Preparation only: use the sounding implementation's full descriptor admission.
    ModalRoom validator;if(!validator.prepareStorage(c.rate())||!validator.configure(p))return false;
    double maximum=0;long double squared=0;
    for(std::size_t k=0;k<c.modes();++k){maximum=std::max(maximum,std::hypot(p.modal.rc[k],p.modal.rs[k]));const auto w=p.modal.inputWeights[k/4];
        squared+=(static_cast<long double>(p.modal.bReal[k])*p.modal.bReal[k]+static_cast<long double>(p.modal.bImag[k])*p.modal.bImag[k])*(static_cast<long double>(w[0])*w[0]+static_cast<long double>(w[1])*w[1]);}
    maximum+=64*std::numeric_limits<double>::epsilon();
    const double trace=squared==0?0:std::nextafter(double(squared),std::numeric_limits<double>::infinity())*(1+1e-12);
    PreparedModalTail t;t.rate_=c.rate();t.modes_=c.modes();t.domain_=1;t.maximumRadius_=maximum;t.injectionSquaredNorm_=trace;
    for(unsigned b=0;b<12;++b)if(maximum<c.radius(b)){
        const long double modal=c.pickup()*std::sqrt(static_cast<long double>(c.modes())*trace)*std::pow(1.4L,8)/(1-static_cast<long double>(maximum)/c.radius(b));
        const double log=double(std::log(modal+.68L))+1e-11;if(!std::isfinite(log))return false;t.logs_[b]=std::max(0.,log);t.mask_|=1U<<b;}
    if(!(t.mask_&lastCircle))return false;out=t;return true;
}
bool prepareModalPostTail(const ModalTailContext& c,const PreparedModalPost& p,PreparedModalTail& out) noexcept {
    PostEq validator;if(c.rate()==0||p.coefficients.sampleRate!=c.rate()||!std::isfinite(p.commonLogL1)||p.commonLogL1<0||!validator.prepare(p.coefficients))return false;
    PreparedModalTail t;t.rate_=c.rate();t.modes_=c.modes();t.domain_=2;
    for(unsigned b=0;b<12;++b){double log=0,maximum=0;bool usable=true;for(std::size_t j=0;j<p.coefficients.count;++j){const auto one=boundModalPostSection(p.coefficients.stages[j],c.radius(b));if(!one.passed){usable=false;break;}log+=one.logL1;maximum=std::max(maximum,one.maximumPole);}
        if(usable){t.mask_|=1U<<b;t.logs_[b]=std::max(0.,log)+1e-11;t.maximumRadius_=std::max(t.maximumRadius_,maximum);}}
    if(!(t.mask_&lastCircle))return false;out=t;return true;
}
bool ModalLiveTail::matches(const PreparedModalTail& t,unsigned domain) const noexcept {
    if(context_.rate_==0||t.rate_!=context_.rate_||t.modes_!=context_.modes_||t.domain_!=domain||!(t.mask_&lastCircle))return false;
    for(unsigned b=0;b<12;++b)if((t.mask_&(1U<<b))&&(!std::isfinite(t.logs_[b])||t.logs_[b]<0))return false;return true;
}
bool ModalLiveTail::configure(const ModalTailContext& c,const PreparedModalTail& room,const PreparedModalTail& post) noexcept {
    if(c.rate_==0||room.rate_!=c.rate_||post.rate_!=c.rate_||room.modes_!=c.modes_||post.modes_!=c.modes_||room.domain_!=1||post.domain_!=2||!(room.mask_&post.mask_&lastCircle))return false;
    context_=c;room_=room.logs_;post_=post.logs_;mask_=room.mask_&post.mask_;peakExponent_=0;return true;
}
bool ModalLiveTail::admitRoom(const PreparedModalTail& t) noexcept {if(!mask_||!matches(t,1))return false;for(unsigned b=0;b<12;++b)room_[b]=std::max(room_[b],t.logs_[b]);mask_&=t.mask_;return true;}
bool ModalLiveTail::admitPost(const PreparedModalTail& t) noexcept {if(!mask_||!matches(t,2))return false;for(unsigned b=0;b<12;++b)post_[b]=std::max(post_[b],t.logs_[b]);mask_&=t.mask_;return true;}
bool ModalLiveTail::observePeak(double peak) noexcept {
    if(!mask_||!std::isfinite(peak)||peak<0)return false;if(peak<=1)return true;const auto bits=std::bit_cast<std::uint64_t>(peak);
    const int exponent=int((bits>>52)&0x7ff)-1023+int((bits&0xfffffffffffffULL)!=0);peakExponent_=std::max(peakExponent_,exponent);return true;
}
bool ModalLiveTail::reset(const PreparedModalTail& room,const PreparedModalTail& post) noexcept {if(!matches(room,1)||!matches(post,2))return false;return configure(context_,room,post);}
bool ModalLiveTail::report(ModalTailReport& out) const noexcept {const std::array<const ModalLiveTail*,1> nodes{this};return composeModalLiveTail(nodes,1,out);}
bool composeModalLiveTail(std::span<const ModalLiveTail* const> nodes,unsigned sources,ModalTailReport& out) noexcept {
    if(nodes.empty()||nodes.size()>8||sources<1||sources>8||!nodes[0]||!nodes[0]->mask_)return false;
    const auto& c=nodes[0]->context_;ModalTailReport r;r.mask=(1U<<12)-1;r.sourceMultiplicity=sources;r.frames=UINT64_MAX;
    for(const auto* node:nodes){if(!node||!node->mask_||node->context_.rate_!=c.rate_||node->context_.modes_!=c.modes_)return false;r.mask&=node->mask_;r.peakExponent=std::max(r.peakExponent,node->peakExponent_);if(node->context_.support_>UINT64_MAX-r.support)return false;r.support+=node->context_.support_;}
    if(r.support>UINT64_MAX-2)return false;
    for(unsigned b=0;b<12;++b)if(r.mask&(1U<<b)){double log=double(r.peakExponent)*c.ln2_+c.sourceLogs_[sources-1]+1e-10;for(const auto* node:nodes)log+=node->room_[b]+node->post_[b]+c.ln2_+node->context_.transportLogGain_;
        const double decay=std::ceil(std::max(0.,(log+c.negativeLogFloor_)*c.inverse_[b]));if(!std::isfinite(decay)||decay>=double(UINT64_MAX-r.support-2))return false;
        r.logGains[b]=log;r.circleFrames[b]=r.support+std::uint64_t(decay)+2;if(r.circleFrames[b]<r.frames){r.frames=r.circleFrames[b];r.selectedBin=b;}}
    if(r.frames==UINT64_MAX)return false;r.passed=true;r.seconds=double(r.frames)/c.rate_;out=r;return true;
}
}

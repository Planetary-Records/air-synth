#include "ModalConverterReference.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
namespace airack::space::probe::converter_reference {
namespace {
using Frame=ModalWetConverter::Frame;
bool finite(std::span<const Frame> x) noexcept {for(auto f:x)if(!std::isfinite(f[0])||!std::isfinite(f[1]))return false;return true;}
bool overlap(std::span<const Frame> a,std::span<Frame> b) noexcept {if(a.empty()||b.empty())return false;const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());return x<y+b.size_bytes()&&y<x+a.size_bytes();}
void push(std::array<Frame,127>& history,std::size_t& cursor,Frame x) noexcept {history[cursor]=x;if(++cursor==127)cursor=0;}
const Frame& age(const std::array<Frame,127>& history,std::size_t cursor,std::size_t n) noexcept {auto index=std::ptrdiff_t(cursor)-1-std::ptrdiff_t(n);if(index<0)index+=127;return history[std::size_t(index)];}
Frame filter(const PreparedModalConverter& p,const std::array<Frame,127>& history,std::size_t cursor) noexcept {const auto middle=age(history,cursor,63);Frame y{.5*middle[0],.5*middle[1]};for(std::size_t k=0;k<63;k+=2){const auto a=age(history,cursor,k),b=age(history,cursor,126-k);const auto h=p.tap(k);for(unsigned c=0;c<2;++c)y[c]+=h*(a[c]+b[c]);}return y;}
}
bool prepareModalConverter(unsigned factor,PreparedModalConverter& out) noexcept {
    if(factor!=1&&factor!=2)return false;PreparedModalConverter p;p.factor_=factor;
    if(factor==1){p.taps_[0]=p.absoluteSum_=p.gain_=1;out=p;return true;}
    long double off=0;for(unsigned k=0;k<63;k+=2){const unsigned distance=63-k;const double sign=(distance/2)%2?-1.:1.;const double a=2*std::numbers::pi*double(k)/126;
        const double window=.35875-.48829*std::cos(a)+.14128*std::cos(2*a)-.01168*std::cos(3*a);p.taps_[k]=sign*window/(std::numbers::pi*distance);off+=2*static_cast<long double>(p.taps_[k]);}
    const double scale=.5/double(off);for(unsigned k=0;k<63;k+=2){p.taps_[k]*=scale;p.taps_[126-k]=p.taps_[k];}p.taps_[63]=.5;
    long double absolute=0;for(double h:p.taps_)absolute+=std::abs(h);p.absoluteSum_=std::nextafter(double(absolute),std::numeric_limits<double>::infinity())*(1+1e-12);p.gain_=2*p.absoluteSum_*p.absoluteSum_*(1+1e-12);p.logGain_=std::log(p.gain_)+1e-12;out=p;return true;
}
bool ModalWetConverter::configure(const PreparedModalConverter& p) noexcept {if(open_||(p.factor_!=1&&p.factor_!=2))return false;prepared_=p;reset();return true;}
void ModalWetConverter::reset() noexcept {down_={};up_={};downCursor_=upCursor_=physicalCount_=logicalCount_=0;even_=startEven_=true;open_=false;}
bool ModalWetConverter::down(std::span<const Frame> physical,std::span<Frame> logical,std::size_t& produced) noexcept {
    if(!prepared_.factor_||open_||physical.size()>512||!finite(physical))return false;const auto count=prepared_.factor_==1?physical.size():(physical.size()+(even_?1:0))/2;
    if(logical.size()<count||(overlap(physical,logical.first(count))&&!(prepared_.factor_==1&&physical.data()==logical.data())))return false;
    startEven_=even_;physicalCount_=physical.size();logicalCount_=count;
    if(prepared_.factor_==1)std::copy(physical.begin(),physical.end(),logical.begin());else {std::size_t at=0;for(auto frame:physical){push(down_,downCursor_,frame);if(even_)logical[at++]=filter(prepared_,down_,downCursor_);even_=!even_;}}
    produced=count;open_=true;return true;
}
bool ModalWetConverter::up(std::span<const Frame> logical,std::span<Frame> physical) noexcept {
    if(!open_||logical.size()!=logicalCount_||physical.size()!=physicalCount_||!finite(logical)||(overlap(logical,physical)&&!(prepared_.factor_==1&&logical.data()==physical.data())))return false;
    if(prepared_.factor_==1)std::copy(logical.begin(),logical.end(),physical.begin());else {bool even=startEven_;std::size_t at=0;for(auto& frame:physical){Frame excitation{0,0};if(even){excitation=logical[at++];for(auto& v:excitation)v*=2;}push(up_,upCursor_,excitation);frame=filter(prepared_,up_,upCursor_);even=!even;}}
    open_=false;return true;
}
}

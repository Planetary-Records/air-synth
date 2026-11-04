#include "ModalConverter.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#if defined(_MSC_VER) && defined(_M_X64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace airack::space::runtime {
namespace {
using Frame=ModalWetConverter::Frame;
// Cache before callbacks: configure is also used by cold prepared installation.
#if defined(_MSC_VER) && defined(_M_X64)
const bool converterAvx2Available=IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)!=FALSE;
#else
constexpr bool converterAvx2Available=false;
#endif
bool finite(std::span<const Frame> x) noexcept {for(auto f:x)if(!std::isfinite(f[0])||!std::isfinite(f[1]))return false;return true;}
bool overlap(std::span<const Frame> a,std::span<Frame> b) noexcept {if(a.empty()||b.empty())return false;const auto x=reinterpret_cast<std::uintptr_t>(a.data()),y=reinterpret_cast<std::uintptr_t>(b.data());return x<y+b.size_bytes()&&y<x+a.size_bytes();}
void push(std::span<Frame,127> history,std::size_t& cursor,Frame x) noexcept {history[cursor]=x;if(++cursor==127)cursor=0;}
const Frame& age(std::span<const Frame,127> history,std::size_t cursor,std::size_t n) noexcept {auto index=std::ptrdiff_t(cursor)-1-std::ptrdiff_t(n);if(index<0)index+=127;return history[std::size_t(index)];}
Frame filter(const PreparedModalConverter& p,std::span<const Frame,127> history,std::size_t cursor) noexcept {const auto middle=age(history,cursor,63);Frame y{.5*middle[0],.5*middle[1]};for(std::size_t k=0;k<63;k+=2){const auto a=age(history,cursor,k),b=age(history,cursor,126-k);const auto h=p.tap(k);for(unsigned c=0;c<2;++c)y[c]+=h*(a[c]+b[c]);}return y;}
Frame referenceKernel(const PreparedModalConverter& p,const Frame* history,std::size_t cursor) noexcept {return filter(p,std::span<const Frame,127>(history,127),cursor);}
void pushMirrored(Frame* history,std::size_t length,std::size_t& cursor,Frame x) noexcept {history[cursor]=history[cursor+length]=x;if(++cursor==length)cursor=0;}
Frame downScalar(const PreparedModalConverter& p,const Frame* history,std::size_t cursor) noexcept {
    const auto middle=history[cursor+63];Frame y{.5*middle[0],.5*middle[1]};
    for(std::size_t k=0;k<63;k+=2){const auto a=history[cursor+126-k],b=history[cursor+k];const auto h=p.tap(k);for(unsigned c=0;c<2;++c)y[c]+=h*(a[c]+b[c]);}return y;
}
Frame upEvenScalar(const PreparedModalConverter& p,const Frame* history,std::size_t cursor) noexcept {
    Frame y{0,0};for(std::size_t j=0;j<32;++j){const auto a=history[cursor+63-j],b=history[cursor+j];const auto h=p.tap(2*j);for(unsigned c=0;c<2;++c)y[c]+=h*(a[c]+b[c]);}return y;
}
Frame upOddScalar(const PreparedModalConverter& p,const Frame* history,std::size_t cursor) noexcept {
    const auto middle=history[cursor+32];Frame y{.5*middle[0],.5*middle[1]};
    // Preserve the original first negative-zero and positive-zero additions.
    // The remaining alternating zero additions are idempotent in each IEEE mode.
    const double first=p.tap(0)*0.,second=p.tap(2)*0.;
    for(unsigned c=0;c<2;++c){y[c]+=first;y[c]+=second;}return y;
}
#if defined(_M_X64) || defined(__SSE2__)
__m128d pair(double h,const Frame& a,const Frame& b) noexcept {return _mm_mul_pd(_mm_set1_pd(h),_mm_add_pd(_mm_loadu_pd(a.data()),_mm_loadu_pd(b.data())));}
Frame stored(__m128d value) noexcept {Frame out;_mm_storeu_pd(out.data(),value);return out;}
Frame downSse2(const PreparedModalConverter& p,const Frame* history,std::size_t cursor) noexcept {
    auto y=_mm_mul_pd(_mm_set1_pd(.5),_mm_loadu_pd(history[cursor+63].data()));
    for(std::size_t k=0;k<63;k+=8){const auto front=cursor+126-k,back=cursor+k;
        const auto a=pair(p.tap(k),history[front],history[back]);
        const auto b=pair(p.tap(k+2),history[front-2],history[back+2]);
        const auto c=pair(p.tap(k+4),history[front-4],history[back+4]);
        const auto d=pair(p.tap(k+6),history[front-6],history[back+6]);
        y=_mm_add_pd(y,a);y=_mm_add_pd(y,b);y=_mm_add_pd(y,c);y=_mm_add_pd(y,d);
    }return stored(y);
}
Frame upEvenSse2(const PreparedModalConverter& p,const Frame* history,std::size_t cursor) noexcept {
    auto y=_mm_setzero_pd();for(std::size_t j=0;j<32;j+=4){const auto front=cursor+63-j,back=cursor+j;
        const auto a=pair(p.tap(2*j),history[front],history[back]);
        const auto b=pair(p.tap(2*(j+1)),history[front-1],history[back+1]);
        const auto c=pair(p.tap(2*(j+2)),history[front-2],history[back+2]);
        const auto d=pair(p.tap(2*(j+3)),history[front-3],history[back+3]);
        y=_mm_add_pd(y,a);y=_mm_add_pd(y,b);y=_mm_add_pd(y,c);y=_mm_add_pd(y,d);
    }return stored(y);
}
Frame upOddSse2(const PreparedModalConverter& p,const Frame* history,std::size_t cursor) noexcept {
    auto y=_mm_mul_pd(_mm_set1_pd(.5),_mm_loadu_pd(history[cursor+32].data()));
    y=_mm_add_pd(y,_mm_mul_pd(_mm_set1_pd(p.tap(0)),_mm_setzero_pd()));
    y=_mm_add_pd(y,_mm_mul_pd(_mm_set1_pd(p.tap(2)),_mm_setzero_pd()));return stored(y);
}
#endif
}
bool modalConverterAvx2Available() noexcept {return converterAvx2Available;}
bool prepareModalConverter(unsigned factor,PreparedModalConverter& out) noexcept {
    if(factor!=1&&factor!=2)return false;PreparedModalConverter p;p.factor_=factor;
    if(factor==1){p.taps_[0]=p.absoluteSum_=p.gain_=1;out=p;return true;}
    long double off=0;for(unsigned k=0;k<63;k+=2){const unsigned distance=63-k;const double sign=(distance/2)%2?-1.:1.;const double a=2*std::numbers::pi*double(k)/126;
        const double window=.35875-.48829*std::cos(a)+.14128*std::cos(2*a)-.01168*std::cos(3*a);p.taps_[k]=sign*window/(std::numbers::pi*distance);off+=2*static_cast<long double>(p.taps_[k]);}
    const double scale=.5/double(off);for(unsigned k=0;k<63;k+=2){p.taps_[k]*=scale;p.taps_[126-k]=p.taps_[k];}p.taps_[63]=.5;
    long double absolute=0;for(double h:p.taps_)absolute+=std::abs(h);p.absoluteSum_=std::nextafter(double(absolute),std::numeric_limits<double>::infinity())*(1+1e-12);p.gain_=2*p.absoluteSum_*p.absoluteSum_*(1+1e-12);p.logGain_=std::log(p.gain_)+1e-12;out=p;return true;
}
bool ModalWetConverter::configure(const PreparedModalConverter& p,ModalConverterBackend backend) noexcept {
    if(open_||(p.factor_!=1&&p.factor_!=2))return false;
    if(backend==ModalConverterBackend::automatic){
#if defined(_M_X64) || defined(__SSE2__)
        backend=ModalConverterBackend::mirroredSse2;
#else
        backend=ModalConverterBackend::mirroredScalar;
#endif
    }
    Kernel down=nullptr,upEven=nullptr,upOdd=nullptr;DownBlock downBlock=nullptr;UpBlock upBlock=nullptr;
    if(backend==ModalConverterBackend::reference)down=upEven=upOdd=&referenceKernel;
    else if(backend==ModalConverterBackend::mirroredScalar){down=&downScalar;upEven=&upEvenScalar;upOdd=&upOddScalar;}
#if defined(_M_X64) || defined(__SSE2__)
    else if(backend==ModalConverterBackend::mirroredSse2){down=&downSse2;upEven=&upEvenSse2;upOdd=&upOddSse2;}
#endif
#if defined(AIRACK_MODAL_CONVERTER_AVX2)
    else if(backend==ModalConverterBackend::mirroredAvx2&&modalConverterAvx2Available()){downBlock=&ModalConverterAvx2::down;upBlock=&ModalConverterAvx2::up;}
#endif
    else return false;
    prepared_=p;downKernel_=down;upEvenKernel_=upEven;upOddKernel_=upOdd;downBlock_=downBlock;upBlock_=upBlock;mirrored_=backend!=ModalConverterBackend::reference;reset();return true;
}
ModalConverterBackend ModalWetConverter::selectedBackend() const noexcept {
#if defined(AIRACK_MODAL_CONVERTER_AVX2)
    if(downBlock_==&ModalConverterAvx2::down&&upBlock_==&ModalConverterAvx2::up)return ModalConverterBackend::mirroredAvx2;
#endif
    if(downKernel_==&referenceKernel&&upEvenKernel_==&referenceKernel&&upOddKernel_==&referenceKernel)return ModalConverterBackend::reference;
    if(downKernel_==&downScalar&&upEvenKernel_==&upEvenScalar&&upOddKernel_==&upOddScalar)return ModalConverterBackend::mirroredScalar;
#if defined(_M_X64) || defined(__SSE2__)
    if(downKernel_==&downSse2&&upEvenKernel_==&upEvenSse2&&upOddKernel_==&upOddSse2)return ModalConverterBackend::mirroredSse2;
#endif
    return ModalConverterBackend::automatic;
}
void ModalWetConverter::reset() noexcept {down_={};up_={};downCursor_=upCursor_=physicalCount_=logicalCount_=0;even_=startEven_=true;open_=false;}
bool ModalWetConverter::down(std::span<const Frame> physical,std::span<Frame> logical,std::size_t& produced) noexcept {
    if(!prepared_.factor_||open_||physical.size()>512||!finite(physical))return false;const auto count=prepared_.factor_==1?physical.size():(physical.size()+(even_?1:0))/2;
    if(logical.size()<count||(overlap(physical,logical.first(count))&&!(prepared_.factor_==1&&physical.data()==logical.data())))return false;
    startEven_=even_;physicalCount_=physical.size();logicalCount_=count;
    if(prepared_.factor_==1)std::copy(physical.begin(),physical.end(),logical.begin());else if(downBlock_)downBlock_(*this,physical,logical.first(count));else {std::size_t at=0;for(auto frame:physical){if(mirrored_)pushMirrored(down_.data(),127,downCursor_,frame);else push(std::span<Frame,127>(down_.data(),127),downCursor_,frame);if(even_)logical[at++]=downKernel_(prepared_,down_.data(),downCursor_);even_=!even_;}}
    produced=count;open_=true;return true;
}
bool ModalWetConverter::up(std::span<const Frame> logical,std::span<Frame> physical) noexcept {
    if(!open_||logical.size()!=logicalCount_||physical.size()!=physicalCount_||!finite(logical)||(overlap(logical,physical)&&!(prepared_.factor_==1&&logical.data()==physical.data())))return false;
    if(prepared_.factor_==1)std::copy(logical.begin(),logical.end(),physical.begin());else if(upBlock_)upBlock_(*this,logical,physical);else {bool even=startEven_;std::size_t at=0;for(auto& frame:physical){Frame excitation{0,0};if(even){excitation=logical[at++];for(auto& v:excitation)v*=2;}
        if(mirrored_){if(even)pushMirrored(up_.data(),64,upCursor_,excitation);frame=(even?upEvenKernel_:upOddKernel_)(prepared_,up_.data(),upCursor_);}
        else {push(std::span<Frame,127>(up_.data(),127),upCursor_,excitation);frame=upEvenKernel_(prepared_,up_.data(),upCursor_);}even=!even;}}
    open_=false;return true;
}
}

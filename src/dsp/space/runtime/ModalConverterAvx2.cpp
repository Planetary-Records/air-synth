#include "ModalConverter.h"
#include <immintrin.h>
namespace airack::space::runtime {
namespace {
using Frame=ModalWetConverter::Frame;
void push(Frame* history,std::size_t length,std::size_t& cursor,Frame frame) noexcept {
    history[cursor]=history[cursor+length]=frame;if(++cursor==length)cursor=0;
}
__m256d two(const Frame& low,const Frame& high) noexcept {
    return _mm256_insertf128_pd(_mm256_castpd128_pd256(_mm_loadu_pd(low.data())),_mm_loadu_pd(high.data()),1);
}
void store(std::array<Frame,4>& out,__m256d low,__m256d high) noexcept {
    _mm_storeu_pd(out[0].data(),_mm256_castpd256_pd128(low));_mm_storeu_pd(out[1].data(),_mm256_extractf128_pd(low,1));
    _mm_storeu_pd(out[2].data(),_mm256_castpd256_pd128(high));_mm_storeu_pd(out[3].data(),_mm256_extractf128_pd(high,1));
}
template<bool down>
void four(const PreparedModalConverter& p,const Frame* h,std::size_t cursor,std::array<Frame,4>& out) noexcept {
    constexpr std::array<std::size_t,4> delay=down?std::array<std::size_t,4>{6,4,2,0}:std::array<std::size_t,4>{3,2,1,0};
    auto a=_mm256_setzero_pd(),b=a;
    if constexpr(down){const auto center=cursor+69;a=_mm256_mul_pd(_mm256_set1_pd(.5),two(h[center-delay[0]],h[center-delay[1]]));b=_mm256_mul_pd(_mm256_set1_pd(.5),two(h[center-delay[2]],h[center-delay[3]]));}
    for(std::size_t j=0;j<32;++j){
        const auto front=cursor+(down?132-2*j:66-j),back=cursor+(down?6+2*j:3+j);const auto tap=_mm256_set1_pd(p.tap(2*j));
        const auto first=_mm256_mul_pd(tap,_mm256_add_pd(two(h[front-delay[0]],h[front-delay[1]]),two(h[back-delay[0]],h[back-delay[1]])));
        const auto second=_mm256_mul_pd(tap,_mm256_add_pd(two(h[front-delay[2]],h[front-delay[3]]),two(h[back-delay[2]],h[back-delay[3]])));
        a=_mm256_add_pd(a,first);b=_mm256_add_pd(b,second);
    }
    store(out,a,b);
}
Frame single(const PreparedModalConverter& p,const Frame* h,std::size_t cursor,bool down) noexcept {
    auto y=down?_mm_mul_pd(_mm_set1_pd(.5),_mm_loadu_pd(h[cursor+69].data())):_mm_setzero_pd();
    for(std::size_t j=0;j<32;++j){const auto front=cursor+(down?132-2*j:66-j),back=cursor+(down?6+2*j:3+j);
        y=_mm_add_pd(y,_mm_mul_pd(_mm_set1_pd(p.tap(2*j)),_mm_add_pd(_mm_loadu_pd(h[front].data()),_mm_loadu_pd(h[back].data()))));}
    Frame out;_mm_storeu_pd(out.data(),y);return out;
}
Frame odd(const PreparedModalConverter& p,const Frame* h,std::size_t middle) noexcept {
    auto y=_mm_mul_pd(_mm_set1_pd(.5),_mm_loadu_pd(h[middle].data()));
    y=_mm_add_pd(y,_mm_mul_pd(_mm_set1_pd(p.tap(0)),_mm_setzero_pd()));
    y=_mm_add_pd(y,_mm_mul_pd(_mm_set1_pd(p.tap(2)),_mm_setzero_pd()));
    Frame out;_mm_storeu_pd(out.data(),y);return out;
}
Frame excitation(Frame frame) noexcept {for(auto& value:frame)value*=2;return frame;}
}
void ModalConverterAvx2::down(ModalWetConverter& s,std::span<const Frame> input,std::span<Frame> output) noexcept {
    std::size_t at=0,made=0;
    while(at<input.size()){
        if(!s.even_){push(s.down_.data(),133,s.downCursor_,input[at++]);s.even_=true;continue;}
        if(input.size()-at>=7){
            for(unsigned n=0;n<7;++n)push(s.down_.data(),133,s.downCursor_,input[at++]);
            std::array<Frame,4> filtered;four<true>(s.prepared_,s.down_.data(),s.downCursor_,filtered);
            for(auto frame:filtered)output[made++]=frame;
        }else {push(s.down_.data(),133,s.downCursor_,input[at++]);output[made++]=single(s.prepared_,s.down_.data(),s.downCursor_,true);}
        s.even_=false;
    }
    _mm256_zeroupper();
}
void ModalConverterAvx2::up(ModalWetConverter& s,std::span<const Frame> input,std::span<Frame> output) noexcept {
    std::size_t at=0,used=0;bool even=s.startEven_;
    while(at<output.size()){
        if(!even){output[at++]=odd(s.prepared_,s.up_.data(),s.upCursor_+35);even=true;continue;}
        if(output.size()-at>=7){
            for(unsigned n=0;n<4;++n)push(s.up_.data(),67,s.upCursor_,excitation(input[used++]));
            std::array<Frame,4> filtered;four<false>(s.prepared_,s.up_.data(),s.upCursor_,filtered);
            const unsigned count=output.size()-at>=8?8:7;
            for(unsigned n=0;n<4;++n){output[at+2*n]=filtered[n];if(2*n+1<count)output[at+2*n+1]=odd(s.prepared_,s.up_.data(),s.upCursor_+32+n);}
            at+=count;even=count==8;
        }else {push(s.up_.data(),67,s.upCursor_,excitation(input[used++]));output[at++]=single(s.prepared_,s.up_.data(),s.upCursor_,false);even=false;}
    }
    _mm256_zeroupper();
}
}

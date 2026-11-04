#include "ModalKernel.h"
#include <bit>
#include <cstdint>
#if defined(AIRACK_MODAL_AVX2)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace airack::space::runtime {
#if defined(AIRACK_MODAL_AVX2)
void modalAvx2(const PreparedModal&,double*,double*,std::span<const ModalFrame>,std::span<ModalFrame>) noexcept;
void modalAvx2UnitAxis(const PreparedModal&,double*,double*,std::span<const ModalFrame>,std::span<ModalFrame>) noexcept;
void modalAvx2UnitAxisPairedOutput(const PreparedModal&,double*,double*,std::span<const ModalFrame>,std::span<ModalFrame>) noexcept;
#endif
bool modalSse2Available() noexcept {
#if defined(_M_X64) || defined(__SSE2__)
    return true;
#else
    return false;
#endif
}
ModalKernel availableModalAvx2Kernel() noexcept {
#if defined(AIRACK_MODAL_AVX2)
    if(IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE))return &modalAvx2;
#endif
    return nullptr;
}
ModalKernel modalKernelFor(ModalBackend backend,bool sse,ModalKernel avx) noexcept {
    switch(backend){case ModalBackend::scalar:return &modalScalar;case ModalBackend::sse2:return sse?&modalSse2:nullptr;
        case ModalBackend::automatic:return avx?avx:sse?&modalSse2:&modalScalar;case ModalBackend::avx2:return avx;}
    return nullptr;
}
bool modalKernelUsesPairedOutput(ModalKernel kernel) noexcept {
#if defined(AIRACK_MODAL_AVX2)
    return kernel==&modalAvx2UnitAxisPairedOutput;
#else
    (void)kernel;return false;
#endif
}
ModalKernel specializeModalKernel(ModalKernel base,const PreparedModal& p,bool pairedOutput) noexcept {
    (void)pairedOutput;
#if defined(AIRACK_MODAL_AVX2)
    if(base!=&modalAvx2)return base;
    constexpr double rootHalf=.7071067811865475244;
    constexpr std::array<std::array<double,2>,4> weights{{{1,0},{0,1},{rootHalf,rootHalf},{rootHalf,-rootHalf}}};
    for(std::size_t g=0;g<p.count/4;++g){
        for(unsigned c=0;c<2;++c)if(std::bit_cast<std::uint64_t>(p.inputWeights[g][c])!=std::bit_cast<std::uint64_t>(weights[g%4][c]))return base;
        const bool imaginary=((g/4)+(g%4)/2)%2!=0;
        for(std::size_t j=0;j<4;++j){const auto k=g*4+j;const double unit=imaginary?p.bImag[k]:p.bReal[k],zero=imaginary?p.bReal[k]:p.bImag[k];
            if((unit!=1&&unit!=-1)||std::bit_cast<std::uint64_t>(zero)!=0)return base;}
    }
    return pairedOutput?&modalAvx2UnitAxisPairedOutput:&modalAvx2UnitAxis;
#else
    (void)p;return base;
#endif
}
void modalScalar(const PreparedModal& p,double* real,double* imag,std::span<const ModalFrame> input,std::span<ModalFrame> accum) noexcept {
    const auto count=input.size();
    for(std::size_t g=0;g<p.count/4;++g){
        const auto k=g*4;double a[4],b[4];for(std::size_t j=0;j<4;++j){a[j]=real[k+j];b[j]=imag[k+j];}
        const auto weights=p.inputWeights[g];
        for(std::size_t n=0;n<count;++n){
            const double x=input[n][0]*weights[0]+input[n][1]*weights[1];
            for(std::size_t j=0;j<4;++j){const auto index=k+j;const double oldReal=a[j],oldImag=b[j];
                a[j]=(p.rc[index]*oldReal-p.rs[index]*oldImag)+p.bReal[index]*x;
                b[j]=(p.rs[index]*oldReal+p.rc[index]*oldImag)+p.bImag[index]*x;
            }
            accum[n][g%2]+=(a[0]+a[1])+(a[2]+a[3]);
        }
        for(std::size_t j=0;j<4;++j){real[k+j]=a[j];imag[k+j]=b[j];}
    }
}
void modalSse2(const PreparedModal& p,double* real,double* imag,std::span<const ModalFrame> input,std::span<ModalFrame> accum) noexcept {
#if defined(_M_X64) || defined(__SSE2__)
    for(std::size_t group=0;group<p.count/4;++group){
        const auto k=group*4;
        auto a0=_mm_loadu_pd(real+k),a1=_mm_loadu_pd(real+k+2),b0=_mm_loadu_pd(imag+k),b1=_mm_loadu_pd(imag+k+2);
        const auto rc0=_mm_loadu_pd(p.rc.data()+k),rc1=_mm_loadu_pd(p.rc.data()+k+2),rs0=_mm_loadu_pd(p.rs.data()+k),rs1=_mm_loadu_pd(p.rs.data()+k+2);
        const auto br0=_mm_loadu_pd(p.bReal.data()+k),br1=_mm_loadu_pd(p.bReal.data()+k+2),bi0=_mm_loadu_pd(p.bImag.data()+k),bi1=_mm_loadu_pd(p.bImag.data()+k+2);
        const auto weights=p.inputWeights[group];
        for(std::size_t n=0;n<input.size();++n){
            const auto x=_mm_set1_pd(input[n][0]*weights[0]+input[n][1]*weights[1]);
            const auto nextA0=_mm_add_pd(_mm_sub_pd(_mm_mul_pd(rc0,a0),_mm_mul_pd(rs0,b0)),_mm_mul_pd(br0,x));
            const auto nextB0=_mm_add_pd(_mm_add_pd(_mm_mul_pd(rs0,a0),_mm_mul_pd(rc0,b0)),_mm_mul_pd(bi0,x));
            const auto nextA1=_mm_add_pd(_mm_sub_pd(_mm_mul_pd(rc1,a1),_mm_mul_pd(rs1,b1)),_mm_mul_pd(br1,x));
            const auto nextB1=_mm_add_pd(_mm_add_pd(_mm_mul_pd(rs1,a1),_mm_mul_pd(rc1,b1)),_mm_mul_pd(bi1,x));
            a0=nextA0;b0=nextB0;a1=nextA1;b1=nextB1;
            const auto low=_mm_add_sd(a0,_mm_shuffle_pd(a0,a0,1)),high=_mm_add_sd(a1,_mm_shuffle_pd(a1,a1,1));
            accum[n][group%2]+=_mm_cvtsd_f64(_mm_add_sd(low,high));
        }
        _mm_storeu_pd(real+k,a0);_mm_storeu_pd(real+k+2,a1);_mm_storeu_pd(imag+k,b0);_mm_storeu_pd(imag+k+2,b1);
    }
#else
    modalScalar(p,real,imag,input,accum);
#endif
}
}

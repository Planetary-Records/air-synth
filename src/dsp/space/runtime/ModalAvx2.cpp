#include "ModalKernel.h"
#include <immintrin.h>
#include <atomic>
namespace airack::space::runtime {
namespace {
void generic(const PreparedModal& p,double* real,double* imag,std::span<const ModalFrame> input,std::span<ModalFrame> accum) noexcept {
    for(std::size_t group=0;group<p.count/4;group+=2){
        const auto k=group*4;
        auto a0=_mm256_loadu_pd(real+k),a1=_mm256_loadu_pd(real+k+4),b0=_mm256_loadu_pd(imag+k),b1=_mm256_loadu_pd(imag+k+4);
        const auto rc0=_mm256_loadu_pd(p.rc.data()+k),rc1=_mm256_loadu_pd(p.rc.data()+k+4),rs0=_mm256_loadu_pd(p.rs.data()+k),rs1=_mm256_loadu_pd(p.rs.data()+k+4);
        const auto br0=_mm256_loadu_pd(p.bReal.data()+k),br1=_mm256_loadu_pd(p.bReal.data()+k+4),bi0=_mm256_loadu_pd(p.bImag.data()+k),bi1=_mm256_loadu_pd(p.bImag.data()+k+4);
        const auto w0=p.inputWeights[group],w1=p.inputWeights[group+1];
        for(std::size_t n=0;n<input.size();++n){
            const auto x0=_mm256_set1_pd(input[n][0]*w0[0]+input[n][1]*w0[1]);
            const auto nextA0=_mm256_add_pd(_mm256_sub_pd(_mm256_mul_pd(rc0,a0),_mm256_mul_pd(rs0,b0)),_mm256_mul_pd(br0,x0));
            const auto nextB0=_mm256_add_pd(_mm256_add_pd(_mm256_mul_pd(rs0,a0),_mm256_mul_pd(rc0,b0)),_mm256_mul_pd(bi0,x0));
            a0=nextA0;b0=nextB0;
            const auto x1=_mm256_set1_pd(input[n][0]*w1[0]+input[n][1]*w1[1]);
            const auto nextA1=_mm256_add_pd(_mm256_sub_pd(_mm256_mul_pd(rc1,a1),_mm256_mul_pd(rs1,b1)),_mm256_mul_pd(br1,x1));
            const auto nextB1=_mm256_add_pd(_mm256_add_pd(_mm256_mul_pd(rs1,a1),_mm256_mul_pd(rc1,b1)),_mm256_mul_pd(bi1,x1));
            a1=nextA1;b1=nextB1;
            const auto sum0=_mm256_hadd_pd(a0,a0),sum1=_mm256_hadd_pd(a1,a1);
            accum[n][0]+=_mm_cvtsd_f64(_mm_add_sd(_mm256_castpd256_pd128(sum0),_mm256_extractf128_pd(sum0,1)));
            accum[n][1]+=_mm_cvtsd_f64(_mm_add_sd(_mm256_castpd256_pd128(sum1),_mm256_extractf128_pd(sum1,1)));
        }
        _mm256_storeu_pd(real+k,a0);_mm256_storeu_pd(real+k+4,a1);_mm256_storeu_pd(imag+k,b0);_mm256_storeu_pd(imag+k+4,b1);
    }
    _mm256_zeroupper();
}
constexpr double rootHalf=.7071067811865475244;
constexpr std::array<std::array<double,2>,4> weights{{{1,0},{0,1},{rootHalf,rootHalf},{rootHalf,-rootHalf}}};
using Routes=std::array<std::array<double,4>,ModalTank::maximumBlockFrames>;
template<bool imaginary> __forceinline void stepCached(__m256d& a,__m256d& b,const __m256d rc,const __m256d rs,const __m256d sign,const double* input,double& sum) noexcept {
    const auto nextA=_mm256_sub_pd(_mm256_mul_pd(rc,a),_mm256_mul_pd(rs,b));
    const auto nextB=_mm256_add_pd(_mm256_mul_pd(rs,a),_mm256_mul_pd(rc,b));
    a=nextA;b=nextB;
    // Keep the old rotation temporaries dead before broadcasting excitation.
    std::atomic_signal_fence(std::memory_order_acq_rel);
    const auto mask=_mm256_set1_pd(-0.),x=_mm256_broadcast_sd(input);
    const auto signedInput=_mm256_xor_pd(x,sign),zero=_mm256_and_pd(x,mask);
    a=_mm256_add_pd(a,imaginary?zero:signedInput);
    b=_mm256_add_pd(b,imaginary?signedInput:zero);
    const auto horizontal=_mm256_hadd_pd(a,a);
    sum+=_mm_cvtsd_f64(_mm_add_sd(_mm256_castpd256_pd128(horizontal),_mm256_extractf128_pd(horizontal,1)));
}
template<bool imaginary> __forceinline void pair(const PreparedModal& p,double* real,double* imag,const Routes& routes,std::span<ModalFrame> accum,std::size_t group) noexcept {
    const auto k=group*4;
    auto a0=_mm256_loadu_pd(real+k),a1=_mm256_loadu_pd(real+k+4);
    auto b0=_mm256_loadu_pd(imag+k),b1=_mm256_loadu_pd(imag+k+4);
    const auto rc0=_mm256_loadu_pd(p.rc.data()+k),rc1=_mm256_loadu_pd(p.rc.data()+k+4);
    const auto rs0=_mm256_loadu_pd(p.rs.data()+k),rs1=_mm256_loadu_pd(p.rs.data()+k+4);
    const auto* unit=imaginary?p.bImag.data():p.bReal.data();
    const auto mask=_mm256_set1_pd(-0.);
    const auto sign0=_mm256_and_pd(_mm256_loadu_pd(unit+k),mask),sign1=_mm256_and_pd(_mm256_loadu_pd(unit+k+4),mask);
    const auto route=group%4;
    for(std::size_t n=0;n<accum.size();++n){
        stepCached<imaginary>(a0,b0,rc0,rs0,sign0,&routes[n][route],accum[n][0]);
        stepCached<imaginary>(a1,b1,rc1,rs1,sign1,&routes[n][route+1],accum[n][1]);
    }
    _mm256_storeu_pd(real+k,a0);_mm256_storeu_pd(real+k+4,a1);
    _mm256_storeu_pd(imag+k,b0);_mm256_storeu_pd(imag+k+4,b1);
}

template<bool imaginary> __forceinline void stepPaired(__m256d& a,__m256d& b,const __m256d rc,const __m256d rs,const __m256d sign,const double* input) noexcept {
    const auto nextA=_mm256_sub_pd(_mm256_mul_pd(rc,a),_mm256_mul_pd(rs,b));
    const auto nextB=_mm256_add_pd(_mm256_mul_pd(rs,a),_mm256_mul_pd(rc,b));
    a=nextA;b=nextB;
    // Keep the old rotation temporaries dead before broadcasting excitation.
    std::atomic_signal_fence(std::memory_order_acq_rel);
    const auto mask=_mm256_set1_pd(-0.),x=_mm256_broadcast_sd(input);
    const auto signedInput=_mm256_xor_pd(x,sign),zero=_mm256_and_pd(x,mask);
    a=_mm256_add_pd(a,imaginary?zero:signedInput);
    b=_mm256_add_pd(b,imaginary?signedInput:zero);
}

template<bool imaginary> __forceinline void pairPaired(const PreparedModal& p,double* real,double* imag,const Routes& routes,std::span<ModalFrame> accum,std::size_t group) noexcept {
    const auto k=group*4;
    auto a0=_mm256_loadu_pd(real+k),a1=_mm256_loadu_pd(real+k+4);
    auto b0=_mm256_loadu_pd(imag+k),b1=_mm256_loadu_pd(imag+k+4);
    const auto rc0=_mm256_loadu_pd(p.rc.data()+k),rc1=_mm256_loadu_pd(p.rc.data()+k+4);
    const auto rs0=_mm256_loadu_pd(p.rs.data()+k),rs1=_mm256_loadu_pd(p.rs.data()+k+4);
    const auto* unit=imaginary?p.bImag.data():p.bReal.data();
    const auto mask=_mm256_set1_pd(-0.);
    const auto sign0=_mm256_and_pd(_mm256_loadu_pd(unit+k),mask),sign1=_mm256_and_pd(_mm256_loadu_pd(unit+k+4),mask);
    const auto route=group%4;
    for(std::size_t n=0;n<accum.size();++n){
        stepPaired<imaginary>(a0,b0,rc0,rs0,sign0,&routes[n][route]);
        stepPaired<imaginary>(a1,b1,rc1,rs1,sign1,&routes[n][route+1]);
        // hadd lanes are {Llo,Rlo,Lhi,Rhi}; preserve each channel's pairwise sum.
        const auto horizontal=_mm256_hadd_pd(a0,a1);
        const auto stereo=_mm_add_pd(_mm256_castpd256_pd128(horizontal),_mm256_extractf128_pd(horizontal,1));
        const auto previous=_mm_loadu_pd(accum[n].data());
        _mm_storeu_pd(accum[n].data(),_mm_add_pd(previous,stereo));
    }
    _mm256_storeu_pd(real+k,a0);_mm256_storeu_pd(real+k+4,a1);
    _mm256_storeu_pd(imag+k,b0);_mm256_storeu_pd(imag+k+4,b1);
}
}
void modalAvx2(const PreparedModal& p,double* real,double* imag,std::span<const ModalFrame> input,std::span<ModalFrame> accum) noexcept {
    generic(p,real,imag,input,accum);
}
void modalAvx2UnitAxis(const PreparedModal& p,double* real,double* imag,std::span<const ModalFrame> input,std::span<ModalFrame> accum) noexcept {
    Routes routes;
    for(std::size_t n=0;n<input.size();++n)for(std::size_t g=0;g<4;++g)routes[n][g]=input[n][0]*weights[g][0]+input[n][1]*weights[g][1];
    for(std::size_t group=0;group<p.count/4;group+=2){
        if(((group/4)+(group%4)/2)%2)pair<true>(p,real,imag,routes,accum,group);
        else pair<false>(p,real,imag,routes,accum,group);
    }
    _mm256_zeroupper();
}
void modalAvx2UnitAxisPairedOutput(const PreparedModal& p,double* real,double* imag,std::span<const ModalFrame> input,std::span<ModalFrame> accum) noexcept {
    Routes routes;
    for(std::size_t n=0;n<input.size();++n)for(std::size_t g=0;g<4;++g)routes[n][g]=input[n][0]*weights[g][0]+input[n][1]*weights[g][1];
    for(std::size_t group=0;group<p.count/4;group+=2){
        if(((group/4)+(group%4)/2)%2)pairPaired<true>(p,real,imag,routes,accum,group);
        else pairPaired<false>(p,real,imag,routes,accum,group);
    }
    _mm256_zeroupper();
}
}

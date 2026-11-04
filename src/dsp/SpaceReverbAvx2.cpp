#include "SpaceReverbPacked.h"
#include <immintrin.h>
namespace airack::space::detail {
template<std::size_t N> void tick(PackedStage<N>* stages,std::size_t count,double* values,std::size_t lines) noexcept
{
    for(std::size_t group=0;group<lines;group+=8){
        auto x0=_mm256_loadu_pd(values+group),x1=_mm256_loadu_pd(values+group+4);
        for(std::size_t index=0;index<count;++index){auto& s=stages[index];
            const auto tick=[&](const __m256d x,std::size_t lane){
                const auto y=_mm256_add_pd(_mm256_mul_pd(_mm256_loadu_pd(s.b0.data()+lane),x),_mm256_loadu_pd(s.z1.data()+lane));
                _mm256_storeu_pd(s.z1.data()+lane,_mm256_add_pd(_mm256_sub_pd(_mm256_mul_pd(_mm256_loadu_pd(s.b1.data()+lane),x),_mm256_mul_pd(_mm256_loadu_pd(s.a1.data()+lane),y)),_mm256_loadu_pd(s.z2.data()+lane)));
                _mm256_storeu_pd(s.z2.data()+lane,_mm256_sub_pd(_mm256_mul_pd(_mm256_loadu_pd(s.b2.data()+lane),x),_mm256_mul_pd(_mm256_loadu_pd(s.a2.data()+lane),y)));
                return y;
            };
            x0=tick(x0,group);x1=tick(x1,group+4);
        }
        _mm256_storeu_pd(values+group,x0);_mm256_storeu_pd(values+group+4,x1);
    }
    _mm256_zeroupper();
}
__declspec(noinline) void tickPackedAvx2_8(PackedStage<8>* s,std::size_t n,double* v,std::size_t l) noexcept {tick(s,n,v,l);}
__declspec(noinline) void tickPackedAvx2_16(PackedStage<16>* s,std::size_t n,double* v,std::size_t l) noexcept {tick(s,n,v,l);}
}

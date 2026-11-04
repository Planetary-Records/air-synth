#include "SpaceReverbPacked.h"
#if defined(AIRACK_SPACE_AVX2)
#define NOMINMAX
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace airack::space::detail {
#if defined(AIRACK_SPACE_AVX2)
void tickPackedAvx2_8(PackedStage<8>*,std::size_t,double*,std::size_t) noexcept;
void tickPackedAvx2_16(PackedStage<16>*,std::size_t,double*,std::size_t) noexcept;
#endif
PackedKernel<8> availableAvx2Kernel8() noexcept {
#if defined(AIRACK_SPACE_AVX2)
    if(IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE))return &tickPackedAvx2_8;
#endif
    return nullptr;
}
PackedKernel<16> availableAvx2Kernel16() noexcept {
#if defined(AIRACK_SPACE_AVX2)
    if(IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE))return &tickPackedAvx2_16;
#endif
    return nullptr;
}
void tickPackedSse2(PackedStage<8>* stages,std::size_t count,double* values,std::size_t lines) noexcept {
#if defined(_M_X64) || defined(__SSE2__)
    for(std::size_t group=0;group<lines;group+=8){
        auto x0=_mm_loadu_pd(values+group),x1=_mm_loadu_pd(values+group+2),x2=_mm_loadu_pd(values+group+4),x3=_mm_loadu_pd(values+group+6);
        for(std::size_t index=0;index<count;++index){auto& s=stages[index];
            auto pair=[&](const __m128d x,std::size_t lane){
                const auto y=_mm_add_pd(_mm_mul_pd(_mm_load_pd(s.b0.data()+lane),x),_mm_load_pd(s.z1.data()+lane));
                _mm_store_pd(s.z1.data()+lane,_mm_add_pd(_mm_sub_pd(_mm_mul_pd(_mm_load_pd(s.b1.data()+lane),x),_mm_mul_pd(_mm_load_pd(s.a1.data()+lane),y)),_mm_load_pd(s.z2.data()+lane)));
                _mm_store_pd(s.z2.data()+lane,_mm_sub_pd(_mm_mul_pd(_mm_load_pd(s.b2.data()+lane),x),_mm_mul_pd(_mm_load_pd(s.a2.data()+lane),y)));return y;
            };
            x0=pair(x0,group);x1=pair(x1,group+2);x2=pair(x2,group+4);x3=pair(x3,group+6);
        }
        _mm_storeu_pd(values+group,x0);_mm_storeu_pd(values+group+2,x1);_mm_storeu_pd(values+group+4,x2);_mm_storeu_pd(values+group+6,x3);
    }
#else
    for(std::size_t lane=0;lane<lines;++lane)for(std::size_t i=0;i<count;++i){auto& s=stages[i];const double x=values[lane],y=s.b0[lane]*x+s.z1[lane];s.z1[lane]=s.b1[lane]*x-s.a1[lane]*y+s.z2[lane];s.z2[lane]=s.b2[lane]*x-s.a2[lane]*y;values[lane]=y;}
#endif
}
}

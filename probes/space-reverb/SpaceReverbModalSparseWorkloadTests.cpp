#include "ModalServiceProfile.h"
#include "ModalField.h"
#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <type_traits>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;unsigned checks=0,failures=0;}
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t a){if(tracking)++allocations;
#if defined(_MSC_VER)
    auto* p=_aligned_malloc(n?n:1,std::size_t(a));
#else
    auto* p=std::aligned_alloc(std::size_t(a),((n?n:1)+std::size_t(a)-1)/std::size_t(a)*std::size_t(a));
#endif
    if(p)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept {if(tracking&&p)++frees;
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete(void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
void operator delete[](void* p,std::size_t,std::align_val_t a) noexcept {::operator delete(p,a);}
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"MODAL_SERVICE_PROFILE_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space::probe;
void densities(){unsigned complete=0;for(std::size_t modes:{128U,192U,256U,512U,1024U})for(unsigned row:{0U,6U,12U,18U}){
 ServiceCpuWorkload a,b;const bool one=a.prepare(row,false,modes),two=b.prepare(row,false,modes);CHECK(one&&two);if(!one||!two)continue;
 for(unsigned bank=0;bank<2;++bank){const auto* ready=a.prepared(bank);CHECK(ready&&ready->modes()==modes);if(!ready)continue;for(unsigned i=0;i<8;++i){const auto& slot=ready->slot(i);CHECK(slot.room.modal.count==modes&&slot.wet.post.coefficients.count==12);CHECK(slot.settings.character==1&&slot.settings.width==2&&slot.settings.duckRangeDb==24);}}
 const auto* saved=a.prepared(0);CHECK(!a.prepare(row,false,768));CHECK(a.prepared(0)==saved&&saved->modes()==modes);CHECK(!a.prepare(24,false,modes));CHECK(a.prepared(0)==saved);
 bool exact=true,clean=true;unsigned admissions=0;for(unsigned n=0;n<32;++n){const std::uint64_t ordinal=row>=12?std::uint64_t(n)*173:n*7;allocations=frees=0;tracking=true;const bool ok=a.begin(ordinal)&&a.renderAndEnd()&&b.begin(ordinal)&&b.renderAndEnd();const auto x=a.observe(),y=b.observe();tracking=false;exact=exact&&ok&&x.valid&&y.valid&&x.peak==y.peak&&x.checksum==y.checksum&&x.status==y.status;clean=clean&&allocations==0&&frees==0;admissions+=unsigned(x.roomCopy+x.postAdmission+x.coldInstall);CHECK(a.pump()==b.pump());}CHECK(exact&&clean);CHECK(admissions>0);CHECK(a.storageBytes()>0&&a.storageBytes()<=8*16*1024*1024);++complete;
 }CHECK(complete==20);std::cout<<"MODAL_SPARSE_WORKLOAD complete="<<complete<<" expected=20 actual_timing=0\n";}
void defaults(){ServiceCpuWorkload w;CHECK(w.prepare(0,false));const auto* p=w.prepared(0);CHECK(p&&p->modes()==1024);}
}
int main(){densities();defaults();std::cout<<"SpaceReverbModalSparseWorkload checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

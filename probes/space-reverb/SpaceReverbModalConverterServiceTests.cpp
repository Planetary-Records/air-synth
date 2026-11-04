#include "ModalService.h"
#include <cfenv>

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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"CONVERTER_SERVICE_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;
ModalConverterBackend resolved(ModalConverterBackend b){if(b!=ModalConverterBackend::automatic)return b;
#if defined(_M_X64) || defined(__SSE2__)
    return ModalConverterBackend::mirroredSse2;
#else
    return ModalConverterBackend::mirroredScalar;
#endif
}
void run(unsigned factor,ModalConverterBackend backend){
    ModalSlotSettings settings;ModalSlotIdentities ids{21,22,23,24,25,26,27,28};
    for(unsigned i=0;i<8;++i){settings[i].size=.1*(i+1);settings[i].mix=.7;settings[i].decay.decaySeconds=.2+.1*i;}
    auto first=prepareModalBundle(8000,128,91,settings,ids,{},factor);auto second=prepareModalBundle(8000,128,92,settings,ids,{},factor);CHECK(first&&second);if(!first||!second)return;
    ModalService service;ModalServiceOptions options;options.threaded=false;options.factor=factor;options.converterBackend=backend;
    const bool prepared=service.prepareStorage(8000,128,options);
#if !defined(_M_X64) && !defined(__SSE2__)
    if(backend==ModalConverterBackend::mirroredSse2){CHECK(!prepared);return;}
#endif
    CHECK(prepared);if(!prepared)return;CHECK(!service.converterBackend());
    allocations=frees=0;tracking=true;const bool installed=service.installBundle(*first,91);tracking=false;CHECK(installed);CHECK(allocations==0&&frees==0);
    CHECK(service.converterBackend()==resolved(backend));CHECK(service.storageBytes()<128*1024*1024);
    std::array<ModalRoom::Frame,512> samples{};
    allocations=frees=0;tracking=true;
    bool ok=true,finite=true;
    for(std::size_t length:{1U,3U,17U,64U,511U,512U}){
        ok=service.beginParent(settings,255,ids,length)&&ok;
        for(unsigned slot=0;slot<8;++slot){for(std::size_t n=0;n<length;++n)samples[n]={double(int(n*17%29)-14)/64.,double(int(n*13%31)-15)/64.};
            ok=service.renderSlot(slot,std::span(samples).first(length))&&ok;
            for(auto frame:std::span(samples).first(length))for(double v:frame)finite=finite&&std::isfinite(v);
        }
        ok=service.endParent()&&ok;CHECK(service.converterBackend()==resolved(backend));
    }
    for(unsigned slot=0;slot<8;++slot)ok=service.resetSlot(slot)&&ok;
    ok=service.installBundle(*second,92)&&ok;tracking=false;
    CHECK(ok&&finite);CHECK(allocations==0&&frees==0);CHECK(service.converterBackend()==resolved(backend));
    ModalServiceStatus before,after;CHECK(service.snapshot(before));options.converterBackend=static_cast<ModalConverterBackend>(999);
    CHECK(!service.prepareStorage(8000,128,options));CHECK(service.snapshot(after));CHECK(before==after);CHECK(service.converterBackend()==resolved(backend));
    std::cout<<"CONVERTER_SERVICE_SELECTION factor="<<factor<<" requested="<<int(backend)<<" actual="<<int(*service.converterBackend())<<" slots=8 cold_installs=2 heap=0 finite="<<finite<<" actual_timing=0\n";
}
}
int main(){
    CHECK(ModalServiceOptions{}.converterBackend==ModalConverterBackend::automatic);
    for(unsigned factor:{1U,2U})for(auto backend:{ModalConverterBackend::reference,ModalConverterBackend::mirroredScalar,ModalConverterBackend::mirroredSse2,ModalConverterBackend::automatic})run(factor,backend);
    std::cout<<"CONVERTER_SERVICE checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

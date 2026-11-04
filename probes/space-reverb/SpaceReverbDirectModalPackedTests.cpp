#include "ModalKernel.h"
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<50)std::cerr<<"DIRECT_MODAL_PACKED_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;
std::uint64_t framesCompared=0;unsigned comparisons=0;
bool equalAudio(const std::vector<ModalFrame>& a,const std::vector<ModalFrame>& b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(ModalFrame))==0;}
bool equalStates(const ModalTank& a,const ModalTank& b,std::size_t count){
    for(std::size_t k=0;k<count;++k)if(std::bit_cast<std::array<std::uint64_t,2>>(a.modeState(k))!=std::bit_cast<std::array<std::uint64_t,2>>(b.modeState(k)))return false;return true;
}
bool run(ModalTank& tank,std::vector<ModalFrame>& audio,std::size_t partition){
    bool ok=true;for(std::size_t n=0;n<audio.size();n+=partition)ok=tank.processBlock(std::span(audio).subspan(n,std::min(partition,audio.size()-n)))&&ok;return ok;
}
DecayCurve cut(){DecayCurve c;c.brightness=1;for(auto& b:c.bands)b={true,false,DecayShape::bell,20,.25,4};return c;}
std::vector<ModalBackend> backends(){std::vector<ModalBackend> out;if(modalSse2Available())out.push_back(ModalBackend::sse2);out.push_back(ModalBackend::automatic);if(availableModalAvx2Kernel())out.push_back(ModalBackend::avx2);return out;}
const char* expectedName(ModalBackend b){return b==ModalBackend::sse2?"sse2":availableModalAvx2Kernel()?"avx2":modalSse2Available()?"sse2":"scalar";}
void dispatch(){
    CHECK(modalKernelFor(ModalBackend::scalar,false,nullptr)==&modalScalar);
    CHECK(modalKernelFor(ModalBackend::sse2,false,nullptr)==nullptr);
    CHECK(modalKernelFor(ModalBackend::sse2,true,nullptr)==&modalSse2);
    CHECK(modalKernelFor(ModalBackend::automatic,false,nullptr)==&modalScalar);
    CHECK(modalKernelFor(ModalBackend::automatic,true,nullptr)==&modalSse2);
    CHECK(modalKernelFor(ModalBackend::automatic,true,&modalScalar)==&modalScalar);
    CHECK(modalKernelFor(ModalBackend::avx2,true,nullptr)==nullptr);
    CHECK(modalKernelFor(static_cast<ModalBackend>(99),true,&modalScalar)==nullptr);
    ModalTank t;CHECK(std::string_view(t.backendName())=="unprepared");
    CHECK(t.prepareStorage(48000,ModalBackend::scalar));CHECK(std::string_view(t.backendName())=="scalar");
    CHECK(!t.prepareStorage(48000,static_cast<ModalBackend>(99)));CHECK(std::string_view(t.backendName())=="scalar");
    CHECK(t.prepareStorage(48000,ModalBackend::avx2)==bool(availableModalAvx2Kernel()));
    allocations=frees=0;tracking=true;auto* known=::operator new(32,std::align_val_t(64));::operator delete(known,std::align_val_t(64));tracking=false;CHECK(allocations==1&&frees==1);
}
void equivalent(){
    auto p=std::make_unique<PreparedModal>();auto flat=DecayCurve{};flat.brightness=1;
    std::vector<ModalFrame> input(4096);std::uint32_t random=0x9182ac31U;
    for(std::size_t n=0;n<input.size();++n){auto noise=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return .002*(double(random)/4294967295.-.5);};input[n]={n==0?.5:noise(),n==0?.25:noise()};}
    const auto policies=backends();
    for(const auto& curve:{flat,cut()})for(auto count:{1024U,2048U})for(double rate:{44100.,48000.,96000.})for(double size:{0.,1.}){
        CHECK(prepareModal(curve,size,rate,count,*p));ModalTank scalar;CHECK(scalar.prepareStorage(rate));CHECK(scalar.configure(*p));
        auto reference=input;allocations=frees=0;tracking=true;const bool referenceOk=run(scalar,reference,64);tracking=false;CHECK(referenceOk);CHECK(allocations==0&&frees==0);
        for(auto policy:policies)for(std::size_t partition:{64U,512U}){
            ModalTank packed;CHECK(packed.prepareStorage(rate,policy));CHECK(std::string_view(packed.backendName())==expectedName(policy));CHECK(packed.configure(*p));
            auto actual=input;allocations=frees=0;tracking=true;const bool ok=run(packed,actual,partition);const bool states=equalStates(packed,scalar,count);tracking=false;
            CHECK(ok);CHECK(equalAudio(actual,reference));CHECK(states);CHECK(allocations==0&&frees==0);CHECK(packed.storageBytes()<=16*1024*1024);
            ++comparisons;framesCompared+=input.size();
            actual=input;allocations=frees=0;tracking=true;packed.reset();const bool resetOk=run(packed,actual,partition);tracking=false;
            CHECK(resetOk);CHECK(equalAudio(actual,reference));CHECK(allocations==0&&frees==0);
        }
    }
}
void copied(){
    auto p=std::make_unique<PreparedModal>(),q=std::make_unique<PreparedModal>();auto flat=DecayCurve{};flat.brightness=1;
    CHECK(prepareModal(flat,.5,48000,1024,*p));CHECK(prepareModal(cut(),1,48000,1024,*q));
    for(auto backend:backends()){
        ModalTank source,control,packed;CHECK(source.prepareStorage(48000));CHECK(control.prepareStorage(48000));CHECK(packed.prepareStorage(48000,backend));CHECK(source.configure(*p));
        std::vector<ModalFrame> history(1024);history[0]={.5,.25};CHECK(run(source,history,64));
        for(const auto* descriptor:{p.get(),q.get()}){
            std::vector<ModalFrame> a(512),b(512);allocations=frees=0;tracking=true;
            const bool adopted=packed.adoptCopied(*descriptor,source)&&control.adoptCopied(*descriptor,source);
            const bool states=equalStates(packed,control,1024);const bool ok=run(packed,a,64)&&run(control,b,512);const bool continued=equalStates(packed,control,1024);tracking=false;
            CHECK(adopted);CHECK(states);CHECK(ok);CHECK(equalAudio(a,b));CHECK(continued);CHECK(allocations==0&&frees==0);
        }
        const auto* name=packed.backendName();CHECK(!packed.prepareStorage(48000,static_cast<ModalBackend>(99)));CHECK(packed.prepared()==*q);CHECK(std::string_view(packed.backendName())==name);
    }
}
void excitationEdges(){
    auto p=std::make_unique<PreparedModal>();std::vector<ModalFrame> input(1536);
    for(std::size_t n=0;n<input.size();++n){const double a=n%11==0?-0.:n%7==0?0.:n%13==0?0x1p-900:double(int(n%19)-9)/16384.;const double b=n%5==0?-0.:n%3==0?0.:n%17==0?-0x1p-900:double(int(n%23)-11)/32768.;input[n]={a,b};}
    for(auto count:{1024U,2048U})for(double rate:{8000.,48000.,768000.})for(bool generic:{false,true}){
        CHECK(prepareModal({},.5,rate,count,*p));
        if(generic){for(std::size_t k=0;k<p->count;k+=3){if(p->bReal[k]==0)p->bReal[k]=-0.;if(p->bImag[k]==0)p->bImag[k]=-0.;}
            p->bReal[0]=.3;p->bImag[0]=-.4;p->inputWeights[0]={.25,-.75};}
        ModalTank scalar;CHECK(scalar.prepareStorage(rate));CHECK(scalar.configure(*p));auto expected=input;CHECK(run(scalar,expected,17));
        for(auto backend:backends())for(std::size_t partition:{17U,512U}){
            ModalTank candidate;CHECK(candidate.prepareStorage(rate,backend));CHECK(candidate.configure(*p));auto actual=input;
            allocations=frees=0;tracking=true;const bool ok=run(candidate,actual,partition),states=equalStates(candidate,scalar,count);tracking=false;
            CHECK(ok);CHECK(equalAudio(actual,expected));CHECK(states);CHECK(allocations==0&&frees==0);++comparisons;framesCompared+=input.size();
        }
    }
}
}
int main(){dispatch();equivalent();copied();excitationEdges();std::cout<<"SpaceReverbDirectModalPacked checks="<<checks<<" failures="<<failures<<" comparisons="<<comparisons<<" frames="<<framesCompared<<" sse2="<<airack::space::probe::modalSse2Available()<<" avx2="<<bool(airack::space::probe::availableModalAvx2Kernel())<<'\n';return failures?1:0;}

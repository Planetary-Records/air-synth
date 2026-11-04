#include "ModalService.h"
#include "ModalConverter.h"
#include <thread>
#include <atomic>
#include <chrono>
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"MODAL_512_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;using Frame=ModalTank::Frame;
bool same(double a,double b){return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
DecayCurve shaped(){DecayCurve c;c.decaySeconds=20;c.brightness=.5;for(unsigned i=0;i<6;++i)c.bands[i]={true,false,DecayShape::bell,53.*std::pow(2.,i*1.5),i%2?4.:.25,4};return c;}
void core(){unsigned complete=0;for(double rate:{8000.,44100.,48000.,96000.,192000.,384000.,768000.})for(double size:{0.,.5,1.})for(const auto& curve:{DecayCurve{},shaped()}){
 auto p=std::make_unique<PreparedModal>(),reference=std::make_unique<PreparedModal>();const bool prepared=prepareModal(curve,size,rate,512,*p);CHECK(prepared);if(!prepared)continue;
 CHECK(p->count==512);CHECK(prepareModal(curve,size,rate,1024,*reference));CHECK(std::abs(p->pickup/reference->pickup-std::sqrt(2.))<1e-12);
 for(unsigned k=0;k<512;++k){const double radius=std::hypot(p->rc[k],p->rs[k]),time=-std::log(1000.)/(rate*std::log(radius));CHECK(radius>0&&radius<1);CHECK(std::isfinite(time)&&std::abs(time/p->target[k]-1)<=1e-6);}
 ModalTank scalar,automatic,copied;CHECK(scalar.prepareStorage(rate,ModalBackend::scalar));CHECK(automatic.prepareStorage(rate,ModalBackend::automatic));CHECK(copied.prepareStorage(rate,ModalBackend::automatic));CHECK(scalar.configure(*p));CHECK(automatic.configure(*p));CHECK(copied.configure(*p));
 std::array<Frame,777> a{},b{};for(unsigned n=0;n<a.size();++n)a[n]=b[n]={n<513?double(int(n*17%101)-50)/128.:0,n<513?double(int(n*31%97)-48)/256.:0};
 allocations=frees=0;tracking=true;std::size_t at=0,index=0;constexpr std::array<std::size_t,5> parts{1,3,17,64,512};while(at<a.size()){const auto n=std::min(parts[index++%parts.size()],a.size()-at);CHECK(scalar.processBlock(std::span(a).subspan(at,n)));CHECK(automatic.processBlock(std::span(b).subspan(at,n)));at+=n;}
 for(unsigned n=0;n<a.size();++n)for(unsigned c=0;c<2;++c)CHECK(same(a[n][c],b[n][c]));
 CHECK(copied.adoptCopied(*p,automatic));for(unsigned k=0;k<512;++k){const auto x=scalar.modeState(k),y=automatic.modeState(k),z=copied.modeState(k);CHECK(same(x.real,y.real)&&same(x.imag,y.imag));CHECK(same(y.real,z.real)&&same(y.imag,z.imag));}
 automatic.reset();for(unsigned k=0;k<512;++k){const auto x=automatic.modeState(k);CHECK(x.real==0&&x.imag==0);}tracking=false;CHECK(allocations==0&&frees==0);CHECK(automatic.storageBytes()<=16*1024*1024);
 Settings settings;settings.decay=curve;settings.size=size;settings.character=1;settings.mix=1;auto field=std::make_unique<PreparedModalField>();CHECK(prepareModalField(settings,rate,512,*field));CHECK(field->tail.passed&&std::isfinite(field->tail.seconds)&&field->tail.seconds>0);
 auto room=std::make_unique<PreparedModalRoom>();ModalTailContext context;PreparedModalTail roomTail;CHECK(prepareModalRoom(curve,size,rate,512,*room));CHECK(prepareModalTailContext(rate,512,context));CHECK(prepareModalRoomTail(context,*room,roomTail));++complete;
 }std::cout<<"MODAL_512_CORE configurations="<<complete<<" expected=42\n";CHECK(complete==42);}
void service(){for(unsigned factor:{1U,2U})for(double rate:{8000.,44100.,48000.,96000.}){
 ModalSlotSettings settings;ModalSlotIdentities ids{11,12,13,14,15,16,17,18};for(unsigned i=0;i<8;++i){settings[i].decay=shaped();settings[i].character=1;settings[i].mix=.35;settings[i].duckRangeDb=24;}
 auto bundle=prepareModalBundle(rate,512,7,settings,ids,{},factor);CHECK(bool(bundle));if(!bundle)continue;ModalService a,b;ModalServiceOptions options;options.threaded=false;options.factor=factor;CHECK(a.prepareStorage(rate,512,options));CHECK(b.prepareStorage(rate,512,options));CHECK(a.installBundle(*bundle,7));CHECK(b.installBundle(*bundle,7));
 std::array<Frame,128> x{},y{};const auto frames=std::size_t(64*factor);for(unsigned k=0;k<frames;++k)x[k]=y[k]={k==0?.5:0,k==0?-.25:0};
 allocations=frees=0;tracking=true;CHECK(a.beginParent(settings,255,ids,frames));CHECK(b.beginParent(settings,255,ids,frames));for(unsigned i=0;i<8;++i){CHECK(a.renderSlot(i,std::span(x).first(frames)));CHECK(b.renderSlot(i,std::span(y).first(frames)));}CHECK(a.endParent());CHECK(b.endParent());for(unsigned k=0;k<frames;++k)for(unsigned c=0;c<2;++c)CHECK(std::isfinite(x[k][c])&&same(x[k][c],y[k][c]));
 std::array<std::size_t,8> slots{0,1,2,3,4,5,6,7};ModalTailReport tail;CHECK(a.composeTail(slots,1,tail));CHECK(tail.passed&&tail.frames>0&&std::isfinite(tail.seconds));for(unsigned i=0;i<8;++i)CHECK(a.resetSlot(i));tracking=false;CHECK(allocations==0&&frees==0);CHECK(a.storageBytes()+bundle->storageBytes()<=8*16*1024*1024);
 }}
}
int main(){core();service();std::cout<<"SpaceReverbModal512 checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

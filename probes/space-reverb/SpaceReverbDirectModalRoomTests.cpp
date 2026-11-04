#include "ModalRoom.h"
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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<60)std::cerr<<"DIRECT_MODAL_ROOM_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;using namespace airack::space::probe;using Frame=ModalRoom::Frame;
std::uint64_t compared=0;std::size_t cases=0;
std::vector<Frame> stimulus(std::size_t n){std::vector<Frame> a(n);for(std::size_t i=0;i<n;++i)a[i]={i==0?.5:double(int(i%29)-14)/65536.,i==0?.25:double(int(i%31)-15)/65536.};return a;}
bool render(ModalRoom& r,std::vector<Frame>& a,const std::vector<double>& c,std::size_t block){bool ok=true;for(std::size_t i=0;i<a.size();i+=block){const auto n=std::min(block,a.size()-i);ok=r.processBlock(std::span(a).subspan(i,n),std::span(c).subspan(i,n))&&ok;}return ok;}
bool equal(const std::vector<Frame>& a,const std::vector<Frame>& b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size()*sizeof(Frame))==0;}
bool sameModes(const ModalRoom& a,const ModalRoom& b,std::size_t n){for(std::size_t k=0;k<n;++k)if(a.modeState(k)!=b.modeState(k))return false;return true;}
void invalid(){
    auto p=std::make_unique<PreparedModalRoom>(),saved=std::make_unique<PreparedModalRoom>();CHECK(prepareModalRoom({},.5,48000,1024,*p));*saved=*p;
    auto curve=DecayCurve{};curve.bands[5].q=.2;CHECK(!prepareModalRoom(curve,.5,48000,1024,*p));CHECK(*p==*saved);
    CHECK(!prepareModalRoom({},-.1,48000,1024,*p));CHECK(!prepareModalRoom({},.5,7999,1024,*p));CHECK(!prepareModalRoom({},.5,768001,1024,*p));CHECK(!prepareModalRoom({},.5,48000,1000,*p));
    unsigned calls=0;const auto cancelled=[](void* v) noexcept {return ++*static_cast<unsigned*>(v)>2;};CHECK(!prepareModalRoom({},.5,48000,1024,*p,{cancelled,&calls}));CHECK(*p==*saved);
    ModalRoom a,b;auto input=stimulus(513),before=input;std::vector<double> chars(513,.25);CHECK(!a.processBlock(input,chars));CHECK(equal(input,before));CHECK(!a.configure(*p));CHECK(!a.adoptCopied(*p,b));CHECK(!a.prepareStorage(0));a.reset();
    allocations=frees=0;tracking=true;auto* known=::operator new(32,std::align_val_t(64));::operator delete(known,std::align_val_t(64));tracking=false;CHECK(allocations==1&&frees==1);
}
void fieldEquivalent(){
    std::array<DecayCurve,3> curves{};curves[1].brightness=1;curves[2].brightness=1;for(auto& b:curves[1].bands)b={true,false,DecayShape::bell,20,.25,4};for(auto& b:curves[2].bands)b={true,false,DecayShape::bell,1000,4,4};
    auto p=std::make_unique<PreparedModalRoom>();auto f=std::make_unique<PreparedModalField>();
    for(double rate:{8000.,44100.,48000.,96000.,768000.})for(auto count:{1024U,2048U})for(double size:{0.,1.})for(const auto& curve:curves)for(double character:{0.,.25,1.}){
        Settings s;s.decay=curve;s.size=size;s.predelayMs=0;s.distance=1;s.mix=1;s.character=character;
        CHECK(prepareModalField(s,rate,count,*f));CHECK(prepareModalRoom(curve,size,rate,count,*p));CHECK(p->modal==f->modal);
        ModalField reference;ModalRoom room;CHECK(reference.prepareStorage(rate));CHECK(reference.configure(*f));CHECK(room.prepareStorage(rate));CHECK(room.configure(*p));
        const auto input=stimulus(5120);auto expected=input,actual=input;std::vector<double> chars(input.size(),character);bool ok=true;
        for(std::size_t at=0;at<expected.size();at+=512)ok=reference.processBlock(std::span(expected).subspan(at,std::min<std::size_t>(512,expected.size()-at)))&&ok;
        allocations=frees=0;tracking=true;ok=render(room,actual,chars,64)&&ok;tracking=false;CHECK(ok);CHECK(equal(expected,actual));CHECK(allocations==0&&frees==0);CHECK(room.storageBytes()<=16*1024*1024);++cases;compared+=input.size();
        if(rate==48000&&count==1024&&size==0&&curve==curves[0])for(std::size_t block:{1U,17U,512U}){actual=input;allocations=frees=0;tracking=true;room.reset();ok=render(room,actual,chars,block);tracking=false;CHECK(ok);CHECK(equal(expected,actual));CHECK(allocations==0&&frees==0);}
    }
}
void copied(){
    auto p=std::make_unique<PreparedModalRoom>(),changed=std::make_unique<PreparedModalRoom>(),bad=std::make_unique<PreparedModalRoom>();
    for(double rate:{8000.,48000.,768000.}){
        CHECK(prepareModalRoom({},0,rate,1024,*p));auto curve=DecayCurve{};curve.decaySeconds=20;CHECK(prepareModalRoom(curve,1,rate,1024,*changed));
        ModalRoom source,destination;CHECK(source.prepareStorage(rate));CHECK(destination.prepareStorage(rate));CHECK(source.configure(*p));CHECK(destination.configure(*changed));
        auto in=stimulus(16384),other=stimulus(1024);std::vector<double> c(in.size()),d(other.size(),.83);for(std::size_t n=0;n<c.size();++n)c[n]=double(n%257)/256.;CHECK(render(source,in,c,512));CHECK(render(destination,other,d,64));
        const auto state=source.modeState(0);allocations=frees=0;tracking=true;const bool adopted=destination.adoptCopied(*p,source),states=sameModes(source,destination,1024);tracking=false;CHECK(adopted);CHECK(states);CHECK(source.modeState(0)==state);CHECK(allocations==0&&frees==0);CHECK(destination.historyCopyBytes()>=2*1024*sizeof(double));
        auto a=stimulus(8192),b=a;std::vector<double> chars(a.size(),.61);allocations=frees=0;tracking=true;const bool continued=render(source,a,chars,17)&&render(destination,b,chars,512);tracking=false;CHECK(continued);CHECK(equal(a,b));CHECK(sameModes(source,destination,1024));CHECK(allocations==0&&frees==0);
        *bad=*p;bad->diffusionSamples[0]=-1;const auto before=destination.modeState(0);CHECK(!destination.adoptCopied(*bad,source));CHECK(destination.modeState(0)==before);CHECK(!destination.adoptCopied(*p,destination));CHECK(destination.modeState(0)==before);
        ModalRoom unready;CHECK(unready.prepareStorage(rate));CHECK(!destination.adoptCopied(*p,unready));CHECK(destination.modeState(0)==before);
        allocations=frees=0;tracking=true;const bool changedOk=destination.adoptCopied(*changed,source),copiedModes=sameModes(destination,source,1024);tracking=false;CHECK(changedOk);CHECK(copiedModes);CHECK(allocations==0&&frees==0);
        auto tail=stimulus(1024);for(auto& x:tail)x={};std::vector<double> motion(tail.size(),1);CHECK(render(destination,tail,motion,64));double energy=0;for(auto x:tail)for(double v:x)energy+=v*v;CHECK(energy>0&&std::isfinite(energy));
        auto data=stimulus(512),saved=data;std::vector<double> invalidChars(512,.25);invalidChars.back()=std::numeric_limits<double>::quiet_NaN();const auto preserved=destination.modeState(0);CHECK(!destination.processBlock(data,invalidChars));CHECK(equal(data,saved));CHECK(destination.modeState(0)==preserved);CHECK(!destination.processBlock(data,std::span(invalidChars).first(511)));CHECK(destination.modeState(0)==preserved);CHECK(destination.processBlock({},{}));CHECK(destination.modeState(0)==preserved);
        auto tooMany=stimulus(513);std::vector<double> tooManyChars(513,.25);CHECK(!destination.processBlock(tooMany,tooManyChars));CHECK(destination.modeState(0)==preserved);
        destination.reset();source.reset();CHECK(destination.configure(*p));data=stimulus(2048);saved=data;motion.assign(data.size(),.3);CHECK(render(destination,data,motion,512));CHECK(render(source,saved,motion,17));CHECK(equal(data,saved));CHECK(destination.storageBytes()<=16*1024*1024);
        std::cout<<"MODAL_ROOM_RESOURCE rate="<<rate<<" bytes="<<destination.storageBytes()<<" history_copy_bytes="<<destination.historyCopyBytes()<<'\n';
    }
    ModalRoom source,dest;CHECK(source.prepareStorage(48000));CHECK(dest.prepareStorage(96000));CHECK(prepareModalRoom({},.5,48000,1024,*p));CHECK(source.configure(*p));CHECK(!dest.adoptCopied(*p,source));
    CHECK(dest.prepareStorage(48000));CHECK(prepareModalRoom({},.5,48000,2048,*changed));CHECK(dest.configure(*changed));CHECK(!dest.adoptCopied(*changed,source));
}
}
int main(){invalid();fieldEquivalent();copied();std::cout<<"SpaceReverbDirectModalRoom checks="<<checks<<" failures="<<failures<<" reference_cases="<<cases<<" compared_frames="<<compared<<'\n';return failures?1:0;}

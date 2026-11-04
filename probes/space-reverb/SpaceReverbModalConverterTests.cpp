#include "ModalConverter.h"

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
#define CHECK(x) do{++checks;if(!(x)){++failures;if(failures<70)std::cerr<<"MODAL_CONVERTER_FAIL line="<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space::probe;using Frame=ModalWetConverter::Frame;
std::vector<Frame> input(std::size_t n){std::vector<Frame> x(n);for(std::size_t i=0;i<n;++i)x[i]={double(int(i*17%97)-48)/16.,double(int(i*37%89)-44)/16.};return x;}
Frame transform(Frame x){return {.25*x[0]+.75*x[1],.5*x[0]-.5*x[1]};}
bool bits(std::span<const Frame> a,std::span<const Frame> b){return a.size()==b.size()&&std::memcmp(a.data(),b.data(),a.size_bytes())==0;}
std::vector<Frame> oracle(std::span<const Frame> x,const PreparedModalConverter& p,bool changed){std::vector<Frame> excitation(x.size()),out(x.size());if(p.factor()==1){for(std::size_t n=0;n<x.size();++n)out[n]=changed?transform(x[n]):x[n];return out;}
    for(std::size_t n=0;n<x.size();n+=2){Frame y{};for(unsigned c=0;c<2;++c){long double v=0;for(std::size_t k=0;k<127&&k<=n;++k)v+=static_cast<long double>(p.tap(k))*x[n-k][c];y[c]=double(v);}if(changed)y=transform(y);excitation[n]={2*y[0],2*y[1]};}
    for(std::size_t n=0;n<x.size();++n)for(unsigned c=0;c<2;++c){long double v=0;for(std::size_t k=0;k<127&&k<=n;++k)v+=static_cast<long double>(p.tap(k))*excitation[n-k][c];out[n][c]=double(v);}return out;
}
bool run(ModalWetConverter& converter,std::span<Frame> x,std::size_t block,bool changed){std::array<Frame,512> wet;for(std::size_t at=0;at<x.size();at+=block){auto physical=x.subspan(at,std::min(block,x.size()-at));std::size_t produced=999;if(!converter.down(physical,wet,produced))return false;auto logical=std::span(wet).first(produced);if(changed)for(auto& frame:logical)frame=transform(frame);if(!converter.up(logical,physical))return false;}return true;}
void metadata(){PreparedModalConverter p;CHECK(!prepareModalConverter(0,p));CHECK(p.factor()==0);CHECK(!prepareModalConverter(3,p));CHECK(prepareModalConverter(2,p));CHECK(p.factor()==2);const auto saved=p;CHECK(!prepareModalConverter(99,p));CHECK(p==saved);long double sum=0,absolute=0;for(std::size_t i=0;i<127;++i){CHECK(std::isfinite(p.tap(i)));CHECK(std::bit_cast<std::uint64_t>(p.tap(i))==std::bit_cast<std::uint64_t>(p.tap(126-i)));if(i%2&&i!=63)CHECK(p.tap(i)==0);sum+=p.tap(i);absolute+=std::abs(p.tap(i));std::cout<<"MODAL_CONVERTER_TAP index="<<i<<" value="<<p.tap(i)<<'\n';}
    CHECK(p.tap(63)==.5);CHECK(std::abs(sum-1)<1e-14);CHECK(p.absoluteSum()>=absolute);CHECK(p.peakGain()>=2*absolute*absolute);CHECK(p.logGain()>=std::log(static_cast<long double>(p.peakGain())));CHECK(p.physicalSupport()==252&&p.logicalSupport()==127&&p.physicalGroupDelay()==126);
    double passError=0,stopMaximum=0;for(unsigned n=0;n<=4000;++n){const long double f=static_cast<long double>(n)/8000;std::complex<long double> h{};for(unsigned k=0;k<127;++k)h+=static_cast<long double>(p.tap(k))*std::polar(1.L,-2*std::numbers::pi_v<long double>*f*k);const double magnitude=double(std::abs(h));if(f<=.2L)passError=std::max(passError,std::abs(40*std::log10(magnitude)));if(f>=.3L)stopMaximum=std::max(stopMaximum,magnitude);}
    CHECK(passError<=.01);CHECK(stopMaximum<=std::pow(10.,-90./20));std::cout<<"MODAL_CONVERTER_RESPONSE pass_error_db="<<passError<<" stop_maximum="<<stopMaximum<<" l1="<<p.absoluteSum()<<" gain="<<p.peakGain()<<" log_gain="<<p.logGain()<<'\n';
    CHECK(prepareModalConverter(1,p));CHECK(p.factor()==1&&p.tap(0)==1&&p.absoluteSum()==1&&p.peakGain()==1&&p.logGain()==0&&p.physicalSupport()==0);
}
void partitions(){unsigned comparisons=0;double maximum=0;for(unsigned factor:{1U,2U})for(bool changed:{false,true}){PreparedModalConverter p;CHECK(prepareModalConverter(factor,p));auto raw=input(8193);auto expected=oracle(raw,p,changed);std::vector<Frame> baseline;
    for(std::size_t block:{1U,3U,17U,64U,511U,512U}){ModalWetConverter converter;CHECK(converter.configure(p));auto actual=raw;allocations=frees=0;tracking=true;const bool ok=run(converter,actual,block,changed);tracking=false;CHECK(ok);CHECK(allocations==0&&frees==0);double error=0;for(std::size_t n=0;n<raw.size();++n)for(unsigned c=0;c<2;++c)error=std::max(error,std::abs(actual[n][c]-expected[n][c]));maximum=std::max(maximum,error);CHECK(error<=2e-12);if(baseline.empty())baseline=actual;else CHECK(bits(actual,baseline));converter.reset();auto again=raw;CHECK(run(converter,again,block,changed));CHECK(bits(actual,again));++comparisons;}
    if(factor==1&&!changed)CHECK(bits(baseline,raw));
    }std::cout<<"MODAL_CONVERTER_PARTITIONS comparisons="<<comparisons<<" frames_per_case=8193 max_oracle_error="<<maximum<<'\n';
}
void impulseAndGain(){PreparedModalConverter p;CHECK(prepareModalConverter(2,p));ModalWetConverter c;CHECK(c.configure(p));std::vector<Frame> x(1024);x[0]={1,0};CHECK(run(c,x,17,false));std::size_t last=0,peak=0;for(std::size_t n=0;n<x.size();++n){if(x[n][0]!=0)last=n;if(std::abs(x[n][0])>std::abs(x[peak][0]))peak=n;CHECK(x[n][1]==0);}CHECK(last==252&&peak==126);for(std::size_t n=253;n<x.size();++n)CHECK(x[n][0]==0);c.reset();x.assign(4096,{1,-1});CHECK(run(c,x,511,false));for(std::size_t n=512;n<x.size();++n){CHECK(std::abs(x[n][0]-1)<1e-12);CHECK(std::abs(x[n][1]+1)<1e-12);}c.reset();x=input(4096);CHECK(run(c,x,3,false));double maximum=0;for(auto frame:x)for(double value:frame)maximum=std::max(maximum,std::abs(value));CHECK(maximum<=3*p.peakGain());std::cout<<"MODAL_CONVERTER_IMPULSE last="<<last<<" peak="<<peak<<" bounded_peak="<<maximum<<'\n';}

void adversarial(){PreparedModalConverter p;CHECK(prepareModalConverter(2,p));ModalWetConverter c;CHECK(c.configure(p));std::array<Frame,127> physical;std::array<Frame,512> logical;for(unsigned n=0;n<127;++n){const auto sign=std::copysign(1.,p.tap(126-n));physical[n]={sign,-sign};}std::size_t count=0;CHECK(c.down(physical,logical,count));CHECK(count==64);CHECK(std::abs(logical[63][0]-p.absoluteSum())<1e-10);CHECK(logical[63][1]==-logical[63][0]);CHECK(!c.configure(p));CHECK(c.up(std::span(logical).first(count),physical));c.reset();std::array<Frame,256> output{};CHECK(c.down(output,logical,count));CHECK(count==128);long double expected=0;for(unsigned m=0;m<128;++m){const int tap=252-int(2*m);const double h=tap>=0&&tap<127?p.tap(std::size_t(tap)):0;logical[m]={4*std::copysign(1.,h),-4*std::copysign(1.,h)};if(h!=0)expected+=8*std::abs(h);}CHECK(c.up(std::span(logical).first(count),output));CHECK(std::abs(output[252][0]-expected)<1e-11);CHECK(std::abs(output[252][0])<=8*p.absoluteSum());CHECK(output[252][0]==-output[252][1]);std::cout<<"MODAL_CONVERTER_ADVERSARIAL down_peak="<<p.absoluteSum()<<" up_peak="<<output[252][0]<<'\n';}

void invalid(){PreparedModalConverter empty,p;CHECK(prepareModalConverter(2,p));ModalWetConverter c;std::array<Frame,3> physical{{{1,-1},{2,-2},{3,-3}}},before=physical;std::array<Frame,3> logical{};std::size_t count=77;CHECK(!c.down(physical,logical,count));CHECK(count==77&&physical==before);CHECK(!c.up(logical,physical));CHECK(!c.configure(empty));CHECK(c.configure(p));auto control=c;CHECK(!c.down(physical,std::span(logical).first(1),count));CHECK(!c.open()&&c.evenPhase()&&count==77);physical[2][0]=NAN;CHECK(!c.down(physical,logical,count));CHECK(count==77&&!c.open());physical=before;CHECK(!c.down(physical,physical,count));std::array<Frame,513> large{};CHECK(!c.down(large,logical,count));CHECK(c.down(physical,logical,count));CHECK(count==2&&c.open()&&!c.evenPhase());CHECK(!c.down(physical,logical,count));const auto held=logical;CHECK(!c.up(std::span(logical).first(1),physical));CHECK(physical==before&&c.open());logical[1][0]=NAN;CHECK(!c.up(std::span(logical).first(2),physical));CHECK(physical==before&&c.open());logical=held;CHECK(!c.up(std::span(logical).first(2),std::span(logical).first(3)));CHECK(c.up(std::span(logical).first(2),physical));auto expected=before;CHECK(run(control,expected,3,false));CHECK(bits(physical,expected));
    std::array<Frame,1> one{{{0,0}}};std::size_t none=88;CHECK(c.down(one,{},none));CHECK(none==0&&c.evenPhase());CHECK(c.up({},one));CHECK(!c.open());CHECK(!c.up({},{}));c.reset();CHECK(c.evenPhase()&&!c.open());count=55;CHECK(c.down({},logical,count));CHECK(count==0&&c.open());CHECK(c.up({},{}));CHECK(!c.open());
    allocations=frees=0;tracking=true;bool ok=c.configure(p);for(unsigned n=0;n<1000;++n){std::array<Frame,1> sample{{{double(n%5),0}}};std::size_t produced=0;ok=c.down(sample,logical,produced)&&ok;ok=c.up(std::span(logical).first(produced),sample)&&ok;if(n%13==0)c.reset();}tracking=false;CHECK(ok);CHECK(allocations==0&&frees==0);CHECK(c.storageBytes()<8192);
}
}
int main(){std::cout.precision(17);metadata();partitions();impulseAndGain();adversarial();invalid();std::cout<<"SpaceReverbModalConverter checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

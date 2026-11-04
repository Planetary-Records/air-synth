#include "SpaceReverbTank.h"
#include "SpaceReverbTankGolden.h"
#include "SpaceTank.h"
#include "ProbeCases.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;int checks=0,failures=0;}
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t alignment){if(tracking)++allocations;const auto a=std::size_t(alignment);void* p=nullptr;
#if defined(_MSC_VER)
 p=_aligned_malloc(n?n:1,a);
#else
 p=std::aligned_alloc(a,((n?n:1)+a-1)/a*a);
#endif
 if(p)return p;throw std::bad_alloc();}
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
#define CHECK(x) do{++checks;if(!(x)){++failures;std::cerr<<"FAIL "<<__LINE__<<" " #x "\n";}}while(false)
namespace {
using namespace airack::space;
using namespace airack::space::detail;
namespace probe=airack::space::probe;
void word(std::uint64_t& hash,std::uint64_t value,int bytes){for(int i=0;i<bytes;++i){hash^=(value>>(8*i))&255;hash*=1099511628211ULL;}}
void number(std::uint64_t& hash,double value){word(hash,std::bit_cast<std::uint64_t>(value),8);}
std::uint64_t fingerprint(const PreparedTank& p)
{
    std::uint64_t hash=14695981039346656037ULL;
    for(const auto& line:p.lines){number(hash,double(line.length)/p.sampleRate);number(hash,line.scalar);word(hash,line.count,4);
        for(std::size_t i=0;i<line.count;++i){const auto& c=line.coefficients[i];for(double x:{c.b0,c.b1,c.b2,c.a1,c.a2})number(hash,x);}
    }return hash;
}
void validation()
{
    const double nan=std::numeric_limits<double>::quiet_NaN();DecayCurve c;PreparedTank p;CHECK(prepareTank(c,.5,48000,p));const auto saved=p;
    for(double size:{-1.,2.,nan}){CHECK(!prepareTank(c,size,48000,p));CHECK(p==saved);}
    for(double rate:{0.,7999.,768001.,nan}){CHECK(!prepareTank(c,.5,rate,p));CHECK(p==saved);}
    auto bad=c;bad.bands[5].q=0;CHECK(!prepareTank(bad,.5,48000,p));CHECK(p==saved);
    TankRuntime empty;CHECK((empty.tick({1,1})==std::array<double,2>{}));CHECK(!empty.configure(saved));empty.reset();
    CHECK(!empty.prepareStorage(0));CHECK(!empty.prepareStorage(48000,static_cast<RenderBackend>(99)));
    allocations=frees=0;tracking=true;auto* known=::operator new(32,std::align_val_t(32));::operator delete(known,std::align_val_t(32));tracking=false;CHECK(allocations==1&&frees==1);
}
void characterized()
{
    const auto cases=probe::corpus();
    for(const auto& golden:test::tankGolden){
        const auto& curve=cases[std::size_t(golden.curve)].curve;PreparedTank p;TankPreparationReport report;
        CHECK(prepareTank(curve,golden.size,golden.rate,p,&report));CHECK(report.failure==TankPreparationFailure::none);
        CHECK(p.admission.passed&&p.admission.timeBound==46&&p.admission.maxLoopMagnitude<1);
        CHECK(p.maximumFitError<=.1&&p.stageCount<=38);CHECK(p.curve==curve&&p.size==golden.size&&p.sampleRate==golden.rate);
        CHECK(fingerprint(p)==golden.fingerprint);
        probe::SpaceTank reference;CHECK(reference.prepare(golden.rate,512,curve,golden.size,probe::FitGeometry::compact,true,true,probe::TankGeometry::eight,probe::FitDomain::poleContour));
        TankRuntime scalar,sse,automatic;
        CHECK(scalar.prepareStorage(golden.rate,RenderBackend::scalar));CHECK(sse.prepareStorage(golden.rate,RenderBackend::sse2));CHECK(automatic.prepareStorage(golden.rate));
        allocations=frees=0;tracking=true;const bool a=scalar.configure(p),b=sse.configure(p),c=automatic.configure(p);tracking=false;
        CHECK(a&&b&&c);CHECK(allocations==0&&frees==0);CHECK(scalar.prepared()==p);
        CHECK(std::string_view(scalar.backendName())=="scalar");CHECK(std::string_view(sse.backendName())=="sse2");
        CHECK(std::string_view(automatic.backendName())==(probe::availableAvx2Kernel()?"avx2":"sse2"));
        CHECK(automatic.storageBytes()<=16*1024*1024);CHECK(scalar.storageBytes()<=16*1024*1024);
        bool same=true,nonzero=false,finite=true;double maximum=0,error=0;unsigned random=0x5eedabcd;allocations=frees=0;tracking=true;
        for(int trial=0;trial<2;++trial){for(std::size_t n=0;n<100000;++n){random^=random<<13;random^=random>>17;random^=random<<5;
            const double x=n<10000?(double(random)/4294967296.-.5)*.001:0;const std::array<double,2> input{n==0?.5:x,n==0?.25:-.37*x};
            const auto expected=reference.tick(input),first=scalar.tick(input),second=sse.tick(input),third=automatic.tick(input);
            if(first!=expected||second!=expected||third!=expected)same=false;
            for(std::size_t ch=0;ch<2;++ch){if(third[ch]!=0)nonzero=true;if(!std::isfinite(third[ch]))finite=false;
                maximum=std::max(maximum,std::abs(third[ch]));error=std::max(error,std::abs(third[ch]-expected[ch]));}
        }reference.reset();scalar.reset();sse.reset();automatic.reset();}
        tracking=false;CHECK(same);CHECK(nonzero);CHECK(finite&&maximum<=4);CHECK(allocations==0&&frees==0);
        auto invalid=p;invalid.lines[0].count=39;CHECK(!scalar.configure(invalid));CHECK(scalar.prepared()==p);
        CHECK(!scalar.prepareStorage(0));CHECK(scalar.prepared()==p);
        scalar.reset();sse.reset();bool preserved=true;
        for(int n=0;n<1000;++n){const std::array<double,2> in{n==0?.5:0,n==0?.25:0};if(scalar.tick(in)!=sse.tick(in))preserved=false;}
        CHECK(!scalar.configure(invalid));
        for(int n=0;n<10000;++n)if(scalar.tick({0,0})!=sse.tick({0,0}))preserved=false;
        CHECK(preserved);
        std::cout<<"PRODUCT_TANK configuration="<<golden.configuration<<" backend="<<automatic.backendName()<<" exact="<<same<<" peak="<<maximum<<" max_error="<<error<<" bytes="<<automatic.storageBytes()<<'\n';
    }
}
void partitions()
{
    const auto c=probe::corpus()[35].curve;PreparedTank p;CHECK(prepareTank(c,.5,48000,p));TankRuntime a,b;CHECK(a.prepareStorage(48000));CHECK(b.prepareStorage(48000));CHECK(a.configure(p));CHECK(b.configure(p));
    constexpr std::size_t frames=8192;std::vector<std::array<double,2>> first(frames),second(frames);
    allocations=frees=0;tracking=true;
    for(std::size_t start=0;start<frames;start+=64)for(std::size_t n=start;n<std::min(start+64,frames);++n)first[n]=a.tick(n==0?std::array<double,2>{.5,.25}:std::array<double,2>{});
    for(std::size_t start=0;start<frames;start+=512)for(std::size_t n=start;n<std::min(start+512,frames);++n)second[n]=b.tick(n==0?std::array<double,2>{.5,.25}:std::array<double,2>{});
    a.reset();b.reset();tracking=false;CHECK(first==second);CHECK(allocations==0&&frees==0);
}
}
int main(){validation();characterized();partitions();std::cout<<"SpaceReverbProductTank checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

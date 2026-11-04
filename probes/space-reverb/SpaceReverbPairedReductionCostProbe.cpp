#include "ModalServiceProfile.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <new>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {thread_local bool tracking=false;thread_local bool failAllocation=false;thread_local std::size_t allocations=0,frees=0;unsigned checks=0,failures=0;}
void* operator new(std::size_t n){if(failAllocation){failAllocation=false;throw std::bad_alloc();}if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t a){if(failAllocation){failAllocation=false;throw std::bad_alloc();}if(tracking)++allocations;
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


namespace {
using namespace airack::space;using namespace airack::space::probe;
using Clock=std::chrono::steady_clock;using Frame=ModalService::Frame;
Settings family(unsigned n){Settings s;s.decay.decaySeconds=20;s.size=n?1:0;for(auto& b:s.decay.bands)b={true,false,DecayShape::lowShelf,20,n?4:.25,4};s.predelayMs=n?250:0;s.character=1;s.distance=.7;s.width=2;s.mix=.35;s.duckRangeDb=24;s.duckThresholdDb=-60;s.duckAttackMs=.1;s.duckReleaseMs=2000;
    constexpr std::array<double,6> centers{20,18000,40,16000,60,12000};for(unsigned j=0;j<6;++j)s.post[j]={true,false,j%2?PostShape::highCut:PostShape::lowCut,centers[j]*(n?1.05:1),0,1,1,CutSlope::db24};return s;}
struct Sample {double begin=0,kernel=0;bool ok=false,clean=false;ModalServiceStatus status;};
Sample call(ModalService& service,const ModalSlotSettings& target,const ModalSlotIdentities& ids,std::span<Frame> frames){
    Sample s;allocations=frees=0;tracking=true;const auto start=Clock::now();s.ok=service.beginParent(target,255,ids,frames.size());const auto admitted=Clock::now();
    for(unsigned i=0;i<8;++i)for(std::size_t at=0;at<frames.size();at+=512)s.ok=service.renderSlot(i,frames.subspan(at,std::min<std::size_t>(512,frames.size()-at)))&&s.ok;
    s.ok=service.endParent()&&s.ok;const auto finish=Clock::now();tracking=false;s.clean=allocations==0&&frees==0;
    s.begin=std::chrono::duration<double>(admitted-start).count();s.kernel=std::chrono::duration<double>(finish-start).count();s.ok=service.snapshot(s.status)&&s.ok;return s;
}
struct Series {std::vector<double> kernel,copy;unsigned post=0;};
void print(const Series& s,unsigned row,unsigned order,bool paired){
    auto k=s.kernel,c=s.copy;std::sort(k.begin(),k.end());std::sort(c.begin(),c.end());double total=0;for(auto v:k)total+=v;
    std::cout<<"PAIRED_REDUCTION_COST row="<<row<<" order="<<order<<" scatter_mode=blocked reduction_mode="<<(paired?"paired_stereo":"reference")<<" samples="<<k.size()<<" copies="<<c.size()<<" post="<<s.post<<" mean_seconds="<<total/k.size()<<" p99_seconds="<<k[(99*k.size()+99)/100-1]<<" copy_p99_seconds="<<c[(99*c.size()+99)/100-1]<<" max_seconds="<<k.back()<<'\n';
}
bool paired(unsigned row,unsigned order){
    const auto c=serviceCpuCase(row);if(!c)return false;ModalSlotIdentities ids{101,102,103,104,105,106,107,108};std::array<ModalSlotSettings,2> targets;
    for(unsigned f=0;f<2;++f)for(unsigned i=0;i<8;++i)targets[f][i]=family(f^(i%2));
    auto bundle=prepareModalBundle(c->rate,256,701,targets[0],ids,{},c->factor);if(!bundle)return false;
    std::array<ModalService,2> services;for(unsigned i=0;i<2;++i){ModalServiceOptions o;o.threaded=false;o.factor=c->factor;o.preparedCopies=true;o.blockedScatter=true;o.pairedReduction=i==1;if(!services[i].prepareStorage(c->rate,256,o)||!services[i].usesBlockedScatter()||!services[i].installBundle(*bundle,701)||services[i].usesPairedReduction()!=(i==1))return false;}
    std::array<Series,2> series;for(auto& s:series){s.kernel.reserve(512);s.copy.reserve(512);}
    std::array<std::array<Frame,1024>,2> work{};std::uint32_t random=0x5eeda123;const auto frames=c->block*c->factor;const auto flip=(c->rate+100*c->block-1)/(100*c->block);
    bool exact=true,clean=true,valid=true;for(unsigned n=0;n<576;++n){for(unsigned k=0;k<frames;++k)for(unsigned channel=0;channel<2;++channel){random^=random<<13;random^=random>>17;random^=random<<5;work[0][k][channel]=double(int(random&65535)-32768)/65536.;}work[1]=work[0];
        std::array<Sample,2> s;for(unsigned position=0;position<2;++position){const auto i=position^order;s[i]=call(services[i],targets[(n/flip)%2],ids,std::span(work[i]).first(frames));}
        exact=exact&&s[0].status==s[1].status&&std::memcmp(work[0].data(),work[1].data(),frames*sizeof(Frame))==0;
        for(unsigned i=0;i<2;++i){clean=clean&&s[i].clean;valid=valid&&s[i].ok;for(auto slot:s[i].status.slots)valid=valid&&slot.preparationFailures==0;for(auto frame:std::span(work[i]).first(frames))for(double v:frame)valid=valid&&std::isfinite(v);
            if(n>=64){series[i].kernel.push_back(s[i].kernel);if(s[i].status.admissionsThisParent){if(s[i].status.maximumRoomsThisParent==9)series[i].copy.push_back(s[i].begin);else ++series[i].post;}}}
        for(unsigned k=0;k<32;++k){const auto a=services[0].pumpHeavy(),b=services[1].pumpHeavy();valid=valid&&a==b;if(!a&&!b)break;if(k==31)valid=false;}
        for(unsigned k=0;k<64;++k){const auto a=services[0].pumpLight(),b=services[1].pumpLight();valid=valid&&a==b;if(!a&&!b)break;if(k==63)valid=false;}
    }
    for(unsigned i=0;i<2;++i){if(series[i].copy.empty()||!series[i].post)return false;print(series[i],row,order,services[i].usesPairedReduction());}
    std::cout<<"PAIRED_REDUCTION_COST_PAIR row="<<row<<" order="<<order<<" exact="<<exact<<" heap="<<clean<<" valid="<<valid<<" caller=ordinary_unpaced workers=manual_off_audio acceptance=0\n";return exact&&clean&&valid;
}
}
int main(){std::cout<<std::setprecision(17);bool ok=true;for(unsigned order=0;order<2;++order)for(unsigned row:{4U,5U,10U,11U})ok=paired(row,order)&&ok;
    std::cout<<"PAIRED_REDUCTION_COST_COMPLETE exact_and_contracts="<<ok<<" capacity_acceptance=0 cache_and_caller_limitations=1\n";return ok?0:1;}

#include "SpaceReverbTank.h"
#include "SpaceReverbPacked.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace {thread_local bool tracking=false;thread_local std::size_t allocations=0,frees=0;}
void* operator new(std::size_t n){if(tracking)++allocations;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept {if(tracking&&p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
void* operator new(std::size_t n,std::align_val_t align){if(tracking)++allocations;const auto a=std::size_t(align);
#if defined(_MSC_VER)
    auto* p=_aligned_malloc(n?n:1,a);
#else
    auto* p=std::aligned_alloc(a,((n?n:1)+a-1)/a*a);
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
using namespace airack::space;
using namespace airack::space::detail;
namespace {
using Frame=TankRuntime::Frame;
bool exact(Frame a,Frame b){return std::bit_cast<std::array<std::uint64_t,2>>(a)==std::bit_cast<std::array<std::uint64_t,2>>(b);}
}
int main(){
    int checks=0,failures=0;const auto check=[&](bool value,const char* label){++checks;if(!value){++failures;std::cout<<"FAIL "<<label<<'\n';std::cerr<<"ENDPOINT_FAIL block "<<label<<std::endl;}};
    const auto packedAlignment=[&]<std::size_t N>(){
        check(alignof(PackedStage<N>)>=64,"packed_cache_line_guarantee");
        check(sizeof(PackedStage<N>)==7*N*sizeof(double),"packed_no_size_increase");
        std::vector<PackedStage<N>> stages(38);
        bool aligned=true;
        for(auto& stage:stages)for(const auto* row:{stage.b0.data(),stage.b1.data(),stage.b2.data(),stage.a1.data(),stage.a2.data(),stage.z1.data(),stage.z2.data()})
            aligned=aligned&&reinterpret_cast<std::uintptr_t>(row)%64==0;
        check(aligned,"packed_heap_rows_aligned");
    };
    packedAlignment.template operator()<8>();packedAlignment.template operator()<16>();
    std::array<Frame,4096> input;unsigned random=0x5eeda123;
    for(auto& frame:input)for(auto& value:frame){random^=random<<13;random^=random>>17;random^=random<<5;value=(double(random)/4294967296.-.5)*.001;}
    tracking=true;auto* allocationProbe=::operator new(32,std::align_val_t(32));::operator delete(allocationProbe,std::align_val_t(32));tracking=false;
    check(allocations==1&&frees==1,"counter_sanity");
    for(double rate:{8000.,44100.,48000.,96000.,768000.}){
        DecayCurve curve;curve.bands[0]={true,false,DecayShape::bell,1000,.5,2};PreparedTank prepared;
        check(prepareTank(curve,.5,rate,prepared),"prepared");
        for(const auto backend:{RenderBackend::scalar,RenderBackend::sse2,RenderBackend::automatic}){
            TankRuntime reference,whole,partition;
            check(reference.prepareStorage(rate,backend)&&whole.prepareStorage(rate,backend)&&partition.prepareStorage(rate,backend),"storage");
            for(int pattern=0;pattern<3;++pattern){
                check(reference.configure(prepared)&&whole.configure(prepared)&&partition.configure(prepared),"configured");
                // Make long physical histories audible before checking block/state
                // equivalence; an all-zero high-rate prefix is not a useful oracle.
                for(std::size_t n=0;n<std::size_t(std::ceil(.2*rate));++n){const auto x=input[n%input.size()];reference.tick(x,1);whole.tick(x,1);partition.tick(x,1);}
                std::array<double,512> characters;for(std::size_t n=0;n<512;++n)characters[n]=pattern==1?1:double(n*37%512)/511.;
                const auto chars=pattern==0?std::span<const double>{}:std::span<const double>(characters);
                bool wholeOk=true,partitionOk=true,audioWhole=true,audioPartition=true,finite=true;
                allocations=frees=0;tracking=true;
                for(std::size_t block=0;block<9;++block){
                    std::array<Frame,512> a,b,expected;
                    for(std::size_t n=0;n<512;++n){const auto x=input[(block*512+n)%input.size()];a[n]=b[n]=x;expected[n]=reference.tick(x,chars.empty()?0:chars[n]);}
                    wholeOk=whole.processBlock(a,chars)&&wholeOk;
                    for(std::size_t n=0;n<512;n+=64)partitionOk=partition.processBlock(std::span(b).subspan(n,64),chars.empty()?chars:chars.subspan(n,64))&&partitionOk;
                    for(std::size_t n=0;n<512;++n){audioWhole=audioWhole&&exact(a[n],expected[n]);audioPartition=audioPartition&&exact(b[n],expected[n]);for(double v:a[n])finite=finite&&std::isfinite(v)&&std::abs(v)<4;}
                }
                bool continuation=true;for(std::size_t n=0;n<32;++n){const auto x=input[n];const auto expected=reference.tick(x,.37);continuation=continuation&&exact(whole.tick(x,.37),expected)&&exact(partition.tick(x,.37),expected);}
                tracking=false;
                check(wholeOk,"whole_accepted");check(partitionOk,"partitions_accepted");check(audioWhole,"whole_exact_tick");check(audioPartition,"partitions_exact_tick");check(finite,"raw_finite");check(continuation,"continuation");check(allocations==0&&frees==0,"no_callback_allocation_free");
            }
        }
    }
    PreparedTank prepared;check(prepareTank(DecayCurve{},.5,48000,prepared),"invalid_fixture");
    TankRuntime tank,control,unready;check(tank.prepareStorage(48000)&&control.prepareStorage(48000)&&tank.configure(prepared)&&control.configure(prepared),"invalid_storage");
    std::array<Frame,513> frames;frames.fill({.1,-.2});const auto original=frames;std::array<double,3> badCharacters{};
    check(!unready.processBlock(std::span(frames).first(64)),"unready_rejected");check(frames==original,"unready_output_preserved");
    check(!tank.processBlock(frames),"oversize_rejected");check(frames==original,"oversize_output_preserved");
    check(!tank.processBlock(std::span(frames).first(64),badCharacters),"mismatch_rejected");check(frames==original,"mismatch_output_preserved");
    check(tank.processBlock({}),"empty_accepted");check(exact(tank.tick({.1,.2},1),control.tick({.1,.2},1)),"invalid_state_preserved");
    std::cout<<"SpaceReverbBlock checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
}

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
struct Fixture {ServiceCpuHeader header;std::vector<ServiceCpuRecord> records;};
Fixture fixture(unsigned row){Fixture f;auto& h=f.header;const unsigned index=row%12;h.row=row;h.rate=std::array<unsigned,3>{44100,48000,96000}[(index/2)%3];h.block=index%2?512:64;h.factor=index/6+1;h.scenario=row/12;h.ticksPerSecond=1000000000;h.warmExpected=(10ULL*h.rate+h.block-1)/h.block;h.measureExpected=(20ULL*h.rate+h.block-1)/h.block;h.warmCompleted=h.warmExpected;h.recordCount=h.measureExpected;h.warmStart=1000000000;h.warmEnd=h.warmStart+(h.warmExpected+1)*h.ticksPerSecond*h.block/h.rate;h.measureAnchor=h.warmEnd+1000000;h.measureEnd=h.measureAnchor+(h.measureExpected+1)*h.ticksPerSecond*h.block/h.rate;h.productBytes=32*1024*1024;f.records.resize(std::size_t(h.recordCount));const auto period=h.ticksPerSecond*h.block/h.rate;const auto coldInterval=(h.rate+h.block*4-1)/(h.block*4);
    for(std::size_t i=0;i<f.records.size();++i){auto& r=f.records[i];r.release=h.measureAnchor+(i+1)*h.ticksPerSecond*h.block/h.rate;r.start=r.release+period/100;r.beginFinish=r.start+period/20;r.finish=r.start+period/4;r.cycles=1234;r.valid=1;r.rawPeakBits=std::bit_cast<std::uint64_t>(.0001);r.checksumBits=std::bit_cast<std::uint64_t>(double(i)/1024.);
        if(h.scenario){r.coldInstall=(h.warmExpected+i)%coldInterval==0;r.maximumRooms=8;}else {r.roomCopy=i%16==0;r.postAdmission=i%16==8;r.maximumRooms=i%16<8?9:8;}}
    return f;
}
void schemaAndGates(){CHECK(sizeof(ServiceCpuHeader)==128&&sizeof(ServiceCpuRecord)==112);CHECK(!serviceCpuCase(24));CHECK(!serviceCpuCase(UINT32_MAX));for(unsigned row=0;row<24;++row){const auto c=serviceCpuCase(row);CHECK(bool(c));const auto f=fixture(row);CHECK(c&&c->rate==f.header.rate&&c->block==f.header.block&&c->factor==f.header.factor&&c->scenario==f.header.scenario&&c->row==row);const auto a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&a.complete&&a.processingPass&&!a.knownFailure&&a.workloadComplete);CHECK(a.overruns==0&&a.dispatchMisses==0);CHECK(a.meanSeconds>0&&a.p99Seconds>0&&a.maximumSeconds>0);CHECK(row<12?a.roomCopies>=16&&a.postAdmissions>=16&&a.nineRoomCallbacks>=f.records.size()/4:a.coldInstalls>=16);}
    auto f=fixture(0);const auto original=f;const auto period=f.header.ticksPerSecond*f.header.block/f.header.rate;
    f.records[10].finish=f.records[10].start+2*period;auto a=assessServiceCpu(f.header,f.records);CHECK(a.knownFailure&&!a.processingPass);f=original;f.records[10].finish=f.records[10].start-1;a=assessServiceCpu(f.header,f.records);CHECK(!a.schemaValid&&!a.processingPass);
    f=original;for(std::size_t i=0;i<f.records.size()/50;++i)f.records[i].finish=f.records[i].start+period*3/5;a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&a.overruns==0&&a.knownFailure&&a.p99Seconds>.5*double(f.header.block)/f.header.rate);
    f=original;for(std::size_t i=0;i<f.records.size();++i)f.records[i].finish=f.records[i].start+(i<f.records.size()/100?period*9/10:period*499/1000);a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&a.overruns==0&&a.p99Seconds<=.5*double(f.header.block)/f.header.rate&&a.meanSeconds>.5*double(f.header.block)/f.header.rate&&a.knownFailure);
    f=original;for(auto& r:f.records)if(r.roomCopy)r.beginFinish=r.start+period*11/100;a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&a.copyP99Seconds>.1*double(f.header.block)/f.header.rate&&a.knownFailure);
    f=original;std::uint64_t previous=0;for(std::size_t i=0;i<f.records.size();++i){auto& r=f.records[i];const auto duration=r.finish-r.start,begin=r.beginFinish-r.start;const auto target=r.release+period/100+(i==10?2*period:0);r.start=std::max(target,previous);r.beginFinish=r.start+begin;r.finish=r.start+duration;previous=r.finish;}a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&a.processingPass&&!a.knownFailure&&a.dispatchMisses>0&&a.startLatenessP99Seconds>=0);
    f=fixture(12);const auto coldPeriod=f.header.ticksPerSecond*f.header.block/f.header.rate;for(auto& r:f.records)if(r.coldInstall)r.finish=r.start+coldPeriod*3/5;a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&a.overruns==0&&a.p99Seconds<=.5*double(f.header.block)/f.header.rate&&a.coldP99Seconds>.5*double(f.header.block)/f.header.rate&&a.knownFailure);
    f=original;f.records[0].allocations=1;f.header.callbackAllocations=1;a=assessServiceCpu(f.header,f.records);CHECK(a.knownFailure&&!a.processingPass);f=original;f.header.callbackFrees=1;a=assessServiceCpu(f.header,f.records);CHECK(a.knownFailure);
    f=original;f.records[0].rawPeakBits=std::bit_cast<std::uint64_t>(double(INFINITY));a=assessServiceCpu(f.header,f.records);CHECK(!a.schemaValid&&!a.processingPass);f=original;f.records[0].valid=0;a=assessServiceCpu(f.header,f.records);CHECK(a.knownFailure);f=original;f.records[0].maximumRooms=10;CHECK(!assessServiceCpu(f.header,f.records).schemaValid);
    f=original;for(auto& r:f.records){r.roomCopy=0;r.postAdmission=0;r.maximumRooms=8;}a=assessServiceCpu(f.header,f.records);CHECK(a.complete&&!a.workloadComplete&&!a.processingPass&&a.knownFailure);
    f=original;f.records.resize(1);f.header.recordCount=1;f.header.measureEnd=f.records[0].finish;a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&!a.complete&&!a.processingPass&&!a.knownFailure);f.records[0].finish=f.records[0].start+2*period;f.header.measureEnd=f.records[0].finish;a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&!a.complete&&a.knownFailure&&a.overruns==1);
    f=fixture(2);f.records.resize(1);f.header.recordCount=1;const auto integerPeriod=f.header.ticksPerSecond*f.header.block/f.header.rate;f.records[0].finish=f.records[0].start+integerPeriod;f.header.measureEnd=f.records[0].finish;a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&a.overruns==0);f.records[0].finish++;f.header.measureEnd++;a=assessServiceCpu(f.header,f.records);CHECK(a.schemaValid&&a.overruns==1);
    f=original;f.header.version=2;CHECK(!assessServiceCpu(f.header,f.records).schemaValid);f=original;f.header.recordCount++;CHECK(!assessServiceCpu(f.header,f.records).schemaValid);f=original;f.header.productBytes=8*16*1024*1024+1;CHECK(assessServiceCpu(f.header,f.records).knownFailure);f=original;f.records[1].release++;CHECK(!assessServiceCpu(f.header,f.records).schemaValid);
    f=original;f.header.measureEnd=UINT64_MAX;CHECK(!assessServiceCpu(f.header,f.records).schemaValid);
    std::cout<<"MODAL_PROFILE_MODEL rows=24 scheduled_lateness_separate=1 rational_boundary=1 known_partial_failure=1\n";
}
void workload(){ServiceCpuWorkload empty;CHECK(!empty.prepare(24,false));CHECK(!empty.begin(0));CHECK(!empty.renderAndEnd());CHECK(!empty.observe().valid);for(unsigned row:{0U,6U,12U,18U}){ServiceCpuWorkload a,b;CHECK(a.prepare(row,false));CHECK(b.prepare(row,false));const auto* ready=a.prepared(0);const auto* other=a.prepared(1);CHECK(ready&&other);if(!ready||!other)continue;CHECK(ready->factor()==(row%12)/6+1);for(unsigned bank=0;bank<2;++bank)for(unsigned slot=0;slot<8;++slot){const auto& p=a.prepared(bank)->slot(slot);CHECK(p.room.modal.count==1024&&p.wet.post.coefficients.count==12);CHECK(p.settings.character==1&&p.settings.width==2&&p.settings.duckRangeDb==24);CHECK(p.final.duck.coefficients.sampleRate==ready->rate()*ready->factor());}CHECK(a.storageBytes()>0&&a.storageBytes()<=8*16*1024*1024);bool exact=true,clean=true;unsigned admissions=0;for(unsigned n=0;n<32;++n){const std::uint64_t ordinal=row>=12?std::uint64_t(n)*173:n*7;allocations=frees=0;tracking=true;const bool ok=a.begin(ordinal)&&a.renderAndEnd()&&b.begin(ordinal)&&b.renderAndEnd();const auto one=a.observe(),two=b.observe();tracking=false;clean=clean&&allocations==0&&frees==0;exact=exact&&ok&&one.valid&&two.valid&&one.peak==two.peak&&one.checksum==two.checksum&&one.status==two.status;admissions+=unsigned(one.roomCopy+one.postAdmission+one.coldInstall);a.pump();b.pump();}CHECK(exact&&clean);CHECK(admissions>0);std::cout<<"MODAL_PROFILE_WORKLOAD row="<<row<<" parents=32 admissions="<<admissions<<" bytes="<<a.storageBytes()<<'\n';}}
}
int main(){schemaAndGates();workload();std::cout<<"SpaceReverbModalServiceProfile checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

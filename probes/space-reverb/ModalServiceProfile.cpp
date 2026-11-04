#include "ModalServiceProfile.h"
#include "PeriodicCaller.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>
namespace airack::space::probe {
std::optional<ServiceCpuCase> serviceCpuCase(unsigned row) noexcept {if(row>=24)return {};const unsigned i=row%12;return ServiceCpuCase{row,std::array<std::uint32_t,3>{44100,48000,96000}[(i/2)%3],i%2?512U:64U,i/6+1,row/12};}
namespace {
std::uint64_t p99(std::vector<std::uint64_t> x){if(x.empty())return 0;std::sort(x.begin(),x.end());return x[(99*x.size()+99)/100-1];}
ServiceCpuAssessment invalidAssessment(){ServiceCpuAssessment a;a.knownFailure=true;return a;}
}
ServiceCpuAssessment assessServiceCpu(const ServiceCpuHeader& h,std::span<const ServiceCpuRecord> records){
    const auto c=serviceCpuCase(h.row);if(h.magic!=serviceCpuMagic||h.version!=1||!c||h.rate!=c->rate||h.block!=c->block||h.factor!=c->factor||h.scenario!=c->scenario||h.ticksPerSecond==0||h.ticksPerSecond>1000000000||h.warmExpected!=(10ULL*h.rate+h.block-1)/h.block||h.measureExpected!=(20ULL*h.rate+h.block-1)/h.block||h.recordCount!=records.size()||h.recordCount>h.measureExpected||h.warmCompleted>h.warmExpected||h.warmEnd<h.warmStart||h.measureEnd<h.measureAnchor||h.measureAnchor>INT64_MAX||h.warmStart>INT64_MAX||h.warmEnd>INT64_MAX||h.measureEnd>INT64_MAX)return invalidAssessment();
    if(!records.empty()&&(h.warmCompleted!=h.warmExpected||h.measureAnchor<h.warmEnd))return invalidAssessment();
    ReleaseSequence sequence;if(!sequence.initialize(std::int64_t(h.measureAnchor),h.ticksPerSecond,h.rate,h.block))return invalidAssessment();
    ServiceCpuAssessment a;std::vector<std::uint64_t> durations,copies,colds,startLateness,releaseFinish;durations.reserve(records.size());copies.reserve(records.size());colds.reserve(records.size());startLateness.reserve(records.size());releaseFinish.reserve(records.size());std::uint64_t sum=0,allocationSum=0,freeSum=0,previous=0;const auto period=h.ticksPerSecond*h.block/h.rate,half=h.ticksPerSecond*h.block/(2*h.rate),tenth=h.ticksPerSecond*h.block/(10*h.rate);const auto coldEvery=(h.rate+4*h.block-1)/(4*h.block);
    for(std::size_t n=0;n<records.size();++n){const auto& r=records[n];const double peak=std::bit_cast<double>(r.rawPeakBits),checksum=std::bit_cast<double>(r.checksumBits);
        if(r.release>INT64_MAX||r.start>INT64_MAX||r.finish>INT64_MAX||r.beginFinish<r.start||r.beginFinish>r.finish||r.start<previous||r.finish>h.measureEnd||r.roomCopy>1||r.postAdmission>1||r.coldInstall>1||r.roomCopy+r.postAdmission>1||r.valid>1||(r.maximumRooms!=8&&r.maximumRooms!=9)||(r.roomCopy&&r.maximumRooms!=9)||(r.postAdmission&&r.maximumRooms!=8)||!std::isfinite(peak)||peak<0||!std::isfinite(checksum))return invalidAssessment();
        if(h.scenario?(r.coldInstall!=std::uint64_t((h.warmExpected+n)%coldEvery==0)||r.roomCopy||r.postAdmission||r.maximumRooms!=8):r.coldInstall!=0)return invalidAssessment();
        const auto schedule=classifySchedule(sequence,n,{std::int64_t(r.release),std::int64_t(r.start),std::int64_t(r.finish)});if(schedule==ScheduleDisposition::invalid)return invalidAssessment();if(schedule==ScheduleDisposition::late)++a.dispatchMisses;
        const auto duration=r.finish-r.start;sum+=duration;previous=r.finish;durations.push_back(duration);if(duration>period)++a.overruns;if(r.roomCopy){++a.roomCopies;copies.push_back(r.beginFinish-r.start);}if(r.postAdmission)++a.postAdmissions;if(r.coldInstall){++a.coldInstalls;colds.push_back(duration);}if(r.maximumRooms==9)++a.nineRoomCallbacks;
        if(r.allocations>UINT64_MAX-allocationSum||r.frees>UINT64_MAX-freeSum)return invalidAssessment();allocationSum+=r.allocations;freeSum+=r.frees;if(!r.valid||r.allocations||r.frees)a.knownFailure=true;startLateness.push_back(r.start-r.release);releaseFinish.push_back(r.finish-r.release);
    }
    if(allocationSum>h.callbackAllocations||freeSum>h.callbackFrees)return invalidAssessment();a.schemaValid=true;a.complete=h.warmCompleted==h.warmExpected&&h.recordCount==h.measureExpected&&h.warmEnd-h.warmStart>=10*h.ticksPerSecond&&h.measureEnd-h.measureAnchor>=20*h.ticksPerSecond;
    const auto q=p99(durations),copy=p99(copies),cold=p99(colds);const double ticks=double(h.ticksPerSecond);if(!durations.empty()){a.meanSeconds=double(sum)/(durations.size()*ticks);a.p99Seconds=q/ticks;a.maximumSeconds=*std::max_element(durations.begin(),durations.end())/ticks;}a.copyP99Seconds=copy/ticks;a.coldP99Seconds=cold/ticks;a.startLatenessP99Seconds=p99(startLateness)/ticks;a.releaseFinishP99Seconds=p99(releaseFinish)/ticks;
    const auto meanLimit=std::uint64_t(records.size())*h.ticksPerSecond*h.block/(2*h.rate);if(sum>meanLimit||q>half||copy>tenth||cold>half||a.overruns||h.callbackAllocations||h.callbackFrees||h.productBytes==0||h.productBytes>8*16*1024*1024)a.knownFailure=true;
    a.workloadComplete=h.scenario?a.coldInstalls>=16:a.roomCopies>=16&&a.postAdmissions>=16&&a.nineRoomCallbacks*4>=records.size();if(a.complete&&!a.workloadComplete)a.knownFailure=true;a.processingPass=a.schemaValid&&a.complete&&a.workloadComplete&&!a.knownFailure;return a;
}
namespace {
Settings settings(unsigned family){Settings s;s.decay.decaySeconds=20;s.size=family?1:0;for(auto& b:s.decay.bands)b={true,false,DecayShape::lowShelf,20,family?4:.25,4};s.predelayMs=family?250:0;s.character=1;s.distance=.7;s.width=2;s.mix=.35;s.duckRangeDb=24;s.duckThresholdDb=-60;s.duckAttackMs=.1;s.duckReleaseMs=2000;
    constexpr std::array<double,6> centers{20,18000,40,16000,60,12000};for(unsigned j=0;j<6;++j)s.post[j]={true,false,j%2?PostShape::highCut:PostShape::lowCut,centers[j]*(family?1.05:1),0,1,1,CutSlope::db24};return s;}
}
struct ServiceCpuWorkload::State {
    ServiceCpuCase c;ModalService service;std::array<std::unique_ptr<const ModalReadyBundle>,2> bundles;std::array<ModalSlotSettings,2> targets;std::array<ModalSlotIdentities,2> identities;
    std::array<ModalRoom::Frame,4096> input{};std::array<ModalRoom::Frame,1024> work{};std::size_t offset=0;unsigned current=0;bool open=false,valid=true,completed=false,installed=false;double peak=0,checksum=0;
};
ServiceCpuWorkload::ServiceCpuWorkload()=default;ServiceCpuWorkload::~ServiceCpuWorkload()=default;
bool ServiceCpuWorkload::prepare(unsigned row,bool threaded){return prepare(row,threaded,1024);}
bool ServiceCpuWorkload::prepare(unsigned row,bool threaded,std::size_t modes,ModalConverterBackend converter){if(modes!=128&&modes!=192&&modes!=256&&modes!=512&&modes!=1024)return false;const auto c=serviceCpuCase(row);if(!c)return false;try {auto s=std::make_unique<State>();s->c=*c;for(unsigned bank=0;bank<2;++bank){for(unsigned i=0;i<8;++i){s->targets[bank][i]=settings(bank^(i%2));s->identities[bank][i]=100*(bank+1)+i+1;}s->bundles[bank]=prepareModalBundle(c->rate,modes,11+bank,s->targets[bank],s->identities[bank],{},c->factor);if(!s->bundles[bank])return false;}
        ModalServiceOptions o;o.threaded=threaded;o.factor=c->factor;o.converterBackend=converter;if(!s->service.prepareStorage(c->rate,modes,o)||!s->service.installBundle(*s->bundles[0],11))return false;
        std::uint32_t random=0x5eeda123;for(auto& frame:s->input)for(auto& v:frame){random^=random<<13;random^=random>>17;random^=random<<5;v=double(int(random&65535)-32768)/65536.;}state_=std::move(s);return true;}catch(...){return false;}}
bool ServiceCpuWorkload::begin(std::uint64_t ordinal) noexcept {if(!state_||state_->open||!state_->valid)return false;auto& s=*state_;s.installed=false;s.completed=false;
    if(s.c.scenario){const auto interval=(s.c.rate+4*s.c.block-1)/(4*s.c.block);if(ordinal%interval==0){s.current=1-s.current;s.installed=true;s.valid=s.service.installBundle(*s.bundles[s.current],11+s.current);}}else {const auto interval=(s.c.rate+100*s.c.block-1)/(100*s.c.block);s.current=unsigned((ordinal/interval)%2);}
    const auto& identity=s.identities[s.c.scenario?s.current:0];s.valid=s.valid&&s.service.beginParent(s.targets[s.current],255,identity,s.c.block*s.c.factor);s.open=s.valid;return s.valid;}
bool ServiceCpuWorkload::renderAndEnd() noexcept {if(!state_||!state_->open)return false;auto& s=*state_;const auto count=std::size_t(s.c.block*s.c.factor);auto block=std::span(s.work).first(count);for(std::size_t n=0;n<count;++n)block[n]=s.input[(s.offset+n)%s.input.size()];s.offset=(s.offset+count)%s.input.size();
    for(unsigned i=0;i<8;++i)for(std::size_t at=0;at<count;at+=512)s.valid=s.service.renderSlot(i,block.subspan(at,std::min<std::size_t>(512,count-at)))&&s.valid;s.valid=s.service.endParent()&&s.valid;s.peak=0;for(auto frame:block)for(double v:frame){s.valid=s.valid&&std::isfinite(v);s.peak=std::max(s.peak,std::abs(v));s.checksum+=v;}s.valid=s.valid&&std::isfinite(s.checksum);s.open=false;s.completed=true;return s.valid;}
ServiceCpuObservation ServiceCpuWorkload::observe() const noexcept {ServiceCpuObservation out;if(!state_||!state_->completed||state_->open)return out;const auto& s=*state_;out.valid=s.valid&&s.service.snapshot(out.status);out.coldInstall=s.installed;out.peak=s.peak;out.checksum=s.checksum;out.maximumRooms=out.status.maximumRoomsThisParent;
    if(out.status.admissionsThisParent){out.roomCopy=out.maximumRooms==9;out.postAdmission=out.maximumRooms==8;}for(const auto& slot:out.status.slots)out.valid=out.valid&&slot.preparationFailures==0;return out;}
bool ServiceCpuWorkload::pump() noexcept {if(!state_||state_->open)return false;bool any=false;for(unsigned n=0;n<32&&state_->service.pumpHeavy();++n)any=true;for(unsigned n=0;n<64&&state_->service.pumpLight();++n)any=true;return any;}
const ModalReadyBundle* ServiceCpuWorkload::prepared(unsigned bank) const noexcept {return state_&&bank<2?state_->bundles[bank].get():nullptr;}
bool ServiceCpuWorkload::usesPairedReduction() const noexcept {return state_&&state_->service.usesPairedReduction();}
std::optional<ModalConverterBackend> ServiceCpuWorkload::converterBackend() const noexcept {return state_?state_->service.converterBackend():std::nullopt;}
bool ServiceCpuWorkload::usesPreparedCopies() const noexcept {return state_&&state_->service.usesPreparedCopies();}
bool ServiceCpuWorkload::usesBlockedScatter() const noexcept {return state_&&state_->service.usesBlockedScatter();}
std::size_t ServiceCpuWorkload::storageBytes() const noexcept {if(!state_)return sizeof(*this);const auto& s=*state_;return sizeof(*this)+sizeof(State)-sizeof(s.input)+s.service.storageBytes()-sizeof(s.service)+s.bundles[0]->storageBytes()+s.bundles[1]->storageBytes();}
}

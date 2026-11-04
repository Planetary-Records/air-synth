#include "ScopedBenchmarkAffinity.h"
#include "PeriodicCaller.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <string_view>

namespace {
using namespace airack::space::probe;
int checks=0,failures=0;
void check(bool value,const char* expression,int line){++checks;if(!value){++failures;std::cerr<<"FAIL "<<line<<' '<<expression<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
constexpr std::uint64_t bit(unsigned index){return std::uint64_t(1)<<index;}
std::array<BenchmarkCpuSet,24> topology(){std::array<BenchmarkCpuSet,24> result{};for(unsigned i=0;i<result.size();++i)result[i]={256+i,0,std::uint8_t(i),std::uint8_t(i),std::uint8_t(i<8?1:0),0};return result;}
struct Fake {
    BenchmarkGroupAffinity current{bit(24)-1,0};
    unsigned reads=0,writes=0,failRead=0,failWrite=0,wrongRead=0;
    bool mutateBeforeFailure=false;
    BenchmarkAffinityApi api(){return {this,
        [](void* raw,BenchmarkGroupAffinity& output,std::uint32_t& error) noexcept {
            auto& self=*static_cast<Fake*>(raw);++self.reads;
            if(self.reads==self.failRead){error=5;return false;}
            output=self.current;if(self.reads==self.wrongRead)output.mask^=bit(23);return true;
        },
        [](void* raw,const BenchmarkGroupAffinity& input,std::uint32_t& error) noexcept {
            auto& self=*static_cast<Fake*>(raw);++self.writes;
            if(self.writes==self.failWrite){if(self.mutateBeforeFailure)self.current=input;error=5;return false;}
            self.current=input;return true;
        }};}
};
void deterministicEligibleSelection(){
    auto cpus=topology();cpus[0].efficiency=255;cpus[1].flags=1;cpus[2].flags=2;cpus[3].flags=6;
    const auto initialMask=(bit(24)-1)&~bit(3);
    for(unsigned order=0;order<3;++order){
        Fake fake;fake.current.mask=initialMask;ScopedBenchmarkAffinity affinity;
        CHECK(affinity.open(fake.api(),cpus,1));CHECK(affinity.selected().index==4&&affinity.selected().efficiency==1);
        CHECK(affinity.during().mask==bit(4)&&affinity.before().mask==initialMask);
        CHECK(affinity.eligibleMask()==((bit(24)-1)&~(bit(4)-1)));
        CHECK(affinity.performanceMask()==(bit(4)|bit(5)|bit(6)|bit(7)));
        CHECK(affinity.close());CHECK(affinity.close());CHECK(affinity.qualified());CHECK(fake.current.mask==initialMask);
        CHECK(fake.writes==2&&fake.reads==3);std::reverse(cpus.begin(),cpus.end());if(order==1)std::rotate(cpus.begin(),cpus.begin()+7,cpus.end());
    }
    auto own=topology();own[1].flags=2;own[2].flags=6;Fake fake;ScopedBenchmarkAffinity affinity;
    CHECK(affinity.open(fake.api(),own,1));CHECK(affinity.selected().index==2);CHECK(affinity.close());
    Fake efficientOnly;efficientOnly.current.mask=(bit(24)-1)&~(bit(8)-1);ScopedBenchmarkAffinity efficient;
    CHECK(efficient.open(efficientOnly.api(),own,1));CHECK(efficient.selected().index==8&&efficient.selected().efficiency==0);CHECK(efficient.close());
}
void invalidInputsPreserveCaller(){
    auto cpus=topology();
    for(unsigned kind=0;kind<7;++kind){Fake fake;auto input=cpus;unsigned groups=1;
        if(kind==0)groups=2;
        if(kind==1)fake.current.group=1;
        if(kind==2)fake.current.mask=bit(0);
        if(kind==3)input[4].id=input[3].id;
        if(kind==4)input[4].index=input[3].index;
        if(kind==5)input[4].index=64;
        if(kind==6)input[4].group=1;
        const auto before=fake.current;ScopedBenchmarkAffinity affinity;CHECK(!affinity.open(fake.api(),input,groups));
        CHECK(affinity.setupError()!=0);CHECK(fake.current==before&&fake.writes==0);CHECK(affinity.close());CHECK(!affinity.qualified());
    }
    Fake fake;ScopedBenchmarkAffinity empty;CHECK(!empty.open(fake.api(),{},1));CHECK(fake.writes==0);
    ScopedBenchmarkAffinity missing;CHECK(!missing.open({},cpus,1));CHECK(missing.setupError()!=0);
}
void movableTimerSelection(){
    auto cpus=topology();
    for(unsigned order=0;order<2;++order){Fake fake;const auto before=fake.current;ScopedBenchmarkAffinity affinity;
        CHECK(affinity.open(fake.api(),cpus,1,BenchmarkPlacement::movableTimer));
        CHECK(affinity.during().mask==((bit(8)-1)&~(bit(2)-1)));CHECK(affinity.verifyCurrent());
        CHECK(affinity.close()&&affinity.qualified()&&fake.current==before);std::reverse(cpus.begin(),cpus.end());}
    auto siblings=topology();for(unsigned i=0;i<8;++i)siblings[i].core=std::uint8_t(i/2);
    Fake fake;ScopedBenchmarkAffinity affinity;CHECK(affinity.open(fake.api(),siblings,1,BenchmarkPlacement::movableTimer));
    CHECK(affinity.during().mask==(bit(0)|bit(2)|bit(4)|bit(6)));CHECK(affinity.close());
    for(unsigned count:{3u,4u,5u}){auto limited=topology();for(unsigned i=count;i<limited.size();++i)limited[i].efficiency=0;
        Fake f;const auto before=f.current;ScopedBenchmarkAffinity a;const bool opened=a.open(f.api(),limited,1,BenchmarkPlacement::movableTimer);
        CHECK(opened==(count>=4));if(opened)CHECK(a.during().mask==bit(count)-1);CHECK(a.close()&&f.current==before);}
    Fake restricted;restricted.current.mask=bit(3)|bit(4)|bit(5)|bit(6);const auto before=restricted.current;ScopedBenchmarkAffinity inherited;
    CHECK(inherited.open(restricted.api(),topology(),1,BenchmarkPlacement::movableTimer));CHECK(inherited.during()==before);CHECK(inherited.close());
    auto parked=topology();for(auto& cpu:parked)cpu.flags|=1;Fake waking;ScopedBenchmarkAffinity preferred;
    CHECK(preferred.open(waking.api(),parked,1,BenchmarkPlacement::movableTimer));CHECK(preferred.during().mask==((bit(8)-1)&~(bit(2)-1)));CHECK(preferred.close());
    Fake broken;broken.failWrite=1;broken.mutateBeforeFailure=true;const auto original=broken.current;ScopedBenchmarkAffinity rollback;
    CHECK(!rollback.open(broken.api(),topology(),1,BenchmarkPlacement::movableTimer));CHECK(broken.current==original);CHECK(rollback.close());
    Fake overridden;const auto originalMask=overridden.current;ScopedBenchmarkAffinity changed;
    CHECK(changed.open(overridden.api(),topology(),1,BenchmarkPlacement::movableTimer));overridden.current.mask=bit(9);
    CHECK(!changed.verifyCurrent());CHECK(changed.close()&&overridden.current==originalMask&&!changed.qualified());
    Fake failing;const auto initial=failing.current;ScopedBenchmarkAffinity cleanup;
    CHECK(cleanup.open(failing.api(),topology(),1,BenchmarkPlacement::movableTimer));failing.failWrite=2;
    CHECK(!cleanup.close());CHECK(cleanup.close()&&failing.current==initial&&!cleanup.qualified());
}
void reservedCallerSelection(){
    auto cpus=topology();cpus[8].core=0;cpus[9].core=1;
    for(unsigned order=0;order<2;++order){
        Fake fake;const auto before=fake.current;ScopedBenchmarkAffinity affinity;
        CHECK(affinity.open(fake.api(),cpus,1,BenchmarkPlacement::reservedCaller));
        CHECK(affinity.selected().index==2&&affinity.during().mask==bit(2));
        CHECK(affinity.eligibleMask()==((bit(24)-1)&~(bit(0)|bit(1)|bit(8)|bit(9))));
        CHECK(affinity.performanceMask()==((bit(8)-1)&~(bit(0)|bit(1))));
        CHECK(affinity.verifyCurrent()&&affinity.close()&&affinity.qualified()&&fake.current==before);
        std::reverse(cpus.begin(),cpus.end());
    }
    Fake restricted;restricted.current.mask=bit(1)|bit(8)|bit(9);const auto before=restricted.current;ScopedBenchmarkAffinity none;
    CHECK(!none.open(restricted.api(),cpus,1,BenchmarkPlacement::reservedCaller));
    CHECK(restricted.current==before&&restricted.writes==0&&none.close()&&!none.qualified());
    for(unsigned fail:{1u,2u}){Fake fake;fake.failWrite=fail;fake.mutateBeforeFailure=true;const auto initial=fake.current;ScopedBenchmarkAffinity affinity;
        const bool opened=affinity.open(fake.api(),cpus,1,BenchmarkPlacement::reservedCaller);
        CHECK(opened==(fail==2));if(opened)CHECK(!affinity.close());
        CHECK(affinity.close()&&fake.current==initial&&!affinity.qualified());
    }
}
void setupRollback(){
    const auto cpus=topology();
    for(unsigned kind=0;kind<5;++kind){Fake fake;
        if(kind==0)fake.failRead=1;
        if(kind==1)fake.failWrite=1;
        if(kind==2){fake.failWrite=1;fake.mutateBeforeFailure=true;}
        if(kind==3)fake.failRead=2;
        if(kind==4)fake.wrongRead=2;
        const auto before=fake.current;ScopedBenchmarkAffinity affinity;CHECK(!affinity.open(fake.api(),cpus,1));
        CHECK(affinity.setupError()!=0&&!affinity.opened()&&!affinity.qualified());CHECK(fake.current==before);
        CHECK(affinity.close());CHECK(affinity.close());if(kind)CHECK(affinity.restored()&&affinity.after()==before);
    }
}
void stableCallerSelection(){
    for(unsigned parked:{0u,1u,255u})for(unsigned reversed:{0u,1u}){
        auto cpus=topology();for(unsigned i=0;i<cpus.size();++i)if((parked>>i)&1)cpus[i].flags|=1;
        cpus[8].core=0;cpus[9].core=1;
        if(reversed)std::reverse(cpus.begin(),cpus.end());
        Fake fake;const auto before=fake.current;ScopedBenchmarkAffinity affinity;
        CHECK(affinity.open(fake.api(),cpus,1,BenchmarkPlacement::stableCaller));
        CHECK(affinity.selected().index==7&&affinity.during().mask==bit(7));
        CHECK(affinity.performanceMask()==((bit(8)-1)&~(bit(0)|bit(1))));
        CHECK(affinity.verifyCurrent()&&affinity.close()&&affinity.qualified()&&fake.current==before);
    }
    auto cpus=topology();cpus[7].core=0;cpus[6].flags=2;cpus[5].flags=7;
    Fake fake;fake.current.mask=bit(0)|bit(5)|bit(6)|bit(7);const auto before=fake.current;ScopedBenchmarkAffinity affinity;
    CHECK(affinity.open(fake.api(),cpus,1,BenchmarkPlacement::stableCaller));
    CHECK(affinity.selected().index==5&&affinity.during().mask==bit(5));
    CHECK(affinity.close()&&affinity.qualified()&&fake.current==before);
    for(unsigned failure:{1u,2u}){Fake f;f.failWrite=failure;f.mutateBeforeFailure=true;const auto old=f.current;ScopedBenchmarkAffinity a;
        const bool opened=a.open(f.api(),topology(),1,BenchmarkPlacement::stableCaller);CHECK(opened==(failure==2));
        if(opened)CHECK(!a.close());CHECK(a.close()&&f.current==old&&!a.qualified());}
}
void restorationFailureIsNeverQualified(){
    const auto cpus=topology();
    for(unsigned kind=0;kind<3;++kind){Fake fake;const auto before=fake.current;ScopedBenchmarkAffinity affinity;
        CHECK(affinity.open(fake.api(),cpus,1));
        if(kind==0)fake.failWrite=2;
        if(kind==1)fake.failRead=3;
        if(kind==2)fake.wrongRead=3;
        CHECK(!affinity.close());CHECK(affinity.cleanupError()!=0&&!affinity.qualified());
        CHECK(affinity.close());CHECK(affinity.after()==before&&fake.current==before&&affinity.restored());
        CHECK(!affinity.qualified());const auto writes=fake.writes;CHECK(affinity.close());CHECK(fake.writes==writes);
    }
}
void externalOverrideInvalidatesRun(){
    Fake fake;const auto before=fake.current;ScopedBenchmarkAffinity affinity;
    CHECK(affinity.open(fake.api(),topology(),1));CHECK(affinity.verifyCurrent());
    fake.current.mask=bit(9);CHECK(!affinity.verifyCurrent());CHECK(affinity.setupError()!=0);
    CHECK(affinity.close());CHECK(fake.current==before&&affinity.restored());CHECK(!affinity.qualified());
}
void scopedAndUntouched(){
    const auto cpus=topology();Fake fake;const auto before=fake.current;
    {ScopedBenchmarkAffinity affinity;CHECK(affinity.open(fake.api(),cpus,1));CHECK(fake.current.mask==bit(1));}
    CHECK(fake.current==before&&fake.writes==2);
    ScopedBenchmarkAffinity untouched;CHECK(untouched.close()&&untouched.close());
    CHECK(!untouched.attempted()&&!untouched.opened()&&!untouched.restored()&&!untouched.qualified());
    CHECK(!untouched.beforeOk()&&!untouched.duringOk()&&!untouched.afterOk());
    CHECK(untouched.groupCount()==0&&untouched.topology().empty()&&untouched.eligibleMask()==0&&untouched.performanceMask()==0);
    CHECK(untouched.setupError()==0&&untouched.cleanupError()==0&&untouched.topologyError()==0);
    Fake again;ScopedBenchmarkAffinity once;CHECK(once.open(again.api(),cpus,1));const auto writes=again.writes;
    CHECK(!once.open(again.api(),cpus,1));CHECK(again.writes==writes);CHECK(once.close());CHECK(!once.qualified());
}
void timerDoesNotShareCallerCore(){
    const std::array<BenchmarkCpuSet,7> cpus{{{1,0,2,1,2,0},{2,0,3,1,2,0},{3,0,4,2,2,0},{4,0,6,3,2,0},{5,0,8,4,2,0},{6,0,10,5,2,0},{7,0,12,6,2,0}}};
    const BenchmarkGroupAffinity available{bit(2)|bit(4)|bit(6)|bit(8)|bit(10)|bit(12),0};
    CHECK(timerAffinityAwayFromCaller(available,cpus,0,2).mask==(available.mask&~bit(2)));
    CHECK(timerAffinityAwayFromCaller(available,cpus,0,3).mask==(available.mask&~bit(2)));
    CHECK(timerAffinityAwayFromCaller(available,cpus,1,2)==available);
    CHECK(timerAffinityAwayFromCaller(available,cpus,0,63)==available);
    CHECK(timerAffinityAwayFromCaller(available,cpus,0,64)==available);
    const BenchmarkGroupAffinity four{bit(2)|bit(4)|bit(6)|bit(8),0};
    CHECK(timerAffinityAwayFromCaller(four,cpus,0,3)==four);
    const BenchmarkGroupAffinity five{four.mask|bit(10),0};
    const auto moved=timerAffinityAwayFromCaller(five,cpus,0,3);
    CHECK(moved.group==five.group&&moved.mask==(five.mask&~bit(2))&&std::popcount(moved.mask)==4);
}
void timerLocalHandoffSelection(){
    const BenchmarkGroupAffinity inherited{bit(0)|bit(2)|bit(23)|bit(63),0};
    CHECK((timerAffinityOnCaller(inherited,0,0)==BenchmarkGroupAffinity{bit(0),0}));
    CHECK((timerAffinityOnCaller(inherited,0,2)==BenchmarkGroupAffinity{bit(2),0}));
    CHECK((timerAffinityOnCaller(inherited,0,23)==BenchmarkGroupAffinity{bit(23),0}));
    CHECK((timerAffinityOnCaller(inherited,0,63)==BenchmarkGroupAffinity{bit(63),0}));
    CHECK(timerAffinityOnCaller(inherited,0,1)==BenchmarkGroupAffinity{});
    CHECK(timerAffinityOnCaller(inherited,1,2)==BenchmarkGroupAffinity{});
    CHECK(timerAffinityOnCaller(inherited,0,64)==BenchmarkGroupAffinity{});
    CHECK(timerAffinityOnCaller({},0,0)==BenchmarkGroupAffinity{});
}
void live(bool periodic=false,bool reserved=false,bool stable=false){
    PeriodicSession caller;BenchmarkGroupAffinity inherited{};std::uint32_t error=0;const auto api=nativeBenchmarkAffinityApi();
    CHECK(api.get(nullptr,inherited,error));
    if(periodic)CHECK(caller.open(nativePeriodicApi(),nativePeriodicTicksPerSecond()));
    ScopedBenchmarkAffinity affinity;const bool opened=affinity.openNative(stable?BenchmarkPlacement::stableCaller:reserved?BenchmarkPlacement::reservedCaller:BenchmarkPlacement::pinnedCaller);
    CHECK(opened&&affinity.before()==inherited&&affinity.verifyCurrent());
    if(periodic){CHECK(caller.waitTo(nativePeriodicApi().nowTick(nullptr)+std::int64_t(nativePeriodicTicksPerSecond()/1000)));CHECK(affinity.verifyCurrent());CHECK(caller.close()&&caller.close());}
    const bool first=affinity.close(),second=affinity.close();
    BenchmarkGroupAffinity afterCleanup{};CHECK(api.get(nullptr,afterCleanup,error)&&afterCleanup==inherited);
    CHECK(opened&&first&&second&&affinity.qualified());
    const auto before=affinity.before(),during=affinity.during(),after=affinity.after();const auto selected=affinity.selected();
    CHECK(before==after&&during.group==before.group&&during.mask==bit(selected.index)&&(during.mask&before.mask)==during.mask&&selected.index>0);
    for(const auto& cpu:affinity.topology())std::cout<<"SERVICE_CPU28_CPU id="<<cpu.id<<" group="<<cpu.group<<" index="<<unsigned(cpu.index)<<" core="<<unsigned(cpu.core)<<" efficiency="<<unsigned(cpu.efficiency)<<" flags="<<unsigned(cpu.flags)<<'\n';
    std::cout<<"SERVICE_CPU28_AFFINITY attempted="<<affinity.attempted()<<" opened="<<affinity.opened()<<" restored="<<affinity.restored()<<" close1="<<first<<" close2="<<second
      <<" setup_error="<<affinity.setupError()<<" cleanup_error="<<affinity.cleanupError()<<" topology_error="<<affinity.topologyError()<<" group_count="<<affinity.groupCount()
      <<" before_ok="<<affinity.beforeOk()<<" during_ok="<<affinity.duringOk()<<" after_ok="<<affinity.afterOk()
      <<" before_group="<<before.group<<" before_mask="<<before.mask<<" during_group="<<during.group<<" during_mask="<<during.mask<<" after_group="<<after.group<<" after_mask="<<after.mask
      <<" selected_id="<<selected.id<<" selected_group="<<selected.group<<" selected_index="<<unsigned(selected.index)<<" selected_core="<<unsigned(selected.core)<<" selected_efficiency="<<unsigned(selected.efficiency)<<" selected_flags="<<unsigned(selected.flags)
      <<" eligible_mask="<<affinity.eligibleMask()<<" performance_mask="<<affinity.performanceMask()<<'\n';
}
}
int main(int argc,char** argv){
    if(argc==2&&std::string_view(argv[1])=="--live")live();
    else if(argc==2&&std::string_view(argv[1])=="--periodic-live")live(true);
    else if(argc==2&&std::string_view(argv[1])=="--reserved-live")live(false,true);
    else if(argc==2&&std::string_view(argv[1])=="--reserved-periodic-live")live(true,true);
    else if(argc==2&&std::string_view(argv[1])=="--stable-live")live(false,false,true);
    else if(argc==2&&std::string_view(argv[1])=="--stable-periodic-live")live(true,false,true);
    else if(argc==1){deterministicEligibleSelection();invalidInputsPreserveCaller();movableTimerSelection();reservedCallerSelection();stableCallerSelection();setupRollback();restorationFailureIsNeverQualified();externalOverrideInvalidatesRun();scopedAndUntouched();timerDoesNotShareCallerCore();timerLocalHandoffSelection();}
    else return 2;
    std::cout<<"CPU28_AFFINITY_TESTS checks="<<checks<<" failures="<<failures<<" actual_timing=0\n";return failures?1:0;
}

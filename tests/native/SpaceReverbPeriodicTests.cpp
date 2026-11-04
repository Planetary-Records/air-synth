#include "PeriodicCaller.h"
#include "ScopedBenchmarkAffinity.h"
#include <array>
#include <chrono>
#include <string_view>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#include <emmintrin.h>
#endif
using namespace airack::space::probe;
namespace {
int checks=0,failures=0;
void check(bool good,const char* text,int line){++checks;if(!good){++failures;std::cerr<<"FAIL "<<line<<' '<<text<<'\n';}}
#define CHECK(x) check(bool(x),#x,__LINE__)
void sequenceTests(){
    struct Case {std::uint32_t rate,block;std::uint64_t ordinal;std::int64_t offset,shortPeriod;};
    constexpr std::array<Case,8> cases{{{44100,64,1,1451247,1451247},{44100,64,11025,16000000000,1451247},
        {44100,512,1,11609977,11609977},{44100,512,11025,128000000000,11609977},
        {48000,64,750,1000000000,1333333},{48000,512,375,4000000000,10666666},
        {96000,64,1500,1000000000,666666},{96000,512,375,2000000000,5333333}}};
    constexpr std::int64_t anchor=123456789;
    for(const auto c:cases){ReleaseSequence s;CHECK(s.initialize(anchor,1000000000,c.rate,c.block));
        std::int64_t first=-1,last=-1;CHECK(s.tickAt(0,first));CHECK(first==anchor);CHECK(s.tickAt(c.ordinal,last));CHECK(last==anchor+c.offset);
        bool bounded=true;std::int64_t previous=anchor;
        for(std::uint64_t i=1;i<=20000;++i){std::int64_t value=-1;if(!s.tickAt(i,value)||value-previous<c.shortPeriod||value-previous>c.shortPeriod+1){bounded=false;break;}previous=value;}
        CHECK(bounded);
    }
    ReleaseSequence s;CHECK(s.initialize(7,10000000,48000,64));std::int64_t out=0;CHECK(s.tickAt(750,out));CHECK(out==10000007);
    // Invalid reinitialization keeps the already admitted clock unchanged.
    for(const auto bad:std::array<std::array<std::uint64_t,4>,7>{{{0,0,48000,64},{0,1000000001,48000,64},{0,1000000000,0,64},{0,1000000000,7999,64},{0,1000000000,768001,64},{0,1000000000,48000,0},{0,1000000000,48000,513}}}){
        CHECK(!s.initialize(std::int64_t(bad[0]),bad[1],std::uint32_t(bad[2]),std::uint32_t(bad[3])));out=0;CHECK(s.tickAt(750,out));CHECK(out==10000007);
    }
    CHECK(!s.initialize(-1,1000000000,48000,64));out=91;CHECK(!s.tickAt(std::numeric_limits<std::uint64_t>::max(),out));CHECK(out==91);
    ReleaseSequence edge;CHECK(edge.initialize(std::numeric_limits<std::int64_t>::max()-1,1000000000,48000,64));out=92;CHECK(!edge.tickAt(1,out));CHECK(out==92);
    ReleaseSequence unready;out=93;CHECK(!unready.tickAt(0,out));CHECK(out==93);
}
void dueTests(){
    for(const auto pair:std::array<std::array<std::int64_t,2>,8>{{{0,0},{1,0},{-1,0},{-1499,0},{-1500,0},{-1501,-1},{-10000,-8500},{std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::min()+1500}}})
        CHECK(periodicCallerTimerDue100ns(pair[0])==pair[1]);
    // A sub-millisecond helper sleep can overshoot the whole audio period.
    for(const auto pair:std::array<std::array<std::int64_t,2>,12>{{{0,0},{1,0},{-1,0},{-9999,0},{-10000,0},{-10001,0},{-11200,0},{-19999,0},{-20000,-10000},{-20001,-10001},{-100000,-90000},{std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::min()+10000}}})
        CHECK(periodicTimerSleep100ns(pair[0])==pair[1]);
    struct Case {std::int64_t release,now;std::uint64_t ticks;std::int64_t due;};
    for(const auto c:std::array<Case,7>{{{1200,1000,1000000000,-2},{1201,1000,1000000000,-3},{1000,1000,1000000000,0},{999,1000,1000000000,0},{1001,1000,1000000000,-1},{15,0,10000000,-15},{1,0,48000,-209}}}){
        std::int64_t value=94;CHECK(relativeDue100ns(c.release,c.now,c.ticks,value));CHECK(value==c.due);
    }
    for(const auto c:std::array<Case,5>{{{-1,0,1000000000,0},{1,-1,1000000000,0},{1,0,0,0},{1,0,1000000001,0},{std::numeric_limits<std::int64_t>::max(),0,1,0}}}){
        std::int64_t value=95;CHECK(!relativeDue100ns(c.release,c.now,c.ticks,value));CHECK(value==95);
    }
}
void scheduleRecordTests(){
    ReleaseSequence s;CHECK(s.initialize(10000000000,1000000000,48000,64));
    struct Case {ScheduleRecord record;ScheduleDisposition expected;};
    for(const auto c:std::array<Case,6>{{
        {{10001333333,10001333433,10002666666},ScheduleDisposition::onTime},
        {{10001333333,10002666566,10002666667},ScheduleDisposition::late},
        {{10001333333,10001333333,10001333403},ScheduleDisposition::onTime},
        {{10001333334,10001333433,10001333533},ScheduleDisposition::invalid},
        {{10001333333,10001333332,10001333533},ScheduleDisposition::invalid},
        {{10001333333,10001333433,10001333432},ScheduleDisposition::invalid}}})CHECK(classifySchedule(s,0,c.record)==c.expected);
    ReleaseSequence empty;const ScheduleRecord valid{10001333333,10001333433,10001333533};
    CHECK(classifySchedule(empty,0,valid)==ScheduleDisposition::invalid);
    CHECK(classifySchedule(s,std::numeric_limits<std::uint64_t>::max(),valid)==ScheduleDisposition::invalid);
    CHECK(classifySchedule(s,std::numeric_limits<std::uint64_t>::max()-1,valid)==ScheduleDisposition::invalid);
}
struct FakeApi {
    bool failRegister=false,failCreate=false,failWait=false,earlyOnce=false,freezeClock=false;
    unsigned failClose=0,failRevert=0,registers=0,creates=0,closes=0,reverts=0,waits=0,taskLive=0,timerLive=0;
    std::int64_t now=1000;
    std::array<int,128> events{};std::size_t eventCount=0;
    std::array<std::int64_t,64> dues{},origins{};
    void event(int value) noexcept {if(eventCount<events.size())events[eventCount]=value;++eventCount;}
    PeriodicApi api() noexcept {
        return {this,
            [](void* p,std::uint32_t& error) noexcept->PeriodicToken {auto& f=*static_cast<FakeApi*>(p);++f.registers;f.event(1);if(f.failRegister){error=7001;return 0;}f.taskLive=1;return 11;},
            [](void* p,PeriodicToken token,std::uint32_t& error) noexcept {auto& f=*static_cast<FakeApi*>(p);++f.reverts;f.event(5);if(token!=11||!f.taskLive){error=7999;return false;}if(f.failRevert){--f.failRevert;error=7002;return false;}f.taskLive=0;return true;},
            [](void* p,std::uint32_t& error) noexcept->PeriodicToken {auto& f=*static_cast<FakeApi*>(p);++f.creates;f.event(2);if(f.failCreate){error=7003;return 0;}f.timerLive=1;return 22;},
            [](void* p,PeriodicToken token,std::uint32_t& error) noexcept {auto& f=*static_cast<FakeApi*>(p);++f.closes;f.event(4);if(token!=22||!f.timerLive){error=7999;return false;}if(f.failClose){--f.failClose;error=7004;return false;}f.timerLive=0;return true;},
            [](void* p,PeriodicToken token,std::int64_t due,std::int64_t origin,std::uint32_t& error) noexcept {auto& f=*static_cast<FakeApi*>(p);if(f.waits<f.dues.size()){f.dues[f.waits]=due;f.origins[f.waits]=origin;}++f.waits;f.event(3);if(token!=22||!f.timerLive||due>=0){error=7999;return false;}if(f.failWait){error=7005;return false;}if(f.earlyOnce){f.earlyOnce=false;++f.now;}else if(!f.freezeClock)f.now+=(-due)*100;return true;},
            [](void* p) noexcept {return static_cast<FakeApi*>(p)->now;}};
    }
};
void ownershipTests(){
    FakeApi f;
    {PeriodicSession s;CHECK(s.open(f.api(),1000000000));CHECK(s.ready());CHECK(f.taskLive==1&&f.timerLive==1);CHECK(!s.open(f.api(),1000000000));CHECK(f.registers==1&&f.creates==1);
        CHECK(s.waitTo(1301));CHECK(f.waits==1&&f.dues[0]==-4&&f.now==1400);CHECK(s.waitTo(1200));CHECK(f.waits==1);
        CHECK(s.close());CHECK(!s.ready());CHECK(f.taskLive==0&&f.timerLive==0);CHECK(f.closes==1&&f.reverts==1);CHECK(s.close());}
    CHECK(f.closes==1&&f.reverts==1);CHECK(f.eventCount==5&&f.events[0]==1&&f.events[1]==2&&f.events[2]==3&&f.events[3]==4&&f.events[4]==5);
    CHECK(f.origins[0]==1000);
    {FakeApi x;x.earlyOnce=true;PeriodicSession s;CHECK(s.open(x.api(),1000000000));CHECK(s.waitTo(1301));CHECK(x.waits==2&&x.dues[0]==-4&&x.dues[1]==-3&&x.now==1301);CHECK(s.close());}
    {FakeApi x;x.failRegister=true;PeriodicSession s;CHECK(!s.open(x.api(),1000000000));CHECK(s.failure()==PeriodicFailure::registration&&s.platformError()==7001);CHECK(x.registers==1&&x.creates==0&&x.reverts==0);CHECK(s.close());}
    {FakeApi x;x.failCreate=true;PeriodicSession s;CHECK(!s.open(x.api(),1000000000));CHECK(s.failure()==PeriodicFailure::timer&&s.platformError()==7003);CHECK(x.registers==1&&x.creates==1&&x.reverts==1&&x.taskLive==0);CHECK(s.close());}
    {FakeApi x;auto api=x.api();api.armAndWait=nullptr;PeriodicSession s;CHECK(!s.open(api,1000000000));CHECK(s.failure()==PeriodicFailure::invalidApi);CHECK(x.eventCount==0);CHECK(!s.open(x.api(),0));CHECK(x.eventCount==0);CHECK(!s.waitTo(1301));CHECK(x.waits==0);}
    {FakeApi x;x.failWait=true;PeriodicSession s;CHECK(s.open(x.api(),1000000000));CHECK(!s.waitTo(1301));CHECK(s.failure()==PeriodicFailure::wait&&s.platformError()==7005);CHECK(s.close());CHECK(s.failure()==PeriodicFailure::wait&&s.platformError()==7005);CHECK(x.taskLive==0&&x.timerLive==0);}
    {FakeApi x;x.now=-1;PeriodicSession s;CHECK(s.open(x.api(),1000000000));CHECK(!s.waitTo(1301));CHECK(s.failure()==PeriodicFailure::clock&&x.waits==0);CHECK(s.close());}
    {FakeApi x;x.freezeClock=true;PeriodicSession s;CHECK(s.open(x.api(),1000000000));CHECK(!s.waitTo(1301));CHECK(s.failure()==PeriodicFailure::earlyWakeLimit&&x.waits==64);CHECK(s.close());}
    {FakeApi x;x.failClose=1;PeriodicSession s;CHECK(s.open(x.api(),1000000000));CHECK(!s.close());CHECK(s.cleanupError()==7004&&x.timerLive==1&&x.taskLive==0);CHECK(s.close());CHECK(x.closes==2&&x.reverts==1&&x.timerLive==0);}
    {FakeApi x;x.failRevert=1;PeriodicSession s;CHECK(s.open(x.api(),1000000000));CHECK(!s.close());CHECK(s.cleanupError()==7002&&x.timerLive==0&&x.taskLive==1);CHECK(s.close());CHECK(x.closes==1&&x.reverts==2&&x.taskLive==0);}
    {FakeApi x;x.failCreate=true;x.failRevert=1;PeriodicSession s;CHECK(!s.open(x.api(),1000000000));CHECK(s.failure()==PeriodicFailure::timer&&s.platformError()==7003&&s.cleanupError()==7002);CHECK(!s.ready()&&x.taskLive==1);CHECK(s.close());CHECK(x.reverts==2&&x.closes==0&&x.taskLive==0);}
    FakeApi unwind;bool caught=false;
    try{PeriodicSession s;CHECK(s.open(unwind.api(),1000000000));throw 42;}catch(int value){caught=value==42;}
    CHECK(caught);CHECK(unwind.closes==1&&unwind.reverts==1&&unwind.taskLive==0&&unwind.timerLive==0);
}
void nativeOwnershipTest(){
#if defined(_WIN32)
    const auto api=nativePeriodicApi();const auto ticks=nativePeriodicTicksPerSecond();
    CHECK(ticks>0&&ticks<=1000000000);
    const auto before=GetThreadPriority(GetCurrentThread());CHECK(before!=THREAD_PRIORITY_ERROR_RETURN);
    GROUP_AFFINITY affinity{};CHECK(GetThreadGroupAffinity(GetCurrentThread(),&affinity));
    THREAD_POWER_THROTTLING_STATE power{THREAD_POWER_THROTTLING_CURRENT_VERSION};CHECK(GetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&power,sizeof(power)));
    const auto processPriority=GetPriorityClass(GetCurrentProcess());CHECK(processPriority!=0);
    PeriodicSession s;const bool opened=s.open(api,ticks);CHECK(opened);CHECK(s.ready());
    const auto during=GetThreadPriority(GetCurrentThread());const auto now=api.nowTick?api.nowTick(api.context):0;
    CHECK(now>0);const auto release=now+std::int64_t(ticks/500);
    CHECK(s.waitTo(release));const auto finished=api.nowTick?api.nowTick(api.context):0;CHECK(finished>=release&&finished>0);
    for(unsigned i=0;i<32;++i){
        const auto next=api.nowTick(api.context)+std::int64_t(ticks/(i%2?500:2000));
        CHECK(s.waitTo(next));CHECK(api.nowTick(api.context)>=next);
    }
    CHECK(s.close());CHECK(s.cleanupError()==0);CHECK(!s.ready());
    std::uint32_t error=0;const auto timer=api.createTimer(api.context,error);CHECK(timer!=0);
    CHECK(!api.armAndWait(api.context,timer,0,api.nowTick(api.context),error)&&error==ERROR_INVALID_PARAMETER);
    CHECK(!api.armAndWait(api.context,0,-1,api.nowTick(api.context),error)&&error==ERROR_INVALID_HANDLE);
    CHECK(!api.armAndWait(api.context,timer,std::numeric_limits<std::int64_t>::min(),api.nowTick(api.context),error)&&error==ERROR_ARITHMETIC_OVERFLOW);
    CHECK(!api.armAndWait(api.context,timer,-1,-1,error)&&error==ERROR_INVALID_PARAMETER);
    CHECK(!api.armAndWait(api.context,timer,-1,std::numeric_limits<std::int64_t>::max(),error)&&error==ERROR_INVALID_PARAMETER);
    CHECK(api.closeTimer(api.context,timer,error));
    DWORD handlesBefore=0,handlesAfter=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesBefore));
    for(unsigned i=0;i<8;++i){PeriodicSession repeated;CHECK(repeated.open(api,ticks));CHECK(repeated.waitTo(api.nowTick(api.context)+std::int64_t(ticks/2000)));CHECK(repeated.close());}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handlesAfter));CHECK(handlesAfter<=handlesBefore);
    GROUP_AFFINITY affinityAfter{};CHECK(GetThreadGroupAffinity(GetCurrentThread(),&affinityAfter));CHECK(affinity.Group==affinityAfter.Group&&affinity.Mask==affinityAfter.Mask);
    THREAD_POWER_THROTTLING_STATE powerAfter{THREAD_POWER_THROTTLING_CURRENT_VERSION};CHECK(GetThreadInformation(GetCurrentThread(),ThreadPowerThrottling,&powerAfter,sizeof(powerAfter)));CHECK(power.ControlMask==powerAfter.ControlMask&&power.StateMask==powerAfter.StateMask);CHECK(GetPriorityClass(GetCurrentProcess())==processPriority);
    const auto after=GetThreadPriority(GetCurrentThread());CHECK(after==before);
    std::cout<<"PERIODIC_NATIVE_OWNERSHIP opened="<<opened<<" ticks_per_second="<<ticks<<" priority_before="<<before<<" priority_during="<<during<<" priority_after="<<after<<" cleanup_error="<<s.cleanupError()<<" cpu_acceptance=0\n";
#endif
}
void nativeTimerPlacementTest(){
#if defined(_WIN32)
    ScopedBenchmarkAffinity reference;
    if(!reference.openNative(BenchmarkPlacement::movableTimer)){CHECK(reference.setupError()==50||reference.topologyError()==50);CHECK(reference.close());return;}
    const auto expected=reference.during();CHECK(reference.close());
    DWORD referenceIndex=0;const auto task=AvSetMmThreadCharacteristicsW(L"Pro Audio",&referenceIndex);CHECK(task!=nullptr&&referenceIndex!=0);if(!task)return;
    const auto api=nativePeriodicApi();std::uint32_t error=0;const auto timer=api.createTimer(nullptr,error);CHECK(timer!=0);
    if(!timer){CHECK(AvRevertMmThreadCharacteristics(task));return;}
    const auto info=nativePeriodicTimerInfo(timer);CHECK(info.thread!=0);CHECK(info.task==0);
    const auto thread=OpenThread(THREAD_QUERY_INFORMATION|SYNCHRONIZE,FALSE,info.thread);CHECK(thread!=nullptr);
    if(thread){CHECK(GetThreadPriority(thread)==THREAD_PRIORITY_TIME_CRITICAL);GROUP_AFFINITY actual{};CHECK(GetThreadGroupAffinity(thread,&actual));CHECK(actual.Group==expected.group&&actual.Mask==expected.mask);
        std::cout<<"PERIODIC_TIMER_PLACEMENT helper_tid="<<info.thread<<" helper_task="<<info.task<<" reference_task="<<referenceIndex<<" expected_mask="<<expected.mask<<" actual_mask="<<actual.Mask<<" cpu_acceptance=0\n";}
    if(thread&&std::popcount(expected.mask)>=5){
        GROUP_AFFINITY saved{};const bool known=GetThreadGroupAffinity(GetCurrentThread(),&saved)!=FALSE;CHECK(known);
        if(known){auto pinned=saved;pinned.Group=expected.group;pinned.Mask=KAFFINITY(1)<<std::countr_zero(expected.mask);
            const bool moved=SetThreadGroupAffinity(GetCurrentThread(),&pinned,nullptr)!=FALSE;CHECK(moved);
            if(moved){const auto sampled=api.nowTick(nullptr);CHECK(api.armAndWait(nullptr,timer,-200000,sampled,error));
                GROUP_AFFINITY helper{},caller{};CHECK(GetThreadGroupAffinity(thread,&helper));CHECK(GetThreadGroupAffinity(GetCurrentThread(),&caller));
                CHECK(helper.Group==pinned.Group&&helper.Mask==pinned.Mask);
                CHECK(caller.Group==pinned.Group&&caller.Mask==pinned.Mask);
                std::cout<<"PERIODIC_TIMER_LOCAL_HANDOFF caller_mask="<<pinned.Mask<<" helper_mask="<<helper.Mask<<" cpu_acceptance=0\n";
            }
            CHECK(SetThreadGroupAffinity(GetCurrentThread(),&saved,nullptr));
        }
    }
    CHECK(api.closeTimer(nullptr,timer,error));
    if(thread){CHECK(WaitForSingleObject(thread,0)==WAIT_OBJECT_0);CHECK(CloseHandle(thread));}
    CHECK(AvRevertMmThreadCharacteristics(task));
#endif
}
void blockingWaitAudit(){
#if defined(_WIN32)
    using Clock=std::chrono::steady_clock;
    ULONG64 a=0,b=0;CHECK(QueryThreadCycleTime(GetCurrentThread(),&a));
    for(unsigned i=0;i<32;++i){const auto end=Clock::now()+std::chrono::microseconds(500);while(Clock::now()<end)_mm_pause();}
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&b));const auto busy=b-a;
    const auto api=nativePeriodicApi();PeriodicSession session;CHECK(session.open(api,nativePeriodicTicksPerSecond()));
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&a));
    for(unsigned i=0;i<32;++i){const auto release=api.nowTick(api.context)+500000;CHECK(session.waitTo(release));CHECK(api.nowTick(api.context)>=release);}
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&b));const auto waited=b-a;CHECK(session.close());
    CHECK(busy>0&&waited<double(busy)*.5);
    std::cout<<"PERIODIC_BLOCKING_WAIT busy_cycles="<<busy<<" caller_wait_cycles="<<waited<<" ratio="<<double(waited)/busy<<" fixed_release_ns=500000 iterations=32\n";
#endif
}
void suspendedHelperBackupTest(){
#if defined(_WIN32)
    const auto api=nativePeriodicApi();const auto ticks=nativePeriodicTicksPerSecond();std::uint32_t error=0;
    const auto task=api.registerTask(nullptr,error);CHECK(task!=0);if(!task)return;
    const auto timer=api.createTimer(nullptr,error);CHECK(timer!=0);
    if(!timer){CHECK(api.revertTask(nullptr,task,error));return;}
    const auto info=nativePeriodicTimerInfo(timer);
    const auto helper=OpenThread(THREAD_SUSPEND_RESUME|THREAD_QUERY_INFORMATION,FALSE,info.thread);CHECK(helper!=nullptr);
    if(helper){
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto suspended=SuspendThread(helper);CHECK(suspended!=DWORD(-1));
        if(suspended!=DWORD(-1)){
            bool allWaited=true;
            for(unsigned n=0;n<3;++n){
                const auto sampled=api.nowTick(nullptr);error=0;
                const auto waited=api.armAndWait(nullptr,timer,-50000,sampled,error);
                const auto elapsed=api.nowTick(nullptr)-sampled;
                CHECK(waited&&error==0);CHECK(elapsed>=std::int64_t(ticks/200)&&elapsed<std::int64_t(ticks/10));
                allWaited&=waited;
            }
            CHECK(ResumeThread(helper)!=DWORD(-1));
            if(allWaited)for(unsigned n=0;n<16;++n){const auto sampled=api.nowTick(nullptr);error=0;
                CHECK(api.armAndWait(nullptr,timer,-20000,sampled,error)&&error==0);
                CHECK(api.nowTick(nullptr)>=sampled+std::int64_t(ticks/500));
            }
        }
        CHECK(CloseHandle(helper));
    }
    CHECK(api.closeTimer(nullptr,timer,error));CHECK(api.revertTask(nullptr,task,error));
#endif
}
void staleOriginAudit(){
#if defined(_WIN32)
    const auto api=nativePeriodicApi();const auto ticks=nativePeriodicTicksPerSecond();std::uint32_t error=0;
    const auto timer=api.createTimer(api.context,error);CHECK(timer!=0);if(!timer)return;
    const auto start=api.nowTick(api.context);CHECK(start>std::int64_t(ticks));
    const bool waited=api.armAndWait(api.context,timer,-2000000,start-std::int64_t(ticks),error);
    const auto finish=api.nowTick(api.context);CHECK(waited&&error==0);CHECK(finish-start<std::int64_t(ticks/10));
    CHECK(api.closeTimer(api.context,timer,error));
    std::cout<<"PERIODIC_STALE_ORIGIN elapsed_ticks="<<finish-start<<" maximum_ticks="<<ticks/10<<" sampled_age_seconds=1 relative_delay_ms=200 cpu_acceptance=0\n";
#endif
}
}
int main(int argc,char** argv){if(argc==2&&std::string_view(argv[1])=="--blocking-wait-audit"){blockingWaitAudit();staleOriginAudit();return failures?1:0;}sequenceTests();dueTests();scheduleRecordTests();ownershipTests();nativeTimerPlacementTest();nativeOwnershipTest();suspendedHelperBackupTest();std::cout<<"SpaceReverbPeriodic checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}

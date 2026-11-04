#pragma once
#include <cstdint>
#include <limits>
namespace airack::space::probe {
class ReleaseSequence {
public:
    bool initialize(std::int64_t anchor,std::uint64_t ticksPerSecond,std::uint32_t rate,std::uint32_t block) noexcept {
        if(anchor<0||ticksPerSecond==0||ticksPerSecond>1000000000||rate<8000||rate>768000||block==0||block>512)return false;
        anchor_=anchor;numerator_=ticksPerSecond*block;rate_=rate;return true;
    }
    bool tickAt(std::uint64_t ordinal,std::int64_t& output) const noexcept {
        if(rate_==0||ordinal>std::numeric_limits<std::uint64_t>::max()/numerator_)return false;
        const auto offset=ordinal*numerator_/rate_;
        if(offset>std::uint64_t(std::numeric_limits<std::int64_t>::max()-anchor_))return false;
        output=anchor_+std::int64_t(offset);return true;
    }
private:
    std::int64_t anchor_=0;
    std::uint64_t numerator_=0;
    std::uint32_t rate_=0;
};
struct ScheduleRecord {std::int64_t releaseTick=0,startTick=0,finishTick=0;};
static_assert(sizeof(ScheduleRecord)==24);
enum class ScheduleDisposition {invalid,onTime,late};
inline ScheduleDisposition classifySchedule(const ReleaseSequence& sequence,std::uint64_t ordinal,const ScheduleRecord& record) noexcept {
    if(ordinal>std::numeric_limits<std::uint64_t>::max()-2)return ScheduleDisposition::invalid;
    std::int64_t release=0,deadline=0;
    if(!sequence.tickAt(ordinal+1,release)||!sequence.tickAt(ordinal+2,deadline)||record.releaseTick!=release||record.startTick<release||record.finishTick<record.startTick)return ScheduleDisposition::invalid;
    return record.finishTick>deadline?ScheduleDisposition::late:ScheduleDisposition::onTime;
}
inline bool relativeDue100ns(std::int64_t release,std::int64_t now,std::uint64_t ticksPerSecond,std::int64_t& output) noexcept {
    if(release<0||now<0||ticksPerSecond==0||ticksPerSecond>1000000000)return false;
    if(release<=now){output=0;return true;}
    const auto difference=std::uint64_t(release-now),whole=difference/ticksPerSecond,remainder=difference%ticksPerSecond;
    constexpr std::uint64_t units=10000000,maximum=std::uint64_t(std::numeric_limits<std::int64_t>::max());
    if(whole>maximum/units)return false;
    const auto fraction=(remainder*units+ticksPerSecond-1)/ticksPerSecond,integral=whole*units;
    if(fraction>maximum-integral)return false;
    output=-std::int64_t(integral+fraction);return true;
}
using PeriodicToken=std::uintptr_t;
inline constexpr std::int64_t periodicHelperMinimumSleep100ns=10000;
inline std::int64_t periodicTimerSleep100ns(std::int64_t relativeDue) noexcept {
    // Short timer waits can return milliseconds late; retain at least a 1ms guard.
    return relativeDue <= -2*periodicHelperMinimumSleep100ns ? relativeDue+periodicHelperMinimumSleep100ns : 0;
}
inline std::int64_t periodicCallerTimerDue100ns(std::int64_t relativeDue) noexcept {
    return relativeDue < -1500 ? relativeDue+1500 : 0;
}
struct PeriodicApi {
    void* context=nullptr;
    PeriodicToken (*registerTask)(void*,std::uint32_t&) noexcept=nullptr;
    bool (*revertTask)(void*,PeriodicToken,std::uint32_t&) noexcept=nullptr;
    PeriodicToken (*createTimer)(void*,std::uint32_t&) noexcept=nullptr;
    bool (*closeTimer)(void*,PeriodicToken,std::uint32_t&) noexcept=nullptr;
    // Anchor the relative duration to the supplied nowTick sample, including delayed entry.
    bool (*armAndWait)(void*,PeriodicToken,std::int64_t,std::int64_t,std::uint32_t&) noexcept=nullptr;
    std::int64_t (*nowTick)(void*) noexcept=nullptr;
};
enum class PeriodicFailure {none,invalidApi,invalidState,registration,timer,clock,wait,earlyWakeLimit};
class PeriodicSession {
public:
    PeriodicSession()=default;
    ~PeriodicSession(){close();}
    PeriodicSession(const PeriodicSession&)=delete;
    PeriodicSession& operator=(const PeriodicSession&)=delete;
    bool open(const PeriodicApi& api,std::uint64_t ticksPerSecond) noexcept {
        if(task_||timer_)return fail(PeriodicFailure::invalidState);
        if(!api.registerTask||!api.revertTask||!api.createTimer||!api.closeTimer||!api.armAndWait||!api.nowTick)return fail(PeriodicFailure::invalidApi);
        if(ticksPerSecond==0||ticksPerSecond>1000000000)return fail(PeriodicFailure::clock);
        api_=api;ticksPerSecond_=ticksPerSecond;failure_=PeriodicFailure::none;platformError_=cleanupError_=0;
        std::uint32_t error=0;task_=api_.registerTask(api_.context,error);
        if(!task_)return fail(PeriodicFailure::registration,error);
        error=0;timer_=api_.createTimer(api_.context,error);
        if(!timer_){fail(PeriodicFailure::timer,error);close();return false;}
        return true;
    }
    bool waitTo(std::int64_t release) noexcept {
        if(!ready())return fail(PeriodicFailure::invalidState);
        for(unsigned early=0;early<64;++early){
            const auto sampled=api_.nowTick(api_.context);
            std::int64_t due=0;if(!relativeDue100ns(release,sampled,ticksPerSecond_,due))return fail(PeriodicFailure::clock);
            if(due==0)return true;
            std::uint32_t error=0;if(!api_.armAndWait(api_.context,timer_,due,sampled,error))return fail(PeriodicFailure::wait,error);
        }
        std::int64_t due=0;if(!relativeDue100ns(release,api_.nowTick(api_.context),ticksPerSecond_,due))return fail(PeriodicFailure::clock);
        return due==0?true:fail(PeriodicFailure::earlyWakeLimit);
    }
    bool close() noexcept {
        bool passed=true;std::uint32_t error=0;
        if(timer_){if(api_.closeTimer(api_.context,timer_,error))timer_=0;else {passed=false;if(!cleanupError_)cleanupError_=error;}}
        error=0;
        if(task_){if(api_.revertTask(api_.context,task_,error))task_=0;else {passed=false;if(!cleanupError_)cleanupError_=error;}}
        return passed;
    }
    bool ready() const noexcept {return task_&&timer_;}
    PeriodicFailure failure() const noexcept {return failure_;}
    std::uint32_t platformError() const noexcept {return platformError_;}
    std::uint32_t cleanupError() const noexcept {return cleanupError_;}
private:
    bool fail(PeriodicFailure failure,std::uint32_t error=0) noexcept {failure_=failure;platformError_=error;return false;}
    PeriodicApi api_;
    PeriodicToken task_=0,timer_=0;
    std::uint64_t ticksPerSecond_=0;
    PeriodicFailure failure_=PeriodicFailure::none;
    std::uint32_t platformError_=0,cleanupError_=0;
};
PeriodicApi nativePeriodicApi() noexcept;
struct NativePeriodicTimerInfo {std::uint32_t thread=0,task=0;};
// Requires a live timer token returned by nativePeriodicApi().
NativePeriodicTimerInfo nativePeriodicTimerInfo(PeriodicToken) noexcept;
std::uint64_t nativePeriodicTicksPerSecond() noexcept;
}

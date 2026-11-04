#include "ProcessHighQos.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace airack::space::probe {
namespace {
constexpr std::uint32_t invalidState=0xe0000001,invalidApi=0xe0000002,
    invalidVersion=0xe0000003,readbackMismatch=0xe0000004,unspecifiedError=0xe0000005;
constexpr std::uint32_t ownedMechanisms=1U|4U;
std::uint32_t errorCode(std::uint32_t error) noexcept {return error?error:unspecifiedError;}
#if defined(_WIN32)
static_assert(PROCESS_POWER_THROTTLING_CURRENT_VERSION==1&&PROCESS_POWER_THROTTLING_EXECUTION_SPEED==1
              &&PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION==4);
bool getOwned(void*,ProcessPowerState& output,std::uint32_t& error) noexcept {
    PROCESS_POWER_THROTTLING_STATE state{};state.Version=PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if(!GetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&state,sizeof(state))){error=GetLastError();return false;}
    output={state.Version,state.ControlMask,state.StateMask};return true;
}
bool setOwned(void*,const ProcessPowerState& input,std::uint32_t& error) noexcept {
    PROCESS_POWER_THROTTLING_STATE state{input.version,input.control,input.state};
    if(!SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&state,sizeof(state))){error=GetLastError();return false;}
    return true;
}
#endif
}
ProcessPowerApi nativeProcessPowerApi() noexcept {
#if defined(_WIN32)
    return {nullptr,getOwned,setOwned};
#else
    return {};
#endif
}
ProcessHighQos::~ProcessHighQos(){close();}
bool ProcessHighQos::open(ProcessPowerApi api) noexcept {
    if(used_){if(!setupError_)setupError_=invalidState;return false;}used_=true;
    if(!api.get||!api.set){setupError_=invalidApi;return false;}api_=api;
    std::uint32_t error=0;
    if(!api_.get(api_.context,before_,error)){setupError_=errorCode(error);return false;}
    if(before_.version!=1){setupError_=invalidVersion;return false;}
    // Hidden Windows 11 probes must explicitly honor their owned timer-resolution request.
    const ProcessPowerState desired{before_.version,before_.control|ownedMechanisms,before_.state&~ownedMechanisms};
    // A failed setter may still have mutated state; always attempt exact rollback.
    attempted_=pending_=true;
    if(!api_.set(api_.context,desired,error)){setupError_=errorCode(error);close();return false;}
    error=0;
    if(!api_.get(api_.context,during_,error)){setupError_=errorCode(error);close();return false;}
    if(during_!=desired){setupError_=readbackMismatch;close();return false;}
    opened_=true;return true;
}
bool ProcessHighQos::close() noexcept {
    if(!pending_)return true;
    std::uint32_t error=0;
    if(!api_.set(api_.context,before_,error)){if(!cleanupError_)cleanupError_=errorCode(error);return false;}
    error=0;
    if(!api_.get(api_.context,after_,error)){if(!cleanupError_)cleanupError_=errorCode(error);return false;}
    if(after_!=before_){if(!cleanupError_)cleanupError_=readbackMismatch;return false;}
    pending_=false;restored_=true;return true;
}
}

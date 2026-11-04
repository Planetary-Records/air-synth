#pragma once
#include <cstdint>

namespace airack::space::probe {
struct ProcessPowerState {
    std::uint32_t version=0,control=0,state=0;
    friend bool operator==(const ProcessPowerState&,const ProcessPowerState&)=default;
};
struct ProcessPowerApi {
    void* context=nullptr;
    bool (*get)(void*,ProcessPowerState&,std::uint32_t&) noexcept=nullptr;
    bool (*set)(void*,const ProcessPowerState&,std::uint32_t&) noexcept=nullptr;
};
// Development caller only. The native adapter accepts no foreign process handle.
ProcessPowerApi nativeProcessPowerApi() noexcept;
class ProcessHighQos {
public:
    ProcessHighQos()=default;
    ~ProcessHighQos();
    ProcessHighQos(const ProcessHighQos&)=delete;
    ProcessHighQos& operator=(const ProcessHighQos&)=delete;
    bool open(ProcessPowerApi) noexcept;
    bool close() noexcept;
    bool attempted() const noexcept {return attempted_;}
    bool opened() const noexcept {return opened_;}
    bool restored() const noexcept {return restored_;}
    bool qualified() const noexcept {return opened_&&restored_&&!setupError_&&!cleanupError_;}
    std::uint32_t setupError() const noexcept {return setupError_;}
    std::uint32_t cleanupError() const noexcept {return cleanupError_;}
    ProcessPowerState before() const noexcept {return before_;}
    ProcessPowerState during() const noexcept {return during_;}
    ProcessPowerState after() const noexcept {return after_;}
private:
    ProcessPowerApi api_;
    ProcessPowerState before_,during_,after_;
    bool used_=false,attempted_=false,opened_=false,pending_=false,restored_=false;
    std::uint32_t setupError_=0,cleanupError_=0;
};
}

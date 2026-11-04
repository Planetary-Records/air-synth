#pragma once
#include "SpaceReverbControls.h"
#include "SpaceReverbTank.h"
#include <memory>
namespace airack::space {
struct EarlyTap {
    double samples=0,gain=0;
    bool cross=false;
    bool operator==(const EarlyTap&) const = default;
};
struct TailBound {
    bool passed=false;
    double seconds=0,timeBound=0,radius=0,logTransferBound=0,logOutputBoundAtStop=0;
    bool operator==(const TailBound&) const = default;
};
struct PreparedField {
    Settings settings;
    detail::PreparedTank tank;
    PreparedPostEq post;
    PreparedDuck duck;
    std::array<std::array<EarlyTap,3>,2> early {};
    TailBound tail;
    bool operator==(const PreparedField&) const = default;
};
struct FieldPreparationReport {
    const char* reason="unprepared";
    detail::TankPreparationReport tank;
};
bool prepareField(const Settings&,double physicalRate,PreparedField&,FieldPreparationReport* =nullptr,detail::PreparationControl ={}) noexcept;
struct PostEqBound {bool passed=false;double logMagnitude=0,maximumPole=0;};
PostEqBound boundPostEq(const PreparedPostEq&,double radius) noexcept;
namespace detail {
// Pure log-domain tail arithmetic; numerical/certificate preparation is separate.
TailBound makeSpaceTailBound(double logTransferBound,double physicalRate,double timeBound) noexcept;
}
class FieldRuntime {
public:
    FieldRuntime();~FieldRuntime();
    FieldRuntime(const FieldRuntime&)=delete;
    FieldRuntime& operator=(const FieldRuntime&)=delete;
    FieldRuntime(FieldRuntime&&) noexcept;
    FieldRuntime& operator=(FieldRuntime&&) noexcept;
    bool prepareStorage(double physicalRate,detail::RenderBackend=detail::RenderBackend::automatic);
    bool configure(const PreparedField&) noexcept;
    void reset() noexcept;
    std::array<double,2> tick(std::array<double,2>) noexcept;
    const PreparedField& prepared() const noexcept;
    std::size_t storageBytes() const noexcept;
    double duckEnvelope() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}

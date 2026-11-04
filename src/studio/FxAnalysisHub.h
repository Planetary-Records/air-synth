#pragma once
#include "FxSpectrum.h"
#include <optional>

namespace airack::studio
{
// One editor owns this consumer. All views for the same monitor must share it.
class FxAnalysisHub final
{
public:
    struct Lease {
        std::uint64_t id=0;
        std::uint8_t slot=0;
        bool operator==(const Lease&) const=default;
    };
    explicit FxAnalysisHub(std::shared_ptr<FxMonitor>);
    ~FxAnalysisHub();
    std::optional<Lease> acquire(std::uint8_t);
    void release(Lease);
    void update();
    const FxSpectrum::Result* result(Lease) const noexcept;
    std::size_t activeAnalyzers() const noexcept;
    bool setSpectrumEnabled(Lease,bool) noexcept;
    bool requestAudition(Lease,std::optional<std::size_t>,bool detector=false) noexcept;
    std::optional<std::size_t> requestedAudition(Lease) const noexcept;
    std::uint64_t generation(Lease) const noexcept;
    void retire();
    std::size_t packetsLastTick() const noexcept;
    std::size_t packetsLastTick(std::uint8_t) const noexcept;
    static constexpr std::size_t packetBudgetPerTick=FxMonitor::slots*FxMonitor::packetCapacity;
private:
    struct State;
    std::unique_ptr<State> state;
};
}

#pragma once
#include "DynamicEq.h"
#include "Saturation.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace airack
{
class SpectralEq;
// One render producer per physical slot, owned by the callback or a completed
// prepared stage; one message-thread consumer. Storage
// outlives the attached engine. All FIFO payload and staging memory is fixed.
class FxMonitor final
{
public:
    static constexpr std::size_t slots=8, packetFrames=256, packetCapacity=128;
    using Frame=std::array<double,2>;
    using ActiveSlots=std::array<bool,slots>;
    struct Token
    {
        std::uint64_t id=0;std::uint8_t slot=0;
        bool operator==(const Token&) const=default;
    };
    struct SaturationToken
    {
        std::uint64_t id=0;std::uint8_t slot=0;
        bool operator==(const SaturationToken&) const=default;
    };
    struct Packet
    {
        std::uint64_t token=0,generation=0,firstSample=0;
        double sampleRate=0;
        // Slot input L/R, slot output L/R. A complete packet is256 frames.
        std::array<std::array<float,4>,packetFrames> samples{};
        std::array<float,8> movementDb{};
        std::array<float,8> spectralMovementDb{};
        // Render-owned meter snapshot at the packet's last frame.
        std::array<DynamicEqDetectorReadback,8> detectorReadbacks{};
        bool detectorReadbacksValid=false,externalAvailable=false;
    };
    struct Audition {std::size_t band=0;bool detector=false;};
    struct Stats
    {
        // Sample counters are published at packet boundaries or invalidation.
        std::uint64_t capturedFrames=0,published=0,dropped=0,clipped=0;
        int queued=0;
    };
    FxMonitor();
    ~FxMonitor();
    FxMonitor(const FxMonitor&)=delete;
    FxMonitor& operator=(const FxMonitor&)=delete;
    // Message-thread subscription ownership; tokens do not authorize sound edits.
    std::optional<Token> subscribe(std::uint8_t) noexcept;
    bool retire(Token) noexcept;
    bool read(Token,Packet&) noexcept;
    // Shared scan budget bounds a complete analyzer tick, including stale data.
    bool read(Token,Packet&,std::size_t& remainingPackets) noexcept;
    std::uint64_t generation(Token) const noexcept;
    Stats stats(std::uint8_t) const noexcept;
    // Prepared internal rate is available even without a capture subscriber.
    double sampleRate() const noexcept;
    // Render owner, or lifecycle owner while stopped; zero detaches the source.
    bool publishSampleRate(double) noexcept;
    bool requestAudition(Token,std::optional<std::size_t>,bool detector=false) noexcept;
    std::optional<std::size_t> requestedAudition(Token) const noexcept;
    std::optional<std::size_t> auditionBand(std::uint8_t) const noexcept;
    // Render-owner snapshot: band and mode are read from one atomic command.
    std::optional<Audition> auditionRequest(std::uint8_t) const noexcept;
    bool externalAvailable() const noexcept;
    void cancelAudition(std::uint8_t) noexcept;
    void auditionRunning(std::uint8_t,bool) noexcept;
    double auditionTailSeconds() const noexcept;
    std::optional<SaturationToken> acquireSaturationAudition(std::uint8_t) noexcept;
    bool requestSaturationAudition(SaturationToken,std::optional<SaturationLane>) noexcept;
    bool retireSaturationAudition(SaturationToken) noexcept;
    std::optional<SaturationLane> saturationAuditionLane(std::uint8_t) const noexcept;
    void cancelSaturationAudition(std::uint8_t) noexcept;
    void saturationAuditionRunning(std::uint8_t,bool) noexcept;
    void beginSaturationBlock(const ActiveSlots&) noexcept;
    // Render owner, or prepare/teardown while rendering is stopped.
    bool beginBlock(double internalRate,const ActiveSlots&,bool externalAvailable=false) noexcept;
    void capture(std::uint8_t,Frame input,Frame output,const DynamicEq* meter=nullptr,const SpectralEq* spectral=nullptr) noexcept;
    // Render owner: zero means this block has no capture subscription.
    std::size_t framesUntilPacket(std::uint8_t) const noexcept;
    void invalidate() noexcept;
private:
    friend struct FxMonitorTestAccess;
    std::uint64_t nextSaturationGeneration=1;
    struct State;
    std::unique_ptr<State> state;
};
}

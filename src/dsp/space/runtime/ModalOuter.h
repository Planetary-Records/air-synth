#pragma once
#include "ModalRoom.h"
#include "SpaceReverbControls.h"
namespace airack::space::runtime {
struct ModalPosition {std::size_t whole=0;double fraction=0;bool operator==(const ModalPosition&) const=default;};
struct PreparedModalEarly {
    double rate=0,size=.5;std::array<std::array<double,3>,2> samples{};std::array<std::array<ModalPosition,3>,2> positions{};
    bool operator==(const PreparedModalEarly&) const=default;
};
struct ModalWetTargets {double predelayMs=20,character=.25,distance=.5;bool operator==(const ModalWetTargets&) const=default;};
struct PreparedModalPost {PreparedPostEq coefficients;double commonLogL1=0;bool operator==(const PreparedModalPost&) const=default;};
struct PreparedModalDuck {PreparedDuck coefficients;double thresholdLinear=1,minimumGain=1;bool operator==(const PreparedModalDuck&) const=default;};
struct PreparedModalWet {
    double rate=0;ModalWetTargets targets;PreparedModalEarly early;PreparedModalPost post;
    bool operator==(const PreparedModalWet&) const=default;
};
struct PreparedModalFinal {
    double rate=0,width=1,mix=.25;PreparedModalDuck duck;
    bool operator==(const PreparedModalFinal&) const=default;
};
bool prepareModalEarly(double size,double rate,PreparedModalEarly&) noexcept;
bool prepareModalPost(const Settings&,double rate,PreparedModalPost&) noexcept;
bool prepareModalDuck(const Settings&,double rate,PreparedModalDuck&) noexcept;
bool prepareModalWet(const Settings&,double rate,PreparedModalWet&) noexcept;
bool prepareModalFinal(const Settings&,double rate,PreparedModalFinal&) noexcept;
class ModalWetOuter {
public:
    using Frame=ModalRoom::Frame;
    ModalWetOuter();~ModalWetOuter();ModalWetOuter(const ModalWetOuter&)=delete;ModalWetOuter& operator=(const ModalWetOuter&)=delete;
    bool prepareStorage(double rate);
    bool configure(const PreparedModalWet&) noexcept;
    bool setFastTargets(ModalWetTargets) noexcept;
    bool setEarlyGeometries(const PreparedModalEarly&,const PreparedModalEarly&) noexcept;
    bool excite(std::span<Frame>) noexcept;
    std::span<const double> characters() const noexcept;
    bool mixRooms(std::span<Frame> oldLate,std::span<const Frame> newLate={},std::span<const double> alpha={}) noexcept;
    bool adoptPost(const PreparedModalPost&) noexcept;
    bool post(std::span<Frame>,std::span<const double> alpha={}) noexcept;
    bool commitPost() noexcept;
    bool postPending() const noexcept;
    const PreparedModalPost& activePost() const noexcept;
    void reset() noexcept;
    std::size_t storageBytes() const noexcept;
private:
    struct State;std::unique_ptr<State> state_;
};
class ModalFinalOuter {
public:
    using Frame=ModalRoom::Frame;
    ModalFinalOuter();~ModalFinalOuter();ModalFinalOuter(const ModalFinalOuter&)=delete;ModalFinalOuter& operator=(const ModalFinalOuter&)=delete;
    bool prepareStorage(double rate);
    bool configure(const PreparedModalFinal&) noexcept;
    bool setFastTargets(double width,double mix) noexcept;
    bool adoptDuck(const PreparedModalDuck&) noexcept;
    bool capture(std::span<const Frame>) noexcept;
    bool finish(std::span<Frame>) noexcept;
    double duckEnvelope() const noexcept;
    double duckGain() const noexcept;
    double observedPeak() const noexcept;
    void reset() noexcept;
    std::size_t storageBytes() const noexcept;
private:
    struct State;std::unique_ptr<State> state_;
};
}

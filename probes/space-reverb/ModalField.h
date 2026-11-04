#pragma once
#include "ModalTank.h"
#include "space/runtime/ModalTailMath.h"
#include "SpaceReverbControls.h"
namespace airack::space::probe {
struct ModalFieldTail {
    bool passed=false;double seconds=0,radius=0,timeBound=0,logPost=0,logRoom=0,referenceInputPeak=1;
    std::size_t finiteSupportFrames=0;
};
using runtime::ModalWeightedSection;
using runtime::boundModalPostSection;
struct PreparedModalField {
    Settings settings;PreparedModal modal;PreparedPostEq post;PreparedDuck duck;
    std::array<double,8> diffusionSamples{};
    std::array<std::array<double,3>,2> earlySamples{};
    std::array<std::array<double,2>,2> oscillatorStep{};
    ModalFieldTail tail;
};
bool prepareModalField(const Settings&,double rate,std::size_t modes,PreparedModalField&,detail::PreparationControl={}) noexcept;
class ModalField {
public:
    using Frame=ModalTank::Frame;
    ModalField();~ModalField();ModalField(const ModalField&)=delete;ModalField& operator=(const ModalField&)=delete;
    bool prepareStorage(double rate,ModalBackend=ModalBackend::automatic);
    bool configure(const PreparedModalField&) noexcept;
    bool processBlock(std::span<Frame>) noexcept;
    void reset() noexcept;
    std::size_t storageBytes() const noexcept;
    double duckEnvelope() const noexcept;
    const PreparedModalField& prepared() const noexcept;
    ModalSample modeState(std::size_t) const noexcept;
private:
    struct State;std::unique_ptr<State> state_;
};
}

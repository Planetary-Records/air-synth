#pragma once
#include "ModalTank.h"
namespace airack::space::runtime {
struct PreparedModalRoom {
    PreparedModal modal;
    std::array<double,8> diffusionSamples{};
    std::array<std::array<double,2>,2> oscillatorStep{};
    bool operator==(const PreparedModalRoom&) const=default;
};
bool prepareModalRoom(const DecayCurve&,double size,double rate,std::size_t modes,PreparedModalRoom&,detail::PreparationControl={}) noexcept;
class ModalRoom {
public:
    using Frame=ModalTank::Frame;
    ModalRoom();~ModalRoom();ModalRoom(const ModalRoom&)=delete;ModalRoom& operator=(const ModalRoom&)=delete;
    bool prepareStorage(double rate,ModalBackend=ModalBackend::automatic,bool blockedScatter=true,bool pairedReduction=true);
    bool configure(const PreparedModalRoom&) noexcept;
    bool adoptCopied(const PreparedModalRoom&,const ModalRoom&) noexcept;
    bool processBlock(std::span<Frame>,std::span<const double> characters) noexcept;
    void reset() noexcept;
    const PreparedModalRoom& prepared() const noexcept;
    ModalSample modeState(std::size_t) const noexcept;
    std::size_t storageBytes() const noexcept;
    std::size_t historyCopyBytes() const noexcept;
    bool usesBlockedScatter() const noexcept;
    bool usesPairedReduction() const noexcept;
private:
    bool configureImpl(const PreparedModalRoom&,bool factory) noexcept;
    void resetHistory() noexcept;
    bool processScatterBlocked(std::span<Frame>) noexcept;
    bool adoptPreparedCopied(const PreparedModalRoom&,const ModalRoom&) noexcept;
    friend class ModalService;
    struct State;std::unique_ptr<State> state_;
};
}

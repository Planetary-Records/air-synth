#pragma once
#include "ModalOuter.h"
#include <cstdint>
namespace airack::space::runtime {
inline constexpr std::array<double,12> modalTailTimes{.115,.23,.46,.92,1.84,2.3,3.68,7.36,14.72,23,29.44,46};
struct ModalTailTransport {std::uint64_t supportFrames=0;double logGain=0;};
class ModalTailContext {
public:
    double rate() const noexcept {return rate_;}
    std::size_t modes() const noexcept {return modes_;}
    double radius(std::size_t b) const noexcept {return b<12?rho_[b]:0;}
    double inverseLoss(std::size_t b) const noexcept {return b<12?inverse_[b]:0;}
    std::uint64_t support() const noexcept {return support_;}
    double pickup() const noexcept {return pickup_;}
private:
    double rate_=0,pickup_=0,ln2_=0,negativeLogFloor_=0,transportLogGain_=0;
    std::size_t modes_=0;std::uint64_t support_=0;
    std::array<double,12> rho_{},inverse_{};std::array<double,8> sourceLogs_{};
    friend bool prepareModalTailContext(double,std::size_t,ModalTailContext&,ModalTailTransport) noexcept;
    friend class ModalLiveTail;
    friend bool composeModalLiveTail(std::span<const class ModalLiveTail* const>,unsigned,struct ModalTailReport&) noexcept;
};
class PreparedModalTail {
public:
    unsigned mask() const noexcept {return mask_;}
    double logGain(std::size_t b) const noexcept {return b<12?logs_[b]:0;}
    double maximumRadius() const noexcept {return maximumRadius_;}
    double injectionSquaredNorm() const noexcept {return injectionSquaredNorm_;}
    bool operator==(const PreparedModalTail&) const=default;
private:
    double rate_=0,maximumRadius_=0,injectionSquaredNorm_=0;
    std::size_t modes_=0;unsigned mask_=0,domain_=0;std::array<double,12> logs_{};
    friend bool prepareModalRoomTail(const ModalTailContext&,const PreparedModalRoom&,PreparedModalTail&) noexcept;
    friend bool prepareModalPostTail(const ModalTailContext&,const PreparedModalPost&,PreparedModalTail&) noexcept;
    friend class ModalLiveTail;
};
struct ModalTailReport {
    bool passed=false;unsigned mask=0,selectedBin=0,sourceMultiplicity=0;int peakExponent=0;
    std::uint64_t frames=0,support=0;double seconds=0;
    std::array<double,12> logGains{};std::array<std::uint64_t,12> circleFrames{};
    bool operator==(const ModalTailReport&) const=default;
};
bool prepareModalTailContext(double rate,std::size_t modes,ModalTailContext&,ModalTailTransport={}) noexcept;
bool prepareModalRoomTail(const ModalTailContext&,const PreparedModalRoom&,PreparedModalTail&) noexcept;
bool prepareModalPostTail(const ModalTailContext&,const PreparedModalPost&,PreparedModalTail&) noexcept;
// Metadata reset is legal only after associated DSP histories have been reset.
class ModalLiveTail {
public:
    bool configure(const ModalTailContext&,const PreparedModalTail& room,const PreparedModalTail& post) noexcept;
    bool admitRoom(const PreparedModalTail&) noexcept;
    bool admitPost(const PreparedModalTail&) noexcept;
    bool observePeak(double absolutePeak) noexcept;
    bool reset(const PreparedModalTail& activeRoom,const PreparedModalTail& activePost) noexcept;
    bool report(ModalTailReport&) const noexcept;
    int peakExponent() const noexcept {return peakExponent_;}
private:
    bool matches(const PreparedModalTail&,unsigned domain) const noexcept;
    ModalTailContext context_;std::array<double,12> room_{},post_{};unsigned mask_=0;int peakExponent_=0;
    friend bool composeModalLiveTail(std::span<const ModalLiveTail* const>,unsigned,ModalTailReport&) noexcept;
};
bool composeModalLiveTail(std::span<const ModalLiveTail* const>,unsigned sourceMultiplicity,ModalTailReport&) noexcept;
}

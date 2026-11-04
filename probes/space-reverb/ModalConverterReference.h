#pragma once
#include <array>
#include <cstddef>
#include <span>
namespace airack::space::probe::converter_reference {
class PreparedModalConverter {
public:
    unsigned factor() const noexcept {return factor_;}
    double tap(std::size_t i) const noexcept {return i<127?taps_[i]:0;}
    double absoluteSum() const noexcept {return absoluteSum_;}
    double peakGain() const noexcept {return gain_;}
    double logGain() const noexcept {return logGain_;}
    std::size_t physicalSupport() const noexcept {return factor_==2?252:0;}
    std::size_t logicalSupport() const noexcept {return factor_==2?127:0;}
    std::size_t physicalGroupDelay() const noexcept {return factor_==2?126:0;}
    bool operator==(const PreparedModalConverter&) const=default;
private:
    unsigned factor_=0;std::array<double,127> taps_{};double absoluteSum_=0,gain_=0,logGain_=0;
    friend bool prepareModalConverter(unsigned,PreparedModalConverter&) noexcept;friend class ModalWetConverter;
};
bool prepareModalConverter(unsigned factor,PreparedModalConverter&) noexcept;
class ModalWetConverter {
public:
    using Frame=std::array<double,2>;
    bool configure(const PreparedModalConverter&) noexcept;
    void reset() noexcept;
    bool down(std::span<const Frame> physical,std::span<Frame> logical,std::size_t& produced) noexcept;
    bool up(std::span<const Frame> logical,std::span<Frame> physical) noexcept;
    bool evenPhase() const noexcept {return even_;}
    bool open() const noexcept {return open_;}
    std::size_t storageBytes() const noexcept {return sizeof(*this);}
private:
    PreparedModalConverter prepared_;std::array<Frame,127> down_{},up_{};
    std::size_t downCursor_=0,upCursor_=0,physicalCount_=0,logicalCount_=0;bool even_=true,startEven_=true,open_=false;
};
}

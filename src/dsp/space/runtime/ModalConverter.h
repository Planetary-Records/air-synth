#pragma once
#include <array>
#include <cstddef>
#include <span>
namespace airack::space::runtime {
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
enum class ModalConverterBackend {reference,mirroredScalar,mirroredSse2,automatic,mirroredAvx2};
bool modalConverterAvx2Available() noexcept;
class ModalWetConverter {
public:
    using Frame=std::array<double,2>;
    bool configure(const PreparedModalConverter&,ModalConverterBackend=ModalConverterBackend::reference) noexcept;
    ModalConverterBackend selectedBackend() const noexcept;
    void reset() noexcept;
    bool down(std::span<const Frame> physical,std::span<Frame> logical,std::size_t& produced) noexcept;
    bool up(std::span<const Frame> logical,std::span<Frame> physical) noexcept;
    bool evenPhase() const noexcept {return even_;}
    bool open() const noexcept {return open_;}
    std::size_t storageBytes() const noexcept {return sizeof(*this);}
private:
    using Kernel=Frame(*)(const PreparedModalConverter&,const Frame*,std::size_t) noexcept;
    using DownBlock=void(*)(ModalWetConverter&,std::span<const Frame>,std::span<Frame>) noexcept;
    using UpBlock=void(*)(ModalWetConverter&,std::span<const Frame>,std::span<Frame>) noexcept;
    PreparedModalConverter prepared_;
    alignas(16) std::array<Frame,266> down_{};
    alignas(16) std::array<Frame,134> up_{};
    Kernel downKernel_=nullptr,upEvenKernel_=nullptr,upOddKernel_=nullptr;
    DownBlock downBlock_=nullptr;UpBlock upBlock_=nullptr;
    std::size_t downCursor_=0,upCursor_=0,physicalCount_=0,logicalCount_=0;bool even_=true,startEven_=true,open_=false;
    bool mirrored_=false;
    friend struct ModalConverterAvx2;
};
struct ModalConverterAvx2 {
    static void down(ModalWetConverter&,std::span<const ModalWetConverter::Frame>,std::span<ModalWetConverter::Frame>) noexcept;
    static void up(ModalWetConverter&,std::span<const ModalWetConverter::Frame>,std::span<ModalWetConverter::Frame>) noexcept;
};
}

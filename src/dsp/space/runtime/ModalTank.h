#pragma once
#include "SpaceReverbModel.h"
#include "SpaceReverbPreparation.h"
#include <array>
#include <cstddef>
#include <memory>
#include <span>
namespace airack::space::runtime {
inline constexpr std::size_t maximumDirectModes=2048;
enum class ModalBackend {scalar,sse2,automatic,avx2};
struct PreparedModal {
    double sampleRate=0,size=.5,pickup=0,weightedRadius=1;
    std::size_t count=0;
    DecayCurve curve;
    alignas(64) std::array<double,maximumDirectModes> rc{},rs{},bReal{},bImag{},frequency{},target{};
    std::array<std::array<double,2>,maximumDirectModes/4> inputWeights{};
    bool operator==(const PreparedModal&) const =default;
};
struct ModalSample {double real=0,imag=0;bool operator==(const ModalSample&) const=default;};
void advanceModal(ModalSample&,double rc,double rs,double bReal,double bImag,double input) noexcept;
bool prepareModal(const DecayCurve&,double size,double physicalRate,std::size_t count,PreparedModal&,detail::PreparationControl={}) noexcept;
class ModalTank {
public:
    using Frame=std::array<double,2>;
    static constexpr std::size_t maximumBlockFrames=512;
    ModalTank();~ModalTank();
    ModalTank(const ModalTank&)=delete;ModalTank& operator=(const ModalTank&)=delete;
    bool prepareStorage(double rate,ModalBackend=ModalBackend::scalar,bool pairedReduction=true);
    bool configure(const PreparedModal&) noexcept;
    bool adoptCopied(const PreparedModal&,const ModalTank&) noexcept;
    void reset() noexcept;
    bool processBlock(std::span<Frame>) noexcept;
    Frame tick(Frame) noexcept;
    ModalSample modeState(std::size_t) const noexcept;
    const PreparedModal& prepared() const noexcept;
    std::size_t storageBytes() const noexcept;
    const char* backendName() const noexcept;
    bool usesPairedReduction() const noexcept;
private:
    bool configureImpl(const PreparedModal&,bool factory) noexcept;
    bool adoptPreparedCopied(const PreparedModal&,const ModalTank&) noexcept;
    friend class ModalRoom;
    struct State;std::unique_ptr<State> state_;
};
}

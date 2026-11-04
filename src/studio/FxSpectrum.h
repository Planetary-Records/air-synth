#pragma once
#include "FxMonitor.h"
#include <array>
#include <cstdint>
#include <memory>

namespace airack::studio
{
// Message-thread analyzer. The render producer knows only FxMonitor, and never
// calls an FFT or dereferences this object. Work is bounded to one FFT window
// per update, keeping the newest complete50%-hop window when catching up.
class FxSpectrum final
{
public:
    static constexpr int fftOrder=14,fftSize=1<<fftOrder,bins=fftSize/2+1;
    static constexpr float floorDb=-120;
    struct Result
    {
        bool valid=false;
        double sampleRate=0;
        std::uint64_t generation=0,windows=0,skipped=0;
        std::uint64_t endSample=0,continuity=0;
        std::array<float,bins> inputDb{},outputDb{};
        std::array<float,8> movementDb{};
        std::array<float,8> spectralMovementDb{};
        std::array<DynamicEqDetectorReadback,8> detectorReadbacks{};
        bool detectorReadbacksValid=false,externalAvailable=false;
    };
    FxSpectrum();
    ~FxSpectrum();
    FxSpectrum(const FxSpectrum&)=delete;
    FxSpectrum& operator=(const FxSpectrum&)=delete;
    // Only the construction/message thread may consume; another thread fails
    // without dequeuing samples, changing state or executing an FFT.
    // Metadata-only updates drain packets without accumulating audio or running FFTs.
    bool update(FxMonitor&,FxMonitor::Token,bool analyseSpectrum=true) noexcept;
    bool update(FxMonitor&,FxMonitor::Token,bool analyseSpectrum,std::size_t& remainingPackets) noexcept;
    void clear() noexcept;
    const Result& result() const noexcept;
    float dbAt(double frequencyHz,bool output) const noexcept;
    static float dbAt(const Result&,double frequencyHz,bool output) noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
}

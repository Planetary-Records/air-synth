#pragma once
#include "DynamicEq.h"

namespace airack {
enum class SpectralEqWork { immediate, bounded };
struct SpectralEqWorkStats {
 std::uint64_t framesStarted=0,framesCompleted=0,deadlineMisses=0;
 std::uint64_t maximumSliceWork=0,maximumSliceAllowance=0;
};
// Feasibility prototype; not connected to the product audio path.
// shortcut: live bypass transitions need qualification before product adoption.
class SpectralEq final {
public:
 SpectralEq();
 ~SpectralEq();
 SpectralEq(const SpectralEq&)=delete;
 SpectralEq& operator=(const SpectralEq&)=delete;
 bool prepare(double sampleRate,int maximumBlockSize,int referenceWindow=4096,int hopOffset=0,
              SpectralEqWork work=SpectralEqWork::immediate);
 bool setSettings(const DynamicEqSettings&) noexcept;
 void processBlock(std::span<DynamicEq::Frame>) noexcept;
 void reset() noexcept;
 int latencySamples() const noexcept;
 int transformSize() const noexcept;
 SpectralEqWorkStats workStats() const noexcept;
 std::size_t preparedBytes() const noexcept;
 std::uint64_t transformsPerformed() const noexcept;
 std::array<float,8> dynamicGainsDb() const noexcept;
 std::size_t weightedBinCount(std::size_t band) const noexcept;
private:
 struct State;
 std::unique_ptr<State> state;
};
}

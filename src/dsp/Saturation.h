#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace airack {
enum class SaturationMode : std::uint8_t { fullBand, threeBand };
enum class SaturationCharacter : std::uint8_t { soft, glow, dense };
enum class SaturationLane : std::uint8_t { full, low, mid, high };
struct SaturationLaneSettings {
    bool bypassed = false;
    SaturationCharacter character = SaturationCharacter::soft;
    float driveDb = 0, preTiltDb = 0, mix = 1, trimDb = 0;
    bool operator==(const SaturationLaneSettings&) const = default;
};
struct SaturationSettings {
    SaturationMode mode = SaturationMode::fullBand;
    float inputTrimDb = 0, outputTrimDb = 0, mix = 1;
    bool driveCompensation = true;
    float lowCrossoverHz = 180, highCrossoverHz = 3500;
    bool preLowCutEnabled = false; float preLowCutHz = 20;
    bool postHighCutEnabled = false; float postHighCutHz = 20000;
    std::array<SaturationLaneSettings, 4> lanes {};
    bool operator==(const SaturationSettings&) const = default;
};
struct SaturationCrossovers { double lowHz, highHz; };
bool validateSaturation(const SaturationSettings&) noexcept;
SaturationCrossovers effectiveSaturationCrossovers(const SaturationSettings&, double rate) noexcept;
float saturationDriveCompensationDb(SaturationCharacter, float driveDb) noexcept;
double saturationTailSeconds(const SaturationSettings&, double rate) noexcept;

class Saturation {
public:
    using Frame = std::array<double, 2>;
    Saturation();
    ~Saturation();
    Saturation(const Saturation&) = delete;
    Saturation& operator=(const Saturation&) = delete;
    bool prepare(double effectiveSampleRate) noexcept;
    bool setSettings(const SaturationSettings&) noexcept;
    const SaturationSettings& settings() const noexcept;
    Frame process(Frame) noexcept;
    void processBlock(std::span<Frame>) noexcept;
    void reset() noexcept;
    bool setAudition(std::optional<SaturationLane>) noexcept;
    bool auditioning() const noexcept;
    void cancelAudition() noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
}

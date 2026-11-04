#pragma once
#include <array>
#include <cstdint>
#include <memory>
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

namespace probe {
double shape(SaturationCharacter, double x) noexcept;
double primitive(SaturationCharacter, double x) noexcept;
double adaa(SaturationCharacter, double x, double previous) noexcept;
// Probe-only numerical witnesses for the prepared evaluator. The three scalar
// helpers above remain independent analytic oracles.
double preparedPrimitive(SaturationCharacter, double x) noexcept;
double preparedAdaa(SaturationCharacter, double x, double previous) noexcept;
bool preparedAdaaUsesExactPrimitive(double x, double previous) noexcept;
double advanceSag(double inputPeak, double rate, double& envelope) noexcept;
struct CoefficientView { const char* name; std::span<const double> values; };
std::span<const CoefficientView> saturationCoefficientManifest() noexcept;
std::span<const double> saturationCompensationTable(SaturationCharacter) noexcept;
class Candidate {
public:
    using Frame = std::array<double, 2>;
    Candidate();
    ~Candidate();
    Candidate(const Candidate&) = delete;
    Candidate& operator=(const Candidate&) = delete;
    bool prepare(double sampleRate, unsigned factor) noexcept;
    bool setSettings(const SaturationSettings&) noexcept;
    Frame process(Frame) noexcept;
    void reset() noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
}

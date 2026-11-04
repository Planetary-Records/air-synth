#pragma once
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
namespace airack
{
inline constexpr std::size_t dynamicEqBandCount = 8;
enum class DynamicEqProcessingMode : std::uint8_t { ordinary, spectral };
enum class DynamicEqShape : std::uint8_t { bell, lowShelf, highShelf, lowCut, highCut, notch, bandPass, tiltShelf, flatTilt };
enum class DynamicEqSlope : std::uint8_t { db12, db24, db6, db36, db48, db96 };
enum class DynamicEqPlacement : std::uint8_t { stereo, left, right, mid, side };
enum class DynamicEqDetectorSource : std::uint8_t { internalBand, internalFree, externalBand, externalFree };
struct DynamicEqBand
{
    bool active = false, bypassed = false;
    DynamicEqShape shape = DynamicEqShape::bell;
    float frequencyHz = 1000, gainDb = 0, q = 1;
    DynamicEqSlope slope = DynamicEqSlope::db12;
    float rangeDb = 0, thresholdDb = -24, attackMs = 10, releaseMs = 100;
    DynamicEqPlacement placement = DynamicEqPlacement::stereo;
    bool autoThreshold = false, adaptiveTiming = false;
    DynamicEqDetectorSource detectorSource = DynamicEqDetectorSource::internalBand;
    float detectorLowHz = 20, detectorHighHz = 20000;
    bool spectralEnabled = false;
    float spectralDensity = .5f, spectralTiltDbOct = 0;
    bool operator==(const DynamicEqBand&) const = default;
};
struct DynamicEqSettings
{
    std::array<DynamicEqBand, dynamicEqBandCount> bands {};
    float outputGainDb = 0;
    bool autoGain = false;
    float gainScale = 1;
    DynamicEqProcessingMode processingMode = DynamicEqProcessingMode::ordinary;
    bool operator==(const DynamicEqSettings&) const = default;
};
struct DynamicEqDetectorReadback
{
    float thresholdDb = -24, attackMs = 10, releaseMs = 100;
};
bool validateDynamicEq(const DynamicEqSettings&) noexcept;
// Settings-based estimate in dB; invalid settings return NaN.
float dynamicEqAutoGainDb(const DynamicEqSettings&) noexcept;
// Allocation-free steady-configuration engineering bound; invalid input is NaN.
double dynamicEqTailSeconds(const DynamicEqSettings&, double sampleRate) noexcept;
float dynamicEqTargetGainDb(float levelDb, float thresholdDb, float rangeDb) noexcept;
std::optional<std::complex<double>> dynamicEqBandResponse(const DynamicEqBand&, double sampleRate, double frequencyHz,
    float dynamicGainDb = 0, float gainScale = 1) noexcept;
// Product of selected-component transfers; a full channel response only when
// all enabled bands use Stereo. Component views use the ordered matrix below.
std::optional<std::complex<double>> dynamicEqResponse(const DynamicEqSettings&, double sampleRate, double frequencyHz,
    const std::array<float, dynamicEqBandCount>& dynamics = {}) noexcept;
// Row-major LL/LR/RL/RR, with optional frozen detector movement for rendering.
std::optional<std::array<std::complex<double>,4>> dynamicEqMatrixResponse(const DynamicEqSettings&,
    double sampleRate, double frequencyHz, const std::array<float, dynamicEqBandCount>& dynamics = {}) noexcept;
// RMS output magnitude for unit uncorrelated stereo input. This is a modeled
// response, not a measured component spectrum. Stereo keeps the scalar path.
std::optional<double> dynamicEqEnergyResponse(const DynamicEqSettings&,double sampleRate,double frequencyHz,
    const std::array<float, dynamicEqBandCount>& dynamics = {}) noexcept;
// Control-thread curve preparation; render-owned filters and histories are untouched.
class PreparedDynamicEqResponse final
{
public:
    PreparedDynamicEqResponse();
    ~PreparedDynamicEqResponse();
    PreparedDynamicEqResponse(const PreparedDynamicEqResponse&)=delete;
    PreparedDynamicEqResponse& operator=(const PreparedDynamicEqResponse&)=delete;
    bool prepare(const DynamicEqSettings&,double sampleRate,
        const std::array<float,dynamicEqBandCount>& dynamics={}) noexcept;
    std::optional<double> energy(double frequencyHz) const noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
class DynamicEq
{
public:
    using Frame = std::array<double, 2>;
    DynamicEq();
    ~DynamicEq();
    DynamicEq(const DynamicEq&) = delete;
    DynamicEq& operator=(const DynamicEq&) = delete;
    bool prepare(double sampleRate) noexcept;
    bool setSettings(const DynamicEqSettings&) noexcept;
    const DynamicEqSettings& settings() const noexcept;
    Frame process(Frame) noexcept;
    Frame process(Frame, Frame external) noexcept;
    void processBlock(std::span<Frame>) noexcept;
    bool processBlock(std::span<Frame>,std::span<const Frame> external) noexcept;
    void reset() noexcept;
    // Transient render-owner monitoring; never persisted with settings.
    bool setAudition(std::optional<std::size_t>) noexcept;
    bool setDetectorAudition(std::optional<std::size_t>) noexcept;
    bool auditioning() const noexcept;
    void cancelAudition() noexcept;
    // Render-owner readback: nominal endpoint movement actually blended,
    // relative to the static path, including its current bypass/structural fade.
    // Coefficient ramps settle independently; this is not a transient FFT.
    std::array<float, dynamicEqBandCount> dynamicGainsDb() const noexcept;
    std::array<DynamicEqDetectorReadback,dynamicEqBandCount> detectorReadbacks() const noexcept;
    double tailSeconds() const noexcept;
private:
    void processBlockImpl(std::span<Frame>,std::span<const Frame>) noexcept;
    struct State;
    std::unique_ptr<State> state;
};
}

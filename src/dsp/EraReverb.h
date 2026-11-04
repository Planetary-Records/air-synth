#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace airack
{
enum class EraVoicing : std::uint8_t { hall, space, tape, plate, room, gate };
enum class EraAge : std::uint8_t { dark, warm, clear };
inline constexpr std::size_t eraVoicingCount = 6, eraAgeCount = 3;

struct EraReverbSettings
{
    EraVoicing voicing = EraVoicing::hall;
    EraAge age = EraAge::dark;
    float decaySeconds = 4, predelayMs = 20, size = 1, attack = .5f, diffusion = 1;
    float wobble = .38f, wobbleRateHz = 2.53f, grit = 0;
    float bassMultiplier = 1.5f, dampingHz = 6000, lowCutHz = 10, highCutHz = 8000, mix = .25f;
    bool operator==(const EraReverbSettings&) const = default;
};

bool validateEraReverb(const EraReverbSettings&) noexcept;
// The engine's tail rule (amplitude below 1e-6 / 9) for these settings; NaN when invalid.
double eraReverbTailSeconds(const EraReverbSettings&) noexcept;

class EraReverb
{
public:
    using Frame = std::array<double, 2>;
    EraReverb();
    ~EraReverb();
    EraReverb(const EraReverb&) = delete;
    EraReverb& operator=(const EraReverb&) = delete;
    // Allocates every buffer for the rate; false outside 8 to 768 kHz or when memory runs out.
    bool prepare(double sampleRate) noexcept;
    bool setSettings(const EraReverbSettings&) noexcept;
    const EraReverbSettings& settings() const noexcept;
    Frame process(Frame) noexcept;
    void processBlock(std::span<Frame>) noexcept;
    // Clears all sound and applies the current settings at once, without glides or fades.
    void reset() noexcept;
private:
    struct State;
    std::unique_ptr<State> state;
};
}

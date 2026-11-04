#pragma once
#include <array>
#include <optional>

namespace airack::space
{
enum class DecayShape { bell, lowShelf, highShelf };
struct DecayBand
{
    bool active=false, bypassed=false;
    DecayShape shape=DecayShape::bell;
    double frequencyHz=1000, multiplier=1, q=1;
    bool operator==(const DecayBand&) const = default;
};
struct DecayCurve
{
    double decaySeconds=2, brightness=.5;
    std::array<DecayBand,6> bands {};
    bool operator==(const DecayCurve&) const = default;
};
struct DecayPoint
{
    double effectiveHz=0, seconds=0, unclampedSeconds=0;
    bool frequencyLimited=false, decayLimited=false;
};
bool valid(const DecayCurve&) noexcept;
// Preparation/UI operation; never called once per audio sample.
// No mutation, hidden allocation or coefficient fitting.
std::optional<DecayPoint> evaluate(const DecayCurve&,double requestedHz,double sampleRate) noexcept;
}


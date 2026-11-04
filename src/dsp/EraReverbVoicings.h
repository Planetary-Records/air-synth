#pragma once
#include "EraReverb.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace airack::era
{
enum class WobbleKind : std::uint8_t { chorus, random, tape };
struct Voicing { std::array<double, 8> tankSeconds; WobbleKind wobble; };
// Tank line lengths at Size 1; Size scales them, and the diffusers, by sizeScale.
inline constexpr std::array<Voicing, eraVoicingCount> voicings {{
    { { .0531, .0637, .0719, .0823, .0947, .1069, .1171, .1303 }, WobbleKind::chorus }, // hall
    { { .0717, .0861, .0973, .1109, .1279, .1441, .1583, .1759 }, WobbleKind::random }, // space
    { { .0491, .0593, .0673, .0761, .0883, .0997, .1091, .1213 }, WobbleKind::tape },   // tape
    { { .0217, .0263, .0311, .0359, .0409, .0467, .0523, .0587 }, WobbleKind::chorus }, // plate
    { { .0197, .0241, .0283, .0331, .0373, .0421, .0467, .0521 }, WobbleKind::chorus }, // room
    { { .0531, .0637, .0719, .0823, .0947, .1069, .1171, .1303 }, WobbleKind::chorus }, // gate: the tank is idle
}};
inline constexpr double longestTankSeconds = .1759;
inline constexpr std::size_t diffuserStages = 8;
inline constexpr std::array<std::array<double, diffuserStages>, 2> diffuserSeconds {{
    { .0019, .0023, .0029, .0037, .0047, .0071, .0113, .0167 }, { .0021, .0026, .0031, .0041, .0053, .0079, .0127, .0181 } }};
// Room's early reflections of the undiffused input at Size 1, per channel, and their shared gains (sum of squares
// 0.5, so the reflections stay below the tail's energy and the decay fit reads the tail).
inline constexpr std::array<std::array<double, 12>, 2> roomEarlySeconds {{
    { .0031, .0074, .0119, .0163, .0218, .0271, .0337, .0402, .0468, .0537, .0611, .0692 },
    { .0043, .0089, .0131, .0187, .0233, .0296, .0352, .0427, .0489, .0563, .0638, .0717 } }};
inline constexpr std::array<double, 12> roomEarlyGains { .33, -.29, .26, -.24, .21, -.19, .17, -.15, .13, -.12, .11, -.09 };
// The Gate's diffuser delays as multiples of its tap spacing, per channel.
inline constexpr std::array<std::array<double, 4>, 2> gateDiffuserRatios {{ { .31, .53, .79, 1.13 }, { .37, .59, .83, 1.07 } }};
inline double sizeScale(double size) noexcept { return .1 + .9 * size; }
// The swell taps' spread at Attack 1 per voicing (hall, space, tape, plate, room, gate). Each tank's build-up adds 90
// to 170 ms, so these put Attack 1's peak near the record's 253 ms (Room 158 ms) after the input. Up to Attack 0.5
// every voicing spreads 0.231 x Attack squared; above it the spread moves linearly in Attack squared to its own value.
inline constexpr std::array<double, 6> swellSpreadSeconds { .127, .077, .109, .142, .072, .231 };
inline double swellSeconds(EraVoicing voicing, double attack) noexcept
{
    const double spread = swellSpreadSeconds[std::size_t(voicing)], squared = attack * attack, middle = .231 * .25;
    if (voicing == EraVoicing::room) return spread;
    return squared <= .25 ? .231 * squared : middle + (spread - middle) * (squared - .25) / .75;
}
inline double gateLengthSeconds(double size) noexcept { return .1 + 1.11 * size * size; }
inline double gateCloseSeconds(double decaySeconds) noexcept { return std::clamp(.1 * decaySeconds, .01, 1.0); }
}

#pragma once
#include "SaturationCandidate.h"
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

// Only case/settings/stimulus data are shared by the two process images.
namespace airack::probe::charged {
constexpr unsigned prefixFrames = 4096;
struct Case {
    std::string id;
    SaturationSettings from, to;
    enum Prefix { musical, nyquist, dc } prefix = musical;
};
inline SaturationSettings profile(unsigned mode, unsigned character, bool linear = false) {
    SaturationSettings s;
    s.mode = SaturationMode(mode); s.driveCompensation = false;
    s.preLowCutHz = 600; s.postHighCutHz = 1200;
    for (auto& lane : s.lanes) {
        lane.character = SaturationCharacter(character);
        lane.driveDb = linear ? 0.f : 18.f;
        lane.preTiltDb = linear ? 0.f : 3.f;
    }
    return s;
}
inline std::vector<Case> cases() {
    std::vector<Case> result;
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned from = 0; from < 3; ++from)
            for (unsigned to = 0; to < 3; ++to) if (from != to)
                result.push_back({ "character-" + std::to_string(mode) + '-' + std::to_string(from) + '-' + std::to_string(to),
                                   profile(mode, from), profile(mode, to) });
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned character = 0; character < 3; ++character)
            result.push_back({ "mode-" + std::to_string(mode) + '-' + std::to_string(character),
                               profile(mode, character), profile(1 - mode, character) });
    for (unsigned cut = 0; cut < 2; ++cut) for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned enabled = 0; enabled < 2; ++enabled) {
            auto from = profile(mode, 0, true), to = from;
            (cut ? from.postHighCutEnabled : from.preLowCutEnabled) = enabled != 0;
            (cut ? to.postHighCutEnabled : to.preLowCutEnabled) = enabled == 0;
            result.push_back({ std::string(cut ? "post-" : "pre-") + std::to_string(mode) + '-' + std::to_string(enabled),
                               from, to, cut ? Case::dc : Case::nyquist });
        }
    return result;
}
inline Candidate::Frame input(const Case& c, unsigned frame, double rate) {
    if (frame < prefixFrames) {
        if (c.prefix == Case::nyquist) {
            const double sign = frame % 2 ? -1. : 1.;
            return { sign / 16, -sign / 32 };
        }
        // Predeclared dyadic DC keeps coefficient-cancellation roundoff below
        // 1e-12 at 64x/96k while leaving a measurable charged filter history.
        if (c.prefix == Case::dc) return { 1. / 4096, -1. / 8192 };
    }
    const double t = double(frame) / rate, tau = 2 * std::numbers::pi;
    return { .021 * std::sin(tau * 307 * t) + .008 * std::cos(tau * 7103 * t),
             .017 * std::cos(tau * 983 * t) - .005 * std::sin(tau * 11371 * t) };
}
}

#pragma once
#include "SaturationCandidate.h"
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

// Frozen before capture: full-size stereo, charged prefix, native 20ms ramp.
namespace airack::probe::control_law {
constexpr unsigned prefixFrames = 4096, settledFrames = 128;
struct Case {
    std::string id;
    unsigned mode, character, mask;
    float drive;
    bool compensation, reverse;
    SaturationSettings stationary() const {
        SaturationSettings s;
        s.mode = SaturationMode(mode); s.driveCompensation = false;
        for (unsigned n = 0; n < 4; ++n) {
            s.lanes[n].character = SaturationCharacter(character);
            s.lanes[n].driveDb = drive;
            s.lanes[n].bypassed = (mask & (1u << n)) == 0;
        }
        return s;
    }
    SaturationSettings endpoint(bool on) const {
        auto s = stationary();
        if (compensation) s.driveCompensation = on;
        else for (unsigned n = 0; n < 4; ++n)
            if (mask & (1u << n)) s.lanes[n].bypassed = !on;
        return s;
    }
};
inline std::vector<Case> cases() {
    std::vector<Case> result;
    for (bool compensation : { true, false }) for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned character = 0; character < 3; ++character) for (float drive : { 18.f, 35.5f })
            for (unsigned mask : { mode == 0 ? 1u : 14u, 2u, 4u, 8u }) {
                if ((mode == 0 || compensation) && mask != (mode == 0 ? 1u : 14u)) continue;
                for (bool reverse : { false, true }) {
                    const auto id = std::string(compensation ? "comp" : "bypass") + '-' + std::to_string(mode)
                        + '-' + std::to_string(character) + (drive == 18 ? "-18-" : "-35.5-")
                        + std::to_string(mask) + '-' + (reverse ? "off" : "on");
                    result.push_back({ id, mode, character, mask, drive, compensation, reverse });
                }
            }
    return result;
}
inline Candidate::Frame input(unsigned frame, double rate) {
    const double t = double(frame) / rate, tau = 2 * std::numbers::pi;
    return { .021 * std::sin(tau * 307 * t) + .008 * std::cos(tau * 7103 * t),
             .017 * std::cos(tau * 983 * t) - .005 * std::sin(tau * 11371 * t) };
}
inline double weight(const Case& c, unsigned frame, unsigned fade, int offset = 0) {
    if (frame < prefixFrames) return c.reverse ? 1 : 0;
    const double p = std::min(1., double(frame - prefixFrames + 1 + offset) / fade);
    return c.reverse ? 1 - p : p;
}
}

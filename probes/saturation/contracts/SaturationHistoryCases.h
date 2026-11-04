#pragma once
#include "SaturationCandidate.h"
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

// Frozen before capture: no convergence search or rescaling after a result.
namespace airack::probe::history {
constexpr unsigned prefixFrames = 4096, settledFrames = 128, witnessFrames = 64;
constexpr double commonSeconds = 2, dcLeft = .05, dcRight = -.0375;
struct Case {
    std::string id;
    unsigned mode, character;
    bool modes, zero, pre;
    SaturationSettings settings(unsigned phase, unsigned history) const {
        SaturationSettings s;
        s.mode = SaturationMode(mode); s.driveCompensation = false;
        s.preLowCutHz = 600; s.postHighCutHz = 1200;
        for (unsigned n = 0; n < 4; ++n) {
            const bool inA = (mode == 0) == (n == 0);
            s.lanes[n].character = SaturationCharacter(!modes || inA ? character : 0);
            s.lanes[n].driveDb = 18;
        }
        if (modes) {
            // Fresh starts in B and has never visited A; both banks keep their character.
            if (phase == 1 || (phase == 0 && history == 2)) s.mode = SaturationMode(1 - mode);
        } else {
            const bool enabled = phase == 2 || (phase == 0 && history == 1);
            if (pre) s.preLowCutEnabled = enabled;
            else s.postHighCutEnabled = enabled;
        }
        return s;
    }
};
inline std::vector<Case> cases(bool modes) {
    std::vector<Case> result;
    if (modes) {
        for (unsigned mode = 0; mode < 2; ++mode) for (unsigned character = 0; character < 3; ++character)
            for (bool zero : { true, false }) result.push_back({ "mode-" + std::to_string(mode) + '-'
                + std::to_string(character) + (zero ? "-zero" : "-music"), mode, character, true, zero, false });
    } else {
        for (bool pre : { true, false }) for (unsigned mode = 0; mode < 2; ++mode)
            for (unsigned character = 0; character < 3; ++character) result.push_back({
                std::string(pre ? "pre-" : "post-") + std::to_string(mode) + '-' + std::to_string(character),
                mode, character, false, false, pre });
    }
    return result;
}
inline Candidate::Frame input(unsigned frame, double rate, unsigned history, const Case& c) {
    const double t = double(frame) / rate, tau = 2 * std::numbers::pi;
    if (frame < prefixFrames) {
        if (history == 1) return { -.019 * std::cos(tau * 509 * t) + .006 * std::sin(tau * 5381 * t),
                                  .016 * std::sin(tau * 1409 * t) + .007 * std::cos(tau * 9973 * t) };
    } else if (!c.modes) return { dcLeft, dcRight };
    else if (c.zero) return {};
    return { .021 * std::sin(tau * 307 * t) + .008 * std::cos(tau * 7103 * t),
             .017 * std::cos(tau * 983 * t) - .005 * std::sin(tau * 11371 * t) };
}
}

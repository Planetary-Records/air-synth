#pragma once
#include "SaturationCandidate.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace airack::probe::control_law {
// Probe-only LTI return. No Candidate state/methods or nonlinear equations.
// Allpass sections use y[n]=a*x[n]+x[n-1]-a*y[n-1]; FIR is ordinary convolution.
class ReturnOracle {
    struct Section {
        double coefficient, previousInput = 0, previousOutput = 0;
        double apply(double input) {
            const double output = coefficient * input + previousInput - coefficient * previousOutput;
            previousInput = input; previousOutput = output; return output;
        }
    };
    struct Channel {
        std::vector<Section> direct, delayed;
        std::vector<double> taps, history;
        std::size_t head = 0;
        double hold = 0;
        static double cascade(double x, std::vector<Section>& sections) {
            for (auto& section : sections) x = section.apply(x);
            return x;
        }
        void push(double x) {
            head = (head + history.size() - 1) % history.size(); history[head] = x;
        }
        double pair(double even, double odd) {
            if (taps.empty()) {
                const double result = .5 * (hold + cascade(even, direct));
                hold = cascade(odd, delayed); return result;
            }
            push(even);
            double result = 0;
            auto index = head;
            for (const double tap : taps) {
                result += tap * history[index];
                if (++index == history.size()) index = 0;
            }
            push(odd); return result;
        }
    };
    std::vector<std::array<Channel, 2>> stages;
public:
    using Frame = Candidate::Frame;
    ReturnOracle(unsigned factor, std::span<const CoefficientView> manifest) {
        const auto find = [&](const std::string& name) {
            for (const auto& item : manifest) if (item.name == name) return item.values;
            throw std::runtime_error("missing return coefficient " + name);
        };
        for (unsigned scale = 1; scale < factor; scale *= 2) {
            const auto number = std::to_string(stages.size() + 1);
            std::array<Channel, 2> stage;
            for (auto& channel : stage) {
                if (factor == 64) {
                    const auto taps = find("reference-fir" + number);
                    channel.taps.assign(taps.begin(), taps.end()); channel.history.resize(taps.size());
                } else {
                    for (double a : find("iir" + number + "-down-direct")) channel.direct.push_back({ a });
                    for (double a : find("iir" + number + "-down-delayed")) channel.delayed.push_back({ a });
                }
            }
            stages.push_back(std::move(stage));
        }
    }
    void reset() {
        for (auto& stage : stages) for (auto& channel : stage) {
            for (auto& s : channel.direct) s.previousInput = s.previousOutput = 0;
            for (auto& s : channel.delayed) s.previousInput = s.previousOutput = 0;
            std::fill(channel.history.begin(), channel.history.end(), 0);
            channel.hold = 0; channel.head = 0;
        }
    }
    Frame process(std::array<Frame, 64> frames, unsigned factor) {
        unsigned count = factor;
        for (std::size_t stage = stages.size(); stage-- > 0;) {
            count /= 2;
            for (unsigned n = 0; n < count; ++n) for (unsigned ch = 0; ch < 2; ++ch)
                frames[n][ch] = stages[stage][ch].pair(frames[2 * n][ch], frames[2 * n + 1][ch]);
        }
        return frames[0];
    }
};
}

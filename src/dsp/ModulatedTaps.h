#pragma once
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace airack::detail
{
// Revision 2's modulated-tap read: 16 points of a Kaiser-windowed sinc (beta 6,
// cutoff at Nyquist) tabulated at 128 fractions, each row normalized to unit DC
// gain, blended linearly between rows. Whole-sample delays read exactly.
class SincTapTable
{
public:
    static constexpr int points = 16, phases = 128;
    // Samples newer than the tap's whole part that the kernel reads.
    static constexpr std::size_t lead = points / 2 - 1;
    // Built on first use; prepare() takes it on the control thread.
    static const SincTapTable& shared()
    {
        static const SincTapTable table;
        return table;
    }
    const double* row(int phase) const noexcept { return weights.data() + std::size_t(phase) * points; }
private:
    SincTapTable()
    {
        const auto besselI0 = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 60; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
            return sum;
        };
        constexpr double beta = 6, half = points / 2;
        for (int p = 0; p <= phases; ++p)
        {
            double* w = weights.data() + std::size_t(p) * points;
            double sum = 0;
            for (int j = 0; j < points; ++j)
            {
                const double u = double(j - int(lead)) - double(p) / phases, r = u / half;
                if (p == 0 || p == phases) w[j] = u == 0 ? 1 : 0;
                else w[j] = std::abs(r) < 1 ? std::sin(std::numbers::pi * u) / (std::numbers::pi * u)
                        * besselI0(beta * std::sqrt(1 - r * r)) / besselI0(beta) : 0;
                sum += w[j];
            }
            for (int j = 0; j < points; ++j) w[j] /= sum;
        }
    }
    std::array<double, std::size_t(phases + 1) * points> weights {};
};

// Push, then read any delay up to the prepared maximum; delay 0 is the sample
// just pushed. Storage not yet written since clear() reads as zero.
class ModulatedTapRing
{
public:
    void prepare(std::size_t maximumDelay)
    {
        table = &SincTapTable::shared();
        std::size_t size = 1;
        while (size < maximumDelay + SincTapTable::points + 2) size <<= 1;
        data.assign(size, 0.0); mask = size - 1; limit = double(maximumDelay);
        clear();
    }
    void clear() noexcept { newest = 0; written = 0; }
    void push(double x) noexcept
    {
        newest = (newest + 1) & mask; data[newest] = x;
        if (written < data.size()) ++written;
    }
    double read(double delay) const noexcept
    {
        delay = std::clamp(delay, 0.0, limit);
        const auto whole = static_cast<std::size_t>(delay);
        const double fraction = delay - double(whole);
        // Short flanger taps at low rates: the kernel would reach unwritten frames.
        if (whole < SincTapTable::lead) { const double recent = at(whole); return recent + fraction * (at(whole + 1) - recent); }
        const double position = fraction * SincTapTable::phases;
        const int phase = std::min(int(position), SincTapTable::phases - 1);
        const double blend = position - phase;
        const double* lo = table->row(phase);
        const double* hi = lo + SincTapTable::points;
        const std::size_t youngest = whole - SincTapTable::lead;
        double sum = 0;
        if (youngest + SincTapTable::points <= written)
            for (std::size_t j = 0; j < std::size_t(SincTapTable::points); ++j)
                sum += (lo[j] + blend * (hi[j] - lo[j])) * data[(newest - youngest - j) & mask];
        else
            for (std::size_t j = 0; j < std::size_t(SincTapTable::points); ++j)
                sum += (lo[j] + blend * (hi[j] - lo[j])) * at(youngest + j);
        return sum;
    }
private:
    double at(std::size_t age) const noexcept { return age < written ? data[(newest - age) & mask] : 0.0; }
    const SincTapTable* table = nullptr;
    std::vector<double> data;
    std::size_t mask = 0, newest = 0, written = 0;
    double limit = 0;
};

// Revision 2's chorus keeps juce::dsp::Chorus's law: its oscillator, 50 ms
// linear smoothing of depth and feedback, the 1 ms floor and feedback taken
// from the previous output before the push. Only the tap read changes.
class ModulatedChorus
{
public:
    void prepare(double sampleRate, double maximumDelayMs)
    {
        rate = sampleRate;
        for (auto& ring : rings) ring.prepare(std::size_t(std::ceil(maximumDelayMs * rate / 1000)) + 2);
        reset();
    }
    void setRate(double hz) noexcept { frequency.setTargetValue(hz); }
    void setDepth(double depth) noexcept { oscillatorVolume.setTargetValue(depth * .5); }
    void setFeedback(double feedback) noexcept { for (auto& volume : feedbackVolume) volume.setTargetValue(feedback); }
    void setCentreDelay(double ms) noexcept { centre = std::clamp(ms, 1.0, 100.0); }
    void reset() noexcept
    {
        lastOutput = {};
        for (auto& ring : rings) ring.clear();
        phase.reset(); frequency.reset(rate, .05); oscillatorVolume.reset(rate, .05);
        for (auto& volume : feedbackVolume) volume.reset(rate, .05);
    }
    std::array<double, 2> process(const std::array<double, 2>& input) noexcept
    {
        const double increment = (juce::MathConstants<double>::twoPi / rate) * frequency.getNextValue();
        const double modulation = std::sin(phase.advance(increment) - juce::MathConstants<double>::pi) * oscillatorVolume.getNextValue();
        const double delay = std::max(1.0, 20 * modulation + centre) * rate / 1000.0;
        std::array<double, 2> output {};
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            rings[ch].push(input[ch] - lastOutput[ch]);
            output[ch] = rings[ch].read(delay);
            lastOutput[ch] = output[ch] * feedbackVolume[ch].getNextValue();
        }
        return output;
    }
private:
    std::array<ModulatedTapRing, 2> rings;
    juce::dsp::Phase<double> phase;
    juce::SmoothedValue<double> frequency {1}, oscillatorVolume {.125};
    std::array<juce::SmoothedValue<double>, 2> feedbackVolume;
    std::array<double, 2> lastOutput {};
    double rate = 44100, centre = 7;
};
}

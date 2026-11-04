#include "EraReverb.h"
#include "EraReverbParts.h"
#include "EraReverbVoicings.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numbers>

namespace airack
{
namespace
{
using era::Frame;
constexpr int controlInterval = 16, hostInterval = 32;
constexpr double rampSeconds = .03, bassCrossoverHz = 300, highDecayRatio = .375, diffuserGain = .7;
constexpr double chorusDepthSeconds = .00045, randomDepthSeconds = .00045;
constexpr double wowSeconds = .004, wowDepthSeconds = .0012, flutterDepthSeconds = .00008, flutterRatio = 7.3;
constexpr std::array<double, 8> chorusRates { 1, 1.07, .93, 1.13, .89, 1.03, .97, 1.11 };
constexpr std::size_t swellTaps = 16;
constexpr double swellRise = 5;
// The early path leaves the diffusers after the short stages, so Attack 0 peaks at once.
constexpr std::size_t earlyStages = 6;
constexpr std::size_t gateBurstTaps = 48, gateCloseTaps = 32, gateTaps = gateBurstTaps + gateCloseTaps;
constexpr double gateDiffuserGain = .6, gateShapeDb = 16, gateSmearSeconds = .02, gateJitter = .8;
constexpr double fadeSeconds = .02;
constexpr std::size_t clearChunk = 4096;
constexpr double logThreshold = -16.0127351353; // the engine's tail rule, log(1e-6 / 9)
constexpr double lineScale = .35355339059327373; // 1 / sqrt(8)
constexpr std::array<double, 8> inL { 1, -1, 1, -1, 1, -1, 1, -1 }, inR { 1, 1, -1, -1, 1, 1, -1, -1 };
constexpr std::array<double, 8> outL { 1, 1, 1, 1, -1, -1, -1, -1 }, outR { 1, -1, -1, 1, 1, -1, -1, 1 };

constexpr int darkGrainBits = 17;
constexpr double grainHoldSeconds = .1, warmEdgeHz = 18000;
// Dark's tank runs near 24 kHz whatever the host rate; below 44.1 kHz there is nothing to divide.
int rateFactor(double hostRate, EraAge age) noexcept
{
    if (age != EraAge::dark || hostRate < 44100) return 1;
    return hostRate <= 50000 ? 2 : hostRate <= 100000 ? 4 : hostRate <= 200000 ? 8 : hostRate <= 400000 ? 16 : 32;
}
// Places a 4th-order Butterworth so it is 20 dB down at 18 kHz, or at 0.45 of the rate where 18 kHz is out of reach.
double warmCornerHz(double rate) noexcept
{
    const double edge = std::min(warmEdgeHz, .45 * rate);
    return rate / std::numbers::pi * std::atan(std::tan(std::numbers::pi * edge / rate) / std::pow(99.0, .125));
}

bool within(float v, float low, float high) noexcept { return std::isfinite(v) && v >= low && v <= high; }
double filterTail(double hz) noexcept { return -logThreshold / (2 * std::numbers::pi * hz); }
void hadamard(std::array<double, 8>& v) noexcept
{
    for (std::size_t stride = 1; stride < 8; stride *= 2)
        for (std::size_t start = 0; start < 8; start += 2 * stride)
            for (std::size_t j = start; j < start + stride; ++j) {
                const double a = v[j], b = v[j + stride];
                v[j] = a + b;
                v[j + stride] = a - b;
            }
    for (auto& x : v) x *= lineScale;
}
}

bool validateEraReverb(const EraReverbSettings& s) noexcept
{
    return std::size_t(s.voicing) < eraVoicingCount && std::size_t(s.age) < eraAgeCount
        && within(s.decaySeconds, .2f, 70) && within(s.predelayMs, 0, 500) && within(s.size, 0, 1)
        && within(s.attack, 0, 1) && within(s.diffusion, 0, 1) && within(s.wobble, 0, 1)
        && within(s.wobbleRateHz, .1f, 10) && within(s.grit, 0, 1) && within(s.bassMultiplier, .25f, 4)
        && within(s.dampingHz, 100, 20000) && within(s.lowCutHz, 10, 1500) && within(s.highCutHz, 100, 20000)
        && within(s.mix, 0, 1);
}

double eraReverbTailSeconds(const EraReverbSettings& s) noexcept
{
    if (!validateEraReverb(s)) return std::numeric_limits<double>::quiet_NaN();
    const double common = s.predelayMs * .001 + filterTail(s.lowCutHz) + filterTail(s.highCutHz) + .01;
    if (s.voicing == EraVoicing::gate)
        return common + era::gateLengthSeconds(s.size) + 1.62 * era::gateCloseSeconds(s.decaySeconds) + .03;
    return common + era::swellSeconds(s.voicing, s.attack) + .1
        + -logThreshold / std::log(1000.0) * s.decaySeconds * std::max(1.0, double(s.bassMultiplier));
}

struct EraReverb::State
{
    EraReverbSettings settings, running;
    double hostRate = 0, rate = 0;
    int hostCount = 0, internalCount = 0;
    bool cutsDirty = true, coefficientsDirty = true, snapDelays = true;
    era::Ramp mix, predelaySamples, lowCutHz, highCutHz;
    era::Ramp decay, size, attack, diffusion, wobble, wobbleRate, bass, damping;
    std::array<era::DelayLine, 2> predelay, injection, source;
    std::array<era::OnePole, 2> lowCut, highCut;
    std::array<std::array<era::DelayLine, era::diffuserStages>, 2> diffusers;
    std::array<std::array<double, era::diffuserStages>, 2> diffuserFrom {}, diffuserTo {};
    std::array<era::DelayLine, 8> lines;
    std::array<double, 8> baseLine {}, lineFrom {}, lineTo {}, lineGain {};
    std::array<era::Biquad, 8> lowShelf, highShelf;
    std::array<era::DelayLine, 2> wowLine;
    std::array<double, 8> wobbleOffset {}, lfoPhase {}, randomTarget {}, randomValue {}, randomSmooth {};
    double wowFrom = 0, wowTo = 0, wowPhase = 0, flutterPhase = 1, randomClock = 0;
    std::uint32_t randomSeed = 0x2545F491u;
    std::array<double, swellTaps> swellAt {}, swellGain {};
    std::array<std::array<double, 12>, 2> earlyAt {};
    double earlyGain = 0, reflectionGain = 0, lateGain = 1;
    Frame partlyDiffused {};
    era::RateChanger rates;
    std::array<Frame, era::RateChanger::maximumFactor> expanded {};
    int expandedRead = 0;
    std::array<era::Grain, 2> grain;
    std::array<era::Biquad, 2> warmLow, warmHigh;
    era::Ramp drive;
    std::array<era::OnePole, 2> dcBlock;
    era::DelayLine gateLine;
    std::array<std::array<era::DelayLine, 4>, 2> gateDiffusers;
    std::array<std::array<double, gateTaps>, 2> gateAt {}, gateGain {}, gateOffset {}, gateSign {};
    std::array<std::array<double, 4>, 2> gateDiffuserAt {};
    std::array<double, 2> gateChorusFrom {}, gateChorusTo {};
    std::array<era::Biquad, 2> gateLow, gateHigh;
    enum class Phase : std::uint8_t { running, fadingOut, clearing, fadingIn } phase = Phase::running;
    double fade = 1;
    std::array<era::DelayLine*, 39> clearable {};
    std::size_t clearIndex = 0, clearFrom = 0;

    std::size_t samples(double seconds) const noexcept { return std::size_t(std::ceil(seconds * hostRate)) + 8; }

    bool prepare(double sampleRate)
    {
        hostRate = sampleRate;
        for (auto& d : predelay) d.allocate(samples(.5));
        for (auto& d : injection) d.allocate(samples(.35));
        for (auto& d : source) d.allocate(samples(.075));
        for (auto& side : diffusers) for (auto& d : side) d.allocate(samples(.02));
        for (auto& d : lines) d.allocate(samples(era::longestTankSeconds + .003));
        for (auto& d : wowLine) d.allocate(samples(wowSeconds + wowDepthSeconds + flutterDepthSeconds + .001));
        gateLine.allocate(samples(era::gateLengthSeconds(1) + 1.62 * era::gateCloseSeconds(70) + .01));
        for (auto& side : gateDiffusers) for (auto& d : side) d.allocate(samples(.03));
        std::uint32_t seed = 0x1234567u;
        for (auto& side : gateOffset) for (auto& j : side) j = gateJitter * (era::uniform(seed) - .5);
        for (auto& side : gateSign) for (auto& g : side) g = era::uniform(seed) < .5 ? -1.0 : 1.0;
        era::halfband();
        std::size_t k = 0;
        for (auto& d : injection) clearable[k++] = &d;
        for (auto& d : source) clearable[k++] = &d;
        for (auto& side : diffusers) for (auto& d : side) clearable[k++] = &d;
        for (auto& d : wowLine) clearable[k++] = &d;
        for (auto& d : lines) clearable[k++] = &d;
        clearable[k++] = &gateLine;
        for (auto& side : gateDiffusers) for (auto& d : side) clearable[k++] = &d;
        reset();
        return true;
    }

    void reset() noexcept
    {
        running = settings;
        applyAge();
        for (auto& d : predelay) d.clear();
        for (auto& d : injection) d.clear();
        for (auto& d : source) d.clear();
        for (auto& side : diffusers) for (auto& d : side) d.clear();
        for (auto& d : lines) d.clear();
        gateLine.clear();
        for (auto& side : gateDiffusers) for (auto& d : side) d.clear();
        for (auto& f : gateLow) f.clear();
        for (auto& f : gateHigh) f.clear();
        gateChorusFrom = gateChorusTo = {};
        for (auto& f : lowCut) f.clear();
        for (auto& f : highCut) f.clear();
        for (auto& f : lowShelf) f.clear();
        for (auto& f : highShelf) f.clear();
        for (auto& d : wowLine) d.clear();
        for (std::size_t i = 0; i < 8; ++i) lfoPhase[i] = double(i) * std::numbers::pi / 4;
        wobbleOffset = randomTarget = randomValue = randomSmooth = {};
        wowFrom = wowTo = wowPhase = randomClock = 0;
        flutterPhase = 1;
        randomSeed = 0x2545F491u;
        mix.snap(settings.mix);
        drive.snap(era::gritDrive(settings.grit));
        for (auto& f : dcBlock) { f.highpass(5, hostRate); f.clear(); }
        predelaySamples.snap(std::round(settings.predelayMs * .001 * hostRate));
        lowCutHz.snap(settings.lowCutHz);
        highCutHz.snap(settings.highCutHz);
        decay.snap(settings.decaySeconds);
        size.snap(settings.size);
        attack.snap(settings.attack);
        diffusion.snap(settings.diffusion);
        wobble.snap(settings.wobble);
        wobbleRate.snap(settings.wobbleRateHz);
        bass.snap(settings.bassMultiplier);
        damping.snap(settings.dampingHz);
        hostCount = internalCount = 0;
        phase = Phase::running;
        fade = 1;
        cutsDirty = coefficientsDirty = snapDelays = true;
    }
    void applyAge() noexcept
    {
        const int factor = rateFactor(hostRate, running.age);
        rate = hostRate / factor;
        rates.setFactor(factor);
        expanded = {};
        expandedRead = 0;
        const double step = std::ldexp(1.0, 1 - darkGrainBits);
        const int hold = int(grainHoldSeconds * rate);
        grain[0].set(step, hold, 0x9E3779B9u);
        grain[1].set(step, hold, 0x7F4A7C15u);
        for (std::size_t c = 0; c < 2; ++c) {
            warmLow[c].lowpass(warmCornerHz(rate), .5412, rate);
            warmHigh[c].lowpass(warmCornerHz(rate), 1.3066, rate);
            warmLow[c].clear();
            warmHigh[c].clear();
        }
    }
    Frame ageStage(Frame wet) noexcept
    {
        if (running.age == EraAge::dark)
            for (std::size_t c = 0; c < 2; ++c) wet[c] = grain[c].process(wet[c]);
        else if (running.age == EraAge::warm)
            for (std::size_t c = 0; c < 2; ++c) wet[c] = warmHigh[c].process(warmLow[c].process(wet[c]));
        return wet;
    }

    void hostControl() noexcept
    {
        const int ticks = std::max(1, int(rampSeconds * hostRate / hostInterval)), steps = std::max(1, int(rampSeconds * hostRate));
        mix.to(settings.mix, steps);
        predelaySamples.to(std::round(settings.predelayMs * .001 * hostRate), steps);
        drive.to(era::gritDrive(settings.grit), steps);
        for (auto& f : dcBlock) f.flushState();
        lowCutHz.to(settings.lowCutHz, ticks);
        highCutHz.to(settings.highCutHz, ticks);
        if (lowCutHz.advance() || cutsDirty) for (auto& f : lowCut) f.highpass(lowCutHz.value, hostRate);
        if (highCutHz.advance() || cutsDirty) for (auto& f : highCut) f.lowpass(highCutHz.value, hostRate);
        cutsDirty = false;
        for (auto& f : lowCut) f.flushState();
        for (auto& f : highCut) f.flushState();
    }

    void updateCoefficients() noexcept
    {
        const auto& voicing = era::voicings[std::size_t(running.voicing)];
        const double scale = era::sizeScale(size.value), limit = .45 * rate;
        const double reach = std::clamp((limit - damping.value) / (.1 * rate), 0.0, 1.0);
        const double highRatio = 1 - (1 - highDecayRatio) * reach, corner = std::min(damping.value, limit);
        for (std::size_t i = 0; i < 8; ++i) {
            baseLine[i] = std::max(2.0, std::round(voicing.tankSeconds[i] * scale * rate));
            const double loss = -3 * baseLine[i] / rate / decay.value;
            lineGain[i] = std::pow(10.0, loss);
            lowShelf[i].lowShelf(bassCrossoverHz, std::pow(10.0, loss * (1 / bass.value - 1)), rate);
            highShelf[i].highShelf(corner, std::pow(10.0, loss * (1 / highRatio - 1)), rate);
        }
        // Whole-sample, evenly spaced taps: the linear reads stay exact, and the taps' comb keeps shallow troughs
        // (uneven spacing would cut deep notches into the tail at high frequencies).
        const double step = std::round(era::swellSeconds(running.voicing, attack.value) * rate / (swellTaps - 1));
        const double spread = step * (swellTaps - 1);
        double total = 0, squares = 0;
        for (std::size_t k = 0; k < swellTaps; ++k) {
            const double x = double(k) / (swellTaps - 1);
            swellAt[k] = double(k) * step;
            swellGain[k] = std::exp(swellRise * (x - 1));
            total += swellGain[k];
            squares += swellGain[k] * swellGain[k];
        }
        const double coherence = std::exp(-spread / (.005 * rate));
        const double norm = 1 / std::sqrt(squares + coherence * (total * total - squares));
        for (auto& g : swellGain) g *= norm;
        const bool room = running.voicing == EraVoicing::room;
        const double a = attack.value;
        earlyGain = room ? 0 : (1 - a) * (1 - a);
        reflectionGain = room ? std::cos(std::numbers::pi / 2 * a) : 0;
        lateGain = room ? std::sin(std::numbers::pi / 2 * (.04 + .96 * a)) : 1;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t k = 0; k < 12; ++k) earlyAt[c][k] = std::round(era::roomEarlySeconds[c][k] * scale * rate);
        updateGate();
    }
    void updateGate() noexcept
    {
        const double length = era::gateLengthSeconds(size.value), close = era::gateCloseSeconds(decay.value);
        const double span = std::max(.05, length - gateSmearSeconds), shape = std::clamp(2 * attack.value - 1, -1.0, 1.0);
        const double startDb = -gateShapeDb * std::max(shape, 0.0), endDb = -gateShapeDb * std::max(-shape, 0.0);
        const double burstSpacing = span / gateBurstTaps, closeSpacing = 1.62 * close / gateCloseTaps;
        // Closing taps carry the burst's energy per second across the join; the diffusers shrink with short
        // closings so their own ring stays faster than the closing's fall.
        const double density = std::sqrt(closeSpacing / burstSpacing), diffuserSpacing = std::min(burstSpacing, closeSpacing) * rate;
        const double limit = .45 * rate, reach = std::clamp((limit - damping.value) / (.1 * rate), 0.0, 1.0);
        for (std::size_t c = 0; c < 2; ++c) {
            double energy = 0;
            for (std::size_t k = 0; k < gateTaps; ++k) {
                const bool burst = k < gateBurstTaps;
                const double x = (double(burst ? k : k - gateBurstTaps) + .5 + gateOffset[c][k]) / double(burst ? gateBurstTaps : gateCloseTaps);
                const double seconds = burst ? x * span : span + 1.62 * close * x;
                const double db = burst ? startDb + (endDb - startDb) * x : endDb - 37 / close * (seconds - span);
                gateAt[c][k] = seconds * rate;
                gateGain[c][k] = gateSign[c][k] * std::pow(10.0, db / 20) * (burst ? 1 : density);
                if (burst) energy += gateGain[c][k] * gateGain[c][k];
            }
            for (auto& g : gateGain[c]) g /= std::sqrt(energy);
            for (std::size_t s = 0; s < 4; ++s) gateDiffuserAt[c][s] = std::max(1.0, era::gateDiffuserRatios[c][s] * diffuserSpacing);
            gateLow[c].lowShelf(bassCrossoverHz, bass.value, rate);
            gateHigh[c].highShelf(std::min(damping.value, limit), 1 - .75 * reach, rate);
        }
    }

    void advanceWobble() noexcept
    {
        const double dt = controlInterval / rate, depth = wobble.value * rate, twoPi = 2 * std::numbers::pi;
        const auto kind = era::voicings[std::size_t(running.voicing)].wobble;
        if (kind == era::WobbleKind::random) {
            randomClock += wobbleRate.value * dt;
            const bool draw = randomClock >= 1;
            if (draw) randomClock -= 1;
            const double a = 1 - std::exp(-twoPi * wobbleRate.value * dt);
            for (std::size_t i = 0; i < 8; ++i) {
                if (draw) randomTarget[i] = 2 * era::uniform(randomSeed) - 1;
                randomValue[i] += a * (randomTarget[i] - randomValue[i]);
                randomSmooth[i] += a * (randomValue[i] - randomSmooth[i]);
                wobbleOffset[i] = randomDepthSeconds * depth * randomSmooth[i];
            }
        } else {
            for (std::size_t i = 0; i < 8; ++i) {
                lfoPhase[i] = std::fmod(lfoPhase[i] + twoPi * wobbleRate.value * chorusRates[i] * dt, twoPi);
                wobbleOffset[i] = chorusDepthSeconds * depth * std::sin(lfoPhase[i]);
            }
        }
        if (kind == era::WobbleKind::tape) {
            wowPhase = std::fmod(wowPhase + twoPi * wobbleRate.value * dt, twoPi);
            flutterPhase = std::fmod(flutterPhase + twoPi * wobbleRate.value * flutterRatio * dt, twoPi);
            wowFrom = wowTo;
            wowTo = wowSeconds * rate + depth * (wowDepthSeconds * std::sin(wowPhase) + flutterDepthSeconds * std::sin(flutterPhase));
        }
        gateChorusFrom = gateChorusTo;
        for (std::size_t c = 0; c < 2; ++c) gateChorusTo[c] = chorusDepthSeconds * depth * .5 * (1 + std::sin(lfoPhase[4 * c]));
    }

    void internalControl() noexcept
    {
        const int ticks = std::max(1, int(rampSeconds * rate / controlInterval));
        bool moved = coefficientsDirty;
        const auto follow = [&](era::Ramp& ramp, double target) { ramp.to(target, ticks); moved = ramp.advance() || moved; };
        follow(decay, settings.decaySeconds);
        follow(size, settings.size);
        follow(attack, settings.attack);
        follow(diffusion, settings.diffusion);
        follow(wobble, settings.wobble);
        follow(wobbleRate, settings.wobbleRateHz);
        follow(bass, settings.bassMultiplier);
        follow(damping, settings.dampingHz);
        if (moved) updateCoefficients();
        coefficientsDirty = false;
        advanceWobble();
        const double scale = era::sizeScale(size.value);
        for (std::size_t i = 0; i < 8; ++i) { lineFrom[i] = lineTo[i]; lineTo[i] = baseLine[i] + wobbleOffset[i]; }
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t s = 0; s < era::diffuserStages; ++s) {
                diffuserFrom[c][s] = diffuserTo[c][s];
                diffuserTo[c][s] = std::max(1.0, std::round(era::diffuserSeconds[c][s] * scale * rate));
            }
        if (snapDelays) { lineFrom = lineTo; diffuserFrom = diffuserTo; wowFrom = wowTo; gateChorusFrom = gateChorusTo; snapDelays = false; }
        for (auto& f : lowShelf) f.flushState();
        for (auto& f : highShelf) f.flushState();
        for (std::size_t c = 0; c < 2; ++c) { warmLow[c].flushState(); warmHigh[c].flushState(); }
        for (std::size_t c = 0; c < 2; ++c) { gateLow[c].flushState(); gateHigh[c].flushState(); }
    }

    Frame diffuse(Frame x, double t) noexcept
    {
        const double g = diffuserGain * diffusion.value;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t s = 0; s < era::diffuserStages; ++s) {
                auto& line = diffusers[c][s];
                const double y = line.readLinear(diffuserFrom[c][s] + (diffuserTo[c][s] - diffuserFrom[c][s]) * t) - g * x[c];
                line.write(x[c] + g * y);
                x[c] = y;
                if (s + 1 == earlyStages) partlyDiffused[c] = y;
            }
        return x;
    }
    Frame tankFeed(const Frame&) noexcept
    {
        Frame feed {};
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t k = 0; k < swellTaps; ++k) feed[c] += swellGain[k] * injection[c].readLinear(swellAt[k] + 1);
        return feed;
    }
    Frame addEarly(const Frame& early, const Frame& late) noexcept
    {
        Frame wet {};
        for (std::size_t c = 0; c < 2; ++c) {
            double reflections = 0;
            if (reflectionGain > 0)
                for (std::size_t k = 0; k < 12; ++k) reflections += era::roomEarlyGains[k] * source[c].readLinear(earlyAt[c][k] + 1);
            wet[c] = earlyGain * early[c] + reflectionGain * reflections + lateGain * late[c];
        }
        return wet;
    }
    Frame runTank(const Frame& feed, double t) noexcept
    {
        const bool cubic = running.age == EraAge::clear;
        std::array<double, 8> out {}, back {};
        for (std::size_t i = 0; i < 8; ++i) {
            out[i] = lines[i].readAt(lineFrom[i] + (lineTo[i] - lineFrom[i]) * t, cubic);
            back[i] = lineGain[i] * highShelf[i].process(lowShelf[i].process(out[i]));
        }
        hadamard(back);
        Frame late {};
        for (std::size_t i = 0; i < 8; ++i) {
            lines[i].write(back[i] + lineScale * (feed[0] * inL[i] + feed[1] * inR[i]));
            late[0] += lineScale * outL[i] * out[i];
            late[1] += lineScale * outR[i] * out[i];
        }
        return late;
    }
    Frame tapeWow(Frame x, double t) noexcept
    {
        const double delay = wowFrom + (wowTo - wowFrom) * t;
        for (std::size_t c = 0; c < 2; ++c) {
            wowLine[c].write(x[c]);
            x[c] = wowLine[c].readAt(delay + 1, running.age == EraAge::clear);
        }
        return x;
    }
    Frame tickTank(Frame x, double t) noexcept
    {
        if (running.voicing == EraVoicing::tape) x = tapeWow(x, t);
        for (std::size_t c = 0; c < 2; ++c) source[c].write(x[c]);
        x = diffuse(x, t);
        for (std::size_t c = 0; c < 2; ++c) injection[c].write(x[c]);
        return addEarly(partlyDiffused, runTank(tankFeed(x), t));
    }
    Frame tickGate(Frame x, double t) noexcept
    {
        gateLine.write(.5 * (x[0] + x[1]));
        const double g = gateDiffuserGain * diffusion.value;
        Frame wet {};
        for (std::size_t c = 0; c < 2; ++c) {
            const double chorus = gateChorusFrom[c] + (gateChorusTo[c] - gateChorusFrom[c]) * t + 1;
            double sum = 0;
            for (std::size_t k = 0; k < gateTaps; ++k) sum += gateGain[c][k] * gateLine.readLinear(gateAt[c][k] + chorus);
            for (std::size_t s = 0; s < 4; ++s) {
                auto& line = gateDiffusers[c][s];
                const double y = line.readLinear(gateDiffuserAt[c][s]) - g * sum;
                line.write(sum + g * y);
                sum = y;
            }
            wet[c] = gateHigh[c].process(gateLow[c].process(sum));
        }
        return wet;
    }
    Frame tick(Frame x) noexcept
    {
        if (internalCount == 0) internalControl();
        const double t = double(internalCount + 1) / controlInterval;
        internalCount = internalCount + 1 == controlInterval ? 0 : internalCount + 1;
        return ageStage(running.voicing == EraVoicing::gate ? tickGate(x, t) : tickTank(x, t));
    }

    Frame wetPath(Frame x) noexcept
    {
        if (drive.value > 0)
            for (std::size_t c = 0; c < 2; ++c)
                x[c] = running.voicing == EraVoicing::tape ? dcBlock[c].process(era::gritTape(x[c], drive.value)) : era::gritOdd(x[c], drive.value);
        Frame internal {};
        if (rates.push(x, internal)) { rates.expand(tick(internal), expanded); expandedRead = 0; }
        return expanded[std::size_t(expandedRead++)];
    }
    bool clearSome() noexcept
    {
        std::size_t budget = clearChunk;
        while (clearIndex < clearable.size() && budget > 0) {
            auto& line = *clearable[clearIndex];
            const auto next = line.clearChunk(clearFrom, budget);
            budget -= next - clearFrom;
            clearFrom = next;
            if (clearFrom == line.size()) { ++clearIndex; clearFrom = 0; }
        }
        return clearIndex == clearable.size();
    }
    void applyRunning() noexcept
    {
        running.voicing = settings.voicing;
        running.age = settings.age;
        for (auto& f : lowShelf) f.clear();
        for (auto& f : highShelf) f.clear();
        for (auto& f : gateLow) f.clear();
        for (auto& f : gateHigh) f.clear();
        applyAge();
        internalCount = 0;
        coefficientsDirty = snapDelays = true;
    }
    // A voicing or age change fades the wet path out, clears the tank in chunks, applies, and fades back in.
    Frame switching(const Frame& x) noexcept
    {
        const double step = 1 / (fadeSeconds * hostRate);
        switch (phase) {
        case Phase::running:
            if (settings.voicing != running.voicing || settings.age != running.age) phase = Phase::fadingOut;
            break;
        case Phase::fadingOut:
            fade = std::max(0.0, fade - step);
            if (fade == 0) { phase = Phase::clearing; clearIndex = clearFrom = 0; }
            break;
        case Phase::clearing:
            if (clearSome()) { applyRunning(); phase = Phase::fadingIn; }
            return {};
        case Phase::fadingIn:
            fade = std::min(1.0, fade + step);
            if (fade == 1) phase = Phase::running;
            break;
        }
        const Frame wet = wetPath(x);
        return { fade * wet[0], fade * wet[1] };
    }
    Frame process(Frame in) noexcept
    {
        if (hostCount == 0) hostControl();
        hostCount = hostCount + 1 == hostInterval ? 0 : hostCount + 1;
        mix.advance();
        predelaySamples.advance();
        drive.advance();
        Frame x = in;
        for (auto& v : x) if (!std::isfinite(v)) v = 0;
        const Frame wet = switching(x);
        const double dry = std::cos(std::numbers::pi / 2 * mix.value), gain = std::sin(std::numbers::pi / 2 * mix.value);
        Frame out {};
        for (std::size_t c = 0; c < 2; ++c) {
            predelay[c].write(wet[c]);
            const double w = highCut[c].process(lowCut[c].process(predelay[c].readLinear(predelaySamples.value + 1)));
            out[c] = dry * in[c] + gain * w;
        }
        return out;
    }
};

EraReverb::EraReverb() : state(std::make_unique<State>()) {}
EraReverb::~EraReverb() = default;
bool EraReverb::prepare(double sampleRate) noexcept
{
    if (!(sampleRate >= 8000 && sampleRate <= 768000)) return false;
    try { return state->prepare(sampleRate); }
    catch (const std::bad_alloc&) { state->hostRate = 0; return false; }
}
bool EraReverb::setSettings(const EraReverbSettings& s) noexcept
{
    if (!validateEraReverb(s)) return false;
    state->settings = s;
    return true;
}
const EraReverbSettings& EraReverb::settings() const noexcept { return state->settings; }
EraReverb::Frame EraReverb::process(Frame in) noexcept { return state->hostRate > 0 ? state->process(in) : in; }
void EraReverb::processBlock(std::span<Frame> frames) noexcept { for (auto& f : frames) f = process(f); }
void EraReverb::reset() noexcept { if (state->hostRate > 0) state->reset(); }
}

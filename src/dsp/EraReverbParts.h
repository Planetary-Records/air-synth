#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace airack::era
{
using Frame = std::array<double, 2>;

inline double flush(double v) noexcept { return std::abs(v) < 1e-30 ? 0.0 : v; }

// Float ring buffer. Delays count back from the next write: read(1) is the last sample written.
class DelayLine
{
public:
    void allocate(std::size_t maxDelay)
    {
        std::size_t size = 8;
        while (size < maxDelay + 4) size <<= 1;
        buffer.assign(size, 0.f);
        mask = size - 1;
        position = 0;
        written = size;
    }
    std::size_t size() const noexcept { return buffer.size(); }
    // Zeroes [from, from + count) and returns where the next chunk starts.
    std::size_t clearChunk(std::size_t from, std::size_t count) noexcept
    {
        const auto end = std::min(buffer.size(), from + count);
        std::fill(buffer.begin() + std::ptrdiff_t(from), buffer.begin() + std::ptrdiff_t(end), 0.f);
        return end;
    }
    // Constant time, so a host reset of eight slots stays inside one callback: reads older than every write since
    // return silence, exactly as a zeroed buffer would.
    void clear() noexcept { position = 0; written = 0; }
    void write(double value) noexcept
    {
        const auto f = float(value);
        buffer[position] = std::abs(f) < 1e-30f ? 0.f : f;
        position = (position + 1) & mask;
        written += written <= mask;
    }
    double read(std::size_t delay) const noexcept
    {
        if (written <= mask && ((delay - 1) & mask) >= written) [[unlikely]] return 0.0;
        return buffer[(position - delay) & mask];
    }
    double readLinear(double delay) const noexcept
    {
        const auto whole = std::size_t(delay);
        const double f = delay - double(whole), a = read(whole), b = read(whole + 1);
        return a + f * (b - a);
    }
    // Catmull-Rom; needs delay >= 2.
    double readCubic(double delay) const noexcept
    {
        const auto whole = std::size_t(delay);
        const double f = delay - double(whole);
        const double ym1 = read(whole - 1), y0 = read(whole), y1 = read(whole + 1), y2 = read(whole + 2);
        const double c1 = .5 * (y1 - ym1), c2 = ym1 - 2.5 * y0 + 2 * y1 - .5 * y2, c3 = .5 * (y2 - ym1) + 1.5 * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }
    double readAt(double delay, bool cubic) const noexcept { return cubic ? readCubic(delay) : readLinear(delay); }
private:
    std::vector<float> buffer;
    std::size_t mask = 0, position = 0, written = 0;
};

// First-order low- or high-pass: impulse-invariant pole, zero placed so the gain at Nyquist matches the
// analog prototype, which keeps -3 dB at the corner and 6 dB per octave up to Nyquist.
struct OnePole
{
    double b0 = 1, b1 = 0, a1 = 0, x1 = 0, y1 = 0;
    void lowpass(double hz, double rate) noexcept
    {
        const double p = std::exp(-2 * std::numbers::pi * hz / rate), r = rate / (2 * hz), nyquist = 1 / std::sqrt(1 + r * r);
        b0 = (1 - p + nyquist * (1 + p)) / 2;
        b1 = (1 - p - nyquist * (1 + p)) / 2;
        a1 = -p;
    }
    void highpass(double hz, double rate) noexcept
    {
        const double p = std::exp(-2 * std::numbers::pi * hz / rate), r = 2 * hz / rate, nyquist = 1 / std::sqrt(1 + r * r);
        b0 = nyquist * (1 + p) / 2;
        b1 = -b0;
        a1 = -p;
    }
    double process(double x) noexcept
    {
        const double y = b0 * x + b1 * x1 - a1 * y1;
        x1 = x; y1 = y;
        return y;
    }
    void clear() noexcept { x1 = y1 = 0; }
    void flushState() noexcept { x1 = flush(x1); y1 = flush(y1); }
};

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    double process(double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void clear() noexcept { z1 = z2 = 0; }
    void flushState() noexcept { z1 = flush(z1); z2 = flush(z2); }
    void set(double c0, double c1, double c2, double d0, double d1, double d2) noexcept
    {
        b0 = c0 / d0; b1 = c1 / d0; b2 = c2 / d0; a1 = d1 / d0; a2 = d2 / d0;
    }
    // RBJ cookbook shelves at slope 1; gain is the linear amplitude below (low) or above (high) the corner.
    void lowShelf(double hz, double gain, double rate) noexcept
    {
        const double A = std::sqrt(gain), w = 2 * std::numbers::pi * hz / rate, c = std::cos(w);
        const double k = std::numbers::sqrt2 * std::sqrt(A) * std::sin(w);
        set(A * ((A + 1) - (A - 1) * c + k), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - k),
            (A + 1) + (A - 1) * c + k, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - k);
    }
    void highShelf(double hz, double gain, double rate) noexcept
    {
        const double A = std::sqrt(gain), w = 2 * std::numbers::pi * hz / rate, c = std::cos(w);
        const double k = std::numbers::sqrt2 * std::sqrt(A) * std::sin(w);
        set(A * ((A + 1) + (A - 1) * c + k), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - k),
            (A + 1) - (A - 1) * c + k, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - k);
    }
    void lowpass(double hz, double q, double rate) noexcept
    {
        const double w = 2 * std::numbers::pi * hz / rate, c = std::cos(w), alpha = std::sin(w) / (2 * q);
        set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    }
};

inline constexpr int halfbandTaps = 95, halfbandCentre = 47, halfbandLobes = 48;
struct Halfband { std::array<double, halfbandLobes> gain {}; std::array<int, halfbandLobes> offset {}; };
// Kaiser-windowed (beta 10) half-band sinc: about 100 dB of rejection from 0.284 of the rate upward.
inline const Halfband& halfband() noexcept
{
    static const Halfband kernel = [] {
        const auto bessel = [](double x) {
            double sum = 1, term = 1;
            for (int k = 1; k < 50; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
            return sum;
        };
        Halfband h;
        double total = 0;
        std::size_t j = 0;
        for (int n = 0; n < halfbandTaps; ++n) {
            const int m = n - halfbandCentre;
            if (m % 2 == 0) continue;
            const double t = double(m) / halfbandCentre;
            h.gain[j] = std::sin(std::numbers::pi * m / 2) / (std::numbers::pi * m) * bessel(10 * std::sqrt(1 - t * t)) / bessel(10);
            h.offset[j] = n;
            total += h.gain[j];
            ++j;
        }
        for (auto& g : h.gain) g *= .5 / total;
        return h;
    }();
    return kernel;
}

class HalfbandDown
{
public:
    void clear() noexcept { line.fill({}); write = 0; odd = false; }
    // Returns true on every second sample, with the filtered, decimated output.
    bool push(Frame in, Frame& out) noexcept
    {
        line[std::size_t(write)] = line[std::size_t(write + halfbandTaps)] = in;
        const int newest = write + halfbandTaps;
        write = write + 1 == halfbandTaps ? 0 : write + 1;
        odd = !odd;
        if (odd) return false;
        const auto& h = halfband();
        const auto& centre = line[std::size_t(newest - halfbandCentre)];
        Frame sum { .5 * centre[0], .5 * centre[1] };
        for (std::size_t j = 0; j < halfbandLobes; ++j) {
            const auto& x = line[std::size_t(newest - h.offset[j])];
            sum[0] += h.gain[j] * x[0];
            sum[1] += h.gain[j] * x[1];
        }
        out = sum;
        return true;
    }
private:
    std::array<Frame, 2 * halfbandTaps> line {};
    int write = 0;
    bool odd = false;
};

class HalfbandUp
{
public:
    void clear() noexcept { line.fill({}); write = 0; }
    // One sample in, two out, in time order.
    void push(Frame in, Frame& first, Frame& second) noexcept
    {
        line[std::size_t(write)] = line[std::size_t(write + history)] = in;
        const int newest = write + history;
        write = write + 1 == history ? 0 : write + 1;
        const auto& h = halfband();
        Frame sum {};
        for (std::size_t j = 0; j < halfbandLobes; ++j) {
            const auto& x = line[std::size_t(newest - h.offset[j] / 2)];
            sum[0] += 2 * h.gain[j] * x[0];
            sum[1] += 2 * h.gain[j] * x[1];
        }
        first = sum;
        second = line[std::size_t(newest - halfbandCentre / 2)];
    }
private:
    static constexpr int history = halfbandCentre + 1;
    std::array<Frame, 2 * history> line {};
    int write = 0;
};

// Cascaded half-band stages between the host rate and an internal rate 1, 2, 4, 8, 16 or 32 times lower.
class RateChanger
{
public:
    static constexpr int maximumStages = 5;
    static constexpr std::size_t maximumFactor = std::size_t(1) << maximumStages;
    void setFactor(int value) noexcept
    {
        stages = 0;
        while (stages < maximumStages && (2 << stages) <= value) ++stages;
        clear();
    }
    void clear() noexcept
    {
        for (auto& d : down) d.clear();
        for (auto& u : up) u.clear();
    }
    bool push(Frame in, Frame& out) noexcept
    {
        for (int s = 0; s < stages; ++s)
            if (!down[std::size_t(s)].push(in, in)) return false;
        out = in;
        return true;
    }
    void expand(Frame in, std::array<Frame, maximumFactor>& out) noexcept
    {
        std::array<Frame, maximumFactor> next {};
        out[0] = in;
        std::size_t count = 1;
        for (int s = stages - 1; s >= 0; --s) {
            for (std::size_t i = 0; i < count; ++i) up[std::size_t(s)].push(out[i], next[2 * i], next[2 * i + 1]);
            count *= 2;
            std::copy_n(next.begin(), count, out.begin());
        }
    }
private:
    std::array<HalfbandDown, maximumStages> down;
    std::array<HalfbandUp, maximumStages> up;
    int stages = 0;
};

inline double gritDrive(double grit) noexcept { return 4 * grit * grit * grit; }
// Symmetric tanh with unity small-signal gain: odd harmonics only.
inline double gritOdd(double x, double drive) noexcept { return drive > 0 ? std::tanh(drive * x) / drive : x; }
// Biased tanh with the offset removed and the slope normalised: even harmonics as well, like tape.
inline double gritTape(double x, double drive) noexcept
{
    if (drive <= 0) return x;
    constexpr double bias = .5;
    const double t = std::tanh(bias);
    return (std::tanh(drive * x + bias) - t) / (drive * (1 - t * t));
}

inline std::uint32_t xorshift(std::uint32_t& s) noexcept { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
inline double uniform(std::uint32_t& s) noexcept { return double(xorshift(s)) / 4294967296.0; }

// Fixed-step truncation, with triangular dither while sound is present; silence stays exactly zero.
struct Grain
{
    double step = 1;
    int hold = 0, holdSamples = 0;
    std::uint32_t seed = 1;
    void set(double stepSize, int holdLength, std::uint32_t seedValue) noexcept
    {
        step = stepSize; holdSamples = holdLength; seed = seedValue; hold = 0;
    }
    double process(double x) noexcept
    {
        if (std::abs(x) > step) hold = holdSamples;
        else if (hold > 0) --hold;
        const double dither = hold > 0 ? (uniform(seed) - uniform(seed)) * step : 0;
        return step * std::round((x + dither) / step);
    }
};

// Moves to a target in equal steps and lands on it exactly.
struct Ramp
{
    double value = 0, target = 0, step = 0;
    int remaining = 0;
    void snap(double v) noexcept { value = target = v; remaining = 0; }
    void to(double v, int steps) noexcept
    {
        if (v == target) return;
        target = v;
        remaining = std::max(1, steps);
        step = (target - value) / remaining;
    }
    bool advance() noexcept
    {
        if (remaining == 0) return false;
        value = --remaining == 0 ? target : value + step;
        return true;
    }
};
}

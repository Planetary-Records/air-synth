#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <optional>
#include <vector>

// The VintageVerb investigation's measure.py methods, ported for offline development checks.
namespace reverbAnalysis
{
using Signal = std::vector<double>;
struct Stereo { Signal left, right; };

inline Stereo impulse() { return { Signal { 1.0 }, Signal { 1.0 } }; }
inline Stereo leftImpulse() { return { Signal { 1.0 }, Signal { 0.0 } }; }
// measure.py's sine(): 10 ms fades at both ends, the same on both channels.
inline Stereo sine(double rate, double hz, double seconds, double amplitude)
{
    const auto n = std::size_t(seconds * rate);
    Signal s(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = double(i) / rate, fade = std::min(1.0, std::min(t, seconds - t) / .01);
        s[i] = amplitude * fade * std::sin(2 * std::numbers::pi * hz * t);
    }
    return { s, s };
}
inline Signal sum(const Stereo& y)
{
    Signal s(y.left.size());
    for (std::size_t i = 0; i < s.size(); ++i) s[i] = y.left[i] + y.right[i];
    return s;
}
inline Signal slice(const Signal& x, double rate, double start, double stop)
{
    const auto a = std::min(x.size(), std::size_t(start * rate)), b = std::min(x.size(), std::size_t(stop * rate));
    return Signal(x.begin() + std::ptrdiff_t(a), x.begin() + std::ptrdiff_t(b));
}

// In-place radix-2 FFT; the size must be a power of two.
inline void fft(std::vector<std::complex<double>>& a)
{
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2 * std::numbers::pi / double(length);
        for (std::size_t i = 0; i < n; i += length)
            for (std::size_t k = 0; k < length / 2; ++k) {
                const auto w = std::polar(1.0, angle * double(k));
                const auto u = a[i + k], v = a[i + k + length / 2] * w;
                a[i + k] = u + v;
                a[i + k + length / 2] = u - v;
            }
    }
}

struct Section { double b0, b1, b2, a1, a2; };
inline Section butterworth(bool high, double hz, double rate, double q)
{
    const double w = 2 * std::numbers::pi * hz / rate, c = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
    return high ? Section { (1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - alpha) / a0 }
                : Section { (1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - alpha) / a0 };
}
inline void run(Signal& x, const Section& s)
{
    double z1 = 0, z2 = 0;
    for (auto& v : x) {
        const double y = s.b0 * v + z1;
        z1 = s.b1 * v - s.a1 * y + z2;
        z2 = s.b2 * v - s.a2 * y;
        v = y;
    }
}
// One octave around the centre: 4th-order Butterworth edges, forward and backward (zero phase).
inline Signal octave(const Signal& x, double rate, double centre)
{
    const double low = centre / std::numbers::sqrt2, high = std::min(centre * std::numbers::sqrt2, rate / 2 * .95);
    const Section sections[] { butterworth(true, low, rate, .5412), butterworth(true, low, rate, 1.3066),
        butterworth(false, high, rate, .5412), butterworth(false, high, rate, 1.3066) };
    Signal y = x;
    for (const auto& s : sections) run(y, s);
    std::reverse(y.begin(), y.end());
    for (const auto& s : sections) run(y, s);
    std::reverse(y.begin(), y.end());
    return y;
}
inline Stereo octave(const Stereo& y, double rate, double centre) { return { octave(y.left, rate, centre), octave(y.right, rate, centre) }; }

// RT60 from both channels' Schroeder decay, fitted between hiDb and loDb; none if it ends above loDb - 5.
inline std::optional<double> rt60(const Stereo& y, double rate, double hiDb = -5, double loDb = -25)
{
    const auto n = y.left.size();
    Signal edc(n);
    double total = 0;
    for (std::size_t i = n; i-- > 0;) { total += y.left[i] * y.left[i] + y.right[i] * y.right[i]; edc[i] = total; }
    if (total <= 0) return std::nullopt;
    double st = 0, sd = 0, stt = 0, std_ = 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = 10 * std::log10(std::max(edc[i] / total, 1e-30));
        if (d <= hiDb && d >= loDb) { const double t = double(i) / rate; ++count; st += t; sd += d; stt += t * t; std_ += t * d; }
    }
    if (count < std::size_t(rate * .01) || 10 * std::log10(std::max(edc[n - 1] / total, 1e-30)) > loDb - 5) return std::nullopt;
    const double slope = (double(count) * std_ - st * sd) / (double(count) * stt - st * st);
    return slope < 0 ? std::optional<double>(-60 / slope) : std::nullopt;
}

// First sample whose larger channel exceeds relDb below the largest sample.
inline std::size_t onset(const Stereo& y, double relDb = -40)
{
    double peak = 0;
    for (std::size_t i = 0; i < y.left.size(); ++i) peak = std::max({ peak, std::abs(y.left[i]), std::abs(y.right[i]) });
    const double threshold = peak * std::pow(10.0, relDb / 20);
    for (std::size_t i = 0; i < y.left.size(); ++i)
        if (std::max(std::abs(y.left[i]), std::abs(y.right[i])) > threshold) return i;
    return 0;
}

// Centred moving RMS of both channels' energy, in dB.
inline Signal envelopeDb(const Stereo& y, double rate, double ms)
{
    const auto n = std::max<std::size_t>(1, std::size_t(rate * ms / 1000)), size = y.left.size();
    Signal prefix(size + 1, 0.0), out(size);
    for (std::size_t i = 0; i < size; ++i) prefix[i + 1] = prefix[i] + y.left[i] * y.left[i] + y.right[i] * y.right[i];
    for (std::size_t i = 0; i < size; ++i) {
        const auto from = i >= (n - 1) / 2 ? i - (n - 1) / 2 : 0, to = std::min(size, i + n / 2 + 1);
        out[i] = 10 * std::log10(std::max((prefix[to] - prefix[from]) / double(n), 1e-24));
    }
    return out;
}

// Time after onset at which the normalised echo density (20 ms windows, 1 ms hop, first 0.6 s) first reaches 0.9.
// Windows more than 60 dB below the response's peak never count: between sparse echoes they hold only the smooth
// residue of an output high-pass, which the scale-free count would read as diffuse.
inline std::optional<double> nedDiffuseMs(const Stereo& y, double rate)
{
    const auto start = onset(y);
    Signal h;
    double peak = 0;
    for (std::size_t i = start; i < y.left.size(); ++i) {
        h.push_back(y.left[i] + y.right[i]);
        peak = std::max(peak, std::abs(h.back()));
    }
    const auto half = std::size_t(rate * .02) / 2, hop = std::size_t(rate / 1000);
    const double expected = std::erfc(1 / std::numbers::sqrt2);
    for (std::size_t c = half; c + half <= h.size() && c < std::size_t(rate * .6); c += hop) {
        double mean = 0, squares = 0;
        for (std::size_t j = c - half; j < c + half; ++j) mean += h[j];
        mean /= double(2 * half);
        for (std::size_t j = c - half; j < c + half; ++j) squares += (h[j] - mean) * (h[j] - mean);
        const double deviation = std::sqrt(squares / double(2 * half));
        if (deviation <= 1e-3 * peak) continue;
        std::size_t above = 0;
        for (std::size_t j = c - half; j < c + half; ++j) above += std::abs(h[j]) > deviation;
        if (double(above) / double(2 * half) / expected >= .9) return double(c) / rate * 1000;
    }
    return std::nullopt;
}

// Peak normalised cross-correlation within 1 ms, over start to stop seconds.
inline double iacc(const Stereo& y, double rate, double start = .1, double stop = .6)
{
    const Signal l = slice(y.left, rate, start, stop), r = slice(y.right, rate, start, stop);
    double el = 0, er = 0;
    for (std::size_t i = 0; i < l.size(); ++i) { el += l[i] * l[i]; er += r[i] * r[i]; }
    const auto lag = std::ptrdiff_t(rate * .001);
    double best = 0;
    for (std::ptrdiff_t k = -lag; k <= lag; ++k) {
        double c = 0;
        for (std::size_t i = 0; i < l.size(); ++i) {
            const auto j = std::ptrdiff_t(i) + k;
            if (j >= 0 && j < std::ptrdiff_t(r.size())) c += l[i] * r[std::size_t(j)];
        }
        best = std::max(best, std::abs(c));
    }
    return el > 0 && er > 0 ? best / std::sqrt(el * er) : 0;
}

// RMS spread of a tone's spectrum within 300 Hz of f0 (symmetric Hann, zero-padded to a power of two).
inline double modWidth(const Stereo& y, double rate, double start, double stop, double f0 = 1000)
{
    const Signal seg = slice(sum(y), rate, start, stop);
    std::size_t size = 1;
    while (size < seg.size()) size <<= 1;
    std::vector<std::complex<double>> a(size);
    for (std::size_t i = 0; i < seg.size(); ++i)
        a[i] = seg[i] * (.5 - .5 * std::cos(2 * std::numbers::pi * double(i) / double(seg.size() - 1)));
    fft(a);
    double total = 0, moment = 0;
    for (std::size_t k = 0; k <= size / 2; ++k) {
        const double f = double(k) * rate / double(size);
        if (f <= f0 - 300 || f >= f0 + 300) continue;
        const double p = std::norm(a[k]);
        total += p; moment += (f - f0) * (f - f0) * p;
    }
    return total > 0 ? std::sqrt(moment / total) : 0;
}

struct Spectrum { std::vector<double> hz, db; };
// scipy.signal.welch defaults: periodic Hann, half overlap, each segment's mean removed, one-sided density.
inline Spectrum welch(const Signal& x, double rate, std::size_t segment)
{
    while (segment > x.size()) segment >>= 1;
    std::vector<double> window(segment), accumulated(segment / 2 + 1, 0.0);
    double power = 0;
    for (std::size_t i = 0; i < segment; ++i) {
        window[i] = .5 - .5 * std::cos(2 * std::numbers::pi * double(i) / double(segment));
        power += window[i] * window[i];
    }
    std::vector<std::complex<double>> a(segment);
    std::size_t count = 0;
    for (std::size_t start = 0; start + segment <= x.size(); start += segment / 2, ++count) {
        double mean = 0;
        for (std::size_t i = 0; i < segment; ++i) mean += x[start + i];
        mean /= double(segment);
        for (std::size_t i = 0; i < segment; ++i) a[i] = (x[start + i] - mean) * window[i];
        fft(a);
        for (std::size_t k = 0; k <= segment / 2; ++k) accumulated[k] += std::norm(a[k]);
    }
    Spectrum s;
    for (std::size_t k = 0; k <= segment / 2; ++k) {
        double p = accumulated[k] / double(std::max<std::size_t>(count, 1)) / (rate * power);
        if (k != 0 && k != segment / 2) p *= 2;
        s.hz.push_back(double(k) * rate / double(segment));
        s.db.push_back(10 * std::log10(std::max(p, 1e-30)));
    }
    return s;
}

// First frequency above 2 kHz at which the spectrum is 20 dB below its 500 Hz to 2 kHz mean.
inline double bandwidth20(const Spectrum& s)
{
    double reference = 0;
    std::size_t n = 0;
    for (std::size_t k = 0; k < s.hz.size(); ++k)
        if (s.hz[k] >= 500 && s.hz[k] <= 2000) { reference += s.db[k]; ++n; }
    reference /= double(std::max<std::size_t>(n, 1));
    for (std::size_t k = 0; k < s.hz.size(); ++k)
        if (s.hz[k] > 2000 && s.db[k] < reference - 20) return s.hz[k];
    return s.hz.back();
}

// measure_grit.py's spectrum: Welch (16384) over 1.0 to 2.8 s of L+R.
inline Spectrum gritSpectrum(const Stereo& y, double rate) { return welch(slice(sum(y), rate, 1.0, 2.8), rate, 16384); }
// Largest bin within 15 Hz.
inline double peakDb(const Spectrum& s, double hz)
{
    double best = -400;
    for (std::size_t k = 0; k < s.hz.size(); ++k) if (std::abs(s.hz[k] - hz) < 15) best = std::max(best, s.db[k]);
    return best;
}
// Median between 2 and 8 kHz, at least 40 Hz from every harmonic of f0.
inline double floorDb(const Spectrum& s, double f0)
{
    std::vector<double> between;
    for (std::size_t k = 0; k < s.hz.size(); ++k) {
        if (s.hz[k] <= 2000 || s.hz[k] >= 8000) continue;
        bool clear = true;
        for (int h = 1; h * f0 <= s.hz.back(); ++h) if (std::abs(s.hz[k] - h * f0) <= 40) { clear = false; break; }
        if (clear) between.push_back(s.db[k]);
    }
    std::sort(between.begin(), between.end());
    const auto m = between.size();
    return m == 0 ? -400 : m % 2 ? between[m / 2] : (between[m / 2 - 1] + between[m / 2]) / 2;
}
struct Harmonics { double fundamentalDb, h2, h3, floor; };
// measure_grit.py's analysis: harmonics and floor relative to the fundamental's peak bin.
inline Harmonics harmonics(const Stereo& y, double rate, double f0 = 220)
{
    const auto s = gritSpectrum(y, rate);
    const double fundamental = peakDb(s, f0);
    return { fundamental, peakDb(s, 2 * f0) - fundamental, peakDb(s, 3 * f0) - fundamental, floorDb(s, f0) - fundamental };
}

// Amplitude, in dB, of the sinusoid at exactly hz (single-bin Hann DFT).
inline double toneDb(const Signal& x, double rate, double hz)
{
    std::complex<double> acc;
    double weights = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double w = .5 - .5 * std::cos(2 * std::numbers::pi * double(i) / double(x.size() - 1));
        acc += x[i] * w * std::polar(1.0, -2 * std::numbers::pi * hz * double(i) / rate);
        weights += w;
    }
    return 20 * std::log10(std::max(2 * std::abs(acc) / weights, 1e-30));
}
}

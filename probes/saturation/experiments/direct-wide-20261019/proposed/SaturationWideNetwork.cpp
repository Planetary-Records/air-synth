#include "SaturationWideNetwork.h"
#if defined(_WIN32) && defined(_M_X64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <immintrin.h>
#endif

namespace airack::probe::wide_network {
bool operatingSystemSupportsAvx() noexcept {
#if defined(_WIN32) && defined(_M_X64)
    constexpr auto required = XSTATE_MASK_LEGACY_SSE | XSTATE_MASK_AVX;
    return (GetEnabledXStateFeatures() & required) == required;
#else
    return false;
#endif
}
namespace {
#if defined(_WIN32) && defined(_M_X64)
using FourFrames = std::array<Quad, 4>;
__forceinline void processBlock(BiquadState& s, const Coefficients& c, FourFrames& samples) noexcept {
    const auto b0 = _mm256_set1_pd(c.b0), b1 = _mm256_set1_pd(c.b1), b2 = _mm256_set1_pd(c.b2);
    const auto a1 = _mm256_set1_pd(c.a1), a2 = _mm256_set1_pd(c.a2);
    auto z1 = _mm256_load_pd(s.z1.data()), z2 = _mm256_load_pd(s.z2.data());
    for (std::size_t n = 0; n < 4; ++n) {
        const auto x = _mm256_load_pd(samples[n].data());
        const auto y = _mm256_add_pd(_mm256_mul_pd(b0, x), z1);
        const auto nextZ1 = _mm256_add_pd(_mm256_sub_pd(_mm256_mul_pd(b1, x), _mm256_mul_pd(a1, y)), z2);
        const auto nextZ2 = _mm256_sub_pd(_mm256_mul_pd(b2, x), _mm256_mul_pd(a2, y));
        z1 = nextZ1; z2 = nextZ2;
        _mm256_store_pd(samples[n].data(), y);
    }
    _mm256_store_pd(s.z1.data(), z1); _mm256_store_pd(s.z2.data(), z2);
}
#else
Quad process(BiquadState& s, Quad x, const Coefficients& c) noexcept {
    for (std::size_t ch = 0; ch < 4; ++ch) {
        const double y = c.b0 * x[ch] + s.z1[ch];
        s.z1[ch] = c.b1 * x[ch] - c.a1 * y + s.z2[ch];
        s.z2[ch] = c.b2 * x[ch] - c.a2 * y; x[ch] = y;
    }
    return x;
}
#endif
}
void process4(State& s, const NetworkCoefficients& c, const InputBlock& wet,
              const InputBlock& dry, WetBandBlock& wetBands, DryBandBlock& dryBands) noexcept {
#if defined(_WIN32) && defined(_M_X64)
    // Each independent biquad advances its four frames in the original order.
    // Its two histories stay local until this native frame's block is complete.
    alignas(32) FourFrames low, mid, high;
    for (std::size_t n = 0; n < 4; ++n) {
        const auto x = _mm256_set_pd(dry[n][1], dry[n][0], wet[n][1], wet[n][0]);
        _mm256_store_pd(low[n].data(), x); _mm256_store_pd(high[n].data(), x);
    }
    for (std::size_t stage = 0; stage < 2; ++stage) processBlock(s.lowLP[stage], c.lowLP, low);
    for (std::size_t stage = 0; stage < 2; ++stage) processBlock(s.lowHP[stage], c.lowHP, high);
    for (std::size_t n = 0; n < 4; ++n) _mm256_store_pd(mid[n].data(), _mm256_load_pd(high[n].data()));
    for (std::size_t stage = 0; stage < 2; ++stage) processBlock(s.highLP[stage], c.highLP, mid);
    for (std::size_t stage = 0; stage < 2; ++stage) processBlock(s.highHP[stage], c.highHP, high);
    processBlock(s.lowCompensation, c.highAP, low);
    for (std::size_t n = 0; n < 4; ++n) {
        const auto lowOutput = _mm256_load_pd(low[n].data());
        _mm_storeu_pd(wetBands[0][n].data(), _mm256_castpd256_pd128(lowOutput));
        _mm_storeu_pd(dryBands[n][0].data(), _mm256_extractf128_pd(lowOutput, 1));
        const auto midOutput = _mm256_load_pd(mid[n].data());
        _mm_storeu_pd(wetBands[1][n].data(), _mm256_castpd256_pd128(midOutput));
        _mm_storeu_pd(dryBands[n][1].data(), _mm256_extractf128_pd(midOutput, 1));
        const auto highOutput = _mm256_load_pd(high[n].data());
        _mm_storeu_pd(wetBands[2][n].data(), _mm256_castpd256_pd128(highOutput));
        _mm_storeu_pd(dryBands[n][2].data(), _mm256_extractf128_pd(highOutput, 1));
    }
    _mm256_zeroupper();
#else

    for (std::size_t n = 0; n < 4; ++n) {
#if defined(_WIN32) && defined(_M_X64)
        const auto input = _mm256_set_pd(dry[n][1], dry[n][0], wet[n][1], wet[n][0]);
#else
        const Quad input { wet[n][0], wet[n][1], dry[n][0], dry[n][1] };
#endif
        auto low = input, high = input;
        for (std::size_t stage = 0; stage < 2; ++stage) {
            low = process(s.lowLP[stage], low, c.lowLP);
            high = process(s.lowHP[stage], high, c.lowHP);
        }
        auto mid = high, upper = high;
        for (std::size_t stage = 0; stage < 2; ++stage) {
            mid = process(s.highLP[stage], mid, c.highLP);
            upper = process(s.highHP[stage], upper, c.highHP);
        }
        low = process(s.lowCompensation, low, c.highAP);
#if defined(_WIN32) && defined(_M_X64)
        _mm_storeu_pd(wetBands[0][n].data(), _mm256_castpd256_pd128(low));
        _mm_storeu_pd(dryBands[n][0].data(), _mm256_extractf128_pd(low, 1));
        _mm_storeu_pd(wetBands[1][n].data(), _mm256_castpd256_pd128(mid));
        _mm_storeu_pd(dryBands[n][1].data(), _mm256_extractf128_pd(mid, 1));
        _mm_storeu_pd(wetBands[2][n].data(), _mm256_castpd256_pd128(upper));
        _mm_storeu_pd(dryBands[n][2].data(), _mm256_extractf128_pd(upper, 1));
#else
        wetBands[0][n] = { low[0], low[1] }; dryBands[n][0] = { low[2], low[3] };
        wetBands[1][n] = { mid[0], mid[1] }; dryBands[n][1] = { mid[2], mid[3] };
        wetBands[2][n] = { upper[0], upper[1] }; dryBands[n][2] = { upper[2], upper[3] };
#endif
    }
#if defined(_WIN32) && defined(_M_X64)
    _mm256_zeroupper();
#endif

#endif
}
}

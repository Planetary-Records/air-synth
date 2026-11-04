#include "SaturationWideNetwork.h"
#if defined(_WIN32) && defined(_M_X64)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <immintrin.h>
#endif

namespace airack::saturation_detail::wide_network {
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
              const InputBlock& dry, BandBlock& bands) noexcept {
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
        _mm256_storeu_pd(bands[n][0].data(), _mm256_load_pd(low[n].data()));
        _mm256_storeu_pd(bands[n][1].data(), _mm256_load_pd(mid[n].data()));
        _mm256_storeu_pd(bands[n][2].data(), _mm256_load_pd(high[n].data()));
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
        _mm256_storeu_pd(bands[n][0].data(), low);
        _mm256_storeu_pd(bands[n][1].data(), mid);
        _mm256_storeu_pd(bands[n][2].data(), upper);
#else
        bands[n] = { low, mid, upper };
#endif
    }
#if defined(_WIN32) && defined(_M_X64)
    _mm256_zeroupper();
#endif

#endif
}
}

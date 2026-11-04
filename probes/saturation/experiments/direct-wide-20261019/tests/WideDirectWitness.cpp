#include "WideDirectApi.h"
#include <windows.h>
#include <immintrin.h>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <malloc.h>
#include <new>
#include <stdexcept>
#include <type_traits>
#include "CppHeapWitness.inc"

#if !defined(_MSC_VER) || !defined(_M_X64) || defined(__clang__)
#error This focused private-Wide witness requires actual Microsoft MSVC x64.
#endif

namespace {
using namespace wide_direct_layout;
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
static_assert(sizeof(Frame) == 16 && sizeof(Quad) == 32 && sizeof(InputBlock) == 64);
static_assert(sizeof(History) == 576 && sizeof(LegacyResult) == 960 && sizeof(DirectResult) == 992);
static_assert(offsetof(DirectResult, afterWet) == 192 && offsetof(DirectResult, dry) == 208);
static_assert(offsetof(DirectResult, afterDry) == 400 && offsetof(DirectResult, history) == 416);
static_assert(std::is_trivially_copyable_v<LegacyResult> && std::is_trivially_copyable_v<DirectResult>);
constexpr std::uint64_t poison = 0x7ff8ac125fedcba0ULL;
constexpr std::array<std::uint64_t, 2> fieldGuard { 0xb4d1629ac53870efULL, 0x67fe0825d391ac4bULL };
constexpr std::array<std::uint64_t, 3> guards { 0x1a2b3c4d5e6f7890ULL, 0xfedcba0987654321ULL, 0x8796a5b4c3d2e1f0ULL };
std::uint64_t checks = 0, blocks = 0, outputWords = 0, historyWords = 0, scenarios = 0, resets = 0, environments = 0;
const char* stage = "initial";

void require(bool okay, const char* message) {
    ++checks;
    if (!okay) throw std::runtime_error(message);
}
std::uint64_t bits(double value) noexcept { return std::bit_cast<std::uint64_t>(value); }
double value(std::uint64_t word) noexcept { return std::bit_cast<double>(word); }
void equal(double expected, double actual, const char* field, std::size_t index) {
    ++checks;
    if (bits(expected) != bits(actual)) {
        std::cerr << "BIT MISMATCH stage=" << stage << " block=" << blocks << " field=" << field
                  << " index=" << index << " old=0x" << std::hex << bits(expected)
                  << " new=0x" << bits(actual) << std::dec << '\n';
        throw std::runtime_error("raw output/history mismatch");
    }
}
template<class Result> struct alignas(32) Guarded {
    std::array<std::uint64_t, 3> before = guards;
    Result result;
    std::array<std::uint64_t, 3> after = guards;
};
// Actual output destinations are deliberately eight bytes off 16-byte alignment.
static_assert(offsetof(Guarded<LegacyResult>, result) == 24);
static_assert(offsetof(Guarded<DirectResult>, result) == 24);
void fill(LegacyResult& result) {
    for (auto& frame : result.bands) for (auto& band : frame) for (auto& x : band) x = value(poison);
    for (auto& x : result.history) x = value(poison);
}
void fill(DirectResult& result) {
    result.afterWet = fieldGuard; result.afterDry = fieldGuard;
    for (auto& band : result.wet) for (auto& frame : band) for (auto& x : frame) x = value(poison);
    for (auto& frame : result.dry) for (auto& band : frame) for (auto& x : band) x = value(poison);
    for (auto& x : result.history) x = value(poison);
}
struct Observation { unsigned csr; int error; std::size_t allocations, frees; };
template<class Call> Observation observe(Call call, unsigned entry) {
    const auto saved = _mm_getcsr();
    int* const error = &errno;
    const int savedError = *error;
    allocationWitness::allocations = allocationWitness::frees = 0;
    _mm_setcsr(entry); *error = E2BIG; allocationWitness::enabled = true;
    call();
    const auto after = _mm_getcsr(); const int afterError = *error;
    allocationWitness::enabled = false;
    const Observation result { after, afterError, allocationWitness::allocations, allocationWitness::frees };
    _mm_setcsr(saved); *error = savedError;
    return result;
}
struct Pair {
    void* original = nullptr;
    void* proposed = nullptr;
    Guarded<LegacyResult> legacy;
    Guarded<DirectResult> direct;
    Pair() {
        original = createOriginal();
        try { proposed = createProposed(); }
        catch (...) { if (original) destroyOriginal(original); throw; }
        if (!original || !proposed) {
            if (original) destroyOriginal(original); if (proposed) destroyProposed(proposed);
            throw std::runtime_error("test state creation failed");
        }
    }
    ~Pair() { destroyOriginal(original); destroyProposed(proposed); }
    Pair(const Pair&) = delete; Pair& operator=(const Pair&) = delete;
    void clear() { clearOriginal(original); clearProposed(proposed); ++resets; }
    void seed(const History& history) { seedOriginal(original, history); seedProposed(proposed, history); }
    void run(const Network& coefficients, const InputBlock& wet, const InputBlock& dry, unsigned entry = 0x1f80u) {
        fill(legacy.result); fill(direct.result);
        require(reinterpret_cast<std::uintptr_t>(&legacy.result) % 16 == 8
            && reinterpret_cast<std::uintptr_t>(&direct.result) % 16 == 8, "unaligned destination fixture failed");
        const auto a = observe([&] { runOriginal(original, coefficients, wet, dry, legacy.result); }, entry);
        const auto b = observe([&] { runProposed(proposed, coefficients, wet, dry, direct.result); }, entry);
        require(a.csr == b.csr && a.error == b.error, "full MXCSR/errno differ from identical entry context");
        require(a.allocations == 0 && a.frees == 0 && b.allocations == 0 && b.frees == 0,
                "admitted private-kernel call C++ heap observation is nonzero");
        require(legacy.before == guards && legacy.after == guards && direct.before == guards && direct.after == guards
            && direct.result.afterWet == fieldGuard && direct.result.afterDry == fieldGuard,
                "output canary overwritten");
        for (std::size_t n = 0; n < 4; ++n) for (std::size_t band = 0; band < 3; ++band)
            for (std::size_t channel = 0; channel < 2; ++channel) {
                const auto wetOld = legacy.result.bands[n][band][channel];
                const auto dryOld = legacy.result.bands[n][band][channel + 2];
                const auto wetNew = direct.result.wet[band][n][channel];
                const auto dryNew = direct.result.dry[n][band][channel];
                require(bits(wetOld) != poison && bits(dryOld) != poison && bits(wetNew) != poison && bits(dryNew) != poison,
                        "kernel output field not written");
                equal(wetOld, wetNew, "wet", (band * 4 + n) * 2 + channel);
                equal(dryOld, dryNew, "dry", (n * 3 + band) * 2 + channel);
                outputWords += 2;
            }
        for (std::size_t i = 0; i < 72; ++i) {
            require(bits(legacy.result.history[i]) != poison && bits(direct.result.history[i]) != poison,
                    "history snapshot field not written");
            equal(legacy.result.history[i], direct.result.history[i], "history", i); ++historyWords;
        }
        ++blocks;
    }
};
std::uint64_t next(std::uint64_t& seed) noexcept { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
double dyadic(std::uint64_t& seed) noexcept { return double(std::int64_t(next(seed) & 0xfffff) - 0x80000) / 1048576.; }
InputBlock input(std::uint64_t& seed) {
    InputBlock result;
    for (auto& frame : result) for (auto& x : frame) x = dyadic(seed);
    return result;
}
History history(std::uint64_t& seed) { History result; for (auto& x : result) x = dyadic(seed) / 16.; return result; }
Network network(unsigned style, std::uint64_t& seed) {
    Network result;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (style == 0) result[i] = { 1, 0, 0, 0, 0 };
        else if (style == 1) result[i] = { double(i + 1) / 16, double(int(i) - 2) / 16, .0625, -.375, .125 };
        else result[i] = { .375 + dyadic(seed) / 4, dyadic(seed) / 4, .0625 + dyadic(seed) / 16,
                           dyadic(seed), .125 + dyadic(seed) / 8 };
    }
    return result;
}
void compareInstances(const Pair& a, const Pair& b) {
    for (std::size_t n = 0; n < 4; ++n) for (std::size_t band = 0; band < 3; ++band) for (std::size_t ch = 0; ch < 4; ++ch)
        equal(a.legacy.result.bands[n][band][ch], b.legacy.result.bands[n][band][ch], "independent-old", (n * 3 + band) * 4 + ch);
    for (std::size_t i = 0; i < 72; ++i) {
        equal(a.legacy.result.history[i], b.legacy.result.history[i], "independent-old-history", i);
        equal(a.direct.result.history[i], b.direct.result.history[i], "independent-new-history", i);
    }
}
void finiteTraces() {
    std::uint64_t seed = 0x374ac91b2d0658efULL;
    const auto distinct = network(1, seed);
    for (std::size_t channel = 0; channel < 4; ++channel) for (std::size_t frame = 0; frame < 4; ++frame) {
        Pair pair; ++scenarios; stage = "asymmetric-channel-frame-impulse";
        for (unsigned n = 0; n < 32; ++n) {
            InputBlock wet {}, dry {};
            if (n == 0) (channel < 2 ? wet : dry)[frame][channel % 2] = channel % 2 ? -.5 : 1.;
            pair.run(distinct, wet, dry);
        }
    }
    for (unsigned style = 0; style < 3; ++style) {
        Pair pair; ++scenarios; stage = "seeded-asymmetric-random"; pair.seed(history(seed));
        const auto coefficients = network(style, seed);
        for (unsigned n = 0; n < 256; ++n) { const auto wet = input(seed), dry = input(seed); pair.run(coefficients, wet, dry); }
    }
    Pair pair; ++scenarios; stage = "changing-coefficients-with-retained-history"; pair.seed(history(seed));
    for (unsigned n = 0; n < 512; ++n) {
        const auto coefficients = network(2, seed); const auto wet = input(seed), dry = input(seed);
        pair.run(coefficients, wet, dry); if (n % 73 == 0) pair.clear();
    }
    Pair a, b, control; ++scenarios; stage = "independent-interleaved-instances-and-reset";
    const auto initial = history(seed); a.seed(initial); control.seed(initial); b.seed(history(seed));
    for (unsigned n = 0; n < 256; ++n) {
        const auto coefficients = network(2, seed); const auto wet = input(seed), dry = input(seed);
        a.run(coefficients, wet, dry);
        const auto other = network(2, seed); const auto otherWet = input(seed), otherDry = input(seed);
        b.run(other, otherWet, otherDry); control.run(coefficients, wet, dry); compareInstances(a, control);
        if (n % 61 == 0) { a.clear(); control.clear(); b.clear(); }
    }
}
constexpr std::array<std::uint64_t, 24> edges {
    0, 0x8000000000000000ULL, 1, 0x8000000000000001ULL,
    0x000fffffffffffffULL, 0x800fffffffffffffULL, 0x0010000000000000ULL, 0x8010000000000000ULL,
    0x3ff0000000000000ULL, 0xbff0000000000000ULL, 0x7fefffffffffffffULL, 0xffefffffffffffffULL,
    0x7ff0000000000000ULL, 0xfff0000000000000ULL, 0x7ff0000000000001ULL, 0xfff0000000000001ULL,
    0x7ff7ffffffffffffULL, 0xfff7ffffffffffffULL, 0x7ff8000000000000ULL, 0xfff8000000000000ULL,
    0x7ff8000000000001ULL, 0xfff8000000000001ULL, 0x7fffffffffffffffULL, 0xffffffffffffffffULL };
void boundaryTraces() {
    std::uint64_t seed = 0x845e719a2d6f03bcULL; Pair pair; const auto coefficients = network(1, seed);
    for (auto edge : edges) for (std::size_t ch = 0; ch < 4; ++ch) for (std::size_t n = 0; n < 4; ++n) {
        ++scenarios; stage = "input-extremes-payload-sign-and-recovery"; pair.clear(); pair.seed(history(seed));
        InputBlock wet {}, dry {}; (ch < 2 ? wet : dry)[n][ch % 2] = value(edge);
        pair.run(coefficients, wet, dry); pair.run(coefficients, {}, {});
        pair.clear(); pair.run(coefficients, input(seed), input(seed));
    }
    for (std::size_t filter = 0; filter < 5; ++filter) for (std::size_t coefficient = 0; coefficient < 5; ++coefficient)
        for (auto edge : edges) {
            ++scenarios; stage = "coefficient-extremes-and-reset"; pair.clear(); pair.seed(history(seed));
            auto changed = coefficients; changed[filter][coefficient] = value(edge);
            pair.run(changed, input(seed), input(seed)); pair.run(coefficients, {}, {});
            pair.clear(); pair.run(coefficients, input(seed), input(seed));
        }
}
void controlStatusTraces() {
    std::uint64_t seed = 0x43f62db891a507ceULL; Pair pair;
    for (unsigned rounding = 0; rounding < 4; ++rounding) for (unsigned denormal = 0; denormal < 4; ++denormal)
        for (unsigned sticky : {0u, 1u, 2u, 4u, 8u, 16u, 32u, 63u}) {
            ++environments; ++scenarios; stage = "masked-rounding-FTZ-DAZ-sticky-status";
            const auto entry = 0x1f80u | (rounding << 13) | ((denormal & 1) ? 0x40u : 0u)
                | ((denormal & 2) ? 0x8000u : 0u) | sticky;
            pair.clear(); auto seeded = history(seed); seeded[3] = value(1); seeded[47] = value(0x8000000000000000ULL); pair.seed(seeded);
            for (unsigned n = 0; n < 8; ++n) {
                auto wet = input(seed), dry = input(seed);
                if (n < 4) wet[n][n % 2] = value(edges[12 + n]);
                pair.run(network(2, seed), wet, dry, entry);
            }
        }
}
void counterControl() {
    allocationWitness::allocations = allocationWitness::frees = 0; allocationWitness::enabled = true;
    void* memory = ::operator new(1); ::operator delete(memory); allocationWitness::enabled = false;
    require(allocationWitness::allocations == 1 && allocationWitness::frees == 1, "C++ counter positive control failed");
}
struct Environment {
    unsigned saved = _mm_getcsr(); int savedError = errno;
    ~Environment() { allocationWitness::enabled = false; _mm_setcsr(saved); errno = savedError; }
};
}
int main() {
    try {
        Environment restore;
        if (!wide_direct_layout::avxAdmitted()) {
            std::cout << "WIDE DIRECT UNAVAILABLE: CPU/OS AVX admission refused; no kernel entered.\n";
            return 2;
        }
        counterControl(); finiteTraces(); boundaryTraces(); controlStatusTraces();
        require(blocks == 6536 && scenarios == 1133 && resets == 2119 && environments == 128,
                "source-defined trace coverage incomplete");
        require(outputWords == blocks * 48 && historyWords == blocks * 72, "raw output/history coverage incomplete");
        std::cout << "WIDE DIRECT PASS checks=" << checks << " blocks=" << blocks << " scenarios=" << scenarios
                  << " output_words=" << outputWords << " history_words=" << historyWords << " resets=" << resets
                  << " masked_environments=" << environments
                  << "; actual old Quad versus new wet/dry mapping and all576B histories bitwise; fullMXCSR/errno matched; admitted-call C++heap0/0. "
                     "NaN payload/sign and signed zero compared as raw bits. Unaligned outputs/canaries/sentinel writes checked. "
                     "Handles allocate outside admitted calls; counters do not intercept direct malloc/OS heaps. "
                     "No scalar/unavailable-kernel, unmasked trap, timing, CPU gate or adoption claim. "
                     "Performance RED is prior FullCPU4/12; no invented behavioral RED. Inherited parent model; no overrides.\n";
        return 0;
    } catch (const std::exception& error) {
        allocationWitness::enabled = false;
        std::cerr << "WIDE DIRECT INCOMPLETE/FAIL stage=" << stage << " blocks=" << blocks
                  << " checks=" << checks << ": " << error.what() << '\n';
        return 1;
    }
}
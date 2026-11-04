#include <Saturation.h>
#include <SaturationGainRuntime.h>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <new>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#include <malloc.h>
#endif
#include "../../probes/saturation/SaturationCalibration.h"

namespace airack::saturation_detail {
struct CoefficientView { const char* name; std::span<const double> values; };
std::span<const CoefficientView> saturationCoefficientManifest() noexcept;
double preparedAdaa(SaturationCharacter, double x, double previous) noexcept;
}

#if !defined(_DEBUG)
namespace allocationWitness {
thread_local bool enabled = false;
thread_local std::size_t allocations = 0, frees = 0, bytes = 0;
void allocated(std::size_t size) noexcept { if (enabled) { ++allocations; bytes += size; } }
void freed(void* pointer) noexcept { if (enabled && pointer) ++frees; }
}
void* operator new(std::size_t size) { if (auto* p = std::malloc(size ? size : 1)) { allocationWitness::allocated(size); return p; } throw std::bad_alloc(); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { allocationWitness::freed(p); std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
#if defined(_WIN32)
void* operator new(std::size_t size, std::align_val_t a) { if (auto* p = _aligned_malloc(size ? size : 1, std::size_t(a))) { allocationWitness::allocated(size); return p; } throw std::bad_alloc(); }
void* operator new[](std::size_t size, std::align_val_t a) { return ::operator new(size, a); }
void operator delete(void* p, std::align_val_t) noexcept { allocationWitness::freed(p); _aligned_free(p); }
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
#endif
#endif

namespace {
using namespace airack;
using Frame = Saturation::Frame;
namespace fs = std::filesystem;
constexpr std::array partitions {1u, 17u, 64u, 127u, 512u};
constexpr double tolerance = 1e-12;
int failures = 0, checks = 0;
const char* section = "";
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) { ++failures; std::cout << "FAIL " << section << ':' << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
template<class Test> void group(const char* name, Test test) {
    if (const auto* selected = std::getenv("RACKSYNTH_SATURATION_TEST_GROUP"); selected && std::string_view(selected) != name) return;
    section = name; const int before = failures, count = checks; test();
    std::cout << "GROUP " << name << " checks=" << checks - count << " failures=" << failures - before << std::endl;
}
bool finite(Frame x) { return std::isfinite(x[0]) && std::isfinite(x[1]); }
bool exact(Frame a, Frame b) { return std::memcmp(a.data(), b.data(), sizeof(Frame)) == 0; }
double error(Frame a, Frame b = {}) { return std::max(std::abs(a[0] - b[0]), std::abs(a[1] - b[1])); }
unsigned duration(double rate) { return static_cast<unsigned>(std::max(1., std::round(.020 * rate))); }
Frame signal(unsigned n, double rate) {
    return {.021 * std::sin(2 * std::numbers::pi * 307 * n / rate) + .008 * std::cos(2 * std::numbers::pi * 7103 * n / rate),
            .017 * std::cos(2 * std::numbers::pi * 983 * n / rate) - .005 * std::sin(2 * std::numbers::pi * 11371 * n / rate)};
}
SaturationSettings profile(unsigned mode = 0, unsigned character = 0) {
    SaturationSettings s; s.mode = SaturationMode(mode); s.driveCompensation = false;
    for (auto& lane : s.lanes) { lane.character = SaturationCharacter(character); lane.driveDb = 18; }
    return s;
}
SaturationSettings distinct() {
    auto s = profile(); s.inputTrimDb = -24; s.outputTrimDb = 12; s.mix = .7f;
    s.lowCrossoverHz = 40; s.highCrossoverHz = 16000;
    s.preLowCutEnabled = s.postHighCutEnabled = true; s.preLowCutHz = 20; s.postHighCutHz = 1000;
    for (unsigned n = 0; n < 4; ++n) {
        auto& l = s.lanes[n]; l.character = SaturationCharacter(n % 3); l.bypassed = n % 2 != 0;
        l.driveDb = float(12 * n); l.preTiltDb = float(4 * int(n) - 6);
        l.mix = float(n) / 3; l.trimDb = float(12 * int(n) - 18);
    }
    return s;
}
void ready(Saturation& c, const SaturationSettings& s, double rate) {
    CHECK(c.setSettings(s)); CHECK(c.prepare(rate)); CHECK(c.settings() == s);
}
void lockstep(Saturation& a, Saturation& b, unsigned count, double rate, unsigned begin = 0) {
    bool same = true;
    for (unsigned n = 0; n < count; ++n) { const auto x = signal(n + begin, rate); same = exact(a.process(x), b.process(x)) && same; }
    CHECK(same);
}
void charge(Saturation& a, Saturation& b, double rate) {
    lockstep(a, b, 79, rate);
    auto next = a.settings(); next.mode = SaturationMode::threeBand; next.lanes[0].character = SaturationCharacter::glow;
    CHECK(a.setSettings(next)); CHECK(b.setSettings(next)); lockstep(a, b, 17, rate, 79);
    next.mode = SaturationMode::fullBand; next.lanes[0].character = SaturationCharacter::dense;
    CHECK(a.setSettings(next)); CHECK(b.setSettings(next));
}
void admission() {
    for (unsigned factor : {4u}) {
        Saturation candidate;
        const bool ordinary = candidate.prepare(384000);
        const bool extended = candidate.prepare(768000);
        std::cout << "ADMISSION rate=384000 factor=" << factor << " actual=" << ordinary << " required=1\n"
                  << "ADMISSION rate=768000 factor=" << factor << " actual=" << extended << " required=1\n";
        CHECK(ordinary); CHECK(extended);
        for (double rate : {8000., 44100., 48000., 96000., 384000., 384001., 768000.}) {
            CHECK(candidate.prepare(rate));
            std::cout << "ADMISSION rate=" << rate << " factor=" << factor << " required=1\n";
        }
        Saturation actual, control; ready(actual, distinct(), 384001); ready(control, distinct(), 384001);
        charge(actual, control, 384001); const auto target = actual.settings();
        for (double rate : {std::nextafter(8000., 0.), std::nextafter(768000., std::numeric_limits<double>::infinity()),
                            0., -1., std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
            CHECK(!actual.prepare(rate)); CHECK(actual.settings() == target); lockstep(actual, control, 31, 384001);
        }
        Saturation fresh; CHECK(exact(fresh.process({1, -1}), {})); CHECK(!fresh.prepare(7999));
        CHECK(exact(fresh.process({1, -1}), {})); CHECK(fresh.prepare(8000));
    }
}
void retainedTargets() {
    const auto bank = distinct();
    for (unsigned factor : {4u}) {
        Saturation before, after; CHECK(before.settings() == SaturationSettings{});
        ready(before, bank, 384001); CHECK(after.prepare(384001)); CHECK(after.setSettings(bank));
        CHECK(after.settings() == bank); lockstep(before, after, 97, 384001);
        for (unsigned mode : {0u, 1u, 0u}) {
            auto edit = bank; edit.mode = SaturationMode(mode); CHECK(before.setSettings(edit)); CHECK(after.setSettings(edit));
            CHECK(before.settings() == edit); CHECK(before.settings().lanes == bank.lanes); lockstep(before, after, 17, 384001);
        }
        charge(before, after, 384001);
        std::vector<SaturationSettings> rejected;
        const auto target = before.settings();
        for (unsigned lane = 0; lane < 4; ++lane) {
            auto bad = target; bad.lanes[lane].character = SaturationCharacter(255); rejected.push_back(bad);
            for (unsigned field = 0; field < 4; ++field) for (unsigned kind = 0; kind < 4; ++kind) {
                bad = target; auto& l = bad.lanes[lane];
                std::array<float*, 4> values {&l.driveDb, &l.preTiltDb, &l.mix, &l.trimDb};
                constexpr std::array lows {0.f, -6.f, 0.f, -18.f};
                constexpr std::array highs {36.f, 6.f, 1.f, 18.f};
                *values[field] = kind == 0 ? std::numeric_limits<float>::quiet_NaN() : kind == 1 ?
                    std::numeric_limits<float>::infinity() : kind == 2 ? std::nextafter(lows[field], -INFINITY) : std::nextafter(highs[field], INFINITY);
                rejected.push_back(bad);
            }
        }
        auto bad = target; bad.mode = SaturationMode(255); rejected.push_back(bad);
        for (unsigned field = 0; field < 7; ++field) for (unsigned kind = 0; kind < 4; ++kind) {
            bad = target;
            std::array<float*, 7> values {&bad.inputTrimDb, &bad.outputTrimDb, &bad.mix, &bad.lowCrossoverHz,
                                        &bad.highCrossoverHz, &bad.preLowCutHz, &bad.postHighCutHz};
            constexpr std::array lows {-24.f, -24.f, 0.f, 40.f, 500.f, 20.f, 1000.f};
            constexpr std::array highs {24.f, 12.f, 1.f, 2000.f, 16000.f, 2000.f, 20000.f};
            *values[field] = kind == 0 ? std::numeric_limits<float>::quiet_NaN() : kind == 1 ?
                std::numeric_limits<float>::infinity() : kind == 2 ? std::nextafter(lows[field], -INFINITY) : std::nextafter(highs[field], INFINITY);
            rejected.push_back(bad);
        }
        for (const auto& invalid : rejected) {
            CHECK(!before.setSettings(invalid)); CHECK(before.settings() == target); lockstep(before, after, 3, 384001);
        }
        before.reset(); CHECK(before.settings() == target);
        Saturation resetControl; ready(resetControl, target, 384001); lockstep(before, resetControl, 97, 384001);
        CHECK(before.prepare(768000)); CHECK(before.settings() == target);
        Saturation preparedControl; ready(preparedControl, target, 768000); lockstep(before, preparedControl, 97, 768000);
        std::cout << "RETAINED factor=" << factor << " distinct_banks=4 rejected=" << rejected.size() << " exact_audio=1\n";
    }
}
struct Event { unsigned frame; unsigned operation; SaturationSettings settings; };
std::vector<Event> schedule(unsigned length) {
    constexpr unsigned start = 7;
    const unsigned boundary = start + length, secondStart = boundary + 1, secondBoundary = secondStart + length;
    auto a = profile(), b = profile(1, 1), c = profile(0, 2);
    b.preLowCutEnabled = b.postHighCutEnabled = true; b.driveCompensation = true; b.lanes[2].bypassed = true;
    c.outputTrimDb = -6; c.lanes[3].mix = .25f;
    auto bad = c; bad.lanes[3].mix = std::numeric_limits<float>::quiet_NaN();
    return {{start, 0, b}, {start, 0, c}, {start + 1, 0, c}, {boundary - 1, 0, b}, {boundary, 1, bad},
            {secondStart, 0, a}, {secondBoundary - 1, 2, {}}, {secondBoundary, 0, b}, {secondBoundary + 1, 0, c}};
}
void scheduleCoverage() {
    const auto events = schedule(duration(44101));
    CHECK(events.size() == 9); CHECK(events[0].frame == 7);
    const unsigned boundary = events[0].frame + 882, second = events[5].frame + 882;
    CHECK(events[5].operation == 0 && events[5].frame == boundary + 1);
    CHECK(events[6].operation == 2);
    for (unsigned n = 0; n < 3; ++n) {
        CHECK(events[3 + n].frame == boundary - 1 + n);
        CHECK(events[6 + n].frame == second - 1 + n);
    }
    std::cout << "EVENT_EDGES rate=44101 first_structural_frame=" << events[0].frame << " independently_checked_round_frames=882"
              << " first_actual=" << events[3].frame << ',' << events[4].frame << ',' << events[5].frame
              << " first_required=" << boundary - 1 << ',' << boundary << ',' << boundary + 1
              << " second_structural_frame=" << events[5].frame
              << " second_actual=" << events[6].frame << ',' << events[7].frame << ',' << events[8].frame
              << " second_required=" << second - 1 << ',' << second << ',' << second + 1
              << " second_lastframe_reset=planned_interruption_not_observed_completion\n";
}
void apply(Saturation& c, const Event& e) {
    if (e.operation == 2) c.reset(); else if (e.operation == 1) CHECK(!c.setSettings(e.settings)); else CHECK(c.setSettings(e.settings));
}
std::vector<Frame> renderSchedule(unsigned partition, unsigned factor, bool block, bool wrongOrder = false, bool repeat = true) {
    constexpr double rate = 44101; const unsigned length = duration(rate), count = 3 * length + 73;
    Saturation c; ready(c, profile(), rate); auto events = schedule(length);
    if (wrongOrder) std::swap(events[0], events[1]);
    std::vector<Frame> output(count); for (unsigned n = 0; n < count; ++n) output[n] = signal(n, rate);
    std::size_t event = 0; unsigned at = 0;
    while (at < count) {
        while (event < events.size() && events[event].frame == at) apply(c, events[event++]);
        if (repeat) CHECK(c.setSettings(c.settings()));
        if (block) c.processBlock({}); // Scalar reference makes no empty call: catches any advancement.
        const unsigned end = std::min({count, at + partition, event < events.size() ? events[event].frame : count});
        if (block) c.processBlock(std::span<Frame>(output).subspan(at, end - at));
        else for (unsigned n = at; n < end; ++n) output[n] = c.process(output[n]);
        at = end;
    }
    return output;
}
void blockOrder() {
    for (unsigned factor : {4u}) {
        const auto scalar = renderSchedule(1, factor, false, false, false);
        const auto wrong = renderSchedule(64, factor, true, true);
        double witness = 0; for (unsigned n = 0; n < scalar.size(); ++n) witness = std::max(witness, error(scalar[n], wrong[n]));
        CHECK(witness > 1e-7);
        for (unsigned partition : partitions) {
            const auto block = renderSchedule(partition, factor, true);
            CHECK(block.size() == scalar.size()); CHECK(std::memcmp(block.data(), scalar.data(), scalar.size() * sizeof(Frame)) == 0);
            std::cout << "BLOCK rate=44101 factor=" << factor << " partition=" << partition << " frames=" << block.size()
                      << " differing_words=0 wrong_order_witness=" << witness << '\n';
        }
    }
}
Frame blend(Frame a, Frame b, double weight) {
    for (unsigned ch = 0; ch < 2; ++ch) a[ch] += weight * (b[ch] - a[ch]); return a;
}
void transitionLaws() {
    for (double rate : {8000., 44100., 48000., 96000., 384001., 768000., 44101.}) {
    const auto length = duration(rate);
    CHECK(length == unsigned(std::max(1., std::round(.020 * rate))));
    if (rate == 44101) { CHECK(length == 882); CHECK(std::ceil(.020 * rate) == 883); }
    for (unsigned factor : {4u}) for (unsigned edit = 0; edit < 8; ++edit) for (bool reverse : {false, true}) {
        auto a = profile(), b = a;
        if (edit < 3) { for (auto& l : a.lanes) l.character = SaturationCharacter(edit); b = a; for (auto& l : b.lanes) l.character = SaturationCharacter((edit + 1) % 3); }
        if (edit == 3) b.mode = SaturationMode::threeBand;
        if (edit == 4) { a.preLowCutHz = b.preLowCutHz = 600; b.preLowCutEnabled = true; }
        if (edit == 5) { a.postHighCutHz = b.postHighCutHz = 1200; b.postHighCutEnabled = true; }
        if (edit == 6) { b.mode = SaturationMode::threeBand; b.preLowCutEnabled = b.postHighCutEnabled = true; for (auto& l : b.lanes) l.character = SaturationCharacter::dense; }
        if (edit == 7) { b.mode = SaturationMode::threeBand; for (auto& l : b.lanes) l.character = SaturationCharacter::glow; }
        if (reverse) std::swap(a, b);
        Saturation actual, from, to; ready(actual, a, rate); ready(from, a, rate); ready(to, b, rate);
        auto target = b;
        if (edit == 7) {
            auto continuous = a;
            for (auto* s : {&target, &continuous}) { s->driveCompensation = true; s->outputTrimDb = -6; for (auto& l : s->lanes) l.bypassed = true; }
            CHECK(from.setSettings(continuous)); CHECK(to.setSettings(target));
        }
        CHECK(actual.setSettings(target)); CHECK(actual.settings() == target);
        double maximum = 0, separation = 0;
        for (unsigned n = 0; n <= length + 8; ++n) {
            const auto x = signal(n, rate), old = from.process(x), next = to.process(x), y = actual.process(x);
            const auto expected = blend(old, next, std::min(1., double(n) / (length - 1)));
            maximum = std::max(maximum, error(y, expected)); separation = std::max(separation, error(old, next));
            if (n == 0 || n == length / 2 || n == length - 2 || n == length - 1 || n == length || n == length + 1) CHECK(error(y, expected) <= tolerance);
        }
        CHECK(maximum <= tolerance); CHECK(separation > 1e-7);
        std::cout << "STRUCTURAL rate=" << rate << " factor=" << factor << " edit=" << edit << " reverse=" << reverse
                  << " round_frames=" << length << " first_weight=0 final_weight=1 maxabs=" << maximum << '\n';
    }
    }
    // Latest structural requests cannot affect the active fade; at its boundary the newest request starts.
    for (double fractional : {44101., 384001.}) for (unsigned factor : {4u}) for (unsigned step : {1u, 17u, 64u}) {
        const auto fade = duration(fractional); auto a = profile(), b = profile(0, 1), c = profile(0, 2);
        Saturation queued, explicitChange; ready(queued, a, fractional); ready(explicitChange, a, fractional);
        CHECK(queued.setSettings(b)); CHECK(explicitChange.setSettings(b)); double maximum = 0;
        for (unsigned n = 0; n < 2 * fade + 33; ++n) {
            if (n < fade && n % step == 0) { CHECK(queued.setSettings(n % 2 ? a : b)); CHECK(queued.setSettings(c)); CHECK(queued.settings() == c); }
            if (n == fade) CHECK(explicitChange.setSettings(c));
            const auto x = signal(n, fractional); maximum = std::max(maximum, error(queued.process(x), explicitChange.process(x)));
        }
        CHECK(maximum == 0); CHECK(queued.settings() == c);
        std::cout << "RETARGET rate=" << fractional << " factor=" << factor << " every=" << step << " round_frames=" << fade << " maxabs=" << maximum << '\n';
    }
}
void integrity() {
    unsigned cases = 0;
    for (double rate : {8000., 384000., 768000.}) for (unsigned factor : {4u})
        for (unsigned mode : {0u, 1u}) for (unsigned character : {0u, 1u, 2u}) {
        auto s = profile(mode, character); s.preLowCutEnabled = s.postHighCutEnabled = true;
        s.lowCrossoverHz = 40; s.highCrossoverHz = 16000; s.preLowCutHz = 20; s.postHighCutHz = 1000;
        s.inputTrimDb = 24; s.outputTrimDb = -24;
        for (unsigned n = 0; n < 4; ++n) { auto& l = s.lanes[n]; l.driveDb = 36; l.preTiltDb = n % 2 ? -6 : 6; l.trimDb = n % 2 ? -18 : 18; }
        Saturation actual, swapped; ready(actual, s, rate); ready(swapped, s, rate);
        auto neutral = profile(mode, character); for (auto& lane : neutral.lanes) lane.driveDb = 0;
        Saturation neutralControl; ready(neutralControl, neutral, rate); bool neutralFinite = true; double neutralPeak = 0;
        for (unsigned n = 0; n < 128; ++n) { const auto y = neutralControl.process(signal(n, rate)); neutralFinite = finite(y) && neutralFinite; neutralPeak = std::max(neutralPeak, error(y)); }
        CHECK(neutralFinite); CHECK(neutralPeak > 1e-8);
        bool valid = true, silent = true; double peak = 0, null = 0;
        for (unsigned n = 0; n < 64; ++n) silent = exact(actual.process({}), {}) && silent;
        CHECK(silent);
        for (unsigned n = 0; n < 2048; ++n) {
            const auto x = signal(n, rate), y = actual.process(x), z = swapped.process({x[1], x[0]});
            valid = finite(y) && finite(z) && valid; peak = std::max(peak, error(y)); null = std::max(null, error(y, {z[1], z[0]}));
        }
        CHECK(valid); CHECK(peak > 1e-8); CHECK(null <= tolerance);
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max()}) {
            auto pending = s; pending.mode = SaturationMode(1 - mode); pending.lanes[0].character = SaturationCharacter((character + 1) % 3);
            CHECK(actual.setSettings(pending)); actual.process(signal(9, rate)); CHECK(actual.setSettings(s));
            CHECK(exact(actual.process({invalid, .1}), {})); CHECK(actual.settings() == s);
            Saturation resetControl; ready(resetControl, s, rate); lockstep(actual, resetControl, 64, rate);
        }
        actual.reset(); CHECK(actual.settings() == s); silent = true;
        for (unsigned n = 0; n < 64; ++n) silent = exact(actual.process({}), {}) && silent;
        CHECK(silent); CHECK(actual.prepare(rate)); Saturation reprepare; ready(reprepare, s, rate); lockstep(actual, reprepare, 64, rate);
        std::cout << "INTEGRITY rate=" << rate << " factor=" << factor << " mode=" << mode << " character=" << character
                  << " peak=" << peak << " neutral_peak=" << neutralPeak << " swap_maxabs=" << null << " recovery_inputs=4\n"; ++cases;
    }
    CHECK(cases == 18);
}
SaturationSettings rapid(unsigned n) {
    auto s = profile(n % 2, n % 3); s.inputTrimDb = n % 2 ? -24.f : 24.f; s.outputTrimDb = n % 2 ? 12.f : -24.f;
    s.preLowCutEnabled = n % 2 != 0; s.postHighCutEnabled = n % 3 != 0; s.driveCompensation = n % 5 != 0;
    s.lowCrossoverHz = 40; s.highCrossoverHz = 16000; s.preLowCutHz = 20; s.postHighCutHz = 1000;
    for (auto& l : s.lanes) { l.bypassed = n % 7 == 0; l.driveDb = n % 2 ? 0.f : 36.f; l.preTiltDb = n % 2 ? -6.f : 6.f; l.trimDb = n % 2 ? -18.f : 18.f; l.mix = n % 2 ? 0.f : 1.f; }
    return s;
}
void rapidOverlap() {
    for (unsigned factor : {4u}) for (unsigned step : {1u, 17u, 64u}) {
        constexpr double rate = 768000; Saturation scalar, block, swapped;
        const auto initial = rapid(0); ready(scalar, initial, rate); ready(block, initial, rate); ready(swapped, initial, rate);
        bool valid = true, same = true; double null = 0, peak = 0; Frame buffer {};
        const unsigned count = 2 * duration(rate) + 65;
        for (unsigned n = 0; n < count; ++n) {
            if (n % step == 0) { const auto s = rapid(n / step); CHECK(scalar.setSettings(s)); CHECK(block.setSettings(s)); CHECK(swapped.setSettings(s)); CHECK(block.settings() == s); }
            if (n == duration(rate) / 2) { scalar.reset(); block.reset(); swapped.reset(); }
            const auto x = signal(n, rate), y = scalar.process(x), z = swapped.process({x[1], x[0]});
            buffer = x; block.processBlock(std::span<Frame>(&buffer, 1)); block.processBlock({});
            same = exact(y, buffer) && same; valid = finite(y) && finite(z) && valid;
            null = std::max(null, error(y, {z[1], z[0]})); peak = std::max(peak, error(y));
        }
        CHECK(valid); CHECK(same); CHECK(null <= tolerance); CHECK(peak > 1e-8);
        std::cout << "OVERLAP rate=" << rate << " factor=" << factor << " every=" << step << " frames=" << count
                  << " endpoint_bound=2 exact_block=" << same << " swap_maxabs=" << null << " peak=" << peak << '\n';
    }
}
void retainedRawControls() {
    Saturation p; auto s = distinct(); CHECK(p.setSettings(s) && p.prepare(48000));
    const auto before = p.settings(); auto invalid = s;
    invalid.lanes[3].driveDb = std::numeric_limits<float>::quiet_NaN();
    CHECK(!p.setSettings(invalid)); CHECK(p.settings() == before);
    CHECK(p.setSettings(s)); p.reset(); CHECK(p.settings() == s);
    s.lowCrossoverHz = 2000; s.highCrossoverHz = 500; CHECK(p.setSettings(s));
    const auto c = effectiveSaturationCrossovers(s, 8000);
    CHECK(c.highHz == 500 && c.lowHz == 400); CHECK(p.settings().lowCrossoverHz == 2000);
}
void neutralAndDryRoutes() {
    for (unsigned mode : {0u, 1u}) for (unsigned character : {0u, 1u, 2u}) {
        auto s = profile(mode, character); for (auto& lane : s.lanes) lane.driveDb = 0;
        Saturation wet, half, dry, laneDry, bypassed, tilted;
        ready(wet, s, 48000); auto h = s; h.mix = .5f; ready(half, h, 48000);
        auto g = profile(mode, character); g.mix = 0; g.preLowCutEnabled = g.postHighCutEnabled = true;
        g.preLowCutHz = 600; g.postHighCutHz = 1200; ready(dry, g, 48000);
        auto l = profile(mode, character), b = l, t = s;
        for (auto& lane : l.lanes) { lane.mix = 0; lane.preTiltDb = 6; lane.trimDb = 12; }
        for (auto& lane : b.lanes) { lane.bypassed = true; lane.preTiltDb = -6; lane.trimDb = -12; }
        for (auto& lane : t.lanes) lane.preTiltDb = 6;
        ready(laneDry, l, 48000); ready(bypassed, b, 48000); ready(tilted, t, 48000);
        double maximum = 0, energy = 0, toneDifference = 0;
        for (unsigned n = 0; n < 8192; ++n) {
            const auto x = signal(n, 48000), y = wet.process(x);
            maximum = std::max({maximum, error(y, half.process(x)), error(y, dry.process(x)), error(y, laneDry.process(x)), error(y, bypassed.process(x))});
            toneDifference = std::max(toneDifference, error(y, tilted.process(x))); energy += y[0] * y[0] + y[1] * y[1];
        }
        CHECK(maximum <= tolerance); CHECK(energy > .01); CHECK(toneDifference > 1e-4);
        std::cout << "NEUTRAL mode=" << mode << " character=" << character << " mix_endpoints_and_half_maxabs=" << maximum
                  << " energy=" << energy << " zero_drive_tilt_difference=" << toneDifference << '\n';
    }
}
using Complex = std::complex<double>;
Complex response(const SaturationSettings& s, double hz) {
    constexpr double rate = 48000, amplitude = .01; constexpr unsigned count = 8192, warmup = 24000;
    const double omega = 2 * std::numbers::pi * hz / rate;
    Saturation c; ready(c, s, rate); Complex result {};
    for (int n = -int(warmup); n < int(count); ++n) {
        const auto y = c.process({amplitude * std::cos(omega * n), 0});
        if (n >= 0) result += y[0] * std::polar(2. / (amplitude * count), -omega * n);
    }
    return result;
}
Complex filterResponse(bool high, double corner, double filterRate, double hz) {
    const double k = std::tan(std::numbers::pi * corner / filterRate), k2 = k * k;
    const double d = 1 + std::sqrt(2.) * k + k2, a1 = 2 * (k2 - 1) / d, a2 = (1 - std::sqrt(2.) * k + k2) / d;
    const auto z = std::polar(1., -2 * std::numbers::pi * hz / filterRate);
    return (high ? (1. - 2. * z + z * z) / d : k2 * (1. + 2. * z + z * z) / d) / (1. + a1 * z + a2 * z * z);
}
Complex allpassResponse(double corner, double filterRate, double hz) {
    const double k = std::tan(std::numbers::pi * corner / filterRate), d = 1 + std::sqrt(2.) * k + k * k;
    const double a1 = 2 * (k * k - 1) / d, a2 = (1 - std::sqrt(2.) * k + k * k) / d;
    const auto z = std::polar(1., -2 * std::numbers::pi * hz / filterRate);
    return (a2 + a1 * z + z * z) / (1. + a1 * z + a2 * z * z);
}
Complex shelfResponse(bool high, double db, double hz) {
    constexpr double filterRate = 192000, corner = 1000;
    const double a = std::pow(10., db / 40), w = 2 * std::numbers::pi * corner / filterRate;
    const double cs = std::cos(w), root = std::sqrt(2 * a) * std::sin(w);
    double b0, b1, b2, a0, a1, a2;
    if (high) {
        b0 = a * ((a + 1) + (a - 1) * cs + root); b1 = -2 * a * ((a - 1) + (a + 1) * cs); b2 = a * ((a + 1) + (a - 1) * cs - root);
        a0 = (a + 1) - (a - 1) * cs + root; a1 = 2 * ((a - 1) - (a + 1) * cs); a2 = (a + 1) - (a - 1) * cs - root;
    } else {
        b0 = a * ((a + 1) - (a - 1) * cs + root); b1 = 2 * a * ((a - 1) - (a + 1) * cs); b2 = a * ((a + 1) - (a - 1) * cs - root);
        a0 = (a + 1) + (a - 1) * cs + root; a1 = -2 * ((a - 1) + (a + 1) * cs); a2 = (a + 1) + (a - 1) * cs - root;
    }
    const auto z = std::polar(1., -2 * std::numbers::pi * hz / filterRate);
    return (b0 + b1 * z + b2 * z * z) / (a0 + a1 * z + a2 * z * z);
}
void linearComplexLaws() {
    // Ratios cancel the retained antialiasing return, exposing the independent
    // bilinear Butterworth/shelf and compensated crossover transfer functions.
    for (double requested : {100., 1000., 6000.}) {
        const double hz = 48000 * std::round(requested * 8192 / 48000) / 8192;
        SaturationSettings full; const auto base = response(full, hz); CHECK(std::abs(base) > .1);
        auto three = full; three.mode = SaturationMode::threeBand;
        const auto ratio = response(three, hz) / base;
        const auto expected = allpassResponse(180, 192000, hz) * allpassResponse(3500, 192000, hz);
        CHECK(std::abs(ratio - expected) <= 1e-8); CHECK(std::abs(std::abs(ratio) - 1) <= 1e-8);
        for (unsigned mode : {0u, 1u}) {
            auto s = full; s.mode = SaturationMode(mode); const auto neutral = response(s, hz);
            auto pre = s; pre.preLowCutEnabled = true; pre.preLowCutHz = 600;
            auto post = s; post.postHighCutEnabled = true; post.postHighCutHz = 1200;
            auto tilt = s; for (auto& lane : tilt.lanes) lane.preTiltDb = 6;
            const double lowError = std::abs(response(pre, hz) / neutral - filterResponse(true, 600, 48000, hz));
            const double highError = std::abs(response(post, hz) / neutral - filterResponse(false, 1200, 192000, hz));
            const double tiltError = std::abs(response(tilt, hz) / neutral - shelfResponse(false, -3, hz) * shelfResponse(true, 3, hz));
            CHECK(lowError <= 1e-8); CHECK(highError <= 1e-8); CHECK(tiltError <= 1e-8);
            std::cout << "LINEAR_COMPLEX mode=" << mode << " hz=" << hz << " pre12db_error=" << lowError << " post12db_error=" << highError << " shelf1khz_S1_error=" << tiltError << '\n';
        }
    }
}
// A small scalar signal-path oracle puts the independently calculated dB ramp
// before the return filters. Multiplying the final output by a changing gain
// would incorrectly commute a time-varying operation through filter histories.
struct ScalarPhase {
    std::array<double, 5> direct {}, directState {};
    std::array<double, 4> delayed {}, delayedState {};
    unsigned directCount = 0, delayedCount = 0; double held = 0;
    ScalarPhase(double width, double rejection) {
        const auto c = juce::dsp::FilterDesign<double>::designIIRLowpassHalfBandPolyphaseAllpassMethod(width, rejection);
        directCount = unsigned(c.directPath.size()); delayedCount = unsigned(c.delayedPath.size() - 1);
        CHECK(directCount <= direct.size() && delayedCount <= delayed.size());
        for (unsigned n = 0; n < directCount; ++n) direct[n] = c.directPath.getObjectPointer(int(n))->coefficients[0];
        for (unsigned n = 0; n < delayedCount; ++n) delayed[n] = c.delayedPath.getObjectPointer(int(n + 1))->coefficients[0];
    }
    template<std::size_t N> static double cascade(double x, const std::array<double, N>& c, std::array<double, N>& state, unsigned count) {
        for (unsigned n = 0; n < count; ++n) { const double y = c[n] * x + state[n]; state[n] = x - c[n] * y; x = y; }
        return x;
    }
    std::array<double, 2> up(double x) { return {cascade(x, direct, directState, directCount), cascade(x, delayed, delayedState, delayedCount)}; }
    double down(double even, double odd) {
        const double result = .5 * (held + cascade(even, direct, directState, directCount));
        held = cascade(odd, delayed, delayedState, delayedCount); return result;
    }
};
struct ScalarFullBand {
    std::array<ScalarPhase, 2> up1 {{{.045, -110}, {.045, -110}}}, up2 {{{.12, -100}, {.12, -100}}};
    std::array<ScalarPhase, 2> down1 {{{.045, -110}, {.045, -110}}}, down2 {{{.12, -100}, {.12, -100}}};
    Frame previousX {}, previousU {}, previousDifference {}, dc {}; double envelope = 0;
    double attackSeconds = .002, releaseSeconds = .080, sagLimitDb = 6, minimumSag = 1;
    Frame process(Frame input, SaturationCharacter character, double rate, double compensationDb, double bypassWet = 1) {
        constexpr double reference = 1. / 16, drive = 12, amount = drive / 36;
        const double driveGain = gain_math::canonicalPower10(drive / 20), wetGain = gain_math::canonicalPower10(compensationDb / 20);
        std::array<Frame, 4> high {};
        for (unsigned ch = 0; ch < 2; ++ch) {
            const auto first = up1[ch].up(input[ch]), a = up2[ch].up(first[0]), b = up2[ch].up(first[1]);
            high[0][ch] = a[0]; high[1][ch] = a[1]; high[2][ch] = b[0]; high[3][ch] = b[1];
        }
        for (auto& x : high) {
            Frame u {driveGain * x[0] / reference, driveGain * x[1] / reference};
            if (character == SaturationCharacter::dense) {
                const double peak = std::max(std::abs(u[0]), std::abs(u[1]));
                const double seconds = peak > envelope ? attackSeconds : releaseSeconds;
                envelope = peak + std::exp(-1 / (4 * rate * seconds)) * (envelope - peak);
                const double reduction = std::clamp(sagLimitDb * std::max(envelope - .5, 0.) / (envelope + .5), 0., sagLimitDb);
                const double sag = gain_math::canonicalPower10(-reduction / 20); minimumSag = std::min(minimumSag, sag); u[0] *= sag; u[1] *= sag;
            }
            const auto original = x;
            for (unsigned ch = 0; ch < 2; ++ch) {
                const double linear = .5 * (x[ch] + previousX[ch]);
                double difference = reference * airack::saturation_detail::preparedAdaa(character, u[ch], previousU[ch]) - linear;
                dc[ch] = difference - previousDifference[ch] + std::exp(-2 * std::numbers::pi * 5 / (4 * rate)) * dc[ch];
                previousDifference[ch] = difference;
                if (character == SaturationCharacter::glow) difference = dc[ch];
                x[ch] = linear + bypassWet * ((linear + amount * difference) * wetGain - linear);
            }
            previousX = original; previousU = u;
        }
        Frame result {};
        for (unsigned ch = 0; ch < 2; ++ch) {
            const double even = down2[ch].down(high[0][ch], high[1][ch]);
            const double odd = down2[ch].down(high[2][ch], high[3][ch]);
            result[ch] = down1[ch].down(even, odd);
        }
        return result;
    }
};
void compensationAndWarmBypass() {
    const std::array curves {airack::probe::calibration::soft, airack::probe::calibration::glow, airack::probe::calibration::dense};
    for (unsigned character : {0u, 1u, 2u}) {
        for (unsigned knot = 0; knot <= 36; ++knot) {
            CHECK(saturationDriveCompensationDb(SaturationCharacter(character), float(knot)) == float(curves[character][knot]));
            if (knot < 36) CHECK(saturationDriveCompensationDb(SaturationCharacter(character), float(knot) + .5f) == float(.5 * (curves[character][knot] + curves[character][knot + 1])));
        }
        for (double rate : {8000., 44101., 768000.}) {
            auto off = profile(0, character); for (auto& lane : off.lanes) lane.driveDb = 12;
            auto on = off; on.driveCompensation = true;
            Saturation actual, raw; ScalarFullBand oracle, rawOracle;
            ready(actual, off, rate); ready(raw, off, rate); CHECK(actual.setSettings(on));
            double maximum = 0, rawMaximum = 0; const auto length = duration(rate); unsigned firstFailure = length + 2;
            for (unsigned n = 0; n < length + 2; ++n) {
                const auto x = signal(n, rate), y = actual.process(x);
                const double weight = std::min(1., double(n + 1) / length);
                const double db = n + 1 >= length ? double(float(curves[character][12])) : weight * double(float(curves[character][12]));
                const auto expected = oracle.process(x, SaturationCharacter(character), rate, db);
                const double rawError = error(raw.process(x), rawOracle.process(x, SaturationCharacter(character), rate, 0));
                rawMaximum = std::max(rawMaximum, rawError);
                if (firstFailure == length + 2 && (rawError > tolerance || error(y, expected) > tolerance)) {
                    firstFailure = n;
                    std::cout << "COMPENSATION_DIAGNOSTIC first_frame=" << n << " gain_position=before_AA_return db=" << db
                              << " actual=" << y[0] << ',' << y[1] << " expected=" << expected[0] << ',' << expected[1] << " raw_oracle_error=" << rawError << '\n';
                }
                maximum = std::max(maximum, error(y, expected));
                if (n >= length - 2) CHECK(error(y, expected) <= tolerance);
            }
            CHECK(rawMaximum <= tolerance); CHECK(maximum <= tolerance);
            std::cout << "COMPENSATION character=" << character << " rate=" << rate << " knots=37 round_frames=" << length << " db_ramp_maxabs=" << maximum << " raw_oracle_maxabs=" << rawMaximum << '\n';
            auto bypass = off; for (auto& lane : bypass.lanes) lane.bypassed = true;
            for (bool reverse : {false, true}) {
                Saturation fading; ScalarFullBand bypassOracle; ready(fading, reverse ? bypass : off, rate);
                CHECK(fading.setSettings(reverse ? off : bypass)); double fadeError = 0;
                for (unsigned n = 0; n < length + 2; ++n) {
                    const auto x = signal(n, rate); const double progress = std::min(1., double(n + 1) / length);
                    const auto expected = bypassOracle.process(x, SaturationCharacter(character), rate, 0, reverse ? progress : 1 - progress);
                    const double delta = error(fading.process(x), expected); fadeError = std::max(fadeError, delta);
                    if (n >= length - 2) CHECK(delta <= tolerance);
                }
                CHECK(fadeError <= tolerance);
                std::cout << "BYPASS_LAW character=" << character << " rate=" << rate << " reverse=" << reverse << " round_frames=" << length << " before_return_maxabs=" << fadeError << '\n';
            }
        }
        auto s = profile(1, character); Saturation warm, control, dry;
        ready(warm, s, 48000); ready(control, s, 48000); auto b = s;
        for (auto& lane : b.lanes) lane.bypassed = true;
        ready(dry, b, 48000); CHECK(warm.setSettings(b));
        const auto length = duration(48000); double bypassError = 0, restoredError = 0;
        for (unsigned n = 0; n < 3 * length + 2048; ++n) {
            if (n == 2 * length) CHECK(warm.setSettings(s));
            const auto x = signal(n, 48000), y = warm.process(x), hot = control.process(x), d = dry.process(x);
            if (n >= 2 * length - 32 && n < 2 * length) bypassError = std::max(bypassError, error(y, d));
            if (n >= 3 * length + 1024) restoredError = std::max(restoredError, error(y, hot));
        }
        CHECK(bypassError <= tolerance); CHECK(restoredError <= tolerance);
        std::cout << "WARM_BYPASS character=" << character << " return_settled_bypass_maxabs=" << bypassError << " restored_maxabs=" << restoredError << '\n';
    }
}
void characterMemoryAndRetirement() {
    for (double rate : {8000., 384000., 768000.}) {
        auto settings = profile(0, 2); for (auto& lane : settings.lanes) lane.driveDb = 12;
        Saturation actual; ready(actual, settings, rate); ScalarFullBand oracle;
        std::array<ScalarFullBand, 3> wrong; wrong[0].attackSeconds = .004; wrong[1].releaseSeconds = .040; wrong[2].sagLimitDb = 3;
        const unsigned attack = unsigned(.020 * rate), count = unsigned(.200 * rate);
        double maximum = 0, energy = 0; std::array<double, 3> sensitivity {};
        for (unsigned n = 0; n < count; ++n) {
            // The larger input changes sides on release, testing max(abs(L),abs(R)).
            const Frame input = n < attack ? Frame {8., -.5} : Frame {.001, -.006};
            const auto y = actual.process(input), expected = oracle.process(input, SaturationCharacter::dense, rate, 0);
            maximum = std::max(maximum, error(y, expected)); energy += y[0] * y[0] + y[1] * y[1];
            if (rate == 8000) for (unsigned k = 0; k < wrong.size(); ++k)
                sensitivity[k] = std::max(sensitivity[k], error(y, wrong[k].process(input, SaturationCharacter::dense, rate, 0)));
        }
        CHECK(maximum <= tolerance); CHECK(energy > 1);
        CHECK(oracle.minimumSag >= std::pow(10., -6. / 20)); CHECK(oracle.minimumSag < std::pow(10., -5.9 / 20));
        if (rate == 8000) for (const double delta : sensitivity) CHECK(delta > 1e-6);
        std::cout << "DENSE_PUBLIC_ORACLE rate=" << rate << " internal_rate=" << 4 * rate << " attack_seconds=.002 release_seconds=.080 sag_bound_db=6 maxabs=" << maximum
                  << " energy=" << energy << " minimum_sag=" << oracle.minimumSag << " wrong_attack=" << sensitivity[0] << " wrong_release=" << sensitivity[1] << " wrong_bound=" << sensitivity[2] << '\n';
    }
    auto s = profile(0, 2); Saturation hot, fresh, linked, mono;
    ready(hot, s, 48000); ready(fresh, s, 48000); ready(linked, s, 48000); ready(mono, s, 48000);
    for (unsigned n = 0; n < 24000; ++n) hot.process({.0625, .03125});
    double history = 0, recovered = 0, linkedDifference = 0;
    for (unsigned n = 0; n < 57600; ++n) {
        const double delta = error(hot.process({.00390625, .001953125}), fresh.process({.00390625, .001953125}));
        if (n < 960) history = std::max(history, delta);
        if (n >= 48000) recovered = std::max(recovered, delta);
        const auto pair = linked.process({.00390625, -.0625}), alone = mono.process({.00390625, .00390625});
        if (n > 24000) linkedDifference = std::max(linkedDifference, std::abs(pair[0] - alone[0]));
    }
    CHECK(history > 1e-6); CHECK(recovered <= 1e-6); CHECK(linkedDifference > 1e-6);
    auto glowSettings = profile(0, 1); Saturation glow, soft, dc;
    ready(glow, glowSettings, 48000); ready(soft, profile(0, 0), 48000); ready(dc, glowSettings, 48000);
    Complex evenGlow {}, evenSoft {}; double dcMean = 0, energy = 0; constexpr unsigned count = 8192, bin = 37;
    for (unsigned n = 0; n < 96000 + count; ++n) {
        const double phase = 2 * std::numbers::pi * bin * n / count, x = .03125 * std::sin(phase);
        const auto g = glow.process({x, x}), f = soft.process({x, x}), d = dc.process({.02, .02});
        if (n >= 96000) { evenGlow += g[0] * std::polar(2. / count, -2 * phase); evenSoft += f[0] * std::polar(2. / count, -2 * phase); dcMean += d[0] / count; energy += g[0] * g[0]; }
    }
    CHECK(std::abs(evenGlow) > 1e-5); CHECK(std::abs(evenGlow) > 100 * std::abs(evenSoft)); CHECK(energy > .01);
    CHECK(std::abs(dcMean - .02) <= 1e-8);
    for (unsigned cut : {0u, 1u, 2u}) {
        Saturation retired; SaturationSettings enabled; enabled.lanes[0].preTiltDb = 6;
        enabled.preLowCutEnabled = cut != 1; enabled.postHighCutEnabled = cut != 0; enabled.preLowCutHz = 600; enabled.postHighCutHz = 1200;
        ready(retired, enabled, 48000); for (unsigned n = 0; n < 4096; ++n) retired.process(signal(n, 48000));
        SaturationSettings disabled; CHECK(retired.setSettings(disabled));
        for (unsigned n = 0; n < 96000; ++n) retired.process({});
        CHECK(retired.setSettings(enabled)); double resurrected = 0;
        for (unsigned n = 0; n < 2 * duration(48000); ++n) resurrected = std::max(resurrected, error(retired.process({})));
        CHECK(resurrected <= 1e-9);
    }
    std::cout << "CHARACTERS dense_history=" << history << " dense_recovered=" << recovered << " stereo_link=" << linkedDifference
              << " glow_even=" << std::abs(evenGlow) << " soft_even=" << std::abs(evenSoft) << " glow_dc=" << dcMean << " stale_cut_and_shelf_retirement=3\n";
}
// Derive one lane from ordinary production returns using linear superposition.
// Subtracting a -6dB trim reference cancels every unselected lane; a neutral
// selected lane supplies its linear dry contribution. No reference auditions.
struct SelectedLaneReference {
    Saturation wet, quieter, dry, dryQuieter;
    double dryWeight = 0;
    SelectedLaneReference(SaturationSettings settings, unsigned lane, double rate) {
        settings.mix = 1;
        dryWeight = settings.lanes[lane].bypassed ? 1 : 1 - settings.lanes[lane].mix;
        ready(wet, settings, rate);
        auto reduced = settings; reduced.lanes[lane].trimDb -= 6;
        ready(quieter, reduced, rate);
        auto neutral = settings; neutral.lanes[lane] = {}; neutral.driveCompensation = false;
        ready(dry, neutral, rate); neutral.lanes[lane].trimDb = -6;
        ready(dryQuieter, neutral, rate);
    }
    Frame process(Frame input) {
        const auto a = wet.process(input), b = quieter.process(input), c = dry.process(input), d = dryQuieter.process(input);
        Frame result {};
        const double divisor = 1 - gain_math::canonicalPower10(-6. / 20);
        for (unsigned ch = 0; ch < 2; ++ch) result[ch] = ((a[ch] - b[ch]) + dryWeight * (c[ch] - d[ch])) / divisor;
        return result;
    }
};
void bandAudition() {
    Saturation unprepared; CHECK(!unprepared.setAudition(SaturationLane::low));
    CHECK(!unprepared.setAudition(SaturationLane::full)); CHECK(!unprepared.setAudition(SaturationLane(255)));
    CHECK(unprepared.setAudition(std::nullopt)); CHECK(!unprepared.auditioning());
    unsigned cases = 0; double maximum = 0, minimumEnergy = std::numeric_limits<double>::max();
    for (double rate : {8000., 48000., 384000., 768000.}) for (unsigned partition : partitions)
        for (unsigned lane : {1u, 2u, 3u}) for (bool bypass : {false, true}) {
        auto settings = profile(1); settings.inputTrimDb = -3; settings.outputTrimDb = 4;
        settings.preLowCutEnabled = settings.postHighCutEnabled = true; settings.preLowCutHz = 45; settings.postHighCutHz = 2700;
        for (unsigned band = 1; band <= 3; ++band) {
            auto& l = settings.lanes[band]; l.character = SaturationCharacter(band % 3);
            l.driveDb = float(6 + 3 * band); l.preTiltDb = float(int(band) * 2 - 4);
            l.mix = .25f * float(band); l.trimDb = float(2 * band); l.bypassed = band == lane && bypass;
        }
        SelectedLaneReference reference(settings, lane, rate);
        std::array<Saturation, 3> subjects, controls;
        for (unsigned mix = 0; mix < 3; ++mix) { settings.mix = .5f * mix;
            ready(subjects[mix], settings, rate); ready(controls[mix], settings, rate);
            CHECK(subjects[mix].setAudition(SaturationLane(lane))); }
        std::array<std::array<Frame, 512>, 3> audio;
        double delta = 0, energy = 0; bool valid = true;
        const auto length = duration(rate), count = length + 1024;
        for (unsigned at = 0; at < count; at += partition) {
            const auto size = std::min(partition, count - at);
            for (unsigned n = 0; n < size; ++n) for (auto& buffer : audio) buffer[n] = signal(at + n, rate);
            for (unsigned mix = 0; mix < 3; ++mix) subjects[mix].processBlock(std::span(audio[mix]).first(size));
            for (unsigned n = 0; n < size; ++n) {
                const auto input = signal(at + n, rate), expected = reference.process(input);
                for (auto& control : controls) control.process(input);
                if (at + n >= length) { energy += expected[0] * expected[0] + expected[1] * expected[1];
                    for (const auto& buffer : audio) { delta = std::max(delta, error(buffer[n], expected)); valid = finite(buffer[n]) && valid; } }
            }
        }
        CHECK(valid); CHECK(energy > 1e-12); CHECK(delta <= tolerance);
        maximum = std::max(maximum, delta); minimumEnergy = std::min(minimumEnergy, energy);
        for (auto& subject : subjects) subject.cancelAudition();
        bool restored = true;
        for (unsigned n = 0; n < length + 17; ++n) for (unsigned mix = 0; mix < 3; ++mix) {
            const auto input = signal(count + n, rate), a = subjects[mix].process(input), b = controls[mix].process(input);
            if (n + 1 >= length) restored = exact(a, b) && restored;
        }
        CHECK(restored); for (const auto& subject : subjects) CHECK(!subject.auditioning());
        ++cases;
        if (failures) { std::cout << "AUDITION_RED GlobalMix0 normal dry/unselected bands leaked maxabs=" << delta << " selected_reference_energy=" << energy << '\n'; return; }
    }
    CHECK(cases == 120);
    std::cout << "AUDITION_DERIVED cases=" << cases << " global_mixes=0,.5,1 maxabs=" << maximum << " min_selected_energy=" << minimumEnergy << " restoration_ms=20\n";
    for (double rate : {8000., 48000., 384000., 768000.}) {
        auto settings = profile(1); Saturation actual, normal; ready(actual, settings, rate); ready(normal, settings, rate);
        std::array<std::unique_ptr<SelectedLaneReference>,3> references;
        for(unsigned lane=1;lane<=3;++lane)references[lane-1]=std::make_unique<SelectedLaneReference>(settings,lane,rate);
        unsigned frame = 0; bool valid = true;
        for (auto lane : {SaturationLane::low, SaturationLane::high, SaturationLane::mid}) {
            CHECK(actual.setAudition(lane));
            double switchedError=0,switchedEnergy=0;
            for (unsigned n = 0; n < duration(rate) + 17; ++n, ++frame) { const auto x = signal(frame, rate),audio=actual.process(x);valid = finite(audio) && valid;normal.process(x);
                for(unsigned band=0;band<3;++band){const auto expected=references[band]->process(x);if(band+1==unsigned(lane)&&n+1>=duration(rate)){switchedError=std::max(switchedError,error(audio,expected));switchedEnergy+=expected[0]*expected[0]+expected[1]*expected[1];}}}
            CHECK(switchedEnergy>1e-12);CHECK(switchedError<=tolerance);
        }
        actual.cancelAudition();
        for (unsigned n = 0; n < duration(rate) / 2; ++n, ++frame) { const auto x = signal(frame, rate); actual.process(x); normal.process(x); }
        CHECK(actual.setAudition(SaturationLane::low));
        for (unsigned n = 0; n < duration(rate); ++n, ++frame) { const auto x = signal(frame, rate); actual.process(x); normal.process(x); }
        actual.cancelAudition();
        for (unsigned n = 0; n < duration(rate); ++n, ++frame) { const auto x = signal(frame, rate); const auto a = actual.process(x), b = normal.process(x); if (n + 1 == duration(rate)) CHECK(exact(a, b)); }
        CHECK(valid); CHECK(!actual.auditioning()); lockstep(actual, normal, 17, rate, frame);
        CHECK(actual.setAudition(SaturationLane::mid)); settings.mode = SaturationMode::fullBand;
        CHECK(actual.setSettings(settings)); CHECK(normal.setSettings(settings)); CHECK(!actual.setAudition(SaturationLane::high));
        for (unsigned n = 0; n < duration(rate) + 17; ++n, ++frame) { const auto x = signal(frame, rate); const auto a = actual.process(x), b = normal.process(x); if (n + 1 >= duration(rate)) CHECK(exact(a, b)); }
        CHECK(!actual.auditioning());
        settings.mode = SaturationMode::threeBand; CHECK(actual.setSettings(settings)); CHECK(actual.setAudition(SaturationLane::low));
        CHECK(!actual.prepare(0)); CHECK(actual.auditioning()); actual.reset(); CHECK(!actual.auditioning());
        CHECK(actual.setAudition(SaturationLane::mid)); CHECK(exact(actual.process({NAN, 0}), {})); CHECK(!actual.auditioning());
        CHECK(actual.setAudition(SaturationLane::high)); CHECK(actual.prepare(rate)); CHECK(!actual.auditioning());
    }
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool counting = false, failNext = false, allocationRefused = false;
thread_local std::size_t allocations = 0, frees = 0, allocationBytes = 0;
int __cdecl heapHook(int operation, void*, std::size_t bytes, int type, long, const unsigned char*, int) {
    if (type != _CRT_BLOCK && (operation == _HOOK_ALLOC || operation == _HOOK_REALLOC)) {
        if (counting) { ++allocations; allocationBytes += bytes; }
        if (failNext) { failNext = false; allocationRefused = true; return 0; }
    }
    if (counting && type != _CRT_BLOCK && operation == _HOOK_FREE) ++frees;
    return 1;
}
#endif
void heap() {
    const auto shared=airack::saturation_detail::saturationCoefficientManifest();std::size_t sharedBytes=0;
    for(const auto& resource:shared){sharedBytes+=resource.values.size_bytes();std::cout<<"SHARED_RESOURCE name="<<resource.name<<" payload_bytes="<<resource.values.size_bytes()<<'\n';}
    CHECK(shared.size()==20);CHECK(sharedBytes>0);std::cout<<"SHARED_RESOURCES_ONCE arrays="<<shared.size()<<" payload_bytes="<<sharedBytes<<" ownership=immutable_process_shared_not_per_processor\n";
#if defined(_MSC_VER) && defined(_DEBUG)
    Saturation common; CHECK(common.prepare(48000)); // Canonical tables initialize outside the counted region.
    for (unsigned factor : {4u}) {
        Saturation actual, control; ready(actual, profile(), 768000); ready(control, profile(), 768000);
        charge(actual, control, 768000);auto monitoring=actual.settings();monitoring.mode=SaturationMode::threeBand;
        CHECK(actual.setSettings(monitoring)&&control.setSettings(monitoring));CHECK(actual.setAudition(SaturationLane::low)&&control.setAudition(SaturationLane::low));
        lockstep(actual,control,17,768000);const auto target = actual.settings();
        allocationRefused = false; const auto old = _CrtSetAllocHook(heapHook); failNext = true;
        const bool prepared = actual.prepare(768000);
        failNext = false; _CrtSetAllocHook(old);
        CHECK(allocationRefused); CHECK(!prepared); CHECK(actual.settings() == target);CHECK(actual.auditioning());lockstep(actual, control, 257, 768000);
        Saturation fresh; allocationRefused = false; _CrtSetAllocHook(heapHook); failNext = true;
        const bool first = fresh.prepare(768000); failNext = false; _CrtSetAllocHook(old);
        CHECK(allocationRefused); CHECK(!first); CHECK(exact(fresh.process({1, -1}), {})); CHECK(fresh.prepare(768000));
        Saturation storage; allocations = frees = allocationBytes = 0; _CrtSetAllocHook(heapHook); counting = true;
        const bool ok = storage.prepare(768000); counting = false; _CrtSetAllocHook(old);
        CHECK(ok); CHECK(allocations == 1); CHECK(frees == 1); CHECK(allocationBytes < 200000);
        std::cout << "STORAGE factor=" << factor << " prepared_bytes=" << allocationBytes << " allocations=" << allocations << " replaced_states=" << frees << " endpoint_bound=2\n";
        std::array<Frame, 17> frames {}; auto bad = profile(); bad.lanes[3].mix = std::numeric_limits<float>::quiet_NaN();
        bool admitted = true, readback = true, finiteOutput = true;
        allocations = frees = allocationBytes = 0; _CrtSetAllocHook(heapHook); counting = true;
        for (unsigned n = 0; n < 1900; ++n) {
            const auto s = rapid(n); admitted = storage.setSettings(s) && admitted; readback = (storage.settings() == s) && readback;
            admitted = !storage.setSettings(bad) && admitted;
            admitted = storage.setAudition(s.mode==SaturationMode::threeBand?std::optional<SaturationLane>(SaturationLane(1+n%3)):std::nullopt) && admitted;
            if(n%7==0)storage.cancelAudition();
            for (unsigned k = 0; k < frames.size(); ++k) frames[k] = signal(17 * n + k, 768000);
            storage.processBlock(frames); storage.processBlock({});
            for (auto frame : frames) finiteOutput = finite(frame) && finiteOutput;
            if (n == 701) storage.reset();
        }
        counting = false; _CrtSetAllocHook(old);
        CHECK(admitted); CHECK(readback); CHECK(finiteOutput); CHECK(allocations == 0); CHECK(frees == 0);
        std::cout << "CALLBACK factor=" << factor << " allocations=" << allocations << " frees=" << frees << " actual_OOM_atomic=1 first_OOM_silent=1\n";
    }
#else
    using namespace allocationWitness;
    allocations = frees = bytes = 0; enabled = true;
    void* (*volatile allocate)(std::size_t) = ::operator new;
    void* sanity = allocate(1); ::operator delete(sanity); enabled = false;
    CHECK(allocations == 1 && frees == 1);
    Saturation first; allocations = frees = bytes = 0; enabled = true;
    const bool prepared = first.prepare(768000); enabled = false;
    const auto preparedBytes = bytes;
    CHECK(prepared); CHECK(allocations == 1 && frees == 1 && preparedBytes < 200000);
    std::array<Frame, 17> frames {}; auto bad = profile(); bad.lanes[3].mix = std::numeric_limits<float>::quiet_NaN();
    bool admitted = true, readback = true, valid = true;
    allocations = frees = bytes = 0; enabled = true;
    // Includes first admitted settings, first callback, first transition and reset.
    for (unsigned n = 0; n < 1900; ++n) {
        const auto s = rapid(n); admitted = first.setSettings(s) && admitted;
        readback = first.settings() == s && readback; admitted = !first.setSettings(bad) && admitted;
        admitted = first.setSettings(first.settings()) && admitted;
        admitted = first.setAudition(s.mode==SaturationMode::threeBand?std::optional<SaturationLane>(SaturationLane(1+n%3)):std::nullopt) && admitted;
        if(n%7==0)first.cancelAudition();
        for (unsigned k = 0; k < frames.size(); ++k) frames[k] = signal(17 * n + k, 768000);
        first.processBlock(frames); first.processBlock({});
        for (const auto frame : frames) valid = finite(frame) && valid;
        if (n == 701) first.reset();
    }
    enabled = false;
    CHECK(admitted); CHECK(readback); CHECK(valid); CHECK(allocations == 0 && frees == 0 && bytes == 0);
    allocations = frees = bytes = 0; enabled = true;
    const bool reprepared = first.prepare(768000); enabled = false;
    CHECK(reprepared); CHECK(allocations == 1 && frees == 1 && bytes == preparedBytes);
    std::cout << "RELEASE_FIRST_USE prepared_bytes=" << preparedBytes << " callback_allocations=0 callback_frees=0 repeated_state_bytes=" << bytes << " endpoint_bound=2\n";
#endif
}
std::string digest(const std::string& bytes) { return juce::SHA256(bytes.data(), bytes.size()).toHexString().toStdString(); }
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
std::string readFile(const fs::path& path, std::size_t limit) {
    require(fs::is_regular_file(fs::symlink_status(path)), "missing/nonregular file: " + path.string());
    const auto size = fs::file_size(path); require(size <= limit, "oversized file: " + path.string());
    std::string bytes(std::size_t(size), '\0'); std::ifstream stream(path, std::ios::binary);
    stream.read(bytes.data(), std::streamsize(bytes.size())); require(stream.good() && stream.peek() == EOF, "short/changed file: " + path.string());
    return bytes;
}
void writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary); stream.write(bytes.data(), std::streamsize(bytes.size())); require(stream.good(), "write failed: " + path.string());
}
bool validPayload(const std::string& bytes, const juce::var& record) {
    return record.isObject() && std::int64_t(bytes.size()) == std::int64_t(record["bytes"]) && digest(bytes) == record["sha256"].toString().toStdString();
}
std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> row; std::size_t begin = 0;
    for (;;) { const auto end = line.find(',', begin); row.push_back(line.substr(begin, end == std::string::npos ? end : end - begin)); if (end == std::string::npos) return row; begin = end + 1; }
}
double number(const std::string& value) {
    double result = 0; const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(result), "invalid saved number"); return result;
}
SaturationSettings savedSettings(const std::vector<std::string>& row) {
    require(row.size() == 39, "saved settings width"); std::size_t n = 4; SaturationSettings s;
    s.mode = SaturationMode(unsigned(number(row[n++]))); s.inputTrimDb = float(number(row[n++])); s.outputTrimDb = float(number(row[n++])); s.mix = float(number(row[n++]));
    s.driveCompensation = number(row[n++]) != 0; s.lowCrossoverHz = float(number(row[n++])); s.highCrossoverHz = float(number(row[n++]));
    s.preLowCutEnabled = number(row[n++]) != 0; s.preLowCutHz = float(number(row[n++])); s.postHighCutEnabled = number(row[n++]) != 0; s.postHighCutHz = float(number(row[n++]));
    for (auto& l : s.lanes) { l.bypassed = number(row[n++]) != 0; l.character = SaturationCharacter(unsigned(number(row[n++])));
        l.driveDb = float(number(row[n++])); l.preTiltDb = float(number(row[n++])); l.mix = float(number(row[n++])); l.trimDb = float(number(row[n++])); }
    require(validateSaturation(s), "saved settings domain"); return s;
}
void fixtures() {
    const fs::path root(RACKSYNTH_LIFECYCLE_FIXTURE_DIR), output = fs::path(RACKSYNTH_LIFECYCLE_RESULT_DIR) / "fixtures";
    const auto manifestBytes = readFile(root / "manifest.json", 1024 * 1024);
    require(digest(manifestBytes) == "30fc96fc8b4e0f1e9708786e9cf6ae80af42212adb42f7105f4620c8e51cd588", "external manifest pin");
    const auto manifest = juce::JSON::parse(juce::String::fromUTF8(manifestBytes.data(), int(manifestBytes.size())));
    const auto files = manifest["files"]; require(files.isObject(), "manifest file map");
    std::set<std::string> inventory {"manifest.json"};
    for (const auto& property : files.getDynamicObject()->getProperties()) {
        const auto name = property.name.toString().toStdString(); require(fs::path(name).filename() == name, "manifest path");
        require(validPayload(readFile(root / name, 1024 * 1024), property.value), "manifest payload: " + name); inventory.insert(name);
    }
    std::set<std::string> actualInventory; for (const auto& entry : fs::directory_iterator(root)) actualInventory.insert(entry.path().filename().string());
    require(inventory == actualInventory, "complete frozen directory inventory");
    const auto inputBytes = readFile(root / "input.f64", 32768), settingsBytes = readFile(root / "settled-settings.csv", 32768);
    require(inputBytes.size() == 32768 && digest(inputBytes) == "73aee579e8868a65cf6ce2792477680d544b3833e51731c8fada0115892ae6fb", "saved input pin");
    require(digest(settingsBytes) == "6ce200bcaf26930be84061e9ed63a91a6c209081a3940226828ee4aaec116254", "saved settled settings pin");
    fs::create_directories(output);
    auto changed = readFile(root / "settled-44100-0-0.f64", 32768); changed[0] ^= 1;
    const auto changedPath = output / "changed-expected-negative.f64"; writeFile(changedPath, changed);
    require(!validPayload(readFile(changedPath, 32768), files["settled-44100-0-0.f64"]), "changed private expected must be rejected before processing");
    std::cout << "FIXTURE_PREFLIGHT external_pin=1 inventory=" << inventory.size() << " all_hashes_sizes=1 changed_private_rejected=1 processing_started=0\n";
    std::vector<Frame> input(2048); std::memcpy(input.data(), inputBytes.data(), inputBytes.size());
    std::istringstream csv(settingsBytes); std::string line; require(bool(std::getline(csv, line)), "saved header");
    unsigned cases = 0, comparisons = 0; std::size_t totalBits = 0; double maximum = 0;
    while (std::getline(csv, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); const auto row = split(line); require(row.size() == 39 && cases < 18, "saved case count/width");
        const unsigned rate = cases < 6 ? 44100 : cases < 12 ? 48000 : 96000, mode = (cases / 3) % 2, character = cases % 3;
        const auto name = "settled-" + std::to_string(rate) + '-' + std::to_string(mode) + '-' + std::to_string(character) + ".f64";
        require(number(row[0]) == cases && row[1] == name && number(row[2]) == rate && number(row[3]) == 4, "settled saved identity");
        auto expectedSettings = profile(mode, character); expectedSettings.mix = .75f;
        for (auto& l : expectedSettings.lanes) { l.driveDb = 12; l.mix = .75f; }
        const auto settings = savedSettings(row); require(settings == expectedSettings, "settled settings exact common contract");
        const auto expectedBytes = readFile(root / name, 32768); require(expectedBytes.size() == 32768, "expected audio size");
        std::vector<Frame> expected(2048); std::memcpy(expected.data(), expectedBytes.data(), expectedBytes.size());
        for (unsigned partition : partitions) {
            Saturation c; ready(c, settings, rate); c.reset(); auto audio = input;
            for (std::size_t at = 0; at < audio.size(); at += partition) c.processBlock(std::span<Frame>(audio).subspan(at, std::min<std::size_t>(partition, audio.size() - at)));
            double maxabs = 0; std::size_t bits = 0; bool valid = true;
            for (std::size_t n = 0; n < audio.size(); ++n) { valid = finite(audio[n]) && valid; maxabs = std::max(maxabs, error(audio[n], expected[n]));
                for (unsigned ch = 0; ch < 2; ++ch) bits += std::bit_cast<std::uint64_t>(audio[n][ch]) != std::bit_cast<std::uint64_t>(expected[n][ch]); }
            CHECK(valid); CHECK(maxabs <= tolerance); CHECK(bits == 0); maximum = std::max(maximum, maxabs); totalBits += bits; ++comparisons;
            writeFile(output / ("p" + std::to_string(partition) + '-' + name), std::string(reinterpret_cast<const char*>(audio.data()), audio.size() * sizeof(Frame)));
            std::cout << "FIXTURE case=" << name << " factor=4 partition=" << partition << " frames=2048 maxabs=" << maxabs << " differing_words=" << bits << '\n';
        }
        ++cases;
    }
    CHECK(cases == 18); CHECK(comparisons == 90); CHECK(maximum <= tolerance); CHECK(totalBits == 0);
    std::cout << "FIXTURE_TOTAL cases=" << cases << " comparisons=" << comparisons << " maxabs=" << maximum << " differing_words=" << totalBits << '\n';
}
void tail() {
#if defined(_DEBUG)
    CHECK(std::isfinite(saturationTailSeconds(distinct(), 768000))); CHECK(saturationTailSeconds(distinct(), 768000) >= 2.); return;
#else
    unsigned cases = 0;
    for (double rate : {8000., 384000., 768000.}) for (unsigned factor : {4u}) for (unsigned state : {0u, 1u}) {
        auto s = profile(state == 0 ? 1 : 0, 1); s.inputTrimDb = 24; s.outputTrimDb = 12;
        s.lowCrossoverHz = 40; s.highCrossoverHz = 16000; s.preLowCutEnabled = s.postHighCutEnabled = true; s.preLowCutHz = 20; s.postHighCutHz = 1000;
        for (unsigned n = 0; n < 4; ++n) { auto& l = s.lanes[n]; l.driveDb = 36; l.trimDb = 18; l.preTiltDb = n % 2 ? -6.f : 6.f; if (state == 0) l.character = SaturationCharacter(n % 3); }
        const double seconds = saturationTailSeconds(s, rate); CHECK(seconds == 2);
        const unsigned count = unsigned(seconds * rate), window = unsigned(std::ceil(.020 * rate)), excitation = unsigned(.05 * rate);
        std::cout << "TAIL_BEGIN rate=" << rate << " factor=" << factor << " state=" << state << " declared_seconds=" << seconds
                  << " silence_frames=" << count << " window_frames=" << window << " excitation_frames=" << excitation << std::endl;
        Saturation c; ready(c, s, rate); bool valid = true; double driven = 0, finalPeak = 0;
        for (unsigned n = 0; n < excitation; ++n) { auto x = signal(n, rate); if (state == 1) { x[0] += .02; x[1] -= .01; } const auto y = c.process(x); valid = finite(y) && valid; driven = std::max(driven, error(y)); }
        for (unsigned n = 0; n < count; ++n) { const auto y = c.process({}); valid = finite(y) && valid; if (n >= count - window) finalPeak = std::max(finalPeak, error(y)); }
        c.reset(); bool resetSilent = true; for (unsigned n = 0; n < window; ++n) resetSilent = exact(c.process({}), {}) && resetSilent;
        CHECK(valid); CHECK(driven > 1e-8); CHECK(finalPeak <= 1e-9); CHECK(resetSilent); CHECK(c.settings() == s); ++cases;
        std::cout << "TAIL rate=" << rate << " factor=" << factor << " state=" << state << " finite=" << valid << " driven_peak=" << driven
                  << " final20ms_peak=" << finalPeak << " threshold=1e-9 reset_exact_silence=" << resetSilent << std::endl;
    }
    CHECK(cases == 6); std::cout << "TAIL_TOTAL cases=" << cases << " helper_seconds=2 final_window_ms=20\n";
#endif
}
void provenance() {
#if defined(_WIN32)
    HMODULE provider = nullptr; const auto procedure = reinterpret_cast<LPCWSTR>(&airack_saturation_release_pow);
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, procedure, &provider) != 0, "canonical gain provider");
    std::array<wchar_t, 32768> path {}; const auto size = GetModuleFileNameW(provider, path.data(), DWORD(path.size()));
    require(size > 0 && size < path.size(), "canonical gain provider path"); const fs::path providerPath(std::wstring(path.data(), size));
    const auto bytes = readFile(providerPath, 32 * 1024 * 1024);
    std::cout << "GAIN_PROVIDER path=" << providerPath.string() << " bytes=" << bytes.size() << " sha256=" << digest(bytes) << '\n';
#endif
    std::cout << "STATUS CPU=OPEN/USER_DEFERRED_FOR_MVP factor=4 production=CORE_ONLY\n";
}
}
int main(int argc, char** argv) {
    if (argc != 2 || (std::string_view(argv[1]) != "--admission" && std::string_view(argv[1]) != "--functional"
            && std::string_view(argv[1]) != "--audition" && std::string_view(argv[1]) != "--fixtures" && std::string_view(argv[1]) != "--tail")) {
        std::cerr << "Expected exactly one supported mode\n"; return 2;
    }
    std::cout << std::setprecision(17);
    try {
        provenance();
        const std::string_view mode(argv[1]);
        if (mode == "--admission") group("admission_and_atomic_refusal", admission);
        if (mode == "--functional") {
            group("exact_event_boundary_coverage", scheduleCoverage);
            if (failures) { std::cout << "TOTAL checks=" << checks << " failures=" << failures << " incomplete_lifecycle_matrix=1\n"; return 1; }
            group("retained_targets_hidden_refusal", retainedTargets); group("ordered_blocks_and_events", blockOrder);
            group("round_structural_and_latest_retarget", transitionLaws); group("high_rate_integrity_and_recovery", integrity);
            group("rapid_simultaneous_overlap", rapidOverlap);
            group("retained_raw_controls", retainedRawControls); group("neutral_matched_dry_routes", neutralAndDryRoutes);
            group("linear_complex_response", linearComplexLaws); group("frozen_compensation_and_warm_bypass", compensationAndWarmBypass);
            group("character_memory_and_history_retirement", characterMemoryAndRetirement);
            group("storage_first_use_and_allocation_failure", heap);
        }
        if (mode == "--audition") group("transient_selected_band_returns", bandAudition);
        if (mode == "--fixtures") group("frozen_v2_common", fixtures);
        if (mode == "--tail") group("Release_fixed_two_second_tail", tail);
    } catch (const std::exception& error) { ++failures; std::cout << "FAIL exception: " << error.what() << '\n'; }
    if (checks == 0) { ++failures; std::cout << "FAIL no selected assertion group ran\n"; }
    std::cout << "TOTAL checks=" << checks << " failures=" << failures << std::endl;
    return failures ? 1 : 0;
}

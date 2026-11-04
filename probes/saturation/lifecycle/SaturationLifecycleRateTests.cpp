#include <SaturationCandidate.h>
#include <SaturationGainRuntime.h>
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
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
#endif

namespace {
using namespace airack;
using airack::probe::Candidate;
using Frame = Candidate::Frame;
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
    section = name; const int before = failures, count = checks; test();
    std::cout << "GROUP " << name << " checks=" << checks - count << " failures=" << failures - before << std::endl;
}
bool finite(Frame x) { return std::isfinite(x[0]) && std::isfinite(x[1]); }
bool exact(Frame a, Frame b) { return std::memcmp(a.data(), b.data(), sizeof(Frame)) == 0; }
double error(Frame a, Frame b = {}) { return std::max(std::abs(a[0] - b[0]), std::abs(a[1] - b[1])); }
unsigned duration(double rate) { return unsigned(std::ceil(.020 * rate)); }
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
void ready(Candidate& c, const SaturationSettings& s, double rate, unsigned factor) {
    CHECK(c.setSettings(s)); CHECK(c.prepare(rate, factor)); CHECK(c.settings() == s);
}
void lockstep(Candidate& a, Candidate& b, unsigned count, double rate, unsigned begin = 0) {
    bool same = true;
    for (unsigned n = 0; n < count; ++n) { const auto x = signal(n + begin, rate); same = exact(a.process(x), b.process(x)) && same; }
    CHECK(same);
}
void charge(Candidate& a, Candidate& b, double rate) {
    lockstep(a, b, 79, rate);
    auto next = a.settings(); next.mode = SaturationMode::threeBand; next.lanes[0].character = SaturationCharacter::glow;
    CHECK(a.setSettings(next)); CHECK(b.setSettings(next)); lockstep(a, b, 17, rate, 79);
    next.mode = SaturationMode::fullBand; next.lanes[0].character = SaturationCharacter::dense;
    CHECK(a.setSettings(next)); CHECK(b.setSettings(next));
}
void admission() {
    for (unsigned factor : {4u, 8u}) {
        Candidate candidate;
        const bool ordinary = candidate.prepare(384000, factor);
        const bool extended = candidate.prepare(768000, factor);
        std::cout << "ADMISSION rate=384000 factor=" << factor << " actual=" << ordinary << " required=1\n"
                  << "ADMISSION rate=768000 factor=" << factor << " actual=" << extended << " required=1\n";
        CHECK(ordinary); CHECK(extended);
        for (double rate : {8000., 44100., 48000., 96000., 384000., 384001., 768000.}) {
            CHECK(candidate.prepare(rate, factor));
            std::cout << "ADMISSION rate=" << rate << " factor=" << factor << " required=1\n";
        }
        Candidate actual, control; ready(actual, distinct(), 384001, factor); ready(control, distinct(), 384001, factor);
        charge(actual, control, 384001); const auto target = actual.settings();
        for (double rate : {std::nextafter(8000., 0.), std::nextafter(768000., std::numeric_limits<double>::infinity()),
                            0., -1., std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
            CHECK(!actual.prepare(rate, factor)); CHECK(actual.settings() == target); lockstep(actual, control, 31, 384001);
        }
        for (unsigned invalid : {0u, 3u, 16u}) {
            CHECK(!actual.prepare(768000, invalid)); CHECK(actual.settings() == target); lockstep(actual, control, 31, 384001);
        }
        Candidate fresh; CHECK(exact(fresh.process({1, -1}), {})); CHECK(!fresh.prepare(7999, factor));
        CHECK(exact(fresh.process({1, -1}), {})); CHECK(fresh.prepare(8000, factor));
    }
    for (unsigned factor : {1u, 2u, 4u, 8u, 64u}) { Candidate c; CHECK(c.prepare(768000, factor)); }
}
void retainedTargets() {
    const auto bank = distinct();
    for (unsigned factor : {4u, 8u}) {
        Candidate before, after; CHECK(before.settings() == SaturationSettings{});
        ready(before, bank, 384001, factor); CHECK(after.prepare(384001, factor)); CHECK(after.setSettings(bank));
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
        Candidate resetControl; ready(resetControl, target, 384001, factor); lockstep(before, resetControl, 97, 384001);
        CHECK(before.prepare(768000, factor)); CHECK(before.settings() == target);
        Candidate preparedControl; ready(preparedControl, target, 768000, factor); lockstep(before, preparedControl, 97, 768000);
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
    const unsigned boundary = events[0].frame + 883, second = events[5].frame + 883;
    CHECK(events[5].operation == 0 && events[5].frame == boundary + 1);
    CHECK(events[6].operation == 2);
    for (unsigned n = 0; n < 3; ++n) {
        CHECK(events[3 + n].frame == boundary - 1 + n);
        CHECK(events[6 + n].frame == second - 1 + n);
    }
    std::cout << "EVENT_EDGES rate=44101 first_structural_frame=" << events[0].frame << " independently_checked_ceil_frames=883"
              << " first_actual=" << events[3].frame << ',' << events[4].frame << ',' << events[5].frame
              << " first_required=" << boundary - 1 << ',' << boundary << ',' << boundary + 1
              << " second_structural_frame=" << events[5].frame
              << " second_actual=" << events[6].frame << ',' << events[7].frame << ',' << events[8].frame
              << " second_required=" << second - 1 << ',' << second << ',' << second + 1
              << " second_lastframe_reset=planned_interruption_not_observed_completion\n";
}
void apply(Candidate& c, const Event& e) {
    if (e.operation == 2) c.reset(); else if (e.operation == 1) CHECK(!c.setSettings(e.settings)); else CHECK(c.setSettings(e.settings));
}
std::vector<Frame> renderSchedule(unsigned partition, unsigned factor, bool block, bool wrongOrder = false, bool repeat = true) {
    constexpr double rate = 44101; const unsigned length = duration(rate), count = 3 * length + 73;
    Candidate c; ready(c, profile(), rate, factor); auto events = schedule(length);
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
    for (unsigned factor : {4u, 8u}) {
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
    constexpr double rate = 44101; const auto length = duration(rate);
    CHECK(length == 883); CHECK(std::round(.020 * rate) == 882);
    for (unsigned factor : {4u, 8u}) for (unsigned edit = 0; edit < 8; ++edit) for (bool reverse : {false, true}) {
        auto a = profile(), b = a;
        if (edit < 3) { for (auto& l : a.lanes) l.character = SaturationCharacter(edit); b = a; for (auto& l : b.lanes) l.character = SaturationCharacter((edit + 1) % 3); }
        if (edit == 3) b.mode = SaturationMode::threeBand;
        if (edit == 4) { a.preLowCutHz = b.preLowCutHz = 600; b.preLowCutEnabled = true; }
        if (edit == 5) { a.postHighCutHz = b.postHighCutHz = 1200; b.postHighCutEnabled = true; }
        if (edit == 6) { b.mode = SaturationMode::threeBand; b.preLowCutEnabled = b.postHighCutEnabled = true; for (auto& l : b.lanes) l.character = SaturationCharacter::dense; }
        if (edit == 7) { b.mode = SaturationMode::threeBand; for (auto& l : b.lanes) l.character = SaturationCharacter::glow; }
        if (reverse) std::swap(a, b);
        Candidate actual, from, to; ready(actual, a, rate, factor); ready(from, a, rate, factor); ready(to, b, rate, factor);
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
            if (n == 0 || n == length / 2 || n == length - 1 || n == length) CHECK(error(y, expected) <= tolerance);
        }
        CHECK(maximum <= tolerance); CHECK(separation > 1e-7);
        std::cout << "STRUCTURAL rate=" << rate << " factor=" << factor << " edit=" << edit << " reverse=" << reverse
                  << " ceil_frames=" << length << " first_weight=0 final_weight=1 maxabs=" << maximum << '\n';
    }
    // Latest structural requests cannot affect the active fade; at its boundary the newest request starts.
    for (double fractional : {44101., 384001.}) for (unsigned factor : {4u, 8u}) for (unsigned step : {1u, 17u, 64u}) {
        const auto fade = duration(fractional); auto a = profile(), b = profile(0, 1), c = profile(0, 2);
        Candidate queued, explicitChange; ready(queued, a, fractional, factor); ready(explicitChange, a, fractional, factor);
        CHECK(queued.setSettings(b)); CHECK(explicitChange.setSettings(b)); double maximum = 0;
        for (unsigned n = 0; n < 2 * fade + 33; ++n) {
            if (n < fade && n % step == 0) { CHECK(queued.setSettings(n % 2 ? a : b)); CHECK(queued.setSettings(c)); CHECK(queued.settings() == c); }
            if (n == fade) CHECK(explicitChange.setSettings(c));
            const auto x = signal(n, fractional); maximum = std::max(maximum, error(queued.process(x), explicitChange.process(x)));
        }
        CHECK(maximum == 0); CHECK(queued.settings() == c);
        std::cout << "RETARGET rate=" << fractional << " factor=" << factor << " every=" << step << " ceil_frames=" << fade << " maxabs=" << maximum << '\n';
    }
}
void integrity() {
    unsigned cases = 0;
    for (double rate : {8000., 384000., 768000.}) for (unsigned factor : {4u, 8u})
        for (unsigned mode : {0u, 1u}) for (unsigned character : {0u, 1u, 2u}) {
        auto s = profile(mode, character); s.preLowCutEnabled = s.postHighCutEnabled = true;
        s.lowCrossoverHz = 40; s.highCrossoverHz = 16000; s.preLowCutHz = 20; s.postHighCutHz = 1000;
        s.inputTrimDb = 24; s.outputTrimDb = -24;
        for (unsigned n = 0; n < 4; ++n) { auto& l = s.lanes[n]; l.driveDb = 36; l.preTiltDb = n % 2 ? -6 : 6; l.trimDb = n % 2 ? -18 : 18; }
        Candidate actual, swapped; ready(actual, s, rate, factor); ready(swapped, s, rate, factor);
        auto neutral = profile(mode, character); for (auto& lane : neutral.lanes) lane.driveDb = 0;
        Candidate neutralControl; ready(neutralControl, neutral, rate, factor); bool neutralFinite = true; double neutralPeak = 0;
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
            Candidate resetControl; ready(resetControl, s, rate, factor); lockstep(actual, resetControl, 64, rate);
        }
        actual.reset(); CHECK(actual.settings() == s); silent = true;
        for (unsigned n = 0; n < 64; ++n) silent = exact(actual.process({}), {}) && silent;
        CHECK(silent); CHECK(actual.prepare(rate, factor)); Candidate reprepare; ready(reprepare, s, rate, factor); lockstep(actual, reprepare, 64, rate);
        std::cout << "INTEGRITY rate=" << rate << " factor=" << factor << " mode=" << mode << " character=" << character
                  << " peak=" << peak << " neutral_peak=" << neutralPeak << " swap_maxabs=" << null << " recovery_inputs=4\n"; ++cases;
    }
    CHECK(cases == 36);
}
SaturationSettings rapid(unsigned n) {
    auto s = profile(n % 2, n % 3); s.inputTrimDb = n % 2 ? -24.f : 24.f; s.outputTrimDb = n % 2 ? 12.f : -24.f;
    s.preLowCutEnabled = n % 2 != 0; s.postHighCutEnabled = n % 3 != 0; s.driveCompensation = n % 5 != 0;
    s.lowCrossoverHz = 40; s.highCrossoverHz = 16000; s.preLowCutHz = 20; s.postHighCutHz = 1000;
    for (auto& l : s.lanes) { l.bypassed = n % 7 == 0; l.driveDb = n % 2 ? 0.f : 36.f; l.preTiltDb = n % 2 ? -6.f : 6.f; l.trimDb = n % 2 ? -18.f : 18.f; l.mix = n % 2 ? 0.f : 1.f; }
    return s;
}
void rapidOverlap() {
    for (unsigned factor : {4u, 8u}) for (unsigned step : {1u, 17u, 64u}) {
        constexpr double rate = 768000; Candidate scalar, block, swapped;
        const auto initial = rapid(0); ready(scalar, initial, rate, factor); ready(block, initial, rate, factor); ready(swapped, initial, rate, factor);
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
#if defined(_MSC_VER) && defined(_DEBUG)
    Candidate common; CHECK(common.prepare(48000, 4)); // Canonical tables initialize outside the counted region.
    for (unsigned factor : {4u, 8u}) {
        Candidate actual, control; ready(actual, profile(), 768000, factor); ready(control, profile(), 768000, factor);
        charge(actual, control, 768000); const auto target = actual.settings();
        allocationRefused = false; const auto old = _CrtSetAllocHook(heapHook); failNext = true;
        const bool prepared = actual.prepare(768000, factor);
        failNext = false; _CrtSetAllocHook(old);
        CHECK(allocationRefused); CHECK(!prepared); CHECK(actual.settings() == target); lockstep(actual, control, 257, 768000);
        Candidate fresh; allocationRefused = false; _CrtSetAllocHook(heapHook); failNext = true;
        const bool first = fresh.prepare(768000, factor); failNext = false; _CrtSetAllocHook(old);
        CHECK(allocationRefused); CHECK(!first); CHECK(exact(fresh.process({1, -1}), {})); CHECK(fresh.prepare(768000, factor));
        Candidate storage; allocations = frees = allocationBytes = 0; _CrtSetAllocHook(heapHook); counting = true;
        const bool ok = storage.prepare(768000, factor); counting = false; _CrtSetAllocHook(old);
        CHECK(ok); CHECK(allocations == 1); CHECK(frees == 1); CHECK(allocationBytes < 200000);
        std::cout << "STORAGE factor=" << factor << " prepared_bytes=" << allocationBytes << " allocations=" << allocations << " replaced_states=" << frees << " endpoint_bound=2\n";
        std::array<Frame, 17> frames {}; auto bad = profile(); bad.lanes[3].mix = std::numeric_limits<float>::quiet_NaN();
        bool admitted = true, readback = true, finiteOutput = true;
        allocations = frees = allocationBytes = 0; _CrtSetAllocHook(heapHook); counting = true;
        for (unsigned n = 0; n < 1900; ++n) {
            const auto s = rapid(n); admitted = storage.setSettings(s) && admitted; readback = (storage.settings() == s) && readback;
            admitted = !storage.setSettings(bad) && admitted;
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
    std::cout << "HEAP NOT_RUN: Debug CRT allocation hooks required; functional allocation checks provide no CPU acceptance\n";
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
            Candidate c; ready(c, settings, rate, 4); c.reset(); auto audio = input;
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
    throw std::runtime_error("--tail requires Release; Debug cannot claim this matrix");
#else
    unsigned cases = 0;
    for (double rate : {8000., 384000., 768000.}) for (unsigned factor : {4u, 8u}) for (unsigned state : {0u, 1u}) {
        auto s = profile(state == 0 ? 1 : 0, 1); s.inputTrimDb = 24; s.outputTrimDb = 12;
        s.lowCrossoverHz = 40; s.highCrossoverHz = 16000; s.preLowCutEnabled = s.postHighCutEnabled = true; s.preLowCutHz = 20; s.postHighCutHz = 1000;
        for (unsigned n = 0; n < 4; ++n) { auto& l = s.lanes[n]; l.driveDb = 36; l.trimDb = 18; l.preTiltDb = n % 2 ? -6.f : 6.f; if (state == 0) l.character = SaturationCharacter(n % 3); }
        const double seconds = saturationTailSeconds(s, rate); CHECK(seconds == 2);
        const unsigned count = unsigned(seconds * rate), window = unsigned(std::ceil(.020 * rate)), excitation = unsigned(.05 * rate);
        std::cout << "TAIL_BEGIN rate=" << rate << " factor=" << factor << " state=" << state << " declared_seconds=" << seconds
                  << " silence_frames=" << count << " window_frames=" << window << " excitation_frames=" << excitation << std::endl;
        Candidate c; ready(c, s, rate, factor); bool valid = true; double driven = 0, finalPeak = 0;
        for (unsigned n = 0; n < excitation; ++n) { auto x = signal(n, rate); if (state == 1) { x[0] += .02; x[1] -= .01; } const auto y = c.process(x); valid = finite(y) && valid; driven = std::max(driven, error(y)); }
        for (unsigned n = 0; n < count; ++n) { const auto y = c.process({}); valid = finite(y) && valid; if (n >= count - window) finalPeak = std::max(finalPeak, error(y)); }
        c.reset(); bool resetSilent = true; for (unsigned n = 0; n < window; ++n) resetSilent = exact(c.process({}), {}) && resetSilent;
        CHECK(valid); CHECK(driven > 1e-8); CHECK(finalPeak <= 1e-9); CHECK(resetSilent); CHECK(c.settings() == s); ++cases;
        std::cout << "TAIL rate=" << rate << " factor=" << factor << " state=" << state << " finite=" << valid << " driven_peak=" << driven
                  << " final20ms_peak=" << finalPeak << " threshold=1e-9 reset_exact_silence=" << resetSilent << std::endl;
    }
    CHECK(cases == 12); std::cout << "TAIL_TOTAL cases=" << cases << " helper_seconds=2 final_window_ms=20\n";
#endif
}
void provenance() {
    const auto descriptor = readFile(RACKSYNTH_LIFECYCLE_DESCRIPTOR, 32768);
    std::cout << "SOURCE_DESCRIPTOR sha256=" << digest(descriptor) << " bytes=" << descriptor.size() << '\n';
#if defined(_WIN32)
    HMODULE provider = nullptr; const auto procedure = reinterpret_cast<LPCWSTR>(&airack_saturation_release_pow);
    require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, procedure, &provider) != 0, "canonical gain provider");
    std::array<wchar_t, 32768> path {}; const auto size = GetModuleFileNameW(provider, path.data(), DWORD(path.size()));
    require(size > 0 && size < path.size(), "canonical gain provider path"); const fs::path providerPath(std::wstring(path.data(), size));
    const auto bytes = readFile(providerPath, 32 * 1024 * 1024);
    std::cout << "GAIN_PROVIDER path=" << providerPath.string() << " bytes=" << bytes.size() << " sha256=" << digest(bytes) << '\n';
#endif
    std::cout << "STATUS CPU=DEFERRED selected_factor=null adoptable=false production=FORCED_BYPASS numeric_gates=UNCHANGED R4-R7=OPEN"
                 " CPU_runner_heap=NOT_RUN CPU_output_integrity=NOT_RUN production_round_law=OPEN\n";
}
}
int main(int argc, char** argv) {
    if (argc != 2 || (std::string_view(argv[1]) != "--admission" && std::string_view(argv[1]) != "--functional"
            && std::string_view(argv[1]) != "--fixtures" && std::string_view(argv[1]) != "--tail")) {
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
            group("ceil_structural_and_latest_retarget", transitionLaws); group("high_rate_integrity_and_recovery", integrity);
            group("rapid_simultaneous_overlap", rapidOverlap); group("Debug_storage_heap_and_allocation_failure", heap);
        }
        if (mode == "--fixtures") group("frozen_v2_common", fixtures);
        if (mode == "--tail") group("Release_fixed_two_second_tail", tail);
    } catch (const std::exception& error) { ++failures; std::cout << "FAIL exception: " << error.what() << '\n'; }
    std::cout << "TOTAL checks=" << checks << " failures=" << failures << std::endl;
    return failures ? 1 : 0;
}

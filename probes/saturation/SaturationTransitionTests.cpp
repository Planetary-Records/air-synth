#include "SaturationCandidate.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using namespace airack;
using airack::probe::Candidate;
using Frame = Candidate::Frame;
constexpr double tolerance = 1e-12;
constexpr std::array rates { 44100., 48000., 96000. };
constexpr std::array modes { SaturationMode::fullBand, SaturationMode::threeBand };
constexpr std::array characters { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense };
int checks = 0, failures = 0;
const char* section = "";
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) { ++failures; std::cout << "FAIL " << section << ':' << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
template<class Test> void group(const char* name, Test test) {
    section = name; const auto before = checks, failed = failures; test();
    std::cout << name << ": " << checks - before << " checks, " << failures - failed << " failures\n";
}
unsigned duration(double rate) { return unsigned(std::ceil(.020 * rate)); }
double error(Frame a, Frame b) { return std::max(std::abs(a[0] - b[0]), std::abs(a[1] - b[1])); }
bool finite(Frame x) { return std::isfinite(x[0]) && std::isfinite(x[1]); }
Frame signal(unsigned n, double rate = 48000) {
    return { .021 * std::sin(2 * std::numbers::pi * 307 * n / rate) + .008 * std::cos(2 * std::numbers::pi * 7103 * n / rate),
             .017 * std::cos(2 * std::numbers::pi * 983 * n / rate) - .005 * std::sin(2 * std::numbers::pi * 11371 * n / rate) };
}
SaturationSettings profile(SaturationMode mode, SaturationCharacter character) {
    SaturationSettings s; s.mode = mode; s.driveCompensation = false;
    for (auto& l : s.lanes) { l.character = character; l.driveDb = 18; }
    return s;
}
void ready(Candidate& c, const SaturationSettings& s, double rate, unsigned factor = 1) {
    CHECK(c.setSettings(s)); CHECK(c.prepare(rate, factor));
}
Frame blend(Frame a, Frame b, double weight) {
    for (unsigned ch = 0; ch < 2; ++ch) a[ch] += weight * (b[ch] - a[ch]); return a;
}

// Both independent endpoint histories start empty. This checks actual audio,
// including the first and last output coefficients, without observing internals.
void structuralLaws() {
    for (double rate : rates) for (auto mode : modes) for (auto character : characters)
        for (unsigned edit = 0; edit < 4; ++edit) for (bool reverse : { false, true }) {
            auto a = profile(mode, character), b = a;
            if (edit == 0) for (auto& l : b.lanes) l.character = characters[(unsigned(character) + 1) % 3];
            if (edit == 1) b.mode = mode == SaturationMode::fullBand ? SaturationMode::threeBand : SaturationMode::fullBand;
            if (edit == 2) { b.preLowCutEnabled = true; b.preLowCutHz = a.preLowCutHz = 600; }
            if (edit == 3) { b.postHighCutEnabled = true; b.postHighCutHz = a.postHighCutHz = 1200; }
            if (reverse) std::swap(a, b);
            Candidate actual, from, to; ready(actual, a, rate); ready(from, a, rate); ready(to, b, rate);
            CHECK(actual.setSettings(b));
            double maxError = 0, separation = 0;
            for (unsigned n = 0; n < duration(rate) + 32; ++n) {
                const auto input = signal(n, rate), old = from.process(input), next = to.process(input);
                const double weight = std::min(1., double(n) / (duration(rate) - 1));
                const auto expected = blend(old, next, weight), y = actual.process(input);
                maxError = std::max(maxError, error(y, expected)); separation = std::max(separation, error(old, next));
                if (n == 0 || n == duration(rate) / 2 || n == duration(rate) - 1) CHECK(error(y, expected) <= tolerance);
            }
            CHECK(separation > 1e-7); CHECK(maxError <= tolerance);
        }
}
void compensationLaw() {
    for (double rate : rates) for (auto character : characters) for (bool reverse : { false, true }) {
        auto off = profile(SaturationMode::fullBand, character), on = off; on.driveCompensation = true;
        Candidate actual, raw; ready(actual, reverse ? on : off, rate); ready(raw, off, rate);
        for (unsigned n = 0; n < 2048; ++n) { const auto x = signal(n, rate); actual.process(x); raw.process(x); }
        CHECK(actual.setSettings(reverse ? off : on)); double maxError = 0;
        for (unsigned n = 0; n < duration(rate) + 16; ++n) {
            const double progress = std::min(1., double(n + 1) / duration(rate));
            const double correction = saturationDriveCompensationDb(character, 18) * (reverse ? 1 - progress : progress);
            auto expected = raw.process(signal(n + 2048, rate)); for (auto& x : expected) x *= std::pow(10., correction / 20);
            maxError = std::max(maxError, error(actual.process(signal(n + 2048, rate)), expected));
        }
        CHECK(maxError <= tolerance);
    }
}
void settledCompensationArithmetic() {
    for (auto character : characters) for (float drive : { 0.5f, 9.5f, 12.f, 35.5f }) for (float trim : { -18.f, -5.37f, 6.13f, 18.f }) {
        auto raw = profile(SaturationMode::fullBand, character); raw.lanes[0].driveDb = drive;
        auto compensated = raw; compensated.driveCompensation = true; compensated.lanes[0].trimDb = trim;
        Candidate actual, reference; ready(actual, compensated, 48000); ready(reference, raw, 48000);
        // The original gain input adds two floats before promotion to double.
        const float summedDb = trim + saturationDriveCompensationDb(character, drive);
        const double multiplier = std::pow(10., double(summedDb) / 20); double maxError = 0;
        for (unsigned n = 0; n < 2048; ++n) {
            const auto x = signal(n); auto expected = reference.process(x); for (auto& value : expected) value *= multiplier;
            maxError = std::max(maxError, error(actual.process(x), expected));
        }
        CHECK(maxError <= tolerance);
    }
}
void bypassLaw() {
    for (double rate : rates) for (auto mode : modes) for (auto character : characters) for (bool reverse : { false, true }) {
        auto wet = profile(mode, character), dry = wet; for (auto& l : dry.lanes) l.bypassed = true;
        Candidate actual, from, to; ready(actual, reverse ? dry : wet, rate); ready(from, wet, rate); ready(to, dry, rate);
        for (unsigned n = 0; n < 2048; ++n) { const auto x = signal(n, rate); actual.process(x); from.process(x); to.process(x); }
        CHECK(actual.setSettings(reverse ? wet : dry)); double maxError = 0;
        for (unsigned n = 0; n < duration(rate) + 16; ++n) {
            const auto x = signal(n + 2048, rate); const double progress = std::min(1., double(n + 1) / duration(rate));
            maxError = std::max(maxError, error(actual.process(x), blend(from.process(x), to.process(x), reverse ? 1 - progress : progress)));
        }
        CHECK(maxError <= tolerance);
    }
}
void retiredHistories() {
    for (double rate : rates) for (auto mode : modes) for (unsigned cut = 0; cut < 2; ++cut) {
        auto s = profile(mode, SaturationCharacter::glow); s.preLowCutHz = 600; s.postHighCutHz = 1200;
        (cut ? s.postHighCutEnabled : s.preLowCutEnabled) = true;
        Candidate actual; ready(actual, s, rate);
        for (unsigned n = 0; n < 4096; ++n) actual.process(signal(n, rate));
        (cut ? s.postHighCutEnabled : s.preLowCutEnabled) = false; CHECK(actual.setSettings(s));
        for (unsigned n = 0; n < unsigned(rate * 2); ++n) actual.process({});
        (cut ? s.postHighCutEnabled : s.preLowCutEnabled) = true; CHECK(actual.setSettings(s));
        double revived = 0; for (unsigned n = 0; n < duration(rate) + 4096; ++n) revived = std::max(revived, error(actual.process({}), {}));
        CHECK(revived <= tolerance);
        std::cout << "CUT_HISTORY rate=" << rate << " mode=" << unsigned(mode) << " cut=" << cut << " revived=" << revived << '\n';
    }
    for (double rate : rates) for (auto mode : modes) {
        auto s = profile(mode, SaturationCharacter::dense); Candidate actual; ready(actual, s, rate);
        for (unsigned n = 0; n < 4096; ++n) actual.process(signal(n, rate));
        s.mode = mode == SaturationMode::fullBand ? SaturationMode::threeBand : SaturationMode::fullBand; CHECK(actual.setSettings(s));
        for (unsigned n = 0; n < unsigned(rate * 2); ++n) actual.process({});
        s.mode = mode; CHECK(actual.setSettings(s)); double revived = 0;
        for (unsigned n = 0; n < duration(rate) + 4096; ++n) revived = std::max(revived, error(actual.process({}), {}));
        CHECK(revived <= tolerance);
    }
}
void disabledCutTracksCurrentStream() {
    for (double rate : rates) for (unsigned cut = 0; cut < 2; ++cut) {
        SaturationSettings off; off.driveCompensation = false; off.preLowCutHz = 600; off.postHighCutHz = 1200;
        auto on = off; (cut ? on.postHighCutEnabled : on.preLowCutEnabled) = true;
        Candidate actual, from, to; ready(actual, off, rate); ready(from, off, rate); ready(to, on, rate);
        for (unsigned n = 0; n < 4096; ++n) { const auto x = signal(n, rate); actual.process(x); from.process(x); to.process(x); }
        CHECK(actual.setSettings(on)); double maxError = 0;
        for (unsigned n = 0; n < duration(rate); ++n) {
            const auto x = signal(n + 4096, rate), y = actual.process(x);
            const auto expected = blend(from.process(x), to.process(x), double(n) / (duration(rate) - 1));
            // Pre-cut selection changes the ADAA lane's one-sample input
            // history; after one frame the neutral lane has the same history.
            if (n >= 1) maxError = std::max(maxError, error(y, expected));
        }
        CHECK(maxError <= tolerance);
    }
}
// A pending target cannot affect the current blend, and returning to its
// destination cancels it. Continuous edits must reach both running endpoints.
void pendingAndContinuous() {
    for (double rate : rates) {
        auto a = profile(SaturationMode::fullBand, SaturationCharacter::soft), b = a, c = a;
        for (auto& l : b.lanes) l.character = SaturationCharacter::glow;
        for (auto& l : c.lanes) l.character = SaturationCharacter::dense;
        Candidate actual, cancelled, expected; ready(actual, a, rate); ready(cancelled, a, rate); ready(expected, b, rate);
        CHECK(actual.setSettings(b)); CHECK(cancelled.setSettings(b));
        double firstFadeError = 0, afterCancelError = 0;
        for (unsigned n = 0; n < 2 * duration(rate) + 32; ++n) {
            if (n < duration(rate) - 1) {
                CHECK(actual.setSettings(n % 2 ? a : c)); CHECK(cancelled.setSettings(c)); CHECK(cancelled.setSettings(b));
            }
            const auto x = signal(n, rate), y = actual.process(x), z = cancelled.process(x), endpoint = expected.process(x);
            if (n < duration(rate)) firstFadeError = std::max(firstFadeError, error(y, z));
            if (n >= duration(rate)) afterCancelError = std::max(afterCancelError, error(z, endpoint));
        }
        CHECK(firstFadeError <= tolerance); CHECK(afterCancelError <= tolerance);
        Candidate queued, explicitChange; ready(queued, a, rate); ready(explicitChange, a, rate);
        CHECK(queued.setSettings(b)); CHECK(explicitChange.setSettings(b)); double queuedError = 0;
        for (unsigned n = 0; n < 2 * duration(rate) + 32; ++n) {
            if (n < duration(rate)) { CHECK(queued.setSettings(n % 2 ? a : c)); CHECK(queued.setSettings(c)); }
            if (n == duration(rate)) CHECK(explicitChange.setSettings(c));
            const auto x = signal(n, rate); queuedError = std::max(queuedError, error(queued.process(x), explicitChange.process(x)));
        }
        CHECK(queuedError <= tolerance);
        // Begin from silence, so independently prepared endpoint histories match.
        Candidate changing, from, to; ready(changing, a, rate); ready(from, a, rate); ready(to, b, rate); CHECK(changing.setSettings(b));
        double maxError = 0;
        for (unsigned n = 0; n < duration(rate); ++n) {
            if (n == 17) {
                auto edit = b; edit.outputTrimDb = -6; CHECK(changing.setSettings(edit));
                auto old = a; old.outputTrimDb = -6; CHECK(from.setSettings(old)); CHECK(to.setSettings(edit));
            }
            const auto x = signal(n, rate);
            maxError = std::max(maxError, error(changing.process(x), blend(from.process(x), to.process(x), double(n) / (duration(rate) - 1))));
        }
        CHECK(maxError <= tolerance);
    }
}
void simultaneousEditsAndSilence() {
    for (double rate : { 44100., 44101., 48000., 96000. }) {
        auto a = profile(SaturationMode::fullBand, SaturationCharacter::soft); a.preLowCutHz = 600; a.postHighCutHz = 1200;
        auto b = a; b.mode = SaturationMode::threeBand; b.preLowCutEnabled = b.postHighCutEnabled = true;
        for (auto& l : b.lanes) l.character = SaturationCharacter::dense;
        const auto toBase = b; auto fromTarget = a;
        for (auto* s : { &b, &fromTarget }) {
            s->driveCompensation = true; s->outputTrimDb = -6; s->mix = .4f;
            for (auto& l : s->lanes) { l.bypassed = true; l.driveDb = 12.5f; l.preTiltDb = -3; }
        }
        Candidate actual, from, to; ready(actual, a, rate); ready(from, a, rate); ready(to, toBase, rate);
        CHECK(actual.setSettings(b)); CHECK(from.setSettings(fromTarget)); CHECK(to.setSettings(b)); double maxError = 0;
        for (unsigned n = 0; n < duration(rate) + 32; ++n) {
            const auto x = signal(n, rate);
            maxError = std::max(maxError, error(actual.process(x), blend(from.process(x), to.process(x), std::min(1., double(n) / (duration(rate) - 1)))));
        }
        CHECK(maxError <= tolerance);
        actual.reset(); CHECK(actual.setSettings(a));
        bool silent = true; for (unsigned n = 0; n < 2 * duration(rate); ++n) silent = silent && error(actual.process({}), {}) == 0;
        CHECK(silent);
    }
}
void lifecycleAndPartitions() {
    for (double rate : rates) for (unsigned factor : { 1u, 2u, 4u, 8u, 64u }) {
        auto a = profile(SaturationMode::fullBand, SaturationCharacter::soft), b = profile(SaturationMode::threeBand, SaturationCharacter::dense);
        b.driveCompensation = true; b.preLowCutEnabled = b.postHighCutEnabled = true; b.lanes[2].bypassed = true;
        Candidate actual, reference; ready(actual, a, rate, factor); ready(reference, a, rate, factor);
        double null = 0; bool valid = true;
        for (unsigned n = 0; n < duration(rate) * 3; ++n) {
            if (n == 7 || n == 23) { CHECK(actual.setSettings(b)); CHECK(reference.setSettings(b)); }
            CHECK(actual.setSettings(n >= 7 ? b : a)); // Exact repeated intent cannot restart a ramp.
            auto bad = b; bad.lanes[3].mix = std::numeric_limits<float>::quiet_NaN(); CHECK(!actual.setSettings(bad));
            CHECK(!actual.prepare(7999, factor)); CHECK(!actual.prepare(rate, 3));
            const auto x = signal(n, rate), y = actual.process(x), z = reference.process(x); valid = valid && finite(y); null = std::max(null, error(y, z));
        }
        CHECK(valid); CHECK(null <= tolerance);
        // Latest intent, including a queued structure, survives reset/recovery.
        auto latest = b; latest.mode = SaturationMode::fullBand; CHECK(actual.setSettings(a)); actual.process(signal(0)); CHECK(actual.setSettings(latest));
        actual.reset(); Candidate fresh; ready(fresh, latest, rate, factor);
        for (unsigned n = 0; n < 64; ++n) CHECK(error(actual.process(signal(n)), fresh.process(signal(n))) <= tolerance);
        CHECK(actual.setSettings(b)); actual.process(signal(0)); CHECK(actual.setSettings(a));
        CHECK(error(actual.process({ std::numeric_limits<double>::quiet_NaN(), 0 }), {}) == 0);
        Candidate recovered; ready(recovered, a, rate, factor);
        for (unsigned n = 0; n < 64; ++n) CHECK(error(actual.process(signal(n)), recovered.process(signal(n))) <= tolerance);
    }
    // Same audio and one-frame edits, visited through independent irregular partitions.
    auto render = [](unsigned partition) {
        Candidate c; auto s = profile(SaturationMode::fullBand, SaturationCharacter::soft); ready(c, s, 48000, 4);
        std::vector<Frame> result(6000);
        for (unsigned begin = 0; begin < result.size(); begin += partition) for (unsigned n = begin; n < std::min<unsigned>(unsigned(result.size()), begin + partition); ++n) {
            if (n < 1000 || n % 701 == 0) { s.mode = n % 2 ? SaturationMode::fullBand : SaturationMode::threeBand; s.lanes[0].character = characters[n % 3]; s.driveCompensation = n % 5 == 0; CHECK(c.setSettings(s)); }
            result[n] = c.process(signal(n));
        }
        return result;
    };
    const auto expected = render(1); for (unsigned partition : { 17u, 64u, 127u, 512u }) {
        const auto actual = render(partition); double maxError = 0; for (unsigned n = 0; n < actual.size(); ++n) maxError = std::max(maxError, error(actual[n], expected[n])); CHECK(maxError == 0);
    }
}
void settledFixtures() {
    const auto root = std::filesystem::path(RACKSYNTH_SATURATION_FIXTURE_DIR);
    std::ifstream input(root / "input.f64", std::ios::binary); std::vector<Frame> x(2048); input.read(reinterpret_cast<char*>(x.data()), std::streamsize(x.size() * sizeof(Frame))); CHECK(input.good());
    for (double rate : rates) for (auto mode : modes) for (auto character : characters) {
        auto s = profile(mode, character); s.mix = .75f; for (auto& l : s.lanes) { l.driveDb = 12; l.mix = .75f; }
        Candidate c; ready(c, s, rate, 4); c.reset();
        const auto name = "settled-" + std::to_string(unsigned(rate)) + '-' + std::to_string(unsigned(mode)) + '-' + std::to_string(unsigned(character)) + ".f64";
        std::ifstream stream(root / name, std::ios::binary); double maxError = 0;
        for (auto frame : x) { Frame expected {}; stream.read(reinterpret_cast<char*>(expected.data()), sizeof(Frame)); maxError = std::max(maxError, error(c.process(frame), expected)); }
        CHECK(stream.good()); CHECK(maxError <= tolerance);
    }
}
#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool counting = false;
thread_local std::size_t allocations = 0, frees = 0, allocationBytes = 0;
int __cdecl heapHook(int operation, void*, std::size_t bytes, int type, long, const unsigned char*, int) {
    if (counting && type != _CRT_BLOCK) {
        if (operation == _HOOK_FREE) ++frees;
        else if (operation == _HOOK_ALLOC || operation == _HOOK_REALLOC) { ++allocations; allocationBytes += bytes; }
    } return 1;
}
#endif
void preparedStorage() {
#if defined(_MSC_VER) && defined(_DEBUG)
    for (unsigned factor : { 1u, 2u, 4u, 8u, 64u }) {
        Candidate c; allocations = frees = allocationBytes = 0;
        const auto old = _CrtSetAllocHook(heapHook); counting = true;
        const bool prepared = c.prepare(48000, factor);
        counting = false; _CrtSetAllocHook(old);
        CHECK(prepared); CHECK(allocations == (factor == 64 ? 7u : 1u)); CHECK(frees == 1); CHECK(allocationBytes < 200000);
        std::cout << "PREPARED_STORAGE factor=" << factor << " bytes=" << allocationBytes << " allocations=" << allocations << " replaced_states=" << frees << '\n';
    }
#endif
}
void heapAndStereo() {
    for (unsigned factor : { 1u, 4u, 64u }) {
        Candidate c, swapped; auto s = profile(SaturationMode::fullBand, SaturationCharacter::dense); ready(c, s, 48000, factor); ready(swapped, s, 48000, factor);
#if defined(_MSC_VER) && defined(_DEBUG)
        allocations = frees = 0; const auto old = _CrtSetAllocHook(heapHook); counting = true;
#endif
        bool admitted = true, valid = true; double null = 0;
        for (unsigned n = 0; n < 4500; ++n) {
            if (n < 2000 || n % 211 == 0) {
                s.mode = n % 2 ? SaturationMode::fullBand : SaturationMode::threeBand; s.preLowCutEnabled = n % 3 == 0; s.postHighCutEnabled = n % 7 == 0;
                s.driveCompensation = n % 11 == 0; s.outputTrimDb = float(int(n % 13) - 6);
                for (auto& l : s.lanes) { l.character = characters[n % 3]; l.bypassed = n % 5 == 0; l.driveDb = float(n % 37); l.preTiltDb = float(int(n % 13) - 6); l.mix = float(n % 11) / 10; }
                admitted = c.setSettings(s) && swapped.setSettings(s) && admitted;
            }
            if (n == 2190) { c.reset(); swapped.reset(); }
            const auto x = signal(n), y = c.process(x), z = swapped.process({ x[1], x[0] }); valid = valid && finite(y); null = std::max(null, error(y, { z[1], z[0] }));
        }
#if defined(_MSC_VER) && defined(_DEBUG)
        counting = false; _CrtSetAllocHook(old); CHECK(allocations == 0 && frees == 0);
        std::cout << "CALLBACK factor=" << factor << " allocations=" << allocations << " frees=" << frees << '\n';
#endif
        CHECK(admitted && valid); CHECK(null <= tolerance);
    }
}
}
int main() {
    group("structural_linear_20ms_audio", structuralLaws);
    group("compensation_linear_db_20ms", compensationLaw);
    group("settled_compensation_original_float_arithmetic", settledCompensationArithmetic);
    group("lane_bypass_matched_dry_20ms", bypassLaw);
    group("disabled_cuts_and_inactive_modes_retire_history", retiredHistories);
    group("disabled_cuts_track_current_stream", disabledCutTracksCurrentStream);
    group("pending_latest_cancel_and_continuous_controls", pendingAndContinuous);
    group("simultaneous_edits_silence_and_ceil", simultaneousEditsAndSilence);
    group("atomicity_reset_recovery_partitions", lifecycleAndPartitions);
    group("frozen_18_common_fixtures", settledFixtures);
    group("callback_heap_and_stereo", heapAndStereo);
    group("prepared_storage_bound", preparedStorage);
    std::cout << "Saturation transitions: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}

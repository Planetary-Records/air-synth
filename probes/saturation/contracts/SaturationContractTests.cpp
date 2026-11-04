#include "SaturationCandidate.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

namespace {
using namespace airack;
using airack::probe::Candidate;
using Frame = Candidate::Frame;
constexpr std::size_t frames = 2048;
constexpr unsigned diagnosticFactor = 4; // UNADOPTED: this is a fixture configuration.
constexpr std::array<std::size_t, 5> partitions { 1, 17, 64, 127, 512 };
int checks = 0, failures = 0;
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) { ++failures; std::cout << "FAIL line=" << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)

std::vector<Frame> input() {
    std::vector<Frame> result(frames);
    // Dyadic input values have no trigonometric library or random-device dependency.
    for (std::size_t n = 0; n < frames; ++n)
        result[n] = { (int((n * 73 + 19) % 2048) - 1024) / 32768.,
                      (int((n * 151 + 61) % 2048) - 1024) / 65536. };
    return result;
}

SaturationSettings profile(unsigned index) {
    SaturationSettings s;
    s.mode = index % 2 ? SaturationMode::threeBand : SaturationMode::fullBand;
    s.driveCompensation = false;
    s.mix = .75f;
    s.lowCrossoverHz = 120.f + 30.f * (index % 5);
    s.highCrossoverHz = 2000.f + 400.f * (index % 5);
    for (unsigned lane = 0; lane < s.lanes.size(); ++lane) {
        auto& l = s.lanes[lane];
        l.character = static_cast<SaturationCharacter>((index + lane) % 3);
        l.driveDb = 6.f + 3.f * ((index + lane) % 7);
        l.preTiltDb = float(int((index + lane) % 5) - 2);
        l.mix = .5f + .125f * (lane % 3);
    }
    return s;
}

enum class Operation { settings, reset, prepare };
struct Event {
    std::size_t frame;
    Operation operation;
    SaturationSettings settings {};
    double rate = 48000;
    unsigned factor = diagnosticFactor;
    bool admitted = true;
};
std::vector<Event> events() {
    std::vector<Event> result;
    unsigned index = 0;
    // Before/on/after the first and later block boundaries, including adjacent edits.
    for (std::size_t frame : { 0, 1, 2, 16, 17, 18, 63, 64, 65, 126, 127, 128,
                              511, 512, 513, 1023, 1024, 1025, 1535, 1536, 1537 })
        result.push_back({ frame, Operation::settings, profile(index++) });
    result.push_back({ 1024, Operation::settings, profile(30) }); // Same-frame order matters.
    auto invalid = profile(0);
    invalid.lanes[3].mix = std::numeric_limits<float>::quiet_NaN(); // Retained bank too.
    result.push_back({ 64, Operation::settings, invalid, 48000, diagnosticFactor, false });
    result.push_back({ 127, Operation::prepare, {}, 48000, 3, false });
    result.push_back({ 511, Operation::prepare, {}, 7999, diagnosticFactor, false });
    result.push_back({ 512, Operation::prepare, {}, 384001, diagnosticFactor, false });
    result.push_back({ 513, Operation::prepare, {}, 768000, diagnosticFactor, false });
    result.push_back({ 1023, Operation::reset });
    result.push_back({ 1024, Operation::prepare, {}, 8000 });
    result.push_back({ 1535, Operation::prepare, {}, 384000 });
    result.push_back({ 1536, Operation::reset });
    result.push_back({ 1537, Operation::prepare });
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
    return result;
}

void apply(Candidate& candidate, const Event& event) {
    switch (event.operation) {
        case Operation::settings: CHECK(candidate.setSettings(event.settings) == event.admitted); break;
        case Operation::prepare: CHECK(candidate.prepare(event.rate, event.factor) == event.admitted); break;
        case Operation::reset: candidate.reset(); break;
    }
}

std::vector<Frame> blocked(const std::vector<Frame>& source, const std::vector<Event>& timeline, std::size_t partition) {
    Candidate candidate;
    CHECK(candidate.prepare(48000, diagnosticFactor));
    std::vector<Frame> output(source.size());
    std::size_t event = 0;
    for (std::size_t begin = 0; begin < source.size(); begin += partition) {
        const auto end = std::min(begin + partition, source.size());
        auto cursor = begin;
        while (cursor < end) {
            while (event < timeline.size() && timeline[event].frame == cursor)
                apply(candidate, timeline[event++]);
            const auto segmentEnd = event < timeline.size() ? std::min(end, timeline[event].frame) : end;
            for (; cursor < segmentEnd; ++cursor) output[cursor] = candidate.process(source[cursor]);
        }
    }
    CHECK(event == timeline.size());
    return output;
}

std::vector<Frame> sampleOracle(const std::vector<Frame>& source, const std::vector<Event>& timeline) {
    Candidate candidate;
    CHECK(candidate.prepare(48000, diagnosticFactor));
    std::vector<Frame> output;
    output.reserve(source.size());
    std::size_t next = 0;
    // Deliberately independent of the block dispatcher and its event-application helper.
    for (std::size_t frame = 0; frame < source.size(); ++frame) {
        for (; next < timeline.size() && timeline[next].frame == frame; ++next) {
            const auto& e = timeline[next];
            if (e.operation == Operation::reset) candidate.reset();
            else if (e.operation == Operation::prepare) CHECK(candidate.prepare(e.rate, e.factor) == e.admitted);
            else CHECK(candidate.setSettings(e.settings) == e.admitted);
        }
        output.push_back(candidate.process(source[frame]));
    }
    CHECK(next == timeline.size());
    return output;
}

struct Comparison { std::size_t different = 0; double maxError = 0; };
Comparison compare(const std::vector<Frame>& a, const std::vector<Frame>& b, double tolerance = 0) {
    Comparison result;
    CHECK(a.size() == b.size());
    for (std::size_t n = 0; n < std::min(a.size(), b.size()); ++n) {
        bool different = false;
        for (unsigned channel = 0; channel < 2; ++channel) {
            const auto x = a[n][channel], y = b[n][channel];
            const bool finite = std::isfinite(x) && std::isfinite(y);
            const auto error = finite ? std::abs(x - y) : std::numeric_limits<double>::infinity();
            result.maxError = std::max(result.maxError, error);
            different |= !finite || (tolerance == 0 ? std::bit_cast<std::uint64_t>(x) != std::bit_cast<std::uint64_t>(y) : error > tolerance);
        }
        result.different += different;
    }
    return result;
}

bool shiftOffsetControl(std::vector<Event>& timeline) {
    auto event = std::find_if(timeline.begin(), timeline.end(), [](const auto& e) { return e.frame == 512 && e.operation == Operation::settings; });
    if (event == timeline.end()) {
        std::cout << "HARNESS_CONTROL_REFUSED missing_offset_event\n";
        return false;
    }
    ++event->frame;
    std::stable_sort(timeline.begin(), timeline.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
    return true;
}
bool reorderControl(std::vector<Event>& timeline) {
    auto first = std::find_if(timeline.begin(), timeline.end(), [](const auto& e) { return e.frame == 1024 && e.operation == Operation::settings; });
    if (first == timeline.end()) {
        std::cout << "HARNESS_CONTROL_REFUSED missing_first_order_event\n";
        return false;
    }
    auto second = std::find_if(first + 1, timeline.end(), [](const auto& e) { return e.frame == 1024 && e.operation == Operation::settings; });
    if (second == timeline.end()) {
        std::cout << "HARNESS_CONTROL_REFUSED missing_second_order_event\n";
        return false;
    }
    std::swap(first->settings, second->settings);
    return true;
}
void negativeControlGuards() {
    std::vector<Event> missingOffset { { 1, Operation::settings, profile(0) } };
    CHECK(!shiftOffsetControl(missingOffset));
    CHECK(missingOffset.size() == 1 && missingOffset[0].frame == 1);
    std::vector<Event> missingFirst;
    CHECK(!reorderControl(missingFirst));
    CHECK(missingFirst.empty());
    std::vector<Event> missingSecond { { 1024, Operation::settings, profile(0) } };
    CHECK(!reorderControl(missingSecond));
    CHECK(missingSecond.size() == 1 && missingSecond[0].frame == 1024
          && missingSecond[0].settings == profile(0));
    std::cout << "HARNESS_GUARD_REGRESSIONS missing_offset=1 missing_first=1 missing_second=1 expected_refusals=3\n";
}

void negativeControls(const std::vector<Frame>& source, const std::vector<Event>& timeline) {
    const auto expected = sampleOracle(source, timeline);
    auto shifted = timeline;
    const bool shiftedReady = shiftOffsetControl(shifted);
    CHECK(shiftedReady);
    if (!shiftedReady) return;
    const auto offset = compare(expected, blocked(source, shifted, 64));
    CHECK(offset.different > 0 && offset.maxError > 1e-12);
    auto reordered = timeline;
    const bool reorderedReady = reorderControl(reordered);
    CHECK(reorderedReady);
    if (!reorderedReady) return;
    const auto order = compare(expected, blocked(source, reordered, 127));
    CHECK(order.different > 0 && order.maxError > 1e-12);
    std::cout << "HARNESS_NEGATIVE_CONTROL offset_mismatches=" << offset.different << " offset_max_error=" << offset.maxError
              << " order_mismatches=" << order.different << " order_max_error=" << order.maxError << " rejected=1 product_RED=0\n";
}

using Bank = std::array<Candidate, 8>;
using Bypass = std::array<bool, 8>;
struct SerialFixture {
    Bank candidates;
    Bypass bypass {};
    Frame process(Frame frame) {
        for (std::size_t slot = 0; slot < candidates.size(); ++slot)
            if (!bypass[slot]) frame = candidates[slot].process(frame);
        return frame;
    }
    void reset() { for (auto& candidate : candidates) candidate.reset(); }
};
Frame manualChain(Bank& bank, const Bypass& bypass, Frame frame) {
    // This is intentionally unrolled instead of invoking the fixture route loop.
    if (!bypass[0]) frame = bank[0].process(frame);
    if (!bypass[1]) frame = bank[1].process(frame);
    if (!bypass[2]) frame = bank[2].process(frame);
    if (!bypass[3]) frame = bank[3].process(frame);
    if (!bypass[4]) frame = bank[4].process(frame);
    if (!bypass[5]) frame = bank[5].process(frame);
    if (!bypass[6]) frame = bank[6].process(frame);
    if (!bypass[7]) frame = bank[7].process(frame);
    return frame;
}
void prepare(Bank& bank, bool fullOnly = false) {
    for (unsigned slot = 0; slot < bank.size(); ++slot) {
        auto s = profile(slot);
        if (fullOnly) s.mode = SaturationMode::fullBand;
        CHECK(bank[slot].prepare(48000, diagnosticFactor));
        CHECK(bank[slot].setSettings(s));
    }
}

std::vector<Frame> serialContracts(const std::vector<Frame>& source) {
    SerialFixture fixture;
    Bank manual;
    Bypass bypass {};
    prepare(fixture.candidates); prepare(manual);
    std::vector<Frame> actual, expected;
    // Fixture policy: raw bypass freezes history; settings still admit; reset clears ALL slots.
    for (std::size_t n = 0; n < source.size(); ++n) {
        if (n == 63) fixture.bypass[2] = bypass[2] = true;
        if (n == 64 || n == 65) {
            auto s = profile(n == 64 ? 14 : 15);
            CHECK(fixture.candidates[2].setSettings(s)); CHECK(manual[2].setSettings(s));
        }
        if (n == 127) fixture.bypass[2] = bypass[2] = false;
        if (n == 511) { fixture.bypass.fill(true); bypass.fill(true); }
        if (n == 512) {
            CHECK(fixture.candidates[7].setSettings(profile(19))); CHECK(manual[7].setSettings(profile(19)));
            fixture.reset(); for (auto& candidate : manual) candidate.reset();
        }
        if (n == 513) { fixture.bypass.fill(false); bypass.fill(false); }
        if (n == 1024) { fixture.reset(); for (auto& candidate : manual) candidate.reset(); }
        actual.push_back(fixture.process(source[n]));
        expected.push_back(manualChain(manual, bypass, source[n]));
        if (n == 511 || n == 512) CHECK(actual.back() == source[n]);
    }
    const auto result = compare(actual, expected);
    CHECK(result.different == 0);
    std::cout << "SERIAL mixed_eight_slots exact_mismatches=" << result.different << " frames=" << source.size() << '\n';

    fixture.bypass.fill(true);
    std::vector<Frame> raw;
    for (auto frame : source) raw.push_back(fixture.process(frame));
    CHECK(compare(raw, source).different == 0);

    SerialFixture dry, neutral;
    for (unsigned slot = 0; slot < 8; ++slot) {
        auto s = profile(slot); s.mix = 0;
        SaturationSettings reference; reference.mode = s.mode;
        reference.lowCrossoverHz = s.lowCrossoverHz; reference.highCrossoverHz = s.highCrossoverHz;
        CHECK(dry.candidates[slot].prepare(48000, diagnosticFactor)); CHECK(dry.candidates[slot].setSettings(s));
        CHECK(neutral.candidates[slot].prepare(48000, diagnosticFactor)); CHECK(neutral.candidates[slot].setSettings(reference));
    }
    std::vector<Frame> engaged, matched;
    for (auto frame : source) { engaged.push_back(dry.process(frame)); matched.push_back(neutral.process(frame)); }
    const auto aligned = compare(engaged, matched, 1e-12);
    CHECK(aligned.different == 0);
    CHECK(compare(engaged, source, 1e-12).different > 0);
    std::cout << "SERIAL engaged_mix_zero matched_reference_max_error=" << aligned.maxError << " tolerance=1e-12 raw_bypass_exact=1\n";

    SerialFixture retained, unchanged;
    prepare(retained.candidates, true); prepare(unchanged.candidates, true);
    std::vector<Frame> edited, control;
    for (std::size_t n = 0; n < source.size(); ++n) {
        if (n == 128) for (unsigned slot = 0; slot < 8; ++slot) {
            auto s = profile(slot); s.mode = SaturationMode::fullBand;
            for (unsigned lane = 1; lane < 4; ++lane) { s.lanes[lane].driveDb = 36; s.lanes[lane].character = SaturationCharacter::dense; }
            CHECK(retained.candidates[slot].setSettings(s));
        }
        edited.push_back(retained.process(source[n])); control.push_back(unchanged.process(source[n]));
    }
    CHECK(compare(edited, control).different == 0);
    std::cout << "SERIAL inactive_bank_edits exact_mismatches=0 history_policy=freeze_on_raw_bypass_reset_all\n";
    return expected;
}

void settingsRow(std::ostream& out, const SaturationSettings& s) {
    out << ',' << int(s.mode) << ',' << s.inputTrimDb << ',' << s.outputTrimDb << ',' << s.mix << ',' << s.driveCompensation
        << ',' << s.lowCrossoverHz << ',' << s.highCrossoverHz << ',' << s.preLowCutEnabled << ',' << s.preLowCutHz
        << ',' << s.postHighCutEnabled << ',' << s.postHighCutHz;
    for (const auto& lane : s.lanes)
        out << ',' << lane.bypassed << ',' << int(lane.character) << ',' << lane.driveDb << ',' << lane.preTiltDb << ',' << lane.mix << ',' << lane.trimDb;
}
void exportReference(const std::filesystem::path& directory, const std::vector<Frame>& source,
                     const std::vector<Event>& timeline, const std::vector<Frame>& eventExpected, const std::vector<Frame>& serialExpected) {
    std::filesystem::create_directories(directory);
    for (const auto& item : { std::pair{ "input.f64", &source }, { "event-expected.f64", &eventExpected }, { "serial-expected.f64", &serialExpected } }) {
        std::ofstream out(directory / item.first, std::ios::binary);
        for (const auto& frame : *item.second) out.write(reinterpret_cast<const char*>(frame.data()), sizeof(double) * 2);
        CHECK(out.good());
    }
    std::ofstream out(directory / "events-settings.csv");
    out << std::setprecision(17) << "frame,operation,admitted,rate,factor,mode,input_trim,output_trim,mix,compensation,low_hz,high_hz,pre_on,pre_hz,post_on,post_hz";
    for (unsigned lane = 0; lane < 4; ++lane)
        for (const auto* field : { "bypass", "character", "drive", "tilt", "mix", "trim" }) out << ",lane" << lane << '_' << field;
    out << '\n';
    for (const auto& e : timeline) {
        out << e.frame << ',' << (e.operation == Operation::settings ? "settings" : e.operation == Operation::reset ? "reset" : "prepare")
            << ',' << e.admitted << ',' << e.rate << ',' << e.factor;
        settingsRow(out, e.settings); out << '\n';
    }
    CHECK(out.good());
    std::ofstream route(directory / "serial-settings.csv");
    route << std::setprecision(17) << "frame,slot,mode,input_trim,output_trim,mix,compensation,low_hz,high_hz,pre_on,pre_hz,post_on,post_hz";
    for (unsigned lane = 0; lane < 4; ++lane)
        for (const auto* field : { "bypass", "character", "drive", "tilt", "mix", "trim" }) route << ",lane" << lane << '_' << field;
    route << '\n';
    for (unsigned slot = 0; slot < 8; ++slot) { route << "0," << slot; settingsRow(route, profile(slot)); route << '\n'; }
    for (const auto& edit : { std::array<unsigned, 3>{ 64, 2, 14 }, { 65, 2, 15 }, { 512, 7, 19 } }) {
        route << edit[0] << ',' << edit[1]; settingsRow(route, profile(edit[2])); route << '\n';
    }
    CHECK(route.good());
    std::ofstream routeEvents(directory / "serial-events.csv");
    routeEvents << "frame,operation,slot,bypassed\n63,bypass,2,1\n64,settings,2,\n65,settings,2,\n127,bypass,2,0\n511,bypass,all,1\n512,settings,7,\n512,reset,all,\n513,bypass,all,0\n1024,reset,all,\n";
    CHECK(routeEvents.good());
    std::ofstream settled(directory / "settled-settings.csv");
    settled << std::setprecision(17) << "case,file,rate,factor,mode,input_trim,output_trim,mix,compensation,low_hz,high_hz,pre_on,pre_hz,post_on,post_hz";
    for (unsigned lane = 0; lane < 4; ++lane)
        for (const auto* field : { "bypass", "character", "drive", "tilt", "mix", "trim" }) settled << ",lane" << lane << '_' << field;
    settled << '\n';
    unsigned index = 0;
    for (const auto rate : { 44100, 48000, 96000 })
        for (const auto mode : { SaturationMode::fullBand, SaturationMode::threeBand })
            for (const auto character : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
                SaturationSettings s;
                s.mode = mode; s.driveCompensation = false; s.mix = .75f;
                for (auto& lane : s.lanes) { lane.character = character; lane.driveDb = 12; lane.mix = .75f; }
                Candidate candidate;
                // First admission before prepare establishes settled targets; reset clears zero histories.
                CHECK(candidate.setSettings(s)); CHECK(candidate.prepare(rate, diagnosticFactor)); candidate.reset();
                const auto name = "settled-" + std::to_string(rate) + '-' + std::to_string(int(mode)) + '-' + std::to_string(int(character)) + ".f64";
                settled << index++ << ',' << name << ',' << rate << ',' << diagnosticFactor;
                settingsRow(settled, s); settled << '\n';
                std::ofstream audio(directory / name, std::ios::binary);
                for (const auto& frame : source) {
                    const auto output = candidate.process(frame);
                    audio.write(reinterpret_cast<const char*>(output.data()), sizeof(double) * 2);
                }
                audio.close();
                CHECK(audio.good());
            }
    settled.close();
    CHECK(index == 18 && settled.good());
    std::cout << "REFERENCE UNADOPTED source_bound frames=" << frames << " format=IEEE754_f64_native_little_endian_stereo\n";
}
}

#if defined(AIRACK_SATURATION_TRANSITION_CONTRACTS)
namespace {
constexpr std::size_t chargedPrefix = 4096;
constexpr std::array<std::size_t, 9> irregular { 0, 17, 1, 512, 0, 127, 64, 3, 251 };
enum class TransitionOperation { settings, prepare, reset, bypass, resetAll };
struct TransitionEvent {
    std::size_t frame;
    TransitionOperation operation;
    unsigned slot = 0;
    SaturationSettings settings {};
    double rate = 48000;
    unsigned factor = diagnosticFactor;
    bool admitted = true, bypassed = false;
};
std::size_t fadeFrames(double rate) { return std::size_t(std::ceil(.020 * rate)); }
std::vector<Frame> transitionInput(double rate) {
    // Declared before observing output: a real charged prefix and >=2 fades after the final edit.
    std::vector<Frame> source(chargedPrefix + 12 * fadeFrames(rate));
    for (std::size_t n = 0; n < source.size(); ++n)
        source[n] = { (int((n * 73 + 19) % 2048) - 1024) / 32768.,
                      (int((n * 151 + 61) % 2048) - 1024) / 65536. };
    return source;
}
SaturationSettings transitionProfile(unsigned index) {
    auto s = profile(index);
    s.preLowCutEnabled = index % 3 == 1; s.preLowCutHz = 200.f + 30.f * (index % 7);
    s.postHighCutEnabled = index % 3 == 2; s.postHighCutHz = 3500.f + 250.f * (index % 7);
    return s;
}
SaturationSettings movingControls(SaturationSettings s) {
    s.driveCompensation = true; s.inputTrimDb = 1.5f; s.outputTrimDb = -2.f; s.mix = .625f;
    s.lowCrossoverHz += 15; s.highCrossoverHz += 100;
    for (unsigned lane = 0; lane < 4; ++lane) {
        s.lanes[lane].bypassed = lane % 2 == 0; s.lanes[lane].driveDb += 1;
        s.lanes[lane].preTiltDb += .5f; s.lanes[lane].trimDb = -.75f; s.lanes[lane].mix = .375f;
    }
    return s;
}
std::vector<TransitionEvent> transitionEvents(double rate, bool route) {
    const auto f = fadeFrames(rate), start = chargedPrefix;
    std::vector<TransitionEvent> timeline;
    const auto setting = [&](std::size_t frame, unsigned slot, SaturationSettings s, bool admitted = true) {
        timeline.push_back({ frame, TransitionOperation::settings, slot, s, rate, diagnosticFactor, admitted });
    };
    for (unsigned slot = 0; slot < (route ? 8u : 1u); ++slot) {
        const auto destination = transitionProfile(slot + 8);
        setting(start, slot, destination); // Idle fade start: the offset negative control must be audible.
        setting(start + 1, slot, movingControls(destination));
        setting(start + f / 2, slot, transitionProfile(slot + 17));
        setting(start + f / 2 + 1, slot, transitionProfile(slot + 18)); // Replace pending intent.
        setting(start + f - 1, slot, movingControls(destination)); // Cancel pending intent.
        setting(start + f, slot, movingControls(destination));
        for (const auto partition : { 17u, 64u, 127u, 512u }) {
            const auto boundary = ((start + 32 + partition - 1) / partition) * partition;
            for (const auto frame : { boundary - 1, boundary, boundary + 1 }) {
                const auto offset = frame - start;
                auto s = movingControls(offset < f / 2 ? destination : transitionProfile(slot + 18));
                s.outputTrimDb -= .125f * float(frame % 5);
                setting(frame, slot, s);
            }
        }
        // Both same-frame destinations remain observable for >=2 fades before another structural edit.
        setting(start + 2 * f, slot, transitionProfile(slot + 20));
        setting(start + 2 * f, slot, transitionProfile(slot + 21));
        setting(start + 5 * f, slot, transitionProfile(slot + 22));
        timeline.push_back({ start + 5 * f + f / 2, TransitionOperation::reset, slot });
        setting(start + 6 * f, slot, transitionProfile(slot + 23));
        timeline.push_back({ start + 6 * f + f / 2, TransitionOperation::prepare, slot, {}, rate });
        setting(start + 7 * f, slot, transitionProfile(slot + 24));
        setting(start + 8 * f, slot, transitionProfile(slot + 25));
        setting(start + 9 * f, slot, transitionProfile(slot + 26));
    }
    // Before/on/after boundaries, including continuous edits while both endpoints are running.
    for (const auto n : { 16u, 17u, 18u, 63u, 64u, 65u, 126u, 127u, 128u, 511u, 512u, 513u }) {
        auto s = transitionProfile(0);
        s.outputTrimDb = -.125f * float(n % 7);
        setting(n, 0, s);
    }
    // Rejected admissions are also compared with a trace where these calls never happened.
    auto invalid = transitionProfile(0); invalid.mode = SaturationMode::fullBand;
    invalid.lanes[3].mix = std::numeric_limits<float>::quiet_NaN();
    setting(start + f / 3, 0, invalid, false);
    for (const auto badRate : { 7999., 384001., 768000. })
        timeline.push_back({ start + f / 3 + 1, TransitionOperation::prepare, 0, {}, badRate, diagnosticFactor, false });
    timeline.push_back({ start + f / 3 + 2, TransitionOperation::prepare, 0, {}, rate, 3, false });
    if (route) {
        for (unsigned slot = 0; slot < 8; ++slot) {
            timeline.push_back({ start + 4 * f + slot, TransitionOperation::bypass, slot, {}, rate, diagnosticFactor, true, true });
            setting(start + 4 * f + 8 + slot, slot, movingControls(transitionProfile(slot + 21)));
            setting(start + 4 * f + f / 3, slot, transitionProfile(slot + 27));
        }
        timeline.push_back({ start + 4 * f + f / 3 + 1, TransitionOperation::resetAll });
        for (unsigned slot = 0; slot < 8; ++slot)
            timeline.push_back({ start + 5 * f - 1, TransitionOperation::bypass, slot, {}, rate, diagnosticFactor, true, false });
    }
    std::stable_sort(timeline.begin(), timeline.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
    return timeline;
}

struct TransitionBlockAdapter {
    SerialFixture route;
    const std::vector<TransitionEvent>& timeline;
    std::size_t cursor = 0, next = 0, emptyBlocks = 0;
    TransitionBlockAdapter(double rate, bool eightSlots, const std::vector<TransitionEvent>& events) : timeline(events) {
        route.bypass.fill(!eightSlots); route.bypass[0] = false;
        for (unsigned slot = 0; slot < (eightSlots ? 8u : 1u); ++slot) {
            CHECK(route.candidates[slot].setSettings(transitionProfile(slot)));
            CHECK(route.candidates[slot].prepare(rate, diagnosticFactor));
        }
    }
    void block(const std::vector<Frame>& source, std::vector<Frame>& output, std::size_t count) {
        if (count == 0) { ++emptyBlocks; return; } // No admission or audio consumption in an empty block.
        const auto end = std::min(cursor + count, source.size());
        while (cursor < end) {
            while (next < timeline.size() && timeline[next].frame == cursor) {
                const auto& e = timeline[next++];
                auto& c = route.candidates[e.slot];
                switch (e.operation) {
                    case TransitionOperation::settings: CHECK(c.setSettings(e.settings) == e.admitted); break;
                    case TransitionOperation::prepare: CHECK(c.prepare(e.rate, e.factor) == e.admitted); break;
                    case TransitionOperation::reset: c.reset(); break;
                    case TransitionOperation::bypass: route.bypass[e.slot] = e.bypassed; break;
                    case TransitionOperation::resetAll: route.reset(); break;
                }
            }
            const auto segmentEnd = next < timeline.size() ? std::min(end, timeline[next].frame) : end;
            for (; cursor < segmentEnd; ++cursor) {
                output[cursor] = route.process(source[cursor]);
                if (std::all_of(route.bypass.begin(), route.bypass.end(), [](bool b) { return b; }))
                    for (unsigned channel = 0; channel < 2; ++channel)
                        CHECK(std::bit_cast<std::uint64_t>(output[cursor][channel]) == std::bit_cast<std::uint64_t>(source[cursor][channel]));
            }
        }
    }
};
std::vector<Frame> transitionBlocked(const std::vector<Frame>& source, const std::vector<TransitionEvent>& timeline,
                                     double rate, bool route, std::size_t partition) {
    TransitionBlockAdapter adapter(rate, route, timeline);
    std::vector<Frame> output(source.size());
    std::size_t block = 0;
    while (adapter.cursor < source.size()) {
        const auto beforeCursor = adapter.cursor, beforeEvent = adapter.next;
        adapter.block(source, output, 0);
        CHECK(adapter.cursor == beforeCursor && adapter.next == beforeEvent);
        adapter.block(source, output, partition ? partition : irregular[block++ % irregular.size()]);
    }
    adapter.block(source, output, 0);
    CHECK(adapter.next == timeline.size() && adapter.emptyBlocks > 0);
    return output;
}
std::vector<Frame> transitionOracle(const std::vector<Frame>& source, const std::vector<TransitionEvent>& timeline,
                                   double rate, bool route, bool omitRejected = false) {
    Bank bank; Bypass bypass {}; bypass.fill(!route); bypass[0] = false;
    for (unsigned slot = 0; slot < (route ? 8u : 1u); ++slot) {
        CHECK(bank[slot].setSettings(transitionProfile(slot)));
        CHECK(bank[slot].prepare(rate, diagnosticFactor));
    }
    std::vector<Frame> output; output.reserve(source.size());
    std::size_t next = 0;
    // Independent frame schedule and event application; no block dispatcher or fixture route loop.
    for (std::size_t frame = 0; frame < source.size(); ++frame) {
        for (; next < timeline.size() && timeline[next].frame == frame; ++next) {
            const auto& e = timeline[next];
            if (omitRejected && !e.admitted) continue;
            if (e.operation == TransitionOperation::settings) CHECK(bank[e.slot].setSettings(e.settings) == e.admitted);
            else if (e.operation == TransitionOperation::prepare) CHECK(bank[e.slot].prepare(e.rate, e.factor) == e.admitted);
            else if (e.operation == TransitionOperation::reset) bank[e.slot].reset();
            else if (e.operation == TransitionOperation::bypass) bypass[e.slot] = e.bypassed;
            else {
                bank[0].reset(); bank[1].reset(); bank[2].reset(); bank[3].reset();
                bank[4].reset(); bank[5].reset(); bank[6].reset(); bank[7].reset();
            }
        }
        output.push_back(manualChain(bank, bypass, source[frame]));
    }
    CHECK(next == timeline.size());
    return output;
}
bool transitionControl(std::vector<TransitionEvent>& timeline, double rate, std::string_view control) {
    const auto at = chargedPrefix + (control == "order" ? 2 * fadeFrames(rate) : 0);
    auto first = std::find_if(timeline.begin(), timeline.end(), [&](const auto& e) {
        return e.frame == at && e.slot == 0 && e.operation == TransitionOperation::settings;
    });
    if (first == timeline.end()) { std::cout << "TRANSITION_CONTROL_REFUSED missing_first control=" << control << '\n'; return false; }
    if (control == "order") {
        auto second = std::find_if(std::next(first), timeline.end(), [&](const auto& e) {
            return e.frame == at && e.slot == 0 && e.operation == TransitionOperation::settings;
        });
        if (second == timeline.end()) { std::cout << "TRANSITION_CONTROL_REFUSED missing_second\n"; return false; }
        std::swap(first->settings, second->settings);
    } else if (control == "offset") {
        ++first->frame;
        std::stable_sort(timeline.begin(), timeline.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
    } else if (control == "slot") first->slot = 7;
    else return false;
    return true;
}
void transitionGuards() {
    for (const auto control : { "offset", "order", "slot" }) {
        std::vector<TransitionEvent> missing;
        CHECK(!transitionControl(missing, 48000, control)); CHECK(missing.empty());
    }
    std::vector<TransitionEvent> one { { chargedPrefix + 2 * fadeFrames(48000), TransitionOperation::settings, 0, transitionProfile(20) } };
    CHECK(!transitionControl(one, 48000, "order"));
    CHECK(one.size() == 1 && one[0].settings == transitionProfile(20));
    std::cout << "TRANSITION_GUARDS missing_first=3 missing_second=1 unchanged=1\n";
}
void transitionSerialLaws(double rate, const std::vector<Frame>& source) {
    SerialFixture dry, neutral, retained, unchanged;
    for (unsigned slot = 0; slot < 8; ++slot) {
        auto s = profile(slot); s.mix = 0;
        SaturationSettings matched; matched.mode = s.mode;
        matched.lowCrossoverHz = s.lowCrossoverHz; matched.highCrossoverHz = s.highCrossoverHz;
        auto full = profile(slot); full.mode = SaturationMode::fullBand;
        CHECK(dry.candidates[slot].setSettings(s)); CHECK(dry.candidates[slot].prepare(rate, diagnosticFactor));
        CHECK(neutral.candidates[slot].setSettings(matched)); CHECK(neutral.candidates[slot].prepare(rate, diagnosticFactor));
        CHECK(retained.candidates[slot].setSettings(full)); CHECK(retained.candidates[slot].prepare(rate, diagnosticFactor));
        CHECK(unchanged.candidates[slot].setSettings(full)); CHECK(unchanged.candidates[slot].prepare(rate, diagnosticFactor));
    }
    std::vector<Frame> engaged, matched, edited, control;
    for (std::size_t n = 0; n < source.size(); ++n) {
        if (n == chargedPrefix) for (unsigned slot = 0; slot < 8; ++slot) {
            auto s = profile(slot); s.mode = SaturationMode::fullBand;
            for (unsigned lane = 1; lane < 4; ++lane) {
                s.lanes[lane].character = SaturationCharacter::dense;
                s.lanes[lane].driveDb = 36; s.lanes[lane].mix = .125f;
            }
            CHECK(retained.candidates[slot].setSettings(s));
        }
        engaged.push_back(dry.process(source[n])); matched.push_back(neutral.process(source[n]));
        edited.push_back(retained.process(source[n])); control.push_back(unchanged.process(source[n]));
    }
    const auto aligned = compare(engaged, matched, 1e-12);
    CHECK(aligned.different == 0); CHECK(compare(engaged, source, 1e-12).different > 0);
    const auto retainedResult = compare(edited, control);
    CHECK(retainedResult.different == 0);
    dry.bypass.fill(true); std::vector<Frame> raw;
    for (auto frame : source) raw.push_back(dry.process(frame));
    CHECK(compare(raw, source).different == 0);
    std::cout << "TRANSITION_SERIAL_LAWS rate=" << rate << " slots=8 matched_mix0_max_error=" << aligned.maxError
              << " tolerance=1e-12 retained_exact_mismatches=" << retainedResult.different << " raw_bypass_exact=1\n";
}
void transitionRange() {
    Candidate c;
    CHECK(c.setSettings(transitionProfile(1)));
    CHECK(c.prepare(8000, diagnosticFactor)); CHECK(c.prepare(384000, diagnosticFactor));
    CHECK(!c.prepare(7999, diagnosticFactor)); CHECK(!c.prepare(384001, diagnosticFactor));
    CHECK(!c.prepare(768000, diagnosticFactor));
    std::cout << "TRANSITION_RANGE admitted=8000,384000 rejected=7999,384001,768000 factor=4\n";
}
int transitionMain(int argc, char** argv) {
    const bool negative = argc == 2 && std::string_view(argv[1]) == "--negative-control";
    const bool wrong = argc == 3 && std::string_view(argv[1]) == "--wrong-stream";
    if (argc != 1 && !negative && !wrong) return 2;
    if (wrong && std::string_view(argv[2]) != "offset" && std::string_view(argv[2]) != "order" && std::string_view(argv[2]) != "slot") return 2;
    std::cout << std::setprecision(17) << "Transition scalar block/serial fixtures UNADOPTED factor=4 selected_factor=null production=FORCED_BYPASS CPU=DEFERRED R4-R7=OPEN\n";
    transitionGuards(); transitionRange();
    for (const auto rate : { 44100., 48000., 96000. }) {
        const auto source = transitionInput(rate);
        for (const bool route : { false, true }) {
            const auto timeline = transitionEvents(rate, route);
            const auto expected = transitionOracle(source, timeline, rate, route);
            if (negative || wrong) {
                for (const auto control : { "offset", "order", "slot" }) {
                    if ((!route && std::string_view(control) == "slot") || (wrong && std::string_view(argv[2]) != control)) continue;
                    auto changed = timeline;
                    const bool ready = transitionControl(changed, rate, control); CHECK(ready);
                    if (!ready) continue;
                    const auto result = compare(expected, transitionBlocked(source, changed, rate, route, 64), 1e-12);
                    if (wrong) CHECK(result.different == 0);
                    else CHECK(result.different > 0 && result.maxError > 1e-12);
                    std::cout << "TRANSITION_NEGATIVE_CONTROL rate=" << rate << " slots=" << (route ? 8 : 1)
                              << " control=" << control << " mismatches=" << result.different << " max_error=" << result.maxError
                              << " product_RED=0\n";
                }
            } else {
                CHECK(compare(expected, transitionOracle(source, timeline, rate, route, true)).different == 0);
                for (const auto partition : { 1u, 17u, 64u, 127u, 512u, 0u }) {
                    const auto result = compare(expected, transitionBlocked(source, timeline, rate, route, partition));
                    CHECK(result.different == 0);
                    std::cout << "TRANSITION_CONTRACT rate=" << rate << " slots=" << (route ? 8 : 1)
                              << " partition=" << partition << " frames=" << source.size() << " fade=" << fadeFrames(rate)
                              << " events=" << timeline.size() << " exact_mismatches=" << result.different << " max_error=" << result.maxError << '\n';
                }
            }
        }
        if (!negative && !wrong) transitionSerialLaws(rate, source);
    }
    std::cout << "LIMITS scalar_black_box_schedule_oracle fixture_bypass_freezes_history no_production_block_API no_worker_host_CPU_or_higher_factor_analytic_claim\n"
              << "checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
}
#endif

int main(int argc, char** argv) {
#if defined(AIRACK_SATURATION_TRANSITION_CONTRACTS)
    return transitionMain(argc, argv);
#else
    std::cout << std::setprecision(17) << "Task20 probe-only Candidate contracts factor=4 UNADOPTED selected_factor=null production=FORCED_BYPASS CPU=DEFERRED\n";
    if (argc != 1 && !(argc == 2 && std::string_view(argv[1]) == "--negative-control")
        && !(argc == 3 && std::string_view(argv[1]) == "--export-reference")) {
        std::cerr << "Usage: RackSynthSaturationContractTests [--negative-control | --export-reference DIRECTORY]\n";
        return 2;
    }
    const auto source = input();
    const auto timeline = events();
    negativeControlGuards();
    if (argc == 2) negativeControls(source, timeline);
    else {
        const auto expected = sampleOracle(source, timeline);
        for (const auto partition : partitions) {
            const auto result = compare(expected, blocked(source, timeline, partition));
            CHECK(result.different == 0);
            std::cout << "EVENT partition=" << partition << " frames=" << frames << " events=" << timeline.size()
                      << " exact_mismatches=" << result.different << " max_error=" << result.maxError << '\n';
        }
        const auto serialExpected = serialContracts(source);
        if (argc == 3) exportReference(argv[2], source, timeline, expected, serialExpected);
    }
    std::cout << "OPEN graph_effective_768kHz structural_20ms_fades_retargeting production_block_API audition_cancellation callback_worker_heap real_routing host CPU_adoption\n"
              << "checks=" << checks << " failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
#endif
}

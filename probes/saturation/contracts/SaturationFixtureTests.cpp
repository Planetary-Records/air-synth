#include "SaturationCandidate.h"
#if RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS
#include "SaturationNumericalWitness.h"
#endif
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace airack;
using airack::probe::Candidate;
using Frame = Candidate::Frame;
using Row = std::vector<std::string>;
using Table = std::vector<Row>;
namespace fs = std::filesystem;
constexpr std::size_t frameCount = 2048, audioBytes = frameCount * 2 * 8;
constexpr std::array<std::size_t, 5> partitions { 1, 17, 64, 127, 512 };
// External trust anchor: a directory cannot bless changed expected data with its own hashes.
constexpr auto manifestPin = "e0d1592a45083191d3e3d14f2548ec4b032c5fed0e0109d14969859917953599";
int checks = 0;
void require(bool ok, const std::string& reason) {
    ++checks;
    if (!ok) throw std::runtime_error(reason);
}
std::string digest(const std::string& bytes) { return juce::SHA256(bytes.data(), bytes.size()).toHexString().toStdString(); }
std::string readFile(const fs::path& path, std::size_t limit) {
    require(fs::is_regular_file(fs::symlink_status(path)), "file: missing/nonregular " + path.filename().string());
    const auto size = fs::file_size(path);
    require(size <= limit, "file: oversized " + path.filename().string());
    std::string result(static_cast<std::size_t>(size), '\0');
    std::ifstream in(path, std::ios::binary);
    in.read(result.data(), static_cast<std::streamsize>(result.size()));
    require(in.good() && in.peek() == std::char_traits<char>::eof(), "file: short/changed read " + path.filename().string());
    return result;
}
std::size_t integer(std::string_view text, std::size_t maximum, const char* field) {
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= maximum,
            std::string("parser: invalid ") + field);
    return value;
}
std::size_t frameIndex(std::string_view text) { return integer(text, frameCount - 1, "frame"); }
bool boolean(std::string_view text) { return integer(text, 1, "boolean") != 0; }
double number(std::string_view text) {
    double result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(result), "parser: nonfinite/invalid number");
    return result;
}
float control(std::string_view text) {
    const auto value = number(text);
    require(std::abs(value) <= std::numeric_limits<float>::max(), "parser: control overflow");
    return static_cast<float>(value);
}
Table csv(const std::string& bytes) {
    require(bytes.size() <= 32768 && !bytes.empty() && bytes.back() == '\n', "parser: CSV size/final newline");
    Table rows;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        const auto end = bytes.find('\n', begin);
        auto line = bytes.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        require(!line.empty() && line.find('"') == std::string::npos && line.find('\r') == std::string::npos && line.find('\0') == std::string::npos, "parser: CSV syntax");
        Row row;
        std::size_t first = 0;
        for (;;) {
            const auto comma = line.find(',', first);
            row.push_back(line.substr(first, comma == std::string::npos ? comma : comma - first));
            if (comma == std::string::npos) break;
            first = comma + 1;
        }
        rows.push_back(std::move(row));
        require(rows.size() <= 100, "parser: CSV row bound");
        begin = end + 1;
    }
    return rows;
}
Row header(Row prefix) {
    for (const auto* field : { "mode", "input_trim", "output_trim", "mix", "compensation", "low_hz", "high_hz", "pre_on", "pre_hz", "post_on", "post_hz" }) prefix.emplace_back(field);
    for (unsigned lane = 0; lane < 4; ++lane)
        for (const auto* field : { "bypass", "character", "drive", "tilt", "mix", "trim" }) prefix.push_back("lane" + std::to_string(lane) + '_' + field);
    return prefix;
}
Table settingsTable(const std::string& bytes, Row prefix, std::size_t count) {
    auto rows = csv(bytes);
    const auto expected = header(std::move(prefix));
    require(rows.size() == count + 1 && rows.front() == expected, "parser: settings header/row count");
    for (const auto& row : rows) require(row.size() == expected.size(), "parser: settings column count");
    rows.erase(rows.begin());
    return rows;
}
SaturationSettings settings(const Row& row, std::size_t first, bool refusal = false) {
    require(row.size() == first + 35, "parser: settings width");
    auto n = first;
    SaturationSettings s;
    s.mode = static_cast<SaturationMode>(integer(row[n++], 1, "mode"));
    s.inputTrimDb = control(row[n++]); s.outputTrimDb = control(row[n++]); s.mix = control(row[n++]);
    s.driveCompensation = boolean(row[n++]); s.lowCrossoverHz = control(row[n++]); s.highCrossoverHz = control(row[n++]);
    s.preLowCutEnabled = boolean(row[n++]); s.preLowCutHz = control(row[n++]);
    s.postHighCutEnabled = boolean(row[n++]); s.postHighCutHz = control(row[n++]);
    bool negativeNaN = false;
    for (unsigned i = 0; i < 4; ++i) {
        auto& lane = s.lanes[i];
        lane.bypassed = boolean(row[n++]); lane.character = static_cast<SaturationCharacter>(integer(row[n++], 2, "character"));
        lane.driveDb = control(row[n++]); lane.preTiltDb = control(row[n++]);
        // Only the pinned declared-refusal event may carry the historical lane3.mix=nan stimulus.
        if (refusal && i == 3 && row[n] == "nan") { lane.mix = 1; negativeNaN = true; ++n; }
        else lane.mix = control(row[n++]);
        lane.trimDb = control(row[n++]);
    }
    require(validateSaturation(s), "parser: settings domain");
    if (negativeNaN) s.lanes[3].mix = std::numeric_limits<float>::quiet_NaN();
    return s;
}
struct Settled { std::string file; double rate; SaturationSettings settings; };
std::vector<Settled> settledSettings(const std::string& bytes) {
    const auto rows = settingsTable(bytes, { "case", "file", "rate", "factor" }, 18);
    std::vector<Settled> result;
    constexpr std::array<int, 3> rates { 44100, 48000, 96000 };
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require(integer(row[0], 17, "case") == i, "parser: case order");
        const auto rate = rates[i / 6]; const auto mode = (i / 3) % 2, character = i % 3;
        const auto file = "settled-" + std::to_string(rate) + '-' + std::to_string(mode) + '-' + std::to_string(character) + ".f64";
        require(row[1] == file && number(row[2]) == rate && integer(row[3], 4, "factor") == 4, "parser: settled identity/rate/factor");
        auto s = settings(row, 4);
        SaturationSettings expected;
        expected.mode = static_cast<SaturationMode>(mode); expected.mix = .75f; expected.driveCompensation = false;
        for (auto& lane : expected.lanes) { lane.character = static_cast<SaturationCharacter>(character); lane.driveDb = 12; lane.mix = .75f; }
        require(s == expected, "parser: common settings contract");
        result.push_back({ file, double(rate), s });
    }
    return result;
}
struct Event { std::size_t frame; std::string operation; bool admitted; double rate; unsigned factor; SaturationSettings settings; };
std::vector<Event> eventSettings(const std::string& bytes) {
    const auto rows = settingsTable(bytes, { "frame", "operation", "admitted", "rate", "factor" }, 32);
    std::vector<Event> result;
    unsigned refusedSettings = 0, refusedPrepare = 0;
    for (const auto& row : rows) {
        Event e { frameIndex(row[0]), row[1], boolean(row[2]), number(row[3]), static_cast<unsigned>(integer(row[4], 64, "factor")), {} };
        require(result.empty() || e.frame >= result.back().frame, "parser: event frame order");
        require(e.operation == "settings" || e.operation == "prepare" || e.operation == "reset", "parser: event operation");
        e.settings = settings(row, 5, e.operation == "settings" && !e.admitted);
        const bool validPrepare = e.rate >= 8000 && e.rate <= 384000 && (e.factor == 1 || e.factor == 2 || e.factor == 4 || e.factor == 8 || e.factor == 64);
        const bool admitted = e.operation == "settings" ? validateSaturation(e.settings) : e.operation == "prepare" ? validPrepare : true;
        require(e.admitted == admitted, "parser: declared admission mismatch");
        if (!admitted) { if (e.operation == "settings") ++refusedSettings; else ++refusedPrepare; }
        result.push_back(e);
    }
    require(refusedSettings == 1 && refusedPrepare == 4, "parser: missing negative characterization records");
    return result;
}
struct SerialSetting { std::size_t frame, slot; SaturationSettings settings; };
std::vector<SerialSetting> serialSettings(const std::string& bytes) {
    const auto rows = settingsTable(bytes, { "frame", "slot" }, 11);
    std::vector<SerialSetting> result;
    for (const auto& row : rows) {
        SerialSetting s { frameIndex(row[0]), integer(row[1], 7, "slot"), settings(row, 2) };
        require(result.empty() || s.frame >= result.back().frame, "parser: serial settings order");
        if (result.size() < 8) require(s.frame == 0 && s.slot == result.size(), "parser: initial serial slot coverage");
        result.push_back(s);
    }
    return result;
}
struct Route { std::size_t frame, slot; std::string operation; bool bypassed; };
std::vector<Route> serialEvents(const std::string& bytes, const std::vector<SerialSetting>& settings) {
    auto rows = csv(bytes);
    require(rows.size() == 10 && rows.front() == Row{ "frame", "operation", "slot", "bypassed" }, "parser: serial event header/row count");
    std::vector<Route> result;
    std::size_t setting = 8;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        require(row.size() == 4, "parser: serial event columns");
        Route e { frameIndex(row[0]), row[2] == "all" ? 8 : integer(row[2], 7, "slot"), row[1], false };
        require(result.empty() || e.frame >= result.back().frame, "parser: serial event order");
        if (e.operation == "bypass") e.bypassed = boolean(row[3]);
        else {
            require(row[3].empty(), "parser: unexpected bypass field");
            if (e.operation == "reset") require(e.slot == 8, "parser: reset must address all slots");
            else {
                require(e.operation == "settings" && e.slot < 8 && setting < settings.size(), "parser: settings event slot/count");
                require(e.frame == settings[setting].frame && e.slot == settings[setting].slot, "parser: settings event binding");
                ++setting;
            }
        }
        result.push_back(e);
    }
    require(setting == settings.size(), "parser: unused serial settings");
    return result;
}
std::vector<Frame> samples(const std::string& bytes) {
    require(bytes.size() == audioBytes, "parser: audio frame/channel/size");
    std::vector<Frame> result(frameCount);
    for (std::size_t n = 0; n < frameCount; ++n)
        for (unsigned channel = 0; channel < 2; ++channel) {
            std::uint64_t bits = 0;
            for (unsigned byte = 0; byte < 8; ++byte) bits |= std::uint64_t(static_cast<unsigned char>(bytes[(n * 2 + channel) * 8 + byte])) << (8 * byte);
            const auto value = std::bit_cast<double>(bits);
            require(std::isfinite(value), "parser: nonfinite audio");
            result[n][channel] = value;
        }
    return result;
}
std::set<std::string> payloadNames() {
    std::set<std::string> names { "input.f64", "event-expected.f64", "serial-expected.f64", "events-settings.csv", "serial-settings.csv", "serial-events.csv", "settled-settings.csv", "producer-receipt.json" };
    for (int rate : { 44100, 48000, 96000 }) for (int mode = 0; mode < 2; ++mode) for (int character = 0; character < 3; ++character)
        names.insert("settled-" + std::to_string(rate) + '-' + std::to_string(mode) + '-' + std::to_string(character) + ".f64");
    return names;
}
juce::var json(const std::string& bytes) {
    auto value = juce::JSON::parse(juce::String::fromUTF8(bytes.data(), static_cast<int>(bytes.size())));
    require(value.isObject(), "parser: invalid JSON object");
    return value;
}
bool textIs(const juce::var& value, const char* expected) { return value.isString() && value.toString() == expected; }
bool intIs(const juce::var& value, int expected) { return value.isInt() && int(value) == expected; }
void validateManifest(const juce::var& value) {
    require(value.isObject() && intIs(value["schema"], 1) && textIs(value["status"], "UNADOPTED") && textIs(value["CPU"], "DEFERRED")
        && value.getDynamicObject()->hasProperty("selected_factor") && value["selected_factor"].isVoid()
        && textIs(value["production"], "FORCED_BYPASS") && textIs(value["gates"], "UNCHANGED") && intIs(value["probe_factor"], 4), "parser: manifest status/factor");
    require(intIs(value["frames"], 2048) && intIs(value["channels"], 2) && intIs(value["common_cases"], 18)
        && textIs(value["format"], "IEEE754_f64_little_endian_interleaved_stereo"), "parser: manifest audio metadata");
    const auto* chunks = value["partitions"].getArray();
    require(chunks && chunks->size() == 5, "parser: partition metadata");
    for (int i = 0; i < 5; ++i) require(intIs((*chunks)[i], static_cast<int>(partitions[static_cast<std::size_t>(i)])), "parser: partition identity");
    require(textIs(value["initial_sequence"], "fresh Candidate; setSettings; prepare(rate,4); reset; process saved input; no edits or warmup")
        && textIs(value["reset"], "zero audio histories and settled admitted targets"), "parser: initialization metadata");
}
struct Corpus {
    std::vector<Frame> input, eventExpected, serialExpected;
    std::vector<Settled> settled;
    std::vector<std::vector<Frame>> settledExpected;
    std::vector<Event> events;
    std::vector<SerialSetting> settings;
    std::vector<Route> routes;
};
Corpus load(const fs::path& directory) {
    const auto manifestBytes = readFile(directory / "manifest.json", 32768);
    require(digest(manifestBytes) == manifestPin, "hash: manifest pin mismatch");
    const auto manifest = json(manifestBytes);
    validateManifest(manifest);
    auto names = payloadNames();
    auto members = names; members.insert("manifest.json");
    std::set<std::string> actual;
    for (const auto& entry : fs::directory_iterator(directory)) {
        require(fs::is_regular_file(entry.symlink_status()), "file: nonregular corpus member");
        actual.insert(entry.path().filename().string());
    }
    require(actual == members, "membership: missing/unexpected corpus file");
    const auto files = manifest["files"];
    require(files.isObject() && files.getDynamicObject()->getProperties().size() == static_cast<int>(names.size()), "parser: manifest file count");
    for (const auto& name : names) {
        const auto entry = files[juce::Identifier(name)];
        require(entry.isObject() && entry["bytes"].isInt() && int(entry["bytes"]) > 0 && int(entry["bytes"]) <= 32768 && entry["sha256"].isString(), "parser: manifest file metadata");
        const auto bytes = readFile(directory / name, 32768);
        require(bytes.size() == static_cast<std::size_t>(int(entry["bytes"])) && digest(bytes) == entry["sha256"].toString().toStdString(), "hash: payload mismatch " + name);
    }
    const auto receipt = json(readFile(directory / "producer-receipt.json", 32768));
    require(textIs(receipt["status"], "UNADOPTED") && textIs(receipt["CPU"], "DEFERRED") && intIs(receipt["probe_factor"], 4)
        && receipt["image_sha256"] == manifest["producer_image_sha256"] && intIs(receipt["export"]["exit"], 0)
        && receipt["source_inputs"].isObject() && receipt["commands"].isObject() && receipt["link_inputs"].isObject(), "parser: producer binding");
    Corpus c;
    c.input = samples(readFile(directory / "input.f64", audioBytes));
    c.eventExpected = samples(readFile(directory / "event-expected.f64", audioBytes));
    c.serialExpected = samples(readFile(directory / "serial-expected.f64", audioBytes));
    c.settled = settledSettings(readFile(directory / "settled-settings.csv", 32768));
    for (const auto& s : c.settled) c.settledExpected.push_back(samples(readFile(directory / s.file, audioBytes)));
    c.events = eventSettings(readFile(directory / "events-settings.csv", 32768));
    c.settings = serialSettings(readFile(directory / "serial-settings.csv", 32768));
    c.routes = serialEvents(readFile(directory / "serial-events.csv", 32768), c.settings);
    std::cout << "VALIDATED pinned_manifest complete_membership=27 payload_files=26 frames=2048 channels=2 before_processing=1\n";
    return c;
}
#if RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS
Table quotientRows { { "frame", "order", "record", "site", "lane", "channel", "character", "kind", "previous_kind",
    "u", "p", "q", "current_primitive", "prior_primitive", "cached_primitive", "old_q", "branch" } };
std::string hexadecimal(double value) {
    std::ostringstream out; out << std::hexfloat << value; return out.str();
}
Frame quotientFrame(Candidate& candidate, Frame input, std::size_t frame) {
    const auto before = airack::probe::saturationNumericalRecords().size();
    const auto output = candidate.process(input);
    const auto records = airack::probe::saturationNumericalRecords();
    require(records.size() >= before, "quotients: collector shrank");
    for (auto i = before; i < records.size(); ++i) {
        const auto& r = records[i];
        std::ostringstream lane; lane << "0x" << std::hex << r.lane;
        // Witness only: the actual output above always uses the unchanged Candidate q.
        const double old = r.kind == 1 ? (r.currentPrimitive - r.priorPrimitive) / (r.u - r.p) : r.q;
        quotientRows.push_back({ std::to_string(frame), std::to_string(i - before), std::to_string(i), std::to_string(r.site),
            lane.str(), std::to_string(r.channel), std::to_string(r.character), std::to_string(r.kind), std::to_string(r.previousKind),
            hexadecimal(r.u), hexadecimal(r.p), hexadecimal(r.q), hexadecimal(r.currentPrimitive), hexadecimal(r.priorPrimitive),
            hexadecimal(r.cachedPrimitive), hexadecimal(old), r.kind == 1 ? "exact_primitive" : r.kind == 2 ? "unchanged_curve" : "unchanged_midpoint" });
    }
    return output;
}
#endif
std::vector<Frame> runSettled(const Corpus& c, const Settled& s, std::size_t partition) {
    Candidate candidate;
    require(candidate.setSettings(s.settings) && candidate.prepare(s.rate, 4), "replay: settled admission");
    candidate.reset();
    std::vector<Frame> output; output.reserve(frameCount);
    for (std::size_t begin = 0; begin < frameCount; begin += partition)
        for (auto n = begin; n < std::min(begin + partition, frameCount); ++n)
#if RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS
            output.push_back(quotientFrame(candidate, c.input[n], n));
#else
            output.push_back(candidate.process(c.input[n]));
#endif
    return output;
}
std::vector<Frame> runEvents(const Corpus& c, std::size_t partition) {
    Candidate candidate;
    require(candidate.prepare(48000, 4), "replay: event initial prepare");
    std::size_t next = 0; unsigned refusedSettings = 0, refusedPrepare = 0;
    std::vector<Frame> output; output.reserve(frameCount);
    for (std::size_t begin = 0; begin < frameCount; begin += partition)
        for (auto n = begin; n < std::min(begin + partition, frameCount); ++n) {
            while (next < c.events.size() && c.events[next].frame == n) {
                const auto& e = c.events[next++];
                bool admitted = true;
                if (e.operation == "settings") { admitted = candidate.setSettings(e.settings); if (!admitted) ++refusedSettings; }
                else if (e.operation == "prepare") { admitted = candidate.prepare(e.rate, e.factor); if (!admitted) ++refusedPrepare; }
                else candidate.reset();
                require(admitted == e.admitted, "replay: actual admission differs from declared event");
            }
            output.push_back(candidate.process(c.input[n]));
        }
    require(next == c.events.size() && refusedSettings == 1 && refusedPrepare == 4, "replay: negative records not observed");
    std::cout << "OBSERVED partition=" << partition << " settings_nan_refusals=" << refusedSettings << " prepare_refusals=" << refusedPrepare << '\n';
    return output;
}
std::vector<Frame> runSerial(const Corpus& c, std::size_t partition) {
    std::array<Candidate, 8> bank;
    std::array<bool, 8> bypass {};
    for (std::size_t slot = 0; slot < bank.size(); ++slot)
        require(bank[slot].prepare(48000, 4) && bank[slot].setSettings(c.settings[slot].settings), "replay: serial initial admission");
    std::size_t next = 0, setting = 8;
    std::vector<Frame> output; output.reserve(frameCount);
    for (std::size_t begin = 0; begin < frameCount; begin += partition)
        for (auto n = begin; n < std::min(begin + partition, frameCount); ++n) {
            while (next < c.routes.size() && c.routes[next].frame == n) {
                const auto& e = c.routes[next++];
                if (e.operation == "settings") require(bank[e.slot].setSettings(c.settings[setting++].settings), "replay: serial edit admission");
                else if (e.operation == "reset") for (auto& candidate : bank) candidate.reset();
                else if (e.slot == 8) bypass.fill(e.bypassed);
                else bypass[e.slot] = e.bypassed;
            }
            auto frame = c.input[n];
            for (std::size_t slot = 0; slot < bank.size(); ++slot) if (!bypass[slot]) frame = bank[slot].process(frame);
            output.push_back(frame);
        }
    require(next == c.routes.size() && setting == c.settings.size(), "replay: serial event consumption");
    return output;
}
#if RACKSYNTH_SATURATION_FIXTURE_DIAGNOSIS || RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS
fs::path diagnosisOutput;
Table diagnosisRows;
std::size_t diagnosisPassed = 0;
void writeFile(const fs::path& path, const std::string& bytes);
std::string bitsText(std::uint64_t bits) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << bits;
    return out.str();
}
#endif
void compare(const std::vector<Frame>& actual, const std::vector<Frame>& expected, const std::string& name, std::size_t partition) {
    require(actual.size() == expected.size() && actual.size() == frameCount, "comparison: frame count");
    double maximum = 0; std::size_t bitDifferences = 0;
#if RACKSYNTH_SATURATION_FIXTURE_DIAGNOSIS || RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS
    Row first(4);
    std::string recording;
    recording.reserve(audioBytes);
#endif
    for (std::size_t n = 0; n < actual.size(); ++n) for (unsigned channel = 0; channel < 2; ++channel) {
        const auto a = actual[n][channel], b = expected[n][channel];
        require(std::isfinite(a) && std::isfinite(b), "comparison: nonfinite sample");
        maximum = std::max(maximum, std::abs(a - b));
#if RACKSYNTH_SATURATION_FIXTURE_DIAGNOSIS || RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS
        const auto actualBits = std::bit_cast<std::uint64_t>(a), expectedBits = std::bit_cast<std::uint64_t>(b);
        if (actualBits != expectedBits && bitDifferences == 0)
            first = { std::to_string(n), std::to_string(channel), bitsText(actualBits), bitsText(expectedBits) };
        for (unsigned byte = 0; byte < 8; ++byte) recording += static_cast<char>((actualBits >> (8 * byte)) & 255);
#endif
        bitDifferences += std::bit_cast<std::uint64_t>(a) != std::bit_cast<std::uint64_t>(b);
    }
    std::cout << "COMPARE case=" << name << " partition=" << partition << " max_absolute=" << maximum << " bit_differences=" << bitDifferences << " tolerance=1e-12\n";
#if RACKSYNTH_SATURATION_FIXTURE_DIAGNOSIS || RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS
    const auto file = name + ".partition-" + std::to_string(partition) + ".actual.f64";
    writeFile(diagnosisOutput / file, recording);
    std::ostringstream maxText; maxText << std::setprecision(17) << maximum;
    const auto expectedFile = name == "event-characterization" ? "event-expected.f64"
        : name == "serial-characterization" ? "serial-expected.f64" : name;
    const bool passed = maximum <= 1e-12 && bitDifferences == 0;
    diagnosisPassed += passed;
    Row row { name, std::to_string(partition), expectedFile, file, maxText.str(), std::to_string(bitDifferences), passed ? "true" : "false" };
    row.insert(row.end(), first.begin(), first.end());
    diagnosisRows.push_back(std::move(row));
#else
    require(maximum <= 1e-12 && bitDifferences == 0, "comparison: saved expected output mismatch " + name);
#endif
}
void replay(const Corpus& c) {
    for (auto partition : partitions) {
        for (std::size_t i = 0; i < c.settled.size(); ++i) compare(runSettled(c, c.settled[i], partition), c.settledExpected[i], c.settled[i].file, partition);
        compare(runEvents(c, partition), c.eventExpected, "event-characterization", partition);
        compare(runSerial(c, partition), c.serialExpected, "serial-characterization", partition);
    }
}
std::string csvBytes(const Table& rows) {
    std::string bytes;
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size(); ++i) { if (i) bytes += ','; bytes += row[i]; }
        bytes += '\n';
    }
    return bytes;
}
void writeFile(const fs::path& path, const std::string& bytes) {
    require(!fs::exists(path), "test: refuse overwriting negative evidence");
    std::ofstream out(path, std::ios::binary); out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    require(out.good(), "test: negative evidence write failed");
}
template<class F> void refused(const std::string& name, const char* stage, F operation) {
    try { operation(); }
    catch (const std::runtime_error& e) {
        require(std::string(e.what()).starts_with(std::string(stage) + ':'), "test: refusal at wrong stage " + name);
        std::cout << "EXPECTED_REFUSAL name=" << name << " stage=" << stage << " reason=" << e.what() << '\n';
        return;
    }
    throw std::runtime_error("test: corruption accepted " + name);
}
void semanticTests(const fs::path& directory, const Corpus& c, const fs::path& evidence) {
    require(!fs::exists(evidence), "test: use a fresh semantic evidence directory");
    fs::create_directories(evidence);
    auto exercise = [&](const char* name, const char* file, auto edit, auto parse) {
        auto rows = csv(readFile(directory / file, 32768)); edit(rows);
        const auto path = evidence / (std::string(name) + ".csv"); writeFile(path, csvBytes(rows));
        refused(name, "parser", [&] { parse(readFile(path, 32768)); });
    };
    const auto common = [](const auto& text) { (void)settledSettings(text); };
    exercise("settings_header", "settled-settings.csv", [](auto& rows) { rows[0][4] = "wrong_mode"; }, common);
    exercise("settings_columns", "settled-settings.csv", [](auto& rows) { rows[1].pop_back(); }, common);
    exercise("settings_number", "settled-settings.csv", [](auto& rows) { rows[1][7] = ".75junk"; }, common);
    exercise("settings_nonfinite", "settled-settings.csv", [](auto& rows) { rows[1][7] = "nan"; }, common);
    exercise("settings_enum", "settled-settings.csv", [](auto& rows) { rows[1][4] = "2"; }, common);
    exercise("settings_boolean", "settled-settings.csv", [](auto& rows) { rows[1][8] = "2"; }, common);
    exercise("settings_domain", "settled-settings.csv", [](auto& rows) { rows[1][17] = "37"; }, common);
    exercise("settings_case", "settled-settings.csv", [](auto& rows) { rows[1][0] = "18"; }, common);
    const auto events = [](const auto& text) { (void)eventSettings(text); };
    exercise("frame_bound", "events-settings.csv", [](auto& rows) { rows[1][0] = "2048"; }, events);
    exercise("frame_order", "events-settings.csv", [](auto& rows) { rows[1][0] = "2"; }, events);
    exercise("admitted_nonfinite", "events-settings.csv", [](auto& rows) { rows[1][8] = "nan"; }, events);
    exercise("negative_declared_admitted", "events-settings.csv", [](auto& rows) { for (auto& row : rows) if (row.size() == 40 && row[38] == "nan") row[2] = "1"; }, events);
    exercise("prepare_admission", "events-settings.csv", [](auto& rows) { for (auto& row : rows) if (row[1] == "prepare" && row[4] == "3") row[2] = "1"; }, events);
    exercise("serial_slot", "serial-settings.csv", [](auto& rows) { rows[1][1] = "8"; }, [](const auto& text) { (void)serialSettings(text); });
    exercise("route_slot", "serial-events.csv", [](auto& rows) { rows[1][2] = "8"; }, [&](const auto& text) { (void)serialEvents(text, c.settings); });
    exercise("route_order", "serial-events.csv", [](auto& rows) { rows[1][0] = "66"; }, [&](const auto& text) { (void)serialEvents(text, c.settings); });
    auto manifest = json(readFile(directory / "manifest.json", 32768));
    manifest.getDynamicObject()->setProperty("status", "ADOPTED");
    writeFile(evidence / "status-manifest.json", juce::JSON::toString(manifest).toStdString());
    refused("manifest_status", "parser", [&] { validateManifest(json(readFile(evidence / "status-manifest.json", 32768))); });
    auto audio = readFile(directory / c.settled[0].file, audioBytes);
    const auto nan = std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN());
    for (unsigned byte = 0; byte < 8; ++byte) audio[byte] = static_cast<char>((nan >> (8 * byte)) & 255);
    writeFile(evidence / "nonfinite.f64", audio);
    refused("nonfinite_audio", "parser", [&] { (void)samples(readFile(evidence / "nonfinite.f64", audioBytes)); });
    audio = readFile(directory / c.settled[0].file, audioBytes);
    const auto changed = std::bit_cast<std::uint64_t>(c.settledExpected[0][0][0] + 1e-6);
    for (unsigned byte = 0; byte < 8; ++byte) audio[byte] = static_cast<char>((changed >> (8 * byte)) & 255);
    writeFile(evidence / "changed-expected.f64", audio);
    refused("changed_saved_expected", "comparison", [&] { compare(runSettled(c, c.settled[0], 64), samples(readFile(evidence / "changed-expected.f64", audioBytes)), "changed-saved-file", 64); });
    refused("frame_2048_TDD_regression", "parser", [] { (void)frameIndex("2048"); });
    require(frameIndex("2047") == 2047, "test: last frame must be accepted");
    std::cout << "SEMANTIC parser_tests=19 comparator_tests=1 hash_gate_bypassed_only_for_direct_stage_tests=1 expected_data_regenerated=0\n";
}
}
int main(int argc, char** argv) {
#if RACKSYNTH_SATURATION_FIXTURE_QUOTIENTS
    std::cout << std::setprecision(17) << "First fixture quotient diagnostic only CPU=DEFERRED selected_factor=null adoptable=false production=FORCED_BYPASS\n";
    try {
        require(argc == 3 && std::string_view(argv[1]) == "--output", "usage: --output FRESH_ABSOLUTE_DIRECTORY");
        diagnosisOutput = argv[2];
        require(diagnosisOutput.is_absolute() && !fs::exists(fs::symlink_status(diagnosisOutput)), "output: requires a fresh absolute directory");
        const fs::path corpus = RACKSYNTH_SATURATION_FIXTURE_DIR;
        const auto c = load(corpus);
        require(airack::probe::saturationNumericalRecords().empty(), "quotients: unexpected pre-replay records");
        require(fs::create_directories(diagnosisOutput), "output: could not create directory");
        compare(runSettled(c, c.settled[0], 1), c.settledExpected[0], c.settled[0].file, 1);
        require(diagnosisRows.size() == 1 && quotientRows.size() > 1, "quotients: incomplete first-case replay");
        diagnosisRows.insert(diagnosisRows.begin(), { "case", "partition", "expected_file", "actual_file", "max_absolute", "bit_differences", "pass",
            "first_mismatch_frame", "first_mismatch_channel", "actual_bits", "expected_bits" });
        writeFile(diagnosisOutput / "comparisons.csv", csvBytes(diagnosisRows));
        const auto records = csvBytes(quotientRows);
        writeFile(diagnosisOutput / "q-records.csv", records);
        const auto actual = readFile(diagnosisOutput / (c.settled[0].file + ".partition-1.actual.f64"), audioBytes);
        writeFile(diagnosisOutput / "quotients.json", "{\n  \"schema\": \"saturation-fixture-quotients-v1\",\n  \"case\": \"" + c.settled[0].file
            + "\",\n  \"partition\": 1,\n  \"frames\": 2048,\n  \"channels\": 2,\n  \"records\": " + std::to_string(quotientRows.size() - 1)
            + ",\n  \"manifest_sha256\": \"" + manifestPin + "\",\n  \"input_sha256\": \"" + digest(readFile(corpus / "input.f64", audioBytes))
            + "\",\n  \"settings_sha256\": \"" + digest(readFile(corpus / "settled-settings.csv", 32768))
            + "\",\n  \"actual_sha256\": \"" + digest(actual) + "\",\n  \"records_sha256\": \"" + digest(records)
            + "\",\n  \"status\": \"diagnostic_only\",\n  \"selected_factor\": null,\n  \"adoptable\": false,\n  \"production\": \"FORCED_BYPASS\",\n  \"cpu_validation\": \"DEFERRED\"\n}\n");
        std::cout << "QUOTIENTS frames=2048 channels=2 records=" << quotientRows.size() - 1 << " saved_expected_read_only=1 fixture_pass=" << diagnosisPassed << '\n';
        return diagnosisPassed ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "REFUSED " << e.what() << " checks=" << checks << " incomplete_evidence=1\n";
        return 2;
    }
#elif RACKSYNTH_SATURATION_FIXTURE_DIAGNOSIS
    std::cout << std::setprecision(17) << "Fixture diagnostic only CPU=DEFERRED selected_factor=null adoptable=false production=FORCED_BYPASS\n";
    try {
        require(argc == 3 && std::string_view(argv[1]) == "--output", "usage: --output FRESH_ABSOLUTE_DIRECTORY");
        diagnosisOutput = argv[2];
        require(diagnosisOutput.is_absolute() && !fs::exists(fs::symlink_status(diagnosisOutput)), "output: requires a fresh absolute directory");
        const auto c = load(RACKSYNTH_SATURATION_FIXTURE_DIR);
        require(fs::create_directories(diagnosisOutput), "output: could not create directory");
        replay(c);
        require(diagnosisRows.size() == 100, "diagnosis: incomplete comparisons");
        diagnosisRows.insert(diagnosisRows.begin(), { "case", "partition", "expected_file", "actual_file", "max_absolute", "bit_differences", "pass",
            "first_mismatch_frame", "first_mismatch_channel", "actual_bits", "expected_bits" });
        writeFile(diagnosisOutput / "comparisons.csv", csvBytes(diagnosisRows));
        const auto failed = 100 - diagnosisPassed;
        writeFile(diagnosisOutput / "diagnosis.json", "{\n  \"status\": \"" + std::string(failed ? "fixture_mismatch" : "fixture_match")
            + "\",\n  \"scope\": \"diagnostic_only\",\n  \"selected_factor\": null,\n  \"adoptable\": false,\n  \"production\": \"FORCED_BYPASS\",\n  \"cpu_validation\": \"DEFERRED\",\n"
              "  \"comparisons\": 100,\n  \"frames\": 2048,\n  \"channels\": 2,\n  \"passed\": " + std::to_string(diagnosisPassed)
            + ",\n  \"failed\": " + std::to_string(failed) + "\n}\n");
        std::cout << "DIAGNOSIS comparisons=100 passed=" << diagnosisPassed << " failed=" << failed << " saved_expected_read_only=1 product_acceptance=0\n";
        return failed ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "REFUSED " << e.what() << " checks=" << checks << " incomplete_evidence=1\n";
        return 2;
    }
#else
    std::cout << std::setprecision(17) << "Task23 independent unlinked fixture consumer UNADOPTED factor=4 CPU=DEFERRED selected_factor=null production=FORCED_BYPASS gates=UNCHANGED R4-R7=OPEN\n";
    try {
        fs::path directory = RACKSYNTH_SATURATION_FIXTURE_DIR;
        bool semantics = false; fs::path evidence;
        if (argc == 3 && std::string_view(argv[1]) == "--corpus") directory = argv[2];
        else if (argc == 3 && std::string_view(argv[1]) == "--semantic-tests") { semantics = true; evidence = argv[2]; }
        else require(argc == 1, "usage: [--corpus DIRECTORY | --semantic-tests FRESH_DIRECTORY]");
        const auto c = load(directory); // Validate every file/settings/admission before any processing.
        if (semantics) semanticTests(directory, c, evidence);
        else replay(c);
        std::cout << "checks=" << checks << " failures=0 saved_expected_read_only=1 product_acceptance=0\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "REFUSED " << e.what() << " checks=" << checks << " failures=1\n";
        return 1;
    }
#endif
}

#include "SaturationHistoryCases.h"
#if defined(RACKSYNTH_HISTORY_TRACE)
#include "SaturationHistoryTrace.h"
#endif
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace airack::probe::history;
using airack::probe::Candidate;
namespace fs = std::filesystem;
#if defined(RACKSYNTH_HISTORY_TRACE)
constexpr auto action = "return-phase2-readonly-trace";
#elif defined(RACKSYNTH_HISTORY_RETURN_REPLAY)
constexpr auto action = "return-phase2-unobserved-replay";
#else
constexpr auto action = "hold-phase1-omit-phase2";
#endif
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
unsigned number(std::string_view text) {
    unsigned result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc() && parsed.ptr == text.data() + text.size(), "invalid integer");
    return result;
}
Case selectedCase(const std::string& id) {
    for (bool modes : { false, true }) for (const auto& c : cases(modes)) if (c.id == id) return c;
    throw std::runtime_error("unknown case");
}
struct Options { unsigned factor, rate; Case c; fs::path input, output; };
Options parse(const std::vector<std::string>& args) {
    require(args.size() == 10 && args[0] == "--factor" && args[2] == "--rate" && args[4] == "--case"
            && args[6] == "--input" && args[8] == "--output", "expected factor/rate/case/retained-input/fresh-output");
    const auto factor = number(args[1]), rate = number(args[3]);
    require((factor == 4 || factor == 64) && (rate == 44100 || rate == 48000 || rate == 96000), "unsupported cohort");
    Options result { factor, rate, selectedCase(args[5]), fs::path(args[7]), fs::path(args[9]) };
    require(result.input.is_absolute() && result.output.is_absolute(), "input/output must be absolute");
    return result;
}
std::vector<char> read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(bool(stream), "missing retained file");
    const auto length = stream.tellg(); require(length >= 0, "input extent unavailable");
    std::vector<char> result(static_cast<std::size_t>(length)); stream.seekg(0);
    stream.read(result.data(), static_cast<std::streamsize>(result.size()));
    require(bool(stream), "input read failed"); return result;
}
juce::var json(const std::vector<char>& bytes) {
    juce::var result;
    require(juce::JSON::parse(juce::String::fromUTF8(bytes.data(), int(bytes.size())), result).wasOk()
            && result.isObject(), "invalid retained JSON");
    return result;
}
std::string text(const juce::var& value) { return value.toString().toStdString(); }
void bound(const juce::var& binding, const std::string& name, const std::vector<char>& bytes) {
    const auto entry = binding["files"][juce::Identifier(name)];
    require(entry.isObject() && static_cast<juce::int64>(entry["bytes"]) == juce::int64(bytes.size())
            && text(entry["sha256"]) == juce::SHA256(bytes.data(), bytes.size()).toHexString().toStdString(),
            "retained hash/extent binding");
}
std::vector<Candidate::Frame> frames(const std::vector<char>& bytes, unsigned total) {
    static_assert(std::endian::native == std::endian::little && sizeof(Candidate::Frame) == 16);
    require(bytes.size() == std::size_t(total) * sizeof(Candidate::Frame), "retained input extent");
    std::vector<Candidate::Frame> result(total);
    std::memcpy(result.data(), bytes.data(), bytes.size());
    for (const auto& frame : result) require(std::isfinite(frame[0]) && std::isfinite(frame[1]), "nonfinite input");
    return result;
}
unsigned start(const Options& o) { return prefixFrames + unsigned(commonSeconds * o.rate); }
unsigned fade(const Options& o) { return unsigned(std::ceil(.020 * o.rate)); }
std::vector<std::vector<Candidate::Frame>> inputs(Options& o) {
    require(fs::is_directory(o.input) && !fs::exists(o.output), "missing input or existing output");
    o.input = fs::weakly_canonical(o.input); o.output = fs::weakly_canonical(o.output);
    const auto cohort = o.input.parent_path();
    for (auto ancestor = o.output; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        // Filesystem identity preserves Windows case and directory-alias equivalence.
        if (fs::exists(ancestor)) require(!fs::equivalent(ancestor, cohort), "output must be outside retained cohort");
        if (ancestor == ancestor.root_path()) break;
    }
    const auto manifestBytes = read(o.input / "capture.json");
    const auto manifest = json(manifestBytes), binding = json(read(o.input.parent_path() / "binding.json"));
    require(int(manifest["schema"]) == 1 && text(manifest["role"]) == "actual"
            && unsigned(int(manifest["factor"])) == o.factor && unsigned(int(manifest["rate"])) == o.rate
            && text(manifest["family"]) == (o.c.modes ? "modes" : "cuts")
            && unsigned(int(manifest["prefix"])) == prefixFrames
            && unsigned(int(manifest["common"])) == unsigned(commonSeconds * o.rate)
            && unsigned(int(manifest["fade"])) == fade(o) && unsigned(int(manifest["settled"])) == settledFrames,
            "retained role/cohort/timeline binding");
    require(unsigned(int(binding["factor"])) == o.factor && unsigned(int(binding["rate"])) == o.rate
            && text(binding["family"]) == (o.c.modes ? "modes" : "cuts"), "retained binding cohort");
    bool found = false;
    if (const auto* list = manifest["cases"].getArray()) for (const auto& c : *list)
        if (text(c["id"]) == o.c.id) {
            require(unsigned(int(c["mode"])) == o.c.mode && unsigned(int(c["character"])) == o.c.character
                    && text(c["family"]) == (o.c.modes ? "modes" : "cuts")
                    && (o.c.modes ? bool(c["zero"]) == o.c.zero : text(c["cut"]) == (o.c.pre ? "pre" : "post")),
                    "retained case binding");
            require(!found, "duplicate retained case"); found = true;
        }
    require(found, "case missing from retained capture");
    bound(binding, "actual/capture.json", manifestBytes);
    std::vector<std::vector<Candidate::Frame>> result;
    for (unsigned h = 0; h < (o.c.modes ? 3u : 2u); ++h) {
        const auto name = o.c.id + '.' + (h == 2 ? "fresh" : "old" + std::to_string(h)) + ".input.f64";
        const auto bytes = read(o.input / name);
        result.push_back(frames(bytes, start(o) + fade(o) + settledFrames));
        bound(binding, "actual/" + name, bytes);
    }
    return result;
}
void writeFrame(std::ostream& stream, Candidate::Frame frame) {
    require(std::isfinite(frame[0]) && std::isfinite(frame[1]), "nonfinite output");
    stream.write(reinterpret_cast<const char*>(frame.data()), sizeof(frame));
    require(bool(stream), "capture write failed");
}
struct Capture {
    std::ofstream stream;
    explicit Capture(const fs::path& path) : stream(path, std::ios::binary) {
        stream.exceptions(std::ios::badbit | std::ios::failbit);
    }
    void finish() { stream.flush(); stream.close(); require(bool(stream), "capture close failed"); }
};
void render(const Options& o, const std::vector<std::vector<Candidate::Frame>>& retained) {
    require(fs::create_directory(o.output), "output already exists");
    for (unsigned h = 0; h < retained.size(); ++h) {
        Candidate candidate;
        require(candidate.setSettings(o.c.settings(0, h)) && candidate.prepare(o.rate, o.factor), "prepare refused");
        const auto name = o.c.id + '.' + (h == 2 ? "fresh" : "old" + std::to_string(h)) + ".output.f64";
        Capture file(o.output / name);
#if defined(RACKSYNTH_HISTORY_TRACE)
        airack::probe::history_trace::Session trace(o.output / name, o.factor, o.rate, start(o), h);
#endif
        for (unsigned frame = 0; frame < retained[h].size(); ++frame) {
            if (frame == prefixFrames) require(candidate.setSettings(o.c.settings(1, h)), "source switch refused");
#if defined(RACKSYNTH_HISTORY_TRACE) || defined(RACKSYNTH_HISTORY_RETURN_REPLAY)
            if (frame == start(o)) require(candidate.setSettings(o.c.settings(2, h)), "return switch refused");
#endif
#if defined(RACKSYNTH_HISTORY_TRACE)
            const bool sourceChange = frame == prefixFrames && (o.c.modes ? h != 2 : h == 1);
            trace.begin(frame, sourceChange || frame == start(o));
#endif
            const auto result = candidate.process(retained[h][frame]);
#if defined(RACKSYNTH_HISTORY_TRACE)
            trace.end(result);
#endif
            writeFrame(file.stream, result);
        }
#if defined(RACKSYNTH_HISTORY_TRACE)
        trace.finish();
#endif
        file.finish();
    }
    Capture manifest(o.output / "capture.json");
    manifest.stream << "{\"schema\":1,\"role\":\"" <<
#if defined(RACKSYNTH_HISTORY_TRACE)
        "return-trace"
#elif defined(RACKSYNTH_HISTORY_RETURN_REPLAY)
        "return-replay"
#else
        "held-source"
#endif
        << "\",\"completed\":true,\"action\":\"" << action
        << "\",\"factor\":" << o.factor << ",\"rate\":" << o.rate << ",\"case\":\"" << o.c.id
        << "\",\"prefix\":" << prefixFrames << ",\"common\":" << unsigned(commonSeconds * o.rate)
        << ",\"fade\":" << fade(o) << ",\"settled\":" << settledFrames << ",\"input\":"
        << juce::JSON::toString(juce::String(o.input.generic_string())).toStdString() << "}\n";
    manifest.finish();
}
void selfTest() {
#if defined(RACKSYNTH_HISTORY_TRACE)
    airack::probe::history_trace::selfTest();
#endif
    const std::vector<std::string> valid { "--factor", "4", "--rate", "48000", "--case", "pre-1-0",
        "--input", fs::current_path().string(), "--output", (fs::current_path() / "unused").string() };
    require(parse(valid).c.id == "pre-1-0", "valid parser");
    unsigned timelineHistories = 0;
    for (const auto rate : { 44100u, 48000u, 96000u }) for (bool modes : { false, true })
        for (const auto& c : cases(modes)) for (unsigned h = 0; h < (modes ? 3u : 2u); ++h) {
            const Options o { 4, rate, c, {}, {} };
            require(start(o) == 4096 + 2 * rate && start(o) + fade(o) + settledFrames
                    == 4096 + 2 * rate + unsigned(std::ceil(.020 * rate)) + 128, "complete timeline");
            const auto phase0 = c.settings(0, h), phase1 = c.settings(1, h), phase2 = c.settings(2, h);
            require(c.modes ? phase1.mode != phase2.mode :
                    (c.pre ? !phase1.preLowCutEnabled && phase2.preLowCutEnabled
                           : !phase1.postHighCutEnabled && phase2.postHighCutEnabled), "phase1/phase2 settings");
            require(!c.modes || (phase0.mode == (h == 2 ? phase1.mode : phase2.mode)), "phase0 preparation");
            const auto stereo = input(0, rate, h, c);
            require(stereo[0] != stereo[1], "both non-identical channels");
            ++timelineHistories;
        }
    require(timelineHistories == 180, "all original histories/rates timeline coverage");
    auto rejects = [](const auto& operation) {
        bool refused = false; try { operation(); } catch (const std::exception&) { refused = true; }
        require(refused, "invalid input accepted");
    };
    for (const auto& replacement : std::vector<std::pair<unsigned, std::string>> {
        { 0, "--factor=4" }, { 1, "4junk" }, { 1, "2" }, { 3, "48001" }, { 5, "missing" },
        { 6, "--case" }, { 7, "relative" }, { 9, "relative" } }) {
        auto bad = valid; bad[replacement.first] = replacement.second; rejects([&] { parse(bad); });
    }
    auto extra = valid; extra.push_back("--factor"); extra.push_back("4"); rejects([&] { parse(extra); });
    std::vector<char> raw(16); require(frames(raw, 1).size() == 1, "finite decoder");
    rejects([&] { frames(raw, 2); }); raw.push_back(0); rejects([&] { frames(raw, 1); }); raw.resize(16);
    const double infinity = std::numeric_limits<double>::infinity(); std::memcpy(raw.data(), &infinity, 8);
    rejects([&] { frames(raw, 1); });
    struct RefusingBuffer : std::streambuf {
        std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    } refusing;
    std::ostream broken(&refusing); rejects([&] { writeFrame(broken, { 1, 2 }); });
    std::ostringstream sink; rejects([&] { writeFrame(sink, { infinity, 0 }); });
    const auto path = fs::temp_directory_path() / ("airack-held-selftest-" + std::to_string(std::random_device{}()) + ".f64");
    require(!fs::exists(path), "self-test path exists");
    {
        Capture file(path); writeFrame(file.stream, { 1, 2 });
        file.stream.exceptions(std::ios::goodbit); file.stream.setstate(std::ios::badbit);
        rejects([&] { file.finish(); });
    }
    require(fs::remove(path), "self-test cleanup failed");
    std::cout << "HISTORY_DIAGNOSIS_SELF_TEST PASS parser/finite/extent/write/close;rendering=NOT_RUN\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") { selfTest(); return 0; }
        auto options = parse(std::vector<std::string>(argv + 1, argv + argc));
        const auto retained = inputs(options); // All validation completes before output mutation.
        render(options, retained);
        std::cout << "HISTORY_DIAGNOSIS_CAPTURE factor=" << options.factor << " rate=" << options.rate
                  << " case=" << options.c.id << " action=" << action << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HISTORY_DIAGNOSIS_REFUSED " << error.what() << '\n'; return 1;
    }
}

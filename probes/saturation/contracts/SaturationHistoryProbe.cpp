#include "SaturationHistoryCases.h"
#include <algorithm>
#include <bit>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>

namespace {
using namespace airack::probe::history;
using airack::probe::Candidate;
namespace fs = std::filesystem;
#if defined(RACKSYNTH_HISTORY_LEGACY)
constexpr bool legacy = true;
#else
constexpr bool legacy = false;
#endif
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
unsigned number(std::string_view text) {
    unsigned result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc() && parsed.ptr == text.data() + text.size(), "invalid integer");
    return result;
}
void writeFrame(std::ostream& stream, Candidate::Frame frame) {
    static_assert(std::endian::native == std::endian::little && sizeof(frame) == 16);
    require(std::isfinite(frame[0]) && std::isfinite(frame[1]), "nonfinite frame");
    stream.write(reinterpret_cast<const char*>(frame.data()), sizeof(frame));
    require(bool(stream), "capture write failed");
}
struct Capture {
    std::ofstream stream;
    explicit Capture(const fs::path& path) : stream(path, std::ios::binary) {
        stream.exceptions(std::ios::badbit | std::ios::failbit);
    }
    void write(Candidate::Frame frame) { writeFrame(stream, frame); }
    void finish() { stream.flush(); stream.close(); require(bool(stream), "capture close failed"); }
};
std::vector<Case> selectedCases(bool modes, int character) {
    require(character == -1 || (modes && character >= 0 && character <= 2), "character selector requires modes");
    auto result = cases(modes);
    if (character != -1) std::erase_if(result, [character](const auto& c) { return c.character != unsigned(character); });
    return result;
}
void manifest(const fs::path& out, const std::vector<Case>& cohort, unsigned factor, unsigned rate, bool modes, int character) {
    const unsigned start = prefixFrames + unsigned(commonSeconds * rate);
    std::ofstream stream(out / "capture.json"); stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << std::setprecision(17) << "{\"schema\":1,\"role\":\"" << (legacy ? "legacy" : "actual")
        << "\",\"factor\":" << factor << ",\"rate\":" << rate << ",\"family\":\"" << (modes ? "modes" : "cuts")
        << '"';
    if (character != -1) stream << ",\"character\":" << character;
    stream << ",\"prefix\":" << prefixFrames << ",\"common\":" << unsigned(commonSeconds * rate)
        << ",\"fade\":" << unsigned(std::ceil(.020 * rate)) << ",\"settled\":" << settledFrames
        << ",\"witness\":" << witnessFrames << ",\"drive\":18,\"pre_hz\":600,\"post_hz\":1200,\"dc\":["
        << dcLeft << ',' << dcRight << "],\"events\":[{\"frame\":0,\"event\":\"old-history\"},{\"frame\":"
        << prefixFrames << ",\"event\":\"common-source\"},{\"frame\":" << start << ",\"event\":\"return\"}],\"cases\":[";
    for (std::size_t n = 0; n < cohort.size(); ++n) {
        const auto& c = cohort[n];
        stream << (n ? "," : "") << "{\"id\":\"" << c.id << "\",\"family\":\"" << (modes ? "modes" : "cuts")
            << "\",\"mode\":" << c.mode << ",\"character\":" << c.character;
        if (modes) stream << ",\"zero\":" << (c.zero ? "true" : "false");
        else stream << ",\"cut\":\"" << (c.pre ? "pre" : "post") << '"';
        stream << '}';
    }
    stream << "]}\n"; stream.close();
}
void render(const Case& c, unsigned rate, unsigned factor, const fs::path& out) {
    const unsigned start = prefixFrames + unsigned(commonSeconds * rate);
    const unsigned total = start + unsigned(std::ceil(.020 * rate)) + settledFrames;
    for (unsigned history = 0; history < (c.modes ? 3u : 2u); ++history) {
        Candidate candidate;
        require(candidate.setSettings(c.settings(0, history)) && candidate.prepare(rate, factor), "prepare refused");
        const auto name = c.id + '.' + (history == 2 ? "fresh" : "old" + std::to_string(history));
        Capture inputFile(out / (name + ".input.f64")), outputFile(out / (name + ".output.f64"));
        for (unsigned frame = 0; frame < total; ++frame) {
            if (frame == prefixFrames) require(candidate.setSettings(c.settings(1, history)), "source switch refused");
            if (frame == start) require(candidate.setSettings(c.settings(2, history)), "return switch refused");
            const auto in = input(frame, rate, history, c);
            inputFile.write(in); outputFile.write(candidate.process(in));
        }
        inputFile.finish(); outputFile.finish();
    }
}
void selfTest() {
    require(cases(true).size() == 12 && cases(false).size() == 12, "case count");
    for (int character = 0; character < 3; ++character) {
        const auto subset = selectedCases(true, character);
        require(subset.size() == 4, "character subset count");
        for (const auto& item : subset) require(item.character == unsigned(character), "wrong character subset");
    }
    const auto c = cases(true).front();
    require(c.settings(0, 0).mode == c.settings(2, 0).mode
            && c.settings(1, 0).mode != c.settings(0, 0).mode
            && c.settings(0, 2) == c.settings(1, 2), "visited/fresh timeline");
    for (bool modes : { true, false }) for (const auto& item : cases(modes))
        for (unsigned history = 0; history < (modes ? 3u : 2u); ++history) for (unsigned phase = 0; phase < 3; ++phase)
            require(airack::validateSaturation(item.settings(phase, history)), "invalid settings");
    struct RefusingBuffer : std::streambuf {
        std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    } refusing;
    std::ostream broken(&refusing); bool refused = false;
    try { writeFrame(broken, { 1, 2 }); } catch (const std::exception&) { refused = true; }
    require(refused, "write failure undetected");
    refused = false;
    try { writeFrame(std::cout, { std::numeric_limits<double>::infinity(), 0 }); }
    catch (const std::exception&) { refused = true; }
    require(refused, "nonfinite frame accepted");
    const auto path = fs::temp_directory_path() / ("airack-history-" + std::to_string(std::random_device{}())
                                                  + (legacy ? "-legacy" : "-actual") + ".f64");
    require(!fs::exists(path), "self-test path exists");
    {
        Capture file(path); file.write({ 1, 2 });
        file.stream.exceptions(std::ios::goodbit); file.stream.setstate(std::ios::badbit);
        refused = false;
        try { file.finish(); } catch (const std::exception&) { refused = true; }
        require(refused, "close failure undetected");
    }
    require(fs::remove(path), "self-test cleanup failed");
    std::cout << "HISTORY_SELF_TEST PASS write/close/finite guards;fixed cases;visited/fresh timeline\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") { selfTest(); return 0; }
        require((argc == 9 || argc == 11) && std::string_view(argv[1]) == "--factor" && std::string_view(argv[3]) == "--rate"
                && std::string_view(argv[5]) == "--family", "expected factor/rate/family/[character]/fresh-output");
        const bool grouped = argc == 11;
        require((!grouped || std::string_view(argv[7]) == "--character")
                && std::string_view(argv[grouped ? 9 : 7]) == "--output", "invalid selector/output flags");
        const unsigned factor = number(argv[2]), rate = number(argv[4]);
        const std::string_view family(argv[6]);
        require((factor == 4 || factor == 64) && (rate == 44100 || rate == 48000 || rate == 96000)
                && (family == "modes" || family == "cuts"), "unsupported cohort");
        const auto selected = grouped ? number(argv[8]) : 0;
        require(!grouped || (family == "modes" && selected <= 2), "character selector requires modes 0|1|2");
        const int character = grouped ? int(selected) : -1;
        const auto cohort = selectedCases(family == "modes", character);
        const fs::path out(argv[grouped ? 10 : 8]); require(out.is_absolute(), "output must be absolute");
        require(fs::create_directory(out), "output already exists");
        manifest(out, cohort, factor, rate, family == "modes", character);
        for (const auto& c : cohort) render(c, rate, factor, out);
        std::cout << "HISTORY_CAPTURE role=" << (legacy ? "legacy" : "actual") << " factor=" << factor
                  << " rate=" << rate << " family=" << family << " cases=" << cohort.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HISTORY_CAPTURE_REFUSED " << error.what() << '\n'; return 1;
    }
}

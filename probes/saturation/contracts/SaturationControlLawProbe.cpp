#include "SaturationControlLawCases.h"
#include "SaturationReturnOracle.h"
#include <bit>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>

namespace airack::probe::control_law {
struct Trace {
    std::array<Candidate::Frame, 64> wet {}, dry {};
    unsigned count = 0;
    bool overflow = false;
};
Trace* activeTrace = nullptr;
}
#if defined(RACKSYNTH_CONTROL_REFERENCE)
namespace airack::probe {
void saturationReferenceTrace(Candidate::Frame wet, Candidate::Frame matchedDry) noexcept {
    auto* trace = control_law::activeTrace;
    if (!trace) return;
    if (trace->count == trace->wet.size()) { trace->overflow = true; return; }
    trace->wet[trace->count] = wet; trace->dry[trace->count++] = matchedDry;
}
}
#endif
namespace {
using namespace airack::probe::control_law;
using airack::probe::Candidate;
namespace fs = std::filesystem;
#if defined(RACKSYNTH_CONTROL_REFERENCE)
constexpr bool reference = true;
#else
constexpr bool reference = false;
#endif
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
unsigned number(std::string_view text) {
    unsigned result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    require(parsed.ec == std::errc() && parsed.ptr == text.data() + text.size(), "invalid integer");
    return result;
}
void ready(Candidate& candidate, const airack::SaturationSettings& settings, unsigned rate, unsigned factor) {
    require(candidate.setSettings(settings) && candidate.prepare(rate, factor), "prepare refused");
}
void writeFrame(std::ostream& stream, Candidate::Frame frame) {
    static_assert(std::endian::native == std::endian::little && sizeof(frame) == 16);
    require(std::isfinite(frame[0]) && std::isfinite(frame[1]), "nonfinite output");
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
float correction(const Case& c) {
    const auto table = airack::probe::saturationCompensationTable(airack::SaturationCharacter(c.character));
    const auto low = unsigned(c.drive);
    const float result = float(table[low] * (1 - (double(c.drive) - low))
                              + table[low + 1] * (double(c.drive) - low));
    require(result == airack::saturationDriveCompensationDb(airack::SaturationCharacter(c.character), c.drive),
            "independent compensation interpolation differs");
    return result;
}
double gain(double db) { return std::pow(10., db / 20); }
void manifest(const fs::path& out, const std::vector<Case>& cohort, unsigned factor, unsigned rate) {
    std::ofstream stream(out / "capture.json"); stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << std::setprecision(17) << "{\"schema\":1,\"role\":\"" << (reference ? "reference" : "actual")
        << "\",\"factor\":" << factor << ",\"rate\":" << rate << ",\"prefix\":" << prefixFrames
        << ",\"fade\":" << unsigned(std::ceil(.020 * rate)) << ",\"settled\":" << settledFrames
        << ",\"return_phase\":\"even-before-odd;descending-stages;previous-delayed-hold\",\"cases\":[";
    for (std::size_t n = 0; n < cohort.size(); ++n) {
        const auto& c = cohort[n];
        stream << (n ? "," : "") << "{\"id\":\"" << c.id << "\",\"mode\":" << c.mode
            << ",\"character\":" << c.character << ",\"mask\":" << c.mask << ",\"drive\":" << c.drive
            << ",\"compensation\":" << (c.compensation ? "true" : "false")
            << ",\"reverse\":" << (c.reverse ? "true" : "false") << ",\"correction_db\":" << correction(c) << '}';
    }
    stream << "],\"coefficients\":{";
    bool first = true;
    const auto array = [&](const std::string& name, std::span<const double> values) {
        stream << (first ? "" : ",") << '"' << name << "\":["; first = false;
        for (std::size_t n = 0; n < values.size(); ++n) stream << (n ? "," : "") << values[n];
        stream << ']';
    };
    for (const auto& item : airack::probe::saturationCoefficientManifest()) {
        const std::string name = item.name;
        if (name.find("down-") != std::string::npos || name.starts_with("reference-fir")) array(name, item.values);
    }
    for (unsigned n = 0; n < 3; ++n)
        array("compensation-" + std::to_string(n), airack::probe::saturationCompensationTable(airack::SaturationCharacter(n)));
    stream << "}}\n"; stream.close();
}
void render(const Case& c, unsigned rate, unsigned factor, unsigned total, const fs::path& out) {
    const unsigned fade = unsigned(std::ceil(.020 * rate));
    if constexpr (reference) {
        Candidate source, immediate;
        ready(source, c.stationary(), rate, factor); ready(immediate, c.endpoint(c.reverse), rate, factor);
        const auto coefficients = airack::probe::saturationCoefficientManifest();
        ReturnOracle qualified(factor, coefficients), expected(factor, coefficients), from(factor, coefficients),
            to(factor, coefficients), shifted(factor, coefficients), reset(factor, coefficients), badLaw(factor, coefficients);
        Capture legacy(out / (c.id + ".legacy.f64")), qualification(out / (c.id + ".qualified.f64")),
            correct(out / (c.id + ".expected.f64")), endpointFrom(out / (c.id + ".from.f64")),
            endpointTo(out / (c.id + ".to.f64")), hard(out / (c.id + ".immediate.f64")),
            oneFrame(out / (c.id + ".one-frame.f64")), zero(out / (c.id + ".reset-history.f64")),
            wrong(out / (c.id + (c.compensation ? ".linear-amplitude.f64" : ".raw-dry.f64")));
        const double db = correction(c), endGain = gain(db);
        for (unsigned frame = 0; frame < total; ++frame) {
            const auto in = input(frame, rate);
            if (frame == prefixFrames) {
                require(immediate.setSettings(c.endpoint(!c.reverse)), "switch refused"); reset.reset();
            }
            Trace trace;
            activeTrace = &trace;
            const auto normal = source.process(in);
            activeTrace = nullptr;
            require(!trace.overflow && trace.count == factor, "reference trace extent");
            const auto hardOutput = immediate.process(in);
            const auto transform = [&](double w, bool bad) {
                auto values = trace.wet;
                for (unsigned n = 0; n < factor; ++n) for (unsigned ch = 0; ch < 2; ++ch) {
                    if (c.compensation) values[n][ch] *= bad ? 1 + w * (endGain - 1) : gain(db * w);
                    else {
                        const double dry = bad && frame >= prefixFrames ? in[ch] : trace.dry[n][ch];
                        values[n][ch] = dry + w * (values[n][ch] - dry);
                    }
                }
                return values;
            };
            const double w = weight(c, frame, fade);
            const auto controlled = transform(w, false);
            legacy.write(normal); qualification.write(qualified.process(trace.wet, factor));
            correct.write(expected.process(controlled, factor));
            endpointFrom.write(from.process(transform(c.reverse ? 1 : 0, false), factor));
            endpointTo.write(to.process(transform(c.reverse ? 0 : 1, false), factor));
            hard.write(hardOutput); oneFrame.write(shifted.process(transform(weight(c, frame, fade, 1), false), factor));
            zero.write(reset.process(controlled, factor)); wrong.write(badLaw.process(transform(w, true), factor));
        }
        legacy.finish(); qualification.finish(); correct.finish(); endpointFrom.finish(); endpointTo.finish();
        hard.finish(); oneFrame.finish(); zero.finish(); wrong.finish();
    } else {
        Candidate actual; ready(actual, c.endpoint(c.reverse), rate, factor);
        Capture capture(out / (c.id + ".actual.f64"));
        for (unsigned frame = 0; frame < total; ++frame) {
            if (frame == prefixFrames) require(actual.setSettings(c.endpoint(!c.reverse)), "switch refused");
            capture.write(actual.process(input(frame, rate)));
        }
        capture.finish();
    }
}
void selfTest() {
    require(cases().size() == 84, "case count");
    const Case c = cases().front();
    require(weight(c, prefixFrames - 1, 960) == 0 && weight(c, prefixFrames, 960) == 1. / 960
            && weight(c, prefixFrames + 959, 960) == 1, "ramp endpoints");
    struct RefusingBuffer : std::streambuf {
        std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
    } refusing;
    std::ostream broken(&refusing); bool refused = false;
    try { writeFrame(broken, { 1, 2 }); } catch (const std::exception&) { refused = true; }
    require(refused, "write failure not detected");
    const auto path = fs::temp_directory_path() / ("airack-control-" + std::to_string(std::random_device{}())
                                                 + (reference ? "-reference" : "-actual") + ".f64");
    require(!fs::exists(path), "self-test output exists");
    {
        Capture file(path); file.write({ 1, 2 });
        file.stream.exceptions(std::ios::goodbit); file.stream.setstate(std::ios::badbit);
        refused = false;
        try { file.finish(); } catch (const std::exception&) { refused = true; }
        require(refused, "close failure not detected");
    }
    require(fs::remove(path), "self-test cleanup failed");
    std::cout << "CONTROL_SELF_TEST PASS write/close errors;case extent;ramp endpoints\n";
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") { selfTest(); return 0; }
        require(argc == 7 && std::string_view(argv[1]) == "--factor" && std::string_view(argv[3]) == "--rate"
                && std::string_view(argv[5]) == "--output", "expected --factor 4|64 --rate 44100|48000|96000 --output fresh-absolute-directory");
        const unsigned factor = number(argv[2]), rate = number(argv[4]);
        require((factor == 4 || factor == 64) && (rate == 44100 || rate == 48000 || rate == 96000), "unsupported cohort");
        const fs::path out(argv[6]); require(out.is_absolute(), "output must be absolute");
        require(fs::create_directory(out), "output already exists");
        const auto cohort = cases(); manifest(out, cohort, factor, rate);
        const unsigned total = prefixFrames + unsigned(std::ceil(.020 * rate)) + settledFrames;
        Capture stimulus(out / "input.f64");
        for (unsigned n = 0; n < total; ++n) stimulus.write(input(n, rate));
        stimulus.finish();
        for (const auto& c : cohort) render(c, rate, factor, total, out);
        std::cout << "CONTROL_CAPTURE role=" << (reference ? "reference" : "actual") << " factor=" << factor
                  << " rate=" << rate << " cases=" << cohort.size() << " frames=" << total << '\n';
        return 0;
    } catch (const std::exception& error) {
        activeTrace = nullptr; std::cerr << "CONTROL_CAPTURE_REFUSED " << error.what() << '\n'; return 1;
    }
}

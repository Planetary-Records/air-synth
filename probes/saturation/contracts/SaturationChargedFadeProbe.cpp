#include "SaturationChargedFadeCases.h"
#include <bit>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using airack::probe::Candidate;
using namespace airack::probe::charged;
namespace fs = std::filesystem;
#if defined(RACKSYNTH_CHARGED_REFERENCE)
constexpr bool reference = true;
#else
constexpr bool reference = false;
#endif
unsigned number(std::string_view value) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid integer");
    return result;
}
void ready(Candidate& c, const airack::SaturationSettings& settings, unsigned rate, unsigned factor) {
    if (!c.setSettings(settings) || !c.prepare(rate, factor)) throw std::runtime_error("prepare refused");
}
class Capture {
    std::ofstream stream;
public:
    explicit Capture(const fs::path& path) : stream(path, std::ios::binary) {
        static_assert(std::endian::native == std::endian::little && sizeof(double) == 8 && sizeof(Candidate::Frame) == 16);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
    }
    void write(Candidate::Frame frame) {
        for (double value : frame) if (!std::isfinite(value)) throw std::runtime_error("nonfinite output");
        stream.write(reinterpret_cast<const char*>(frame.data()), sizeof(frame));
    }
    void finish() { stream.close(); }
};
void render(const Case& c, unsigned rate, unsigned factor, unsigned suffix, const fs::path& out) {
    if constexpr (reference) {
        Candidate from, to, immediate, fresh;
        ready(from, c.from, rate, factor);
        // Character/mode endpoints charge under A. The cut oracle always
        // charges the optional filter, including before an on->off switch.
        auto toPrefix = c.from;
        if (c.prefix != Case::musical) {
            toPrefix = c.to;
            (c.prefix == Case::nyquist ? toPrefix.preLowCutEnabled : toPrefix.postHighCutEnabled) = true;
        }
        ready(to, toPrefix, rate, factor);
        ready(immediate, c.from, rate, factor); ready(fresh, c.to, rate, factor);
        Capture x(out / (c.id + ".input.f64")), a(out / (c.id + ".from.f64")),
            b(out / (c.id + ".to.f64")), hard(out / (c.id + ".immediate.f64")),
            zero(out / (c.id + ".fresh.f64"));
        for (unsigned n = 0; n < prefixFrames + suffix; ++n) {
            if (n == prefixFrames) {
                if (!to.setSettings(c.to) || !immediate.setSettings(c.to)) throw std::runtime_error("switch refused");
            }
            const auto in = input(c, n, rate);
            x.write(in); a.write(from.process(in)); b.write(to.process(in)); hard.write(immediate.process(in));
            if (n >= prefixFrames) zero.write(fresh.process(in));
        }
        x.finish(); a.finish(); b.finish(); hard.finish(); zero.finish();
    } else {
        Candidate actual; ready(actual, c.from, rate, factor);
        Capture y(out / (c.id + ".actual.f64"));
        for (unsigned n = 0; n < prefixFrames + suffix; ++n) {
            if (n == prefixFrames && !actual.setSettings(c.to)) throw std::runtime_error("switch refused");
            y.write(actual.process(input(c, n, rate)));
        }
        y.finish();
    }
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 7 || std::string_view(argv[1]) != "--factor" || std::string_view(argv[3]) != "--rate"
            || std::string_view(argv[5]) != "--output") throw std::runtime_error("expected --factor 4|64 --rate 44100|48000|96000 --output fresh-directory");
        const unsigned factor = number(argv[2]), rate = number(argv[4]);
        if ((factor != 4 && factor != 64) || (rate != 44100 && rate != 48000 && rate != 96000))
            throw std::runtime_error("unsupported cohort");
        const fs::path out(argv[6]);
        if (!fs::create_directory(out)) throw std::runtime_error("output already exists");
        const auto cohort = cases();
        const unsigned fade = unsigned(std::ceil(.020 * rate)), suffix = fade + 128;
        for (const auto& c : cohort) render(c, rate, factor, suffix, out);
        std::ofstream manifest(out / "capture.json");
        manifest.exceptions(std::ios::badbit | std::ios::failbit);
        manifest << "{\"schema\":1,\"role\":\"" << (reference ? "reference" : "actual")
                 << "\",\"factor\":" << factor << ",\"rate\":" << rate << ",\"prefix\":" << prefixFrames
                 << ",\"fade\":" << fade << ",\"suffix\":" << suffix << ",\"cases\":[";
        for (std::size_t n = 0; n < cohort.size(); ++n) manifest << (n ? "," : "") << '"' << cohort[n].id << '"';
        manifest << "]}\n"; manifest.close();
        std::cout << "CHARGED_CAPTURE role=" << (reference ? "reference" : "actual") << " factor=" << factor
                  << " rate=" << rate << " cases=" << cohort.size() << " prefix=" << prefixFrames << " suffix=" << suffix << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CHARGED_CAPTURE_REFUSED " << error.what() << '\n'; return 1;
    }
}

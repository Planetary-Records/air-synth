#include "SaturationCandidate.h"
#include "SaturationAnalysis.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

// Additional characterization only. The original feasibility workload is unchanged.
namespace {
using namespace airack;
using namespace airack::probe;
using namespace airack::probe::analysis;
namespace fs = std::filesystem;
constexpr double rate = 48000;
constexpr std::size_t frames = 65536, warm = 96000;
struct Condition { const char* name; double peak; float inputDb, tiltDb; };
constexpr Condition conditions[] {
    { "original", 1. / 64, 0, 0 }, { "hot-level", 1. / 16, 0, 0 },
    { "input-24", 1. / 64, 24, 0 }, { "tilt-6", 1. / 64, 0, 6 }
};
constexpr const char* styles[] { "soft", "glow", "dense" };
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::ofstream output(const fs::path& path, bool binary = false) {
    std::ofstream stream;
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.open(path, std::ios::out | (binary ? std::ios::binary : std::ios::openmode{}));
    stream << std::setprecision(17);
    return stream;
}
SaturationSettings settings(int mode, int style, const Condition& condition) {
    SaturationSettings s;
    s.mode = static_cast<SaturationMode>(mode); s.driveCompensation = false;
    s.inputTrimDb = condition.inputDb;
    for (auto& lane : s.lanes) {
        lane.character = static_cast<SaturationCharacter>(style);
        lane.driveDb = 36; lane.preTiltDb = condition.tiltDb;
    }
    return s;
}
std::vector<double> render(const fs::path& file, unsigned factor,
    const SaturationSettings& s, double peak, std::size_t bin) {
    Candidate candidate;
    require(candidate.prepare(rate, factor) && candidate.setSettings(s), "Candidate rejected diagnostic settings");
    candidate.reset();
    auto raw = output(file, true);
    std::vector<double> left(frames);
    for (std::size_t n = 0; n < warm + frames; ++n) {
        const double input = peak * std::sin(2 * std::numbers::pi * double(bin) * double(n) / frames);
        const auto result = candidate.process({ input, input });
        require(std::isfinite(result[0]) && std::isfinite(result[1]), "Nonfinite diagnostic output");
        if (n >= warm) {
            left[n - warm] = result[0];
            raw.write(reinterpret_cast<const char*>(result.data()), sizeof(result));
        }
    }
    raw.close();
    return left;
}
int selfTest() {
    constexpr std::size_t n = 4096, fundamental = 17;
    std::vector<double> clean(n), contaminated(n);
    for (std::size_t i = 0; i < n; ++i) {
        clean[i] = std::sin(2 * std::numbers::pi * fundamental * i / n);
        contaminated[i] = clean[i] + .1 * std::sin(2 * std::numbers::pi * 23 * i / n);
    }
    const auto reference = rectangularPowerSpectrum(clean);
    const auto known = measureAlias(rectangularPowerSpectrum(contaminated), reference, rate, fundamental);
    require(known.valid && std::abs(known.unwantedDbc + 20) < 1e-6, "Injected alias calibration failed");
    require(!passesAliasGate(known, known), "Injected -20dBc control unexpectedly passed");
    const auto badReference = measureAlias(reference, rectangularPowerSpectrum(contaminated), rate, fundamental);
    require(!badReference.valid, "Contaminated reference must invalidate measurement");
    std::cout << "Stress analyzer calibration and bad-reference control PASS\n";
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") return selfTest();
        require(argc == 3 && std::string(argv[1]) == "--output", "Usage: --self-test | --output FRESH_ABSOLUTE_DIRECTORY");
        const fs::path directory(argv[2]);
        require(directory.is_absolute() && !fs::exists(directory), "Output must be a fresh absolute directory");
        require(fs::create_directory(directory), "Cannot create output directory");
        const auto bin = *nearestOddCoherentBin(7040, rate, frames);
        auto manifest = output(directory / "manifest.txt");
        manifest << "DIAGNOSTIC ONLY; NOT production adoption or original quality acceptance\n"
            << "CPU DEFERRED; production FORCED_BYPASS; selected_factor=null\n"
            << "48000Hz; N65536; warm96000; nearest odd coherent bin " << bin
            << "; Drive36; compensationOff; lane/globalMix1; trims0 except stated input; cutsOff; crossovers180/3500\n"
            << "4x candidate; independent FIR64 reference; 1x negative; stereo-identical sine with continuous phase\n"
            << "Raw: interleaved stereo native IEEE754 binary64 (little endian on Windows), measured frames only\n"
            << "Reference<=-100dBc; candidate<=-50dBc and >=10dB below1x; original gates unchanged\n";
        manifest.close();
        auto rows = output(directory / "aliases.csv");
        rows << "id,mode,style,peak,input_db,tilt_db,bin,exact_hz,reference_unwanted_dbc,candidate_unwanted_dbc,one_x_unwanted_dbc,improvement_db,reference_qualified,valid,meets_original_numeric_limits\n";
        unsigned count = 0, invalid = 0, misses = 0;
        for (int mode = 0; mode < 2; ++mode) for (int style = 0; style < 3; ++style) for (const auto& condition : conditions) {
            const auto id = std::to_string(mode) + "-" + styles[style] + "-" + condition.name;
            const auto s = settings(mode, style, condition);
            const auto y = render(directory / (id + "-4x.f64"), 4, s, condition.peak, bin);
            const auto ref = render(directory / (id + "-64x.f64"), 64, s, condition.peak, bin);
            const auto one = render(directory / (id + "-1x.f64"), 1, s, condition.peak, bin);
            const auto spectrum = rectangularPowerSpectrum(ref);
            const auto result = measureAlias(rectangularPowerSpectrum(y), spectrum, rate, bin);
            const auto negative = measureAlias(rectangularPowerSpectrum(one), spectrum, rate, bin);
            const bool valid = result.valid && negative.valid, meets = passesAliasGate(result, negative);
            ++count; invalid += !valid; misses += valid && !meets;
            rows << id << ',' << mode << ',' << styles[style] << ',' << condition.peak << ',' << condition.inputDb << ',' << condition.tiltDb
                << ',' << bin << ',' << rate * bin / frames << ',' << result.referenceUnwantedDbc << ',' << result.unwantedDbc << ','
                << negative.unwantedDbc << ',' << negative.unwantedDbc - result.unwantedDbc << ',' << (result.referenceUnwantedDbc <= -100)
                << ',' << valid << ',' << meets << '\n';
            rows.flush();
            std::cout << id << ": " << result.unwantedDbc << "dBc; ref " << result.referenceUnwantedDbc
                << "; " << (valid ? meets ? "within numeric limits" : "OUTSIDE numeric limits" : "INVALID reference/measurement") << std::endl;
        }
        rows.close();
        auto summary = output(directory / "summary.txt");
        summary << "complete_cases=" << count << "\ninvalid_cases=" << invalid << "\nvalid_cases_outside_original_numeric_limits=" << misses
            << "\nCPU=DEFERRED\nselected_factor=null\nproduction=FORCED_BYPASS\n";
        summary.close();
        return invalid ? 2 : misses ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Incomplete/invalid saturation stress diagnostic: " << error.what() << '\n'; return 2;
    }
}

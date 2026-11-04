#include "SaturationCandidate.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <limits>
#include <numbers>
#include <vector>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using namespace airack;
using airack::probe::Candidate;
using Frame = Candidate::Frame;
constexpr double pi = std::numbers::pi;
constexpr double referencePeak = 1.0 / 16.0;
constexpr std::array characters { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense };
int checks = 0, failures = 0, sections = 0;
const char* section = "";
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) { ++failures; std::cout << "FAIL " << section << ':' << line << ' ' << expression << '\n'; }
}
#define CHECK(x) check(bool(x), #x, __LINE__)
template<class Test> void group(const char* name, Test test) {
    section = name; ++sections;
    const auto before = checks, failed = failures;
    test();
    std::cout << name << ": " << checks - before << " checks, " << failures - failed << " failures\n";
}
bool finite(Frame x) { return std::isfinite(x[0]) && std::isfinite(x[1]); }
double error(Frame a, Frame b) { return std::max(std::abs(a[0] - b[0]), std::abs(a[1] - b[1])); }
Frame signal(int n, double rate = 48000) {
    return { .021 * std::sin(2 * pi * 307 * n / rate) + .008 * std::cos(2 * pi * 7103 * n / rate),
             .017 * std::cos(2 * pi * 983 * n / rate) - .005 * std::sin(2 * pi * 11371 * n / rate) };
}
SaturationSettings driven(SaturationMode mode, SaturationCharacter character) {
    SaturationSettings s; s.mode = mode; s.driveCompensation = false;
    for (auto& lane : s.lanes) { lane.character = character; lane.driveDb = 18; }
    return s;
}
bool ready(Candidate& c, const SaturationSettings& s, double rate = 48000, unsigned factor = 2) {
    const bool ok = c.prepare(rate, factor) && c.setSettings(s); CHECK(ok); return ok;
}

void validation() {
    const SaturationSettings defaults;
    CHECK(validateSaturation(defaults));
    CHECK(defaults.mode == SaturationMode::fullBand && defaults.driveCompensation);
    CHECK(defaults.mix == 1 && defaults.lanes[0].mix == 1);
    auto corners = effectiveSaturationCrossovers(defaults, 8000);
    CHECK(corners.lowHz == 180 && corners.highHz == 3500);
    auto s = defaults; s.lowCrossoverHz = 2000; s.highCrossoverHz = 500;
    CHECK(validateSaturation(s));
    corners = effectiveSaturationCrossovers(s, 48000);
    CHECK(corners.lowHz == 400 && corners.highHz == 500);
    s.highCrossoverHz = 16000;
    corners = effectiveSaturationCrossovers(s, 8000);
    CHECK(corners.lowHz == 2000 && corners.highHz == 3600);
    CHECK(s.highCrossoverHz == 16000 && s.lowCrossoverHz == 2000);
    for (std::size_t lane = 0; lane < defaults.lanes.size(); ++lane) {
        s = defaults; s.lanes[lane].driveDb = std::numeric_limits<float>::quiet_NaN();
        CHECK(!validateSaturation(s)); // Includes the inactive retained banks.
        s = defaults; s.lanes[lane].character = static_cast<SaturationCharacter>(255);
        CHECK(!validateSaturation(s));
        for (auto member : { &SaturationLaneSettings::driveDb, &SaturationLaneSettings::preTiltDb,
                             &SaturationLaneSettings::mix, &SaturationLaneSettings::trimDb }) {
            s = defaults; s.lanes[lane].*member = std::numeric_limits<float>::infinity();
            CHECK(!validateSaturation(s));
        }
    }
    struct Bound { float SaturationSettings::*member; float low, high; };
    for (const auto b : { Bound{ &SaturationSettings::inputTrimDb, -24, 24 },
                         Bound{ &SaturationSettings::outputTrimDb, -24, 12 },
                         Bound{ &SaturationSettings::mix, 0, 1 },
                         Bound{ &SaturationSettings::lowCrossoverHz, 40, 2000 },
                         Bound{ &SaturationSettings::highCrossoverHz, 500, 16000 },
                         Bound{ &SaturationSettings::preLowCutHz, 20, 2000 },
                         Bound{ &SaturationSettings::postHighCutHz, 1000, 20000 } }) {
        s = defaults; s.*b.member = b.low; CHECK(validateSaturation(s));
        s.*b.member = b.high; CHECK(validateSaturation(s));
        s.*b.member = std::nextafter(b.low, -std::numeric_limits<float>::infinity()); CHECK(!validateSaturation(s));
        s.*b.member = std::nextafter(b.high, std::numeric_limits<float>::infinity()); CHECK(!validateSaturation(s));
    }
    struct LaneBound { float SaturationLaneSettings::*member; float low, high; };
    for (const auto b : { LaneBound{ &SaturationLaneSettings::driveDb, 0, 36 },
                         LaneBound{ &SaturationLaneSettings::preTiltDb, -6, 6 },
                         LaneBound{ &SaturationLaneSettings::mix, 0, 1 },
                         LaneBound{ &SaturationLaneSettings::trimDb, -18, 18 } }) {
        for (std::size_t lane = 0; lane < defaults.lanes.size(); ++lane) {
            s = defaults; s.lanes[lane].*b.member = b.low; CHECK(validateSaturation(s));
            s.lanes[lane].*b.member = b.high; CHECK(validateSaturation(s));
            s.lanes[lane].*b.member = std::nextafter(b.low, -std::numeric_limits<float>::infinity()); CHECK(!validateSaturation(s));
            s.lanes[lane].*b.member = std::nextafter(b.high, std::numeric_limits<float>::infinity()); CHECK(!validateSaturation(s));
        }
    }
    s = defaults; s.mode = static_cast<SaturationMode>(255); CHECK(!validateSaturation(s));
    corners = effectiveSaturationCrossovers(s, 48000);
    CHECK(std::isnan(corners.lowHz) && std::isnan(corners.highHz));
    for (double rate : { 0., -1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() }) {
        corners = effectiveSaturationCrossovers(defaults, rate);
        CHECK(std::isnan(corners.lowHz) && std::isnan(corners.highHz));
        CHECK(std::isnan(saturationTailSeconds(defaults, rate)));
    }
    CHECK(std::isnan(saturationTailSeconds(s, 48000)));
    CHECK(std::isfinite(saturationTailSeconds(defaults, 48000)) && saturationTailSeconds(defaults, 48000) >= 0);
    CHECK(std::isnan(saturationDriveCompensationDb(static_cast<SaturationCharacter>(255), 18)));
    CHECK(std::isnan(saturationDriveCompensationDb(SaturationCharacter::soft, -1)));
    CHECK(std::isnan(saturationDriveCompensationDb(SaturationCharacter::soft, 37)));
    CHECK(std::isnan(saturationDriveCompensationDb(SaturationCharacter::soft, std::numeric_limits<float>::quiet_NaN())));
}

void transactionalAdmission() {
    Candidate idle; CHECK(error(idle.process({ .1, -.1 }), {}) == 0); idle.reset();
    auto settings = driven(SaturationMode::threeBand, SaturationCharacter::glow);
    settings.lanes[2].preTiltDb = 3; settings.preLowCutEnabled = true;
    Candidate a, b; if (!ready(a, settings) || !ready(b, settings)) return;
    for (int n = 0; n < 2048; ++n) { a.process(signal(n)); b.process(signal(n)); }
    for (double rate : { 0., -1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() }) {
        CHECK(!a.prepare(rate, 2));
        for (int n = 0; n < 64; ++n) CHECK(error(a.process(signal(n)), b.process(signal(n))) <= 1e-12);
    }
    for (unsigned factor : { 0u, 3u, 16u }) {
        CHECK(!a.prepare(48000, factor));
        for (int n = 0; n < 64; ++n) CHECK(error(a.process(signal(n)), b.process(signal(n))) <= 1e-12);
    }
    auto invalid = settings; invalid.lanes[0].mix = 2;
    CHECK(!a.setSettings(invalid));
    for (int n = 0; n < 512; ++n) CHECK(error(a.process(signal(n)), b.process(signal(n))) <= 1e-12);
    Candidate diagnostic, reference;
    CHECK(diagnostic.prepare(48000, 1)); CHECK(reference.prepare(48000, 64));
}

void neutralReferences() {
    for (auto mode : { SaturationMode::fullBand, SaturationMode::threeBand })
    for (unsigned factor : { 2u, 4u, 8u }) {
        SaturationSettings s; s.mode = mode;
        Candidate wet, globalDry, laneDry, bypassed;
        if (!ready(wet, s, 48000, factor)) continue;
        auto g = driven(mode, SaturationCharacter::glow); g.mix = 0;
        g.preLowCutEnabled = true; g.preLowCutHz = 1000; g.postHighCutEnabled = true; g.postHighCutHz = 1200;
        auto l = driven(mode, SaturationCharacter::dense); auto b = l;
        for (auto& lane : l.lanes) { lane.mix = 0; lane.preTiltDb = 6; lane.trimDb = 12; }
        for (auto& lane : b.lanes) { lane.bypassed = true; lane.preTiltDb = -6; lane.trimDb = -12; }
        if (!ready(globalDry, g, 48000, factor) || !ready(laneDry, l, 48000, factor) || !ready(bypassed, b, 48000, factor)) continue;
        double maxNull = 0, energy = 0;
        for (int n = 0; n < 8192; ++n) {
            const auto x = signal(n), y = wet.process(x);
            maxNull = std::max({ maxNull, error(y, globalDry.process(x)), error(y, laneDry.process(x)), error(y, bypassed.process(x)) });
            energy += y[0] * y[0];
        }
        CHECK(maxNull <= 1e-10); CHECK(energy > .01);
    }
    // Drive zero keeps the intentional tilt; lane Mix zero removes it.
    SaturationSettings tilted; tilted.lanes[0].preTiltDb = 6;
    Candidate a, b; if (!ready(a, tilted)) return;
    tilted.lanes[0].mix = 0; if (!ready(b, tilted)) return;
    double difference = 0;
    for (int n = 0; n < 8192; ++n) difference = std::max(difference, error(a.process(signal(n)), b.process(signal(n))));
    CHECK(difference > 1e-4);
}

std::complex<double> allpass(double corner, double rate, double hz) {
    const double k = std::tan(pi * corner / rate), d = 1 + std::sqrt(2.) * k + k * k;
    const double a1 = 2 * (k * k - 1) / d, a2 = (1 - std::sqrt(2.) * k + k * k) / d;
    const auto z = std::polar(1., -2 * pi * hz / rate);
    return (a2 + a1 * z + z * z) / (1. + a1 * z + a2 * z * z);
}
void crossoverPhase() {
    for (double rate : { 44100., 48000., 96000. })
    for (auto raw : { std::array<float, 2>{ 180, 3500 }, std::array<float, 2>{ 2000, 500 }, std::array<float, 2>{ 40, 16000 } }) {
        SaturationSettings s; s.mode = SaturationMode::threeBand; s.driveCompensation = false;
        s.lowCrossoverHz = raw[0]; s.highCrossoverHz = raw[1];
        const auto cuts = effectiveSaturationCrossovers(s, rate);
        for (double requested : { 30., 180., 1000., 3500., std::min(18000., .4 * rate) }) {
            constexpr int count = 16384;
            const int bin = std::max(1, int(std::round(requested * count / rate)));
            const double hz = rate * bin / count, omega = 2 * pi * bin / count;
            Candidate c; if (!ready(c, s, rate, 1)) continue;
            const int warmup = int(rate / 2);
            std::complex<double> measured {};
            for (int n = -warmup; n < count; ++n) {
                const auto y = c.process({ .01 * std::cos(omega * n), 0 });
                if (n >= 0) measured += y[0] * std::polar(1., -omega * n) * (2. / (.01 * count));
            }
            const auto alignment = (1. + std::polar(1., -omega)) * .5;
            const auto expected = alignment * allpass(cuts.lowHz, rate, hz) * allpass(cuts.highHz, rate, hz);
            CHECK(std::abs(measured - expected) <= 1e-8);
            CHECK(std::abs(20 * std::log10(std::abs(measured / alignment))) <= .1);
        }
    }
}

void scalarLaws() {
    using namespace airack::probe;
    for (auto character : characters) {
        CHECK(std::abs(shape(character, 0)) < 1e-14);
        CHECK(std::abs((shape(character, 1e-5) - shape(character, -1e-5)) / 2e-5 - 1) < 1e-8);
        CHECK(std::isfinite(primitive(character, 1000)) && std::isfinite(primitive(character, -1000)));
        for (double x : { -3., -1., -.75, -.2, .2, .75, 1., 3. }) {
            constexpr double step = 1e-5;
            const auto derivative = (primitive(character, x + step) - primitive(character, x - step)) / (2 * step);
            CHECK(std::abs(derivative - shape(character, x)) < 2e-9);
            CHECK(std::abs(adaa(character, x, x) - shape(character, x)) < 1e-14);
            CHECK(std::abs(adaa(character, x + 1e-9, x) - shape(character, x + .5e-9)) < 1e-14);
            if (character != SaturationCharacter::glow) CHECK(std::abs(shape(character, x) + shape(character, -x)) < 1e-14);
        }
        // Independent numerical integration catches a mismatched antiderivative.
        constexpr int intervals = 1024; constexpr double low = -1.3, high = 2.1, dx = (high - low) / intervals;
        double sum = shape(character, low) + shape(character, high);
        for (int n = 1; n < intervals; ++n) sum += (n % 2 ? 4 : 2) * shape(character, low + dx * n);
        CHECK(std::abs(adaa(character, high, low) - sum * dx / (3 * (high - low))) < 1e-8);
    }
    CHECK(std::abs(shape(SaturationCharacter::soft, 1) - std::tanh(1.)) < 1e-14);
    CHECK(std::abs(shape(SaturationCharacter::glow, 1) + shape(SaturationCharacter::glow, -1)) > .2);
    CHECK(shape(SaturationCharacter::dense, .5) == .5);
    CHECK(std::abs(shape(SaturationCharacter::dense, .75 - 1e-8) - shape(SaturationCharacter::dense, .75 + 1e-8)) < 3e-8);
    CHECK(shape(SaturationCharacter::dense, 10) <= 1 && shape(SaturationCharacter::dense, -10) >= -1);
}

void sagClosedForm() {
    using airack::probe::advanceSag;
    for (double nativeRate : { 44100., 48000., 96000. }) for (unsigned factor : { 1u, 2u, 4u, 8u, 64u }) {
        const double rate = nativeRate * factor;
        double envelope = 0, gain = 1;
        const int attack = int(std::round(.002 * rate));
        for (int n = 0; n < attack; ++n) gain = advanceSag(2., rate, envelope);
        const double expectedAttack = 2 * -std::expm1(-attack / (.002 * rate));
        CHECK(std::abs(envelope - expectedAttack) <= 1e-10 * std::max(1., expectedAttack));
        CHECK(gain >= std::pow(10., -6. / 20) && gain <= 1);
        const double initial = envelope;
        const int release = int(std::round(.080 * rate));
        for (int n = 0; n < release; ++n) gain = advanceSag(0., rate, envelope);
        const double expectedRelease = initial * std::exp(-release / (.080 * rate));
        CHECK(std::abs(envelope - expectedRelease) <= 1e-10 * std::max(1., expectedRelease));
        CHECK(gain >= std::pow(10., -6. / 20) && gain <= 1);
    }
    double envelope = 0;
    CHECK(advanceSag(0, 48000, envelope) == 1 && envelope == 0);
    for (double peak : { .01, .5, 1., 10., 1e8, 0. }) {
        bool bounded = true;
        for (int n = 0; n < 10000; ++n) {
            const double db = 20 * std::log10(advanceSag(peak, 48000, envelope));
            bounded = bounded && db >= -6.000000000001 && db <= 0;
        }
        CHECK(bounded);
    }
}

void denseIntegrationOracle() {
    using airack::probe::adaa;
    constexpr double drive = 18, gain = 7.943282347242815, amount = drive / 36;
    for (double rate : { 44100., 48000., 96000. }) {
        auto s = driven(SaturationMode::fullBand, SaturationCharacter::dense);
        Candidate c; if (!ready(c, s, rate, 1)) continue;
        c.reset();
        Frame previousInput {}, previousU {};
        const Frame attack { .020, .005 }, release { .001, .006 };
        const int attackFrames = int(.020 * rate), count = int(.200 * rate);
        const double attackPeak = gain * attack[0] / referencePeak;
        const double releasePeak = gain * release[1] / referencePeak;
        const double releaseStart = attackPeak * -std::expm1(-attackFrames / (.002 * rate));
        double maxError = 0, noSagDifference = 0;
        for (int n = 0; n < count; ++n) {
            const Frame x = n < attackFrames ? attack : release;
            const double envelope = n < attackFrames
                ? attackPeak * -std::expm1(-(n + 1) / (.002 * rate))
                : releasePeak + (releaseStart - releasePeak) * std::exp(-(n - attackFrames + 1) / (.080 * rate));
            const double reductionDb = std::clamp(6 * std::max(envelope - .5, 0.) / (envelope + .5), 0., 6.);
            const double sag = std::pow(10., -reductionDb / 20);
            Frame expected {};
            for (int channel = 0; channel < 2; ++channel) {
                const double u = gain * x[std::size_t(channel)] / referencePeak * sag;
                const double linear = .5 * (x[std::size_t(channel)] + previousInput[std::size_t(channel)]);
                expected[std::size_t(channel)] = linear + amount * (referencePeak * adaa(SaturationCharacter::dense, u, previousU[std::size_t(channel)]) - linear);
                previousU[std::size_t(channel)] = u;
                if (n > attackFrames / 2 && n < attackFrames) {
                    const double disconnected = x[std::size_t(channel)] + amount * (referencePeak * airack::probe::shape(SaturationCharacter::dense, gain * x[std::size_t(channel)] / referencePeak) - x[std::size_t(channel)]);
                    noSagDifference = std::max(noSagDifference, std::abs(expected[std::size_t(channel)] - disconnected));
                }
            }
            previousInput = x;
            maxError = std::max(maxError, error(c.process(x), expected));
        }
        CHECK(maxError <= 1e-10);
        CHECK(noSagDifference > 1e-4 * referencePeak);
    }
}

// Uncoloured full-band 1x reference: public scalar ADAA and sag stay independent
// of the integrated processor's repeated-work optimizations.
struct FullBandScalarOracle {
    double rate;
    Frame previousInput {}, previousU {}, previousDifference {}, dc {};
    double envelope = 0;
    Frame process(Frame input, SaturationCharacter character) {
        const double gain = std::pow(10., 18. / 20);
        Frame u { gain * input[0] / referencePeak, gain * input[1] / referencePeak };
        if (character == SaturationCharacter::dense) {
            const double sag = airack::probe::advanceSag(std::max(std::abs(u[0]), std::abs(u[1])), rate, envelope);
            u[0] *= sag; u[1] *= sag;
        }
        Frame expected {};
        for (std::size_t ch = 0; ch < 2; ++ch) {
            const double linear = .5 * (input[ch] + previousInput[ch]);
            const double difference = referencePeak * airack::probe::adaa(character, u[ch], previousU[ch]) - linear;
            dc[ch] = difference - previousDifference[ch] + std::exp(-2 * pi * 5 / rate) * dc[ch];
            previousDifference[ch] = difference;
            expected[ch] = linear + .5 * (character == SaturationCharacter::glow ? dc[ch] : difference);
        }
        previousInput = input; previousU = u;
        return expected;
    }
};

void integratedPrimitiveHistoryOracle() {
    // Quotient/near-equal branches alternate independently between channels.
    // In particular, zero must use Glow's nonzero primitive(0), including after
    // a near-equal fallback, reset or successful preparation.
    constexpr std::array<Frame, 12> normalized {{
        { .20, -.10 }, { .20 + 5e-8, .25 }, { -.35, .25 + 5e-8 },
        { -.35 + 5e-8, .25 + 5e-8 }, { .15, -.30 }, { 0, 0 },
        { 0, .20 }, { -.20, .20 }, { 0, 0 },
        { .18, 0 }, { .18, -.27 }, { -.36, -.27 }
    }};
    const double inputScale = referencePeak / std::pow(10., 18. / 20);
    CHECK(std::abs(airack::probe::primitive(SaturationCharacter::glow, 0)) > .01);
    for (std::size_t style = 0; style < characters.size(); ++style) {
        const auto a = characters[style], b = characters[(style + 1) % characters.size()];
        auto s = driven(SaturationMode::fullBand, a);
        Candidate c; if (!ready(c, s, 44100, 1)) continue;
        FullBandScalarOracle oracle { 44100 };
        const auto compare = [&](Frame input) {
            CHECK(error(c.process(input), oracle.process(input, s.lanes[0].character)) <= 1e-12);
        };
        const auto trace = [&] {
            for (const auto u : normalized) compare({ inputScale * u[0], inputScale * u[1] });
        };
        trace();
        for (const auto character : { b, a }) {
            s.lanes[0].character = character; CHECK(c.setSettings(s)); trace();
        }
        // Repeat A-B-A while Full is inactive. Its prior audio/ADAA/DC/sag
        // histories remain frozen while other retained banks are processed.
        for (const auto character : { b, a }) {
            s.mode = SaturationMode::threeBand; s.lanes[0].character = character;
            CHECK(c.setSettings(s));
            for (int n = 0; n < 32; ++n) CHECK(finite(c.process(signal(n))));
            s.mode = SaturationMode::fullBand; CHECK(c.setSettings(s)); trace();
        }
        // Warm Dense well into its sag attack before refusal: a rejected rate
        // or factor must preserve both prepared coefficients and audio history.
        for (int n = 0; n < 256; ++n) compare({ .08, -.04 });
        CHECK(!c.prepare(0, 1)); trace();
        CHECK(!c.prepare(96000, 3)); trace();
        c.reset(); oracle = FullBandScalarOracle { 44100 }; trace();
        CHECK(error(c.process({ std::numeric_limits<double>::quiet_NaN(), 0 }), {}) == 0);
        oracle = FullBandScalarOracle { 44100 }; trace();
        CHECK(c.prepare(96000, 1)); oracle = FullBandScalarOracle { 96000 }; trace();
        for (int n = 0; n < 256; ++n) compare({ .08, -.04 });
    }
}

double rms(Candidate& c, double rate) {
    double square = 0;
    for (int n = 0; n < int(2 * rate); ++n) {
        const double x = referencePeak / 4 * std::sin(2 * pi * 220 * n / rate);
        const auto y = c.process({ x, x });
        if (n >= int(rate)) square += y[0] * y[0];
    }
    return std::sqrt(square / rate);
}
void compensationCalibration() {
    for (auto character : characters) {
        CHECK(std::abs(saturationDriveCompensationDb(character, 0)) <= 1e-6);
        for (int drive = 0; drive <= 36; ++drive) {
            const double db = saturationDriveCompensationDb(character, float(drive));
            CHECK(std::isfinite(db) && db >= -18 && db <= 6);
            if (drive < 36) {
                const double next = saturationDriveCompensationDb(character, float(drive + 1));
                CHECK(std::abs(saturationDriveCompensationDb(character, float(drive) + .5f) - .5 * (db + next)) <= 2e-6);
            }
        }
        auto s = driven(SaturationMode::fullBand, character); s.lanes[0].driveDb = 0;
        Candidate baseline; if (!ready(baseline, s, 48000, 4)) continue;
        const double target = rms(baseline, 48000);
        for (float drive : { 6.f, 17.5f, 18.f, 35.5f, 36.f }) {
            s.lanes[0].driveDb = drive; s.driveCompensation = false;
            Candidate raw, compensated;
            if (!ready(raw, s, 48000, 4)) continue;
            s.driveCompensation = true; if (!ready(compensated, s, 48000, 4)) continue;
            const double rawRms = rms(raw, 48000), corrected = rms(compensated, 48000);
            const double expectedDb = std::clamp(20 * std::log10(target / rawRms), -18., 6.);
            CHECK(std::abs(saturationDriveCompensationDb(character, drive) - expectedDb) <= .5);
            CHECK(std::abs(20 * std::log10(corrected / rawRms) - saturationDriveCompensationDb(character, drive)) <= .01);
            CHECK(std::abs(20 * std::log10(corrected / target)) <= .5);
            if (expectedDb == -18 || expectedDb == 6)
                std::cout << "COMPENSATION_CLAMP character=" << int(character) << " drive=" << drive << " db=" << expectedDb << '\n';
        }
    }
}

void linkedStereoAndHistory() {
    for (double rate : { 44100., 48000., 96000. }) for (unsigned factor : { 2u, 4u, 8u }) {
        auto s = driven(SaturationMode::threeBand, SaturationCharacter::dense);
        Candidate a, swapped; if (!ready(a, s, rate, factor) || !ready(swapped, s, rate, factor)) continue;
        double maxSwap = 0;
        for (int n = 0; n < 8192; ++n) {
            const auto x = signal(n, rate), y = a.process(x), z = swapped.process({ x[1], x[0] });
            maxSwap = std::max(maxSwap, error(y, { z[1], z[0] }));
        }
        CHECK(maxSwap <= 1e-12);
        s.mode = SaturationMode::fullBand; s.lanes[0].driveDb = 36;
        Candidate hot, fresh; if (!ready(hot, s, rate, factor) || !ready(fresh, s, rate, factor)) continue;
        for (int n = 0; n < int(rate / 2); ++n) hot.process({ referencePeak, referencePeak / 2 });
        s.lanes[0].driveDb = 18; CHECK(hot.setSettings(s)); CHECK(fresh.setSettings(s));
        double initialDifference = 0, recoveredDifference = 0;
        for (int n = 0; n < int(1.1 * rate); ++n) {
            const Frame x { referencePeak / 16, referencePeak / 32 };
            const auto difference = error(hot.process(x), fresh.process(x));
            if (n < int(.020 * rate)) initialDifference = std::max(initialDifference, difference);
            if (n >= int(rate)) recoveredDifference = std::max(recoveredDifference, difference);
        }
        CHECK(initialDifference > 1e-7 * referencePeak);
        CHECK(recoveredDifference <= 1e-5 * referencePeak);
    }
    for (unsigned factor : { 1u, 2u }) {
        auto s = driven(SaturationMode::fullBand, SaturationCharacter::dense);
        Candidate active, bypassed; if (!ready(active, s, 48000, factor)) continue;
        s.lanes[0].bypassed = true; if (!ready(bypassed, s, 48000, factor)) continue;
        for (int n = 0; n < 24000; ++n) { active.process(signal(n)); bypassed.process(signal(n)); }
        s.lanes[0].bypassed = false; CHECK(bypassed.setSettings(s));
        double initialNull = 0, settledNull = 0;
        for (int n = 24000; n < 74048; ++n) {
            const double difference = error(active.process(signal(n)), bypassed.process(signal(n)));
            if (n < 24960) initialNull = std::max(initialNull, difference);
            if (n >= 72000) settledNull = std::max(settledNull, difference);
        }
        // Factor1 directly isolates lane history; oversampled returns also retain different prior wet/dry samples.
        if (factor == 1) CHECK(initialNull <= 1e-10);
        CHECK(settledNull <= 1e-10);
        std::cout << "BYPASS_RETURN factor=" << factor << " first20ms_null=" << initialNull << " after1s_null=" << settledNull << '\n';
    }
}

void logarithmicFrequencyRamp() {
    SaturationSettings s; s.preLowCutEnabled = true; s.preLowCutHz = 40;
    Candidate c; if (!ready(c, s, 48000, 1)) return;
    s.preLowCutHz = 2000; CHECK(c.setSettings(s));
    constexpr int rampFrames = 960;
    for (int n = 0; n < rampFrames / 2 - 1; ++n) c.process({});
    const double midpoint = std::sqrt(40. * 2000.), k = std::tan(pi * midpoint / 48000);
    const double expected = .005 / (1 + std::sqrt(2.) * k + k * k);
    const auto actual = c.process({ .01, -.01 });
    CHECK(std::abs(actual[0] - expected) <= 1e-8);
    CHECK(std::abs(actual[1] + expected) <= 1e-8);
}

void neutralTiltDiscardsOldAudioHistory() {
    SaturationSettings s; s.driveCompensation = false; s.lanes[0].preTiltDb = 6;
    Candidate c; if (!ready(c, s, 48000, 1)) return;
    for (int n = 0; n < 4800; ++n) c.process(signal(n));
    s.lanes[0].preTiltDb = 0; CHECK(c.setSettings(s));
    for (int n = 4800; n < 5760; ++n) c.process(signal(n));
    for (int n = 0; n < 48000; ++n) c.process({});
    s.lanes[0].preTiltDb = 6; CHECK(c.setSettings(s));
    double revivedPeak = 0;
    for (int n = 0; n < 2048; ++n) revivedPeak = std::max(revivedPeak, error(c.process({}), {}));
    CHECK(revivedPeak <= 1e-12);
    std::cout << "NEUTRAL_TILT revived_silent_history_peak=" << revivedPeak << '\n';
}

void resetPartitionAndRefusal() {
    for (auto mode : { SaturationMode::fullBand, SaturationMode::threeBand }) for (auto character : characters) {
        auto s = driven(mode, character); s.preLowCutEnabled = true; s.postHighCutEnabled = true;
        s.lanes[0].preTiltDb = 4; s.lanes[2].preTiltDb = -3;
        Candidate a, b; if (!ready(a, s) || !ready(b, s)) continue;
        std::vector<Frame> expected(4096);
        for (int n = 0; n < int(expected.size()); ++n) expected[std::size_t(n)] = a.process(signal(n));
        double maxNull = 0; int cursor = 0;
        for (int block : { 1, 17, 64, 512, 3502 }) for (int n = 0; n < block; ++n, ++cursor)
            maxNull = std::max(maxNull, error(expected[std::size_t(cursor)], b.process(signal(cursor))));
        CHECK(cursor == int(expected.size())); CHECK(maxNull == 0);
        a.reset();
        for (int n = 0; n < int(expected.size()); ++n) CHECK(error(expected[std::size_t(n)], a.process(signal(n))) <= 1e-12);
        for (Frame bad : { Frame{ std::numeric_limits<double>::quiet_NaN(), 0 }, Frame{ 0, std::numeric_limits<double>::infinity() } }) {
            const auto refused = a.process(bad); CHECK(finite(refused) && error(refused, {}) == 0);
            b.reset(); double recoveryNull = 0;
            for (int n = 0; n < 2048; ++n) recoveryNull = std::max(recoveryNull, error(a.process(signal(n)), b.process(signal(n))));
            CHECK(recoveryNull <= 1e-12);
        }
        CHECK(finite(a.process({ std::numeric_limits<double>::max(), -std::numeric_limits<double>::max() })));
        bool finiteRecovery = true;
        for (int n = 0; n < 4096; ++n) finiteRecovery = finite(a.process(signal(n))) && finiteRecovery;
        CHECK(finiteRecovery);
        a.reset(); b.reset();
        double resetNull = 0;
        for (int n = 0; n < 2048; ++n) resetNull = std::max(resetNull, error(a.process(signal(n)), b.process(signal(n))));
        CHECK(resetNull <= 1e-12);
    }
}

#if defined(_MSC_VER) && defined(_DEBUG)
thread_local bool counting = false;
thread_local std::size_t allocations = 0, frees = 0;
int __cdecl heapHook(int operation, void*, std::size_t, int type, long, const unsigned char*, int) {
    if (counting && type != _CRT_BLOCK) {
        if (operation == _HOOK_FREE) ++frees; else if (operation == _HOOK_ALLOC || operation == _HOOK_REALLOC) ++allocations;
    }
    return 1;
}
#endif
void preparedHeap() {
    Candidate c; auto s = driven(SaturationMode::threeBand, SaturationCharacter::glow);
    if (!ready(c, s, 48000, 8)) return;
    c.process({});
#if defined(_MSC_VER) && defined(_DEBUG)
    allocations = frees = 0; const auto old = _CrtSetAllocHook(heapHook); counting = true;
#endif
    bool admitted = true, finiteOutput = true; double energy = 0;
    for (int block = 0; block < 200; ++block) {
        s.mode = block % 2 ? SaturationMode::fullBand : SaturationMode::threeBand;
        s.lowCrossoverHz = float(40 + 9 * block); s.highCrossoverHz = float(500 + 70 * block);
        s.preLowCutEnabled = block % 3 == 0; s.postHighCutEnabled = block % 4 == 0;
        for (auto& lane : s.lanes) {
            lane.character = characters[std::size_t(block) % characters.size()]; lane.driveDb = float(block % 37);
            lane.preTiltDb = float(block % 13 - 6); lane.mix = float(block % 11) / 10; lane.bypassed = block % 7 == 0;
        }
        admitted = c.setSettings(s) && admitted;
        auto invalid = s; invalid.lanes[3].driveDb = std::numeric_limits<float>::quiet_NaN();
        admitted = !c.setSettings(invalid) && admitted;
        for (int n = 0; n < 64; ++n) { const auto y = c.process(signal(block * 64 + n)); finiteOutput = finiteOutput && finite(y); energy += y[0] * y[0]; }
        if (block % 23 == 0) c.reset();
    }
#if defined(_MSC_VER) && defined(_DEBUG)
    counting = false; _CrtSetAllocHook(old);
    CHECK(allocations == 0 && frees == 0);
    std::cout << "SATURATION_CALLBACK allocations=" << allocations << " frees=" << frees << '\n';
#else
    std::cout << "SATURATION_CALLBACK heap audit unavailable in this configuration\n";
#endif
    CHECK(admitted); CHECK(finiteOutput && energy > 0);
}
}

int main() {
    group("settings_validation_and_effective_clamps", validation);
    group("transactional_prepare_and_settings", transactionalAdmission);
    group("neutral_and_aligned_dry_references", neutralReferences);
    group("lr4_product_allpass_and_unity", crossoverPhase);
    group("original_scalar_laws_and_adaa", scalarLaws);
    group("dense_sag_closed_form_1e", sagClosedForm);
    group("dense_integrated_max_stereo_sag_oracle", denseIntegrationOracle);
    group("integrated_primitive_history_scalar_oracle", integratedPrimitiveHistoryOracle);
    group("fixed_drive_compensation_calibration", compensationCalibration);
    group("dense_linked_stereo_and_retained_history", linkedStereoAndHistory);
    group("continuous_frequency_20ms_log_hz", logarithmicFrequencyRamp);
    group("neutral_tilt_retires_old_audio_history", neutralTiltDiscardsOldAudioHistory);
    group("reset_partition_and_nonfinite_refusal", resetPartitionAndRefusal);
    group("prepared_process_and_settings_heap", preparedHeap);
    std::cout << "Saturation probe: " << checks << " checks, " << sections << " sections, " << failures << " failures\n";
    return failures ? 1 : 0;
}

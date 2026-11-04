#include "SaturationCandidate.h"
#include "SaturationQFixtures.h"
#include "selector-boundaries/SaturationQSelectorFixtures.h"
#include "contracts/SaturationNumericalWitness.h"
#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
#include <sstream>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
double operand(const juce::var& value) {
    const auto text = value.toString().toStdString(); char* end = nullptr;
    require(text.starts_with("0x") || text.starts_with("-0x"), "exact hex fixture operand required");
    const double result = std::strtod(text.c_str(), &end);
    require(end == text.c_str() + text.size() && std::isfinite(result), "finite hex fixture operand required");
    return result;
}
bool same(double x, double y) { return std::bit_cast<std::uint64_t>(x) == std::bit_cast<std::uint64_t>(y); }
std::string jsonNumber(double value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream text; text << std::setprecision(17) << value; return text.str();
}
double scalarQuotient(airack::SaturationCharacter c, double u, double p) noexcept {
#if defined(AIRACK_SATURATION_NUMERICAL_ORIGINAL_EXPRESSION)
    const double delta = u - p;
    return std::abs(delta) < 1e-6 * (1 + std::max(std::abs(u), std::abs(p)))
        ? airack::probe::shape(c, .5 * (u + p))
        : (airack::probe::primitive(c, u) - airack::probe::primitive(c, p)) / delta;
#else
    return airack::probe::adaa(c, u, p);
#endif
}
#if defined(AIRACK_SATURATION_NUMERICAL_ORIGINAL_EXPRESSION)
constexpr auto evaluatorName = "original-primitive-expression-control";
#else
constexpr auto evaluatorName = "repaired-current";
#endif
}
int main(int argc, char** argv) {
    try {
        using namespace airack; using namespace airack::probe;
        using namespace airack::probe::numerical;
        for (const double value : { 1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
            juce::var parsed;
            require(juce::JSON::parse(juce::String("{\"error\":") + jsonNumber(value) + "}", parsed).wasOk(), "JSON number serialization");
            require(std::isfinite(value) ? double(parsed["error"]) == value : parsed["error"].isVoid(), "JSON nonfinite serialization status");
        }
        std::map<std::string, std::string> flags;
        for (int n = 1; n < argc; n += 2) {
            require(n + 1 < argc && (std::string(argv[n]) == "--fixtures" || std::string(argv[n]) == "--output"), "fixture/output flags required");
            require(flags.emplace(argv[n], argv[n + 1]).second, "duplicate flag");
        }
        const std::filesystem::path fixture = flags.contains("--fixtures") ? flags.at("--fixtures") : RACKSYNTH_Q_FIXTURES;
        const auto output = flags.contains("--output") ? std::filesystem::path(flags.at("--output"))
            : std::filesystem::path(juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("saturation-numerical", "", false).getFullPathName().toStdString());
        require(!std::filesystem::exists(output), "fresh output directory required");
        const auto raw = juce::File(juce::String(fixture.string())).loadFileAsString();
        juce::var packet; require(juce::JSON::parse(raw, packet).wasOk(), "fixture JSON required");
        require(packet["schema"].toString() == "saturation-q-fixtures-v1" && packet["oracle_identity"]["sha256"].toString() == fixtureIdentity,
                "fixture/source identity required");
        const auto* rows = packet["rows"].getArray();
        require(rows && rows->size() == int(std::size(qFixtures)) && int(packet["count"]) == rows->size()
            && int(packet["pair_count"]) == 167 && int(packet["retained_scalar_count"]) == 334, "fixture counts required");
        require(same(std::tanh(.35), fixtureBias) && same(1 - fixtureBias * fixtureBias, fixtureNormalizer), "native Glow constants differ");
        double directBias, preparedBias, preparedNormalizer; std::size_t zeroHalfWidths;
        saturationNumericalConstants(directBias, preparedBias, preparedNormalizer, zeroHalfWidths);
        require(same(directBias, fixtureBias) && same(preparedBias, fixtureBias) && same(preparedNormalizer, fixtureNormalizer), "prepared native constants differ");
        std::size_t readonlyFailures = 0;
        for (const auto& row : *rows) {
            require(row["character"].isInt() && int(row["character"]) >= 0 && int(row["character"]) <= 2
                    && row["kind"].isInt() && int(row["kind"]) >= 0 && int(row["kind"]) <= 2
                    && row["midpoint"].isBool() && row["preparedExact"].isBool(), "valid character/kind/branch enum required");
            operand(row["u"]); operand(row["p"]); operand(row["expected"]);
            const auto c = static_cast<SaturationCharacter>(int(row["character"]));
            const double actual = scalarQuotient(c, operand(row["u"]), operand(row["p"]));
            if (!std::isfinite(actual) || std::abs(actual - operand(row["expected"])) > 4e-15) ++readonlyFailures;
        }
        if (juce::SHA256(raw.toRawUTF8(), raw.getNumBytesAsUTF8()).toHexString() != RACKSYNTH_Q_FILE_SHA) {
            std::cout << "FIXTURE_MUTATION real_comparison_failures=" << readonlyFailures << '\n';
            if (readonlyFailures) return 1;
            require(false, "fixture hash/source identity differs");
        }
        const auto selectorRaw = juce::File(RACKSYNTH_Q_SELECTOR_FIXTURES).loadFileAsString(); juce::var selectorPacket;
        require(juce::JSON::parse(selectorRaw, selectorPacket).wasOk()
            && juce::SHA256(selectorRaw.toRawUTF8(), selectorRaw.getNumBytesAsUTF8()).toHexString() == RACKSYNTH_Q_SELECTOR_SHA
            && selectorPacket["schema"].toString() == "saturation-selector-fixtures-v1"
            && selectorPacket["oracle_identity"]["sha256"].toString() == selectorFixtureIdentity
            && selectorPacket["original_fixture"]["sha256"].toString() == RACKSYNTH_Q_FILE_SHA, "selector fixture/source identity");
        const auto* selectorRows = selectorPacket["rows"].getArray();
        static_assert(std::size(selectorFixtures) == 72);
        require(selectorRows && selectorRows->size() == 72 && int(selectorPacket["count"]) == 72, "selector fixture counts");
        for (int n = 0; n < 72; ++n) {
            const auto& row = selectorRows->getReference(n); const auto& bound = selectorFixtures[n];
            require(int(row["character"]) == bound.q.character && same(operand(row["u"]), bound.q.u)
                && same(operand(row["p"]), bound.q.p) && same(operand(row["expected"]), bound.q.expected)
                && same(operand(row["epsilon"]), bound.epsilon) && int(row["relation"]) == bound.relation
                && bool(row["midpoint"]) == bound.q.midpoint && bool(row["preparedExact"]) == bound.q.preparedExact, "selector compiled binding");
        }
        std::filesystem::create_directories(output);
        std::ofstream values(output / "scalars.csv"), failed(output / "failures.csv");
        values << "index,character,u,p,expected,actual,prepared,midpoint,prepared_exact\n";
        failed << "index,character,u,p,expected,actual,absolute_error,origin\n";
        std::size_t failures = 0, retainedFailures = 0, bindings = 0; double maximum = 0;
        std::vector<double> actuals;
        for (int index = 0; index < rows->size(); ++index) {
            const auto& row = rows->getReference(index); const auto& bound = qFixtures[index];
            const int character = int(row["character"]); const double u = operand(row["u"]), p = operand(row["p"]), expected = operand(row["expected"]);
            const auto c = static_cast<SaturationCharacter>(character);
            const double actual = scalarQuotient(c, u, p), prepared = preparedAdaa(c, u, p);
            const double error = std::abs(actual - expected);
            const bool okay = std::isfinite(actual) && error <= 4e-15
                && (!bool(row["preparedExact"]) || (std::isfinite(prepared) && std::abs(prepared - expected) <= 4e-15));
            if (!okay) {
                ++failures; if (row["origin"].toString() == "retained") ++retainedFailures;
                failed << index << ',' << character << ',' << std::hexfloat << u << ',' << p << ',' << expected << ',' << actual << ',' << std::defaultfloat << error << ',' << row["origin"].toString() << '\n';
            }
            if (character != bound.character || !same(u, bound.u) || !same(p, bound.p) || !same(expected, bound.expected)
                || bool(row["midpoint"]) != bound.midpoint || bool(row["preparedExact"]) != bound.preparedExact) ++bindings;
            maximum = std::max(maximum, error); actuals.push_back(actual);
            values << index << ',' << character << ',' << std::hexfloat << u << ',' << p << ',' << expected << ',' << actual << ',' << prepared << ',' << bool(row["midpoint"]) << ',' << bool(row["preparedExact"]) << '\n';
        }
        std::size_t lipschitz = 0;
        for (std::size_t pair = 0; pair < 167; ++pair) {
            const auto& a = qFixtures[2 * pair]; const auto& b = qFixtures[2 * pair + 1];
            const double bound = std::abs(a.u - b.u) + std::abs(a.p - b.p) + 8e-15;
            if (!(std::abs(actuals[2 * pair] - actuals[2 * pair + 1]) <= bound)) ++lipschitz;
        }
        std::size_t selectorFailures = 0; double selectorMaximum = 0;
        std::array<std::size_t, 3> relations {};
        std::ofstream selectors(output / "selectors.csv");
        selectors << "index,character,u,p,epsilon,relation,expected,actual,prepared,midpoint,prepared_exact,error\n";
        for (std::size_t n = 0; n < std::size(selectorFixtures); ++n) {
            const auto& s = selectorFixtures[n]; const auto& w = s.q;
            const auto c = static_cast<SaturationCharacter>(w.character);
            const double delta = std::abs(w.u - w.p), threshold = s.epsilon * (1 + std::max(std::abs(w.u), std::abs(w.p)));
            const int relation = int(delta > threshold) - int(delta < threshold);
            require(relation == s.relation, "native selector equality/neighbor predicate"); ++relations[std::size_t(relation + 1)];
            require((std::abs(w.u - w.p) < 1e-6 * (1 + std::max(std::abs(w.u), std::abs(w.p)))) == w.midpoint, "native midpoint selector branch");
            require(preparedAdaaUsesExactPrimitive(w.u, w.p) == w.preparedExact, "native prepared selector branch");
            const double actual = scalarQuotient(c, w.u, w.p), prepared = preparedAdaa(c, w.u, w.p);
            const double error = std::abs(actual - w.expected); selectorMaximum = std::max(selectorMaximum, error);
            if (!std::isfinite(actual) || error > 4e-15 || !std::isfinite(prepared)
                || std::abs(prepared - w.expected) > (w.preparedExact ? 4e-15 : 16e-12)) ++selectorFailures;
            selectors << n << ',' << w.character << ',' << std::hexfloat << w.u << ',' << w.p << ',' << s.epsilon << ',' << relation << ','
                << w.expected << ',' << actual << ',' << prepared << ',' << w.midpoint << ',' << w.preparedExact << ',' << std::defaultfloat << error << '\n';
        }
        require(relations == std::array<std::size_t, 3>{24, 24, 24}, "native selector relation coverage");
        for (const auto c : { SaturationCharacter::soft, SaturationCharacter::glow, SaturationCharacter::dense }) {
            for (const double bad : { std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() }) {
                require(!std::isfinite(adaa(c, bad, .1)) && !std::isfinite(preparedAdaa(c, bad, .1)), "nonfinite standalone invalid path");
            }
            const double extreme = std::numeric_limits<double>::max();
            require(!preparedAdaaUsesExactPrimitive(extreme, -extreme), "extreme curve selection changed");
            const double originalCurve = (preparedPrimitive(c, extreme) - preparedPrimitive(c, -extreme)) / (extreme - -extreme);
            const double actualCurve = preparedAdaa(c, extreme, -extreme);
            require(std::isnan(originalCurve) ? std::isnan(actualCurve) : same(originalCurve, actualCurve), "original extreme curve invalid/sanitize path changed");
        }
        const int pathFailures = runSaturationNumericalPathChecks();
        const int productStateFailures = runSaturationProductStateIntegrationChecks();
        std::ofstream paths(output / "paths.csv");
        paths << "lane,site,character,channel,kind,previous_kind,u,p,q,current,prior,cached\n";
        for (const auto& row : saturationNumericalRecords())
            paths << row.lane << ',' << row.site << ',' << row.character << ',' << row.channel << ',' << row.kind << ',' << row.previousKind << ','
                  << std::hexfloat << row.u << ',' << row.p << ',' << row.q << ',' << row.currentPrimitive << ',' << row.priorPrimitive << ',' << row.cachedPrimitive << '\n';
        saturationNumericalConstants(directBias, preparedBias, preparedNormalizer, zeroHalfWidths);
        require(zeroHalfWidths == 0, "exact helper received zero halfwidth");
        std::ofstream summary(output / "summary.json");
        summary << "{\"count\":" << rows->size() << ",\"failures\":" << failures << ",\"retained_failures\":" << retainedFailures
            << ",\"binding_failures\":" << bindings << ",\"lipschitz_failures\":" << lipschitz << ",\"path_failures\":" << pathFailures
            << ",\"product_state_failures\":" << productStateFailures
            << ",\"selector_count\":72,\"selector_failures\":" << selectorFailures << ",\"selector_maximum_error\":" << jsonNumber(selectorMaximum)
            << ",\"total_scalar_count\":" << rows->size() + 72 << ",\"evaluator\":\"" << evaluatorName << "\""
            << ",\"max_absolute_error\":" << jsonNumber(maximum) << ",\"max_error_nonfinite\":" << (!std::isfinite(maximum) ? "true" : "false") << ",\"source_sha256\":\"" << RACKSYNTH_Q_SOURCE_SHA
            << "\",\"bias\":\"" << std::hexfloat << std::tanh(.35) << "\",\"normalizer\":\"" << fixtureNormalizer << "\"}\n";
        std::cout << "q_checks=" << rows->size() << " failures=" << failures << " retained_failures=" << retainedFailures
            << " bindings=" << bindings << " lipschitz_failures=" << lipschitz << " max_error=" << std::setprecision(17) << maximum
            << " selector_checks=72 selector_failures=" << selectorFailures << " selector_max_error=" << selectorMaximum << " evaluator=" << evaluatorName
            << " product_state_failures=" << productStateFailures << '\n';
        return failures + bindings + lipschitz + pathFailures + selectorFailures + productStateFailures == 0 ? 0 : 1;
    } catch (const std::exception& e) { std::cerr << "REFUSED " << e.what() << '\n'; return 2; }
}

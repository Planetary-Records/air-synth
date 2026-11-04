#include "SaturationProductState.h"
#include <juce_cryptography/juce_cryptography.h>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using airack::product_state::Coefficients;
using airack::product_state::State;
constexpr int rows = 216, frames = 512, channels = 2;
const std::string columns = "row,frame,channel,control,y,ylow,z1,z1low,z2,z2low";
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
struct Options { fs::path input, output; };
Options parse(const std::vector<std::string>& args) {
    require(args.size() == 4 && args[0] == "--linear-fixtures" && args[2] == "--output",
            "expected --linear-fixtures ABSOLUTE_ORIGINAL_JSON --output FRESH_ABSOLUTE_DIRECTORY");
    Options result { fs::path(args[1]), fs::path(args[3]) };
    require(result.input.is_absolute() && result.output.is_absolute(), "absolute input/output required");
    require(fs::is_regular_file(result.input) && !fs::exists(result.output), "missing input or existing output");
    return result;
}
std::vector<char> read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    const auto length = stream.tellg();
    require(length >= 0 && length <= std::numeric_limits<int>::max(), "input extent");
    std::vector<char> result(static_cast<std::size_t>(length)); stream.seekg(0);
    stream.read(result.data(), static_cast<std::streamsize>(result.size()));
    stream.close(); return result;
}
juce::var json(const std::vector<char>& bytes) {
    juce::var result;
    require(juce::JSON::parse(juce::String::fromUTF8(bytes.data(), int(bytes.size())), result).wasOk()
            && result.isObject(), "invalid JSON object");
    return result;
}
std::string text(const juce::var& value) {
    require(value.isString(), "JSON string required"); return value.toString().toStdString();
}
void extent(const juce::var& value, int size) {
    require(value.isArray() && value.size() == size, "JSON array extent");
}
bool integer(const juce::var& value, int expected) {
    return (value.isInt() || value.isInt64()) && static_cast<juce::int64>(value) == expected;
}
double hex(const juce::var& value) {
    const auto string = text(value);
    static const std::regex pattern("-?0x[0-9a-f]+(\\.[0-9a-f]*)?p[+-]?[0-9]+");
    require(std::regex_match(string, pattern), "finite hexadecimal binary64 required");
    char* end = nullptr;
    const auto result = std::strtod(string.c_str(), &end);
    require(end == string.c_str() + string.size() && std::isfinite(result)
            && (result == 0.0 || std::isnormal(result)), "finite normal or zero required");
    return result;
}
struct Row { Coefficients c; std::array<std::array<double, channels>, frames> input; };
Row parseRow(const juce::var& value, int index) {
    static constexpr const char* sites[] = { "lowLP", "lowHP", "highLP", "highHP", "highAP", "pre", "post", "shelfLow", "shelfHigh" };
    static constexpr const char* sequences[] = { "step", "constant-stereo", "alternating-stereo", "charged256-then-zero" };
    static constexpr int rates[] = { 44100, 48000, 96000 };
    require(value.isObject() && value.getDynamicObject()->getProperties().size() == 10
            && integer(value["index"], index) && integer(value["factor"], index < 108 ? 4 : 64)
            && integer(value["rate"], rates[(index / 36) % 3])
            && text(value["site"]) == sites[(index / 4) % 9]
            && text(value["sequence"]) == sequences[index % 4], "original row metadata/order");
    const auto coefficients = value["coefficients"], input = value["inputs"];
    extent(coefficients, 5); extent(input, frames);
    Row row { { hex(coefficients[0]), hex(coefficients[1]), hex(coefficients[2]), hex(coefficients[3]), hex(coefficients[4]) }, {} };
    for (int n = 0; n < frames; ++n) {
        extent(input[n], channels);
        for (int ch = 0; ch < channels; ++ch) row.input[n][ch] = hex(input[n][ch]);
    }
    const auto original = value["original"], expected = value["expected"], maximum = value["control_max"];
    extent(original, channels); extent(expected, channels); extent(maximum, channels);
    for (int ch = 0; ch < channels; ++ch) {
        extent(original[ch], frames); extent(expected[ch], frames); text(maximum[ch]);
        for (int n = 0; n < frames; ++n) { hex(original[ch][n]); text(expected[ch][n]); }
    }
    return row;
}
std::vector<Row> fixtures(const juce::var& packet) {
    require(packet.isObject() && packet.getDynamicObject()->getProperties().size() == 9
            && text(packet["schema"]) == "saturation-front-recurrence-v1"
            && integer(packet["updates"], frames) && integer(packet["sets"], 6), "original fixture shape");
    extent(packet["precisions"], 2);
    require(integer(packet["precisions"][0], 120) && integer(packet["precisions"][1], 180), "original MP precisions");
    extent(packet["rows"], rows);
    std::vector<Row> result; result.reserve(rows);
    for (int i = 0; i < rows; ++i) result.push_back(parseRow(packet["rows"][i], i));
    return result;
}
struct Control {
    double z1 = 0.0, z2 = 0.0;
    double step(double x, const Coefficients& c) noexcept {
        const double y = c.b0 * x + z1;
        const double n1 = (c.b1 * x - c.a1 * y) + z2;
        const double n2 = c.b2 * x - c.a2 * y;
        z1 = n1; z2 = n2;
        return y;
    }
};
juce::var identity(const fs::path& path) {
    const auto bytes = read(path);
    auto* result = new juce::DynamicObject;
    result->setProperty("bytes", juce::int64(bytes.size()));
    result->setProperty("sha256", juce::SHA256(bytes.data(), bytes.size()).toHexString());
    return result;
}
struct Writer {
    std::ofstream stream;
    explicit Writer(const fs::path& path) {
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        try { stream.open(path, std::ios::binary); }
        catch (const std::ios_base::failure&) { throw std::runtime_error("capture IO open failure"); }
        stream.imbue(std::locale::classic());
    }
    void finish() { stream.flush(); stream.close(); }
};
void writeJson(const fs::path& path, const juce::var& value) {
    Writer file(path); file.stream << juce::JSON::toString(value).toStdString() << '\n'; file.finish();
}
void capture(const Options& options, const std::vector<Row>& input, const juce::var& corpusIdentity) {
    require(!fs::exists(options.output), "fresh output required");
    // All writes remain under an ineligible ancestor until one final directory rename.
    const auto staging = options.output.parent_path() / ("." + options.output.filename().string() + "-ineligible-" + juce::Uuid().toString().toStdString());
    require(fs::create_directory(staging), "fresh staging creation failed");
    auto* invalid = new juce::DynamicObject;
    invalid->setProperty("status", "INELIGIBLE"); invalid->setProperty("accepted", false);
    invalid->setProperty("scope", "This staging envelope and all descendants are never accepted output. Only the requested target is authoritative after atomic publication.");
    writeJson(staging / "invalid.json", invalid);
    const auto complete = staging / "complete";
    require(fs::create_directory(complete), "capture directory creation failed");
    const auto csv = complete / "native.csv";
    Writer file(csv); file.stream << columns << '\n' << std::hexfloat;
    std::size_t records = 0;
    for (int r = 0; r < rows; ++r) {
        std::array<State, channels> candidate {};
        std::array<Control, channels> control {};
        for (int n = 0; n < frames; ++n) for (int ch = 0; ch < channels; ++ch) {
            const auto& row = input[r]; auto& state = candidate[ch];
            const double y = state.step(row.input[n][ch], row.c);
            require(std::bit_cast<std::uint64_t>(y) == std::bit_cast<std::uint64_t>(state.y.h), "returned y/state mismatch");
            const std::array<double, 7> values { control[ch].step(row.input[n][ch], row.c), y, state.y.l,
                                               state.z1.h, state.z1.l, state.z2.h, state.z2.l };
            file.stream << r << ',' << n << ',' << ch;
            for (double value : values) {
                require(std::isfinite(value) && (value == 0.0 || std::isnormal(value)), "nonfinite/subnormal capture");
                file.stream << ',' << value;
            }
            file.stream << '\n'; ++records;
        }
    }
    file.finish();
    require(records == std::size_t(rows * frames * channels), "complete record extent");
    auto* metadata = new juce::DynamicObject;
    metadata->setProperty("schema", "saturation-product-state-native-capture-v1"); metadata->setProperty("status", "complete");
    metadata->setProperty("stereo_rows", rows); metadata->setProperty("updates", frames); metadata->setProperty("channels", channels);
    metadata->setProperty("records", juce::int64(records)); metadata->setProperty("corpus", corpusIdentity);
    juce::Array<juce::var> names;
    for (const auto& name : juce::StringArray::fromTokens(columns, ",", "")) names.add(name);
    metadata->setProperty("columns", juce::var(names)); metadata->setProperty("csv", identity(csv));
    writeJson(complete / "capture.json", metadata);
    // No fallible success reporting or cleanup may follow this publication.
    fs::rename(complete, options.output);
}
template <typename Function> void refuses(Function function, const char* expected) {
    try { function(); } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, "self-test wrong refusal boundary"); return;
    }
    throw std::runtime_error("self-test expected refusal");
}
juce::var syntheticRow() {
    auto* row = new juce::DynamicObject;
    row->setProperty("index", 0); row->setProperty("factor", 4); row->setProperty("rate", 44100);
    row->setProperty("site", "lowLP"); row->setProperty("sequence", "step");
    juce::Array<juce::var> coefficients, input, original, expected, maxima, values;
    for (int i = 0; i < 5; ++i) coefficients.add("0x0p+0");
    juce::Array<juce::var> pair; pair.add("0x0p+0"); pair.add("-0x0p+0");
    for (int i = 0; i < frames; ++i) { input.add(juce::var(pair)); values.add("0x0p+0"); }
    for (int ch = 0; ch < channels; ++ch) { original.add(juce::var(values)); expected.add(juce::var(values)); maxima.add("0"); }
    row->setProperty("coefficients", juce::var(coefficients)); row->setProperty("inputs", juce::var(input));
    row->setProperty("original", juce::var(original)); row->setProperty("expected", juce::var(expected));
    row->setProperty("control_max", juce::var(maxima)); return row;
}
void selfTest(const fs::path& directory) {
    require(directory.is_absolute() && !fs::exists(directory) && fs::create_directory(directory), "fresh absolute self-test directory required");
    Writer tiny(directory / "tiny.json"); tiny.stream << "{}\n"; tiny.finish();
    const std::vector<std::string> valid { "--linear-fixtures", (directory / "tiny.json").string(), "--output", (directory / "output").string() };
    parse(valid);
    for (auto args : std::vector<std::vector<std::string>> { {}, { "--output" },
            { "--linear-fixtures", valid[1], "--linear-fixtures", valid[3] },
            { "--linear-fixtures", valid[1], "--output", valid[3], "--output", valid[3] } })
        refuses([&] { parse(args); }, "expected --linear-fixtures");
    auto relative = valid; relative[1] = "relative.json";
    refuses([&] { parse(relative); }, "absolute");
    auto existing = valid; existing[3] = directory.string();
    refuses([&] { parse(existing); }, "existing output");
    for (auto value : { "0", "nan", "inf", "0x1p+9999", "0x1p-1074", "0x1", " 0x1p+0" })
        refuses([&] { hex(juce::String(value)); }, "finite");
    require(hex("0x1p-1") == .5 && std::signbit(hex("-0x0p+0")), "hex/signed-zero self-test");
    refuses([] { json(std::vector<char> { '{' }); }, "invalid JSON");
    refuses([&] { fixtures(json(read(directory / "tiny.json"))); }, "fixture shape");
    juce::var empty(juce::Array<juce::var> {});
    refuses([&] { extent(empty, frames); }, "extent");
    refuses([&] { parseRow(juce::var(new juce::DynamicObject), 0); }, "row metadata");
    parseRow(syntheticRow(), 0);
    for (const auto* field : { "coefficients", "inputs", "original", "expected", "control_max" }) {
        auto row = syntheticRow(); row.getDynamicObject()->setProperty(field, empty);
        refuses([&] { parseRow(row, 0); }, "extent");
    }
    for (const auto* value : { "nan", "0", "0x1p+9999" }) {
        auto row = syntheticRow(); row["coefficients"].getArray()->set(0, value);
        refuses([&] { parseRow(row, 0); }, "finite");
    }
    refuses([&] { Writer impossible(directory / "missing" / "file.csv"); }, "capture IO open failure");
    Writer blocked(directory / "blocked"); blocked.stream << 'x'; blocked.finish();
    refuses([&] { fs::create_directory(directory / "blocked" / "child"); }, "directory");
    Writer result(directory / "self-test.txt"); result.stream << "PASS parser/shape/extent/hex/signedzero/real-IO; no DSP arithmetic\n"; result.finish();
}
}
int main(int argc, char** argv) {
    try {
        require(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53
                && std::fegetround() == FE_TONEAREST, "binary64 nearest environment required");
        if (argc == 3 && std::string(argv[1]) == "--self-test") {
            selfTest(fs::path(argv[2])); return 0;
        }
        std::vector<std::string> args(argv + 1, argv + argc);
        const auto options = parse(args);
        const auto bytes = read(options.input);
        const auto input = fixtures(json(bytes));
        auto corpus = identity(options.input);
        corpus.getDynamicObject()->setProperty("path", juce::String(options.input.string()));
        capture(options, input, corpus);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "INVALID: " << error.what() << '\n'; return 1;
    }
}

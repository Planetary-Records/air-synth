#include "DrumPreparation.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <iostream>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <limits>
#include <numbers>
#include <thread>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <malloc.h>
#endif
#include <vector>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
using namespace airack::drums;
namespace
{
using Bytes = std::vector<std::uint8_t>;
int checks = 0, failures = 0, sections = 0;
const char *section = "literal WAV";
bool check(bool p, const char *text, int line)
{
    ++checks;
    if (!p)
    {
        ++failures;
        if (failures < 200)
            std::cerr << section << ':' << line << ' ' << text << '\n';
    }
    return p;
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void begin(const char *name)
{
    section = name;
    ++sections;
    std::cout << name << '\n';
}
void number(Bytes &b, std::uint64_t n, unsigned width)
{
    for (unsigned i = 0; i < width; ++i)
        b.push_back(std::uint8_t(n >> (8 * i)));
}
void big(Bytes &b, std::uint64_t n, unsigned width)
{
    for (unsigned i = width; i--;)
        b.push_back(std::uint8_t(n >> (8 * i)));
}
void put(Bytes &b, std::size_t p, std::uint64_t n, unsigned width)
{
    for (unsigned i = 0; i < width; ++i)
        b[p + i] = std::uint8_t(n >> (8 * i));
}
void tag(Bytes &b, const char *s)
{
    b.insert(b.end(), s, s + 4);
}
Bytes wav(std::span<const std::uint32_t> bits, std::uint32_t channels = 2,
          std::uint32_t rate = 48000)
{
    Bytes b;
    tag(b, "RIFF");
    number(b, 36 + bits.size() * 4, 4);
    tag(b, "WAVE");
    tag(b, "fmt ");
    number(b, 16, 4);
    number(b, 3, 2);
    number(b, channels, 2);
    number(b, rate, 4);
    number(b, rate * channels * 4, 4);
    number(b, channels * 4, 2);
    number(b, 32, 2);
    tag(b, "data");
    number(b, bits.size() * 4, 4);
    for (auto x : bits)
        number(b, x, 4);
    return b;
}
void literal(const juce::File &root)
{
    begin("literal WAV signed-zero unity");
    const std::array<std::uint32_t, 8> bits{0x3f000000, 0x3e000000, 0x80000000, 0,
                                            0x3e800000, 0xbf000000, 0x3f400000, 0xbe800000};
    const auto file = root.getChildFile("literal.wav");
    const auto b = wav(bits);
    CHECK(file.replaceWithData(b.data(), b.size()));
    std::atomic_bool cancel{false};
    PreparationError error;
    std::unique_ptr<DrumSource> source;
    CHECK(inspectDrumSource(file, std::nullopt, cancel, source, error));
    if (!CHECK(source != nullptr))
        return;
    PreparedDrumSample prepared;
    CHECK(prepareDrumSample(*source, 1, 3, 1.f, cancel, prepared, error));
    if (!CHECK(prepared.asset != nullptr))
        return;
    CHECK(prepared.asset->frames() == 2 && prepared.asset->channels() == 2);
    const std::array<std::uint32_t, 4> expected{0x80000000u, 0u, 0x3e800000u, 0xbf000000u};
    for (std::size_t i = 0; i < expected.size(); ++i)
        CHECK(std::bit_cast<std::uint32_t>(prepared.asset->samples()[i]) == expected[i]);
    CHECK(prepared.source.kind == SourceKind::imported && prepared.source.begin == 1 &&
          prepared.source.end == 3);
    CHECK(prepared.source.sourceRate == 48000 && prepared.source.gain == 1.f);
    CHECK(prepared.source.generator.empty() && prepared.source.condition.empty());
}
juce::File write(const juce::File &root, const Bytes &b)
{
    const auto f = root.getChildFile(juce::Uuid().toString() + ".audio");
    if (b.empty())
        CHECK(f.create().wasOk());
    else
        CHECK(f.replaceWithData(b.data(), b.size()));
    return f;
}
AssetId digest(std::span<const std::uint8_t> b)
{
    AssetId id{};
    const auto d = juce::SHA256(b.data(), b.size()).getRawData();
    std::memcpy(id.data(), d.getData(), 32);
    return id;
}
std::unique_ptr<DrumSource> inspect(const juce::File &f)
{
    std::unique_ptr<DrumSource> s;
    std::atomic_bool c{false};
    PreparationError e{};
    CHECK(inspectDrumSource(f, std::nullopt, c, s, e));
    if (!s)
        std::cerr << "error=" << int(e) << '\n';
    return s;
}
void rejectBeforeAllocation(const juce::File &, PreparationError);
void rejection(const juce::File &root, const Bytes &b, PreparationError expected)
{
    const auto file = write(root, b);
    if (std::strcmp(section, "container admission and corrupt bounds") == 0 ||
        std::strcmp(section, "extensible WAV AIFC FLOAT metadata padding and duration limits") ==
            0 ||
        expected == PreparationError::tooLarge)
    {
        rejectBeforeAllocation(file, expected);
        return;
    }
    std::atomic_bool c{false};
    PreparationError e{};
    std::unique_ptr<DrumSource> s;
    CHECK(!inspectDrumSource(file, std::nullopt, c, s, e));
    if (e != expected)
        std::cerr << "fixture bytes=" << b.size() << " expected=" << int(expected)
                  << " actual=" << int(e) << '\n';
    CHECK(e == expected);
    CHECK(!s);
}
Bytes pcm(std::span<const std::int32_t> samples, unsigned bits, unsigned channels, unsigned rate,
          bool aiff = false, const char *compression = nullptr)
{
    Bytes data;
    for (auto n : samples)
    {
        if (aiff && (!compression || std::string_view(compression) != "sowt"))
            big(data, std::uint32_t(n), bits / 8);
        else
            number(data, bits == 8 && !aiff ? std::uint32_t(n + 128) : std::uint32_t(n), bits / 8);
    }
    Bytes b;
    if (!aiff)
    {
        tag(b, "RIFF");
        number(b, 36 + data.size() + (data.size() & 1), 4);
        tag(b, "WAVE");
        tag(b, "fmt ");
        number(b, 16, 4);
        number(b, 1, 2);
        number(b, channels, 2);
        number(b, rate, 4);
        number(b, rate * channels * (bits / 8), 4);
        number(b, channels * (bits / 8), 2);
        number(b, bits, 2);
        tag(b, "data");
        number(b, data.size(), 4);
    }
    else
    {
        tag(b, "FORM");
        big(b, 0, 4);
        tag(b, compression ? "AIFC" : "AIFF");
        if (compression)
        {
            tag(b, "FVER");
            big(b, 4, 4);
            big(b, 0xa2805140, 4);
        }
        tag(b, "COMM");
        big(b, compression ? 24 : 18, 4);
        big(b, channels, 2);
        big(b, samples.size() / channels, 4);
        big(b, bits, 2);
        const unsigned exponent = unsigned(std::floor(std::log2(rate)));
        big(b, 16383 + exponent, 2);
        big(b, std::uint64_t(rate) << (63 - exponent), 8);
        if (compression)
        {
            tag(b, compression);
            b.push_back(0);
            b.push_back(0);
        }
        tag(b, "SSND");
        big(b, data.size() + 8, 4);
        big(b, 0, 4);
        big(b, 0, 4);
    }
    b.insert(b.end(), data.begin(), data.end());
    if (data.size() & 1)
        b.push_back(0);
    if (aiff)
    {
        const auto n = b.size() - 8;
        for (unsigned i = 0; i < 4; ++i)
            b[4 + i] = std::uint8_t(n >> (24 - 8 * i));
    }
    return b;
}
void codecs(const juce::File &root)
{
    begin("literal PCM/AIFF integer scaling and identities");
    for (unsigned bits : {8u, 16u, 24u, 32u})
        for (unsigned channels : {1u, 2u})
            for (bool aiff : {false, true})
            {
                const std::int32_t q = std::int32_t(std::uint32_t(1) << (bits - 3));
                const std::array<std::int32_t, 4> v{0, q, -q, 0};
                for (const char *comp :
                     {static_cast<const char *>(nullptr), "NONE", "twos", "sowt"})
                {
                    if (!aiff && comp)
                        continue;
                    auto s = inspect(write(root, pcm(v, bits, channels, 48000, aiff, comp)));
                    if (!s)
                        continue;
                    CHECK(s->info().frames == 4 / channels && s->info().bits == bits);
                    std::atomic_bool c{false};
                    PreparationError e{};
                    PreparedDrumSample out;
                    CHECK(prepareDrumSample(*s, 0, 4 / channels, 1, c, out, e));
                    if (!out.asset)
                        continue;
                    const std::array<float, 4> expected{0, .25f, -.25f, 0};
                    for (unsigned i = 0; i < 4; ++i)
                        CHECK(out.asset->samples()[i] == expected[i]);
                    Bytes canonical;
                    canonical.insert(canonical.end(), {'A', 'R', 'S', 'A', 'M', 'P', '0', '1'});
                    number(canonical, 48000, 4);
                    number(canonical, 4 / channels, 4);
                    number(canonical, channels, 4);
                    for (auto x : expected)
                        number(canonical, std::bit_cast<std::uint32_t>(x), 4);
                    CHECK(out.asset->id() == digest(canonical));
                }
            }
}
std::uint8_t crc8(std::span<const std::uint8_t> b)
{
    std::uint8_t c = 0;
    for (auto x : b)
    {
        c ^= x;
        for (int i = 0; i < 8; ++i)
            c = std::uint8_t((c << 1) ^ ((c & 128) ? 7 : 0));
    }
    return c;
}
std::uint16_t crc16(std::span<const std::uint8_t> b)
{
    std::uint16_t c = 0;
    for (auto x : b)
    {
        c ^= std::uint16_t(x) << 8;
        for (int i = 0; i < 8; ++i)
            c = std::uint16_t((c << 1) ^ ((c & 32768) ? 0x8005 : 0));
    }
    return c;
}
Bytes flac(unsigned channels = 2, unsigned bits = 16)
{
    const std::array<std::int16_t, 16> values{0,   8192, -16384, 32767, -32768, 1,    -1,  42,
                                              -42, 4096, -2048,  100,   200,    -300, 123, -456};
    Bytes raw;
    const auto sample = [bits](std::int16_t x, unsigned channel)
    {
        const auto scaled = std::int32_t(x) * (bits == 24 ? 256 : 1);
        return std::uint32_t(channel ? -scaled : scaled);
    };
    for (auto x : values)
        for (unsigned ch = 0; ch < channels; ++ch)
            number(raw, sample(x, ch), bits / 8);
    juce::MD5 md5(raw.data(), raw.size());
    Bytes b{'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
    big(b, 16, 2);
    big(b, 16, 2);
    big(b, 0, 3);
    big(b, 0, 3);
    big(b,
        (std::uint64_t(48000) << 44) | (std::uint64_t(channels - 1) << 41) |
            (std::uint64_t(bits - 1) << 36) | 16,
        8);
    const auto m = md5.getRawChecksumData();
    b.insert(b.end(), m.begin(), m.end());
    Bytes frame{0xff, 0xf8, 0x6a, std::uint8_t(((channels - 1) << 4) | (bits == 16 ? 8 : 12)),
                0,    15};
    frame.push_back(crc8(frame));
    for (unsigned ch = 0; ch < channels; ++ch)
    {
        frame.push_back(2);
        for (auto x : values)
            big(frame, sample(x, ch), bits / 8);
    }
    big(frame, crc16(frame), 2);
    b.insert(b.end(), frame.begin(), frame.end());
    return b;
}
void flacChecks(const juce::File &root)
{
    begin("strict native FLAC literal CRC MD5 frame-end");
    for (unsigned bits : {16u, 24u})
        for (unsigned channels : {1u, 2u})
        {
            auto source = inspect(write(root, flac(channels, bits)));
            if (!source)
                continue;
            std::atomic_bool cancel{false};
            PreparationError error{};
            PreparedDrumSample prepared;
            CHECK(source->info().bits == bits && source->info().md5Verified);
            CHECK(prepareDrumSample(*source, 0, 16, .5f, cancel, prepared, error));
            if (!prepared.asset)
                continue;
            constexpr std::array<std::int32_t, 16> literal{0,   8192, -16384, 32767, -32768, 1,
                                                           -1,  42,   -42,    4096,  -2048,  100,
                                                           200, -300, 123,    -456};
            for (std::size_t i = 0; i < literal.size(); ++i)
                for (unsigned channel = 0; channel < channels; ++channel)
                {
                    auto value = channel ? -literal[i] : literal[i];
                    if (value == 32768)
                        value = -32768;
                    const auto expected = float(value) * 0x1p-16f;
                    CHECK(std::bit_cast<std::uint32_t>(
                              prepared.asset->samples()[i * channels + channel]) ==
                          std::bit_cast<std::uint32_t>(expected));
                }
        }
    for (unsigned channels : {1u, 2u})
    {
        auto b = flac(channels);
        auto s = inspect(write(root, b));
        if (s)
        {
            CHECK(s->info().md5Verified && s->info().frames == 16);
            std::atomic_bool c{false};
            PreparationError e{};
            PreparedDrumSample p;
            CHECK(prepareDrumSample(*s, 0, 16, .5f, c, p, e));
            if (p.asset)
            {
                CHECK(p.asset->samples()[channels] == .125f);
                CHECK(p.asset->samples()[2 * channels] == -.25f);
            }
        }
        auto bad = b;
        bad.back() ^= 1;
        rejection(root, bad, PreparationError::malformedSource);
        bad = b;
        bad[26] ^= 1;
        rejection(root, bad, PreparationError::malformedSource);
        bad = b;
        bad[46] = 1;
        bad[48] = crc8(std::span<const std::uint8_t>(bad).subspan(42, 6));
        const auto frameCrc =
            crc16(std::span<const std::uint8_t>(bad).subspan(42, bad.size() - 44));
        bad[bad.size() - 2] = std::uint8_t(frameCrc >> 8);
        bad.back() = std::uint8_t(frameCrc);
        rejection(root, bad, PreparationError::malformedSource);
        for (std::size_t n : {b.size() - 1, b.size() / 2})
        {
            bad = b;
            bad.resize(n);
            rejection(root, bad, PreparationError::malformedSource);
        }
        for (std::uint64_t count : {0ull, 15ull, 17ull, 0x100000010ull})
        {
            bad = b;
            for (unsigned i = 0; i < 5; ++i)
                bad[25 - i] = std::uint8_t(count >> (8 * i));
            bad[21] |= b[21] & 0xf0;
            rejection(root, bad,
                      count == 0         ? PreparationError::malformedSource
                      : count > 28800000 ? PreparationError::tooLarge
                                         : PreparationError::malformedSource);
        }
        for (auto tail : std::vector<Bytes>{{0xff}, {0}, {0, 0, 0}, b})
        {
            bad = b;
            bad.insert(bad.end(), tail.begin(), tail.end());
            rejection(root, bad, PreparationError::malformedSource);
        }
        bad = b;
        std::fill(bad.begin() + 26, bad.begin() + 42, 0);
        s = inspect(write(root, bad));
        if (s)
            CHECK(!s->info().md5Verified);
    }
}
void admission(const juce::File &root)
{
    begin("container admission and corrupt bounds");
    const std::array<std::uint32_t, 2> v{};
    const auto b = wav(v, 1);
    auto bad = b;
    for (unsigned rate : {7999u, 384001u})
        rejection(root, wav(v, 1, rate), PreparationError::unsupportedFormat);
    for (unsigned ch : {0u, 3u})
        rejection(root, wav(v, ch), PreparationError::unsupportedFormat);
    for (unsigned tagValue : {2u, 6u})
    {
        bad = b;
        put(bad, 20, tagValue, 2);
        rejection(root, bad, PreparationError::unsupportedFormat);
    }
    bad = b;
    put(bad, 34, 64, 2);
    rejection(root, bad, PreparationError::unsupportedFormat);
    for (auto n : {0u, 3u, 12u, 23u, 43u})
    {
        bad = b;
        bad.resize(n);
        rejection(root, bad, PreparationError::malformedSource);
    }
    bad = b;
    bad.push_back(0);
    rejection(root, bad, PreparationError::malformedSource);
    bad = b;
    put(bad, 40, 3, 4);
    rejection(root, bad, PreparationError::malformedSource);
    bad = b;
    bad[0] = 'X';
    rejection(root, bad, PreparationError::unsupportedFormat);
    for (unsigned rate : {8000u, 384000u})
    {
        auto s = inspect(write(root, wav(v, 1, rate)));
        if (s)
            CHECK(s->info().rate == rate);
    }
    bad = b;
    Bytes metadata;
    tag(metadata, "LIST");
    number(metadata, 8, 4);
    metadata.insert(metadata.end(), {'I', 'N', 'F', 'O', 0xff, 0xff, 0xff, 0x7f});
    bad.insert(bad.begin() + 12, metadata.begin(), metadata.end());
    put(bad, 4, bad.size() - 8, 4);
    auto s = inspect(write(root, bad));
    auto t = inspect(write(root, b));
    if (s && t)
    {
        CHECK(s->info().fileId != t->info().fileId);
        CHECK(s->info().decodedPcmId == t->info().decodedPcmId);
    }
    bad = b;
    Bytes chunks;
    for (unsigned i = 0; i < 4097; ++i)
    {
        tag(chunks, "JUNK");
        number(chunks, 0, 4);
    }
    bad.insert(bad.begin() + 12, chunks.begin(), chunks.end());
    put(bad, 4, bad.size() - 8, 4);
    rejection(root, bad, PreparationError::tooLarge);
}
Bytes floatSamples(std::span<const float> samples, unsigned channels, unsigned rate)
{
    std::vector<std::uint32_t> bits;
    bits.reserve(samples.size());
    for (auto x : samples)
        bits.push_back(std::bit_cast<std::uint32_t>(x));
    return wav(bits, channels, rate);
}
void transactions(const juce::File &root)
{
    begin("crop gain and cancellation transactions");
    std::atomic_bool c{false};
    PreparationError e{};
    std::array<float, 4> v{.25f, -.5f, .75f, 0};
    auto s = inspect(write(root, floatSamples(v, 1, 48000)));
    if (!s)
        return;
    PreparedDrumSample p;
    CHECK(prepareDrumSample(*s, 0, 4, .5f, c, p, e));
    if (!p.asset)
        return;
    CHECK(p.asset->samples()[0] == .125f);
    const auto old = p;
    for (float gain : {0.f, -0.f, -1.f, 17.f, std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()})
    {
        CHECK(!prepareDrumSample(*s, 0, 4, gain, c, p, e));
        CHECK(e == PreparationError::invalidGain);
        CHECK(p.asset == old.asset && p.source == old.source && p.peak == old.peak);
    }
    for (auto range :
         std::array<std::pair<std::uint64_t, std::uint64_t>, 3>{{{0, 0}, {3, 2}, {0, 5}}})
    {
        CHECK(!prepareDrumSample(*s, range.first, range.second, 1, c, p, e));
        CHECK(e == PreparationError::invalidSelection);
        CHECK(p.asset == old.asset);
    }
    c = true;
    CHECK(!prepareDrumSample(*s, 0, 4, 1, c, p, e));
    CHECK(e == PreparationError::cancelled);
    CHECK(p.asset == old.asset);
    c = false;
    for (auto x : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        v[0] = x;
        rejection(root, floatSamples(v, 1, 48000), PreparationError::nonFinite);
    }
    v[0] = 32;
    auto huge = inspect(write(root, floatSamples(v, 1, 48000)));
    if (huge)
    {
        CHECK(huge->info().peak == 32);
        CHECK(!prepareDrumSample(*huge, 0, 4, 1, c, p, e));
        CHECK(e == PreparationError::gainRequired);
        CHECK(prepareDrumSample(*huge, 0, 4, .015625f, c, p, e));
    }
}
double projection(std::span<const float> pcm, double frequency)
{
    double a = 0, b = 0;
    const auto count = pcm.size() - 2048;
    for (std::size_t i = 1024; i < pcm.size() - 1024; ++i)
    {
        const auto angle = 2 * std::numbers::pi * frequency * double(i);
        a += pcm[i] * std::cos(angle);
        b += pcm[i] * std::sin(angle);
    }
    return 2 * std::hypot(a, b) / count;
}
void quality(const juce::File &root)
{
    begin("independent SRC sine passband alias images and phases");
    std::atomic_bool c{false};
    PreparationError e{};
    for (unsigned rate : {8000u, 11025u, 22050u, 44100u, 48000u, 88200u, 96000u, 192000u, 384000u})
        for (double fraction : {.01, .15, .45})
        {
            const double frequency = fraction / std::max(1., rate / 48000.);
            std::vector<float> v(rate);
            for (unsigned i = 0; i < rate; ++i)
                v[i] = float(.25 * std::sin(2 * std::numbers::pi * frequency * i));
            auto s = inspect(write(root, floatSamples(v, 1, rate)));
            if (!s)
                continue;
            PreparedDrumSample p;
            CHECK(prepareDrumSample(*s, 0, rate, 1, c, p, e));
            if (!p.asset)
                continue;
            const auto out = p.asset->samples();
            double maxError = 0;
            for (std::size_t i = 1024; i < out.size() - 1024; ++i)
                maxError =
                    std::max(maxError, std::abs(double(out[i]) -
                                                .25 * std::sin(2 * std::numbers::pi * frequency *
                                                               double(i) * rate / 48000.)) /
                                           .25);
            CHECK(maxError <= .001);
            const double amp = projection(out, frequency * rate / 48000.);
            CHECK(std::abs(20 * std::log10(amp / .25)) <= .05);
        }
    for (auto [rate, hz, alias] :
         std::array<std::array<unsigned, 3>, 2>{{{96000, 30000, 18000}, {384000, 60000, 12000}}})
    {
        std::vector<float> v(rate / 2);
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = float(.25 * std::sin(2 * std::numbers::pi * hz * double(i) / rate));
        auto s = inspect(write(root, floatSamples(v, 1, rate)));
        if (!s)
            continue;
        PreparedDrumSample p;
        CHECK(prepareDrumSample(*s, 0, v.size(), 1, c, p, e));
        if (p.asset)
            CHECK(projection(p.asset->samples(), alias / 48000.) <= .00025);
    }
    std::vector<float> v(8000);
    for (unsigned i = 0; i < v.size(); ++i)
        v[i] = float(.25 * std::sin(2 * std::numbers::pi * i / 8));
    auto s = inspect(write(root, floatSamples(v, 1, 8000)));
    if (s)
    {
        PreparedDrumSample p;
        CHECK(prepareDrumSample(*s, 0, v.size(), 1, c, p, e));
        if (p.asset)
            for (unsigned hz : {7000u, 9000u, 15000u, 17000u, 23000u})
                CHECK(projection(p.asset->samples(), hz / 48000.) <= .00025);
    }
}
bool writable(const juce::File &f)
{
    const auto h = CreateFileW(f.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}
void ioAndCancellation(const juce::File &root)
{
    using namespace preparation_test;
    begin("same-handle lease real I/O and scripted pending lifetimes");
    const std::array<float, 4> values{.25f, -.5f, 0, 0};
    const auto bytes = floatSamples(values, 1, 48000);
    const auto oldFile = write(root, bytes), otherFile = write(root, bytes);
    const auto writer =
        CreateFileW(oldFile.getFullPathName().toWideCharPointer(), GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(writer != INVALID_HANDLE_VALUE);
    std::atomic_bool cancel{false};
    PreparationError error{};
    std::unique_ptr<DrumSource> held;
    CHECK(!inspectDrumSource(oldFile, std::nullopt, cancel, held, error));
    CHECK(error == PreparationError::sourceBusy);
    if (writer != INVALID_HANDLE_VALUE)
        CloseHandle(writer);
    held = inspect(oldFile);
    if (!held)
        return;
    CHECK(!writable(oldFile));
    const auto reader = CreateFileW(oldFile.getFullPathName().toWideCharPointer(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(reader != INVALID_HANDLE_VALUE);
    if (reader != INVALID_HANDLE_VALUE)
        CloseHandle(reader);
    const auto deleter = CreateFileW(oldFile.getFullPathName().toWideCharPointer(), DELETE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(deleter == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == ERROR_SHARING_VIOLATION);
    if (deleter != INVALID_HANDLE_VALUE)
        CloseHandle(deleter);
    CHECK(!DeleteFileW(oldFile.getFullPathName().toWideCharPointer()));
    CHECK(GetLastError() == ERROR_SHARING_VIOLATION);
    CHECK(!MoveFileExW(otherFile.getFullPathName().toWideCharPointer(),
                       oldFile.getFullPathName().toWideCharPointer(), MOVEFILE_REPLACE_EXISTING));
    const auto moveError = GetLastError();
    CHECK(moveError == ERROR_SHARING_VIOLATION || moveError == ERROR_ACCESS_DENIED);
    PreparedDrumSample result;
    CHECK(prepareDrumSample(*held, 0, 4, 1, cancel, result, error));
    const auto old = result;
    auto *oldPointer = held.get();
    const auto oldInfo = held->info();
    ReadTrace trace;
    Control control;
    struct PendingCancel
    {
        ReadTrace *trace;
        std::atomic_bool *cancel;
    };
    PendingCancel pendingState{&trace, &cancel};
    control.trace = &trace;
    control.context = &pendingState;
    for (auto mode :
         {ReadMode::readError, ReadMode::shortSuccess, ReadMode::pendingCancel,
          ReadMode::pendingDeadline, ReadMode::pendingCompletionRace, ReadMode::pendingWaitFailure})
        for (auto phase : {Phase::hash, Phase::decode})
        {
            if (mode == ReadMode::pendingWaitFailure)
                std::cerr << "WAIT_FAILED regression phase=" << int(phase)
                          << " expects cancelled operation drained then ioError\n";
            trace = {};
            cancel = false;
            control.readMode = mode;
            control.readPhase = phase;
            control.readOffset = phase == Phase::hash ? 0 : 44;
            if (mode != ReadMode::pendingCancel && mode != ReadMode::pendingCompletionRace)
                control.progress = nullptr;
            else
                control.progress = [](Phase p, std::uint64_t, void *c) noexcept
                {
                    auto &s = *static_cast<PendingCancel *>(c);
                    if (p == Phase::readPending && s.trace->pending)
                        s.cancel->store(true);
                };
            useControl(&control);
            if (phase == Phase::hash)
            {
                CHECK(!inspectDrumSource(otherFile, std::nullopt, cancel, held, error));
                CHECK(held.get() == oldPointer);
                CHECK(held->info().fileId == oldInfo.fileId &&
                      held->info().decodedPcmId == oldInfo.decodedPcmId);
                CHECK(writable(otherFile));
            }
            else
            {
                CHECK(!prepareDrumSample(*held, 0, 4, 1, cancel, result, error));
                CHECK(result.asset == old.asset && result.source == old.source &&
                      result.peak == old.peak);
            }
            const auto expected =
                (mode == ReadMode::pendingCancel || mode == ReadMode::pendingCompletionRace)
                    ? PreparationError::cancelled
                    : PreparationError::ioError;
            if (error != expected || trace.issued != 1 || !trace.valid)
                std::cerr << "mode=" << int(mode) << " phase=" << int(phase)
                          << " error=" << int(error) << " issued=" << trace.issued
                          << " valid=" << trace.valid << '\n';
            CHECK(error == expected);
            CHECK(trace.valid && trace.issued == 1);
            CHECK(!trace.pending);
            CHECK(!writable(oldFile));
            if (mode == ReadMode::pendingCancel || mode == ReadMode::pendingDeadline ||
                mode == ReadMode::pendingCompletionRace || mode == ReadMode::pendingWaitFailure)
            {
                CHECK(trace.cancels == 1 && trace.completions == 1 && trace.drains == 1);
                auto find = [&](IoEvent e)
                { return std::find(trace.events.begin(), trace.events.begin() + trace.used, e); };
                CHECK(find(IoEvent::issue) < find(IoEvent::completion));
                CHECK(find(IoEvent::completion) < find(IoEvent::drained));
                CHECK(find(IoEvent::drained) < find(IoEvent::releaseOverlapped));
            }
            else
                CHECK(trace.cancels == 0 && trace.drains <= 1);
            useControl(nullptr);
            cancel = false;
            CHECK(prepareDrumSample(*held, 0, 4, 1, cancel, result, error));
            CHECK(result.asset->id() == old.asset->id());
            result = old;
        }
    trace = {};
    control.readMode = ReadMode::system;
    control.readPhase = Phase::decode;
    control.progress = nullptr;
    std::array<std::uint8_t, 4> data{};
    for (std::uint64_t offset : {44ull, 52ull})
    {
        trace = {};
        control.readOffset = offset;
        useControl(&control);
        CHECK(Access::readAt(*held, offset, data, cancel, error));
        CHECK(std::equal(data.begin(), data.end(), bytes.begin() + offset));
    }
    trace = {};
    control.readOffset = bytes.size() + 1;
    CHECK(!Access::readAt(*held, bytes.size() + 1, data, cancel, error));
    CHECK(error == PreparationError::ioError && trace.issued == 0);
    CHECK(!Access::viewPosition(*held, UINT64_MAX, error));
    CHECK(error == PreparationError::ioError && trace.issued == 0);
    useControl(nullptr);
    held.reset();
    CHECK(trace.valid);
    CHECK(writable(oldFile));
    begin("deterministic phase cancellation preserves both transactions");
    for (auto phase : {Phase::preflight, Phase::hash, Phase::decode, Phase::beforeCommit})
    {
        cancel = false;
        Control stop;
        struct Stop
        {
            Phase phase;
            std::atomic_bool *cancel;
        };
        Stop state{phase, &cancel};
        stop.context = &state;
        stop.progress = [](Phase p, std::uint64_t, void *x) noexcept
        {
            auto &s = *static_cast<Stop *>(x);
            if (p == s.phase)
                s.cancel->store(true);
        };
        useControl(&stop);
        CHECK(!inspectDrumSource(oldFile, std::nullopt, cancel, held, error));
        CHECK(error == PreparationError::cancelled && !held);
        useControl(nullptr);
    }
    cancel = false;
    held = inspect(oldFile);
    if (!held)
        return;
    for (auto phase : {Phase::decode, Phase::resample, Phase::beforeCommit})
    {
        cancel = false;
        Control stop;
        struct Stop
        {
            Phase phase;
            std::atomic_bool *cancel;
        };
        Stop state{phase, &cancel};
        stop.context = &state;
        stop.progress = [](Phase p, std::uint64_t, void *x) noexcept
        {
            auto &s = *static_cast<Stop *>(x);
            if (p == s.phase)
                s.cancel->store(true);
        };
        useControl(&stop);
        CHECK(!prepareDrumSample(*held, 0, 4, 1, cancel, result, error));
        CHECK(error == PreparationError::cancelled);
        CHECK(result.asset == old.asset && result.source == old.source && result.peak == old.peak);
        useControl(nullptr);
    }
}
void boundaries(const juce::File &root)
{
    using namespace preparation_test;
    begin("crop-edge isolation partition invariance DC and phase");
    std::atomic_bool cancel{false};
    PreparationError error{};
    for (unsigned rate : {8000u, 44100u, 48000u, 96000u, 384000u})
    {
        std::array<float, 10> v{16, -16, 0, .125f, -.25f, .5f, 0, .25f, 16, -16};
        auto s = inspect(write(root, floatSamples(v, 1, rate)));
        if (!s)
            continue;
        PreparedDrumSample baseline;
        CHECK(prepareDrumSample(*s, 2, 8, .25f, cancel, baseline, error));
        if (!baseline.asset)
            continue;
        v[0] = -8;
        v[1] = 8;
        v[8] = 8;
        v[9] = -8;
        auto other = inspect(write(root, floatSamples(v, 1, rate)));
        if (other)
        {
            PreparedDrumSample p;
            CHECK(prepareDrumSample(*other, 2, 8, .25f, cancel, p, error));
            if (p.asset)
                CHECK(p.asset->id() == baseline.asset->id());
        }
        for (unsigned partition : {1u, 7u, 4096u})
        {
            Control control;
            control.decodePartition = partition;
            useControl(&control);
            PreparedDrumSample p;
            CHECK(prepareDrumSample(*s, 2, 8, .25f, cancel, p, error));
            if (p.asset)
                CHECK(p.asset->id() == baseline.asset->id());
            useControl(nullptr);
        }
        for (std::uint64_t index : {2ull, 3ull, 7ull})
        {
            PreparedDrumSample p;
            CHECK(prepareDrumSample(*s, index, index + 1, .5f, cancel, p, error));
            if (p.asset)
                for (float x : p.asset->samples())
                    CHECK(std::abs(x - v[index] * .5f) < 1e-7f);
        }
        for (std::size_t index : {0u, 3u, 5u})
            for (float sign : {-1.f, 1.f})
            {
                std::array<float, 6> impulse{};
                impulse[index] = sign * .25f;
                auto a = inspect(write(root, floatSamples(impulse, 1, rate)));
                if (a)
                {
                    PreparedDrumSample p;
                    CHECK(prepareDrumSample(*a, 0, 6, 1, cancel, p, error));
                    if (p.asset)
                        CHECK(p.peak > 0 && p.peak <= 1);
                }
            }
    }
    for (unsigned rate : {8000u, 44100u, 48000u, 96000u, 384000u})
    {
        std::vector<float> dc(std::size_t(rate) * 10 + 1, .125f);
        auto s = inspect(write(root, floatSamples(dc, 1, rate)));
        if (!s)
            continue;
        PreparedDrumSample p;
        CHECK(!prepareDrumSample(*s, 0, dc.size(), 1, cancel, p, error));
        CHECK(error == PreparationError::invalidSelection);
        CHECK(prepareDrumSample(*s, 0, dc.size() - 1, 1, cancel, p, error));
        if (p.asset)
        {
            CHECK(p.asset->frames() == 480000);
            CHECK(std::all_of(p.asset->samples().begin(), p.asset->samples().end(),
                              [](float x) { return x == .125f; }));
        }
    }
    // A 160-frame rational-phase sweep spans every selected phase and adjacent lookup cells.
    std::vector<float> sine(44100);
    for (unsigned i = 0; i < sine.size(); ++i)
        sine[i] = float(.25 * std::sin(2 * std::numbers::pi * .193123 * i));
    auto s = inspect(write(root, floatSamples(sine, 1, 44100)));
    if (s)
    {
        PreparedDrumSample p;
        CHECK(prepareDrumSample(*s, 137, 22050, 1, cancel, p, error));
        if (p.asset)
            for (unsigned i = 1600; i < 1760; ++i)
            {
                const double x = 137 + double(i) * 44100 / 48000.;
                CHECK(std::abs(p.asset->samples()[i] -
                               .25 * std::sin(2 * std::numbers::pi * .193123 * x)) /
                          .25 <=
                      .001);
            }
    }
}
void formatEdges(const juce::File &root)
{
    begin("extensible WAV AIFC FLOAT metadata padding and duration limits");
    const std::array<std::uint32_t, 4> bits{0x80000000, 0x3e800000, 0xbf000000, 0};
    std::atomic_bool cancel{false};
    PreparationError error{};
    auto extensible = wav(bits, 2);
    put(extensible, 20, 0xfffe, 2);
    put(extensible, 16, 40, 4);
    Bytes extra;
    number(extra, 22, 2);
    number(extra, 32, 2);
    number(extra, 3, 4);
    number(extra, 3, 4);
    extra.insert(extra.end(), {0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71});
    extensible.insert(extensible.begin() + 36, extra.begin(), extra.end());
    put(extensible, 4, extensible.size() - 8, 4);
    auto s = inspect(write(root, extensible));
    if (s)
    {
        PreparedDrumSample p;
        CHECK(prepareDrumSample(*s, 0, 2, 1, cancel, p, error));
        if (p.asset)
            for (unsigned i = 0; i < 4; ++i)
                CHECK(std::bit_cast<std::uint32_t>(p.asset->samples()[i]) == bits[i]);
    }
    auto bad = extensible;
    bad[48] ^= 1;
    rejection(root, bad, PreparationError::malformedSource);
    bad = extensible;
    put(bad, 38, 24, 2);
    rejection(root, bad, PreparationError::malformedSource);
    std::array<std::int32_t, 4> values{};
    for (unsigned i = 0; i < 4; ++i)
        values[i] = std::bit_cast<std::int32_t>(bits[i]);
    for (const char *marker : {"fl32", "FL32"})
    {
        auto b = pcm(values, 32, 2, 48000, true, marker);
        s = inspect(write(root, b));
        if (s)
        {
            PreparedDrumSample p;
            CHECK(prepareDrumSample(*s, 0, 2, 1, cancel, p, error));
            if (p.asset)
                for (unsigned i = 0; i < 4; ++i)
                    CHECK(std::bit_cast<std::uint32_t>(p.asset->samples()[i]) == bits[i]);
        }
    }
    const std::array<std::int32_t, 4> integers{0, 8192, -16384, 1};
    auto aiff = pcm(integers, 16, 1, 48000, true, "NONE");
    // COMM's Pascal compression label is irrelevant metadata, including a long valid label.
    auto longName = aiff;
    longName[54] = 100;
    longName[55] = 'x';
    longName.insert(longName.begin() + 56, 100, 'x');
    longName[155] = 0;
    for (unsigned i = 0; i < 4; ++i)
        longName[28 + i] = std::uint8_t(124 >> (24 - 8 * i));
    const auto form = longName.size() - 8;
    for (unsigned i = 0; i < 4; ++i)
        longName[4 + i] = std::uint8_t(form >> (24 - 8 * i));
    s = inspect(write(root, longName));
    // An SSND offset is metadata and cannot become decoded PCM.
    std::cout << "FORMAT_EDGE fixture=SSND_offset\n" << std::flush;
    auto offset = aiff;
    constexpr std::array<std::uint8_t, 4> ssndTag{'S', 'S', 'N', 'D'};
    const auto ssnd =
        std::search(offset.begin(), offset.end(), ssndTag.begin(), ssndTag.end()) - offset.begin();
    CHECK(ssnd < offset.size());
    if (ssnd < offset.size())
    {
        offset.insert(offset.begin() + ssnd + 16, {0xaa, 0xbb});
        for (unsigned i = 0; i < 4; ++i)
        {
            offset[ssnd + 4 + i] = std::uint8_t(18 >> (24 - 8 * i));
            offset[ssnd + 8 + i] = std::uint8_t(2 >> (24 - 8 * i));
            offset[4 + i] = std::uint8_t((offset.size() - 8) >> (24 - 8 * i));
        }
        s = inspect(write(root, offset));
        if (s)
            CHECK(s->info().frames == 4);
    }
    for (unsigned byte : {40u, 49u})
    {
        bad = aiff;
        bad[byte] |= byte == 40 ? 0x80 : 1;
        rejection(root, bad, PreparationError::unsupportedFormat);
    }
    auto b = wav(bits, 2);
    Bytes odd;
    tag(odd, "JUNK");
    number(odd, 1, 4);
    odd.push_back(0x42);
    odd.push_back(0);
    b.insert(b.begin() + 12, odd.begin(), odd.end());
    put(b, 4, b.size() - 8, 4);
    s = inspect(write(root, b));
    if (s)
        CHECK(s->info().frames == 2);
    bad = wav(bits, 2);
    Bytes metadata;
    tag(metadata, "JUNK");
    number(metadata, 1048576, 4);
    metadata.resize(1048584, 0);
    bad.insert(bad.begin() + 12, metadata.begin(), metadata.end());
    put(bad, 4, bad.size() - 8, 4);
    rejection(root, bad, PreparationError::tooLarge);
    bad = wav(bits, 2);
    const Bytes duplicate(bad.begin() + 12, bad.begin() + 36);
    bad.insert(bad.begin() + 36, duplicate.begin(), duplicate.end());
    put(bad, 4, bad.size() - 8, 4);
    rejection(root, bad, PreparationError::malformedSource);
    std::cout << "FORMAT_EDGE fixture=raw_442368\n" << std::flush;
    std::vector<float> raw(442368, 0);
    s = inspect(write(root, floatSamples(raw, 1, 44100)));
    if (s)
    {
        PreparedDrumSample p;
        CHECK(!prepareDrumSample(*s, 0, 442368, .125f, cancel, p, error));
        CHECK(error == PreparationError::invalidSelection);
        CHECK(prepareDrumSample(*s, 0, 441000, .125f, cancel, p, error));
        if (p.asset)
            CHECK(p.asset->frames() == 480000 && p.source.sourceFrames == 442368);
    }
    std::cout << "FORMAT_EDGE fixture=reconstructed_overshoot\n" << std::flush;
    std::vector<float> step(4000, 1);
    std::fill(step.begin() + 2000, step.end(), -1.f);
    s = inspect(write(root, floatSamples(step, 1, 44100)));
    if (s)
    {
        PreparedDrumSample p;
        CHECK(!prepareDrumSample(*s, 0, 4000, 1, cancel, p, error));
        CHECK(error == PreparationError::gainRequired);
        CHECK(prepareDrumSample(*s, 0, 4000, .5f, cancel, p, error));
    }
    std::cout << "FORMAT_EDGE fixture=cast_overflow\n" << std::flush;
    const std::array<float, 2> huge{std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max()};
    s = inspect(write(root, floatSamples(huge, 1, 8000)));
    if (s)
    {
        PreparedDrumSample p;
        CHECK(!prepareDrumSample(*s, 0, 2, 16, cancel, p, error));
        CHECK(error == PreparationError::nonFinite);
        CHECK(prepareDrumSample(*s, 0, 2, std::numeric_limits<float>::denorm_min(), cancel, p,
                                error));
    }
    std::cout << "FORMAT_EDGE fixture=sparse_encoded_limit\n" << std::flush;
    const auto sparse = root.getChildFile("encoded-too-large.wav");
    const auto h = CreateFileW(sparse.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0,
                               nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (CHECK(h != INVALID_HANDLE_VALUE))
    {
        DWORD returned = 0;
        CHECK(DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr));
        LARGE_INTEGER length{};
        length.QuadPart = 256ll * 1024 * 1024 + 1;
        CHECK(SetFilePointerEx(h, length, nullptr, FILE_BEGIN));
        CHECK(SetEndOfFile(h));
        CHECK(CloseHandle(h));
        rejectBeforeAllocation(sparse, PreparationError::tooLarge);
    }
    auto longDuration = flac();
    const std::uint64_t packed = (std::uint64_t(8000) << 44) | (std::uint64_t(1) << 41) |
                                 (std::uint64_t(15) << 36) | 4800001;
    for (unsigned i = 0; i < 8; ++i)
        longDuration[18 + i] = std::uint8_t(packed >> (56 - 8 * i));
    rejection(root, longDuration, PreparationError::tooLarge);
}
Bytes load(const juce::File &file)
{
    juce::MemoryBlock b;
    CHECK(file.existsAsFile() && file.getSize() > 0 && file.getSize() <= 16 * 1024 * 1024);
    if (!file.existsAsFile() || file.getSize() > 16 * 1024 * 1024)
        return {};
    CHECK(file.loadFileAsData(b));
    const auto *p = static_cast<const std::uint8_t *>(b.getData());
    return Bytes(p, p + b.getSize());
}
std::uint64_t little(const std::uint8_t *p, unsigned width)
{
    std::uint64_t n = 0;
    for (unsigned i = 0; i < width; ++i)
        n |= std::uint64_t(p[i]) << (8 * i);
    return n;
}
void qualified(const juce::File &fixtures)
{
    begin("twelve retained qualified pairs: whole-PCM bitstream digest equality");
    struct Row
    {
        const char *name;
        unsigned seed;
        const char *flacHash;
        const char *wavHash;
    };
    constexpr std::array<Row, 12> rows{
        {{"kick", 17, "ed55fa9465f8a64830c868037bc793e3f8fadf2b97180650518dcb73feb139b0",
          "42199ed3e6542c6d450ce61326db0d0e4f176f9bb2ccf4c9b50390d7c9fd7794"},
         {"kick", 73, "954af084cebc6dc4a8a8a81ffa53338436265b878e87412dd8a9781435de9c54",
          "d286e79471cdeed962aed7756cd92c2218190869607d635010862c0a8fc4c84e"},
         {"kick", 211, "be0885b66587735fbd66ba83bf9cf4d8bc5a3e3114024d4b195c3a5ba8ac981b",
          "a7c2856dbdcc698ef66b77ebb7bc5ca8d92edffa7acec2412c3f5aeacc1c8805"},
         {"snare", 17, "829ad2a93d4a7718100220aae90855099ea4eaa8a9d227afdab94cf5b0789c4b",
          "a7d4491d7e9bbd6a4f9a0d74b9fce3facc48466307ae4b15f4c372c2de42a53d"},
         {"snare", 73, "5a20feb4807845c888411f38e43ca47e7552204576031403ec9390a4d1d09c99",
          "dfc2a88fd93cc272635eba518e7d3fe616e8cf560d1b58a21118ba7a8db60d94"},
         {"snare", 211, "81236116c5506c7bdd6a41ec9b894c8143c49ee212ab3703c4d3480ab99a6cf7",
          "02175cddf037b5cddf331a0cc88d45450ec0cde5fa4e77792f6529dbe6d82060"},
         {"hat", 17, "08f530a7e9b7632dfa41d3be451c7dcd7069472757caa7bc68cda48f6117126d",
          "4339e5eb9963daf98561ce03e0e224aa5955d47e898fe53bdddbc8df387b187a"},
         {"hat", 73, "ff5d51d321fbb8796e78f2aaa1a606704d38f119d8caee4906598ed639ca167b",
          "869ef527692e493885f36b40d38bd7155fc7b8393c2a383024e2b2649dff8169"},
         {"hat", 211, "a54f324174f7caa4e962c3ecb647ca992f9407617a42eb2a93a27ec896bf2938",
          "ba32e9bc70e2db65be3f4fe537eb35c75d0f4d4584d1d7527de0a6616389d353"},
         {"percussion", 17, "e419ffd6dc85c17a838ded7e49a4c02d65d1b4ffc1f1d59fd01091376a35e693",
          "55121d17f0f2ffddd3f5ec886d0f5c8e0882f5d21f51d305e6b8a6e1251d3552"},
         {"percussion", 73, "a2b31a65aea522e5c23815e0add3b828da352a51f9f5e17407910ea2e5ea1eb6",
          "7ebfdb47625f574009690a4c8639996d6071673ca85d5621a47e7316ef5e8d1f"},
         {"percussion", 211, "30e47cb9538ae4d91625ae144358f1558d71b9c41447369cea27da7299653dac",
          "15ef1ab424de03836bc4655a01bff06bd35cc17410ea01bf2501c3085913538e"}}};
    std::atomic_bool cancel{false};
    PreparationError error{};
    for (const auto &row : rows)
    {
        std::cout << "QUALIFIED " << row.name << '/' << row.seed << '\n';
        const auto original = fixtures.getChildFile("base/output/trial/" + juce::String(row.name) +
                                                    "-" + juce::String(row.seed) + "_00001.flac");
        const auto reference = fixtures.getChildFile("attempts/" + juce::String(row.name) + "/" +
                                                     juce::String(row.seed) + "-original.wav");
        const auto encoded = load(original), wavBytes = load(reference);
        CHECK(assetIdText(digest(encoded)) == row.flacHash);
        CHECK(assetIdText(digest(wavBytes)) == row.wavHash);
        bool format = false;
        std::span<const std::uint8_t> audio;
        if (CHECK(wavBytes.size() >= 12 && std::memcmp(wavBytes.data(), "RIFF", 4) == 0 &&
                  std::memcmp(wavBytes.data() + 8, "WAVE", 4) == 0 &&
                  little(wavBytes.data() + 4, 4) + 8 == wavBytes.size()))
        {
            std::size_t offset = 12;
            while (offset + 8 <= wavBytes.size())
            {
                const auto n = little(wavBytes.data() + offset + 4, 4);
                if (!CHECK(n <= wavBytes.size() - offset - 8))
                    break;
                const auto *body = wavBytes.data() + offset + 8;
                if (std::memcmp(wavBytes.data() + offset, "fmt ", 4) == 0)
                {
                    CHECK(n >= 16);
                    if (n >= 16)
                        format = little(body, 2) == 3 && little(body + 2, 2) == 2 &&
                                 little(body + 4, 4) == 44100 && little(body + 12, 2) == 8 &&
                                 little(body + 14, 2) == 32;
                }
                if (std::memcmp(wavBytes.data() + offset, "data", 4) == 0)
                {
                    CHECK(audio.empty());
                    audio = {body, std::size_t(n)};
                }
                offset += 8 + std::size_t(n) + (n & 1);
            }
        }
        CHECK(format && audio.size() == 442368 * 2 * 4);
        if (!format || audio.size() != 442368 * 2 * 4)
            continue;
        const auto referencePcm = digest(audio);
        GeneratedOrigin origin;
        parseAssetIdText(row.flacHash, origin.expectedFileId);
        origin.promptId.fill(0x53);
        origin.generator = "stable_audio_3_small_sfx";
        origin.condition = "retained-qualified-composite-test";
        origin.seed = row.seed;
        std::unique_ptr<DrumSource> s;
        CHECK(inspectDrumSource(original, origin, cancel, s, error));
        if (!s)
            continue;
        const auto &info = s->info();
        CHECK(info.rate == 44100 && info.channels == 2 && info.bits == 16 &&
              info.frames == 442368 && info.md5Verified);
        CHECK(info.decodedPcmId == referencePcm);
        auto ref = inspect(reference);
        if (ref)
            CHECK(ref->info().decodedPcmId == referencePcm);
        PreparedDrumSample p;
        CHECK(!prepareDrumSample(*s, 0, 442368, .125f, cancel, p, error));
        CHECK(error == PreparationError::invalidSelection && !p.asset);
        CHECK(prepareDrumSample(*s, 0, 441000, .125f, cancel, p, error));
        if (p.asset)
        {
            CHECK(p.asset->frames() == 480000 && p.asset->channels() == 2);
            CHECK(p.source.sourceFileId == info.fileId && p.source.sourceFrames == 442368 &&
                  p.source.begin == 0 && p.source.end == 441000 && p.source.sourceRate == 44100 &&
                  p.source.gain == .125f);
            CHECK(p.source.kind == SourceKind::generated && p.source.seed == row.seed &&
                  p.source.promptId == origin.promptId);
        }
        auto *old = s.get();
        origin.expectedFileId[0] ^= 1;
        CHECK(!inspectDrumSource(original, origin, cancel, s, error));
        CHECK(error == PreparationError::identityMismatch && s.get() == old);
        origin.expectedFileId[0] ^= 1;
        for (unsigned i = 0; i < 4; ++i)
        {
            auto invalid = origin;
            if (i == 0)
                invalid.generator.clear();
            if (i == 1)
                invalid.condition = "\n";
            if (i == 2)
                invalid.promptId = {};
            if (i == 3)
                invalid.expectedFileId = {};
            CHECK(!inspectDrumSource(original, invalid, cancel, s, error));
            CHECK(error == PreparationError::identityMismatch && s.get() == old);
        }
        CHECK(assetIdText(digest(load(original))) == row.flacHash);
        CHECK(assetIdText(digest(load(reference))) == row.wavHash);
        std::cout << "PCM_SHA256 " << assetIdText(referencePcm)
                  << " frames=442368 channels=2 rate=44100 md5=verified\n";
    }
}
// Private integration snippet only; not compiled or executed.
// Routing inherited, no overrides. Add these includes to the test TU:
// #include <chrono>
// #include <climits>
// #include <cstring>
// #if defined(_MSC_VER) && defined(_DEBUG)
// #include <crtdbg.h>
// #include <malloc.h>
// #endif
// Fits the current test's Bytes/number/big/tag/crc8/crc16/write/CHECK/begin helpers.
// Place inside the test's anonymous namespace; call capacity(root) only from --capacity.

// Independent frame construction; all byte CRCs use the existing test-owned functions.
struct FixtureBits
{
    Bytes bytes;
    unsigned used = 0;
    void put(std::uint64_t value, unsigned count)
    {
        for (unsigned i = count; i--;)
        {
            if (used == 0)
                bytes.push_back(0);
            bytes.back() |= std::uint8_t(((value >> i) & 1u) << (7 - used));
            used = (used + 1) & 7u;
        }
    }
};
Bytes flacHeader(unsigned bits, std::uint64_t frames, unsigned minimum = 16,
                 unsigned maximum = 16384)
{
    Bytes b{'f', 'L', 'a', 'C', 0x80, 0, 0, 34};
    big(b, minimum, 2);
    big(b, maximum, 2);
    big(b, 0, 3);
    big(b, 0, 3);
    big(b,
        (std::uint64_t(48000) << 44) | (std::uint64_t(1) << 41) | (std::uint64_t(bits - 1) << 36) |
            frames,
        8);
    b.insert(b.end(), 16, 0x01); // Nonzero MD5 keeps dependency MD5 accumulation enabled.
    return b;
}
void appendMaxConstantFrame(Bytes &b, bool afterLegal)
{
    // Variable block number, explicit blocksize65535,48k,8 independent channels,32bits.
    Bytes frame{0xff, 0xf9, 0x7a, 0x7e};
    if (afterLegal)
        frame.insert(frame.end(), {0xe4, 0x80, 0x80}); // sample number16384
    else
        frame.push_back(0);
    frame.insert(frame.end(), {0xff, 0xfe});
    frame.push_back(crc8(frame));
    for (unsigned ch = 0; ch < 8; ++ch)
    {
        frame.push_back(0); // Constant subframe, no wasted bits.
        big(frame, 0, 4);   // All65535 samples in this channel are the32bit constant zero.
    }
    big(frame, crc16(frame), 2);
    b.insert(b.end(), frame.begin(), frame.end());
}
Bytes maximumDisagreement(bool retainRice)
{
    auto b = flacHeader(retainRice ? 24u : 16u, retainRice ? 81919u : 65535u);
    if (retainRice)
    {
        // A legal16,384-frame stereo24bit frame first retains Rice tables and98,304 MD5 bytes.
        Bytes frame{0xff, 0xf9, 0x7a, 0x1c, 0, 0x3f, 0xff};
        frame.push_back(crc8(frame));
        FixtureBits body;
        for (unsigned ch = 0; ch < 2; ++ch)
        {
            body.put(0x10, 8); // Fixed predictor order0.
            body.put(0, 2);
            body.put(14, 4); // Rice method0,partition order14.
            for (unsigned partition = 0; partition < 16384; ++partition)
            {
                body.put(15, 4);
                body.put(0, 5); // Escape with zero raw width => one zero residual.
            }
        }
        // The bit builder leaves trailing bits zero; only whole-frame end is padded.
        frame.insert(frame.end(), body.bytes.begin(), body.bytes.end());
        big(frame, crc16(frame), 2);
        b.insert(b.end(), frame.begin(), frame.end());
    }
    appendMaxConstantFrame(b, retainRice);
    return b;
}

#if defined(_MSC_VER) && defined(_DEBUG)
namespace prepHeap
{
struct Row
{
    long request = 0;
    std::size_t bytes = 0;
};
struct Result
{
    std::size_t live = 0, peak = 0, overlap = 0, allocated = 0, freed = 0, baselineFreed = 0;
    std::size_t allocations = 0, reallocations = 0, denials = 0, md5Requests = 0;
    std::array<std::size_t, 128> requests{};
    bool valid = true, sawMaxMd5 = false, sawLegalMd5 = false;
};
// Static storage avoids both recording self-allocation and a large test stack frame.
static std::array<Row, 2048> rows;
thread_local Result result;
thread_local bool recording = false, inside = false;
thread_local std::size_t denySize = 0, denyOrdinal = 0, ordinal = 0;
thread_local bool md5Armed = false, denyMd5 = false;
void md5Progress(preparation_test::Phase phase, std::uint64_t offset, void *) noexcept
{
    if (phase == preparation_test::Phase::decode && offset == 42)
        md5Armed = true;
}
void remove(void *data, int use) noexcept
{
    if (!data)
        return;
    long request = 0;
    const auto n = _msize_dbg(data, use);
    if (n > UINT_MAX || !_CrtIsMemoryBlock(data, unsigned(n), &request, nullptr, nullptr))
    {
        result.valid = false;
        return;
    }
    const auto row =
        std::find_if(rows.begin(), rows.end(), [&](const Row &r) { return r.request == request; });
    if (row == rows.end())
    {
        result.baselineFreed += n;
        return;
    }
    if (row->bytes > result.live)
    {
        result.valid = false;
        return;
    }
    result.live -= row->bytes;
    result.freed += row->bytes;
    *row = {};
}
int __cdecl hook(int kind, void *data, std::size_t n, int use, long request, const unsigned char *,
                 int)
{
    if (!recording || inside)
        return 1;
    inside = true;
    struct Exit
    {
        ~Exit() { inside = false; }
    } exit;
    if (data && (kind == _HOOK_FREE || kind == _HOOK_REALLOC))
    {
        use = _CrtReportBlockType(data);
        if (use == -1)
        {
            result.valid = false;
            return 1;
        }
    }
    use &= 0xffff;
    if (use != _NORMAL_BLOCK && use != _CLIENT_BLOCK)
        return 1;
    if (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC)
    {
        ++ordinal;
        if (ordinal <= result.requests.size())
            result.requests[ordinal - 1] = n;
        if (md5Armed && n == 64)
            ++result.md5Requests;
        if ((denyMd5 && md5Armed && n == 64 &&
             (result.md5Requests == 3 || result.md5Requests == 4)) ||
            (result.denials == 0 &&
             ((denySize != 0 && n == denySize) || (denyOrdinal != 0 && ordinal == denyOrdinal))))
        {
            ++result.denials;
            return 0;
        }
        result.sawMaxMd5 |= n == 2097120;
        result.sawLegalMd5 |= n == 98304;
        if (kind == _HOOK_REALLOC)
        {
            ++result.reallocations;
            // Keep both old and new requested storage charged before observing the free.
            result.overlap = std::max(result.overlap, result.live + n);
            remove(data, use);
        }
        const auto row =
            std::find_if(rows.begin(), rows.end(), [](const Row &r) { return r.request == 0; });
        if (request <= 0 || row == rows.end())
        {
            result.valid = false;
            return 1;
        }
        *row = {request, n};
        result.live += n;
        result.allocated += n;
        ++result.allocations;
        result.peak = std::max(result.peak, result.live);
    }
    else if (kind == _HOOK_FREE)
        remove(data, use);
    return 1;
}
template <class F>
Result observe(F &&work, std::size_t deny = 0, std::size_t denyAt = 0, bool denyMd5Work = false)
{
    rows.fill({});
    result = {};
    denySize = deny;
    denyOrdinal = denyAt;
    denyMd5 = denyMd5Work;
    md5Armed = false;
    ordinal = 0;
    inside = false;
    _CrtMemState before{}, after{};
    _CrtMemCheckpoint(&before);
    const auto previous = _CrtSetAllocHook(hook);
    recording = true;
    try
    {
        work();
    }
    catch (...)
    {
        recording = false;
        _CrtSetAllocHook(previous);
        throw;
    }
    recording = false;
    _CrtSetAllocHook(previous);
    _CrtMemCheckpoint(&after);
    const auto observed = result;
    const auto delta =
        std::int64_t(after.lSizes[_NORMAL_BLOCK]) - std::int64_t(before.lSizes[_NORMAL_BLOCK]) +
        std::int64_t(after.lSizes[_CLIENT_BLOCK]) - std::int64_t(before.lSizes[_CLIENT_BLOCK]);
    CHECK(observed.valid);
    if (deny == 0 && denyAt == 0 && !denyMd5Work)
        CHECK(std::int64_t(observed.live) - std::int64_t(observed.baselineFreed) == delta);
    return observed;
}
void calibrate()
{
    const auto block = observe([] { juce::MemoryBlock b(1024, false); });
    CHECK(block.live == 0 && block.peak == 1024 && block.allocated == 1024 && block.freed == 1024);
    void *baseline = _malloc_dbg(80, _NORMAL_BLOCK, __FILE__, __LINE__);
    if (!CHECK(baseline != nullptr))
        return;
    bool ok = false;
    const auto resize = observe(
        [&]
        {
            void *p = _malloc_dbg(64, _NORMAL_BLOCK, __FILE__, __LINE__);
            if (p)
            {
                void *q = _realloc_dbg(p, 128, _NORMAL_BLOCK, __FILE__, __LINE__);
                ok = q != nullptr;
                _free_dbg(q ? q : p, _NORMAL_BLOCK);
            }
            _free_dbg(baseline, _NORMAL_BLOCK);
        });
    CHECK(ok && resize.live == 0 && resize.peak == 128 && resize.overlap == 192);
    CHECK(resize.allocated == 192 && resize.freed == 192 && resize.baselineFreed == 80);
    bool deniedOk = false;
    const auto denied = observe(
        [&]
        {
            void *p = _malloc_dbg(12345, _NORMAL_BLOCK, __FILE__, __LINE__);
            deniedOk = p == nullptr;
            if (p)
                _free_dbg(p, _NORMAL_BLOCK);
        },
        12345);
    CHECK(deniedOk && denied.denials == 1 && denied.live == 0);
}
std::size_t peak(const Result &r) noexcept
{
    return std::max(r.peak, r.overlap);
}
} // namespace prepHeap
#endif

// Setup outside observed regions. Streams the real source file with fixed64KiB scratch.
juce::File capacityPcm(const juce::File &root, const char *name, unsigned rate, unsigned channels,
                       std::uint64_t frames)
{
    const auto file = root.getChildFile(name);
    const auto dataBytes = frames * channels * 2;
    CHECK(dataBytes <= UINT32_MAX - 36);
    Bytes header;
    tag(header, "RIFF");
    number(header, 36 + dataBytes, 4);
    tag(header, "WAVE");
    tag(header, "fmt ");
    number(header, 16, 4);
    number(header, 1, 2);
    number(header, channels, 2);
    number(header, rate, 4);
    number(header, rate * channels * 2, 4);
    number(header, channels * 2, 2);
    number(header, 16, 2);
    tag(header, "data");
    number(header, dataBytes, 4);
    const auto h =
        CreateFileW(file.getFullPathName().toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ,
                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!CHECK(h != INVALID_HANDLE_VALUE))
        return file;
    DWORD done = 0;
    bool ok =
        WriteFile(h, header.data(), DWORD(header.size()), &done, nullptr) && done == header.size();
    std::array<std::uint8_t, 65536> block{};
    for (std::size_t i = 0; i < block.size(); i += 2)
    {
        // +.125 mono; opposite stereo. Int16 representation is exact on decode.
        const auto value = std::uint16_t(channels == 2 && ((i / 2) & 1) ? -4096 : 4096);
        block[i] = std::uint8_t(value);
        block[i + 1] = std::uint8_t(value >> 8);
    }
    for (std::uint64_t left = dataBytes; ok && left;)
    {
        const auto n = DWORD(std::min<std::uint64_t>(left, block.size()));
        ok = WriteFile(h, block.data(), n, &done, nullptr) && done == n;
        left -= ok ? n : 0;
    }
    CHECK(CloseHandle(h));
    CHECK(ok);
    return file;
}
PreparedDrumSample retainedDestination()
{
    const std::array<float, 2> values{.25f, -.5f};
    PreparedDrumSample old;
    AssetError ae{};
    CHECK(makeSampleAsset(values, 1, old.asset, ae));
    old.peak = .5f;
    old.source.sourceFileId.fill(0x5a);
    old.source.sourceFrames = 2;
    old.source.end = 2;
    return old;
}
struct HealthyCancel
{
    preparation_test::Phase phase{};
    std::uint64_t at = 0;
    std::atomic_bool *flag = nullptr;
    bool requested = false;
    std::chrono::steady_clock::time_point when{};
    static void progress(preparation_test::Phase phase, std::uint64_t position, void *raw) noexcept
    {
        auto &self = *static_cast<HealthyCancel *>(raw);
        if (!self.requested && self.phase == phase && position >= self.at)
        {
            self.when = std::chrono::steady_clock::now();
            self.requested = true;
            self.flag->store(true, std::memory_order_release);
        }
    }
};
struct PreparationControlGuard
{
    preparation_test::Control control;
    PreparationControlGuard() { preparation_test::useControl(&control); }
    ~PreparationControlGuard() { preparation_test::useControl(nullptr); }
};

void rejectBeforeAllocation(const juce::File &file, PreparationError expected)
{
    PreparationControlGuard guard;
    std::atomic_bool cancel{false};
    PreparationError error{};
    std::unique_ptr<DrumSource> destination;
    bool accepted = false;
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto measured = prepHeap::observe(
        [&] { accepted = inspectDrumSource(file, std::nullopt, cancel, destination, error); });
    CHECK(prepHeap::peak(measured) <= 8u * 1024 * 1024);
    std::cout << "ADMISSION_ALLOC section=" << section << " encoded_bytes=" << file.getSize()
              << " requested_peak=" << prepHeap::peak(measured)
              << " decoder_constructions=" << guard.control.decoderConstructions
              << " canonical_allocations=" << guard.control.canonicalAllocations << '\n';
#else
    accepted = inspectDrumSource(file, std::nullopt, cancel, destination, error);
#endif
    CHECK(!accepted && error == expected && !destination);
    CHECK(guard.control.decoderConstructions == 0 && guard.control.canonicalAllocations == 0);
}

void failurePrecedence(const juce::File &root)
{
    begin("latched physical I/O failure survives a later hash allocation failure");
#if defined(_MSC_VER) && defined(_DEBUG)
    const std::array<std::uint32_t, 2> bits{0, 0x3e800000};
    const auto file = write(root, wav(bits, 1));
    (void)file.getFullPathName().toWideCharPointer();
    PreparationControlGuard guard;
    auto source = inspect(file);
    if (!source)
        return;
    auto *const old = source.get();
    const auto oldId = source->info().fileId;
    std::atomic_bool cancel{false};
    PreparationError error{};
    bool accepted = false;
    CHECK(guard.control.decoderConstructions == 1 && guard.control.canonicalAllocations == 0);
    PreparedDrumSample prepared;
    CHECK(prepareDrumSample(*source, 0, 2, 1, cancel, prepared, error));
    CHECK(guard.control.decoderConstructions == 2 && guard.control.canonicalAllocations == 1);
    guard.control.decoderConstructions = guard.control.canonicalAllocations = 0;
    CHECK(!prepareDrumSample(*source, 0, 3, 1, cancel, prepared, error));
    CHECK(error == PreparationError::invalidSelection);
    CHECK(!prepareDrumSample(*source, 0, 2, 0, cancel, prepared, error));
    CHECK(error == PreparationError::invalidGain);
    CHECK(guard.control.decoderConstructions == 0 && guard.control.canonicalAllocations == 0);
    guard.control.readMode = preparation_test::ReadMode::readError;
    guard.control.readPhase = preparation_test::Phase::hash;
    guard.control.readOffset = 0;
    const auto denied = prepHeap::observe(
        [&] { accepted = inspectDrumSource(file, std::nullopt, cancel, source, error); }, 32);
    CHECK(denied.denials == 1 && !accepted && error == PreparationError::ioError);
    CHECK(source.get() == old && source->info().fileId == oldId);
#else
    (void)root;
#endif
}

void md5AllocationFailures(const juce::File &root)
{
    begin("valid FLAC MD5 workspace and fallback allocation failure");
#if defined(_MSC_VER) && defined(_DEBUG)
    const auto file = write(root, flac());
    CHECK(file.getSize() == 117);
    (void)file.getFullPathName().toWideCharPointer();
    PreparationControlGuard guard;
    guard.control.progress = prepHeap::md5Progress;
    std::atomic_bool cancel{false};
    PreparationError error{};
    std::unique_ptr<DrumSource> source;
    bool accepted = false;
    const auto positive = prepHeap::observe(
        [&] { accepted = inspectDrumSource(file, std::nullopt, cancel, source, error); });
    CHECK(accepted && source && error == PreparationError::none);
    // Two residual buffers precede the 64-byte interleaved PCM16 MD5 workspace.
    CHECK(positive.md5Requests == 3 && positive.denials == 0);
    if (!source)
        return;
    const auto *old = source.get();
    const auto oldId = source->info().fileId;
    const auto oldPcmId = source->info().decodedPcmId;
    PreparedDrumSample prepared;
    CHECK(prepareDrumSample(*source, 0, 16, .5f, cancel, prepared, error));
    const auto oldPrepared = prepared;
    const auto inspectDenied = prepHeap::observe(
        [&] { accepted = inspectDrumSource(file, std::nullopt, cancel, source, error); }, 0, 0,
        true);
    std::cout << "MD5_OOM inspect accepted=" << accepted << " error=" << int(error)
              << " requests64=" << inspectDenied.md5Requests
              << " denials=" << inspectDenied.denials << '\n';
    CHECK(!accepted && inspectDenied.md5Requests == 4 && inspectDenied.denials == 2);
    CHECK(error == PreparationError::allocationFailed);
    CHECK(source.get() == old && source->info().fileId == oldId &&
          source->info().decodedPcmId == oldPcmId && !writable(file));
    const auto prepareDenied = prepHeap::observe(
        [&] { accepted = prepareDrumSample(*source, 0, 16, .5f, cancel, prepared, error); }, 0, 0,
        true);
    std::cout << "MD5_OOM prepare accepted=" << accepted << " error=" << int(error)
              << " requests64=" << prepareDenied.md5Requests
              << " denials=" << prepareDenied.denials << '\n';
    CHECK(!accepted && prepareDenied.md5Requests == 4 && prepareDenied.denials == 2);
    CHECK(error == PreparationError::allocationFailed);
    CHECK(prepared.asset == oldPrepared.asset && prepared.source == oldPrepared.source &&
          prepared.peak == oldPrepared.peak && !writable(file));
    CHECK(prepareDrumSample(*source, 0, 16, .5f, cancel, prepared, error));
    CHECK(prepared.asset->id() == oldPrepared.asset->id());
    source.reset();
    CHECK(writable(file));
#else
    (void)root;
#endif
}

void capacity(const juce::File &root)
{
    begin("exclusive preparation CRT workspace and healthy-file cancellation");
#if defined(_MSC_VER) && defined(_DEBUG)
    prepHeap::calibrate();
    constexpr std::size_t inspectorBound = 8u * 1024 * 1024, preparationBound = 16u * 1024 * 1024;
    constexpr std::size_t sharedPrototype = 49153u * sizeof(double);
    static_assert(sharedPrototype == 393224);
    const auto longFile = capacityPcm(root, "long-600s-8k.wav", 8000, 1, 600ull * 8000);
    const auto maximumFile = capacityPcm(root, "maximum-10s-384k.wav", 384000, 2, 10ull * 384000);
    std::atomic_bool cancel{false};
    PreparationError error{};
    std::unique_ptr<DrumSource> longSource, maximumSource;
    bool accepted = false;
    const auto longInspect = prepHeap::observe(
        [&] { accepted = inspectDrumSource(longFile, std::nullopt, cancel, longSource, error); });
    CHECK(accepted && error == PreparationError::none && longSource != nullptr);
    CHECK(prepHeap::peak(longInspect) <= inspectorBound);
    if (!longSource)
        return;
    CHECK(longSource->info().frames == 4800000 && longSource->info().rate == 8000 &&
          longSource->info().buckets == 512);
    auto out = retainedDestination();
    const auto old = out;
    const auto oldBytes = old.asset ? old.asset->samples().size() * sizeof(float) : 0;
    const auto longPrepare = prepHeap::observe(
        [&]
        { accepted = prepareDrumSample(*longSource, 4792000, 4800000, .5f, cancel, out, error); });
    CHECK(accepted && error == PreparationError::none && out.asset != nullptr);
    if (out.asset)
        CHECK(out.asset->frames() == 48000 && out.source.begin == 4792000 &&
              out.source.end == 4800000);
    CHECK(prepHeap::peak(longPrepare) + longInspect.live <= preparationBound);
    std::cout << "PREPARATION_WORKSPACE source=600s8k inspector_peak="
              << prepHeap::peak(longInspect) << " held_source=" << longInspect.live
              << " preparation_peak=" << prepHeap::peak(longPrepare)
              << " prep_plus_held=" << prepHeap::peak(longPrepare) + longInspect.live
              << " old_destination_pcm=" << oldBytes
              << " shared_table_additional=" << sharedPrototype
              << " retained_result=" << longPrepare.live << '\n';
    out = {};
    longSource.reset();
    const auto maxInspect = prepHeap::observe(
        [&]
        { accepted = inspectDrumSource(maximumFile, std::nullopt, cancel, maximumSource, error); });
    CHECK(accepted && error == PreparationError::none && maximumSource != nullptr);
    CHECK(prepHeap::peak(maxInspect) <= inspectorBound);
    if (!maximumSource)
        return;
    CHECK(maximumSource->info().frames == 3840000 && maximumSource->info().rate == 384000 &&
          maximumSource->info().channels == 2);
    out = old;
    const auto maxPrepare = prepHeap::observe(
        [&] { accepted = prepareDrumSample(*maximumSource, 0, 3840000, .5f, cancel, out, error); });
    CHECK(accepted && error == PreparationError::none && out.asset != nullptr);
    if (out.asset)
    {
        CHECK(out.asset->frames() == 480000 && out.asset->channels() == 2 && out.peak <= 1);
        bool oppositeStereo = true;
        const auto samples = out.asset->samples();
        for (std::size_t i = 0; i < samples.size(); i += 2)
            oppositeStereo = oppositeStereo && samples[i] == .0625f && samples[i + 1] == -.0625f;
        CHECK(oppositeStereo);
    }
    CHECK(prepHeap::peak(maxPrepare) + maxInspect.live <= preparationBound);
    std::cout << "PREPARATION_WORKSPACE source=10s384kstereo inspector_peak="
              << prepHeap::peak(maxInspect) << " held_source=" << maxInspect.live
              << " preparation_peak=" << prepHeap::peak(maxPrepare)
              << " realloc_overlap=" << maxPrepare.overlap
              << " prep_plus_held=" << prepHeap::peak(maxPrepare) + maxInspect.live
              << " old_destination_pcm=" << oldBytes
              << " shared_table_additional=" << sharedPrototype
              << " retained_result=" << maxPrepare.live << " allocations=" << maxPrepare.allocations
              << " reallocations=" << maxPrepare.reallocations << '\n';
    std::cout << "PREP_REQUESTS";
    for (auto n : maxPrepare.requests)
        if (n)
            std::cout << ' ' << n;
    std::cout << '\n';

    // A unique canonical hash allocation is reached after both conversion and factory-owned PCM
    // allocation. It tests late factory OOM, rather than only denying the first canonical vector.
    const auto hashRequest =
        *std::max_element(maxPrepare.requests.begin(), maxPrepare.requests.end());
    CHECK(hashRequest >= 3840020 && hashRequest < 3840020 + 256);
    out = old;
    const auto denied = prepHeap::observe(
        [&] { accepted = prepareDrumSample(*maximumSource, 0, 3840000, .5f, cancel, out, error); },
        hashRequest);
    CHECK(denied.denials == 1 && !accepted && error == PreparationError::allocationFailed);
    CHECK(out.asset == old.asset && out.source == old.source && out.peak == old.peak);
    std::cout << "PREPARATION_DENIAL factory_canonical_hash_payload=3840020 actual_STL_request="
              << hashRequest << " denials=" << denied.denials << '\n';

    for (bool retainRice : {false, true})
    {
        const auto malformed = write(root, maximumDisagreement(retainRice));
        // Warm the caller File's native-path cache before charging helper workspace.
        (void)malformed.getFullPathName().toWideCharPointer();
        auto *const oldPointer = maximumSource.get();
        const auto oldId = maximumSource->info().fileId;
        const auto measured = prepHeap::observe(
            [&]
            {
                accepted = inspectDrumSource(malformed, std::nullopt, cancel, maximumSource, error);
            });
        CHECK(!accepted && error == PreparationError::malformedSource);
        CHECK(maximumSource.get() == oldPointer && maximumSource->info().fileId == oldId);
        // Exact2,097,120-byte allocation occurs in pre-client MD5 formatting, after CRC-correct
        // full frame decode.
        CHECK(measured.sawMaxMd5);
        if (retainRice)
            CHECK(measured.sawLegalMd5);
        CHECK(measured.live == 0 && prepHeap::peak(measured) <= inspectorBound);
        std::cout << "PREPARATION_FLAC_MAX retain_rice=" << retainRice
                  << " requested_peak=" << measured.peak
                  << " realloc_overlap_upper=" << measured.overlap
                  << " max_md5=" << measured.sawMaxMd5 << " legal_old_md5=" << measured.sawLegalMd5
                  << " reallocations=" << measured.reallocations << '\n';
        std::cout << "FLAC_LIVE live=" << measured.live
                  << " baseline_freed=" << measured.baselineFreed << " requests";
        for (auto n : measured.requests)
            if (n)
                std::cout << ' ' << n;
        std::cout << '\n';
        const auto deniedInternal = prepHeap::observe(
            [&]
            {
                accepted = inspectDrumSource(malformed, std::nullopt, cancel, maximumSource, error);
            },
            262156);
        CHECK(deniedInternal.denials == 1 && !accepted &&
              error == PreparationError::allocationFailed);
        CHECK(maximumSource.get() == oldPointer && maximumSource->info().fileId == oldId);
    }
    // No scripted ReadMode: these are real healthy disk sources. Request-to-return wall time,
    // not virtual-deadline arithmetic; this does not prove kernel-pending/failed-device latency.
    using Phase = preparation_test::Phase;
    for (auto phase : {Phase::preflight, Phase::hash, Phase::decode, Phase::beforeCommit})
    {
        PreparationControlGuard guard;
        cancel = false;
        HealthyCancel request{phase,
                              phase == Phase::hash     ? 65536ull
                              : phase == Phase::decode ? 4096ull
                                                       : 0ull,
                              &cancel};
        guard.control.progress = HealthyCancel::progress;
        guard.control.context = &request;
        auto *const oldPointer = maximumSource.get();
        accepted = inspectDrumSource(maximumFile, std::nullopt, cancel, maximumSource, error);
        const auto returned = std::chrono::steady_clock::now();
        CHECK(request.requested && !accepted && error == PreparationError::cancelled &&
              maximumSource.get() == oldPointer);
        if (request.requested)
        {
            const auto elapsed =
                std::chrono::duration<double, std::milli>(returned - request.when).count();
            CHECK(elapsed <= 250.0);
            std::cout << "HEALTHY_CANCEL operation=inspect phase=" << int(phase)
                      << " request_to_return_ms=" << elapsed << '\n';
        }
    }
    for (auto phase : {Phase::decode, Phase::resample, Phase::beforeCommit})
    {
        PreparationControlGuard guard;
        cancel = false;
        out = old;
        HealthyCancel request{phase,
                              phase == Phase::decode     ? 4096ull
                              : phase == Phase::resample ? 64ull
                                                         : 0ull,
                              &cancel};
        guard.control.progress = HealthyCancel::progress;
        guard.control.context = &request;
        accepted = prepareDrumSample(*maximumSource, 0, 3840000, .5f, cancel, out, error);
        const auto returned = std::chrono::steady_clock::now();
        CHECK(request.requested && !accepted && error == PreparationError::cancelled);
        CHECK(out.asset == old.asset && out.source == old.source && out.peak == old.peak);
        if (request.requested)
        {
            const auto elapsed =
                std::chrono::duration<double, std::milli>(returned - request.when).count();
            CHECK(elapsed <= 250.0);
            std::cout << "HEALTHY_CANCEL operation=prepare phase=" << int(phase)
                      << " request_to_return_ms=" << elapsed << '\n';
        }
    }
    cancel = false;
#else
    (void)root;
    std::cerr << "capacity requires calibrated MSVC Debug CRT; gate unavailable\n";
    CHECK(false);
#endif
}

// Integration notes:
// * At max canonical stereo size, canonical/owned/hash payloads alone overlap at11,520,020 bytes.
// * peak=max(live requested peak,realloc old/new upper overlap); neither cumulative bytes nor
// operator-new-only totals suffice.
// * Held source requested bytes and existing caller destination are disclosed independently.
// Table393,224 is additional
//   when its static storage is not observed by CRT; do not silently count it as zero or call16MiB a
//   whole-process ceiling.
// * Preserve actual FAIL outputs. A denied hash allocation path may trigger MSVC/JUCE allocation
// diagnostics; do not relabel
//   an assertion/abort as transactional OOM success. If allocator behavior aborts, report the
//   actual failure and fix fixture/code.
// * Only the parent invokes this opt-in mode through RackSynthNativeRun --exclusive; no foreign
// process control.
// * The actual helper path must report sawMaxMd5, malformedSource and an unchanged old source;
//   CRC rejection before MD5 does not satisfy the maximum-allocation witness.

} // namespace
int main(int argc, char **argv)
{
    const bool qualifiedMode = argc == 4 && std::string_view(argv[2]) == "--qualified-fixtures";
    const bool capacityMode = argc == 3 && std::string_view(argv[2]) == "--capacity";
    if (argc != 2 && !qualifiedMode && !capacityMode)
        return 2;
    const juce::File supplied(juce::String::fromUTF8(argv[1]));
    CHECK(supplied.createDirectory().wasOk());
    const auto root = supplied.getChildFile("owned-" + juce::Uuid().toString());
    CHECK(root.createDirectory().wasOk());
    if (capacityMode)
        capacity(root);
    else if (qualifiedMode)
        qualified(juce::File(juce::String::fromUTF8(argv[3])));
    else
    {
        literal(root);
        failurePrecedence(root);
        md5AllocationFailures(root);
        ioAndCancellation(root);
        codecs(root);
        flacChecks(root);
        admission(root);
        transactions(root);
        quality(root);
        formatEdges(root);
        boundaries(root);
    }
    CHECK(root.isAChildOf(supplied));
    if (root.isAChildOf(supplied))
        CHECK(root.deleteRecursively());
    std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
    return failures ? 1 : 0;
}

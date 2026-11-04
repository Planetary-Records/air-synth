#include "DrumAsset.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocation_test { bool failNext = false; }
void* operator new(std::size_t n)
{
    if (allocation_test::failNext) { allocation_test::failNext = false; throw std::bad_alloc(); }
    if (auto* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace airack::drums;
namespace
{
int checks = 0, failures = 0, sections = 0;
const char* section = "";
bool check(bool pass, const char* expression, int line)
{
    ++checks;
    if (!pass) { ++failures; if (failures < 1000) std::cerr << section << ':' << line << ' ' << expression << '\n'; }
    return pass;
}
#define CHECK(x) check(bool(x), #x, __LINE__)
void begin(const char* name) { section = name; ++sections; std::cout << name << '\n'; }
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::uint8_t, 60> golden {
    0x41,0x52,0x53,0x41,0x4d,0x50,0x30,0x31, 0x80,0xbb,0,0,
    2,0,0,0, 1,0,0,0, 0,0,0x80,0x3e, 0,0,0,0xbf,
    0x4e,0x6d,0xc7,0xc8,0xfd,0xf3,0xc6,0xca,0xfd,0xbe,0x16,0xe1,0x4e,0x49,0xa1,0xac,
    0xe8,0x90,0x78,0xd5,0x34,0x93,0x8b,0x74,0x88,0xcc,0xb4,0x5a,0x07,0x1f,0xa3,0x31
};
Bytes bytes(const juce::MemoryBlock& b)
{
    const auto* p = static_cast<const std::uint8_t*>(b.getData());
    return b.getSize() == 0 ? Bytes{} : Bytes(p, p + b.getSize());
}
void put(Bytes& b, std::size_t offset, std::uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) b[offset + i] = static_cast<std::uint8_t>(value >> (8 * i)); }
void rehash(Bytes& b)
{
    const auto digest = juce::SHA256(b.data(), b.size() - 32).getRawData();
    std::memcpy(b.data() + b.size() - 32, digest.getData(), 32);
}
bool sameBits(std::span<const float> a, std::span<const float> b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float x, float y)
        { return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y); });
}
AssetPtr fixture()
{
    const std::array<float, 2> input { .25f, -.5f };
    AssetPtr asset; AssetError error = AssetError::invalidBlob;
    CHECK(makeSampleAsset(input, 1, asset, error));
    CHECK(error == AssetError::none);
    return asset;
}
void rejectMake(std::span<const float> input, std::uint32_t channels, AssetError expected, const AssetPtr& old)
{
    auto dest = old; AssetError error = AssetError::none;
    CHECK(!makeSampleAsset(input, channels, dest, error));
    CHECK(error == expected); CHECK(dest == old);
}
void rejectDecode(std::span<const std::uint8_t> input, AssetError expected, const AssetPtr& old)
{
    auto dest = old; AssetError error = AssetError::none;
    CHECK(!decodeSampleAsset(input, dest, error));
    CHECK(error == expected); CHECK(dest == old);
}
void literalGoldenAndIdentity()
{
    begin("literal golden bytes and identity");
    static_assert(std::is_same_v<AssetId, std::array<std::uint8_t, 32>>);
    static_assert(std::is_same_v<AssetPtr, std::shared_ptr<const SampleAsset>>);
    static_assert(!std::is_default_constructible_v<SampleAsset> && !std::is_copy_assignable_v<SampleAsset>);
    static_assert(std::is_same_v<decltype(std::declval<const SampleAsset&>().samples()), std::span<const float>>);
    auto asset = fixture(); if (!CHECK(asset != nullptr)) return;
    CHECK(asset->channels() == 1); CHECK(asset->frames() == 2);
    CHECK(assetIdText(asset->id()) == "4e6dc7c8fdf3c6cafdbe16e14e49a1ace89078d534938b7488ccb45a071fa331");
    juce::MemoryBlock encoded; AssetError error = AssetError::invalidBlob;
    if (!CHECK(encodeSampleAsset(*asset, encoded, error))) return;
    CHECK(error == AssetError::none); CHECK(bytes(encoded) == Bytes(golden.begin(), golden.end()));
    AssetPtr decoded;
    if (!CHECK(decodeSampleAsset(golden, decoded, error)) || !CHECK(decoded != nullptr)) return;
    CHECK(error == AssetError::none); CHECK(decoded->id() == asset->id());
    CHECK(sameBits(decoded->samples(), asset->samples()));
    auto repeat = fixture(); if (!CHECK(repeat != nullptr)) return;
    juce::MemoryBlock encodedAgain;
    CHECK(encodeSampleAsset(*repeat, encodedAgain, error)); CHECK(encodedAgain == encoded);
    CHECK(repeat->id() == asset->id());
}
void exactBitsAndFullDuration()
{
    begin("stereo payload, signed zero, full ten seconds and independent storage");
    std::array<float, 8> input { -0.0f, 0.0f, 16.0f, -16.0f,
        std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min(), .25f, -.5f };
    const auto original = input;
    AssetPtr asset; AssetError error = AssetError::invalidBlob;
    if (!CHECK(makeSampleAsset(input, 2, asset, error)) || !CHECK(asset != nullptr)) return;
    CHECK(error == AssetError::none); input.fill(1.0f);
    CHECK(asset->channels() == 2); CHECK(asset->frames() == 4); CHECK(sameBits(asset->samples(), original));
    AssetPtr singleFrame;
    CHECK(makeSampleAsset(std::span<const float>(original).first(2), 2, singleFrame, error));
    if (CHECK(singleFrame != nullptr)) CHECK(singleFrame->frames() == 1);
    juce::MemoryBlock blob;
    if (!CHECK(encodeSampleAsset(*asset, blob, error))) return;
    const std::array<std::uint8_t, 32> payload {
        0,0,0,0x80, 0,0,0,0, 0,0,0x80,0x41, 0,0,0x80,0xc1,
        1,0,0,0, 1,0,0,0x80, 0,0,0x80,0x3e, 0,0,0,0xbf };
    const auto wire = bytes(blob);
    CHECK(std::equal(payload.begin(), payload.end(), wire.begin() + 20));
    AssetPtr restored;
    CHECK(decodeSampleAsset(wire, restored, error));
    if (CHECK(restored != nullptr)) CHECK(sameBits(restored->samples(), original));
    const std::array<float, 1> plus { 0.0f }, minus { -0.0f };
    AssetPtr a, b;
    CHECK(makeSampleAsset(plus, 1, a, error)); CHECK(makeSampleAsset(minus, 1, b, error));
    if (CHECK(a != nullptr && b != nullptr)) CHECK(a->id() != b->id());
    std::vector<float> full(480000 * 2);
    for (std::size_t i = 0; i < full.size(); ++i) full[i] = (static_cast<int>(i % 31) - 15) * .0625f;
    if (!CHECK(makeSampleAsset(full, 2, asset, error))) return;
    CHECK(asset->frames() == 480000); CHECK(asset->channels() == 2);
    CHECK(encodeSampleAsset(*asset, blob, error)); CHECK(blob.getSize() == 3840052);
    CHECK(decodeSampleAsset(bytes(blob), restored, error));
    if (CHECK(restored != nullptr)) CHECK(sameBits(restored->samples(), full));
}
void factoryRejection()
{
    begin("factory bounds preserve destination");
    const auto old = fixture(); if (!CHECK(old != nullptr)) return;
    const std::array<float, 3> input { .1f, .2f, .3f };
    rejectMake({}, 1, AssetError::invalidGeometry, old);
    rejectMake(input, 0, AssetError::invalidGeometry, old);
    rejectMake(input, 3, AssetError::invalidGeometry, old);
    rejectMake(input, 2, AssetError::invalidGeometry, old);
    std::vector<float> tooLongMono(480001), tooLongStereo(960002);
    rejectMake(tooLongMono, 1, AssetError::tooLong, old);
    rejectMake(tooLongStereo, 2, AssetError::tooLong, old);
    for (float x : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity() })
    { const std::array<float, 1> bad { x }; rejectMake(bad, 1, AssetError::nonFinite, old); }
    for (float x : { std::nextafter(16.0f, 17.0f), std::nextafter(-16.0f, -17.0f) })
    { const std::array<float, 1> bad { x }; rejectMake(bad, 1, AssetError::outOfRange, old); }
}
void decoderRejection()
{
    begin("strict blob admission and correctly signed invalid payloads");
    const auto old = fixture(); if (!CHECK(old != nullptr)) return;
    const Bytes good(golden.begin(), golden.end());
    for (std::size_t size = 0; size < good.size(); ++size)
        rejectDecode(std::span<const std::uint8_t>(good.data(), size), AssetError::invalidBlob, old);
    auto bad = good; bad.push_back(0); rejectDecode(bad, AssetError::invalidBlob, old);
    bad.assign(3840053, 0); rejectDecode(bad, AssetError::invalidBlob, old);
    bad = good; bad[0] ^= 1; rehash(bad); rejectDecode(bad, AssetError::invalidBlob, old);
    bad = good; put(bad, 8, 44100); rehash(bad); rejectDecode(bad, AssetError::invalidBlob, old);
    for (auto channels : { 0u, 3u, 0xffffffffu })
    { bad = good; put(bad, 16, channels); rehash(bad); rejectDecode(bad, AssetError::invalidGeometry, old); }
    bad = good; put(bad, 12, 0); rehash(bad); rejectDecode(bad, AssetError::invalidGeometry, old);
    for (auto frames : { 480001u, 0xffffffffu })
    { bad = good; put(bad, 12, frames); rehash(bad); rejectDecode(bad, AssetError::tooLong, old); }
    bad = good; put(bad, 12, 1); rehash(bad); rejectDecode(bad, AssetError::invalidBlob, old);
    bad = good; put(bad, 12, 0xffffffffu); put(bad, 16, 0xffffffffu); rehash(bad);
    rejectDecode(bad, AssetError::invalidGeometry, old);
    bad = good; bad[20] ^= 1; rejectDecode(bad, AssetError::digestMismatch, old);
    bad = good; bad.back() ^= 1; rejectDecode(bad, AssetError::digestMismatch, old);
    for (auto bits : { 0x7fc00000u, 0x7f800000u, 0xff800000u })
    { bad = good; put(bad, 20, bits); rehash(bad); rejectDecode(bad, AssetError::nonFinite, old); }
    for (auto bits : { 0x41800001u, 0xc1800001u })
    { bad = good; put(bad, 20, bits); rehash(bad); rejectDecode(bad, AssetError::outOfRange, old); }
}
void textAndAllocationFailures()
{
    begin("canonical text and allocation failures preserve destinations");
    const auto old = fixture(); if (!CHECK(old != nullptr)) return;
    const auto text = assetIdText(old->id()); AssetId parsed {};
    CHECK(parseAssetIdText(text, parsed)); CHECK(parsed == old->id());
    std::vector<std::string> invalid { text.substr(1), text + "0", "0x" + text.substr(2),
        " " + text.substr(1), text.substr(0, 63) + "\n" };
    auto altered = text; altered[0] = 'A'; invalid.push_back(altered);
    altered = text; altered[0] = 'g'; invalid.push_back(altered);
    altered = text; altered[1] = '\0'; invalid.push_back(altered);
    altered = text; altered[1] = static_cast<char>(0xc3); altered[2] = static_cast<char>(0xa9); invalid.push_back(altered);
    for (const auto& bad : invalid)
    { parsed.fill(0xa5); const auto before = parsed; CHECK(!parseAssetIdText(bad, parsed)); CHECK(parsed == before); }
    const std::array<float, 2> input { .25f, -.5f };
    auto dest = old; AssetError error = AssetError::none;
    allocation_test::failNext = true;
    CHECK(!makeSampleAsset(input, 1, dest, error));
    CHECK(!allocation_test::failNext); CHECK(error == AssetError::allocationFailed); CHECK(dest == old);
    const std::array<std::uint8_t, 3> marker { 7, 8, 9 };
    juce::MemoryBlock encoded(marker.data(), marker.size());
    allocation_test::failNext = true;
    CHECK(!encodeSampleAsset(*old, encoded, error));
    CHECK(!allocation_test::failNext); CHECK(error == AssetError::allocationFailed);
    CHECK(bytes(encoded) == Bytes(marker.begin(), marker.end()));
    allocation_test::failNext = true;
    CHECK(!decodeSampleAsset(golden, dest, error));
    CHECK(!allocation_test::failNext); CHECK(error == AssetError::allocationFailed); CHECK(dest == old);
}
void boundedMalformedFuzz()
{
    begin("bounded deterministic mutation and random fuzz");
    const auto old = fixture(); if (!CHECK(old != nullptr)) return;
    std::uint32_t state = 0x20261022u;
    auto random = [&]() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; };
    for (unsigned trial = 0; trial < 512; ++trial)
    {
        Bytes input;
        if (trial % 2 == 0)
        {
            input.assign(golden.begin(), golden.end());
            const auto offset = random() % input.size(); input[offset] ^= static_cast<std::uint8_t>(1u << (random() % 8));
            if (trial % 4 == 0) rehash(input);
        }
        else { input.resize(random() % 257); for (auto& x : input) x = static_cast<std::uint8_t>(random()); }
        auto dest = old; AssetError error = AssetError::none;
        if (decodeSampleAsset(input, dest, error))
        {
            CHECK(error == AssetError::none); if (!CHECK(dest != nullptr)) continue;
            juce::MemoryBlock encoded; CHECK(encodeSampleAsset(*dest, encoded, error)); CHECK(bytes(encoded) == input);
        }
        else { CHECK(error != AssetError::none); CHECK(dest == old); }
    }
}
}
int main()
{
    try
    {
        literalGoldenAndIdentity(); exactBitsAndFullDuration(); factoryRejection();
        decoderRejection(); textAndAllocationFailures(); boundedMalformedFuzz();
    }
    catch (const std::exception& e) { ++failures; std::cerr << "unexpected exception: " << e.what() << '\n'; }
    std::cout << checks << " checks, " << sections << " sections, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

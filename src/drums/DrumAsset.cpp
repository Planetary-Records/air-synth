#include "DrumAsset.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace airack::drums
{
namespace
{
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
constexpr std::uint32_t sampleRate = 48000, maxFrames = 480000;
constexpr std::size_t headerBytes = 20, digestBytes = 32;
constexpr std::size_t maxBlobBytes = headerBytes + std::size_t(maxFrames) * 2 * 4 + digestBytes;
constexpr std::array<std::uint8_t, 8> magic { 'A', 'R', 'S', 'A', 'M', 'P', '0', '1' };
bool fail(AssetError value, AssetError& error) noexcept { error = value; return false; }
void put(std::uint8_t* p, std::uint32_t value) noexcept
{ for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i)); }
std::uint32_t get(const std::uint8_t* p) noexcept
{
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(p[i]) << (8 * i);
    return value;
}
bool validate(std::span<const float> data, std::uint32_t channels, AssetError& error) noexcept
{
    if ((channels != 1 && channels != 2) || data.empty() || data.size() % channels != 0)
        return fail(AssetError::invalidGeometry, error);
    if (data.size() / channels > maxFrames) return fail(AssetError::tooLong, error);
    for (float x : data)
    {
        if (!std::isfinite(x)) return fail(AssetError::nonFinite, error);
        if (std::abs(x) > 16.0f) return fail(AssetError::outOfRange, error);
    }
    return true;
}
std::vector<std::uint8_t> canonicalBytes(std::span<const float> data, std::uint32_t channels)
{
    std::vector<std::uint8_t> result(headerBytes + data.size() * 4);
    std::copy(magic.begin(), magic.end(), result.begin());
    put(result.data() + 8, sampleRate);
    put(result.data() + 12, static_cast<std::uint32_t>(data.size() / channels));
    put(result.data() + 16, channels);
    for (std::size_t i = 0; i < data.size(); ++i)
        put(result.data() + headerBytes + i * 4, std::bit_cast<std::uint32_t>(data[i]));
    return result;
}
AssetId digestOf(std::span<const std::uint8_t> data)
{
    const auto raw = juce::SHA256(data.data(), data.size()).getRawData();
    AssetId id {};
    std::memcpy(id.data(), raw.getData(), id.size());
    return id;
}
}

SampleAsset::SampleAsset(AssetId id, std::uint32_t channels, std::vector<float> samples) noexcept
    : id_(id), channels_(channels), samples_(std::move(samples)) {}

bool makeSampleAsset(std::span<const float> data, std::uint32_t channels,
                     AssetPtr& destination, AssetError& error) noexcept
{
    if (!validate(data, channels, error)) return false;
    try
    {
        std::vector<float> owned(data.begin(), data.end());
        const auto id = digestOf(canonicalBytes(owned, channels));
        AssetPtr candidate(new SampleAsset(id, channels, std::move(owned)));
        destination = std::move(candidate);
        error = AssetError::none;
        return true;
    }
    catch (const std::bad_alloc&) { return fail(AssetError::allocationFailed, error); }
}

bool encodeSampleAsset(const SampleAsset& asset, juce::MemoryBlock& destination, AssetError& error) noexcept
{
    try
    {
        auto bytes = canonicalBytes(asset.samples(), asset.channels());
        bytes.insert(bytes.end(), asset.id().begin(), asset.id().end());
        juce::MemoryBlock candidate(bytes.data(), bytes.size());
        destination.swapWith(candidate);
        error = AssetError::none;
        return true;
    }
    catch (const std::bad_alloc&) { return fail(AssetError::allocationFailed, error); }
}

bool decodeSampleAsset(std::span<const std::uint8_t> bytes, AssetPtr& destination, AssetError& error) noexcept
{
    if (bytes.size() < headerBytes + digestBytes || bytes.size() > maxBlobBytes)
        return fail(AssetError::invalidBlob, error);
    if (!std::equal(magic.begin(), magic.end(), bytes.begin()) || get(bytes.data() + 8) != sampleRate)
        return fail(AssetError::invalidBlob, error);
    const auto frames = get(bytes.data() + 12), channels = get(bytes.data() + 16);
    if ((channels != 1 && channels != 2) || frames == 0) return fail(AssetError::invalidGeometry, error);
    if (frames > maxFrames) return fail(AssetError::tooLong, error);
    // Bounds precede multiplication, allocation and hashing of an untrusted payload.
    const auto count = std::size_t(frames) * channels;
    const auto bodyBytes = headerBytes + count * 4;
    if (bytes.size() != bodyBytes + digestBytes) return fail(AssetError::invalidBlob, error);
    try
    {
        const auto id = digestOf(bytes.first(bodyBytes));
        if (!std::equal(id.begin(), id.end(), bytes.begin() + bodyBytes))
            return fail(AssetError::digestMismatch, error);
        std::vector<float> samples(count);
        for (std::size_t i = 0; i < count; ++i)
            samples[i] = std::bit_cast<float>(get(bytes.data() + headerBytes + i * 4));
        return makeSampleAsset(samples, channels, destination, error);
    }
    catch (const std::bad_alloc&) { return fail(AssetError::allocationFailed, error); }
}

std::string assetIdText(const AssetId& id)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text(64, '0');
    for (std::size_t i = 0; i < id.size(); ++i)
    { text[2 * i] = digits[id[i] >> 4]; text[2 * i + 1] = digits[id[i] & 15]; }
    return text;
}

bool parseAssetIdText(std::string_view text, AssetId& destination) noexcept
{
    if (text.size() != 64) return false;
    AssetId candidate {};
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto c = text[i];
        const int nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (nibble < 0) return false;
        candidate[i / 2] |= static_cast<std::uint8_t>(nibble << (i % 2 == 0 ? 4 : 0));
    }
    destination = candidate;
    return true;
}
}

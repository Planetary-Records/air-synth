#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace juce { class MemoryBlock; }
namespace airack::drums
{
using AssetId = std::array<std::uint8_t, 32>;
class SampleAsset;
using AssetPtr = std::shared_ptr<const SampleAsset>;
enum class AssetError : std::uint8_t
{
    none, invalidGeometry, tooLong, nonFinite, outOfRange,
    invalidBlob, digestMismatch, allocationFailed
};

// Construction, serialization and shared ownership belong on the control thread.
bool makeSampleAsset(std::span<const float> interleaved, std::uint32_t channels,
                     AssetPtr& destination, AssetError& error) noexcept;
class SampleAsset final
{
public:
    SampleAsset(const SampleAsset&) = delete;
    SampleAsset& operator=(const SampleAsset&) = delete;
    const AssetId& id() const noexcept { return id_; }
    std::uint32_t channels() const noexcept { return channels_; }
    std::uint32_t frames() const noexcept { return static_cast<std::uint32_t>(samples_.size() / channels_); }
    std::span<const float> samples() const noexcept { return samples_; }
private:
    SampleAsset(AssetId, std::uint32_t, std::vector<float>) noexcept;
    friend bool makeSampleAsset(std::span<const float>, std::uint32_t, AssetPtr&, AssetError&) noexcept;
    const AssetId id_;
    const std::uint32_t channels_;
    const std::vector<float> samples_;
};
bool encodeSampleAsset(const SampleAsset&, juce::MemoryBlock& destination, AssetError&) noexcept;
bool decodeSampleAsset(std::span<const std::uint8_t>, AssetPtr& destination, AssetError&) noexcept;
std::string assetIdText(const AssetId&);
bool parseAssetIdText(std::string_view, AssetId& destination) noexcept;
}

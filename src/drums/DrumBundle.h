#pragma once
#include "DrumDocument.h"

namespace airack::drums
{
inline constexpr std::size_t maxCompactDocumentBytes = 1048576;
inline constexpr std::size_t maxBundleAudioBytes = 256 * 1024 * 1024;
inline constexpr std::size_t maxDrumBundleBytes = 24 + maxCompactDocumentBytes + maxDrumDocumentBytes + maxBundleAudioBytes + 32;
struct BundleGeometry final
{
    std::uint64_t decodedBytes=0,largestDecodedAsset=0;
    std::size_t encodedBytes=0,compactBytes=0,manifestBytes=0;
    std::uint32_t assets=0;
    bool operator==(const BundleGeometry&) const=default;
};
bool inspectDrumBundleGeometry(std::span<const std::uint8_t>,BundleGeometry&,DrumError&) noexcept;
class DrumBundle;
using DrumBundlePtr = std::shared_ptr<const DrumBundle>;
bool makeDrumBundle(DrumDocumentPtr, std::span<const std::uint8_t> compact,
                    std::span<const AssetPtr>, DrumBundlePtr&, DrumError&) noexcept;
class DrumBundle final
{
public:
    DrumBundle(const DrumBundle&) = delete;
    DrumBundle& operator=(const DrumBundle&) = delete;
    const DrumDocument& document() const noexcept { return *document_; }
    std::span<const std::uint8_t> compactDocument() const noexcept { return compact_; }
    std::span<const AssetPtr> assets() const noexcept { return assets_; }
    std::uint64_t decodedAudioBytes() const noexcept { return decodedBytes_; }
    const SampleAsset* findAsset(const AssetId&) const noexcept;
private:
    DrumBundle(DrumDocumentPtr, std::vector<std::uint8_t>, std::vector<AssetPtr>, std::uint64_t) noexcept;
    friend bool makeDrumBundle(DrumDocumentPtr, std::span<const std::uint8_t>, std::span<const AssetPtr>, DrumBundlePtr&, DrumError&) noexcept;
    const DrumDocumentPtr document_;
    const std::vector<std::uint8_t> compact_;
    const std::vector<AssetPtr> assets_;
    const std::uint64_t decodedBytes_;
};
bool encodeDrumBundle(const DrumBundle&, juce::MemoryBlock&, DrumError&) noexcept;
bool decodeDrumBundle(std::span<const std::uint8_t>, DrumBundlePtr&, DrumError&) noexcept;
}

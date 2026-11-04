#pragma once
#include "DrumAsset.h"
#include <optional>

namespace airack::drums
{
inline constexpr std::size_t maxDrumDocumentBytes = 65536;
enum class DrumError : std::uint8_t
{
    none, invalidDocument, invalidBlob, digestMismatch, missingAsset,
    unexpectedAsset, tooManyAssets, tooLarge, invalidAsset, allocationFailed
};
enum class SourceKind : std::uint8_t { imported = 1, generated = 2 };
struct SourceProvenance
{
    SourceKind kind = SourceKind::imported;
    AssetId sourceFileId {};
    std::uint32_t sourceRate = 48000, sourceChannels = 1;
    std::uint64_t sourceFrames = 0, begin = 0, end = 0;
    float gain = 1;
    std::string generator, condition;
    std::uint64_t seed = 0;
    AssetId promptId {};
    bool operator==(const SourceProvenance&) const = default;
};
struct DrumPad
{
    std::optional<AssetId> asset;
    std::string name;
    std::optional<SourceProvenance> source;
    bool operator==(const DrumPad&) const = default;
};
using DrumKit = std::array<DrumPad, 16>;
using DrumSlots = std::array<DrumKit, 4>;
class DrumDocument;
using DrumDocumentPtr = std::shared_ptr<const DrumDocument>;
bool makeDrumDocument(const DrumSlots&, DrumDocumentPtr&, DrumError&) noexcept;
class DrumDocument final
{
public:
    DrumDocument(const DrumDocument&) = delete;
    DrumDocument& operator=(const DrumDocument&) = delete;
    const DrumSlots& slots() const noexcept { return slots_; }
private:
    explicit DrumDocument(DrumSlots) noexcept;
    friend bool makeDrumDocument(const DrumSlots&, DrumDocumentPtr&, DrumError&) noexcept;
    const DrumSlots slots_;
};
bool encodeDrumDocument(const DrumDocument&, juce::MemoryBlock&, DrumError&) noexcept;
bool decodeDrumDocument(std::span<const std::uint8_t>, DrumDocumentPtr&, DrumError&) noexcept;
}

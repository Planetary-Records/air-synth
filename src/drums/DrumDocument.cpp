#include "DrumDocument.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

namespace airack::drums
{
namespace
{
constexpr std::array<std::uint8_t, 8> magic { 'A','R','D','O','C','0','0','1' };
bool fail(DrumError value, DrumError& error) noexcept { error = value; return false; }
bool textValid(const std::string& s, std::size_t cap) noexcept
{
    return s.size() <= cap && std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; })
        && juce::CharPointer_UTF8::isValidString(s.data(), static_cast<int>(s.size()));
}
bool valid(const DrumSlots& slots) noexcept
{
    for (const auto& kit : slots) for (const auto& pad : kit)
    {
        if (!textValid(pad.name, 64) || (pad.source && !pad.asset)) return false;
        if (!pad.source) continue;
        const auto& s = *pad.source;
        if ((s.kind != SourceKind::imported && s.kind != SourceKind::generated) || s.sourceRate == 0 || s.sourceRate > 384000
            || (s.sourceChannels != 1 && s.sourceChannels != 2) || s.begin >= s.end || s.end > s.sourceFrames
            || s.end - s.begin > std::uint64_t(s.sourceRate) * 10 || !std::isfinite(s.gain) || s.gain <= 0 || s.gain > 16)
            return false;
        if (s.kind == SourceKind::generated)
        { if (s.generator.empty() || s.condition.empty() || !textValid(s.generator, 96) || !textValid(s.condition, 128)) return false; }
        else if (!s.generator.empty() || !s.condition.empty() || s.seed != 0 || s.promptId != AssetId{}) return false;
    }
    return true;
}
void number(std::vector<std::uint8_t>& out, std::uint64_t x, unsigned width)
{ for (unsigned i = 0; i < width; ++i) out.push_back(static_cast<std::uint8_t>(x >> (8 * i))); }
void text(std::vector<std::uint8_t>& out, const std::string& s)
{ number(out, s.size(), 2); out.insert(out.end(), s.begin(), s.end()); }
void id(std::vector<std::uint8_t>& out, const AssetId& value) { out.insert(out.end(), value.begin(), value.end()); }
struct Reader
{
    std::span<const std::uint8_t> data; std::size_t position = 0; bool good = true;
    std::span<const std::uint8_t> take(std::size_t n) noexcept
    {
        if (!good || n > data.size() - position) { good = false; return {}; }
        auto result = data.subspan(position, n); position += n; return result;
    }
    std::uint64_t number(unsigned width) noexcept
    {
        const auto b = take(width); std::uint64_t result = 0;
        if (good) for (unsigned i = 0; i < width; ++i) result |= std::uint64_t(b[i]) << (8 * i);
        return result;
    }
    std::string text(std::size_t cap)
    {
        const auto n = number(2); if (n > cap) { good = false; return {}; }
        const auto b = take(static_cast<std::size_t>(n));
        return good && !b.empty() ? std::string(reinterpret_cast<const char*>(b.data()), b.size()) : std::string{};
    }
    AssetId id() noexcept
    { AssetId result {}; const auto b = take(32); if (good) std::copy(b.begin(), b.end(), result.begin()); return result; }
};
}
DrumDocument::DrumDocument(DrumSlots slots) noexcept : slots_(std::move(slots)) {}
bool makeDrumDocument(const DrumSlots& slots, DrumDocumentPtr& dest, DrumError& error) noexcept
{
    if (!valid(slots)) return fail(DrumError::invalidDocument, error);
    try
    { DrumDocumentPtr candidate(new DrumDocument(slots)); dest = std::move(candidate); error = DrumError::none; return true; }
    catch (const std::bad_alloc&) { return fail(DrumError::allocationFailed, error); }
}
bool encodeDrumDocument(const DrumDocument& doc, juce::MemoryBlock& dest, DrumError& error) noexcept
{
    try
    {
        std::vector<std::uint8_t> out(magic.begin(), magic.end());
        for (const auto& kit : doc.slots()) for (const auto& pad : kit)
        {
            text(out, pad.name); number(out, pad.asset ? 1 : 0, 1); if (!pad.asset) continue;
            id(out, *pad.asset); number(out, pad.source ? 1 : 0, 1); if (!pad.source) continue;
            const auto& s = *pad.source;
            number(out, static_cast<std::uint8_t>(s.kind), 1); id(out, s.sourceFileId);
            number(out, s.sourceRate, 4); number(out, s.sourceChannels, 1); number(out, s.sourceFrames, 8);
            number(out, s.begin, 8); number(out, s.end, 8); number(out, std::bit_cast<std::uint32_t>(s.gain), 4);
            if (s.kind == SourceKind::generated) { text(out, s.generator); text(out, s.condition); number(out, s.seed, 8); id(out, s.promptId); }
        }
        if (out.size() > maxDrumDocumentBytes - 32) return fail(DrumError::tooLarge, error);
        const auto digest = juce::SHA256(out.data(), out.size()).getRawData();
        const auto* d = static_cast<const std::uint8_t*>(digest.getData()); out.insert(out.end(), d, d + 32);
        juce::MemoryBlock candidate(out.data(), out.size()); dest.swapWith(candidate); error = DrumError::none; return true;
    }
    catch (const std::bad_alloc&) { return fail(DrumError::allocationFailed, error); }
}
bool decodeDrumDocument(std::span<const std::uint8_t> bytes, DrumDocumentPtr& dest, DrumError& error) noexcept
{
    if (bytes.size() < 232 || bytes.size() > maxDrumDocumentBytes || !std::equal(magic.begin(), magic.end(), bytes.begin()))
        return fail(DrumError::invalidBlob, error);
    try
    {
        const auto digest = juce::SHA256(bytes.data(), bytes.size() - 32).getRawData();
        if (std::memcmp(digest.getData(), bytes.data() + bytes.size() - 32, 32) != 0) return fail(DrumError::digestMismatch, error);
        Reader in { bytes.subspan(8, bytes.size() - 40) }; DrumSlots slots;
        for (auto& kit : slots) for (auto& pad : kit)
        {
            pad.name = in.text(64); const auto hasAsset = in.number(1);
            if (!in.good || hasAsset > 1) return fail(DrumError::invalidBlob, error);
            if (!hasAsset) continue;
            pad.asset = in.id(); const auto hasSource = in.number(1);
            if (!in.good || hasSource > 1) return fail(DrumError::invalidBlob, error);
            if (!hasSource) continue;
            SourceProvenance s; s.kind = static_cast<SourceKind>(in.number(1));
            if (s.kind != SourceKind::imported && s.kind != SourceKind::generated) return fail(DrumError::invalidDocument, error);
            s.sourceFileId = in.id(); s.sourceRate = static_cast<std::uint32_t>(in.number(4)); s.sourceChannels = static_cast<std::uint32_t>(in.number(1));
            s.sourceFrames = in.number(8); s.begin = in.number(8); s.end = in.number(8);
            s.gain = std::bit_cast<float>(static_cast<std::uint32_t>(in.number(4)));
            if (s.kind == SourceKind::generated) { s.generator = in.text(96); s.condition = in.text(128); s.seed = in.number(8); s.promptId = in.id(); }
            pad.source = std::move(s);
        }
        if (!in.good || in.position != in.data.size()) return fail(DrumError::invalidBlob, error);
        return makeDrumDocument(slots, dest, error);
    }
    catch (const std::bad_alloc&) { return fail(DrumError::allocationFailed, error); }
}
}

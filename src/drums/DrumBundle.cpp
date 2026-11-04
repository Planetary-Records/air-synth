#include "DrumBundle.h"
#include <juce_cryptography/juce_cryptography.h>
#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace airack::drums
{
namespace
{
constexpr std::array<std::uint8_t, 8> magic { 'A','R','B','N','D','L','0','1' };
bool fail(DrumError value, DrumError& error) noexcept { error = value; return false; }
std::uint32_t get(const std::uint8_t* p) noexcept
{ return p[0] | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24; }
void put(std::uint8_t*& out, std::uint32_t value) noexcept
{ for (unsigned i = 0; i < 4; ++i) *out++ = static_cast<std::uint8_t>(value >> (8 * i)); }
const SampleAsset* find(std::span<const AssetPtr> assets, const AssetId& id) noexcept
{
    const auto found = std::lower_bound(assets.begin(), assets.end(), id, [](const AssetPtr& a, const AssetId& b) { return a->id() < b; });
    return found != assets.end() && (*found)->id() == id ? found->get() : nullptr;
}
}
DrumBundle::DrumBundle(DrumDocumentPtr doc, std::vector<std::uint8_t> compact, std::vector<AssetPtr> assets, std::uint64_t decoded) noexcept
    : document_(std::move(doc)), compact_(std::move(compact)), assets_(std::move(assets)), decodedBytes_(decoded) {}
const SampleAsset* DrumBundle::findAsset(const AssetId& id) const noexcept { return find(assets_, id); }
bool makeDrumBundle(DrumDocumentPtr doc, std::span<const std::uint8_t> compact,
                    std::span<const AssetPtr> input, DrumBundlePtr& dest, DrumError& error) noexcept
{
    if (!doc) return fail(DrumError::invalidDocument, error);
    if (compact.size() > maxCompactDocumentBytes) return fail(DrumError::tooLarge, error);
    if (input.size() > 64) return fail(DrumError::tooManyAssets, error);
    if (std::any_of(input.begin(), input.end(), [](const AssetPtr& a) { return !a; })) return fail(DrumError::invalidAsset, error);
    try
    {
        std::vector<AssetPtr> assets(input.begin(), input.end());
        std::sort(assets.begin(), assets.end(), [](const AssetPtr& a, const AssetPtr& b) { return a->id() < b->id(); });
        assets.erase(std::unique(assets.begin(), assets.end(), [](const AssetPtr& a, const AssetPtr& b) { return a->id() == b->id(); }), assets.end());
        for (const auto& kit : doc->slots()) for (const auto& pad : kit)
            if (pad.asset && !find(assets, *pad.asset)) return fail(DrumError::missingAsset, error);
        std::uint64_t decoded = 0, bodies = 0;
        for (const auto& asset : assets)
        {
            bool used = false;
            for (const auto& kit : doc->slots()) for (const auto& pad : kit) if (pad.asset && *pad.asset == asset->id()) used = true;
            if (!used) return fail(DrumError::unexpectedAsset, error);
            const auto n = std::uint64_t(asset->frames()) * asset->channels() * 4;
            decoded += n; bodies += n + 56;
            if (decoded > maxBundleAudioBytes || bodies > maxBundleAudioBytes) return fail(DrumError::tooLarge, error);
        }
        DrumBundlePtr candidate(new DrumBundle(std::move(doc), std::vector<std::uint8_t>(compact.begin(), compact.end()), std::move(assets), decoded));
        dest = std::move(candidate); error = DrumError::none; return true;
    }
    catch (const std::bad_alloc&) { return fail(DrumError::allocationFailed, error); }
}
bool encodeDrumBundle(const DrumBundle& bundle, juce::MemoryBlock& dest, DrumError& error) noexcept
{
    try
    {
        juce::MemoryBlock manifest; if (!encodeDrumDocument(bundle.document(), manifest, error)) return false;
        const auto compact = bundle.compactDocument();
        std::size_t total = 24 + compact.size() + manifest.getSize() + 32;
        for (const auto& a : bundle.assets()) total += 4 + 52 + a->samples().size() * 4;
        // The serialized audio section includes each four-byte record length.
        if (total > maxDrumBundleBytes) return fail(DrumError::tooLarge, error);
        juce::MemoryBlock candidate(total, false);
        auto* const begin = static_cast<std::uint8_t*>(candidate.getData()); auto* p = begin;
        std::memcpy(p, magic.data(), magic.size()); p += magic.size();
        put(p, static_cast<std::uint32_t>(compact.size())); put(p, static_cast<std::uint32_t>(manifest.getSize()));
        put(p, static_cast<std::uint32_t>(bundle.assets().size())); put(p, 0);
        if (!compact.empty()) std::memcpy(p, compact.data(), compact.size());
        p += compact.size();
        std::memcpy(p, manifest.getData(), manifest.getSize()); p += manifest.getSize();
        for (const auto& asset : bundle.assets())
        {
            juce::MemoryBlock blob; AssetError assetError;
            if (!encodeSampleAsset(*asset, blob, assetError)) return fail(assetError == AssetError::allocationFailed ? DrumError::allocationFailed : DrumError::invalidAsset, error);
            put(p, static_cast<std::uint32_t>(blob.getSize()));
            std::memcpy(p, blob.getData(), blob.getSize()); p += blob.getSize();
        }
        jassert(p == begin + total - 32);
        const auto digest = juce::SHA256(begin, total - 32).getRawData();
        std::memcpy(p, digest.getData(), digest.getSize());
        dest.swapWith(candidate); error = DrumError::none; return true;
    }
    catch (const std::bad_alloc&) { return fail(DrumError::allocationFailed, error); }
}
bool inspectDrumBundleGeometry(std::span<const std::uint8_t> bytes,BundleGeometry& output,DrumError& error) noexcept
{
    if(bytes.size()<24+232+32||bytes.size()>maxDrumBundleBytes
        ||!std::equal(magic.begin(),magic.end(),bytes.begin()))return fail(DrumError::invalidBlob,error);
    const auto compact=get(bytes.data()+8),manifest=get(bytes.data()+12),count=get(bytes.data()+16);
    if(get(bytes.data()+20)!=0)return fail(DrumError::invalidBlob,error);
    if(compact>maxCompactDocumentBytes||manifest>maxDrumDocumentBytes)return fail(DrumError::tooLarge,error);
    if(count>64)return fail(DrumError::tooManyAssets,error);
    const auto end=bytes.size()-32;
    if(manifest<232||compact>end-24||manifest>end-24-compact)return fail(DrumError::invalidBlob,error);
    BundleGeometry candidate;candidate.encodedBytes=bytes.size();candidate.compactBytes=compact;
    candidate.manifestBytes=manifest;candidate.assets=count;
    std::size_t position=24+std::size_t(compact)+manifest,bodies=0;
    constexpr std::array<std::uint8_t,8> sampleMagic{'A','R','S','A','M','P','0','1'};
    for(std::uint32_t i=0;i<count;++i){
        if(position>end||end-position<4)return fail(DrumError::invalidBlob,error);
        const auto size=get(bytes.data()+position);position+=4;
        if(size<56||size>3840052||size>end-position)return fail(DrumError::invalidBlob,error);
        if(std::size_t(size)+4>maxBundleAudioBytes-bodies)return fail(DrumError::tooLarge,error);
        bodies+=std::size_t(size)+4;
        const auto* sample=bytes.data()+position;
        const auto rate=get(sample+8),frames=get(sample+12),channels=get(sample+16);
        if(!std::equal(sampleMagic.begin(),sampleMagic.end(),sample)||rate!=48000
            ||frames<1||frames>480000||(channels!=1&&channels!=2))return fail(DrumError::invalidAsset,error);
        const auto decoded=std::uint64_t(frames)*channels*4;
        if(decoded+52!=size)return fail(DrumError::invalidBlob,error);
        if(decoded>maxBundleAudioBytes-candidate.decodedBytes)return fail(DrumError::tooLarge,error);
        candidate.decodedBytes+=decoded;
        candidate.largestDecodedAsset=std::max(candidate.largestDecodedAsset,decoded);
        position+=size;
    }
    if(position!=end)return fail(DrumError::invalidBlob,error);
    output=candidate;error=DrumError::none;return true;
}
bool decodeDrumBundle(std::span<const std::uint8_t> bytes, DrumBundlePtr& dest, DrumError& error) noexcept
{
    if (bytes.size() < 24 + 232 + 32 || bytes.size() > maxDrumBundleBytes || !std::equal(magic.begin(), magic.end(), bytes.begin()))
        return fail(DrumError::invalidBlob, error);
    const auto compactBytes = get(bytes.data() + 8), manifestBytes = get(bytes.data() + 12), count = get(bytes.data() + 16);
    if (get(bytes.data() + 20) != 0) return fail(DrumError::invalidBlob, error);
    if (compactBytes > maxCompactDocumentBytes || manifestBytes > maxDrumDocumentBytes) return fail(DrumError::tooLarge, error);
    if (count > 64) return fail(DrumError::tooManyAssets, error);
    if (manifestBytes < 232 || std::size_t(compactBytes) + manifestBytes > bytes.size() - 56) return fail(DrumError::invalidBlob, error);
    try
    {
        const auto digest = juce::SHA256(bytes.data(), bytes.size() - 32).getRawData();
        if (std::memcmp(digest.getData(), bytes.data() + bytes.size() - 32, 32) != 0) return fail(DrumError::digestMismatch, error);
        const auto compact = bytes.subspan(24, compactBytes); DrumDocumentPtr doc;
        if (!decodeDrumDocument(bytes.subspan(24 + compactBytes, manifestBytes), doc, error)) return false;
        std::vector<AssetPtr> assets; assets.reserve(count);
        std::size_t position = 24 + compactBytes + manifestBytes, bodies = 0;
        const auto end = bytes.size() - 32;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (end - position < 4) return fail(DrumError::invalidBlob, error);
            const auto n = get(bytes.data() + position); position += 4;
            if (n < 56 || n > 3840052 || n > end - position) return fail(DrumError::invalidBlob, error);
            if (std::size_t(n) + 4 > maxBundleAudioBytes - bodies) return fail(DrumError::tooLarge, error);
            bodies += std::size_t(n) + 4; AssetPtr asset; AssetError assetError;
            if (!decodeSampleAsset(bytes.subspan(position, n), asset, assetError))
                return fail(assetError == AssetError::allocationFailed ? DrumError::allocationFailed : DrumError::invalidAsset, error);
            if (!assets.empty() && !(assets.back()->id() < asset->id())) return fail(DrumError::invalidBlob, error);
            assets.push_back(std::move(asset)); position += n;
        }
        if (position != end) return fail(DrumError::invalidBlob, error);
        return makeDrumBundle(std::move(doc), compact, assets, dest, error);
    }
    catch (const std::bad_alloc&) { return fail(DrumError::allocationFailed, error); }
}
}

#pragma once
#include "RackDesign.h"
#include <functional>
#include <span>
#include <string_view>

namespace airack {
namespace rackPackageLimits {
inline constexpr std::uint64_t manifestBytes=65536, designBytes=65536, skinBytes=1048576;
inline constexpr std::uint64_t archiveBytes=384ull*1024*1024, expandedBytes=archiveBytes, fileCount=256;
inline constexpr std::uint64_t pngBytes=8ull*1024*1024, pngEdge=8192, pngPixels=16777216, decodedSkinBytes=64ull*1024*1024;
inline constexpr std::size_t pathBytes=160, idBytes=64, versionBytes=32, tagCount=8, tagBytes=32;
inline constexpr std::size_t nameBytes=128, descriptionBytes=4096, makerBytes=128, purposeBytes=512;
inline constexpr std::size_t installedCount=512, embeddedCount=12, inventoryBytes=40*1024*1024, canonicalInventoryBytes=32*1024*1024;
inline constexpr std::size_t catalogBytes=4*1024*1024, tagDictionaryCount=128, tagLabelBytes=64, releasesPerRack=64;
inline constexpr std::size_t countPages=16, countPageReleases=100;
}
enum class RackLibraryStatus : std::uint8_t {
    ready, unavailable, invalidData, incompatible, identityConflict, byteLimit, countLimit,
    cancelled, timeout, networkFailure, rateLimited, storageFailure, busy
};
using RackLibraryCancel=std::function<bool()>;
struct RackTag { juce::String id,label; };
struct RackMetadata {
    juce::Uuid design; std::uint64_t revision=0;
    juce::String canonicalSha256,name,maker,purpose,description;
    RackRole role=RackRole::synth; juce::StringArray tags;
};
struct RackPackageFile { juce::String path,sha256,kind; std::uint64_t bytes=0; };
struct RackPackageManifest {
    RackPackageRef ref; RackMetadata metadata;
    juce::String designPath,skinPath,screenshotPath;
    std::vector<RackPackageFile> files; juce::var capabilities;
};
bool decodeRackPackageManifest(std::string_view,RackPackageManifest&,RackLibraryStatus* =nullptr);
bool encodeRackPackageManifest(const RackPackageManifest&,juce::String&,RackLibraryStatus* =nullptr);
bool validRackPackagePath(const juce::String&);
bool validRackPackageId(const juce::String&);
bool validRackPackageVersion(const juce::String&);
bool validRackTagId(const juce::String&);
bool validateRackTagDictionary(std::string_view,std::vector<RackTag>&,RackLibraryStatus* =nullptr);
bool validateRackPackageDesign(const RackPackageManifest&,std::string_view,RackDesign&,RackLibraryStatus* =nullptr);
// IHDR inspection precedes image decoding; no image allocation occurs here.
bool inspectRackPackagePng(std::span<const std::byte>,std::uint32_t& width,std::uint32_t& height,RackLibraryStatus* =nullptr);
// Pass the same zero-initialized decoded counter for every PNG before publishing a package.
bool validateRackPackageFileBytes(const RackPackageFile&,std::span<const std::byte>,RackLibraryStatus* =nullptr,
                                  std::uint64_t* decodedPngBytes=nullptr);
}

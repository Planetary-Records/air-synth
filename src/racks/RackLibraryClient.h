#pragma once
#include "RackPackageStore.h"
#include <optional>
#include "RackInventory.h"
#include <juce_graphics/juce_graphics.h>

namespace airack {
struct RackHttpRequest { juce::String url,etag; std::uint64_t maximumBytes=0; };
struct RackHttpResponse { int code=0; juce::String etag,location,link,retryAfter,rateLimitRemaining,rateLimitReset; };
using RackHttpSink=std::function<bool(const void*,std::size_t)>;
using RackLibraryTransport=std::function<RackLibraryStatus(const RackHttpRequest&,const RackHttpSink&,const RackLibraryCancel&,RackHttpResponse&)>;
struct RackRemoteRelease {
    RackMetadata metadata; RackPackageRef ref;
    juce::String archiveUrl,screenshotUrl,screenshotSha256;
    std::uint64_t archiveAssetId=0,archiveBytes=0,screenshotBytes=0;
    bool compatible=false; juce::String incompatibility;
};
struct RackDownloadCount { juce::String id; std::optional<std::uint64_t> value; };
struct RackLibraryClientView {
    std::uint64_t generation=0,job=0,bytesDone=0,bytesTotal=0;
    bool busy=false,cached=false,offline=false,countsStale=true;
    RackLibraryStatus status=RackLibraryStatus::unavailable;
    juce::String message;
    std::vector<RackTag> tags;
    std::vector<RackRemoteRelease> releases;
    std::vector<RackDownloadCount> counts;
    std::vector<RackPackageRef> committed;
};
namespace studio {struct MakerSkin;}
class RackLibrarySkinPreparation;
class RackLibraryInventoryPreparation;
class RackLibraryPlacementPreparation;
using RackLibraryInventoryPreparationPtr=std::shared_ptr<RackLibraryInventoryPreparation>;
using RackLibraryPlacementPreparationPtr=std::shared_ptr<RackLibraryPlacementPreparation>;
struct RackLibraryInventoryView {
    RackInventoryPtr snapshot;
    RackLibraryStatus status=RackLibraryStatus::unavailable;
    std::uint64_t revision=0,storeGeneration=0;
    bool busy=false;
};
struct RackLibraryPlacementView {
    RackInventoryPtr root;
    std::vector<RackPackageLeasePtr> leases;
    RackLibraryStatus status=RackLibraryStatus::unavailable;
    std::uint64_t revision=0,storeGeneration=0;
};
using RackLibrarySkinPreparationPtr=std::shared_ptr<RackLibrarySkinPreparation>;
using RackLibrarySkinPreparer=std::function<std::shared_ptr<const studio::MakerSkin>(const RackInstance&,const RackInventorySnapshot*,const RackLibraryCancel&)>;
struct RackLibraryThumbnail {RackLibraryStatus status=RackLibraryStatus::unavailable;juce::Image image;};
class RackLibraryClient final {
public:
    RackLibraryClient(std::shared_ptr<RackPackageStore>,RackLibraryTransport);
    ~RackLibraryClient();
    std::uint64_t refresh();
    std::uint64_t openLibrary();
    std::uint64_t revealLibraryFolder();
    std::uint64_t install(const RackPackageRef&);
    std::uint64_t restore(const RackPackageRef&);
    std::uint64_t remove(const RackPackageRef&);
    void cancel(std::uint64_t);
    std::shared_ptr<const RackLibraryClientView> view() const;
    void requestThumbnail(const RackPackageRef&,unsigned maximumEdge=512);
    std::shared_ptr<const RackLibraryThumbnail> thumbnail(const RackPackageRef&,unsigned maximumEdge=512) const;
    void requestBuiltInThumbnail(const RackMetadata&,unsigned maximumEdge=512);
    std::shared_ptr<const RackLibraryThumbnail> builtInThumbnail(const RackMetadata&,unsigned maximumEdge=512) const;
    RackLibrarySkinPreparationPtr createSkinPreparation(RackLibrarySkinPreparer);
    void prepareSkins(const RackLibrarySkinPreparationPtr&,RackInventoryPtr,std::span<const RackInstance>);
    void requestSkin(const RackLibrarySkinPreparationPtr&,const RackInstance&);
    std::shared_ptr<const studio::MakerSkin> preparedSkin(const RackLibrarySkinPreparationPtr&,const RackInstance&) const;
    RackLibraryInventoryPreparationPtr createInventoryPreparation();
    void requestInventory(const RackLibraryInventoryPreparationPtr&,std::span<const RackInstance>);
    void retryInventory(const RackLibraryInventoryPreparationPtr&);
    void cancelInventory(const RackLibraryInventoryPreparationPtr&);
    std::shared_ptr<const RackLibraryInventoryView> inventory(const RackLibraryInventoryPreparationPtr&) const;
    RackLibraryPlacementPreparationPtr createPlacementPreparation();
    void requestPlacement(const RackLibraryPlacementPreparationPtr&,RackInventoryPtr,std::span<const RackPackageRef>);
    void cancelPlacement(const RackLibraryPlacementPreparationPtr&);
    std::shared_ptr<const RackLibraryPlacementView> placement(const RackLibraryPlacementPreparationPtr&) const;
    bool placementCurrent(const RackLibraryPlacementPreparationPtr&,const RackLibraryPlacementView&) const noexcept;
private:
    struct Impl; std::unique_ptr<Impl> impl;
};
struct RackLibraryRuntime { std::shared_ptr<RackPackageStore> store; std::shared_ptr<RackLibraryClient> client; };
std::shared_ptr<RackLibraryRuntime> defaultRackLibraryRuntime();
inline constexpr auto rackLibraryOwner="Planetary-Records";
inline constexpr auto rackLibraryRepository="air-rack-library";
inline constexpr auto rackLibraryCatalogUrl="https://raw.githubusercontent.com/Planetary-Records/air-rack-library/main/catalog.json";
inline constexpr auto rackLibraryCountsUrl="https://api.github.com/repos/Planetary-Records/air-rack-library/releases?per_page=100&page=1";
bool validRackLibraryUrl(const juce::String&,bool redirect=false);
RackLibraryTransport rackLibraryHttpWindows();
}

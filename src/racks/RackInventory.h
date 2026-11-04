#pragma once
#include "RackPackageStore.h"

namespace airack {
namespace ai { enum class CatalogRendering : std::uint8_t; }
enum class RackLibraryCollection { all, installed, updates };
struct RackLibraryQuery {
    juce::String text;
    std::optional<RackRole> role;
    juce::StringArray tags;
    RackLibraryCollection collection=RackLibraryCollection::all;
};
struct RackInventoryRow {
    RackMetadata metadata;
    std::optional<RackPackageRef> package;
    bool builtIn=false,installed=false,updateAvailable=false;
};
class RackInventorySnapshot;
using RackInventoryPtr=std::shared_ptr<const RackInventorySnapshot>;
class RackInventorySnapshot final {
public:
    ~RackInventorySnapshot();
    std::span<const RackInventoryRow> rows() const;
    juce::Uuid id() const;
    std::uint64_t storeGeneration() const;
    RackPackageLeasePtr openPackage(const RackPackageRef&,RackLibraryStatus* =nullptr,const RackLibraryCancel& ={}) const;
private:
    struct Impl;
    RackInventorySnapshot(std::unique_ptr<Impl>,const RackPackageStore&);
    std::unique_ptr<const Impl> impl;
    friend bool captureRackInventory(const RackPackageStore&,std::span<const RackInstance>,RackInventoryPtr&,RackLibraryStatus*,const RackLibraryCancel&);
    friend bool resolveInventoryDesign(const RackInventorySnapshot&,juce::Uuid,std::uint64_t,const juce::String&,RackDesign&,std::optional<RackPackageRef>&);
    friend juce::var rackDiscoveryDescriptor(const RackInventorySnapshot&,ai::CatalogRendering);
};
// Worker-thread capture performs filesystem I/O and transient package validation.
bool captureRackInventory(const RackPackageStore&,std::span<const RackInstance>,RackInventoryPtr&,RackLibraryStatus* =nullptr,const RackLibraryCancel& ={});
std::vector<std::size_t> queryRackRows(std::span<const RackInventoryRow>,const RackLibraryQuery&,std::span<const RackTag> tags={});
std::vector<std::size_t> queryRackInventory(const RackInventorySnapshot&,const RackLibraryQuery&,std::span<const RackTag> tags={});
bool resolveInventoryDesign(const RackInventorySnapshot&,juce::Uuid,std::uint64_t,const juce::String&,RackDesign&,std::optional<RackPackageRef>&);
juce::var rackDiscoveryDescriptor(const RackInventorySnapshot&,ai::CatalogRendering);
}

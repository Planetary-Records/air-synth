#pragma once
#include "RackPackage.h"
#include <memory>

namespace airack {
#if defined(AIRACK_STORE_TESTS)
namespace testing {
struct RenameTrial {
    int errors[2]{-1,-1}; int attempts=0, waits=0, elapsedMs=0;
    bool virtualTime=false, holdChild=false;
    std::function<void(int,const juce::File&)> event;
};
void setRenameTrial(RenameTrial*) noexcept;
void forceNextStoreRenameFailure(bool) noexcept;
bool storeRenameChildHeld() noexcept;
std::optional<juce::File> mcpTestRackLibraryRoot(const juce::String&);
void releaseStoreRenameChild() noexcept;
}
#endif
struct RackLibraryPaths {
    juce::File root;
    static RackLibraryPaths forCurrentUser();
};
class RackPackageLease final {
public:
    ~RackPackageLease();
    const RackPackageManifest& manifest() const;
    std::string_view canonicalRecipe() const;
    std::span<const NativeModule> unitModules() const;
    // Failure leaves output unchanged. Paths must exactly match the manifest.
    RackLibraryStatus readListed(const juce::String& path, juce::MemoryBlock& output) const;
private:
    struct Impl;
    explicit RackPackageLease(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
    friend class RackPackageStore;
};
using RackPackageLeasePtr = std::shared_ptr<const RackPackageLease>;
enum class RackPackageActivation : std::uint8_t { makeActive, preserveActive };
class RackPackageStore final {
public:
    explicit RackPackageStore(RackLibraryPaths);
    ~RackPackageStore();
    RackPackageStore(const RackPackageStore&)=delete;
    RackPackageStore& operator=(const RackPackageStore&)=delete;
    RackLibraryStatus install(const juce::File& archive, const RackPackageRef& expected,
                              RackPackageActivation, const RackLibraryCancel&);
    RackLibraryStatus remove(const RackPackageRef&, const RackLibraryCancel&);
    RackPackageLeasePtr open(const RackPackageRef&, RackLibraryStatus* = nullptr, const RackLibraryCancel& = {}) const;
    std::vector<RackPackageRef> active(RackLibraryStatus* = nullptr, const RackLibraryCancel& = {}) const;
    std::vector<RackPackageRef> committed(RackLibraryStatus* = nullptr, const RackLibraryCancel& = {}) const;
    std::uint64_t generation(RackLibraryStatus* = nullptr, const RackLibraryCancel& = {}) const;
    std::uint64_t observedGeneration() const noexcept;
    RackLibraryPaths paths() const;
private:
    struct Impl;
    explicit RackPackageStore(std::shared_ptr<Impl>);
    std::shared_ptr<Impl> impl;
    friend class RackInventorySnapshot;
};
}

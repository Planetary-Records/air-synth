#pragma once
#include "RackDesign.h"
#include <span>
namespace airack {
struct SoundDocument;
// Bounded native control-thread operations; failure preserves output/file.
bool saveRackDesign(const RackDesign&,const juce::File&);
bool loadRackDesign(const juce::File&,RackDesign&);
bool captureGenericRackDesign(const SoundDocument&,RackRole,std::span<const UnitBinding>,const RackPanel&,
    juce::Uuid,const juce::String&,const juce::String&,RackDesign&);

// Native control-thread inventory. These fences cover fresh owned requested
// payload, not process memory or the shared JUCE StringPool.
// Complete native library plus twelve embedded and thirty-two registered definitions.
inline constexpr std::size_t maximumRackCatalogEntries=72;
inline constexpr std::size_t maximumRackCatalogBytes=64*65536;
struct RackCatalogCaptureLimits {
    std::size_t maximumEntries=maximumRackCatalogEntries,maximumDefinitionBytes=maximumRackCatalogBytes,maximumDescriptorBytes=maximumRackCatalogBytes;
    std::size_t maximumSources=maximumRackCatalogEntries,maximumSourceBytes=maximumRackCatalogBytes,maximumPreparedBytes=41943040;
};
enum class RackCatalogStatus : std::uint8_t {
    ready,unavailable,invalidSource,identityConflict,countLimit,byteLimit,preparedLimit,
    overflow,invalidQuery,invalidCursor,singleEntryTooLarge,allocationFailure
};
struct RackCatalogEntry { RackDesign definition;juce::String contentSha256;bool operator==(const RackCatalogEntry&) const=default; };
struct RackCatalogQuery { juce::String text;std::optional<RackRole> role; };
struct RackCatalogPage { juce::Uuid snapshot;std::vector<RackCatalogEntry> entries;std::size_t next=0;bool complete=false; };
struct RackCatalogSnapshot {
    juce::Uuid id;std::vector<RackCatalogEntry> entries;
private:
    std::vector<std::uint8_t> native;
    juce::String authority;
    friend bool captureRackCatalog(std::span<const RackDesign>,std::span<const RackDesign>,const RackCatalogCaptureLimits&,RackCatalogSnapshot&,RackCatalogStatus*);
    friend bool rackCatalogNativeEntry(const RackCatalogSnapshot&,std::size_t);
    friend bool catalogPage(const RackCatalogSnapshot&,const RackCatalogQuery&,std::size_t,std::size_t,RackCatalogPage&,RackCatalogStatus*);
    friend bool resolveCatalogDesign(const RackCatalogSnapshot&,juce::Uuid,std::uint64_t,const juce::String&,RackDesign&,RackCatalogStatus*);
};
bool captureRackCatalog(std::span<const RackDesign> factory,std::span<const RackDesign> embedded,
    const RackCatalogCaptureLimits&,RackCatalogSnapshot&,RackCatalogStatus* =nullptr);
bool catalogPage(const RackCatalogSnapshot&,const RackCatalogQuery&,std::size_t cursor,std::size_t byteBudget,RackCatalogPage&,RackCatalogStatus* =nullptr);
bool resolveCatalogDesign(const RackCatalogSnapshot&,juce::Uuid,std::uint64_t,const juce::String&,RackDesign&,RackCatalogStatus* =nullptr);
// True only for an untampered, privately captured factory/embedded identity.
bool rackCatalogNativeEntry(const RackCatalogSnapshot&,std::size_t);
juce::var rackCatalogDescriptor(const RackCatalogEntry&);
juce::var rackCatalogPageDescriptor(const RackCatalogPage&);
bool rackCatalogEscapedSize(const juce::var&,std::size_t ceiling,std::size_t&);
}

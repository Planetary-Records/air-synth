#pragma once
#include "DrumDocument.h"
#include <atomic>

namespace airack { class InstrumentProcessor; struct InstrumentDrumTestAccess; }

namespace airack::drums
{
inline constexpr std::size_t publicationCapacity = 65;
inline constexpr std::size_t referenceCapacity = 192;
inline constexpr std::uint64_t intendedDecodedLimit = 256ull * 1024 * 1024;
inline constexpr std::uint64_t retainedDecodedLimit = 512ull * 1024 * 1024;
using BorrowedSamples = std::array<std::array<const SampleAsset*,16>,4>;
using TargetTokens = std::array<std::uint64_t,64>;
struct BorrowedPublication final
{
    std::uint64_t generation = 0;
    BorrowedSamples samples {};
};
struct PublicationIdentity final
{
    std::uint64_t generation = 0;
    TargetTokens targets {};
    bool operator==(const PublicationIdentity&) const = default;
};
struct ReferenceReport final
{
    std::uint64_t lifecycle = 0, adoptedGeneration = 0;
    std::array<const SampleAsset*,referenceCapacity> references {};
    bool complete = false;
    bool operator==(const ReferenceReport&) const = default;
};
// All owners and accounting are control-only; audio sees only the fixed values above.
class DrumOwnership final
{
public:
    struct McpImage final {
        DrumDocumentPtr document;
        std::array<AssetPtr,64> owners;
        std::uint64_t generation=0;
    };
    DrumOwnership();
    const BorrowedPublication* published() const noexcept { return published_.load(); }
    bool enqueue(const ReferenceReport&) noexcept;
private:
    friend class airack::InstrumentProcessor;
    friend struct airack::InstrumentDrumTestAccess;
    using Owners = std::array<AssetPtr,64>;
    struct PublicationRecord final
    {
        BorrowedPublication borrowed;
        DrumDocumentPtr document;
        Owners owners;
        PublicationRecord(std::uint64_t,DrumDocumentPtr,const Owners&);
        PublicationRecord(const PublicationRecord&)=delete;
        PublicationRecord& operator=(const PublicationRecord&)=delete;
    };
    struct Preparation final
    {
        std::uint64_t ticket=0,decoded=0,workspace=0;
        enum class Phase : std::uint8_t {free,rawInput,preparing,prepared,draining};
        std::uint64_t inputId=0,sourceWorkspace=0,operationWorkspace=0;
        std::uint64_t encodedReservation=0,batchWorkspace=0;
        std::uint32_t frames=0,channels=0;
        Phase phase=Phase::free;
        bool running=false,cancelled=false;
        AssetPtr asset;
    };
    static constexpr std::size_t ledgerCapacity=12675+3*64;
    std::array<std::unique_ptr<PublicationRecord>,publicationCapacity> publications_ {};
    PublicationRecord* current_=nullptr;
    std::atomic<const BorrowedPublication*> published_ {nullptr};
    std::array<AssetPtr,ledgerCapacity> ledger_ {};
    std::array<Preparation,3> preparations_ {};
    struct AiImage final
    {
        Owners owners {};
        DrumDocumentPtr document;
    };
    std::array<std::unique_ptr<AiImage>,3> aiImages_ {};
    std::shared_ptr<const McpImage> mcpImage_;
    std::shared_ptr<const McpImage> captureMcpImage() noexcept;
    Owners saveOwners_ {},restoreOwners_ {};
    DrumDocumentPtr saveDocument_;
    bool saving_=false,restoring_=false,borrowing_=false;
    std::uint64_t reservedDecoded_=0,reservedWorkspace_=0,nextTicket_=0,lifecycle_=1;
    std::uint64_t reservedEncoded_=0;
    bool inputPrototypeCharged_=false;
    ReferenceReport latest_ {};
    std::array<ReferenceReport,2> reports_ {};
    std::atomic<unsigned> reportRead_ {0},reportWrite_ {0};
    std::array<const SampleAsset*,8192> historyRoots_ {};
    std::size_t historyRootCount_=0;
    std::array<std::array<std::uint8_t,16>,516> protectedIds_ {};
    std::size_t protectedIdCount_=0;
    std::uint64_t retainedBytes() const noexcept;
    bool reserveDecoded(std::uint64_t) noexcept;
    void releaseDecoded(std::uint64_t) noexcept;
    bool registerAsset(AssetPtr&) noexcept;
    enum class AiImageStatus : std::uint8_t {ready,invalid,budget,allocation};
    AiImageStatus installAiImage(std::size_t,DrumDocumentPtr,const Owners&) noexcept;
    void clearAiImage(std::size_t) noexcept;
    bool aiImagesBusy() const noexcept;
    std::size_t freePublicationSlot() const noexcept;
    bool intendedFits(const Owners&) const noexcept;
    void install(std::size_t,std::unique_ptr<PublicationRecord>) noexcept;
    void drain() noexcept;
    void collect() noexcept;
    bool rooted(const SampleAsset*) const noexcept;
    void quiescent() noexcept;
};
static_assert(std::atomic<const BorrowedPublication*>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
}

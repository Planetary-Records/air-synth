#pragma once
#include "ModalLiveTail.h"
#include "ModalConverter.h"
namespace airack::space::runtime {
using ModalSlotSettings=std::array<Settings,8>;
using ModalSlotIdentities=std::array<std::uint64_t,8>;
struct ModalReadySlot {Settings settings;std::uint64_t identity=0;PreparedModalRoom room;PreparedModalWet wet;PreparedModalFinal final;PreparedModalTail roomTail,postTail;};
class ModalReadyBundle {
public:
    ModalReadyBundle(const ModalReadyBundle&)=delete;ModalReadyBundle& operator=(const ModalReadyBundle&)=delete;
    std::uint64_t key() const noexcept {return key_;}
    double rate() const noexcept {return context_.rate();}
    std::size_t modes() const noexcept {return context_.modes();}
    unsigned factor() const noexcept {return factor_;}
    const ModalReadySlot& slot(std::size_t i) const noexcept {return slots_[i];} // i<8, immutable borrow.
    std::size_t storageBytes() const noexcept {return sizeof(*this);}
private:
    ModalReadyBundle()=default;std::uint64_t key_=0;unsigned factor_=1;ModalTailContext context_;std::array<ModalReadySlot,8> slots_;
    friend std::unique_ptr<const ModalReadyBundle> prepareModalBundle(double,std::size_t,std::uint64_t,const ModalSlotSettings&,const ModalSlotIdentities&,detail::PreparationControl,unsigned) noexcept;
    friend class ModalService;
};
std::unique_ptr<const ModalReadyBundle> prepareModalBundle(double rate,std::size_t modes,std::uint64_t key,const ModalSlotSettings&,const ModalSlotIdentities&,detail::PreparationControl={},unsigned factor=1) noexcept;
enum class ModalPreparationDomain:std::uint64_t {room,post,duck};
struct ModalPreparationTag {std::size_t slot=0;ModalPreparationDomain domain=ModalPreparationDomain::room;std::uint64_t epoch=0,generation=0;};
struct ModalServiceOptions {
    bool threaded=true;
    // Off-audio checkpoint instrumentation; null in the product. May block only the worker.
    void (*checkpoint)(void*,ModalPreparationTag,detail::PreparationControl) noexcept=nullptr;void* checkpointContext=nullptr;unsigned factor=1;
    bool preparedCopies=true; // Internal diagnostic/reference selection; no host parameter.
    bool blockedScatter=true; // Internal exact reference/candidate selection.
    bool pairedReduction=true; // Internal exact reference/candidate selection.
    ModalConverterBackend converterBackend=ModalConverterBackend::automatic;
};
struct ModalSlotStatus {
    std::uint64_t identity=0,epoch=0,requestedRoom=0,requestedPost=0,requestedDuck=0,appliedRoom=0,appliedPost=0,appliedDuck=0;
    std::uint64_t preparationFailures=0,tailFrames=0,peakExponent=0;
    bool operator==(const ModalSlotStatus&) const=default;
};
struct ModalServiceStatus {
    std::uint64_t documentKey=0,completedParents=0,ticketSlot=8,ticketDomain=0,admissionsThisParent=0,maximumRoomsThisParent=0;
    std::array<ModalSlotStatus,8> slots;
    bool operator==(const ModalServiceStatus&) const=default;
};
class ModalService {
public:
    using Frame=ModalRoom::Frame;
    ModalService();~ModalService();ModalService(const ModalService&)=delete;ModalService& operator=(const ModalService&)=delete;
    bool prepareStorage(double rate,std::size_t modes,ModalServiceOptions={});
    bool installBundle(const ModalReadyBundle&,std::uint64_t expectedDocumentKey,unsigned installMask=255) noexcept;
    // Lifecycle owner only, with a closed inactive audio acknowledgement.
    bool startWorkers() noexcept;bool stopWorkers() noexcept;bool workersRunning() const noexcept;
    bool makeInactive() noexcept;unsigned admissionsThisParent() const noexcept;
    bool beginParent(const ModalSlotSettings&,unsigned activeMask,const ModalSlotIdentities&,std::size_t frameCount,bool allowNewAdmission=true) noexcept;
    bool renderSlot(std::size_t,std::span<Frame>) noexcept;
    bool endParent() noexcept;bool resetSlot(std::size_t) noexcept;
    bool pumpHeavy() noexcept;bool pumpLight() noexcept; // Manual mode only, off audio.
    bool snapshot(ModalServiceStatus&) const noexcept; // Published at install/endParent; bounded atomic read.
    bool composeTail(std::span<const std::size_t> slotIndices,unsigned sourceMultiplicity,ModalTailReport&) const noexcept;
    std::size_t storageBytes() const noexcept;
    std::size_t modeCount() const noexcept; // Owner-side actual active-room inspection.
    bool usesPreparedCopies() const noexcept;
    bool usesBlockedScatter() const noexcept;
    bool usesPairedReduction() const noexcept;
    std::optional<ModalConverterBackend> converterBackend() const noexcept; // Owner-side actual entry-point inspection.
private:struct State;std::unique_ptr<State> state_;
};
}

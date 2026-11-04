#pragma once
#include "RackDesign.h"
#include <span>
namespace airack {
struct SoundDocument;
struct RackCatalogSnapshot;
class RackInventorySnapshot;
enum class SoundEditOrigin { manual, aiCandidate };
struct RackDestination { RackRole role=RackRole::synth;std::uint8_t chain=4,position=0;bool operator==(const RackDestination&) const=default; };
struct RackControlResolution { juce::Uuid instance;std::uint8_t control=0;std::optional<FocusedControl> replacement; };
struct RackReconcileChoices { std::vector<juce::Uuid> detach,remove,replace;std::vector<RackControlResolution> controls; };
enum class RackCandidateStatus : std::uint8_t { ready,invalid,capacityReached,replacementRequired,reconciliationRequired,lockedDependency,stale };
// A face edit gives the selected rack its design's panel and name, with the same ID, role and units, and forks it.
enum class RackOperationKind : std::uint8_t { place,adopt,swap,duplicate,detach,move,remove,face };
struct RackOperation {
    RackOperationKind kind=RackOperationKind::place;juce::Uuid selected,newInstance;
    std::optional<RackDesign> design;RackDestination destination;std::vector<UnitBinding> allocation;RackReconcileChoices choices;bool allowSlotReuse=false;
    std::optional<RackPackageRef> package;
};
struct RackDifference { juce::String sonic,presentation,structural,automationWarning; };
enum class SourceAssignmentEffectKind : std::uint8_t { preserve,clear,copy };
struct SourceAssignmentEffect final {
    SourceAssignmentEffectKind kind=SourceAssignmentEffectKind::preserve;
    std::uint8_t bank=0,copyFrom=noSlot;
    std::uint16_t operation=0;
    SlotUse beforeUse=SlotUse::neverUsed,afterUse=SlotUse::neverUsed;
    juce::Uuid beforeOwner=juce::Uuid::null(),afterOwner=juce::Uuid::null();
    std::uint8_t beforeUnit=0,afterUnit=0;
    bool invalidateTarget=false,fullNumericBank=false,freshLogicalOwner=false;
    bool operator==(const SourceAssignmentEffect&) const=default;
};
struct SourceAssignmentTrace final {
    // At most64 low-level edits and12 catalog choices; two effects per each of4 source banks.
    std::array<SourceAssignmentEffect,2*4*(64+12)> effects {};
    std::uint16_t count=0;
    bool operator==(const SourceAssignmentTrace&) const=default;
};
bool validateRackInstances(const SoundDocument&);
bool rackControlActive(const SoundDocument&,const RackInstance&,std::size_t);
RackCandidateStatus reconcileRackEdit(const SoundDocument&,const SoundDocument&,const RackReconcileChoices&,SoundEditOrigin,SoundDocument&,RackDifference&);
RackCandidateStatus makeRackCandidate(const SoundDocument&,const RackOperation&,SoundEditOrigin,SoundDocument&,RackDifference&,SourceAssignmentTrace* trace=nullptr);
RackCandidateStatus makeCatalogRackCandidate(const SoundDocument&,const RackCatalogSnapshot&,juce::Uuid,std::uint64_t,const juce::String&,
    const RackOperation&,SoundEditOrigin,SoundDocument&,RackDifference&,SourceAssignmentTrace* trace=nullptr);
RackCandidateStatus makeCatalogRackCandidate(const SoundDocument&,const RackInventorySnapshot&,juce::Uuid,std::uint64_t,const juce::String&,
    const RackOperation&,SoundEditOrigin,SoundDocument&,RackDifference&,SourceAssignmentTrace* trace=nullptr);
}

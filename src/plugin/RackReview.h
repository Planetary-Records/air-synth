#pragma once
#include <juce_core/juce_core.h>
#include "RackReconciliation.h"
#include "SoundDocument.h"
#include "RackLibraryClient.h"
#include <optional>
#include <vector>
namespace airack {
class InstrumentProcessor;
// Where a placed rack sits: its synth bank, synth order and owned inserts, or its FX chain.
juce::String rackDestinationText(const SoundDocument&,const RackInstance&);
// A review's state: "Ready" to "Stale selection".
juce::String rackStatusText(RackCandidateStatus);
// The rack's three header lines: name, Edited and revision; destination and banks with their bypass; AI lock and intent.
juce::String rackHeaderText(const SoundDocument&,const RackInstance&);
// None of the rack's banks is disabled or bypassed.
bool rackOn(const SoundDocument&,const RackInstance&);
// A bank of the rack carries an AI lock (synthLocks or effectLocks).
bool rackLocked(const SoundDocument&,const RackInstance&);
// Exact allocation: local unit:physical bank pairs, comma separated, banks from 1; blank is automatic.
bool parseRackAllocation(const juce::String&,std::vector<UnitBinding>&);
// What the rack panel's Review reads from its fields.
struct RackReviewRequest{
 // The Operation item: 1 Create example, 4 Swap selected, 5 Duplicate selected, 6 Detach selected, 7 Remove selected,
 // 8 Move selected.
 int operation=1;
 // The design in libraryRackDesigns(): 0 to 2 the examples, then the factory designs; clamped to the list.
 int example=0;
 // A manual-only local recipe, absent for the shared example/factory catalogue.
 std::optional<RackDesign> localDesign;
 std::optional<RackPackageRef> package;
 juce::String allocation;
 bool reuse=false;
 // The destination chain and the position in it from 0, as the Destination and Position items give them.
 std::uint8_t chain=masterChain,position=1;
 juce::Uuid selected=juce::Uuid::null();
 bool moveDependencies=false;
 // The focused control to resolve (-1 for none), the Resolution item (1 Keep mappings, 2 Remove control,
 // 3 Remap control, 4 Detach ownership) and the remap key.
 int control=-1,resolution=1;
 juce::String remap;
};
// A Review: the state it reached, empty where it stopped before the preview; the difference; and, only when Ready,
// the draft with the edit revision and reset token it was reviewed at. The role is the reviewed design's, once loaded.
struct RackReview{juce::String state,difference;std::vector<RackOperation> draft;std::uint64_t expected=0,reset=0;std::optional<RackRole> role;};
RackReview reviewRack(InstrumentProcessor&,const RackReviewRequest&);
// An Apply: whether it applied and its sentence, empty when the sound moved on since the Review; and the new rack
// after a placement or a duplicate.
struct RackApplied{bool applied=false;juce::String status;std::optional<juce::Uuid> placed;};
RackApplied applyRackReview(InstrumentProcessor&,const RackReview&);
RackApplied applyRackReview(InstrumentProcessor&,const RackReview&,const RackLibraryPlacementPreparationPtr&,
    const std::shared_ptr<const RackLibraryPlacementView>&);
}

#pragma once
#include <juce_core/juce_core.h>
#include "RackReconciliation.h"
#include "SoundDocument.h"
#include <optional>
#include <vector>
namespace airack {
// A line in the stack: a placed rack, or a bare bank no rack holds. `chain` is a synth line's bank, or an FX line's chain
// (masterChain for the shared master); `bank` is a rack's first bank, or the bare bank.
struct StackRack{juce::Uuid id;RackRole role=RackRole::synth;std::uint8_t chain=0,bank=0;bool bare=false;};
// A present bank no rack holds (the user-reach spec, decision 3).
struct BareBank{RackRole role=RackRole::synth;std::uint8_t bank=0;bool operator==(const BareBank&) const=default;};
// A bare bank's line ID: fixed bytes, then the role and the bank. Any other ID is no bare bank's.
juce::Uuid bareBankId(BareBank);
std::optional<BareBank> bareBankOf(juce::Uuid);
// "SYNTH 2 ANALOG", or "FX 3 DELAY".
juce::String bareBankName(const SoundDocument&,BareBank);
// "SYNTH 2", "SYNTH 2 INSERT" or "SHARED MASTER", as rackPlaceText words a rack's.
juce::String bareBankPlace(const SoundDocument&,BareBank);
// Top to bottom: each synth bank in synth order with its synth rack, or its bare line while no rack holds it; then its
// insert chain in the chain's order, an FX rack at its first bank and a bare line for each bank no rack holds; then the
// shared master's chain the same way. Racks the walk misses follow the synth part in instance order.
std::vector<StackRack> rackOrder(const SoundDocument&);
// UP or DOWN: today's move past the rack of the same role above or below it, synth racks in the synth order and FX
// racks in their chain; none when there is no such rack.
std::optional<RackOperation> rackStep(const SoundDocument&,juce::Uuid rack,bool up);
// "SYNTH 2", "SYNTH 2 INSERT" or "SHARED MASTER", then " | EDITED" while the rack is marked edited.
juce::String rackPlaceText(const SoundDocument&,const RackInstance&);
// "Moved Glass Current up", or "Glass Current not moved: Stale selection; sound preserved".
juce::String rackMoveSentence(const juce::String& name,bool up,RackCandidateStatus);
// Where a placement or a drop sits in rackOrder's list, bare lines counted: before the line at `before` (the list's size
// for the end), and whether under the divider.
struct StackSpot{std::size_t before=0;bool master=false;bool operator==(const StackSpot&) const=default;};
// Where a destination draws: a synth order position before the first group at or after it that shows something; an
// insert chain's position in its bank's group, after its synth rack; the shared master's under the divider. None for a
// bank the sound does not have.
std::optional<StackSpot> rackSpot(const SoundDocument&,RackDestination);
// The lines that lift with a rack: the rack, then for a synth rack the FX racks and bare banks on its insert chain.
std::vector<juce::Uuid> rackGroup(const SoundDocument&,juce::Uuid rack);
// A line a lifted rack can land on, in the order without the lifted racks, with today's move destination there; the
// line where they were has none.
struct StackDrop{StackSpot spot;std::optional<RackDestination> destination;};
std::vector<StackDrop> rackDrops(const SoundDocument&,juce::Uuid rack);
// "SHARED MASTER POSITION 2", "SYNTH 1 INSERT POSITION 1" or "SYNTH ORDER POSITION 3".
juce::String rackDestinationText(RackDestination);
// "Moved Ember Reed to synth order position 2", "Moved Thread Press to Shared Master, position 1" or "Moved Thread Press
// to Synth 1 insert, position 2"; a refusal as rackMoveSentence's.
juce::String rackDropSentence(const juce::String& name,RackDestination,RackCandidateStatus);
// A design bound for show only: its synth units to synth banks 0, 1 and on, its effect units to effect banks 0, 1 and
// on, in unit order, so each control resolves its parameter's range, format and choices.
RackInstance rackPreviewInstance(const RackDesign&);
// Normalized, one per panel control: the unit's recipe value for the control's key, else the parameter's default; 0
// where the control resolves no parameter.
std::vector<float> rackPreviewValues(const RackDesign&);
// Normalized, every parameter, for a faceplate's preview: the preview's banks at each unit's recipe values, the rest at
// their defaults.
ParameterValues rackPreviewParameters(const RackDesign&);
// Normalized, one per panel control: the sound's value; 0 where the control resolves no parameter.
std::vector<float> rackInstanceValues(const SoundDocument&,const RackInstance&);
// What the stack shows that is not placed: its destination, its letters, and the rack and values it previews, if any.
struct StackPlacement{
 RackDestination destination;juce::String letters;
 std::optional<RackInstance> preview;std::vector<float> values;
 bool operator==(const StackPlacement&) const=default;
};
}

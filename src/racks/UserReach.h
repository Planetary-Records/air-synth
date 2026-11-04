#pragma once

#include "ParameterRegistry.h"
#include "RackDesign.h"
#include "SoundDocument.h"
#include <bitset>
#include <span>
#include <vector>

namespace airack
{
// The registry indices the house unit binds, ascending: the master, the macros, the voice controls, each synth's
// level, pan, mute and solo, and the output's gain and protection.
std::span<const std::size_t> houseParameters();
// The rack unit that owns a bank, else null.
const RackUnit* bankUnit(const SoundDocument&, RackRole, std::uint8_t bank);
// The module a bank holds: its owning rack unit's, else the sound's engine or effect kind.
NativeModule bankModule(const SoundDocument&, RackRole, std::uint8_t bank);
// A unit's controls on its bank, ascending: its module's active parameters there, except both source selectors.
// A revision-1 wavetable unit leaves out the warmth keys, which its kernel ignores.
std::vector<std::size_t> unitReach(NativeModule, std::uint8_t bank, AlgorithmRevision = modernSynthAlgorithmRevision);
// What the stack puts in the user's reach: the house unit and the unit of every present bank.
std::bitset<parameterCount> userReach(const SoundDocument&);
}

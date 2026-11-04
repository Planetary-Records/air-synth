#pragma once
#include "RackDesign.h"
#include "SoundDocument.h"
#include <array>
namespace airack {
std::vector<RackDesign> factoryRackDesigns();
// AIR's seventeen built-in recipes. Maker content is installed through the inventory.
std::vector<RackDesign> libraryRackDesigns();
// Conservative artwork identity for an exact frozen legacy recipe; no installed/latest lookup.
std::optional<RackPackageRef> legacyRackPackageRef(const RackDesign&);
// A new instance's sound: no synth bank, no effect and no rack, with the new instance's kernels. It is silent until a
// rack is placed.
SoundDocument newInstanceSound();
// Copper Arc alone on synth 1, with the new instance's kernels and a fixed instance ID.
SoundDocument copperArcSound();
}

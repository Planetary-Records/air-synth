#pragma once

#include "InstrumentProcessor.h"
#include "ProjectDocument.h"
#include "RackExamples.h"

// A new instance's sound before decision 10 placed Copper Arc on synth 1: SoundDocument's own default with the new
// instance's kernels, recalled through the host's state path.
inline bool blankInstance(airack::InstrumentProcessor& p)
{
    airack::ProjectDocument blank;
    blank.sound.compatibility = airack::newInstanceKernelCompatibility();
    juce::MemoryBlock state;
    return airack::encodeProjectState(blank, state) && p.restoreProjectState(state.getData(), int(state.getSize()));
}

// A new instance's sound before the studio feedback spec's decision 4 emptied it: Copper Arc alone on synth 1, recalled
// through the host's state path.
inline bool copperArcInstance(airack::InstrumentProcessor& p)
{
    airack::ProjectDocument copper;
    copper.sound = airack::copperArcSound();
    juce::MemoryBlock state;
    return airack::encodeProjectState(copper, state) && p.restoreProjectState(state.getData(), int(state.getSize()));
}

#pragma once

#include <juce_core/juce_core.h>

namespace airack::studio
{
// For 400 ms after the rack area last scrolled by the wheel, a control under the pointer passes the wheel to its parent,
// so the rack keeps scrolling. Message thread only.
// shortcut: one latch per process; a rack scroll in one open editor also holds another's controls for 400 ms.
inline double& rackScrollTime() { static double at = -1.0e9; return at; }
inline void noteRackScroll() { rackScrollTime() = juce::Time::getMillisecondCounterHiRes(); }
inline bool rackScrolling() { return juce::Time::getMillisecondCounterHiRes() - rackScrollTime() < 400.0; }
}

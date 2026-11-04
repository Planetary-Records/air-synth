#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

// Button::triggerClick only posts the click, and a fixed dispatch can end before it runs: JUCE's loop
// handles one window message per pass, so a slower one queued ahead of the click's outlasts the window.
// Clicks and returns once the click has run, false if that takes longer than timeoutMs.
bool clickPosted(juce::Button& button,int timeoutMs=5000);
// Whether click(button) returns with the button's click run while a 150 ms window message is queued ahead of it.
bool clickRunsBehindSlowMessage(const std::function<void(juce::Button&)>& click);

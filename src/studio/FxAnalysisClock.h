#pragma once
#include "FxAnalysisHub.h"
#include <juce_events/juce_events.h>
#include <functional>

namespace airack::studio
{
// The editor owns exactly one clock, separately from view repaint timers.
// Retirement is explicit because queued views may retain the hub afterwards.
class FxAnalysisClock final : private juce::Timer
{
public:
    explicit FxAnalysisClock(std::shared_ptr<FxMonitor> monitor)
        :hub(std::make_shared<FxAnalysisHub>(std::move(monitor))){startTimerHz(30);}
    ~FxAnalysisClock() override{retire();}
    void retire(){stopTimer();onUpdated=nullptr;hub->retire();}
    const std::shared_ptr<FxAnalysisHub> hub;
    std::function<void()> onUpdated;
private:
    void timerCallback() override{hub->update();if(onUpdated)onUpdated();}
};
}

#pragma once
#include "ParameterListenerRetirement.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>

namespace airack::detail {
// Queued UI must leave before the skin and other DeletedAtShutdown globals.
class UiRetirement final:private juce::MessageManager::LifetimeListener {
public:
 explicit UiRetirement(std::function<void()> owner):held(std::move(owner)){juce::MessageManager::addLifetimeListener(*this);}
 ~UiRetirement() override{juce::MessageManager::removeLifetimeListener(*this);}
private:
 void messageManagerStarting() override{}
 void messageManagerStopping() override{held={};}
 std::function<void()> held;
};
inline void retireUi(const InstrumentProcessor& processor,std::function<void()> owner){
 auto retained=std::make_shared<UiRetirement>(std::move(owner));
 ParameterListenerDispatch::retainUi(processor,retained);
 juce::MessageManager::callAsync([retained]{});
}
}

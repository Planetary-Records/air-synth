#pragma once

#include "ParameterListenerRetirement.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace airack::detail
{
// Retain the actual UI through conversion, synchronous callbacks and the native drag destructor.
class SliderAccessibility final : public juce::AccessibilityHandler
{
public:
    using Frame = std::function<std::shared_ptr<void>()>;

    SliderAccessibility (juce::Slider& sliderIn, InstrumentProcessor& processor, Frame frame)
        : AccessibilityHandler (sliderIn, juce::AccessibilityRole::slider, {},
                                Interfaces { std::make_unique<Value> (sliderIn, processor, std::move (frame)) }),
          slider (sliderIn)
    {
    }

    juce::String getHelp() const override { return slider.getTooltip(); }

private:
    class Value final : public juce::AccessibilityValueInterface
    {
    public:
        Value (juce::Slider& sliderIn, InstrumentProcessor& processorIn, Frame frameIn)
            : slider (sliderIn), processor (processorIn), frame (std::move (frameIn)), useMaxValue (slider.isTwoValue()) {}

        bool isReadOnly() const override { return false; }
        double getCurrentValue() const override { return useMaxValue ? slider.getMaximum() : slider.getValue(); }
        juce::String getCurrentValueAsString() const override { return slider.getTextFromValue (getCurrentValue()); }

        void setValue (double newValue) override
        {
            const auto owner = frame ? frame() : nullptr;
            const ParameterListenerDispatch dispatch (processor);
            write (newValue);
        }

        void setValueAsString (const juce::String& newValue) override
        {
            const auto owner = frame ? frame() : nullptr;
            const ParameterListenerDispatch dispatch (processor);
            write (slider.getValueFromText (newValue));
        }

        AccessibleValueRange getRange() const override
        {
            return { { slider.getMinimum(), slider.getMaximum() },
                     ! juce::approximatelyEqual (slider.getInterval(), 0.0) ? slider.getInterval() : slider.getRange().getLength() * .01 };
        }

    private:
        void write (double newValue)
        {
            juce::Slider::ScopedDragNotification drag (slider);
            if (useMaxValue)
                slider.setMaxValue (newValue, juce::sendNotificationSync);
            else
                slider.setValue (newValue, juce::sendNotificationSync);
        }

        juce::Slider& slider;
        InstrumentProcessor& processor;
        Frame frame;
        const bool useMaxValue;
    };

    juce::Slider& slider;
};
} // namespace airack::detail

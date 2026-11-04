#pragma once

#include "PatternControls.h"
#include "ScreenControls.h"

namespace airack::studio
{
/** One view (the session bar, the folded pattern strip, the unfolded pattern unit, the AI unit or the LIBRARY): the
    plate with the unlit dots drawn back, and its controls. A control whose binding is a control today's editor shows once takes that control's
    component ID, and the pattern unit's knobs, note buttons and keyboard take the IDs the pattern checks address
    (pattern.note.pitch and the like); every other control takes its element ID. Each control carries its element ID
    in the property "element". The pattern unit's knobs come without bindings, which its owner gives them. */
class SkinView final : public juce::Component
{
public:
    SkinView (const Model& m, const String& viewName);

    void paint (juce::Graphics& g) override;

    /** A control by its element ID ("session.save"). */
    juce::Component* control (const String& elementId) const;

    template <typename T>
    T* get (const String& elementId) const { return dynamic_cast<T*> (control (elementId)); }

    const Model& model;
    const String name;
    const Lettering lettering, screenLettering;
    Context ctx;
    std::vector<std::unique_ptr<SkinButton>> buttons;
    std::vector<std::unique_ptr<Lever>> levers;
    std::unique_ptr<Disclosure> fold;
    std::unique_ptr<Display> display;
    std::unique_ptr<Overview> overview;
    std::unique_ptr<Indicator> error;
    // the unfolded pattern unit's
    std::unique_ptr<Display> noteDisplay;
    std::vector<std::unique_ptr<Knob>> knobs;
    std::unique_ptr<Roll> roll;
    std::unique_ptr<Keyboard> keyboard;
    std::unique_ptr<Indicator> playing;
    // the AI unit's and the LIBRARY's
    std::vector<std::unique_ptr<Target>> targets;
    std::unique_ptr<Readout> readout;
    std::unique_ptr<Screen> screen;
};
} // namespace airack::studio

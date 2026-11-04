#pragma once

#include "ProgrammerLook.h"

#include <vector>

// The rack's empty space under its units (spec decision 9), as round 3b's create-rack drawing shows it
// (docs/design/racks/2026-10-09-studio-rack/window/all-gear-create-rack.png).
namespace airack::studio
{
/** The rack's empty space: the house unit's fold rail continued as a column, the two front rails with their square
    holes on the rack's pitch, and the rack's dark interior between them, which is + ADD RACK. The hint stands beside the
    lettering on a strip and under it in a taller space; the pair is centred in the interior. */
class EmptyRackSpace final : public juce::Component
{
public:
    explicit EmptyRackSpace (const Model& model);

    /** The house unit's scale, which sizes the rails as its ears and fold rail, and how far this space's top lies under
        the rack area's top, in stage pixels, which keeps the holes on the rack's pitch. */
    void setRack (float scale, float depth);
    /** The hint's sentence; the lettering and the hint are laid out again around it. */
    void setHint (const String& text);
    /** Whether the hint stands under the lettering rather than beside it. */
    bool stacked() const;

    /** In this component's coordinates: the front rails' holes, + ADD RACK's ink and the slot's outline. */
    const std::vector<Rect>& holes() const { return holeRects; }
    Rect lettering() const { return letters; }
    Rect slot() const { return slotRect; }

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** + ADD RACK: the interior between the rails, lettered; ringed only while Tab gave it the focus. */
    class Add final : public KeyFocus<juce::Button>
    {
    public:
        explicit Add (EmptyRackSpace& s);
        void paintButton (juce::Graphics& g, bool over, bool down) override;

    private:
        EmptyRackSpace& space;
    };

    /** The hint's sentence, its capitals centred in its bounds; clicks pass to + ADD RACK under it. */
    class Hint final : public juce::Label
    {
    public:
        void paint (juce::Graphics& g) override;
        float cap = 0;
    };

    Add add;
    Hint hint;

private:
    void layout();
    /** The right rail's inner edge in design px: the panel's, or this space's right edge where it is narrower. */
    float rightRail() const;

    const Model& model;
    float scale = 1.0f, depth = 0.0f;
    std::vector<Rect> holeRects;
    Rect letters, slotRect;
    juce::Path shape;
};
} // namespace airack::studio

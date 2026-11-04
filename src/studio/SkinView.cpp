#include "SkinView.h"

namespace airack::studio
{
SkinView::SkinView (const Model& m, const String& viewName)
    : model (m), name (viewName), lettering (m.face), screenLettering (m.screenFace), ctx { m, lettering, screenLettering }
{
    static const juce::StringArray shownOnceToday { "preset.save", "preset.load", "preset.browse", "undo", "redo", "rack.toggle", "ai.toggle", "ai.preview" };
    static const std::map<String, String> patternToday { { "pattern.meter", "pattern.meter.numerator" }, { "pattern.pitch", "pattern.note.pitch" },
                                                         { "pattern.start", "pattern.note.start" }, { "pattern.length", "pattern.note.duration" },
                                                         { "pattern.velocity", "pattern.note.velocity" }, { "pattern.channel", "pattern.note.channel" },
                                                         { "pattern.add", "pattern.note.add" }, { "pattern.delete", "pattern.note.delete" },
                                                         { "pattern.keys", "pattern.keyboard" } };
    const auto& plate = model.plates.at (name);
    setSize (plate.getWidth(), plate.getHeight());
    setTitle (name == "session" ? "Session" : name == "ai" ? "AI" : name == "library" ? "Library" : name == "pattern" ? "Pattern unit" : "Pattern");
    const auto& order = model.focusOrder.at (name);
    for (const auto& e : model.elements)
    {
        if (e.view != name)
            continue;
        juce::Component* comp = nullptr;
        if (e.kind == "action" || e.kind == "toggle" || e.kind == "mode")
        {
            const auto lit = e.layer ("lit");
            const auto lensR = lit.isNotEmpty() ? model.asset (lit).lensR : 0.0f;
            comp = buttons.emplace_back (std::make_unique<SkinButton> (ctx, e, lensR > 0.0f && e.lensR > 0.0f ? e.lensR / lensR : 1.0f)).get();
        }
        else if (e.kind == "lever")
            comp = levers.emplace_back (std::make_unique<Lever> (ctx, e)).get();
        else if (e.kind == "disclosure")
            comp = (fold = std::make_unique<Disclosure> (ctx, e)).get();
        else if (e.kind == "display" && e.display == "pattern.note")
            comp = (noteDisplay = std::make_unique<Display> (ctx, e)).get();
        else if (e.kind == "display")
            comp = (display = std::make_unique<Display> (ctx, e)).get();
        else if (e.kind == "overview")
            comp = (overview = std::make_unique<Overview> (ctx, e)).get();
        else if (e.kind == "indicator" && e.control.isNotEmpty())
            comp = targets.emplace_back (std::make_unique<Target> (ctx, e)).get();
        else if (e.kind == "indicator" && e.status == "state.load.error")
            comp = (error = std::make_unique<Indicator> (ctx, e)).get();
        else if (e.kind == "indicator")
            comp = (playing = std::make_unique<Indicator> (ctx, e)).get();
        else if (e.kind == "readout")
            comp = (readout = std::make_unique<Readout> (ctx, e)).get();
        else if (e.kind == "screen")
            comp = (screen = std::make_unique<Screen> (ctx, e)).get();
        else if (e.kind == "knob")
            comp = knobs.emplace_back (std::make_unique<Knob> (ctx, e)).get();
        else if (e.kind == "roll")
            comp = (roll = std::make_unique<Roll> (ctx, e)).get();
        else if (e.kind == "keyboard")
            comp = (keyboard = std::make_unique<Keyboard> (ctx, e)).get();
        if (comp == nullptr)
        {
            jassertfalse; // an element kind none of the views has
            continue;
        }
        const auto today = patternToday.find (e.id);
        comp->setComponentID (today != patternToday.end() ? today->second : shownOnceToday.contains (e.control) ? e.control : e.id);
        if (auto* k = dynamic_cast<Knob*> (comp))
            k->entry.setComponentID (comp->getComponentID() + ".entry");
        comp->getProperties().set ("element", e.id);
        const auto idx = order.indexOf (e.localId());
        if (idx >= 0)
            comp->setExplicitFocusOrder (idx + 1);
        addAndMakeVisible (comp);
    }
    if (error != nullptr)
        error->toFront (false);
}

void SkinView::paint (juce::Graphics& g)
{
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageAt (model.plates.at (name), 0, 0);
}

juce::Component* SkinView::control (const String& elementId) const
{
    for (auto* c : getChildren())
        if (c->getProperties()["element"].toString() == elementId)
            return c;
    return nullptr;
}
} // namespace airack::studio

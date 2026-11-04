#pragma once

#include "HouseControls.h"
#include "SoundDocument.h"
#include "StudioSkin.h"

#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace airack::studio
{
/** Every placed rack's active continuous focused control, in rack and panel order. */
std::vector<house::RackTarget> rackTargets (const SoundDocument& d);
/** A parameter's name on a tape: a placed rack's label for it, else the PROGRAMMER's name or the registry's, its bank
    in front. */
String targetName (std::size_t index, const std::vector<house::RackTarget>& targets, const Model& model);

/** The AI Rack Synth unit over the processor: its rail, and the unit (3U), its strip (1U) or, in a narrow column, its
    narrow strip (1U). The fold is view state:
    it writes nothing, and focus follows it to the same control in the other view, or to the rail. Each control writes
    its registry parameter as today's controls do, refused while another gesture or an AI reservation holds the
    processor. The tapes and ASSIGN show the sound document as of the last refresh. */
class HouseUnit final : public juce::Component
{
public:
    static constexpr int railW = 44;

    HouseUnit (InstrumentProcessor& p, const Model& model, house::Host h);
    ~HouseUnit() override;

    void setFolded (bool f);
    bool isFolded() const { return folded; }
    /** While the pattern is unfolded the unit shows its strip, whatever the user's fold. */
    void setStripOnly (bool s);
    /** Whether a 1U view shows: the strip, or the narrow strip. */
    bool showsStrip() const { return stripOnly || (column > 0 ? ! open : folded); }
    /** The narrow-width rule: the column's width between the rails in design px while it is narrower than the panel,
        else 0. Narrow, the unit spans the column and shows its narrow strip, or its 3U unit while open, whatever its
        fold, which shows again when the column widens. */
    void setNarrow (int width);
    int narrowWidth() const { return column; }
    /** While narrow: the 3U unit, scrolled sideways from its left end, instead of the narrow strip. */
    void setOpen (bool o);
    bool isOpen() const { return open; }
    /** How far the open 3U unit is scrolled sideways, in design px, kept from 0 to across(). */
    void setShift (int px);
    int shift() const { return shiftPx; }
    /** How much of the open 3U unit lies beyond the column, else 0. */
    int across() const;
    /** Letters the tapes from the document: each slot tape names the synth rack in its bank, each macro tape its
        target and depth. An open ASSIGN editor follows the document unless its depth is being dragged. */
    void refresh (const SoundDocument& d);
    /** A tape's lettering by its local ID; empty shows the dash or bare panel. A depth drag's binding shows on its
        macro's tape. */
    String tapeText (const String& localId) const;

    void visibilityChanged() override;
    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

    /** Called when the rail is pressed, instead of folding the unit or, while narrow, opening it; the editor decides
        what the rail does. */
    std::function<void()> onRail;
    /** Called when the unit changes height. */
    std::function<void()> onLayout;
    /** A horizontal wheel, or Shift and the wheel, over the open 3U unit off its controls: how far to scroll it
        sideways, in design px. */
    std::function<void (int)> onAcross;
    /** Called after an ASSIGN editor opens. */
    std::function<void()> onOpened;

private:
    house::View& current();
    void switchView (house::View& from);
    void layout();
    void assign (int macro, const house::Assignment& a);

    const Model& model;
    house::Host host;
    house::Context context;
    std::unique_ptr<juce::Drawable> wordmark;
    juce::Image unitTitle, stripTitle;

public:
    house::View unit, strip, narrow;
    house::FoldRail rail;
    house::AssignEditor editor;

private:
    /** The document's macros, racks, layout and values as last refreshed. */
    SoundDocument shown;
    std::vector<house::RackTarget> targets;
    std::array<String, 4> slots;
    /** A depth drag's binding, shown on its macro's tape until the release. */
    std::optional<std::pair<int, house::Assignment>> preview;
    bool folded = false, stripOnly = false, refreshed = false, open = false;
    int column = 0, shiftPx = 0;
};

/** A wheel's delta as a distance in px, as juce::Viewport scrolls by it: 14 x 16 px a unit, at least 1. */
int wheelDistance (float delta);
} // namespace airack::studio

#pragma once

#include "ProgrammerUnit.h"
#include "RackDesign.h"
#include "DynamicEq.h"

#include <array>
#include <map>
#include <memory>
#include <vector>

// The all-controls panel (docs/superpowers/specs/2026-10-15-ai-uses-user-controls.md, decision 2): in a rack's drawer,
// every parameter the rack's units hold, on the PROGRAMMER's knobs.
namespace airack::studio
{
class SaturationGraph;
struct MakerSkin;
/** A rack's units laid out as the PROGRAMMER lays out their pages, a key for each view: a synth unit's COMMON (without
    ENGINE), its engine's page, LFO 1/2 and ROUTES' four pages; an effect unit's FX N, its SLOT and its kind's section.
    ROUTES pressed while it shows turns to its next page. The knobs keep the PROGRAMMER's rules, its route edit
    included, and each continuous knob's menu assigns it to a macro. A proposal's changes are marked: a shown knob's
    pointer dashed at the proposed turn, and the key of a view holding a change it does not show. The shown key, each
    ROUTES key's page and the last touched knob are view state. */
class AllControlsPanel final : public juce::Component
{
    // First, so that they outlive the knobs, which use them.
    InstrumentProcessor& processor;
    const Model& model;
    ProgrammerLook look;
    ProgrammerHost host;

public:
    struct Unit
    {
        NativeModule module = NativeModule::analog;
        std::uint8_t bank = 0;
        AlgorithmRevision revision = modernSynthAlgorithmRevision;
        bool operator== (const Unit&) const = default;
    };
    struct Mark
    {
        std::size_t index = 0;
        float proposed = 0;
        bool operator== (const Mark&) const = default;
    };
    /** The tallest page's height, a two-row page's. */
    static constexpr int panelHeight = 601;

    AllControlsPanel (InstrumentProcessor& p, const Model& m, ProgrammerHost h);
    ~AllControlsPanel() override;

    /** The units' keys and knobs, on the first key. The units shown already change nothing, so the knobs and their
        gestures survive. */
    void show (std::vector<Unit> units);
    std::vector<String> keys() const;
    /** A key's press: its view, or ROUTES' next page while it shows. */
    void showKey (int key);
    int shownKey() const { return key; }
    /** Reveal the existing numeric page holding a parameter. */
    bool showParameter(std::size_t);
    /** The shown page's height: 20 px under its last row's dividers. */
    int shownHeight() const;
    /** After the shown page's height changes. */
    std::function<void()> onHeight;
    /** The physical saturation service cover exposes the existing design/mapping form. View state only. */
    std::function<void(bool)> onPatchBay;
    void setPatchBayOpen(bool);
    bool focusPatchBay();
    bool patchBayVisible() const;
    std::function<void()> onPageChanged;
    std::function<void(std::size_t slot, std::size_t band)> onBandSelected;
    std::function<void(std::size_t slot, bool postEq, std::size_t band)> onSpaceBandSelected;
    /** The plate's face over this area, in the panel's own pixels, so that a form under the panel continues it; the
        panel's bounds while it is empty. */
    void setFace (Rect area);
    /** The shown key's current page (ROUTES or a Dynamic EQ band/output page). */
    int routesPage() const;
    bool detectorPage() const;
    bool spectralPage() const;
    void setDetectorReadbacks(std::uint8_t,const std::array<DynamicEqDetectorReadback,8>&,bool valid,bool externalAvailable);
    ProgrammerKnob* knob (std::size_t index) const;
    /** The shown view's knobs, row by row. */
    std::vector<ProgrammerKnob*> shownKnobs() const;
    /** Every knob's parameter, ascending. */
    std::vector<std::size_t> parameters() const;
    /** The live sound: the knobs' locks, the value line and the marks against it. */
    void refresh (const SoundDocument& d);
    void showProposal (std::shared_ptr<const SoundDocument> candidate);
    void retireGraph();
    /** Each knob not locked live whose candidate value differs from the live one, ascending, with its proposed physical
        value. */
    std::vector<Mark> marks() const;
    /** The keys holding a mark their view doesn't show, ascending. */
    std::vector<int> outlinedKeys() const;

    void paint (juce::Graphics& g) override;
    void paintOverChildren (juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

private:
    class View;
    class PatchBay;
    class EquipmentKey;
    struct Key
    {
        std::unique_ptr<juce::Button> button;
        std::vector<View*> pages;
        int page = 0;
        int eqSlot = -1, saturationSlot = -1;
        int spaceSlot = -1;
    };

    void addKey (const String& label, std::vector<View*> pages, const Unit&);
    View& addView (const std::vector<std::vector<const SectionSpec*>>& rows, const std::vector<std::size_t>& reach, bool compactEq = false, bool compactSaturation = false);
    void wire (ProgrammerKnob& k);
    const View* shownView() const;
    void showShown();
    void rereadRoutes();
    bool compactEq() const;
    bool serviceCover() const;
    bool saturationGlobal() const;
    void selectEqPage (int);

    std::shared_ptr<const MakerSkin> equipmentOwner;
    const house::Model* equipment = nullptr;
    std::vector<Unit> shownUnits;
    std::vector<std::unique_ptr<View>> views;
    std::vector<Key> keyList;
    std::array<ProgrammerKnob*, parameterCount> byIndex {};
    /** Each knob's key and page. */
    std::map<std::size_t, std::pair<int, int>> placeOf;
    juce::Label line;
    juce::ComboBox unitChoice, bandChoice;
    std::unique_ptr<juce::TextButton> previous, next;
    std::unique_ptr<PatchBay> patchBay;
    std::unique_ptr<SaturationGraph> saturationGraph;
    ProgrammerKnob* lastTouched = nullptr;
    SoundDocument shown;
    struct DetectorMeter {std::array<DynamicEqDetectorReadback,8> values{};bool valid=false,externalAvailable=false;};
    std::array<DetectorMeter,8> detectorMeters{};
    std::shared_ptr<const SoundDocument> proposal;
    int key = 0;
    int heightShown = panelHeight;
    Rect face;
};

/** A placed rack's units for the panel: each design unit in unit order, on its binding's bank. */
std::vector<AllControlsPanel::Unit> panelUnits (const RackInstance& rack);
} // namespace airack::studio

#pragma once

#include "HouseControls.h"
#include "ParameterGesture.h"
#include "ParameterRegistry.h"
#include "ProgrammerLook.h"
#include "SoundDocument.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

// The PROGRAMMER (window/skin/app/Main.cpp, stage C) over the processor: on ADVANCED, a knob for each registry
// parameter on its page, and RACK LAYOUT's bank controls.
namespace airack::studio
{
/** A PROGRAMMER knob for one registry parameter: JUCE's rotary slider over its physical value, drawn by
    ProgrammerLook, stepped over a choice's or a toggle's positions. Its edits keep the house unit's gesture rules
    (ParameterGesture). Its mouse and keys are the prototype's: a vertical drag of 200 px covers the range (Shift:
    1000 px), a stepped control moving by detents and editing only when its detent changes, and Escape cancels a drag
    back to its start inside its gesture; the wheel and the arrows step, by hundredths or a detent, Page Up and Down
    ten steps, and Home and End go to the ends; a double click or Enter opens the entry over its label for a value in
    the display's form; a right click opens its menu. Its label and entry are its parent's children, beside it. */
class ProgrammerKnob final : public KeyFocus<juce::Slider>
{
public:
    using Menu = std::vector<std::pair<String, std::function<void()>>>;

    ProgrammerKnob (InstrumentProcessor& p, const ProgrammerLook& l, house::Host& h, std::size_t parameter);
    ~ProgrammerKnob() override;
    /** End gestures and detach listeners while the processor still lives. */
    void retire();

    /** The body centred on `centre`, the label under it and the entry over the label, in the parent's coordinates. */
    void place (Pt centre);
    /** The EQ drawer keeps this control's bindings while presenting discrete fields directly. */
    enum class Presentation { rotary, toggle, choice, segments, horizontal };
    void setPresentation (Presentation p) { presentation = p; repaint(); }
    Presentation getPresentation() const { return presentation; }
    /** The saturation equipment face changes artwork only; parameter editing remains this control's. */
    void setEquipmentArtwork(const house::Model* m) { equipment=m;label.getProperties().set("programmer.equipment",m!=nullptr);repaint(); }
    void paint (juce::Graphics&) override;
    Pt centre() const { return middle; }
    /** Degrees from 12 o'clock, clockwise. */
    float angle() const;
    /** The angle at a physical value of its parameter. */
    float angleAt (double physical) const;
    /** Detents over the sweep; 0 for a continuous control. */
    int steps() const;
    /** The compact EQ's one choice controls its classic and appended parameters. */
    void useEqSelector (std::size_t extension);
    std::optional<std::size_t> pairedParameter() const { return eqExtension; }
    /** "CUTOFF 1.80 kHz": its name on the display and its value in the display's form. */
    String valueLine() const;
    void setLocked (bool on);
    bool isLocked() const { return locked; }
    /** The label's lettering, in the parent's coordinates. */
    Rect labelInk() const;
    /** Where the label draws the AI lock's dot, in the parent's coordinates. */
    Rect lockArea() const;
    /** Shows the parameter's value as the processor holds it. */
    void reread();

    /** Lock or Unlock, Default, then what addToMenu adds. */
    Menu menu();
    void showMenu();
    void openEntry();
    /** Closes the entry and enters what it holds. */
    void commitEntry();
    /** Closes the entry, keeping the value. */
    void closeEntry();
    /** A new position: a stepped control lands on the nearest detent and edits only when that detent changes. */
    void move (double proportion);
    void stepBy (int k);
    bool isDragging() const { return dragging; }
    /** Close numeric entry and end a drag before disabling; false if a host callback deleted this control. */
    bool finishDrag();

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void enablementChanged() override;
    bool hitTest (int x, int y) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const std::size_t index;
    juce::Label label;
    /** Its component ID is the knob's with ".entry". */
    juce::TextEditor entry;
    /** Run as a drag starts and after each edit made through the knob: the PROGRAMMER's last touched control. */
    std::function<void (ProgrammerKnob&)> onTouched;
    /** Adds a macro's items to the menu. */
    std::function<void (Menu&)> addToMenu;
    /** When set, takes each edit's physical value in place of the knob's gesture, and a drag holds no gesture: a
        route's on, source, target and bank go through the unit's route edit. */
    std::function<void (float)> onEdit;

private:
    String displayName() const;
    double proportion() const;
    void read (float physical);
    void touched();
    void showChoices();
    String formatted (float) const;
    std::optional<float> parsed (const String&, const String&) const;
    const std::vector<String>& choiceValues() const;
    float eqSelection() const;
    struct ChoiceContext { SoundDocument sound; std::uint64_t revision=0, reset=0; };
    std::optional<ChoiceContext> captureChoice() const;
    void writeEqChoice (int, const std::optional<ChoiceContext>&);

    InstrumentProcessor& processor;
    const ProgrammerLook& look;
    house::Host& host;
    ParameterGesture gesture;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    std::unique_ptr<juce::ParameterAttachment> extensionAttachment;
    std::optional<std::size_t> eqExtension;
    bool eqShape = false;
    Pt middle;
    /** The unit the display showed as the entry opened, which a bare number is read in. */
    String entryUnit;
    double dragFrom = 0.0, dragStart = 0.0;
    bool dragging = false, locked = false;
    Presentation presentation = Presentation::rotary;
    const house::Model* equipment = nullptr;
};

/** Dim unused retained EQ fields; added static shapes disable their unavailable fields. */
void showDynamicEqRelevance(ProgrammerKnob&, const ParameterValues&);
void showSaturationRelevance(ProgrammerKnob&, const SoundDocument&);
/** Disable inapplicable retained wet-EQ values; false if an end callback retired the control. */
bool showSpaceReverbRelevance(ProgrammerKnob&, const ParameterValues&);

/** What the PROGRAMMER asks of the editor: the house unit's host, and for the route edit today's snapshot, which says
    why it fails, and the editor's refresh after it. */
struct ProgrammerHost
{
    house::Host house;
    std::function<bool (SoundDocument&, std::uint64_t&)> snapshot;
    std::function<void()> refresh;
};

/** Today's route edit of a route's on, source, target or bank to a normalized value: the route's other fields kept
    consistent, one gesture over the fields it changes, "Route edit complete" or "Route edit failed; sound preserved",
    the host's refresh, then `reread`, which a rejected or unattempted edit also runs. */
void editRoute (InstrumentProcessor& p, const ProgrammerHost& host, std::size_t index, float value, const std::function<void()>& reread);
/** The page of FX N with a section titled the kind, else its first. */
int pageOfKind (const SubpageSpec& fx, EffectKind kind);

/** The PROGRAMMER over the processor, as ADVANCED shows it (Main.cpp's Programmer): the page buttons, the shown page's
    sub-page buttons and the display over the plate, then the page's knobs in rows of engraved sections, each knob a
    placed rack names with its name tape. The unit makes each registry knob once on its native page and shows one
    page. RACK LAYOUT lists the twelve banks; a row selects its bank, whose controls show under the synth banks. The
    page, each page's last sub-page, the routes page, the FX page, the selected bank and the last touched control are view state;
    the unit opens on GLOBAL with no value line, and RACK LAYOUT on SYNTH 1. Its edits keep today's rules: a knob's
    gestures, a route's on, source, target and bank through today's route edit, the rack edits, and the locks and
    macro assignments as the editor's sound edits. */
class ProgrammerUnit final : public juce::Component
{
    // First, so that they outlive the controls, which use them.
    InstrumentProcessor& processor;
    const Model& model;
    ProgrammerLook look;
    /** The tapes' lettering: the skin's tape style names the marker face, which these give it. */
    const Lettering marker;
    const Faces markerFaces;
    const TextStyle tapeStyle;
    ProgrammerHost host;
    Context ctx;
    std::deque<Element> pageElements;
    Element displayElement;

public:
    /** A section's title as shown: its section's left edge, and the grey it is lettered at. */
    struct ShownTitle { String text; Rect ink; float left = 0; int grey = 0; };
    /** A name tape as shown: its body on the plate, and the width of its name's ink. */
    struct ShownTape { std::size_t index = 0; String text; Rect body; float inkWidth = 0; };

    ProgrammerUnit (InstrumentProcessor& p, const Model& m, ProgrammerHost h);
    ~ProgrammerUnit() override;

    /** Shows the document as of the editor's refresh: the locks, the tapes, the bank line, the retained titles, the
        effect banks' status and RACK LAYOUT's rows. An open assignment editor follows it unless its depth is being
        dragged. As today's rows do, an effect bank's KIND and OWNER keep a pending choice until the bank changes, or
        until `recalled` says the sound was replaced. */
    void refresh (const SoundDocument& d, bool recalled = false);
    /** A page button: the page shows the sub-page it showed last, else its first. FX N opens on its bank's kind. */
    void showPage (const String& id);
    /** A sub-page button; ROUTES pressed while it shows turns to the next four routes, and FX N to its next page. */
    void showSubpage (const String& id);
    const String& page() const { return pageId; }
    const String& subpage() const { return subpageId; }
    int routesPage() const { return routes; }

    ProgrammerKnob* knob (std::size_t index) const;
    /** The last touched knob; nullptr until one is touched. */
    ProgrammerKnob* touched() const { return lastTouched; }
    /** The shown page's knobs, row by row. */
    std::vector<ProgrammerKnob*> shownKnobs() const;
    std::vector<ShownTitle> shownTitles() const;
    std::vector<ShownTape> shownTapes() const;
    /** RACK LAYOUT's rows: the four synth banks, then the eight effect banks. */
    std::vector<juce::Button*> rackRows() const;
    /** The bank line in full, before the display squeezes or elides it. */
    String bankLine() const;

    void paint (juce::Graphics& g) override;
    void visibilityChanged() override;

    std::vector<std::unique_ptr<SkinButton>> pageButtons;
    /** As many as the page with the most has, relabelled for the page shown; the rest hide. */
    std::vector<std::unique_ptr<KeyFocus<juce::ToggleButton>>> subpageButtons;
    std::unique_ptr<Display> display;
    house::AssignEditor assign;
    /** Today's effect status sentences, undrawn: the display's accessible value on FX N. */
    std::array<juce::Label, 8> effectStatus;

private:
    class Leaf;
    class BankRow;
    class BankControls;

    /** The shown page's leaf, buttons and display. */
    void show();
    /** The shown sub-page, FX N's at the FX page and ROUTES' at the routes page. */
    const SubpageSpec& shownSpec() const;
    /** Turns a shown FX N to the page holding its bank's kind. */
    void openEffect();
    void showDisplay();
    /** The layout as last refreshed: the effect banks' status, their retained titles and RACK LAYOUT's rows. */
    void showLayout();
    void letterTapes();
    String tapeText (std::size_t index) const;
    void route (std::size_t index, float normalized);
    /** Today's rack edit, with today's sentences. */
    void rackEdit (const RackEdit& edit);
    void macroMenu (int macro, ProgrammerKnob::Menu& m);
    void assignMacro (int macro, const house::Assignment& a);
    void editAssignment (int macro);
    Leaf* leafFor (const SubpageSpec& s) const;
    std::shared_ptr<void> frame() const;
    bool closing() const;

    std::vector<std::unique_ptr<Leaf>> leaves;
    Leaf* layoutLeaf = nullptr;
    std::array<ProgrammerKnob*, parameterCount> byIndex {};
    Leaf* shownLeaf = nullptr;
    ProgrammerKnob* lastTouched = nullptr;
    String pageId { "global" }, subpageId { "main" };
    int routes = 0, fxPage = 0;
    std::array<int,4> drumPages {};
    std::map<String, String> lastSubpage;
    /** The document as last refreshed. */
    SoundDocument shown;
    std::vector<house::RackTarget> targets;
    std::array<String, 4> synthOwners;
    std::array<String, 8> effectOwners;
    /** A depth drag's binding, shown on its macro's tape until the release. */
    std::optional<std::pair<int, house::Assignment>> preview;
    bool refreshed = false;
};
} // namespace airack::studio

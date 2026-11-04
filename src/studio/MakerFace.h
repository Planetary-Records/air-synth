#pragma once

#include "HouseControls.h"
#include "HouseSkin.h"
#include "ParameterRegistry.h"
#include "RackDesign.h"
#include "RackInventory.h"

namespace airack { struct DrumPadTarget; }

// Package-backed maker faceplates retain their exact prepared ownership in the house skin model.
namespace airack::studio
{
struct Asset;
class Lettering;
class TileArrayFace;

/** A VFD's text: its two lines, and how many of its bar's segments are lit where line 2 is the bar, else -1. */
struct VfdText
{
    juce::String line1, line2;
    int lit = -1;
    bool operator== (const VfdText&) const = default;
};

/** What a VFD reads for the touched control at the values (docs/superpowers/specs/2026-10-13-glass-current-faceplate.md,
    decision 4). Line 1 is the control's VFD name and its compact value; when longer than `cells1`, the name drops its
    first word. For a soft knob, line 2 is the other controls shown on its screen that are not choices, in slot order,
    each as its LCD label and value, as many as fit `cells2`. For a face knob, line 2 is the skin's VFD bar, lit for the
    first ceil(p n) of its n segments at the pointer's proportion p of its travel. */
VfdText vfdLines (const house::Model&, const ParameterValues&, const house::Element& touched, int cells1, int cells2);

/** A maker's faceplate: the design it faces, the maker and model it engraves, its height and its skin. */
struct MakerSkin
{
    /** A live native editor in the reveal's pixels; its rack unit resolves separately for each placement. */
    struct NativeEditor
    {
        juce::String type, theme;
        int rackUnit = -1;
        juce::Rectangle<float> rect;
    };
    juce::Uuid design;
    juce::String maker, model;
    int units = 2;
    std::shared_ptr<const house::Model> owner;
    const house::Model& skin;
    std::optional<NativeEditor> nativeEditor;
    RackPackageLeasePtr lease;
};
using MakerSkinPtr=std::shared_ptr<const MakerSkin>;
// Worker preparation; callers retain the result. UI lookup performs no I/O or parsing.
MakerSkinPtr prepareMakerSkin(const RackInstance&,const RackInventorySnapshot*,const RackLibraryCancel& ={});
MakerSkinPtr makerFaceFor(const RackInstance&,const RackInventorySnapshot&);

/** Pure prepared lookup for an exact package-backed definition, or the editable house fallback.
    Legacy/custom revisions cannot borrow another definition's artwork. */
MakerSkinPtr makerFaceFor (const RackDesign& design);

/** One placed rack's faceplate over the processor: the maker's plate with its controls bound to the rack's own banks,
    and what its skin's reveal opens: a recess in place over the plate (Thread Press's SET-UP), a drawer whose deck
    slides out under the face, its front following (Ember Reed's PROGRAMMER) or, without a front, pulled from the face
    and its pull following the deck in Tab order (Night Dial's deck), or a door whose panel goes over the face
    and whose hanging view below it (Glass Current's EDIT), the panel's page buttons choosing the screen of soft knobs
    it shows. Opening writes nothing; the rack's unit keeps the state, as it keeps its fold. It is rack.face, and its
    controls are their element IDs. */
class MakerFace final : public juce::Component, private juce::Timer
{
public:
    /** The faceplate for a rack whose design makerFaceFor gave `skin`, or nullptr when one of its controls has no
        parameter in the rack's banks. */
    static std::unique_ptr<MakerFace> forRack (InstrumentProcessor&, MakerSkinPtr, const RackInstance&, house::Host);
    ~MakerFace() override;
    /** An inert faceplate may outlive both its editor and processor in the message queue. */
    void retire();

    /** Shows or hides the revealed view; a drawer grows the faceplate by its deck, a door by its units. Closing with
        focus inside it hands focus to the opener. A door's opener leaves Tab order while open, and opening it from
        the opener hands the focus to the closer. Folded, it stays closed. While `moving`, with the skin's time and
        without reduced motion, it moves there over that time: the state is the new one at once and the drawn
        fraction follows, a press while moving turning it back from where it is drawn; else it jumps there. */
    void setOpen (bool, bool moving = true);
    /** Escape closes an open recess through the same focus path as its physical closer. */
    bool keyPressed (const juce::KeyPress&) override;
    bool isOpen() const { return open; }
    /** How far open the reveal is drawn, 0 closed to 1 open: eased along its time while it moves. */
    float opening() const;
    /** The faceplate's drawn height in units: the skin's, and a drawer's or a door's units at the drawn fraction. */
    float shownUnits() const;
    bool isMoving() const { return isTimerRunning(); }
    /** While a recess or a door moves, where its moving part is drawn in face pixels: the cover, grown as it lifts; the
        lid that slides, as far as it is still over the recess; the lid that turns up, foreshortened to its hinge; the
        closed door, foreshortened to its foot; or the hanging door, unfolding below its top. Empty while still or for a
        drawer. */
    juce::Rectangle<float> movingArea() const;
    /** A recess's fasteners' turn at the drawn fraction, 0 to 90 degrees, a quarter by the time the cover lifts; 0 for
        another reveal or a recess without fasteners. */
    float fastenerTurn() const;
    /** Folds to the skin's strip, closing the reveal first: only the strip shows, at the face's height over its units;
        unfolded, the closed face. Nothing for a skin without a strip. */
    void setFolded (bool);
    bool isFolded() const { return folded; }
    bool hasStrip() const { return stripView != nullptr; }
    /** The strip, folded or not; none without one. */
    house::View* strip() { return stripView.get(); }
    /** The strip at the given values, drawn as the live strip draws it; empty without one. */
    juce::Image composeStrip (const ParameterValues&) const;
    /** The skin's units, with the drawer's or the door's while it is open. */
    int units() const;
    /** While open, what opening shows below the face, as far as it is drawn: the door, or the drawer's deck and its
        front. */
    juce::Rectangle<int> openedArea() const;
    /** The door's chosen page and screen ("delay", "delay.1"), which close and open keep; empty without pages. */
    juce::String page() const;
    juce::String screen() const;
    /** What the LCD reads, slot by slot; empty without one. */
    juce::StringArray lcd() const;
    /** The faceplate at the given values, closed or open, drawn as the live controls draw it: each view's plate, each
        pointer, grip ring, cap and bat, the lamp, the needle and the displays. Open, a drawer's pull is lit where it
        has a lit layer, and a door shows its drawing's screen, its page
        lit, the spares on the screen's unused slots and its drawing's control touched. */
    juce::Image compose (const ParameterValues&, bool opened = false) const;
    /** compose, closed, for a rack that has no faceplate, such as the LIBRARY's preview; empty when one of its controls
        has no parameter in the rack's banks. */
    static juce::Image composeFor (const MakerSkin&, const RackInstance&, const ParameterValues&);
    /** The live sound, whose locks and values the lock dots and the marks follow. */
    void refresh (const SoundDocument&);
    /** The host's lock dot, and its pixel in face pixels: the dots' sprites and the marks' dashes are drawn in it; and
        the house skin, whose tape blank and marker letter a route's tape and the skin's own lettered tapes, each of
        those shown while its view and its shown_when are. */
    void setHostLook (const Asset& lockDot, float pixel, const house::Model& houseSkin);
    /** A taped knob's legend stays true: while the route it sets is not the enabled route the legend prints (VEL TO
        AIR's, from velocity to the analog bank's cutoff, unless the skin prints another), the route as the PROGRAMMER
        names it, upper-cased, the printed bank left out ("LFO 1 TO CUTOFF", "VELOCITY TO WAVETABLE CUTOFF", "ROUTE 1
        OFF"), which the tape over its legend reads and, in the registry's case, the knob's title; else empty. The tape
        shows while its view does. */
    juce::String routeTape() const { return routeWords (true); }
    /** What the VFD reads now: the last control touched, the face's first until one is; empty without a VFD. */
    VfdText vfd() const { return shownVfd; }
    /** The proposal's candidate sound, or none. */
    void showProposal (std::shared_ptr<const SoundDocument>);
    /** A marked control: its element and the candidate's physical value for it. */
    struct Mark
    {
        juce::String element;
        float proposed = 0;
        bool operator== (const Mark&) const = default;
    };
    /** The AI's marks, by the rows' rule: while the candidate keeps the rack with this faceplate and the rack's banks,
        each control not locked live whose value differs, in focus order, the face's then the recess's; a door's soft
        knob only while its screen shows it. */
    std::vector<Mark> marks() const;
    /** The candidate keeps the rack with another panel or other banks, which would lose the faceplate. */
    bool proposesPanel() const;
    /** Every parameter a control of the faceplate sets, the reveal's included, ascending. */
    std::vector<std::size_t> parameters() const;
    /** The live editor's parameter reach: hidden proposal edits mark the reveal's opener while closed. */
    void setRevealParameters (std::vector<std::size_t>);
    /** Retire held edits before hiding, rebinding or removing this faceplate. */
    void suspendInputs();
    /** The lock dot at its place beside each locked control, and the marks: a knob's dashed pointer at its proposed
        angle, a lever's dashed outline, or the whole faceplate's outline. The recess's dots and marks show only while
        it is open, a soft knob's only while its screen shows it. Closed, the opener is outlined while the reveal's
        controls change what no face control shows; open, so is the button of each page that holds such a change off
        the screen. Folded, the strip's display is outlined for any of them. The face is drawn again only when these
        change. What the reveal shows of these waits for it to settle open. */
    void paintOverChildren (juce::Graphics&) override;

private:
    MakerSkinPtr skinOwner;
public:
    const MakerSkin& skin;
    /** Called when the revealed view opens or closes, on each frame while it moves, and once as it settles. */
    std::function<void()> onOpen;
    /** An external SET-UP press opens the owning rack's existing numeric drawer; it changes no sound state. */
    std::function<void()> onSetup;
    std::function<void(const DrumPadTarget&,juce::Component*)> onDrumSource;
    std::function<void()> onDrumSourceClose;
    TileArrayFace* tileArray() { return tileArrayFace.get(); }
    void visibilityChanged() override;

private:
    friend class TileArrayFace;
    std::unique_ptr<TileArrayFace> tileArrayFace;
    int tileHiddenChanges = 0;
    MakerFace (InstrumentProcessor&, MakerSkinPtr, const RackInstance&, house::Model, house::Host);
    bool isDrawer() const { return model.reveal.kind == "drawer"; }
    bool isDoor() const { return model.reveal.kind == "door"; }
    bool isRecess() const { return model.reveal.kind == "recess"; }
    /** A recess whose lid slides away over it instead of lifting off. */
    bool slides() const { return isRecess() && ! model.reveal.motion.slide.isOrigin(); }
    /** A recess whose lid turns up about its top edge instead of lifting off. */
    bool hinges() const { return isRecess() && model.reveal.motion.hinge == "top"; }
    /** Open and not moving: what the reveal's marks wait for. */
    bool settledOpen() const { return open && ! isMoving(); }
    /** Each view at its place for the drawn fraction, its alpha while it moves, and the faceplate's size. */
    void layout();
    /** The fraction of the move's time gone towards the state pressed for, 1 while still. */
    float along() const;
    /** How far a recess's cover has lifted, 0 until its fasteners have turned, to 1 open. */
    float lift() const;
    /** A door's angle about its foot, 0 closed to 180 degrees hanging open. */
    float swing() const { return 180.0f * opening(); }
    /** The fasteners turned on the face, under the recess. */
    void paintFasteners (juce::Graphics&) const;
    /** Over the faceplate: the cover lifting over its shadow, the lid sliding or turning up, or the door swinging. */
    void paintMove (juce::Graphics&) const;
    /** A frame of the move: the views at the drawn fraction; at its end the closed views hidden. */
    void timerCallback() override;
    /** Where a view's pixels start in the faceplate's. */
    juce::Point<float> offsetOf (const juce::String& view) const;
    house::View* viewNamed (const juce::String& view);
    /** The marks, the outline and the descriptions again from the live sound and the candidate. */
    void remark();
    juce::String routeWords (bool upper) const;

    /** The skin with each control bound to the rack's banks; its images are the skin's. */
    house::Model model;
    house::Host host;
    std::unique_ptr<house::GeneratedText> tileText;
    house::Context context;

public:
    house::View face;
    /** The recess, or the drawer's deck. */
    house::View& revealed() { return *reveal; }
    /** The drawer's front, under the face or the deck; none for a recess. */
    house::View* front() { return drawerFront.get(); }
    /** The door, below the face while open; none for a recess or a drawer. */
    house::View* hanging() { return hang.get(); }

private:
    std::unique_ptr<house::View> reveal, drawerFront, hang, stripView;
    /** SET-UP's cover, the drawer's pull or the door's top notch, and the door's foot notch. */
    house::SkinButton* opener = nullptr;
    house::SkinButton* closer = nullptr;
    bool open = false, folded = false;
    /** The move: the fraction of its time already gone at its press, and the clock then. */
    float from = 1.0f;
    double since = 0;
    class Layer;
    /** Shown while moving: a recess's turned fasteners under it, and the cover's lift or the door's swing over all. */
    std::unique_ptr<Layer> turnLayer, moveLayer;

    class Still;
    std::vector<std::unique_ptr<Still>> stills;
    std::size_t pageIndex = 0, screenIndex = 0;
    house::SkinButton* pageButton (std::size_t page);
    /** Presses a page's button: the chosen page steps to its next screen, wrapping, another opens on its first. The
        screen's first shown item is touched. */
    void choosePage (std::size_t page, bool focus);
    /** The screen's shown items, the spares on its unused slots, and the page buttons' lights, titles and focus. */
    void showScreen();
    ParameterValues liveValues() const;
    /** The rack and the banks the face is bound to. */
    juce::Uuid rack;
    std::vector<UnitBinding> bindings;
    /** The live sound's values and locks, and the candidate. */
    ParameterValues values {};
    std::array<bool, parameterCount> locks {};
    std::shared_ptr<const SoundDocument> proposal;
    /** The locked elements, the marked ones in focus order and the outline. */
    std::vector<const house::Element*> locked;
    std::vector<std::pair<const house::Element*, float>> marked;
    bool outlined = false;
    /** The reveal's changes that no face control shows, and of those each page's off the screen. */
    int revealChanges = 0;
    std::vector<std::size_t> revealParameters;
    bool nativeLocked = false;
    std::vector<int> pageChanges;
    const Asset* lockDot = nullptr;
    float pixel = 1.0f;
    /** The route a knob with a tape rect sets: the knob, its other fields' indices and its number. */
    struct Route
    {
        const house::Element* knob = nullptr;
        std::size_t enabled = 0, source = 0, target = 0, bank = 0;
        juce::String number;
    };
    std::optional<Route> route;
    std::optional<house::TapeArt> tape;
    const house::Model* tapeSkin = nullptr;
    /** The skin's tapes with their own lettering, on the house skin's blank. */
    std::vector<std::pair<const house::Element*, house::TapeArt>> lettered;
    /** Which of those the values show. */
    std::vector<bool> letteredShown() const;

    class Glass;
    /** Each display lettered again from the live values and the last control touched. */
    void letter();
    /** The window skin's dot-matrix lettering, or none without the window skin. */
    std::unique_ptr<Lettering> letters;
    std::vector<std::unique_ptr<Glass>> glasses;
    const house::Element* touched = nullptr;
    VfdText shownVfd;
};
} // namespace airack::studio

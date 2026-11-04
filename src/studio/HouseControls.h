#pragma once

#include "HouseSkin.h"
#include "ParameterGesture.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <memory>
#include <optional>
#include <vector>

namespace airack
{
class InstrumentProcessor;
struct SoundDocument;
}

namespace airack::studio::house
{
/** What the unit needs from the editor that holds it. */
struct Host
{
    /** Held around each begin, write and end, as the editor's other controls hold theirs, so that the editor retires
        only once they unwind. */
    std::function<std::shared_ptr<void>()> frame;
    /** True while the editor retires: then nothing writes and nothing is reported. */
    std::function<bool()> closing;
    /** Says a sentence where the editor says its errors. */
    std::function<void (const String&)> report;
    /** One sound edit, as the editor makes it: the change on a snapshot, reported as caption and " complete" or
        " failed; sound preserved". */
    std::function<void (const std::function<void (SoundDocument&)>& change, const String& caption)> change;
};

/** A macro's binding by registry index: its target, if any, and its signed depth from -1 to 1. */
struct Assignment
{
    std::optional<std::size_t> target;
    float amount = 0;
    bool operator== (const Assignment&) const = default;
};

/** A placed rack's continuous focused control: its label in capitals, its rack's name and its parameter. */
struct RackTarget
{
    String label, rack;
    std::size_t index = 0;
    bool operator== (const RackTarget&) const = default;
};

/** Windows review-text renderer owned by a Tile view or one native composition scope. */
class GeneratedText final
{
public:
    GeneratedText();
    ~GeneratedText();
    GeneratedText (const GeneratedText&) = delete;
    GeneratedText& operator= (const GeneratedText&) = delete;
    bool available() const { return token != 0; }
    bool draw (juce::Graphics&, const String&, const juce::Font&, juce::Colour,
               juce::Rectangle<float>, bool centred) const;
    bool drawSprite (juce::Graphics&, const juce::Image&, Pt anchor, Pt pivot, Pt scale) const;
private:
    std::uintptr_t token = 0;
};

/** What both views share. */
struct Context
{
    const Model& model;
    InstrumentProcessor& processor;
    Host& host;
    /** A tape's text, empty for the printed dash. */
    std::function<String (const Element&)> tapeText;
    /** Called when the user touches a control: a drag, a change of its value, its keyboard focus, a lever's or a
        toggle's gesture; never a hover or automation. */
    std::function<void (const Element&)> touched;
    /** Called when a knob or fader shows a new value, the user's or automation's. */
    std::function<void (const Element&)> valueShown;
    /** False where the skin's own display reads the controls out: then the house readout never shows. */
    bool readouts = true;
    GeneratedText* generatedText = nullptr;

    void touch (const Element& e) const
    {
        if (touched)
            touched (e);
    }
    std::shared_ptr<void> frame() const { return host.frame ? host.frame() : nullptr; }
    bool closing() const { return host.closing && host.closing(); }
    void report (const String& sentence) const
    {
        if (host.report)
            host.report (sentence);
    }
};

//============================================================================== controls

class View;

/** The moving layers as the controls draw them, in view pixels less `origin`: a knob's or selector's pointer layer
    turned to `angle` over its grip ring's frame for that angle, a fader's cap at a proportion of its travel, a lever's
    bat in a position, and a lamp's or LED's lit layer. A faceplate's still picture draws the same. */
/** Optional stationary native print and knob skirt, behind the existing moving control. A missing element option
    returns before touching graphics state. The live View and MakerFace's still composition share this renderer. */
void paintNativeEngraving (juce::Graphics&, const Model&, const Element&, Pt origin = {});
void paintNativeViewText (juce::Graphics&, const Model&, const String& view, Pt origin = {});
/** Source-pixel frame corners and borders, with its measured interior resized to fit. */
void paintNineSlice (juce::Graphics&, const Asset&, juce::Rectangle<int>);
void paintPointer (juce::Graphics&, const Model&, const Element&, float angle, Pt origin = {});
void paintCap (juce::Graphics&, const Model&, const Element&, double proportion, Pt origin = {});
void paintBat (juce::Graphics&, const Model&, const Element&, const Position&, Pt origin = {});
void paintLight (juce::Graphics&, const Model&, const Element&, Pt origin = {});
/** A needle's layer standing at the x its scale gives a proportion of its travel. */
void paintNeedle (juce::Graphics&, const Model&, const Element&, double proportion, Pt origin = {});
/** A lever's position for a value: the one whose value is nearest. */
const Position& positionFor (const Element&, double value);
/** Whether a lamp is lit at a value. */
inline bool litAt (const Element& e, double value) { return std::abs (value - e.litValue) < 0.5; }

class Readout final : public juce::Component
{
public:
    Readout() { setInterceptsMouseClicks (false, false); }

    /** Right of the control, or left of it where the panel ends; legends and tapes above and below stay visible. */
    void show (const String& t, juce::Rectangle<float> beside, juce::Rectangle<int> limits);
    void paint (juce::Graphics& g) override;

    String text;

private:
    static juce::Font font() { return juce::Font (juce::FontOptions (26.0f, juce::Font::bold)); }
};

/** Knobs and faders: JUCE's vertical-drag slider over a registry parameter. A drag is one gesture; wheel, keys and
    double-click are complete gestures. The processor refuses an edit while another gesture or an AI reservation
    holds it; then the control shows the parameter again and the host reports why. A detented control takes whole
    values only, and its keys step one value a press. Only drawing and hit areas differ. */
class BoundSlider : public juce::Slider
{
public:
    BoundSlider (View& ownerIn, const Element& el, int dragPixels);
    ~BoundSlider() override;
    void retire();

    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void focusGained (FocusChangeType) override;
    void focusLost (FocusChangeType) override;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;
    /** End a held edit before its view hides; late events cannot start another edit. */
    void suspend();

    virtual void updateBounds() = 0;
    /** The part of the control the readout sits beside. */
    virtual juce::Rectangle<float> readoutBeside() = 0;

    double proportion() { return valueToProportionOfLength (getValue()); }
    String readoutText() const { return formatValue (element, param.convertFrom0to1 (param.getValue())); }
    bool showsFocusRing() const { return focusFromKeyboard && hasKeyboardFocus (false); }
    bool gestureHeld() const { return gesture.held(); }

    const Element& element;
    juce::RangedAudioParameter& param;

protected:
    View& owner;
    void resumeInput() { suspended = false; }
    bool inputSuspended() const { return suspended; }

private:
    void shown();
    void read (float physical);

    ParameterGesture gesture;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    bool focusFromKeyboard = false;
    bool suspended = false;
};

/** A knob. Where the skin's steps print legends that take presses, a press on one (or within a few pixels of the
    nearest) chooses that step where it lets go, one complete gesture, and shows the readout; it turns nothing, and a
    double-click there leaves the step it chose. */
class Knob final : public BoundSlider
{
public:
    Knob (View& o, const Element& e) : BoundSlider (o, e, 300) {}

    float pointerAngle() { return pointerAngleFor (element, proportion()); }
    void updateBounds() override;
    void paint (juce::Graphics&) override;
    /** The skin's hit_rect where it gives one, else the hit circle. */
    bool hitTest (int x, int y) override
    {
        const auto at = Pt ((float) x, (float) y) + getPosition().toFloat();
        return element.hitRect.isEmpty() ? at.getDistanceFrom (element.hitCentre()) <= element.hitR : element.hitRect.contains (at);
    }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    juce::Rectangle<float> readoutBeside() override { return element.footprint(); }

private:
    /** The step whose printed legend is nearest a point of the knob, within reach of it. */
    std::optional<std::size_t> legendAt (Pt local) const;

    std::optional<std::size_t> legendPressed;
};

class Fader final : public BoundSlider
{
public:
    Fader (View& o, const Element& e) : BoundSlider (o, e, juce::roundToInt (std::abs (e.y0 - e.y1))) {}

    float capY() { return capYAt (element, proportion()); }
    static float capYAt (const Element& e, double p) { return e.y0 + (e.y1 - e.y0) * (float) p; }
    void updateBounds() override;
    void paint (juce::Graphics&) override;
    bool hitTest (int x, int y) override { return element.hitRect.contains (Pt ((float) x, (float) y) + getPosition().toFloat()); }
    juce::Rectangle<float> readoutBeside() override { return element.hitRect.withY (capY()).withHeight (0.0f); }
};

/** A panel button: the face and LED are in the plate; hover, pressed, focus and disabled are drawn here, the LED's
    lit layer comes from the skin. A toggle writes one complete gesture through the processor; an action calls
    onAction. */
class SkinButton final : public juce::Component
{
public:
    SkinButton (View& ownerIn, const Element& el);
    void retire();

    void updateBounds();
    void paint (juce::Graphics&) override;
    bool hitTest (int x, int y) override { return area().expanded (3.0f).contains (Pt ((float) x, (float) y) + getPosition().toFloat()); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    /** Where it is pressed and drawn: its lit place while lit, else its rect. */
    juce::Rectangle<float> area() const { return lit() && ! element.litRect.isEmpty() ? element.litRect : element.rect; }
    void focusGained (FocusChangeType cause) override { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
    void focusLost (FocusChangeType) override { focusFromKeyboard = false; repaint(); }
    void enablementChanged() override { pressed = false; repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    void trigger();
    bool isToggle() const { return param != nullptr; }
    bool lit() const { return isToggle() ? juce::roundToInt (value) == element.onIndex : actionLit; }
    void setActionLit (bool on) { actionLit = on; repaint(); }
    bool showsFocusRing() const { return focusFromKeyboard && hasKeyboardFocus (false); }

    const Element& element;
    std::function<void()> onAction;
    /** The left and right arrow keys, -1 and 1; unset, the arrows are not the button's. */
    std::function<void (int)> onArrow;
    /** One of a group of which one is chosen: its role is a radio button's, checked while lit. Set before its
        accessibility handler is made. */
    bool radio = false;
    bool hover = false, pressed = false;

private:
    void write (float physical);
    void reject();

    View& owner;
    const juce::Colour panel;
    juce::RangedAudioParameter* const param;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    float value = 0;
    bool actionLit = false, focusFromKeyboard = false, pressInside = false;
};

/** A bat lever with any number of positions, or a selector: its pointer layer at its position's angle. A click inside
    it, Space or Enter steps to its next position, wrapping; a click on a selector's printed legend chooses that
    position; the arrow keys and the wheel step to the next or previous one, stopping at the ends: a selector's Up and
    wheel away turn it clockwise, a lever's move its bat up. Each step is one complete gesture, refused as every
    control's edit is; hover and focus are drawn as on a panel button, but a lever pressed on its legends (a pair of
    buttons) draws no hover, since a box over both is not on the faceplate. Its hit area is the skin's hit_rect where it
    gives one, else its rect. */
class Lever final : public juce::Component
{
public:
    Lever (View& ownerIn, const Element& el);
    ~Lever() override;
    void retire();

    void updateBounds();
    void paint (juce::Graphics&) override;
    bool hitTest (int x, int y) override { return area().contains (Pt ((float) x, (float) y) + getPosition().toFloat()); }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { pressed = isEnabled(); }
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (FocusChangeType cause) override { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
    void focusLost (FocusChangeType) override { focusFromKeyboard = false; repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    void trigger();
    const Position& position() const { return positionFor (element, value); }
    String legend() const { return position().legend; }
    bool showsFocusRing() const { return focusFromKeyboard && hasKeyboardFocus (false); }
    bool isSelector() const { return element.kind == "selector"; }

    const Element& element;
    bool hover = false;

private:
    juce::Rectangle<float> area() const { return (element.hitRect.isEmpty() ? element.rect : element.hitRect).expanded (3.0f); }
    std::size_t at() const { return (std::size_t) (&position() - element.positions.data()); }
    /** Steps `by` positions towards the end, stopping there. */
    void step (int by);
    void choose (std::size_t index);
    void read (float physical);

    View& owner;
    juce::RangedAudioParameter& param;
    ParameterGesture gesture;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    float value = 0;
    bool pressed = false, focusFromKeyboard = false;
};

/** A lamp: the skin's lit layer while its parameter is at its lit value. It only shows the parameter, and takes no
    clicks or focus. */
class Lamp final : public juce::Component
{
public:
    Lamp (View& ownerIn, const Element& el);
    ~Lamp() override;
    void retire();

    void updateBounds();
    void paint (juce::Graphics&) override;
    bool lit() const { return litAt (element, value); }

    const Element& element;

private:
    View& owner;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    float value = 0;
};

/** A needle: the skin's needle layer at the x its printed scale gives its parameter, linear between the scale's ticks
    and pinned at its ends. It only shows the parameter, and takes no clicks or focus. */
class Needle final : public juce::Component
{
public:
    Needle (View& ownerIn, const Element& el);
    ~Needle() override;
    void retire();

    void updateBounds();
    void paint (juce::Graphics&) override;
    /** Where it stands, in its view's pixels. */
    float x() const;

    const Element& element;

private:
    /** Its layer standing at `at`, in its view's pixels. */
    juce::Rectangle<int> layerAt (float at) const;

    View& owner;
    std::unique_ptr<juce::ParameterAttachment> attachment;
    float value = 0;
};

/** A console tape's art: the skin's blank 9-sliced to a body of `size` design px, lettered in the unit's marker. */
class TapeArt
{
public:
    TapeArt (const Model& model, const String& blankAsset, Pt size);

    /** Centred on `centre`, turned `angle` degrees; two lines split at the space nearest the middle when `twoLines`; a
        `tint` fills the blank's shape first, the blank over it at 30%. */
    void paint (juce::Graphics&, Pt centre, float angle, const String& text, bool twoLines = false,
                std::optional<juce::Colour> tint = {}) const;
    /** The body width at which one line of `text` keeps the lettering's full cap at body height `height`. */
    static float widthFor (const Model&, const String& text, float height);
    /** Half the blank's diagonal: the art stays within it of its centre at any angle. */
    float radius() const;

private:
    const Model& model;
    String asset;
    Pt size;
    juce::Image blank;
};

/** A console tape: blank 9-slice from the skin plus marker text, or the printed dash. */
class Tape final : public juce::Component
{
public:
    Tape (View& ownerIn, const Element& el);

    void updateBounds();
    void paint (juce::Graphics&) override;
    String text() const;

    const Element& element;

private:
    View& owner;
    TapeArt art;
};

/** One view of a skin, the house unit's "unit" or "strip" or a faceplate's: its plate with the controls and tapes on it,
    a tape without its own blank being its faceplate's to letter.
    Its controls' component IDs are `idPrefix` and their element IDs, and their "element" property is the element ID,
    as the window skin's are; a control outside the view's focus order takes no keyboard focus. A plate with an anchor,
    as a faceplate's recess has, is drawn at it. */
class View final : public juce::Component
{
public:
    View (Context& c, const String& viewName, const String& idPrefix = "house.");
    ~View() override;
    /** Detach processor listeners and finish gestures before deferred UI deletion. */
    void retire();

    void paint (juce::Graphics& g) override
    {
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImageAt (plate, plateAt.x, plateAt.y);
        for (const auto& e : ctx.model.elements)
            if (e.view == name && e.nativeEngraving)
                paintNativeEngraving (g, ctx.model, e);
        paintNativeViewText (g, ctx.model, name);
    }
    bool hitTest (int x, int y) override { return x >= covered && juce::Component::hitTest (x, y); }
    /** The view's first px take no press: they lie under the unit's rail while the view is scrolled sideways. */
    void setCovered (int px) { covered = px; }

    void showReadout (BoundSlider& s) { active = &s; refreshReadout(); }
    void hideReadout (BoundSlider& s);
    void hideReadouts();
    void suspendInputs();
    bool hasHeldGesture() const;
    /** Called after a continuous edit releases its processor gesture, including suspension. */
    std::function<void()> onGestureEnd;
    juce::Rectangle<int> readoutBounds() const { return readout.isVisible() ? readout.getBounds() : juce::Rectangle<int>(); }
    void refreshReadout();
    void elementChanged (BoundSlider& s);
    void refreshTapes();

    template <typename T>
    static T* findIn (const std::vector<std::unique_ptr<T>>& list, const String& localId)
    {
        for (auto& c : list)
            if (c->element.localId() == localId)
                return c.get();
        return nullptr;
    }

    template <typename T = BoundSlider>
    T* slider (const String& localId) { return dynamic_cast<T*> (findIn (sliders, localId)); }
    SkinButton* button (const String& localId) { return findIn (buttons, localId); }
    Tape* tape (const String& localId) { return findIn (tapes, localId); }
    Lever* lever (const String& localId) { return findIn (levers, localId); }
    Lamp* lamp (const String& localId) { return findIn (lamps, localId); }
    Needle* needle (const String& localId) { return findIn (needles, localId); }

    juce::Component* control (const String& localId)
    {
        if (auto* s = findIn (sliders, localId))
            return s;
        if (auto* l = lever (localId))
            return l;
        return button (localId);
    }

    /** The plate's colour in a ring just outside r (per-channel median, so nearby legends don't count). */
    juce::Colour panelAround (juce::Rectangle<float> r) const;

    Context& ctx;
    const String name;
    std::vector<std::unique_ptr<BoundSlider>> sliders;
    std::vector<std::unique_ptr<SkinButton>> buttons;
    std::vector<std::unique_ptr<Tape>> tapes;
    std::vector<std::unique_ptr<Lever>> levers;
    std::vector<std::unique_ptr<Lamp>> lamps;
    std::vector<std::unique_ptr<Needle>> needles;
    /** The element IDs in Tab order. */
    juce::StringArray focusOrder;

private:
    juce::Image plate;
    juce::Point<int> plateAt;
    Readout readout;
    BoundSlider* active = nullptr;
    int covered = 0;
};

class EditorLook final : public juce::LookAndFeel_V4
{
public:
    EditorLook()
    {
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff26272b));
        setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff5a5c62));
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1d1e21));
        setColour (juce::TextButton::buttonColourId, juce::Colour (0xff303136));
        setColour (juce::Slider::thumbColourId, accent);
        setColour (juce::Slider::trackColourId, juce::Colour (0xff8a6a32));
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff5a5c62));
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (26.0f)); }
    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (26.0f)); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::Font (juce::FontOptions (24.0f, juce::Font::bold)); }
    juce::Font getLabelFont (juce::Label&) override { return juce::Font (juce::FontOptions (26.0f)); }
};

/** Today's Assign and Clear for one macro, live: a target and a signed depth. Picking a target, a depth outside a
    drag, None and Clear are each one edit; a depth drag previews until its release, then is one edit. */
class AssignEditor final : public juce::Component
{
public:
    AssignEditor();
    ~AssignEditor() override;

    /** column: the knob, tape and ASSIGN button being edited. The editor opens beside it (left where there is room)
        so the tape shows the binding live. */
    void open (int macroIndex, juce::Component& from, juce::Rectangle<float> column);
    void close (bool restoreFocus = true);
    /** Shows the open macro's binding as the document holds it. */
    void show (const Assignment& a);
    /** The targets after None: these, then every continuous parameter but the macros, by its registry name. */
    void setRackTargets (const std::vector<RackTarget>& targets);
    /** What the editor shows. */
    Assignment assignment() const;
    bool isDragging() const { return dragging; }

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

    int macro = 0;   // 1..8 while open
    juce::ComboBox target;
    juce::Slider depth;
    juce::TextButton clear { "Clear" }, done { "Close" };
    std::function<void (int, bool)> onOpenChanged;
    std::function<void (int, const Assignment&)> onAssign;
    std::function<void (int, std::optional<Assignment>)> onPreview;

private:
    int idFor (std::optional<std::size_t> index) const;
    void push();

    EditorLook look;
    /** The registry index of each item after None, by its item ID less 2. */
    std::vector<std::size_t> items;
    juce::Component* opener = nullptr;
    bool loading = false, dragging = false;
};

/** The rail left of a unit: folds it to its strip and back. */
class FoldRail final : public juce::Component
{
public:
    explicit FoldRail (const String& id = "house.fold", const String& title = "Fold the air unit");

    void paint (juce::Graphics& g) override;
    bool hitTest (int, int y) override { return y < 90; }
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override { pressed = true; repaint(); }
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
    void focusLost (FocusChangeType) override { focusFromKeyboard = false; repaint(); }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    std::function<void()> onToggle;
    bool folded = false, hover = false, pressed = false, focusFromKeyboard = false;
};
} // namespace airack::studio::house

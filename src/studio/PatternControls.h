#pragma once

#include "SkinControls.h"

#include <optional>

namespace airack::studio
{
/** "D4": MIDI 60 is C4. */
String noteName (int pitch);
/** START as bar.beat.sixteenth from 1, the beat the meter's denominator; a fourth field in 64ths off the sixteenth. */
String startText (double start, int numerator, int denominator);
/** LENGTH as the reduced fraction of a whole note ("1/8", "3/16"). */
String lengthText (double duration);

/** What a knob turns: its detents and position, its texts, and the edits a turn, a typed value or its menu make.
    A drag is begin(), then set() at each new detent, then end (true) on release or end (false) when it is cancelled;
    outside a drag, each set() is one complete edit. */
struct KnobBinding
{
    using Menu = std::vector<std::pair<String, std::function<void()>>>;
    virtual ~KnobBinding() = default;
    /** Detents over the sweep; 0 for a continuous control, which the keys and the wheel move in hundredths. */
    virtual int steps() const = 0;
    virtual double fraction() const = 0;
    virtual void set (double fraction) = 0;
    virtual String valueText() const = 0;
    virtual String entryText() const = 0;
    /** A typed value in the entry's text; false leaves the control as it was. */
    virtual bool enter (const String& text) = 0;
    virtual bool enabled() const { return true; }
    virtual void addMenu (Menu&) {}
    /** A drag starts; false refuses it. */
    virtual bool begin() { return true; }
    virtual void end (bool keep) { juce::ignoreUnused (keep); }
};

/** A pattern unit knob: a pointer sprite turned over the sweep by its binding, its body and legend in the plate. A
    vertical drag of 200 px covers the range (Shift: 1000 px), stepped controls moving by detents, and Escape cancels it;
    the wheel and the arrows step, Page Up and Down ten steps, Home and End go to the ends; a double click or Enter opens
    an entry for a typed value; a right click opens the menu. Without a binding it is disabled. Coordinates are the
    view's. */
class Knob final : public juce::Component
{
public:
    Knob (Context& c, const Element& e);

    void setBinding (std::unique_ptr<KnobBinding> b);
    KnobBinding* binding() const { return bound.get(); }
    /** Shows the binding's state: enabled or not, the pointer, the closed entry's text; repaints only on a change. */
    void refresh();

    /** Degrees from 12 o'clock, clockwise. */
    float angle() const;
    Rect entryArea() const;
    KnobBinding::Menu menu() const;

    void paint (juce::Graphics& g) override;
    bool hitTest (int x, int y) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;
    void enablementChanged() override;

    /** A new position: a stepped control lands on the nearest detent and edits only when that detent changes. */
    void move (double f);
    void stepBy (int k);
    void openEntry();
    /** Closes the entry and enters what it holds. */
    void commitEntry();
    /** Closes the entry, keeping the value. */
    void closeEntry();
    void showMenu();
    /** Drops a drag without ending its gesture: the owner has cancelled it. */
    void cancelDrag();
    bool isDragging() const { return dragging; }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    const Pt centre;
    /** The hit box and the legend's ink. */
    const Rect hit, labelInk;
    const float skirt;
    /** Its component ID is the knob's with ".entry". */
    juce::TextEditor entry;
    bool focusFromKeyboard = false;

private:
    Context& ctx;
    const float sweep;
    const Asset& pointer;
    std::unique_ptr<KnobBinding> bound;
    double dragFrom = 0.0, shownFraction = -1.0;
    String shownValue;
    bool dragging = false;
};

/** What the roll shows: the pattern, its phrase and grid, the selection and the playhead. */
struct RollShown
{
    std::vector<Note> notes;
    int bars = 4, numerator = 4, denominator = 4;
    String grid = "1/16";
    double gridBeats = 0.25;
    int selected = -1;
    /** Beats from the phrase's start while the internal pattern plays. */
    std::optional<double> playhead;

    double lengthBeats() const { return (double) bars * numerator * 4.0 / denominator; }
    bool operator== (const RollShown&) const = default;
};

/** The roll on its glass: bars and beats over the phrase, a row per semitone beside the ruler's keys, each note a bar
    with a velocity stem, the selected one outlined with its handle, the playhead while the internal pattern plays.
    The gestures: a double click on empty grid adds a note, a click selects (on empty grid, nothing), a drag moves
    the note (from its last 4 px resizes it; with Alt sets its velocity) and edits it once, on release; Escape cancels a
    drag; Delete or Backspace removes the selected note; the wheel, the arrows and the scrollbar scroll the rows. The
    owner edits; the roll shows what the owner shows it. Coordinates are the view's. */
class Roll final : public juce::Component
{
public:
    Roll (Context& c, const Element& e);

    /** Repaints only what changed. */
    void show (const RollShown& s);
    const RollShown& shown() const { return s; }

    float beatWidth() const { return (x1 - x0) / (float) s.lengthBeats(); }
    float rowHeight() const { return rowH; }
    int rowsShown() const { return rows; }
    float xOf (double beat) const { return x0 + (float) beat * beatWidth(); }
    float rowCentre (int pitch) const { return top + ((float) (rollTop - pitch) + 0.5f) * rowH; }
    Pt at (double beat, int pitch) const { return { xOf (beat), rowCentre (pitch) }; }
    /** A note's bar, on whole pixels as drawn. */
    Rect noteRect (int i) const;
    Rect selectedOutline() const;
    float playheadX() const { return xOf (s.playhead.value_or (0.0)); }
    /** The scrollbar's thumb: the shown rows over the 128 pitches. */
    Rect thumb() const;

    /** The pitch of the top row. */
    int topRow() const { return rollTop; }
    void scrollTo (int topPitch);
    /** Scrolls the rows the least that shows the pitch. */
    void reveal (int pitch);
    /** Drops a drag in progress without a callback. */
    void cancelDrag();
    bool isDragging() const { return dragging; }

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    /** Asked before a press or a double click on the roll acts; false refuses it. */
    std::function<bool()> onAdmit;
    /** A press selected the note (-1: none). */
    std::function<void (int)> onSelect;
    std::function<void (const Note&)> onAdd;
    /** A drag changed the note: once, on release. */
    std::function<void (int, const Note&)> onEdit;
    std::function<void()> onCancel, onDelete;
    /** Whether a drag may show the note in its place. */
    std::function<bool (int, const Note&)> accepts;

    const Element& element;
    bool focusFromKeyboard = false;

private:
    const Note& noteShown (int i) const;
    int noteAt (Pt p) const;
    bool inGrid (Pt p) const { return p.x >= x0 && p.x < x1 && p.y >= top && p.y < top + (float) rows * rowH; }
    Rect track() const;
    Rect playheadArea() const;
    void describe();

    Context& ctx;
    const juce::var spec;
    float x0 = 0, x1 = 0, top = 0, rowH = 8, laneFoot = 0;
    int rows = 28, rollTop = 75;
    RollShown s;
    std::optional<Note> preview;
    int previewIndex = -1, scrollFrom = 0;
    Note captured;
    Pt origin;
    bool dragging = false, resizing = false, velocity = false, scrolling = false;
};

/** The flat membrane keyboard printed over the drawn bed: two octaves from the lowest key. A press sends a note on at
    velocity 100 on MIDI channel 1, a release or leaving the key its note off; a drag across the keys plays each in
    turn. With the focus, the computer keys A W S E D F T G Y H U J K O L P ; play MIDI 72 to 88 at velocity 127,
    released when the focus leaves. Coordinates are the view's. */
class Keyboard final : public juce::Component
{
public:
    Keyboard (Context& c, const Element& e);

    int keyCount() const { return (int) spec["keys"]; }
    int lowestKey() const { return lowest; }
    /** Moves the lowest key by octaves within the skin's range. */
    void shift (int octaves);
    /** White keys fill the rect gap px apart; a black key straddles the line between its white neighbours. */
    Rect keyRect (int k) const;
    int pitchAt (Pt p) const;
    bool isHeld (int pitch) const { return down == pitch || computer.count (pitch) > 0; }
    bool isPlaying() const { return down >= 0 || ! computer.empty(); }
    /** Drops the held keys without note offs. */
    void forget();
    /** Sends a note off for each held key. */
    void releaseAll();

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress& key) override;
    bool keyStateChanged (bool isKeyDown) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    /** Sends a note (velocity 0 for a note off); false when the queue is full, and the keyboard forgets its keys. */
    std::function<bool (bool on, int pitch, int velocity)> onKey;
    /** Whether a computer key is down: the operating system's state, which tests replace. */
    std::function<bool (const juce::KeyPress&)> isKeyDown = [] (const juce::KeyPress& k) { return k.isCurrentlyDown(); };

    const Element& element;
    bool focusFromKeyboard = false;

private:
    bool send (bool on, int pitch, int velocity);
    void press (int pitch);
    void release();

    Context& ctx;
    const juce::var spec;
    juce::Array<juce::KeyPress> computerKeys;
    std::set<int> computer;
    int lowest = 48, down = -1;
};
} // namespace airack::studio

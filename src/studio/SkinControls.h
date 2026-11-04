#pragma once

#include "DotMatrix.h"

#include <functional>
#include <set>

namespace airack::studio
{
inline const juce::Colour accent (0xffe8a23a);

struct Context
{
    const Model& model;
    const Lettering& lettering;
    const Lettering& screenLettering;
    bool showHitAreas = false;
};

void drawHitArea (juce::Graphics& g, const Context& ctx, Rect r);

/** Procedural states over a face that lives in the plate: hover lightens, a press darkens, keyboard focus rings. */
void drawFaceStates (juce::Graphics& g, Rect face, bool enabled, bool hover, bool pressed, bool focusRing, juce::Colour panel);

/** A panel button: face, legend and unlit LED in the plate. A press released inside, Space, Enter and triggerClick()
    each run onClick once; a toggle lights its LED from the skin while its toggle state is on. With onDragOut set, a
    left-button press dragged 6 px or more runs it once instead, and that press clicks nothing. */
class SkinButton final : public juce::Button
{
public:
    /** `litScale` sizes the lit sprite to an LED whose lens differs from the sprite's (the PROGRAMMER's page LEDs, the
        AI unit's and the LIBRARY's action LEDs). */
    SkinButton (Context& c, const Element& e, float litScaleIn = 1.0f);

    bool isToggle() const { return element.kind == "toggle"; }
    bool isMode() const { return element.kind == "mode" || element.kind == "page"; }
    bool lit() const;
    /** Repaints when `lit()` differs from what was last painted: for a `litWhen` that changes without a press. */
    void showLit();
    /** A mode button is checked while its mode is chosen, which its toggle state holds, though its LED lights only
        while the mode's page shows; another toggle is checked while lit. */
    bool checked() const { return isMode() ? getToggleState() : lit(); }

    void paintButton (juce::Graphics& g, bool, bool) override;
    bool hitTest (int x, int y) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;
    void enablementChanged() override;

    void trigger();

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    bool hover = false, pressed = false, focusFromKeyboard = false;
    /** A host-drawn button's own lit test, in place of its toggle state. */
    std::function<bool()> litWhen;
    std::function<void()> onDragOut;
    const float litScale;

private:
    Context& ctx;
    bool pressInside = false, dragArmed = false, paintedLit = false;
};

/** A two-position bat lever: the plate has bare panel, the skin has a sprite per position. A click flips it, a drag of
    8 px or more picks the direction, Up/Down pick a position and Space/Enter flip it; each change runs onClick once.
    Its toggle state is the lower position. */
class Lever final : public juce::Button
{
public:
    Lever (Context& c, const Element& e);

    int position() const { return getToggleState() ? 1 : 0; }
    String positionName() const { return element.positions[position()]; }
    void setPosition (int p, juce::NotificationType notification) { setToggleState (p == 1, notification); }

    void paintButton (juce::Graphics& g, bool, bool) override;
    bool hitTest (int x, int y) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;

    void set (int p);

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    bool focusFromKeyboard = false;

private:
    Context& ctx;
    bool decided = false;
};

/** The fold triangle and PATTERN legend: on the strip it unfolds the pattern unit, on the unfolded unit it folds it. */
class Disclosure final : public juce::Component
{
public:
    Disclosure (Context& c, const Element& e);

    void paint (juce::Graphics& g) override;
    bool hitTest (int x, int y) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;

    void press();
    void setExpanded (bool shouldBeExpanded);

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    Context& ctx;
    bool hover = false, pressed = false, focusFromKeyboard = false, expanded = false;
    std::function<void()> onPress;
};

/** The session's ERROR LED: lit from the skin while a load error stands or a failed result is showing. */
class Indicator final : public juce::Component
{
public:
    Indicator (Context& c, const Element& e);

    void paint (juce::Graphics& g) override;
    void setLit (bool shouldBeLit);
    bool isLit() const { return lit; }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override { return createIgnoredAccessibilityHandler (*this); }

    const Element& element;

private:
    Context& ctx;
    bool lit = false;
};

struct SessionShown
{
    String layout, name, tempoKey, tempoText, headline, detail, accessible;
    bool errorLed = false;
    /** The preset path the name comes from, which the path entry opens with. */
    String path;
};

struct StripShown
{
    String layout, text, accessible;
    bool amber = false;
    /** The pattern path the pattern unit's settings display opens its entry with. */
    String path;
};

struct NoteShown
{
    String text, accessible;
    bool selected = false;
};

/** The PROGRAMMER's display: the bank line, and the last touched control's value, empty until one is touched. */
struct ProgrammerShown
{
    String bank, value, accessible;
};

/** A display over its glass: renders the plate's glass (with the unlit lattice) plus the lines and indicators it was
    last shown. The session display opens the preset path's entry, the pattern unit's settings display the pattern
    path's; the note display lights the marks under the note knobs while a note is selected; the PROGRAMMER's (an
    element of view "advanced") squeezes, then elides, its bank line short of its value line. */
class Display final : public juce::Component
{
public:
    enum class Kind { session, strip, note, programmer };

    Display (Context& c, const Element& e);

    void show (const SessionShown& s);
    void show (const StripShown& s);
    void show (const NoteShown& s);
    void show (const ProgrammerShown& s);

    /** The texts on the glass now, by line key. */
    std::map<String, Placed> placed;

    void paint (juce::Graphics& g) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;
    void resized() override;

    void openEntry();
    /** Closes the entry and takes what it holds as the path, on Enter or when the open entry loses the focus. */
    void commitPath();
    /** Closes the entry, keeping the path last shown. */
    void closeEntry();

    String accessibleValue() const
    {
        return kind == Kind::session ? session.accessible : kind == Kind::note ? note.accessible : kind == Kind::programmer ? programmer.accessible : strip.accessible;
    }
    /** The pixels per design pixel it last rendered at. */
    float renderScale() const { return scale; }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    SessionShown session;
    StripShown strip;
    NoteShown note;
    ProgrammerShown programmer;
    juce::TextEditor entry;
    /** A path committed in the entry, trimmed. */
    std::function<void (const String&)> onPath;
    /** A right-click on the display. */
    std::function<void()> onContext;
    bool focusFromKeyboard = false;

private:
    void showKey (const String& key, const String& accessibleBefore);
    void render();

    Context& ctx;
    const DisplaySpec& spec;
    const Kind kind;
    const bool pathEntry;
    juce::Image image;
    float scale = 1.0f;
    String shownKey;
};

struct Block { int c0 = 0, c1 = 0, top = 0; float x0 = 0, x1 = 0; };

/** The overview's note blocks for a pattern: columns by time over the phrase; one row per semitone, compressed when
    the range would not fit; each note rowsPerNote tall; the range centred. Mirrors window_skin.py. */
std::vector<Block> overviewBlocks (const std::vector<Note>& notes, const OverviewSpec& o, int bars, int numerator, int denominator);

/** The lattice cells (column, row) the blocks light. */
std::set<std::pair<int, int>> cellsOf (const std::vector<Block>& blocks, int rowsPerNote);

std::set<std::pair<int, int>> overviewCells (const std::vector<Note>& notes, const OverviewSpec& o, int bars, int numerator, int denominator);

/** The strip's overview: bar lines and numbers, and the pattern's notes as lit dots on the overview's lattice. */
class Overview final : public juce::Component
{
public:
    Overview (Context& c, const Element& e);

    void show (const std::vector<Note>& notesToShow, int barsToShow, int numeratorToShow, int denominatorToShow);

    void paint (juce::Graphics& g) override;
    /** The pixels per design pixel it last rendered at. */
    float renderScale() const { return scale; }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    std::vector<Block> blocks;
    std::set<std::pair<int, int>> cells;

private:
    void render();
    /** Each note fills its time span over its rows, inset on every side, area-sampled, with a seam dimming the
        boundary between its rows; overlapping notes merge. Pixel-index coordinates, as measured: at 1x pixel i
        covers [i - 0.5, i + 0.5] of the glass, and at scale s [i / s - 0.5, (i + 1) / s - 0.5]. */
    void litBlocks (const OverviewSpec& o, Pt origin);

    Context& ctx;
    juce::Image image;
    float scale = 1.0f;
    std::vector<Note> notes;
    int bars = 0, numerator = 4, denominator = 4;
};
} // namespace airack::studio

#pragma once

#include "SkinControls.h"

#include <optional>
#include <vector>

namespace airack::studio
{
/** A screen style: the condensed capitals (the screen face) or the display face at a cap height, with tracking and a
    horizontal scale; grey styles draw in that level rather than the glass's ink. */
struct TextStyle
{
    bool caps = true;
    float cap = 12.0f, tracking = 0.0f, hscale = 1.0f;
    int grey = -1;
    /** The narrowest the drawings condense this style: wrapped text squeezes this far before it breaks a line. */
    float narrowest = 1.0f;
};

/** A style of elements.json's screens.styles. */
TextStyle styleOf (const juce::var& screens, const String& name);

struct Faces
{
    const Lettering& caps;
    const Lettering& mono;
    const Lettering& of (const TextStyle& s) const { return s.caps ? caps : mono; }
};

/** A line's outline in a style, its pen at 0 on the baseline: each glyph moved on by the tracking, then scaled. */
juce::Path styledPath (const Faces& f, const TextStyle& st, const String& text, float hscale);

/** Text placed on a screen: its outline in glass pixels, what it shows after any eliding, and its scale. */
struct ScreenText
{
    juce::Path path;
    String text, shown;
    float hscale = 1.0f;
    Rect ink() const { return path.getBounds(); }
};

/** Text with its ink's left edge at x (or its centre at centreX) on the baseline. Too wide for maxRight it squeezes
    down to minH, then drops characters for an ellipsis. */
ScreenText setText (const Faces& f, const TextStyle& st, const String& text, float x, float baseline, float maxRight, float minH,
                    std::optional<float> centreX = {});

/** An LED with its legend that is also a button: ACCOUNT, LINKED INSTANCES and MCP. The plate has the unlit LED and the
    legend (MCP's are printed over it by its unit); the lit sprite comes from the skin. A press released inside, Space, Enter and triggerClick() each run
    onClick once. */
class Target final : public juce::Button
{
public:
    Target (Context& c, const Element& e);

    /** The lit layer (green, amber or lit; empty for none) and the state the accessible description reads. */
    void show (const String& layer, const String& state);
    const String& litLayer() const { return lit; }
    const String& state() const { return stateText; }

    void paintButton (juce::Graphics& g, bool, bool) override;
    bool hitTest (int x, int y) override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;

    void trigger();

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    bool hover = false, pressed = false, focusFromKeyboard = false;

private:
    Context& ctx;
    String lit, stateText;
};

/** The model display: the chosen model's name, or what the account lacks, centred on the plate's clean glass in the
    screen face, squeezed and then elided when it runs long. A press released inside, Space, Enter and triggerClick()
    each run onClick once. */
class Readout final : public juce::Button
{
public:
    Readout (Context& c, const Element& e);

    void show (const String& t);
    /** The style's centre: the glass's, moved as the drawings place it. */
    float centreX() const;

    void paintButton (juce::Graphics& g, bool, bool) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;

    void trigger();

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    ScreenText text;

private:
    Context& ctx;
    Faces faces;
    bool pressed = false, focusFromKeyboard = false;
};

struct ChangeRow
{
    String name, before, after;
    /** Kept by the original sound's lock: KEPT: LOCKED and the lock in place of the after value. */
    bool locked = false;

    bool operator== (const ChangeRow&) const = default;
};

struct PeerRow
{
    String role, source, state, tip;
    bool selected = false;

    bool operator== (const PeerRow&) const = default;
};

/** What a side unit's screen shows, as its unit decides it: the page, and the texts and choices on it. */
struct ScreenShown
{
    /** proposal, create, alert, signed_out, connected, join, linked, mcp or library. */
    String page;
    // the create and proposal pages
    String prompt;
    /** The caret ends the prompt, as it does while the prompt is being written. */
    bool promptFocus = false;
    /** The context fields by label (KEY, SCALE, ROLE, BPM). The page shows its mode's fields; an empty value shows
        the label alone, and an empty BPM HOST BPM. */
    std::vector<std::pair<String, String>> context;
    /** The option rows' indices by binding (ai.scope). */
    std::map<String, int> options;
    /** Option bindings drawn dim and inert. */
    juce::StringArray dim;
    String title;
    /** A named proposal's name, which the header shows in capitals in place of CREATE SOUND. */
    String name;
    /** The small button right of the title: CANCEL or none. */
    String action;
    std::vector<ChangeRow> changes;
    String status;
    // the account, alert, linked, join and MCP pages
    String heading, button;
    juce::StringArray body;
    /** The models, or the join page's discovered groups. */
    juce::StringArray list;
    /** The chosen model, or the LIBRARY's chosen row; -1 for none. */
    int chosen = -1;
    /** A model row the bar marks in place of the chosen one. */
    int highlight = -1;
    String joinLabel = "JOIN";
    bool leaveEnabled = false;
    std::vector<PeerRow> peers;
    /** What CREATE's entry opens with. */
    String groupName;
    // the LIBRARY
    /** The examples' rows: name, maker, role and height. */
    std::vector<juce::StringArray> rows;
    String place, banks;
    bool reuse = false;
    String review;
    /** The full sentences behind the page's short ones, read after its texts by the accessible value. */
    String spoken;

    bool operator== (const ScreenShown&) const = default;
};

/** A side unit's screen: the page it was last shown, drawn from elements.json's template on the plate's clean glass.
    The rows, fields and buttons on the page are its hits, each with the command or the entry its unit takes. It
    reports a press or a committed entry and leaves the deciding to its unit, which shows it the result. */
class Screen final : public juce::Component,
                     public juce::TooltipClient
{
public:
    struct Hit
    {
        Rect r;
        /** Empty for a row that only shows (a proposal's change). */
        String command;
        int value = -1;
        std::optional<int> back;
        /** A text entry's command: the click opens an editor over `entryRect` (else the hit's rect) instead. */
        String entry;
        bool multiLine = false;
        /** The row of a list the wheel and the cursor scroll; -1 off a list. */
        int row = -1;
        Rect entryRect;
        String tip;
        /** The item and its value, as the accessible description reads it under the cursor. */
        String name;
    };

    Screen (Context& c, const Element& e);
    ~Screen() override;

    /** A page change closes an open entry without committing it. */
    void show (const ScreenShown& s);

    void paint (juce::Graphics& g) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void focusGained (FocusChangeType cause) override;
    void focusLost (FocusChangeType) override;
    String getTooltip() override { return hoverTip; }

    /** A press: a row's index, an option's next index (its previous for `back`), or PLACE AS's step of 1 or -1. */
    std::function<void (const String& command, int value)> onCommand;
    /** A committed entry, trimmed but for the prompt's. */
    std::function<void (const String& command, const String& text)> onEntry;
    /** Escape with no entry open on a page that leaves for the mode's page. */
    std::function<void()> onEscape;

    const Hit* hitAt (Pt p) const;
    /** A hit by its command (or entry) and its value or row, for the tests. */
    const Hit* hitFor (const String& command, int value = -1) const;
    /** A click on the hit: its entry opens, or its command is reported. */
    void act (const Hit& h, bool back);
    void openEntry (const Hit& h);
    /** The keyboard's cursor, an index into `hits`; -1 for none. */
    int cursor() const { return cursorIndex; }
    int scrollOf (const String& p) const { return scroll.count (p) ? scroll.at (p) : 0; }
    /** The page and its texts in reading order, unelided. */
    String accessibleText() const;
    /** The cursor's item and its value. */
    String cursorName() const;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    const Element& element;
    ScreenShown shown;
    /** The page shown, and every text on it by slot (for the tests and accessibility). */
    String page;
    std::map<String, ScreenText> texts;
    juce::StringArray order;
    std::set<String> centred;
    std::vector<Hit> hits;
    /** Over an item while its text is written; it takes the item's entry command as its component ID. */
    juce::TextEditor entry;

private:
    const juce::var& pages() const { return ctx.model.screens["pages"]; }
    const juce::var& furniture() const { return ctx.model.screens["furniture"]; }
    float minH() const;
    float glassW() const { return (float) getWidth(); }
    float right() const { return glassW() - 18.0f; }
    juce::Colour ink() const;
    juce::Colour dim() const;
    juce::Colour barInk() const;
    int top() const;
    int optionIndex (const String& bind) const;
    /** The account, alert, linked, join and MCP pages, which Escape leaves; the model and join lists' bar is the cursor. */
    bool leavable() const { return page != "create" && page != "proposal" && page != "library"; }
    bool barList() const { return page == "connected" || page == "join"; }

    void addHit (Hit h) { hits.push_back (std::move (h)); }
    String entryValue (const String& command) const;
    void commitEntry();
    void closeEntry();

    const Hit* cursorHit() const;
    /** Up or Down through the items; at a list's edge the list scrolls while it can. */
    void moveCursor (int step);
    /** Left or Right to the neighbouring item on the same line. */
    void moveAlongRow (int step);
    void cursorMoved();
    void setHover (int row);
    /** Draws the cursor's change: the bar is in the drawing, the ring over it. */
    void redrawCursor();

    const ScreenText& put (const String& key, const String& style, const String& text, float x, float baseline, float maxRight,
                           std::optional<juce::Colour> colour = {}, std::optional<float> centreX = {});
    /** A template slot: x, baseline, style and an optional max_right. */
    const ScreenText& put (const String& key, const juce::var& slot, const String& text, std::optional<juce::Colour> colour = {});
    void rules (const juce::var& ys);
    void bar (Rect r);
    void outline (Rect r, bool small, bool enabled);
    void checkbox (Pt topLeft, bool ticked);
    void arrow (float x, float y, juce::Colour colour);
    void lock (Pt c);
    /** An outlined button with its legend centred; dim and inert when disabled. */
    const ScreenText& button (const String& key, Rect r, const String& text, const String& style, bool small, bool enabled);
    /** Lines that fit `width` once squeezed to the style's narrowest; setText squeezes each into place. */
    juce::StringArray wrap (const String& text, const TextStyle& st, float width) const;
    /** The prompt in its box: wrapped, the last lines when it runs longer (the caret ends it). */
    void prompt (const juce::var& slot);
    /** The musical context: the slot's fields at its x and binds, at a baseline; a click edits a field. */
    void context (const juce::var& slot, float baseline);
    void status (const juce::var& slot);

    void render();
    void proposalPage();
    void changeRows (const juce::var& C);
    void createPage();
    void alertPage();
    /** The account page's heading, sentences and large button, which presses `command`; the MCP page is drawn on it. */
    void signedOutPage (const String& command);
    /** The account's model list, or the join page's discovered groups on the same template. */
    void listPage();
    void linkedPage();
    void libraryPage();

    Context& ctx;
    Faces faces;
    juce::Image image;
    float scale = 1.0f;
    juce::Graphics* gfx = nullptr;
    /** Rows scrolled, until the page, the mode or the LIBRARY's tab changes. */
    std::map<String, int> scroll;
    String scrollKey;
    int maxScroll = 0, hover = -1, cursorIndex = -1;
    String entryCommand, hoverTip;
    /** The cursor is drawn once the focus has come from the keyboard or a key has moved it, until a click. */
    bool cursorShown = false, popup = false;
};
} // namespace airack::studio

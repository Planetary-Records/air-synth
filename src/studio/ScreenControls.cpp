#include "ScreenControls.h"

#include <array>

namespace airack::studio
{
static float num (const juce::var& v) { return (float) (double) v; }
static juce::Colour grey (int level) { return juce::Colour::greyLevel ((float) level / 255.0f); }
static juce::Colour fromBgr (const juce::var& v) { return juce::Colour ((juce::uint8) (int) v[2], (juce::uint8) (int) v[1], (juce::uint8) (int) v[0]); }
static Rect rectOf (const juce::var& v) { return { num (v[0]), num (v[1]), num (v[2]), num (v[3]) }; }
static bool isPress (const juce::KeyPress& key)
{
    return key.getKeyCode() == juce::KeyPress::spaceKey || key.getKeyCode() == juce::KeyPress::returnKey;
}

//============================================================================== lettering

TextStyle styleOf (const juce::var& screens, const String& name)
{
    const auto s = screens["styles"][juce::Identifier (name)];
    jassert (s.isObject()); // a style the skin's screens do not have
    const auto hscale = num (s.getProperty ("hscale", 1.0));
    return { s["font"].toString() == "screen", num (s.getProperty ("cap", 12.0)), num (s.getProperty ("tracking", 0.0)), hscale,
             (int) s.getProperty ("grey", -1), num (s.getProperty ("narrowest", hscale)) };
}

juce::Path styledPath (const Faces& f, const TextStyle& st, const String& text, float hscale)
{
    juce::GlyphArrangement ga;
    ga.addLineOfText (f.of (st).fontForCap (st.cap), text, 0.0f, 0.0f);
    for (int i = 0; i < ga.getNumGlyphs(); ++i)
        ga.getGlyph (i).moveBy ((float) i * st.tracking, 0.0f);
    juce::Path p;
    ga.createPath (p);
    p.applyTransform (juce::AffineTransform::scale (hscale, 1.0f));
    return p;
}

ScreenText setText (const Faces& f, const TextStyle& st, const String& text, float x, float baseline, float maxRight, float minH,
                    std::optional<float> centreX)
{
    ScreenText out { {}, text, text, st.hscale };
    if (text.trim().isEmpty())
        return out;
    auto p = styledPath (f, st, text, st.hscale);
    auto b = p.getBounds();
    const auto avail = centreX ? 2.0f * std::min (*centreX - x, maxRight - *centreX) : maxRight - x;
    // A squeeze to exactly the room left can land a rounding error over it.
    const auto tooWide = [avail] (Rect r) { return r.getWidth() > avail + 0.01f; };
    if (maxRight > 0.0f && tooWide (b))
    {
        out.hscale = std::max (minH, st.hscale * avail / b.getWidth());
        p = styledPath (f, st, text, out.hscale);
        b = p.getBounds();
        for (auto keep = text.length() - 1; tooWide (b) && keep > 0; --keep)
        {
            out.shown = text.substring (0, keep).trimEnd() + String::charToString ((juce::juce_wchar) 0x2026);
            p = styledPath (f, st, out.shown, out.hscale);
            b = p.getBounds();
        }
    }
    const auto dx = centreX ? *centreX - b.getCentreX() : x - b.getX();
    p.applyTransform (juce::AffineTransform::translation (dx, baseline));
    out.path = p;
    return out;
}

//============================================================================== Target

Target::Target (Context& c, const Element& e) : juce::Button (e.label), element (e), ctx (c)
{
    setWantsKeyboardFocus (true);
    setTitle (e.label);
    auto r = e.hitRect;
    for (const auto& [role, key] : e.layers)
    {
        const auto& a = ctx.model.asset (key);
        r = r.getUnion (Rect ((float) a.image.getWidth(), (float) a.image.getHeight()).withPosition (e.led - a.anchor));
    }
    setBounds (r.getSmallestIntegerContainer());
}

void Target::show (const String& layer, const String& state)
{
    if (layer == lit && state == stateText)
        return;
    lit = layer;
    stateText = state;
    repaint();
}

void Target::paintButton (juce::Graphics& g, bool, bool)
{
    const auto o = getPosition().toFloat();
    drawHitArea (g, ctx, element.hitRect - o);
    if (lit.isNotEmpty())
        drawSprite (g, ctx.model.asset (element.layer (lit)), element.led - o);
    if (hover || pressed)
    {
        g.setColour (juce::Colours::white.withAlpha (pressed ? 0.10f : 0.05f));
        g.fillRoundedRectangle (element.legend.expanded (4.0f, 3.0f) - o, 3.0f);
    }
    if (focusFromKeyboard && hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (element.hitRect.reduced (1.0f) - o, 4.0f, 2.0f);
    }
}

bool Target::hitTest (int x, int y) { return element.hitRect.contains (Pt ((float) x, (float) y) + getPosition().toFloat()); }
void Target::mouseEnter (const juce::MouseEvent&) { hover = true; repaint(); }
void Target::mouseExit (const juce::MouseEvent&) { hover = false; repaint(); }
void Target::mouseDown (const juce::MouseEvent&) { pressed = true; repaint(); }

void Target::mouseUp (const juce::MouseEvent& e)
{
    const auto activate = pressed && hitTest (juce::roundToInt (e.position.x), juce::roundToInt (e.position.y));
    pressed = false;
    repaint();
    if (activate)
        trigger();
}

bool Target::keyPressed (const juce::KeyPress& key)
{
    if (! isPress (key))
        return false;
    trigger();
    return true;
}

void Target::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
void Target::focusLost (FocusChangeType) { focusFromKeyboard = false; repaint(); }

void Target::trigger()
{
    if (isEnabled() && onClick)
        onClick();
}

namespace
{
class TargetAccessibility final : public juce::AccessibilityHandler
{
public:
    explicit TargetAccessibility (Target& t)
        : AccessibilityHandler (t, juce::AccessibilityRole::button,
                                juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [&t] { t.trigger(); })),
          owner (t) {}

    String getTitle() const override { return owner.element.label; }
    String getDescription() const override { return owner.state(); }

private:
    Target& owner;
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> Target::createAccessibilityHandler() { return std::make_unique<TargetAccessibility> (*this); }

//============================================================================== Readout

Readout::Readout (Context& c, const Element& e) : juce::Button (e.label), element (e), ctx (c), faces { c.screenLettering, c.lettering }
{
    setTitle (e.label);
    setWantsKeyboardFocus (true);
    setBounds (e.glass.getSmallestIntegerContainer());
}

float Readout::centreX() const { return (float) getWidth() / 2.0f + num (ctx.model.screens["styles"]["model"]["centre_dx"]); }

void Readout::show (const String& t)
{
    if (t == text.text && ! text.path.isEmpty())
        return;
    const auto margin = 8.0f;
    text = setText (faces, styleOf (ctx.model.screens, "model"), t, margin, num (ctx.model.screens["styles"]["model"]["baseline"]),
                    (float) getWidth() - margin, num (ctx.model.screens["min_hscale"]), centreX());
    repaint();
}

void Readout::paintButton (juce::Graphics& g, bool, bool)
{
    g.setColour (fromBgr (ctx.model.screens["furniture"]["ink"]["model_bgr"]));
    g.fillPath (text.path);
    drawHitArea (g, ctx, getLocalBounds().toFloat());
    if (focusFromKeyboard && hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 2.0f);
    }
}

void Readout::mouseDown (const juce::MouseEvent&) { pressed = true; }

void Readout::mouseUp (const juce::MouseEvent& e)
{
    const auto activate = pressed && getLocalBounds().contains (e.getPosition());
    pressed = false;
    if (activate)
        trigger();
}

bool Readout::keyPressed (const juce::KeyPress& key)
{
    if (! isPress (key))
        return false;
    trigger();
    return true;
}

void Readout::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
void Readout::focusLost (FocusChangeType) { focusFromKeyboard = false; repaint(); }

void Readout::trigger()
{
    if (isEnabled() && onClick)
        onClick();
}

namespace
{
class ReadoutAccessibility final : public juce::AccessibilityHandler
{
public:
    explicit ReadoutAccessibility (Readout& r)
        : AccessibilityHandler (r, juce::AccessibilityRole::button,
                                juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [&r] { r.trigger(); })),
          owner (r) {}

    String getTitle() const override { return owner.element.label; }
    String getDescription() const override { return owner.text.text; }

private:
    Readout& owner;
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> Readout::createAccessibilityHandler() { return std::make_unique<ReadoutAccessibility> (*this); }

//============================================================================== Screen

Screen::Screen (Context& c, const Element& e) : element (e), ctx (c), faces { c.screenLettering, c.lettering }
{
    setTitle (e.label);
    setWantsKeyboardFocus (true);
    setBounds (e.glass.getSmallestIntegerContainer());
    addChildComponent (entry);
    entry.setComponentID (e.id + ".entry");
    entry.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff060708));
    entry.setColour (juce::TextEditor::outlineColourId, accent);
    entry.setColour (juce::TextEditor::focusedOutlineColourId, accent);
    entry.setColour (juce::TextEditor::textColourId, ink());
    entry.setColour (juce::CaretComponent::caretColourId, ink());
    entry.onReturnKey = [this] { if (! entry.isMultiLine()) commitEntry(); };
    entry.onEscapeKey = [this] { closeEntry(); };
    entry.onFocusLost = [this] { commitEntry(); };
    render();
}

Screen::~Screen()
{
    entry.onReturnKey = nullptr;
    entry.onEscapeKey = nullptr;
    entry.onFocusLost = nullptr;
    entryCommand.clear();
}

void Screen::show (const ScreenShown& s)
{
    if (s.page != scrollKey)
    {
        closeEntry();
        hover = -1;
        cursorIndex = cursorShown && hasKeyboardFocus (true) ? 0 : -1;
    }
    shown = s;
    render();
    cursorIndex = juce::jmin (cursorIndex, (int) hits.size() - 1);
    repaint();
}

void Screen::paint (juce::Graphics& g)
{
    if (const auto s = renderScaleFor (g); s != scale)
    {
        scale = s;
        render();
    }
    g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale));
    if (ctx.showHitAreas)
        for (auto& h : hits)
            if (h.command.isNotEmpty() || h.entry.isNotEmpty())
                drawHitArea (g, ctx, h.r);
    if (! cursorShown || ! hasKeyboardFocus (false))
        return;
    g.setColour (accent);
    if (const auto* h = cursorHit())
    {
        if (! barList())
            g.drawRoundedRectangle (h->r, 4.0f, 2.0f);
    }
    else
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 2.0f);
}

void Screen::mouseMove (const juce::MouseEvent& e)
{
    const auto* h = hitAt (e.position);
    hoverTip = h != nullptr ? h->tip : String();
    // The status line may be cut to its slot; its full sentence, in its own case, is the tooltip.
    if (const auto line = texts.find ("status"); h == nullptr && line != texts.end() && line->second.ink().contains (e.position))
        hoverTip = shown.status;
    setHover (h != nullptr && barList() ? h->row : -1);
}

void Screen::mouseExit (const juce::MouseEvent&)
{
    hoverTip.clear();
    setHover (-1);
}

void Screen::mouseDown (const juce::MouseEvent& e)
{
    popup = e.mods.isPopupMenu();
    if (std::exchange (cursorShown, false))
        redrawCursor();
}

void Screen::mouseUp (const juce::MouseEvent& e)
{
    if (! e.mouseWasClicked())
        return;
    for (size_t i = 0; i < hits.size(); ++i)
        if (hits[i].r.contains (e.position))
        {
            cursorIndex = (int) i;
            act (hits[i], popup);
            return;
        }
}

void Screen::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto step = w.deltaY < 0.0f ? 1 : w.deltaY > 0.0f ? -1 : 0;
    if (step == 0 || maxScroll == 0)
        return Component::mouseWheelMove (e, w);
    scroll[page] = juce::jlimit (0, maxScroll, top() + step);
    render();
    repaint();
}

bool Screen::keyPressed (const juce::KeyPress& key)
{
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
    {
        moveCursor (code == juce::KeyPress::downKey ? 1 : -1);
        return true;
    }
    const auto actOnCursor = [this] (bool back)
    {
        // Copied: drawing the bar renders the page again, which replaces the hits.
        const auto hit = *cursorHit();
        if (! std::exchange (cursorShown, true))
            redrawCursor();
        act (hit, back);
    };
    if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
    {
        const auto on = code == juce::KeyPress::rightKey;
        if (const auto* h = cursorHit(); h != nullptr && h->back)
            actOnCursor (! on);
        else
            moveAlongRow (on ? 1 : -1);
        return true;
    }
    if (isPress (key))
    {
        if (cursorHit() != nullptr)
            actOnCursor (false);
        return true;
    }
    if (code == juce::KeyPress::escapeKey && leavable())
    {
        if (onEscape)
            onEscape();
        return true;
    }
    return false;
}

void Screen::focusGained (FocusChangeType cause)
{
    if (cause == focusChangedByTabKey)
    {
        cursorShown = true;
        if (cursorHit() == nullptr)
            cursorIndex = hits.empty() ? -1 : 0;
    }
    redrawCursor();
}

void Screen::focusLost (FocusChangeType) { redrawCursor(); }

void Screen::act (const Hit& h, bool back)
{
    // A report can show the screen again, which replaces the hits.
    const auto hit = h;
    if (hit.entry.isNotEmpty())
        return openEntry (hit);
    if (hit.command.isNotEmpty() && onCommand)
        onCommand (hit.command, back && hit.back ? *hit.back : hit.value);
}

void Screen::openEntry (const Hit& h)
{
    static const std::map<String, String> titles { { "ai.prompt", "Prompt" }, { "ai.key", "Key" }, { "ai.scale", "Scale" },
                                                   { "ai.role", "Role" }, { "ai.tempo", "Tempo" },
                                                   { "group.name", "Group name" }, { "library.allocation", "Banks" } };
    entryCommand = h.entry;
    entry.setComponentID (h.entry);
    entry.setTitle (titles.count (h.entry) ? titles.at (h.entry) : h.entry);
    entry.setMultiLine (h.multiLine, true);
    entry.setReturnKeyStartsNewLine (h.multiLine);
    const auto st = styleOf (ctx.model.screens, h.multiLine ? "body" : "label");
    entry.setFont (faces.of (st).fontForCap (st.cap));
    entry.setText (entryValue (h.entry), false);
    entry.setBounds ((h.entryRect.isEmpty() ? h.r : h.entryRect).getSmallestIntegerContainer());
    entry.setVisible (true);
    entry.grabKeyboardFocus();
    entry.moveCaretToEnd();
}

String Screen::entryValue (const String& command) const
{
    const auto field = [this] (const String& label)
    {
        for (const auto& [l, v] : shown.context)
            if (l == label)
                return v;
        return String();
    };
    if (command == "ai.prompt") return shown.prompt;
    if (command == "ai.key") return field ("KEY");
    if (command == "ai.scale") return field ("SCALE");
    if (command == "ai.role") return field ("ROLE");
    if (command == "ai.tempo") return field ("BPM");
    if (command == "group.name") return shown.groupName;
    if (command == "library.allocation") return shown.banks;
    return {};
}

void Screen::commitEntry()
{
    if (entryCommand.isEmpty())
        return;
    const auto command = entryCommand;
    const auto text = entry.isMultiLine() ? entry.getText() : entry.getText().trim();
    closeEntry();
    if (onEntry)
        onEntry (command, text);
}

void Screen::closeEntry()
{
    // Cleared first: hiding the focused entry loses its focus, which commits.
    entryCommand.clear();
    entry.setVisible (false);
    entry.setComponentID (element.id + ".entry");
}

const Screen::Hit* Screen::cursorHit() const
{
    return juce::isPositiveAndBelow (cursorIndex, (int) hits.size()) ? &hits[(size_t) cursorIndex] : nullptr;
}

void Screen::moveCursor (int step)
{
    const auto n = (int) hits.size();
    if (n == 0)
        return;
    cursorShown = true;
    if (cursorHit() == nullptr)
        cursorIndex = step > 0 ? 0 : n - 1;
    else
    {
        const auto next = cursorIndex + step;
        const auto leavesList = hits[(size_t) cursorIndex].row >= 0 && (! juce::isPositiveAndBelow (next, n) || hits[(size_t) next].row < 0);
        if (leavesList && juce::isPositiveAndNotGreaterThan (top() + step, maxScroll))
        {
            scroll[page] = top() + step;
            render();
        }
        else if (juce::isPositiveAndBelow (next, n))
            cursorIndex = next;
    }
    cursorMoved();
}

void Screen::moveAlongRow (int step)
{
    const auto* h = cursorHit();
    if (h == nullptr)
        return moveCursor (step);
    const auto next = cursorIndex + step;
    if (! juce::isPositiveAndBelow (next, (int) hits.size()))
        return;
    const auto& r = hits[(size_t) next].r;
    if (std::abs (r.getCentreY() - h->r.getCentreY()) >= 0.5f * juce::jmin (r.getHeight(), h->r.getHeight()))
        return;
    cursorShown = true;
    cursorIndex = next;
    cursorMoved();
}

void Screen::cursorMoved()
{
    redrawCursor();
    if (auto* a = getAccessibilityHandler())
        a->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

void Screen::setHover (int row)
{
    if (row == hover)
        return;
    hover = row;
    render();
    repaint();
}

void Screen::redrawCursor()
{
    if (barList())
        render();
    repaint();
}

String Screen::cursorName() const
{
    const auto* h = cursorHit();
    return h != nullptr ? h->name : String();
}

const Screen::Hit* Screen::hitAt (Pt p) const
{
    for (auto& h : hits)
        if (h.r.contains (p))
            return &h;
    return nullptr;
}

const Screen::Hit* Screen::hitFor (const String& command, int value) const
{
    for (auto& h : hits)
        if ((h.command == command || h.entry == command) && (value < 0 || h.value == value || h.row == value))
            return &h;
    return nullptr;
}

String Screen::accessibleText() const
{
    juce::StringArray parts { page };
    for (auto& key : order)
        parts.add (texts.at (key).text);
    if (shown.spoken.isNotEmpty())
        parts.add (shown.spoken);
    return parts.joinIntoString (". ");
}

namespace
{
class ScreenAccessibility final : public juce::AccessibilityHandler
{
public:
    explicit ScreenAccessibility (Screen& s)
        : AccessibilityHandler (s, juce::AccessibilityRole::staticText, juce::AccessibilityActions(), Interfaces { std::make_unique<Value> (s) }),
          owner (s) {}

    String getDescription() const override { return owner.cursorName(); }

private:
    Screen& owner;

    struct Value final : public juce::AccessibilityTextValueInterface
    {
        explicit Value (Screen& s) : owner (s) {}
        bool isReadOnly() const override { return true; }
        String getCurrentValueAsString() const override { return owner.accessibleText(); }
        void setValueAsString (const String&) override {}
        Screen& owner;
    };
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> Screen::createAccessibilityHandler() { return std::make_unique<ScreenAccessibility> (*this); }

float Screen::minH() const { return num (ctx.model.screens["min_hscale"]); }
juce::Colour Screen::ink() const { return fromBgr (furniture()["ink"][element.view == "library" ? "library_bgr" : "screen_bgr"]); }
juce::Colour Screen::dim() const { return grey ((int) ctx.model.screens["styles"]["fine"]["grey"]); }
juce::Colour Screen::barInk() const { return fromBgr (furniture()["bar"]["ink_bgr"]); }
int Screen::top() const { return juce::jlimit (0, maxScroll, scroll.count (page) ? scroll.at (page) : 0); }
int Screen::optionIndex (const String& bind) const { return shown.options.count (bind) ? shown.options.at (bind) : 0; }

// drawing -------------------------------------------------------------------------------------------------------

const ScreenText& Screen::put (const String& key, const String& style, const String& text, float x, float baseline, float maxRight,
                               std::optional<juce::Colour> colour, std::optional<float> centreX)
{
    const auto st = styleOf (ctx.model.screens, style);
    auto t = setText (faces, st, text, x, baseline, maxRight > 0.0f ? maxRight : right(), minH(), centreX);
    gfx->setColour (colour ? *colour : st.grey >= 0 ? grey (st.grey) : ink());
    gfx->fillPath (t.path);
    if (! texts.count (key))
        order.add (key);
    if (centreX)
        centred.insert (key);
    return texts[key] = t;
}

const ScreenText& Screen::put (const String& key, const juce::var& slot, const String& text, std::optional<juce::Colour> colour)
{
    return put (key, slot["style"].toString(), text, num (slot["x"]), num (slot["baseline"]), num (slot.getProperty ("max_right", 0.0)), colour);
}

void Screen::rules (const juce::var& ys)
{
    const auto r = furniture()["rule"];
    const auto t = num (r["thickness"]);
    gfx->setColour (grey ((int) r["grey"]));
    for (auto& y : *ys.getArray())
        gfx->fillRect (Rect (16.0f, num (y) - t / 2.0f, glassW() - 32.0f, t));
}

void Screen::bar (Rect r)
{
    gfx->setColour (grey ((int) furniture()["bar"]["fill"]));
    gfx->fillRect (r);
}

void Screen::outline (Rect r, bool small, bool enabled)
{
    const auto o = furniture()[small ? "small_outline" : "outline"];
    const auto t = num (o["thickness"]);
    gfx->setColour (enabled ? grey ((int) o["grey"]) : dim().withMultipliedBrightness (0.6f));
    gfx->drawRoundedRectangle (r.reduced (t / 2.0f), num (o["radius"]), t);
}

void Screen::checkbox (Pt topLeft, bool ticked)
{
    const auto c = furniture()["checkbox"];
    const auto size = num (c["size"]), t = num (c["thickness"]);
    const Rect r (topLeft.x, topLeft.y, size, size);
    gfx->setColour (grey ((int) c["grey"]));
    gfx->drawRoundedRectangle (r.reduced (t / 2.0f), num (c["radius"]), t);
    if (! ticked)
        return;
    juce::Path p;
    const auto* pts = c["tick"].getArray();
    for (int i = 0; i < pts->size(); ++i)
    {
        const Pt q (r.getX() + num ((*pts)[i][0]) * size, r.getY() + num ((*pts)[i][1]) * size);
        if (i == 0)
            p.startNewSubPath (q);
        else
            p.lineTo (q);
    }
    gfx->strokePath (p, juce::PathStrokeType (num (c["tick_thickness"]), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void Screen::arrow (float x, float y, juce::Colour colour)
{
    const auto a = furniture()["arrow"];
    const auto w = num (a["width"]), h = num (a["height"]);
    juce::Path p;
    p.addTriangle (x, y, x + w, y + h / 2.0f, x, y + h);
    gfx->setColour (colour);
    gfx->fillPath (p);
}

void Screen::lock (Pt c)
{
    const auto l = furniture()["lock"];
    const auto r = num (l["r"]);
    gfx->setColour (fromBgr (l["bgr"]));
    gfx->fillEllipse (c.x - r, c.y - r, 2.0f * r, 2.0f * r);
}

const ScreenText& Screen::button (const String& key, Rect r, const String& text, const String& style, bool small, bool enabled)
{
    outline (r, small, enabled);
    const auto st = styleOf (ctx.model.screens, style);
    return put (key, style, text, r.getX() + 4.0f, r.getCentreY() + st.cap / 2.0f, r.getRight() - 4.0f, enabled ? ink() : dim(), r.getCentreX());
}

juce::StringArray Screen::wrap (const String& text, const TextStyle& st, float width) const
{
    juce::StringArray out;
    for (const auto& para : juce::StringArray::fromLines (text))
    {
        String line;
        for (const auto& word : juce::StringArray::fromTokens (para, " ", ""))
        {
            const auto trial = line.isEmpty() ? word : line + " " + word;
            if (line.isNotEmpty() && styledPath (faces, st, trial, st.narrowest).getBounds().getWidth() > width)
            {
                out.add (line);
                line = word;
            }
            else
                line = trial;
        }
        out.add (line);
    }
    return out;
}

void Screen::prompt (const juce::var& slot)
{
    const auto st = styleOf (ctx.model.screens, "body");
    const auto x = num (slot["x"]), maxR = num (slot["max_right"]);
    const auto* bases = slot["baselines"].getArray();
    const auto empty = shown.prompt.trim().isEmpty();
    auto lines = wrap (empty ? String ("Describe a sound or a change.") : shown.prompt, st, maxR - x);
    while (lines.size() > bases->size())
        lines.remove (0);
    for (int i = 0; i < lines.size(); ++i)
        put ("prompt." + String (i), "body", lines[i], x, num ((*bases)[i]), maxR, empty ? std::optional<juce::Colour> (dim()) : std::nullopt);
    if (shown.promptFocus && ! empty)
    {
        const auto c = furniture()["caret"];
        const auto last = juce::jmax (0, lines.size() - 1);
        const auto b = num ((*bases)[last]);
        const auto cx = texts.at ("prompt." + String (last)).ink().getRight() + num (c["gap"]);
        gfx->setColour (ink());
        gfx->fillRect (Rect (cx, b - num (c["above"]), num (c["width"]), num (c["above"]) + num (c["below"])));
    }
    addHit ({ .r = Rect::leftTopRightBottom (x - 4.0f, num (slot["top"]), maxR, num (slot["bottom"])), .entry = "ai.prompt", .multiLine = true,
              .name = empty ? String ("PROMPT") : "PROMPT: " + shown.prompt });
}

void Screen::context (const juce::var& slot, float baseline)
{
    const auto* fields = slot["fields"].getArray();
    const auto* xs = slot["x"].getArray();
    const auto* binds = slot["binds"].getArray();
    for (int i = 0; i < fields->size(); ++i)
    {
        const auto f = (*fields)[i].toString();
        String v;
        for (const auto& [label, value] : shown.context)
            if (label == f)
                v = value;
        // In capitals as drawn; the entry opens with the text as written.
        const auto text = f == "BPM" ? (v.isEmpty() ? String ("HOST BPM") : v.toUpperCase() + " BPM") : v.isEmpty() ? f : f + " " + v.toUpperCase();
        const auto next = i + 1 < xs->size() ? num ((*xs)[i + 1]) - 8.0f : right();
        const auto& t = put ("context." + f, "small", text, num ((*xs)[i]), baseline, next);
        addHit ({ .r = t.ink().expanded (5.0f, 7.0f), .entry = (*binds)[i].toString(),
                  .entryRect = Rect::leftTopRightBottom (t.ink().getX() - 4.0f, baseline - 19.0f, next, baseline + 7.0f),
                  .name = v.isEmpty() ? text : f + ": " + v });
    }
}

void Screen::status (const juce::var& slot) { put ("status", slot, shown.status.toUpperCase()); }

// pages ---------------------------------------------------------------------------------------------------------

void Screen::render()
{
    image = scaledArea (ctx.model.plates.at (element.view), element.glass.getSmallestIntegerContainer(), scale);
    texts.clear();
    order.clear();
    centred.clear();
    hits.clear();
    maxScroll = 0;
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::scale (scale));
    gfx = &g;
    page = shown.page;
    if (const auto key = page; key != scrollKey)
    {
        scroll.clear();
        scrollKey = key;
    }
    if (page == "proposal") proposalPage();
    else if (page == "create") createPage();
    else if (page == "alert") alertPage();
    else if (page == "signed_out") signedOutPage ("ai.account");
    else if (page == "mcp")
    {
        signedOutPage ("mcp.access");
        status (pages()["create"]["status"]);
    }
    else if (page == "connected" || page == "join") listPage();
    else if (page == "linked") linkedPage();
    else if (page == "library") libraryPage();
    gfx = nullptr;
}

void Screen::proposalPage()
{
    const auto& s = shown;
    const auto P = pages()["proposal"];
    put ("header.title", P["header"]["label"], s.name.isNotEmpty() ? s.name.toUpperCase() : String ("CREATE SOUND"));
    rules (P["rules"]);
    prompt (P["prompt"]);
    context (P["context"], num (P["context"]["baseline"]));
    const auto T = P["title"], A = P["action"];
    if (s.action.isNotEmpty())
    {
        // The title with a small outlined button at its right, which the title keeps clear of.
        const auto w = num (A["cancel"]), b = num (T["baseline"]);
        const Rect r (num (A["right"]) - w, b - num (A["above"]), w, num (A["height"]));
        put ("title", T["style"].toString(), s.title, num (T["x"]), b, r.getX() - num (A["gap"]));
        button ("cancel", r, s.action, A["style"].toString(), true, true);
        addHit ({ .r = r, .command = "ai.cancel", .name = s.action });
    }
    else
        put ("title", T, s.title);
    changeRows (P["changes"]);
    status (P["status"]);
}

void Screen::changeRows (const juce::var& C)
{
    const auto first = num (C["first"]), pitch = num (C["pitch"]);
    const auto rows = (int) C["rows"], n = (int) shown.changes.size();
    maxScroll = juce::jmax (0, n - rows);
    const auto from = top();
    const auto arrowX = num (C["arrow"]["x"]);
    const auto lockR = num (furniture()["lock"]["r"]);
    for (int i = 0; i < rows && from + i < n; ++i)
    {
        const auto& c = shown.changes[(size_t) (from + i)];
        const auto b = first + (float) i * pitch;
        const auto key = "change." + String (i);
        put (key + ".name", "label", c.name, num (C["name"]), b, num (C["before"]) - 8.0f);
        put (key + ".before", "label", c.before, num (C["before"]), b, arrowX - 6.0f);
        if (! c.locked)
        {
            arrow (arrowX, b + num (C["arrow"]["top"]), ink());
            put (key + ".after", "label", c.after, num (C["after"]), b, right());
        }
        else
        {
            const auto cx = num (C["lock"]["cx"]);
            put (key + ".kept", "label", "KEPT: LOCKED", num (C["kept"]), b, cx - lockR - 6.0f);
            lock ({ cx, b + num (C["lock"]["dy"]) });
        }
        addHit ({ .r = Rect::leftTopRightBottom (16.0f, b - pitch + 7.0f, glassW() - 16.0f, b + 7.0f), .row = from + i,
                  .name = c.name + ": " + c.before + (c.locked ? String (", KEPT: LOCKED") : " TO " + c.after) });
    }
}

void Screen::createPage()
{
    const auto& s = shown;
    const auto P = pages()["create"];
    put ("title", P["title"], "CREATE SOUND");
    rules (P["rules"]);
    prompt (P["prompt"]);
    const auto O = P["options"];
    const auto* rows = O["sound"].getArray();
    const auto visible = (int) O["rows"];
    maxScroll = juce::jmax (0, rows->size() - visible);
    const auto from = top();
    const auto first = num (O["first"]), pitch = num (O["pitch"]);
    for (int i = 0; i < visible && from + i < rows->size(); ++i)
    {
        const auto row = (*rows)[from + i];
        const auto b = first + (float) i * pitch;
        const auto bind = row["binds"].toString();
        const auto* values = row["values"].getArray();
        const auto enabled = ! s.dim.contains (bind);
        const auto colour = enabled ? ink() : dim();
        const auto idx = juce::jlimit (0, values->size() - 1, optionIndex (bind));
        const auto value = (*values)[idx].toString();
        const auto key = "option." + String (i);
        put (key + ".label", "label", row["label"].toString(), num (O["label"]), b, num (O["value"]) - 8.0f, colour);
        put (key + ".value", "label", value, num (O["value"]), b, num (O["value_right"]), colour);
        arrow (num (O["arrow"]["x"]), b + num (O["arrow"]["top"]), colour);
        const auto r = Rect::leftTopRightBottom (16.0f, b - pitch + 7.0f, glassW() - 16.0f, b + 7.0f);
        if (! enabled)
            continue;
        const auto name = row["label"].toString() + ": " + value;
        if (row.hasProperty ("entry"))
            addHit ({ .r = r, .entry = bind, .row = from + i,
                      .entryRect = Rect::leftTopRightBottom (num (O["value"]) - 4.0f, b - 19.0f, num (O["value_right"]), b + 7.0f), .name = name });
        else
        {
            const auto n = values->size();
            addHit ({ .r = r, .command = bind, .value = (idx + 1) % n, .back = (idx + n - 1) % n, .row = from + i, .name = name });
        }
    }
    context (pages()["proposal"]["context"], num (P["context"]["baseline"]));
    status (P["status"]);
}

void Screen::alertPage()
{
    const auto P = pages()["alert"];
    const auto cx = glassW() / 2.0f;
    const auto H = P["heading"];
    put ("heading", "alert", shown.heading, 16.0f, num (H["baseline"]), glassW() - 16.0f, {}, cx + num (H["dx"]));
    const auto* body = P["body"].getArray();
    for (int i = 0; i < juce::jmin (body->size(), shown.body.size()); ++i)
    {
        const auto b = (*body)[i];
        put ("body." + String (i), "body", shown.body[i], 16.0f, num (b["baseline"]), glassW() - 16.0f, {}, cx + num (b["dx"]));
    }
    if (shown.button.isEmpty())
        return;
    const auto B = P["button"];
    button ("button", rectOf (B["rect"]), shown.button, B["style"].toString(), false, true);
    addHit ({ .r = rectOf (B["rect"]), .command = "ai.account", .name = shown.button });
}

void Screen::signedOutPage (const String& command)
{
    const auto P = pages()["signed_out"];
    put ("heading", P["heading"], shown.heading);
    rules (P["rules"]);
    const auto* slots = P["body"].getArray();
    for (int i = 0; i < juce::jmin (slots->size(), shown.body.size()); ++i)
        put ("body." + String (i), (*slots)[i], shown.body[i]);
    if (shown.button.isEmpty())
        return;
    const auto B = P["button"];
    button ("button", rectOf (B["rect"]), shown.button, B["style"].toString(), false, true);
    addHit ({ .r = rectOf (B["rect"]), .command = command, .name = shown.button });
}

void Screen::listPage()
{
    const auto& s = shown;
    const auto P = pages()["connected"];
    const auto join = page == "join";
    put ("heading", P["heading"], s.heading);
    rules (P["rules"]);
    put ("body.0", P["body"][0], s.body[0]);
    const auto M = P["models"];
    const auto rows = (int) M["rows"];
    maxScroll = juce::jmax (0, s.list.size() - rows);
    const auto from = top();
    // The rows are the page's only hits, so the cursor's index is its visible row.
    const auto keyed = cursorShown && cursorIndex >= 0 && hasKeyboardFocus (false) ? from + cursorIndex : -1;
    const auto marked = keyed >= 0 ? keyed : hover >= 0 ? hover : join ? -1 : s.highlight >= 0 ? s.highlight : s.chosen;
    for (int i = 0; i < rows && from + i < s.list.size(); ++i)
    {
        const auto k = from + i;
        const auto b = num (M["first"]) + (float) i * num (M["pitch"]);
        const Rect r (num (M["bar"]["x"]), b + num (M["bar"]["top"]), num (M["bar"]["w"]), num (M["bar"]["h"]));
        if (k == marked)
            bar (r);
        put ("row." + String (i), "label", s.list[k].toUpperCase(), num (M["x"]), b, r.getRight() - 8.0f,
             k == marked ? std::optional<juce::Colour> (barInk()) : std::nullopt);
        addHit ({ .r = r, .command = join ? "group.join" : "ai.model", .value = k, .row = k, .name = s.list[k].toUpperCase() });
    }
}

void Screen::linkedPage()
{
    const auto& s = shown;
    const auto P = pages()["linked"];
    const auto H = P["heading"];
    put ("heading", H, s.heading);
    rules (P["rules"]);
    put ("body.0", P["body"][0], s.body[0]);
    const auto headingRect = Rect::leftTopRightBottom (num (H["x"]) - 4.0f, num (H["baseline"]) - 26.0f, right(), num (H["baseline"]) + 8.0f);
    const auto* buttons = P["buttons"].getArray();
    for (int i = 0; i < buttons->size(); ++i)
    {
        const auto b = (*buttons)[i];
        const auto r = rectOf (b["rect"]);
        auto text = b["text"].toString();
        const auto enabled = text != "LEAVE" || s.leaveEnabled;
        if (text == "JOIN")
            text = s.joinLabel;
        button ("button." + String (i), r, text, b["style"].toString(), true, enabled);
        if (! enabled)
            continue;
        if (text == "CREATE")
            addHit ({ .r = r, .entry = "group.name", .entryRect = headingRect, .name = text });
        else
            addHit ({ .r = r,
                      .command = text == "JOIN" ? "ai.page.join" : text == "RESUME" ? "group.resume" : text == "REFRESH" ? "group.refresh" : "group.leave",
                      .name = text });
    }
    put ("peers.label", P["peers_label"], P["peers_label"]["text"].toString());
    const auto R = P["peers"];
    const auto rows = (int) R["rows"], n = (int) s.peers.size();
    maxScroll = juce::jmax (0, n - rows);
    const auto from = top();
    for (int i = 0; i < rows && from + i < n; ++i)
    {
        const auto& p = s.peers[(size_t) (from + i)];
        const auto y = num (R["first"]) + (float) i * num (R["pitch"]);
        const auto key = "peer." + String (i);
        checkbox ({ num (R["box"]["x"]), y }, p.selected);
        put (key + ".role", "label", p.role, num (R["role"]["x"]), y + num (R["role"]["dy"]), num (R["source"]["x"]) - 8.0f);
        put (key + ".source", "label", p.source, num (R["source"]["x"]), y + num (R["source"]["dy"]), right());
        put (key + ".state", "fine", p.state, num (R["status"]["x"]), y + num (R["status"]["dy"]), right());
        addHit ({ .r = Rect::leftTopRightBottom (16.0f, y - 4.0f, glassW() - 16.0f, y + num (R["pitch"]) - 6.0f), .command = "group.peer",
                  .value = from + i, .row = from + i, .tip = p.tip, .name = p.role + ": " + p.source + ", " + p.state });
    }
}

void Screen::libraryPage()
{
    const auto& s = shown;
    const auto P = pages()["library"];
    const auto L = P["list"];
    const auto first = num (L["first"]), pitch = num (L["pitch"]);
    const auto C = L["columns"];
    const std::array<float, 4> cols { num (C["name"]), num (C["maker"]), num (C["role"]), num (C["height"]) };
    const auto sep = furniture()["separator"];
    const auto count = (int) s.rows.size(), rows = (int) L["rows"];
    maxScroll = juce::jmax (0, count - rows);
    const auto from = top();
    // The text keys are places on the glass; the chosen bar, the separators and the hits go by the list's index.
    for (int i = 0; i < rows && from + i < count; ++i)
    {
        const auto k = from + i;
        const auto& cells = s.rows[(size_t) k];
        const auto chosen = k == s.chosen;
        const auto b = first + (float) i * pitch;
        const Rect r (num (L["bar"]["x"]), b + num (L["bar"]["top"]), num (L["bar"]["w"]), num (L["bar"]["h"]));
        if (chosen)
            bar (r);
        const auto colour = chosen ? barInk() : ink();
        for (int c = 0; c < cells.size() && c < (int) cols.size(); ++c)
        {
            const auto limit = c + 1 < (int) cols.size() ? cols[(size_t) c + 1] - 8.0f : r.getRight() - 8.0f;
            put ("list." + String (i) + "." + String (c), "label", cells[c], cols[(size_t) c], b, limit, colour);
        }
        if (i + 1 < rows && k + 1 < count && ! chosen && k + 1 != s.chosen)
        {
            const auto th = num (sep["thickness"]);
            gfx->setColour (grey ((int) sep["grey"]));
            gfx->fillRect (Rect::leftTopRightBottom (num (L["separator"]["x"][0]), b + num (L["separator"]["dy"]) - th / 2.0f,
                                                     num (L["separator"]["x"][1]), b + num (L["separator"]["dy"]) + th / 2.0f));
        }
        addHit ({ .r = r, .command = "library.example", .value = k, .row = k, .name = cells.joinIntoString (", ") });
    }
    // While the rows overflow, where they are in the list, ending at the slot's right.
    if (const auto PS = P["position"]; count > rows && PS.isObject())
    {
        const auto text = String (from + 1) + "-" + String (juce::jmin (count, from + rows)) + " OF " + String (count);
        const auto style = PS["style"].toString();
        const auto edge = num (PS["right"]), baseline = num (PS["baseline"]);
        const auto w = setText (faces, styleOf (ctx.model.screens, style), text, 0.0f, baseline, edge + 1000.0f, minH()).ink().getWidth();
        put ("position", style, text, edge - w, baseline, edge + 1.0f);
    }
    rules (P["rules"]);

    const auto PL = P["place"];
    put ("place.label", PL["label"], PL["label"]["text"].toString());
    const auto& v = put ("place.value", PL["value"], s.place);
    addHit ({ .r = Rect::leftTopRightBottom (16.0f, num (PL["label"]["baseline"]) - 20.0f, glassW() - 16.0f, v.ink().getBottom() + 8.0f),
              .command = "library.placement", .value = 1, .back = -1, .name = PL["label"]["text"].toString() + ": " + s.place });

    const auto BK = P["banks"];
    const auto labelBase = num (BK["label"]["baseline"]);
    put ("banks.label", BK["label"], BK["label"]["text"].toString());
    const auto& reuse = put ("banks.reuse", "label", s.reuse ? "REUSE ON" : "REUSE OFF", right() - 90.0f, labelBase, right(),
                             s.reuse ? std::optional<juce::Colour>() : std::optional<juce::Colour> (dim()));
    const auto& banks = put ("banks.value", "label", s.banks.isEmpty() ? String ("AUTOMATIC") : s.banks, num (BK["value"]["x"]),
                             num (BK["value"]["baseline"]), reuse.ink().getX() - 12.0f);
    addHit ({ .r = Rect::leftTopRightBottom (16.0f, labelBase - 20.0f, reuse.ink().getX() - 8.0f, labelBase + 9.0f), .entry = "library.allocation",
              .entryRect = Rect::leftTopRightBottom (banks.ink().getX() - 4.0f, labelBase - 19.0f, reuse.ink().getX() - 8.0f, labelBase + 7.0f),
              .name = BK["label"]["text"].toString() + ": " + banks.text });
    addHit ({ .r = reuse.ink().expanded (6.0f, 8.0f), .command = "library.reuse", .name = reuse.text });
    put ("review", P["review"], s.review.toUpperCase());
}
} // namespace airack::studio

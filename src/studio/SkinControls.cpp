#include "SkinControls.h"

#include <optional>

namespace airack::studio
{
void drawHitArea (juce::Graphics& g, const Context& ctx, Rect r)
{
    if (! ctx.showHitAreas)
        return;
    g.setColour (juce::Colours::cyan.withAlpha (0.22f));
    g.fillRect (r);
}

void drawFaceStates (juce::Graphics& g, Rect face, bool enabled, bool hover, bool pressed, bool focusRing, juce::Colour panel)
{
    if (! enabled)
    {
        g.setColour (panel.withAlpha (0.7f));
        g.fillRoundedRectangle (face.expanded (2.0f), 5.0f);
    }
    else if (pressed)
    {
        g.setColour (juce::Colours::black.withAlpha (0.30f));
        g.fillRoundedRectangle (face.reduced (2.5f), 3.5f);
    }
    else if (hover)
    {
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRoundedRectangle (face.reduced (2.5f), 3.5f);
    }
    if (focusRing)
    {
        g.setColour (accent);
        g.drawRoundedRectangle (face.expanded (3.0f), 5.0f, 2.0f);
    }
}

//============================================================================== SkinButton

SkinButton::SkinButton (Context& c, const Element& e, float litScaleIn) : juce::Button (e.label), element (e), litScale (litScaleIn), ctx (c)
{
    setWantsKeyboardFocus (true);
    setTitle (e.label);
    Rect r = e.rect.expanded (8.0f);
    if (e.layer ("lit").isNotEmpty())
    {
        const auto& a = ctx.model.asset (e.layer ("lit"));
        r = r.getUnion (Rect ((float) a.image.getWidth(), (float) a.image.getHeight()).withPosition (-a.anchor) * litScale + e.led);
    }
    setBounds (r.getSmallestIntegerContainer());
}

bool SkinButton::lit() const
{
    if (litWhen)
        return litWhen();
    return (isToggle() || isMode() || element.ledStatus.isNotEmpty()) && getToggleState();
}

void SkinButton::paintButton (juce::Graphics& g, bool, bool)
{
    const auto o = getPosition().toFloat();
    drawHitArea (g, ctx, element.rect.expanded (3.0f) - o);
    drawFaceStates (g, element.rect - o, isEnabled(), hover, pressed && pressInside, focusFromKeyboard && hasKeyboardFocus (false), juce::Colour (0xff2b2c2e));
    paintedLit = lit();
    if (paintedLit)
        drawSprite (g, ctx.model.asset (element.layer ("lit")), element.led - o, litScale);
}

void SkinButton::showLit()
{
    if (lit() != paintedLit)
        repaint();
}

bool SkinButton::hitTest (int x, int y) { return element.rect.expanded (3.0f).contains (Pt ((float) x, (float) y) + getPosition().toFloat()); }
void SkinButton::mouseEnter (const juce::MouseEvent&) { hover = true; repaint(); }
void SkinButton::mouseExit (const juce::MouseEvent&) { hover = false; repaint(); }

void SkinButton::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled())
        return;
    pressed = pressInside = true;
    dragArmed = onDragOut != nullptr && e.mods.isLeftButtonDown();
    repaint();
}

void SkinButton::mouseDrag (const juce::MouseEvent& e)
{
    if (pressed && dragArmed && e.mods.isLeftButtonDown() && e.getDistanceFromDragStart() >= 6)
    {
        pressed = pressInside = dragArmed = false;
        repaint();
        onDragOut();
        return;
    }
    const auto inside = hitTest (juce::roundToInt (e.position.x), juce::roundToInt (e.position.y));
    if (pressed && inside != pressInside)
    {
        pressInside = inside;
        repaint();
    }
}

void SkinButton::mouseUp (const juce::MouseEvent& e)
{
    const auto activate = pressed && hitTest (juce::roundToInt (e.position.x), juce::roundToInt (e.position.y));
    pressed = pressInside = dragArmed = false;
    repaint();
    if (activate)
        trigger();
}

bool SkinButton::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::spaceKey || key.getKeyCode() == juce::KeyPress::returnKey)
    {
        trigger();
        return true;
    }
    return false;
}

void SkinButton::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
void SkinButton::focusLost (FocusChangeType) { focusFromKeyboard = false; repaint(); }
void SkinButton::enablementChanged() { pressed = false; repaint(); }

void SkinButton::trigger()
{
    if (isEnabled() && onClick)
        onClick();
}

namespace
{
class ButtonAccessibility final : public juce::AccessibilityHandler
{
public:
    explicit ButtonAccessibility (SkinButton& b)
        : AccessibilityHandler (b, b.isToggle() || b.isMode() ? juce::AccessibilityRole::toggleButton : juce::AccessibilityRole::button,
                                juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [&b] { b.trigger(); })),
          button (b) {}

    juce::AccessibleState getCurrentState() const override
    {
        auto s = AccessibilityHandler::getCurrentState();
        if (button.isToggle() || button.isMode())
        {
            s = s.withCheckable();
            if (button.checked())
                s = s.withChecked();
        }
        return s;
    }

    String getTitle() const override { return button.element.label; }

private:
    SkinButton& button;
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> SkinButton::createAccessibilityHandler() { return std::make_unique<ButtonAccessibility> (*this); }

//============================================================================== Lever

Lever::Lever (Context& c, const Element& e) : juce::Button (e.label), element (e), ctx (c)
{
    setWantsKeyboardFocus (true);
    setTitle (e.label);
    setClickingTogglesState (true);
    auto r = e.hitRect;
    for (auto key : { "0", "1" })
    {
        const auto& a = ctx.model.asset (e.layer (key));
        r = r.getUnion (Rect ((float) a.image.getWidth(), (float) a.image.getHeight()).withPosition (e.pivot - a.anchor));
    }
    setBounds (r.getSmallestIntegerContainer());
}

void Lever::paintButton (juce::Graphics& g, bool, bool)
{
    const auto o = getPosition().toFloat();
    drawHitArea (g, ctx, element.hitRect - o);
    drawSprite (g, ctx.model.asset (element.layer (String (position()))), element.pivot - o);
    if (focusFromKeyboard && hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (element.footprint.expanded (4.0f) - o, 6.0f, 2.0f);
    }
}

bool Lever::hitTest (int x, int y) { return element.hitRect.contains (Pt ((float) x, (float) y) + getPosition().toFloat()); }
void Lever::mouseDown (const juce::MouseEvent&) { decided = false; }

void Lever::mouseDrag (const juce::MouseEvent& e)
{
    const auto dy = e.position.y - e.mouseDownPosition.y;
    if (! decided && std::abs (dy) >= 8.0f)
    {
        decided = true;
        set (dy > 0 ? 1 : 0);
    }
}

void Lever::mouseUp (const juce::MouseEvent& e)
{
    if (! decided && hitTest (juce::roundToInt (e.position.x), juce::roundToInt (e.position.y)))
        set (1 - position());
    decided = false;
}

bool Lever::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::upKey) { set (0); return true; }
    if (key.getKeyCode() == juce::KeyPress::downKey) { set (1); return true; }
    if (key.getKeyCode() == juce::KeyPress::spaceKey || key.getKeyCode() == juce::KeyPress::returnKey) { set (1 - position()); return true; }
    return false;
}

void Lever::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
void Lever::focusLost (FocusChangeType) { focusFromKeyboard = false; repaint(); }

void Lever::set (int p)
{
    if (p != position())
        setToggleState (p == 1, juce::sendNotificationSync);
}

namespace
{
class LeverAccessibility final : public juce::AccessibilityHandler
{
public:
    explicit LeverAccessibility (Lever& l)
        : AccessibilityHandler (l, juce::AccessibilityRole::toggleButton,
                                juce::AccessibilityActions().addAction (juce::AccessibilityActionType::toggle, [&l] { l.set (1 - l.position()); }),
                                Interfaces { std::make_unique<Value> (l) }),
          lever (l) {}

    juce::AccessibleState getCurrentState() const override
    {
        auto s = AccessibilityHandler::getCurrentState().withCheckable();
        return lever.position() == 1 ? s.withChecked() : s;
    }

    String getTitle() const override { return lever.element.label; }

private:
    struct Value final : public juce::AccessibilityTextValueInterface
    {
        explicit Value (Lever& l) : lever (l) {}
        bool isReadOnly() const override { return true; }
        String getCurrentValueAsString() const override { return lever.positionName(); }
        void setValueAsString (const String&) override {}
        Lever& lever;
    };

    Lever& lever;
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> Lever::createAccessibilityHandler() { return std::make_unique<LeverAccessibility> (*this); }

//============================================================================== Disclosure

Disclosure::Disclosure (Context& c, const Element& e) : element (e), ctx (c)
{
    setWantsKeyboardFocus (true);
    setTitle (e.label);
    setBounds (e.hitRect.expanded (4.0f).getSmallestIntegerContainer());
}

void Disclosure::paint (juce::Graphics& g)
{
    const auto o = getPosition().toFloat();
    drawHitArea (g, ctx, element.hitRect - o);
    drawFaceStates (g, element.hitRect - o, isEnabled(), hover, pressed, focusFromKeyboard && hasKeyboardFocus (false), juce::Colour (0xff2b2c2e));
}

bool Disclosure::hitTest (int x, int y) { return element.hitRect.contains (Pt ((float) x, (float) y) + getPosition().toFloat()); }
void Disclosure::mouseEnter (const juce::MouseEvent&) { hover = true; repaint(); }
void Disclosure::mouseExit (const juce::MouseEvent&) { hover = false; repaint(); }
void Disclosure::mouseDown (const juce::MouseEvent&) { pressed = true; repaint(); }

void Disclosure::mouseUp (const juce::MouseEvent& e)
{
    const auto activate = pressed && hitTest (juce::roundToInt (e.position.x), juce::roundToInt (e.position.y));
    pressed = false;
    repaint();
    if (activate)
        press();
}

bool Disclosure::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::spaceKey || key.getKeyCode() == juce::KeyPress::returnKey)
    {
        press();
        return true;
    }
    return false;
}

void Disclosure::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
void Disclosure::focusLost (FocusChangeType) { focusFromKeyboard = false; repaint(); }

void Disclosure::press()
{
    if (onPress)
        onPress();
}

void Disclosure::setExpanded (bool shouldBeExpanded)
{
    if (expanded == shouldBeExpanded)
        return;
    expanded = shouldBeExpanded;
    if (auto* h = getAccessibilityHandler())
        h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

std::unique_ptr<juce::AccessibilityHandler> Disclosure::createAccessibilityHandler()
{
    struct Handler final : public juce::AccessibilityHandler
    {
        explicit Handler (Disclosure& d)
            : AccessibilityHandler (d, juce::AccessibilityRole::button,
                                    juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [&d] { d.press(); })),
              owner (d) {}
        juce::AccessibleState getCurrentState() const override
        {
            auto s = AccessibilityHandler::getCurrentState().withExpandable();
            return owner.expanded ? s.withExpanded() : s.withCollapsed();
        }
        String getTitle() const override { return owner.element.label; }
        String getHelp() const override { return "Unfold or fold the pattern editor"; }
        Disclosure& owner;
    };
    return std::make_unique<Handler> (*this);
}

//============================================================================== Indicator

Indicator::Indicator (Context& c, const Element& e) : element (e), ctx (c)
{
    setInterceptsMouseClicks (false, false);
    const auto& a = ctx.model.asset (e.layer ("lit"));
    setBounds (Rect ((float) a.image.getWidth(), (float) a.image.getHeight()).withPosition (e.led - a.anchor).getSmallestIntegerContainer().expanded (1));
}

void Indicator::paint (juce::Graphics& g)
{
    if (lit)
        drawSprite (g, ctx.model.asset (element.layer ("lit")), element.led - getPosition().toFloat());
}

void Indicator::setLit (bool shouldBeLit)
{
    if (lit == shouldBeLit)
        return;
    lit = shouldBeLit;
    repaint();
}

//============================================================================== Display

Display::Display (Context& c, const Element& e)
    : element (e), ctx (c), spec (c.model.displays.count (e.view) > 0 ? c.model.displays.at (e.view) : c.model.displays.at (e.display)),
      kind (e.view == "session" ? Kind::session : e.view == "advanced" ? Kind::programmer : e.display == "pattern.note" ? Kind::note : Kind::strip),
      pathEntry (e.view == "session" || e.display == "pattern.settings")
{
    setTitle (e.label);
    setWantsKeyboardFocus (true);
    setBounds (e.glass.getSmallestIntegerContainer());
    if (pathEntry)
    {
        const auto preset = kind == Kind::session;
        addChildComponent (entry);
        entry.setComponentID (preset ? "preset.path" : "pattern.path");
        entry.setTitle (preset ? "Sound preset absolute path" : "Pattern absolute path");
        entry.setTextToShowWhenEmpty (preset ? "Sound preset path (.airack)" : "Pattern path (.aipattern, or .mid to export)", juce::Colours::grey);
        entry.setFont (juce::Font (juce::FontOptions (ctx.lettering.face).withHeight (18.0f)));
        entry.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff050606));
        entry.setColour (juce::TextEditor::textColourId, spec.lattice.ink);
        entry.setColour (juce::TextEditor::outlineColourId, accent);
        entry.onReturnKey = [this] { commitPath(); };
        entry.onEscapeKey = [this] { closeEntry(); };
        entry.onFocusLost = [this] { if (entry.isVisible()) commitPath(); };
    }
}

void Display::show (const SessionShown& s)
{
    const auto before = accessibleValue();
    session = s;
    showKey (s.layout + "|" + s.name + "|" + s.tempoKey + "|" + s.tempoText + "|" + s.headline + "|" + s.detail, before);
}

void Display::show (const StripShown& s)
{
    const auto before = accessibleValue();
    strip = s;
    showKey (s.layout + "|" + s.text + "|" + String ((int) s.amber), before);
}

void Display::show (const NoteShown& s)
{
    const auto before = accessibleValue();
    note = s;
    showKey (s.text + "|" + String ((int) s.selected), before);
}

void Display::show (const ProgrammerShown& s)
{
    const auto before = accessibleValue();
    programmer = s;
    showKey (s.bank + "|" + s.value, before);
}

void Display::showKey (const String& key, const String& accessibleBefore)
{
    if (key != shownKey)
    {
        shownKey = key;
        render();
        repaint();
    }
    if (accessibleValue() != accessibleBefore)
        if (auto* h = getAccessibilityHandler())
            h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

void Display::paint (juce::Graphics& g)
{
    if (const auto s = renderScaleFor (g); s != scale && image.isValid())
    {
        scale = s;
        render();
    }
    g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale));
    drawHitArea (g, ctx, getLocalBounds().toFloat());
    if (focusFromKeyboard && hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 2.0f);
    }
}

bool Display::keyPressed (const juce::KeyPress& key)
{
    if (pathEntry && key.getKeyCode() == juce::KeyPress::returnKey)
    {
        openEntry();
        return true;
    }
    return false;
}

void Display::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() && onContext)
        onContext();
}

void Display::mouseDoubleClick (const juce::MouseEvent&)
{
    if (pathEntry)
        openEntry();
}

void Display::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
void Display::focusLost (FocusChangeType) { focusFromKeyboard = false; repaint(); }
void Display::resized() { entry.setBounds (getLocalBounds().reduced (10, juce::jmin (16, getHeight() / 2 - 12))); }

void Display::openEntry()
{
    entry.setText (kind == Kind::session ? session.path : strip.path, false);
    entry.setVisible (true);
    entry.grabKeyboardFocus();
    entry.selectAll();
}

void Display::commitPath()
{
    const auto path = entry.getText().trim();
    entry.setVisible (false);
    if (onPath)
        onPath (path);
}

void Display::closeEntry()
{
    entry.setText (kind == Kind::session ? session.path : strip.path, false);
    entry.setVisible (false);
}

std::unique_ptr<juce::AccessibilityHandler> Display::createAccessibilityHandler()
{
    struct Value final : public juce::AccessibilityTextValueInterface
    {
        explicit Value (Display& d) : owner (d) {}
        bool isReadOnly() const override { return true; }
        String getCurrentValueAsString() const override { return owner.accessibleValue(); }
        void setValueAsString (const String&) override {}
        Display& owner;
    };
    juce::AccessibilityActions actions;
    if (pathEntry)
        actions.addAction (juce::AccessibilityActionType::press, [this] { openEntry(); });
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::staticText, std::move (actions),
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

void Display::render()
{
    const auto g = element.glass.getSmallestIntegerContainer();
    const auto origin = g.getPosition().toFloat();
    image = scaledArea (ctx.model.plates.at (element.view), g, scale);
    placed.clear();
    auto show = [&] (const String& key, const String& text, std::optional<Line> adjusted = {})
    {
        const auto& l = adjusted ? *adjusted : spec.line (key);
        placed[key] = place (ctx.lettering, l, text, spec.minHscale);
        drawLit (image, origin, placed[key].path, spec.lattice, scale);
        if (l.indicator.isNotEmpty())
        {
            juce::Graphics gg (image);
            drawSprite (gg, ctx.model.asset (l.indicator), (l.indicatorCentre - origin) * scale, scale);
        }
    };
    if (kind == Kind::session)
    {
        if (session.layout == "message")
        {
            show ("headline", session.headline);
            show ("detail", session.detail);
        }
        else
            show ("name", session.name);
        show (session.tempoKey != "host" ? "tempo." + session.tempoKey : session.layout == "message" ? "tempo.host.message" : "tempo.host",
              session.tempoText);
    }
    else if (kind == Kind::note)
    {
        if (! note.selected)
            return show ("empty", note.text);
        show ("note", note.text);
        const auto marks = spec.spec["marks"];
        const auto w = (float) (double) marks["width"], h = (float) (double) marks["height"], y = (float) (double) marks["y"];
        juce::Path bars;
        for (const auto& u : *marks["under"].getArray())
            bars.addRectangle (Rect (w, h).withCentre ({ ctx.model.element ("pattern." + u.toString()).centre.x, y + h * 0.5f }));
        drawLit (image, origin, bars, spec.lattice, scale);
    }
    else if (kind == Kind::programmer)
    {
        // The value keeps its last cell; the bank squeezes, then elides, short of it, or of the glass's far margin.
        auto bank = spec.line ("bank");
        if (programmer.value.isNotEmpty())
        {
            show ("value", programmer.value);
            bank.maxRight = placed["value"].path.getBounds().getX() - (float) (double) spec.spec.getProperty ("gap", 24);
        }
        else
            bank.maxRight = element.glass.getRight() - (bank.ink.getX() - element.glass.getX());
        show ("bank", programmer.bank, bank);
    }
    else if (strip.layout == "alert")
        show ("alert", strip.text);
    else
        show ("settings", strip.text);
}

//============================================================================== the overview

std::vector<Block> overviewBlocks (const std::vector<Note>& notes, const OverviewSpec& o, int bars, int numerator, int denominator)
{
    std::vector<Block> out;
    if (notes.empty())
        return out;
    const auto cpb = (double) o.columns / ((double) bars * numerator * 4.0 / denominator);
    auto lo = notes.front().pitch, hi = notes.front().pitch;
    for (auto& n : notes)
    {
        lo = std::min (lo, n.pitch);
        hi = std::max (hi, n.pitch);
    }
    const auto span = o.rows - o.rowsPerNote;
    const auto k = std::min (1.0, (double) span / std::max (1, hi - lo));
    const auto centre = 0.5 * (lo + hi);
    for (auto& n : notes)
    {
        const auto c0 = (int) std::floor (n.start * cpb + 0.5);
        const auto c1 = std::max (c0 + 1, (int) std::floor ((n.start + n.duration) * cpb + 0.5));
        const auto top = juce::jlimit (0, span, (int) std::floor (span / 2.0 + (centre - n.pitch) * k + 0.5));
        const auto x0 = (float) std::max (0.0, n.start * cpb), x1 = (float) std::min ((double) o.columns, (n.start + n.duration) * cpb);
        if (const Block b { std::max (c0, 0), std::min (c1, o.columns), top, x0, x1 }; b.c0 < b.c1 && x0 < x1)
            out.push_back (b);
    }
    return out;
}

std::set<std::pair<int, int>> cellsOf (const std::vector<Block>& blocks, int rowsPerNote)
{
    std::set<std::pair<int, int>> out;
    for (const auto& b : blocks)
        for (int c = b.c0; c < b.c1; ++c)
            for (int r = b.top; r < b.top + rowsPerNote; ++r)
                out.insert ({ c, r });
    return out;
}

std::set<std::pair<int, int>> overviewCells (const std::vector<Note>& notes, const OverviewSpec& o, int bars, int numerator, int denominator)
{
    return cellsOf (overviewBlocks (notes, o, bars, numerator, denominator), o.rowsPerNote);
}

Overview::Overview (Context& c, const Element& e) : element (e), ctx (c)
{
    setTitle (e.label);
    setBounds (e.glass.getSmallestIntegerContainer());
}

void Overview::show (const std::vector<Note>& notesToShow, int barsToShow, int numeratorToShow, int denominatorToShow)
{
    const auto same = [] (const Note& a, const Note& b) { return a.start == b.start && a.duration == b.duration && a.pitch == b.pitch; };
    if (image.isValid() && barsToShow == bars && numeratorToShow == numerator && denominatorToShow == denominator
        && std::equal (notesToShow.begin(), notesToShow.end(), notes.begin(), notes.end(), same))
        return;
    notes = notesToShow;
    bars = barsToShow;
    numerator = numeratorToShow;
    denominator = denominatorToShow;
    const auto& o = ctx.model.overviewFor (element.view);
    blocks = overviewBlocks (notes, o, bars, numerator, denominator);
    cells = cellsOf (blocks, o.rowsPerNote);
    render();
    repaint();
}

void Overview::paint (juce::Graphics& g)
{
    if (const auto s = renderScaleFor (g); s != scale && image.isValid())
    {
        scale = s;
        render();
    }
    g.drawImageTransformed (image, juce::AffineTransform::scale (1.0f / scale));
    drawHitArea (g, ctx, getLocalBounds().toFloat());
}

std::unique_ptr<juce::AccessibilityHandler> Overview::createAccessibilityHandler()
{
    struct Handler final : public juce::AccessibilityHandler
    {
        explicit Handler (Overview& o) : AccessibilityHandler (o, juce::AccessibilityRole::image), owner (o) {}
        String getTitle() const override { return owner.element.label; }
        String getDescription() const override
        {
            const auto n = owner.notes.size();
            return String (owner.bars) + (owner.bars == 1 ? " bar, " : " bars, ") + String ((int) n) + (n == 1 ? " note" : " notes");
        }
        Overview& owner;
    };
    return std::make_unique<Handler> (*this);
}

void Overview::render()
{
    const auto& o = ctx.model.overviewFor (element.view);
    const auto gr = element.glass.getSmallestIntegerContainer();
    const auto origin = gr.getPosition().toFloat();
    image = scaledArea (ctx.model.plates.at (element.view), gr, scale);
    litBlocks (o, origin);
    juce::Graphics g (image);
    g.addTransform (juce::AffineTransform::translation (-origin.x, -origin.y).scaled (scale));
    const auto shownBars = std::max (1, bars);
    const auto barW = (float) o.columns * o.lattice.pitch.x / (float) shownBars;
    g.setColour (juce::Colour::greyLevel ((float) o.linePeak / 255.0f));
    for (int i = 1; i < shownBars; ++i)
    {
        const auto x = o.barsX0 + (float) i * barW;
        g.fillRect (Rect (x - o.lineWidth * 0.5f, o.lineTop, o.lineWidth, o.lineBottom + 1.0f - o.lineTop));
    }
    g.setColour (juce::Colour::greyLevel ((float) o.numberPeak / 255.0f));
    for (int i = 0; i < shownBars; ++i)
    {
        auto p = Lettering::path (ctx.lettering.fontForCap (o.numberCap), String (i + 1));
        const auto b = p.getBounds();
        p.applyTransform (juce::AffineTransform::translation (o.barsX0 + (float) i * barW + o.numberOffsetX - b.getX(), o.numberBaseline));
        g.fillPath (p);
    }
}

void Overview::litBlocks (const OverviewSpec& o, Pt origin)
{
    const auto w = image.getWidth(), h = image.getHeight();
    const auto s = scale;
    const auto& l = o.lattice;
    std::vector<float> cover ((size_t) (w * h), 0.0f), seam ((size_t) (w * h), 1.0f);
    const auto overlap = [s] (int i, float a, float b)
    { return juce::jlimit (0.0f, 1.0f, (std::min (((float) i + 1.0f) / s - 0.5f, b) - std::max ((float) i / s - 0.5f, a)) * s); };
    const auto first = [s] (float a) { return (int) std::floor ((a + 0.5f) * s); };
    const auto last = [s] (float b) { return (int) std::ceil ((b + 0.5f) * s); };
    for (const auto& b : blocks)
    {
        const auto x0 = l.origin.x + (b.x0 - 0.5f) * l.pitch.x + o.litInset[0] - origin.x;
        const auto x1 = l.origin.x + (b.x1 - 0.5f) * l.pitch.x - o.litInset[2] - origin.x;
        const auto y0 = l.origin.y + ((float) b.top - 0.5f) * l.pitch.y + o.litInset[1] - origin.y;
        const auto y1 = l.origin.y + ((float) (b.top + o.rowsPerNote) - 0.5f) * l.pitch.y - o.litInset[3] - origin.y;
        const auto xa = std::max (0, first (x0)), xb = std::min (w - 1, last (x1));
        for (int y = std::max (0, first (y0)); y <= std::min (h - 1, last (y1)); ++y)
            for (int x = xa; x <= xb; ++x)
            {
                auto& v = cover[(size_t) (y * w + x)];
                v = std::max (v, overlap (x, x0, x1) * overlap (y, y0, y1));
            }
        for (int r = b.top; r + 1 < b.top + o.rowsPerNote; ++r)
        {
            const auto yb = l.origin.y + ((float) r + 0.5f) * l.pitch.y - origin.y;
            for (int y = std::max (0, first (yb - o.seamWidth)); y <= std::min (h - 1, last (yb + o.seamWidth)); ++y)
            {
                const auto centre = ((float) y + 0.5f) / s - 0.5f;
                const auto f = 1.0f - o.seamDepth * std::max (0.0f, 1.0f - std::abs (centre - yb) / o.seamWidth);
                for (int x = xa; x <= xb; ++x)
                    seam[(size_t) (y * w + x)] = std::min (seam[(size_t) (y * w + x)], f);
            }
        }
    }
    juce::Image::BitmapData bd (image, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (const auto a = std::min (1.0f, cover[(size_t) (y * w + x)]) * seam[(size_t) (y * w + x)]; a > 0.0f)
                bd.setPixelColour (x, y, bd.getPixelColour (x, y).interpolatedWith (o.lit, a));
}
} // namespace airack::studio

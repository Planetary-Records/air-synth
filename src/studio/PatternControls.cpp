#include "PatternControls.h"
#include "WheelLatch.h"

#include <numeric>

namespace airack::studio
{
static float num (const juce::var& v) { return (float) (double) v; }
static juce::Colour grey (int level) { return juce::Colour::greyLevel ((float) level / 255.0f); }

String noteName (int pitch)
{
    static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return String (names[((pitch % 12) + 12) % 12]) + String (pitch / 12 - 1);
}

String startText (double start, int numerator, int denominator)
{
    const auto s = (int) std::lround (start * 16.0), beat = 64 / denominator, bar = beat * numerator;
    const auto inBar = s % bar, inBeat = inBar % beat;
    auto text = String (s / bar + 1) + "." + String (inBar / beat + 1) + "." + String (inBeat / 4 + 1);
    if (inBeat % 4 != 0)
        text << "." << (inBeat % 4 + 1);
    return text;
}

String lengthText (double duration)
{
    const auto n = (int) std::lround (duration * 16.0), g = std::gcd (std::max (1, n), 64);
    return 64 / g == 1 ? String (n / g) : String (n / g) + "/" + String (64 / g);
}

//============================================================================== Knob

Knob::Knob (Context& c, const Element& e)
    : element (e), centre (e.centre), hit (e.hitRect), labelInk (e.legend), skirt (e.skirtR), ctx (c), sweep (e.sweep),
      pointer (c.model.asset (e.layer ("pointer")))
{
    setTitle (e.label);
    setWantsKeyboardFocus (true);
    setEnabled (false);
    addChildComponent (entry);
    entry.setTitle (e.label + " value");
    entry.setFont (juce::Font (juce::FontOptions (ctx.lettering.face).withHeight (15.0f)));
    entry.setJustification (juce::Justification::centred);
    entry.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff050606));
    entry.setColour (juce::TextEditor::textColourId, juce::Colour (0xffe9ecef));
    entry.setColour (juce::TextEditor::outlineColourId, accent);
    entry.setColour (juce::TextEditor::focusedOutlineColourId, accent);
    entry.onReturnKey = [this] { commitEntry(); };
    entry.onEscapeKey = [this] { closeEntry(); };
    // JUCE posts the loss: by its delivery the entry may have been reopened and focused again.
    entry.onFocusLost = [this]
    {
        if (! entry.hasKeyboardFocus (false))
            closeEntry();
    };

    const auto w = (float) pointer.image.getWidth(), h = (float) pointer.image.getHeight();
    const auto reach = std::max ({ pointer.anchor.getDistanceFrom ({}), pointer.anchor.getDistanceFrom ({ w, 0 }),
                                   pointer.anchor.getDistanceFrom ({ 0, h }), pointer.anchor.getDistanceFrom ({ w, h }) });
    auto r = hit.getUnion (Rect (2 * reach, 2 * reach).withCentre (centre));
    if (! labelInk.isEmpty())
        r = r.getUnion (labelInk.expanded (4.0f));
    r = r.getUnion (entryArea());
    setBounds (r.expanded (2.0f).getSmallestIntegerContainer());
    entry.setBounds ((entryArea() - getPosition().toFloat()).getSmallestIntegerContainer());
}

void Knob::setBinding (std::unique_ptr<KnobBinding> b)
{
    bound = std::move (b);
    shownFraction = -1.0;
    refresh();
}

void Knob::refresh()
{
    setEnabled (bound != nullptr && bound->enabled());
    if (bound == nullptr)
        return;
    if (! entry.isVisible())
        entry.setText (bound->entryText(), false);
    if (const auto f = bound->fraction(); f != shownFraction)
    {
        shownFraction = f;
        repaint();
    }
    if (const auto v = bound->valueText(); v != shownValue)
    {
        shownValue = v;
        if (auto* h = getAccessibilityHandler())
            h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
    }
}

float Knob::angle() const { return bound != nullptr ? (float) ((2.0 * bound->fraction() - 1.0) * sweep) : -sweep; }

Rect Knob::entryArea() const
{
    return Rect (96.0f, 22.0f).withCentre ({ centre.x, labelInk.isEmpty() ? hit.getBottom() - 11.0f : labelInk.getCentreY() });
}

KnobBinding::Menu Knob::menu() const
{
    KnobBinding::Menu m;
    if (bound != nullptr)
        bound->addMenu (m);
    return m;
}

void Knob::paint (juce::Graphics& g)
{
    const auto o = getPosition().toFloat();
    drawHitArea (g, ctx, hit - o);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImageTransformed (pointer.image, juce::AffineTransform::translation (-pointer.anchor.x, -pointer.anchor.y)
                                               .rotated (juce::degreesToRadians (angle()))
                                               .translated (centre.x - o.x, centre.y - o.y));
    const auto face = Rect (2 * skirt, 2 * skirt).withCentre (centre - o);
    if (! isEnabled())
    {
        g.setColour (juce::Colour (0xff2b2c2e).withAlpha (0.55f));
        g.fillEllipse (face.expanded (1.0f));
    }
    if (focusFromKeyboard && hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawEllipse (face.expanded (4.0f), 2.0f);
    }
}

bool Knob::hitTest (int x, int y)
{
    const auto p = Pt ((float) x, (float) y) + getPosition().toFloat();
    if (entry.isVisible() && entryArea().contains (p))
        return true;
    return hit.contains (p);
}

void Knob::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled())
        return;
    if (e.mods.isPopupMenu())
        return showMenu();
    if (! bound->begin())
        return;
    dragFrom = bound->fraction();
    dragging = true;
    if (isShowing())
        grabKeyboardFocus();
}

void Knob::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        move (dragFrom + (e.mouseDownPosition.y - e.position.y) / (e.mods.isShiftDown() ? 1000.0 : 200.0));
}

void Knob::mouseUp (const juce::MouseEvent&)
{
    if (! dragging)
        return;
    dragging = false;
    bound->end (true);
    refresh();
}

void Knob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu())
        openEntry();
}

void Knob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (rackScrolling())
        return juce::Component::mouseWheelMove (e, w);
    if (isEnabled() && ! dragging && w.deltaY != 0.0f)
        stepBy (w.deltaY > 0.0f ? 1 : -1);
}

bool Knob::keyPressed (const juce::KeyPress& key)
{
    if (! isEnabled())
        return false;
    const auto code = key.getKeyCode();
    if (dragging)
    {
        if (code != juce::KeyPress::escapeKey)
            return false;
        dragging = false;
        bound->end (false);
        refresh();
        return true;
    }
    if (code == juce::KeyPress::upKey || code == juce::KeyPress::rightKey) stepBy (1);
    else if (code == juce::KeyPress::downKey || code == juce::KeyPress::leftKey) stepBy (-1);
    else if (code == juce::KeyPress::pageUpKey) stepBy (10);
    else if (code == juce::KeyPress::pageDownKey) stepBy (-10);
    else if (code == juce::KeyPress::homeKey) move (0.0);
    else if (code == juce::KeyPress::endKey) move (1.0);
    else if (code == juce::KeyPress::returnKey) openEntry();
    else return false;
    return true;
}

void Knob::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
void Knob::focusLost (FocusChangeType) { focusFromKeyboard = false; repaint(); }

void Knob::enablementChanged()
{
    if (dragging)
    {
        dragging = false;
        if (bound != nullptr)
            bound->end (false);
    }
    if (! isEnabled())
        closeEntry();
    repaint();
}

void Knob::move (double f)
{
    f = juce::jlimit (0.0, 1.0, f);
    const auto n = bound->steps();
    if (n > 0)
    {
        const auto step = (int) std::lround (f * n);
        if (step != (int) std::lround (bound->fraction() * n))
            bound->set ((double) step / n);
    }
    else if (std::abs (f - bound->fraction()) > 1.0e-6)
        bound->set (f);
    refresh();
}

void Knob::stepBy (int k)
{
    const auto n = bound->steps();
    move (n > 0 ? (double) (std::lround (bound->fraction() * n) + k) / n : bound->fraction() + k / 100.0);
}

void Knob::openEntry()
{
    if (! isEnabled() || dragging)
        return;
    entry.setText (bound->entryText(), false);
    entry.setVisible (true);
    if (isShowing())
        entry.grabKeyboardFocus();
    entry.selectAll();
}

void Knob::commitEntry()
{
    if (! entry.isVisible())
        return;
    const auto text = entry.getText();
    entry.setVisible (false);
    if (bound != nullptr)
        bound->enter (text);
    refresh();
}

void Knob::closeEntry()
{
    if (! entry.isVisible())
        return;
    entry.setVisible (false);
    if (bound != nullptr)
        entry.setText (bound->entryText(), false);
}

void Knob::showMenu()
{
    auto items = menu();
    if (items.empty())
        return;
    juce::PopupMenu pm;
    for (size_t i = 0; i < items.size(); ++i)
        pm.addItem ((int) i + 1, items[i].first);
    pm.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                      [safe = juce::Component::SafePointer<Knob> (this), items] (int chosen)
                      {
                          if (safe != nullptr && chosen > 0)
                              items[(size_t) chosen - 1].second();
                      });
}

void Knob::cancelDrag()
{
    dragging = false;
    repaint();
}

std::unique_ptr<juce::AccessibilityHandler> Knob::createAccessibilityHandler()
{
    struct Value final : public juce::AccessibilityValueInterface
    {
        explicit Value (Knob& k) : knob (k) {}
        bool isReadOnly() const override { return ! knob.isEnabled(); }
        double getCurrentValue() const override { return knob.bound != nullptr ? knob.bound->fraction() : 0.0; }
        void setValue (double v) override
        {
            if (knob.isEnabled())
                knob.move (v);
        }
        String getCurrentValueAsString() const override { return knob.bound != nullptr ? knob.bound->valueText() : String(); }
        void setValueAsString (const String& s) override
        {
            if (! knob.isEnabled())
                return;
            knob.bound->enter (s);
            knob.refresh();
        }
        AccessibleValueRange getRange() const override
        {
            const auto n = knob.bound != nullptr ? knob.bound->steps() : 0;
            return { { 0.0, 1.0 }, n > 0 ? 1.0 / n : 0.01 };
        }
        Knob& knob;
    };
    juce::AccessibilityActions actions;
    actions.addAction (juce::AccessibilityActionType::showMenu, [this] { showMenu(); });
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::slider, std::move (actions),
                                                         juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

//============================================================================== Roll

Roll::Roll (Context& c, const Element& e) : element (e), ctx (c), spec (c.model.rolls["pattern"])
{
    setTitle (e.label);
    setHelpText ("Double-click to add, drag to move, right edge to resize, Alt-drag for velocity");
    setWantsKeyboardFocus (true);
    setBounds (e.glass.getSmallestIntegerContainer());
    const auto gr = spec["grid"];
    x0 = num (gr["x"][0]);
    x1 = num (gr["x"][1]);
    top = num (gr["top"]);
    rowH = num (gr["row_h"]);
    rows = (int) gr["rows"];
    laneFoot = num (spec["lane"]["stem"]["bottom"]);
    rollTop = (int) c.model.patternMockup["top_pitch"];
    describe();
}

void Roll::show (const RollShown& shown)
{
    if (shown == s)
        return;
    auto withPlayhead = s;
    withPlayhead.playhead = shown.playhead;
    const auto before = playheadArea();
    s = shown;
    describe();
    if (withPlayhead == shown)
    {
        repaint ((before - getPosition().toFloat()).getSmallestIntegerContainer().expanded (1));
        repaint ((playheadArea() - getPosition().toFloat()).getSmallestIntegerContainer().expanded (1));
        return;
    }
    repaint();
}

const Note& Roll::noteShown (int i) const { return preview && previewIndex == i ? *preview : s.notes[(size_t) i]; }

Rect Roll::noteRect (int i) const
{
    const auto& n = noteShown (i);
    const auto h = num (spec["notes"]["height"]);
    const auto t = std::round (rowCentre (n.pitch) - h * 0.5f);
    auto r = Rect::leftTopRightBottom (std::round (xOf (n.start)) + 1.0f, t, std::round (xOf (n.start + n.duration)) - 1.0f, t + h);
    // A short note stays 8 px wide, inside the grid, so that it can be seen and grabbed.
    if (r.getWidth() < 8.0f)
        r = r.withWidth (8.0f);
    return r.getRight() > x1 ? r.withX (x1 - r.getWidth()) : r;
}

Rect Roll::selectedOutline() const
{
    if (s.selected < 0)
        return {};
    const auto r = noteRect (s.selected).expanded (0.0f, num (spec["selected"]["grow"]));
    return Rect::leftTopRightBottom (r.getX(), std::floor (r.getY()), r.getRight(), std::ceil (r.getBottom()));
}

Rect Roll::thumb() const
{
    const auto sb = spec["scrollbar"];
    const auto t0 = num (sb["track"][0]), t1 = num (sb["track"][1]);
    const auto h = (t1 - t0) * (float) rows / 128.0f;
    const auto y = t0 + (float) (127 - rollTop) / (float) (128 - rows) * (t1 - t0 - h);
    return Rect::leftTopRightBottom (num (sb["x"][0]), y, num (sb["x"][1]), y + h);
}

Rect Roll::track() const
{
    const auto sb = spec["scrollbar"];
    return Rect::leftTopRightBottom (num (sb["x"][0]) - 2.0f, num (sb["track"][0]), num (sb["x"][1]) + 2.0f, num (sb["track"][1]));
}

Rect Roll::playheadArea() const
{
    if (! s.playhead)
        return {};
    const auto w = num (spec["playhead"]["width"]);
    return Rect (playheadX() - w * 0.5f, element.glass.getY() + 3.0f, w, laneFoot - element.glass.getY() - 3.0f);
}

void Roll::describe()
{
    setDescription ("Piano roll: " + String ((int) s.notes.size()) + " notes over " + String (s.bars) + (s.bars == 1 ? " bar" : " bars") + " of "
                    + String (s.numerator) + "/" + String (s.denominator) + ", grid " + s.grid
                    + (s.selected >= 0 ? ", note " + String (s.selected + 1) + " selected" : String()));
}

void Roll::scrollTo (int topPitch)
{
    const auto t = juce::jlimit (rows - 1, 127, topPitch);
    if (t == rollTop)
        return;
    rollTop = t;
    repaint();
}

void Roll::reveal (int pitch)
{
    if (pitch > rollTop)
        scrollTo (pitch);
    else if (pitch < rollTop - rows + 1)
        scrollTo (pitch + rows - 1);
}

void Roll::cancelDrag()
{
    const auto hadPreview = preview.has_value();
    preview.reset();
    dragging = scrolling = false;
    if (hadPreview)
        repaint();
}

void Roll::paint (juce::Graphics& g)
{
    g.addTransform (juce::AffineTransform::translation (-getPosition().toFloat()));
    g.reduceClipRegion (element.glass.getSmallestIntegerContainer());
    const auto H = spec["header"], L = spec["lane"], B = spec["bars"], R = spec["ruler"], N = spec["notes"], S = spec["selected"];
    const auto bottom = top + (float) rows * rowH;
    const auto barW = (x1 - x0) / (float) s.bars;
    const auto hx0 = num (H["rule"]["x"][0]), hx1 = num (H["rule"]["x"][1]);

    // beats dashed between the bars, from the header rule through the lane; the bars over them
    g.setColour (grey ((int) spec["beats"]["grey"]));
    const auto on = num (spec["beats"]["dash"][0]), off = num (spec["beats"]["dash"][1]);
    for (int b = 0; b < s.bars; ++b)
        for (int k = 1; k < s.numerator; ++k)
        {
            const auto x = std::round (x0 + (float) b * barW + (float) k * barW / (float) s.numerator);
            for (auto y = num (H["rule"]["y"]) + 1.0f; y < laneFoot; y += on + off)
                g.fillRect (Rect (x, y, 1.0f, std::min (on, laneFoot - y)));
        }
    for (int b = 0; b <= s.bars; ++b)
    {
        g.setColour (grey ((int) (b == s.bars ? B["end_grey"] : B["grey"])));
        g.fillRect (Rect (std::round (x0 + (float) b * barW), element.glass.getY() + 3.0f, num (B["width"]), laneFoot - element.glass.getY() - 3.0f));
    }
    g.setColour (grey ((int) H["rule"]["grey"]));
    g.fillRect (Rect (hx0, num (H["rule"]["y"]), hx1 - hx0, 1.0f));
    g.setColour (grey ((int) L["rule"]["grey"]));
    g.fillRect (Rect (hx0, num (L["rule"]["y"]), hx1 - hx0, num (L["rule"]["height"])));

    // bar numbers, their ink ink_dx right of the bar line
    const auto numFont = ctx.lettering.fontForCap (num (H["numbers"]["cap"]));
    g.setColour (grey ((int) H["numbers"]["peak"]));
    for (int b = 0; b < s.bars; ++b)
    {
        auto p = Lettering::path (numFont, String (b + 1));
        p.applyTransform (juce::AffineTransform::scale (num (H["numbers"]["hscale"]), 1.0f));
        const auto ink = p.getBounds();
        g.fillPath (p, juce::AffineTransform::translation (x0 + (float) b * barW + num (H["numbers"]["ink_dx"]) - ink.getX(), num (H["numbers"]["baseline"])));
    }

    // the ruler: a row per semitone, black keys from the left; seams where two white keys meet
    const auto rx0 = num (R["x"][0]), rx1 = num (R["x"][1]), blackRight = num (R["black_right"]);
    const auto labelFont = ctx.lettering.fontForCap (num (spec["labels"]["cap"]));
    for (int r = 0; r < rows; ++r)
    {
        const auto pitch = rollTop - r;
        const auto y = top + (float) r * rowH;
        const auto black = juce::MidiMessage::isMidiNoteBlack (pitch);
        g.setColour (grey ((int) R["white"]));
        g.fillRect (Rect::leftTopRightBottom (rx0, y, rx1, y + rowH));
        g.setColour (grey ((int) R["seam"]));
        if (black)
        {
            g.fillRect (Rect::leftTopRightBottom (blackRight, y + rowH * 0.5f - 0.5f, rx1, y + rowH * 0.5f + 0.5f));
            g.setColour (grey ((int) R["black"]));
            g.fillRect (Rect::leftTopRightBottom (rx0, y, blackRight, y + rowH));
        }
        else if (pitch % 12 == 11 || pitch % 12 == 4)
            g.fillRect (Rect::leftTopRightBottom (rx0, y - 0.5f, rx1, y + 0.5f));
        if (pitch % 12 == 0)
        {
            auto p = Lettering::path (labelFont, noteName (pitch));
            p.applyTransform (juce::AffineTransform::scale (num (spec["labels"]["hscale"]), 1.0f));
            const auto ink = p.getBounds();
            g.setColour (grey ((int) spec["labels"]["peak"]));
            g.fillPath (p, juce::AffineTransform::translation (num (spec["labels"]["x"]) - ink.getX(), y + rowH * 0.5f - ink.getCentreY()));
        }
    }

    // the notes and their velocity stems; the selected one darkened inside its outline, its handle out of its end
    g.saveState();
    g.reduceClipRegion (Rect::leftTopRightBottom (x0, top, x1 + 1.0f, bottom).getSmallestIntegerContainer());
    for (int i = 0; i < (int) s.notes.size(); ++i)
        if (i != s.selected)
        {
            g.setColour (grey ((int) N["fill"]));
            g.fillRect (noteRect (i));
        }
    if (s.selected >= 0)
    {
        const auto o = selectedOutline();
        g.setColour (grey ((int) S["fill"]));
        g.fillRect (o);
        g.setColour (grey ((int) S["outline"]));
        g.drawRect (o, num (S["width"]));
        const auto gap = num (S["handle"]["gap"]), size = num (S["handle"]["size"]);
        g.fillRect (Rect (o.getRight(), o.getCentreY() - num (S["width"]) * 0.5f, gap, num (S["width"])));
        g.fillRect (Rect (size, size).withCentre ({ o.getRight() + gap + size * 0.5f, o.getCentreY() }));
    }
    g.restoreState();
    g.setColour (grey ((int) L["stem"]["peak"]));
    for (int i = 0; i < (int) s.notes.size(); ++i)
    {
        const auto& n = noteShown (i);
        const auto h = num (L["stem"]["full"]) * (float) n.velocity / 127.0f;
        g.fillRect (Rect (xOf (n.start) + num (L["stem"]["dx"]), laneFoot - h, num (L["stem"]["width"]), h));
    }

    if (s.playhead)
    {
        g.setColour (grey ((int) spec["playhead"]["grey"]));
        g.fillRect (playheadArea());
    }

    const auto sb = spec["scrollbar"];
    g.setColour (grey ((int) sb["track_grey"]));
    g.fillRoundedRectangle (Rect::leftTopRightBottom (num (sb["x"][0]), num (sb["track"][0]), num (sb["x"][1]), num (sb["track"][1])), 3.0f);
    g.setColour (grey ((int) sb["thumb_grey"]));
    g.fillRoundedRectangle (thumb(), 3.0f);

    if (focusFromKeyboard && hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawRoundedRectangle (element.glass.reduced (2.0f), 4.0f, 2.0f);
    }
    drawHitArea (g, ctx, element.glass);
}

int Roll::noteAt (Pt p) const
{
    const auto under = [&] (int i) { return noteRect (i).expanded (0.0f, 1.0f).contains (p); };
    if (s.selected >= 0 && under (s.selected))
        return s.selected;
    for (int i = (int) s.notes.size() - 1; i >= 0; --i)
        if (under (i))
            return i;
    return -1;
}

void Roll::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    cancelDrag();
    const auto p = e.position + getPosition().toFloat();
    if (isShowing())
        grabKeyboardFocus();
    if (track().contains (p))
    {
        scrolling = true;
        scrollFrom = rollTop;
        origin = p;
        return;
    }
    if (onAdmit && ! onAdmit())
        return;
    if (! inGrid (p))
        return;
    const auto i = noteAt (p);
    s.selected = i;
    describe();
    repaint();
    if (onSelect)
        onSelect (i);
    if (i < 0 || s.selected != i)
        return;
    previewIndex = i;
    captured = s.notes[(size_t) i];
    origin = p;
    resizing = p.x >= noteRect (i).getRight() - 4.0f;
    velocity = e.mods.isAltDown();
    dragging = true;
}

void Roll::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = e.position + getPosition().toFloat();
    if (scrolling)
    {
        const auto span = track().getHeight() - thumb().getHeight();
        scrollTo (scrollFrom - juce::roundToInt ((p.y - origin.y) * (float) (128 - rows) / std::max (1.0f, span)));
        return;
    }
    if (! dragging)
        return;
    auto note = captured;
    const auto grid = s.gridBeats, length = s.lengthBeats();
    if (velocity)
        note.velocity = juce::jlimit (1, 127, captured.velocity + juce::roundToInt (origin.y - p.y));
    else if (resizing)
    {
        const auto end = std::round ((captured.start + captured.duration + (p.x - origin.x) / beatWidth()) / grid) * grid;
        note.duration = juce::jlimit (std::min (grid, length - captured.start), length - captured.start, end - captured.start);
    }
    else
    {
        note.start = juce::jlimit (0.0, length - captured.duration, std::round ((captured.start + (p.x - origin.x) / beatWidth()) / grid) * grid);
        note.pitch = juce::jlimit (0, 127, captured.pitch + juce::roundToInt ((origin.y - p.y) / rowH));
    }
    if (! accepts || accepts (previewIndex, note))
    {
        preview = note;
        repaint();
    }
}

void Roll::mouseUp (const juce::MouseEvent&)
{
    scrolling = false;
    if (! dragging)
        return;
    dragging = false;
    if (! preview)
        return;
    const auto n = *preview;
    preview.reset();
    repaint();
    if (n != captured && onEdit)
        onEdit (previewIndex, n);
}

void Roll::mouseDoubleClick (const juce::MouseEvent& e)
{
    cancelDrag();
    const auto p = e.position + getPosition().toFloat();
    if (onAdmit && ! onAdmit())
        return;
    if (! inGrid (p) || noteAt (p) >= 0)
        return;
    const auto grid = s.gridBeats, length = s.lengthBeats();
    const auto start = std::floor ((p.x - x0) / beatWidth() / grid) * grid;
    if (start < 0.0 || start >= length)
        return;
    Note n { start, std::min (grid, length - start), juce::jlimit (0, 127, rollTop - (int) std::floor ((p.y - top) / rowH)), 100, 1 };
    if (s.selected >= 0)
    {
        n.velocity = s.notes[(size_t) s.selected].velocity;
        n.channel = s.notes[(size_t) s.selected].channel;
    }
    if (onAdd)
        onAdd (n);
}

void Roll::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w)
{
    if (w.deltaY != 0.0f)
        scrollTo (rollTop + (w.deltaY > 0.0f ? 2 : -2));
}

bool Roll::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        cancelDrag();
        if (onCancel)
            onCancel();
        return true;
    }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        cancelDrag();
        if (onDelete)
            onDelete();
        return true;
    }
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
    {
        scrollTo (rollTop + (key == juce::KeyPress::upKey ? 1 : -1));
        return true;
    }
    return false;
}

void Roll::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }
void Roll::focusLost (FocusChangeType) { focusFromKeyboard = false; repaint(); }

std::unique_ptr<juce::AccessibilityHandler> Roll::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group);
}

//============================================================================== Keyboard

Keyboard::Keyboard (Context& c, const Element& e) : element (e), ctx (c), spec (c.model.keyboards["pattern"])
{
    setTitle ("Keyboard");
    setDescription ("Playable keyboard: MIDI channel 1, velocity 100");
    setWantsKeyboardFocus (true);
    setBounds (e.rect.getSmallestIntegerContainer());
    lowest = (int) spec["home"];
    for (const auto k : String ("awsedftgyhujkolp;"))
        computerKeys.add (juce::KeyPress ((int) k, juce::ModifierKeys(), 0));
}

void Keyboard::shift (int octaves)
{
    const auto range = spec["octave_range"];
    const auto l = juce::jlimit ((int) range[0], (int) range[1], lowest + 12 * octaves);
    if (l == lowest)
        return;
    lowest = l;
    repaint();
}

Rect Keyboard::keyRect (int k) const
{
    const auto r = element.rect;
    const auto ww = r.getWidth() / (float) (int) spec["white_keys"];
    const auto gap = num (spec["membrane"]["gap"]);
    int whites = 0;
    for (int i = 0; i < k; ++i)
        whites += ! juce::MidiMessage::isMidiNoteBlack (lowest + i);
    if (! juce::MidiMessage::isMidiNoteBlack (lowest + k))
        return Rect (r.getX() + (float) whites * ww + gap * 0.5f, r.getY(), ww - gap, r.getHeight());
    const auto bw = ww * num (spec["black"]["width_of_white"]), bh = r.getHeight() * num (spec["black"]["depth"]);
    return Rect (r.getX() + (float) whites * ww - bw * 0.5f, r.getY(), bw, bh);
}

int Keyboard::pitchAt (Pt p) const
{
    if (! element.rect.contains (p))
        return -1;
    const auto gap = num (spec["membrane"]["gap"]);
    for (int pass = 0; pass < 2; ++pass)
        for (int k = 0; k < keyCount(); ++k)
        {
            const auto black = juce::MidiMessage::isMidiNoteBlack (lowest + k);
            if (black == (pass == 0) && keyRect (k).expanded (black ? 0.0f : gap * 0.5f, 0.0f).contains (p))
                return lowest + k;
        }
    return -1;
}

void Keyboard::paint (juce::Graphics& g)
{
    const auto o = getPosition().toFloat();
    const auto m = spec["membrane"];
    const auto radius = num (m["radius"]);
    g.setColour (grey ((int) m["base"]));
    g.fillRect (element.rect - o);
    for (int pass = 0; pass < 2; ++pass)
        for (int k = 0; k < keyCount(); ++k)
        {
            const auto pitch = lowest + k;
            const auto black = juce::MidiMessage::isMidiNoteBlack (pitch);
            if (black != (pass == 1))
                continue;
            const auto r = keyRect (k) - o;
            const auto held = isHeld (pitch);
            g.setColour (grey ((int) (black ? (held ? m["pressed_black"] : m["black"]) : (held ? m["pressed_white"] : m["white"]))));
            g.fillRoundedRectangle (r, radius);
            g.setColour (grey ((int) m["lip"]["grey"]));
            g.fillRect (r.withTrimmedLeft (radius).withTrimmedRight (radius).removeFromBottom (num (m["lip"]["height"])));
            if (pitch % 12 == 0)
            {
                const auto& L = spec["label"];
                auto p = Lettering::path (ctx.screenLettering.fontForCap (num (L["cap"])), noteName (pitch));
                const auto ink = p.getBounds();
                g.setColour (grey ((int) L["grey"]));
                g.fillPath (p, juce::AffineTransform::translation (r.getCentreX() - ink.getCentreX(), r.getBottom() - num (L["foot"])));
            }
        }
    if (focusFromKeyboard && hasKeyboardFocus (false))
    {
        g.setColour (accent);
        g.drawRoundedRectangle ((element.rect - o).expanded (2.0f), 4.0f, 2.0f);
    }
    drawHitArea (g, ctx, element.rect - o);
}

bool Keyboard::send (bool on, int pitch, int velocity)
{
    if (! onKey || onKey (on, pitch, velocity))
        return true;
    forget();
    return false;
}

void Keyboard::press (int pitch)
{
    if (pitch < 0 || ! send (true, pitch, 100))
        return;
    down = pitch;
    repaint();
}

void Keyboard::release()
{
    if (down < 0)
        return;
    const auto p = down;
    down = -1;
    repaint();
    send (false, p, 0);
}

void Keyboard::forget()
{
    if (! isPlaying())
        return;
    down = -1;
    computer.clear();
    repaint();
}

void Keyboard::releaseAll()
{
    auto held = computer;
    if (down >= 0)
        held.insert (down);
    down = -1;
    computer.clear();
    repaint();
    for (const auto p : held)
        if (! send (false, p, 0))
            return;
}

void Keyboard::mouseDown (const juce::MouseEvent& e) { press (pitchAt (e.position + getPosition().toFloat())); }

void Keyboard::mouseDrag (const juce::MouseEvent& e)
{
    const auto p = pitchAt (e.position + getPosition().toFloat());
    if (p != down)
    {
        release();
        press (p);
    }
}

void Keyboard::mouseUp (const juce::MouseEvent&) { release(); }

bool Keyboard::keyPressed (const juce::KeyPress& key) { return computerKeys.contains (key); }

bool Keyboard::keyStateChanged (bool)
{
    auto used = false;
    for (int i = 0; i < computerKeys.size(); ++i)
    {
        const auto pitch = 72 + i;
        const auto isDown = isKeyDown (computerKeys.getReference (i));
        if (isDown == (computer.count (pitch) > 0))
            continue;
        used = true;
        if (isDown)
        {
            if (! send (true, pitch, 127))
                return true;
            computer.insert (pitch);
        }
        else
        {
            computer.erase (pitch);
            if (! send (false, pitch, 0))
                return true;
        }
    }
    if (used)
        repaint();
    return used;
}

void Keyboard::focusGained (FocusChangeType cause) { focusFromKeyboard = cause == focusChangedByTabKey; repaint(); }

void Keyboard::focusLost (FocusChangeType)
{
    focusFromKeyboard = false;
    releaseAll();
    repaint();
}

std::unique_ptr<juce::AccessibilityHandler> Keyboard::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler> (*this, juce::AccessibilityRole::group);
}
} // namespace airack::studio
